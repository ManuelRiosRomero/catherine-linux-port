#!/usr/bin/env python3
"""Differential checker: run each lifted function in Unicorn (original x86 bytes) and as lifted C,
from identical random states, and compare registers + memory.
Usage: check.py <exe> <gen_dir> [trials=30] [fn hex ...]   (needs gen_dir/lifted.so + manifest.json)"""
import bisect, ctypes, json, mmap, os, random, struct, sys
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL, UC_HOOK_BLOCK, UC_HOOK_MEM_WRITE, UC_HOOK_CODE, UC_HOOK_MEM_READ
from unicorn import x86_const as X

GB4, SENT, STACK, SCR, TEB = 1 << 32, 0xDEAD0000, 0x10000000, 0x20000000, 0x7FFD0000
ESP0 = STACK + 0xF0000
GPR = [("EAX", 0x0), ("ECX", 0x4), ("EDX", 0x8), ("EBX", 0xC), ("ESP", 0x10), ("EBP", 0x14), ("ESI", 0x18), ("EDI", 0x1C)]
# OF is left out: Intel leaves it undefined after multi-bit shifts/rotates, and Ghidra and Unicorn pick differently.
FLAGS = [("CF", 0x200, 0), ("ZF", 0x206, 6), ("SF", 0x207, 7)]
REGIONS = [(STACK, 0x100000), (SCR, 0x10000), (TEB, 0x1000)]


def guest_mem():
    # +64 KB tail: an access straddling 4 GB (random pointer near 0xFFFFFFFF) must not segfault the C side
    mm = mmap.mmap(-1, GB4 + 0x10000, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS | 0x4000)  # 0x4000 = MAP_NORESERVE
    return mm, ctypes.addressof(ctypes.c_char.from_buffer(mm))


def load_pe(path, mems):
    """Map headers + sections at the preferred image base into each guest memory.
    Returns (base, size, [(lo, hi)] of executable sections)."""
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec, optsz = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    base, size, hdrs = struct.unpack_from("<I", d, opt + 28)[0], struct.unpack_from("<I", d, opt + 56)[0], struct.unpack_from("<I", d, opt + 60)[0]
    for m in mems: m[base:base + hdrs] = d[:hdrs]
    code = []
    for i in range(nsec):
        s = opt + optsz + 40 * i
        vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, s + 8)
        n = min(rsz, vsz) if vsz else rsz
        for m in mems: m[base + va:base + va + n] = d[rptr:rptr + n]
        if struct.unpack_from("<I", d, s + 36)[0] & 0x20000000: code.append((base + va, base + va + max(vsz, rsz)))
    return base, size, code


class Checker:
    def __init__(self, exe, gen):
        self.A, self.pa = guest_mem()   # Unicorn's memory
        self.B, self.pb = guest_mem()   # lifted C's memory
        self.base, self.size, self.code = load_pe(exe, [self.A, self.B])
        self.regions = [(self.base, self.size)] + REGIONS
        bodies = json.load(open(os.path.join(gen, "manifest.json")))
        self.fns = sorted(int(k, 16) for k in bodies)
        iv = sorted((lo, hi) for b in bodies.values() for lo, hi in b)
        self.lo, self.hi = [a for a, _ in iv], [b for _, b in iv]

        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        chunk = 1 << 28
        for a in range(0, GB4, chunk):
            self.uc.mem_map_ptr(a, chunk, UC_PROT_ALL, self.pa + a)
        # 32-bit mode has no FS_BASE register: build a GDT. Entry 1 (0x08) = FS at the TEB, entry 2 (0x10) = flat
        # 32-bit data for SS/DS/ES (without it, loading FS leaves the stack 16-bit and push/pop use only SP).
        gdt = 0x7FFC0000
        fs = 0xFFF | (TEB & 0xFFFFFF) << 16 | 0x92 << 40 | 0x4 << 52 | (TEB >> 24) << 56
        flat = 0xFFFF | 0x92 << 40 | 0xF << 48 | 0xC << 52
        self.A[gdt + 8:gdt + 24] = struct.pack("<QQ", fs, flat)
        self.uc.reg_write(X.UC_X86_REG_GDTR, (0, gdt, 0x17, 0))
        for r in (X.UC_X86_REG_SS, X.UC_X86_REG_DS, X.UC_X86_REG_ES): self.uc.reg_write(r, 0x10)
        self.uc.hook_add(UC_HOOK_BLOCK, self._block)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._write)
        # An access straddling 4 GB wraps to address 0 in Unicorn but hits the guard tail in C (real Windows would fault).
        def wrap(uc, acc, a, s, v, _):
            if a + s > GB4: self.wrapped = a
        self.uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, wrap, begin=GB4 - 0x10000, end=GB4 - 1)
        # TRACE=1: use lifted_trace.so (built with -DRT_TRACE) and report where the two execution paths split
        self.trace = bool(os.environ.get("TRACE"))
        if self.trace: self.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, _: self.utr.append(a))

        self.lib = ctypes.CDLL(os.path.join(gen, "lifted_trace.so" if self.trace else "lifted.so"))
        ctypes.c_void_p.in_dll(self.lib, "M").value = self.pb
        self.lib.rt_enter.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        self.libc = ctypes.CDLL(None)
        self.libc.memcmp.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
        self.libc.memcpy.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]

    def in_set(self, a):
        i = bisect.bisect_right(self.lo, a) - 1
        return i >= 0 and a <= self.hi[i]

    def _block(self, uc, addr, size, _):
        if addr != SENT and not self.in_set(addr):
            self.left = addr; uc.emu_stop()

    def _write(self, uc, access, addr, size, value, _):
        self.writes.add((addr, size))
        # a random pointer landing in .text makes Unicorn run patched code; lifted C can't (and the game never does this)
        if any(lo <= addr < hi for lo, hi in self.code): self.smc = addr

    def rand_val(self, rng):
        k = rng.random()
        if k < 0.3: return rng.randrange(17)
        if k < 0.7: return SCR + rng.randrange(0, 0xF000, 4)
        return rng.getrandbits(32)

    def trial(self, fn, rng):
        # identical starting memory: random stack + scratch, minimal TEB, sentinel return address
        for a, n in REGIONS:
            blob = rng.randbytes(n) if a != TEB else bytes(n)
            self.A[a:a + n] = blob; self.B[a:a + n] = blob
        args = b"".join(struct.pack("<I", self.rand_val(rng)) for _ in range(16))
        for m in (self.A, self.B):
            m[TEB:TEB + 4] = b"\xff\xff\xff\xff"; m[TEB + 0x18:TEB + 0x1C] = struct.pack("<I", TEB)
            m[ESP0:ESP0 + 4] = struct.pack("<I", SENT)
            m[ESP0 + 4:ESP0 + 0x44] = args
        regs = {n: self.rand_val(rng) for n, _ in GPR}
        regs["ESP"] = ESP0

        uc = self.uc
        for n, _ in GPR: uc.reg_write(getattr(X, "UC_X86_REG_" + n), regs[n])
        uc.reg_write(X.UC_X86_REG_EFLAGS, 0x202)
        uc.reg_write(X.UC_X86_REG_FS, 0x08)
        for i in range(8): uc.reg_write(X.UC_X86_REG_XMM0 + i, 0)
        uc.reg_write(X.UC_X86_REG_FPCW, 0x37F); uc.reg_write(X.UC_X86_REG_FPSW, 0); uc.reg_write(X.UC_X86_REG_FPTAG, 0xFFFF)
        self.left, self.writes, self.smc, self.utr, self.wrapped = None, set(), None, [], None
        # Any skip must leave A and B identical again, or the next trial starts from different memories.
        sync = lambda dst, src: [self.libc.memcpy(dst + a, src + a, n) for a, n in self.regions + sorted(self.writes)]
        try:
            uc.emu_start(fn, SENT, count=2_000_000)
        except UcError as e:
            sync(self.pa, self.pb); return "skip", f"unicorn: {e}"
        eip = uc.reg_read(X.UC_X86_REG_EIP)
        if self.wrapped is not None:
            sync(self.pa, self.pb); return "skip", f"4GB wrap @{self.wrapped:x}"
        if self.smc is not None:
            sync(self.pa, self.pb); return "skip", f"wrote code @{self.smc:x}"
        if self.left is None and eip != SENT:
            sync(self.pa, self.pb); return "skip", "timeout"
        ua = ("left", self.left) if self.left is not None else ("ret", None)

        cpu = ctypes.create_string_buffer(0x4000)
        for n, o in GPR: struct.pack_into("<I", cpu, o, regs[n])
        struct.pack_into("<I", cpu, 0x110, TEB)          # FS_OFFSET
        struct.pack_into("<H", cpu, 0x10A0, 0x37F)       # FPUControlWord
        struct.pack_into("<I", cpu, 0x1094, 0x1F80)      # MXCSR
        r = self.lib.rt_enter(cpu, fn)
        if r == 2:
            sync(self.pb, self.pa)  # ponytail: C writes outside A's write set aren't undone; rare, only after unimpl
            return "skip", "unimpl " + (ctypes.c_char * 128).in_dll(self.lib, "rt_msg").value.decode()
        cb = ("left", ctypes.c_uint32.in_dll(self.lib, "rt_left_at").value) if r == 1 else ("ret", None)

        diffs = []
        if ua != cb: diffs.append(f"exit unicorn={ua} c={cb}")
        for n, o in GPR:
            u, v = uc.reg_read(getattr(X, "UC_X86_REG_" + n)), struct.unpack_from("<I", cpu, o)[0]
            if u != v: diffs.append(f"{n} {u:#x}!={v:#x}")
        fl = uc.reg_read(X.UC_X86_REG_EFLAGS)
        for n, o, bit in FLAGS:
            if (fl >> bit) & 1 != cpu.raw[o] & 1: diffs.append(f"{n} {(fl >> bit) & 1}!={cpu.raw[o] & 1}")
        for i in range(8):
            u = uc.reg_read(X.UC_X86_REG_XMM0 + i).to_bytes(16, "little")
            if u != cpu.raw[0x1200 + 0x40 * i:0x1210 + 0x40 * i]: diffs.append(f"XMM{i} {u.hex()}!={cpu.raw[0x1200 + 0x40 * i:0x1210 + 0x40 * i].hex()}")
        top = (uc.reg_read(X.UC_X86_REG_FPSW) >> 11) & 7
        for i in range((8 - top) % 8):
            mant, exp = uc.reg_read(X.UC_X86_REG_FP0 + ((top + i) & 7))
            u = struct.pack("<QH", mant, exp)
            s = 0x1100 + 0x10 * ((cpu.raw[0x10b0] + i) & 7)   # ST(i) in the lifted code: slot (top + i) & 7 (rt.h STP)
            if u != cpu.raw[s:s + 10]: diffs.append(f"ST{i} {u.hex()}!={cpu.raw[s:s + 10].hex()}")
        ranges = self.regions + sorted(self.writes)
        for a, n in ranges:
            if self.libc.memcmp(self.pa + a, self.pb + a, n):
                first = next(i for i in range(n) if self.A[a + i] != self.B[a + i])
                p = a + first
                diffs.append(f"mem {p:#x} {bytes(self.A[p:p + 4]).hex()}!={bytes(self.B[p:p + 4]).hex()}")
                break
        for a, n in ranges: self.libc.memcpy(self.pb + a, self.pa + a, n)  # resync for the next trial
        if diffs and self.trace:
            n = ctypes.c_uint32.in_dll(self.lib, "rt_trn").value
            ctr = list((ctypes.c_uint32 * min(n, 1 << 20)).in_dll(self.lib, "rt_tr"))
            utr = [a for a in self.utr if a != SENT and self.in_set(a)]
            i = next((i for i, (x, y) in enumerate(zip(utr, ctr)) if x != y), min(len(utr), len(ctr)))
            ctx = " ".join(f"{a:x}" for a in utr[max(0, i - 4):i])
            nxt = lambda t: f"{t[i]:x}" if i < len(t) else "end"
            diffs.insert(0, f"split after [{ctx}] uc->{nxt(utr)} c->{nxt(ctr)} (step {i}/{len(utr)})")
        return ("pass", ua[0]) if not diffs else ("FAIL", "; ".join(diffs[:6]))


def main():
    exe, gen = sys.argv[1], sys.argv[2]
    trials = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    ck = Checker(exe, gen)
    fns = [int(a, 16) for a in sys.argv[4:]] or ck.fns
    tot = {"pass": 0, "FAIL": 0, "skip": 0, "CRASH": 0}
    for fn in fns:
        # Each function runs in a forked child: a crash in lifted code only loses that function, and the
        # copy-on-write guest memories come back clean for the next one.
        rd, wr = os.pipe()
        pid = os.fork()
        if pid == 0:
            os.close(rd)
            rng, res = random.Random(fn), {}
            for _ in range(trials):
                k, why = ck.trial(fn, rng)
                res.setdefault(k, []).append(why)
            line = " ".join(f"{k}={len(v)}" for k, v in res.items())
            notes = sorted({w for k in ("FAIL", "skip") for w in res.get(k, [])})[:3]
            os.write(wr, json.dumps([{k: len(v) for k, v in res.items()}, f"{fn:08x} {line}  {' | '.join(notes)}"]).encode())
            os._exit(0)
        os.close(wr)
        out = b"".join(iter(lambda: os.read(rd, 1 << 16), b""))
        os.close(rd)
        _, status = os.waitpid(pid, 0)
        if out:
            counts, line = json.loads(out)
            for k, n in counts.items(): tot[k] += n
        else:
            tot["CRASH"] += 1
            line = f"{fn:08x} CRASH signal={os.WTERMSIG(status) if os.WIFSIGNALED(status) else status}"
        print(line, flush=True)
    print("TOTAL", tot)


if __name__ == "__main__":
    main()
