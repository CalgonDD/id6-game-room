/* gip.h: Xbox One / Series "GIP" packet framing, host side, without authentication (Freedoom pad probe).
 *
 * Header: [0] command, [1] flags, [2] sequence, then the payload length as LEB128 (1-4 bytes).
 *   flags: 0x0F client, 0x10 needs-ack, 0x20 system, 0x40 chunk start, 0x80 chunked.
 *   A chunked packet carries one more LEB128 after the length: the total size on the first chunk,
 *   the byte offset on the following ones.
 * Commands seen or used: 0x01 ack, 0x02 arrival ("hello"), 0x03 status, 0x04 descriptor (request = empty),
 *   0x05 power/configuration, 0x06 authentication, 0x07 guide button, 0x20 input.
 * Every packet with needs-ack set must be answered with an ack carrying the SAME sequence number:
 *   01 20 <seq> 09 | 00 <cmd> <flags> <bytes received u16 LE> 00 00 <remaining u16 LE>
 * Sources: TheNathannator's GIP notes (superseded by Microsoft's MS-GIPUSB), the Linux xpad and xone drivers.
 * Pure C, host-tested in tests/test_gip.c. */
#ifndef GIP_H
#define GIP_H

#include <stdint.h>
#include <string.h>

enum {
    GIP_ACK = 0x01, GIP_ARRIVAL = 0x02, GIP_STATUS = 0x03, GIP_DESCRIPTOR = 0x04, GIP_POWER = 0x05,
    GIP_AUTH = 0x06, GIP_GUIDE = 0x07, GIP_INPUT = 0x20
};
enum { GIP_F_NEEDS_ACK = 0x10, GIP_F_SYSTEM = 0x20, GIP_F_CHUNK_START = 0x40, GIP_F_CHUNKED = 0x80 };

typedef struct {
    uint8_t cmd, flags, seq;
    uint32_t len;        /* payload length */
    uint32_t chunk;      /* chunked only: total size (first chunk) or offset (later chunks) */
    unsigned header;     /* bytes before the payload */
} gip_header;

/* LEB128, at most 4 bytes. Returns bytes used, 0 if malformed or cut short. */
static inline unsigned gip_leb(const uint8_t *p, unsigned avail, uint32_t *out) {
    uint32_t v = 0;
    unsigned i;
    for (i = 0; i < 4 && i < avail; ++i) {
        v |= (uint32_t)(p[i] & 0x7Fu) << (7u * i);
        if (!(p[i] & 0x80u)) { *out = v; return i + 1; }
    }
    return 0;
}

/* Parses one packet header. Returns 1 when the header (not necessarily the whole payload) is valid. */
static inline int gip_parse(const uint8_t *p, unsigned n, gip_header *h) {
    unsigned used, more;
    memset(h, 0, sizeof *h);
    if (n < 4) return 0;
    h->cmd = p[0];
    h->flags = p[1];
    h->seq = p[2];
    used = gip_leb(p + 3, n - 3, &h->len);
    if (!used) return 0;
    h->header = 3 + used;
    if (h->flags & GIP_F_CHUNKED) {
        more = gip_leb(p + h->header, n - h->header, &h->chunk);
        if (!more) return 0;
        h->header += more;
    }
    return 1;
}

/* Builds the ack for packet h. received = bytes of that message received so far (cumulative for chunks),
 * remaining = what is still to come (0 when complete). Returns the packet length (13). */
static inline unsigned gip_ack(const gip_header *h, uint32_t received, uint32_t remaining, uint8_t out[13]) {
    out[0] = GIP_ACK;
    out[1] = GIP_F_SYSTEM;
    out[2] = h->seq;
    out[3] = 0x09;
    out[4] = 0x00;
    out[5] = h->cmd;
    out[6] = (uint8_t)(h->flags & ~(GIP_F_NEEDS_ACK | GIP_F_CHUNK_START | GIP_F_CHUNKED)) | GIP_F_SYSTEM;
    out[7] = (uint8_t)(received & 0xFFu);
    out[8] = (uint8_t)((received >> 8) & 0xFFu);
    out[9] = 0x00;
    out[10] = 0x00;
    out[11] = (uint8_t)(remaining & 0xFFu);
    out[12] = (uint8_t)((remaining >> 8) & 0xFFu);
    return 13;
}

/* Bookkeeping for one chunked message (the descriptor). */
typedef struct {
    uint8_t cmd;
    uint32_t total, received;
    int active, complete;
} gip_chunks;

/* Feeds one chunked packet; returns bytes received so far for the ack. */
static inline uint32_t gip_chunk_feed(gip_chunks *c, const gip_header *h) {
    if (h->flags & GIP_F_CHUNK_START) {
        c->cmd = h->cmd;
        c->total = h->chunk;
        c->received = h->len;
        c->active = 1;
        c->complete = 0;
    } else if (c->active && c->cmd == h->cmd) {
        if (h->len == 0) {                    /* the closing empty chunk */
            c->complete = 1;
            c->active = 0;
        } else if (h->chunk + h->len > c->received) {
            c->received = h->chunk + h->len;
        }
    }
    if (c->active && c->total && c->received >= c->total) c->complete = 1;
    return c->received;
}

#endif
