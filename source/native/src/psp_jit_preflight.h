#ifndef ID6_PSP_JIT_PREFLIGHT_H
#define ID6_PSP_JIT_PREFLIGHT_H
#ifdef __QNX__
#include "../psp/qnx_jit_cache.h"
/* Run in the renderer child before creating any graphics/audio resource. */
static int psp_jit_preflight(void) {
    long page=sysconf(_SC_PAGESIZE);void *block;volatile uint32_t *code;
    int (*function)(void);unsigned i;
    if(page<8 || page>65536)return 0;
    block=mmap(0,(size_t)page*2,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_ANON|MAP_PRIVATE,-1,0);
    if(block==MAP_FAILED){mps_log("psp jit_preflight=map_failed errno=%d",errno);return 0;}
    code=(volatile uint32_t*)((char*)block+page-4);
    function=(int(*)(void))(uintptr_t)code;code[1]=0xe12fff1e;
    for(i=1;i<=64;++i){
        code[0]=0xe3a00000|i;
        if(!id6_qnx_flush_jit((void*)code,(void*)(code+2)) || function()!=(int)i){
            mps_log("psp jit_preflight=failed iteration=%u errno=%d",i,errno);
            munmap(block,(size_t)page*2);return 0;
        }
    }
    if(munmap(block,(size_t)page*2))return 0;
    mps_log("psp jit_preflight=pass rewrites=64");return 1;
}
#else
static int psp_jit_preflight(void){return 1;}
#endif
#endif
