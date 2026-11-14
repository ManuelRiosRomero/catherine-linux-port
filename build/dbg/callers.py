import json, sys
idx = {int(k): v for k, v in json.load(open("build/catherine.pcode.txt.idx.json"))["fns"].items()}
targets = [int(a, 16) for a in sys.argv[1:]]
for t in targets:
    print(f"{t:x} <-", ", ".join(f"{a:x}" for a, f in sorted(idx.items()) if t in f["calls"] or t in f["br"]) or "(no direct callers)")
