#!/bin/bash
# Steam launcher for the native port (runs ON the Deck, copied to ~/catherine-port/ by install.sh).
# In Steam: Catherine Classic -> Properties -> Launch Options:
#     /home/deck/catherine-port/catherine-launch.sh %command%
# Steam then starts this script instead of Proton (%command% is Proton's command line; we only take the game folder from
# it). Keeping Catherine's own Steam entry matters: Steam sets SteamAppId=893180 (ownership, achievements, play time) and
# loads the game's Steam Input layout, which the port reads its buttons from (a "non-Steam game" shortcut would get
# neither). Remove the launch option to go back to Proton.
P=$(dirname "$(readlink -f "$0")")
# the game folder: from the Catherine.exe path inside %command%, else the default install
GAME=~/.local/share/Steam/steamapps/common/CatherineClassic
for a in "$@"; do case $a in */Catherine.exe) GAME=$(dirname "$a") ;; esac; done
cd "$GAME" || exit 1
# one log per run, the previous one kept: a crash report is ~/catherine-port/last_run.log ("catherine:" lines at the end)
[ -f $P/last_run.log ] && mv -f $P/last_run.log $P/last_run.prev.log
export DXVK_WSI_DRIVER=SDL3
# X11 needs the session's key file; without it SDL falls back to native Wayland, which KWin kills now and then (user32.c)
[ -n "$DISPLAY" ] && [ -z "$XAUTHORITY" ] && export XAUTHORITY=$(ls -t /run/user/$(id -u)/xauth_* 2>/dev/null | head -1)
# Steam Cloud: use the save in Proton's prefix, the file Steam syncs with the PC (host path.c maps only SaveGame.bin
# there). Only if it exists: a missing one would make a fresh save that fights the cloud copy. A copy of it is kept
# before every launch (last 30) in save-backups/. Delete these lines to go back to the port's own saves.
CLOUD=${STEAM_COMPAT_DATA_PATH:-$HOME/.local/share/Steam/steamapps/compatdata/893180}/pfx/drive_c/users/steamuser/AppData/Roaming/TheEccentricApe/Catherine/SaveGame.bin
if [ -f "$CLOUD" ]; then
    mkdir -p $P/save-backups && cp -p "$CLOUD" $P/save-backups/SaveGame-$(date +%F_%H%M%S).bin
    ls -t $P/save-backups/SaveGame-*.bin | tail -n +31 | xargs -r rm --
    export CATH_CLOUD_SAVE=$CLOUD
fi
# test extras for a Steam launch (e.g. CATH_INPUT_FILE for scripted presses); delete the file after the test
[ -f $P/launch.env ] && . $P/launch.env
# reference runs: CATH_PROTON=1 in launch.env hands Steam's own command back, so the Windows game runs under Proton
[ -n "$CATH_PROTON" ] && { echo "[launch] $(date '+%F %T') CATH_PROTON: running Steam's Proton command" > $P/last_run.log; exec "$@"; }
{ echo "[launch] $(date '+%F %T') args: $*"; env | grep -E '^(DISPLAY|WAYLAND_DISPLAY|XAUTHORITY|XDG_RUNTIME_DIR|SteamAppId|SDL_VIDEO_DRIVER|STEAM_COMPAT_DATA_PATH|CATH_CLOUD_SAVE|CATH_ASPECT)='; } > $P/last_run.log
exec $P/catherine ./Catherine.exe >> $P/last_run.log 2>&1
