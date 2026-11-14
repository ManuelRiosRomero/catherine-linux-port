#!/bin/bash
. "$(dirname "$0")/../build/paths.sh"
# Build the movie backend + self-test in WSL and compare every frame and all audio against the ffmpeg CLI.
# Usage (repo root, inside WSL): bash runtime/movie_test.sh [movie files...]   (default: the first cutscene)
set -e
DEPS=~/catherine-deps/usr/include/x86_64-linux-gnu   # ffmpeg headers, see build/getdeps.sh
LIB=/usr/lib/x86_64-linux-gnu
MOV="$GAME/data/movie"
mkdir -p gen/movie_test
gcc -O2 -Wall -I"$DEPS" -o gen/movie_test/movie_test runtime/movie.c runtime/movie_test.c \
    $LIB/libavformat.so.60 $LIB/libavcodec.so.60 $LIB/libavutil.so.58 $LIB/libswscale.so.7 $LIB/libswresample.so.4
[ $# -gt 0 ] || set -- "$MOV/005_00.wmv"
fail=0
for f in "$@"; do
  out=gen/movie_test/$(basename "$f").txt
  gen/movie_test/movie_test "$f" > "$out"
  ref_v=$(ffmpeg -v error -i "$f" -map 0:v -fps_mode passthrough -pix_fmt bgra -f framemd5 - | grep -v '^#' | awk -F', *' '{print "V " $6}')
  ref_a=$(ffmpeg -v error -i "$f" -map 0:a -f f32le -ac 2 -ar 48000 - | md5sum | awk '{print "A " $1}')
  if diff <(printf '%s\n%s\n' "$ref_v" "$ref_a") "$out" > /dev/null; then
    echo "MATCH $(basename "$f")"
  else
    echo "DIFF  $(basename "$f"): $(diff <(printf '%s\n%s\n' "$ref_v" "$ref_a") "$out" | grep -c '^[<>]') differing lines"
    fail=1
  fi
done
exit $fail
