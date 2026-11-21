# Sourced by the build scripts: REPO = this clone; GAME = the Catherine Classic folder (set GAME= to override), EXE in it.
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export PATH="$HOME/catherine-deps/usr/bin:$PATH"   # small tools unpacked by getdeps.sh
if [ -z "$GAME" ]; then   # every command here must succeed: callers run with set -e
    { echo "$HOME/.local/share/Steam"; echo "/mnt/c/Program Files (x86)/Steam"
      sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"$/\1/p' "$HOME/.local/share/Steam/steamapps/libraryfolders.vdf" 2>/dev/null || true
    } > "${TMPDIR:-/tmp}/catherine-libs.$$"
    while IFS= read -r lib; do
        if [ -f "$lib/steamapps/common/CatherineClassic/Catherine.exe" ]; then GAME="$lib/steamapps/common/CatherineClassic"; break; fi
    done < "${TMPDIR:-/tmp}/catherine-libs.$$"
    rm -f "${TMPDIR:-/tmp}/catherine-libs.$$"
fi
EXE="$GAME/Catherine.exe"
