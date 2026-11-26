/* freedoom_mixer.h: DMX lump parsing (padding skipped), resampling 11025 -> 44100, panning, the effects gain,
 * mixing, clipping. */
#include <assert.h>
#include <stdio.h>
#include "../src/freedoom_mixer.h"

int main(void) {
    static uint8_t lump[8 + 16 + 100 + 16];
    const uint8_t *data;
    uint32_t len, rate;
    fdm_mixer m;
    int16_t out[2 * 400];
    unsigned i, nonzero_l = 0, nonzero_r = 0;

    lump[0] = 3; lump[1] = 0; lump[2] = 0x11; lump[3] = 0x2B;        /* 11025 Hz */
    lump[4] = 132; lump[5] = 0; lump[6] = 0; lump[7] = 0;            /* 132 samples = 16 + 100 + 16 */
    for (i = 0; i < 132; ++i) lump[8 + i] = (uint8_t)(i < 16 || i >= 116 ? 0 : 255);   /* padding 0, body loud */
    assert(fdm_parse_lump(lump, sizeof lump, &data, &len, &rate) && rate == 11025 && len == 100 && data == lump + 24);
    assert(!fdm_parse_lump(lump, 6, &data, &len, &rate));
    lump[0] = 2;
    assert(!fdm_parse_lump(lump, sizeof lump, &data, &len, &rate));
    lump[0] = 3;

    memset(&m, 0, sizeof m);
    m.out_rate = 44100;
    fdm_start(&m, 0, data, len, rate, 127, 0);                       /* full volume, hard left */
    assert(fdm_playing(&m, 0) && m.v[0].step == (11025u << 16) / 44100u);
    fdm_mix(&m, out, 400);
    for (i = 0; i < 400; ++i) {
        if (out[2 * i]) ++nonzero_l;
        if (out[2 * i + 1]) ++nonzero_r;
    }
    /* 100 samples at a quarter of the output rate = 400 frames, all on the left, none of the padding */
    assert(nonzero_l == 400 && nonzero_r == 0 && out[0] == 127 * 254 * FDM_SFX_GAIN / 256);
    assert(!fdm_playing(&m, 0) || (m.v[0].pos >> 16) >= 100);
    fdm_mix(&m, out, 4);
    assert(!fdm_playing(&m, 0) && out[0] == 0);

    /* centre pan; two voices add up; enough loud voices clip instead of wrapping */
    fdm_start(&m, 1, data, len, rate, 127, 128);
    fdm_start(&m, 2, data, len, rate, 127, 128);
    assert(m.v[1].left == 126 && m.v[1].right == 128);
    fdm_mix(&m, out, 1);
    assert(out[0] == 127 * 126 * 2 * FDM_SFX_GAIN / 256 && out[1] == 127 * 128 * 2 * FDM_SFX_GAIN / 256);
    for (i = 3; i < FDM_VOICES; ++i) fdm_start(&m, (int)i, data, len, rate, 127, 128);
    fdm_mix(&m, out, 1);
    assert(out[0] == 32767 && out[1] == 32767);
    for (i = 4; i < FDM_VOICES; ++i) fdm_stop(&m, (int)i);
    /* one full-volume effect stays at a quarter of full scale (it was full scale before the car test) */
    assert(127 * 254 * FDM_SFX_GAIN / 256 < 8200);
    fdm_params(&m, 1, 0, 128);                                        /* volume 0: silent */
    fdm_stop(&m, 2);
    fdm_stop(&m, 3);
    fdm_mix(&m, out, 1);
    assert(out[0] == 0 && out[1] == 0 && !fdm_playing(&m, 2));
    printf("freedoom mixer: DMX padding skipped, 11025 Hz stretched to 44100, panning, effects at a quarter, mixing and clipping.\n");
    return 0;
}
