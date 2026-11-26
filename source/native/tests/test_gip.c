/* gip.h: GIP header parsing (with the Turtle Beach packets the car logged), acks, chunk bookkeeping. */
#include <assert.h>
#include <stdio.h>
#include "../src/gip.h"

int main(void) {
    gip_header h;
    uint8_t ack[13];
    gip_chunks c;
    /* Logged on the car 2026-11-19 from 10f5:7113. */
    static const uint8_t status[8] = {0x03, 0x20, 0x02, 0x04, 0x80, 0x00, 0x00, 0x00};
    static const uint8_t input[36] = {0x20, 0x00, 0x02, 0x20};
    /* Synthetic: a guide press wanting an ack, a two-byte length, and a chunked descriptor. */
    static const uint8_t guide[6] = {0x07, 0x30, 0x05, 0x02, 0x01, 0x5b};
    static const uint8_t longlen[5] = {0x04, 0x20, 0x09, 0xBA, 0x01};          /* len 0xBA = 186 */
    static const uint8_t chunk1[8] = {0x04, 0xF0, 0x01, 0x3A, 0xBA, 0x01, 0, 0}; /* start, total 186, 58 bytes */
    static const uint8_t chunk2[6] = {0x04, 0xA0, 0x01, 0x3A, 0x3A, 0};         /* offset 58 */
    static const uint8_t chunk3[6] = {0x04, 0xB0, 0x01, 0x46, 0x74, 0};         /* offset 116, 70 bytes, ack */

    assert(gip_parse(status, sizeof status, &h) && h.cmd == GIP_STATUS && h.seq == 2 && h.len == 4 && h.header == 4);
    assert(gip_parse(input, sizeof input, &h) && h.cmd == GIP_INPUT && h.len == 32 && !(h.flags & GIP_F_NEEDS_ACK));
    assert(gip_parse(guide, sizeof guide, &h) && (h.flags & GIP_F_NEEDS_ACK));
    gip_ack(&h, h.len, 0, ack);
    assert(ack[0] == 0x01 && ack[1] == 0x20 && ack[2] == 0x05 && ack[3] == 0x09 && ack[5] == 0x07 &&
           ack[6] == 0x20 && ack[7] == 2 && ack[8] == 0 && ack[11] == 0);
    assert(gip_parse(longlen, sizeof longlen, &h) && h.len == 186 && h.header == 5);
    assert(!gip_parse(longlen, 4, &h));                                         /* cut inside the length */

    memset(&c, 0, sizeof c);
    assert(gip_parse(chunk1, sizeof chunk1, &h) && h.chunk == 186 && h.len == 58 && h.header == 6);
    assert(gip_chunk_feed(&c, &h) == 58 && c.active && !c.complete);
    assert(gip_parse(chunk2, sizeof chunk2, &h) && h.chunk == 58);
    assert(gip_chunk_feed(&c, &h) == 116 && !c.complete);
    assert(gip_parse(chunk3, sizeof chunk3, &h) && h.chunk == 116 && h.len == 70);
    assert(gip_chunk_feed(&c, &h) == 186 && c.complete);
    gip_ack(&h, c.received, c.total - c.received, ack);
    assert(ack[2] == 1 && ack[5] == 0x04 && ack[7] == 186 && ack[11] == 0);
    printf("gip: car-logged status/input parse, needs-ack guide acked with its own sequence, LEB128 lengths, "
           "chunked descriptor tracked to completion.\n");
    return 0;
}
