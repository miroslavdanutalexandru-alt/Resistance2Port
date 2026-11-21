// Resistance 2: native replacements for the game's generic libc routines.
//
// The lifted versions run every byte/word through the guest-memory accessors
// (bounds check, coherence-bitmap probe, byte swap each); these do the same
// work on the host copy of guest memory directly. Guest memory is raw
// big-endian bytes at vm_base + ea, so a byte copy, fill or compare needs no
// swapping at all.
//
// Each lifted function starts with `if (r2_hot_xxx(ctx)) return;` (hooks in
// tools/hooks/r2.json). The native routine handles the call only in the
// "plain" domain and returns 0 otherwise, so the original lifted body runs:
//   - every byte touched, widened to the aligned 8-byte blocks the lifted
//     word-at-a-time loops may over-read, lies in [0x10000, 0xDFFFFFF8) (the
//     lifted accessors' inline range) and outside the gcm poll-word block;
//   - no destination line is reserved by an SPU (g_spu_coh_bitmap_p): such a
//     store must take the out-of-line path that raises the lost-reservation
//     event;
//   - source and destination do not overlap (memcpy/strcpy: the lifted copy
//     picks its direction from the low 12 address bits, not from overlap);
//   - lengths below 2 GiB; the word-at-a-time TOC constant is 0x7F..7F.
// Equivalence is at the ABI level: memory, r3 and every non-volatile register
// (r1, r2, r13-r31, lr, f14-f31, v20-v31, cr2-cr4) match the lifted code.
// The scratch registers the lifted body would also have changed (r0, r4-r12,
// ctr, xer, cr0/1/5-7) are left alone -- no caller may depend on them.
// scratch/hot/difftest proves it on random and edge-case inputs.
//
//   R2_HOT_NATIVE=0           all off (the lifted code runs, as before);
//                             default 1 (the libc routines below native)
//   R2_HOT_MEMCPY=0  R2_HOT_MEMSET=0  R2_HOT_STRCMP=0  R2_HOT_STRLEN=0
//   R2_HOT_STRCPY=0  R2_HOT_MEMCMP=0  R2_HOT_GSTRCMP=0   one routine off
//   Engine leaves (hot_native_plan.md #5-#8), default ON since the native-hot
//   pass (scratch\hot\difftest: 0 failures, mutations caught); =0 turns one off:
//   R2_HOT_BITSCAN=0  func_00AF2700 first set bit of a 192-bit BE bitmap
//   R2_HOT_MIN6=0     func_0079F394 (-O0) min of 7 u16, stack image reproduced
//   R2_HOT_U16MIN=0   func_00B15354 (-O0) min of two u16, stack image reproduced
//   R2_HOT_GCMTEX=0   func_00A92CDC gcm SET_TEXTURE command inline
//   R2_HOT_GCMVPC=0   func_00AD74F4 gcm TRANSFORM_CONSTANT_LOAD (4x4) inline
//   The gcm writers advance the command pointer (not idempotent), so
//   R2_HOT_SAMPLE never times them (their [hot] line has no cyc/call).
//   R2_HOT_LOG=0              no [hot] lines (default: one block every ~10 s)
//   R2_HOT_SAMPLE=<n>         time 1 call in n per thread (default 4096): the
//                             lifted body, then the native one on the same
//                             inputs (every replacement is idempotent), and
//                             check r3 agrees -- [hot] reports the estimated
//                             time saved and any mismatch. A mismatch is re-run
//                             once on both sides: if each repeats its result
//                             (a real difference) the routine is turned off
//                             for the rest of the run (the lifted result is
//                             kept); if either changes, the inputs moved under
//                             it (SPU-written counters, for min6) and it only
//                             counts as race=. 0 = no sampling.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include "ppu_recomp.h"
#include "port_native_summary.h"

/* lifted functions come from ppu_recomp.h (C++ linkage) */
void func_00A56E30(ppu_context* ctx);   /* memcpy */
void func_00A63258(ppu_context* ctx);   /* memset */
void func_00A59DB0(ppu_context* ctx);   /* strcmp */
void func_00A5A310(ppu_context* ctx);   /* strlen */
void func_00A5A180(ppu_context* ctx);   /* strcpy */
void func_00A63330(ppu_context* ctx);   /* memcmp */
void func_006BB908(ppu_context* ctx);   /* game strcmp: signed chars, returns the difference */
void func_00AF2700(ppu_context* ctx);   /* bitmap: first set bit in [r4, 0xBF], words at r3+0x24 */
void func_0079F394(ppu_context* ctx);   /* -O0: min of the 7 u16 at *(obj)+idx*16+0x40+2 */
void func_00B15354(ppu_context* ctx);   /* -O0: min(*(u16*)r3, *(u16*)r4) */
void func_00A92CDC(ppu_context* ctx);   /* gcm inline SET_TEXTURE (0x1A00 x7 + 0x1840) */
void func_00AD74F4(ppu_context* ctx);   /* gcm inline TRANSFORM_CONSTANT_LOAD (0x1EFC x17) */

namespace {

enum { H_MEMCPY, H_MEMSET, H_STRCMP, H_STRLEN, H_STRCPY, H_MEMCMP, H_GSTRCMP,
       H_BITSCAN, H_MIN6, H_U16MIN, H_GCMTEX, H_GCMVPC, H_N };
const char* const k_name[H_N] = { "memcpy", "memset", "strcmp", "strlen", "strcpy", "memcmp", "gstrcmp",
                                  "bitscan", "min6", "u16min", "gcmtex", "gcmvpc" };
const char* const k_env[H_N] = { "R2_HOT_MEMCPY", "R2_HOT_MEMSET", "R2_HOT_STRCMP", "R2_HOT_STRLEN",
                                 "R2_HOT_STRCPY", "R2_HOT_MEMCMP", "R2_HOT_GSTRCMP",
                                 "R2_HOT_BITSCAN", "R2_HOT_MIN6", "R2_HOT_U16MIN", "R2_HOT_GCMTEX", "R2_HOT_GCMVPC" };
/* per-routine default under R2_HOT_NATIVE=1 (all proven by scratch\hot\difftest) */
const int k_def[H_N] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
/* R2_HOT_SAMPLE re-runs the native routine after the lifted one: only for
 * routines whose second run is a no-op (not the gcm writers, which advance
 * the command pointer) */
constexpr unsigned kNoSample = (1u << H_GCMTEX) | (1u << H_GCMVPC);

/* ---- the domain ---- */
constexpr uint64_t kLo = 0x10000u, kHi = 0xDFFFFFF8u;
constexpr uint64_t kPollLo = 0x20002000u, kPollHi = 0x20002010u;

/* bytes [ea, ea+n), widened to whole 8-byte blocks, all in the inline range */
inline bool span_ok(uint32_t ea, uint64_t n)
{
    const uint64_t a = ea & ~7ull, b = ((uint64_t)ea + n + 7) & ~7ull;
    return a >= kLo && b <= kHi && !(a < kPollHi && b > kPollLo);
}
inline bool blk_ok(uint32_t ea) { return span_ok(ea, 1); }

/* no SPU-reserved 128-byte line in [ea, ea+n) */
inline bool coh_ok(uint32_t ea, uint64_t n)
{
    if (!g_spu_coh_armed || !n) return true;
    const unsigned char* bm = g_spu_coh_bitmap_p;
    const uint32_t l1 = (uint32_t)(((uint64_t)ea + n - 1) >> 7);
    for (uint32_t l = ea >> 7; l <= l1; l++) {
        if (!(l & 7) && l + 7 <= l1) { if (bm[l >> 3]) return false; l += 7; continue; }
        if ((bm[l >> 3] >> (l & 7)) & 1u) return false;
    }
    return true;
}

/* widened ranges intersect */
inline bool overlap(uint32_t a, uint64_t na, uint32_t b, uint64_t nb)
{
    const uint64_t a0 = a & ~7ull, a1 = ((uint64_t)a + na + 7) & ~7ull;
    const uint64_t b0 = b & ~7ull, b1 = ((uint64_t)b + nb + 7) & ~7ull;
    return a0 < b1 && b0 < a1;
}

inline uint64_t ld_be64(uint32_t ea) { uint64_t v; memcpy(&v, vm_base + ea, 8); return _byteswap_uint64(v); }
inline uint32_t hn_ld32(uint32_t ea) { uint32_t v; memcpy(&v, vm_base + ea, 4); return _byteswap_ulong(v); }
inline uint16_t hn_ld16(uint32_t ea) { uint16_t v; memcpy(&v, vm_base + ea, 2); return _byteswap_ushort(v); }
inline void hn_st64(uint32_t ea, uint64_t x) { x = _byteswap_uint64(x); memcpy(vm_base + ea, &x, 8); }
inline void hn_st32(uint32_t ea, uint32_t x) { x = _byteswap_ulong(x);  memcpy(vm_base + ea, &x, 4); }
inline void hn_st16(uint32_t ea, uint16_t x) { x = _byteswap_ushort(x); memcpy(vm_base + ea, &x, 2); }

/* the word-at-a-time zero-byte constant the lifted strlen/strcmp/strcpy load
 * from the TOC: if it is not 0x7F7F...7F they are not the routines we know */
inline bool toc_ok(uint64_t r2, uint32_t off)
{
    const uint32_t ea = (uint32_t)(r2 - off);
    return span_ok(ea, 8) && !(ea & 7) && ld_be64(ea) == 0x7F7F7F7F7F7F7F7Full;
}

inline bool has_zero(uint64_t w) { return ((w - 0x0101010101010101ull) & ~w & 0x8080808080808080ull) != 0; }

/* length of the NUL-terminated string at s, staying in the inline range;
 * -1 if it runs out of it */
inline int64_t bounded_strlen(uint32_t s)
{
    if (s < kLo || s >= kHi) return -1;
    const uint64_t lim = kHi - s;
    const size_t len = strnlen((const char*)vm_base + s, (size_t)lim);
    if (len >= lim) return -1;
    if (!span_ok(s, (uint64_t)len + 1)) return -1;
    return (int64_t)len;
}

/* ---- the routines: 1 = done (ctx updated), 0 = run the lifted body ---- */
int nat_memcpy(ppu_context* c, uint64_t* bytes)
{
    const uint64_t n = c->gpr[5];
    if (n >= 0x80000000ull) return 0;
    const uint32_t d = (uint32_t)c->gpr[3], s = (uint32_t)c->gpr[4];
    if (n) {
        if (!span_ok(s, n) || !span_ok(d, n) || overlap(d, n, s, n) || !coh_ok(d, n)) return 0;
        memcpy(vm_base + d, vm_base + s, (size_t)n);
    }
    c->gpr[3] = d;
    *bytes = n;
    return 1;
}

int nat_memset(ppu_context* c, uint64_t* bytes)
{
    const uint64_t n = c->gpr[5], r3 = c->gpr[3];
    if (n >= 0x80000000ull || (r3 >> 32)) return 0;
    const uint32_t d = (uint32_t)r3;
    if (n) {
        if (!span_ok(d, n) || !coh_ok(d, n)) return 0;
        memset(vm_base + d, (int)(uint8_t)c->gpr[4], (size_t)n);
    }
    *bytes = n;
    return 1;
}

int nat_strlen(ppu_context* c, uint64_t* bytes)
{
    const uint64_t r3 = c->gpr[3];
    if ((r3 >> 32) || !toc_ok(c->gpr[2], 0x1250)) return 0;
    const int64_t len = bounded_strlen((uint32_t)r3);
    if (len < 0) return 0;
    c->gpr[3] = (uint64_t)len;
    *bytes = (uint64_t)len;
    return 1;
}

int nat_strcpy(ppu_context* c, uint64_t* bytes)
{
    if (!toc_ok(c->gpr[2], 0x1258)) return 0;
    const uint32_t d = (uint32_t)c->gpr[3], s = (uint32_t)c->gpr[4];
    const int64_t len = bounded_strlen(s);
    if (len < 0) return 0;
    const uint64_t n = (uint64_t)len + 1;
    if (!span_ok(d, n) || overlap(d, n, s, n) || !coh_ok(d, n)) return 0;
    memcpy(vm_base + d, vm_base + s, (size_t)n);
    c->gpr[3] = d;
    *bytes = n;
    return 1;
}

int nat_memcmp(ppu_context* c, uint64_t* bytes)
{
    const uint32_t n = (uint32_t)c->gpr[5];
    if (n >= 0x80000000u) return 0;
    const uint32_t a = (uint32_t)c->gpr[3], b = (uint32_t)c->gpr[4];
    int r = 0;
    if (n) {
        if (!span_ok(a, n) || !span_ok(b, n)) return 0;
        r = memcmp(vm_base + a, vm_base + b, n);
    }
    c->gpr[3] = (uint64_t)(int64_t)(r < 0 ? -1 : r > 0 ? 1 : 0);
    *bytes = n;
    return 1;
}

/* unsigned-char strcmp returning -1/0/1 */
int nat_strcmp(ppu_context* c, uint64_t* bytes)
{
    if (!toc_ok(c->gpr[2], 0x1268)) return 0;
    const uint32_t a = (uint32_t)c->gpr[3], b = (uint32_t)c->gpr[4];
    const uint8_t* m = vm_base;
    int r = 0;
    for (uint32_t i = 0;; ) {
        const uint32_t pa = a + i, pb = b + i;
        if ((i == 0 || !(pa & 7)) && !blk_ok(pa)) return 0;
        if ((i == 0 || !(pb & 7)) && !blk_ok(pb)) return 0;
        if (!((pa | pb) & 7)) {
            uint64_t wa, wb; memcpy(&wa, m + pa, 8); memcpy(&wb, m + pb, 8);
            if (wa == wb && !has_zero(wa)) { i += 8; continue; }
        }
        const uint8_t x = m[pa], y = m[pb];
        if (x != y) { r = x < y ? -1 : 1; *bytes = i; break; }
        if (!x) { *bytes = i; break; }
        i++;
    }
    c->gpr[3] = (uint64_t)(int64_t)r;
    return 1;
}

/* func_006BB908: signed chars; returns -s2[i] at the end of s1, s1[i] at the
 * end of s2, else s1[i] - s2[i] at the first difference (sign-extended) */
int nat_gstrcmp(ppu_context* c, uint64_t* bytes)
{
    const uint32_t a = (uint32_t)c->gpr[3], b = (uint32_t)c->gpr[4];
    const uint8_t* m = vm_base;
    int32_t r;
    for (uint32_t i = 0;; i++) {
        const uint32_t pa = a + i, pb = b + i;
        if ((i == 0 || !(pa & 7)) && !blk_ok(pa)) return 0;
        if ((i == 0 || !(pb & 7)) && !blk_ok(pb)) return 0;
        const int32_t x = (int8_t)m[pa], y = (int8_t)m[pb];
        if (x == 0) { r = -y; *bytes = i; break; }
        if (y == 0) { r = x; *bytes = i; break; }
        if (x != y) { r = x - y; *bytes = i; break; }
    }
    c->gpr[3] = (uint64_t)(int64_t)r;
    return 1;
}

/* ---- engine leaves (hot_native_plan.md #5-#8; opt-in, see the header) ---- */

/* func_00AF2700: first i in [r4, 0xBF] whose bit (i & 31) is set in the BE word at
 * (u32)r3 + 0x24 + 4*(i >> 5), else -1. (s32)r4 > 0xBF returns -1 without reading
 * (cmpwi cr7,r4,0xBF; bgt cr7). r4 < 0: the lifted loop starts below the bitmap
 * (ctr = 0xC0 - r4) -- decline. */
int nat_bitscan(ppu_context* c, uint64_t*)
{
    const int32_t i0 = (int32_t)c->gpr[4];
    if (i0 > 0xBF) { c->gpr[3] = ~0ull; return 1; }
    if (i0 < 0) return 0;
    const uint64_t base = (uint64_t)(uint32_t)c->gpr[3] + 0x24u;
    const int w0 = i0 >> 5;
    if (base + 24 > 0xFFFFFFFFull || !span_ok((uint32_t)base + 4u * w0, 4u * (6 - w0))) return 0;
    for (int w = w0; w < 6; w++) {
        uint32_t m = hn_ld32((uint32_t)base + 4u * w);
        if (w == w0) m &= ~0u << (i0 & 31);
        if (m) { c->gpr[3] = (uint64_t)(int64_t)(w * 32 + __builtin_ctz(m)); return 1; }
    }
    c->gpr[3] = ~0ull;
    return 1;
}

/* func_00B15354 (-O0): r3 = *(u16*)a < *(u16*)b ? *a : *b (a = (u32)r3, b = (u32)r4).
 * Reproduces the stack traffic in the lifted order: back chain + saved r31 in the
 * dead frame below sp, a/b homed in the caller's parameter save area (sp+0x30/0x38),
 * then the loads, then the result local (sp-0x20). Operands inside that stack range
 * are declined, so a second run (R2_HOT_SAMPLE) is a no-op. */
int nat_u16min(ppu_context* c, uint64_t*)
{
    const uint64_t sp = c->gpr[1];
    if (sp >> 32) return 0;
    const uint32_t a = (uint32_t)c->gpr[3], b = (uint32_t)c->gpr[4], f = (uint32_t)sp - 0x50u;
    if (!span_ok(f, 0x90) || !coh_ok(f, 0x90) || !span_ok(a, 2) || !span_ok(b, 2)) return 0;
    if (overlap(a, 2, f, 0x90) || overlap(b, 2, f, 0x90)) return 0;
    hn_st64(f, sp);
    hn_st64(f + 0x48, c->gpr[31]);
    hn_st32(f + 0x80, a);
    hn_st32(f + 0x88, b);
    const uint16_t va = hn_ld16(a), vb = hn_ld16(b);
    const uint32_t r = va < vb ? va : vb;
    hn_st32(f + 0x30, r);
    c->gpr[3] = r;
    return 1;
}

/* func_0079F394 (-O0): e = *(u32*)obj + idx*16 + 0x40; return min(e[1..7]) as u16
 * (e[k] = BE u16 at e + 2k), via 6 calls of func_00B15354 (the loop is i <= 5). idx = (u32)r4 > 3 takes
 * the assert path -- decline. Reproduces the final stack image of the lifted code
 * (its frame and the last nested func_00B15354 frame). The memory it reads must
 * not alias that stack range (the lifted code reads it between the writes). */
int nat_min6(ppu_context* c, uint64_t*)
{
    const uint64_t sp = c->gpr[1];
    const uint32_t obj = (uint32_t)c->gpr[3], idx = (uint32_t)c->gpr[4];
    if (idx > 3 || (sp >> 32)) return 0;
    const uint32_t F = (uint32_t)sp - 0xA0u, G = F - 0x50u;
    constexpr uint32_t kStk = 0x50u + 0xE0u;                  /* [G, F + 0xE0) */
    if (!span_ok(G, kStk) || !coh_ok(G, kStk)) return 0;
    if (!span_ok(obj, 4) || overlap(obj, 4, G, kStk)) return 0;
    const uint32_t e = hn_ld32(obj) + (idx << 4) + 0x40u;
    if (!span_ok(e + 2, 14) || overlap(e + 2, 14, G, kStk)) return 0;
    uint16_t m = hn_ld16(e + 2), v = 0;
    std::atomic_thread_fence(std::memory_order_acq_rel);      /* lwsync */
    for (int i = 0; i < 6; i++) { v = hn_ld16(e + 4 + 2 * i); if (v < m) m = v; }
    hn_st64(F, sp);                                           /* stdu r1,-0xA0(r1) */
    hn_st64(F + 0x98, c->gpr[31]);
    hn_st64(F + 0xB0, c->lr);                                 /* LR save in the caller's frame */
    hn_st32(F + 0xD0, obj);
    hn_st32(F + 0xD8, idx);
    vm_base[F + 0x80] = 1;
    hn_st32(F + 0x74, e);
    hn_st16(F + 0x78, m);
    hn_st16(F + 0x7A, v);
    hn_st32(F + 0x70, 6);
    hn_st64(G, F);                                            /* last func_00B15354 frame */
    hn_st64(G + 0x48, F);                                     /* its saved r31 == F */
    hn_st32(G + 0x80, F + 0x7A);
    hn_st32(G + 0x88, F + 0x78);
    hn_st32(G + 0x30, m);
    c->gpr[3] = m;
    return 1;
}

/* func_00A92CDC: gcm SET_TEXTURE inline. p = *(u32*)(r3+8);
 * p[0] = 0x001C0000 | (0x1A00 + u*32), p[1..7] = src[0..6], p[8] = 0x00040000 | (0x1840 + u*4),
 * p[9] = src[7]; *(r3+8) = p + 0x28. Both sides are BE, so the payload is a raw copy.
 * Aliasing of p with src / r3+8 is declined (the lifted code interleaves the loads and
 * stores). In the domain the lifted vm_write32 is a plain inline store (no watch or
 * FIFO side effect: ppu_recomp.h vm_write32_inl), which this reproduces. */
int nat_gcmtex(ppu_context* c, uint64_t* bytes)
{
    if ((c->gpr[3] >> 32) || (c->gpr[5] >> 32)) return 0;
    const uint32_t cp = (uint32_t)c->gpr[3] + 8u, s = (uint32_t)c->gpr[5], u = (uint32_t)c->gpr[4];
    if (!span_ok(cp, 4) || !span_ok(s, 32)) return 0;
    const uint32_t p = hn_ld32(cp);
    if (!span_ok(p, 0x28) || !coh_ok(p, 0x28) || !coh_ok(cp, 4) ||
        overlap(p, 0x28, s, 32) || overlap(p, 0x28, cp, 4)) return 0;
    hn_st32(p, ((u << 5) + 0x1A00u) | 0x001C0000u);
    memcpy(vm_base + p + 4, vm_base + s, 28);
    hn_st32(p + 0x20, ((u << 2) + 0x1840u) | 0x00040000u);
    memcpy(vm_base + p + 0x24, vm_base + s + 0x1C, 4);
    std::atomic_signal_fence(std::memory_order_seq_cst);     /* command words before the pointer */
    hn_st32(cp, p + 0x28);
    *bytes = 0x28;
    return 1;
}

/* func_00AD74F4: gcm TRANSFORM_CONSTANT_LOAD inline (one 4x4 matrix).
 * p[0] = 0x00441EFC, p[1] = (u32)r4, p[2..17] = src[0..15], *(r3+8) = p + 0x48.
 * Also reproduces the r19..r29 saves the lifted code makes below sp (dead, but the
 * difftest compares all memory). */
int nat_gcmvpc(ppu_context* c, uint64_t* bytes)
{
    const uint64_t sp = c->gpr[1];
    if ((c->gpr[3] >> 32) || (c->gpr[5] >> 32) || (sp >> 32)) return 0;
    const uint32_t cp = (uint32_t)c->gpr[3] + 8u, s = (uint32_t)c->gpr[5], st = (uint32_t)sp - 0x68u;
    if (!span_ok(cp, 4) || !span_ok(s, 64) || !span_ok(st, 0x58) || !coh_ok(st, 0x58)) return 0;
    const uint32_t p = hn_ld32(cp);
    if (!span_ok(p, 0x48) || !coh_ok(p, 0x48) || !coh_ok(cp, 4) ||
        overlap(p, 0x48, s, 64) || overlap(p, 0x48, cp, 4) || overlap(s, 64, st, 0x58) ||
        overlap(p, 0x48, st, 0x58) || overlap(cp, 4, st, 0x58)) return 0;
    for (int k = 0; k < 11; k++) hn_st64(st + 8u * k, c->gpr[19 + k]);
    hn_st32(p, 0x00441EFCu);
    hn_st32(p + 4, (uint32_t)c->gpr[4]);
    memcpy(vm_base + p + 8, vm_base + s, 64);
    std::atomic_signal_fence(std::memory_order_seq_cst);     /* command words before the pointer */
    hn_st32(cp, p + 0x48);
    *bytes = 0x48;
    return 1;
}

/* ---- switches and stats ---- */
int env_int(const char* n, int def)
{
    const char* e = getenv(n);
    if (!e || !*e) return def;
    if (!_stricmp(e, "off") || !_stricmp(e, "false") || !_stricmp(e, "no")) return 0;
    if (!_stricmp(e, "on") || !_stricmp(e, "true") || !_stricmp(e, "yes")) return 1;
    return atoi(e);
}

std::atomic<int> g_init{0};
std::atomic<unsigned> g_on{0};   /* bit per routine; a sampled mismatch clears it */
unsigned g_sample = 4096;
int g_log = 1;

struct Stat {
    std::atomic<long long> calls{0}, native{0}, bytes{0}, samples{0}, cyc_orig{0}, cyc_nat{0}, mismatch{0}, races{0};
};
Stat g_st[H_N];
std::atomic<unsigned long long> g_next_log{0};
long long g_qpc0, g_qpf; unsigned long long g_tsc0;

struct Tls {
    unsigned calls[H_N], native[H_N], samp[H_N];
    unsigned long long bytes[H_N];
    unsigned tick;
    int bypass;
};
thread_local Tls t_tls;

void init_once()
{
    int z = 0;
    if (!g_init.compare_exchange_strong(z, 1)) { while (g_init.load() != 2) _mm_pause(); return; }
    unsigned on = 0;
    if (port_native_parse_bool(getenv("R2_HOT_NATIVE"), 1))
        for (int i = 0; i < H_N; i++) if (env_int(k_env[i], k_def[i])) on |= 1u << i;
    const int s = env_int("R2_HOT_SAMPLE", 4096);
    g_sample = s <= 0 ? 0 : (unsigned)s;
    g_log = env_int("R2_HOT_LOG", 1);
    LARGE_INTEGER f, q; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&q);
    g_qpf = f.QuadPart; g_qpc0 = q.QuadPart; g_tsc0 = __rdtsc();
    g_next_log.store(GetTickCount64() + 10000);
    g_on = on;
    fprintf(stderr, "[hot] native routines:");
    for (int i = 0; i < H_N; i++) fprintf(stderr, " %s=%u", k_name[i], (on >> i) & 1u);
    fprintf(stderr, " (R2_HOT_NATIVE=0: all lifted) sample 1/%u\n", g_sample);
    g_init.store(2, std::memory_order_release);
}

inline bool enabled(int h)
{
    if (__builtin_expect(g_init.load(std::memory_order_acquire) != 2, 0)) init_once();
    return (g_on.load(std::memory_order_relaxed) >> h) & 1u;
}

void report()
{
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    const double secs = (double)(q.QuadPart - g_qpc0) / (double)g_qpf;
    const double tsc_per_ms = secs > 0 ? (double)(__rdtsc() - g_tsc0) / (secs * 1000.0) : 3.0e6;
    static long long l_calls[H_N], l_nat[H_N], l_bytes[H_N];
    double saved_total = 0;
    for (int h = 0; h < H_N; h++) {
        Stat& s = g_st[h];
        const long long c = s.calls.load(), n = s.native.load(), b = s.bytes.load();
        const long long dc = c - l_calls[h], dn = n - l_nat[h], db = b - l_bytes[h];
        l_calls[h] = c; l_nat[h] = n; l_bytes[h] = b;
        if (!dc) continue;
        const long long ns = s.samples.load();
        const double co = ns ? (double)s.cyc_orig.load() / ns : 0, cn = ns ? (double)s.cyc_nat.load() / ns : 0;
        const double saved_ms = ns ? (double)dn * (co - cn) / tsc_per_ms : 0;
        saved_total += saved_ms;
        fprintf(stderr, "[hot] %-7s calls=%lld native=%.1f%% KB=%lld | cyc/call lifted=%.0f native=%.0f (n=%lld) "
                        "saved~%.1f ms/10s mismatch=%lld race=%lld\n",
                k_name[h], dc, 100.0 * (double)dn / (double)dc, db >> 10, co, cn, ns, saved_ms, s.mismatch.load(), s.races.load());
    }
    if (saved_total > 0) fprintf(stderr, "[hot] total saved ~%.1f ms per 10 s across all PPU threads\n", saved_total);
}

void flush(Tls& t)
{
    for (int h = 0; h < H_N; h++) {
        if (!t.calls[h]) continue;
        g_st[h].calls.fetch_add(t.calls[h], std::memory_order_relaxed);
        g_st[h].native.fetch_add(t.native[h], std::memory_order_relaxed);
        g_st[h].bytes.fetch_add((long long)t.bytes[h], std::memory_order_relaxed);
        t.calls[h] = t.native[h] = 0; t.bytes[h] = 0;
    }
    if (!g_log) return;
    const unsigned long long now = GetTickCount64();
    unsigned long long nx = g_next_log.load(std::memory_order_relaxed);
    if (now >= nx && g_next_log.compare_exchange_strong(nx, now + 10000)) report();
}

typedef int (*nat_fn)(ppu_context*, uint64_t*);

/* A sampled call whose lifted (r3 l1) and native (r3 n1) results differ:
 * run both once more on the same inputs. 2 = confirmed (each side repeats
 * its own result and they still differ: a real bug, disable), 1 = the inputs
 * changed between runs (a concurrent writer, e.g. SPU job counters): ignore. */
int sample_verdict(const ppu_context* in, uint64_t l1, uint64_t n1, nat_fn nat, void (*lifted)(ppu_context*))
{
    Tls& t = t_tls;
    ppu_context l2 = *in, n3 = *in;
    uint64_t b = 0;
    const int saved = t.bypass;
    t.bypass = 1; lifted(&l2); t.bypass = saved;
    if (!nat(&n3, &b)) return 1;
    return (l2.gpr[3] == l1 && n3.gpr[3] == n1 && n1 != l1) ? 2 : 1;
}

int run(int h, ppu_context* c, nat_fn nat, void (*lifted)(ppu_context*))
{
    Tls& t = t_tls;
    if (t.bypass) return 0;
    t.calls[h]++;
    if ((++t.tick & 1023u) == 0) flush(t);
    uint64_t bytes = 0;
    if (g_sample && !((kNoSample >> h) & 1u) && ++t.samp[h] >= g_sample) {
        t.samp[h] = 0;
        ppu_context in = *c;
        ppu_context tmp = in;
        if (!nat(&tmp, &bytes)) return 0;          /* outside the domain: the lifted body runs */
        /* time the lifted body on the real ctx, then the native one again on
         * a copy of the inputs (memcpy/memset/strcpy do not overlap here, so
         * redoing them is a no-op) */
        const unsigned long long t0 = __rdtsc();
        t.bypass = 1; lifted(c); t.bypass = 0;
        const unsigned long long t1 = __rdtsc();
        ppu_context n2 = in;
        nat(&n2, &bytes);
        const unsigned long long t2 = __rdtsc();
        Stat& s = g_st[h];
        s.samples.fetch_add(1, std::memory_order_relaxed);
        s.cyc_orig.fetch_add((long long)(t1 - t0), std::memory_order_relaxed);
        s.cyc_nat.fetch_add((long long)(t2 - t1), std::memory_order_relaxed);
        if (n2.gpr[3] != c->gpr[3]) {
            /* min6 / u16min read u16 job-progress counters that SPU threads
             * write concurrently, so the two runs can legitimately see
             * different inputs: confirm before acting */
            const int v = sample_verdict(&in, c->gpr[3], n2.gpr[3], nat, lifted);
            if (v == 1) {
                s.races.fetch_add(1, std::memory_order_relaxed);
            } else {
                const long long k = s.mismatch.fetch_add(1, std::memory_order_relaxed);
                if (k < 8)
                    fprintf(stderr, "[hot] MISMATCH %s r3 lifted=0x%llX native=0x%llX (in r3=0x%llX r4=0x%llX r5=0x%llX)\n",
                            k_name[h], (unsigned long long)c->gpr[3], (unsigned long long)n2.gpr[3],
                            (unsigned long long)in.gpr[3], (unsigned long long)in.gpr[4], (unsigned long long)in.gpr[5]);
                if ((g_on.fetch_and(~(1u << h)) >> h) & 1u)
                    fprintf(stderr, "[hot] %s DISABLED after a confirmed mismatch: the lifted code runs from now on\n", k_name[h]);
            }
        }
        t.native[h]++; t.bytes[h] += bytes;
        return 1;                                   /* the lifted body already ran */
    }
    if (!nat(c, &bytes)) return 0;
    t.native[h]++; t.bytes[h] += bytes;
    return 1;
}

}  // namespace

#define R2_HOT_ENTRY(fn, h, nat, lifted) \
    extern "C" int fn(ppu_context* ctx) { return enabled(h) ? run(h, ctx, nat, lifted) : 0; }
R2_HOT_ENTRY(r2_hot_memcpy,  H_MEMCPY,  nat_memcpy,  func_00A56E30)
R2_HOT_ENTRY(r2_hot_memset,  H_MEMSET,  nat_memset,  func_00A63258)
R2_HOT_ENTRY(r2_hot_strcmp,  H_STRCMP,  nat_strcmp,  func_00A59DB0)
R2_HOT_ENTRY(r2_hot_strlen,  H_STRLEN,  nat_strlen,  func_00A5A310)
R2_HOT_ENTRY(r2_hot_strcpy,  H_STRCPY,  nat_strcpy,  func_00A5A180)
R2_HOT_ENTRY(r2_hot_memcmp,  H_MEMCMP,  nat_memcmp,  func_00A63330)
R2_HOT_ENTRY(r2_hot_gstrcmp, H_GSTRCMP, nat_gstrcmp, func_006BB908)
R2_HOT_ENTRY(r2_hot_bitscan, H_BITSCAN, nat_bitscan, func_00AF2700)
R2_HOT_ENTRY(r2_hot_min6,    H_MIN6,    nat_min6,    func_0079F394)
R2_HOT_ENTRY(r2_hot_u16min,  H_U16MIN,  nat_u16min,  func_00B15354)
R2_HOT_ENTRY(r2_hot_gcmtex,  H_GCMTEX,  nat_gcmtex,  func_00A92CDC)
R2_HOT_ENTRY(r2_hot_gcmvpc,  H_GCMVPC,  nat_gcmvpc,  func_00AD74F4)

/* difftest entry: the native routine alone (no switches, stats or sampling) */
extern "C" int r2_hot_native_only(int h, ppu_context* ctx)
{
    static const nat_fn k[H_N] = { nat_memcpy, nat_memset, nat_strcmp, nat_strlen, nat_strcpy, nat_memcmp, nat_gstrcmp,
                                   nat_bitscan, nat_min6, nat_u16min, nat_gcmtex, nat_gcmvpc };
    uint64_t b = 0;
    return h >= 0 && h < H_N ? k[h](ctx, &b) : 0;
}

/* difftest entry: the sampled-mismatch confirmation (0 1 2 as sample_verdict) */
extern "C" int r2_hot_sample_verdict(const ppu_context* in, uint64_t l1, uint64_t n1,
                                     int (*nat)(ppu_context*, uint64_t*), void (*lifted)(ppu_context*))
{
    return sample_verdict(in, l1, n1, nat, lifted);
}


/* ---- occlusion-query report wait (func_006483A8) ----
 * At the start of frame N the main thread reads the visibility results of the
 * occlusion queries it issued in frame N-1: it polls the 16-byte RSX report of
 * the LAST query in the set until its timestamp is non-zero (usleep(30) per
 * poll, 1 s timeout), then turns every report into a visible flag. On timeout
 * it keeps the previous flags and does not mark the set as read -- the
 * function's own "no result" path. In the port rsx::thread finishes frame N-1
 * only ~20 ms after the PPU kicked it in heavy scenes (5k draws at ~4 us), so
 * the main thread sleeps here and PPU and RSX run one after the other.
 *   R2_OCC_BLOCKING=1        DEFAULT (user test 2026-11-12): the game's own lifted blocking wait, exactly
 *                            as 0.2.3 (R2_OCC_SKIP=0 R2_OCC_WAIT_US=-1; no native code, no marks). The
 *                            non-blocking skip below with relaxed ZCULL (the R2_ZCULL default) made objects
 *                            pop on/off (logs\run_pop0.log); see scratch\cpu_occ.md, "Popping root cause".
 *                            R2_OCC_SKIP / R2_OCC_WAIT_US only apply with R2_OCC_BLOCKING=0 (WAIT_US also =2).
 *                  =0        the non-blocking skip (the previous default):
 *                  =2        blocking, but the native wait capped at R2_OCC_WAIT_US (R2_OCC_SKIP=0).
 *   R2_OCC_SKIP=<n>          when the result is not in yet, do not wait: write the
 *                            set's flags at once (R2_OCC_SKIP_MODE), at most n frames
 *                            in a row per set (the n+1-th waits as before): PPU frame
 *                            N+1 then overlaps RSX frame N. Default 1 with R2_OCC_BLOCKING=0 (airfield fight
 *                            35.7 -> 45.5 fps); 0 = the wait every frame. The old skip
 *                            (keep last frame's flags) made nearby objects pop in
 *                            and out; skipped/timed-out sets now never use stale or
 *                            hidden flags: a query that is not in yet only ever draws
 *                            more (as RPCS3's conditional render, which draws until a
 *                            result says otherwise).
 *   R2_OCC_SYNC_SITES=<hex,...>  caller return addresses whose sets are never skipped (they
 *                            wait as with R2_OCC_SKIP=0). There are two callers:
 *                            0x001723C8 (func_001721C8: per-object sets, the 21 ms wait) and
 *                            0x0026C1E0 (one global set, b=0, read in the frame-render setup;
 *                            the candidate for screen effects such as flares/glows). Default
 *                            none. The [occ] line counts skips/waits per caller.
 *   R2_OCC_WAIT_US=<us>      wait natively at most <us>, then the timeout path
 *                            Default 50000 (50 ms): well above the normal ~20 ms
 *                            RSX lag, so it only bounds the rare 1 s stalls of the
 *                            lifted wait. A few ms would time out every heavy
 *                            frame (every pending query drawn). -1 = the lifted
 *                            wait (1 s timeout). On timeout the set's flags are
 *                            written like the lifted result loop, except that
 *                            reports the RSX has not written yet count as VISIBLE
 *                            (never last frame's / hidden flags): the whole set is
 *                            drawn, at worst one frame of extra drawing, no pop-out.
 *                            Only if the set's layout
 *                            fails validation does it fall back to the lifted
 *                            timeout path (stale flags; counted as "stale").
 *                            R2_OCC_TIMEOUT_VISIBLE=0: the old timeout path (stale flags).
 *   R2_OCC_SKIP_MODE         an R2_OCC_SKIP-skipped or timed-out set is always written
 *                            all-visible. "partial" (real results for reports already
 *                            written) is refused with a log line: the game zeroes only
 *                            the last report of a set each frame, so an earlier slot's
 *                            timestamp cannot prove the report arrived. Never stale/
 *                            hidden: if the flags cannot be written the set is not
 *                            skipped (waits as usual).
 *   R2_OCC_FENCE_FLIPS=<n>   after a skip or a visible timeout the object is "unproven": reports issued before
 *                            then can still land late in slots later frames reuse (the game zeroes only the
 *                            last one, indices restart every frame), so a non-zero last-report timestamp proves
 *                            nothing. Its sets keep being written all-visible -- also when the report "arrives"
 *                            -- until rsx::thread has executed n more flip commands (r2rsx_flips_executed;
 *                            2n guest frames without the RPCS3 renderer), n guest frames passed, and a wait
 *                            completes normally again; the clear takes effect from the next guest frame (other sets of the object read later this frame stay visible). Default 3. R2_OCC_BLOCKING=1
 *                            (or R2_OCC_WAIT_US=-1 with R2_OCC_SKIP=0), or R2_OCC_TIMEOUT_VISIBLE=0 with R2_OCC_SKIP=0, never mark
 *                            (old behaviour exactly).
 * [occ] lines (every ~10 s): sets read, ready at once, skipped, waited, timeouts.
 *   RSX_ZCULL_GUEST_WAIT=1   DEFAULT: when a set is late (the game's wait is about to block), name its last report to
 *                            rsx::thread (r2rsx_occ_guest_wait), which submits the command buffer holding that report's
 *                            query at once instead of waiting for RSX_ZCULL_CRIT_FLUSH_US / a natural submit. Report
 *                            values are untouched (exact). Under R2_OCC_BLOCKING=1 the hook now counts ready/late sets
 *                            (then the lifted body). [occ] gw line: named, found claimed / claimed while waited /
 *                            written unseen, early submits, latencies. 0 = off (blocking mode: hook returns at once). */
extern "C" uint64_t r2rsx_flips_executed(void);   /* rpcs3_bridge: UINT64_MAX when the RPCS3 renderer is not up */
extern "C" void r2rsx_occ_guest_wait(uint32_t ea);            /* rpcs3_bridge: RSX_ZCULL_GUEST_WAIT */
extern "C" int r2rsx_occ_guest_wait_stats(uint64_t out[11]);  /* rpcs3_bridge: 0 when off */
extern "C" int r2rsx_occ_spec_arm(uint32_t ea);               /* rpcs3_bridge: RSX_OCC_SPEC_VISIBLE report fence */
extern "C" uint32_t r2rsx_occ_spec_pending(void);
namespace {
int g_occ_init = 0;
int g_occ_gw = 1;     /* RSX_ZCULL_GUEST_WAIT */
long long g_occ_us = -1;
int g_occ_skip = 0;
int g_occ_block = 0;  /* R2_OCC_BLOCKING */
int g_occ_tvis = 1;   /* R2_OCC_TIMEOUT_VISIBLE */
uint32_t g_occ_sync_lr[8]; int g_occ_nsync = 0;   /* R2_OCC_SYNC_SITES */
/* per caller: 0 = 0x001723C8 (per-object sets), 1 = 0x0026C1E0 (global set), 2 = other */
std::atomic<long long> g_occ_site_skip[3], g_occ_site_wait[3];
std::atomic<long long> g_occ_calls{0}, g_occ_ready{0}, g_occ_skipped{0}, g_occ_waited{0}, g_occ_timeouts{0}, g_occ_wait_ns{0},
    g_occ_pend{0} /* queries forced visible (report not in yet) */, g_occ_stale{0} /* timeouts left stale (bad layout) */;
std::atomic<unsigned long long> g_occ_next{0};
struct OccSet { uint32_t obj; int32_t b; int run; };
OccSet g_occ_sets[16];   /* main thread only */
/* Unproven objects: a set of the object was skipped/timed out (written visible). Its reports from before that point
   may still land late in slots that later frames reuse (indices restart every frame, all four sets of an object share
   them), so a later non-zero last-report timestamp does not prove that frame's result. Main thread only. */
struct OccMark { uint32_t obj; uint32_t frame; uint64_t flip; int clearing; uint32_t clear_frame; };
OccMark g_occ_marks[16];
int g_occ_mark_all = 0;   /* table full: every object counts as unproven until g_occ_all's fence passes */
OccMark g_occ_all;
int g_occ_fence = 3;      /* R2_OCC_FENCE_FLIPS */
std::atomic<long long> g_occ_unproven{0}, g_occ_marked{0}, g_occ_cleared{0};
int g_sp_on = 0, g_sp_need = 2;   /* RSX_OCC_SPEC_VISIBLE, RSX_OCC_SPEC_NEED (see sp_update_mark) */
int g_sp_mark = 0;                /* global unproven period: every set read is written visible */
uint32_t g_sp_mark_frame = 0, g_sp_settle_frame = 0;
int g_sp_settling = 0;
std::atomic<long long> g_sp_avoided{0}, g_sp_forced{0}, g_sp_refused{0}, g_sp_mwait{0}, g_sp_episodes{0}, g_sp_mframes{0};

inline uint32_t ld_be32(uint32_t ea) { uint32_t v; memcpy(&v, vm_base + ea, 4); return _byteswap_ulong(v); }
inline uint16_t ld_be16(uint32_t ea) { uint16_t v; memcpy(&v, vm_base + ea, 2); return _byteswap_ushort(v); }

}  // namespace

/* For a set whose reports are not all in: every flag of the set 1 (visible), then u16[obj+6] |= 1 << b (set read),
 * as the lifted result loop of func_006483A8 (ppu_recomp_003.cpp loc_006484CC..loc_0064850C) would with all reports
 * visible. No report's count is used: the game's per-frame reset (func_006489AC, loop loc_006489FC) zeroes only the
 * LAST report of each set and report indices restart every frame, so a non-zero timestamp in an earlier slot can be
 * last frame's result of another object -- reading it could hide an object. Only the last report proves arrival
 * (RSX writes in order), and then the lifted body runs. All addresses are validated first; returns 0 with nothing
 * written if any is unexpected. */
extern "C" int r2_occ_write_visible(uint32_t obj, int32_t b, uint32_t count, uint32_t list, long long* pend_out)
{
    if (b < 0 || b > 31 || !span_ok(obj, 0x18) || !span_ok(list, 2 * (uint64_t)count)) return 0;
    const uint32_t flags = ld_be32(obj + 0);
    for (uint32_t i = 0; i < count; i++)
        if (!span_ok(flags + ld_be16(list + 2 * i), 1)) return 0;
    for (uint32_t i = 0; i < count; i++) vm_write8(flags + ld_be16(list + 2 * i), 1);
    vm_write16(obj + 6, (uint16_t)(ld_be16(obj + 6) | (1u << b)));
    if (pend_out) *pend_out = count;
    return 1;
}

namespace {
void occ_gw_report()   /* RSX_ZCULL_GUEST_WAIT stats since the last [occ] line */
{
    uint64_t s[11];
    static uint64_t l[11];
    if (!g_occ_gw || !r2rsx_occ_guest_wait_stats(s)) return;
    uint64_t d[11];
    for (int i = 0; i < 11; i++) d[i] = s[i] - l[i];
    d[9] = s[9];
    auto mean = [](uint64_t sum, uint64_t n) { return n ? (double)sum / 1000.0 / (double)n : 0.0; };
    const uint64_t named = d[2] + d[3];
    fprintf(stderr, "[occ] gw 10 s: late sets named %llu (raced %llu); found claimed %llu (late->written %.2f ms), claimed while "
                    "waited %llu (%.2f ms), written unseen %llu (%.2f ms); early submits %llu; max %.2f ms; claim->written "
                    "%.2f ms (RSX_ZCULL_GUEST_WAIT=0 off)\n",
            (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[2], mean(d[6], d[2]),
            (unsigned long long)d[3], mean(d[7], d[3]), (unsigned long long)d[4], mean(d[8], d[4]), (unsigned long long)d[5],
            (double)d[9] / 1000.0, mean(d[10], named));
    for (int i = 0; i < 11; i++) l[i] = s[i];
}

void occ_report()
{
    static long long lc, lr, ls, lw, lt, lns, lp, lst, lu, lm, lcl;
    const long long c = g_occ_calls.load(), r = g_occ_ready.load(), s = g_occ_skipped.load(), w = g_occ_waited.load(),
                    t = g_occ_timeouts.load(), ns = g_occ_wait_ns.load(), p = g_occ_pend.load(), sl = g_occ_stale.load(),
                    u = g_occ_unproven.load(), mk = g_occ_marked.load(), cl = g_occ_cleared.load();
    fprintf(stderr, "[occ] report sets %lld: ready %lld, skipped %lld (R2_OCC_SKIP=%d, visible), waited %lld, %lld queries forced visible",
            c - lc, r - lr, s - ls, g_occ_skip, w - lw, p - lp);
    if (g_occ_us >= 0)
        fprintf(stderr, " (%.2f ms/s, %.0f us each, timeouts %lld [stale %lld], R2_OCC_WAIT_US=%lld)", (double)(ns - lns) / 1e7,
                w - lw ? (double)(ns - lns) / 1e3 / (double)(w - lw) : 0.0, t - lt, sl - lst, g_occ_us);
    if (mk - lm || u - lu || cl - lcl)
        fprintf(stderr, ", unproven: marked %lld, arrived-but-visible %lld, cleared %lld (R2_OCC_FENCE_FLIPS=%d)", mk - lm,
                u - lu, cl - lcl, g_occ_fence);
    if (g_occ_skip) {
        static long long lss[3], lsw[3];
        long long ss[3], sw[3];
        for (int i = 0; i < 3; i++) { ss[i] = g_occ_site_skip[i].load(); sw[i] = g_occ_site_wait[i].load(); }
        fprintf(stderr, ", skip/wait by caller: 0x001723C8 %lld/%lld, 0x0026C1E0 %lld/%lld, other %lld/%lld%s", ss[0] - lss[0],
                sw[0] - lsw[0], ss[1] - lss[1], sw[1] - lsw[1], ss[2] - lss[2], sw[2] - lsw[2],
                g_occ_nsync ? " (R2_OCC_SYNC_SITES set)" : "");
        for (int i = 0; i < 3; i++) { lss[i] = ss[i]; lsw[i] = sw[i]; }
    }
    fprintf(stderr, "\n");
    lc = c; lr = r; ls = s; lw = w; lt = t; lns = ns; lp = p; lst = sl; lu = u; lm = mk; lcl = cl;
    if (g_sp_on) {
        static long long la, lf, lrf, lmw, le, lmf;
        const long long a = g_sp_avoided.load(), fo = g_sp_forced.load(), rf = g_sp_refused.load(), mw = g_sp_mwait.load(),
                        ep = g_sp_episodes.load(), mf = g_sp_mframes.load();
        fprintf(stderr, "[occ] spec-visible 10 s: waits avoided %lld in %lld periods, refused (fence slots full) %lld; "
                        "unproven frames %lld: sets forced visible %lld, native waits %lld (RSX_OCC_SPEC_VISIBLE=1, need %d)\n",
                a - la, ep - le, rf - lrf, mf - lmf, fo - lf, mw - lmw, g_sp_need);
        la = a; lf = fo; lrf = rf; lmw = mw; le = ep; lmf = mf;
    }
    occ_gw_report();
}

HANDLE occ_timer()   /* main thread only */
{
    static HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                             TIMER_ALL_ACCESS);
    return h;
}

OccSet* occ_set(uint32_t obj, int32_t b)
{
    OccSet* lru = &g_occ_sets[0];
    for (OccSet& e : g_occ_sets) {
        if (e.obj == obj && e.b == b) return &e;
        if (!e.obj) { e.obj = obj; e.b = b; e.run = 0; return &e; }
    }
    lru->obj = obj; lru->b = b; lru->run = 0;
    return lru;
}

/* Mark obj unproven at guest frame `frame`. The fence is the count of flip commands rsx::thread has executed: every
   report issued before now precedes, in the FIFO, the flip of the frame being built, so once the RSX has executed
   R2_OCC_FENCE_FLIPS more flips (and as many guest frames passed) those reports are processed. Never evicts a mark:
   a full table marks every object. */
void occ_mark(uint32_t obj, uint32_t frame)
{
    const uint64_t f = r2rsx_flips_executed();
    g_occ_marked.fetch_add(1, std::memory_order_relaxed);
    OccMark* free_e = nullptr;
    for (OccMark& m : g_occ_marks) {
        if (m.obj == obj) { m.frame = frame; m.flip = f; m.clearing = 0; return; }
        /* a clear from an earlier frame has taken effect: the slot is free */
        if (!free_e && (!m.obj || (m.clearing && m.clear_frame != frame))) free_e = &m;
    }
    if (free_e) { free_e->obj = obj; free_e->frame = frame; free_e->flip = f; free_e->clearing = 0; return; }
    g_occ_mark_all = 1; g_occ_all.obj = 1; g_occ_all.frame = frame; g_occ_all.flip = f; g_occ_all.clearing = 0;
}

bool occ_fence_passed(const OccMark& m, uint32_t frame)
{
    const uint32_t k = (uint32_t)g_occ_fence, df = frame - m.frame;
    const uint64_t f = r2rsx_flips_executed();
    if (f == UINT64_MAX || m.flip == UINT64_MAX) return df >= 2 * k;   /* no RSX flip count: frames only, doubled */
    return f - m.flip >= k && df >= k;
}

/* A clear takes effect from the next guest frame: the game reads several sets of one object per frame
   (func_001721C8), and a later set's last slot this frame can still hold a late report from before the fence.
   The first call in a later frame frees the slot. */
bool occ_is_marked(uint32_t obj, uint32_t frame)
{
    bool marked = false;
    if (g_occ_mark_all) {
        if (g_occ_all.clearing && g_occ_all.clear_frame != frame) g_occ_mark_all = 0;
        else marked = true;
    }
    for (OccMark& m : g_occ_marks) {
        if (m.obj != obj) continue;
        if (m.clearing && m.clear_frame != frame) m.obj = 0;
        else marked = true;
    }
    return marked;
}

/* Only after a normal completion (the last report arrived): obj's mark (and the all-mark) whose fence passed is
   cleared as of the next guest frame. The caller still writes this frame visible. */
void occ_try_clear(uint32_t obj, uint32_t frame)
{
    for (OccMark& m : g_occ_marks)
        if (m.obj == obj && !m.clearing && occ_fence_passed(m, frame)) {
            m.clearing = 1; m.clear_frame = frame; g_occ_cleared.fetch_add(1, std::memory_order_relaxed);
        }
    /* the all-mark's fence covers every unproven event recorded in it (later ones moved it forward) */
    if (g_occ_mark_all && !g_occ_all.clearing && occ_fence_passed(g_occ_all, frame)) {
        g_occ_all.clearing = 1; g_occ_all.clear_frame = frame; g_occ_cleared.fetch_add(1, std::memory_order_relaxed);
    }
}

/* Native poll of a report timestamp (the lifted wait polls with sys_timer_usleep(30)). Returns 1 on timeout. */
int occ_native_wait(const volatile uint64_t* ts, long long wait_us)
{
    LARGE_INTEGER f, q0, q; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&q0);
    const long long lim = wait_us * f.QuadPart / 1000000;
    int timed_out = 0;
    for (unsigned i = 0;; i++) {
        if (*ts) break;
        QueryPerformanceCounter(&q);
        if (q.QuadPart - q0.QuadPart >= lim) { timed_out = 1; break; }
        if (i < 64) _mm_pause();
        else if (q.QuadPart - q0.QuadPart < f.QuadPart / 5000 || !occ_timer()) SwitchToThread();   /* first 200 us */
        else {   /* then poll every ~30 us like the lifted sys_timer_usleep(30), without burning the core */
            LARGE_INTEGER due; due.QuadPart = -300;
            if (SetWaitableTimer(occ_timer(), &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(occ_timer(), 5);
            else SwitchToThread();
        }
    }
    QueryPerformanceCounter(&q);
    g_occ_wait_ns.fetch_add((q.QuadPart - q0.QuadPart) * 1000000000 / f.QuadPart, std::memory_order_relaxed);
    return timed_out;
}

/* ---- RSX_OCC_SPEC_VISIBLE=1 (default 0; R2_OCC_BLOCKING=1 only) -------------------------------------------------
   A set whose last report is late, and whose flags were all VISIBLE after the last RSX_OCC_SPEC_NEED (default 2) real
   evaluations, is answered at once with r2_occ_write_visible (every flag 1, set read -- never hidden). Its reports are
   still in flight and can land after the game reset and reused slots (scratch\cpu_occ.md "Popping root cause": why the
   game's own wait is the default), so the answer starts a global unproven period:
   - a report-write fence is armed on the skipped report (r2rsx_occ_spec_arm): rsx::thread frees it when it writes that
     report, after every report claimed before it;
   - until the fence passed AND two more guest frames began, every set the game reads is written visible (ready ones
     too) and a late set waits natively (lifted 1 s cap), then visible;
   - new answers only in the guest frame that started the period, none when the fence slots are full.
   "Visible" only costs drawing an object the GPU would have culled. */
struct SpecHist { uint32_t obj; int32_t b; uint32_t rep, count, frame; int real, vis_run; uint64_t used; };
SpecHist g_sp_hist[64];   /* main thread only */
uint64_t g_sp_use = 0;

SpecHist* sp_hist(uint32_t obj, int32_t b)
{
    SpecHist* lru = &g_sp_hist[0];
    for (SpecHist& e : g_sp_hist) {
        if (e.obj == obj && e.b == b) { e.used = ++g_sp_use; return &e; }
        if (e.used < lru->used) lru = &e;
    }
    *lru = SpecHist{ obj, b, 0, 0, 0, 0, 0, ++g_sp_use };
    return lru;
}

bool sp_flags_all_visible(uint32_t obj, uint32_t count, uint32_t list)
{
    if (!span_ok(obj, 4) || !span_ok(list, 2 * (uint64_t)count)) return false;
    const uint32_t flags = ld_be32(obj + 0);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t fl = flags + ld_be16(list + 2 * i);
        if (!span_ok(fl, 1) || !vm_base[fl]) return false;
    }
    return true;
}

void sp_update_mark(uint32_t frame)
{
    if (!g_sp_mark || frame == g_sp_mark_frame) return;
    if (r2rsx_occ_spec_pending() != 0) { g_sp_settling = 0; return; }
    if (!g_sp_settling) { g_sp_settling = 1; g_sp_settle_frame = frame; return; }
    if (frame - g_sp_settle_frame >= 2) { g_sp_mark = 0; g_sp_settling = 0; }
}
}  // namespace

extern "C" int r2_occ_wait(ppu_context* ctx)
{
    if (__builtin_expect(!g_occ_init, 0)) {
        const char* e = getenv("R2_OCC_WAIT_US");
        g_occ_us = (e && *e) ? atoll(e) : 50000;   /* -1 = the lifted wait */
        g_occ_skip = env_int("R2_OCC_SKIP", 1);
        if (g_occ_skip < 0) g_occ_skip = 0;
        g_occ_block = env_int("R2_OCC_BLOCKING", 1);   /* default: the lifted blocking wait; 0 = the non-blocking skip */
        if (g_occ_block == 1) { g_occ_skip = 0; g_occ_us = -1; }   /* 0.2.3 exactly: the game's own wait, nothing native */
        else if (g_occ_block >= 2) g_occ_skip = 0;                 /* blocking, native wait capped at R2_OCC_WAIT_US */
            g_sp_need = env_int("RSX_OCC_SPEC_NEED", 2);
            if (g_sp_need < 1) g_sp_need = 1;
            g_sp_on = env_int("RSX_OCC_SPEC_VISIBLE", 0) != 0 && g_occ_block == 1;
            if (env_int("RSX_OCC_SPEC_VISIBLE", 0) != 0)
                fprintf(stderr, "[occ] RSX_OCC_SPEC_VISIBLE: %s (late sets visible %d real evaluations in a row are answered visible "
                                "at once, report-write fence + 2 frames all-visible after; RSX_OCC_SPEC_NEED)\n",
                        g_sp_on ? "on" : "off (needs R2_OCC_BLOCKING=1)", g_sp_need);
        if (const char* s = getenv("R2_OCC_SYNC_SITES")) {
            for (char* p = (char*)s; *p && g_occ_nsync < 8;) {
                char* q = p;
                const unsigned long v = strtoul(p, &q, 16);
                if (q == p) { p++; continue; }
                if (v) g_occ_sync_lr[g_occ_nsync++] = (uint32_t)v;
                p = q;
            }
        }
        const char* m = getenv("R2_OCC_SKIP_MODE");
        g_occ_tvis = env_int("R2_OCC_TIMEOUT_VISIBLE", 1) != 0;
        g_occ_fence = env_int("R2_OCC_FENCE_FLIPS", 3);
        if (g_occ_fence < 1) g_occ_fence = 1;
        g_occ_gw = env_int("RSX_ZCULL_GUEST_WAIT", 1) != 0;
        g_occ_init = 1;
        if (m && !_stricmp(m, "partial"))
            fprintf(stderr, "[occ] R2_OCC_SKIP_MODE=partial is not available (a pending report's slot can hold another "
                            "object's result from last frame): using visible\n");
        fprintf(stderr, "[occ] occlusion report wait: skip %d frame(s) in a row, native timeout %lld us (-1 = lifted);"
                        " a set not fully in counts as visible%s\n",
                g_occ_skip, g_occ_us,
                g_occ_block == 1 ? " (R2_OCC_BLOCKING=1, default: the game's own blocking wait, as 0.2.3)"
                : g_occ_block >= 2 ? " (R2_OCC_BLOCKING=2: blocking, native capped wait)"
                : g_occ_skip ? " (R2_OCC_BLOCKING=0: non-blocking skip)" : " (R2_OCC_BLOCKING=0)");
        for (int i = 0; i < g_occ_nsync; i++)
            fprintf(stderr, "[occ] R2_OCC_SYNC_SITES: sets read from caller 0x%08X are never skipped\n", g_occ_sync_lr[i]);
        g_occ_next.store(GetTickCount64() + 10000);
    }
    if (g_occ_us < 0 && !g_occ_skip && !g_occ_gw && !g_sp_on) return 0;
    const bool lifted_only = g_occ_us < 0 && !g_occ_skip;   /* R2_OCC_BLOCKING=1: count + RSX_ZCULL_GUEST_WAIT, then the lifted body */

    /* same address arithmetic as the lifted entry; anything unexpected runs the lifted body */
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    const int32_t b = (int32_t)ctx->gpr[4];
    if (b < 0 || b > 64) return 0;
    const uint32_t g = (uint32_t)(ctx->gpr[2] - 0x6EC4);
    if (!span_ok(g, 4) || !span_ok(obj, 0x80 + 2 * (uint64_t)b)) return 0;
    const uint32_t r30 = ld_be32(g);
    if (!span_ok(r30 - 0x8000, 8)) return 0;
    const uint32_t gs = ld_be32(r30 - 0x8000);
    if (!span_ok(gs + 0x6C, 4)) return 0;
    if ((int32_t)ld_be32(obj + 8) != (int32_t)(ld_be32(gs + 0x6C) - 1)) return 0;   /* not last frame's set: no wait */
    const uint32_t count = ld_be16(obj + 0x78 + 2 * b);
    if (!count) return 0;
    const uint32_t list = ld_be32(obj + 0x68 + 4 * b);
    if (!span_ok(list + 2 * (count - 1), 2)) return 0;
    const uint32_t last = ld_be16(list + 2 * (count - 1));
    const uint32_t pbase = ld_be32(r30 - 0x7FFC);
    if (!span_ok(pbase, 4)) return 0;
    const uint32_t rbase = ld_be32(pbase);
    const uint32_t rep = rbase + (last << 4);
    if (!span_ok(rep, 8)) return 0;

    g_occ_calls.fetch_add(1, std::memory_order_relaxed);
    const unsigned long long now = GetTickCount64();
    unsigned long long nx = g_occ_next.load(std::memory_order_relaxed);
    if (now >= nx && g_occ_next.compare_exchange_strong(nx, now + 10000)) occ_report();

    const volatile uint64_t* ts = (const volatile uint64_t*)(vm_base + rep);
    if (lifted_only) {
        if (g_sp_on) {
            const uint32_t frame = ld_be32(gs + 0x6C);
            SpecHist* h = sp_hist(obj, b);
            if (h->frame != frame) {   /* first read of this set in this frame: score the previous evaluation */
                const bool cont = h->frame == frame - 1 && h->rep == rep && h->count == count;
                h->vis_run = (cont && h->real && sp_flags_all_visible(obj, count, list)) ? h->vis_run + 1 : 0;
                h->frame = frame; h->rep = rep; h->count = count;
            }
            h->real = 0;
            sp_update_mark(frame);
            static uint32_t last_mframe = 0;
            if (g_sp_mark && last_mframe != frame) { last_mframe = frame; g_sp_mframes.fetch_add(1, std::memory_order_relaxed); }
            if (*ts) {
                g_occ_ready.fetch_add(1, std::memory_order_relaxed);
                /* unproven period: the value may be a stale pre-fence write */
                if (g_sp_mark && r2_occ_write_visible(obj, b, count, list, nullptr)) {
                    g_sp_forced.fetch_add(1, std::memory_order_relaxed);
                    return 1;
                }
                h->real = !g_sp_mark;
                return 0;
            }
            g_occ_waited.fetch_add(1, std::memory_order_relaxed);
            r2rsx_occ_guest_wait(rep);
            if (h->vis_run >= g_sp_need && (!g_sp_mark || frame == g_sp_mark_frame)) {
                if (r2rsx_occ_spec_arm(rep)) {
                    if (r2_occ_write_visible(obj, b, count, list, nullptr)) {
                        h->vis_run = 0;
                        if (!g_sp_mark) {
                            g_sp_mark = 1; g_sp_mark_frame = frame; g_sp_settling = 0;
                            g_sp_episodes.fetch_add(1, std::memory_order_relaxed);
                        }
                        g_sp_avoided.fetch_add(1, std::memory_order_relaxed);
                        return 1;
                    }
                    /* layout unexpected (nothing written): the armed fence only delays the settle; fall through */
                } else {
                    g_sp_refused.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (g_sp_mark) {   /* unproven period: wait natively (lifted 1 s cap), then visible */
                g_sp_mwait.fetch_add(1, std::memory_order_relaxed);
                occ_native_wait(ts, 1000000);
                if (r2_occ_write_visible(obj, b, count, list, nullptr)) {
                    g_sp_forced.fetch_add(1, std::memory_order_relaxed);
                    return 1;
                }
                return 0;
            }
            h->real = 1;
            return 0;
        }
        if (*ts) g_occ_ready.fetch_add(1, std::memory_order_relaxed);
        else {
            g_occ_waited.fetch_add(1, std::memory_order_relaxed);   /* late: the game's own wait blocks on the RSX */
            r2rsx_occ_guest_wait(rep);
        }
        return 0;
    }
    OccSet* st = occ_set(obj, b);
    const uint32_t frame = ld_be32(gs + 0x6C);
    const bool marked = occ_is_marked(obj, frame);
    const uint32_t site_lr = (uint32_t)ctx->lr;   /* the caller's return address (set before the call) */
    const int site = site_lr == 0x001723C8u ? 0 : site_lr == 0x0026C1E0u ? 1 : 2;
    bool sync_site = false;
    for (int i = 0; i < g_occ_nsync; i++) sync_site |= g_occ_sync_lr[i] == site_lr;
    /* write all-visible flags for the set (never stale/hidden); 0 = layout unexpected, nothing written */
    auto occ_write = [&]() -> int {
        long long pend = 0;
        if (!r2_occ_write_visible(obj, b, count, list, &pend)) return 0;
        g_occ_pend.fetch_add(pend, std::memory_order_relaxed);
        return 1;
    };
    /* the last report arrived; for an unproven object it may be a late write from before the skip/timeout: visible */
    auto occ_done = [&]() -> int {
        if (!marked) return 0;
        occ_try_clear(obj, frame);
        if (!occ_write()) return 0;
        g_occ_unproven.fetch_add(1, std::memory_order_relaxed);
        return 1;
    };
    if (*ts) { st->run = 0; g_occ_ready.fetch_add(1, std::memory_order_relaxed); return occ_done(); }
    if (!sync_site && st->run < g_occ_skip && occ_write()) {
        occ_mark(obj, frame);
        st->run++; g_occ_skipped.fetch_add(1, std::memory_order_relaxed);
        g_occ_site_skip[site].fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    st->run = 0;
    g_occ_waited.fetch_add(1, std::memory_order_relaxed);
    g_occ_site_wait[site].fetch_add(1, std::memory_order_relaxed);
    if (g_occ_gw) r2rsx_occ_guest_wait(rep);   /* RSX_ZCULL_GUEST_WAIT */
    if (g_occ_us < 0 && !marked) return 0;   /* the lifted wait */

    /* an unproven object under R2_OCC_WAIT_US=-1 waits natively with the lifted 1 s timeout */
    const long long wait_us = g_occ_us < 0 ? 1000000 : g_occ_us;
    const int timed_out = occ_native_wait(ts, wait_us);
    if (!timed_out) return occ_done();   /* ready: the lifted body reads the results without sleeping */
    g_occ_timeouts.fetch_add(1, std::memory_order_relaxed);
    /* timed out: the whole set counts as visible. Only on an unexpected layout the lifted timeout path (no flags
       written: last frame's flags) */
    if ((!g_occ_tvis && !marked) || !occ_write()) g_occ_stale.fetch_add(1, std::memory_order_relaxed);
    else occ_mark(obj, frame);
    return 1;
}
