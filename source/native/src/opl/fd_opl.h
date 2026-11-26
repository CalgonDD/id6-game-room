/* fd_opl.h: the audio thread's side of the emulated OPL chip (fd_opl.c). */
#ifndef FD_OPL_H
#define FD_OPL_H

#include <stdint.h>

/* Adds `frames` stereo frames of music into out (clipped), scaled by gain_q8 (256 = as generated), and
 * advances the song. Returns 1 if any music sample was non-zero. */
int fd_opl_render(int16_t *out, unsigned frames, int gain_q8);

/* Samples clipped so far: inside the emulated chip, and when the music was added to the effects. */
unsigned fd_opl_chip_clips(void);
unsigned fd_opl_mix_clips(void);

#endif
