#!/bin/bash
# Configure + build dxvk-native's D3D9 (SDL3 WSI) -> ~/catherine-deps/dxvk (libdxvk_d3d9.so + headers).
set -e
D=~/catherine-deps; SRC=$D/src
export PATH=$D/glslang/bin:$D/pkgconf/bin:$PATH PKG_CONFIG_PATH=$D/sdl3/lib/pkgconfig CMAKE_PREFIX_PATH=$D/sdl3   # no pkg-config in WSL: meson finds SDL3 via its CMake files
cd $SRC/dxvk
rm -rf build.native
python3 $SRC/meson-1.6.1/meson.py setup build.native --buildtype=release --prefix=$D/dxvk \
  -Denable_d3d9=true -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false -Denable_dxgi=false \
  -Dnative_sdl3=enabled -Dnative_sdl2=disabled -Dnative_glfw=disabled > /tmp/dxvk_setup.log 2>&1 || { tail -25 /tmp/dxvk_setup.log; exit 1; }
time ninja -C build.native > /tmp/dxvk_build.log 2>&1 || { grep -m5 -B2 -A8 "error" /tmp/dxvk_build.log; exit 1; }
ninja -C build.native install > /dev/null
find $D/dxvk -name "*.so*" | head; ls $D/dxvk/include 2>/dev/null | head
