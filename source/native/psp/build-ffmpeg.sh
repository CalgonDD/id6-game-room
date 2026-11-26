#!/bin/sh
set -eu
p=arm-unknown-nto-qnx6.5.0eabi
build_dir=${FFMPEG_BUILD_DIR:-/build/ffmpeg}
prefix=${FFMPEG_PREFIX:-/build/ffmpeg-prefix}
mkdir -p "$build_dir"
cd "$build_dir"
/src/ppsspp/ffmpeg/configure --target-os=qnx --enable-cross-compile \
 --arch=arm --cpu=cortex-a15 --cc=$p-gcc --cross-prefix=$p- \
 --prefix="$prefix" --extra-cflags='-O2 -fPIC -marm -mfloat-abi=softfp -mfpu=neon -mno-unaligned-access -include /audit/qnx_c_types.h' \
 --disable-shared --enable-static --enable-pic --disable-asm --disable-fast-unaligned --disable-everything --enable-memalign-hack \
 --disable-programs --disable-doc --disable-network --disable-avdevice \
 --disable-avfilter --disable-postproc --disable-hwaccels --disable-zlib \
 --enable-decoder=h264,mpeg4,mpeg2video,mjpeg,mjpegb,aac,aac_latm,atrac3,atrac3p,mp3,pcm_s16le,pcm_s8 \
 --enable-demuxer=h264,m4v,mpegvideo,mpegps,mp3,avi,aac,pmp,oma,pcm_s16le,pcm_s8,wav \
 --enable-parser=h264,mpeg4video,mpegaudio,mpegvideo,aac,aac_latm
make -j4
make install
