/* nes-qnx / gb-qnx: the Retro app's game processes: an NES emulator (FCEUmm, GPL-2.0) or, built with
 * RETRO_CORE_GB, a Game Boy / Game Boy Color emulator (Gambatte, GPL-2.0), each a statically linked libretro
 * core, with the same game picker and menu, drawn on our own screen layer. Both list every game (.nes, .gb,
 * .gbc); choosing one the other emulator plays hands over to it (exec, same process id, so the iDrive page
 * still knows the game). Two libretro cores cannot share one program (they define the same functions).
 *   --hmi-trace   the shared HMI trace writer every app's worker carries
 *   --play        the game picker, then the chosen game, read from a wired Xbox pad on USB
 *   --play-rom P  straight into game P (a hand-over from the other emulator)
 *   --restore     takes the layer off the screen and gives the audio source back (safe any time)
 *
 * Built from the pieces Freedoom proved on the car: freedoom_screen.h (a 512x480 area: the NES's 256x240
 * doubled), gr_pad.c (pad states over UDP NS_PORT), car_audio.h (the browser audio source; the radio
 * pauses). This file is a small libretro front end: it gives the core its pictures, sound, buttons and
 * folders. The iDrive page only has Play: choosing a game happens here, so the page never changes.
 * Games (*.nes) come from NS_GAMES_DIR on the hard drive (the games installer puts the bundled homebrew there,
 * and the owner's own games later); battery saves (.srm) and save states (.state) go to NS_SAVE_DIR on the
 * flash. Safety as the other games: below the HMI's priority, ends on "stop"/signal/parent gone (saving the
 * battery save first), always takes its layer off. */
#define _POSIX_C_SOURCE 200809L
#ifdef __QNX__
#define _QNX_SOURCE
#endif
#pragma GCC diagnostic ignored "-Wunused-function"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include "hmi_trace_sink.h"
#include "gr_show.h"
#ifdef RETRO_CORE_PS1
#define FDS_GAME_W 640                 /* PS1 pictures are fitted to 4:3 (their sizes vary: 256..640 x 224..480) */
#else
#define FDS_GAME_W 512
#endif
#define FDS_GAME_H 480
#include "freedoom_screen.h"
#include "nes_input.h"
#ifdef RETRO_CORE_PS1
#include "ps1_controls.h"
#include "ps1_runtime.h"
#endif
#include "car_audio.h"
#ifdef __QNX__
#include "gr_pad.c"
#endif
#include "libretro.h"
#include "font8x8_basic.h"
#include "pad_remap_screen.h"   /* your own buttons per game (after the font: it has no include guard) */

#ifndef NS_PORT
#define NS_PORT 42893
#endif
#ifndef NS_GAMES_DIR
#define NS_GAMES_DIR "/mnt/data/ID6Games/Retro"
#endif
#ifndef NS_DATA_DIR
#define NS_DATA_DIR "/var/opt/hmi/data/ID5/Plugins/Retro"
#endif
#ifndef NS_SAVE_DIR
#define NS_SAVE_DIR "/var/opt/hmi/data/ID5/Retro-data"
#endif
#ifndef NS_PRIORITY
#define NS_PRIORITY 8          /* the HMI's own thread runs at 10; the game must never starve it */
#endif
#define NS_MAX_GAMES 200
#ifndef NS_PS1_BOOTLOGO
#define NS_PS1_BOOTLOGO "disabled"   /* the PlayStation logo at each start: off (5 s more wait) */
#endif
#if defined(RETRO_CORE_GB)
#define NS_CORE 'g'
#define NS_SELF "gb-qnx"
#elif defined(RETRO_CORE_PS1)
#define NS_CORE 'p'
#define NS_SELF "ps1-qnx"
#else
#define NS_CORE 'n'
#define NS_SELF "nes-qnx"
#endif
/* The program that plays a core's games (all three live in the Retro plugin folder). */
static const char *ns_program(char core) { return core == 'g' ? "gb-qnx" : core == 'p' ? "ps1-qnx" : "nes-qnx"; }

static fd_screen screen;
static mps_api api;
static mps_runner runner = mps_popen_runner;
static int ctl = -1, cleaned = 0, game_loaded = 0;
static uint64_t start_ms, last_parent_check;
static unsigned pad_msgs, games_started;
static xpad_state pad;
#if defined(RETRO_CORE_PS1)
#define NS_RM_SYS rm_ps1
#elif defined(RETRO_CORE_GB)
#define NS_RM_SYS rm_gb
#else
#define NS_RM_SYS rm_nes
#endif
static rm_map ns_map;          /* this game's own button layout (pad_remap.h); active = 0: the built-in one */
static rm_screen ns_rm;        /* the "Change buttons" screen */
#ifdef RETRO_CORE_PS1
static ps1_audio_queue ps1_sound = PS1_AUDIO_INIT;
static unsigned ns_ps1_profile = PS1_SHOOTER, ns_ps1_period = 16666667u;
static unsigned ns_ps1_muted_input, ns_ps1_dupes, ns_ps1_full_waits;
static void (*ns_ps1_buffer_cb)(bool,unsigned,bool);
static unsigned ns_ps1_buffer_reports, ns_ps1_low_reports, ns_ps1_stall_logs;
static uint64_t ns_ps1_video_ns;
static uint64_t ns_cpu_ns(void) {
#ifdef CLOCK_THREAD_CPUTIME_ID
    struct timespec t;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)==0) return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;
#endif
    return 0;
}
static uint64_t ns_ps1_input_stamp;
static unsigned ns_ps1_input_seen, ns_ps1_input_max_ms;
static void ns_poll(void);
static void ns_update_av(const struct retro_system_av_info *av);
static uint64_t ns_now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec*1000000000u + (uint64_t)t.tv_nsec;
}
static unsigned ns_period(void) {
    unsigned n; pthread_mutex_lock(&ps1_sound.lock); n=ns_ps1_period; pthread_mutex_unlock(&ps1_sound.lock); return n;
}

#endif
static char save_dir[512];
#ifdef __QNX__
static pthread_t pad_thread;
static int pad_started;
static volatile int pad_done;   /* the pad thread has closed the pad */
#endif

/* ---- the game list ---- */
static char *games[NS_MAX_GAMES];       /* full paths */
static char games_yours[NS_MAX_GAMES];  /* 1 = the owner's own (shown with a * in the list) */
static char games_core[NS_MAX_GAMES];   /* 'n' = NES (nes-qnx), 'g' = Game Boy / Color (gb-qnx), 'p' = PS1 (ps1-qnx) */
static char self_path[600];             /* this program, for finding the other emulator */

/* Which emulator plays a file, by its extension (any case), or 0 if neither. */
static char ns_core_for(const char *name) {
    size_t n = strlen(name);
    char ext[5];
    unsigned i;
    if (n < 4) return 0;
    for (i = 0; i < 4 && name[n - 4 + i]; ++i) ext[i] = (char)tolower((unsigned char)name[n - 4 + i]);
    ext[4] = 0;
    if (!strcmp(ext, ".nes")) return 'n';
    if (!strcmp(ext, ".gbc") || !strcmp(ext + 1, ".gb")) return 'g';
    if (!strcmp(ext, ".chd") || !strcmp(ext, ".cue") || !strcmp(ext, ".pbp") || !strcmp(ext, ".m3u")) return 'p';
    return 0;
}
/* Whether the program for a core is here (our own always is): a game nobody here can play isn't listed. */
static int ns_have_program(char core) {
    char path[700], *slash;
    if (core == NS_CORE) return 1;
    snprintf(path, sizeof path, "%s", self_path);
    slash = strrchr(path, '/');
    if (slash) snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "%s", ns_program(core));
    else snprintf(path, sizeof path, "%s", ns_program(core));
    return access(path, X_OK) == 0;
}
static unsigned game_count, current = 0;
#ifdef RETRO_CORE_PS1
static void ns_profile_path(char *path, unsigned size) {
    const unsigned char *c=(const unsigned char *)games[current];
    uint32_t h=2166136261u;
    while (*c) { h^=*c++; h*=16777619u; }
    snprintf(path,size,"%s/ps1-controls-%08x.cfg",save_dir,(unsigned)h);
}
static int ns_ps1_save_profile(void) {
    char path[600]; FILE *f;
    ns_profile_path(path,sizeof path);
    f=fopen(path,"w"); if(!f) return 0;
    fprintf(f,"%u\n",ns_ps1_profile);
    return fclose(f)==0;
}
#endif

static int ns_cmp(const void *a, const void *b) {
    const char *x = strrchr(*(char *const *)a, '/'), *y = strrchr(*(char *const *)b, '/');
    return strcmp(x ? x + 1 : *(char *const *)a, y ? y + 1 : *(char *const *)b);
}
static void ns_scan(const char *dir, int yours) {
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != 0 && game_count < NS_MAX_GAMES) {
        size_t n = strlen(e->d_name);
        char path[700];
        struct stat st;
        if (n < 4 || !ns_core_for(e->d_name) || e->d_name[0] == '.' || !ns_have_program(ns_core_for(e->d_name))) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        games_yours[game_count] = (char)yours;
        games_core[game_count] = ns_core_for(e->d_name);
        games[game_count++] = strdup(path);
    }
    closedir(d);
}
static void ns_name(const char *path, char *out, size_t size) {   /* "dir/Nova the Squirrel.nes" -> "Nova the Squirrel" */
    const char *b = strrchr(path, '/');
    char *dot;
    b = b ? b + 1 : path;
    snprintf(out, size, "%s", b);
    dot = strrchr(out, '.');
    if (dot && dot != out && ns_core_for(out)) *dot = 0;
}

/* ---- drawing ---- */
static uint32_t frame[kGameW * kGameH];
static uint32_t game_pic[kGameW * kGameH];   /* the last game picture (behind the menu) */

static void ns_fill(uint32_t c) { unsigned i; for (i = 0; i < (unsigned)(kGameW * kGameH); ++i) frame[i] = c; }
static void ns_dim(void) {
    unsigned i;
    for (i = 0; i < (unsigned)(kGameW * kGameH); ++i) frame[i] = 0xFF000000u | ((game_pic[i] >> 2) & 0x3F3F3Fu);
}
static void ns_text(int x, int y, const char *s, uint32_t colour, int scale) {
    for (; *s; ++s, x += 8 * scale) {
        unsigned char c = (unsigned char)*s;
        int r, k, a, b;
        if (c >= 128) c = '?';
        for (r = 0; r < 8; ++r)
            for (k = 0; k < 8; ++k)
                if (font8x8_basic[c][r] & (1 << k))
                    for (a = 0; a < scale; ++a)
                        for (b = 0; b < scale; ++b) {
                            int px = x + k * scale + b, py = y + r * scale + a;
                            if (px >= 0 && px < kGameW && py >= 0 && py < kGameH) frame[py * kGameW + px] = colour;
                        }
    }
}
static void ns_bar(int y, int h, uint32_t c) {
    int i, j;
    for (j = y; j < y + h && j < kGameH; ++j)
        for (i = 0; i < kGameW; ++i) frame[j * kGameW + i] = c;
}
#define NS_WHITE 0xFFFFFFFFu
#define NS_GREY 0xFF8C97A6u
#define NS_ACCENT 0xFFFF4F1Au
#define NS_BG 0xFF0B0F14u
#define NS_SEL 0xFF263342u

/* ---- the core's callbacks (libretro) ---- */
static unsigned pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static void ns_log_cb(enum retro_log_level level, const char *fmt, ...) {
    char line[200];
    va_list ap;
#ifdef RETRO_CORE_PS1
    static unsigned info_lines;
    if (level < RETRO_LOG_WARN && ++info_lines > 300) return;   /* its start-up steps, not a flood */
#else
    if (level < RETRO_LOG_WARN) return;
#endif
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    line[strcspn(line, "\r\n")] = 0;
    mps_log("game core: %s", line);
}
#ifdef RETRO_CORE_PS1
/* PS1 safe mode (2026-11-20: the first PS1 start on the car crashed in loading and the head unit restarted).
 * ps1-starting.txt is written before a game loads and removed once it has run 10 s, or on a clean exit; found at
 * the next start, the last one crashed. If that one used the dynarec (the fast recompiler), ps1-dynarec-failed.txt
 * records this build, and from then on this build uses the interpreter (slower, simpler). A new build tries the
 * dynarec again. */
static int ns_ps1_safe;
static const char ns_ps1_build[] = __DATE__ " " __TIME__;
static void ns_ps1_marker_path(char *out, size_t size, const char *name) { snprintf(out, size, "%s/%s", save_dir, name); }
static void ns_ps1_before_load(void) {
    char starting[700], failed[700], line[128] = "";
    FILE *f;
    ns_ps1_marker_path(starting, sizeof starting, "ps1-starting.txt");
    ns_ps1_marker_path(failed, sizeof failed, "ps1-dynarec-failed.txt");
    if ((f = fopen(starting, "r")) != 0) {   /* the last start never finished: it crashed */
        if (!fgets(line, sizeof line, f)) line[0] = 0;
        fclose(f);
        line[strcspn(line, "\r\n")] = 0;
        mps_log("game ps1 last_start_crashed mode=%s", line);
        if (!strncmp(line, "dynarec ", 8) && !strcmp(line + 8, ns_ps1_build) && (f = fopen(failed, "w")) != 0) {
            fputs(ns_ps1_build, f);
            fclose(f);
        }
        remove(starting);
    }
    ns_ps1_safe = 0;
    if ((f = fopen(failed, "r")) != 0) {
        if (!fgets(line, sizeof line, f)) line[0] = 0;
        fclose(f);
        line[strcspn(line, "\r\n")] = 0;
        ns_ps1_safe = !strcmp(line, ns_ps1_build);
    }
    if ((f = fopen(starting, "w")) != 0) {
        fprintf(f, "%s %s\n", ns_ps1_safe ? "interpreter" : "dynarec", ns_ps1_build);
        fclose(f);
    }
    mps_log("game ps1 mode=%s build=%s", ns_ps1_safe ? "interpreter (safe: the dynarec crashed before)" : "dynarec", ns_ps1_build);
}
static void ns_ps1_started_ok(void) {
    char starting[700];
    ns_ps1_marker_path(starting, sizeof starting, "ps1-starting.txt");
    if (remove(starting) == 0) mps_log("game ps1 started_ok mode=%s", ns_ps1_safe ? "interpreter" : "dynarec");
}
#endif
static bool ns_environment(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        unsigned f = *(const unsigned *)data;
        if (f != RETRO_PIXEL_FORMAT_RGB565 && f != RETRO_PIXEL_FORMAT_XRGB8888 && f != RETRO_PIXEL_FORMAT_0RGB1555) return false;
        pixfmt = f;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = (struct retro_variable *)data;
        if (v->key && !strcmp(v->key, "fceumm_sndrate_hint")) { v->value = "44KHz"; return true; }  /* the car's rate */
#ifdef RETRO_CORE_PS1
        /* Ask the core to save raster work only when the audio lead is low. */
        if (v->key && !strcmp(v->key,"pcsx_rearmed_frameskip_type")) { v->value="auto"; return true; }
        if (v->key && !strcmp(v->key,"pcsx_rearmed_frameskip_interval")) { v->value="1"; return true; }
        if (v->key && !strcmp(v->key, "pcsx_rearmed_drc")) { v->value = ns_ps1_safe ? "disabled" : "enabled"; return true; }
        if (v->key && !strcmp(v->key, "pcsx_rearmed_show_bios_bootlogo")) { v->value = NS_PS1_BOOTLOGO; return true; }
        if (v->key && !strcmp(v->key, "pcsx_rearmed_pad1type")) { v->value = "dualshock"; return true; }
        if (v->key && !strcmp(v->key, "pcsx_rearmed_memcard2")) { v->value = "disabled"; return true; }
#endif
        return false;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
#ifdef RETRO_CORE_PS1
        *(const char **)data = NS_GAMES_DIR "/yours";   /* the owner's BIOS (scph1001.bin), added with Your games */
        return true;
#endif
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = save_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = ns_log_cb;
        return true;
#ifdef RETRO_CORE_PS1
    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK:
        ns_ps1_buffer_cb=data?((const struct retro_audio_buffer_status_callback *)data)->callback:0;
        return true;
    /* Do not claim or silently add the core's requested extra audio latency. */
    case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY: return false;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        if (!data) return false;
        ns_update_av((const struct retro_system_av_info *)data); return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY: return data != 0;
#endif
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    default:
        return false;
    }
}
#ifdef RETRO_CORE_PS1
static int ns_video_dirty;
#endif
static int ns_skip_video;   /* the core runs this frame without it being drawn (see the pacing below) */
static uint32_t ns_pixel(const uint8_t *row, unsigned x) {
    if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) return ((const uint32_t *)row)[x] | 0xFF000000u;
    {
        unsigned p = ((const uint16_t *)row)[x], r, g, bl;
        if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) { r = (p >> 11) & 31; g = (p >> 5) & 63; bl = p & 31; g = (g << 2) | (g >> 4); }
        else { r = (p >> 10) & 31; g = (p >> 5) & 31; bl = p & 31; g = (g << 3) | (g >> 2); }
        r = (r << 3) | (r >> 2);
        bl = (bl << 3) | (bl >> 2);
        return 0xFF000000u | (r << 16) | (g << 8) | bl;
    }
}
static void ns_video(const void *data, unsigned w, unsigned h, size_t pitch) {
    unsigned x, y, k, m, sc;
    int ox, oy;
    static unsigned last_w, last_h;
    if (!data || !w || !h || ns_skip_video) return;   /* a repeated or skipped frame: keep the last */
#ifdef RETRO_CORE_PS1
    ns_video_dirty = 1;
    {   /* the whole kGameW x kGameH (4:3) picture, nearest pixel; each source row converted once */
        uint64_t video_start=ns_now_ns();
        static unsigned map_w, xmap[FDS_GAME_W];
        static uint32_t line[1024];
        int prev = -1;
        (void)k; (void)m; (void)sc; (void)ox; (void)oy; (void)last_w; (void)last_h;
        if (w > 1024) w = 1024;
        if (map_w != w) {
            for (x = 0; x < (unsigned)kGameW; ++x) xmap[x] = x * w / (unsigned)kGameW;
            map_w = w;
        }
        for (y = 0; y < (unsigned)kGameH; ++y) {
            int sy = (int)(y * h / (unsigned)kGameH);
            uint32_t *out = game_pic + (size_t)y * kGameW;
            if (sy == prev) { memcpy(out, out - kGameW, sizeof(uint32_t) * (size_t)kGameW); continue; }
            {
                const uint8_t *row = (const uint8_t *)data + (size_t)sy * pitch;
                for (x = 0; x < w; ++x) line[x] = ns_pixel(row, x);
            }
            for (x = 0; x < (unsigned)kGameW; ++x) out[x] = line[xmap[x]];
            prev = sy;
        }
        ns_ps1_video_ns += ns_now_ns()-video_start;
        return;
    }
#endif
    sc = (unsigned)kGameW / w < (unsigned)kGameH / h ? (unsigned)kGameW / w : (unsigned)kGameH / h;
    if (sc < 1) sc = 1;
    if (w * sc > (unsigned)kGameW) w = (unsigned)kGameW / sc;
    if (h * sc > (unsigned)kGameH) h = (unsigned)kGameH / sc;
    if (w != last_w || h != last_h) {        /* new size: clear the border around it */
        memset(game_pic, 0, sizeof game_pic);
        for (k = 0; k < (unsigned)(kGameW * kGameH); ++k) game_pic[k] = 0xFF000000u;
        last_w = w;
        last_h = h;
    }
    ox = (kGameW - (int)(w * sc)) / 2;
    oy = (kGameH - (int)(h * sc)) / 2;
    for (y = 0; y < h; ++y) {
        const uint8_t *row = (const uint8_t *)data + y * pitch;
        uint32_t *a = game_pic + (size_t)(oy + (int)(sc * y)) * kGameW + ox;
        for (x = 0; x < w; ++x) {
            uint32_t v;
            if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                v = ((const uint32_t *)row)[x] | 0xFF000000u;
            } else {
                unsigned p = ((const uint16_t *)row)[x], r, g, bl;
                if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) { r = (p >> 11) & 31; g = (p >> 5) & 63; bl = p & 31; g = (g << 2) | (g >> 4); }
                else { r = (p >> 10) & 31; g = (p >> 5) & 31; bl = p & 31; g = (g << 3) | (g >> 2); }
                r = (r << 3) | (r >> 2);
                bl = (bl << 3) | (bl >> 2);
                v = 0xFF000000u | (r << 16) | (g << 8) | bl;
            }
            for (k = 0; k < sc; ++k)
                for (m = 0; m < sc; ++m) a[(size_t)k * kGameW + sc * x + m] = v;
        }
    }
}

/* Sound: the core hands over 44100 Hz stereo; the car audio thread takes it from a ring. */
#define NS_RING 32768u                       /* stereo frames, about 0.74 s (room for catch-up bursts) */
static int16_t ring[NS_RING * 2];
static volatile unsigned ring_w, ring_r;     /* frame counters (wrap) */
static unsigned ring_drops;
static unsigned ring_short;
static unsigned long long sound_in;         /* frames the core handed over (before resampling) */                  /* frames the car's sound asked for that weren't there (gaps) */
/* Pacing (car 2026-11-20: Super Mario Bros. Deluxe ran at 56 of 59.7 fps, so its music broke up): the core
 * keeps real time, because its sound does. When the loop is more than a frame behind the clock the core runs a
 * frame without drawing it (no conversion, no post; a post sometimes takes 200 ms), and catches up after a
 * stall of up to 250 ms. */
static unsigned frames_skipped, run_us_max, post_ms_sum, posts_timed;
static uint64_t run_us_sum, runs_timed;
/* The core's rate may differ from the car's 44100 Hz (Gambatte: 32768): linear resampling on the way in. */
static double rs_step = 1.0, rs_pos = 1.0;   /* input frames per output frame; position since the last input */
static int16_t rs_prev[2];
static void ns_ring_put(int16_t l, int16_t r) {
#ifdef RETRO_CORE_PS1
    ps1_audio_put_locked(&ps1_sound,l,r); return;
#endif
    if (ring_w - ring_r >= NS_RING - 1u) { ++ring_drops; return; }
    ring[(ring_w % NS_RING) * 2] = l;
    ring[(ring_w % NS_RING) * 2 + 1] = r;
    ++ring_w;
}
static void ns_audio_rate(double core_rate) {
    rs_step = core_rate > 1000.0 ? core_rate / 44100.0 : 1.0;
    rs_pos = 1.0;
    rs_prev[0] = rs_prev[1] = 0;
}
static size_t ns_audio_batch(const int16_t *d, size_t frames) {
    size_t i;
#ifdef RETRO_CORE_PS1
    pthread_mutex_lock(&ps1_sound.lock);
    if (!ps1_sound.active) { pthread_mutex_unlock(&ps1_sound.lock); return frames; }
#endif
    sound_in += frames;
    for (i = 0; i < frames; ++i) {
        int16_t l = d[2 * i], r = d[2 * i + 1];
        if (rs_step == 1.0) { ns_ring_put(l, r); continue; }
        while (rs_pos < 1.0) {   /* output frames that fall between the previous input frame and this one */
            ns_ring_put((int16_t)(rs_prev[0] + (l - rs_prev[0]) * rs_pos), (int16_t)(rs_prev[1] + (r - rs_prev[1]) * rs_pos));
            rs_pos += rs_step;
        }
        rs_pos -= 1.0;
        rs_prev[0] = l;
        rs_prev[1] = r;
    }
#ifdef RETRO_CORE_PS1
    pthread_mutex_unlock(&ps1_sound.lock);
#endif
    return frames;
}
#ifdef RETRO_CORE_PS1
static void ns_update_av(const struct retro_system_av_info *av) {
    pthread_mutex_lock(&ps1_sound.lock);
    ns_ps1_period=ps1_frame_period(av->timing.fps);
    ns_audio_rate(av->timing.sample_rate);
    pthread_mutex_unlock(&ps1_sound.lock);
    mps_log("game av fps_milli=%u rate=%u period_ns=%u",(unsigned)(av->timing.fps*1000.0+0.5),
            (unsigned)av->timing.sample_rate,ns_ps1_period);
}
#endif
static void ns_audio_one(int16_t l, int16_t r) { int16_t s[2] = {l, r}; ns_audio_batch(s, 1); }
static void ns_audio_fill(int16_t *stereo, unsigned frames) {
    unsigned i;
#ifdef RETRO_CORE_PS1
    ps1_audio_take(&ps1_sound,stereo,frames); return;
#endif
    for (i = 0; i < frames; ++i) {
        if (ring_r != ring_w) {
            stereo[2 * i] = ring[(ring_r % NS_RING) * 2];
            stereo[2 * i + 1] = ring[(ring_r % NS_RING) * 2 + 1];
            ++ring_r;
        } else {
            stereo[2 * i] = stereo[2 * i + 1] = 0;
            ++ring_short;
        }
    }
}
#ifdef RETRO_CORE_PS1
static void ns_report_audio(void) {
    unsigned depth,active;
    pthread_mutex_lock(&ps1_sound.lock);
    depth=ps1_sound.write-ps1_sound.read;
    active=ps1_sound.active && ca_started && !ca_done;
    pthread_mutex_unlock(&ps1_sound.lock);
    if(ns_ps1_buffer_cb) {
        unsigned occupancy=depth>=PS1_AUDIO_LEAD?100u:depth*100u/PS1_AUDIO_LEAD;
        int low=depth<1024u;
        ++ns_ps1_buffer_reports;
        if(active && low) ++ns_ps1_low_reports;
        ns_ps1_buffer_cb(active!=0,occupancy,active && low);
    }
}
#endif
static void ns_input_poll(void) {
#ifdef RETRO_CORE_PS1
    ns_poll();
    if (pad_msgs != ns_ps1_input_seen) {
        uint64_t now=mps_now_ms();
        if (ns_ps1_input_stamp && now>=ns_ps1_input_stamp && now-ns_ps1_input_stamp<10000u &&
            now-ns_ps1_input_stamp>ns_ps1_input_max_ms) ns_ps1_input_max_ms=(unsigned)(now-ns_ps1_input_stamp);
        ns_ps1_input_seen=pad_msgs;
    }
#endif
}
static int16_t ns_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0) return 0;
#ifdef RETRO_CORE_PS1
    if (ns_ps1_muted_input) {
        if (!pad.buttons && pad.lt<30 && pad.rt<30 && abs(pad.lx)<8000 && abs(pad.ly)<8000 && abs(pad.rx)<8000 && abs(pad.ry)<8000)
            ns_ps1_muted_input=0;
        else return 0;
    }
    if ((device & RETRO_DEVICE_MASK) == RETRO_DEVICE_ANALOG) return ps1_control_axis(&pad,ns_ps1_profile,index,id);
    if ((device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD || id > 15) return 0;
    if (ns_ps1_profile == PS1_CUSTOM) return (int16_t)((rm_buttons(&NS_RM_SYS,&ns_map,&pad,ns_retropad_ps1(&pad)) >> id) & 1u);
    return (int16_t)((ps1_control_buttons(&pad,ns_ps1_profile) >> id) & 1u);
#else
    (void)index;
    if (device != RETRO_DEVICE_JOYPAD || id > 15) return 0;
    return (int16_t)(((ns_map.active ? rm_buttons(&NS_RM_SYS, &ns_map, &pad, ns_retropad(&pad)) : ns_retropad(&pad)) >> id) & 1u);
#endif
}

/* ---- saves ---- */
static void ns_save_path(char *out, size_t size, const char *ext) {
    char name[256];
#ifdef RETRO_CORE_PS1
    if (!strcmp(ext, "srm")) { snprintf(out, size, "%s/PS1 memory card.srm", save_dir); return; }
#endif
    ns_name(games[current], name, sizeof name);
    snprintf(out, size, "%s/%s.%s", save_dir, name, ext);
}
static int ns_write(const char *path, const void *data, size_t size) {
    char tmp[800];
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.new", path);
    f = fopen(tmp, "wb");
    if (!f) return 0;
    if (fwrite(data, 1, size, f) != size) { fclose(f); remove(tmp); return 0; }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return rename(tmp, path) == 0;
}
static void *ns_read(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    long n;
    void *data;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = n > 0 ? malloc((size_t)n) : 0;
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = 0; }
    fclose(f);
    if (data) *size = (size_t)n;
    return data;
}
/* Battery save (cartridges with saving RAM): loaded with the game, written when leaving it. */
static void ns_sram(int write) {
    char path[800];
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM), got = 0;
    void *mem = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM), *data;
    if (!game_loaded || !mem || !size) return;
    ns_save_path(path, sizeof path, "srm");
    if (write) {
        if (!ns_write(path, mem, size)) mps_log("game warning=battery_save_failed");
    } else if ((data = ns_read(path, &got)) != 0) {
        if (got == size) memcpy(mem, data, size);
        free(data);
    }
}
/* Save states are big (a PS1 one is about 4-5 MB) and go on the car's flash (/var, ~492 MiB, shared with the
 * iDrive and every app): one is only written if NS_FLASH_RESERVE stays free after it (owner, 2026-11-20).
 * Memory-card and battery saves are tiny and never refused. */
#ifndef NS_FLASH_RESERVE
#define NS_FLASH_RESERVE (100ull << 20)
#endif
static int ns_room_for(unsigned long long bytes) {
    struct statvfs v;
    unsigned long long free_bytes;
    if (statvfs(save_dir, &v) != 0) return 1;   /* can't tell: don't block (the write itself still reports) */
    free_bytes = (unsigned long long)v.f_bavail * (unsigned long long)(v.f_frsize ? v.f_frsize : v.f_bsize);
    return free_bytes >= bytes + NS_FLASH_RESERVE;
}
/* 1 done, 0 failed (or nothing to load), -1 refused: not enough room on the flash. */
static int ns_state(int save) {
    char path[800];
    size_t size = retro_serialize_size(), got = 0;
    void *data;
    int ok = 0;
    if (!game_loaded || !size) return 0;
    ns_save_path(path, sizeof path, "state");
    if (save) {
        if (!ns_room_for(size)) {
            mps_log("game state_save refused=flash_low bytes=%u", (unsigned)size);
            return -1;
        }
        data = malloc(size);
        if (data && retro_serialize(data, size)) ok = ns_write(path, data, size);
        free(data);
    } else if ((data = ns_read(path, &got)) != 0) {
        ok = retro_unserialize(data, got);
        free(data);
    }
    mps_log("game state_%s ok=%d", save ? "save" : "load", ok);
    return ok;
}

static void ns_unload(void) {
    if (!game_loaded) return;
#ifdef RETRO_CORE_PS1
    ns_ps1_started_ok();
    ps1_audio_active(&ps1_sound,0);
#endif
    ns_sram(1);
    retro_unload_game();
#ifdef RETRO_CORE_PS1
    ns_ps1_buffer_cb=0;
#endif
    game_loaded = 0;
}

static void ns_cleanup(void) {
    if (cleaned) return;
    cleaned = 1;
#ifdef __QNX__
    stop_flag = 1;
#endif
    ns_unload();
    ca_stop();
#ifdef RETRO_CORE_PS1
    ring_drops=ps1_sound.drops; ring_short=ps1_sound.gaps;
    mps_log("game adaptive_audio reports=%u low=%u core_skip=auto max_consecutive=1",ns_ps1_buffer_reports,ns_ps1_low_reports);
    mps_log("game latency input_queue_max_ms=%u duplicate_posts_saved=%u audio_waits=%u audio_high_frames=%u period_ns=%u",
            ns_ps1_input_max_ms,ns_ps1_dupes,ns_ps1_full_waits,ps1_sound.high,ns_period());
#endif
    mps_log("game end frames=%u posts_failed=%u post_max_ms=%u pad_msgs=%u seconds=%u games=%u loud_blocks=%u drops=%u",
            screen.frame, screen.posts_failed, screen.max_post_ms, pad_msgs,
            (unsigned)((mps_now_ms() - start_ms) / 1000u), games_started, ca_loud_blocks, ring_drops);
    mps_log("game timing core_runs=%llu run_avg_us=%llu run_max_us=%u draw_avg_ms=%u skipped=%u sound_gap_frames=%u sound_in=%llu",
            (unsigned long long)runs_timed, (unsigned long long)(runs_timed ? run_us_sum / runs_timed : 0), run_us_max,
            posts_timed ? post_ms_sum / posts_timed : 0, frames_skipped, ring_short, sound_in);
    fds_close(&screen);
    if (ctl >= 0) close(ctl);
    ctl = -1;
}

/* Control port: pad states and "stop"; signals and a lost parent end the game too. */
static void ns_poll(void) {
    char msg[128];
    ssize_t got;
    while (ctl >= 0 && (got = recv(ctl, msg, sizeof msg - 1, 0)) > 0) {
        msg[got] = 0;
        if (!strncmp(msg, "stop", 4)) {
            mps_log("game stop reason=page");
            exit(0);
        }
        if (xpad_parse(msg, &pad)) {
            if (!pad_msgs) mps_log("game pad_input first=%s", msg);
            ++pad_msgs;
#ifdef RETRO_CORE_PS1
            { const char *stamp=strstr(msg," t="); if (stamp) ns_ps1_input_stamp=strtoull(stamp+3,0,10); }
#endif
        }
    }
    if (mps_stop_flag) {
        mps_log("game stop reason=signal");
        exit(0);
    }
#ifndef NS_TEST
    if (mps_now_ms() - last_parent_check >= 1000u) {
        last_parent_check = mps_now_ms();
        if (getppid() <= 1) {
            mps_log("game stop reason=parent_gone");
            exit(0);
        }
    }
#endif
}

/* Menu input: a press is reported once, and repeats after 0.4 s while held (lists). */
static uint32_t nav_held;
static uint64_t nav_repeat_at;
static uint32_t ns_nav_pressed(void) {
    uint32_t now = ns_nav(&pad), fresh = now & ~nav_held;
    uint64_t t = mps_now_ms();
    if (fresh) nav_repeat_at = t + 400u;
    else if ((now & (NS_UP | NS_DOWN)) && t >= nav_repeat_at) { fresh = now & (NS_UP | NS_DOWN); nav_repeat_at = t + 120u; }
    nav_held = now;
    return fresh;
}

/* ---- screens ---- */
static void ns_draw_picker(unsigned cursor, const char *note) {
    unsigned i, first = cursor >= 9 ? cursor - 8 : 0;
    char name[64];
    ns_fill(NS_BG);
    ns_text(24, 20, "RETRO", NS_ACCENT, 3);
    ns_text(160, 28, "NES  GAME BOY", NS_GREY, 2);
    if (!game_count) {
        ns_text(24, 120, "No games found.", NS_WHITE, 2);
        ns_text(24, 150, "Run the games installer again.", NS_GREY, 2);
    }
    for (i = first; i < game_count && i < first + 9; ++i) {
        int y = 90 + (int)(i - first) * 34;
        if (games_yours[i]) { name[0] = '*'; name[1] = ' '; ns_name(games[i], name + 2, sizeof name - 2); }
        else ns_name(games[i], name, sizeof name);
        name[29] = 0;
        if (i == cursor) ns_bar(y - 8, 32, NS_SEL);
        ns_text(32, y, name, i == cursor ? NS_WHITE : NS_GREY, 2);
    }
    ns_text(24, 412, "A: play   * = your own games", NS_GREY, 2);
    ns_text(24, 440, note ? note : "BACK on the iDrive ends", NS_GREY, 2);
    fds_frame(&screen, frame);
}

static const char *const pause_items[] = {"Resume", "Save state", "Load state", "Reset game", "Choose another game", "Change buttons"};
#ifdef RETRO_CORE_PS1
#define NS_PAUSE_ITEMS 7   /* 5: the Controls layout, 6: Change buttons */
#else
#define NS_PAUSE_ITEMS 6
#endif
static void ns_draw_pause(unsigned cursor, const char *note) {
    unsigned i;
    char name[64];
    ns_dim();
    ns_name(games[current], name, sizeof name);
    name[29] = 0;
    ns_text(24, 40, name, NS_ACCENT, 2);
    for (i = 0; i < NS_PAUSE_ITEMS; ++i) {
#ifdef RETRO_CORE_PS1
        int y = 90 + (int)i * 34;
#else
        int y = 110 + (int)i * 40;
#endif
        if (i == cursor) ns_bar(y - 8, 32, NS_SEL);
#ifdef RETRO_CORE_PS1
        ns_text(48,y,i==5?ps1_control_name(ns_ps1_profile):pause_items[i==6?5:i],i==cursor?NS_WHITE:NS_GREY,2);
#else
        ns_text(48, y, pause_items[i], i == cursor ? NS_WHITE : NS_GREY, 2);
#endif
    }
#ifdef RETRO_CORE_PS1
    if(ns_ps1_profile==PS1_SHOOTER) {
        ns_text(24,320,"LS: move  RS: look  RT: fire",NS_GREY,2);
        ns_text(24,342,"A: jump B: crouch X: reload/use",NS_GREY,2);
        ns_text(24,364,"Y: weapon  LT: aim",NS_GREY,2);
    } else if(ns_ps1_profile==PS1_CUSTOM) {
        ns_text(24,320,"Your own buttons for this game.",NS_GREY,2);
        ns_text(24,342,"Change buttons: edit them",NS_GREY,2);
        ns_text(24,364,"Controls: the ready-made ones",NS_GREY,2);
    } else if(ns_ps1_profile==PS1_ADVENTURE) {
        ns_text(24,320,"A: jump X/RT: action/fire Y: draw",NS_GREY,2);
        ns_text(24,342,"B: roll LT/RS: look LB: walk",NS_GREY,2);
        ns_text(24,364,"RB: sprint LS click: crouch",NS_GREY,2);
    } else {
        ns_text(24,320,"A: Cross B: Circle X: Square",NS_GREY,2);
        ns_text(24,342,"Y: Triangle LB/RB: L1/R1",NS_GREY,2);
        ns_text(24,364,"LT/RT: L2/R2  sticks: native",NS_GREY,2);
    }
    if(note) ns_text(24,406,note,NS_WHITE,2);
#else
    if (note) ns_text(48, 372, note, NS_WHITE, 2);
#endif
    ns_text(24, 440, "A: choose   B: back to game", NS_GREY, 2);
    fds_frame(&screen, frame);
}

/* The chosen game belongs to the other emulator: take our picture and sound down cleanly and become it. */
static void ns_exec_program(const char *program, const char *a1, const char *a2) {
    char other[700];
    char *slash;
    char *args[4];
    snprintf(other, sizeof other, "%s", self_path);
    slash = strrchr(other, '/');
    if (slash) snprintf(slash + 1, sizeof other - (size_t)(slash + 1 - other), "%s", program);
    else snprintf(other, sizeof other, "%s", program);
    mps_log("game handoff to=%s args=%s %s", program, a1, a2 ? a2 : "");
    ns_unload();
    ca_stop();
    fds_close(&screen);
    if (ctl >= 0) close(ctl);
    ctl = -1;
    cleaned = 1;
#ifdef __QNX__
    /* The other emulator opens the USB pad itself, so ours must let go of it first: exec keeps this process
     * and its USB connection, and the pad then refused the new one as busy (car, 2026-11-20: attach_failed
     * error=16, no controls in Game Boy games). Stop the pad thread and wait for it to close the pad (2 s at
     * most), and let exec close anything else still open. */
    stop_flag = 1;
    if (pad_started) {
        unsigned waited = 0;
        while (!pad_done && waited < 2000u) { mps_sleep_ms(10); waited += 10u; }
        if (pad_done) pthread_join(pad_thread, 0);
        mps_log("game handoff pad_released=%s waited_ms=%u", pad_done ? "yes" : "no", waited);
        pad_started = 0;
    }
#endif
    {
        int fd;
        for (fd = 3; fd < 256; ++fd) (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    args[0] = other;
    args[1] = (char *)a1;
    args[2] = (char *)a2;
    args[3] = 0;
#ifndef NS_TEST
    execv(other, args);
#else
    (void)args;
#endif
    mps_log("game fail step=handoff errno=%d path=%s", errno, other);
    exit(1);
}
/* The chosen game belongs to another emulator: become it, straight into the game. */
static void ns_handoff(unsigned index) { ns_exec_program(ns_program(games_core[index]), "--play-rom", games[index]); }
/* Game Room (2026-11-20): the emulators live in its folder next to freedoom-qnx, which shows the one list of all
 * the games; "Choose another game" goes back to that list. Without it (Retro on its own) our own picker opens. */
static int ns_in_game_room(void) {
    char path[700], *slash;
    snprintf(path, sizeof path, "%s", self_path);
    slash = strrchr(path, '/');
    if (!slash) return 0;
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "freedoom-qnx");
    return access(path, X_OK) == 0;
}

static int ns_start_game(unsigned index) {
    struct retro_game_info info;
    struct retro_system_av_info av;
    size_t size = 0;
    void *data;
    char name[256];
    current = index;
    {
        struct retro_system_info si;
        memset(&si, 0, sizeof si);
        retro_get_system_info(&si);
        if (si.need_fullpath) {
            struct stat st;
            memset(&info, 0, sizeof info);
            info.path = games[index];
            if (stat(games[index], &st) == 0) size = (size_t)st.st_size;
#ifdef RETRO_CORE_PS1
            ns_ps1_before_load();
#endif
            mps_log("game load_begin path=%s bytes=%u", games[index], (unsigned)size);
            if (!retro_load_game(&info)) {
                ns_name(games[index], name, sizeof name);
                mps_log("game fail step=load_rom name=%s bytes=%u", name, (unsigned)size);
                return 0;
            }
            data = 0;
            goto loaded;
        }
    }
    data = ns_read(games[index], &size);
    if (!data) {
        mps_log("game fail step=read_rom path=%s", games[index]);
        return 0;
    }
    memset(&info, 0, sizeof info);
    info.path = games[index];
    info.data = data;
    info.size = size;
    if (!retro_load_game(&info)) {
        free(data);
        ns_name(games[index], name, sizeof name);
        mps_log("game fail step=load_rom name=%s bytes=%u", name, (unsigned)size);
        return 0;
    }
    free(data);
loaded:
#ifdef RETRO_CORE_PS1
    retro_set_controller_port_device(0, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1));   /* DualShock */
#endif
    game_loaded = 1;
    ns_sram(0);
    rm_load(&NS_RM_SYS, save_dir, games[index], &ns_map);
    mps_log("game buttons layout=%s", ns_map.active ? "own" : "default");
    retro_get_system_av_info(&av);
#ifdef RETRO_CORE_PS1
    ns_update_av(&av);
    { char path[600]; unsigned saved; FILE *f;
      ns_ps1_profile=ps1_default_controls(games[index]); ns_profile_path(path,sizeof path);
      f=fopen(path,"r"); if(f) { if(fscanf(f,"%u",&saved)==1 && saved<=PS1_CUSTOM) ns_ps1_profile=saved; fclose(f); }
      if(ns_ps1_profile==PS1_CUSTOM && !ns_map.active) ns_ps1_profile=ps1_default_controls(games[index]);
      mps_log("game controls profile=%s",ps1_control_name(ns_ps1_profile));
    }
    ns_ps1_muted_input=1;
    ps1_audio_active(&ps1_sound,1);
#else
    ns_audio_rate(av.timing.sample_rate);
#endif
    ns_name(games[index], name, sizeof name);
    mps_log("game rom=%s bytes=%u fps=%.3f rate=%.0f size=%ux%u", name, (unsigned)size, av.timing.fps,
            av.timing.sample_rate, av.geometry.base_width, av.geometry.base_height);
    ++games_started;
    {   /* remember it: the picker opens on the last game played */
        char path[600];
        FILE *f;
        snprintf(path, sizeof path, "%s/last.txt", save_dir);
        f = fopen(path, "w");
        if (f) { fputs(name, f); fclose(f); }
    }
#ifdef RETRO_CORE_PS1
    { unsigned pixel; for(pixel=0;pixel<(unsigned)(kGameW*kGameH);++pixel) game_pic[pixel]=0xff000000u; }
#else
    memset(game_pic, 0, sizeof game_pic);
#endif
    return 1;
}

static unsigned ns_last_index(void) {
    char path[600], last[256] = "", name[256];
    FILE *f;
    unsigned i;
    snprintf(path, sizeof path, "%s/last.txt", save_dir);
    f = fopen(path, "r");
    if (!f) return 0;
    if (fgets(last, sizeof last, f)) last[strcspn(last, "\r\n")] = 0;
    fclose(f);
    for (i = 0; i < game_count; ++i) {
        ns_name(games[i], name, sizeof name);
        if (!strcmp(name, last)) return i;
    }
    return 0;
}

/* The picker, the game and the menu. max_ms > 0: the host test's limit. */
static const char *ns_start_rom;   /* --play-rom: the game to start straight away */
static void ns_loop(unsigned max_ms) {
    enum { PICK, PLAY, PAUSE, REMAP } mode = PICK;
    unsigned cursor = ns_last_index(), pause_cursor = 0;
    const char *note = 0;
#ifndef RETRO_CORE_PS1
    double fps = 60.0, next_f = 0.0;           /* next_f: when the next core frame is due (ms, fractional) */
#endif
    unsigned skips_in_row = 0;
#ifdef RETRO_CORE_PS1
    ps1_pacer pacer;
    uint64_t perf_at=0,perf_runs=0,perf_posts=0;
    unsigned force_post=1;
    ps1_pacer_reset(&pacer,ns_now_ns(),ns_period());
#endif
    uint64_t next = mps_now_ms(), note_until = 0;
    int dirty = 1;
    if (ns_start_rom) {   /* a hand-over: find the game and start it, as if A had been pressed on it */
        unsigned k;
        for (k = 0; k < game_count; ++k)
            if (!strcmp(games[k], ns_start_rom) && games_core[k] == NS_CORE && ns_start_game(k)) {
                struct retro_system_av_info av;
                retro_get_system_av_info(&av);
#ifndef RETRO_CORE_PS1
                fps = av.timing.fps > 1.0 ? av.timing.fps : 60.0;
#endif
                cursor = k;
                mode = PLAY;
#ifdef RETRO_CORE_PS1
                ps1_pacer_reset(&pacer,ns_now_ns(),ns_period()); force_post=1;
#endif
                next = mps_now_ms();
                nav_held = ns_nav(&pad);
                break;
            }
    }
    while (!max_ms || mps_now_ms() - start_ms < max_ms) {
        uint32_t nav;
        ns_poll();
#ifdef RETRO_CORE_PS1
        if(mode==PLAY && mps_now_ms()>=perf_at) {
            unsigned depth,gaps;
            pthread_mutex_lock(&ps1_sound.lock); depth=ps1_sound.write-ps1_sound.read; gaps=ps1_sound.gaps; pthread_mutex_unlock(&ps1_sound.lock);
            mps_log("game perf interval_ms=%u runs=%u posts=%u audio_depth=%u gap_frames=%u waits=%u late_resets=%u",
                    perf_at?(unsigned)(mps_now_ms()-(perf_at-5000u)):0u,(unsigned)(runs_timed-perf_runs),(unsigned)(posts_timed-perf_posts),depth,gaps,ns_ps1_full_waits,pacer.late);
            perf_at=mps_now_ms()+5000u; perf_runs=runs_timed; perf_posts=posts_timed;
        }
#endif
        nav = ns_nav_pressed();
        if (note && mps_now_ms() > note_until) { note = 0; dirty = 1; }
        if (mode == PICK) {
            if ((nav & NS_DOWN) && cursor + 1 < game_count) { ++cursor; dirty = 1; }
            if ((nav & NS_UP) && cursor > 0) { --cursor; dirty = 1; }
            if ((nav & NS_OK) && game_count && games_core[cursor] != NS_CORE) ns_handoff(cursor);
            if ((nav & NS_OK) && game_count) {
                if (ns_start_game(cursor)) {
                    struct retro_system_av_info av;
                    retro_get_system_av_info(&av);
    #ifndef RETRO_CORE_PS1
                fps = av.timing.fps > 1.0 ? av.timing.fps : 60.0;
#endif
                    mode = PLAY;
#ifdef RETRO_CORE_PS1
                    ps1_pacer_reset(&pacer,ns_now_ns(),ns_period()); force_post=1;
#endif
                    next = mps_now_ms();
                    nav_held = ns_nav(&pad);   /* the A that started the game is not a menu press */
                } else {
                    note = "That game would not start.";
                    note_until = mps_now_ms() + 3000u;
                    dirty = 1;
                }
            }
            if (dirty) { ns_draw_picker(cursor, note); dirty = 0; }
            mps_sleep_ms(30);
            continue;
        }
        if (mode == PLAY) {
#ifndef RETRO_CORE_PS1
            uint64_t now = mps_now_ms();
#endif
            if (nav & NS_MENU) {
                mode = PAUSE;
#ifdef RETRO_CORE_PS1
                ps1_audio_active(&ps1_sound,0);
#endif
                pause_cursor = 0;
                dirty = 1;
                continue;
            }
#ifdef RETRO_CORE_PS1
            {
                uint64_t t=ns_now_ns(); unsigned period=ns_period();
                if(pacer.period!=period) ps1_pacer_reset(&pacer,t,period);
                if(ca_started && !ca_done && ps1_audio_depth(&ps1_sound)>=PS1_AUDIO_LEAD) {
                    ++ns_ps1_full_waits; ps1_pacer_reset(&pacer,t,period); mps_sleep_ms(2); continue;
                }
                if(t<pacer.due) { mps_sleep_ms((unsigned)((pacer.due-t)/1000000u>2?2:(pacer.due-t)/1000000u)+1u); continue; }
            }
#else
            if (next_f < (double)next) next_f = (double)next;   /* (re)started: begin from the plain clock */
            if (now < next) { mps_sleep_ms((unsigned)(next - now > 5 ? 5 : next - now)); continue; }
#endif
            {
                struct timespec a, b;
                unsigned us;
                /* behind the clock: run without drawing (never more than 3 frames in a row, so the picture moves) */
#ifdef RETRO_CORE_PS1
                ns_skip_video=ns_now_ns()>pacer.due+pacer.period && skips_in_row<2;
                ns_video_dirty=0; ns_ps1_video_ns=0;
                ns_report_audio();
#else
                ns_skip_video = (double)now > next_f + 1000.0 / fps && skips_in_row < 3;
#endif
#ifdef RETRO_CORE_PS1
                uint64_t cpu_start=ns_cpu_ns(),cpu_end;
#endif
                clock_gettime(CLOCK_MONOTONIC, &a);
                retro_run();
                clock_gettime(CLOCK_MONOTONIC, &b);
#ifdef RETRO_CORE_PS1
                cpu_end=ns_cpu_ns();
#endif
#ifdef RETRO_CORE_PS1
                if (runs_timed == 0) mps_log("game first_frame_run");
                if (runs_timed == 600) ns_ps1_started_ok();
#endif
                us = (unsigned)((b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000L);
#ifdef RETRO_CORE_PS1
                if(us>=100000u && ns_ps1_stall_logs<16u) {
                    ++ns_ps1_stall_logs;
                    mps_log("game stall wall_us=%u cpu_us=%u cpu_valid=%u video_us=%u audio_depth=%u",us,
                        cpu_start&&cpu_end>=cpu_start?(unsigned)((cpu_end-cpu_start)/1000u):0u,cpu_start!=0 && cpu_end>=cpu_start,
                        (unsigned)(ns_ps1_video_ns/1000u),ps1_audio_depth(&ps1_sound));
                }
#endif
                run_us_sum += us;
                ++runs_timed;
                if (us > run_us_max) run_us_max = us;
                if (ns_skip_video) {
                    ++frames_skipped;
                    ++skips_in_row;
#ifdef RETRO_CORE_PS1
                } else if(!ns_video_dirty && !force_post) { ++ns_ps1_dupes; skips_in_row=0;
#endif
                } else {
                    uint64_t t0 = mps_now_ms();
                    fds_frame(&screen, game_pic);
#ifdef RETRO_CORE_PS1
                    force_post=0;
#endif
                    post_ms_sum += (unsigned)(mps_now_ms() - t0);
                    ++posts_timed;
                    skips_in_row = 0;
                }
                ns_skip_video = 0;
            }
#ifdef RETRO_CORE_PS1
            ps1_pacer_step(&pacer,ns_now_ns());
#else
            next_f += 1000.0 / fps;
            if ((double)now > next_f + 250.0) next_f = (double)now;   /* fell far behind: don't race to catch up */
            next = (uint64_t)next_f;
#endif
            continue;
        }
        if (mode == REMAP) {   /* "Change buttons": its own screen until Done, then saved and back to the menu */
            if (rm_step(&ns_rm, &pad, mps_now_ms()) == RM_DONE) {
                int ok;
                ns_map = ns_rm.map;
                ok = rm_save(&NS_RM_SYS, save_dir, games[current], &ns_map);
#ifdef RETRO_CORE_PS1
                if (ns_map.active) ns_ps1_profile = PS1_CUSTOM;
                else if (ns_ps1_profile == PS1_CUSTOM) ns_ps1_profile = ps1_default_controls(games[current]);
                if (!ns_ps1_save_profile()) ok = 0;
#endif
                note = !ok ? "Could not save the buttons." : ns_map.active ? "Buttons saved for this game." : "Default buttons.";
                note_until = mps_now_ms() + 2500u;
                mps_log("game buttons layout=%s saved=%d", ns_map.active ? "own" : "default", ok);
                mode = PAUSE;
                dirty = 1;
                continue;
            }
            if (ns_rm.dirty) { rm_draw(&ns_rm, frame, kGameW, kGameH); fds_frame(&screen, frame); ns_rm.dirty = 0; }
            mps_sleep_ms(30);
            continue;
        }
        /* PAUSE */
        if ((nav & NS_DOWN) && pause_cursor + 1 < NS_PAUSE_ITEMS) { ++pause_cursor; dirty = 1; }
        if ((nav & NS_UP) && pause_cursor > 0) { --pause_cursor; dirty = 1; }
        if (nav & (NS_BACK | NS_MENU)) { mode = PLAY; next = mps_now_ms();
#ifdef RETRO_CORE_PS1
                ps1_pacer_reset(&pacer,ns_now_ns(),ns_period()); ps1_audio_active(&ps1_sound,1); force_post=1; ns_ps1_muted_input=1;
#endif
                continue; }
        if (nav & NS_OK) {
            switch (pause_cursor) {
            case 0: mode = PLAY; next = mps_now_ms();
#ifdef RETRO_CORE_PS1
                ps1_pacer_reset(&pacer,ns_now_ns(),ns_period()); ps1_audio_active(&ps1_sound,1); force_post=1; ns_ps1_muted_input=1;
#endif
                continue;
            case 1: {
                int r = ns_state(1);
                note = r > 0 ? "Saved." : r < 0 ? "Not enough space on the car for a save state." : "Could not save.";
                break;
            }
            case 2: note = ns_state(0) > 0 ? "Loaded." : "No save state yet."; break;
            case 3: retro_reset(); mode = PLAY; next = mps_now_ms();
#ifdef RETRO_CORE_PS1
                ps1_pacer_reset(&pacer,ns_now_ns(),ns_period()); ps1_audio_active(&ps1_sound,1); force_post=1; ns_ps1_muted_input=1;
#endif
                continue;
#ifdef RETRO_CORE_PS1
            case 5: {   /* the ready-made layouts, and "Your own buttons" once there are some */
                ns_ps1_profile=(ns_ps1_profile+1u)%(ns_map.active?4u:3u);
                note=ns_ps1_save_profile()?"Controls saved for this game.":"Controls changed; could not save.";
                ns_ps1_muted_input=1; mps_log("game controls profile=%s",ps1_control_name(ns_ps1_profile));
                break;
            }
            case 6:
#else
            case 5:
#endif
                rm_open(&ns_rm, &NS_RM_SYS, &ns_map, games[current], &pad);
                mode = REMAP;
                continue;
            case 4:
                if (ns_in_game_room()) ns_exec_program("freedoom-qnx", "--play", 0);   /* back to the Game Room list */
                ns_unload(); mode = PICK; cursor = current; dirty = 1; continue;
            }
            note_until = mps_now_ms() + 2000u;
            dirty = 1;
        }
        if (dirty) { ns_draw_pause(pause_cursor, note); dirty = 0; }
        mps_sleep_ms(30);
    }
}

/* ---- start-up ---- */
static int ns_bind(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    if (fd < 0) return -1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(NS_PORT);
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

#ifdef __QNX__
static void *ns_pad_main(void *arg) {
    (void)arg;
    pad_run(0, NS_PORT, 0);
    pad_done = 1;
    return 0;
}
#endif

static int ns_play(unsigned max_ms) {
    char cwd[400];
    start_ms = mps_now_ms();
    last_parent_check = start_ms;
    ctl = ns_bind();
    if (ctl < 0) {
        mps_log("game refused reason=already_running");
        return 3;
    }
    if (NS_SAVE_DIR[0] == '/' || !getcwd(cwd, sizeof cwd)) snprintf(save_dir, sizeof save_dir, "%s", NS_SAVE_DIR);
    else snprintf(save_dir, sizeof save_dir, "%s/%s", cwd, NS_SAVE_DIR);
    if (mkdir(save_dir, 0755) != 0 && errno != EEXIST) mps_log("game warning=save_dir_create errno=%d", errno);
    ns_scan(NS_GAMES_DIR, 0);
    ns_scan(NS_GAMES_DIR "/yours", 1);   /* the owner's own games (added with the games installer) */
    ns_scan(NS_DATA_DIR "/games", 0);    /* a copy in the plugin folder, if any */
    {   /* sort by name, keeping each game's "yours" mark with it */
        unsigned i, j;
        for (i = 1; i < game_count; ++i)
            for (j = i; j > 0 && ns_cmp(&games[j - 1], &games[j]) > 0; --j) {
                char *t = games[j]; char y = games_yours[j], c = games_core[j];
                games[j] = games[j - 1]; games_yours[j] = games_yours[j - 1]; games_core[j] = games_core[j - 1];
                games[j - 1] = t; games_yours[j - 1] = y; games_core[j - 1] = c;
            }
    }
#ifdef __QNX__
    {
        struct sched_param p;
        int policy;
        if (pthread_getschedparam(pthread_self(), &policy, &p) == 0) {
            int was = p.sched_priority;
            p.sched_priority = NS_PRIORITY;
            mps_log("game priority was=%d now=%d rc=%d", was, NS_PRIORITY, pthread_setschedparam(pthread_self(), policy, &p));
        }
        pad_started = pthread_create(&pad_thread, 0, ns_pad_main, 0) == 0;
        mps_log("game pad_thread=%s port=%d", pad_started ? "started" : "failed", NS_PORT);
    }
#endif
    atexit(ns_cleanup);
    mps_log("game begin games=%u dir=%s saves=%s", game_count, NS_GAMES_DIR, save_dir);
    if (!fds_open(&screen, &api, runner)) return 5;
    retro_set_environment(ns_environment);
    retro_set_video_refresh(ns_video);
    retro_set_audio_sample(ns_audio_one);
    retro_set_audio_sample_batch(ns_audio_batch);
    retro_set_input_poll(ns_input_poll);
    retro_set_input_state(ns_input_state);
    retro_init();
    ca_start(ns_audio_fill);
    ns_loop(max_ms);
    ns_cleanup();
    return 0;
}

#ifndef NS_TEST
static int load_screen(mps_api *s) {
    static const char *const paths[] = {"libscreen.so.1", "/lib/libscreen.so.1", "/fs/sda0/lib/libscreen.so.1",
                                        "/usr/lib/libscreen.so.1"};
    void *lib = 0;
    int missing = 0;
    unsigned i;
    for (i = 0; i < sizeof paths / sizeof paths[0] && !lib; ++i) lib = dlopen(paths[i], RTLD_NOW);
    if (!lib) {
        const char *why = dlerror();
        mps_log("game fail step=libscreen_open error=%s", why ? why : "?");
        return 0;
    }
#define BIND(field, name) do { void *f_ = dlsym(lib, name); if (!f_) { mps_log("game fail step=libscreen_symbol name=%s", name); ++missing; } memcpy(&s->field, &f_, sizeof f_); } while (0)
    BIND(create_context, "screen_create_context");
    BIND(destroy_context, "screen_destroy_context");
    BIND(create_window_type, "screen_create_window_type");
    BIND(destroy_window, "screen_destroy_window");
    BIND(create_window_buffers, "screen_create_window_buffers");
    BIND(set_window_iv, "screen_set_window_property_iv");
    BIND(set_window_cv, "screen_set_window_property_cv");
    BIND(get_window_pv, "screen_get_window_property_pv");
    BIND(get_buffer_pv, "screen_get_buffer_property_pv");
    BIND(post_window, "screen_post_window");
#undef BIND
    return missing == 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--hmi-trace")) return hmi_trace_sink();
    if (argc == 2 && !strcmp(argv[1], "--restore")) {
        alarm(20);
        mps_restore(mps_popen_runner);
        ca_release_source();
        return 0;
    }
    if ((argc == 2 && !strcmp(argv[1], "--play")) || (argc == 3 && !strcmp(argv[1], "--play-rom"))) {
        struct sigaction a;
        snprintf(self_path, sizeof self_path, "%s", argv[0]);
        if (argc == 3) ns_start_rom = argv[2];
        if (!getenv("NS_ENV_OK")) {   /* once, with the car's media libraries on the path (libasound, audioconn) */
            static char lib_path[] = "LD_LIBRARY_PATH=/opt/mm/lib:/opt/mm/lib/libasound:/opt/sys/lib:/lib:/usr/lib:"
                                     "/boot/lib:/fs/sda0/lib:/fs/sda0/opt/sys/lib:/fs/sda0/opt/mm/lib";
            static char marker[] = "NS_ENV_OK=1";
            char *env[] = {lib_path, marker, 0};
            execve(argv[0], argv, env);
            mps_log("game warning=reexec_failed errno=%d (sound may not load)", errno);
        }
        memset(&a, 0, sizeof a);
        a.sa_handler = mps_on_signal;
        sigemptyset(&a.sa_mask);
        sigaction(SIGTERM, &a, 0);
        sigaction(SIGINT, &a, 0);
        sigaction(SIGHUP, &a, 0);
        signal(SIGPIPE, SIG_IGN);
        memset(&api, 0, sizeof api);
        if (!load_screen(&api)) return 4;
        return ns_play(0);
    }
    return 2;
}
#endif
