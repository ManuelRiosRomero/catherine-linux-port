# Scan MSVC C++ EH FuncInfo tables (magic 0x1993052x): how many functions have try/catch, and what they catch.
import struct, sys, collections
d = open(sys.argv[1], "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]
nsec, optsz = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
secs = []
for i in range(nsec):
    s = pe + 24 + optsz + 40 * i
    vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, s + 8)
    secs.append((base + va, rsz, rptr))
def va2off(a):
    for va, rsz, rptr in secs:
        if va <= a < va + rsz: return rptr + a - va
def cstr(a):
    o = va2off(a); return d[o:d.index(b"\0", o)].decode(errors="replace") if o else "?"
funcinfos = tries = 0
catches = collections.Counter()
for o in range(0, len(d) - 20, 4):
    magic, maxstate, unwind, ntry, ptry = struct.unpack_from("<IIIII", d, o)
    if magic not in (0x19930520, 0x19930521, 0x19930522) or maxstate > 10000 or ntry > 1000: continue
    if ntry and va2off(ptry) is None: continue
    funcinfos += 1
    for t in range(ntry):
        tb = va2off(ptry + 20 * t)
        lo, hi, catchhigh, ncatch, pcatch = struct.unpack_from("<iiiII", d, tb)
        tries += 1
        for k in range(min(ncatch, 50)):
            ho = va2off(pcatch + 16 * k)
            if ho is None: continue
            adj, ptype, disp, handler = struct.unpack_from("<IIiI", d, ho)
            catches[cstr(ptype + 8) if ptype else "... (catch all)"] += 1
print(f"{funcinfos} FuncInfo tables, {tries} try blocks")
for k, v in catches.most_common(): print(f"  {v:4d} catch {k}")
