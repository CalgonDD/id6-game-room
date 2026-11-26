#ifndef PS1_QNX_GPU_ACCESS_H
#define PS1_QNX_GPU_ACCESS_H

#include <stdint.h>

/* QNX PS1 VRAM is a 16-bit pixel array: odd x gives only 2-byte alignment.
 * Volatile halfword accesses prevent GCC from merging a pair into a word
 * access. These helpers require 2-byte alignment, not arbitrary byte alignment.
 * The car build is explicitly little-endian. */
static inline uint32_t ps1_gpu_read32(const void *address)
{
    const volatile uint16_t *pixels = (const volatile uint16_t *)address;
    return (uint32_t)pixels[0] | ((uint32_t)pixels[1] << 16);
}

static inline void ps1_gpu_write32(void *address, uint32_t value)
{
    volatile uint16_t *pixels = (volatile uint16_t *)address;
    pixels[0] = (uint16_t)value;
    pixels[1] = (uint16_t)(value >> 16);
}

#endif
