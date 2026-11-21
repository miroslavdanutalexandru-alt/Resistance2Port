/* R2 native audio-synth kernels -- see r2_native_audio.h and
 * scratch/cpu_audio_native.md for the derivation from the SPU disassembly
 * (scratch/native_audio/bin/jobmod_*.dis.txt).
 *
 * Exactness rules followed here:
 *  - float math uses the very same spu_fm/spu_fa/spu_fs/spu_fma/spu_cfltu/
 *    spu_cuflt helpers as the lifted code, on the same operand order, and this
 *    file is compiled with the lifted sources' flags (CMake SPU_SOURCES), so
 *    each lane result is identical by construction;
 *  - every LS access is quadword-masked (& 0x3FFF0) like lqd/stqd, or, for the
 *    zero fill, byte-wrapped at 256 KB;
 *  - loads and stores that could alias are issued in the SPU program order
 *    (deinterleave/interleave/alloc), or the kernel computes into a private
 *    buffer and declines before writing anything when an alias that the SPU
 *    pipelining would make order-dependent is possible (resampler);
 *  - only the LS constants the kernel bakes in (shuffle masks) are checked;
 *    data tables (coefficients, {0,1,2,3}, the state merge mask) are read
 *    from LS at run time. */
#include "spu_helpers.h"
#include "r2_native_audio.h"
#include <string.h>
#include <immintrin.h>

#ifndef R2NA_MAX_RESAMPLE
#define R2NA_MAX_RESAMPLE  8192u   /* samples per 0x4800 call (lifted: any multiple of 8) */
#endif
#ifndef R2NA_MAX_XLEAVE
#define R2NA_MAX_XLEAVE    16384u  /* r8 words per 0x4B80/0x4C80 call */
#endif

#define QM R2NA_QMASK

/* per-word byte swap of 16 bytes (SSSE3 pshufb when the SPU sources are built
 * with it -- PORT_SPU_AVX2, the shipped config -- else SSE2 shifts) */
static inline __m128i bswap4(__m128i x) {
#if defined(__SSSE3__) || defined(__AVX2__)
    return _mm_shuffle_epi8(x, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
#else
    x = _mm_or_si128(_mm_slli_epi16(x, 8), _mm_srli_epi16(x, 8));            /* swap bytes in 16-bit halves */
    return _mm_shufflehi_epi16(_mm_shufflelo_epi16(x, 0xB1), 0xB1);         /* swap the halves */
#endif
}
/* lqd/stqd: LS big-endian words <-> u128 native words */
static inline u128 ldu(const uint8_t* p) {
    u128 v;
    const __m128i x = bswap4(_mm_loadu_si128((const __m128i*)p));
    memcpy(&v, &x, 16);
    return v;
}
static inline u128 ldq(const uint8_t* ls, uint32_t a) { return ldu(ls + (a & QM)); }
static inline void stq(uint8_t* ls, uint32_t a, u128 v) {
    __m128i x;
    memcpy(&x, &v, 16);
    _mm_storeu_si128((__m128i*)(ls + (a & QM)), bswap4(x));
}
/* raw quad (word moves only: no swap needed) */
static inline __m128i ldr(const uint8_t* ls, uint32_t a) { return _mm_loadu_si128((const __m128i*)(ls + (a & QM))); }
static inline void str_(uint8_t* ls, uint32_t a, __m128i v) { _mm_storeu_si128((__m128i*)(ls + (a & QM)), v); }

static inline u128 splat32(uint32_t w) { u128 r; r._u32[0] = r._u32[1] = r._u32[2] = r._u32[3] = w; return r; }
static inline u128 addw(u128 a, uint32_t d) { for (int i = 0; i < 4; i++) a._u32[i] += d; return a; }

/* [a, a+an) and [b, b+bn) intersect (no wrap: callers guarantee ends <= 256K) */
static inline int ovl(uint32_t a, uint32_t an, uint32_t b, uint32_t bn) { return a < b + bn && b < a + an; }

/* ---- LS signatures (constants baked into the kernels) ------------------- */
static const uint8_t SIG_TRANSPOSE[5][16] = {    /* @0x11510 / 0x11660 / 0x116D0 */
    { 0x00,0x01,0x02,0x03, 0x10,0x11,0x12,0x13, 0x04,0x05,0x06,0x07, 0x14,0x15,0x16,0x17 },
    { 0x08,0x09,0x0A,0x0B, 0x18,0x19,0x1A,0x1B, 0x0C,0x0D,0x0E,0x0F, 0x1C,0x1D,0x1E,0x1F },
    { 0x04,0x05,0x06,0x07, 0x14,0x15,0x16,0x17, 0x00,0x01,0x02,0x03, 0x10,0x11,0x12,0x13 },
    { 0x0C,0x0D,0x0E,0x0F, 0x1C,0x1D,0x1E,0x1F, 0x08,0x09,0x0A,0x0B, 0x18,0x19,0x1A,0x1B },
    { 0x08,0x09,0x0A,0x0B, 0x0C,0x0D,0x0E,0x0F, 0x10,0x11,0x12,0x13, 0x14,0x15,0x16,0x17 },
};
static const uint8_t SIG_SEL[16] = { 0,0,0,0, 0,0,0,0, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF };

static int sig_transpose(const uint8_t* ls, uint32_t base, uint32_t sel) {
    return memcmp(ls + base, SIG_TRANSPOSE, sizeof SIG_TRANSPOSE) == 0 &&
           memcmp(ls + sel, SIG_SEL, 16) == 0;
}
/* 0x11570 + 16k: bytes 4k .. 4k+15 (words k..k+3 of the q0:q1 pair) */
static int sig_window(const uint8_t* ls) {
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < 16; i++)
            if (ls[0x11570 + 16 * k + i] != (uint8_t)(4 * k + i)) return 0;
    return 1;
}

/* ======================================================================== *
 * a8 0x4800 -- 4-tap interpolating resampler                                *
 *   in : r3.w0 state quad {inc, phase, ...}; r4.w0 source base (+12 bias);  *
 *        r5.w0 dst; r6 count (word 0 drives the loop: 8 samples / iter)     *
 *   out: dst[count] floats, state.w1 = frac(phase + count*inc);             *
 *        r4 = splat(r4.w0-12), r5 += 32*it, r6 -= 8*it (all lanes)          *
 * Output sample s (group g = s/4, lane j = s%4), P = phase vector of g:     *
 *   c = coef quad @ 0x104F0 + (cfltu(P,161) & 4095)                         *
 *   t = words k..k+3 of LS[(cfltu(P,171)+r4s)] : LS[... + 16],              *
 *       k = ((cfltu(P,169) + ((r4s & 15) << 2)) & 63) >> 4                  *
 *   y = (t0*c0 + t2*c2) + (t1*c1 + t3*c3), SPU operand order kept below.    *
 * ======================================================================== */
#define TBL_LO 0x104F0u        /* coef table .. last constant used (0x11730) */
#define TBL_HI 0x11740u

static SPU_THREAD_LOCAL u128 t_rs_out[R2NA_MAX_RESAMPLE / 4];

static int k_resample4(uint8_t* ls, u128* g)
{
    const uint32_t cnt = g[6]._u32[0];
    if (cnt == 0 || (cnt & 7u) || cnt > R2NA_MAX_RESAMPLE) return 0;
    const uint32_t ngrp = cnt >> 2;
    const uint32_t S = g[3]._u32[0] & QM;
    const uint32_t D = g[5]._u32[0] & QM;
    if (D + 16u * ngrp > R2NA_LS_SIZE) return 0;                 /* dst would wrap */
    if (ovl(S, 16, TBL_LO, TBL_HI - TBL_LO) || ovl(D, 16u * ngrp, TBL_LO, TBL_HI - TBL_LO)) return 0;
    if (!sig_transpose(ls, 0x11510, 0x11630) || !sig_window(ls)) return 0;

    const uint32_t r4s = g[4]._u32[0] - 12u;
    const uint32_t off11 = (r4s & 15u) << 2;

    /* prologue, exactly the SPU's op sequence (0x4800..0x48C8) */
    const u128 st   = ldq(ls, S);
    const u128 inc  = splat32(st._u32[0]);
    const u128 ph   = splat32(st._u32[1]);
    const u128 cntf = spu_cuflt(g[6], 155);
    u128 P          = spu_fma(ldq(ls, 0x11640), inc, ph);
    const u128 endp = spu_fma(inc, cntf, ph);
    const u128 inc4 = spu_fm(splat32(0x40800000u), inc);
    const u128 ipf  = spu_cuflt(spu_cfltu(endp, 173), 155);
    const u128 frac = spu_fs(endp, ipf);
    const u128 nst  = spu_shufb(frac, st, ldq(ls, 0x11560));

    uint32_t tmin = 0xFFFFFFFFu, tmax = 0;
    for (uint32_t gi = 0; gi < ngrp; gi++) {
        const u128 c161 = spu_cfltu(P, 161);
        const u128 c169 = spu_cfltu(P, 169);
        const u128 c171 = spu_cfltu(P, 171);
        P = spu_fa(P, inc4);
        u128 M[4];
        for (int j = 0; j < 4; j++) {
            const uint32_t ca = (TBL_LO + (c161._u32[j] & 4095u)) & QM;
            const uint32_t k  = ((c169._u32[j] + off11) & 63u) >> 4;
            const uint32_t a0 = (c171._u32[j] + r4s) & QM;
            if (a0 > R2NA_LS_SIZE - 32u) return 0;                /* q1 wraps to LS 0 */
            if (a0 < tmin) tmin = a0;
            if (a0 > tmax) tmax = a0;
            const u128 t = ldu(ls + a0 + 4u * k);   /* words k..k+3 of q0:q1 */
            const u128 c = ldq(ls, ca);
            /* fm operand order as in the SPU code: the odd group's lane 0 is (taps, coef) */
            M[j] = ((gi & 1u) && j == 0) ? spu_fm(t, c) : spu_fm(c, t);
        }
        u128 T[4];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) T[i]._u32[j] = M[j]._u32[i];
        if (!(gi & 1u)) {       /* 0x4A08 / 0x4A10 / 0x4A40 */
            const u128 x = spu_fa(T[2], T[0]);
            const u128 y = spu_fa(T[1], T[3]);
            t_rs_out[gi] = spu_fa(y, x);
        } else {                /* 0x4B20 / 0x4B28 / 0x4B48 */
            const u128 x = spu_fa(T[0], T[2]);
            const u128 y = spu_fa(T[3], T[1]);
            t_rs_out[gi] = spu_fa(x, y);
        }
    }
    /* The SPU loop is software-pipelined (next group's taps load before this
     * group's store; the state quad is stored mid-prologue): any alias of the
     * written quads with the tap window makes the result order-dependent. */
    const uint32_t tlo = tmin, tn = tmax + 32u - tmin;
    if (ovl(S, 16, tlo, tn) || ovl(D, 16u * ngrp, tlo, tn)) return 0;

    stq(ls, S, nst);
    for (uint32_t gi = 0; gi < ngrp; gi++) stq(ls, D + 16u * gi, t_rs_out[gi]);

    const uint32_t it = cnt >> 3;
    g[4] = splat32(r4s);
    g[5] = addw(g[5], 32u * it);
    g[6] = addw(g[6], 0u - 8u * it);
    return 1;
}

/* ======================================================================== *
 * a8 0x4B80 -- deinterleave 4 channels                                      *
 *   r3 src (4 quads/iter), r4..r7 channel dsts, r8 word count (&~3, 4/iter) *
 * ======================================================================== */
static inline void tr4(__m128i c0, __m128i c1, __m128i c2, __m128i c3,
                       __m128i* o0, __m128i* o1, __m128i* o2, __m128i* o3)
{
    const __m128i t0 = _mm_unpacklo_epi32(c0, c1), t1 = _mm_unpacklo_epi32(c2, c3);
    const __m128i t2 = _mm_unpackhi_epi32(c0, c1), t3 = _mm_unpackhi_epi32(c2, c3);
    *o0 = _mm_unpacklo_epi64(t0, t1); *o1 = _mm_unpackhi_epi64(t0, t1);
    *o2 = _mm_unpacklo_epi64(t2, t3); *o3 = _mm_unpackhi_epi64(t2, t3);
}

static int k_deinterleave4(uint8_t* ls, u128* g)
{
    const uint32_t n = g[8]._u32[0] & ~3u;
    if (n == 0 || n > R2NA_MAX_XLEAVE) return 0;
    if (!sig_transpose(ls, 0x11660, 0x116C0)) return 0;
    const uint32_t it = n >> 2;
    uint32_t s = g[3]._u32[0];
    uint32_t d4 = g[4]._u32[0] - 16u, d5 = g[5]._u32[0] - 16u, d6 = g[6]._u32[0] - 16u, d7 = g[7]._u32[0] - 16u;
    __m128i c0 = ldr(ls, s), c1 = ldr(ls, s + 0x10), c2 = ldr(ls, s + 0x20), c3 = ldr(ls, s + 0x30);
    for (uint32_t i = 0; i < it; i++) {
        s += 64u; d4 += 16u; d5 += 16u; d6 += 16u; d7 += 16u;
        __m128i o0, o1, o2, o3;
        tr4(c0, c1, c2, c3, &o0, &o1, &o2, &o3);
        c0 = ldr(ls, s); c1 = ldr(ls, s + 0x10); c2 = ldr(ls, s + 0x20); c3 = ldr(ls, s + 0x30);
        str_(ls, d4, o0); str_(ls, d6, o2); str_(ls, d5, o1); str_(ls, d7, o3);
    }
    g[3] = addw(g[3], 64u * it);
    for (int r = 4; r <= 7; r++) g[r] = addw(g[r], 16u * it - 16u);
    for (int l = 0; l < 4; l++) g[8]._u32[l] = (g[8]._u32[l] & ~3u) - 4u * it;
    return 1;
}

/* ======================================================================== *
 * a8 0x4C80 -- interleave 4 channels                                        *
 *   r3..r6 channel srcs, r7 dst (4 quads/iter), r8 word count               *
 * ======================================================================== */
static int k_interleave4(uint8_t* ls, u128* g)
{
    const uint32_t n = g[8]._u32[0] & ~3u;
    if (n == 0 || n > R2NA_MAX_XLEAVE) return 0;
    if (!sig_transpose(ls, 0x116D0, 0x11730)) return 0;
    const uint32_t it = n >> 2;
    uint32_t a = g[3]._u32[0], b = g[4]._u32[0], c = g[5]._u32[0], d = g[6]._u32[0], o = g[7]._u32[0];
    __m128i A = ldr(ls, a), B = ldr(ls, b), C = ldr(ls, c), Dq = ldr(ls, d);
    for (uint32_t i = 0; i < it; i++) {
        a += 16u; b += 16u; c += 16u; d += 16u;
        __m128i o0, o1, o2, o3;
        tr4(A, B, C, Dq, &o0, &o1, &o2, &o3);
        A = ldr(ls, a); B = ldr(ls, b); C = ldr(ls, c); Dq = ldr(ls, d);
        str_(ls, o, o0); str_(ls, o + 0x20, o2); str_(ls, o + 0x10, o1); str_(ls, o + 0x30, o3);
        o += 64u;
    }
    for (int r = 3; r <= 6; r++) g[r] = addw(g[r], 16u * it);
    g[7] = addw(g[7], 64u * it);
    for (int l = 0; l < 4; l++) g[8]._u32[l] = (g[8]._u32[l] & ~3u) - 4u * it;
    return 1;
}

/* ======================================================================== *
 * a8 0x6630 / 0x6788 (= 5e0b 0xDC08 / 0xDD60) -- arena allocator           *
 *   r3.w0 = arena A: q0 @A {.., lo, hi, ..}, q1 @A+16 {used, cap, .., align}*
 *   r4 = size. len = (size + align - 1) & -align (per lane, lane 0 used).   *
 *   fail (len == 0 || len > cap - used): r3 = 0.                            *
 *   ok: q1.used += len; up: p = lo, lo += len; down: hi -= len, p = hi;     *
 *       zero len bytes at p; r3 = rotated q0 (up) / rotated q0 - len (down) *
 *   The word slots are cwd-selected by $sp (normally 0 / 1 / 2).            *
 * ======================================================================== */
static inline u128 rotw(u128 v, int k) {   /* rotqbyi by 4k bytes */
    u128 r;
    for (int i = 0; i < 4; i++) r._u32[i] = v._u32[(i + k) & 3];
    return r;
}

static int k_alloc(uint8_t* ls, u128* g, int down)
{
    const uint32_t A = g[3]._u32[0];
    const u128 q1 = ldq(ls, A + 0x10u);
    const u128 r5 = rotw(q1, 3);
    u128 r8;
    for (int i = 0; i < 4; i++) r8._u32[i] = (r5._u32[i] + g[4]._u32[i] - 1u) & (0u - r5._u32[i]);
    const uint32_t len = r8._u32[0];
    if (len == 0 || len > q1._u32[1] - q1._u32[0]) {
        g[3] = splat32(0);
        return 1;
    }
    if (len >= R2NA_LS_SIZE) return 0;
    const u128 q0 = ldq(ls, A);
    const uint32_t sp = g[1]._u32[0];
    u128 n1 = q1, n0 = q0, ret;
    n1._u32[(sp & 0xCu) >> 2] = q1._u32[0] + len;
    uint32_t p;
    if (!down) {
        ret = rotw(q0, 1);
        p = ret._u32[0];
        n0._u32[((sp + 4u) & 0xCu) >> 2] = p + len;
    } else {
        ret = rotw(q0, 2);
        for (int i = 0; i < 4; i++) ret._u32[i] -= r8._u32[i];
        p = ret._u32[0];
        n0._u32[((sp + 8u) & 0xCu) >> 2] = p;
    }
    stq(ls, A + 0x10u, n1);
    stq(ls, A, n0);
    p &= R2NA_LS_SIZE - 1u;
    if (p + len <= R2NA_LS_SIZE) memset(ls + p, 0, len);
    else { memset(ls + p, 0, R2NA_LS_SIZE - p); memset(ls, 0, len - (R2NA_LS_SIZE - p)); }
    g[3] = ret;
    return 1;
}

/* ========================================================================
 * 5e0b 0x5E08 ring read / 0x6150 ring write: two-segment byte copies.
 *   R = r3 ring header, quads q30/q40/q50 at R+0x30/0x40/0x50 (lqd: & QM).
 *   0x5E08 (r4 = dst): n1 = q50.w2 << 2, n2 = q50.w3 << 2;
 *       copy n1 from q30.w1 to dst; then (q30/q40 re-read)
 *       copy n2 from (n1 ? q30.w0 + q40.w1 : q30.w1) to dst + n1.
 *   0x6150 (r4 = src, r5 = count words): a = q40.w2, b = q50.w1;
 *       n1 = min(count, a) << 2; n2 = count > a ? min(count - a, b) << 2 : 0;
 *       copy n1 from src to q30.w2; then (q30/q40 re-read)
 *       copy n2 from src + n1 to q30.w0 + q40.w1.
 *   Each copy is quad loads + selb head/tail merges; the misaligned path
 *   shufb's with the byte-shift table at LS 0xF2A0 (entry i = i..i+15). With
 *   that table intact and no src/dst overlap a copy is exactly memcpy
 *   (bytes outside [dst, dst+n) are rewritten unchanged). Leaf routines: only
 *   $r2..$r79 are written, callers re-set r3..r8 and read only r80+, so
 *   there are no output registers and LS is the only result.
 *   Declined: n > 64K, any read (incl. the 32-byte quad over-read) or write
 *   near the top of LS (no wrap), src quads overlapping dst quads, a dst
 *   touching the shuffle table, copy 1 touching the header quads that are
 *   re-read for copy 2, or a table that is not the expected pattern.
 * ======================================================================== */
#define RC_TBL   0xF2A0u
#define RC_MAXN  0x10000u

static int rc_sig(const uint8_t* ls)
{
    const __m128i base = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    for (int i = 0; i < 16; i++) {
        const __m128i e = _mm_add_epi8(base, _mm_set1_epi8((char)i));
        const __m128i v = _mm_loadu_si128((const __m128i*)(ls + RC_TBL + 16u * (uint32_t)i));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(v, e)) != 0xFFFF) return 0;
    }
    return 1;
}

/* quad-rounded write range of [dst, dst+n) */
static inline uint32_t rc_w0(uint32_t dst) { return dst & ~15u; }
static inline uint32_t rc_wn(uint32_t dst, uint32_t n) { return ((dst + n + 15u) & ~15u) - (dst & ~15u); }

static int rc_seg_ok(uint32_t src, uint32_t dst, uint32_t n)
{
    if (!n) return 1;
    if (n > RC_MAXN || (uint64_t)src + n + 32u > R2NA_LS_SIZE || (uint64_t)dst + n + 16u > R2NA_LS_SIZE) return 0;
    const uint32_t w0 = rc_w0(dst), wn = rc_wn(dst, n);
    const uint32_t r0 = src & ~15u, rn_ = src + n + 32u - r0;
    if (ovl(r0, rn_, w0, wn)) return 0;
    if (ovl(RC_TBL, 0x100u, w0, wn)) return 0;
    return 1;
}

/* copy 1 must not touch the header quads re-read for copy 2 */
static int rc_hdr_hit(uint32_t R, uint32_t dst, uint32_t n)
{
    if (!n) return 0;
    const uint32_t w0 = rc_w0(dst), wn = rc_wn(dst, n);
    for (uint32_t o = 0x30u; o <= 0x50u; o += 0x10u)
        if (ovl((R + o) & QM, 16u, w0, wn)) return 1;
    return 0;
}

static int k_ringread(uint8_t* ls, u128* g)
{
    const uint32_t R = g[3]._u32[0], dst = g[4]._u32[0];
    const u128 q30 = ldq(ls, R + 0x30u), q40 = ldq(ls, R + 0x40u), q50 = ldq(ls, R + 0x50u);
    const uint32_t n1 = q50._u32[2] << 2, n2 = q50._u32[3] << 2;
    const uint32_t src1 = q30._u32[1];
    const uint32_t src2 = n1 ? q30._u32[0] + q40._u32[1] : q30._u32[1];
    const uint32_t dst2 = dst + n1;
    if (!n1 && !n2) return 1;
    if (!rc_sig(ls) || !rc_seg_ok(src1, dst, n1) || !rc_seg_ok(src2, dst2, n2)) return 0;
    if (n2 && rc_hdr_hit(R, dst, n1)) return 0;
    if (n1) memcpy(ls + dst, ls + src1, n1);
    if (n2) memcpy(ls + dst2, ls + src2, n2);
    return 1;
}

static int k_ringwrite(uint8_t* ls, u128* g)
{
    const uint32_t R = g[3]._u32[0], src = g[4]._u32[0], cnt = g[5]._u32[0];
    const u128 q30 = ldq(ls, R + 0x30u), q40 = ldq(ls, R + 0x40u), q50 = ldq(ls, R + 0x50u);
    const uint32_t a = q40._u32[2], b = q50._u32[1];
    const uint32_t n1 = (cnt > a ? a : cnt) << 2;
    const uint32_t n2 = cnt > a ? ((cnt - a > b ? b : cnt - a) << 2) : 0u;
    const uint32_t dst1 = q30._u32[2];
    const uint32_t src2 = src + n1, dst2 = q30._u32[0] + q40._u32[1];
    if (!n1 && !n2) return 1;
    if (!rc_sig(ls) || !rc_seg_ok(src, dst1, n1) || !rc_seg_ok(src2, dst2, n2)) return 0;
    if (n2 && rc_hdr_hit(R, dst1, n1)) return 0;
    if (n1) memcpy(ls + dst1, ls + src, n1);
    if (n2) memcpy(ls + dst2, ls + src2, n2);
    return 1;
}

/* ---- table ------------------------------------------------------------- */
static const struct { const char* name; uint32_t entry; uint64_t out; } KT[R2NA_K_COUNT] = {
    { "a8_resample4",     0x4800, (1ull << 4) | (1ull << 5) | (1ull << 6) },
    { "a8_deinterleave4", 0x4B80, 0x1F8ull },          /* r3..r8 */
    { "a8_interleave4",   0x4C80, 0x1F8ull },          /* r3..r8 */
    { "a8_alloc_up",      0x6630, 1ull << 3 },
    { "a8_alloc_down",    0x6788, 1ull << 3 },
    { "5e_alloc_up",      0xDC08, 1ull << 3 },
    { "5e_alloc_down",    0xDD60, 1ull << 3 },
    { "5e_ringread",      0x5E08, 0 },
    { "5e_ringwrite",     0x6150, 0 },
};

const char* r2na_kernel_name(int k)  { return (unsigned)k < R2NA_K_COUNT ? KT[k].name : "?"; }
uint32_t r2na_kernel_entry(int k)    { return (unsigned)k < R2NA_K_COUNT ? KT[k].entry : 0; }
uint64_t r2na_kernel_outregs(int k)  { return (unsigned)k < R2NA_K_COUNT ? KT[k].out : 0; }

int r2na_kernel_run(int k, uint8_t* ls, u128* gpr)
{
    switch (k) {
    case R2NA_K_A8_RESAMPLE4:     return k_resample4(ls, gpr);
    case R2NA_K_A8_DEINTERLEAVE4: return k_deinterleave4(ls, gpr);
    case R2NA_K_A8_INTERLEAVE4:   return k_interleave4(ls, gpr);
    case R2NA_K_A8_ALLOC_UP:
    case R2NA_K_5E_ALLOC_UP:      return k_alloc(ls, gpr, 0);
    case R2NA_K_A8_ALLOC_DOWN:
    case R2NA_K_5E_ALLOC_DOWN:    return k_alloc(ls, gpr, 1);
    case R2NA_K_5E_RINGREAD:      return k_ringread(ls, gpr);
    case R2NA_K_5E_RINGWRITE:     return k_ringwrite(ls, gpr);
    default:                      return 0;
    }
}
