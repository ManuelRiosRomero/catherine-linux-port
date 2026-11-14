#!/bin/bash
# Everything from a clean machine to ~/catherine-build/all/catherine, from the game's own files: tools, libraries, the
# Microsoft DLLs, Ghidra analysis, translation, compile. Needs Ubuntu 24.04's packages (install.sh runs it in a container).
set -e
. "$(dirname "$0")/paths.sh"; cd "$REPO"
sha256sum "$EXE" | grep -q '^ab4801d2882238fdcb1197dc0191f33543123a65fb3e1e28113603075c5ed7dc' ||
    { echo "This Catherine.exe is not the known Steam version: the port's fixes only match that exact file."; exit 1; }
D=~/catherine-deps
step() { echo; echo "=== $(date +%H:%M) $*"; }
step "1/6 small tools"
[ -x $D/usr/bin/cabextract ] || bash build/getdeps.sh
step "2/6 libraries (SDL3, DXVK, FFmpeg)"
[ -d $D/sdl3 ] || bash build/getsdl3.sh
[ -x $D/pkgconf/bin/pkg-config ] || bash build/getpkgconf.sh
[ -d $D/dxvk ] || { bash build/getdxvk.sh; bash build/builddxvk.sh; }
[ -d $D/ffmpeg-min ] || bash build/getffmpeg.sh
step "3/6 Ghidra"
bash build/getghidra.sh
step "4/6 Microsoft DLLs"
bash build/getdlls.sh
step "5/6 analysing the game (the long part)"
bash build/ghidra_all.sh
step "6/6 translating and compiling"
bash build/host_build.sh --relift
[ -x ~/catherine-build/all/catherine ]
echo; echo "=== $(date +%H:%M) build finished"
