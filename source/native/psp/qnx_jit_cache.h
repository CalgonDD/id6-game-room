/* QNX ARM generated-code coherency. Shared by the port and offline probe. */
#ifndef ID6_QNX_JIT_CACHE_H
#define ID6_QNX_JIT_CACHE_H
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#ifndef ID6_CACHE_TEST
#include <unistd.h>
#include <sys/mman.h>
#endif
static inline int id6_qnx_flush_jit(void *start,void *end) {
    uintptr_t first=(uintptr_t)start,last=(uintptr_t)end,base,limit;
    long page;
    if(last==first)return 1;
    if(last<first){errno=EINVAL;return 0;}
    page=sysconf(_SC_PAGESIZE);
    if(page<=0 || ((uintptr_t)page&((uintptr_t)page-1))){errno=EINVAL;return 0;}
    if(last>UINTPTR_MAX-((uintptr_t)page-1)){errno=EOVERFLOW;return 0;}
    base=first&~((uintptr_t)page-1);
    limit=(last+(uintptr_t)page-1)&~((uintptr_t)page-1);
    /* RAM cache only: never request backing-file/storage synchronization. */
    return msync((void*)base,limit-base,MS_SYNC|MS_CACHE_ONLY|MS_INVALIDATE_ICACHE)==0;
}
#endif
