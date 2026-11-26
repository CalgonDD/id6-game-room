#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#define ID6_CACHE_TEST
#define _SC_PAGESIZE 1
#define MS_SYNC 2
#define MS_CACHE_ONLY 0x2000000
#define MS_INVALIDATE_ICACHE 0x1000000
static int mode,calls;
static long sysconf(int key){assert(key==_SC_PAGESIZE);return mode==1?-1:4096;}
static int msync(void *addr,size_t length,int flags){
    ++calls;assert((uintptr_t)addr==0x1000 && length==8192);
    assert(flags==(MS_SYNC|MS_CACHE_ONLY|MS_INVALIDATE_ICACHE));
    if(mode==2){errno=ENOMEM;return -1;}return 0;
}
#include "../psp/qnx_jit_cache.h"
int main(void){
    assert(id6_qnx_flush_jit((void*)0x1ffc,(void*)0x2004) && calls==1);
    assert(id6_qnx_flush_jit((void*)0x2000,(void*)0x2000) && calls==1);
    assert(!id6_qnx_flush_jit((void*)0x2000,(void*)0x1000) && errno==EINVAL && calls==1);
    assert(!id6_qnx_flush_jit((void*)0x1000,(void*)UINTPTR_MAX) && errno==EOVERFLOW && calls==1);
    mode=1;assert(!id6_qnx_flush_jit((void*)0x1ffc,(void*)0x2004) && errno==EINVAL && calls==1);
    mode=2;assert(!id6_qnx_flush_jit((void*)0x1ffc,(void*)0x2004) && errno==ENOMEM && calls==2);
    puts("PSP_JIT_CACHE_POLICY_PASS range, flags, empty, overflow and failed flush");return 0;
}
