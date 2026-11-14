# Which function body contains each address, and is it an entry? Also: which INS lines reference it.
import json, sys
idx = {int(k): v for k, v in json.load(open("build/catherine.pcode.txt.idx.json"))["fns"].items()}
for t in (int(a, 16) for a in sys.argv[1:]):
    hits = [(a, f["name"]) for a, f in idx.items() if any(lo <= t <= hi for lo, hi in f["body"])]
    print(f"{t:x}: entry={t in idx} inside={[(hex(a), n) for a, n in hits]}")
