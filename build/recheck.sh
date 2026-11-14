#!/bin/bash
# Re-lift + re-check the movie closure and the random sample (run inside WSL from repo root).
run() {
  name=$1; shift
  python3 tools/recomp/lift.py build/catherine.pcode.txt gen/$name "$@" &&
  gcc -O1 -fno-strict-aliasing -w -fPIC -shared -Itools/recomp/rt -o gen/$name/lifted.so gen/$name/lifted_*.c tools/recomp/rt/rt.c -lm &&
  PYTHONPATH=~/catherine-py python3 tools/recomp/check.py "$EXE" gen/$name 10 > gen/$name/check.txt 2>&1
  echo "== $name"; tail -1 gen/$name/check.txt; grep -E "FAIL|CRASH" gen/$name/check.txt | cut -c1-300
}
run movie 6a96d0 6aa140 6a9a00 6a8d70
run sample sample:300
