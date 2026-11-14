#!/bin/bash
# Build dxvk-native (D3D9 -> Vulkan, SDL3 window system) in WSL without sudo.
# Tools: meson runs from its source release; glslang is built from source (no SPIRV-Tools needed).
set -e
D=~/catherine-deps; SRC=$D/src; mkdir -p "$SRC" && cd "$SRC"
MESON_V=1.6.1; GLSLANG_V=15.1.0; DXVK_V=v2.7.1
[ -d meson-$MESON_V ] || { curl -sSfL --retry 5 --retry-all-errors https://github.com/mesonbuild/meson/releases/download/$MESON_V/meson-$MESON_V.tar.gz | tar xz; }
if [ ! -x $D/glslang/bin/glslang ]; then
  rm -rf glslang && git clone -q --depth 1 --branch $GLSLANG_V https://github.com/KhronosGroup/glslang.git
  cmake -S glslang -B glslang/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$D/glslang -DENABLE_OPT=OFF \
        -DGLSLANG_TESTS=OFF -DBUILD_TESTING=OFF -DENABLE_GLSLANG_BINARIES=ON > /tmp/glslang_cmake.log
  cmake --build glslang/build -j8 > /tmp/glslang_build.log && cmake --install glslang/build > /dev/null
fi
[ -d dxvk ] || git clone -q --depth 1 --branch $DXVK_V --recurse-submodules --shallow-submodules https://github.com/doitsujin/dxvk.git
echo "meson: $(python3 meson-$MESON_V/meson.py --version)  glslang: $($D/glslang/bin/glslang --version | head -1)"
grep -n "option(" dxvk/meson_options.txt
