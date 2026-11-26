/* xpad.h: Xbox controller report decoding (wired USB), shared by the game programs and the host tests.
 *
 * Two wire formats, both vendor class 0xFF (no HID driver claims them):
 *   Xbox 360 (XInput), interface FF/5D/01: 20-byte reports, report[0] = 0x00, report[1] = 0x14.
 *   Xbox One / Series (GIP), interface FF/47/D0: report[0] = 0x20 input, 0x07 guide button, 0x03 keep-alive.
 *     The pad stays silent until the host sends the power-on packet xpad_gip_start.
 * Layouts as the Linux xpad driver documents them. Nothing here touches USB. */
#ifndef XPAD_H
#define XPAD_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    XPAD_UP = 1u << 0, XPAD_DOWN = 1u << 1, XPAD_LEFT = 1u << 2, XPAD_RIGHT = 1u << 3,
    XPAD_START = 1u << 4, XPAD_BACK = 1u << 5, XPAD_LS = 1u << 6, XPAD_RS = 1u << 7,
    XPAD_LB = 1u << 8, XPAD_RB = 1u << 9, XPAD_GUIDE = 1u << 10,
    XPAD_A = 1u << 11, XPAD_B = 1u << 12, XPAD_X = 1u << 13, XPAD_Y = 1u << 14
};
#define XPAD_BUTTON_COUNT 15

typedef struct {
    uint16_t buttons;
    int16_t lx, ly, rx, ry;   /* -32768..32767, up = positive */
    uint8_t lt, rt;           /* 0..255 */
} xpad_state;

enum { XPAD_NONE = 0, XPAD_360 = 1, XPAD_ONE = 2 };

/* Which protocol an interface speaks, from its class/subclass/protocol. */
static inline int xpad_kind(unsigned cls, unsigned sub, unsigned proto) {
    if (cls == 0xFFu && sub == 0x5Du && proto == 0x01u) return XPAD_360;
    if (cls == 0xFFu && sub == 0x47u && proto == 0xD0u) return XPAD_ONE;
    return XPAD_NONE;
}

static const uint8_t xpad_gip_start[5] = {0x05, 0x20, 0x00, 0x01, 0x00};

static inline int16_t xpad_le16(const uint8_t *p) { return (int16_t)(uint16_t)(p[0] | (p[1] << 8)); }

/* Updates *s from one report. Returns 1 when it was an input report (state may have changed), else 0. */
static inline int xpad_decode(int kind, const uint8_t *r, unsigned len, xpad_state *s) {
    if (kind == XPAD_360) {
        uint16_t b = 0;
        if (len < 14 || r[0] != 0x00 || r[1] < 0x14) return 0;
        if (r[2] & 0x01) b |= XPAD_UP;
        if (r[2] & 0x02) b |= XPAD_DOWN;
        if (r[2] & 0x04) b |= XPAD_LEFT;
        if (r[2] & 0x08) b |= XPAD_RIGHT;
        if (r[2] & 0x10) b |= XPAD_START;
        if (r[2] & 0x20) b |= XPAD_BACK;
        if (r[2] & 0x40) b |= XPAD_LS;
        if (r[2] & 0x80) b |= XPAD_RS;
        if (r[3] & 0x01) b |= XPAD_LB;
        if (r[3] & 0x02) b |= XPAD_RB;
        if (r[3] & 0x04) b |= XPAD_GUIDE;
        if (r[3] & 0x10) b |= XPAD_A;
        if (r[3] & 0x20) b |= XPAD_B;
        if (r[3] & 0x40) b |= XPAD_X;
        if (r[3] & 0x80) b |= XPAD_Y;
        s->buttons = b;
        s->lt = r[4];
        s->rt = r[5];
        s->lx = xpad_le16(r + 6);
        s->ly = xpad_le16(r + 8);
        s->rx = xpad_le16(r + 10);
        s->ry = xpad_le16(r + 12);
        return 1;
    }
    if (kind == XPAD_ONE) {
        if (len >= 5 && r[0] == 0x07) {          /* guide button: its own report */
            if (r[4] & 0x01) s->buttons |= XPAD_GUIDE; else s->buttons &= (uint16_t)~XPAD_GUIDE;
            return 1;
        }
        if (len >= 18 && r[0] == 0x20) {
            uint16_t b = (uint16_t)(s->buttons & XPAD_GUIDE);
            unsigned lt = (unsigned)(r[6] | (r[7] << 8)), rt = (unsigned)(r[8] | (r[9] << 8));   /* 0..1023 */
            if (r[4] & 0x04) b |= XPAD_START;   /* menu */
            if (r[4] & 0x08) b |= XPAD_BACK;    /* view */
            if (r[4] & 0x10) b |= XPAD_A;
            if (r[4] & 0x20) b |= XPAD_B;
            if (r[4] & 0x40) b |= XPAD_X;
            if (r[4] & 0x80) b |= XPAD_Y;
            if (r[5] & 0x01) b |= XPAD_UP;
            if (r[5] & 0x02) b |= XPAD_DOWN;
            if (r[5] & 0x04) b |= XPAD_LEFT;
            if (r[5] & 0x08) b |= XPAD_RIGHT;
            if (r[5] & 0x10) b |= XPAD_LB;
            if (r[5] & 0x20) b |= XPAD_RB;
            if (r[5] & 0x40) b |= XPAD_LS;
            if (r[5] & 0x80) b |= XPAD_RS;
            s->buttons = b;
            s->lt = (uint8_t)(lt > 1023u ? 255u : lt >> 2);
            s->rt = (uint8_t)(rt > 1023u ? 255u : rt >> 2);
            s->lx = xpad_le16(r + 10);
            s->ly = xpad_le16(r + 12);
            s->rx = xpad_le16(r + 14);
            s->ry = xpad_le16(r + 16);
            return 1;
        }
        return 0;
    }
    return 0;
}

/* "p <buttons> <lx> <ly> <rx> <ry> <lt> <rt>": the datagram the pad worker sends to the display test. */
static inline int xpad_format(const xpad_state *s, char *out, unsigned size) {
    return snprintf(out, size, "p %u %d %d %d %d %u %u", (unsigned)s->buttons, s->lx, s->ly, s->rx, s->ry,
                    (unsigned)s->lt, (unsigned)s->rt);
}
static inline int xpad_parse(const char *in, xpad_state *s) {
    unsigned b, lt, rt;
    int lx, ly, rx, ry;
    if (sscanf(in, "p %u %d %d %d %d %u %u", &b, &lx, &ly, &rx, &ry, &lt, &rt) != 7) return 0;
    s->buttons = (uint16_t)b;
    s->lx = (int16_t)lx; s->ly = (int16_t)ly; s->rx = (int16_t)rx; s->ry = (int16_t)ry;
    s->lt = (uint8_t)lt; s->rt = (uint8_t)rt;
    return 1;
}

#endif
