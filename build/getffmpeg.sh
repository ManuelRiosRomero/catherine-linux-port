#!/bin/bash
# Build a small static FFmpeg 7.1.1 (inside WSL, no sudo) with only what the game's movies need: ASF container,
# MPEG-4 video, WMA v2 audio (all 99 movies, checked with ffprobe), plus swscale/swresample. Linked into the
# binary, so a SteamOS update that swaps the system FFmpeg (7 -> 8 on 2026-10-25: libavformat.so.61 gone, the port
# no longer started) can't break the port again. No nasm in WSL: C code paths only (fine for 720p MPEG-4).
set -e
V=7.1.1
SRC=~/catherine-deps/src; PREFIX=~/catherine-deps/ffmpeg-min
mkdir -p "$SRC" && cd "$SRC"
[ -f ffmpeg-$V.tar.xz ] || curl -sSfL --retry 5 --retry-all-errors -O https://ffmpeg.org/releases/ffmpeg-$V.tar.xz
rm -rf ffmpeg-$V && tar xJf ffmpeg-$V.tar.xz && cd ffmpeg-$V
./configure --prefix="$PREFIX" --enable-static --disable-shared --enable-pic --disable-x86asm \
  --disable-programs --disable-doc --disable-network --disable-autodetect --disable-everything \
  --disable-avdevice --disable-avfilter --enable-protocol=file --enable-demuxer=asf \
  --enable-decoder=mpeg4,wmav2 --enable-parser=mpeg4video > /tmp/ffmpeg_configure.log
make -j8 > /tmp/ffmpeg_build.log 2>&1
make install > /dev/null
ls "$PREFIX"/lib/*.a
