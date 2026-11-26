/* The Freedoom worker (src/freedoom_worker.c) with the REAL engine and freedoom1.wad, against a fake QNX
 * Screen and a fake LayerManagerControl: the picture goes on top of what was there, the engine draws real
 * frames into the centred 640x400 area (opaque, black around it), pad datagrams become Doom keys and start a
 * game through the menus, and the layer comes off again at the end. About 9 s of game time. */
#define FD_TEST 1
#define GR_PSP_GAMES "build/state/games/PSP"
#define GR_PSP_SAVE "build/state/PSP-data"
#include "../src/freedoom_worker.c"
#include <assert.h>
#include <utime.h>
#include "../src/opl/fd_opl.h"
#ifndef FD_SHOT
#define FD_SHOT "freedoom-last.ppm"
#endif

/* ---- fake LayerManagerControl (same rules as the real one) ---- */
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
static int f_post(void *w, void *b, int n, const int *r, int f) {
    (void)w; (void)b; (void)n; (void)r; (void)f;
    if (!strcmp(window_id, "1300")) surface_made = 1;
    ++posts;
    return 0;
}

/* A pad: tap A every 0.8 s from 1 s on (title -> menu -> New Game -> episode -> skill -> playing),
 * then hold the left stick forward for 1.5 s and fire with RT. */
static int launcher_run;   /* --launcher: go through the game list first */
static void *pad(void *arg) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0), i;
    struct sockaddr_in a;
    char m[96];
    xpad_state s;
    (void)arg;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(FD_PORT);
#define SEND() do { xpad_format(&s, m, sizeof m); sendto(fd, m, strlen(m), 0, (struct sockaddr *)&a, sizeof a); } while (0)
    mps_sleep_ms(1000);
    if (launcher_run) {        /* the game list: down one, A */
        memset(&s, 0, sizeof s); s.buttons = XPAD_DOWN; SEND(); mps_sleep_ms(150);
        memset(&s, 0, sizeof s); SEND(); mps_sleep_ms(300);
        memset(&s, 0, sizeof s); s.buttons = XPAD_A; SEND(); mps_sleep_ms(150);
        memset(&s, 0, sizeof s); SEND(); mps_sleep_ms(1500);
    }
    for (i = 0; i < 5; ++i) {
        memset(&s, 0, sizeof s); s.buttons = XPAD_A; SEND(); mps_sleep_ms(150);
        memset(&s, 0, sizeof s); SEND(); mps_sleep_ms(650);
    }
    memset(&s, 0, sizeof s); s.ly = 30000; s.rt = 255; SEND(); mps_sleep_ms(1500);
    memset(&s, 0, sizeof s); SEND();
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    int phase = argc > 1 && !strcmp(argv[1], "--phase2") ? 2 : 1;
    launcher_run = argc > 1 && !strcmp(argv[1], "--launcher");
    if (launcher_run) phase = 0;
    static const mps_api fake = {f_create_context, f_destroy_context, f_create_window, f_destroy_window,
                                 f_create_buffers, f_set_iv, f_set_cv, f_get_window_pv, f_get_buffer_pv, f_post};
    pthread_t t;
    unsigned lit = 0, k, f, outside_ok = 1;
    api = fake;
    runner = fake_runner;
    pthread_create(&t, 0, pad, 0);
    {   /* Continue: the newest save wins, by modification time; none -> -1 */
        const char *dir = "build/state/savetest/";
        char path[256];
        FILE *f;
        struct utimbuf old = {1000000, 1000000};
        mkdir("build/state", 0755);
        mkdir("build/state/savetest", 0755);
        remove("build/state/savetest/doomsav1.dsg");
        remove("build/state/savetest/doomsav4.dsg");
        assert(fd_newest_save(dir) == -1);
        snprintf(path, sizeof path, "%sdoomsav1.dsg", dir);
        f = fopen(path, "wb"); assert(f); fputs("save", f); fclose(f);
        snprintf(path, sizeof path, "%sdoomsav4.dsg", dir);
        f = fopen(path, "wb"); assert(f); fputs("save", f); fclose(f);
        utime(path, &old);                                /* slot 4 is older */
        assert(fd_newest_save(dir) == 1);
        {   /* game files: the hard-drive games folder first, the plugin folder second */
            char found[512];
            struct stat st;
            mkdir("build/state/games", 0755);
            mkdir("build/state/games/Freedoom", 0755);
            remove("build/state/games/Freedoom/freedoom1.wad");
            assert(fd_find_wad("freedoom1.wad", found, sizeof found, &st) && strstr(found, "freedoom-0.13.0"));
            f = fopen("build/state/games/Freedoom/freedoom1.wad", "wb"); assert(f); fputs("IWAD", f); fclose(f);
            assert(fd_find_wad("freedoom1.wad", found, sizeof found, &st) && strstr(found, "games/Freedoom"));
            remove("build/state/games/Freedoom/freedoom1.wad");
            assert(!fd_find_wad("nothing.wad", found, sizeof found, &st) && strstr(found, "games/Freedoom/nothing.wad"));
            printf("freedoom files: the hard-drive games folder is used first, the plugin folder second\n");
        }
        printf("freedoom continue: the newest of the saved games is picked (slot 2 in the menu)\n");
    }
    {   /* the game list: Freedoom's two, plus the owner's own IWADs in yours/ (add-on PWADs are not games) */
        FILE *w;
        unsigned k, doom2 = 0;
        mkdir("build/state/games", 0755);
        mkdir("build/state/games/Freedoom", 0755);
        mkdir("build/state/games/Freedoom/yours", 0755);
        w = fopen("build/state/games/Freedoom/yours/DOOM2.WAD", "wb"); assert(w); fputs("IWAD....", w); fclose(w);
        w = fopen("build/state/games/Freedoom/yours/sigil.wad", "wb"); assert(w); fputs("PWAD....", w); fclose(w);
        fd_list_games();
        for (k = 0; k < fd_game_count; ++k)
            if (!strcmp(fd_games[k].title, "Doom II") && !strcmp(fd_games[k].sub, "yours-doom2")) doom2 = 1;
        assert(fd_game_count == 3 && !strcmp(fd_games[0].title, "Freedoom: Phase 1") && !strcmp(fd_games[1].sub, "phase2") && doom2);
        printf("freedoom games: Phase 1, Phase 2 and the owner's Doom II are listed; an add-on level file is not\n");
    }
    {   /* Game Room: Tyrian and the Quake games join the list when their programs and files are there */
        const char *prog, *a1, *a2, *a3;
        unsigned k;
        int ty = -1, lq = -1, qu = -1, slot = 0;
        FILE *w;
        struct utimbuf later;
        mkdir("build/state/gr-bin", 0755);
        mkdir("build/state/games/Tyrian", 0755);
        mkdir("build/state/games/LibreQuake", 0755);
        mkdir("build/state/games/LibreQuake/yours", 0755);
        mkdir("build/state/games/LibreQuake/yours/id1", 0755);
        mkdir("build/state/LibreQuake-data", 0755);
        mkdir("build/state/LibreQuake-data/quake", 0755);
        mkdir("build/state/LibreQuake-data/quake/id1", 0755);
        w = fopen("build/state/games/Tyrian/tyrian1.lvl", "wb"); assert(w); fputs("lvl", w); fclose(w);
        w = fopen("build/state/games/LibreQuake/pak0.pak", "wb"); assert(w); fputs("PACK", w); fclose(w);
        w = fopen("build/state/games/LibreQuake/yours/id1/pak0.pak", "wb"); assert(w); fputs("PACK", w); fclose(w);
        remove("build/state/gr-bin/tyrian-qnx");
        remove("build/state/gr-bin/quake-qnx");
        snprintf(gr_dir, sizeof gr_dir, "build/state/gr-bin");
        fd_list_games();
        assert(fd_game_count == 3);                       /* no programs next to us: Doom games only */
        w = fopen("build/state/gr-bin/tyrian-qnx", "wb"); assert(w); fputs("#!/bin/sh\n", w); fclose(w);
        w = fopen("build/state/gr-bin/quake-qnx", "wb"); assert(w); fputs("#!/bin/sh\n", w); fclose(w);
        chmod("build/state/gr-bin/tyrian-qnx", 0755);
        chmod("build/state/gr-bin/quake-qnx", 0755);
        fd_list_games();
        for (k = 0; k < fd_game_count; ++k) {
            if (!strcmp(fd_games[k].title, "Tyrian") && fd_games[k].engine == 't') ty = (int)k;
            if (!strcmp(fd_games[k].title, "LibreQuake") && fd_games[k].engine == 'q') lq = (int)k;
            if (!strcmp(fd_games[k].title, "Quake") && fd_games[k].engine == 'q') qu = (int)k;
        }
        assert(fd_game_count == 6 && ty == 3 && lq == 4 && qu == 5);
        gr_args(ty, &prog, &a1, &a2, &a3);
        assert(!strcmp(prog, "tyrian-qnx") && !strcmp(a1, "--play") && !a2);
        gr_args(qu, &prog, &a1, &a2, &a3);
        assert(!strcmp(prog, "quake-qnx") && !strcmp(a2, "--game") && !strcmp(a3, "quake"));
        gr_args(lq, &prog, &a1, &a2, &a3);
        assert(!strcmp(a3, "librequake"));
        gr_args(-1, &prog, &a1, &a2, &a3);
        assert(!strcmp(prog, "quake-qnx") && !strcmp(a2, "--continue"));
        /* a Quake save newer than any Doom save: Continue goes to Quake */
        w = fopen("build/state/LibreQuake-data/quake/id1/s0.sav", "wb"); assert(w); fputs("save", w); fclose(w);
        later.actime = later.modtime = time(0) + 3600;
        utime("build/state/LibreQuake-data/quake/id1/s0.sav", &later);
        assert(fd_newest_of_all(&slot) == -3 && slot == -1);
        remove("build/state/LibreQuake-data/quake/id1/s0.sav");
        assert(fd_newest_of_all(&slot) != -3);
        {   /* the console games: listed by system after the PC games, each started by its emulator */
            int nes = -1, ps1 = -1;
            mkdir("build/state/games/Retro", 0755);
            mkdir("build/state/games/Retro/yours", 0755);
            w = fopen("build/state/games/Retro/Thwaite.nes", "wb"); assert(w); fputs("NES", w); fclose(w);
            w = fopen("build/state/games/Retro/yours/Tomb Raider.chd", "wb"); assert(w); fputs("MComprHD", w); fclose(w);
            w = fopen("build/state/games/Retro/yours/scph1001.bin", "wb"); assert(w); fputs("bios", w); fclose(w);
            w = fopen("build/state/gr-bin/nes-qnx", "wb"); assert(w); fputs("#!/bin/sh\n", w); fclose(w);
            w = fopen("build/state/gr-bin/ps1-qnx", "wb"); assert(w); fputs("#!/bin/sh\n", w); fclose(w);
            chmod("build/state/gr-bin/nes-qnx", 0755);
            chmod("build/state/gr-bin/ps1-qnx", 0755);
            fd_list_games();
            for (k = 0; k < fd_game_count; ++k) {
                if (!strcmp(fd_games[k].title, "NES: Thwaite") && fd_games[k].engine == 'r') nes = (int)k;
                if (!strcmp(fd_games[k].title, "PS1: Tomb Raider") && fd_games[k].engine == 'r') ps1 = (int)k;
            }
            assert(fd_game_count == 8 && nes == 6 && ps1 == 7);   /* the BIOS is no game */
            gr_args(ps1, &prog, &a1, &a2, &a3);
            assert(!strcmp(prog, "ps1-qnx") && !strcmp(a1, "--play-rom") && strstr(a2, "yours/Tomb Raider.chd") && !a3);
            gr_args(nes, &prog, &a1, &a2, &a3);
            assert(!strcmp(prog, "nes-qnx") && strstr(a2, "Retro/Thwaite.nes"));
            /* The same PBP extension keeps its console identity by folder. */
            mkdir("build/state/games/PSP",0755);
            mkdir("build/state/games/PSP/yours",0755);
            w=fopen("build/state/games/PSP/yours/PSP title.pbp","wb");assert(w);fputs("PBP",w);fclose(w);
            w=fopen("build/state/games/PSP/yours/Wrong.nes","wb");assert(w);fputs("NES",w);fclose(w);
            w=fopen("build/state/gr-bin/psp-qnx","wb");assert(w);fputs("#!/bin/sh\n",w);fclose(w);chmod("build/state/gr-bin/psp-qnx",0755);
            fd_list_games();
            assert(fd_game_count==9 && !strcmp(fd_games[8].title,"PSP: PSP title") && fd_games[8].core=='s');
            gr_args(8,&prog,&a1,&a2,&a3);
            assert(!strcmp(prog,"psp-qnx") && !strcmp(a1,"--play-rom") && strstr(a2,"PSP/yours/PSP title.pbp") && !a3);
            /* Y on a PSP game in the list: its buttons, kept in psp-qnx's save folder under the very path it is
             * started with, so psp-qnx finds them; other games have their own Change buttons in their menu */
            assert(fd_is_psp(8) && !fd_is_psp(6) && !fd_is_psp(-1) && !fd_is_psp((int)fd_game_count));
            { rm_map m, back;
              rm_defaults(&rm_psp,&m); rm_assign(&rm_psp,&m,0,RM_RT);
              assert(rm_save(&rm_psp,GR_PSP_SAVE,fd_games[8].wad,&m));
              assert(rm_load(&rm_psp,GR_PSP_SAVE,a2,&back) && back.src[0]==RM_RT);
              rm_defaults(&rm_psp,&m); assert(rm_save(&rm_psp,GR_PSP_SAVE,a2,&m) && !rm_load(&rm_psp,GR_PSP_SAVE,a2,&back)); }
            w=fopen("build/state/games/PSP/yours/PSP title.chd","wb");assert(w);fputs("CHD",w);fclose(w);
            fd_list_games();
            { unsigned j;int found=0;
              for(j=0;j<fd_game_count;++j)if(strstr(fd_games[j].wad,"PSP/yours/PSP title.chd")) {
                assert(fd_games[j].core=='s');gr_args(j,&prog,&a1,&a2,&a3);
                assert(!strcmp(prog,"psp-qnx") && strstr(a2,"PSP/yours/PSP title.chd"));found=1;
              }
              assert(found && fd_game_count==10);
            }
            remove("build/state/games/PSP/yours/PSP title.chd");
            remove("build/state/gr-bin/psp-qnx");
            remove("build/state/games/PSP/yours/PSP title.pbp");
            remove("build/state/games/PSP/yours/Wrong.nes");
            remove("build/state/gr-bin/nes-qnx");
            remove("build/state/gr-bin/ps1-qnx");
        }
        remove("build/state/gr-bin/tyrian-qnx");
        remove("build/state/gr-bin/quake-qnx");
        fd_list_games();
        assert(fd_game_count == 3);
        printf("game room: Tyrian, Quake and the console games (NES, Game Boy, PS1) join the list and start their own programs; a newer Quake save is Continue\n");
    }
    if (phase == 2) printf("freedoom: Phase 2 (freedoom2.wad)\n");
    if (launcher_run) printf("freedoom: through the game list (down, A)\n");
    assert(fd_play(35 * 9, phase, 0) == 0);                 /* 9 s of Doom tics */
    pthread_join(t, 0);

    assert(!strcmp(window_id, "1300") && windows == 0);
    assert(!strcmp(order_text, "256(0x100)"));    /* on top while playing, taken off at the end */
    assert(screen.frame >= 200 && posts >= 200);
    assert(pad_msgs >= 10);
    /* the last frame that was shown: real pixels in the game area, opaque black around it */
    f = (screen.frame + 1u) % 2u;                  /* fds_close cleared the other one */
    for (k = 0; k < (unsigned)(kGameW * kGameH); k += 97) {
        uint32_t v = buffers[f][(size_t)(kGameY + k / kGameW) * kPanelW + kGameX + k % kGameW];
        if ((v & 0xFF000000u) == 0xFF000000u && (v & 0x00FFFFFFu)) ++lit;
    }
    for (k = 0; k < (unsigned)kPanelH; k += 7)
        if (buffers[f][(size_t)k * kPanelW + 5] != 0xFF000000u) outside_ok = 0;
    {   /* the last frame shown, for a look (build/freedoom-last.ppm) */
        FILE *ppm = fopen(FD_SHOT, "wb");
        if (ppm) {
            unsigned x, y;
            fprintf(ppm, "P6 %d %d 255\n", kPanelW, kPanelH);
            for (y = 0; y < (unsigned)kPanelH; ++y)
                for (x = 0; x < (unsigned)kPanelW; ++x) {
                    uint32_t v = buffers[f][(size_t)y * kPanelW + x];
                    unsigned char rgb[3] = {(unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v};
                    fwrite(rgb, 1, 3, ppm);
                }
            fclose(ppm);
        }
    }
    printf("freedoom: %u frames, %u pad messages, %u/%u sampled game pixels lit, border %s, layer restored to %s\n",
           screen.frame, pad_msgs, lit, (unsigned)(kGameW * kGameH + 96) / 97, outside_ok ? "black" : "WRONG",
           order_text);
    assert(lit > 500 && outside_ok);
    printf("freedoom sound: %u sound effects started (menu clicks, pistol)\n", fd_sound_starts());
    printf("freedoom sound: %u mixed blocks (23 ms each) carried sound\n", fd_sound_loud_blocks());
    printf("freedoom music: %u mixed blocks carried music (title tune, then E1M1), loudest sample %u of 32767\n",
           fd_sound_music_blocks(), fd_sound_peak());
    printf("freedoom music: %u samples clipped in the chip, %u when mixed with the effects\n", fd_opl_chip_clips(),
           fd_opl_mix_clips());
    assert(fd_sound_starts() >= (launcher_run ? 2u : 3u) && fd_sound_loud_blocks() >= 5);   /* the list takes some of the time */
    assert(fd_sound_music_blocks() >= 100);
    return 0;
}
