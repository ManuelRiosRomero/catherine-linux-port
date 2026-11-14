#!/usr/bin/env python3
"""Mis-decodes Ghidra left inside real code: an instruction of a real function falls through, jumps or calls into bytes
that are not an instruction start (Ghidra decoded them from another byte: e.g. 0x669df7, where "ADD byte ptr [EAX],AL"
junk began 4 bytes later). Follows the code from every function entry the way the lifter does, so it also finds
functions reached only through pointers (the readiness traversal can't see those). Each candidate is judged by a second
disassembler (objdump): if the bytes there decode as normal code, it is printed as "<from>-<resync> <from>" = the
arguments for tools/ghidra/Redecode.java (clear up to where the old decode lines up again, re-disassemble from <from>).
Usage: misdecodes.py <image> <dump.pcode.txt> <out.txt>"""
import re, struct, subprocess, sys

image, dump, out = sys.argv[1:4]
d = open(image, "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]
nsec, optsz = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
code = []   # executable sections (VA ranges)
for i in range(nsec):
    vsz, va, rsz, rptr, _, _, _, ch = struct.unpack_from("<IIIIIIHHI", d, pe + 24 + optsz + 40 * i + 8)[:4] + (0, 0, 0, 0)
    if struct.unpack_from("<I", d, pe + 24 + optsz + 40 * i + 36)[0] & 0x20000000: code.append((base + va, base + va + vsz))
in_code = lambda a: any(lo <= a < hi for lo, hi in code)

# same junk rule as readiness.py: a "function" whose first instructions compilers never emit is data decoded as code
JUNK = {"IN", "OUT", "INSB", "INSW", "INSD", "OUTSB", "OUTSW", "OUTSD", "INTO", "INT3", "HLT", "LDS", "LES", "ARPL",
        "BOUND", "SLDT", "STR", "LLDT", "LTR", "SGDT", "SIDT", "LGDT", "LIDT", "CLI", "STI", "AAA", "AAS", "DAA", "DAS",
        "AAM", "AAD", "SALC", "IRET", "IRETD", "RETF", "CALLF", "JMPF"}
ins, fns, junk_fn = {}, [], set()
cur, first = None, 0
for line in open(dump, encoding="latin1"):
    if line.startswith("FN "):
        cur = int(line.split()[1], 16); fns.append(cur); first = 0
    elif line.startswith("INS "):
        head, text = line.rstrip("\n").split(" | ", 1)
        f = head.split()
        a = int(f[1], 16)
        nxt = [] if f[3] == "-" else [int(f[3], 16)]
        tg = [] if f[4] == "-" else [int(x, 16) for x in f[4].split(",")]
        ins[a] = (nxt, tg, text)
        if a == cur or 0 < first < 4:   # the entry and the next few instructions in the dump
            first += 1
            if text.split(" ", 1)[0] in JUNK or text.startswith("ADD byte ptr [EAX],AL"): junk_fn.add(cur)

# flow closure from every real entry, as the lifter derives code; collect flows into non-starts
seen, cand = set(), {}
for e in fns:
    if e in junk_fn or e not in ins: continue
    todo = [e]
    while todo:
        a = todo.pop()
        if a in seen: continue
        seen.add(a)
        nxt, tg, text = ins[a]
        for t in nxt + tg:
            if t in ins: todo.append(t)
            elif in_code(t): cand.setdefault(t, a)

BAD = re.compile(r"\(bad\)|^(in|out|ins[bwd]?|outs[bwd]?|into|int3|hlt|lds|les|arpl|bound|cli|sti|aaa|aas|daa|das|aam|aad|"
                 r"iret|iretd|retf|lcall|ljmp|enter|leave)\b|add\s+BYTE PTR \[eax\],al|\b(push|pop)\s+(es|ss|ds|cs)\b|"
                 r"\b(fs|gs|ss|es|cs|ds):")
def decode(t, n=12):   # [(addr, text)] from objdump, linear from t
    r = subprocess.run(["objdump", "-d", "-M", "intel", f"--start-address={t:#x}", f"--stop-address={t + 64:#x}", image],
                       capture_output=True, text=True).stdout
    rows = []
    for line in r.splitlines():
        m = re.match(r"\s+([0-9a-f]+):\t(?:[0-9a-f]{2} )+\s*\t(.*)", line)
        if m: rows.append((int(m.group(1), 16), m.group(2).strip()))
    return rows[:n]

# the source must look like compiled code too: most candidates come from data (switch tables, constants) decoded as code
# inside a real function, where these show up (XLAT, CMC, LOOP, PUSH ES, ADD byte ptr [x],reg8 = typical data bytes)
BAD_SRC = re.compile(r"^(IN|OUT|XLAT|CMC|LOOP\w*|JCXZ|JECXZ|SAHF|LAHF|SALC|DAA|DAS|AAA|AAS|AAM|AAD|INT3|HLT|LDS|LES|ARPL|"
                     r"BOUND|INTO|IRETD?|RETF|(PUSH|POP) (ES|SS|DS|CS)|(ADD|OR|ADC|SBB|AND|SUB|XOR|XCHG) byte ptr \[)\b")
WILD = re.compile(r"BYTE PTR \[[^\]]*0x[0-9a-f]{5,}\],[a-d][hl]\b|\.byte|data16")   # data bytes decoded as an instruction
starts_sorted = sorted(ins)
import bisect
def src_ok(a):   # the source and the 4 instructions before it in the dump
    i = bisect.bisect_left(starts_sorted, a)
    return not any(BAD_SRC.search(ins[x][2]) for x in starts_sorted[max(0, i - 4):i + 1])
real, junk = [], []
for t, src in sorted(cand.items()):
    rows = decode(t)
    sync = next((a for a, _ in rows[1:] if a in ins), None)   # the old decode lines up again here
    upto = [x for a, x in rows if sync is None or a < sync] or [x for _, x in rows[:4]]
    if sync is None or not src_ok(src) or any(BAD.search(x) or WILD.search(x) for x in upto): junk.append((t, src, upto[:3]))
    else: real.append((t, sync, src, upto))
with open(out, "w") as o:
    for t, sync, src, _ in real: o.write(f"{t:x}-{sync:x} {t:x}\n")
print(f"{len(cand)} flows into non-instruction bytes; {len(real)} decode as code -> {out}; {len(junk)} look like data")
for t, sync, src, upto in real[:60]: print(f"  real {t:x} (from {src:x}, resync {sync:x}): {' ; '.join(upto[:3])}")
for t, src, upto in junk[:15]: print(f"  data {t:x} (from {src:x}): {' ; '.join(upto)}")
