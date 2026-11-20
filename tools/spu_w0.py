#!/usr/bin/env python3
"""SPU lift pass, run last: preferred-slot scalar reads of a cached register,
S_n._u32[0] / S_n._u16[0], become SPU_W0(S_n) / SPU_H0(S_n) (spu_helpers.h).

The field read made clang SROA-split every such local into i32 + <3 x i32>, so
each vector op paid insert/extract and wrote ctx->gpr back as split stores
(store-forwarding stalls on the next 16-byte load). The macro extracts lane 0
from the whole vector: same value, one xmm. Only rvalue uses exist (an lvalue
use stops the pass). Earlier passes (spu_loop_ib, hooks) match the plain text,
so this one must run after them. Idempotent.

    spu_w0.py in.c [out.c]
"""
import re, sys

RD = re.compile(r'\bS_(\d+)\._u(32|16)\[0\]')
LV = re.compile(r'\bS_\d+\._u(?:32|16)\[0\]\s*(?:=(?!=)|[-+|&^*/%]=|<<=|>>=|\+\+|--)')


def rewrite(t):
    m = LV.search(t)
    if m: raise SystemExit('spu_w0: lvalue use: ' + t[max(0, m.start() - 60):m.end() + 20])
    return RD.sub(lambda m: 'SPU_%s0(S_%s)' % ('W' if m.group(2) == '32' else 'H', m.group(1)), t)


if __name__ == '__main__':
    inp = sys.argv[1]; out = sys.argv[2] if len(sys.argv) > 2 else inp
    t = open(inp, encoding='utf-8', errors='surrogateescape', newline='').read()
    open(out, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(rewrite(t))
