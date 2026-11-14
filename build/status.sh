# What the build/test scripts are doing, for Claude Code's status line (~/.claude/statusline-command.sh) and the
# port-board pane's "Now" box. Three files in build/tmp:
#   status.txt  one line "HH:MM step"            status "text"
#   why.txt     why we are waiting on this job   set from $WHY by the job's first script (Claude passes it)
#   feed.log    live output of the job           written by the scripts (tee), the pane shows its last lines
# Source it, then call status / status_every.
TMP_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/tmp
STATUS_FILE=$TMP_DIR/status.txt
FEED_FILE=$TMP_DIR/feed.log
mkdir -p $TMP_DIR
status() { echo "$(date +%H:%M) $*" > "$STATUS_FILE"; }
# status_every N "command printing the text" &  ->  refreshes the line every N s until killed
status_every() { while true; do status "$(eval "$2")"; sleep "$1"; done; }
# a new job (only the outermost script counts): record why ("" when not given) and start a fresh feed
job_start() { [ -n "$JOB_STARTED" ] && return; echo "$WHY" > $TMP_DIR/why.txt; : > "$FEED_FILE"; export JOB_STARTED=1; }
