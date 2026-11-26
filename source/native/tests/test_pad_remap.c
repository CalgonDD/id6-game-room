/* Your own buttons (src/pad_remap.h, src/pad_remap_screen.h): the layouts, swaps, the saved file (and what is
 * not trusted), and the "Change buttons" screen driven by pad states, with pictures of it for a look. */
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include "font8x8_basic.h"
#include "../src/pad_remap_screen.h"
#include "../src/nes_input.h"

static xpad_state pad(uint16_t buttons) {
    xpad_state s;
    memset(&s, 0, sizeof s);
    s.buttons = buttons;
    return s;
}
static void save_ppm(const char *name, const uint32_t *px, int w, int h) {
    FILE *f = fopen(name, "wb");
    int i;
    assert(f);
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; ++i) {
        unsigned char rgb[3] = {(unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
static unsigned lit(const uint32_t *px, int w, int h) {
    unsigned n = 0;
    int i;
    for (i = 0; i < w * h; ++i) n += px[i] == RM_WHITE || px[i] == RM_GREY || px[i] == RM_ACCENT;
    return n;
}
/* One look with these buttons held, then one with nothing (a press and a release). */
static int tap(rm_screen *r, uint16_t buttons, uint64_t *now) {
    xpad_state s = pad(buttons), none = pad(0);
    int a = rm_step(r, &s, *now += 40);
    int b = rm_step(r, &none, *now += 40);
    return a | b;
}

int main(void) {
    rm_map m;
    xpad_state s;
    char dir[] = "build/remap-test";
    FILE *f;
    char path[700];
    static uint32_t px[640 * 480];

    /* defaults: the built-in main buttons, and not "active" */
    rm_defaults(&rm_nes, &m);
    assert(rm_is_default(&rm_nes, &m) && !m.active);
    assert(m.src[8] == 11 && m.src[0] == 13 && m.src[3] == 4 && m.src[2] == 5);
    rm_defaults(&rm_ps1, &m);
    assert(m.src[0] == 11 && m.src[8] == 12 && m.src[12] == RM_LT && m.src[13] == RM_RT && m.src[15] == 7);

    /* the buttons under a layout: directions kept from the built-in mapping, the rest from the layout */
    rm_defaults(&rm_nes, &m);
    s = pad(XPAD_A | XPAD_UP);
    assert(rm_buttons(&rm_nes, &m, &s, ns_retropad(&s)) == ((1u << 8) | (1u << 4)));
    s = pad(XPAD_B);   /* the built-in mapping also has B as NES A; a layout gives each console button one */
    assert(rm_buttons(&rm_nes, &m, &s, 0) == 0);
    s = pad(XPAD_START | XPAD_BACK);   /* both together open the menu: neither reaches the game */
    assert(rm_buttons(&rm_nes, &m, &s, 0) == 0);
    s = pad(XPAD_START);
    assert(rm_buttons(&rm_nes, &m, &s, 0) == (1u << 3));

    /* swap: NES A onto X, so NES B (which had X) takes A */
    rm_assign(&rm_nes, &m, 0, 13);
    assert(m.src[8] == 13 && m.src[0] == 11 && !rm_is_default(&rm_nes, &m));
    rm_assign(&rm_nes, &m, 0, 0);   /* the d-pad is not a button for a layout */
    rm_assign(&rm_nes, &m, 0, 10);  /* nor the Xbox button */
    assert(m.src[8] == 13);

    /* triggers: PS1 Cross on RT (Square... unchanged), and R2 takes A */
    rm_defaults(&rm_ps1, &m);
    rm_assign(&rm_ps1, &m, 0, RM_RT);
    assert(m.src[0] == RM_RT && m.src[13] == 11);
    s = pad(0);
    s.rt = 200;
    assert(rm_buttons(&rm_ps1, &m, &s, 0) == 1u);
    s.rt = 50;
    assert(rm_buttons(&rm_ps1, &m, &s, 0) == 0);

    /* saving and loading, per game file name (not folder) */
    mkdir("build", 0755);
    mkdir(dir, 0755);
    assert(rm_save(&rm_ps1, dir, "/mnt/data/ID6Games/Retro/yours/Some Game.chd", &m) && m.active);
    {
        rm_map back;
        assert(rm_load(&rm_ps1, dir, "/somewhere/else/Some Game.chd", &back) && back.active);
        assert(!memcmp(back.src, m.src, sizeof m.src));
        assert(!rm_load(&rm_ps1, dir, "/somewhere/else/Other Game.chd", &back) && !back.active);
        assert(!rm_load(&rm_psp, dir, "/x/Some Game.chd", &back));   /* another console's layouts are separate */
    }
    /* back to the default: the file goes */
    rm_path(&rm_ps1, dir, "Some Game.chd", path, sizeof path);
    rm_defaults(&rm_ps1, &m);
    assert(rm_save(&rm_ps1, dir, "Some Game.chd", &m) && !m.active);
    assert(access(path, F_OK) != 0);
    /* files that are not trusted: wrong header, missing buttons, two buttons on one Xbox button, bad numbers */
    rm_path(&rm_nes, dir, "Bad.nes", path, sizeof path);
    f = fopen(path, "w"); fputs("something else\n8 13\n0 11\n3 4\n2 5\n", f); fclose(f);
    assert(!rm_load(&rm_nes, dir, "Bad.nes", &m) && m.src[8] == 11);
    f = fopen(path, "w"); fputs("game-room-controls 1\n8 13\n0 11\n", f); fclose(f);
    assert(!rm_load(&rm_nes, dir, "Bad.nes", &m) && m.src[8] == 11);
    f = fopen(path, "w"); fputs("game-room-controls 1\n8 13\n0 13\n3 4\n2 5\n", f); fclose(f);
    assert(!rm_load(&rm_nes, dir, "Bad.nes", &m) && m.src[8] == 11);
    f = fopen(path, "w"); fputs("game-room-controls 1\n8 13\n0 99\n3 4\n2 5\n", f); fclose(f);
    assert(!rm_load(&rm_nes, dir, "Bad.nes", &m) && m.src[8] == 11);
    f = fopen(path, "w"); fputs("game-room-controls 1\n8 13\n0 11\n3 4\n2 5\n", f); fclose(f);
    assert(rm_load(&rm_nes, dir, "Bad.nes", &m) && m.src[8] == 13 && m.src[0] == 11);
    remove(path);

    /* the screen */
    {
        rm_screen r;
        uint64_t now = 1000;
        xpad_state held_a = pad(XPAD_A), none = pad(0);
        rm_defaults(&rm_nes, &m);
        rm_open(&r, &rm_nes, &m, "/mnt/data/ID6Games/Retro/Nova the Squirrel.nes", &held_a);
        assert(!strcmp(r.game, "Nova the Squirrel") && r.cursor == 0 && !r.waiting);
        /* the A that opened it is still held: not a press */
        assert(rm_step(&r, &held_a, now += 40) == RM_STAY && !r.waiting);
        assert(rm_step(&r, &none, now += 40) == RM_STAY);
        rm_draw(&r, px, 512, 480);
        assert(lit(px, 512, 480) > 2000);
        save_ppm("build/remap-nes.ppm", px, 512, 480);
        /* NES B: A, then press Y */
        assert(tap(&r, XPAD_DOWN, &now) == RM_STAY && r.cursor == 1);
        assert(tap(&r, XPAD_A, &now) == RM_STAY && r.waiting);
        rm_draw(&r, px, 512, 480);
        save_ppm("build/remap-nes-waiting.ppm", px, 512, 480);
        assert(tap(&r, XPAD_Y, &now) == RM_STAY && !r.waiting && r.map.src[0] == 14 && r.cursor == 2);
        /* NES Start: A, then the d-pad cancels */
        assert(tap(&r, XPAD_A, &now) == RM_STAY && r.waiting);
        assert(tap(&r, XPAD_LEFT, &now) == RM_STAY && !r.waiting && r.map.src[3] == 4);
        /* NES Start again: nobody presses, it gives up */
        assert(tap(&r, XPAD_A, &now) == RM_STAY && r.waiting);
        now += RM_WAIT_MS;
        assert(rm_step(&r, &none, now) == RM_STAY && !r.waiting);
        /* NES A onto B (the B the screen otherwise leaves with is a button choice while waiting) */
        assert(tap(&r, XPAD_UP, &now) == RM_STAY && tap(&r, XPAD_UP, &now) == RM_STAY && r.cursor == 0);
        assert(tap(&r, XPAD_A, &now) == RM_STAY && r.waiting);
        assert(tap(&r, XPAD_B, &now) == RM_STAY && r.map.src[8] == 12 && r.cursor == 1);
        /* B leaves (not waiting) */
        assert(tap(&r, XPAD_B, &now) == RM_DONE);
        assert(r.map.src[8] == 12 && r.map.src[0] == 14 && !rm_is_default(&rm_nes, &r.map));
        /* Reset to default, then Done (opened again on that layout, as saved: active) */
        r.map.active = 1;
        rm_open(&r, &rm_nes, &r.map, "Nova.nes", &none);
        assert(r.map.src[8] == 12);
        while (r.cursor < rm_nes.count) assert(tap(&r, XPAD_DOWN, &now) == RM_STAY);
        assert(tap(&r, XPAD_A, &now) == RM_STAY && rm_is_default(&rm_nes, &r.map));
        assert(tap(&r, XPAD_DOWN, &now) == RM_STAY && r.cursor == rm_nes.count + 1);
        assert(tap(&r, XPAD_A, &now) == RM_DONE);
        /* the Xbox button leaves too */
        rm_open(&r, &rm_nes, &m, "Nova.nes", &none);
        assert(tap(&r, XPAD_GUIDE, &now) == RM_DONE);
        /* holding down repeats (after 0.4 s), through the PS1's longer list, which scrolls */
        rm_defaults(&rm_ps1, &m);
        rm_open(&r, &rm_ps1, &m, "Some PS1 Game.chd", &none);
        s = pad(XPAD_DOWN);
        assert(rm_step(&r, &s, now += 40) == RM_STAY && r.cursor == 1);
        now += 400;
        while (now < 10000000u && r.cursor < rm_ps1.count + 1) assert(rm_step(&r, &s, now += 130) == RM_STAY);
        assert(r.cursor == rm_ps1.count + 1);
        rm_draw(&r, px, 640, 480);
        save_ppm("build/remap-ps1-bottom.ppm", px, 640, 480);
        /* the PSP one in the Game Room list's 640 x 400 picture */
        rm_defaults(&rm_psp, &m);
        rm_open(&r, &rm_psp, &m, "/mnt/data/ID6Games/PSP/yours/A Long PSP Game Name That Goes On.iso", &none);
        rm_draw(&r, px, 640, 400);
        assert(lit(px, 640, 400) > 2000);
        {   /* all ten rows fit: "Done" is drawn without scrolling */
            unsigned visible = (unsigned)((400 - 150) / RM_ROW_H(400));
            assert(visible >= rm_psp.count + 2);
        }
        save_ppm("build/remap-psp.ppm", px, 640, 400);
    }
    rmdir(dir);
    printf("pad remap: layouts, swaps, triggers, saved files (bad ones ignored), and the screen all work\n");
    return 0;
}
