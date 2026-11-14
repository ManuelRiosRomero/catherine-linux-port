#!/bin/bash
# Quick regression: lift the movie closure, run the Unicorn checker (10 trials per function).
. "$(dirname "$0")/paths.sh"; cd "$REPO"
/usr/bin/time -f "lift+index: %e s" python3 tools/recomp/lift.py build/catherine.pcode.txt gen/movie 6a96d0 6aa140 6a9a00 6a8d70 &&
gcc -O1 -fno-strict-aliasing -w -fPIC -shared -Itools/recomp/rt -o gen/movie/lifted.so gen/movie/lifted_*.c tools/recomp/rt/rt.c -lm &&
PYTHONPATH=~/catherine-py python3 tools/recomp/check.py "$EXE" gen/movie 10 | tail -4
