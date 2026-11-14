# fn.py <pe> <dump> <fn hex> [max]: one function's instructions, IAT calls named, strings shown (debug helper)
import re, struct, sys
pe_path, dump, fn = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
mx = int(sys.argv[4]) if len(sys.argv) > 4 else 400
d = open(pe_path, "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]; opt = pe + 24
nsec, optsz = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
base = struct.unpack_from("<I", d, opt + 28)[0]
secs = [struct.unpack_from("<IIII", d, opt + optsz + 40 * i + 8) for i in range(nsec)]
def off(rva):
    for vsz, va, rsz, raw in secs:
        if va <= rva < va + max(vsz, rsz): return raw + rva - va
cstr = lambda o: d[o:d.index(b"\0", o)].decode("latin1")
iat = {}
imp = struct.unpack_from("<I", d, opt + 104)[0]
o = off(imp)
while struct.unpack_from("<I", d, o + 12)[0]:
    names, dll, first = struct.unpack_from("<I", d, o)[0], struct.unpack_from("<I", d, o + 12)[0], struct.unpack_from("<I", d, o + 16)[0]
    k = 0
    while True:
        th = struct.unpack_from("<I", d, off(names or first) + 4 * k)[0]
        if not th: break
        iat[base + first + 4 * k] = cstr(off(dll)) + "!" + (f"#{th & 0xFFFF}" if th >> 31 else cstr(off(th) + 2))
        k += 1
    o += 20
def note(text):
    out = []
    for m in re.finditer(r"0x([0-9a-f]{6,8})", text):
        a = int(m.group(1), 16)
        if a in iat: out.append(iat[a])
        elif base <= a < base + 0x4000000 and off(a - base):
            s = d[off(a - base):off(a - base) + 60].split(b"\0")[0]
            if len(s) >= 3 and all(32 <= ch < 127 for ch in s): out.append(repr(s.decode()))
    return ("   ; " + " ".join(out)) if out else ""
started, n = False, 0
for line in open(dump):
    if line.startswith("FN "):
        a = int(line.split()[1], 16)
        if started and a != fn: break
        started = started or a == fn
    elif started and line.startswith("INS "):
        h, t = line.rstrip("\n").split(" | ", 1)
        print(h.split()[1], t, note(t)); n += 1
        if n >= mx: break
