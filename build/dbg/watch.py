import sys, random
sys.path.insert(0, "tools/recomp")
import check as C
from unicorn import UC_HOOK_MEM_WRITE
from unicorn import x86_const as X
ck = C.Checker(sys.argv[1], "gen/sample")
W = 0x100effc7
hits = []
ck.uc.hook_add(UC_HOOK_MEM_WRITE, lambda uc, acc, a, s, v, _: hits.append((uc.reg_read(X.UC_X86_REG_EIP), a, s, v)) if a <= W < a + s else None)
rng = random.Random(0xacd8c0)
for t in range(10):
    hits.clear()
    k, why = ck.trial(0xacd8c0, rng)
    if k == "FAIL": print(t, why); [print(f"  eip={e:x} addr={a:x} size={s} val={v & ((1 << 8*s) - 1):x}") for e, a, s, v in hits]
