#!/usr/bin/env python3
"""Holes: code flows (fall-through or direct branch) into an address Ghidra never decoded, because a junk function
created a few bytes further on claims the bytes. Prints the holes with a function entry just inside, one per line:
the input list for tools/ghidra/FixHoles.java (which re-checks each one before changing anything).
Usage: find_holes.py <dump.pcode.txt> [lo hi]   (code range, default the exe's 0x401000..0xae0000)"""
import sys

lo, hi = (int(sys.argv[2], 16), int(sys.argv[3], 16)) if len(sys.argv) > 3 else (0x401000, 0xAE0000)
starts, fns, flows = set(), set(), []
for line in open(sys.argv[1], encoding="latin1"):
    if line.startswith("FN "):
        fns.add(int(line.split()[1], 16))
    elif line.startswith("INS "):
        f = line.split(" | ", 1)[0].split()   # INS addr len fallthrough targets
        a = int(f[1], 16)
        starts.add(a)
        for t in f[3:5]:
            if t != "-": flows += [(a, int(x, 16)) for x in t.split(",")]
holes = {t for a, t in flows if t not in starts and lo <= t < hi}
print(f"{len(holes)} holes", file=sys.stderr)
for t in sorted(t for t in holes if any(t < f <= t + 15 for f in fns)): print(f"{t:x}")
