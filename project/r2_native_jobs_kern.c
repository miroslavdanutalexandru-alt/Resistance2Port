/* R2 native SPURS job kernels -- the kernels themselves (no ctx / env), so the
 * offline harness (scratch\native_jobs) links exactly this file. See
 * r2_native_jobs.h. Must be compiled with the lifted SPU sources' flags
 * (/arch:AVX2, FMA contraction as in spu_helpers.h). */
#include "r2_native_jobs.h"
#include "native_edge_zlib.h"
#include <immintrin.h>
#include <string.h>

#define NJ_QM 0x3FFF0u

#if defined(__AVX2__)
/* ---- LS access: LS is the big-endian image; "host form" = the u128 layout
 * the lifted code uses (each 32-bit word byte-swapped). ------------------ */
static inline __m128i nj_wswap128(void)
{ return _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12); }
static inline __m128i nj_ldq(const uint8_t* ls, uint32_t a)
{ return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + (a & NJ_QM))), nj_wswap128()); }
static inline void nj_stq(uint8_t* ls, uint32_t a, __m128i v)
{ _mm_storeu_si128((__m128i*)(ls + (a & NJ_QM)), _mm_shuffle_epi8(v, nj_wswap128())); }
/* two quads (record pair) in one ymm, already in host form */
static inline __m256i nj_ld2(const uint8_t* ls, uint32_t a0, uint32_t a1)
{
    const __m256i raw = _mm256_inserti128_si256(
        _mm256_castsi128_si256(_mm_loadu_si128((const __m128i*)(ls + (a0 & NJ_QM)))),
        _mm_loadu_si128((const __m128i*)(ls + (a1 & NJ_QM))), 1);
    return _mm256_shuffle_epi8(raw, _mm256_broadcastsi128_si256(nj_wswap128()));
}

/* ---- shufb with a loop-invariant control (spu_helpers.h spu_shufb, host
 * order): result host byte t = control host byte t; source host byte
 * (sel & 15) ^ 3 of a (sel bit 4 clear) or b; specials 10x -> 00,
 * 110 -> FF, 111 -> 80. Precomputed into pshufb index / blend / OR masks. */
typedef struct { __m256i idx, sel, spec; } nj_shf;
static inline void nj_shf_mk(nj_shf* s, __m128i c)
{
    const __m128i e0m = _mm_set1_epi8((char)0xE0);
    const __m128i hi = _mm_cmplt_epi8(c, _mm_setzero_si128());                  /* 1xxxxxxx: special */
    const __m128i top = _mm_and_si128(c, e0m);
    const __m128i pb = _mm_or_si128(_mm_and_si128(_mm_cmpeq_epi8(top, e0m), _mm_set1_epi8((char)0x80)),
                                    _mm_cmpeq_epi8(top, _mm_set1_epi8((char)0xC0)));
    const __m128i ib = _mm_or_si128(_mm_xor_si128(_mm_and_si128(c, _mm_set1_epi8(15)), _mm_set1_epi8(3)), hi);
    const __m128i bit4 = _mm_set1_epi8(0x10);
    const __m128i sb = _mm_andnot_si128(hi, _mm_cmpeq_epi8(_mm_and_si128(c, bit4), bit4));
    s->idx  = _mm256_broadcastsi128_si256(ib);
    s->sel  = _mm256_broadcastsi128_si256(sb);
    s->spec = _mm256_broadcastsi128_si256(pb);
}
static inline __m256i nj_shf_ap(const nj_shf* s, __m256i a, __m256i b)
{
    return _mm256_or_si256(_mm256_blendv_epi8(_mm256_shuffle_epi8(a, s->idx),
                                              _mm256_shuffle_epi8(b, s->idx), s->sel), s->spec);
}

/* ---- float ops: the spu_helpers.h SPU_XF_NO_EXACT fast path, 8 lanes ---- */
static inline __m256 nj_xf_in(__m256i x)    /* clamp extended-range operands */
{
    const __m256i sm = _mm256_set1_epi32((int)0x80000000u);
    const __m256i mag = _mm256_min_epu32(_mm256_andnot_si256(sm, x), _mm256_set1_epi32(0x7F7FFFFF));
    return _mm256_castsi256_ps(_mm256_or_si256(_mm256_and_si256(x, sm), mag));
}
static inline __m256 nj_xf_out(__m256 r)
{
    const __m256 mx = _mm256_set1_ps(3.40282347e+38f);
    return _mm256_min_ps(_mm256_max_ps(_mm256_add_ps(r, _mm256_setzero_ps()),
                                       _mm256_sub_ps(_mm256_setzero_ps(), mx)), mx);
}
/* Operands here are xf_out results or csflt results (finite, in range), for
 * which the helper's xf_in is the identity; matrix splats are xf_in'd once. */
static inline __m256 nj_fma(__m256 a, __m256 b, __m256 c)
{
#if defined(__FMA__)
    return nj_xf_out(_mm256_fmadd_ps(a, b, c));
#else
    return nj_xf_out(_mm256_add_ps(_mm256_mul_ps(a, b), c));
#endif
}
static inline __m256 nj_fa(__m256 a, __m256 b) { return nj_xf_out(_mm256_add_ps(a, b)); }
/* fcgt: compare keys (zero/denormal = 0, sign-magnitude order) */
static inline __m256i nj_key(__m256i x)
{
    const __m256i m = _mm256_and_si256(x, _mm256_set1_epi32(0x7FFFFFFF));
    const __m256i dn = _mm256_cmpeq_epi32(_mm256_and_si256(x, _mm256_set1_epi32(0x7F800000)), _mm256_setzero_si256());
    const __m256i mag = _mm256_andnot_si256(dn, m);
    const __m256i neg = _mm256_srai_epi32(x, 31);
    return _mm256_sub_epi32(_mm256_xor_si256(mag, neg), neg);
}
static inline __m256i nj_fcgt(__m256 a, __m256 b)
{ return _mm256_cmpgt_epi32(nj_key(_mm256_castps_si256(a)), nj_key(_mm256_castps_si256(b))); }
/* csflt(v, 139) = v * 2^-16 as an SPU single (truncated to 24 bits). Exact via
 * double like spu__x_dsf; the common |v| < 2^24 case converts exactly. */
static inline __m128 nj_csflt16_half(__m128i v)
{
    const __m256d d = _mm256_mul_pd(_mm256_cvtepi32_pd(v), _mm256_set1_pd(1.0 / 65536.0));
    const __m256d t = _mm256_castsi256_pd(_mm256_and_si256(_mm256_castpd_si256(d), _mm256_set1_epi64x(~0x1FFFFFFFll)));
    const __m256d tiny = _mm256_cmp_pd(_mm256_andnot_pd(_mm256_set1_pd(-0.0), t), _mm256_set1_pd(1.1754943508222875e-38), _CMP_LT_OQ);
    return _mm256_cvtpd_ps(_mm256_andnot_pd(tiny, t));
}
static inline __m256 nj_csflt16(__m256i v)
{
    /* exact in a single (no truncation needed) when |v| <= 2^24 or the low 8
     * bits are 0 (<= 24 significant bits) -- the game's data is int16 << 16 */
    const __m256i small = _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_add_epi32(v, _mm256_set1_epi32(1 << 24)),
                                                              _mm256_set1_epi32((int)0xFE000000u)), _mm256_setzero_si256());
    const __m256i low0 = _mm256_cmpeq_epi32(_mm256_and_si256(v, _mm256_set1_epi32(0xFF)), _mm256_setzero_si256());
    if (_mm256_movemask_epi8(_mm256_or_si256(small, low0)) == -1)
        return _mm256_mul_ps(_mm256_cvtepi32_ps(v), _mm256_set1_ps(1.0f / 65536.0f));
    return _mm256_set_m128(nj_csflt16_half(_mm256_extracti128_si256(v, 1)),
                           nj_csflt16_half(_mm256_castsi256_si128(v)));
}

/* ======================================================================
 * dd413 0x4030 -- packed-position transform + clip code.
 * In:  $r3 src (80-byte records, 4 vertices each), $r4 dst (64 bytes per
 *      record), $r5 vertex count, $r6..$r9 matrix rows (x, y, z, w; words
 *      0..2 = coefficients, word 3 = translation).
 * LS tables: unpack controls at 0x5B00 + ((src & 12) << 5) (+0x00..+0x50),
 *      transposes at 0x7D60/0x7D70 and 0x5D00..0x5D30 (read before any store).
 * Per record: 3x shufb unpack, csflt 139, 4 fma chains, clip bits
 *      cx = fcgt(X,W) | sign(X+W), cy = fcgt(Y,W) | sign(Y+W),
 *      cz = fcgt(Z,W) | sign(Z), then a shufb transpose to 4 quads.
 * The lifted loop is software-pipelined (reads up to 4 records ahead before
 * the stores); the native order is equivalent when the output never lands on
 * input that is read later: dst <= src, or disjoint ranges. Otherwise, and for
 * counts the lifted loop would mis-handle (n <= 0) or LS wrap, decline. */
int r2nj_dd413_xform4(uint8_t* ls, u128* gpr)
{
    const uint32_t src = gpr[3]._u32[0], dst = gpr[4]._u32[0];
    const int32_t n = (int32_t)(gpr[5]._u32[0] + 3u) >> 2;
    if (n <= 0 || n > 4096) return 0;
    const uint64_t in_end = (uint64_t)src + 80ull * (uint64_t)(n + 4) + 16u;   /* read-ahead included */
    const uint64_t out_end = (uint64_t)dst + 64ull * (uint64_t)n;
    if (in_end > R2NJ_LS_SIZE || out_end > R2NJ_LS_SIZE) return 0;
    if (!(dst <= src || (uint64_t)dst >= in_end || out_end <= (src & ~15u))) return 0;

    const uint32_t tb = 0x5B00u + ((src & 12u) << 5);
    nj_shf T0, T1, T2, T3, T4, T5, L60, L70, C00, C10, C20, C30;
    nj_shf_mk(&T0, nj_ldq(ls, tb + 0x00)); nj_shf_mk(&T1, nj_ldq(ls, tb + 0x10));
    nj_shf_mk(&T2, nj_ldq(ls, tb + 0x20)); nj_shf_mk(&T3, nj_ldq(ls, tb + 0x30));
    nj_shf_mk(&T4, nj_ldq(ls, tb + 0x40)); nj_shf_mk(&T5, nj_ldq(ls, tb + 0x50));
    nj_shf_mk(&L60, nj_ldq(ls, 0x7D60)); nj_shf_mk(&L70, nj_ldq(ls, 0x7D70));
    nj_shf_mk(&C00, nj_ldq(ls, 0x5D00)); nj_shf_mk(&C10, nj_ldq(ls, 0x5D10));
    nj_shf_mk(&C20, nj_ldq(ls, 0x5D20)); nj_shf_mk(&C30, nj_ldq(ls, 0x5D30));

    __m256 M[4][4];
    for (int r = 0; r < 4; r++)
        for (int w = 0; w < 4; w++)
            M[r][w] = nj_xf_in(_mm256_set1_epi32((int)gpr[6 + r]._u32[w]));
    const __m256i SIGN = _mm256_set1_epi32((int)0x80000000u);

    for (int32_t k = 0; k < n; k += 2) {
        const uint32_t a = src + 80u * (uint32_t)k;
        const uint32_t b = (k + 1 < n) ? a + 80u : a;
        const __m256i q0 = nj_ld2(ls, a + 0x00, b + 0x00), q1 = nj_ld2(ls, a + 0x10, b + 0x10);
        const __m256i q2 = nj_ld2(ls, a + 0x20, b + 0x20), q3 = nj_ld2(ls, a + 0x30, b + 0x30);
        const __m256i q4 = nj_ld2(ls, a + 0x40, b + 0x40);
        const __m256i t69 = nj_shf_ap(&T0, q0, q2);
        const __m256i t52 = nj_shf_ap(&T2, t69, q4);
        const __m256i t53 = nj_shf_ap(&T1, q1, q3);
        const __m256 x = nj_csflt16(nj_shf_ap(&T3, t52, t53));
        const __m256 y = nj_csflt16(nj_shf_ap(&T4, t52, t53));
        const __m256 z = nj_csflt16(nj_shf_ap(&T5, t52, t53));
        const __m256 X = nj_fma(z, M[0][2], nj_fma(y, M[0][1], nj_fma(x, M[0][0], M[0][3])));
        const __m256 Y = nj_fma(z, M[1][2], nj_fma(y, M[1][1], nj_fma(x, M[1][0], M[1][3])));
        const __m256 Z = nj_fma(z, M[2][2], nj_fma(y, M[2][1], nj_fma(x, M[2][0], M[2][3])));
        const __m256 W = nj_fma(z, M[3][2], nj_fma(y, M[3][1], nj_fma(x, M[3][0], M[3][3])));
        const __m256i Xi = _mm256_castps_si256(X), Yi = _mm256_castps_si256(Y);
        const __m256i Zi = _mm256_castps_si256(Z), Wi = _mm256_castps_si256(W);
        /* selb(fcgt, sum, 0x80000000): sign bit from the second operand */
        const __m256i cx = _mm256_or_si256(_mm256_andnot_si256(SIGN, nj_fcgt(X, W)),
                                           _mm256_and_si256(SIGN, _mm256_castps_si256(nj_fa(X, W))));
        const __m256i cy = _mm256_or_si256(_mm256_andnot_si256(SIGN, nj_fcgt(Y, W)),
                                           _mm256_and_si256(SIGN, _mm256_castps_si256(nj_fa(Y, W))));
        const __m256i cz = _mm256_or_si256(_mm256_andnot_si256(SIGN, nj_fcgt(Z, W)),
                                           _mm256_and_si256(SIGN, Zi));
        const __m256i lo = nj_shf_ap(&L60, Xi, Wi), hi = nj_shf_ap(&L70, Xi, Wi);
        const __m256i c59 = nj_shf_ap(&C10, nj_shf_ap(&C00, cx, cy), cz);
        const __m256i yl = nj_shf_ap(&L60, Yi, c59), yh = nj_shf_ap(&L70, Yi, c59);
        const __m256i o0 = nj_shf_ap(&C20, lo, yl), o1 = nj_shf_ap(&C30, lo, yl);
        const __m256i o2 = nj_shf_ap(&C20, hi, yh), o3 = nj_shf_ap(&C30, hi, yh);
        const uint32_t d0 = dst + 64u * (uint32_t)k;
        nj_stq(ls, d0 + 0x10, _mm256_castsi256_si128(o1));
        nj_stq(ls, d0 + 0x00, _mm256_castsi256_si128(o0));
        nj_stq(ls, d0 + 0x20, _mm256_castsi256_si128(o2));
        nj_stq(ls, d0 + 0x30, _mm256_castsi256_si128(o3));
        if (k + 1 < n) {
            const uint32_t d1 = d0 + 64u;
            nj_stq(ls, d1 + 0x10, _mm256_extracti128_si256(o1, 1));
            nj_stq(ls, d1 + 0x00, _mm256_extracti128_si256(o0, 1));
            nj_stq(ls, d1 + 0x20, _mm256_extracti128_si256(o2, 1));
            nj_stq(ls, d1 + 0x30, _mm256_extracti128_si256(o3, 1));
        }
    }
    return 1;
}
/* ---- helpers for the round-2 kernels ----------------------------------- */
static inline __m256 nj_fs(__m256 a, __m256 b) { return nj_xf_out(_mm256_sub_ps(a, b)); }
static inline __m256 nj_fm(__m256 a, __m256 b) { return nj_xf_out(_mm256_mul_ps(a, b)); }
static inline __m256 nj_fms(__m256 a, __m256 b, __m256 c)      /* a*b - c */
{
#if defined(__FMA__)
    return nj_xf_out(_mm256_fmsub_ps(a, b, c));
#else
    return nj_xf_out(_mm256_sub_ps(_mm256_mul_ps(a, b), c));
#endif
}
static inline __m256 nj_in(__m256i x) { return nj_xf_in(x); }
/* shufb with a different control in each 128-bit half */
static inline void nj_shf_mk2(nj_shf* s, __m128i clo, __m128i chi)
{
    nj_shf a, b;
    nj_shf_mk(&a, clo); nj_shf_mk(&b, chi);
    s->idx  = _mm256_permute2x128_si256(a.idx, b.idx, 0x20);
    s->sel  = _mm256_permute2x128_si256(a.sel, b.sel, 0x20);
    s->spec = _mm256_permute2x128_si256(a.spec, b.spec, 0x20);
}
/* 32 contiguous LS bytes (two quads) in host form */
static inline __m256i nj_ld32(const uint8_t* ls, uint32_t a)
{
    return _mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)(ls + (a & NJ_QM))),
                               _mm256_broadcastsi128_si256(nj_wswap128()));
}
static inline void nj_st32(uint8_t* ls, uint32_t a, __m256i v)
{
    _mm256_storeu_si256((__m256i*)(ls + (a & NJ_QM)), _mm256_shuffle_epi8(v, _mm256_broadcastsi128_si256(nj_wswap128())));
}
static inline __m256i nj_bc(const u128* r) { return _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)r->_u32)); }
/* cflts(v, 149) = v * 2^24 truncated, saturating. v is an xf_out result
 * (finite, never denormal), so v * 2^24 is exact unless it is >= 2^31. */
static inline __m256i nj_cflts24(__m256 v)
{
    const __m256i r = _mm256_cvttps_epi32(_mm256_mul_ps(v, _mm256_set1_ps(16777216.0f)));
    const __m256i big = _mm256_castps_si256(_mm256_cmp_ps(v, _mm256_set1_ps(128.0f), _CMP_GE_OQ));
    return _mm256_blendv_epi8(r, _mm256_set1_epi32(0x7FFFFFFF), big);
}
/* [lo, hi) overlap of two byte ranges */
static inline int nj_ovl(int64_t alo, int64_t ahi, int64_t blo, int64_t bhi) { return alo < bhi && blo < ahi; }
static inline int nj_inls(int64_t lo, int64_t hi) { return lo >= 0 && hi <= (int64_t)R2NJ_LS_SIZE && lo <= hi; }

/* ======================================================================
 * 7db0 0x9440 -- water height field -> packed 2-channel s8 gradient map.
 * In:  $r3 dst, $r4 src (rows of floats), $r5 width (floats, multiple of 8),
 *      $r6 rows, $r7 src row stride (floats), $r8 dst row stride (bytes),
 *      $r9 != 0: wrap horizontally (periodic), $r10..$r13 coefficient quads.
 * LS tables (copied from the module image at job start): 0x3A860 (x+1),
 *      0x3A880 (x-1), 0x3AAF0 (int32 -> high halves), 0x3ACA0 (interleave
 *      high bytes).
 * Per 8-float strip s and row t (A/B = the strip's two quads, L/R = the
 * neighbour quads, wrapped at the row ends when $r9 != 0):
 *   dx = fs(x+1, x-1), dy = fs(row t+1, row t-1)
 *   P = cflts(fma(dy, r12, fm(dx, r10)), 149), U = cflts(fma(dy, r13, fm(dx, r11)), 149)
 *   16 bytes = interleaved high bytes of P and U, stored at dst + 16 s + t * r8.
 * The lifted loop is software-pipelined (reads rows t-1 .. t+3 before row t's
 * store); the native order is equivalent when no store lands on input that
 * the lifted code still uses. Otherwise (and for counts it would loop on
 * forever, or LS wrap) decline. */
int r2nj_7db0_grad(uint8_t* ls, u128* gpr)
{
    const uint32_t dst = gpr[3]._u32[0], src = gpr[4]._u32[0], W = gpr[5]._u32[0], H = gpr[6]._u32[0];
    const uint32_t S26 = gpr[7]._u32[0] << 2, dstr = gpr[8]._u32[0];
    const uint32_t S37 = gpr[9]._u32[0] ? S26 : 0u;
    if (W == 0 || (W & 7u) || W > 0x8000u || H == 0 || H > 0x8000u || S26 >= R2NJ_LS_SIZE) return 0;
    const int32_t ds = (int32_t)dstr;
    if (ds <= -(int32_t)R2NJ_LS_SIZE || ds >= (int32_t)R2NJ_LS_SIZE) return 0;
    const uint32_t ns = W >> 3;
    /* used reads: A/B rows -1..H, L/R rows 0..H-1 */
    int64_t qlo = INT64_MAX, qhi = INT64_MIN;
    for (uint32_t s = 0; s < ns; s++) {
        const int64_t b = (int64_t)src + 32 * (int64_t)s;
        const int64_t lb = b - 16 + (s == 0 ? (int64_t)S37 : 0), rb = b + 32 - (s == ns - 1 ? (int64_t)S37 : 0);
        int64_t lo = b, hi = b + 16;
        if (lb < lo) lo = lb;
        if (rb > hi) hi = rb;
        if (lb > hi) hi = lb;
        if (rb < lo) lo = rb;
        if (lo < qlo) qlo = lo;
        if (hi > qhi) qhi = hi;
    }
    const int64_t rlo = (qlo & ~15ll) - (int64_t)S26, rhi = (qhi & ~15ll) + 16 + (int64_t)H * (int64_t)S26;
    const int64_t w0 = (int64_t)dst, w1 = (int64_t)dst + 16 * (int64_t)(ns - 1);
    const int64_t wt = (int64_t)(H - 1) * (int64_t)ds;
    const int64_t wlo = ((wt < 0 ? w0 + wt : w0) & ~15ll), whi = ((wt < 0 ? w1 : w1 + wt) & ~15ll) + 16;
    if (!nj_inls(rlo, rhi) || !nj_inls(wlo, whi)) return 0;
    if (nj_ovl(wlo, whi, rlo, rhi)) return 0;

    nj_shf C23, C24, C22, C25;
    nj_shf_mk(&C23, nj_ldq(ls, 0x3A860)); nj_shf_mk(&C24, nj_ldq(ls, 0x3A880));
    nj_shf_mk(&C22, nj_ldq(ls, 0x3AAF0)); nj_shf_mk(&C25, nj_ldq(ls, 0x3ACA0));
    const __m256 c10 = nj_in(nj_bc(&gpr[10])), c11 = nj_in(nj_bc(&gpr[11]));
    const __m256 c12 = nj_in(nj_bc(&gpr[12])), c13 = nj_in(nj_bc(&gpr[13]));

    for (uint32_t s = 0; s < ns; s++) {
        const uint32_t b = src + 32u * s, b16 = b + 16u;
        const uint32_t lb = b - 16u + (s == 0 ? S37 : 0u), rb = b + 32u - (s == ns - 1 ? S37 : 0u);
        const int inner = lb == b - 16u && rb == b + 32u;
        const uint32_t d = dst + 16u * s;
        /* one row per ymm: [A|B] (8 contiguous floats), [L|A], [B|R]; the
         * row t +- 1 values roll from row to row */
        __m256 abm = nj_in(nj_ld32(ls, b - S26)), ab0 = nj_in(nj_ld32(ls, b));
        for (uint32_t t = 0; t < H; t++) {
            const uint32_t o = t * S26;
            const __m256i AB = nj_ld32(ls, b + o);
            const __m256i LA = inner ? nj_ld32(ls, lb + o) : nj_ld2(ls, lb + o, b + o);
            const __m256i BR = inner ? nj_ld32(ls, b16 + o) : nj_ld2(ls, b16 + o, rb + o);
            const __m256 abp = nj_in(nj_ld32(ls, b + o + S26));
            const __m256 dx = nj_fs(nj_in(nj_shf_ap(&C23, AB, BR)), nj_in(nj_shf_ap(&C24, LA, AB)));
            const __m256 dy = nj_fs(abp, abm);
            const __m256i PQ = nj_cflts24(nj_fma(dy, c12, nj_fm(dx, c10)));
            const __m256i UV = nj_cflts24(nj_fma(dy, c13, nj_fm(dx, c11)));
            const __m256i Z = nj_shf_ap(&C22, _mm256_permute2x128_si256(PQ, UV, 0x20), _mm256_permute2x128_si256(PQ, UV, 0x31));
            const __m256i O = nj_shf_ap(&C25, Z, _mm256_permute2x128_si256(Z, Z, 0x11));
            nj_stq(ls, d + t * dstr, _mm256_castsi256_si128(O));
            abm = ab0; ab0 = abp;
        }
    }
    return 1;
}

/* ======================================================================
 * 7db0 0xAC48 -- 2x upsample of a half-resolution float grid + blend with
 * two ring buffers (the water simulation's coarse -> fine step).
 * In:  $r3 dst, $r4 src (half-res rows, 2*W bytes each), $r5 / $r7 / $r9
 *      scalars (splatted by the 0x3A820 control), $r6 ring (8 KB, wrap 0x1FF0),
 *      $r8 ring (4 KB, wrap 0xFF0), $r10 W (full-res floats per row, multiple
 *      of 8), $r11 H (full-res rows, even).
 * LS tables: 0x3A890 / 0x3A8A0 (left output quad), 0x3A8E0 / 0x3A8B0 (right).
 * Per 8-float output strip s, half-res row k (bottom-up; row -1 wraps to
 * row 0 of the grid, the strip after the last wraps to strip 0):
 *   cur  = fm(0.5*r5, fa(sh(q0,q1,C890|C8E0), sh(q0,q1,C8A0|C8B0)))
 *   out row 2k'   = fma(r9, R8[j], fma(r7, R6[i], cur))
 *   out row 2k'+1 = fma(r9, R8[j+128], fma(r7, R6[i+128], fm(0.5, fa(cur, prev))))
 * with i / j stepping -256 through the rings. Declines on stores that could
 * reach used input, LS wrap and loop counts the lifted code mis-handles. */
int r2nj_7db0_upblend(uint8_t* ls, u128* gpr)
{
    const uint32_t r3 = gpr[3]._u32[0], r4 = gpr[4]._u32[0], r6 = gpr[6]._u32[0], r8 = gpr[8]._u32[0];
    const uint32_t W = gpr[10]._u32[0], H = gpr[11]._u32[0];
    if (W == 0 || (W & 7u) || W > 0x4000u || H == 0 || (H & 1u) || H > 0x4000u) return 0;
    const uint32_t S24 = (W >> 2) << 5, S48 = (W >> 2) << 4, S26 = W >> 3, S25 = S26 << 4;
    const int32_t S54 = (int32_t)(int16_t)W * (int32_t)(int16_t)(H - 2u);
    const int32_t S55 = (int32_t)(int16_t)(W >> 1) * (int32_t)(int16_t)((H >> 1) - 1u);
    const uint32_t S49 = (uint32_t)S54 << 2, S29 = (uint32_t)S55 << 2;
    const uint32_t S39 = S49 + S24, S40 = r3 - S24, S31 = S40 + S48;
    const uint32_t ns = W >> 3, n = H >> 1;

    /* used reads: src rows 0..n-1 (X_0 .. X_{n-1}, X_{n-1} == the wrap row) */
    const int64_t x0 = (int64_t)(int32_t)S29, step = (int64_t)S25;
    const int64_t xlo = (x0 - (int64_t)(n - 1) * step < 0 ? x0 - (int64_t)(n - 1) * step : 0);
    const int64_t xhi = (x0 > 0 ? x0 : 0);
    const int64_t offmin = 16 - 16 * (int64_t)S26;           /* the last strip's wrap */
    const int64_t slo = (((int64_t)r4 + xlo + (offmin < 0 ? offmin : 0)) & ~15ll);
    const int64_t shi = (((int64_t)r4 + 16 * (int64_t)(ns - 1) + xhi + 16) & ~15ll) + 16;
    const int64_t r6lo = (int64_t)r6 & ~15ll, r6hi = ((int64_t)r6 & ~15ll) + 8192 + 160;
    const int64_t r8lo = (int64_t)r8 & ~15ll, r8hi = ((int64_t)r8 & ~15ll) + 4096 + 160;
    /* stores: S40 + S44_k + {0..S48+32}, S44_k = 32 s + S39 - k S24 */
    const int64_t wb = (int64_t)r3 + (int64_t)(int32_t)S49;
    const int64_t wlo = (wb - (int64_t)(n - 1) * (int64_t)S24) & ~15ll;
    const int64_t whi = ((wb + 32 * (int64_t)(ns - 1) + (int64_t)S48 + 16) & ~15ll) + 16;
    if (!nj_inls(slo, shi) || !nj_inls(r6lo, r6hi) || !nj_inls(r8lo, r8hi) || !nj_inls(wlo, whi)) return 0;
    if (nj_ovl(wlo, whi, slo, shi) || nj_ovl(wlo, whi, r6lo, r6hi) || nj_ovl(wlo, whi, r8lo, r8hi)) return 0;

    nj_shf SPL, CA, CB;
    nj_shf_mk(&SPL, nj_ldq(ls, 0x3A820));
    nj_shf_mk2(&CA, nj_ldq(ls, 0x3A890), nj_ldq(ls, 0x3A8E0));
    nj_shf_mk2(&CB, nj_ldq(ls, 0x3A8A0), nj_ldq(ls, 0x3A8B0));
    const __m256i v5 = nj_bc(&gpr[5]), v7 = nj_bc(&gpr[7]), v9 = nj_bc(&gpr[9]);
    const __m256 half = _mm256_set1_ps(0.5f);
    const __m256 k27 = nj_fm(half, nj_in(nj_shf_ap(&SPL, v5, v5)));
    const __m256 k23 = nj_in(nj_shf_ap(&SPL, v7, v7)), k28 = nj_in(nj_shf_ap(&SPL, v9, v9));

    for (uint32_t s = 0; s < ns; s++) {
        const uint32_t S41 = 32u * s, r10c = W - 8u * s;
        const uint32_t off1 = (1u - (r10c == 8u ? S26 : 0u)) << 4;
        uint32_t S68 = (S41 & 112u) + 3840u;
        uint32_t S45 = S68 + ((S41 & 128u) ? 0u : 4096u);
        const uint32_t Z = r4 + (S41 >> 1);
        uint32_t X = Z + S29, S44 = S41 + S39;
        __m256i q0 = _mm256_broadcastsi128_si256(nj_ldq(ls, Z)), q1 = _mm256_broadcastsi128_si256(nj_ldq(ls, Z + off1));
        __m256 prev = nj_fm(k27, nj_fa(nj_in(nj_shf_ap(&CA, q0, q1)), nj_in(nj_shf_ap(&CB, q0, q1))));
        for (uint32_t k = 0; k < n; k++) {
            q0 = _mm256_broadcastsi128_si256(nj_ldq(ls, X));
            q1 = _mm256_broadcastsi128_si256(nj_ldq(ls, X + off1));
            const __m256 cur = nj_fm(k27, nj_fa(nj_in(nj_shf_ap(&CA, q0, q1)), nj_in(nj_shf_ap(&CB, q0, q1))));
            const __m256 o0 = nj_fma(k28, nj_in(nj_ld32(ls, r8 + S68)),
                                     nj_fma(k23, nj_in(nj_ld32(ls, r6 + S45)), cur));
            const __m256 o1 = nj_fma(k28, nj_in(nj_ld32(ls, r8 + S68 + 128u)),
                                     nj_fma(k23, nj_in(nj_ld32(ls, r6 + S45 + 128u)), nj_fm(half, nj_fa(cur, prev))));
            nj_st32(ls, S40 + S44, _mm256_castps_si256(o0));
            nj_st32(ls, S31 + S44, _mm256_castps_si256(o1));
            prev = cur;
            S45 = (S45 - 256u) & 8176u;
            S68 = (S68 - 256u) & 4080u;
            S44 -= S24;
            X -= S25;
        }
    }
    return 1;
}

/* ======================================================================
 * 4154 0x81C0 -- ocean spectrum phasor rotation (FFT input).
 * In:  $r3 dst (8 bytes per element), $r4 cos/sin table (8 bytes per entry),
 *      $r5 pairs of s8 (a, b), $r6 count, LS[0x335C0] word 0 = pointer to
 *      one u8 phase index per element.
 *   A = a / 128, B = b / 128 (csflt 155, fm 2^-7: exact), (c, s) = table[idx]
 *   dst[0] = fms(c, A, fm(s, B)) = c*A - s*B,  dst[4] = fma(s, A, fm(c, B)) = s*A + c*B.
 * Declines on unaligned table / dst words, LS wrap and stores into any input. */
static inline void nj_phasor8(const uint8_t* ls, uint32_t tab, const __m256i idx, const __m256i ab32a,
                              const __m256i ab32b, __m256i* w0, __m256i* w4)
{
    const __m256i bsw = _mm256_broadcastsi128_si256(nj_wswap128());
    const __m256i off = _mm256_slli_epi32(idx, 3);
    const __m256i cw = _mm256_shuffle_epi8(_mm256_i32gather_epi32((const int*)(ls + tab), off, 1), bsw);
    const __m256i sw = _mm256_shuffle_epi8(_mm256_i32gather_epi32((const int*)(ls + tab + 4u), off, 1), bsw);
    const __m256 c = nj_in(cw), s = nj_in(sw);
    const __m256 sc = _mm256_set1_ps(1.0f / 128.0f);
    const __m256 A = _mm256_mul_ps(_mm256_cvtepi32_ps(ab32a), sc), B = _mm256_mul_ps(_mm256_cvtepi32_ps(ab32b), sc);
    const __m256 cB = nj_fm(c, B), sB = nj_fm(s, B);
    *w4 = _mm256_castps_si256(nj_fma(s, A, cB));
    *w0 = _mm256_castps_si256(nj_fms(c, A, sB));
}
int r2nj_4154_phasor(uint8_t* ls, u128* gpr)
{
    const uint32_t dst = gpr[3]._u32[0], tab = gpr[4]._u32[0], src = gpr[5]._u32[0], n = gpr[6]._u32[0];
    if (n == 0 || n > 0x8000u || (dst & 3u) || (tab & 3u)) return 0;
    const uint32_t p = ((uint32_t)ls[0x335C0] << 24) | ((uint32_t)ls[0x335C1] << 16) | ((uint32_t)ls[0x335C2] << 8) | ls[0x335C3];
    const int64_t dlo = dst, dhi = (int64_t)dst + 8 * (int64_t)n;
    const int64_t ilo = p, ihi = (int64_t)p + n, slo = src, shi = (int64_t)src + 2 * (int64_t)n;
    const int64_t tlo = tab, thi = (int64_t)tab + 256 * 8;
    if (!nj_inls(dlo, dhi) || !nj_inls(ilo, ihi) || !nj_inls(slo, shi) || !nj_inls(tlo, thi)) return 0;
    if (nj_ovl(dlo, dhi, ilo, ihi) || nj_ovl(dlo, dhi, slo, shi) || nj_ovl(dlo, dhi, tlo, thi)) return 0;
    if (ihi + 8 > (int64_t)R2NJ_LS_SIZE || shi + 16 > (int64_t)R2NJ_LS_SIZE) return 0;   /* 8/16-byte vector loads */

    const __m256i bsw = _mm256_broadcastsi128_si256(nj_wswap128());
    const __m128i even = _mm_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i odd  = _mm_setr_epi8(1, 3, 5, 7, 9, 11, 13, 15, -1, -1, -1, -1, -1, -1, -1, -1);
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256i idx = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i*)(ls + p + i)));
        const __m128i ab = _mm_loadu_si128((const __m128i*)(ls + src + 2 * i));
        const __m256i a32 = _mm256_cvtepi8_epi32(_mm_shuffle_epi8(ab, even));
        const __m256i b32 = _mm256_cvtepi8_epi32(_mm_shuffle_epi8(ab, odd));
        __m256i w0, w4;
        nj_phasor8(ls, tab, idx, a32, b32, &w0, &w4);
        const __m256i lo = _mm256_unpacklo_epi32(w0, w4), hi = _mm256_unpackhi_epi32(w0, w4);
        _mm256_storeu_si256((__m256i*)(ls + dst + 8 * i), _mm256_shuffle_epi8(_mm256_permute2x128_si256(lo, hi, 0x20), bsw));
        _mm256_storeu_si256((__m256i*)(ls + dst + 8 * i + 32), _mm256_shuffle_epi8(_mm256_permute2x128_si256(lo, hi, 0x31), bsw));
    }
    if (i < n) {
        int32_t ix[8] = { 0 }, av[8] = { 0 }, bv[8] = { 0 };
        const uint32_t m = n - i;
        for (uint32_t j = 0; j < m; j++) {
            ix[j] = ls[p + i + j];
            av[j] = (int8_t)ls[src + 2 * (i + j)];
            bv[j] = (int8_t)ls[src + 2 * (i + j) + 1];
        }
        __m256i w0, w4;
        nj_phasor8(ls, tab, _mm256_loadu_si256((const __m256i*)ix), _mm256_loadu_si256((const __m256i*)av),
                   _mm256_loadu_si256((const __m256i*)bv), &w0, &w4);
        uint32_t o0[8], o4[8];
        _mm256_storeu_si256((__m256i*)o0, w0); _mm256_storeu_si256((__m256i*)o4, w4);
        for (uint32_t j = 0; j < m; j++) {
            uint8_t* q = ls + dst + 8 * (i + j);
            q[0] = (uint8_t)(o0[j] >> 24); q[1] = (uint8_t)(o0[j] >> 16); q[2] = (uint8_t)(o0[j] >> 8); q[3] = (uint8_t)o0[j];
            q[4] = (uint8_t)(o4[j] >> 24); q[5] = (uint8_t)(o4[j] >> 16); q[6] = (uint8_t)(o4[j] >> 8); q[7] = (uint8_t)o4[j];
        }
    }
    return 1;
}

/* ======================================================================
 * dd413 0x42E0 -- triangle cull + index compaction (the CPU reads its output).
 * In:  $r3 dst (u16 indices, 6 bytes per kept triangle), $r4 u16 index list,
 *      $r5 vertex base (16-byte quads from 0x4030: word 0 clip code, 1..3
 *      x/y/z), $r6 index count, $r7 word k < 0: flip winding of triangles
 *      2k, 2k+1. Out: $r3 word k = min(kept bytes, 2 * $r6.k) / 2 (word 0;
 *      words 1..3 = clamp of 0).
 * Prologue: zero the index list tail (from $r4 + 2 $r6 to the 4th quad).
 * Per group of 8 triangles (48 index bytes, unaligned): vertex quad of an
 *      index word w = (A << 16 | B): A at $r5 + rotl(w, 20), B at
 *      $r5 + rotl(w, 4) (masked to quads; = $r5 + 16 index for < 4096);
 *      kept = clip(A) & clip(B) & clip(C) == 0 && sign(A . (B x C)) == flip;
 *      the 8 kept bits pick a 32-byte shufb table at 0x5D40 that, rotated by
 *      dst & 15, packs the kept triangles' 6-byte index triples into 4 quads
 *      at dst (first quad merged with the bytes below dst); dst += 6 kept.
 * The lifted loop is software-pipelined: group g+1's index and vertex loads
 * (and the next group's first index quad one group earlier) come before group
 * g's stores. Native mirrors exactly that load/store order, so any aliasing
 * of dst with the index list / vertices behaves the same. The constant
 * tables 0x7D40..0x7DAF (read once, after the tail clear) must hold the
 * module image's values; counts that make the lifted loop spin (<= 0) and a
 * tail clear landing on those tables decline. */
static const uint8_t nj_cull_tab[0x70] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
    0x04,0x05,0x06,0x07,0x14,0x15,0x16,0x17,0x00,0x01,0x02,0x03,0x10,0x11,0x12,0x13,
    0x00,0x01,0x02,0x03,0x10,0x11,0x12,0x13,0x04,0x05,0x06,0x07,0x14,0x15,0x16,0x17,
    0x08,0x09,0x0A,0x0B,0x18,0x19,0x1A,0x1B,0x0C,0x0D,0x0E,0x0F,0x1C,0x1D,0x1E,0x1F,
    0x0C,0x0D,0x0E,0x0F,0x1C,0x1D,0x1E,0x1F,0x08,0x09,0x0A,0x0B,0x18,0x19,0x1A,0x1B,
    0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
    0x08,0x09,0x18,0x19,0x0C,0x0D,0x1C,0x1D,0x00,0x01,0x10,0x11,0x04,0x05,0x14,0x15 };
/* raw (LS byte order) quads */
static inline __m128i nj_ldr(const uint8_t* ls, uint32_t a) { return _mm_loadu_si128((const __m128i*)(ls + (a & NJ_QM))); }
static inline void nj_str(uint8_t* ls, uint32_t a, __m128i v) { _mm_storeu_si128((__m128i*)(ls + (a & NJ_QM)), v); }
/* shufb on raw quads with a data-dependent control */
static inline __m128i nj_shufb_raw(__m128i a, __m128i b, __m128i c)
{
    const __m128i e0m = _mm_set1_epi8((char)0xE0);
    const __m128i hi = _mm_cmplt_epi8(c, _mm_setzero_si128());
    const __m128i top = _mm_and_si128(c, e0m);
    const __m128i spec = _mm_or_si128(_mm_and_si128(_mm_cmpeq_epi8(top, e0m), _mm_set1_epi8((char)0x80)),
                                      _mm_cmpeq_epi8(top, _mm_set1_epi8((char)0xC0)));
    const __m128i ix = _mm_or_si128(_mm_and_si128(c, _mm_set1_epi8(15)), hi);
    const __m128i r = _mm_blendv_epi8(_mm_shuffle_epi8(a, ix), _mm_shuffle_epi8(b, ix), _mm_slli_epi16(c, 3));
    return _mm_or_si128(r, spec);
}
static inline __m128i nj_selb(__m128i a, __m128i b, __m128i m) { return _mm_or_si128(_mm_andnot_si128(m, a), _mm_and_si128(b, m)); }
typedef struct { __m128i I[3], q3; __m128i V[24]; } nj_cull_grp;
/* index quads (q0 = the previous group's 4th quad, loaded with it) + the
 * 24 vertex quads of one group */
static inline void nj_cull_ld(const uint8_t* ls, uint32_t ib, uint32_t r5, __m128i q0, nj_cull_grp* g)
{
    _Alignas(16) uint8_t b[64];
    const uint32_t s = ib & 15u;
    g->q3 = nj_ldr(ls, ib + 0x30u);
    _mm_store_si128((__m128i*)b, q0);
    _mm_store_si128((__m128i*)(b + 16), nj_ldr(ls, ib + 0x10u));
    _mm_store_si128((__m128i*)(b + 32), nj_ldr(ls, ib + 0x20u));
    _mm_store_si128((__m128i*)(b + 48), g->q3);
    for (int k = 0; k < 3; k++) g->I[k] = _mm_loadu_si128((const __m128i*)(b + s + 16 * k));
    for (int j = 0; j < 12; j++) {
        const uint8_t* p = b + s + 4 * j;
        const uint32_t w = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        g->V[2 * j]     = nj_ldr(ls, r5 + (w << 20 | w >> 12));
        g->V[2 * j + 1] = nj_ldr(ls, r5 + (w << 4 | w >> 28));
    }
}
/* the 4 words of vertex role r (0..2) of the 8 triangles, host form, lane t = triangle t */
static inline void nj_cull_soa(const nj_cull_grp* g, int r, __m256i c[4])
{
    __m256i x[4];
    for (int t = 0; t < 4; t++)
        x[t] = _mm256_inserti128_si256(_mm256_castsi128_si256(g->V[3 * t + r]), g->V[3 * (t + 4) + r], 1);
    const __m256i t0 = _mm256_unpacklo_epi32(x[0], x[1]), t1 = _mm256_unpackhi_epi32(x[0], x[1]);
    const __m256i t2 = _mm256_unpacklo_epi32(x[2], x[3]), t3 = _mm256_unpackhi_epi32(x[2], x[3]);
    const __m256i sw = _mm256_broadcastsi128_si256(nj_wswap128());
    c[0] = _mm256_shuffle_epi8(_mm256_unpacklo_epi64(t0, t2), sw);
    c[1] = _mm256_shuffle_epi8(_mm256_unpackhi_epi64(t0, t2), sw);
    c[2] = _mm256_shuffle_epi8(_mm256_unpacklo_epi64(t1, t3), sw);
    c[3] = _mm256_shuffle_epi8(_mm256_unpackhi_epi64(t1, t3), sw);
}
int r2nj_dd413_cull(uint8_t* ls, u128* gpr)
{
    const uint32_t r3 = gpr[3]._u32[0], r4 = gpr[4]._u32[0], r5 = gpr[5]._u32[0], r6 = gpr[6]._u32[0];
    const int32_t n = (int32_t)((uint32_t)(uint16_t)(r6 + 23u) * 0xAAABu) >> 20;
    if (n <= 0) return 0;
    if (memcmp(ls + 0x7D40, nj_cull_tab, sizeof nj_cull_tab)) return 0;
    const uint32_t e = r4 + (r6 << 1);
    for (uint32_t k = 0; k < 4; k++) {
        const uint32_t q = (e + 16u * k) & NJ_QM;
        if (q >= 0x7D40u && q < 0x7DB0u) return 0;
    }
    /* tail clear */
    {
        const __m128i iota = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        const __m128i keep = _mm_cmpgt_epi8(_mm_set1_epi8((char)(e & 15u)), iota);
        const __m128i old = nj_ldr(ls, e);
        nj_str(ls, e + 0x10u, _mm_setzero_si128());
        nj_str(ls, e + 0x20u, _mm_setzero_si128());
        nj_str(ls, e + 0x30u, _mm_setzero_si128());
        nj_str(ls, e, _mm_and_si128(old, keep));
    }
    const __m256i flip = _mm256_srai_epi32(_mm256_permutevar8x32_epi32(
        _mm256_castsi128_si256(_mm_loadu_si128((const __m128i*)gpr[7]._u32)), _mm256_setr_epi32(0, 0, 1, 1, 2, 2, 3, 3)), 31);
    const __m128i iota = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    const __m128i hswap = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
    const __m128i m_lo = _mm_set1_epi16((short)0xFE00), m_hi = _mm_set1_epi16(0x00FE);   /* ilh 0x00FE / 0xFE00 */
    nj_cull_grp gb[2];
    nj_cull_grp* cur = &gb[0];
    nj_cull_grp* nxt = &gb[1];
    nj_cull_ld(ls, r4, r5, nj_ldr(ls, r4), cur);
    uint32_t p = r3;
    for (int32_t g = 0; g < n; g++) {
        nj_cull_ld(ls, r4 + 48u * (uint32_t)(g + 1), r5, cur->q3, nxt);
        __m256i A[4], B[4], C[4];
        nj_cull_soa(cur, 0, A); nj_cull_soa(cur, 1, B); nj_cull_soa(cur, 2, C);
        const __m256i out = _mm256_xor_si256(_mm256_cmpeq_epi32(_mm256_and_si256(_mm256_and_si256(A[0], B[0]), C[0]),
                                                                _mm256_setzero_si256()), _mm256_set1_epi32(-1));
        const __m256 a1 = nj_xf_in(A[1]), a2 = nj_xf_in(A[2]), a3 = nj_xf_in(A[3]);
        const __m256 b1 = nj_xf_in(B[1]), b2 = nj_xf_in(B[2]), b3 = nj_xf_in(B[3]);
        const __m256 c1 = nj_xf_in(C[1]), c2 = nj_xf_in(C[2]), c3 = nj_xf_in(C[3]);
        const __m256 e1 = nj_fms(b1, c2, nj_fm(b2, c1));
        const __m256 e2 = nj_fms(b3, c1, nj_fm(b1, c3));
        const __m256 e3 = nj_fms(b2, c3, nj_fm(b3, c2));
        const __m256 d = nj_fma(e1, a3, nj_fma(e2, a2, nj_fm(e3, a1)));
        const __m256i drop = _mm256_or_si256(out, _mm256_xor_si256(_mm256_castps_si256(d), flip));
        const uint32_t v = ~(uint32_t)_mm256_movemask_ps(_mm256_castsi256_ps(drop)) & 0xFFu;
        const uint32_t m = (uint32_t)((((v * 0x0802ull & 0x22110ull) | (v * 0x8020ull & 0x88440ull)) * 0x10101ull) >> 16) & 0xFFu;   /* bit-reverse: gbh puts triangle 0 in bit 7 */
        /* compaction */
        const __m128i ss = _mm_set1_epi8((char)(p & 15u));
        const __m128i rot = _mm_and_si128(_mm_sub_epi8(iota, ss), _mm_set1_epi8(15));
        const __m128i old = nj_ldr(ls, p);
        const __m128i c511 = _mm_shuffle_epi8(nj_ldr(ls, 0x5D40u + 32u * m), rot);
        const __m128i c513 = _mm_shuffle_epi8(nj_ldr(ls, 0x5D50u + 32u * m), rot);
        const __m128i n520 = _mm_shuffle_epi8(c511, hswap);
        const __m128i n531 = nj_selb(c511, n520, m_lo);
        const __m128i n534 = nj_selb(c511, n520, m_hi);
        const __m128i n537 = _mm_cmplt_epi8(n531, _mm_setzero_si128());
        const __m128i n524 = nj_shufb_raw(cur->I[2], cur->I[2], c513);
        const __m128i n542 = _mm_andnot_si128(n537, n524);
        const __m128i n543 = nj_shufb_raw(cur->I[1], cur->I[2], n534);
        const __m128i n548 = _mm_blendv_epi8(nj_shufb_raw(cur->I[0], cur->I[1], n531), n524, n537);
        const __m128i msk = _mm_cmpgt_epi8(iota, _mm_sub_epi8(ss, _mm_set1_epi8(1)));   /* byte >= dst & 15 */
        nj_str(ls, p + 0x30u, _mm_andnot_si128(msk, n542));
        nj_str(ls, p + 0x20u, _mm_blendv_epi8(n543, n542, msk));
        nj_str(ls, p, _mm_blendv_epi8(old, n548, msk));
        nj_str(ls, p + 0x10u, _mm_blendv_epi8(n548, n543, msk));
        p += 6u * (uint32_t)_mm_popcnt_u32(m);
        nj_cull_grp* t = cur; cur = nxt; nxt = t;
    }
    for (int k = 0; k < 4; k++) {
        const uint32_t o = gpr[3]._u32[k], cv = k ? o : p, lim = o + (gpr[6]._u32[k] << 1);
        const uint32_t mn = (int32_t)cv > (int32_t)lim ? lim : cv;
        gpr[3]._u32[k] = (uint32_t)((int32_t)(mn - o) >> 1);
    }
    return 1;
}
#else
/* baseline build (PORT_SPU_AVX2=OFF, no /arch:AVX2): the kernels are
 * AVX2-only (the lifted code there uses the exact soft-float helpers, which
 * these do not reproduce), so every call declines and the lifted code runs;
 * r2nj_init also clears their mask bits. */
int r2nj_dd413_xform4(uint8_t* ls, u128* gpr) { (void)ls; (void)gpr; return 0; }
int r2nj_7db0_grad(uint8_t* ls, u128* gpr) { (void)ls; (void)gpr; return 0; }
int r2nj_7db0_upblend(uint8_t* ls, u128* gpr) { (void)ls; (void)gpr; return 0; }
int r2nj_4154_phasor(uint8_t* ls, u128* gpr) { (void)ls; (void)gpr; return 0; }
int r2nj_dd413_cull(uint8_t* ls, u128* gpr) { (void)ls; (void)gpr; return 0; }
#endif

/* ======================================================================
 * spuelf0 0x8240 -- Edge zlib inflate_fast (exact emulation lives in
 * native_edge_zlib.c; verified by scratch\edge_zlib_native). */
static int r2nj_edge_zlib(uint8_t* ls, u128* gpr)
{
    if (!native_edge_zlib_ls_matches(ls, &native_edge_zlib_r2)) return 0;
    native_edge_zlib_inflate_fast_ls(ls, gpr[3]._u32[0], gpr[4]._u32[0], &native_edge_zlib_r2);
    return 1;
}

/* ======================================================================
 * 029d48175a74 (img 203) 0x1DF90 -- memcpy($r3 dst, $r4 src, $r5 bytes).
 * The lifted routine copies quadwords (read-modify-write of the first and
 * last destination quads, shufb realignment through the shift-control
 * table at LS 0x1F1D0 + 16*k, k = 0..15, entry k = bytes k..k+15) and
 * returns $r3 unchanged. For byte-disjoint ranges (or dst == src) that is
 * exactly a byte copy. Decline: overlapping ranges, LS wrap, a changed
 * control table. Volatile $r2/$r4..$r53 are left as they were (no caller
 * reads them: replay-verified over every captured 203 job). */
static int r2nj_029d_tab_ok(const uint8_t* ls)
{
    for (int k = 0; k < 16; k++) {
        const __m128i want = _mm_add_epi8(_mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15),
                                          _mm_set1_epi8((char)k));
        const __m128i got = _mm_loadu_si128((const __m128i*)(ls + 0x1F1D0u + 16u * (uint32_t)k));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(got, want)) != 0xFFFF) return 0;
    }
    return 1;
}
int r2nj_029d_memcpy(uint8_t* ls, u128* gpr)
{
    const uint32_t d = gpr[3]._u32[0], s = gpr[4]._u32[0], n = gpr[5]._u32[0];
    if (n == 0) return 1;
    if (d >= R2NJ_LS_SIZE || s >= R2NJ_LS_SIZE || n > R2NJ_LS_SIZE - d || n > R2NJ_LS_SIZE - s) return 0;
    if (d != s && d < s + n && s < d + n) return 0;
    if (((d | s) & 15u) && !r2nj_029d_tab_ok(ls)) return 0;
    if (d != s) memcpy(ls + d, ls + s, n);
    return 1;
}

/* ---- table --------------------------------------------------------------- */
int r2nj_kernel_run(int k, uint8_t* ls, u128* gpr)
{
    switch (k) {
    case R2NJ_K_DD413_XFORM4: return r2nj_dd413_xform4(ls, gpr);
    case R2NJ_K_EDGE_ZLIB:    return r2nj_edge_zlib(ls, gpr);
    case R2NJ_K_7DB0_GRAD:    return r2nj_7db0_grad(ls, gpr);
    case R2NJ_K_7DB0_UPBLEND: return r2nj_7db0_upblend(ls, gpr);
    case R2NJ_K_4154_PHASOR:  return r2nj_4154_phasor(ls, gpr);
    case R2NJ_K_DD413_CULL:   return r2nj_dd413_cull(ls, gpr);
    case R2NJ_K_029D_MEMCPY:  return r2nj_029d_memcpy(ls, gpr);
    default:                  return 0;
    }
}
const char* r2nj_kernel_name(int k)
{
    static const char* const n[R2NJ_K_COUNT] = { "dd413_xform4", "edge_zlib", "7db0_grad", "7db0_upblend", "4154_phasor", "dd413_cull", "029d_memcpy" };
    return (unsigned)k < R2NJ_K_COUNT ? n[k] : "?";
}
uint32_t r2nj_kernel_entry(int k)
{
    static const uint32_t e[R2NJ_K_COUNT] = { 0x4030u, 0x8240u, 0x9440u, 0xAC48u, 0x81C0u, 0x42E0u, 0x1DF90u };
    return (unsigned)k < R2NJ_K_COUNT ? e[k] : 0;
}
/* No caller reads a volatile register these routines leave behind
 * (dd413 0x4030: the 0x4E08 continuation uses $r84..$r99 only and reloads
 * the rest; edge zlib: the inflate() caller re-derives everything; 7db0
 * 0x9440 / 0xAC48 are tail calls of wrappers that return straight to their
 * callers, 4154 0x81C0's caller (0x9A88) reloads r3..r6 before the 0x9F70 FFT).
 * dd413 0x42E0 returns the kept-index count in $r3 (its 0x4F38 caller reads
 * $r3 and no other volatile register). 029d 0x1DF90 (memcpy) returns $r3 unchanged. */
uint64_t r2nj_kernel_outregs(int k) { return k == R2NJ_K_DD413_CULL ? 1ull << 3 : 0; }
