#ifndef ID6_PSP_MEMORY_H
#define ID6_PSP_MEMORY_H
#include <stdint.h>
#include <errno.h>
#if defined(__QNX__) || defined(PSP_MEMORY_TEST)
#ifndef PSP_MEMORY_TEST
#include <sys/stat.h>
#endif
/* QNX documents /proc's st_size as free physical memory. Use stat64 to
 * preserve values above 2 GiB. No board-specific typed-memory name, shell
 * command, allocation or map is involved. Never use this on Linux /proc. */
static int psp_memory_available(uint64_t *bytes, int *error) {
    struct stat64 info;
    *bytes=0;*error=0;
    if(stat64("/proc",&info)!=0){*error=errno;return 0;}
    if(info.st_size<0){*error=ERANGE;return 0;}
    *bytes=(uint64_t)info.st_size;return 1;
}
#else
static int psp_memory_available(uint64_t *bytes, int *error) {
    *bytes=UINT64_MAX;*error=0;return 1;
}
#endif
#endif
