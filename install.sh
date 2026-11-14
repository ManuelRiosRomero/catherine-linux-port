#!/bin/bash
# Builds the native Catherine Classic port from YOUR copy of the game and installs it to ~/catherine-port.
#   bash install.sh           (Steam Deck: Desktop Mode, in Konsole. No sudo.)
# Build tools run in a throwaway Ubuntu container (podman or docker), so SteamOS stays untouched. About 45 min on a Deck, plug it in.
#   GAME=/path/to/CatherineClassic   if the game is not found    PREFIX=/some/dir   install elsewhere
#   KEEP=1                           keep the ~8 GB build folder (WORK) after a successful build
set -e -o pipefail
. "$(dirname "$0")/build/paths.sh"
PREFIX=${PREFIX:-$HOME/catherine-port}
WORK=${WORK:-$HOME/.cache/catherine-port-build}

die() { echo; echo "ERROR: $*"; exit 1; }
[ "$(uname -m)" = x86_64 ] || die "this needs an x86-64 machine."
[ -f "$EXE" ] || die "Catherine Classic not found. Install it from Steam, or run: GAME=/path/to/CatherineClassic bash install.sh"
sha256sum "$EXE" | grep -q '^ab4801d2882238fdcb1197dc0191f33543123a65fb3e1e28113603075c5ed7dc' ||
    die "$EXE is not the known Steam version. The port's fixes only match that exact file."
C=$(command -v podman || command -v docker) || die "no podman or docker found (SteamOS 3.5+ has podman). Update your system or install one."
mkdir -p "$WORK"
[ "$(df -Pm "$WORK" | awk 'NR==2 {print $4}')" -ge 10000 ] || [ -x "$WORK/catherine-build/all/catherine" ] || die "needs 10 GB free in $WORK (set WORK=/path/with/space)."
pgrep -f "^$PREFIX/catherine" > /dev/null && die "the game is running: quit it first."

echo "Game:    $GAME"
echo "Install: $PREFIX"
echo "Log:     $WORK/build.log"
echo "Started $(date +%H:%M). Nothing to do until it finishes; progress lines follow."
if [ ! -x "$WORK/catherine-build/all/catherine" ] || [ -n "$REBUILD" ]; then
    # the repo is copied in, so the game-derived files stay in WORK and never land in a git clone
    INH=(systemd-inhibit --what=idle:sleep --who=catherine-port --why=building)   # a sleeping Deck pauses the build
    "${INH[@]}" true 2> /dev/null || { INH=(); echo "Could not block sleep: keep the Deck awake and on its charger."; }
    "${INH[@]}" "$C" run --rm --pull=missing --security-opt label=disable --network=host \
        -v "$REPO":/src:ro -v "$WORK":/work -v "$(dirname "$GAME")":/steam:ro \
        -e HOME=/work -e GAME=/steam/CatherineClassic -e TMPDIR=/tmp \
        docker.io/library/ubuntu:24.04 bash -c '
            set -e
            apt-get update -qq
            DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
                build-essential cmake ninja-build python3 git curl ca-certificates xz-utils libfreetype6 time > /dev/null
            rm -rf /work/repo; mkdir -p /work/repo; cp -r /src/build /src/tools /src/runtime /work/repo/
            bash /work/repo/build/build_all.sh' 2>&1 | tee "$WORK/build.log" | grep --line-buffered '^===\|ERROR\|FAILED\|not the known' ||
        die "the build stopped. The end of $WORK/build.log says why."
fi

mkdir -p "$PREFIX/lib"
cp "$WORK/catherine-build/all/catherine" "$WORK/repo/build/catherine-launch.sh" "$WORK"/repo/build/msdll/rebased/*.dll "$PREFIX/"
cp -P "$WORK"/catherine-deps/dxvk/lib/x86_64-linux-gnu/libdxvk_d3d9.so* "$PREFIX/lib/"
chmod +x "$PREFIX/catherine" "$PREFIX/catherine-launch.sh"
[ -n "$KEEP" ] || "$C" run --rm --security-opt label=disable -v "$WORK":/work docker.io/library/ubuntu:24.04 find /work -mindepth 1 -delete

echo
echo "Done. Last step, once:"
echo "  In Steam: Catherine Classic > gear icon > Properties > General > Launch Options, paste this line:"
echo
echo "      $PREFIX/catherine-launch.sh %command%"
echo
echo "Then press Play. To go back to the normal game, clear that line."
