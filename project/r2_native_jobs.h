/* R2 native SPURS job kernels (native_hot round, see files/native_hot_r2.md).
 *
 * Native host replacements for hot leaf routines of lifted SPU job modules.
 * Each kernel works on a raw LS image (256 KB, big-endian words) plus the
 * 128-entry SPU register file exactly like the lifted function would and is
 * bit-exact with it (float ops reproduce the spu_helpers.h fast path, built
 * with the same flags as the lifted sources). A kernel either completes
 * (returns 1: LS and its output registers updated) or declines (returns 0:
 * NOTHING written) -- then the lifted code runs.
 *
 *   R2NJ_K_DD413_XFORM4   jobmod_dd413431e518_e1970 0x4030: unpack 80-byte
 *                         records of packed 16-bit positions, 4x4 matrix
 *                         transform, clip-code + transpose to 64-byte output
 *   R2NJ_K_EDGE_ZLIB      spuelf0 0x8240: Edge zlib inflate_fast
 *                         (ps3recomp/runtime/spu/native_edge_zlib.c)
 *   R2NJ_K_7DB0_GRAD      jobmod_7db0447de77b_eD748 0x9440: water height
 *                         field -> packed 2x s8 gradient (normal) map
 *   R2NJ_K_7DB0_UPBLEND   jobmod_7db0447de77b_eD748 0xAC48: 2x upsample of
 *                         the coarse water grid + blend with two ring buffers
 *   R2NJ_K_4154_PHASOR    jobmod_41541a94f5a5_e6848 0x81C0: ocean spectrum
 *                         phasor rotation (input of the 0x9F70 FFT)
 *   R2NJ_K_DD413_CULL     jobmod_dd413431e518_e1970 0x42E0: triangle cull
 *                         (clip codes + winding) + index compaction; output
 *                         $r3 = kept index count
 *   R2NJ_K_029D_MEMCPY    jobmod_029d48175a74_e1A6D8 (img 203, the 3 ms/run
 *                         job) 0x1DF90: its memcpy (byte-disjoint ranges)
 *   All seven kernel bits (0..6) are on by default: byte-exact on the offline
 *   fuzz (dd413 0x4030 also on replayed real inputs; bit 6, the 029d memcpy at
 *   0x1DF90, on the real img-203 capture replay) and guarded at run time by
 *   the self-check with confirm-before-disable. dd413 and the water kernels are
 *   AVX2-only; the baseline build clears their bits.
 *
 * Switches (read once; on/off switches take 1/on/true/yes, 0/off/false/no,
 * any case -- port_native_parse_bool, the same rules in all ports):
 *   R2_NATIVE_JOBS=1         native kernels on (DEFAULT). 0 = lifted only.
 *   R2_NATIVE_JOBS_MASK=0x7F kernels to replace (bit k = kernel k). Default =
 *                            the kernels proven byte-exact offline.
 *   R2_NATIVE_JOBS_CHECK=N   runtime self-check: the first 8 calls of each
 *                            kernel and then every Nth (default 4096; 0 = off)
 *                            run lifted on the real state and native on a copy;
 *                            LS + r0/r1/r80..r127 + output registers are
 *                            compared. A mismatch is re-run (lifted and native
 *                            again on copies of the same input); only a repeat-
 *                            able one disables the kernel for the rest of the
 *                            run (logged once; else counted as a race) -- the lifted result
 *                            is kept, so the game never sees the bad output.
 *   R2_NATIVE_JOBS_VERIFY=1  check every call (slow; for test runs).
 *   R2_NATIVE_JOBS_WATER=0   turns the water kernels (bits 2..4) off; =1 adds
 *                            them even if the mask omits them (default: per
 *                            mask = on; [native] line: water-jobs).
 *   R2_NATIVE_JOBS_STATS=1   periodic per-kernel counters on stderr.
 *   R2_NATIVE_JOBS_PROF=1    per-function profiler: times every hooked call
 *                            (native, or the lifted routine when the kernel is
 *                            masked off / declines) with rdtsc
 *                            and how much of it ran while the main thread sat in
 *                            the 0x006B8D3C job wait (its usleep poll was seen
 *                            < 1 ms earlier). Every ~3 s on stderr:
 *                            [native-jobs] prof <name> lifted N x us | native ...
 *                            Self-checks add paired lifted/native timings.
 *   PORT_NATIVE_EDGE_ZLIB=0  forces the zlib kernel off (old switch); =1 adds
 *                            it even if the mask omits it. */
#ifndef R2_NATIVE_JOBS_H
#define R2_NATIVE_JOBS_H

#include "spu_context.h"   /* u128 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    R2NJ_K_DD413_XFORM4 = 0,
    R2NJ_K_EDGE_ZLIB,
    R2NJ_K_7DB0_GRAD,
    R2NJ_K_7DB0_UPBLEND,
    R2NJ_K_4154_PHASOR,
    R2NJ_K_DD413_CULL,       /* dd413 0x42E0 */
    R2NJ_K_029D_MEMCPY,      /* 029d48175a74 (img 203) 0x1DF90 memcpy */
    R2NJ_K_COUNT
};

#define R2NJ_LS_SIZE 0x40000u

int         r2nj_kernel_run(int k, uint8_t* ls, u128* gpr);
const char* r2nj_kernel_name(int k);
uint32_t    r2nj_kernel_entry(int k);
uint64_t    r2nj_kernel_outregs(int k);

/* individual kernels (harness entry points) */
int r2nj_dd413_xform4(uint8_t* ls, u128* gpr);
int r2nj_7db0_grad(uint8_t* ls, u128* gpr);
int r2nj_7db0_upblend(uint8_t* ls, u128* gpr);
int r2nj_4154_phasor(uint8_t* ls, u128* gpr);
int r2nj_dd413_cull(uint8_t* ls, u128* gpr);
int r2nj_029d_memcpy(uint8_t* ls, u128* gpr);

/* Gate called from the hook line at the top of each lifted target function
 * (tools/hooks/spu/r2_jobmod_dd413431e518_e1970.json, r2_spuelf0.json).
 * 1 = handled (the hook does SPU_RET). */
int r2nj_gate(spu_context* ctx, int k, void (*lifted)(spu_context*));

/* Kernels that can run native after the switches (and build) are applied
 * (bit k = kernel k; 0 when R2_NATIVE_JOBS=0). Reads the switches once. */
uint32_t r2nj_effective_mask(void);
/* 1 = kernel k can run native (r2nj_effective_mask bit k); R3: r3nj_kernel_active. */
int      r2nj_kernel_active(int k);

#ifdef __cplusplus
}
#endif
#endif
