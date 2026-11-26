/* The Quake worker (src/quake_worker.c) with the REAL engine and LibreQuake's paks, against a fake QNX Screen and
 * a fake LayerManagerControl: the picture goes on top (640x480), the engine draws real frames, the pad (as keys
 * and analog axes) reaches it, Quake's mixer produces sound through the ring buffer, and the layer comes off at
 * the end. Also the pad mapping (quake_input.h). */
#define QK_TEST 1
#include "../src/quake_worker.c"
#include <assert.h>
#ifndef QK_SHOT
#define QK_SHOT "quake-last.ppm"
#endif
#ifndef QK_SECONDS
#define QK_SECONDS 22
#endif

/* ---- fake LayerManagerControl (same rules as test_freedoom.c) ---- */
static char order_text[128] = "256(0x100)";
static int surface_made = 0;

static void apply_order(const char *list) {
    char *end;
    order_text[0] = 0;
    while (*list) {
        unsigned long v = strtoul(list, &end, 16);
        char one[32];
        if (end == list) break;
        snprintf(one, sizeof one, "%s%lu(0x%lx)", order_text[0] ? ", " : "", v, v);
        strncat(order_text, one, sizeof order_text - strlen(order_text) - 1);
        list = *end == ',' ? end + 1 : end;
    }
}

static int fake_runner(const char *cmd, char *out, size_t size) {
    const char *p = cmd;
    out[0] = 0;
    while ((p = strstr(p, GR_LMC " ")) != 0) {
        char one[256];
        const char *e = strchr(p, ';');
        size_t len;
        p += strlen(GR_LMC " ");
        len = e ? (size_t)(e - p) : strlen(p);
        if (len >= sizeof one) len = sizeof one - 1;
        memcpy(one, p, len);
        one[len] = 0;
        if (strstr(one, " 2>&1")) *strstr(one, " 2>&1") = 0;
        if (!strcmp(one, "get layer 0x80000000"))
            snprintf(out + strlen(out), size - strlen(out), "    - surface render order: %s,\n", order_text);
        else if (!strncmp(one, "set layer 0x80000000 render order ", 34))
            apply_order(one + 34);
        else if (!strcmp(one, "get surface 1300"))
            snprintf(out + strlen(out), size - strlen(out), "  - native surface: %s\n", surface_made ? "77" : "4294967295");
        if (!e) break;
        p = e;
    }
    return 0;
}

/* ---- fake QNX Screen ---- */
static uint32_t buffers[2][kPanelW * kPanelH];
static int windows = 0, posts = 0;
static char window_id[16];
static int f_create_context(void **c, int f) { (void)f; *c = (void *)1; return 0; }
static int f_destroy_context(void *c) { (void)c; return 0; }
static int f_create_window(void **w, void *c, int t) { (void)c; (void)t; *w = (void *)2; ++windows; return 0; }
static int f_destroy_window(void *w) { (void)w; --windows; return 0; }
static int f_create_buffers(void *w, int n) { (void)w; return n == 2 ? 0 : -1; }
static int f_set_iv(void *w, int p, const int *v) { (void)w; (void)p; (void)v; return 0; }
static int f_set_cv(void *w, int p, int len, const char *v) {
    (void)w;
    if (p == MPS_PROPERTY_ID_STRING) snprintf(window_id, sizeof window_id, "%.*s", len, v);
    return 0;
}
static int f_get_window_pv(void *w, int p, void **v) {
    (void)w;
    if (p == MPS_PROPERTY_RENDER_BUFFERS) { v[0] = buffers[0]; v[1] = buffers[1]; }
    return 0;
}
static int f_get_buffer_pv(void *b, int p, void **v) { if (p == MPS_PROPERTY_POINTER) *v = b; return 0; }
static unsigned lit_max;   /* the most varied frame: sampled pixels brighter than dark grey (fades in between) */
static unsigned frames_with_detail;
static void save_ppm(const char *name, const uint32_t *b) {
    FILE *ppm = fopen(name, "wb");
    unsigned x, y;
    if (!ppm) return;
    fprintf(ppm, "P6 %d %d 255\n", kPanelW, kPanelH);
    for (y = 0; y < (unsigned)kPanelH; ++y)
        for (x = 0; x < (unsigned)kPanelW; ++x) {
            uint32_t v = b[(size_t)y * kPanelW + x];
            unsigned char rgb[3] = {(unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v};
            fwrite(rgb, 1, 3, ppm);
        }
    fclose(ppm);
}
static unsigned frames_seen;
static int f_post(void *w, void *b, int n, const int *r, int f) {
    /* "detail" = how many different colours a sampled frame holds (fades and black screens have one) */
    static uint32_t colours[64];
    unsigned k, lit = 0, c;
    (void)w; (void)n; (void)r; (void)f;
    if (!strcmp(window_id, "1300")) surface_made = 1;
    for (k = 0; k < (unsigned)(kGameW * kGameH) && lit < 64; k += 97) {
        uint32_t v = ((uint32_t *)b)[(size_t)(kGameY + k / kGameW) * kPanelW + kGameX + k % kGameW];
        for (c = 0; c < lit && colours[c] != v; ++c) {}
        if (c == lit) colours[lit++] = v;
    }
    ++frames_seen;
    if (lit >= 16) ++frames_with_detail;
    if (lit > lit_max || (lit == lit_max && lit >= 16 && frames_seen % 50 == 0)) {
        lit_max = lit;
        save_ppm(QK_SHOT, (const uint32_t *)b);       /* the most colourful frame, for a look */
    }
    save_ppm("tyrian-final.ppm", (const uint32_t *)b);  /* and the latest one */
    {   /* a snapshot every 2 s of the run (tyrian-snap-NN.ppm), to see the way through the menus */
        static unsigned next_snap;
        unsigned now = (unsigned)(mps_now_ms() - start_ms);
        if (now >= next_snap) {
            char name[40];
            snprintf(name, sizeof name, "tyrian-snap-%02u.ppm", now / 1000u);
            save_ppm(name, (const uint32_t *)b);
            next_snap = now + 2000u;
        }
    }
    ++posts;
    return 0;
}

static void send_to_game(const char *text) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(QK_PORT);
    sendto(fd, text, strlen(text), 0, (struct sockaddr *)&a, sizeof a);
    close(fd);
}
/* Our own log lines (mps_log -> the trace port) printed, so a run shows what the worker did. */
static void *log_listener(void *arg) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    char line[600];
    ssize_t got;
    (void)arg;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(HMI_TRACE_PORT);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) return 0;
    while ((got = recv(fd, line, sizeof line - 1, 0)) > 0) {
        char *m;
        line[got] = 0;
        m = strstr(line, "GameRoom show ");
        if (m) { fputs(m + 12, stdout); fflush(stdout); }
    }
    return 0;
}
/* A pad: watch the demo, open the main menu (Menu), move down twice (d-pad), back out (B), then the analog
 * sticks during the demo. A new game is not started here: this PC build is 64-bit, and the engine's QuakeC
 * interpreter is 32-bit only (it crashes in PR_ExecuteProgram when a map is spawned); the car is 32-bit. */
static volatile int quit_asked;
static void check_at_exit(void);
static void *pad_script(void *arg) {
    char m[96];
    xpad_state s;
    int i;
    (void)arg;
#define SEND() do { xpad_format(&s, m, sizeof m); send_to_game(m); } while (0)
    mps_sleep_ms(4000);
    memset(&s, 0, sizeof s); s.buttons = XPAD_START; SEND(); mps_sleep_ms(200);
    memset(&s, 0, sizeof s); SEND(); mps_sleep_ms(800);
    for (i = 0; i < 2; ++i) {
        memset(&s, 0, sizeof s); s.buttons = XPAD_DOWN; SEND(); mps_sleep_ms(200);
        memset(&s, 0, sizeof s); SEND(); mps_sleep_ms(600);
    }
    memset(&s, 0, sizeof s); s.buttons = XPAD_B; SEND(); mps_sleep_ms(200);
    memset(&s, 0, sizeof s); SEND(); mps_sleep_ms(1000);
    memset(&s, 0, sizeof s); s.ly = 30000; s.rx = 12000; SEND(); mps_sleep_ms(2000);
    memset(&s, 0, sizeof s); SEND(); mps_sleep_ms(500);
    /* Quit from the game's own menu: Menu (the cursor is still on Options, 3rd), down twice to Quit, A, then A
     * again on "press y to quit". On the car the y/n prompts never took the pad's keys (2026-11-20). */
    memset(&s, 0, sizeof s); s.buttons = XPAD_START; SEND(); mps_sleep_ms(200);
    memset(&s, 0, sizeof s); SEND(); mps_sleep_ms(800);
    for (i = 0; i < 2; ++i) {
        memset(&s, 0, sizeof s); s.buttons = XPAD_DOWN; SEND(); mps_sleep_ms(200);
        memset(&s, 0, sizeof s); SEND(); mps_sleep_ms(600);
    }
    memset(&s, 0, sizeof s); s.buttons = XPAD_A; SEND(); mps_sleep_ms(200);
    memset(&s, 0, sizeof s); SEND(); mps_sleep_ms(800);
    quit_asked = 1;
    memset(&s, 0, sizeof s); s.buttons = XPAD_A; SEND(); mps_sleep_ms(200);
    memset(&s, 0, sizeof s); SEND();
    return 0;
}

/* The game ends itself on Quit (exit), so the checks run as the process ends. */
static void check_at_exit(void) {
    printf("quake: %u frames posted (%u with a real picture), most colours in one frame %u (of 64 counted), %u pad messages, layer restored to %s\n",
           screen.frame, frames_with_detail, lit_max, pad_msgs, order_text);
    printf("quake sound: %u blocks (23 ms each) carried sound\n", ca_loud_blocks);
    if (!quit_asked || strcmp(window_id, "1300") || windows != 0 || strcmp(order_text, "256(0x100)") ||
        screen.frame < 100 || frames_with_detail < 50 || lit_max < 32 || pad_msgs < 8 || ca_loud_blocks < 20) {
        fprintf(stderr, "quake test FAILED (quit_asked=%d window=%s windows=%d order=%s)\n", quit_asked, window_id, windows, order_text);
        _exit(1);
    }
    printf("quake: the picture went up and came down, the engine drew and played sound, the pad reached it, and A on the quit prompt ended the game.\n");
}

int main(void) {
    static const mps_api fake = {f_create_context, f_destroy_context, f_create_window, f_destroy_window,
                                 f_create_buffers, f_set_iv, f_set_cv, f_get_window_pv, f_get_buffer_pv, f_post};
    pthread_t t;
    {   /* the pad mapping */
        qk_pad p;
        xpad_state s;
        int down, key;
        float ax[6];
        memset(&p, 0, sizeof p);
        memset(&s, 0, sizeof s);
        s.buttons = XPAD_A; s.rt = 255; s.ly = 32767; s.rx = -32768;
        qk_update(&p, &s, 0);                                   /* in the game */
        assert(qk_pop(&p, &down, &key) && down && key == QK_SPACE);
        assert(qk_pop(&p, &down, &key) && down && key == QK_CTRL);
        assert(!qk_pop(&p, &down, &key));
        qk_axes(&p, ax);
        assert(ax[1] < -0.99f && ax[2] < -0.99f && ax[0] == 0.0f);
        memset(&s, 0, sizeof s);
        qk_update(&p, &s, 1);                                   /* released while a menu is up: same keys */
        assert(qk_pop(&p, &down, &key) && !down && key == QK_SPACE);
        assert(qk_pop(&p, &down, &key) && !down && key == QK_CTRL);
        s.buttons = XPAD_A; s.ly = 30000;
        qk_update(&p, &s, 1);                                   /* menus: Enter and the stick as arrows */
        assert(qk_pop(&p, &down, &key) && down && key == QK_UP);
        assert(qk_pop(&p, &down, &key) && down && key == QK_ENTER);
        qk_axes(&p, ax);
        assert(ax[1] == 0.0f);
        printf("quake input: sticks are analog in the game and arrows in menus; buttons map to keys and are released as pressed\n");
    }
    {   /* the game list: LibreQuake, plus the owner's Quake and a mission pack in yours/ */
        FILE *w;
        mkdir("build/state/qkgames", 0755);
        qk_list_games();
        assert(qk_game_count == 1 && !strcmp(qk_games[0].title, "LibreQuake"));
        printf("quake games: LibreQuake is listed (no own Quake yet)\n");
        (void)w;
    }
    api = fake;
    runner = fake_runner;
    {
        pthread_t l;
        pthread_create(&l, 0, log_listener, 0);
        mps_sleep_ms(100);
    }
    atexit(check_at_exit);
    pthread_create(&t, 0, pad_script, 0);
    qk_play(QK_SECONDS * 1000u, 0);   /* LibreQuake directly (the list is checked above); Quit ends it */
    fprintf(stderr, "quake test FAILED: the game was still running after A on the quit prompt\n");
    _exit(1);
}
