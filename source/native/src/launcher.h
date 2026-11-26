/* launcher.h: the game list a game process shows on its own screen layer before it starts a game
 * (Freedoom and LibreQuake; Retro has its own picker). Drawn with the public-domain 8x8 font into a
 * kGameW x kGameH picture (freedoom_screen.h sizes), so the iDrive page never changes.
 *   d-pad or left stick up/down: choose      A: start      B: leave (back to the iDrive screen)
 * Needs freedoom_screen.h (kGameW, kGameH) and xpad.h first. Host-tested in tests/test_launcher.c. */
#ifndef LAUNCHER_H
#define LAUNCHER_H

#include <stdint.h>
#include <string.h>
#include "font8x8_basic.h"

#define LC_WHITE 0xFFFFFFFFu
#define LC_GREY 0xFF8C97A6u
#define LC_ACCENT 0xFFFF4F1Au
#define LC_BG 0xFF0B0F14u
#define LC_SEL 0xFF263342u
#define LC_STICK 16000
#define LC_MAX 240                  /* Game Room lists every game: PC ones and the consoles' */

enum { LC_UP = 1, LC_DOWN = 2, LC_OK = 4, LC_BACK = 8 };

typedef struct {
    const char *title;              /* the heading (e.g. "DOOM GAMES") */
    const char *items[LC_MAX];      /* entries, top to bottom */
    unsigned count, cursor;
    const char *note;               /* a line shown under the list, or 0 */
    uint32_t held;                  /* navigation bits held at the last look */
    uint64_t repeat_at;
} lc_list;

static void lc_fill(uint32_t *px, uint32_t c) {
    unsigned i;
    for (i = 0; i < (unsigned)(kGameW * kGameH); ++i) px[i] = c;
}

static void lc_bar(uint32_t *px, int y, int h, uint32_t c) {
    int i, j;
    for (j = y < 0 ? 0 : y; j < y + h && j < kGameH; ++j)
        for (i = 0; i < kGameW; ++i) px[j * kGameW + i] = c;
}

static void lc_text(uint32_t *px, int x, int y, const char *s, uint32_t colour, int scale) {
    for (; *s; ++s, x += 8 * scale) {
        unsigned char c = (unsigned char)*s;
        int r, k, a, b;
        if (c >= 128) c = '?';
        for (r = 0; r < 8; ++r)
            for (k = 0; k < 8; ++k)
                if (font8x8_basic[c][r] & (1 << k))
                    for (a = 0; a < scale; ++a)
                        for (b = 0; b < scale; ++b) {
                            int x2 = x + k * scale + b, y2 = y + r * scale + a;
                            if (x2 >= 0 && x2 < kGameW && y2 >= 0 && y2 < kGameH) px[y2 * kGameW + x2] = colour;
                        }
    }
}

/* Draws the list: heading, up to as many rows as fit (scrolling with the cursor), the note and the help line. */
static void lc_draw(const lc_list *l, uint32_t *px) {
    int rows = (kGameH - 150) / 34, first, i, max_chars = (kGameW - 64) / 16;
    char line[80];
    lc_fill(px, LC_BG);
    lc_text(px, 24, 18, l->title, LC_ACCENT, 3);
    if (rows < 1) rows = 1;
    first = (int)l->cursor >= rows ? (int)l->cursor - rows + 1 : 0;
    for (i = first; i < (int)l->count && i < first + rows; ++i) {
        int y = 80 + (i - first) * 34;
        snprintf(line, sizeof line, "%s", l->items[i]);
        if ((int)strlen(line) > max_chars) line[max_chars] = 0;
        if (i == (int)l->cursor) lc_bar(px, y - 8, 32, LC_SEL);
        lc_text(px, 32, y, line, i == (int)l->cursor ? LC_WHITE : LC_GREY, 2);
    }
    if (l->note) lc_text(px, 24, kGameH - 72, l->note, LC_WHITE, 2);
    lc_text(px, 24, kGameH - 40, "A: play   B: leave", LC_GREY, 2);
}

static uint32_t lc_nav(const xpad_state *s) {
    uint32_t n = 0;
    if ((s->buttons & XPAD_UP) || s->ly > LC_STICK) n |= LC_UP;
    if ((s->buttons & XPAD_DOWN) || s->ly < -LC_STICK) n |= LC_DOWN;
    if (s->buttons & (XPAD_A | XPAD_START)) n |= LC_OK;
    if (s->buttons & XPAD_B) n |= LC_BACK;
    return n;
}

/* One look at the pad: moves the cursor (with repeat while held) and returns LC_OK or LC_BACK when pressed
 * (each only on the press, never while held from before). now: milliseconds. */
static uint32_t lc_step(lc_list *l, const xpad_state *s, uint64_t now) {
    uint32_t cur = lc_nav(s), fresh = cur & ~l->held;
    if (fresh & (LC_UP | LC_DOWN)) l->repeat_at = now + 400u;
    else if ((cur & (LC_UP | LC_DOWN)) && now >= l->repeat_at) { fresh |= cur & (LC_UP | LC_DOWN); l->repeat_at = now + 120u; }
    l->held = cur;
    if ((fresh & LC_DOWN) && l->cursor + 1 < l->count) ++l->cursor;
    if ((fresh & LC_UP) && l->cursor > 0) --l->cursor;
    return fresh & (LC_OK | LC_BACK);
}

#endif
