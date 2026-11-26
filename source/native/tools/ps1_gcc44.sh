#!/bin/sh
# Makes a COPY of PCSX ReARMed build with the car's gcc 4.4 (C99 + GNU), run by build.sh on out/ps1_src only.
# gcc 4.4 rejects a typedef repeated with the same type (C11 allows it): guard the two such pairs.
set -eu
d=$1
test -f "$d/.gcc44-strict-v3" && exit 0
if ! test -f "$d/.gcc44"; then
# rthreads.h repeats pcsxr-threads.h's three typedefs (pcsxr-threads.c includes both)
sed -i 's/^#include "pcsxr-threads.h"$/#include "pcsxr-threads.h"\n#define PS1_RTHREADS_TYPES 1/' "$d/frontend/pcsxr-threads.c"
for t in sthread_t slock_t scond_t; do
    sed -i "s/^typedef struct \([a-z]*\) $t;\$/#ifndef PS1_RTHREADS_TYPES\ntypedef struct \1 $t;\n#endif/" "$d/deps/libretro-common/include/rthreads/rthreads.h"
done
# miniz.h and 7zTypes.h both typedef Byte (libchdr includes both)
for f in "$d/deps/libchdr/deps/miniz-3.1.2/miniz.h" "$d/deps/libchdr/deps/lzma-26.02/include/real/7zTypes.h"; do
    sed -i 's/^\( *\)typedef unsigned char Byte;$/\1#ifndef PS1_BYTE_TYPE\n#define PS1_BYTE_TYPE 1\n\1typedef unsigned char Byte;\n#endif/' "$f"
done
# The car's assembler (binutils of QNX 6.5) knows "dmb" but not its "ishst" option: use the full barrier (stronger).
sed -i 's/"dmb ishst"/"dmb"/' "$d/plugins/gpulib/gpu_async.c" "$d/deps/libretro-common/memmap/memcpy_nt.c"
fi
# NEON loads/stores with an alignment hint ([rN, :128]) fault with SIGBUS on a less-aligned address. The code puts
# 16-byte-aligned vertex arrays on the stack (psx_gpu_parse.c), but gcc 4.4 caps stack alignment at 8 bytes on ARM:
# the car crashed in compute_all_gradients on "vld1.32 {d0-d1}, [r1, :128]" with r1 = 0x...598 (ps1-qnx.core,
# 2026-11-20 18:12). This removes the explicit 16-byte alignment assertion; element alignment is handled below.
find "$d" -name '*.S' -exec sed -i -E 's/, *:(64|128|256)\]/]/g' '{}' ';'
# The captured QNX run also faults on an unhinted "vld1.32" at a 2-byte-aligned frame
# buffer pointer (car 2026-11-20 18:27: shade_blocks_shaded_textured_modulated_dithered_direct, vld1.32 {d16-d17},
# [lr] with lr = 0x022a42de). A whole-register vld1/vst1 puts the same bytes in the same places whatever its element
# size on little-endian ARM; the size only sets the alignment it demands. So those become .8 (any address). Lane
# forms ({dN[x]}) and vld2-4 depend on the size and stay as they are.
find "$d" -name '*.S' | while IFS= read -r f; do
    sed -i -E 's/\b(v(ld|st)1)\.(u|s|i|f)?(16|32|64)([[:space:]]+\{[^}[]*\})/\1.8\5/g' "$f"
    sed -i -E 's/\b(v(ld|st)1)\.(u|s|i|f)?(16|32|64)([[:space:]]+d[0-9]+[[:space:]]*,)/\1.8\5/g' "$f"   # "vst1.32 d16, [r3]"
done

# 18:36 fast-mode core: get_reg word-loads a signed-byte map at struct regstat
# +13 (r0=0x44e25). Select PCSX's existing bytewise C fallback on QNX, retaining
# the dynarec and NEON renderer. Aligning the whole struct cannot fix the field.
ndrc="$d/libpcsxcore/new_dynarec/new_dynarec.c"
linkage="$d/libpcsxcore/new_dynarec/linkage_arm.S"
sed -i 's/^#if defined(__arm__) \&\& defined(HAVE_ARMV6) \&\& HOST_REGS == 13 \&\& EXCLUDE_REG == 11$/#if defined(__arm__) \&\& defined(HAVE_ARMV6) \&\& HOST_REGS == 13 \&\& EXCLUDE_REG == 11 \&\& !defined(__QNXNTO__)/' "$ndrc"
sed -i 's/^#ifdef HAVE_ARMV6$/#if defined(HAVE_ARMV6) \&\& !defined(__QNXNTO__)/' "$linkage"
grep -q 'EXCLUDE_REG == 11 && !defined(__QNXNTO__)' "$ndrc" || { echo 'QNX get_reg patch did not apply' >&2; exit 1; }
grep -q '^#if defined(HAVE_ARMV6) && !defined(__QNXNTO__)$' "$linkage" || { echo 'QNX assembly exclusion did not apply' >&2; exit 1; }


# P.E.Op.S QNX renderer: its packed two-pixel paths may start at odd x.
# Route them through explicit halfword operations, not ARMv7 unaligned words.
for f in "$d/plugins/dfxvideo/gpu.h" "$d/plugins/dfxvideo/gpulib_if.c"; do
    sed -i '/^#if defined(__arm__) \&\& !defined(HAVE_ARMV6)$/c\#if defined(__QNXNTO__)\n#include <ps1_gpu_access.h>\n#define GETLE32(X) ps1_gpu_read32(X)\n#define PUTLE32(X, Y) ps1_gpu_write32(X, Y)\n#elif defined(__arm__) \&\& !defined(HAVE_ARMV6)' "$f"
    grep -q 'ps1_gpu_read32(X)' "$f" || { echo 'QNX GPU access patch did not apply' >&2; exit 1; }
done
soft="$d/plugins/dfxvideo/soft.c"
sed -i -E 's/\*DSTPtr\+\+\ =\ lcol;/PUTLE32(DSTPtr, LE2HOST32(lcol)); DSTPtr++;/g' "$soft"
sed -i -E 's/\*\(uint32_t\ \*\)\&psxVuw\[\(i<<10\)\+j\]\ =\ lcolor;/PUTLE32(\&psxVuw[(i<<10)+j], LE2HOST32(lcolor));/g' "$soft"
test "$(grep -c 'PUTLE32(DSTPtr, LE2HOST32(lcol)); DSTPtr++;' "$soft")" = 2 || { echo 'QNX GPU fill patch did not apply' >&2; exit 1; }
test "$(grep -c 'PUTLE32(.*LE2HOST32(lcolor)' "$soft")" = 2 || { echo 'QNX GPU polygon patch did not apply' >&2; exit 1; }

touch "$d/.gcc44"
touch "$d/.gcc44-strict-v3"
