/* nes_input.h: the wired Xbox pad as an NES controller (libretro RetroPad), host-tested in tests/test_nes.c.
 *
 *   d-pad or left stick = NES d-pad      A or B = NES A      X or Y = NES B
 *   Menu = Start      View = Select      Xbox button, or View + Menu together = the emulator's own menu
 * While View and Menu are both held they are not passed to the game (so opening the menu doesn't press
 * Start/Select). Menu navigation uses the same d-pad/stick, A to choose and B to go back. */
#ifndef NES_INPUT_H
#define NES_INPUT_H

#include <stdint.h>
#include "xpad.h"

#define NS_STICK 16000

/* RetroPad ids (libretro.h) */
enum {
    NS_RP_B = 0, NS_RP_Y = 1, NS_RP_SELECT = 2, NS_RP_START = 3, NS_RP_UP = 4, NS_RP_DOWN = 5, NS_RP_LEFT = 6,
    NS_RP_RIGHT = 7, NS_RP_A = 8, NS_RP_X = 9
};

/* Menu-navigation bits (edges are taken by the caller) */
enum { NS_UP = 1, NS_DOWN = 2, NS_LEFT = 4, NS_RIGHT = 8, NS_OK = 16, NS_BACK = 32, NS_MENU = 64 };

/* The RetroPad buttons held, as a bit mask of NS_RP_* ids. */
static inline uint32_t ns_retropad(const xpad_state *s) {
    uint32_t b = 0, both = (s->buttons & XPAD_START) && (s->buttons & XPAD_BACK);
    if ((s->buttons & XPAD_UP) || s->ly > NS_STICK) b |= 1u << NS_RP_UP;
    if ((s->buttons & XPAD_DOWN) || s->ly < -NS_STICK) b |= 1u << NS_RP_DOWN;
    if ((s->buttons & XPAD_LEFT) || s->lx < -NS_STICK) b |= 1u << NS_RP_LEFT;
    if ((s->buttons & XPAD_RIGHT) || s->lx > NS_STICK) b |= 1u << NS_RP_RIGHT;
    if (s->buttons & (XPAD_A | XPAD_B)) b |= 1u << NS_RP_A;
    if (s->buttons & (XPAD_X | XPAD_Y)) b |= 1u << NS_RP_B;
    if ((s->buttons & XPAD_START) && !both) b |= 1u << NS_RP_START;
    if ((s->buttons & XPAD_BACK) && !both) b |= 1u << NS_RP_SELECT;
    return b;
}

/* The PS1 DualShock (RETRO_CORE_PS1):
 *   A cross   B circle   X square   Y triangle   LB/RB L1/R1   LT/RT L2/R2   stick clicks L3/R3
 *   d-pad = d-pad (the sticks are the DualShock's analog sticks)   View = Select   Menu = Start */
static inline uint32_t ns_retropad_ps1(const xpad_state *s) {
    uint32_t b = 0, both = (s->buttons & XPAD_START) && (s->buttons & XPAD_BACK);
    if (s->buttons & XPAD_UP) b |= 1u << NS_RP_UP;
    if (s->buttons & XPAD_DOWN) b |= 1u << NS_RP_DOWN;
    if (s->buttons & XPAD_LEFT) b |= 1u << NS_RP_LEFT;
    if (s->buttons & XPAD_RIGHT) b |= 1u << NS_RP_RIGHT;
    if (s->buttons & XPAD_A) b |= 1u << NS_RP_B;        /* cross */
    if (s->buttons & XPAD_B) b |= 1u << NS_RP_A;        /* circle */
    if (s->buttons & XPAD_X) b |= 1u << NS_RP_Y;        /* square */
    if (s->buttons & XPAD_Y) b |= 1u << NS_RP_X;        /* triangle */
    if (s->buttons & XPAD_LB) b |= 1u << 10;
    if (s->buttons & XPAD_RB) b |= 1u << 11;
    if (s->lt > 100) b |= 1u << 12;
    if (s->rt > 100) b |= 1u << 13;
    if (s->buttons & XPAD_LS) b |= 1u << 14;
    if (s->buttons & XPAD_RS) b |= 1u << 15;
    if ((s->buttons & XPAD_START) && !both) b |= 1u << NS_RP_START;
    if ((s->buttons & XPAD_BACK) && !both) b |= 1u << NS_RP_SELECT;
    return b;
}
/* An analog stick for the core: index 0 left, 1 right; id 0 x, 1 y (libretro: y down is positive). */
static inline int16_t ns_ps1_axis(const xpad_state *s, unsigned index, unsigned id) {
    int v = index == 0 ? (id == 0 ? s->lx : -s->ly) : (id == 0 ? s->rx : -s->ry);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

/* The menu-navigation bits held. */
static inline uint32_t ns_nav(const xpad_state *s) {
    uint32_t n = 0;
    if ((s->buttons & XPAD_UP) || s->ly > NS_STICK) n |= NS_UP;
    if ((s->buttons & XPAD_DOWN) || s->ly < -NS_STICK) n |= NS_DOWN;
    if ((s->buttons & XPAD_LEFT) || s->lx < -NS_STICK) n |= NS_LEFT;
    if ((s->buttons & XPAD_RIGHT) || s->lx > NS_STICK) n |= NS_RIGHT;
    if (s->buttons & XPAD_A) n |= NS_OK;
    if (s->buttons & XPAD_B) n |= NS_BACK;
    if ((s->buttons & XPAD_GUIDE) || ((s->buttons & XPAD_START) && (s->buttons & XPAD_BACK))) n |= NS_MENU;
    return n;
}

#endif
