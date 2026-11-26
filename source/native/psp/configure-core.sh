#!/bin/sh
set -eu
build_dir=${PSP_BUILD_DIR:-/build/ppsspp}
ffmpeg_prefix=${FFMPEG_PREFIX:-/build/ffmpeg-prefix}
cmake -S /src/ppsspp -B "$build_dir" -G Ninja \
 -DCMAKE_TOOLCHAIN_FILE=/opt/qnx-cross/qnx-armv7.cmake \
 -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
 -DCMAKE_C_FLAGS='-D_QNX_SOURCE -marm -mfloat-abi=softfp -mfpu=neon -march=armv7-a -mno-unaligned-access -include /audit/qnx_c_types.h' \
 -DCMAKE_CXX_FLAGS='-D_QNX_SOURCE -marm -mfloat-abi=softfp -mfpu=neon -march=armv7-a -mno-unaligned-access -include /audit/qnx_cpp_compat.h' \
 -DCMAKE_SHARED_LINKER_FLAGS='-static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,--version-script=/audit/libretro-exports.map -Wl,--no-undefined -lsocket -lm' \
 -DLIBRETRO=ON -DUSING_GLES2=ON -DUSING_EGL=ON -DARM_NO_VULKAN=ON \
 -DVULKAN=OFF -DOPENXR=OFF -DUSING_X11_VULKAN=OFF -DUSE_WAYLAND_WSI=OFF \
 -DCMAKE_DISABLE_FIND_PACKAGE_X11=TRUE -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE \
 -DUNITTEST=OFF -DHEADLESS=OFF -DUSE_DISCORD=OFF -DUSE_MINIUPNPC=OFF \
 -DUSE_ARMIPS=OFF -DUSE_SYSTEM_SNAPPY=OFF -DUSE_SYSTEM_LIBZIP=OFF \
 -DUSE_SYSTEM_LIBPNG=OFF -DUSE_SYSTEM_ZSTD=OFF -DUSE_FFMPEG=ON \
 -DFFMPEG_DIR="$ffmpeg_prefix"
# Ninja otherwise notices the always-generated version output only on the
# following build. Seed it before compiling so a clean build has the pin.
cmake -DOUTPUT_DIR="$build_dir" -DSOURCE_DIR=/src/ppsspp -P /src/ppsspp/git-version.cmake
