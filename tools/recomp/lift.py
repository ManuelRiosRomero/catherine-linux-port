#!/usr/bin/env python3
"""Translate Ghidra raw p-code (ExportPcode.java dump) into C, one C function per x86 function.
Usage: lift.py <pcode.txt> <out_dir> <root hex>...   -> out_dir/lifted.c + out_dir/manifest.json
Lifts the roots plus every function they reach through direct calls or tail jumps."""
import json, os, random, sys

CT = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t", 16: "u128"}
ST = {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t", 16: "__int128"}
FT = {4: "float", 8: "double", 10: "long double"}
IMIN = {2: "INT16_MIN", 4: "INT32_MIN", 8: "INT64_MIN"}
ARITH = {"INT_ADD": "+", "INT_SUB": "-", "INT_MULT": "*", "INT_AND": "&", "INT_OR": "|", "INT_XOR": "^"}
UCMP = {"INT_EQUAL": "==", "INT_NOTEQUAL": "!=", "INT_LESS": "<", "INT_LESSEQUAL": "<="}
SCMP = {"INT_SLESS": "<", "INT_SLESSEQUAL": "<="}
FARITH = {"FLOAT_ADD": "+", "FLOAT_SUB": "-", "FLOAT_MULT": "*", "FLOAT_DIV": "/"}
FCMP = {"FLOAT_EQUAL": "==", "FLOAT_NOTEQUAL": "!=", "FLOAT_LESS": "<", "FLOAT_LESSEQUAL": "<="}
FMATH = {"ROUND": "rint", "FLOAT_CEIL": "ceil", "FLOAT_FLOOR": "floor", "FLOAT_SQRT": "sqrt", "FLOAT_ABS": "fabs"}
# MMX/SSE user ops with an rt_<name>(dst, src, size) helper in rt.h
SIMD2 = {"paddsw", "psubsw", "pmulhw", "psllw", "psraw", "pmaddwd", "packssdw", "packsswb", "rcpps", "rcpss", "rsqrtss"}
FSUF = {4: "f", 8: "", 10: "l"}
# Guest registers kept in C locals inside a function (EAX..EDI, then CF PF AF ZF SF DF OF): offset -> size.
# c->r is shared with every callee and the compiler must keep each access a real load/store; locals live in host
# registers. RS_ writes them back before anything that reads c->r (calls, jumps, returns, rt_*), RL_ reloads after.
# Also the FPU instruction pointer (written by every x87 instruction, read almost never) and the x87 top (rt.h STP).
CACHED = {**{o: 4 for o in range(0, 0x20, 4)}, **{o: 1 for o in (0x200, 0x202, 0x204, 0x206, 0x207, 0x20a, 0x20b)},
          0x10ac: 4, 0x10b0: 1}
# XMM0-7 (0x1200 + 0x40 * i): Ghidra splits SSE ops into 4-byte lanes, so each lane can be a uint32 local like a GPR.
# Per function, a lane is cached only when its exact 4-byte uses outweigh the cost: every call writes it back and
# reloads it, and every wider (8/16-byte) access of it must go through c->r (safe_op).
XMM_LANES = [0x1200 + 0x40 * i + 4 * k for i in range(8) for k in range(4)]
CALL_OPS = {"CALL", "CALLIND", "CALLOTHER"}


def vn(s):
    if s == "-": return None
    k, sz = s.split(":")
    return (k[0], int(k[1:], 16), int(sz))


# ---------- index: every instruction in the dump, and each function's code found by following control flow ----------
# Ghidra lets an instruction belong to only one function, and its split can change when functions are added.
# So we ignore ownership: a function is everything reachable from its entry (fallthrough + branches, not into
# calls), stopping at other functions' entries (tail jumps). Shared code (common epilogues) is copied into each.

def build_index(dump):
    entries, users, ins = {}, {}, {}
    with open(dump, "rb") as f:
        off = 0
        for raw in f:
            if raw.startswith(b"INS "):
                head = raw.split(b" | ", 1)
                _, a, ln, ft, fl = head[0].split()
                kind = 1 if head[1].startswith(b"CALL") else 0
                ins[int(a, 16)] = (off, int(ln), None if ft == b"-" else int(ft, 16),
                                   () if fl == b"-" else tuple(int(x, 16) for x in fl.split(b",")), kind)
            elif raw.startswith(b"FN "):
                p = raw.split(); entries[int(p[1], 16)] = p[2].decode()
            elif raw.startswith(b"USEROP"):
                p = raw.split(); users[int(p[1])] = p[2].decode()
            off += len(raw)
    fns = {}
    for e in entries:
        code, calls, br = fn_code(e, entries, ins)
        fns[e] = {"name": entries[e], "body": ranges(code, ins), "calls": calls, "br": br}
    return fns, users, ins


def fn_code(e, entries, ins):
    """Instruction addresses of function e (sorted), its direct call targets, and its tail-jump targets."""
    seen, todo, calls, br = set(), [e], set(), set()
    while todo:
        a = todo.pop()
        if a in seen or a not in ins: continue
        seen.add(a)
        _, _, ft, flows, kind = ins[a]
        succ = [ft] if kind else [ft, *flows]
        if kind: calls.update(flows)
        for t in succ:
            if t is None: continue
            if t != e and t in entries: br.add(t)
            else: todo.append(t)
    return sorted(seen), sorted(calls), sorted(br)


def ranges(code, ins):
    out = []
    for a in code:
        hi = a + ins[a][1] - 1
        if out and a <= out[-1][1] + 1: out[-1][1] = max(out[-1][1], hi)
        else: out.append([a, hi])
    return out


def load_index(dump):
    import pickle
    cache = dump + ".idx.pkl"
    if os.path.exists(cache) and os.path.getmtime(cache) > os.path.getmtime(dump):
        return pickle.load(open(cache, "rb"))
    r = build_index(dump)
    pickle.dump(r, open(cache, "wb"), protocol=pickle.HIGHEST_PROTOCOL)
    fns, users, _ = r   # JSON summary for the helper scripts (bodies, calls, tail jumps per function)
    json.dump({"fns": {str(k): v for k, v in fns.items()}, "userops": users}, open(dump + ".idx.json", "w"))
    return r


def closure(idx, roots):
    seen, todo = set(), list(roots)
    while todo:
        a = todo.pop()
        if a in seen or a not in idx: continue
        seen.add(a); todo += idx[a]["calls"] + idx[a]["br"]
    return seen


def read_fn(dump, entry, fns, ins):
    """Returns [(addr, fallthrough, flows, text, [(mnem, out, [inputs])])] for the function's flow-derived code."""
    code, _, _ = fn_code(entry, fns, ins)
    out = []
    with open(dump) as fh:
        for a in code:
            fh.seek(ins[a][0])
            head, text = fh.readline().rstrip("\n").split(" | ", 1)
            _, _, _, ft, fl = head.split()
            ops = []
            for line in fh:
                if not line.startswith("OP "): break
                p = line.split()
                ops.append((p[1], vn(p[2]), [vn(x) for x in p[3:]]))
            out.append((a, None if ft == "-" else int(ft, 16), [] if fl == "-" else [int(x, 16) for x in fl.split(",")], text, ops))
    return out


# ---------- code generation ----------

class FnGen:
    def __init__(self, entry, f, ins, lifted, userops):
        self.entry, self.ins, self.lifted, self.userops = entry, ins, lifted, userops
        self.body = f["body"]
        self.starts = {i[0] for i in ins}
        self.slots, n = {}, 0  # unique space: merge overlapping (off, size) ranges into compact slots
        rs = sorted({(v[1], v[2]) for i in ins for op in i[4] for v in [op[1]] + op[2] if v and v[0] == "u"})
        blocks = []
        for o, s in rs:
            if blocks and o < blocks[-1][1]: blocks[-1][1] = max(blocks[-1][1], o + s)
            else: blocks.append([o, o + s])
        for a, b in blocks:
            self.slots[a] = (n, a)
            n += (b - a + 15) & ~15
        self.blockof = {}
        for a, b in blocks:
            for o, s in rs:
                if a <= o < b: self.blockof[o] = self.slots[a][0] + (o - a)
        self.usize = max(n, 16)
        self.nocache, self.raw, self.touched, self.written = False, False, set(), set()
        exact, wide, calls = dict.fromkeys(XMM_LANES, 0), dict.fromkeys(XMM_LANES, 0), 0
        for i in ins:
            for m, o, a in i[4]:
                calls += m in CALL_OPS
                for v in [o] + a:
                    if not v or v[0] != "r" or not 0x1200 <= v[1] < 0x1400: continue
                    for b in XMM_LANES:
                        if v[1] == b and v[2] == 4: exact[b] += 1
                        elif v[1] < b + 4 and b < v[1] + v[2]: wide[b] += 1
        self.cached = {**CACHED, **{b: 4 for b in XMM_LANES if exact[b] > 2 * (calls + wide[b])}}

    def creg(self, v):
        """(base, byte offset) when v lies inside one cached register, else None; marks raw when only part overlaps."""
        if v[0] != "r": return None
        k, o, s = v
        for b, z in self.cached.items():
            if b <= o and o + s <= b + z:
                self.touched.add(b)
                return None if self.nocache else (b, o - b)
            if o < b + z and b < o + s: self.touched.add(b); self.raw |= not self.nocache
        return None

    # a branch can only become a goto if it lands on an instruction start of this function (junk "functions"
    # decoded from misaligned bytes branch into the middle of instructions; those leave via rt_jump instead)
    def in_body(self, a): return a in self.starts

    def ptr(self, v):
        k, o, _ = v
        if k == "r" and 0x1100 <= o < 0x1180:   # ST(i) / MMi: slot (top + i) & 7, see x87_stack_moves
            i = (o - 0x1100) >> 4
            self.touched.add(0x10b0)
            p = f"STP(c,{i})" if self.nocache else f"(c->r+0x1100+(((g10b0+{i})&7)<<4))"
            return f"({p}+{o & 15})" if o & 15 else p
        if k == "r":
            if self.creg(v): self.raw = True   # memory view of a cached register: safe_op redoes the op on c->r
            return f"(c->r+0x{o:x})"
        if k == "u": return f"(u+{self.blockof[o]})"
        if k == "m": return f"GM(0x{o:x})"
        raise ValueError(v)

    def rd(self, v):
        k, o, s = v
        if k == "c": return f"(({CT[s]})0x{o & ((1 << 8 * min(s, 8)) - 1):x}ULL)"
        r = self.creg(v)
        if r: return f"g{r[0]:x}" if s == self.cached[r[0]] else f"(({CT[s]})(g{r[0]:x} >> {8 * r[1]}))"
        return f"V({self.ptr(v)},{CT[s]})"

    def srd(self, v): return f"(({ST[v[2]]}){self.rd(v)})"

    def lane(self, v):   # a float exactly filling a cached 4-byte register (an XMM lane): its local's name
        r = self.creg(v) if v[2] == 4 else None
        return f"g{r[0]:x}" if r and r[1] == 0 and self.cached[r[0]] == 4 else None

    def frd(self, v):
        if v[0] == "c": raise ValueError("float constant")
        g = self.lane(v)
        if g: return f"rt_f32({g})"
        return f"ld10({self.ptr(v)})" if v[2] == 10 else f"V({self.ptr(v)},{FT[v[2]]})"

    def wr(self, v, e):
        r = self.creg(v)
        if not r: return f"V({self.ptr(v)},{CT[v[2]]}) = ({CT[v[2]]})({e});"
        g, z = f"g{r[0]:x}", self.cached[r[0]]
        self.written.add(r[0])
        if v[2] == z: return f"{g} = ({CT[z]})({e});"
        m = ((1 << 8 * v[2]) - 1) << 8 * r[1]
        return f"{g} = ({g} & 0x{~m & 0xffffffff:x}u) | ((uint32_t)({CT[v[2]]})({e}) << {8 * r[1]});"

    def fwr(self, v, e):
        g = self.lane(v)
        if g: self.written.add(int(g[1:], 16)); return f"{g} = rt_b32((float)({e}));"
        return f"st10({self.ptr(v)}, (long double)({e}));" if v[2] == 10 else f"V({self.ptr(v)},{FT[v[2]]}) = ({FT[v[2]]})({e});"

    def goto(self, a):
        if self.in_body(a): self.need.add(a); return f"goto L_{a:x};"
        if a in self.lifted: return f"{{ RS_ f_{a:x}(c); return; }}"
        return f"{{ RS_ rt_jump(c, 0x{a:x}); return; }}"

    def op(self, addr, i, nops, mn, out, ins):
        a = ins[0] if ins else None
        b = ins[1] if len(ins) > 1 else None
        if mn == "COPY":
            if out[2] not in CT and a[0] != "c": return f"memmove({self.ptr(out)}, {self.ptr(a)}, {out[2]});"
            return self.wr(out, self.rd(a))
        if mn == "LOAD":
            if out[2] not in CT: return f"memcpy({self.ptr(out)}, GM({self.rd(b)}), {out[2]});"
            return self.wr(out, f"V(GM({self.rd(b)}),{CT[out[2]]})")
        if mn == "STORE":
            val = ins[2]
            if val[2] not in CT and val[0] != "c": return f"memcpy(GM({self.rd(b)}), {self.ptr(val)}, {val[2]});"
            return f"V(GM({self.rd(b)}),{CT[val[2]]}) = {self.rd(val)};"
        if mn in ARITH: return self.wr(out, f"{self.rd(a)} {ARITH[mn]} {self.rd(b)}")
        if mn in UCMP: return self.wr(out, f"{self.rd(a)} {UCMP[mn]} {self.rd(b)}")
        if mn in SCMP: return self.wr(out, f"{self.srd(a)} {SCMP[mn]} {self.srd(b)}")
        if mn == "INT_CARRY":
            return self.wr(out, f"({CT[a[2]]})({self.rd(a)} + {self.rd(b)}) < {self.rd(a)}")
        if mn in ("INT_SCARRY", "INT_SBORROW"):
            x, y, t, s = self.rd(a), self.rd(b), CT[a[2]], ST[a[2]]
            if mn == "INT_SCARRY": e = f"({s})(({x} ^ ({t})({x} + {y})) & ({y} ^ ({t})({x} + {y}))) < 0"
            else: e = f"({s})(({x} ^ {y}) & ({x} ^ ({t})({x} - {y}))) < 0"
            return self.wr(out, e)
        if mn == "INT_2COMP": return self.wr(out, f"-{self.rd(a)}")
        if mn == "INT_NEGATE": return self.wr(out, f"~{self.rd(a)}")
        if mn == "INT_ZEXT": return self.wr(out, self.rd(a))
        if mn == "INT_SEXT": return self.wr(out, self.srd(a))
        if mn in ("INT_LEFT", "INT_RIGHT", "INT_SRIGHT"):
            bits, sh = 8 * a[2], f"((uint64_t){self.rd(b)})"
            if mn == "INT_LEFT": e = f"{sh} >= {bits} ? 0 : ({CT[a[2]]})({self.rd(a)} << {sh})"
            elif mn == "INT_RIGHT": e = f"{sh} >= {bits} ? 0 : {self.rd(a)} >> {sh}"
            else: e = f"{self.srd(a)} >> ({sh} >= {bits} ? {bits - 1} : {sh})"
            return self.wr(out, e)
        if mn in ("INT_DIV", "INT_REM"): return self.wr(out, f"{self.rd(a)} {'/' if mn == 'INT_DIV' else '%'} {self.rd(b)}")
        if mn in ("INT_SDIV", "INT_SREM"): return self.wr(out, f"{self.srd(a)} {'/' if mn == 'INT_SDIV' else '%'} {self.srd(b)}")
        if mn == "BOOL_NEGATE": return self.wr(out, f"!{self.rd(a)}")
        if mn in ("BOOL_AND", "BOOL_OR", "BOOL_XOR"):
            return self.wr(out, f"{self.rd(a)} {dict(BOOL_AND='&', BOOL_OR='|', BOOL_XOR='^')[mn]} {self.rd(b)}")
        if mn == "POPCOUNT":
            x = self.rd(a)
            e = (f"__builtin_popcountll((uint64_t){x}) + __builtin_popcountll((uint64_t)({x} >> 64))"
                 if a[2] == 16 else f"__builtin_popcountll((uint64_t){x})")
            return self.wr(out, e)
        if mn == "SUBPIECE":
            k, n = b[1], out[2]
            if a[0] == "c": return self.wr(out, f"{self.rd(a)} >> {8 * k}")
            if n in CT and a[2] in CT:
                return self.wr(out, f"({CT[n]})(({'unsigned __int128' if a[2] == 16 else 'uint64_t'}){self.rd(a)} >> {8 * k})")
            have = max(0, min(n, a[2] - k))
            pre = f"memset({self.ptr(out)}, 0, {n}); " if have < n else ""
            return pre + f"memmove({self.ptr(out)}, {self.ptr(a)}+{k}, {have});"
        if mn == "PIECE":  # out = a:b (a is the high part)
            if out[2] in CT and a[2] in CT and b[2] in CT:
                w = "unsigned __int128" if out[2] == 16 else "uint64_t"
                return self.wr(out, f"(({w}){self.rd(a)} << {8 * b[2]}) | ({w}){self.rd(b)}")
            return (f"{{ uint8_t t[32]; memcpy(t, {self.ptr(b)}, {b[2]}); memcpy(t+{b[2]}, {self.ptr(a)}, {a[2]}); "
                    f"memcpy({self.ptr(out)}, t, {out[2]}); }}")
        if mn in FARITH: return self.fwr(out, f"({FT[a[2]]})({self.frd(a)} {FARITH[mn]} {self.frd(b)})")
        if mn in FCMP: return self.wr(out, f"{self.frd(a)} {FCMP[mn]} {self.frd(b)}")
        if mn == "FLOAT_NAN": return self.wr(out, f"isnan({self.frd(a)})")
        if mn == "FLOAT_NEG": return self.fwr(out, f"-{self.frd(a)}")
        if mn in FMATH: return self.fwr(out, f"{FMATH[mn]}{FSUF[a[2]]}({self.frd(a)})")
        if mn == "INT2FLOAT": return self.fwr(out, f"({FT[out[2]]}){self.srd(a)}")
        if mn == "FLOAT2FLOAT": return self.fwr(out, f"({FT[out[2]]}){self.frd(a)}")
        if mn == "TRUNC": return self.wr(out, f"TRUNC_TO({ST[out[2]]}, {IMIN[out[2]]}, (long double){self.frd(a)})")
        if mn in ("BRANCH", "CBRANCH"):
            if a[0] == "c":
                t = i + (a[1] if a[1] < 1 << 31 else a[1] - (1 << 32))
                self.oplabels.add((addr, t)); g = f"goto L_{addr:x}_{t};"
            else:
                g = self.goto(a[1])
            return g if mn == "BRANCH" else f"if ({self.rd(b)}) {g}"
        if mn == "BRANCHIND":
            t = self.rd(a)
            cases = " ".join(f"case 0x{x:x}: goto L_{x:x};" for x in self.flows if self.in_body(x))
            self.need.update(x for x in self.flows if self.in_body(x))
            return f"switch ({t}) {{ {cases} }} RS_ rt_jump(c, {t}); return;"
        if mn == "CALL":
            return f"RS_ f_{a[1]:x}(c); RL_" if a[1] in self.lifted else f"RS_ rt_call(c, 0x{a[1]:x}); RL_"
        if mn == "CALLIND": return f"RS_ rt_call(c, {self.rd(a)}); RL_"
        if mn == "RETURN": return "RS_ return;"
        if mn == "CALLOTHER":
            name = self.userops[a[1]]
            if name == "LOCK": return "rt_lock();"
            if name == "UNLOCK": return "rt_unlock();"
            if name in ("fsin", "fcos"): return self.fwr(out, f"rt_{name}({self.frd(b)})")
            if name.startswith("cpuid"): return f"{{ RS_ uint32_t t_ = rt_cpuid(c, {a[1]}); RL_ {self.wr(out, 't_')} }}"
            if name in ("divps", "pmulld"): return self.wr(out, f"rt_{name}({self.rd(b)}, {self.rd(ins[2])})")
            if name in SIMD2: return self.wr(out, f"rt_{name}((u128){self.rd(b)}, (u128){self.rd(ins[2])}, {out[2]})")
            if name == "ffree": return "/* ffree: x87 tags are not modelled */"
            return f'RS_ rt_unimpl("{name}", 0x{addr:x});'
        return f'RS_ rt_unimpl("{mn}", 0x{addr:x});'

    def safe_op(self, addr, i, nops, mn, out, ins):
        # Code that can't be expressed (odd operand sizes in junk "functions" decoded from data) stops at runtime
        # instead of breaking the whole lift; real game code never reaches it.
        try:
            self.raw = False
            s = self.op(addr, i, nops, mn, out, ins)
            if not self.raw: return s
            self.nocache = True   # the op needs c->r's bytes: do all of it on c->r, between a write-back and a reload
            try: return f"RS_ {self.op(addr, i, nops, mn, out, ins)} RL_"
            finally: self.nocache = False
        except (KeyError, ValueError) as e:
            return f'RS_ rt_unimpl("lift {mn} {type(e).__name__}", 0x{addr:x});'

    def x87_stack_moves(self, ops, lines):
        # Ghidra's x87 push copies ST6..ST0 down into ST7..ST1 and a pop copies ST1..ST7 up: 7 moves of 10 bytes each,
        # 36% of the CPU in the Stray Sheep bar. With ST(i) kept in slot (top + i) & 7 (ptr, rt.h STP) each run is one
        # change of top, as on the real FPU. Runs this doesn't recognise still work through the mapping, just slower.
        st = lambda v: v[0] == "r" and v[2] == 10 and 0x1100 <= v[1] < 0x1180 and not v[1] & 15
        for i in range(len(ops) - 6):
            run = ops[i:i + 7]
            if not all(m == "COPY" and st(o) and len(a) == 1 and st(a[0]) for m, o, a in run): continue
            moves = [((o[1] - 0x1100) >> 4, (a[0][1] - 0x1100) >> 4) for _, o, a in run]
            if moves == [(k + 1, k) for k in range(6, -1, -1)]: d = "+ 7"     # push
            elif moves == [(k, k + 1) for k in range(7)]: d = "+ 1"           # pop
            else: continue
            self.touched.add(0x10b0); self.written.add(0x10b0)
            lines[i:i + 7] = [f"g10b0 = (g10b0 {d}) & 7;"] + [""] * 6
            # push = rotation: tmp = ST7 before the moves, ST0 = tmp after. New ST0 is old ST7's slot: a self-copy.
            if d == "+ 7" and 0 < i and i + 7 < len(ops):
                (m1, t, a1), (m2, o2, a2) = ops[i - 1], ops[i + 7]
                if m1 == m2 == "COPY" and t[0] == "u" and a1[0] == ("r", 0x1170, 10) and o2 == ("r", 0x1100, 10) and \
                        a2 == [t] and sum(t in a for _, _, a in ops) == 1:
                    lines[i - 1] = lines[i + 7] = ""

    def dead_ops(self):
        # Ghidra writes CF/PF/AF/ZF/SF/OF after nearly every arithmetic instruction and the next one overwrites them
        # unread; each write is a store the C compiler must keep (CPU state escapes into calls). A flag write is dead when
        # straight-line code (fallthrough only; no branch, call, return or user op) writes the flag again before reading
        # it. Then the temporaries only dead ops used go too (unique space is per instruction). Writes before any exit
        # stay, so a function leaves the same flags as before (what the Unicorn checker compares).
        flags, exits = (0x200, 0x202, 0x204, 0x206, 0x207, 0x20b), {"BRANCH", "CBRANCH", "BRANCHIND", "CALL", "CALLIND",
                                                                  "RETURN", "CALLOTHER"}
        covers = lambda v, f: v is not None and v[0] == "r" and v[1] <= f < v[1] + v[2]
        def overwritten(n, i, f):
            while True:
                ops = self.ins[n][4]
                for m, o, a in ops[i + 1:]:
                    if m in exits or any(covers(v, f) for v in a): return False
                    if covers(o, f): return True
                ft = self.ins[n][1]
                if ft is None or n + 1 >= len(self.ins) or self.ins[n + 1][0] != ft: return False
                n, i = n + 1, -1
        dead = set()
        for n, (_, _, _, _, ops) in enumerate(self.ins):
            for i, (m, o, a) in enumerate(ops):
                if o is not None and o[0] == "r" and o[2] == 1 and o[1] in flags and m not in exits and overwritten(n, i, o[1]):
                    dead.add((n, i))
            live = []   # unique (offset, size) ranges still read later in this instruction
            for i in range(len(ops) - 1, -1, -1):
                m, o, a = ops[i]
                if (n, i) not in dead and o is not None and o[0] == "u" and m not in exits and m != "STORE" and \
                        not any(s < o[1] + o[2] and o[1] < s + z for s, z in live):
                    dead.add((n, i))
                if (n, i) in dead: continue
                live += [(v[1], v[2]) for v in a if v is not None and v[0] == "u"]
        return dead

    def emit(self):
        self.need, self.oplabels = {self.entry}, set()
        body = []
        dead = self.dead_ops()
        for n, (addr, ft, flows, text, ops) in enumerate(self.ins):
            self.flows = flows
            lines = ["" if (n, i) in dead else self.safe_op(addr, i, len(ops), *o) for i, o in enumerate(ops)]
            self.x87_stack_moves(ops, lines)
            nxt = self.ins[n + 1][0] if n + 1 < len(self.ins) else None
            tail = ""
            if ft is not None and ft != nxt: tail = self.goto(ft)
            elif ft is None and (not ops or ops[-1][0] not in ("BRANCH", "BRANCHIND", "RETURN")):
                tail = f'RS_ rt_unimpl("fell off", 0x{addr:x}); return;'
            body.append((addr, text, lines, tail))
        cell = lambda b: f"V(c->r+0x{b:x},{CT[self.cached[b]]})"
        regs = sorted(self.touched)
        out = ["#define RS_ " + " ".join(f"{cell(b)} = g{b:x};" for b in sorted(self.written)),
               "#define RL_ " + " ".join(f"g{b:x} = {cell(b)};" for b in regs),
               f"void f_{self.entry:x}(CPU *c) {{", f"  uint8_t u[{self.usize}] __attribute__((aligned(16)));"]
        out += [f"  {CT[self.cached[b]]} g{b:x} = {cell(b)};" for b in regs]
        if self.ins and self.ins[0][0] != self.entry: out.append(f"  goto L_{self.entry:x};")
        for addr, text, lines, tail in body:
            if addr in self.need: out.append(f"L_{addr:x}:;")
            out.append(f"  // {addr:x}: {text}")
            out.append(f"  TR(0x{addr:x});")
            for i, l in enumerate(lines):
                if (addr, i) in self.oplabels: out.append(f"L_{addr:x}_{i}:;")
                if l: out.append("  " + l)
            if (addr, len(lines)) in self.oplabels: out.append(f"L_{addr:x}_{len(lines)}:;")
            if tail: out.append("  " + tail)
        out += ["}", "#undef RS_", "#undef RL_"]
        return "\n".join(out)


def emit_chunk(job):   # one lifted_NNN.c (~64 KB of x86); runs in a worker process
    n, addrs = job
    dump, out_dir, idx, ins, lset, userops = G
    code = [FnGen(a, idx[a], read_fn(dump, a, idx, ins), lset, userops).emit() for a in addrs]
    open(os.path.join(out_dir, f"lifted_{n:03d}.c"), "w").write('#include "lifted.h"\n' + "\n".join(code) + "\n")


def main():
    dump, out_dir = sys.argv[1], sys.argv[2]
    idx, userops, ins = load_index(dump)
    if sys.argv[3].startswith("sample:"):  # sample:N[:seed] = N random roots whose closure stays small
        _, n, *seed = sys.argv[3].split(":")
        rng, roots = random.Random(int(seed[0]) if seed else 1), []
        for a in rng.sample(sorted(idx), len(idx)):
            if len(roots) == int(n): break
            if len(closure(idx, [a])) <= 40: roots.append(a)
    elif sys.argv[3] == "all":
        roots = list(idx)
    else:
        roots = [int(r, 16) for r in sys.argv[3:]]
    lifted = closure(idx, roots)
    if sys.argv[3] == "all":   # native.txt: functions the host replaces; calls to them become rt_call -> host
        native = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "native.txt")).read()
        lifted -= {int(l.split()[0], 16) for l in native.splitlines() if l.strip() and not l.startswith("#")}
    lifted = sorted(lifted)
    lset = set(lifted)
    os.makedirs(out_dir, exist_ok=True)
    for old in os.listdir(out_dir):
        if old.startswith("lifted_") and old.endswith(".c"): os.remove(os.path.join(out_dir, old))
    # Split into ~64 KB-of-x86 chunks so the C compiles in parallel and no single file is huge.
    open(os.path.join(out_dir, "lifted.h"), "w").write(
        '#include "rt.h"\n' + "".join(f"void f_{a:x}(CPU *c);\n" for a in lifted))
    chunks, size = [[]], 0
    for a in lifted:
        chunks[-1].append(a)
        size += sum(hi - lo + 1 for lo, hi in idx[a]["body"])
        if size >= 0x10000: chunks.append([]); size = 0
    chunks = [ch for ch in chunks if ch]
    global G
    G = (dump, out_dir, idx, ins, lset, userops)   # inherited by the forked workers (no pickling of the index)
    import multiprocessing
    with multiprocessing.get_context("fork").Pool(os.cpu_count()) as pool:
        pool.map(emit_chunk, enumerate(chunks))
    n = len(chunks)
    open(os.path.join(out_dir, "lifted_table.c"), "w").write(
        '#include "lifted.h"\nconst FnEntry fn_table[] = {' + ", ".join(f"{{0x{a:x}, f_{a:x}}}" for a in lifted) + "};\n"
        f"const uint32_t fn_count = {len(lifted)};\n")
    json.dump({f"{a:x}": idx[a]["body"] for a in lifted}, open(os.path.join(out_dir, "manifest.json"), "w"))
    print(f"lifted {len(lifted)} functions -> {out_dir}/lifted_*.c ({n} chunks)")


if __name__ == "__main__":
    main()
