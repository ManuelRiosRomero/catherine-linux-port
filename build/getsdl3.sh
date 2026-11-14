#!/bin/bash
# Build SDL3 3.2.18 (same version as SteamOS's /usr/lib/libSDL3.so.0.2.18) in WSL without sudo, only so the host can
# compile and link against it. At runtime the Deck's system SDL3 is used (same soname libSDL3.so.0).
set -e
V=3.2.18
SRC=~/catherine-deps/src; PREFIX=~/catherine-deps/sdl3
mkdir -p "$SRC" && cd "$SRC"
[ -f SDL3-$V.tar.gz ] || curl -sSfL --retry 5 --retry-all-errors -O https://github.com/libsdl-org/SDL/releases/download/release-$V/SDL3-$V.tar.gz
rm -rf SDL3-$V && tar xzf SDL3-$V.tar.gz
cmake -S SDL3-$V -B SDL3-$V/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
  -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=ON \
  -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_ALSA=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_IBUS=OFF -DSDL_HIDAPI_LIBUSB=OFF -DSDL_UNIX_CONSOLE_BUILD=ON > /tmp/sdl3_cmake.log
cmake --build SDL3-$V/build -j8 > /tmp/sdl3_build.log
cmake --install SDL3-$V/build > /dev/null
ls "$PREFIX"/lib/libSDL3.so* && ls "$PREFIX"/include/SDL3 | head -3
