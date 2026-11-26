/* tyrian-qnx: the OpenTyrian app's game process (OpenTyrian, GPL-2.0, with the Tyrian 2.1 data).
 *   --hmi-trace   the shared HMI trace writer every app's worker carries
 *   --play        runs the game on its own screen layer, read from a wired Xbox pad on USB
 *   --restore     takes the game's layer off the screen and gives the audio source back (safe any time)
 *
 * Built from the pieces Freedoom proved on the car (2026-11-18..20):
 *   - picture: freedoom_screen.h (surface 1300 on top of the iDrive picture, 640x400 centred, black around);
 *   - input: gr_pad.c in a thread; its pad states arrive over UDP TY_PORT and become the SDL joystick the
 *     game reads (tyrian_input.h); the plugin sends "stop" on the same port when the page hides;
 *   - sound: car_audio.h (the browser audio source; the radio pauses while the game plays).
 * OpenTyrian itself is unchanged: src/tyrian/ supplies the SDL2 calls it makes.
 * Its data (tyrian1.lvl ...) is on the hard drive (TY_GAMES_DIR, put there by the games installer); its
 * settings and saves go to TY_SAVE_DIR on the flash (as XDG_CONFIG_HOME, so .../opentyrian/).
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "hmi_trace_sink.h"
#include "gr_show.h"
#include "freedoom_screen.h"
#include "tyrian_input.h"
#include "car_audio.h"
#ifdef __QNX__
#include "gr_pad.c"
#endif
#include "tyrian/SDL.h"

#ifndef TY_PORT
#define TY_PORT 42891
#endif
#ifndef TY_DATA_DIR
#define TY_DATA_DIR "/var/opt/hmi/data/ID5/Plugins/Tyrian"
#endif
#ifndef TY_GAMES_DIR
#define TY_GAMES_DIR "/mnt/data/ID6Games/Tyrian"
#endif
#ifndef TY_SAVE_DIR
#define TY_SAVE_DIR "/var/opt/hmi/data/ID5/Tyrian-data"
#endif
#ifndef TY_PRIORITY
#define TY_PRIORITY 8          /* the HMI's own thread runs at 10; the game must never starve it */
#endif

int tyrian_main(int argc, char *argv[]);          /* OpenTyrian's main (opentyr.c, built with -Dmain=tyrian_main) */
void tyr_audio_fill(Sint16 *stereo, unsigned frames);   /* sdl_shim.c */
void saveConfiguration(void);                           /* config.c: tyrian.cfg + opentyrian.cfg (settings) */

static int game_started;
/* BACK, a signal or a lost parent ends the game where it stands; keep its settings (volume, controls, which the
 * game otherwise only writes when quitting from its own menu). Saved games are written when saved. */
static void ty_end(const char *why) {
    mps_log("game stop reason=%s", why);
    if (game_started) saveConfiguration();
    exit(0);
}

static fd_screen screen;
static mps_api api;
static mps_runner runner = mps_popen_runner;
static int ctl = -1, cleaned = 0;
static uint64_t start_ms, last_parent_check;
static unsigned pad_msgs;
#ifdef __QNX__
static pthread_t pad_thread;
static int pad_started;
#endif

static void ty_cleanup(void) {
    if (cleaned) return;
    cleaned = 1;
#ifdef __QNX__
    stop_flag = 1;                      /* the pad thread's loop (gr_pad.c) */
#endif
    tyr_buttons = 0;
    ca_stop();
    mps_log("game end frames=%u posts_failed=%u post_max_ms=%u pad_msgs=%u seconds=%u loud_blocks=%u", screen.frame,
            screen.posts_failed, screen.max_post_ms, pad_msgs, (unsigned)((mps_now_ms() - start_ms) / 1000u),
            ca_loud_blocks);
    fds_close(&screen);
    if (ctl >= 0) close(ctl);
    ctl = -1;
}

/* ---- what the SDL shim asks of us ---- */
void tyr_platform_poll(void) {
    char msg[128];
    ssize_t got;
    uint64_t now;
    while (ctl >= 0 && (got = recv(ctl, msg, sizeof msg - 1, 0)) > 0) {
        xpad_state s;
        msg[got] = 0;
        if (!strncmp(msg, "stop", 4)) ty_end("page");
        if (xpad_parse(msg, &s)) {
            ty_joy j;
            if (!pad_msgs) mps_log("game pad_input first=%s", msg);
            ++pad_msgs;
            ty_map(&s, &j);
            tyr_axes[0] = j.axes[0];
            tyr_axes[1] = j.axes[1];
            tyr_axes[2] = j.axes[2];
            tyr_axes[3] = j.axes[3];
            tyr_hat = j.hat;
            tyr_buttons = j.buttons;
        }
    }
    if (mps_stop_flag) ty_end("signal");
    now = mps_now_ms();
    if (now - last_parent_check >= 1000u) {
        last_parent_check = now;
        if (getppid() <= 1) ty_end("parent_gone");
    }
}

void tyr_platform_present(const Uint32 *pixels, int pitch, int w, int h) {
    static uint32_t scaled[kGameW * kGameH];
    if (!screen.frame) mps_log("game first_frame size=%dx%d", w, h);
    if (w == kGameW && h == kGameH && pitch == kGameW * 4) {
        fds_frame(&screen, pixels);
    } else {   /* another scaler was chosen in the game's options: nearest-neighbour into 640x400 */
        int x, y;
        for (y = 0; y < kGameH; ++y) {
            const Uint32 *row = (const Uint32 *)((const uint8_t *)pixels + (size_t)(y * h / kGameH) * (size_t)pitch);
            for (x = 0; x < kGameW; ++x) scaled[y * kGameW + x] = row[x * w / kGameW];
        }
        fds_frame(&screen, scaled);
    }
}

/* ---- start-up ---- */
static int ty_bind(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    if (fd < 0) return -1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(TY_PORT);
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

#ifdef __QNX__
static void *ty_pad_main(void *arg) {
    (void)arg;
    pad_run(0, TY_PORT, 0);
    return 0;
}
#endif

/* The game's data folder: the hard drive first (games installer), then a copy in the plugin folder. */
static int ty_find_data(char *out, size_t size) {
    const char *const dirs[] = {TY_GAMES_DIR, TY_DATA_DIR "/data"};
    unsigned i;
    for (i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
        char probe[600];
        struct stat st;
        snprintf(probe, sizeof probe, "%s/tyrian1.lvl", dirs[i]);
        if (stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
            char cwd[400];   /* absolute: the game changes to its saves folder before it opens its data */
            if (dirs[i][0] == '/' || !getcwd(cwd, sizeof cwd)) snprintf(out, size, "%s", dirs[i]);
            else snprintf(out, size, "%s/%s", cwd, dirs[i]);
            return 1;
        }
    }
    snprintf(out, size, "%s", dirs[0]);
    return 0;
}

static int ty_play(void) {
    static char data_dir[512];
    char *args[] = {(char *)"opentyrian", (char *)"-t", data_dir, 0};
    start_ms = mps_now_ms();
    last_parent_check = start_ms;
    if (!ty_find_data(data_dir, sizeof data_dir)) {
        mps_log("game refused reason=no_data path=%s/tyrian1.lvl (run the games installer again)", data_dir);
        return 6;
    }
    ctl = ty_bind();
    if (ctl < 0) {
        mps_log("game refused reason=already_running");
        return 3;
    }
    {   /* OpenTyrian keeps its settings and saves in $XDG_CONFIG_HOME/opentyrian (absolute: we chdir) */
        static char save_dir[512], sub[600];
        char cwd[400];
        if (TY_SAVE_DIR[0] == '/' || !getcwd(cwd, sizeof cwd)) snprintf(save_dir, sizeof save_dir, "%s", TY_SAVE_DIR);
        else snprintf(save_dir, sizeof save_dir, "%s/%s", cwd, TY_SAVE_DIR);
        if (mkdir(save_dir, 0755) != 0 && errno != EEXIST) mps_log("game warning=save_dir_create errno=%d", errno);
        snprintf(sub, sizeof sub, "%s/opentyrian", save_dir);
        mkdir(sub, 0755);
        if (chdir(save_dir) != 0) mps_log("game warning=no_save_dir errno=%d", errno);
        setenv("XDG_CONFIG_HOME", save_dir, 1);
    }
#ifdef __QNX__
    {
        struct sched_param p;
        int policy;
        if (pthread_getschedparam(pthread_self(), &policy, &p) == 0) {
            int was = p.sched_priority;
            p.sched_priority = TY_PRIORITY;
            mps_log("game priority was=%d now=%d rc=%d", was, TY_PRIORITY, pthread_setschedparam(pthread_self(), policy, &p));
        }
        pad_started = pthread_create(&pad_thread, 0, ty_pad_main, 0) == 0;
        mps_log("game pad_thread=%s port=%d", pad_started ? "started" : "failed", TY_PORT);
    }
#endif
    atexit(ty_cleanup);
    mps_log("game begin data=%s saves=%s/opentyrian", data_dir, TY_SAVE_DIR);
    if (!fds_open(&screen, &api, runner)) return 5;
    ca_start(tyr_audio_fill);
    game_started = 1;
    tyrian_main(3, args);
    mps_log("game stop reason=quit");
    ty_cleanup();
    return 0;
}

#ifndef TY_TEST
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
    if (argc == 2 && !strcmp(argv[1], "--play")) {
        struct sigaction a;
        /* Once, start again with the car's media libraries on the library path (libasound, audioconn). */
        if (!getenv("TY_ENV_OK")) {
            static char lib_path[] = "LD_LIBRARY_PATH=/opt/mm/lib:/opt/mm/lib/libasound:/opt/sys/lib:/lib:/usr/lib:"
                                     "/boot/lib:/fs/sda0/lib:/fs/sda0/opt/sys/lib:/fs/sda0/opt/mm/lib";
            static char marker[] = "TY_ENV_OK=1";
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
        return ty_play();
    }
    return 2;
}
#endif
