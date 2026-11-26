/* The OpenTyrian worker (src/tyrian_worker.c) with the REAL game and the Tyrian 2.1 data, against a fake QNX
 * Screen and a fake LayerManagerControl: the picture goes on top, the game draws real frames into the centred
 * 640x400 area, pad datagrams (as the SDL joystick) get through the menus into a game, sound is produced, and
 * a "stop" from the page ends it with the layer taken off again. The checks run at exit, after the worker's own
 * clean-up, because "stop" ends the process the way it does on the car. */
#define TY_TEST 1
#include "../src/tyrian_worker.c"
#include <assert.h>
#ifndef TY_SHOT
#define TY_SHOT "tyrian-last.ppm"
#endif
#ifndef TY_SECONDS
#define TY_SECONDS 24
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
        save_ppm(TY_SHOT, (const uint32_t *)b);       /* the most colourful frame, for a look */
    }
    save_ppm("tyrian-final.ppm", (const uint32_t *)b);  /* and the latest one */
    {   /* a snapshot every 2 s of the run (tyrian-snap-NN.ppm), to see the way through the menus */
        static unsigned next_snap;
        unsigned now = SDL_GetTicks();
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

/* A pad: tap A every 0.7 s from 2 s on (title -> menus -> a game), then fly up and fire with RT; then "stop". */
static void send_to_game(const char *text) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(TY_PORT);
    sendto(fd, text, strlen(text), 0, (struct sockaddr *)&a, sizeof a);
    close(fd);
}
static void *pad(void *arg) {
    char m[96];
    xpad_state s;
    int i;
    (void)arg;
#define SEND() do { xpad_format(&s, m, sizeof m); send_to_game(m); } while (0)
    mps_sleep_ms(2000);
    for (i = 0; i < 13; ++i) {      /* logos, title, Start New Game, episode, difficulty, story screens */
        memset(&s, 0, sizeof s); s.buttons = XPAD_A; SEND(); mps_sleep_ms(150);
        memset(&s, 0, sizeof s); SEND(); mps_sleep_ms(850);
    }
    memset(&s, 0, sizeof s); s.ly = 30000; s.lx = -20000; s.rt = 255; SEND(); mps_sleep_ms(2500);   /* fly, fire */
    memset(&s, 0, sizeof s); s.lx = 30000; s.rt = 255; SEND(); mps_sleep_ms(2500);
    memset(&s, 0, sizeof s); SEND();
    mps_sleep_ms((TY_SECONDS - 20) * 1000);
    send_to_game("stop");
    return 0;
}

extern _Bool windowHasFocus;   /* OpenTyrian pauses every frame without it (release builds) */
static void check_at_exit(void) {
    printf("tyrian: %u frames posted (%u with a real picture), most colours in one frame %u (of 64 counted), %u pad messages, layer restored to %s\n",
           screen.frame, frames_with_detail, lit_max, pad_msgs, order_text);
    printf("tyrian sound: %u blocks (23 ms each) carried sound\n", ca_loud_blocks);
    if (strcmp(window_id, "1300") || windows != 0 || strcmp(order_text, "256(0x100)") || screen.frame < 100 ||
        lit_max < 32 || frames_with_detail < 50 || pad_msgs < 10 || ca_loud_blocks < 20 || !windowHasFocus) {
        fprintf(stderr, "tyrian test FAILED (window=%s windows=%d order=%s)\n", window_id, windows, order_text);
        _exit(1);
    }
    printf("tyrian: the picture went up and came down, the game drew and played sound, the pad reached it.\n");
}

int main(void) {
    static const mps_api fake = {f_create_context, f_destroy_context, f_create_window, f_destroy_window,
                                 f_create_buffers, f_set_iv, f_set_cv, f_get_window_pv, f_get_buffer_pv, f_post};
    pthread_t t;
    {   /* the joystick mapping */
        xpad_state s;
        ty_joy j;
        memset(&s, 0, sizeof s);
        s.ly = 32767; s.lx = -32768; s.buttons = XPAD_A | XPAD_LB | XPAD_UP | XPAD_LEFT | XPAD_START;
        ty_map(&s, &j);
        assert(j.axes[1] == -32767 && j.axes[0] == -32768 && j.hat == (1u | 8u));
        assert(j.buttons == ((1u << 0) | (1u << 2) | (1u << 4)));
        memset(&s, 0, sizeof s); s.rt = 255; s.ly = -32768; s.buttons = XPAD_B | XPAD_Y | XPAD_BACK;
        ty_map(&s, &j);
        assert(j.axes[1] == 32767 && j.buttons == ((1u << 0) | (1u << 1) | (1u << 3) | (1u << 5)));
        printf("tyrian input: sticks, d-pad and buttons map to the joystick OpenTyrian reads\n");
    }
    api = fake;
    runner = fake_runner;
    atexit(check_at_exit);        /* registered first, so it runs after the worker's own clean-up */
    pthread_create(&t, 0, pad, 0);
    ty_play();
    return 0;
}
