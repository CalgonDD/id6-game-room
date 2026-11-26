/* SDL_endian.h for OpenTyrian on the iDrive: the car (ARMv7) and the PC are both little-endian. */
#ifndef ID6_SDL_ENDIAN_H
#define ID6_SDL_ENDIAN_H
#include "SDL_types.h"
#define SDL_LIL_ENDIAN 1234
#define SDL_BIG_ENDIAN 4321
#define SDL_BYTEORDER SDL_LIL_ENDIAN
static inline Uint16 SDL_SwapLE16(Uint16 x) { return x; }
static inline Uint32 SDL_SwapLE32(Uint32 x) { return x; }
static inline Uint16 SDL_SwapBE16(Uint16 x) { return (Uint16)((x >> 8) | (x << 8)); }
static inline Uint32 SDL_SwapBE32(Uint32 x) {
    return (x >> 24) | ((x >> 8) & 0xff00u) | ((x << 8) & 0xff0000u) | (x << 24);
}
#endif
