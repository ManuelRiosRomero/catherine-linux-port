#!/usr/bin/env python3
"""Every Steam method the game can call, found without running it: the game gets its interfaces from the
CSteamAPIContext (SteamInternal_ContextInit, IAT 0xae2870: returns the context in EAX), loads one (MOV r,[EAX+field])
and calls a vtable slot (MOV r2,[r]; CALL [r2+slot], or CALL r2 after MOV r2,[r2+slot]). Slot -> method name comes
from steam_api.dll's flat exports (SteamAPI_ISteamX_Method loads the same vtable offset). Methods already in
runtime/host/steam.c's spec table are marked; the rest would stop the game when reached.
Usage: steam_calls.py <dump.pcode.txt> <steam_api.dll> <steam.c>"""
import re, struct, subprocess, sys, collections

dump, dll, steam_c = sys.argv[1:4]
CTX = {0x0: "SteamClient", 0x4: "SteamUser", 0x8: "SteamFriends", 0xc: "SteamUtils", 0x10: "SteamMatchmaking",
       0x14: "SteamUserStats", 0x18: "SteamApps", 0x1c: "SteamMatchmakingServers", 0x20: "SteamNetworking",
       0x24: "SteamRemoteStorage", 0x28: "SteamScreenshots", 0x2c: "SteamHTTP", 0x30: "SteamController",
       0x34: "SteamUGC", 0x38: "SteamAppList", 0x3c: "SteamMusic", 0x40: "SteamMusicRemote", 0x44: "SteamHTMLSurface",
       0x48: "SteamInventory", 0x4c: "SteamVideo", 0x50: "SteamParentalSettings"}

# ---- slot -> method from the flat exports
d = open(dll, "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]; opt = pe + 24
n, osz = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
secs = [struct.unpack_from("<IIII", d, opt + osz + 40 * i + 8) for i in range(n)]
off = lambda rva: next(raw + rva - va for vs, va, rs, raw in secs if va <= rva < va + rs)
u32 = lambda o: struct.unpack_from("<I", d, o)[0]
e = off(u32(opt + 96)); nn, fa, na, oa = (u32(e + k) for k in (24, 28, 32, 36))
names = {}
for i in range(nn):
    name = d[off(u32(off(na) + 4 * i)):].split(b"\0")[0].decode()
    m = re.match(r"SteamAPI_ISteam(\w+?)_(\w+)$", name)
    if not m: continue
    rva = u32(off(fa) + 4 * struct.unpack_from("<H", d, off(oa) + 2 * i)[0])
    open("/tmp/sf.bin", "wb").write(d[off(rva):off(rva) + 64])
    dis = subprocess.run(["objdump", "-D", "-b", "binary", "-m", "i386", "/tmp/sf.bin"], capture_output=True, text=True).stdout
    # the vtable load: "mov (%ecx),%eax" then the first "0xNN(%eax)" (mov/call/jmp); slot 0 = "*(%eax)"
    lines = dis.splitlines()
    for k, l in enumerate(lines):
        if re.search(r"mov\s+\(%e[a-d]x\),%e[a-d]x", l):
            for l2 in lines[k + 1:k + 8]:
                mm = re.search(r"(?:mov|call|jmp)\s+\*?(0x[0-9a-f]+)?\(%e[a-d]x\)", l2)
                if mm: names.setdefault("Steam" + m.group(1), {})[int(mm.group(1) or "0", 16) // 4] = m.group(2); break
            break

# ---- call sites in the game
calls = collections.defaultdict(set)   # (iface, slot) -> sites
ins = [l.rstrip("\n").split(" | ", 1) for l in open(dump, encoding="latin1") if l.startswith("INS ")]
ctx_reg = {}   # register -> index of "MOV reg,[0xae2870]" (the context function kept in a register, called repeatedly)
for i, (head, text) in enumerate(ins):
    m = re.fullmatch(r"MOV (E[A-D]X|ESI|EDI|EBX|EBP),dword ptr \[0x00ae2870\]", text)
    if m: ctx_reg[m.group(1)] = i; continue
    via_reg = (m := re.fullmatch(r"CALL (E[A-D]X|ESI|EDI|EBX|EBP)", text)) and i - ctx_reg.get(m.group(1), -10**9) < 400
    if text != "CALL dword ptr [0x00ae2870]" and not via_reg: continue
    field, vt_reg, obj_reg = None, None, None
    for head2, t in ins[i + 1:i + 24]:
        a = int(head2.split()[1], 16)
        m = re.fullmatch(r"MOV (E[A-D]X|ESI|EDI),dword ptr \[EAX \+ 0x([0-9a-f]+)\]", t) or \
            re.fullmatch(r"MOV (E[A-D]X|ESI|EDI),dword ptr \[EAX\]", t)
        if m and field is None:
            obj_reg, field = m.group(1), int(m.group(2), 16) if m.lastindex == 2 else 0; continue
        if obj_reg and (m := re.fullmatch(rf"MOV (E[A-D]X|ESI|EDI),dword ptr \[{obj_reg}\]", t)): vt_reg = m.group(1); continue
        if vt_reg and (m := re.fullmatch(rf"CALL dword ptr \[{vt_reg}(?: \+ 0x([0-9a-f]+))?\]", t)):
            calls[(field, int(m.group(1) or "0", 16) // 4)].add(a); break
        if vt_reg and (m := re.fullmatch(rf"MOV E[A-D]X,dword ptr \[{vt_reg}(?: \+ 0x([0-9a-f]+))?\]", t)):
            calls[(field, int(m.group(1) or "0", 16) // 4)].add(a); break
        if t.startswith("RET") or t.startswith("CALL dword ptr [0x00ae2870]"): break

# spec names: "SteamUser019" or "STEAMUSERSTATS_INTERFACE_VERSION011" -> "steamuser" / "steamuserstats"
canon = lambda s: re.sub(r"(_INTERFACE_VERSION)?\d+$", "", s).lower()
bridged = {(canon(m.group(1)), int(m.group(2))) for m in re.finditer(r'\{"(\w+)", (\d+),', open(steam_c).read())}
for (field, slot), sites in sorted(calls.items()):
    iface = CTX.get(field, f"ctx+{field:x}")
    meth = names.get(iface, {}).get(slot, "?")
    mark = "bridged" if (iface.lower(), slot) in bridged else "MISSING"
    print(f"{mark:8s} {iface}#{slot:<3d} {meth:40s} {len(sites)} sites, e.g. {' '.join(f'{s:x}' for s in sorted(sites)[:3])}")
