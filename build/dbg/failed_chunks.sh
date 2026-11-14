#!/bin/bash
cd ~/catherine-build/all
for f in lifted_*.c; do
  [ -f "$f.o" ] && continue
  echo "== $f: $(wc -l < "$f") lines, $(du -h "$f" | cut -f1)"; head -c 600 "$f.err"
done
