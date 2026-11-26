/* pad_remap_screen.h: the "Change buttons" screen for a console game's layout (pad_remap.h). Drawn with the
 * public-domain 8x8 font into a w x h picture. Host-tested in tests/test_pad_remap.c.
 *
 *   d-pad or left stick up/down: choose a console button      A: change it, then press the Xbox button for it
 *   (if another console button had that Xbox button, the two swap)      d-pad while waiting: cancel
 *   "Reset to default": the built-in buttons      "Done", B or the Xbox button: leave (the caller saves)
 * Include font8x8_basic.h first (it has no include guard, so this file does not include it). */
#ifndef PAD_REMAP_SCREEN_H
#define PAD_REMAP_SCREEN_H

#include "pad_remap.h"

#define RM_WHITE 0xFFFFFFFFu
#define RM_GREY 0xFF8C97A6u
#define RM_ACCENT 0xFFFF4F1Au
#define RM_BG 0xFF0B0F14u
#define RM_SEL 0xFF263342u
#define RM_STICK 16000
#define RM_WAIT_MS 6000u      /* waiting for a press gives up after this */
#define RM_ROW_H(h) ((h) >= 480 ? 26 : 24)   /* rows: a little tighter in the Game Room list's 640 x 400 */

enum { RM_STAY = 0, RM_DONE = 1 };

typedef struct {
    const rm_system *sys;
    rm_map map;
    unsigned cursor;          /* 0..count-1 the console buttons, count "Reset to default", count+1 "Done" */
    int waiting;              /* waiting for the Xbox button for the console button under the cursor */
    uint64_t wait_until, repeat_at;
    uint32_t held;            /* sources (bits 0..16) and RM_NAV_* bits held at the last look */
    const char *note;
    char game[40];
    int dirty;
} rm_screen;

/* Bits above the 17 sources, for the movement and leave keys. */
enum { RM_NAV_UP = 1u << 20, RM_NAV_DOWN = 1u << 21, RM_NAV_SIDE = 1u << 22, RM_NAV_LEAVE = 1u << 23 };

static inline uint32_t rm_held_now(const xpad_state *s) {
    uint32_t h = 0;
    unsigned i;
    for (i = 0; i < RM_SOURCES; ++i)
        if (rm_source_ok(i) && rm_pressed(s, i)) h |= 1u << i;
    if ((s->buttons & XPAD_UP) || s->ly > RM_STICK) h |= RM_NAV_UP;
    if ((s->buttons & XPAD_DOWN) || s->ly < -RM_STICK) h |= RM_NAV_DOWN;
    if ((s->buttons & (XPAD_LEFT | XPAD_RIGHT)) || s->lx > RM_STICK || s->lx < -RM_STICK) h |= RM_NAV_SIDE;
    if ((s->buttons & XPAD_GUIDE) || ((s->buttons & XPAD_START) && (s->buttons & XPAD_BACK))) h |= RM_NAV_LEAVE;
    return h;
}
/* game: the game's file name or path (only the name is shown). s: the pad now (whatever is held is not a press). */
static inline void rm_open(rm_screen *r, const rm_system *sys, const rm_map *map, const char *game, const xpad_state *s) {
    const char *name = strrchr(game, '/');
    char *dot;
    rm_map start = *map;   /* copied first: map may be r's own */
    memset(r, 0, sizeof *r);
    r->sys = sys;
    r->map = start;
    if (!start.active) rm_defaults(sys, &r->map);
    snprintf(r->game, sizeof r->game, "%s", name ? name + 1 : game);
    dot = strrchr(r->game, '.');
    if (dot && dot != r->game) *dot = 0;
    r->held = rm_held_now(s);
    r->repeat_at = (uint64_t)-1;   /* a direction held from before does not repeat */
    r->dirty = 1;
}

/* One look at the pad. Returns RM_DONE when the player leaves (r->map is then the layout to save). */
static inline int rm_step(rm_screen *r, const xpad_state *s, uint64_t now) {
    uint32_t cur = rm_held_now(s), fresh = cur & ~r->held;
    unsigned rows = r->sys->count + 2, i;
    if (fresh & (RM_NAV_UP | RM_NAV_DOWN)) r->repeat_at = now + 400u;
    else if ((cur & (RM_NAV_UP | RM_NAV_DOWN)) && !r->waiting && now >= r->repeat_at) {
        fresh |= cur & (RM_NAV_UP | RM_NAV_DOWN);
        r->repeat_at = now + 120u;
    }
    r->held = cur;
    if (r->waiting) {
        if ((fresh & (RM_NAV_UP | RM_NAV_DOWN | RM_NAV_SIDE)) || now >= r->wait_until) {   /* cancelled, or nobody pressed */
            r->waiting = 0;
            r->note = "Not changed.";
            r->dirty = 1;
            return RM_STAY;
        }
        for (i = 0; i < RM_SOURCES; ++i)
            if ((fresh >> i) & 1u) {
                rm_assign(r->sys, &r->map, r->cursor, i);
                r->waiting = 0;
                r->note = 0;
                if (r->cursor + 1 < r->sys->count) ++r->cursor;   /* on to the next, for setting a whole layout */
                r->dirty = 1;
                return RM_STAY;
            }
        return RM_STAY;
    }
    if (fresh & RM_NAV_LEAVE) return RM_DONE;
    if ((fresh & RM_NAV_DOWN) && !(fresh & RM_NAV_UP) && r->cursor + 1 < rows) { ++r->cursor; r->note = 0; r->dirty = 1; }
    if ((fresh & RM_NAV_UP) && !(fresh & RM_NAV_DOWN) && r->cursor > 0) { --r->cursor; r->note = 0; r->dirty = 1; }
    if (fresh & (1u << 12)) return RM_DONE;   /* B */
    if (fresh & (1u << 11)) {                 /* A */
        if (r->cursor < r->sys->count) { r->waiting = 1; r->wait_until = now + RM_WAIT_MS; r->note = 0; }
        else if (r->cursor == r->sys->count) { rm_defaults(r->sys, &r->map); r->note = "Default buttons back."; }
        else return RM_DONE;
        r->dirty = 1;
    }
    return RM_STAY;
}

static inline void rm_fill(uint32_t *px, int w, int h, uint32_t c) {
    int i;
    for (i = 0; i < w * h; ++i) px[i] = c;
}
static inline void rm_bar(uint32_t *px, int w, int h, int y, int bh, uint32_t c) {
    int i, j;
    for (j = y < 0 ? 0 : y; j < y + bh && j < h; ++j)
        for (i = 0; i < w; ++i) px[j * w + i] = c;
}
static inline void rm_text(uint32_t *px, int w, int h, int x, int y, const char *s, uint32_t colour) {
    for (; *s; ++s, x += 16) {
        unsigned char c = (unsigned char)*s;
        int r, k, a, b;
        if (c >= 128) c = '?';
        for (r = 0; r < 8; ++r)
            for (k = 0; k < 8; ++k)
                if (font8x8_basic[c][r] & (1 << k))
                    for (a = 0; a < 2; ++a)
                        for (b = 0; b < 2; ++b) {
                            int x2 = x + k * 2 + b, y2 = y + r * 2 + a;
                            if (x2 >= 0 && x2 < w && y2 >= 0 && y2 < h) px[y2 * w + x2] = colour;
                        }
    }
}

/* Draws the screen into px (w x h, at least 480 x 400). */
static inline void rm_draw(const rm_screen *r, uint32_t *px, int w, int h) {
    unsigned rows = r->sys->count + 2, visible = (unsigned)((h - 150) / RM_ROW_H(h)), first, i;
    int col = w / 2 - 16, max_chars = (w - 48) / 16;
    char line[96];
    rm_fill(px, w, h, RM_BG);
    snprintf(line, sizeof line, "BUTTONS: %s", r->sys->title);
    rm_text(px, w, h, 24, 16, line, RM_ACCENT);
    snprintf(line, sizeof line, "%s", r->game);
    if ((int)strlen(line) > max_chars) line[max_chars] = 0;
    rm_text(px, w, h, 24, 42, line, RM_GREY);
    if (visible < 1) visible = 1;
    first = r->cursor >= visible ? r->cursor - visible + 1 : 0;
    for (i = first; i < rows && i < first + visible; ++i) {
        int y = 80 + (int)(i - first) * RM_ROW_H(h);
        uint32_t c = i == r->cursor ? RM_WHITE : RM_GREY;
        if (i == r->cursor) rm_bar(px, w, h, y - 5, RM_ROW_H(h), RM_SEL);
        if (i < r->sys->count) {
            const char *src = r->waiting && i == r->cursor ? "press..." : rm_source_name(r->map.src[r->sys->b[i].slot]);
            rm_text(px, w, h, 32, y, r->sys->b[i].label, c);
            snprintf(line, sizeof line, "%s", src);
            if ((int)strlen(line) > (w - col - 16) / 16) line[(w - col - 16) / 16] = 0;
            rm_text(px, w, h, col, y, line, r->waiting && i == r->cursor ? RM_ACCENT : c);
        } else rm_text(px, w, h, 32, y, i == r->sys->count ? "Reset to default" : "Done", c);
    }
    if (r->waiting) {
        snprintf(line, sizeof line, "Press the Xbox button for %s", r->sys->b[r->cursor].label);
        if ((int)strlen(line) > max_chars) snprintf(line, sizeof line, "Press a button for %s", r->sys->b[r->cursor].label);
        rm_text(px, w, h, 24, h - 62, line, RM_WHITE);
        rm_text(px, w, h, 24, h - 34, "D-pad: cancel", RM_GREY);
        return;
    }
    if (r->note) rm_text(px, w, h, 24, h - 62, r->note, RM_WHITE);
    rm_text(px, w, h, 24, h - 34, "A: change   B: done", RM_GREY);
}

#endif
