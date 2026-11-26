/* C11 <stdalign.h> for the car's gcc 4.4 (PCSX ReARMed uses alignas). */
#ifndef PS1_COMPAT_STDALIGN_H
#define PS1_COMPAT_STDALIGN_H
#define alignas(x) __attribute__((aligned(x)))
#define alignof(x) __alignof__(x)
#define __alignas_is_defined 1
#define __alignof_is_defined 1
#endif
