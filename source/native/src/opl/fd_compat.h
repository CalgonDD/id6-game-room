/* fd_compat.h: what Chocolate Doom's music code expects from SDL and its own engine that doomgeneric doesn't
 * have. Force-included (-include) when compiling src/opl/. Both the car (ARMv7) and the PC are little-endian. */
#ifndef FD_COMPAT_H
#define FD_COMPAT_H

#include <stdint.h>
#include <stdlib.h>

#ifndef PACKED_STRUCT
#define PACKED_STRUCT(...) struct __VA_ARGS__ __attribute__ ((packed))
#endif

static inline uint16_t SDL_SwapBE16(uint16_t x) { return (uint16_t)((x >> 8) | (x << 8)); }
static inline uint32_t SDL_SwapBE32(uint32_t x) {
    return (x >> 24) | ((x >> 8) & 0xff00u) | ((x << 8) & 0xff0000u) | (x << 24);
}

void I_Error(char *error, ...);
static inline void *I_Realloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (p == NULL && size != 0) I_Error("I_Realloc: failed on reallocation of %u bytes", (unsigned)size);
    return p;
}

#endif
