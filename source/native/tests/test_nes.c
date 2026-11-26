/* The Retro worker (src/nes_worker.c) with the REAL NES core (FCEUmm) and the bundled homebrew, against a fake
 * QNX Screen and LayerManagerControl: the picker lists the games, the pad picks one and plays it, the menu saves
 * a state, sound comes out, and the layer comes off at the end. Also the pad mapping (nes_input.h). */
#define NS_TEST 1
#include "../src/nes_worker.c"
#include <assert.h>
#ifndef NS_SHOT
#define NS_SHOT "nes-last.ppm"
#endif
#ifndef NS_SECONDS
#define NS_SECONDS 16
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
#ifdef RETRO_CORE_PS1
        if(r && r[0]==kGameX && r[2]==kGameW) assert((v&0xff000000u)==0xff000000u);
#endif
        for (c = 0; c < lit && colours[c] != v; ++c) {}
        if (c == lit) colours[lit++] = v;
    }
    ++frames_seen;
    if (lit >= 4) ++frames_with_detail;   /* NES pictures use few colours */
    if (lit > lit_max || (lit == lit_max && lit >= 4 && frames_seen % 50 == 0)) {
        lit_max = lit;
        save_ppm(NS_SHOT, (const uint32_t *)b);       /* the most colourful frame, for a look */
    }
    save_ppm("tyrian-final.ppm", (const uint32_t *)b);  /* and the latest one */
    {   /* a snapshot every 2 s of the run (tyrian-snap-NN.ppm), to see the way through the menus */
        static unsigned next_snap;
        unsigned now = (unsigned)(mps_now_ms() - start_ms);
        if (now >= next_snap) {
            char name[40];
            snprintf(name, sizeof name, "nes-snap-%02u.ppm", now / 1000u);
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
    a.sin_port = htons(NS_PORT);
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
/* Picker (sorted: Concentration Room, Nova the Squirrel, RHDE, Thwaite): down to Nova, A. Then Start twice
 * (title), hold right with A taps, open the menu (Xbox button), down to "Save state", A, B back to the game.
 * Then the menu again, down to "Change buttons", A; A on the first console button (NES/GB A, PS1 Cross) and Y for
 * it; B saves and goes back to the menu, B back to the game. */
static void *pad_script(void *arg) {
    char m[96];
    xpad_state s;
    int i;
    (void)arg;
#define SEND() do { xpad_format(&s, m, sizeof m); send_to_game(m); } while (0)
#define TAP(bits, hold) do { memset(&s, 0, sizeof s); s.buttons = (bits); SEND(); mps_sleep_ms(hold); memset(&s, 0, sizeof s); SEND(); mps_sleep_ms(400); } while (0)
    mps_sleep_ms(1500);
    TAP(XPAD_DOWN, 150);
    TAP(XPAD_A, 150);
    mps_sleep_ms(2000);
    for (i = 0; i < 3; ++i) { TAP(XPAD_START, 150); mps_sleep_ms(800); }
    for (i = 0; i < 6; ++i) { memset(&s, 0, sizeof s); s.buttons = XPAD_RIGHT | ((i & 1) ? XPAD_A : 0); SEND(); mps_sleep_ms(400); }
    memset(&s, 0, sizeof s); SEND(); mps_sleep_ms(500);
    TAP(XPAD_GUIDE, 150);
    TAP(XPAD_DOWN, 150);
    TAP(XPAD_A, 150);
    mps_sleep_ms(500);
    TAP(XPAD_B, 150);
    mps_sleep_ms(500);
    TAP(XPAD_GUIDE, 150);
    for (i = 0; i < NS_PAUSE_ITEMS - 1; ++i) TAP(XPAD_DOWN, 150);
    TAP(XPAD_A, 150);   /* the "Change buttons" screen */
    TAP(XPAD_A, 150);   /* its first console button: waiting for a press */
    TAP(XPAD_Y, 150);   /* Y it is */
    TAP(XPAD_B, 150);   /* done: saved, back to the menu */
    TAP(XPAD_B, 150);   /* back to the game */
    return 0;
}

#ifdef RETRO_CORE_PS1
static unsigned test_audio_active,test_audio_occupancy,test_audio_low;
static void test_audio_status(bool active,unsigned occupancy,bool low) {
    test_audio_active=active; test_audio_occupancy=occupancy; test_audio_low=low;
}
static void test_ps1_adaptive(void) {
    struct retro_audio_buffer_status_callback cb={test_audio_status};
    struct retro_variable var={"pcsx_rearmed_frameskip_type",NULL};
    unsigned latency=128,k;
    assert(ns_environment(RETRO_ENVIRONMENT_GET_VARIABLE,&var) && !strcmp(var.value,"auto"));
    var.key="pcsx_rearmed_frameskip_interval";
    assert(ns_environment(RETRO_ENVIRONMENT_GET_VARIABLE,&var) && !strcmp(var.value,"1"));
    assert(ns_environment(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK,&cb));
    assert(!ns_environment(RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY,&latency));
    ps1_audio_active(&ps1_sound,1); ca_started=1; ca_done=0;
    ns_report_audio();assert(test_audio_active && test_audio_occupancy==0 && test_audio_low);
    pthread_mutex_lock(&ps1_sound.lock);
    for(k=0;k<PS1_AUDIO_LEAD+500u;++k) ps1_audio_put_locked(&ps1_sound,500,-500);
    pthread_mutex_unlock(&ps1_sound.lock);
    ns_report_audio();assert(test_audio_active && test_audio_occupancy==100 && !test_audio_low);
    ca_done=1;ns_report_audio();assert(!test_audio_active && !test_audio_low);
    ca_started=0;ps1_audio_active(&ps1_sound,0);
    assert(ns_environment(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK,NULL));
    assert(!ns_ps1_buffer_cb);
    ns_ps1_buffer_reports=ns_ps1_low_reports=0;
}
#endif
int main(void) {
    static const mps_api fake = {f_create_context, f_destroy_context, f_create_window, f_destroy_window,
                                 f_create_buffers, f_set_iv, f_set_cv, f_get_window_pv, f_get_buffer_pv, f_post};
    pthread_t t;
    char state[800];
    struct stat st;
    {   /* resampling: 32768 Hz in -> 44100 Hz out, about 1.346 output frames per input frame */
        int16_t in[2 * 3276];
        unsigned k, before;
        for (k = 0; k < 2 * 3276; ++k) in[k] = (int16_t)(k & 1 ? -1000 : 1000);
        ns_audio_rate(32768.0);
#ifdef RETRO_CORE_PS1
        if (!ps1_sound.active) ps1_audio_active(&ps1_sound,1);
        before = ps1_audio_depth(&ps1_sound);
#else
        before = ring_w;
#endif
        ns_audio_batch(in, 3276);
        #ifdef RETRO_CORE_PS1
        assert(ps1_audio_depth(&ps1_sound)-before >= 4400 && ps1_audio_depth(&ps1_sound)-before <= 4410);
#else
        assert(ring_w - before >= 4400 && ring_w - before <= 4410);
#endif
        ns_audio_rate(44100.0);
#ifdef RETRO_CORE_PS1
        if (!ps1_sound.active) ps1_audio_active(&ps1_sound,1);
        before = ps1_audio_depth(&ps1_sound);
#else
        before = ring_w;
#endif
        ns_audio_batch(in, 1000);
        #ifdef RETRO_CORE_PS1
        assert(ps1_audio_depth(&ps1_sound)-before == 1000);
        ps1_audio_active(&ps1_sound,0);
#else
        assert(ring_w - before == 1000);
#endif
        ring_r = ring_w;
        printf("retro sound: 32768 Hz (Game Boy) becomes 44100 Hz; 44100 Hz passes straight through\n");
    }
    {   /* the pad mapping */
        xpad_state s;
        memset(&s, 0, sizeof s);
        s.buttons = XPAD_A | XPAD_X | XPAD_START | XPAD_UP;
        assert(ns_retropad(&s) == ((1u << NS_RP_A) | (1u << NS_RP_B) | (1u << NS_RP_START) | (1u << NS_RP_UP)));
        s.buttons = XPAD_START | XPAD_BACK;                    /* both: the menu, not Start/Select */
        assert(ns_retropad(&s) == 0 && (ns_nav(&s) & NS_MENU));
        memset(&s, 0, sizeof s); s.lx = -30000; s.buttons = XPAD_B | XPAD_Y;
        assert(ns_retropad(&s) == ((1u << NS_RP_LEFT) | (1u << NS_RP_A) | (1u << NS_RP_B)));
        assert(ns_nav(&s) == (NS_LEFT | NS_BACK));
        printf("nes input: d-pad/stick, A/B, X/Y, Start, Select map to the NES pad; View+Menu or the Xbox button open the menu\n");
    }
    {   /* the flash guard: a save state is refused when it would leave less than the reserve free */
        mkdir(NS_SAVE_DIR, 0755);
        snprintf(save_dir, sizeof save_dir, "%s", NS_SAVE_DIR);
        assert(ns_room_for(1) && !ns_room_for(1ull << 60));
        printf("retro flash guard: a save state needs %llu MB to stay free after it\n", (unsigned long long)(NS_FLASH_RESERVE >> 20));
    }
    remove("build/state/Retro-data/last.txt");   /* the picker must start on the first game */
    {   /* no button layouts from an earlier run */
        DIR *d = opendir(NS_SAVE_DIR);
        struct dirent *e;
        char path[800];
        while (d && (e = readdir(d)) != 0)
            if (!strncmp(e->d_name, "controls-", 9)) { snprintf(path, sizeof path, "%s/%s", NS_SAVE_DIR, e->d_name); remove(path); }
        if (d) closedir(d);
    }
#ifdef RETRO_CORE_PS1
    {   /* a two-second silent audio CD (cue + bin): the owner's BIOS shows its logo, then its CD player screen */
        FILE *f;
        static char zero[2352];
        unsigned k;
        mkdir(NS_GAMES_DIR, 0755);
        f = fopen(NS_GAMES_DIR "/BIOS test.bin", "wb"); assert(f);
        for (k = 0; k < 150; ++k) fwrite(zero, 1, sizeof zero, f);
        fclose(f);
        f = fopen(NS_GAMES_DIR "/BIOS test.cue", "wb"); assert(f);
        fputs("FILE \"BIOS test.bin\" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n", f);
        fclose(f);
    }
    {   /* the PS1 pad: Xbox A/B/X/Y = cross/circle/square/triangle, triggers L2/R2, sticks analog */
        xpad_state s;
        memset(&s, 0, sizeof s);
        s.buttons = XPAD_A | XPAD_Y | XPAD_LB; s.rt = 255; s.lx = 20000; s.ly = 20000;
        assert(ns_retropad_ps1(&s) == ((1u << NS_RP_B) | (1u << NS_RP_X) | (1u << 10) | (1u << 13)));
        assert(ns_ps1_axis(&s, 0, 0) == 20000 && ns_ps1_axis(&s, 0, 1) == -20000);
        printf("ps1 input: A/B/X/Y are cross/circle/square/triangle, LB/RB/LT/RT the shoulders, sticks analog\n");
    }
#endif
#ifdef RETRO_CORE_PS1
    {   /* Production callback routing, neutral-after-menu latch and dynamic PAL/NTSC. */
        struct retro_system_av_info av;
        memset(&av,0,sizeof av); av.timing.fps=50; av.timing.sample_rate=44100;
        assert(ns_environment(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO,&av)); assert(ns_period()==20000000u);
        av.timing.fps=60; assert(ns_environment(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO,&av)); assert(ns_period()==16666667u);
        ns_video_dirty=0; ns_video(NULL,320,240,640); assert(!ns_video_dirty);
        memset(&pad,0,sizeof pad); pad.rt=255; ns_ps1_profile=PS1_SHOOTER; ns_ps1_muted_input=1;
        assert(ns_input_state(0,RETRO_DEVICE_JOYPAD,0,NS_RP_B)==0);
        memset(&pad,0,sizeof pad); ns_input_state(0,RETRO_DEVICE_JOYPAD,0,NS_RP_B); assert(!ns_ps1_muted_input);
        pad.rt=255; assert(ns_input_state(0,RETRO_DEVICE_JOYPAD,0,NS_RP_B)==1);
        assert(ns_input_state(0,RETRO_DEVICE_JOYPAD,0,13)==0);
        memset(&pad,0,sizeof pad);
    }
#endif
#ifdef RETRO_CORE_PS1
    test_ps1_adaptive();
#endif
    api = fake;
    runner = fake_runner;
    {
        pthread_t l;
        pthread_create(&l, 0, log_listener, 0);
        mps_sleep_ms(100);
    }
    pthread_create(&t, 0, pad_script, 0);
    assert(ns_play((NS_SECONDS + 8) * 1000u) == 0);   /* + 8 s for the Change buttons steps */
    pthread_join(t, 0);
    printf("nes: %u games listed, %u started, %u frames posted (%u with a real picture), most colours %u, %u pad messages, layer restored to %s\n",
           game_count, games_started, screen.frame, frames_with_detail, lit_max, pad_msgs, order_text);
    fflush(stdout);
    printf("nes sound: %u blocks (23 ms each) carried sound, %u frames dropped\n", ca_loud_blocks, ring_drops);
#if defined(RETRO_CORE_GB)
    snprintf(state, sizeof state, "%s/uCity.state", save_dir);     /* Libbet, uCity: down one is uCity (Color) */
#elif defined(RETRO_CORE_PS1)
    snprintf(state, sizeof state, "%s/BIOS test.state", save_dir); /* the one disc: an audio CD, so the BIOS shell */
#else
    snprintf(state, sizeof state, "%s/Nova the Squirrel.state", save_dir);
#endif
    assert(!strcmp(window_id, "1300") && windows == 0 && !strcmp(order_text, "256(0x100)"));
#if defined(RETRO_CORE_GB)
    assert(game_count == 2 && games_core[0] == 'g' && games_core[1] == 'g');
#elif defined(RETRO_CORE_PS1)
    assert(game_count == 1 && games_core[0] == 'p');
#else
    assert(game_count == 4);
#endif
    assert(games_started == 1 && screen.frame >= 200 && frames_with_detail >= 100 && lit_max >= 6);
    assert(pad_msgs >= 20 && ca_loud_blocks >= 20 && stat(state, &st) == 0 && st.st_size > 0);
#ifdef RETRO_CORE_PS1
    assert(ns_ps1_buffer_reports>100 && !ns_ps1_buffer_cb);
#endif
    {   /* "Change buttons" (pad_remap.h): the first console button is on Y now, saved for that game, and used */
        unsigned slot = NS_RM_SYS.b[0].slot;
        rm_map back;
        assert(ns_map.active && ns_map.src[slot] == 14);
        assert(rm_load(&NS_RM_SYS, save_dir, games[current], &back) && back.src[slot] == 14);
#ifdef RETRO_CORE_PS1
        assert(ns_ps1_profile == PS1_CUSTOM);
        memset(&pad, 0, sizeof pad); ns_input_state(0, RETRO_DEVICE_JOYPAD, 0, slot);   /* the latch after the menu */
#endif
        memset(&pad, 0, sizeof pad); pad.buttons = XPAD_Y;
        assert(ns_input_state(0, RETRO_DEVICE_JOYPAD, 0, slot) == 1);
        pad.buttons = XPAD_A;
        assert(ns_input_state(0, RETRO_DEVICE_JOYPAD, 0, slot) == 0);
        memset(&pad, 0, sizeof pad);
        printf("retro buttons: the menu's Change buttons put %s on Y, saved for this game, and the game sees it\n",
               NS_RM_SYS.b[0].label);
    }
    printf("nes: picker, game, save state, sound and pad all work; the layer came off.\n");
    return 0;
}
