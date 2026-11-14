#!/usr/bin/env python3
"""Code pointers stored in data that Ghidra never turned into functions (callbacks, init tables, vtables).
Usage: find_missing.py <exe> <pcode.txt> <out.txt>  -> one hex address per line, to feed CreateFunctions.java"""
import bisect, json, struct, sys

exe, dump, out = sys.argv[1:4]
d = open(exe, "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]
nsec, optsz = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
code, data = [], []
for i in range(nsec):
    s = pe + 24 + optsz + 40 * i
    vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, s + 8)
    (code if struct.unpack_from("<I", d, s + 36)[0] & 0x20000000 else data).append((base + va, rsz, rptr))
idx = {int(k): v for k, v in json.load(open(dump + ".idx.json"))["fns"].items()}
iv = sorted((lo, hi) for f in idx.values() for lo, hi in f["body"])
los = [a for a, _ in iv]
def covered(a):
    i = bisect.bisect_right(los, a) - 1
    return i >= 0 and a <= iv[i][1]
in_code = lambda a: any(va <= a < va + rsz for va, rsz, _ in code)
cands = set()
for va, rsz, rptr in data:
    for o in range(0, rsz - 3, 4):
        a = struct.unpack_from("<I", d, rptr + o)[0]
        if in_code(a) and not covered(a): cands.add(a)
# Microsoft's DLLs merge .rdata into .text: pointer tables (CRT initializers, vtables) sit inside the code section,
# often already mis-decoded as junk instructions. The base relocations list every absolute pointer slot, wherever it is.
def file_off(rva):
    for va, rsz, rptr in code + data:
        if va - base <= rva < va - base + rsz: return rptr + rva - (va - base)
rrva, rsize = struct.unpack_from("<II", d, pe + 24 + 96 + 8 * 5)
p = file_off(rrva) if rsize else None
while p is not None and p < file_off(rrva) + rsize:
    page, blk = struct.unpack_from("<II", d, p)
    for k in range((blk - 8) // 2):
        e = struct.unpack_from("<H", d, p + 8 + 2 * k)[0]
        o = file_off(page + (e & 0xFFF)) if e >> 12 == 3 else None
        a = struct.unpack_from("<I", d, o)[0] if o is not None else 0
        if in_code(a) and not covered(a): cands.add(a)
    p += blk
n_data = len(cands)
# Constants inside instructions too (push offset callback / mov [x], offset fn): Ghidra doesn't follow those either.
with open(dump) as f:
    for line in f:
        if not line.startswith("OP "): continue
        p = line.split()
        if p[1] in ("CALL", "BRANCH", "CBRANCH") and p[3][0] == "m":   # direct call/jump targets nobody disassembled
            a = int(p[3][1:].split(":")[0], 16)
            if in_code(a) and not covered(a): cands.add(a)
        elif " c" in line:
            for v in p[2:]:
                if v[0] == "c" and v.endswith(":4"):
                    a = int(v[1:-2], 16)
                    if in_code(a) and not covered(a): cands.add(a)
open(out, "w").write("".join(f"{a:x}\n" for a in sorted(cands)))
print(f"{n_data} from data + {len(cands) - n_data} from code constants = {len(cands)} uncovered code pointers -> {out}")
