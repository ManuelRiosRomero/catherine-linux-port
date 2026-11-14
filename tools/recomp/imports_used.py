#!/usr/bin/env python3
"""Which functions touch each import (IAT slot)? Writes JSON {"dll!name": [fn hex, ...]} and prints a summary.
Usage: imports_used.py <exe> <pcode.txt> <out.json> [name-substring ...]"""
import json, sys
from slice import iat_map

def main():
    exe, dump, out = sys.argv[1:4]
    iat, users, fn = iat_map(exe), {}, None
    with open(dump) as f:
        for line in f:
            if line.startswith("FN "): fn = line.split()[1]
            elif line.startswith("OP ") and " m" in line:
                for v in line.split()[2:]:
                    if v[0] == "m" and v.endswith(":4"):
                        a = int(v[1:-2], 16)
                        if a in iat: users.setdefault(iat[a], set()).add(fn)
    res = {k: sorted(v) for k, v in sorted(users.items())}
    json.dump(res, open(out, "w"), indent=0)
    print(f"{len(res)} of {len(iat)} imports referenced by code")
    for k in sys.argv[4:]:
        for name, fns in res.items():
            if k.lower() in name.lower(): print(f"{name}: {len(fns)} fns: {' '.join(fns[:12])}")

if __name__ == "__main__":
    main()
