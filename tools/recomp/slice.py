#!/usr/bin/env python3
"""Size a vertical slice: for root functions, list instruction count, direct callees,
indirect calls and imports touched, plus the transitive closure of direct calls.
Usage: slice.py <exe> <pcode.txt> <root hex>... """
import re, subprocess, sys
from collections import defaultdict

BASE = 0x400000

def iat_map(exe):
    """IAT slot VA -> 'dll!name', parsed from `objdump -p`."""
    out = subprocess.run(["objdump", "-p", exe], capture_output=True, text=True).stdout
    m, slot, dll = {}, None, None
    for line in out.splitlines():
        h = re.match(r"\s*[0-9a-f]{8}\s+[0-9a-f]{8}\s+[0-9a-f]{8}\s+[0-9a-f]{8}\s+[0-9a-f]{8}\s+([0-9a-f]{8})$", line)
        if h: slot = BASE + int(h.group(1), 16); continue
        d = re.match(r"\s*DLL Name: (\S+)", line)
        if d: dll = d.group(1); continue
        e = re.match(r"\s*[0-9a-f]+\s+(\d+)\s+(\S+)", line)
        if e and slot is not None and dll:
            m[slot] = f"{dll}!{e.group(2)}"; slot += 4
    return m

def load(path):
    fns, cur = {}, None
    with open(path) as f:
        for line in f:
            t = line[:3]
            if t == "FN ":
                _, a, name, thunk = line.split()
                cur = fns[int(a, 16)] = dict(name=name, ins=0, calls=set(), icalls=0, mem=set())
            elif cur is None: continue
            elif t == "INS": cur["ins"] += 1
            elif t == "OP ":
                p = line.split()
                if p[1] == "CALL": cur["calls"].add(int(p[3][1:].split(":")[0], 16))
                elif p[1] == "CALLIND": cur["icalls"] += 1
                for v in p[2:]:
                    if v[0] == "m" and v.endswith(":4"): cur["mem"].add(int(v[1:-2], 16))
    return fns

def main():
    exe, dump, roots = sys.argv[1], sys.argv[2], [int(r, 16) for r in sys.argv[3:]]
    iat, fns = iat_map(exe), load(dump)
    name = lambda a: fns[a]["name"] if a in fns else hex(a)
    for r in roots:
        f = fns[r]
        imps = sorted(iat[a] for a in f["mem"] if a in iat)
        print(f"\n== {f['name']}  ins={f['ins']}  indirect_calls={f['icalls']}")
        print("  direct callees:", ", ".join(f"{name(c)}({fns[c]['ins'] if c in fns else '?'})" for c in sorted(f["calls"])))
        print("  imports:", ", ".join(imps) or "-")
    # transitive closure of direct calls from all roots
    seen, todo = set(), list(roots)
    while todo:
        a = todo.pop()
        if a in seen or a not in fns: continue
        seen.add(a); todo += fns[a]["calls"]
    imps = defaultdict(int)
    for a in seen:
        for m in fns[a]["mem"]:
            if m in iat: imps[iat[m].split("!")[0]] += 1
    print(f"\nclosure: {len(seen)} functions, {sum(fns[a]['ins'] for a in seen)} instructions, "
          f"{sum(fns[a]['icalls'] for a in seen)} indirect calls")
    print("closure imports by DLL:", dict(sorted(imps.items(), key=lambda x: -x[1])))
    assert len(iat) > 400, "IAT parse broke"  # exe has ~509 imports

if __name__ == "__main__":
    main()
