/* Native Edge zlib inflate_fast for the R2/R3 edgezlib_inflate_task SPU image.
 * See native_edge_zlib.h and scratch/edge_zlib_native.md.
 *
 * Exact emulation of the SPU routine (zlib 1.2.3 inflate_fast as rewritten
 * by Edge for the SPU). Faithful details that matter for bit-exactness:
 *  - the bit reader is a 128-bit register (r28) holding byte-reversed input,
 *    refilled from two aligned quadwords when more than 112 bits were used;
 *    the input position is the triple (r27 base, r26 bit offset, r33 used);
 *  - a literal is not stored where it is decoded: the NEXT loop iteration
 *    loads the output quadword, inserts the byte and stores the quad to r30,
 *    which is the literal's address after a literal and a fixed scratch
 *    quadword (the "dummy", r21) after anything else -- so every iteration
 *    writes 16 bytes somewhere, and that write happens before the loop exit
 *    test;
 *  - copies of <= 16 / > 16 bytes with len <= dist are quadword
 *    read-modify-writes (16 source bytes read before the write); overlapping
 *    copies and window copies are byte loops with two different load/store
 *    orders;
 *  - all LS accesses wrap at 256 KB, quad accesses ignore the low 4 bits,
 *    word fields are read by rotating their quadword (wrap inside the quad)
 *    and written to (addr & 0xC) of their quadword;
 *  - shl counts use 6 bits (>= 32 gives 0).
 * Implementation is original (no zlib source); zlib (C) Jean-loup Gailly and
 * Mark Adler, zlib licence -- this file reproduces the behaviour of the zlib
 * 1.2.3 derived Edge code that ships in the game's SPU image. */
#include "native_edge_zlib.h"
#include <string.h>
#include <stdlib.h>

#ifndef NEZ_STAT
#define NEZ_STAT(i) ((void)0)   /* test builds count path coverage */
#endif

#define NEZ_LSM 0x3FFFFu
#define NEZ_QM  0x3FFF0u

const native_edge_zlib_desc native_edge_zlib_r3 = {
    "r3", 0x8100u, 0x36B30u, 0xC1A0u, 0xC1B0u, 0xC1D0u, 0xC1F0u };
const native_edge_zlib_desc native_edge_zlib_r2 = {
    "r2", 0x8240u, 0x0BC90u, 0xBA90u, 0xBAA0u, 0xBAC0u, 0xBAE0u };

typedef struct { uint64_t hi, lo; } nez_q;   /* SPU quad as a big-endian 128-bit value */

static inline uint32_t nez_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint64_t nez_le64(const uint8_t* p)
{
    uint64_t v; memcpy(&v, p, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap64(v);
#endif
    return v;
}
/* word0 of rotqby(lq(a), a): 4 bytes from a, wrapping inside a's quadword */
static inline uint32_t nez_rd32(const uint8_t* ls, uint32_t a)
{
    const uint32_t b = a & NEZ_QM, o = a & 15u;
    if (o <= 12u) return nez_be32(ls + b + o);
    return ((uint32_t)ls[b + o] << 24) | ((uint32_t)ls[b + ((o + 1) & 15u)] << 16) |
           ((uint32_t)ls[b + ((o + 2) & 15u)] << 8) | ls[b + ((o + 3) & 15u)];
}
/* cwd/cwx + stq: word v at (a & 0xC) of a's quadword */
static inline void nez_wr32(uint8_t* ls, uint32_t a, uint32_t v)
{
    uint8_t* p = ls + (a & NEZ_QM) + (a & 12u);
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline uint32_t nez_shl1m(uint32_t c)      /* shl(1, c) - 1, SPU 6-bit count */
{
    c &= 63u;
    return (c > 31u ? 0u : (1u << c)) - 1u;
}
static inline nez_q nez_rotl(nez_q x, unsigned n)  /* n in 0..127 */
{
    if (n & 64u) { const uint64_t t = x.hi; x.hi = x.lo; x.lo = t; }
    n &= 63u;
    if (n) {
        const uint64_t h = (x.hi << n) | (x.lo >> (64u - n));
        x.lo = (x.lo << n) | (x.hi >> (64u - n));
        x.hi = h;
    }
    return x;
}
static inline nez_q nez_rotr(nez_q x, uint32_t n) { return nez_rotl(x, (128u - n) & 127u); }
/* shufb by the byte-reverse constant: the quad at aligned `a`, reversed */
static inline nez_q nez_ldrev(const uint8_t* ls, uint32_t a)
{
    const uint8_t* p = ls + (a & NEZ_QM);
    nez_q r; r.hi = nez_le64(p + 8); r.lo = nez_le64(p);
    return r;
}
/* shlqbi(shlqbybi(-1, c), c) */
static inline nez_q nez_onesshl(uint32_t c)
{
    nez_q m;
    const uint32_t by = (c >> 3) & 31u;
    if (by >= 16u) { m.hi = m.lo = 0; return m; }
    const unsigned s = by * 8u + (c & 7u);
    if (s >= 64u) { m.hi = ~0ull << (s - 64u); m.lo = 0; }
    else          { m.hi = ~0ull; m.lo = ~0ull << s; }
    return m;
}
/* 32 bytes at aligned `base`, byte-reversed, positioned at bit c (the
 * window build used at entry and by every refill) */
static inline nez_q nez_window(const uint8_t* ls, uint32_t base0, uint32_t base1, uint32_t c)
{
    const nez_q a = nez_rotl(nez_ldrev(ls, base0), c & 127u);
    const nez_q b = nez_rotl(nez_ldrev(ls, base1), c & 127u);
    const nez_q m = nez_onesshl(c);
    nez_q r; r.hi = (a.hi & ~m.hi) | (b.hi & m.hi); r.lo = (a.lo & ~m.lo) | (b.lo & m.lo);
    return nez_rotl(r, 96u);
}
#define NEZ_W0(w) ((uint32_t)((w).hi >> 32))

/* The loop-top store: the quad holding `pos`, byte (pos & 15) <- b, stored
 * at `to`'s quadword. */
static inline void nez_st36(uint8_t* ls, uint32_t to, uint32_t pos, uint8_t b)
{
    const uint32_t tq = to & NEZ_QM, pq = pos & NEZ_QM;
    if (tq != pq) memcpy(ls + tq, ls + pq, 16);
    ls[tq + (pos & 15u)] = b;
}

/* window copy loops (0x8748/0x88E8/0x8940/0x8998): the next source byte is
 * loaded before the current one is stored */
static inline uint32_t nez_wcopy(uint8_t* ls, uint32_t out, uint32_t s, uint32_t n)
{
    if (!n) return out;
    uint8_t cur = ls[s & NEZ_LSM];
    do {
        s++;
        const uint8_t nx = ls[s & NEZ_LSM];
        ls[out & NEZ_LSM] = cur;
        out++;
        cur = nx;
    } while (--n);
    return out;
}

void native_edge_zlib_inflate_fast_ls(uint8_t* ls, uint32_t strm, uint32_t start,
                                      const native_edge_zlib_desc* d)
{
    const uint32_t dummy = d->dummy_ls;
    /* entry loads (all word fields via rotqby of their quadword) */
    const uint32_t in0 = nez_rd32(ls, strm + 0), avail_out = nez_rd32(ls, strm + 16);
    const uint32_t state = nez_rd32(ls, strm + 28), avail_in = nez_rd32(ls, strm + 4);
    const uint32_t out0 = nez_rd32(ls, strm + 12);
    const uint32_t bits0 = nez_rd32(ls, state + 60), lenbits = nez_rd32(ls, state + 84);
    const uint32_t distbits = nez_rd32(ls, state + 88), hold0 = nez_rd32(ls, state + 56);
    const uint32_t wsize = nez_rd32(ls, state + 40), whave = nez_rd32(ls, state + 44);
    const uint32_t wwrite = nez_rd32(ls, state + 48), window = nez_rd32(ls, state + 52);
    const uint32_t lencode = nez_rd32(ls, state + 76), distcode = nez_rd32(ls, state + 80);

    const uint32_t lmask = nez_shl1m(lenbits), dmask = nez_shl1m(distbits);
    const uint32_t bmask = nez_shl1m(bits0);
    const uint32_t beg = out0 - (start - avail_out);
    const uint32_t last = in0 + avail_in - 5u, end = out0 + avail_out - 257u;

    /* bit reader */
    nez_q W = nez_window(ls, in0 & ~15u, (in0 & ~15u) + 16u, 128u - (in0 & 15u) * 8u);
    W = nez_rotl(W, bits0 & 127u);
    W.hi = (W.hi & ~((uint64_t)bmask << 32)) | ((uint64_t)(hold0 & bmask) << 32);
    uint32_t r26, r27, r33 = 0;
    if (bits0 == 0) { r26 = (in0 & 15u) * 8u; r27 = in0 & ~15u; }
    else            { r26 = ((in0 - 1u) & 15u) * 8u - bits0 + 8u; r27 = (in0 - 1u) & ~15u; }

#define NEZ_CONSUME(n) do {                                                   \
        const uint32_t n_ = (n);                                              \
        r33 += n_; W = nez_rotr(W, n_);                                       \
        if (r33 > 112u) {                                                     \
            r26 += r33; const uint32_t t_ = r26 >> 3; r26 &= 127u;            \
            W = nez_window(ls, r27 + t_, r27 + t_ + 16u, 128u - r26);         \
            r27 = (r27 + t_) & ~15u; r33 = 0;                                 \
        }                                                                     \
    } while (0)

    uint32_t r29 = out0 - 1u;       /* last byte written */
    uint32_t r30 = dummy;           /* where the next loop-top store goes */
    uint8_t lit = (uint8_t)bmask;   /* byte 3 of $r34 */
    uint32_t in, pos, e, op, len, dist, from, msg;
    uint8_t lit_prev;

    for (;;) {
        /* 0x8308 loop top */
        pos = r29;
        {
            const uint32_t r37 = r26 + r33;
            in = r27 + ((r37 + 7u) >> 3);
        }
        r29 = pos + 1u;
        e = nez_rd32(ls, lencode + ((NEZ_W0(W) & lmask) << 2));
        nez_st36(ls, r30, pos, lit);
        if (!(last > in) || !(end > r29)) { NEZ_STAT(0); goto done; }
        lit_prev = lit;
    dolen:
        lit = (uint8_t)e;
        op = e >> 24;
        NEZ_CONSUME((e >> 16) & 0xFFu);
        r30 = r29;
        if (op == 0) { NEZ_STAT(1); continue; }      /* literal (stored next iteration) */
        len = e & 0xFFFFu;
        if (!(op & 16u)) {                           /* 0x87A0 */
            const uint32_t idx = len + (NEZ_W0(W) & nez_shl1m(op));
            if (op & 64u) {
                if (!(op & 32u)) { NEZ_STAT(2); msg = d->msg_lit; goto bad; }
                NEZ_STAT(3);
                nez_wr32(ls, state, 11u);           /* TYPE: end of block */
                in = r27 + ((r26 + r33 + 7u) >> 3);
                goto done;
            }
            NEZ_STAT(4);
            e = nez_rd32(ls, lencode + (idx << 2));
            r30 = dummy;
            nez_st36(ls, r30, pos, lit_prev);        /* 0x8384 again, to the dummy */
            goto dolen;
        }
        if (op & 15u) {                              /* 0x86E0 length extra */
            const uint32_t k = op & 15u, x = NEZ_W0(W) & nez_shl1m(k);
            NEZ_CONSUME(k);
            len += x;
            NEZ_STAT(17);
        }
        /* 0x8450 distance */
        e = nez_rd32(ls, distcode + ((NEZ_W0(W) & dmask) << 2));
        for (;;) {
            op = e >> 24;
            dist = e & 0xFFFFu;
            NEZ_CONSUME((e >> 16) & 0xFFu);
            if (op & 16u) break;
            {
                const uint32_t idx = dist + (NEZ_W0(W) & nez_shl1m(op));
                if (op & 64u) { NEZ_STAT(5); msg = d->msg_dist; goto bad; }
                NEZ_STAT(6);
                e = nez_rd32(ls, distcode + (idx << 2));
            }
        }
        {
            const uint32_t k = op & 15u, x = NEZ_W0(W) & nez_shl1m(k);
            const uint32_t done_out = r29 - beg;
            NEZ_CONSUME(k);
            dist += x;
            from = r29 - dist;
            if (dist > done_out) {                   /* 0x8720: from the window */
                const uint32_t opw = dist - done_out;
                if (opw > whave) { NEZ_STAT(7); msg = d->msg_far; goto bad; }
                if (wwrite == 0) {                   /* 0x872C */
                    from = window + (wsize - opw);
                    NEZ_STAT(8);
                    if (len > opw) {
                        NEZ_STAT(9);
                        len -= opw; r29 = nez_wcopy(ls, r29, from, opw);
                        from = r29 - dist;
                    }
                } else if (opw > wwrite) {           /* 0x8898: wraps */
                    const uint32_t a = opw - wwrite;
                    from = window + (wsize + wwrite - opw);
                    NEZ_STAT(10);
                    if (len > a) {
                        NEZ_STAT(11);
                        len -= a; r29 = nez_wcopy(ls, r29, from, a);
                        from = window;
                        if (len > wwrite) {
                            NEZ_STAT(12);
                            len -= wwrite; r29 = nez_wcopy(ls, r29, window, wwrite);
                            from = r29 - dist;
                        }
                    }
                } else {                             /* 0x8980 */
                    from = window + (wwrite - opw);
                    NEZ_STAT(13);
                    if (len > opw) {
                        NEZ_STAT(14);
                        len -= opw; r29 = nez_wcopy(ls, r29, from, opw);
                        from = r29 - dist;
                    }
                }
            }
        }
        /* 0x8570 copy `len` bytes from `from` */
        if (len > dist) {                            /* 0x8B00 overlapping: byte loop, load after store */
            NEZ_STAT(15);
            uint8_t b = ls[from & NEZ_LSM];
            do {
                from++;
                ls[r29 & NEZ_LSM] = b;
                r29++;
                b = ls[from & NEZ_LSM];
            } while (--len);
        } else {                                     /* 0x8590 / 0x89F0 quad copies */
            NEZ_STAT(16);
            do {
                const uint32_t c = len > 16u ? 16u : len;
                uint8_t t[16];
                const uint32_t f = from & NEZ_LSM, o = r29 & NEZ_LSM;
                if (f <= NEZ_LSM + 1u - 16u) memcpy(t, ls + f, 16);
                else for (uint32_t i = 0; i < 16u; i++) t[i] = ls[(from + i) & NEZ_LSM];
                if (o <= NEZ_LSM + 1u - c) memcpy(ls + o, t, c);
                else for (uint32_t i = 0; i < c; i++) ls[(r29 + i) & NEZ_LSM] = t[i];
                len -= c; from += c; r29 += c;
            } while (len);
        }
        r29 -= 1u;
        r30 = dummy;
        continue;
    bad:
        nez_wr32(ls, strm + 24, msg);
        nez_wr32(ls, state, 27u);                   /* BAD */
        in = r27 + ((r26 + r33 + 7u) >> 3);
        goto done;
    }
done:
    {
        const uint32_t b = (8u - (r26 + r33)) & 7u;
        nez_wr32(ls, strm + 0, in);
        nez_wr32(ls, strm + 12, r29);
        nez_wr32(ls, strm + 4, last - in + 5u);
        nez_wr32(ls, strm + 16, end - r29 + 257u);
        nez_wr32(ls, state + 56, NEZ_W0(W) & nez_shl1m(b));
        nez_wr32(ls, state + 60, b);
    }
#undef NEZ_CONSUME
}

int native_edge_zlib_ls_matches(const uint8_t* ls, const native_edge_zlib_desc* d)
{
    static const uint8_t rev[16] = { 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
    return memcmp(ls + d->shuf_ls, rev, 16) == 0 &&
           memcmp(ls + d->msg_dist, "invalid distance code", 22) == 0 &&
           memcmp(ls + d->msg_lit, "invalid literal/length code", 28) == 0 &&
           memcmp(ls + d->msg_far, "invalid distance too far back", 30) == 0;
}

int native_edge_zlib_enabled(void)
{
    static volatile int en = -1;
    int v = en;
    if (v < 0) {
        const char* e = getenv("PORT_NATIVE_EDGE_ZLIB");
        v = (e && *e) ? atoi(e) : 0;
        en = v;
    }
    return v;
}

#ifndef NATIVE_EDGE_ZLIB_NO_CTX
#include "spu_context.h"
int native_edge_zlib_fast(struct spu_context* ctx, const native_edge_zlib_desc* d)
{
    if (!native_edge_zlib_enabled()) return 0;
    if (!native_edge_zlib_ls_matches(ctx->ls, d)) return 0;
    native_edge_zlib_inflate_fast_ls(ctx->ls, ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], d);
    return 1;
}
#endif
