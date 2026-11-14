#!/usr/bin/env python3
"""Rebase a 32-bit PE DLL: apply its base relocations for a new ImageBase and write a copy with that ImageBase.
The copy then maps at a fixed address like the exe, so Ghidra, the checker and the host need no relocation code.
Usage: rebase.py <in.dll> <out.dll> <new base hex>"""
import struct, sys

src, dst, new = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
d = bytearray(open(src, "rb").read())
pe = struct.unpack_from("<I", d, 0x3C)[0]
opt, nsec, optsz = pe + 24, struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
assert struct.unpack_from("<H", d, opt)[0] == 0x10B, "not PE32"
old = struct.unpack_from("<I", d, opt + 28)[0]
secs = [struct.unpack_from("<IIII", d, opt + optsz + 40 * i + 8) for i in range(nsec)]   # vsize, va, rawsize, rawptr

def off(rva):
    for vsz, va, rsz, raw in secs:
        if va <= rva < va + max(vsz, rsz): return raw + rva - va
    raise ValueError(f"rva {rva:x} not in a section")

rva, size = struct.unpack_from("<II", d, opt + 96 + 8 * 5)   # data directory 5 = base relocations
delta, n, p = (new - old) & 0xFFFFFFFF, 0, off(rva)
end = p + size
while p < end:
    page, blk = struct.unpack_from("<II", d, p)
    for k in range((blk - 8) // 2):
        e = struct.unpack_from("<H", d, p + 8 + 2 * k)[0]
        if e >> 12 == 0: continue                                  # padding
        assert e >> 12 == 3, f"relocation type {e >> 12}"          # IMAGE_REL_BASED_HIGHLOW only
        o = off(page + (e & 0xFFF))
        struct.pack_into("<I", d, o, (struct.unpack_from("<I", d, o)[0] + delta) & 0xFFFFFFFF)
        n += 1
    p += blk
struct.pack_into("<I", d, opt + 28, new)
open(dst, "wb").write(d)
print(f"{src}: {old:08x} -> {new:08x}, {n} relocations applied -> {dst}")
