#!/bin/bash
# Unicorn check of the d3dx9 functions that use the lifter's MMX/SSE user ops (rt.h: paddsw ... rsqrtss); ~2 min.
# Expected: 0 CRASH; FAILs only in NaN bit patterns (51baac9) and 3DNow! callees Unicorn cannot run (skips).
. "$(dirname "$0")/paths.sh"; cd "$REPO"
. build/status.sh; job_start; status "Unicorn check of the new MMX/SSE ops"
G=gen/d3dx9_simd; rm -rf $G
timeout 600 python3 tools/recomp/lift.py build/d3dx9.pcode.txt $G 515b4f0 5162c10 5163990 50dcd6f 51bc8fd 51ba380 51b16c0 51baac9 > /dev/null &&
timeout 300 gcc -O1 -fno-strict-aliasing -w -fPIC -shared -Itools/recomp/rt -o $G/lifted.so $G/lifted_*.c tools/recomp/rt/rt.c -lm &&
PYTHONPATH=~/catherine-py timeout 600 python3 tools/recomp/check.py build/msdll/rebased/d3dx9_43.dll $G 30 515b4f0 5162c10 5163990 50dcd6f 51bc8fd 51ba380 51b16c0 51baac9 > $G/check.txt 2>&1
tail -3 $G/check.txt; grep -E "FAIL|CRASH|unimpl" $G/check.txt | cut -c1-260 | head -30
status "Unicorn check of MMX/SSE ops done (idle)"
