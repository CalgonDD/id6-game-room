/* freedoom-qnx: the Freedoom app's game process (doomgeneric engine + freedoom1.wad).
 *   --hmi-trace   the shared HMI trace writer every app's worker carries
 *   --play        runs the game on its own screen layer, read from a wired Xbox pad on USB
 *   --play              the game list (launcher.h): Freedoom Phase 1 and 2, the owner's own Doom-engine games
 *                       (IWADs in FD_GAMES_DIR/yours, added with the games installer), and Continue
 *   --play --phase2     straight into Freedoom: Phase 2 (freedoom2.wad, Doom II style)
 *   --play --continue   straight into the newest saved game of any of them (Phase 1's title screen if none)
 *   --restore     takes the game's layer off the screen (safe to run any time)
 *
 * Built from the pieces the car proved on 2026-11-18/19:
 *   - picture: freedoom_screen.h (our own screen surface 1300 on top of the iDrive picture);
 *   - input: gr_pad.c in a thread (Turtle Beach/PDP pad needs the GIP handshake + PDP auth packet),
 *     its pad states arrive over UDP FD_PORT and become Doom keys through freedoom_input.h;
 *   - the plugin sends "stop" on the same port when the page hides or loses focus.
 * Safety: runs below the HMI's priority; ends on "stop", a signal, Doom's own Quit, or when its parent (the HMI)
 * is gone, and always takes its layer off on the way out (atexit). It talks to no car module.
 * The engine is GPL-2.0 (third_party/doomgeneric); Freedoom's data is BSD-3-Clause. */
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <ctype.h>
#include <strings.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include "hmi_trace_sink.h"
#include "gr_show.h"
#include "freedoom_screen.h"
#include "freedoom_input.h"
#include "launcher.h"
#ifdef __QNX__
#include "gr_pad.c"
#endif
#include "doomgeneric.h"
#include "doomtype.h"
#include "p_saveg.h"   /* SAVESTRINGSIZE */
#include "i_system.h"  /* I_AtExit */
#include "d_mode.h"    /* GameMode_t: commercial = Phase 2 */

#ifndef FD_PORT
#define FD_PORT 42890
#endif
#ifndef FD_DATA_DIR
#define FD_DATA_DIR "/var/opt/hmi/data/ID5/Plugins/Freedoom"
#endif
/* The game files. The games installer (games_installer/) puts them on the hard drive, FD_GAMES_DIR (read-only
 * at run time; the HMI never opens it). Builds from before that kept them in the plugin folder, FD_DATA_DIR,
 * which is still tried second. */
#ifndef FD_GAMES_DIR
#define FD_GAMES_DIR "/mnt/data/ID6Games/Freedoom"
#endif
#define FD_WAD_NAME "freedoom1.wad"
#define FD_WAD2_NAME "freedoom2.wad"
/* Saves and settings (the engine writes them to its working directory: .savegame/, default.cfg). Outside the
 * plugin folder, which every install replaces; the installer moves older ones here (install.go). */
#ifndef FD_SAVE_DIR
#define FD_SAVE_DIR "/var/opt/hmi/data/ID5/Freedoom-data"
#endif
#ifndef FD_PRIORITY
#define FD_PRIORITY 8          /* the HMI's own thread runs at 10; the game must never starve it */
#endif

extern int key_prevweapon, key_nextweapon;   /* m_controls.c: unbound (0) by default */
void fd_sound_stop(void);                     /* freedoom_sound.c: closes the device, gives the audio source back */
unsigned fd_sound_starts(void);
unsigned fd_sound_loud_blocks(void);
unsigned fd_sound_music_blocks(void);
unsigned fd_sound_peak(void);
void fd_sound_release_source(void);

static fd_screen screen;
static xpad_state fd_pad_now;   /* the latest pad state (the launcher reads it) */
static fd_input input;
static mps_api api;
static mps_runner runner = mps_popen_runner;
static int ctl = -1, cleaned = 0;
static uint64_t start_ms, last_parent_check;
static unsigned pad_msgs, tics_limit, tics;
#ifdef __QNX__
static pthread_t pad_thread;
static int pad_started;
static volatile int pad_done;   /* the pad thread has closed the pad */
#endif

static void fd_cleanup(void) {
    if (cleaned) return;
    cleaned = 1;
#ifdef __QNX__
    stop_flag = 1;                      /* the pad thread's loop (gr_pad.c) */
#endif
    fd_release_all(&input);
    fd_sound_stop();
    mps_log("game end frames=%u posts_failed=%u post_max_ms=%u pad_msgs=%u seconds=%u", screen.frame,
            screen.posts_failed, screen.max_post_ms, pad_msgs, (unsigned)((mps_now_ms() - start_ms) / 1000u));
    fds_close(&screen);
    if (ctl >= 0) close(ctl);
    ctl = -1;
}

/* Quit from the game's own menu. doomgeneric's I_Quit runs the engine's exit functions (saving the settings,
 * stopping the sound) but then returns, so the game carried on without sound (car, 2026-11-19). This is
 * registered first, so it runs last of them, and really ends the game; fd_cleanup (atexit) then takes the
 * picture down as BACK does. Not run on I_Error, which exits by itself. */
static void fd_quit_exit(void) {
    mps_log("game stop reason=quit");
    exit(0);
}

/* Control port: pad states ("p ...") and "stop". Also notices signals and a dead parent. */
static void fd_poll(void) {
    char msg[128];
    ssize_t got;
    uint64_t now;
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
            fd_pad_now = s;
            fd_update(&input, &s);
        }
    }
    if (mps_stop_flag) {
        mps_log("game stop reason=signal");
        exit(0);
    }
    now = mps_now_ms();
    if (now - last_parent_check >= 1000u) {
        last_parent_check = now;
        if (getppid() <= 1) {
            mps_log("game stop reason=parent_gone");
            exit(0);
        }
    }
}

/* ---- doomgeneric platform ---- */
void DG_Init(void) {
    if (screen.shown) return;      /* the launcher already put the picture up */
    if (!fds_open(&screen, &api, runner)) {
        mps_log("game no_screen (nothing was changed on screen)");
        exit(7);
    }
}

void DG_DrawFrame(void) {
    fd_poll();
    fds_frame(&screen, (const uint32_t *)DG_ScreenBuffer);
    if (screen.frame == 1) mps_log("game first_frame");
}

void DG_SleepMs(uint32_t ms) { mps_sleep_ms(ms); }

uint32_t DG_GetTicksMs(void) { return (uint32_t)(mps_now_ms() - start_ms); }

/* m_menu.c / doomstat.c state the pad needs (all plain globals in the engine). */
extern int messageToPrint, saveStringEnter, saveSlot, saveCharIndex, gameepisode, gamemap;
extern GameMode_t gamemode;
extern boolean messageNeedsInput;
extern char savegamestrings[10][SAVESTRINGSIZE];

int DG_GetKey(int *pressed, unsigned char *key) {
    fd_poll();
    /* Saving: the menu waits for a typed name; an empty slot gets one, so A (Enter) saves straight away. */
    if (saveStringEnter && saveSlot >= 0 && saveSlot < 10 && savegamestrings[saveSlot][0] == 0) {
        fd_save_name(savegamestrings[saveSlot], SAVESTRINGSIZE, gamemode == commercial, gameepisode, gamemap,
                     saveSlot);
        saveCharIndex = (int)strlen(savegamestrings[saveSlot]);
    }
    if (!fd_pop(&input, pressed, key)) return 0;
    if (messageToPrint && messageNeedsInput) *key = fd_prompt_key(*key);
    return 1;
}

void DG_SetWindowTitle(const char *title) { (void)title; }

/* ---- start-up ---- */
static int fd_bind(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    if (fd < 0) return -1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(FD_PORT);
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

#ifdef __QNX__
static void *fd_pad_main(void *arg) {
    (void)arg;
    pad_run(0, FD_PORT, 0);
    pad_done = 1;
    return 0;
}
#endif

/* The newest saved game in dir (the engine keeps them as dir/doomsavN.dsg), or -1 if there is none; its time
 * goes to *when. With the engine's default config directory "." and our chdir to FD_SAVE_DIR, Phase 1's dir is
 * ".savegame/" and Phase 2's "phase2/.savegame/" (Phase 2 runs in FD_SAVE_DIR/phase2: its own saves and
 * settings, as the two games' saves can't be mixed). */
static int fd_newest_save_at(const char *dir, time_t *when) {
    int slot, best = -1;
    time_t best_time = 0;
    for (slot = 0; slot < 10; ++slot) {
        char path[512];
        struct stat st;
        snprintf(path, sizeof path, "%sdoomsav%d.dsg", dir, slot);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
        if (best < 0 || st.st_mtime > best_time) {
            best = slot;
            best_time = st.st_mtime;
        }
    }
    if (when) *when = best_time;
    return best;
}

static int fd_newest_save(const char *dir) { return fd_newest_save_at(dir, 0); }

/* Finds a game file: the games folder on the hard drive first, then the plugin folder. Fills out (and st)
 * and returns 1, or returns 0 with out holding the first path tried. */
static int fd_find_wad(const char *name, char *out, size_t size, struct stat *st) {
    const char *const dirs[] = {FD_GAMES_DIR, FD_DATA_DIR};
    unsigned i;
    for (i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
        snprintf(out, size, "%s/%s", dirs[i], name);
        if (stat(out, st) == 0 && S_ISREG(st->st_mode) && st->st_size > 0) return 1;
    }
    snprintf(out, size, "%s/%s", dirs[0], name);
    return 0;
}

/* ---- the games: Freedoom's two, and the owner's own Doom-engine IWADs ---- */
typedef struct {
    char title[64];
    char wad[600];
    char sub[80];      /* its own folder under FD_SAVE_DIR for saves and settings ("" = Phase 1's, the original) */
    char engine;       /* 0 = Doom (this program); 't' = Tyrian, 'q' = Quake, 'r' = a console game: another program */
    char core;         /* a console game's emulator: 'n' NES, 'g' Game Boy / Color, 'p' PlayStation */
} fd_game;
static fd_game fd_games[LC_MAX];
static unsigned fd_game_count;

/* The engine recognises an IWAD by its file name (d_iwad.c); these are the names it knows. */
static const char *fd_known_title(const char *lower_name) {
    static const char *const known[][2] = {
        {"doom.wad", "Doom"}, {"doom1.wad", "Doom (shareware)"}, {"doom2.wad", "Doom II"},
        {"tnt.wad", "Final Doom: TNT Evilution"}, {"plutonia.wad", "Final Doom: Plutonia"},
        {"chex.wad", "Chex Quest"}, {"hacx.wad", "HacX"}, {"freedm.wad", "FreeDM"},
        {"freedoom1.wad", "Freedoom: Phase 1"}, {"freedoom2.wad", "Freedoom: Phase 2"}};
    unsigned i;
    for (i = 0; i < sizeof known / sizeof known[0]; ++i)
        if (!strcmp(lower_name, known[i][0])) return known[i][1];
    return 0;
}

static int fd_is_iwad(const char *path) {
    char head[4];
    FILE *f = fopen(path, "rb");
    int ok;
    if (!f) return 0;
    ok = fread(head, 1, 4, f) == 4 && !memcmp(head, "IWAD", 4);
    fclose(f);
    return ok;
}

static void fd_add_game(const char *title, const char *wad, const char *sub) {
    fd_game *g;
    if (fd_game_count >= LC_MAX) return;
    g = &fd_games[fd_game_count++];
    snprintf(g->title, sizeof g->title, "%s", title);
    snprintf(g->wad, sizeof g->wad, "%s", wad);
    snprintf(g->sub, sizeof g->sub, "%s", sub);
    g->engine = 0;
}

static void gr_list_others(void);   /* Game Room's other engines (below) */
static void fd_list_games(void) {
    char path[600], cwd[400];
    struct stat st;
    DIR *d;
    struct dirent *e;
    fd_game_count = 0;
    if (fd_find_wad(FD_WAD_NAME, path, sizeof path, &st)) fd_add_game("Freedoom: Phase 1", path, "");
    if (fd_find_wad(FD_WAD2_NAME, path, sizeof path, &st)) fd_add_game("Freedoom: Phase 2", path, "phase2");
    d = opendir(FD_GAMES_DIR "/yours");
    if (d) {
        while ((e = readdir(d)) != 0) {
            char lower[256], slug[96], title[64];
            const char *known;
            size_t n = strlen(e->d_name), i;
            if (n < 5 || n >= sizeof lower || e->d_name[0] == '.') continue;
            for (i = 0; i <= n; ++i) lower[i] = (char)tolower((unsigned char)e->d_name[i]);
            if (strcmp(lower + n - 4, ".wad")) continue;
            snprintf(path, sizeof path, "%s/yours/%s", FD_GAMES_DIR, e->d_name);
            if (!fd_is_iwad(path)) continue;   /* add-on levels (PWADs) are not games on their own */
            known = fd_known_title(lower);
            if (known) snprintf(title, sizeof title, "%s", known);
            else { snprintf(title, sizeof title, "%.*s", (int)(n - 4), e->d_name); }
            snprintf(slug, sizeof slug, "yours-%.*s", (int)(n - 4), lower);
            fd_add_game(title, path, slug);
        }
        closedir(d);
    }
    gr_list_others();
    /* absolute paths: the game changes to its save folder before it opens its file */
    if (getcwd(cwd, sizeof cwd)) {
        unsigned k;
        for (k = 0; k < fd_game_count; ++k)
            if (!fd_games[k].engine && fd_games[k].wad[0] != '/') {
                char abs_path[1000];
                snprintf(abs_path, sizeof abs_path, "%s/%s", cwd, fd_games[k].wad);
                snprintf(fd_games[k].wad, sizeof fd_games[k].wad, "%s", abs_path);
            }
    }
}

/* ---- Game Room (2026-11-20): Freedoom, Tyrian and the Quake games in one list. This program shows it; a game
 * of another engine is played by that engine's program (tyrian-qnx, quake-qnx, next to this one), which this
 * process becomes (exec: same process id, so the iDrive page still knows the game). ---- */
#ifndef GR_TY_GAMES
#define GR_TY_GAMES "/mnt/data/ID6Games/Tyrian"
#endif
#ifndef GR_QK_GAMES
#define GR_QK_GAMES "/mnt/data/ID6Games/LibreQuake"
#endif
#ifndef GR_RT_GAMES
#define GR_RT_GAMES "/mnt/data/ID6Games/Retro"   /* the console games (and yours/: the owner's, and the PS1 BIOS) */
#endif
#ifndef GR_QK_SAVES
#define GR_QK_SAVES "/var/opt/hmi/data/ID5/LibreQuake-data"
#endif
#ifndef GR_PSP_GAMES
#define GR_PSP_GAMES "/mnt/data/ID6Games/PSP"
#endif
static char gr_dir[600];   /* the folder this program is in (from argv[0]); its siblings are the other engines */

static int gr_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}
static int gr_sibling(const char *name, char *out, size_t size) {
    snprintf(out, size, "%s%s%s", gr_dir, gr_dir[0] ? "/" : "", name);
    return access(out, X_OK) == 0;
}
static void gr_add(const char *title, char engine, const char *arg) {
    fd_game *g;
    if (fd_game_count >= LC_MAX) return;
    g = &fd_games[fd_game_count++];
    snprintf(g->title, sizeof g->title, "%s", title);
    g->wad[0] = 0;
    snprintf(g->sub, sizeof g->sub, "%s", arg);   /* for another engine: its game argument */
    g->engine = engine;
    g->core = 0;
}

/* ---- the console games (Retro's emulators, 2026-11-20: "just have everything in Game Room") ---- */
static char gr_core_for(const char *name) {
    const char *dot = strrchr(name, '.');
    char ext[8];
    unsigned i;
    if (!dot || strlen(dot) > 5) return 0;
    for (i = 0; dot[i] && i < sizeof ext - 1; ++i) ext[i] = (char)tolower((unsigned char)dot[i]);
    ext[i] = 0;
    if (!strcmp(ext, ".nes")) return 'n';
    if (!strcmp(ext, ".gb") || !strcmp(ext, ".gbc")) return 'g';
    if (!strcmp(ext, ".chd") || !strcmp(ext, ".cue") || !strcmp(ext, ".pbp") || !strcmp(ext, ".m3u")) return 'p';
    return 0;
}
static const char *gr_core_prog(char core) { return core == 's' ? "psp-qnx" : core == 'g' ? "gb-qnx" : core == 'p' ? "ps1-qnx" : "nes-qnx"; }
static const char *gr_core_label(char core) { return core == 's' ? "PSP" : core == 'g' ? "Game Boy" : core == 'p' ? "PS1" : "NES"; }
typedef struct { char core; char name[80]; char path[600]; } gr_rom;
static gr_rom gr_roms[200];
static unsigned gr_rom_count;
static void gr_scan_retro(const char *dir, int psp) {
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != 0 && gr_rom_count < sizeof gr_roms / sizeof gr_roms[0]) {
        char prog[700], *dot;
        gr_rom *r;
        char core = gr_core_for(e->d_name);
        if (psp) {
            const char *ext = strrchr(e->d_name, '.');
            core = ext && (!strcasecmp(ext, ".iso") || !strcasecmp(ext, ".cso") || !strcasecmp(ext, ".pbp") || !strcasecmp(ext, ".chd")) ? 's' : 0;
        }
        if (!core || e->d_name[0] == '.' || !gr_sibling(gr_core_prog(core), prog, sizeof prog)) continue;
        r = &gr_roms[gr_rom_count];
        snprintf(r->path, sizeof r->path, "%s/%s", dir, e->d_name);
        if (!gr_file(r->path)) continue;
        snprintf(r->name, sizeof r->name, "%s", e->d_name);
        dot = strrchr(r->name, '.');
        if (dot && dot != r->name) *dot = 0;
        r->core = core;
        ++gr_rom_count;
    }
    closedir(d);
}
static int gr_rom_cmp(const void *a, const void *b) {
    const gr_rom *x = (const gr_rom *)a, *y = (const gr_rom *)b;
    static const char order[] = "ngps";
    if (x->core != y->core) return (int)(strchr(order, x->core) - order) - (int)(strchr(order, y->core) - order);
    return strcasecmp(x->name, y->name);
}
static void gr_list_retro(void) {
    unsigned k;
    gr_rom_count = 0;
    gr_scan_retro(GR_RT_GAMES, 0);
    gr_scan_retro(GR_RT_GAMES "/yours", 0);
    gr_scan_retro(GR_PSP_GAMES "/yours", 1);
    qsort(gr_roms, gr_rom_count, sizeof gr_roms[0], gr_rom_cmp);
    for (k = 0; k < gr_rom_count && fd_game_count < LC_MAX; ++k) {
        char title[64];
        snprintf(title, sizeof title, "%s: %s", gr_core_label(gr_roms[k].core), gr_roms[k].name);
        gr_add(title, 'r', "");
        snprintf(fd_games[fd_game_count - 1].wad, sizeof fd_games[0].wad, "%s", gr_roms[k].path);
        fd_games[fd_game_count - 1].core = gr_roms[k].core;
    }
}
/* Tyrian and the Quake games, when their program and files are there (Quake's rules match quake_worker.c's). */
static void gr_list_others(void) {
    char prog[700];
    if (gr_sibling("tyrian-qnx", prog, sizeof prog) && gr_file(GR_TY_GAMES "/tyrian1.lvl")) gr_add("Tyrian", 't', "");
    if (gr_sibling("quake-qnx", prog, sizeof prog)) {
        if (gr_file(GR_QK_GAMES "/pak0.pak")) gr_add("LibreQuake", 'q', "librequake");
        if (gr_file(GR_QK_GAMES "/yours/id1/pak0.pak")) {
            gr_add("Quake", 'q', "quake");
            if (gr_file(GR_QK_GAMES "/yours/hipnotic/pak0.pak")) gr_add("Quake: Scourge of Armagon", 'q', "hipnotic");
            if (gr_file(GR_QK_GAMES "/yours/rogue/pak0.pak")) gr_add("Quake: Dissolution of Eternity", 'q', "rogue");
        }
    }
    gr_list_retro();
}
/* The newest Quake save (any of its games) and when, or 0: quake-qnx --continue picks the same one. */
static int gr_quake_newest(time_t *when) {
    static const char *const dirs[] = {GR_QK_SAVES "/id1", GR_QK_SAVES "/quake/id1", GR_QK_SAVES "/hipnotic/hipnotic",
                                       GR_QK_SAVES "/rogue/rogue"};
    unsigned i;
    int found = 0;
    *when = 0;
    for (i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
        DIR *d = opendir(dirs[i]);
        struct dirent *e;
        if (!d) continue;
        while ((e = readdir(d)) != 0) {
            char path[900];
            struct stat st;
            size_t len = strlen(e->d_name);
            if (len < 5 || strcmp(e->d_name + len - 4, ".sav")) continue;
            snprintf(path, sizeof path, "%s/%s", dirs[i], e->d_name);
            if (stat(path, &st) != 0 || st.st_size <= 0) continue;
            if (!found || st.st_mtime > *when) { *when = st.st_mtime; found = 1; }
        }
        closedir(d);
    }
    return found;
}
/* The arguments for playing another engine's game k (or Quake's newest save, k = -1). */
static void gr_args(int k, const char **prog, const char **a1, const char **a2, const char **a3) {
    *a1 = "--play";
    *a2 = *a3 = 0;
    if (k >= 0 && fd_games[k].engine == 't') { *prog = "tyrian-qnx"; return; }
    if (k >= 0 && fd_games[k].engine == 'r') {   /* straight into the console game: its emulator's --play-rom */
        *prog = gr_core_prog(fd_games[k].core);
        *a1 = "--play-rom";
        *a2 = fd_games[k].wad;
        return;
    }
    *prog = "quake-qnx";
    if (k < 0) *a2 = "--continue";
    else { *a2 = "--game"; *a3 = fd_games[k].sub; }
}
static char gr_test_handoff[200];   /* host test: what the hand-over would have run */
/* Becomes the other engine: lets go of the screen, the pad (the other program opens it itself; exec keeps this
 * process, and an open USB connection made the pad refuse the next one as busy: Retro, car 2026-11-20) and the
 * control port, then execs it. Returns only in the host test. */
static int gr_handoff(int k) {
    const char *prog, *a1, *a2, *a3;
    char path[700];
    char *args[5];
    int fd;
    gr_args(k, &prog, &a1, &a2, &a3);
    gr_sibling(prog, path, sizeof path);
    mps_log("game handoff to=%s args=%s %s %s", prog, a1, a2 ? a2 : "", a3 ? a3 : "");
    snprintf(gr_test_handoff, sizeof gr_test_handoff, "%s %s%s%s%s%s", prog, a1, a2 ? " " : "", a2 ? a2 : "",
             a3 ? " " : "", a3 ? a3 : "");
    fd_release_all(&input);
    fds_close(&screen);
    if (ctl >= 0) close(ctl);
    ctl = -1;
    cleaned = 1;
#ifdef __QNX__
    stop_flag = 1;
    if (pad_started) {
        unsigned waited = 0;
        while (!pad_done && waited < 2000u) { mps_sleep_ms(10); waited += 10u; }
        if (pad_done) pthread_join(pad_thread, 0);
        mps_log("game handoff pad_released=%s waited_ms=%u", pad_done ? "yes" : "no", waited);
        pad_started = 0;
    }
#endif
    for (fd = 3; fd < 256; ++fd) (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    args[0] = path;
    args[1] = (char *)a1;
    args[2] = (char *)a2;
    args[3] = (char *)a3;
    args[4] = 0;
#ifdef FD_TEST
    (void)args;
    return 9;
#else
    execv(path, args);
    mps_log("game fail step=handoff errno=%d path=%s", errno, path);
    exit(1);
#endif
}

/* The newest save of all the games: which game (index) and slot, or -1. */
static int fd_newest_of_all(int *slot) {
    int best = -1;
    time_t best_t = 0;
    unsigned k;
    for (k = 0; k < fd_game_count; ++k) {
        char dir[700];
        time_t t = 0;
        int s;
        if (fd_games[k].engine) continue;   /* Tyrian, Quake: not Doom saves */
        snprintf(dir, sizeof dir, "%s/%s%s.savegame/", FD_SAVE_DIR, fd_games[k].sub, fd_games[k].sub[0] ? "/" : "");
        s = fd_newest_save_at(dir, &t);
        if (s >= 0 && (best < 0 || t > best_t)) { best = (int)k; best_t = t; *slot = s; }
    }
    {   /* a Quake save newer than every Doom save: Continue goes to Quake (-3) */
        time_t qt;
        char prog[700];
        if (gr_sibling("quake-qnx", prog, sizeof prog) && gr_quake_newest(&qt) && (best < 0 || qt > best_t)) {
            *slot = -1;
            return -3;
        }
    }
    return best;
}

/* The game list on our own screen. Returns the chosen game's index, or -2 for Continue. B leaves. */
static int fd_launcher(int continue_game) {
    static uint32_t px[kGameW * kGameH];
    static char continue_line[96];
    lc_list l;
    unsigned k, base = 0;
    memset(&l, 0, sizeof l);
    l.title = "GAME ROOM";
    if (continue_game >= 0 || continue_game == -3) {
        snprintf(continue_line, sizeof continue_line, "Continue: %s", continue_game == -3 ? "Quake" : fd_games[continue_game].title);
        l.items[l.count++] = continue_line;
        base = 1;
    }
    for (k = 0; k < fd_game_count && l.count < LC_MAX; ++k) l.items[l.count++] = fd_games[k].title;
    if (!fds_open(&screen, &api, runner)) {
        mps_log("game no_screen (nothing was changed on screen)");
        exit(7);
    }
    l.held = lc_nav(&fd_pad_now);
    lc_draw(&l, px);
    fds_frame(&screen, px);
    mps_log("game launcher games=%u continue=%d", fd_game_count, continue_game >= 0 || continue_game == -3);
    for (;;) {
        unsigned before = l.cursor;
        uint32_t act;
        fd_poll();
        act = lc_step(&l, &fd_pad_now, mps_now_ms());
        if (act & LC_BACK) {
            mps_log("game stop reason=launcher_back");
            exit(0);
        }
        if (act & LC_OK) return base && l.cursor == 0 ? -2 : (int)(l.cursor - base);
        if (l.cursor != before) { lc_draw(&l, px); fds_frame(&screen, px); }
        mps_sleep_ms(30);
    }
}

/* Runs the game. max_tics = 0: until stopped (the car); > 0: the host test's limit.
 * phase: 0 = the game list; 1 or 2 = straight into Freedoom Phase 1/2. resume: straight into the newest save. */
static int fd_play(unsigned max_tics, int phase, int resume) {
    static char wad_path[1024], slot_text[8];
    char *args[] = {(char *)"freedoom", (char *)"-iwad", wad_path, 0, 0, 0};
    int nargs = 3, slot = -1, chosen = -1;
    struct stat wad;
    start_ms = mps_now_ms();
    last_parent_check = start_ms;
    tics_limit = max_tics;
    memset(&input, 0, sizeof input);
    fd_list_games();
    if (!fd_game_count) {
        mps_log("game refused reason=no_wad path=%s/%s (run the games installer again)", FD_GAMES_DIR, FD_WAD_NAME);
        return 6;
    }
    ctl = fd_bind();
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
            p.sched_priority = FD_PRIORITY;
            mps_log("game priority was=%d now=%d rc=%d", was, FD_PRIORITY, pthread_setschedparam(pthread_self(), policy, &p));
        }
        pad_started = pthread_create(&pad_thread, 0, fd_pad_main, 0) == 0;
        mps_log("game pad_thread=%s port=%d", pad_started ? "started" : "failed", FD_PORT);
    }
#endif
    atexit(fd_cleanup);
    if (resume || phase == 0) {
        int newest_slot = -1, newest = fd_newest_of_all(&newest_slot);
        if (resume) {
            if (newest == -3) return gr_handoff(-1);              /* the newest save is Quake's */
            chosen = newest >= 0 ? newest : 0;
            slot = newest_slot;
        } else {
            chosen = fd_launcher(newest);
            if (chosen == -2) {
                if (newest == -3) return gr_handoff(-1);
                chosen = newest; slot = newest_slot; resume = 1;
            }
            if (chosen >= 0 && fd_games[chosen].engine) return gr_handoff(chosen);
            memset(&input, 0, sizeof input);    /* the presses that chose the game are not game keys */
        }
        if (fd_games[chosen].engine) chosen = 0;   /* (Continue with no Doom save lands on Freedoom) */
    } else {
        unsigned k;
        for (k = 0; k < fd_game_count; ++k)
            if (!fd_games[k].engine && !strcmp(fd_games[k].sub, phase == 2 ? "phase2" : "")) chosen = (int)k;
        if (chosen < 0) {
            mps_log("game refused reason=no_wad phase=%d", phase);
            return 6;
        }
    }
    snprintf(wad_path, sizeof wad_path, "%s", fd_games[chosen].wad);
    if (stat(wad_path, &wad) != 0) {
        mps_log("game refused reason=no_wad path=%s (run the games installer again)", wad_path);
        return 6;
    }
    if (mkdir(FD_SAVE_DIR, 0755) != 0 && errno != EEXIST) mps_log("game warning=save_dir_create errno=%d", errno);
    if (chdir(FD_SAVE_DIR) == 0) {
        /* Saves left in the plugin folder by an older build (not moved by an installer): bring them over. */
        struct stat here, there;
        if (stat(".savegame", &here) != 0 && stat(FD_DATA_DIR "/.savegame", &there) == 0 &&
            rename(FD_DATA_DIR "/.savegame", ".savegame") == 0)
            mps_log("game saves_moved from=%s", FD_DATA_DIR);
        if (fd_games[chosen].sub[0]) {
            if (mkdir(fd_games[chosen].sub, 0755) != 0 && errno != EEXIST)
                mps_log("game warning=game_dir errno=%d", errno);
            if (chdir(fd_games[chosen].sub) != 0) mps_log("game warning=game_dir_chdir errno=%d (sharing Phase 1's)", errno);
        }
        mps_log("game save_dir=%s%s%s", FD_SAVE_DIR, fd_games[chosen].sub[0] ? "/" : "", fd_games[chosen].sub);
    } else if (chdir(FD_DATA_DIR) == 0) {
        mps_log("game warning=no_save_dir (saves go to the plugin folder and are lost on reinstall)");
    } else {
        mps_log("game warning=no_data_dir (saves and settings may not be kept)");
    }
    if (resume) {
        if (slot >= 0) {
            snprintf(slot_text, sizeof slot_text, "%d", slot);
            args[nargs++] = (char *)"-loadgame";
            args[nargs++] = slot_text;
        }
        mps_log("game continue game=%s slot=%d%s", fd_games[chosen].title, slot, slot < 0 ? " (no save: title screen)" : "");
    }
    mps_log("game begin game=%s phase=%d wad=%s bytes=%ld", fd_games[chosen].title,
            !strcmp(fd_games[chosen].sub, "phase2") ? 2 : 1, wad_path, (long)wad.st_size);
    I_AtExit(fd_quit_exit, false);
    doomgeneric_Create(nargs, args);
    key_prevweapon = FD_KEY_PREVWEAPON;
    key_nextweapon = FD_KEY_NEXTWEAPON;
    for (tics = 0; !max_tics || tics < max_tics; ++tics) doomgeneric_Tick();
    fd_cleanup();
    return 0;
}

#ifndef FD_TEST
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
        fd_sound_release_source();
        return 0;
    }
    if (argc >= 2 && argc <= 4 && !strcmp(argv[1], "--play")) {
        int k, phase = 0, resume = 0;   /* plain --play: the game list */
        {
            char *slash;
            snprintf(gr_dir, sizeof gr_dir, "%s", argv[0]);
            slash = strrchr(gr_dir, '/');
            if (slash) *slash = 0; else gr_dir[0] = 0;
        }
        for (k = 2; k < argc; ++k) {
            if (!strcmp(argv[k], "--phase2")) phase = 2;
            else if (!strcmp(argv[k], "--continue")) resume = 1;
            else return 2;
        }
        struct sigaction a;
        /* Once, start again with the car's media libraries on the library path: the libasound front end
         * (/opt/mm/lib/libasound) and audioconn need them; the plugin starts workers with a short path. */
        if (!getenv("FD_ENV_OK")) {
            static char lib_path[] = "LD_LIBRARY_PATH=/opt/mm/lib:/opt/mm/lib/libasound:/opt/sys/lib:/lib:/usr/lib:"
                                     "/boot/lib:/fs/sda0/lib:/fs/sda0/opt/sys/lib:/fs/sda0/opt/mm/lib";
            static char marker[] = "FD_ENV_OK=1";
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
        return fd_play(0, phase, resume);
    }
    return 2;
}
#endif
