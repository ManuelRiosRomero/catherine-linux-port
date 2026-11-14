# Sourced by the build scripts: REPO = this clone; GAME = the Catherine Classic folder (set GAME= to override), EXE in it.
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export PATH="$HOME/catherine-deps/usr/bin:$PATH"   # small tools unpacked by getdeps.sh
if [ -z "$GAME" ]; then
    { echo "$HOME/.local/share/Steam"; echo "/mnt/c/Program Files (x86)/Steam"
      sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"$/\1/p' "$HOME/.local/share/Steam/steamapps/libraryfolders.vdf" 2>/dev/null; } > "${TMPDIR:-/tmp}/catherine-libs.$$"
    while IFS= read -r lib; do
        [ -f "$lib/steamapps/common/CatherineClassic/Catherine.exe" ] && { GAME="$lib/steamapps/common/CatherineClassic"; break; }
    done < "${TMPDIR:-/tmp}/catherine-libs.$$"
    rm -f "${TMPDIR:-/tmp}/catherine-libs.$$"
fi
EXE="$GAME/Catherine.exe"
