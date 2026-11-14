#!/usr/bin/env python3
"""Safety check after function discovery: every function of the reference export must still exist with all of its
instructions. Usage: compare_exports.py <reference.pcode.txt> <new.pcode.txt>   (exit 1 if any function was damaged)"""
import sys

def load(path):
    fns, cur = {}, None
    with open(path) as f:
        for line in f:
            if line.startswith("FN "):
                cur = fns.setdefault(line.split()[1], set())
            elif line.startswith("INS ") and cur is not None:
                cur.add(line.split(" ", 2)[1])
    return fns

ref, new = load(sys.argv[1]), load(sys.argv[2])
lost = {a: ins - new.get(a, set()) for a, ins in ref.items() if ins - new.get(a, set())}
gone = [a for a in ref if a not in new]
print(f"reference {len(ref)} fns, new {len(new)} fns (+{len(new) - len(ref)}); "
      f"functions removed: {len(gone)}, functions that lost instructions: {len(lost)}")
for a in list(lost)[:10]: print(f"  {a}: lost {len(lost[a])} of {len(ref[a])} instructions, e.g. {sorted(lost[a])[:3]}")
sys.exit(1 if lost or gone else 0)
