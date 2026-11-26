/* QNX 6.5 Dinkum headers and GCC 8 use __SIZE_T for different purposes.
 * Establish C types before any config/header can consume the QNX carrier.
 * No ABI change: use the compiler's actual target types. C++ is handled by
 * the pinned toolchain's os_defines.h, not by this C-only bridge. */
#ifndef ID6_QNX_C_TYPES_H
#define ID6_QNX_C_TYPES_H
#if !defined(__cplusplus) && !defined(__ASSEMBLER__)
#include <stddef.h>
typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#undef __SIZE_T
#undef __PTRDIFF_T
#undef __WCHAR_T
#include <assert.h>
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#ifndef static_assert
#define static_assert _Static_assert
#endif
#endif
#endif
#endif
