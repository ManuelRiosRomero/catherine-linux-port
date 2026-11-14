#!/bin/bash
# Build the native host (runtime/host) against the lifted objects in ~/catherine-build/all, then optionally run it.
# Usage (inside WSL): bash build/host_build.sh [--relift] [--run]
set -e
. "$(dirname "$0")/paths.sh"
OUT=~/catherine-build/all
DEPS=~/catherine-deps
SDL=$DEPS/sdl3                    # build/getsdl3.sh; the Deck uses its own /usr/lib/libSDL3.so.0 at runtime
DXVK=$DEPS/dxvk                   # build/getdxvk.sh + build/builddxvk.sh; copied to the Deck as ~/catherine-port/lib
FF=$DEPS/ffmpeg-min               # static FFmpeg 7.1.1, movie codecs only (build/getffmpeg.sh): no system FFmpeg needed
cd $REPO
. build/status.sh
job_start
exec > >(tee -a "$FEED_FILE") 2>&1   # live feed for the board pane
trap '[ $? = 0 ] || status "BUILD FAILED (see /tmp/host_build.log in WSL)"' EXIT
if [ "$1" = "--relift" ]; then shift; bash build/lift_all.sh; fi
status "building the runtime and linking (~1 min)"
cp tools/recomp/rt/rt.h tools/recomp/rt/rt.c $OUT/
gcc -O1 -mpopcnt -fno-strict-aliasing -w -c $OUT/rt.c -o $OUT/rt.c.o
python3 tools/recomp/gen_d3d9.py $DXVK/include/dxvk/d3d9.h $OUT/d3d9_gen.cpp > /dev/null
H=$OUT/host; mkdir -p $H
gcc -O2 -Wall -I$FF/include -c runtime/movie.c -o $H/movie.c.o
for f in runtime/host/*.c; do gcc -O1 -g -Wall -Wno-unused-function -I$OUT -I$SDL/include -I$DEPS/usr/include/freetype2 -c $f -o $H/$(basename $f).o; done
for f in runtime/host/*.cpp $OUT/d3d9_gen.cpp; do
  g++ -O1 -g -std=c++17 -w -I$OUT -Iruntime/host -I$DXVK/include/dxvk -I$SDL/include -c $f -o $H/$(basename $f).o
done
g++ -o $OUT/catherine $H/*.o $OUT/*.o -L$SDL/lib -L$DXVK/lib/x86_64-linux-gnu \
    -Wl,-rpath,'$ORIGIN/lib' -Wl,-rpath,$SDL/lib -Wl,-rpath,$DXVK/lib/x86_64-linux-gnu -ldxvk_d3d9 -lSDL3 -l:libfreetype.so.6     $FF/lib/libavformat.a $FF/lib/libavcodec.a $FF/lib/libswscale.a $FF/lib/libswresample.a $FF/lib/libavutil.a -Wl,--allow-shlib-undefined -lm -lpthread
echo "built $OUT/catherine"
status "built (idle)"
if [ "$1" = "--run" ]; then cd "$GAME" && $OUT/catherine ./Catherine.exe; fi
