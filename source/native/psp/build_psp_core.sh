#!/bin/sh
# Build the PSP emulator core (ppsspp_libretro.so) for the head unit, inside the QNX 6.5 GCC 8.5 toolchain
# image. See BUILD-PSP-CORE.md. Output: source/native/out/ppsspp_libretro.so (stripped), the file the
# installer ships.
#
#   sh third_party/fetch_third_party.sh --psp        # PPSSPP v1.17.1 + the Game Room QNX patch
#   sh source/native/psp/build_psp_core.sh           # QNX_TOOLCHAIN_IMAGE=... to pick another image tag
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
src=${PPSSPP_SRC:-$repo/third_party/ppsspp}
out=$repo/source/native/out
image=${QNX_TOOLCHAIN_IMAGE:-qnx65-armv7-toolchain:8.5}
test -f "$src/Common/QNXJitCache.h" || { echo "Patched PPSSPP not found: run sh third_party/fetch_third_party.sh --psp" >&2; exit 1; }
mkdir -p "$out"
# Git Bash on Windows: Docker needs Windows paths (C:/...), not /c/...
if (cd "$here" && pwd -W) >/dev/null 2>&1; then
    here=$(cd "$here" && pwd -W); src=$(cd "$src" && pwd -W); out=$(cd "$out" && pwd -W)
fi
# The source must be mounted at /src/ppsspp and these recipes at /audit: the scripts use those paths, and the
# same paths keep the output byte-identical to the released core.
MSYS_NO_PATHCONV=1 docker run --rm -v "$src":/src/ppsspp -v "$here":/audit:ro -v "$out":/output "$image" sh -c '
    set -eu
    sh /audit/build-ffmpeg.sh > /tmp/ffmpeg.log 2>&1 || { tail -40 /tmp/ffmpeg.log; exit 1; }
    sh /audit/configure-core.sh
    cmake --build /build/ppsspp --target ppsspp_libretro -j"$(nproc)"
    arm-unknown-nto-qnx6.5.0eabi-strip --strip-unneeded -o /output/ppsspp_libretro.so /build/ppsspp/lib/ppsspp_libretro.so
    sha256sum /output/ppsspp_libretro.so'
