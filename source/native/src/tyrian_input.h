/* tyrian_input.h: the wired Xbox pad as the SDL joystick OpenTyrian reads (host-tested in tests/test_tyrian_input.c).
 *
 * OpenTyrian's default joystick layout (joystick.c, reset_joystick_assignments): axes 0/1 and hat 0 move; buttons
 * 0..5 are fire, change rear weapon, left sidekick, right sidekick, menu, pause. In menus, buttons 0 or 4
 * confirm and 1 or 5 go back; directions move the cursor. So:
 *   left stick / d-pad = fly (and menus)       A or RT = fire (Enter in menus)
 *   B = change rear weapon (back in menus)       LB or X = left sidekick    RB or Y = right sidekick
 *   Menu = in-game menu                          View = pause
 * SDL's Y axis grows downwards; the pad's grows upwards. */
#ifndef TYRIAN_INPUT_H
#define TYRIAN_INPUT_H

#include <stdint.h>
#include "xpad.h"

#define TY_TRIGGER 100   /* of 255 */

typedef struct {
    int16_t axes[4];     /* left x, left y (down +), right x, right y (down +) */
    uint32_t buttons;    /* bit n = SDL joystick button n */
    uint8_t hat;         /* SDL_HAT_* bits: up 1, right 2, down 4, left 8 */
} ty_joy;

static inline int16_t ty_flip(int16_t v) { return v == -32768 ? 32767 : (int16_t)-v; }

static inline void ty_map(const xpad_state *s, ty_joy *j) {
    j->axes[0] = s->lx;
    j->axes[1] = ty_flip(s->ly);
    j->axes[2] = s->rx;
    j->axes[3] = ty_flip(s->ry);
    j->hat = (uint8_t)(((s->buttons & XPAD_UP) ? 1u : 0u) | ((s->buttons & XPAD_RIGHT) ? 2u : 0u) |
                       ((s->buttons & XPAD_DOWN) ? 4u : 0u) | ((s->buttons & XPAD_LEFT) ? 8u : 0u));
    j->buttons = 0;
    if ((s->buttons & XPAD_A) || s->rt > TY_TRIGGER) j->buttons |= 1u << 0;
    if (s->buttons & XPAD_B) j->buttons |= 1u << 1;
    if (s->buttons & (XPAD_LB | XPAD_X)) j->buttons |= 1u << 2;
    if (s->buttons & (XPAD_RB | XPAD_Y)) j->buttons |= 1u << 3;
    if (s->buttons & XPAD_START) j->buttons |= 1u << 4;
    if (s->buttons & XPAD_BACK) j->buttons |= 1u << 5;
}

#endif
