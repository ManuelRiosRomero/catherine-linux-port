#!/usr/bin/env python3
"""Find switch jump tables Ghidra resolved only partly: `JMP dword ptr [REG*0x4 + TABLE]` guarded by `CMP REG,N`
(+ JA/JNBE) a few instructions earlier. Reads the N+1 table entries from the exe and lists targets Ghidra missed.
Usage: jumptables.py <exe> <pcode.txt> <out.txt>   -> lines "jmp_addr target1 target2 ..." for FixJumpTables.java"""
import re, struct, sys

exe, dump, out = sys.argv[1:4]
d = open(exe, "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]
nsec, optsz = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
secs = []
for i in range(nsec):
    s = pe + 24 + optsz + 40 * i
    vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, s + 8)
    secs.append((base + va, rsz, rptr, struct.unpack_from("<I", d, s + 36)[0]))
def u32(a):
    for va, rsz, rptr, _ in secs:
        if va <= a < va + rsz - 3: return struct.unpack_from("<I", d, rptr + a - va)[0]
def u8(a):
    for va, rsz, rptr, _ in secs:
        if va <= a < va + rsz: return d[rptr + a - va]
in_code = lambda a: any(va <= a < va + rsz and ch & 0x20000000 for va, rsz, _, ch in secs)
# Base relocation slots: every jump-table entry is one, so a table with no bounds check = the run of relocated
# words starting at the table (merging two adjacent tables only adds real case addresses).
relocs = set()
rrva, rsize = struct.unpack_from("<II", d, pe + 24 + 96 + 8 * 5)
fo = next((rptr + rrva - (va - base) for va, rsz, rptr, _ in secs if va - base <= rrva < va - base + rsz), None) if rsize else None
o = fo
while o is not None and o < fo + rsize:
    page, blk = struct.unpack_from("<II", d, o)
    if blk < 8: break
    for k in range((blk - 8) // 2):
        e = struct.unpack_from("<H", d, o + 8 + 2 * k)[0]
        if e >> 12 == 3: relocs.add(base + page + (e & 0xFFF))
    o += blk

jmp_re = re.compile(r"JMP dword ptr \[(\w+)\*0x4 \+ 0x([0-9a-f]+)\]")
cmp_re = re.compile(r"CMP (\w+),0x([0-9a-f]+)$")
movzx_re = re.compile(r"MOVZX (\w+),byte ptr \[(\w+) \+ 0x([0-9a-f]+)\]$")
recent, tables, missing_total = [], [], 0
with open(dump) as f:
    for line in f:
        if not line.startswith("INS "): continue
        head, text = line.rstrip("\n").split(" | ", 1)
        p = head.split()
        recent = (recent + [text])[-6:]
        m = jmp_re.search(text)
        if not m: continue
        reg, table = m.group(1), int(m.group(2), 16)
        # two-level switch (MSVC): MOVZX REG,byte ptr [IDX + BYTES] picks the case number from a byte table first
        mz = movzx_re.search(recent[-2]) if len(recent) > 1 else None
        if mz and mz.group(1) == reg: reg = mz.group(2)
        bound = None
        for t in reversed(recent[:-1]):
            cm = cmp_re.search(t)
            if cm and cm.group(1) == reg: bound = int(cm.group(2), 16); break
        if (bound is None or bound > 1024) and not mz:
            bound = -1
            while bound < 1023 and table + 4 * (bound + 1) in relocs and in_code(u32(table + 4 * (bound + 1)) or 0): bound += 1
            if bound < 0: continue
        if bound is None or bound > 1024: continue
        if mz:
            idx = [u8(int(mz.group(3), 16) + i) for i in range(bound + 1)]
            if None in idx: continue
            bound = max(idx)
        known = set() if p[4] == "-" else {int(x, 16) for x in p[4].split(",")}
        targets = [u32(table + 4 * i) for i in range(bound + 1)]
        if None in targets or not all(in_code(t) for t in targets): continue
        miss = sorted(set(targets) - known)
        if miss:
            tables.append((int(p[1], 16), sorted(set(targets))))
            missing_total += len(miss)
open(out, "w").write("".join(f"{j:x} " + " ".join(f"{t:x}" for t in ts) + "\n" for j, ts in tables))
print(f"{len(tables)} jump tables with targets Ghidra missed ({missing_total} missing targets) -> {out}")
