#ifndef PS1_CONTROLS_H
#define PS1_CONTROLS_H
#include "nes_input.h"
enum { PS1_ORIGINAL, PS1_SHOOTER, PS1_ADVENTURE, PS1_CUSTOM };   /* PS1_CUSTOM: pad_remap.h, handled by the caller */
static const char *ps1_control_name(unsigned profile) {
    return profile == PS1_SHOOTER ? "Modern shooter" : profile == PS1_ADVENTURE ? "Modern adventure" :
           profile == PS1_CUSTOM ? "Your own buttons" : "Original PS1";
}
static int ps1_tomb_title(const char *title) {
    const char *p;
    for (p = title; *p; ++p)
        if ((p[0]=='t'||p[0]=='T') && (p[1]=='o'||p[1]=='O') &&
            (p[2]=='m'||p[2]=='M') && (p[3]=='b'||p[3]=='B')) return 1;
    return 0;
}
static unsigned ps1_default_controls(const char *title) { return ps1_tomb_title(title) ? PS1_ADVENTURE : PS1_SHOOTER; }
#define PS1_LOOK_DEADZONE 3500
static int ps1_right_look(const xpad_state *s) { return s->rx > PS1_LOOK_DEADZONE || s->rx < -PS1_LOOK_DEADZONE || s->ry > PS1_LOOK_DEADZONE || s->ry < -PS1_LOOK_DEADZONE; }
static uint32_t ps1_control_buttons(const xpad_state *s, unsigned profile) {
    xpad_state mapped = *s;
    unsigned face = XPAD_A|XPAD_B|XPAD_X|XPAD_Y;
    if (profile == PS1_ORIGINAL) return ns_retropad_ps1(s);
    mapped.buttons &= ~(face|XPAD_LB|XPAD_RB|XPAD_LS|XPAD_RS);
    mapped.lt = mapped.rt = 0;
    if (profile == PS1_SHOOTER) {
        if (s->buttons & XPAD_A) mapped.buttons |= XPAD_Y;  /* jump / triangle */
        if (s->buttons & XPAD_X) mapped.buttons |= XPAD_X;  /* reload/action / square */
        if (s->buttons & XPAD_Y) mapped.buttons |= XPAD_B;  /* weapon / circle */
        if (s->rt > 100) mapped.buttons |= XPAD_A;          /* fire / cross */
        if (s->lt > 100) mapped.rt = 255;                  /* aim / R2 */
        if (s->buttons & XPAD_B) mapped.lt = 255;          /* crouch / L2 */
        mapped.buttons |= s->buttons & (XPAD_LB|XPAD_RB|XPAD_LS|XPAD_RS);
    } else {
        if (s->buttons & XPAD_A) mapped.buttons |= XPAD_X;  /* jump / square */
        if ((s->buttons & XPAD_X) || s->rt > 100) mapped.buttons |= XPAD_A;
        if (s->buttons & XPAD_Y) mapped.buttons |= XPAD_Y;  /* draw / triangle */
        if (s->buttons & XPAD_B) mapped.buttons |= XPAD_B;  /* roll / circle */
        if (s->lt > 100 || ps1_right_look(s)) mapped.buttons |= XPAD_LB;
        if (s->buttons & XPAD_LB) mapped.buttons |= XPAD_RB; /* walk / R1 */
        if (s->buttons & XPAD_RB) mapped.lt = 255;         /* sprint / L2 */
        if (s->buttons & XPAD_LS) mapped.rt = 255;         /* crouch / R2 */
        if (ps1_right_look(s)) {
            mapped.buttons &= ~(XPAD_UP|XPAD_DOWN|XPAD_LEFT|XPAD_RIGHT);
            if (s->rx < -PS1_LOOK_DEADZONE) mapped.buttons |= XPAD_LEFT;
            if (s->rx > PS1_LOOK_DEADZONE) mapped.buttons |= XPAD_RIGHT;
            if (s->ry > PS1_LOOK_DEADZONE) mapped.buttons |= XPAD_UP;
            if (s->ry < -PS1_LOOK_DEADZONE) mapped.buttons |= XPAD_DOWN;
        }
    }
    return ns_retropad_ps1(&mapped);
}
static int16_t ps1_control_axis(const xpad_state *s, unsigned profile, unsigned index, unsigned id) {
    xpad_state mapped = *s;
    if (index > 1 || id > 1) return 0;
    if (profile == PS1_SHOOTER) { mapped.lx=s->rx; mapped.rx=s->lx; }
    else if (profile == PS1_ADVENTURE && ps1_right_look(s)) {
        mapped.lx=s->rx; mapped.ly=s->ry; mapped.rx=mapped.ry=0;
    }
    return ns_ps1_axis(&mapped,index,id);
}
#endif
