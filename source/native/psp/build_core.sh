#!/bin/sh
set -eu
test ! -e /src/ppsspp || { echo 'Use a fresh build container' >&2; exit 1; }
mkdir -p /src /build /output/psp-symbols
tar -xf /inputs/pinned-source.tar -C /src
python3 /audit/port_source.py
export FFMPEG_BUILD_DIR=/build/ffmpeg-clean
export FFMPEG_PREFIX=/build/ffmpeg-clean-prefix
export PSP_BUILD_DIR=/build/ppsspp-clean
sh /audit/build-ffmpeg.sh
sh /audit/configure-core.sh
cmake --build "$PSP_BUILD_DIR" --target ppsspp_libretro -j4
p=arm-unknown-nto-qnx6.5.0eabi
cp "$PSP_BUILD_DIR/lib/ppsspp_libretro.so" /output/psp-symbols/ppsspp_libretro.so.unstripped
cp "$PSP_BUILD_DIR/lib/ppsspp_libretro.so" /build/ppsspp_libretro-release.so
$p-strip --strip-unneeded /build/ppsspp_libretro-release.so
cp /build/ppsspp_libretro-release.so /output/ppsspp_libretro.so
$p-readelf -h -d -A -V /build/ppsspp_libretro-release.so > /output/psp-symbols/core-elf.txt
sha256sum /output/ppsspp_libretro.so
