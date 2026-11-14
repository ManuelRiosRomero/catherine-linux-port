# Find MSVC vtables for classes by RTTI name: TypeDescriptor -> CompleteObjectLocator -> vtable (vtable[-1] = &COL).
import struct, sys
d = open(sys.argv[1], "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]
nsec, optsz = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
secs = []
for i in range(nsec):
    s = pe + 24 + optsz + 40 * i
    vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, s + 8)
    secs.append((base + va, rsz, rptr, struct.unpack_from("<I", d, s + 36)[0]))
def off2va(o):
    for va, rsz, rptr, _ in secs:
        if rptr <= o < rptr + rsz: return va + o - rptr
def va2off(a):
    for va, rsz, rptr, _ in secs:
        if va <= a < va + rsz: return rptr + a - va
code = [(va, va + rsz) for va, rsz, _, ch in secs if ch & 0x20000000]
iscode = lambda a: any(lo <= a < hi for lo, hi in code)
def find_u32(v):
    p, b, out = 0, struct.pack("<I", v), []
    while (p := d.find(b, p)) != -1:
        if p % 4 == 0: out.append(off2va(p))
        p += 1
    return out
for name in sys.argv[2:]:
    td = off2va(d.find(name.encode() + b"\0")) - 8
    for ref in find_u32(td):
        col = ref - 12                      # COL.pTypeDescriptor is at +12
        sig, offset = struct.unpack_from("<II", d, va2off(col))
        if sig != 0: continue
        for vref in find_u32(col):
            vt, n = vref + 4, 0
            ents = []
            while True:
                (f,) = struct.unpack_from("<I", d, va2off(vt + 4 * n))
                if not iscode(f) or (n and find_u32(vt + 4 * n)): break
                ents.append(f); n += 1
            print(f"{name} offset={offset} vtable={vt:x} [{len(ents)}]: " + " ".join(f"{f:x}" for f in ents))
