/* quake_input.h: the wired Xbox pad for Quake (quakegeneric), host-tested in tests/test_quake_input.c.
 *
 * Every key the pad sends is bound here (LibreQuake's own default.cfg binds Ctrl to swim down and fires with the
 * mouse, so RT did nothing on the car, 2026-11-20).
 * Sticks are Quake's analog joystick (Windows-style "advanced" axes, set up by id6pad.cfg below):
 *   axis 0 = left x (strafe), 1 = left y (move), 2 = right x (turn), 3 = right y (look); -1..1, y down positive.
 * Buttons are keys. In the game:
 *   RT fire (Ctrl)  A jump (Space)  B swim down  LT run (Shift)  LB/RB previous/next weapon
 *   Y or View scores (Tab)  left-stick click centre view  Menu game menu (Escape)  d-pad arrow keys
 * In menus and the console: A Enter, B and Menu Escape, d-pad and left stick the arrow keys.
 * A key is always released as the same key it was pressed as, even if a menu opened in between. */
#ifndef QUAKE_INPUT_H
#define QUAKE_INPUT_H

#include <stdint.h>
#include <string.h>
#include "xpad.h"

/* Quake key codes (quakekeys.h), repeated so this header stands alone. */
enum {
    QK_TAB = 9, QK_ENTER = 13, QK_ESCAPE = 27, QK_SPACE = 32, QK_UP = 128, QK_DOWN = 129, QK_LEFT = 130,
    QK_RIGHT = 131, QK_CTRL = 133, QK_SHIFT = 134, QK_AUX1 = 207, QK_AUX2 = 208, QK_AUX3 = 209, QK_AUX5 = 211
};

/* Written to the game folder as id6pad.cfg and run at every start (+exec). */
#define QK_PAD_CFG \
    "joystick 1\n" \
    "joyadvanced 1\n" \
    "joyadvaxisx 3\n" \
    "joyadvaxisy 1\n" \
    "joyadvaxisz 4\n" \
    "joyadvaxisr 2\n" \
    "joysidesensitivity 1\n" \
    "joyforwardsensitivity -1\n" \
    "joyyawsensitivity -1\n" \
    "joypitchsensitivity 1\n" \
    "joyadvancedupdate\n" \
    "+mlook\n" \
    "lookspring 0\n" \
    "bind CTRL \"+attack\"\n" \
    "bind SPACE \"+jump\"\n" \
    "bind SHIFT \"+speed\"\n" \
    "bind TAB \"+showscores\"\n" \
    "bind ESCAPE \"togglemenu\"\n" \
    "bind UPARROW \"+forward\"\n" \
    "bind DOWNARROW \"+back\"\n" \
    "bind LEFTARROW \"+left\"\n" \
    "bind RIGHTARROW \"+right\"\n" \
    "bind AUX1 \"impulse 12\"\n" \
    "bind AUX2 \"impulse 10\"\n" \
    "bind AUX3 \"+movedown\"\n" \
    "bind AUX5 \"centerview\"\n"

enum { QKV_UP, QKV_DOWN, QKV_LEFT, QKV_RIGHT, QKV_A, QKV_B, QKV_FIRE, QKV_RUN, QKV_PREV, QKV_NEXT, QKV_SCORES,
       QKV_CENTER, QKV_MENU, QKV_COUNT };

#define QK_MENU_STICK 16000
#define QK_TRIGGER 100
#define QK_QUEUE 64

typedef struct {
    uint16_t ev[QK_QUEUE];       /* down << 8 | key */
    unsigned head, tail;
    uint32_t held;               /* virtual buttons down */
    uint8_t sent[QKV_COUNT];     /* the key each held button was pressed as */
    float axes[4];
    int menu;                    /* last mode seen */
} qk_pad;

static inline void qk_push(qk_pad *p, int down, int key) {
    unsigned next = (p->tail + 1u) % QK_QUEUE;
    if (next == p->head || !key) return;
    p->ev[p->tail] = (uint16_t)((down ? 1u : 0u) << 8 | (unsigned)key);
    p->tail = next;
}

static inline uint32_t qk_virtual(const xpad_state *s, int menu) {
    uint32_t v = 0;
    int stick = menu;   /* the left stick moves the menu cursor; in the game it is analog */
    if ((s->buttons & XPAD_UP) || (stick && s->ly > QK_MENU_STICK)) v |= 1u << QKV_UP;
    if ((s->buttons & XPAD_DOWN) || (stick && s->ly < -QK_MENU_STICK)) v |= 1u << QKV_DOWN;
    if ((s->buttons & XPAD_LEFT) || (stick && s->lx < -QK_MENU_STICK)) v |= 1u << QKV_LEFT;
    if ((s->buttons & XPAD_RIGHT) || (stick && s->lx > QK_MENU_STICK)) v |= 1u << QKV_RIGHT;
    if (s->buttons & XPAD_A) v |= 1u << QKV_A;
    if (s->buttons & XPAD_B) v |= 1u << QKV_B;
    if (s->rt > QK_TRIGGER) v |= 1u << QKV_FIRE;
    if (s->lt > QK_TRIGGER) v |= 1u << QKV_RUN;
    if (s->buttons & XPAD_LB) v |= 1u << QKV_PREV;
    if (s->buttons & XPAD_RB) v |= 1u << QKV_NEXT;
    if (s->buttons & (XPAD_Y | XPAD_BACK)) v |= 1u << QKV_SCORES;
    if (s->buttons & XPAD_LS) v |= 1u << QKV_CENTER;
    if (s->buttons & XPAD_START) v |= 1u << QKV_MENU;
    return v;
}

static inline int qk_key_for(unsigned v, int menu) {
    static const int game[QKV_COUNT] = {QK_UP, QK_DOWN, QK_LEFT, QK_RIGHT, QK_SPACE, QK_AUX3, QK_CTRL, QK_SHIFT,
                                        QK_AUX1, QK_AUX2, QK_TAB, QK_AUX5, QK_ESCAPE};
    static const int menus[QKV_COUNT] = {QK_UP, QK_DOWN, QK_LEFT, QK_RIGHT, QK_ENTER, QK_ESCAPE, 0, 0, 0, 0, 0, 0,
                                         QK_ESCAPE};
    return menu ? menus[v] : game[v];
}

/* New pad state. menu: the game is showing a menu or the console (not playing). */
static inline void qk_update(qk_pad *p, const xpad_state *s, int menu) {
    uint32_t now = qk_virtual(s, menu), diff = now ^ p->held;
    unsigned v;
    for (v = 0; v < QKV_COUNT; ++v) {
        if (!(diff & (1u << v))) continue;
        if (now & (1u << v)) {
            p->sent[v] = (uint8_t)qk_key_for(v, menu);
            qk_push(p, 1, p->sent[v]);
        } else {
            qk_push(p, 0, p->sent[v]);
            p->sent[v] = 0;
        }
    }
    p->held = now;
    p->menu = menu;
    if (menu) {
        p->axes[0] = p->axes[1] = p->axes[2] = p->axes[3] = 0.0f;
    } else {
        p->axes[0] = (float)s->lx / 32768.0f;
        p->axes[1] = -(float)s->ly / 32768.0f;
        p->axes[2] = (float)s->rx / 32768.0f;
        p->axes[3] = -(float)s->ry / 32768.0f;
    }
}

static inline int qk_pop(qk_pad *p, int *down, int *key) {
    if (p->head == p->tail) return 0;
    *down = p->ev[p->head] >> 8;
    *key = p->ev[p->head] & 0xFF;
    p->head = (p->head + 1u) % QK_QUEUE;
    return 1;
}

static inline void qk_axes(const qk_pad *p, float *axes) {
    unsigned i;
    for (i = 0; i < 6; ++i) axes[i] = i < 4 ? p->axes[i] : 0.0f;
}

#endif
