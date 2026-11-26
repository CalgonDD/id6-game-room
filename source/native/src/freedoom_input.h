/* freedoom_input.h: Xbox pad state -> Doom key presses/releases (host-tested in tests/test_freedoom_input.c).
 *
 * Layout (2026-11-19, first playable):
 *   left stick  up/down = walk forward/back      left/right = strafe
 *   right stick left/right = turn                d-pad = walk/turn (and menu navigation)
 *   RT or X = fire        A = use / open doors (and Enter in menus)
 *   LT or left-stick click = run                 B = menu back
 *   LB / RB = previous / next weapon             Y or View = automap
 *   Menu (start) = game menu (Escape)
 *   Yes/no questions: A or Y = yes, B = no. Saving: an empty slot gets a name (fd_save_name), A saves.
 * Sticks are digital with a dead zone; good enough for a first playable. */
#ifndef FREEDOOM_INPUT_H
#define FREEDOOM_INPUT_H

#include <stdint.h>
#include <stdio.h>
#include "xpad.h"

#ifndef FD_KEY_PREVWEAPON
#define FD_KEY_PREVWEAPON 0xF0   /* bound to key_prevweapon / key_nextweapon by the worker */
#define FD_KEY_NEXTWEAPON 0xF1
#endif

/* Doom key codes (doomkeys.h), repeated so this header stands alone. */
enum {
    FDK_RIGHT = 0xae, FDK_LEFT = 0xac, FDK_UP = 0xad, FDK_DOWN = 0xaf, FDK_STRAFE_L = 0xa0, FDK_STRAFE_R = 0xa1,
    FDK_USE = 0xa2, FDK_FIRE = 0xa3, FDK_ESCAPE = 27, FDK_ENTER = 13, FDK_TAB = 9, FDK_BACKSPACE = 0x7f,
    FDK_RSHIFT = 0x80 + 0x36
};

enum {
    FDV_UP, FDV_DOWN, FDV_TURN_L, FDV_TURN_R, FDV_STRAFE_L, FDV_STRAFE_R, FDV_FIRE, FDV_USE, FDV_RUN, FDV_BACK,
    FDV_MAP, FDV_MENU, FDV_PREV, FDV_NEXT, FDV_COUNT
};

#define FD_STICK 12000          /* dead zone, of 32767 */
#define FD_TRIGGER 100          /* of 255 */

/* Which virtual buttons the pad state holds down. */
static inline uint32_t fd_virtual(const xpad_state *s) {
    uint32_t v = 0;
    if (s->ly > FD_STICK || (s->buttons & XPAD_UP)) v |= 1u << FDV_UP;
    if (s->ly < -FD_STICK || (s->buttons & XPAD_DOWN)) v |= 1u << FDV_DOWN;
    if (s->rx < -FD_STICK || (s->buttons & XPAD_LEFT)) v |= 1u << FDV_TURN_L;
    if (s->rx > FD_STICK || (s->buttons & XPAD_RIGHT)) v |= 1u << FDV_TURN_R;
    if (s->lx < -FD_STICK) v |= 1u << FDV_STRAFE_L;
    if (s->lx > FD_STICK) v |= 1u << FDV_STRAFE_R;
    if (s->rt > FD_TRIGGER || (s->buttons & XPAD_X)) v |= 1u << FDV_FIRE;
    if (s->buttons & XPAD_A) v |= 1u << FDV_USE;
    if (s->lt > FD_TRIGGER || (s->buttons & XPAD_LS)) v |= 1u << FDV_RUN;
    if (s->buttons & XPAD_B) v |= 1u << FDV_BACK;
    if (s->buttons & (XPAD_Y | XPAD_BACK)) v |= 1u << FDV_MAP;
    if (s->buttons & XPAD_START) v |= 1u << FDV_MENU;
    if (s->buttons & XPAD_LB) v |= 1u << FDV_PREV;
    if (s->buttons & XPAD_RB) v |= 1u << FDV_NEXT;
    return v;
}

/* Up to two Doom keys per virtual button (0 = none). */
static const uint8_t fd_keys[FDV_COUNT][2] = {
    {FDK_UP, 0}, {FDK_DOWN, 0}, {FDK_LEFT, 0}, {FDK_RIGHT, 0}, {FDK_STRAFE_L, 0}, {FDK_STRAFE_R, 0},
    {FDK_FIRE, 0}, {FDK_USE, FDK_ENTER}, {FDK_RSHIFT, 0}, {FDK_BACKSPACE, 0}, {FDK_TAB, 0}, {FDK_ESCAPE, 0},
    {FD_KEY_PREVWEAPON, 0}, {FD_KEY_NEXTWEAPON, 0}
};

/* Key event queue, filled from pad changes, emptied by DG_GetKey. */
#define FD_QUEUE 64
typedef struct {
    uint16_t ev[FD_QUEUE];   /* pressed << 8 | key */
    unsigned head, tail;
    uint32_t held;           /* virtual buttons currently down */
} fd_input;

static inline void fd_push(fd_input *q, int pressed, uint8_t key) {
    unsigned next = (q->tail + 1u) % FD_QUEUE;
    if (next == q->head) return;              /* full: drop (never block the game) */
    q->ev[q->tail] = (uint16_t)((pressed ? 1u : 0u) << 8 | key);
    q->tail = next;
}

/* New pad state: queue a press or release for every virtual button that changed. */
static inline void fd_update(fd_input *q, const xpad_state *s) {
    uint32_t now = fd_virtual(s), diff = now ^ q->held;
    unsigned v, k;
    for (v = 0; v < FDV_COUNT; ++v) {
        if (!(diff & (1u << v))) continue;
        for (k = 0; k < 2; ++k)
            if (fd_keys[v][k]) fd_push(q, (now >> v) & 1u, fd_keys[v][k]);
    }
    q->held = now;
}

/* Releases everything held (pad unplugged, page lost focus). */
static inline void fd_release_all(fd_input *q) {
    xpad_state none;
    memset(&none, 0, sizeof none);
    fd_update(q, &none);
}

/* While the game asks a yes/no question ("press Y to quit"), A (Enter) and Y (Tab) answer yes and B (Backspace)
 * answers no. Doom only takes the letters (key_menu_confirm 'y', key_menu_abort 'n'). */
static inline unsigned char fd_prompt_key(unsigned char key) {
    if (key == FDK_ENTER || key == FDK_TAB) return 'y';
    if (key == FDK_BACKSPACE) return 'n';
    return key;
}

/* The name a new save gets, as nobody can type on the pad: "E1M3 SAVE 2", or "MAP07 SAVE 2" in Phase 2 (a
 * Doom II style game: maps, no episodes). slot is 0-based. */
static inline void fd_save_name(char *out, unsigned size, int maps_only, int episode, int map, int slot) {
    if (maps_only) snprintf(out, size, "MAP%02d SAVE %d", map, slot + 1);
    else snprintf(out, size, "E%dM%d SAVE %d", episode, map, slot + 1);
}

static inline int fd_pop(fd_input *q, int *pressed, unsigned char *key) {
    if (q->head == q->tail) return 0;
    *pressed = q->ev[q->head] >> 8;
    *key = (unsigned char)(q->ev[q->head] & 0xFFu);
    q->head = (q->head + 1u) % FD_QUEUE;
    return 1;
}

#endif
