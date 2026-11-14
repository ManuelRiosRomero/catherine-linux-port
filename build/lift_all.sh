#!/bin/bash
# Lift the whole exe and compile it in parallel inside WSL's own disk (fast). Prints timings and sizes.
set -e
. "$(dirname "$0")/paths.sh"
OUT=~/catherine-build/all
OPT=${OPT:--O1}
mkdir -p $OUT
cd $REPO
. build/status.sh
status "translating game code (lift.py, ~3 min)"
# One dump for every image (exe + translated Microsoft DLLs): addresses don't overlap, so one index and one fn_table.
DUMPS="build/catherine.pcode.txt $(ls build/d3dx9.pcode.txt build/d3dcompiler.pcode.txt 2>/dev/null)"
for d in $DUMPS; do [ $d -nt build/all.pcode.txt ] && { cat $DUMPS > build/all.pcode.txt; break; }; done
/usr/bin/time -f "lift: %e s, %M KB peak" python3 tools/recomp/lift.py build/all.pcode.txt $OUT all
du -sh $OUT; ls $OUT/lifted_*.c | wc -l; cat $OUT/lifted_*.c | wc -l
echo "unimplemented ops emitted (kind: count):"
grep -oh 'rt_unimpl("[^"]*"' $OUT/lifted_*.c | sort | uniq -c | sort -rn | head -20
cp tools/recomp/rt/rt.h tools/recomp/rt/rt.c $OUT/
cd $OUT
rm -f *.o *.err   # chunk numbering shifts between lifts; stale objects would duplicate functions
start=$(date +%s)
status_every 5 'echo "compiling translated code: $(ls lifted_*.c.o 2>/dev/null | wc -l) of $(ls lifted_*.c | wc -l) chunks (~6 min total)"' &
W=$!
trap 'rc=$?; kill $W 2>/dev/null || true; exit $rc' EXIT   # the refresher never outlives this script (keep the script's own exit code; set -e also applies inside the trap, so the kill of an already stopped refresher must not fail: it made --relift stop before linking)
# Chunks over 10 MB of C (the biggest are 35-40 MB) take ~7 GB of RAM each to compile: two at once filled WSL's 16 GB
# and WSL killed its whole VM (the build died with exit 15). They go one at a time, the rest in parallel.
CC="gcc $OPT -mpopcnt -fno-stack-protector -fno-strict-aliasing -w -fPIC -c"
# Over 15 MB at -O0: lifted_130.c holds d3dx9's 0x508148d, one 257k-line C function (load-time code): 15.4 GB at -O1, 4 GB at -O0.
for f in $(find . -maxdepth 1 -name 'lifted_*.c' -size +10M | sort); do
    if [ $(stat -c %s $f) -gt 15000000 ]; then ${CC/$OPT/-O0} $f -o $f.o 2>$f.err || echo FAILED $f; else $CC $f -o $f.o 2>$f.err || echo FAILED $f; fi
done
find . -maxdepth 1 \( -name 'lifted_*.c' ! -size +10M -o -name rt.c \) | xargs -P ${JOBS:-4} -I{} sh -c "$CC {} -o {}.o 2>{}.err || echo FAILED {}"
kill $W 2>/dev/null || true
echo "compile ($OPT, ${JOBS:-4} jobs): $(( $(date +%s) - start )) s"
cat *.err | head -20
gcc -shared -o lifted.so *.o -lm -lpthread && ls -la lifted.so
