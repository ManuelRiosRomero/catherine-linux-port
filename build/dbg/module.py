import json, sys
idx = {int(k): v for k, v in json.load(open("build/catherine.pcode.txt.idx.json"))["fns"].items()}
lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
inside = sorted(a for a in idx if lo <= a < hi)
for a in inside:
    f = idx[a]
    size = sum(b - x + 1 for x, b in f["body"])
    ext = sorted(c for c, g in idx.items() if not lo <= c < hi and (a in g["calls"] or a in g["br"]))
    out = sorted(t for t in f["calls"] if not lo <= t < hi)
    print(f"{a:x} {size:5d}B  ext-callers: {' '.join(f'{c:x}' for c in ext) or '-':40s} calls-out: {' '.join(f'{t:x}' for t in out)}")
