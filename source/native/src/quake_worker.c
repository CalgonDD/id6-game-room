/* quake-qnx: the LibreQuake app's game process (the Quake engine via quakegeneric, GPL-2.0, with LibreQuake's
 * BSD-3-Clause data).
 *   --hmi-trace   the shared HMI trace writer every app's worker carries
 *   --play        the game list (launcher.h): LibreQuake, and the owner's own Quake and mission packs
 *                 (QK_GAMES_DIR/yours/id1, /hipnotic, /rogue, added with the games installer), and Continue
 *   --play --continue   straight into the newest saved game of any of them
 *   --play --game G     straight into game G (librequake, quake, hipnotic, rogue: Game Room's list chose it)
 *   --restore     takes the game's layer off the screen and gives the audio source back (safe any time)
 *
 * Built from the pieces Freedoom proved on the car:
 *   - picture: freedoom_screen.h with a 640x480 area (Quake's 320x240, doubled; the full screen height);
 *   - input: gr_pad.c in a thread; pad states arrive over UDP QK_PORT; sticks are Quake's analog joystick
 *     (left: move and strafe, right: turn and look), buttons are keys (quake_input.h);
 *   - sound: Quake's own mixer (id's snd_dma/snd_mix/snd_mem) into a ring buffer that car_audio.h plays.
 * Data: pak0.pak and pak1.pak on the hard drive (QK_GAMES_DIR, put there by the games installer). Quake writes
 * its settings and saves into its game folder, so the game folder is QK_SAVE_DIR/id1 on the flash, holding
 * links to the paks on the hard drive.
 * Safety: runs below the HMI's priority; ends on "stop", a signal, its own Quit, or when its parent (the HMI)
 * is gone, and always takes its layer off on the way out (atexit). It talks to no car module. */
#define _POSIX_C_SOURCE 200809L
#ifdef __QNX__
#define _QNX_SOURCE
#endif
#pragma GCC diagnostic ignored "-Wunused-function"
#include <arpa/inet.h>
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
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include "hmi_trace_sink.h"
#include "gr_show.h"
#define FDS_GAME_W 640
#define FDS_GAME_H 480
#include "freedoom_screen.h"
#include "quake_input.h"
#include "launcher.h"
#include "car_audio.h"
#ifdef __QNX__
#include "gr_pad.c"
#endif

#ifndef QK_PORT
#define QK_PORT 42892
#endif
#ifndef QK_DATA_DIR
#define QK_DATA_DIR "/var/opt/hmi/data/ID5/Plugins/LibreQuake"
#endif
#ifndef QK_GAMES_DIR
#define QK_GAMES_DIR "/mnt/data/ID6Games/LibreQuake"
#endif
#ifndef QK_SAVE_DIR
#define QK_SAVE_DIR "/var/opt/hmi/data/ID5/LibreQuake-data"
#endif
#ifndef QK_PRIORITY
#define QK_PRIORITY 8          /* the HMI's own thread runs at 10; the game must never starve it */
#endif
#define QK_MEM (48 * 1024 * 1024)

/* ---- the engine's side (quakegeneric.h, sound.h, keys.h), declared here to keep its headers out ---- */
typedef struct {
    char *basedir, *cachedir;
    int argc;
    char **argv;
    void *membase;
    int memsize;
} qk_parms_t;
void COM_InitArgv(int argc, char **argv);
void Host_Init(qk_parms_t *parms);
void Host_Frame(float time);
extern int com_argc;
extern char **com_argv;
typedef enum { qk_key_game, qk_key_console, qk_key_message, qk_key_menu } qk_keydest_t;
extern qk_keydest_t key_dest;

static fd_screen screen;
static mps_api api;
static mps_runner runner = mps_popen_runner;
static int ctl = -1, cleaned = 0;
static uint64_t start_ms, last_parent_check;
static unsigned pad_msgs;
static qk_pad pad;
static xpad_state qk_pad_now;   /* the latest pad state (the launcher reads it) */
#ifdef __QNX__
static pthread_t pad_thread;
static int pad_started;
#endif

static void qk_cleanup(void) {
    if (cleaned) return;
    cleaned = 1;
#ifdef __QNX__
    stop_flag = 1;
#endif
    ca_stop();
    mps_log("game end frames=%u posts_failed=%u post_max_ms=%u pad_msgs=%u seconds=%u loud_blocks=%u", screen.frame,
            screen.posts_failed, screen.max_post_ms, pad_msgs, (unsigned)((mps_now_ms() - start_ms) / 1000u),
            ca_loud_blocks);
    fds_close(&screen);
    if (ctl >= 0) close(ctl);
    ctl = -1;
}

/* Control port: pad states ("p ...") and "stop"; signals and a lost parent end the game too. */
static void qk_poll(void) {
    char msg[128];
    ssize_t got;
    while (ctl >= 0 && (got = recv(ctl, msg, sizeof msg - 1, 0)) > 0) {
        xpad_state s;
        msg[got] = 0;
        if (!strncmp(msg, "stop", 4)) {
            mps_log("game stop reason=page");
            exit(0);
        }
        if (xpad_parse(msg, &s)) {
            if (!pad_msgs) mps_log("game pad_input first=%s", msg);
            ++pad_msgs;
            qk_pad_now = s;
            qk_update(&pad, &s, key_dest != qk_key_game);
        }
    }
    if (mps_stop_flag) {
        mps_log("game stop reason=signal");
        exit(0);
    }
#ifndef QK_TEST   /* PC test runs under a non-Cygwin debugger, which looks like a lost parent */
    if (mps_now_ms() - last_parent_check >= 1000u) {
        last_parent_check = mps_now_ms();
        if (getppid() <= 1) {
            mps_log("game stop reason=parent_gone");
            exit(0);
        }
    }
#endif
}

/* ---- quakegeneric platform ---- */
static uint32_t qk_palette[256];
static uint32_t qk_frame[kGameW * kGameH];

void QG_Init(void) {}
void QG_Quit(void) { mps_log("game stop reason=quit"); }
void QG_SetPalette(unsigned char palette[768]) {
    unsigned i;
    for (i = 0; i < 256; ++i)
        qk_palette[i] = 0xFF000000u | ((uint32_t)palette[3 * i] << 16) | ((uint32_t)palette[3 * i + 1] << 8) | palette[3 * i + 2];
}
/* 320x240 palette indices -> 640x480 doubled */
void QG_DrawFrame(void *pixels) {
    const unsigned char *src = (const unsigned char *)pixels;
    int x, y;
    if (!screen.frame) mps_log("game first_frame");
    for (y = 0; y < kGameH / 2; ++y) {
        uint32_t *a = qk_frame + (size_t)(2 * y) * kGameW, *b = a + kGameW;
        const unsigned char *row = src + (size_t)y * (kGameW / 2);
        for (x = 0; x < kGameW / 2; ++x) {
            uint32_t v = qk_palette[row[x]];
            a[2 * x] = a[2 * x + 1] = b[2 * x] = b[2 * x + 1] = v;
        }
    }
    fds_frame(&screen, qk_frame);
}
/* Quake's yes/no prompts ("start a new game?", "quit?") want the letter y. On the pad, A (Enter in menus) answers
 * yes there and B (Escape) no. A key is released as what it was pressed as, so the game never sees a y stuck down. */
extern int key_count, key_lastpress;
extern int m_state;            /* menu.c's menu enum; m_quit is 12 */
#define QK_M_QUIT 12
void Key_Event(int key, int down);
static unsigned char qk_as[256];
static int qk_prompt(void) { return key_count < 0 || (key_dest == qk_key_menu && m_state == QK_M_QUIT); }
int QG_GetKey(int *down, int *key) {
    int k;
    qk_poll();
    if (!qk_pop(&pad, down, key)) return 0;
    k = *key & 0xFF;
    if (*down) {
        qk_as[k] = (unsigned char)(k == QK_ENTER && qk_prompt() ? 'y' : k);
        *key = qk_as[k];
    } else if (qk_as[k]) {
        *key = qk_as[k];
        qk_as[k] = 0;
    }
    return 1;
}
/* sys_null.c's is empty (built renamed): its keys only came in once a frame, so a yes/no prompt, which loops on
 * this alone, waited forever with the picture frozen and the page's stop unheard (car, 2026-11-20). Now it
 * delivers the pad's keys and the page's messages, and doesn't spin while a prompt waits. */
void Sys_SendKeyEvents(void) {
    int down, key;
    while (QG_GetKey(&down, &key)) Key_Event(key, down);
    if (key_count < 0) mps_sleep_ms(5);
}
void QG_GetMouseMove(int *x, int *y) { *x = *y = 0; }
void QG_GetJoyAxes(float *axes) { qk_axes(&pad, axes); }

/* sys_null.c's are built renamed (-DSys_FloatTime=..., -DSys_Error=...): a real clock and a logged error. */
double Sys_FloatTime(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
void Sys_Error(char *error, ...) {
    char line[256];
    va_list ap;
    va_start(ap, error);
    vsnprintf(line, sizeof line, error, ap);
    va_end(ap);
    mps_log("game fail step=engine error=%.200s", line);
    exit(1);
}

/* ---- sound: Quake mixes ahead into a ring buffer (22050 Hz stereo); the car audio thread plays it at 44100 by
 * doubling each frame and moves Quake's play position on. ---- */
typedef int qboolean_t;
typedef struct {
    qboolean_t gamealive, soundalive, splitbuffer;
    int channels, samples, submission_chunk, samplepos, samplebits, speed;
    unsigned char *buffer;
} qk_dma_t;
extern volatile qk_dma_t *shm;
extern volatile qk_dma_t sn;
#define QK_DMA_SAMPLES 65536u           /* mono samples: 32768 stereo frames, about 1.5 s */
static int16_t qk_dma[QK_DMA_SAMPLES];
static volatile unsigned qk_dma_pos;    /* in mono samples */

qboolean_t SNDDMA_Init(void) {
    memset((void *)&sn, 0, sizeof sn);
    shm = &sn;
    shm->splitbuffer = 0;
    shm->samplebits = 16;
    shm->speed = 22050;
    shm->channels = 2;
    shm->samples = (int)QK_DMA_SAMPLES;
    shm->samplepos = 0;
    shm->submission_chunk = 1;
    shm->buffer = (unsigned char *)qk_dma;
    mps_log("sound quake_dma speed=22050 samples=%u", QK_DMA_SAMPLES);
    return 1;
}
int SNDDMA_GetDMAPos(void) {
    shm->samplepos = (int)qk_dma_pos;
    return shm->samplepos;
}
void SNDDMA_Shutdown(void) {}
void SNDDMA_Submit(void) {}

static void qk_audio_fill(int16_t *stereo, unsigned frames) {
    unsigned i, pos = qk_dma_pos;
    for (i = 0; i < frames; i += 2) {   /* one Quake frame -> two output frames */
        int16_t l = qk_dma[pos % QK_DMA_SAMPLES], r = qk_dma[(pos + 1u) % QK_DMA_SAMPLES];
        stereo[2 * i] = stereo[2 * i + 2] = l;
        stereo[2 * i + 1] = stereo[2 * i + 3] = r;
        pos = (pos + 2u) % QK_DMA_SAMPLES;
    }
    qk_dma_pos = pos;
}

/* ---- start-up ---- */
static int qk_bind(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    if (fd < 0) return -1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(QK_PORT);
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

#ifdef __QNX__
static void *qk_pad_main(void *arg) {
    (void)arg;
    pad_run(0, QK_PORT, 0);
    return 0;
}
#endif

/* ---- the games: LibreQuake, and the owner's own Quake and its mission packs ---- */
typedef struct {
    char title[64];
    char id1[600];       /* the folder holding this game's id1 paks */
    char pack[600];      /* the mission pack's folder (hipnotic/rogue paks), or "" */
    char pack_name[16];  /* "hipnotic" or "rogue" (the engine's switch and game folder), or "" */
    char sub[32];        /* its own folder under QK_SAVE_DIR ("" = LibreQuake's, the original) */
} qk_game;
static qk_game qk_games[LC_MAX];
static unsigned qk_game_count;

static int qk_has_pak(const char *dir) {
    char probe[700];
    struct stat st;
    snprintf(probe, sizeof probe, "%s/pak0.pak", dir);
    return stat(probe, &st) == 0 && S_ISREG(st.st_mode);
}

static void qk_abs(const char *dir, char *out, size_t size) {
    char cwd[400];
    if (dir[0] == '/' || !getcwd(cwd, sizeof cwd)) snprintf(out, size, "%s", dir);
    else snprintf(out, size, "%s/%s", cwd, dir);
}

static void qk_add(const char *title, const char *id1, const char *pack, const char *pack_name, const char *sub) {
    qk_game *g;
    if (qk_game_count >= LC_MAX) return;
    g = &qk_games[qk_game_count++];
    snprintf(g->title, sizeof g->title, "%s", title);
    qk_abs(id1, g->id1, sizeof g->id1);
    if (pack[0]) qk_abs(pack, g->pack, sizeof g->pack); else g->pack[0] = 0;
    snprintf(g->pack_name, sizeof g->pack_name, "%s", pack_name);
    snprintf(g->sub, sizeof g->sub, "%s", sub);
}

static void qk_list_games(void) {
    qk_game_count = 0;
    if (qk_has_pak(QK_GAMES_DIR)) qk_add("LibreQuake", QK_GAMES_DIR, "", "", "");
    else if (qk_has_pak(QK_DATA_DIR "/id1")) qk_add("LibreQuake", QK_DATA_DIR "/id1", "", "", "");
    if (qk_has_pak(QK_GAMES_DIR "/yours/id1")) {
        qk_add("Quake", QK_GAMES_DIR "/yours/id1", "", "", "quake");
        if (qk_has_pak(QK_GAMES_DIR "/yours/hipnotic"))
            qk_add("Quake: Scourge of Armagon", QK_GAMES_DIR "/yours/id1", QK_GAMES_DIR "/yours/hipnotic", "hipnotic", "hipnotic");
        if (qk_has_pak(QK_GAMES_DIR "/yours/rogue"))
            qk_add("Quake: Dissolution of Eternity", QK_GAMES_DIR "/yours/id1", QK_GAMES_DIR "/yours/rogue", "rogue", "rogue");
    }
}

/* Links every pak*.pak of src into dst (made if missing). */
static int qk_link_paks(const char *src, const char *dst) {
    DIR *d;
    struct dirent *e;
    int linked = 0;
    if (mkdir(dst, 0755) != 0 && errno != EEXIST) return 0;
    d = opendir(src);
    if (!d) return 0;
    while ((e = readdir(d)) != 0) {
        char link_path[900], target[900];
        size_t n = strlen(e->d_name);
        if (n < 8 || strncmp(e->d_name, "pak", 3) || strcmp(e->d_name + n - 4, ".pak")) continue;
        snprintf(link_path, sizeof link_path, "%s/%s", dst, e->d_name);
        snprintf(target, sizeof target, "%s/%s", src, e->d_name);
        unlink(link_path);
        if (symlink(target, link_path) != 0) {
            mps_log("game fail step=pak_link errno=%d path=%s", errno, link_path);
            closedir(d);
            return 0;
        }
        ++linked;
    }
    closedir(d);
    return linked > 0;
}

/* The game's folder on the flash: QK_SAVE_DIR[/sub], holding id1 (and the mission pack's folder) with links
 * to the paks on the hard drive; Quake writes config.cfg and saves there. Our pad set-up goes to id1/id6pad.cfg
 * (rewritten every start). Returns the base folder (absolute). */
static int qk_prepare(const qk_game *g, char *base, size_t size) {
    char root[600], dir[700];
    FILE *f;
    qk_abs(QK_SAVE_DIR, root, sizeof root);
    if (mkdir(root, 0755) != 0 && errno != EEXIST) return 0;
    if (g->sub[0]) snprintf(base, size, "%s/%s", root, g->sub); else snprintf(base, size, "%s", root);
    if (mkdir(base, 0755) != 0 && errno != EEXIST) return 0;
    snprintf(dir, sizeof dir, "%s/id1", base);
    if (!qk_link_paks(g->id1, dir)) return 0;
    if (g->pack[0]) {
        snprintf(dir, sizeof dir, "%s/%s", base, g->pack_name);
        if (!qk_link_paks(g->pack, dir)) return 0;
    }
    snprintf(dir, sizeof dir, "%s/id1/id6pad.cfg", base);
    f = fopen(dir, "w");
    if (!f) return 0;
    fputs(QK_PAD_CFG, f);
    fclose(f);
    return 1;
}

/* The newest save (*.sav in a game's folder) of all the games: its game index and name (without .sav), or -1. */
static int qk_newest_save(char *name, size_t size) {
    int best = -1;
    time_t best_t = 0;
    unsigned k;
    char root[600];
    qk_abs(QK_SAVE_DIR, root, sizeof root);
    for (k = 0; k < qk_game_count; ++k) {
        char dir[800];
        DIR *d;
        struct dirent *e;
        snprintf(dir, sizeof dir, "%s%s%s/%s", root, qk_games[k].sub[0] ? "/" : "", qk_games[k].sub,
                 qk_games[k].pack[0] ? qk_games[k].pack_name : "id1");
        d = opendir(dir);
        if (!d) continue;
        while ((e = readdir(d)) != 0) {
            char path[1100];
            struct stat st;
            size_t n = strlen(e->d_name);
            if (n < 5 || strcmp(e->d_name + n - 4, ".sav")) continue;
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            if (stat(path, &st) != 0 || st.st_size <= 0) continue;
            if (best < 0 || st.st_mtime > best_t) {
                best = (int)k;
                best_t = st.st_mtime;
                snprintf(name, size, "%.*s", (int)(n - 4), e->d_name);
            }
        }
        closedir(d);
    }
    return best;
}

/* The game list on our own screen. Returns the chosen game's index, or -2 for Continue. B leaves. */
static int qk_launcher(int continue_game) {
    static uint32_t px[kGameW * kGameH];
    static char continue_line[96];
    lc_list l;
    unsigned k, base = 0;
    memset(&l, 0, sizeof l);
    l.title = "QUAKE GAMES";
    if (continue_game >= 0) {
        snprintf(continue_line, sizeof continue_line, "Continue: %s", qk_games[continue_game].title);
        l.items[l.count++] = continue_line;
        base = 1;
    }
    for (k = 0; k < qk_game_count && l.count < LC_MAX; ++k) l.items[l.count++] = qk_games[k].title;
    l.held = lc_nav(&qk_pad_now);
    lc_draw(&l, px);
    fds_frame(&screen, px);
    mps_log("game launcher games=%u continue=%d", qk_game_count, continue_game >= 0);
    for (;;) {
        unsigned before = l.cursor;
        uint32_t act;
        qk_poll();
        act = lc_step(&l, &qk_pad_now, mps_now_ms());
        if (act & LC_BACK) {
            mps_log("game stop reason=launcher_back");
            exit(0);
        }
        if (act & LC_OK) return base && l.cursor == 0 ? -2 : (int)(l.cursor - base);
        if (l.cursor != before) { lc_draw(&l, px); fds_frame(&screen, px); }
        mps_sleep_ms(30);
    }
}

/* Runs the game. max_ms > 0: the host test's limit. choose: -1 = the game list, -2 = Continue, else a game. */
static int qk_play(unsigned max_ms, int choose) {
    static char base[600], save_name[64], hip[16];
    static char *args[8];
    static qk_parms_t parms;
    int nargs = 0, chosen, newest;
    double last;
    start_ms = mps_now_ms();
    last_parent_check = start_ms;
    qk_list_games();
    if (!qk_game_count) {
        mps_log("game refused reason=no_data path=%s/pak0.pak (run the games installer again)", QK_GAMES_DIR);
        return 6;
    }
    ctl = qk_bind();
    if (ctl < 0) {
        mps_log("game refused reason=already_running");
        return 3;
    }
#ifdef __QNX__
    {
        struct sched_param p;
        int policy;
        if (pthread_getschedparam(pthread_self(), &policy, &p) == 0) {
            int was = p.sched_priority;
            p.sched_priority = QK_PRIORITY;
            mps_log("game priority was=%d now=%d rc=%d", was, QK_PRIORITY, pthread_setschedparam(pthread_self(), policy, &p));
        }
        pad_started = pthread_create(&pad_thread, 0, qk_pad_main, 0) == 0;
        mps_log("game pad_thread=%s port=%d", pad_started ? "started" : "failed", QK_PORT);
    }
#endif
    atexit(qk_cleanup);
    if (!fds_open(&screen, &api, runner)) return 5;
    newest = qk_newest_save(save_name, sizeof save_name);
    chosen = choose == -1 ? qk_launcher(newest) : choose;
    if (chosen == -2) chosen = newest >= 0 ? newest : 0;
    else save_name[0] = 0;
    if (chosen < 0 || (unsigned)chosen >= qk_game_count) chosen = 0;
    if (!qk_prepare(&qk_games[chosen], base, sizeof base)) {
        mps_log("game refused reason=save_dir path=%s", base);
        return 7;
    }
    args[nargs++] = (char *)"quake";
    if (qk_games[chosen].pack[0]) {
        snprintf(hip, sizeof hip, "-%s", qk_games[chosen].pack_name);
        args[nargs++] = hip;
        args[nargs++] = (char *)"-game";
        args[nargs++] = qk_games[chosen].pack_name;
    }
    args[nargs++] = (char *)"+exec";
    args[nargs++] = (char *)"id6pad.cfg";
    if (save_name[0]) {
        args[nargs++] = (char *)"+load";
        args[nargs++] = save_name;
        mps_log("game continue game=%s save=%s", qk_games[chosen].title, save_name);
    }
    args[nargs] = 0;
    if (chdir(base) != 0) mps_log("game warning=chdir errno=%d", errno);
    mps_log("game begin game=%s data=%s game_dir=%s", qk_games[chosen].title, qk_games[chosen].id1, base);
    ca_start(qk_audio_fill);
    parms.memsize = QK_MEM;
    parms.membase = malloc((size_t)parms.memsize);
    if (!parms.membase) {
        mps_log("game fail step=memory bytes=%d", parms.memsize);
        return 8;
    }
    parms.basedir = (char *)".";
    COM_InitArgv(nargs, args);
    parms.argc = com_argc;
    parms.argv = com_argv;
    Host_Init(&parms);
    last = Sys_FloatTime();
    while (!max_ms || mps_now_ms() - start_ms < max_ms) {
        double now = Sys_FloatTime();
        Host_Frame((float)(now - last));
        last = now;
        qk_poll();
        mps_sleep_ms(2);
    }
    qk_cleanup();
    return 0;
}

#ifndef QK_TEST
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
    if ((argc == 2 || (argc == 3 && !strcmp(argv[2], "--continue")) || (argc == 4 && !strcmp(argv[2], "--game"))) &&
        !strcmp(argv[1], "--play")) {
        struct sigaction a;
        if (!getenv("QK_ENV_OK")) {   /* once, with the car's media libraries on the path (libasound, audioconn) */
            static char lib_path[] = "LD_LIBRARY_PATH=/opt/mm/lib:/opt/mm/lib/libasound:/opt/sys/lib:/lib:/usr/lib:"
                                     "/boot/lib:/fs/sda0/lib:/fs/sda0/opt/sys/lib:/fs/sda0/opt/mm/lib";
            static char marker[] = "QK_ENV_OK=1";
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
        if (argc == 4) {   /* Game Room chose the game: librequake, quake, hipnotic or rogue */
            unsigned k;
            qk_list_games();
            for (k = 0; k < qk_game_count; ++k)
                if (!strcmp(qk_games[k].sub[0] ? qk_games[k].sub : "librequake", argv[3])) return qk_play(0, (int)k);
            mps_log("game refused reason=no_such_game name=%s", argv[3]);
            return 6;
        }
        return qk_play(0, argc == 3 ? -2 : -1);
    }
    return 2;
}
#endif
