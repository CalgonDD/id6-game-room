/* xpad.h: Xbox 360 and Xbox One report decoding, and the "p ..." datagram round trip. */
#include <assert.h>
#include "../src/xpad.h"

int main(void) {
    xpad_state s;
    char text[96];
    /* Xbox 360: A + d-pad up + LB, LT 200, left stick full right / half up. */
    static const uint8_t r360[20] = {0x00, 0x14, 0x01, 0x11, 200, 0, 0xFF, 0x7F, 0x00, 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    /* Xbox One: B + view, d-pad left + RB, RT full, right stick full down. */
    static const uint8_t rOne[18] = {0x20, 0x00, 0x05, 0x0E, 0x28, 0x24, 0, 0, 0xFF, 0x03, 0, 0, 0, 0, 0, 0, 0x00, 0x80};
    static const uint8_t guideOn[6] = {0x07, 0x20, 0x01, 0x02, 0x01, 0x5B};
    static const uint8_t keepalive[18] = {0x03, 0x20};

    assert(xpad_kind(0xFF, 0x5D, 0x01) == XPAD_360 && xpad_kind(0xFF, 0x47, 0xD0) == XPAD_ONE &&
           xpad_kind(0x03, 0x00, 0x00) == XPAD_NONE);

    memset(&s, 0, sizeof s);
    assert(xpad_decode(XPAD_360, r360, sizeof r360, &s) == 1);
    assert(s.buttons == (XPAD_A | XPAD_UP | XPAD_LB) && s.lt == 200 && s.lx == 32767 && s.ly == 16384);
    assert(xpad_decode(XPAD_360, r360, 10, &s) == 0);            /* short: ignored */

    memset(&s, 0, sizeof s);
    assert(xpad_decode(XPAD_ONE, guideOn, sizeof guideOn, &s) == 1 && s.buttons == XPAD_GUIDE);
    assert(xpad_decode(XPAD_ONE, rOne, sizeof rOne, &s) == 1);
    assert(s.buttons == (XPAD_GUIDE | XPAD_B | XPAD_BACK | XPAD_LEFT | XPAD_RB) && s.rt == 255 && s.ry == -32768);
    assert(xpad_decode(XPAD_ONE, keepalive, sizeof keepalive, &s) == 0);

    xpad_format(&s, text, sizeof text);
    {
        xpad_state back;
        memset(&back, 0, sizeof back);
        assert(xpad_parse(text, &back) && !memcmp(&back, &s, sizeof s));
        assert(!xpad_parse("v 51", &back));
    }
    printf("xpad: Xbox 360 and Xbox One reports decode to the same buttons/sticks/triggers; datagram round-trips.\n");
    return 0;
}
