/* R2 native audio-synth kernels (default on; R2_NATIVE_AUDIO_JOB=0 = lifted only).
 *
 * Native replacements for the leaf routines of the "nextsynth" WWS job
 * modules that run all game audio on one SPURS lane:
 *   jobmod_a8d79dd61f50_e80 (synth A) and jobmod_5e0b41a9ea8c_e80 (synth B).
 * Each kernel works on a raw LS image (256 KB, big-endian words) plus the
 * 128-entry SPU register file, exactly like the lifted function would, and is
 * bit-exact with it (float ops go through the same spu_helpers.h helpers,
 * compiled with the same flags). A kernel either completes (returns 1, LS and
 * its output registers updated) or declines (returns 0, NOTHING written) --
 * then the lifted code runs. See scratch/cpu_audio_native.md. */
#ifndef R2_NATIVE_AUDIO_H
#define R2_NATIVE_AUDIO_H

#include "spu_context.h"   /* u128 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    R2NA_K_A8_RESAMPLE4 = 0,   /* a8 0x4800: 4-tap table-interpolated resampler       */
    R2NA_K_A8_DEINTERLEAVE4,   /* a8 0x4B80: 4-channel deinterleave (word transpose)  */
    R2NA_K_A8_INTERLEAVE4,     /* a8 0x4C80: 4-channel interleave                     */
    R2NA_K_A8_ALLOC_UP,        /* a8 0x6630: arena alloc (grow up) + zero fill        */
    R2NA_K_A8_ALLOC_DOWN,      /* a8 0x6788: arena alloc (grow down) + zero fill      */
    R2NA_K_5E_ALLOC_UP,        /* 5e0b 0xDC08: byte-identical to a8 0x6630            */
    R2NA_K_5E_ALLOC_DOWN,      /* 5e0b 0xDD60: byte-identical to a8 0x6788            */
    R2NA_K_5E_RINGREAD,        /* 5e0b 0x5E08: two-segment ring read (byte copies)    */
    R2NA_K_5E_RINGWRITE,       /* 5e0b 0x6150: two-segment ring write (byte copies)   */
    R2NA_K_COUNT
};

#define R2NA_LS_SIZE   0x40000u
#define R2NA_QMASK     0x3FFF0u

/* 1 = done (LS + output registers updated), 0 = declined (nothing written). */
int         r2na_kernel_run(int k, uint8_t* ls, u128* gpr);
const char* r2na_kernel_name(int k);
uint32_t    r2na_kernel_entry(int k);
/* Registers (bit n = $rn, n < 64) the kernel defines on return and that a
 * caller may read. All other volatile registers ($r2..$r79) are scratch the
 * lifted code leaves behind (no caller reads them: checked at every brsl site
 * in scratch/cpu_audio_native.md); $r0/$r1/$r80..$r127 are never written. */
uint64_t    r2na_kernel_outregs(int k);

/* Gate called from the hook line at the top of each lifted target function
 * (tools/hooks/spu/r2_jobmod_*.json). 1 = handled (the hook does SPU_RET). */
int r2na_gate(spu_context* ctx, int k, void (*lifted)(spu_context*));

#ifdef __cplusplus
}
#endif
#endif
