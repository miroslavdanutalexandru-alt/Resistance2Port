/* R2 native SPURS job kernels: switches, engage log, runtime self-check with
 * automatic fallback. Kernels: r2_native_jobs_kern.c; switches documented in
 * r2_native_jobs.h. Same verify scheme as r2_native_audio.c (lifted on the
 * real state, native on a copy, compare), but sampled by default and a
 * mismatch turns the kernel off for good. */
#include "spu_context.h"
#include "r2_native_jobs.h"
#include "port_native_summary.h"   /* port_native_parse_bool: switch values, same rules in all ports */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
#ifdef _WIN32
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#ifdef _WIN32
#include <malloc.h>
#define R2NJ_AALLOC(n) _aligned_malloc((n), 64)
#else
static void* R2NJ_AALLOC(size_t n) { void* p = 0; return posix_memalign(&p, 64, n) ? 0 : p; }
#endif

extern SPU_THREAD_LOCAL uint32_t g_spu_grp_entry;

#define R2NJ_DEFAULT_MASK 0x7Fu     /* kernels proven byte-exact offline (bits 0..6; bit 6 = 029d memcpy, replay-gated on the real img-203 capture) */
#define R2NJ_FIRST_CHECKS 8u

static volatile int g_r2nj_en = -1;          /* -1 = not read yet */
static uint32_t g_r2nj_mask = R2NJ_DEFAULT_MASK;
static uint32_t g_r2nj_check = 4096;          /* every Nth call (after the first few) */
static int      g_r2nj_stats = 0;
static volatile uint32_t g_r2nj_dead;         /* bit k: kernel k disabled after a mismatch */

static SPU_THREAD_LOCAL int t_r2nj_guard;    /* lifted reference running: gate off */
static SPU_THREAD_LOCAL int t_r2nj_skip = -1; /* PROF lifted run: its own hook passes once */

static volatile uint32_t s_calls[R2NJ_K_COUNT], s_native[R2NJ_K_COUNT], s_declined[R2NJ_K_COUNT],
                         s_vok[R2NJ_K_COUNT], s_vbad[R2NJ_K_COUNT], s_logged[R2NJ_K_COUNT],
                         s_races[R2NJ_K_COUNT];
static volatile uint32_t s_total;

/* ---- profiler (R2_NATIVE_JOBS_PROF) -------------------------------------- */
static int g_r2nj_prof = 0;
/* [0] lifted, [1] native: timed calls, cycles, cycles while the main thread waited */
static volatile uint64_t p_n[2][R2NJ_K_COUNT], p_cyc[2][R2NJ_K_COUNT], p_crit[2][R2NJ_K_COUNT];
/* self-check pairs: the same input timed both ways */
static volatile uint64_t p_pair_n[R2NJ_K_COUNT], p_pair_l[R2NJ_K_COUNT], p_pair_nat[R2NJ_K_COUNT];
static volatile uint64_t g_main_wait_tsc;      /* last usleep poll of the 0x006B8D3C job wait */
static uint64_t g_tsc_per_ms = 0, g_crit_win = 0;
static uint64_t p_t0_tsc, p_t0_ns, p_next_tsc;
static volatile long p_lock;

static int (*s_prev_watch)(void*, uint32_t*, uint32_t*);
extern int (*volatile g_ps3_usleep_watch)(void* ctx, uint32_t* lo, uint32_t* hi);
extern int r2_jobwait_is_main(void* ppu_ctx);   /* stubs.cpp: lr 0x006B8D3C, usleep(128) */

static uint64_t r2nj_ns(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static int r2nj_wait_watch(void* ctx, uint32_t* lo, uint32_t* hi)
{
    if (r2_jobwait_is_main(ctx)) g_main_wait_tsc = __rdtsc();
    return s_prev_watch ? s_prev_watch(ctx, lo, hi) : 0;
}
static void r2nj_prof_add(int k, int nat, uint64_t t0, uint64_t t1)
{
    const uint64_t c = t1 - t0;
    __atomic_fetch_add(&p_n[nat][k], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&p_cyc[nat][k], c, __ATOMIC_RELAXED);
    const uint64_t w = g_main_wait_tsc;
    if (w && t1 - w < g_crit_win) __atomic_fetch_add(&p_crit[nat][k], c, __ATOMIC_RELAXED);
}
static void r2nj_prof_report(void)
{
    const uint64_t now = __rdtsc();
    if (now < p_next_tsc || __atomic_exchange_n(&p_lock, 1, __ATOMIC_ACQUIRE)) return;
    if (now >= p_next_tsc) {
        const uint64_t ns = r2nj_ns() - p_t0_ns, dt = now - p_t0_tsc;
        if (ns > 100000000ull) g_tsc_per_ms = dt / (ns / 1000000ull);   /* recalibrate */
        const double ms_per_cyc = 1.0 / (double)g_tsc_per_ms, secs = (double)ns * 1e-9;
        for (int k = 0; k < R2NJ_K_COUNT; k++) {
            if (!p_n[0][k] && !p_n[1][k]) continue;
            char pr[96] = "";
            if (p_pair_n[k])
                snprintf(pr, sizeof pr, " | pairs %llu: %.1fx", (unsigned long long)p_pair_n[k],
                         p_pair_nat[k] ? (double)p_pair_l[k] / (double)p_pair_nat[k] : 0.0);
            fprintf(stderr, "[native-jobs] prof %-13s lifted %llu x %.1f us | native %llu x %.1f us%s | "
                            "%.2f ms/s total, %.2f ms/s while main waits\n",
                    r2nj_kernel_name(k),
                    (unsigned long long)p_n[0][k], p_n[0][k] ? 1000.0 * ms_per_cyc * (double)p_cyc[0][k] / (double)p_n[0][k] : 0.0,
                    (unsigned long long)p_n[1][k], p_n[1][k] ? 1000.0 * ms_per_cyc * (double)p_cyc[1][k] / (double)p_n[1][k] : 0.0,
                    pr, ms_per_cyc * (double)(p_cyc[0][k] + p_cyc[1][k]) / secs,
                    ms_per_cyc * (double)(p_crit[0][k] + p_crit[1][k]) / secs);
        }
        p_next_tsc = now + 3000ull * g_tsc_per_ms;
    }
    __atomic_store_n(&p_lock, 0, __ATOMIC_RELEASE);
}
static void r2nj_prof_init(void)
{
    p_t0_ns = r2nj_ns(); p_t0_tsc = __rdtsc();
    {   /* rough tsc rate until the first report recalibrates over seconds */
        uint64_t n1;
        do n1 = r2nj_ns(); while (n1 - p_t0_ns < 2000000ull);
        g_tsc_per_ms = (__rdtsc() - p_t0_tsc) / ((n1 - p_t0_ns) / 1000000ull);
        if (!g_tsc_per_ms) g_tsc_per_ms = 3000000;
    }
    g_crit_win = g_tsc_per_ms;                     /* 1 ms */
    p_next_tsc = p_t0_tsc + 3000ull * g_tsc_per_ms;
    {   /* install once; never chain to ourselves (that would recurse forever) */
        int (*prev)(void*, uint32_t*, uint32_t*) = g_ps3_usleep_watch;
        if (prev != r2nj_wait_watch) {
            s_prev_watch = prev;
            g_ps3_usleep_watch = r2nj_wait_watch;
        }
    }
    fprintf(stderr, "[native-jobs] prof ON (tsc %.0f MHz; main-wait window 1 ms; usleep watch %s)\n",
            (double)g_tsc_per_ms / 1000.0, s_prev_watch ? "chained" : "installed");
}

static uint32_t envu(const char* n, uint32_t def)
{
    const char* e = getenv(n);
    return (e && *e) ? (uint32_t)strtoul(e, 0, 0) : def;
}
/* boolean switch: 1/on/true/yes, 0/off/false/no (port_native_parse_bool); def when unset/unrecognised */
static int envb(const char* n, int def) { return port_native_parse_bool(getenv(n), def); }

static void r2nj_init(void)
{
    const int on = envb("R2_NATIVE_JOBS", 1);
    g_r2nj_mask = envu("R2_NATIVE_JOBS_MASK", R2NJ_DEFAULT_MASK);
    g_r2nj_check = envu("R2_NATIVE_JOBS_CHECK", 4096);
    if (envb("R2_NATIVE_JOBS_VERIFY", 0)) g_r2nj_check = 1;
    g_r2nj_stats = envb("R2_NATIVE_JOBS_STATS", 0);
    {   /* the zlib kernel's original switch: 0 = off, other = on even if the mask omits it */
        const int z = envb("PORT_NATIVE_EDGE_ZLIB", -1);
        if (z == 1) g_r2nj_mask |= (1u << R2NJ_K_EDGE_ZLIB);
        else if (z == 0) g_r2nj_mask &= ~(1u << R2NJ_K_EDGE_ZLIB);
    }
    {   /* R2_NATIVE_JOBS_WATER: 0 = the 7db0 / 4154 water kernels off, 1 = on even if the mask omits them */
        const uint32_t wb = (1u << R2NJ_K_7DB0_GRAD) | (1u << R2NJ_K_7DB0_UPBLEND) | (1u << R2NJ_K_4154_PHASOR);
        const int w = envb("R2_NATIVE_JOBS_WATER", -1);
        if (w == 1) g_r2nj_mask |= wb;
        else if (w == 0) g_r2nj_mask &= ~wb;
    }
    g_r2nj_prof = envb("R2_NATIVE_JOBS_PROF", 0);
#if !defined(__AVX2__)
    /* AVX2-only kernels (baseline build) */
    g_r2nj_mask &= ~((1u << R2NJ_K_DD413_XFORM4) | (1u << R2NJ_K_7DB0_GRAD) | (1u << R2NJ_K_7DB0_UPBLEND) |
                     (1u << R2NJ_K_4154_PHASOR) | (1u << R2NJ_K_DD413_CULL));
#endif
    g_r2nj_mask &= (1u << R2NJ_K_COUNT) - 1u;   /* unknown bits never enable anything */
    if (on && g_r2nj_prof) r2nj_prof_init();
    __atomic_store_n(&g_r2nj_en, (on && (g_r2nj_mask || g_r2nj_prof)) ? 1 : 0, __ATOMIC_RELEASE);
}

/* One-shot init: g_r2nj_en -1 (unread) -> 2 (initialising) -> 0/1. Only the CAS
 * winner reads the switches; concurrent callers see 2 and run lifted. */
static int r2nj_ensure_init(void)
{
    int en = __atomic_load_n(&g_r2nj_en, __ATOMIC_ACQUIRE);
    if (en < 0) {
        int exp = -1;
        if (__atomic_compare_exchange_n(&g_r2nj_en, &exp, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            r2nj_init();
        en = __atomic_load_n(&g_r2nj_en, __ATOMIC_ACQUIRE);
    }
    return en;
}

/* Effective kernel mask (bits of kernels that can run native); for the [native] line. */
uint32_t r2nj_effective_mask(void)
{
    int en;
    while ((en = r2nj_ensure_init()) == 2) { /* another thread is initialising */ }
    return en == 1 ? (g_r2nj_mask & ~g_r2nj_dead) : 0u;
}
int r2nj_kernel_active(int k)
{
    return (unsigned)k < R2NJ_K_COUNT && ((r2nj_effective_mask() >> k) & 1u);
}

static void r2nj_stats_tick(void)
{
    if (!g_r2nj_stats) return;
    const uint32_t t = ++s_total;
    if (t % 20000u) return;
    for (int k = 0; k < R2NJ_K_COUNT; k++)
        if (s_calls[k])
            fprintf(stderr, "[native-jobs] %-14s calls=%u native=%u declined=%u check ok=%u BAD=%u races=%u%s\n",
                    r2nj_kernel_name(k), s_calls[k], s_native[k], s_declined[k], s_vok[k], s_vbad[k], s_races[k],
                    (g_r2nj_dead >> k) & 1u ? " (disabled)" : "");
}

/* ---- self-check ---------------------------------------------------------- */
typedef struct { uint8_t* pre_ls; uint8_t* nat_ls; uint8_t* rep_ls; u128* pre_gpr; u128* nat_gpr; u128* l1_gpr; u128* rep_gpr; } r2nj_buf;
static SPU_THREAD_LOCAL r2nj_buf t_buf;

static int r2nj_buf_ok(void)
{
    if (t_buf.pre_ls) return 1;
    t_buf.pre_ls  = (uint8_t*)R2NJ_AALLOC(R2NJ_LS_SIZE);
    t_buf.nat_ls  = (uint8_t*)R2NJ_AALLOC(R2NJ_LS_SIZE);
    t_buf.rep_ls  = (uint8_t*)R2NJ_AALLOC(R2NJ_LS_SIZE);
    t_buf.pre_gpr = (u128*)R2NJ_AALLOC(128 * sizeof(u128));
    t_buf.nat_gpr = (u128*)R2NJ_AALLOC(128 * sizeof(u128));
    t_buf.l1_gpr  = (u128*)R2NJ_AALLOC(128 * sizeof(u128));
    t_buf.rep_gpr = (u128*)R2NJ_AALLOC(128 * sizeof(u128));
    if (!t_buf.pre_ls || !t_buf.nat_ls || !t_buf.rep_ls || !t_buf.pre_gpr || !t_buf.nat_gpr || !t_buf.l1_gpr ||
        !t_buf.rep_gpr) { t_buf.pre_ls = 0; return 0; }
    return 1;
}

static int r2nj_first_ls_diff(const uint8_t* a, const uint8_t* b)
{
    for (uint32_t q = 0; q < R2NJ_LS_SIZE; q += 4096)
        if (memcmp(a + q, b + q, 4096))
            for (uint32_t i = q; i < q + 4096; i++) if (a[i] != b[i]) return (int)i;
    return -1;
}
/* registers a kernel must reproduce: r0/r1, r80..r127 (non-volatile), outregs */
static int r2nj_reg_diff(int k, const u128* a, const u128* b)
{
    const uint64_t outm = r2nj_kernel_outregs(k);
    for (int r = 0; r < 128; r++) {
        const int must = r < 2 || r >= 80 || (r < 64 && ((outm >> r) & 1));
        if (must && memcmp(&a[r], &b[r], sizeof(u128))) return r;
    }
    return -1;
}

/* the lifted routine as its brsl call site runs it (host frame + drain to
 * the link register), with the gate off */
static void r2nj_run_lifted(spu_context* ctx, void (*lifted)(spu_context*))
{
    const uint32_t ret = ctx->gpr[0]._u32[0];
    const int32_t si = (int32_t)ctx->image_id;
    t_r2nj_guard = 1;
    ctx->host_depth++;
    lifted(ctx);
    spu_drain_call(ctx, ret);
    ctx->host_depth--;
    spu_img_restore(ctx, si);
    t_r2nj_guard = 0;
}

/* A sampled call whose native result differs from the lifted one: run both
 * once more on copies of the same input (pre). 1 = confirmed (each side
 * repeats its own result, so they still differ: a kernel bug), 0 = a side did
 * not repeat (its input changed under it, e.g. a concurrent LS writer: race).
 * The lifted repeat runs on a clone of the context whose ls is rep_ls: the
 * live ctx (its ls pointer, registers, channels) is never touched, so peer
 * LS writers (spu_ls_window_transfer, sys_spu_thread_write_ls) keep landing
 * in the real LS. */
static SPU_THREAD_LOCAL spu_context* t_r2nj_clone;
_Static_assert(offsetof(spu_context, ls_store) < offsetof(spu_context, ls), "clone copy skips ls_store");
static int r2nj_confirm(spu_context* ctx, int k, void (*lifted)(spu_context*), int nrc1)
{
    if (!t_r2nj_clone && !(t_r2nj_clone = (spu_context*)R2NJ_AALLOC(sizeof(spu_context)))) return 1;
    spu_context* const cl = t_r2nj_clone;
    memcpy(cl, ctx, offsetof(spu_context, ls_store));
    memcpy((uint8_t*)cl + offsetof(spu_context, ls), (const uint8_t*)ctx + offsetof(spu_context, ls),
           sizeof(spu_context) - offsetof(spu_context, ls));
    memcpy(t_buf.rep_ls, t_buf.pre_ls, R2NJ_LS_SIZE);
    memcpy(cl->gpr, t_buf.pre_gpr, 128 * sizeof(u128));
    cl->ls = t_buf.rep_ls;
    r2nj_run_lifted(cl, lifted);
    memcpy(t_buf.rep_gpr, cl->gpr, 128 * sizeof(u128));
    const int lif_same = r2nj_first_ls_diff(t_buf.rep_ls, ctx->ls) < 0 && r2nj_reg_diff(k, t_buf.rep_gpr, ctx->gpr) < 0;
    /* native repeat in place on pre_ls (no longer needed); registers on l1_gpr so
     * pre_gpr keeps the input snapshot for the DISABLED log line */
    memcpy(t_buf.l1_gpr, t_buf.pre_gpr, 128 * sizeof(u128));
    const int nrc2 = r2nj_kernel_run(k, t_buf.pre_ls, t_buf.l1_gpr);
    const int nat_same = nrc2 == nrc1 && r2nj_first_ls_diff(t_buf.pre_ls, t_buf.nat_ls) < 0 &&
                         r2nj_reg_diff(k, t_buf.l1_gpr, t_buf.nat_gpr) < 0;
    return lif_same && nat_same;
}

/* returns 0 = could not check (caller runs native normally), 1 = checked:
 * the lifted routine already ran on the real state */
static int r2nj_check(spu_context* ctx, int k, void (*lifted)(spu_context*))
{
    if (!r2nj_buf_ok()) return 0;
    memcpy(t_buf.pre_ls, ctx->ls, R2NJ_LS_SIZE);
    memcpy(t_buf.pre_gpr, ctx->gpr, 128 * sizeof(u128));
    memcpy(t_buf.nat_ls, t_buf.pre_ls, R2NJ_LS_SIZE);
    memcpy(t_buf.nat_gpr, t_buf.pre_gpr, 128 * sizeof(u128));
    const uint64_t t0 = __rdtsc();
    const int nrc = r2nj_kernel_run(k, t_buf.nat_ls, t_buf.nat_gpr);
    const uint64_t t1 = __rdtsc();
    r2nj_run_lifted(ctx, lifted);
    const uint64_t t2 = __rdtsc();
    if (g_r2nj_prof) {
        r2nj_prof_add(k, 0, t1, t2);
        if (nrc) {
            __atomic_fetch_add(&p_pair_n[k], 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&p_pair_nat[k], t1 - t0, __ATOMIC_RELAXED);
            __atomic_fetch_add(&p_pair_l[k], t2 - t1, __ATOMIC_RELAXED);
        }
    }

    int bad = 0, lsd = -1, rd = -1;
    if (!nrc) {
        s_declined[k]++;
        lsd = r2nj_first_ls_diff(t_buf.nat_ls, t_buf.pre_ls);
        if (lsd >= 0 || memcmp(t_buf.nat_gpr, t_buf.pre_gpr, 128 * sizeof(u128))) bad = 2;
    } else {
        lsd = r2nj_first_ls_diff(t_buf.nat_ls, ctx->ls);
        if (lsd >= 0) bad = 1;
        else if ((rd = r2nj_reg_diff(k, t_buf.nat_gpr, ctx->gpr)) >= 0) bad = 1;
    }
    if (bad && !r2nj_confirm(ctx, k, lifted, nrc)) {
        if (!s_races[k]++)
            fprintf(stderr, "[native-jobs] %s self-check differed on call %u but did not repeat (input changed "
                            "during the check: race, not a kernel bug); kernel stays on\n",
                    r2nj_kernel_name(k), s_calls[k]);
        return 1;
    }
    if (bad) {
        s_vbad[k]++;
        g_r2nj_dead |= 1u << k;
        if (!s_logged[k]++)
            fprintf(stderr, "[native-jobs] %s %s on call %u (first LS diff 0x%05X native=%02X lifted=%02X, reg r%d; "
                            "input r3=%08X r4=%08X r5=%08X; repeated): kernel DISABLED, lifted code from now on\n",
                    r2nj_kernel_name(k), bad == 2 ? "declined but wrote" : "MISMATCH", s_calls[k],
                    lsd < 0 ? 0 : lsd, lsd < 0 ? 0 : t_buf.nat_ls[lsd], lsd < 0 ? 0 : ctx->ls[lsd], rd,
                    t_buf.pre_gpr[3]._u32[0], t_buf.pre_gpr[4]._u32[0], t_buf.pre_gpr[5]._u32[0]);
    } else if (nrc) {
        s_vok[k]++;
    }
    return 1;
}

/* PROF: run + time the lifted routine for a call no kernel takes. Its own
 * hook line re-enters the gate first; t_r2nj_skip lets exactly that call
 * through, so kernels nested inside it (dd413 0x42E0 -> 0x4030) still run. */
static int r2nj_prof_lifted(spu_context* ctx, int k, void (*lifted)(spu_context*))
{
    const uint32_t ret = ctx->gpr[0]._u32[0];
    const int32_t si = (int32_t)ctx->image_id;
    const uint64_t t0 = __rdtsc();
    t_r2nj_skip = k;
    ctx->host_depth++;
    lifted(ctx);
    spu_drain_call(ctx, ret);
    ctx->host_depth--;
    spu_img_restore(ctx, si);
    t_r2nj_skip = -1;
    r2nj_prof_add(k, 0, t0, __rdtsc());
    r2nj_prof_report();
    return 1;
}
/* ---- the hook entry ------------------------------------------------------ */
int r2nj_gate(spu_context* ctx, int k, void (*lifted)(spu_context*))
{
    int en = g_r2nj_en;
    if (__builtin_expect(en != 1, 0) && r2nj_ensure_init() != 1) return 0;
    /* a group re-entry (loop back-edge via trampoline) is not a call */
    if (g_spu_grp_entry != 0 || t_r2nj_guard) return 0;
    if (t_r2nj_skip == k) { t_r2nj_skip = -1; return 0; }
    if ((unsigned)k >= R2NJ_K_COUNT) return 0;
    /* a pending SPU interrupt is taken by the lifted loop's first check */
    if (ctx->int_enable && (ctx->event_status & ctx->event_mask)) return 0;
    if (!((g_r2nj_mask >> k) & 1u) || ((g_r2nj_dead >> k) & 1u))
        return g_r2nj_prof ? r2nj_prof_lifted(ctx, k, lifted) : 0;
    const uint32_t n = s_calls[k]++;
    r2nj_stats_tick();
    if (n == 0)
        fprintf(stderr, "[native-jobs] %s (LS 0x%X) native ON (self-check: first %u calls, then 1/%u)\n",
                r2nj_kernel_name(k), r2nj_kernel_entry(k), R2NJ_FIRST_CHECKS, g_r2nj_check);
    if (g_r2nj_check && (n < R2NJ_FIRST_CHECKS || n % g_r2nj_check == 0))
        if (r2nj_check(ctx, k, lifted)) { if (g_r2nj_prof) r2nj_prof_report(); return 1; }
    if (!g_r2nj_prof) {
        const int rc = r2nj_kernel_run(k, ctx->ls, ctx->gpr);
        if (rc) s_native[k]++; else s_declined[k]++;
        return rc;
    }
    const uint64_t t0 = __rdtsc();
    const int rc = r2nj_kernel_run(k, ctx->ls, ctx->gpr);
    if (!rc) { s_declined[k]++; return r2nj_prof_lifted(ctx, k, lifted); }
    s_native[k]++;
    r2nj_prof_add(k, 1, t0, __rdtsc());
    r2nj_prof_report();
    return 1;
}