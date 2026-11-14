#!/usr/bin/env python3
"""Static readiness report: every known place the game can stop on, found without running it.
  1. Windows functions it imports that have no replacement in runtime/host  (from `CATH_REPORT=1 catherine`)
  2. calls/jumps in the translated code to addresses that are not translated functions
  3. CPU instructions the translator leaves unimplemented (rt_unimpl)
  4. holes: real code flows into bytes Ghidra never decoded (repair: tools/ghidra/FixHoles.java)
Not covered (they stop with a clear message at runtime): COM / Steam methods called through vtables, GetProcAddress.
A gap only counts when real code can reach it: from the entry points, the DLLs' exports and every code pointer the
images store (their base relocations: vtables, callbacks, jump tables), following calls and jumps. The rest is data
the disassembler decoded as junk code (the Microsoft DLLs keep data inside their code section).
Usage: readiness.py <dump.pcode.txt> <lifted dir> <missing-imports.txt> <native.txt> <report.txt> <summary.json> <image>..."""
import collections, json, re, struct, sys, os, time

dump, lifted, missing_txt, native_txt, out_txt, out_json = sys.argv[1:7]
images = sys.argv[7:]

# ---- the dump: instructions, function entries, flow, IAT call sites
starts, fns, flows, iat_calls, direct_calls = set(), set(), [], collections.defaultdict(list), []
fn_of = {}   # instruction -> function entry (last FN seen)
# instructions compilers never emit in 32-bit user code: where they sit right at a "function" entry, it is data
# Ghidra decoded as code ("ADD byte ptr [EAX],AL" = 00 00; INT3 = padding)
JUNK = {"IN", "OUT", "INSB", "INSW", "INSD", "OUTSB", "OUTSW", "OUTSD", "INTO", "INT3", "HLT", "LDS", "LES", "ARPL",
        "BOUND", "SLDT", "STR", "LLDT", "LTR", "SGDT", "SIDT", "LGDT", "LIDT", "CLI", "STI", "AAA", "AAS", "DAA", "DAS",
        "AAM", "AAD", "SALC", "IRET", "IRETD", "RETF", "CALLF", "JMPF"}
junk_ins, has_ret, noret_calls, fn_name = set(), set(), {}, {}   # noret_calls: CALL without a fallthrough -> (target, [iat] slot)
cur = None
for line in open(dump, encoding="latin1"):
    if line.startswith("FN "):
        f = line.split(); cur = int(f[1], 16); fns.add(cur)
        ended = False   # a RET/JMP after the entry seen: INT3 padding after a tiny function is not junk
        if not f[2].startswith("FUN_"): fn_name[cur] = f[2].lstrip("_")
    elif line.startswith("INS "):
        head, text = line.rstrip("\n").split(" | ", 1)
        f = head.split()
        a = int(f[1], 16); starts.add(a); fn_of[a] = cur
        for t in f[3:5]:
            if t != "-": flows += [(a, int(x, 16)) for x in t.split(",")]
        for m in re.finditer(r"\[0x([0-9a-f]{8})\]", text):   # CALL [iat], or MOV reg,[iat] then CALL reg
            iat_calls[int(m.group(1), 16)].append(a)
        mn = text.split(" ", 1)[0]
        if mn == "CALL" and re.fullmatch(r"CALL 0x[0-9a-f]+", text): direct_calls.append((a, int(text[5:], 16)))
        # ES/CS/SS/DS as an operand (not a "DS:" prefix): flat 32-bit code never moves segment registers (FS = TEB is fine)
        if cur is not None and cur <= a < cur + 16 and not ended and (
                mn in JUNK or text == "ADD byte ptr [EAX],AL" or re.search(r"\b[ECSD]S\b(?!:)", text)): junk_ins.add(a)
        if cur is not None and a >= cur and mn in ("RET", "JMP"): ended = True
        if mn == "RET": has_ret.add(cur)
        elif mn == "CALL" and f[3] == "-":
            m = re.fullmatch(r"CALL dword ptr \[0x([0-9a-f]{8})\]", text)
            noret_calls[a] = (int(f[4], 16) if f[4] != "-" and not m else None, int(m.group(1), 16) if m else None)
lo, hi = min(starts), max(starts)
junk_fns = {fn_of[a] for a in junk_ins}

# ---- reachability. Roots: entry points, TLS callbacks, DLL exports something imports. Edges: calls/jumps, function
# addresses an instruction mentions (callbacks), and tables of function pointers an instruction mentions (vtables).
mem = {}   # page (va >> 12) -> (bytes, offset of that page in them)
class Img:
    def __init__(s, path):
        d = s.d = open(path, "rb").read()
        pe = struct.unpack_from("<I", d, 0x3C)[0]; s.opt = opt = pe + 24
        n, osz = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
        s.base = struct.unpack_from("<I", d, opt + 28)[0]
        s.secs = [struct.unpack_from("<IIII", d, opt + osz + 40 * i + 8) for i in range(n)]
        for vs, va, rs, raw in s.secs:
            for p in range(0, min(vs or rs, rs), 0x1000): mem[(s.base + va + p) >> 12] = (d, raw + p)
    def off(s, rva):
        for vs, va, rs, raw in s.secs:
            if va <= rva < va + rs: return raw + rva - va
    def u32(s, o): return struct.unpack_from("<I", s.d, o)[0]
    def cstr(s, o): return s.d[o:s.d.index(b"\0", o)].decode("latin1")
    def dirent(s, i): return struct.unpack_from("<II", s.d, s.opt + 96 + 8 * i)
    def exports(s):   # name -> address
        rva, _ = s.dirent(0)
        if not rva: return {}
        e = s.off(rva); nn, fa, na, oa = (s.u32(e + k) for k in (24, 28, 32, 36))
        return {s.cstr(s.off(s.u32(s.off(na) + 4 * i))): s.base + s.u32(s.off(fa) + 4 * struct.unpack_from("<H", s.d, s.off(oa) + 2 * i)[0])
                for i in range(nn)}
    def imports(s):   # imported names and their IAT slots: [(name, slot address)]
        rva, _ = s.dirent(1); out = []
        d = s.off(rva) if rva else None
        while d is not None and s.u32(d + 12):
            names, iat = s.u32(d) or s.u32(d + 16), s.u32(d + 16); k = 0
            while s.u32(s.off(names) + 4 * k):
                th = s.u32(s.off(names) + 4 * k)
                if not th >> 31: out.append((s.cstr(s.off(th) + 2), s.base + iat + 4 * k))
                k += 1
            d += 20
        return out
    def roots(s):
        r = {s.base + s.u32(s.opt + 16)}
        rva, _ = s.dirent(9)                                   # TLS callbacks
        if rva:
            cb = s.u32(s.off(rva) + 12)
            while cb and s.off(cb - s.base) is not None and s.u32(s.off(cb - s.base)):
                r.add(s.u32(s.off(cb - s.base))); cb += 4
        return r
def dword(va):
    p = mem.get(va >> 12)
    if p and (va & 0xFFF) <= 0xFFC: return struct.unpack_from("<I", p[0], p[1] + (va & 0xFFF))[0]
imgs = [Img(p) for p in images]
iat_name = {slot: n for i in imgs for n, slot in i.imports()}
imported = set(iat_name.values())
roots = {r for i in imgs for r in i.roots()} | {a for i in imgs for n, a in i.exports().items() if n in imported}
edges, flow_edges = collections.defaultdict(set), set()
for a, t in flows:
    if fn_of.get(a) is not None and t in fn_of:
        g = fn_of[t] if t not in fns else t
        edges[fn_of[a]].add(g); flow_edges.add((fn_of[a], g))
consts = collections.defaultdict(set)   # function -> (address its instructions mention, read from memory?)
for line in open(dump, encoding="latin1"):
    if line.startswith("INS "):
        a = int(line.split()[1], 16)
        if fn_of.get(a) is None: continue
        for m in re.finditer(r"\[[^\]]*\]|0x([0-9a-f]{6,8})\b", line.split(" | ", 1)[1]):
            if m.group(1): consts[fn_of[a]].add((int(m.group(1), 16), False))
            else: consts[fn_of[a]] |= {(int(c, 16), True) for c in re.findall(r"0x([0-9a-f]{6,8})\b", m.group(0))}
for f, cs in consts.items():
    for c, mem_operand in cs:
        # [addr] reads data at addr: a function pointer stored there counts, addr itself is no code (an import slot
        # Ghidra decoded as a junk "function" would otherwise pull in junk: d3dx9 keeps its IAT inside .text)
        if c in fns and not mem_operand: edges[f].add(c); continue
        if c in starts: continue                                 # jump tables: flows already cover them
        for k in range(512):                                     # a table of function pointers (vtable, callbacks)
            v = dword(c + 4 * k)
            if v not in fns: break
            edges[f].add(v)
natives = {int(l.split()[0], 16) for l in open(native_txt) if l.strip() and not l.startswith("#")}
reach, parent, todo = set(), {}, collections.deque(r for r in roots if r in fns)   # breadth first: shortest "via" chains
into_junk = {}   # junk "function" -> real function that calls/jumps into it (a mention of its address doesn't count)
while todo:
    f = todo.popleft()
    if f in reach or f in natives: continue   # the host replaces natives: their original code never runs
    reach.add(f)
    for g in edges[f]:
        if g in junk_fns:
            if (f, g) in flow_edges and f not in junk_fns: into_junk.setdefault(g, f)
            continue
        if g not in reach and g not in parent and g not in roots: parent[g] = f; todo.append(g)
live = lambda a: fn_of.get(a) in reach            # an instruction address inside reachable code
def via(a):   # how reachable code gets to the function holding a: "root>...>fn", at most 4 hops shown
    f, chain = fn_of.get(a, a), []
    while f is not None and len(chain) < 12: chain.append(f); f = parent.get(f)
    return ">".join(f"{x:x}" for x in (chain[::-1] if len(chain) <= 4 else chain[-1:] + chain[3::-1]))

# ---- 1. missing imports, ranked by call sites
missing = []
for line in open(missing_txt):
    if line.startswith("missing-import "):
        _, name, iat = line.split()
        sites = [a for a in iat_calls.get(int(iat, 16), []) if live(a)]
        if sites: missing.append((name, len(sites), sites[:3]))   # never used by reachable code: not a gap
missing.sort(key=lambda m: -m[1])

# ---- 2 + 3. the translated code
natives = {int(l.split()[0], 16) for l in open(native_txt) if l.strip() and not l.startswith("#")}
defined, targets, unimpl, fell_off = set(), collections.defaultdict(list), collections.defaultdict(list), []
for name in sorted(os.listdir(lifted)):
    if not (name.startswith("lifted_") and name.endswith(".c")): continue
    fn, at = None, None
    for line in open(os.path.join(lifted, name), encoding="latin1"):
        m = re.match(r"void f_([0-9a-f]+)\(CPU \*c\) \{", line)
        if m: fn = int(m.group(1), 16); defined.add(fn); continue
        m = re.match(r"\s*// ([0-9a-f]+): ", line)
        if m: at = int(m.group(1), 16); continue
        for t in re.findall(r"rt_(?:call|jump)\(c, 0x([0-9a-f]+)", line):
            targets[int(t, 16)].append(at)
        m = re.search(r'rt_unimpl\("([^"]+)", 0x([0-9a-f]+)', line)
        if m and m.group(1) == "fell off": fell_off.append((fn, int(m.group(2), 16)))
        elif m: unimpl[m.group(1)].append(fn)
stub = lambda t: t >= 0xFFC00000   # import / Steam / D3D9 stubs and COM vtables the host hands out
bad_targets = {t: [a for a in s if live(a)] for t, s in targets.items() if t not in defined and t not in natives and not stub(t)}
bad_targets = {t: s for t, s in bad_targets.items() if s}
unimpl = {k: [f for f in v if f in reach] for k, v in unimpl.items()}
unimpl = {k: v for k, v in unimpl.items() if v}
null_jumps = bad_targets.pop(0, [])
outside = {t: s for t, s in bad_targets.items() if not lo <= t <= hi}
inside = {t: s for t, s in bad_targets.items() if lo <= t <= hi}

# "fell off": code ends after a CALL Ghidra judged never to return. Harmless when it really never returns: a known
# no-return import (exit, throw, std::_Xlength_error, ...) or a function without any RET. Anything else is missing code.
NORETURN = {"exit", "_exit", "abort", "ExitProcess", "ExitThread", "_endthread", "_endthreadex", "longjmp",
            "_CxxThrowException", "_invalid_parameter_noinfo_noreturn", "_invoke_watson", "__std_terminate",
            "?terminate@@YAXXZ", "_amsg_exit", "RaiseException", "FatalAppExitA", "quick_exit", "_purecall"}
# statically linked CRT functions that end the process but keep a RET Ghidra can see (they end in INT 0x29 = __fastfail)
NORETURN_CRT = {"report_rangecheckfailure", "report_gsfailure", "raise_securityfailure", "invalid_parameter_noinfo_noreturn",
                "invoke_watson", "report_securityfailure", "report_securityfailureEx", "scrt_fastfail"}
def noreturn_ok(a):
    if a not in noret_calls: return False
    t, slot = noret_calls[a]
    if slot is not None: n = iat_name.get(slot, ""); return n in NORETURN or n.startswith("?_X")   # MSVCP ?_Xlength_error...
    return t in fns and (t not in has_ret or fn_name.get(t) in NORETURN_CRT)
fell_off = [(f, a) for f, a in fell_off if f in reach]
fell_bad = [(f, a) for f, a in fell_off if not noreturn_ok(a)]
if fell_bad: unimpl["fell off (not after a no-return call)"] = [f for f, a in fell_bad]

# Not gaps:
#  - 3DNow! (Ghidra: Packed*, FEMMS = FastExitMediaState, PFRCP = FloatingReciprocalAprox): AMD-only, gone from every CPU
#    since 2011 incl. the Deck's, so code behind the CPUID check never runs; every other op in such a function is dead too
#  - XGETBV (xinuse): only runs when CPUID reports OSXSAVE, which rt_cpuid hides
#  - INT3 / INT 0x29 (swi): breakpoint / __fastfail: the original stops there as well
is_3dnow = lambda k: k.startswith("Packed") or k in ("FastExitMediaState", "FloatingReciprocalAprox")
fns_3dnow = {f for k, v in unimpl.items() if is_3dnow(k) for f in v}
not_gaps = {"xinuse", "swi"}
live_unimpl = {k: [f for f in v if f not in fns_3dnow] for k, v in unimpl.items() if not is_3dnow(k) and k not in not_gaps}
live_unimpl = {k: v for k, v in live_unimpl.items() if v}
dead_uses = sum(len(v) for v in unimpl.values()) - sum(len(v) for v in live_unimpl.values())

# ---- 4. holes
hole_src = {t: a for a, t in flows if t not in starts and lo <= t <= hi and live(a)}
holes = sorted(hole_src)
reviewed_txt = os.path.join(os.path.dirname(os.path.abspath(__file__)), "holes_reviewed.txt")
reviewed = {int(l.split()[0], 16) for l in open(reviewed_txt) if l.strip() and not l.startswith("#")}
fixable = [t for t in holes if t not in reviewed]   # all live holes; FixHoles.java repairs those with a junk entry just after
# A direct CALL from real (non-junk) code into bytes with no decoded instruction, reachable or not: the traversal misses
# code reached only through pointers it can't follow (0x684ac7 -> 0x67ba50 hid behind a switch table decoded as code
# and stopped the game in the Night 8 bar). A call target is always a function entry, so this is never harmless.
call_holes = {t: a for a, t in direct_calls if t not in starts and lo <= t <= hi and fn_of.get(a) not in junk_fns
              and t not in hole_src and t not in reviewed}
fixable += sorted(call_holes)

gaps = len(missing) + len(inside) + len(live_unimpl) + len(fixable) + len(into_junk)
with open(out_txt, "w") as o:
    w = lambda s="": o.write(s + "\n")
    w(f"Readiness report  {time.strftime('%Y-%m-%d %H:%M')}  (static: nothing was run)")
    w(f"Known gaps the game can stop on: {gaps}   (reachable code: {len(reach)} of {len(fns)} functions)")
    w(f"  missing Windows functions {len(missing)}, calls to untranslated addresses {len(inside)}, "
      f"unimplemented instruction kinds {len(live_unimpl)}, fixable holes {len(fixable)}, "
      f"real code running into junk {len(into_junk)}")
    w(f"  (left out as junk: {len(junk_fns)} \"functions\" that start with instructions compilers never emit, i.e. "
      f"data decoded as code; {len(fell_off) - len(fell_bad)} of {len(fell_off)} \"fell off\" ends follow a no-return call)")
    w()
    w(f"1. Imported Windows functions without a replacement: {len(missing)}")
    for name, n, sites in missing:
        w(f"   {name:60s} {n:3d} call sites  e.g. {' '.join(f'{s:x}' for s in sites)}  via {via(sites[0])}")
    w()
    w(f"2. Calls/jumps to addresses that are not translated functions: {len(inside)} inside the code")
    for t, s in sorted(inside.items(), key=lambda x: (-len(x[1]), x[0])):
        w(f"   {t:08x}  from {len(s)} places, e.g. {' '.join(f'{a:x}' for a in s[:3] if a)}  via {via(s[0])}")
    w(f"   (also {len(outside)} targets outside any image and {len(null_jumps)} jumps to address 0: data the "
      f"disassembler took for code, never executed)")
    w()
    w(f"3. CPU instructions the translator leaves unimplemented: {len(live_unimpl)} kinds")
    for k, v in sorted(live_unimpl.items(), key=lambda x: -len(x[1])):
        w(f"   {k:45s} in {len(set(v)):4d} functions, e.g. {' '.join(f'{f:x}' for f in sorted(set(v))[:4] if f)}  via {via(min(v))}")
    if fell_bad: w(f"   fell off at: {' '.join(f'{a:x}' for f, a in sorted(fell_bad))}")
    w(f"   (not counted: {dead_uses} uses in {len(fns_3dnow)} 3DNow! functions (the Deck's CPU has no 3DNow!), "
      f"XGETBV (CPUID hides OSXSAVE), INT3 / INT 0x29 (the original stops there too))")
    w()
    w(f"4. Holes (reachable code flows, or any real code calls, into bytes Ghidra never decoded): {len(fixable)} "
      f"(not counted: {len(holes) - len(fixable) + len(call_holes)} reviewed as padding/data in holes_reviewed.txt)")
    for t in fixable:
        how = "FixHoles.java" if any(t < f <= t + 15 for f in fns) else "mis-aligned decode, no junk entry: disassemble by hand"
        src = hole_src.get(t, call_holes.get(t))
        w(f"   {t:08x}  from {src:x}  via {via(src) if live(src) else 'a CALL in code the traversal does not reach'}  ({how})")
    w()
    w(f"5. Real code calling/jumping into a junk-looking \"function\": {len(into_junk)} (wrong decode, or the caller is junk too)")
    for g, f in sorted(into_junk.items()): w(f"   {g:08x}  from {f:x}  via {via(f)}")
    w()
    w("Not covered statically (each stops with a clear message): COM and Steam methods called through vtables,")
    w("functions looked up by name at runtime (GetProcAddress).")
json.dump({"gaps": gaps, "missing_imports": len(missing), "untranslated_targets": len(inside),
           "unimplemented_kinds": len(live_unimpl), "fixable_holes": len(fixable), "into_junk": len(into_junk),
           "time": time.strftime("%Y-%m-%d %H:%M")}, open(out_json, "w"))
print(open(out_txt).read())
