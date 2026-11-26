/* freedoom_mixer.h: Doom sound effects -> 16-bit stereo PCM (host-tested in tests/test_freedoom_mixer.c).
 *
 * Doom keeps every sound effect in the WAD as a "DS..." lump in DMX format:
 *   u16 format (3), u16 sample rate, u32 sample count, then 8-bit unsigned samples; the DMX library skipped the
 *   first and last 16 samples (padding), and so do we (as Chocolate Doom does).
 * The engine (s_sound.c) asks for a sound on a channel with a volume 0..127 and a separation 0..254
 * (0 = hard left, 128 = centre, 254 = hard right). Each voice steps through its samples at rate/out_rate
 * (16.16 fixed point, nearest sample: the original's sound), and all voices are summed, scaled by FDM_SFX_GAIN
 * and clipped.
 * FDM_SFX_GAIN (2026-11-19, car test): at full gain one effect reached full scale, far above the music (peaks
 * about 12k), and effects over music clipped (crackle). A quarter puts one loud effect at about 8k. */
#ifndef FREEDOOM_MIXER_H
#define FREEDOOM_MIXER_H

#include <stdint.h>
#include <string.h>

#define FDM_VOICES 16
#ifndef FDM_SFX_GAIN
#define FDM_SFX_GAIN 64          /* of 256 */
#endif

typedef struct {
    const uint8_t *data;
    uint32_t len;            /* samples */
    uint32_t pos;            /* 16.16 */
    uint32_t step;           /* 16.16 */
    int left, right;         /* gains 0..254 */
    int active;
} fdm_voice;

typedef struct {
    fdm_voice v[FDM_VOICES];
    unsigned out_rate;
} fdm_mixer;

/* Finds the playable samples inside a DMX lump. Returns 1 and fills data/len/rate, or 0 if it is not one. */
static inline int fdm_parse_lump(const uint8_t *lump, uint32_t lumplen, const uint8_t **data, uint32_t *len,
                                 uint32_t *rate) {
    uint32_t count;
    if (!lump || lumplen < 8 || lump[0] != 3 || lump[1] != 0) return 0;
    *rate = (uint32_t)(lump[2] | (lump[3] << 8));
    count = (uint32_t)lump[4] | ((uint32_t)lump[5] << 8) | ((uint32_t)lump[6] << 16) | ((uint32_t)lump[7] << 24);
    if (count > lumplen - 8) count = lumplen - 8;
    if (!*rate || count == 0) return 0;
    if (count > 32) {                       /* the DMX padding */
        *data = lump + 8 + 16;
        *len = count - 32;
    } else {
        *data = lump + 8;
        *len = count;
    }
    return 1;
}

static inline void fdm_gains(int vol, int sep, int *left, int *right) {
    if (vol < 0) vol = 0;
    if (vol > 127) vol = 127;
    if (sep < 0) sep = 0;
    if (sep > 254) sep = 254;
    *left = ((254 - sep) * vol) / 127;
    *right = (sep * vol) / 127;
}

static inline void fdm_start(fdm_mixer *m, int ch, const uint8_t *data, uint32_t len, uint32_t rate, int vol,
                             int sep) {
    fdm_voice *v;
    if (ch < 0 || ch >= FDM_VOICES || !m->out_rate) return;
    v = &m->v[ch];
    v->data = data;
    v->len = len;
    v->pos = 0;
    v->step = (uint32_t)(((uint64_t)rate << 16) / m->out_rate);
    fdm_gains(vol, sep, &v->left, &v->right);
    v->active = len > 0;
}

static inline void fdm_params(fdm_mixer *m, int ch, int vol, int sep) {
    if (ch < 0 || ch >= FDM_VOICES) return;
    fdm_gains(vol, sep, &m->v[ch].left, &m->v[ch].right);
}

static inline void fdm_stop(fdm_mixer *m, int ch) {
    if (ch >= 0 && ch < FDM_VOICES) m->v[ch].active = 0;
}

static inline int fdm_playing(const fdm_mixer *m, int ch) {
    return ch >= 0 && ch < FDM_VOICES && m->v[ch].active;
}

/* Mixes `frames` stereo frames (interleaved L R) into out. */
static inline void fdm_mix(fdm_mixer *m, int16_t *out, unsigned frames) {
    unsigned f, k;
    for (f = 0; f < frames; ++f) {
        int32_t l = 0, r = 0;
        for (k = 0; k < FDM_VOICES; ++k) {
            fdm_voice *v = &m->v[k];
            uint32_t i;
            int s;
            if (!v->active) continue;
            i = v->pos >> 16;
            if (i >= v->len) {
                v->active = 0;
                continue;
            }
            s = (int)v->data[i] - 128;          /* -128..127 */
            l += s * v->left;                    /* at most about +-32k per voice */
            r += s * v->right;
            v->pos += v->step;
        }
        l = (l * FDM_SFX_GAIN) / 256;
        r = (r * FDM_SFX_GAIN) / 256;
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        out[2 * f] = (int16_t)l;
        out[2 * f + 1] = (int16_t)r;
    }
}

#endif
