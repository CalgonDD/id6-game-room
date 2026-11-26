from pathlib import Path
root=Path('/src/ppsspp')
# GCC 8 QNX's __builtin___clear_cache compiled to a no-op in the shipped core.
# Keep the explicit QNX helper identical to the independently executed probe.
(root/'Common/QNXJitCache.h').write_bytes((Path(__file__).parent/'qnx_jit_cache.h').read_bytes())
(root/'Core/MIPS/QNXAlignmentProbe.cpp').write_bytes((Path(__file__).parent/'qnx_alignment_probe.cpp').read_bytes())
# Git's exported RELEASE file carries CRLF and becomes a literal CR inside
# FFmpeg's generated C version string. Normalize this one metadata input.
p=root/'ffmpeg/RELEASE'
p.write_bytes(p.read_bytes().replace(b'\r\n', b'\n'))
def replace(name, old, new):
    path=root/name
    text=path.read_text()
    if new in text: return
    assert text.count(old)==1, name
    path.write_text(text.replace(old,new))
replace('Core/MIPS/ARM/ArmCompLoadStore.cpp', 'if (!js.inDelaySlot && !jo.Disabled(JitDisable::LSU_UNALIGNED)) {', 'if (!PPSSPP_PLATFORM(QNX) && !js.inDelaySlot && !jo.Disabled(JitDisable::LSU_UNALIGNED)) {')
replace('libretro/CMakeLists.txt', 'add_library(ppsspp_libretro SHARED ${LIBRETRO_SRCS})', 'add_library(ppsspp_libretro SHARED ${LIBRETRO_SRCS})\nif(CMAKE_SYSTEM_NAME STREQUAL "QNX")\n    target_sources(ppsspp_libretro PRIVATE ../Core/MIPS/QNXAlignmentProbe.cpp)\nendif()')
replace('Common/ArmEmitter.cpp', '#include "Common/CPUDetect.h"', '#include "Common/CPUDetect.h"\n#if PPSSPP_PLATFORM(QNX)\n#include "Common/QNXJitCache.h"\n#endif')
replace('Common/ArmEmitter.cpp', '#if PPSSPP_PLATFORM(IOS)\n\t// Header file says', '#if PPSSPP_PLATFORM(QNX)\n    if (!id6_qnx_flush_jit(start, end)) {\n        ERROR_LOG(JIT, "QNX JIT cache synchronization failed errno=%d", errno);\n        _exit(90);\n    }\n#elif PPSSPP_PLATFORM(IOS)\n\t// Header file says')
replace('ppsspp_config.h', '#elif defined(__OpenBSD__)', '#elif defined(__QNXNTO__)\n    #define PPSSPP_PLATFORM_QNX 1\n#elif defined(__OpenBSD__)')
replace('libretro/LibretroGraphicsContext.cpp', 'preferred == RETRO_HW_CONTEXT_OPENGL || preferred == RETRO_HW_CONTEXT_OPENGLES3', 'preferred == RETRO_HW_CONTEXT_OPENGL || preferred == RETRO_HW_CONTEXT_OPENGLES2 || preferred == RETRO_HW_CONTEXT_OPENGLES3')
replace('CMakeLists.txt', 'add_definitions(-D_XOPEN_SOURCE=700)', 'if(CMAKE_SYSTEM_NAME STREQUAL "QNX")\n            add_definitions(-D_XOPEN_SOURCE=600)\n        else()\n            add_definitions(-D_XOPEN_SOURCE=700)\n        endif()')
replace('libretro/CMakeLists.txt', 'target_link_libraries(ppsspp_libretro ${LinkCommon})', 'target_link_libraries(ppsspp_libretro ${LinkCommon})\nif(CMAKE_SYSTEM_NAME STREQUAL "QNX")\n    target_link_libraries(ppsspp_libretro socket m c)\nendif()')
replace('Common/GPU/OpenGL/GLCommon.h', '#include <GLES2/gl2ext.h>', '#include <GLES2/gl2ext.h>\n#if PPSSPP_PLATFORM(QNX)\n// QNX 6.5 GLES2 headers spell shader strings as char, without GLchar.\ntypedef char GLchar;\n#endif')
replace('Common/Net/Resolve.cpp', 'hints.ai_flags = /*AI_V4MAPPED |*/ AI_ADDRCONFIG;', '#ifdef AI_ADDRCONFIG\n    hints.ai_flags = AI_ADDRCONFIG;\n#else\n    // QNX 6.5 lacks the optional local-interface address filter.\n    hints.ai_flags = 0;\n#endif')
replace('Common/Thread/ThreadUtil.cpp', '#include <sys/syscall.h>', '#if !PPSSPP_PLATFORM(QNX)\n#include <sys/syscall.h>\n#endif')
replace('Common/MemArenaPosix.cpp', '// Fall back to old tmpfs behavior.', '''// QNX must keep emulated RAM in shared memory, never a flash temp file.
#if PPSSPP_PLATFORM(QNX)
    if (fd < 0) {
        ERROR_LOG(MEMMAP, "QNX shm_open failed: %s", strerror(errno));
        return false;
    }
#endif
\t// Fall back to old tmpfs behavior.''')
replace('Common/MemArenaPosix.cpp', '// Should this be a failure?', '''#if PPSSPP_PLATFORM(QNX)
        close(fd);
        fd = -1;
        return false;
#endif
        // Should this be a failure?''')
replace('Common/MemArenaPosix.cpp', 'void* base = mmap(0, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED | MAP_NORESERVE, -1, 0);', '''#if PPSSPP_PLATFORM(QNX)
    // This is an address-space probe, not emulated RAM. QNX otherwise reserves
    // physical pages for the temporary 256 MiB mapping before unmapping it.
    void* base = mmap(0, size, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_LAZY, -1, 0);
#else
    void* base = mmap(0, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED | MAP_NORESERVE, -1, 0);
#endif''')
p=root/"git-version.cmake"
s=p.read_text().replace("ID6-QNX-cachefix1", "ID6-QNX-portablevideo1").replace("ID6-QNX-alignmentdiag1", "ID6-QNX-portablevideo1")
p.write_text(s.replace('set(GIT_VERSION "unknown")','set(GIT_VERSION "v1.17.1-d479b74ed9-ID6-QNX-portablevideo1")').replace('set(GIT_VERSION "v1.17.1-d479b74ed9-ID6-QNX-gate3")','set(GIT_VERSION "v1.17.1-d479b74ed9-ID6-QNX-portablevideo1")'))
replace('libretro/libretro.cpp', '''   struct retro_log_callback log;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
   {
      LogManager::Init(&g_Config.bEnableLogging);''', '''   // Config::Load requires a LogManager even when the optional frontend log
   // interface is absent. The local QNX guest exercises this API fallback.
   LogManager::Init(&g_Config.bEnableLogging);
   struct retro_log_callback log = {};
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
   {''')
replace('Common/ArmCPUDetect.cpp', '#elif PPSSPP_PLATFORM(WINDOWS)\n\ttruncate_cpy(brand_string', '''#elif PPSSPP_PLATFORM(QNX)
    // This worker is explicitly built for ARMv7/NEON (same target as PS1).
    // Do not infer optional VFPv4/IDIV support from the board name.
    strcpy(brand_string, "QNX ARMv7 NEON");
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
    isVFP3 = true;
#endif
    num_cores = 1;
#elif PPSSPP_PLATFORM(WINDOWS)
\ttruncate_cpy(brand_string''')
replace('ext/libzip/zip_random_unix.c', 'srandom((unsigned int)time(NULL));', '''#ifdef __QNXNTO__
    srand((unsigned int)time(NULL));
#else
    srandom((unsigned int)time(NULL));
#endif
    seeded = true;''')
replace('ext/libzip/zip_random_unix.c', 'return (zip_uint32_t)random();', '''#ifdef __QNXNTO__
    return (zip_uint32_t)rand();
#else
    return (zip_uint32_t)random();
#endif''')
replace('ext/SPIRV-Cross/spirv_glsl.cpp', '#ifndef _WIN32\n#include <langinfo.h>', '#if !defined(_WIN32) && !defined(__QNXNTO__)\n#include <langinfo.h>')
replace('ext/SPIRV-Cross/spirv_glsl.cpp', '#elif defined(__ANDROID__) && __ANDROID_API__ < 26', '#elif defined(__QNXNTO__) || (defined(__ANDROID__) && __ANDROID_API__ < 26)')
p=root/'Core/HLE/proAdhoc.cpp'
t=p.read_text()
t=t.replace('saNet.sin_addr.s_addr, saMask.sin_addr.s_addr', '(uint32_t)saNet.sin_addr.s_addr, (uint32_t)saMask.sin_addr.s_addr')
t=t.replace('#if !PPSSPP_PLATFORM(SWITCH) && !PPSSPP_PLATFORM(OPENBSD)\n', '#if !PPSSPP_PLATFORM(SWITCH) && !PPSSPP_PLATFORM(OPENBSD) && !PPSSPP_PLATFORM(QNX)\n')
p.write_text(t)
replace('ext/jpge/jpge.cpp', '#include "jpge.h"', '#include <cstdio>  // Keep QNX C declarations outside namespace jpge.\n#include "jpge.h"')
p=root/'ext/jpge/jpgd.cpp'
t=p.read_text(); p.write_text(t.replace('::setjmp(', 'setjmp('))
replace('Common/StringUtils.cpp', '#else\n\tchar *buf = nullptr;\n', '''#elif PPSSPP_PLATFORM(QNX)
    va_start(args, format);
    va_list measure;
    va_copy(measure, args);
    int needed = vsnprintf(nullptr, 0, format, measure);
    va_end(measure);
    if (needed >= 0) {
        temp.resize((size_t)needed + 1);
        int written = vsnprintf(&temp[0], temp.size(), format, args);
        if (written == needed) temp.resize((size_t)needed);
        else temp.clear();
    }
    va_end(args);
#else
\tchar *buf = nullptr;
''')
replace('ext/vma/vk_mem_alloc.h', '\n    return aligned_alloc(alignment, size);', '''
#if defined(__QNXNTO__)
    void *pointer = VMA_NULL;
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    return posix_memalign(&pointer, alignment, size) == 0 ? pointer : VMA_NULL;
#else
    return aligned_alloc(alignment, size);
#endif''')
# QNX sys/mman.h reserves MAP_NOINIT. Rename the emulator's unrelated register
# allocation flag at all declarations/use sites, leaving the OS macro intact.
for path in (root/'Core/MIPS').rglob('*'):
    if path.suffix in ('.cpp','.h'):
        text=path.read_text()
        import re
        updated=re.sub(r'\bMAP_NOINIT\b','JIT_MAP_NOINIT',text)
        if updated!=text: path.write_text(updated)

