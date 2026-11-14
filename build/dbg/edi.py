import sys, random
sys.path.insert(0, "tools/recomp")
import check as C
from unicorn import UC_HOOK_CODE
from unicorn import x86_const as X
ck = C.Checker(sys.argv[1], "gen/sample")
log = []
ck.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, _: log.append((a, uc.reg_read(X.UC_X86_REG_EDI), bytes(uc.mem_read(a, s)).hex())))
rng = random.Random(0xacd8c0)
for t in range(10):
    log.clear()
    k, why = ck.trial(0xacd8c0, rng)
    if t == 8:
        prev, pa = None, None
        for a, edi, b in log:
            if edi != prev: print(f"after {pa} -> EDI={edi:x}")
            prev, pa = edi, f"{a:x} {b}"
            if a == 0x7bcac2: break
