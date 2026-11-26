/* Actual P.E.Op.S renderer: odd-x fills, solid and textured triangles; VRAM guards. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ps1_gpu_access.h"
#include "plugins/dfxvideo/gpulib_if.c"
struct psx_gpu gpu;
const unsigned char cmd_lengths[256]={0};
#define VRAM_WORDS (1024*512)
static uint16_t data[VRAM_WORDS+64];
static unsigned long long hashv(void){ unsigned long long h=14695981039346656037ULL; unsigned i; for(i=0;i<VRAM_WORDS;i++){ h^=data[i+32]&255;h*=1099511628211ULL;h^=data[i+32]>>8;h*=1099511628211ULL;}return h;}
static void guards(void){unsigned i;for(i=0;i<32;i++)assert(data[i]==0x5a5a && data[VRAM_WORDS+32+i]==0x5a5a);}
static void test_pairs(void) {
    uint16_t pixels[6];
    unsigned i;
    for(i=0;i<65536;i++) {
        uint32_t value=i*2654435761u;
        unsigned j;
        for(j=0;j<6;j++) pixels[j]=0x5a5a;
        ps1_gpu_write32(pixels+1,value);
        assert(pixels[1]==(uint16_t)value && pixels[2]==(uint16_t)(value>>16));
        assert(ps1_gpu_read32(pixels+1)==value);
        assert(pixels[0]==0x5a5a && pixels[3]==0x5a5a && pixels[4]==0x5a5a && pixels[5]==0x5a5a);
    }
}
int main(void){unsigned i; test_pairs(); memset(data,0x5a,sizeof data); memset(data+32,0,VRAM_WORDS*2);gpu.vram=(void*)(data+32);renderer_init();drawX=0;drawY=0;drawW=1023;drawH=511;
FillSoftwareArea(1,2,11,4,0x1234);for(i=0;i<1024;i++)assert(data[32+2*1024+i]==((i>=1&&i<11)?0x1234:0));guards();printf("fill %016llx\n",hashv());
FillSoftwareAreaTrans(3,5,15,7,0x4321);guards();printf("filltrans %016llx\n",hashv());
drawPoly3Fi(1,11,31,11,1,31,0x00ee8040);guards();printf("solidtri %016llx\n",hashv());
for(i=0;i<16;i++)data[32+256+i]=(uint16_t)(i*0x421);
for(i=0;i<16;i++)data[32+i]=0x4321;
GlobalTextTP=0;GlobalTextAddrX=0;GlobalTextAddrY=0;
drawPoly3TEx4(1,33,15,33,1,43,0,0,8,0,0,8,256,0);guards();printf("texturedtri %016llx\n",hashv());
DrawSemiTrans=1;drawPoly3TEx4(3,45,17,45,3,55,0,0,8,0,0,8,256,0);guards();printf("blendedtexturedtri %016llx\n",hashv());
return 0;}
