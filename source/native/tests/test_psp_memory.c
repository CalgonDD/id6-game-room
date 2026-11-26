#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#define PSP_MEMORY_TEST
struct stat64 { int64_t st_size; };
static int mode;
static int stat64(const char *path,struct stat64 *info) {
    assert(!strcmp(path,"/proc"));
    if(mode==1){errno=EACCES;return -1;}
    info->st_size=mode==2?-1:mode==3?0:mode==4?INT64_C(3)*1024*1024*1024:INT64_C(300)*1024*1024;
    return 0;
}
#include "psp_memory.h"
int main(void){uint64_t bytes;int error;
    assert(psp_memory_available(&bytes,&error) && bytes==300u*1024u*1024u && !error);
    mode=1;assert(!psp_memory_available(&bytes,&error) && error==EACCES && !bytes);
    mode=2;assert(!psp_memory_available(&bytes,&error) && error==ERANGE && !bytes);
    mode=3;assert(psp_memory_available(&bytes,&error) && !bytes && !error);
    mode=4;assert(psp_memory_available(&bytes,&error) && bytes==UINT64_C(3)*1024*1024*1024 && !error);
    puts("PSP_MEMORY_POLICY_PASS: proc stat64, real zero, errors and >2GiB");return 0;
}
