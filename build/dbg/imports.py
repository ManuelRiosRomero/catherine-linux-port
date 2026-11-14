# Imports grouped by DLL (name list), from objdump -p.
import re, subprocess, sys, collections
out = subprocess.run(["objdump", "-p", sys.argv[1]], capture_output=True, text=True).stdout
by, dll = collections.OrderedDict(), None
for line in out.splitlines():
    d = re.match(r"\s*DLL Name: (\S+)", line)
    if d: dll = d.group(1); by[dll] = []; continue
    e = re.match(r"\s*[0-9a-f]+\s+(\d+)\s+(\S+)", line)
    if e and dll: by[dll].append(e.group(2))
for k, v in sorted(by.items(), key=lambda x: -len(x[1])): print(f"{len(v):4d} {k}")
print(sum(map(len, by.values())), "total")
if len(sys.argv) > 2:
    for k in sys.argv[2:]: print(k, ":", " ".join(by.get(k, [])))
