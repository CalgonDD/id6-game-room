// GPL-2.0-or-later. CPU-only diagnostic derived from PPSSPP's JitHarness setup.
#include "ppsspp_config.h"
#include <cstring>
#include <cstdio>
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/Core.h"
#include "Core/CoreParameter.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/SymbolMap.h"
#include "Core/HLE/HLE.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSVFPUUtils.h"
#include "Core/MIPS/JitCommon/JitCommon.h"
#include "Core/MIPS/JitCommon/JitBlockCache.h"
#include "Core/MIPS/JitCommon/JitState.h"
extern "C" {
#include "../../ffmpeg/libavcodec/h264pred.h"
#include "../../ffmpeg/libavcodec/avcodec.h"
}
// Exercise the decoder dispatch that faulted on the car, without opening a game
// or creating any graphics/audio resources. Every byte outside the block is a guard.
extern "C" __attribute__((visibility("default"))) int retro_id6_video_alignment_probe(unsigned *completed) {
    *completed = 0;
    H264PredContext prediction{};
    ff_h264_pred_init(&prediction, AV_CODEC_ID_H264, 8, 1);
    if (!prediction.pred8x8[PLANE_PRED8x8]) return 20;
    alignas(8) unsigned char pixels[4096], before[4096];
    const unsigned strides[] = {240, 241, 243};
    for (unsigned stride : strides) for (unsigned offset = 0; offset < 4; ++offset) {
        memset(pixels, 37, sizeof pixels);
        unsigned base = 512 + offset;
        for (unsigned y = 0; y < 8; ++y) memset(pixels + base + y * stride, 0xcc, 8);
        memcpy(before, pixels, sizeof pixels);
        std::printf("VIDEO_ALIGNMENT_CASE stride=%u offset=%u\n", stride, offset); std::fflush(stdout);
        prediction.pred8x8[PLANE_PRED8x8](pixels + base, stride);
        for (unsigned i = 0; i < sizeof pixels; ++i) {
            bool output = false;
            for (unsigned y = 0; y < 8; ++y) if (i >= base + y * stride && i < base + y * stride + 8) output = true;
            if (pixels[i] != (output ? 37 : before[i])) return 21;
        }
        ++*completed;
    }
    return 0;
}
static void FinishAlignmentProbe(){coreState=CORE_POWERDOWN;hleSkipDeadbeef();}
static const HLEFunction probeCalls[]={{0x1234BEEF,&FinishAlignmentProbe,"FinishAlignmentProbe"}};
extern "C" __attribute__((visibility("default"))) int retro_id6_alignment_probe(int safe,unsigned *completed) {
    *completed=0;
    g_Config.bSaveSettings=false;
    g_Config.bFastMemory=false;
    if(safe)g_Config.uJitDisableFlags|=(uint32_t)MIPSComp::JitDisable::LSU_UNALIGNED;
    else g_Config.uJitDisableFlags&=~(uint32_t)MIPSComp::JitDisable::LSU_UNALIGNED;
    RegisterModule("ID6AlignmentProbe",1,probeCalls);
    coreState=CORE_POWERUP;currentMIPS=&mipsr4k;g_symbolMap=new SymbolMap();
    Memory::g_MemorySize=Memory::RAM_NORMAL_SIZE;
    PSP_CoreParameter().cpuCore=CPUCore::INTERPRETER;
    if(!Memory::Init())return 10;
    mipsr4k.Reset();CoreTiming::Init();InitVFPU();
    mipsr4k.UpdateCore(CPUCore::JIT);
    if(!MIPSComp::jit)return 11;
    const uint32_t codeAddress=0x08800000,source=0x08802000,target=0x08803000;
    int result=0;
    for(unsigned from=0;from<4 && !result;++from)for(unsigned to=0;to<4 && !result;++to){
        MIPSComp::jit->ClearCache();
        auto *program=(uint32_t*)Memory::GetPointerWrite(codeAddress);
        // LWL/LWR + SWL/SWR deliberately cover every alignment combination.
        program[0]=(34u<<26)|(4u<<21)|(2u<<16)|3;
        program[1]=(38u<<26)|(4u<<21)|(2u<<16);
        program[2]=(42u<<26)|(5u<<21)|(2u<<16)|3;
        program[3]=(46u<<26)|(5u<<21)|(2u<<16);
        program[4]=GetSyscallOp("ID6AlignmentProbe",0x1234BEEF);program[5]=0;
        auto *in=Memory::GetPointerWrite(source),*out=Memory::GetPointerWrite(target);
        for(unsigned i=0;i<16;++i)in[i]=(uint8_t)(i*13+7);
        memset(out,0xa5,16);
        currentMIPS->r[4]=source+from;currentMIPS->r[5]=target+to;
        currentMIPS->pc=codeAddress;currentMIPS->downcount=10000;
        std::printf("CPU_ALIGNMENT_CASE source=%u target=%u\n",from,to);std::fflush(stdout);
        coreState=CORE_RUNNING;
        currentMIPS->RunLoopUntil(CoreTiming::GetTicks()+1000000);
        if(coreState!=CORE_POWERDOWN)result=12;
        for(unsigned i=0;i<16;++i){
            uint8_t expected=i>=to && i<to+4?in[from+i-to]:0xa5;
            if(out[i]!=expected)result=13;
        }
        if(!result)++*completed;
    }
    HLEShutdown();CoreTiming::Shutdown();mipsr4k.Shutdown();Memory::Shutdown();
    coreState=CORE_POWERDOWN;currentMIPS=nullptr;delete g_symbolMap;g_symbolMap=nullptr;
    return result;
}
