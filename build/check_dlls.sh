#!/bin/bash
# Unicorn check of a random function sample from each translated Microsoft DLL (run inside WSL from the repo root).
# Usage: bash build/check_dlls.sh [N functions per DLL, default 150]
. "$(dirname "$0")/paths.sh"; cd "$REPO"
N=${1:-150}
for pair in d3dx9:d3dx9_43.dll d3dcompiler:D3DCompiler_43.dll; do
  name=${pair%%:*}; dll=build/msdll/rebased/${pair#*:}
  python3 tools/recomp/lift.py build/$name.pcode.txt gen/${name}_sample sample:$N > /dev/null &&
  gcc -O1 -fno-strict-aliasing -w -fPIC -shared -Itools/recomp/rt -o gen/${name}_sample/lifted.so gen/${name}_sample/lifted_*.c tools/recomp/rt/rt.c -lm &&
  PYTHONPATH=~/catherine-py python3 tools/recomp/check.py $dll gen/${name}_sample 10 > gen/${name}_sample/check.txt 2>&1
  echo "== $name"; tail -1 gen/${name}_sample/check.txt; grep -E "FAIL|CRASH" gen/${name}_sample/check.txt | cut -c1-300 | head -20
done
