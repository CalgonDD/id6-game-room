#ifndef ID6_QNX_CPP_COMPAT_H
#define ID6_QNX_CPP_COMPAT_H
#include <cstddef>
#include <cstring>
#if defined(__QNXNTO__)
/* QNX 6.5 has no strnlen symbol. Bounded local equivalent, no libc override. */
static inline size_t strnlen(const char *text, size_t bound) {
    const char *end = static_cast<const char *>(memchr(text, 0, bound));
    return end ? static_cast<size_t>(end - text) : bound;
}
#endif
#endif
