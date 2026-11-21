/* cpu-replay: SPU job capture for the offline replay benchmark (Q5 of
 * CPU_BOTTLENECK_PLAN.md). See scratch\cpu_replay.md for the design and the
 * capture steps; the offline side is tools\replay\spu\spu_replay.c.
 *
 * Compiled as part of spu_drain.c (#include at its end) so no build-system
 * change is needed and it can see the [spu-jobs] job boundaries directly.
 *
 * Off (PORT_SPU_CAPTURE unset): g_cpurep_spu_on == 0 and g_cpurep_spu_live ==
 * 0; every hook is one predicted-not-taken test of a cached global.
 *
 * On: PORT_SPU_CAPTURE=<dir>
 *   trigger        Ctrl+Shift+F8, or the file <dir>\go appearing, or
 *                  PORT_SPU_CAPTURE_AT_FRAME=<g_port_frame_no>
 *   PORT_SPU_CAPTURE_IMGS=419,623,...   images to capture; unset/"auto" =
 *                  profile PORT_SPU_CAPTURE_PROFILE_FRAMES (30) frames after
 *                  the trigger and take the PORT_SPU_CAPTURE_TOP (6) images
 *                  with the most total job wall time
 *   PORT_SPU_CAPTURE_FRAMES=N           frames to capture (default 3)
 *   PORT_SPU_CAPTURE_PER_FRAME=M        max runs per image per frame (8, 0=all)
 *   PORT_SPU_CAPTURE_MAX_JOBS=J         global cap (default 3000)
 *   PORT_SPU_CAPTURE_PUTS=0             do not store PUT payloads (smaller
 *                  files; the replay then cannot check output against the game)
 * Output: <dir>\spu_t<tid>.crsj per SPU host thread + <dir>\session.txt. */
#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <intrin.h>
#include "spu_cpurep_fmt.h"

extern uint8_t* vm_base;
extern volatile long g_port_frame_no;
extern int g_spu_hop_direct, g_spu_hop_deep, g_spu_inline_ch;

#ifndef CPUREP_GAME
#  define CPUREP_GAME 2
#endif
#define CPUREP_NIMG 1024

int g_cpurep_spu_on;                    /* cached: PORT_SPU_CAPTURE set        */
volatile long g_cpurep_spu_live;        /* threads inside a captured job       */

enum { CR_WAIT = 0, CR_PROFILE = 1, CR_CAPTURE = 2, CR_DONE = 3 };
static volatile long s_cr_phase;
static volatile long s_cr_phase_frame;  /* frame the current phase started at  */
static volatile long s_cr_last_frame = -1;
static char s_cr_dir[MAX_PATH];
static int s_cr_frames = 3, s_cr_per_frame = 8, s_cr_max_jobs = 3000, s_cr_puts = 1;
static int s_cr_prof_frames = 30, s_cr_top = 6, s_cr_auto = 1;
static long s_cr_at_frame = -1;
static unsigned char s_cr_sel[CPUREP_NIMG];
static volatile long long s_cr_prof_wall[CPUREP_NIMG];
static volatile long s_cr_prof_runs[CPUREP_NIMG];
static volatile long s_cr_cap_frame[CPUREP_NIMG], s_cr_cap_n[CPUREP_NIMG], s_cr_cap_total[CPUREP_NIMG];
static volatile long s_cr_seq, s_cr_jobs;
static volatile long long s_cr_bytes;
static long long s_cr_qf;

/* ---- per-thread writer state */
typedef struct { uint8_t* p; size_t n, cap; } cr_buf;
typedef struct {
    FILE* f;
    uint64_t* key; uint32_t* id; uint32_t mask;   /* chunk hash -> id+1    */
    uint8_t* data; uint32_t nchunk, cap;          /* chunk store (verify)  */
} cr_file;
static SPU_THREAD_LOCAL cr_file* t_cr_file;
static SPU_THREAD_LOCAL cr_buf t_cr_job;
static SPU_THREAD_LOCAL spu_context* t_cr_ctx;      /* capturing this job      */
static SPU_THREAD_LOCAL uint32_t t_cr_seq, t_cr_ndma, t_cr_natom;
static SPU_THREAD_LOCAL long long t_cr_q0;
static SPU_THREAD_LOCAL unsigned long long t_cr_c0;
static SPU_THREAD_LOCAL size_t t_cr_win_off;        /* LS-window GET awaiting bytes */
static SPU_THREAD_LOCAL uint32_t t_cr_win_lsa, t_cr_win_size;
static SPU_THREAD_LOCAL int t_cr_pimg;              /* profiling run in flight */
static SPU_THREAD_LOCAL long long t_cr_pq0;

static void cr_put(cr_buf* b, const void* p, size_t n)
{
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : (1u << 20);
        while (c < b->n + n) c *= 2;
        uint8_t* q = (uint8_t*)realloc(b->p, c);
        if (!q) return;
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, p, n); b->n += n;
}
static void cr_rec(cr_buf* b, uint32_t type, const void* h, uint32_t hn, const void* d, uint32_t dn)
{
    const uint32_t r[2] = { type, hn + dn };
    cr_put(b, r, 8); if (hn) cr_put(b, h, hn); if (dn) cr_put(b, d, dn);
}
static uint64_t cr_hash(const uint8_t* p)
{
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (uint32_t i = 0; i < CRSJ_CHUNK; i += 8) {
        uint64_t w; memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull; h ^= h >> 29;
    }
    return h | 1;
}
static int cr_grow(cr_file* cf)
{
    const uint32_t nm = cf->mask ? cf->mask * 2 + 1 : 4095;
    uint64_t* k = (uint64_t*)calloc((size_t)nm + 1, 8);
    uint32_t* d = (uint32_t*)calloc((size_t)nm + 1, 4);
    if (!k || !d) { free(k); free(d); return 0; }
    for (uint32_t i = 0; cf->key && i <= cf->mask; i++) {
        if (!cf->key[i]) continue;
        uint32_t j = (uint32_t)cf->key[i] & nm;
        while (k[j]) j = (j + 1) & nm;
        k[j] = cf->key[i]; d[j] = cf->id[i];
    }
    free(cf->key); free(cf->id); cf->key = k; cf->id = d; cf->mask = nm;
    return 1;
}
/* Chunk id of a 1 KB LS block; a new chunk is appended to the job buffer. */
static uint32_t cr_chunk(cr_file* cf, cr_buf* b, const uint8_t* p)
{
    if ((cf->nchunk + 1) * 2 > cf->mask && !cr_grow(cf)) return 0xFFFFFFFFu;
    const uint64_t h = cr_hash(p);
    uint32_t j = (uint32_t)h & cf->mask;
    for (; cf->key[j]; j = (j + 1) & cf->mask)
        if (cf->key[j] == h && !memcmp(cf->data + (size_t)(cf->id[j] - 1) * CRSJ_CHUNK, p, CRSJ_CHUNK))
            return cf->id[j] - 1;
    if (cf->nchunk >= cf->cap) {
        const uint32_t nc = cf->cap ? cf->cap * 2 : 4096;
        uint8_t* q = (uint8_t*)realloc(cf->data, (size_t)nc * CRSJ_CHUNK);
        if (!q) return 0xFFFFFFFFu;
        cf->data = q; cf->cap = nc;
    }
    const uint32_t id = cf->nchunk++;
    memcpy(cf->data + (size_t)id * CRSJ_CHUNK, p, CRSJ_CHUNK);
    cf->key[j] = h; cf->id[j] = id + 1;
    cr_rec(b, CRSJ_T_CHUNK, &id, 4, p, CRSJ_CHUNK);
    return id;
}
static void cr_ls(cr_file* cf, cr_buf* b, const uint8_t* ls, uint32_t* ids)
{
    for (uint32_t i = 0; i < CRSJ_NCHUNK; i++) ids[i] = cr_chunk(cf, b, ls + (size_t)i * CRSJ_CHUNK);
}
static cr_file* cr_open(void)
{
    if (t_cr_file) return t_cr_file;
    cr_file* cf = (cr_file*)calloc(1, sizeof *cf);
    if (!cf) return NULL;
    char fn[MAX_PATH + 64];
    snprintf(fn, sizeof fn, "%s\\spu_t%lu.crsj", s_cr_dir, (unsigned long)GetCurrentThreadId());
    cf->f = fopen(fn, "wb");
    if (!cf->f) { free(cf); fprintf(stderr, "[cpu-replay] spu: cannot create %s\n", fn); return NULL; }
    setvbuf(cf->f, NULL, _IOFBF, 1 << 22);
    crsj_file_hdr h; memset(&h, 0, sizeof h);
    h.magic = CRSJ_MAGIC; h.version = CRSJ_VERSION; h.game = CPUREP_GAME;
    h.host_tid = GetCurrentThreadId();
    h.hop_direct = g_spu_hop_direct; h.hop_deep = g_spu_hop_deep; h.inline_ch = g_spu_inline_ch;
    h.ctx_size = (uint32_t)sizeof(spu_context); h.qpf = (uint64_t)s_cr_qf;
    fwrite(&h, sizeof h, 1, cf->f);
    t_cr_file = cf;
    return cf;
}

static int cr_list(const char* e, unsigned char* sel)
{
    int n = 0;
    for (const char* p = e; p && *p;) {
        char* end; long v = strtol(p, &end, 0);
        if (end == p) break;
        if (v > 0 && v < CPUREP_NIMG) { sel[v] = 1; n++; }
        if (*end != ',') break;
        p = end + 1;
    }
    return n;
}
static int cr_envi(const char* n, int d) { const char* e = getenv(n); return e && *e ? atoi(e) : d; }

void cpurep_spu_init(void)
{
    const char* d = getenv("PORT_SPU_CAPTURE");
    if (!d || !*d) return;
    snprintf(s_cr_dir, sizeof s_cr_dir, "%s", d);
    CreateDirectoryA(s_cr_dir, NULL);
    { char go[MAX_PATH + 8]; snprintf(go, sizeof go, "%s\\go", s_cr_dir); DeleteFileA(go); } /* stale trigger from an earlier session */
    s_cr_frames = cr_envi("PORT_SPU_CAPTURE_FRAMES", 3);
    s_cr_per_frame = cr_envi("PORT_SPU_CAPTURE_PER_FRAME", 8);
    s_cr_max_jobs = cr_envi("PORT_SPU_CAPTURE_MAX_JOBS", 3000);
    s_cr_puts = cr_envi("PORT_SPU_CAPTURE_PUTS", 1);
    s_cr_prof_frames = cr_envi("PORT_SPU_CAPTURE_PROFILE_FRAMES", 30);
    s_cr_top = cr_envi("PORT_SPU_CAPTURE_TOP", 6);
    s_cr_at_frame = cr_envi("PORT_SPU_CAPTURE_AT_FRAME", -1);
    const char* im = getenv("PORT_SPU_CAPTURE_IMGS");
    s_cr_auto = !(im && *im && _stricmp(im, "auto") && cr_list(im, s_cr_sel) > 0);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_cr_qf = f.QuadPart;
    if (s_cr_frames < 1) s_cr_frames = 1;
    if (s_cr_top < 1) s_cr_top = 1;
    if (s_cr_top > 32) s_cr_top = 32;
    fprintf(stderr, "[cpu-replay] spu capture armed: dir=%s frames=%d per_frame=%d %s; "
            "trigger Ctrl+Shift+F8, file %s\\go%s\n",
            s_cr_dir, s_cr_frames, s_cr_per_frame, s_cr_auto ? "images=auto (top by wall time)" : "images=list",
            s_cr_dir, s_cr_at_frame >= 0 ? " or PORT_SPU_CAPTURE_AT_FRAME" : "");
    g_cpurep_spu_on = 1;
}

static void cr_session_txt(const char* why, long frame)
{
    char fn[MAX_PATH + 32]; snprintf(fn, sizeof fn, "%s\\session.txt", s_cr_dir);
    FILE* f = fopen(fn, "a");
    if (!f) return;
    fprintf(f, "%s frame=%ld game=R%d frames=%d per_frame=%d jobs=%ld bytes=%lld\n",
            why, frame, CPUREP_GAME, s_cr_frames, s_cr_per_frame, s_cr_jobs, s_cr_bytes);
    for (int i = 1; i < CPUREP_NIMG; i++)
        if (s_cr_prof_runs[i] || s_cr_sel[i] || s_cr_cap_total[i])
            fprintf(f, "  img %4d: profile runs %ld wall %.3f ms%s, captured %ld\n", i, s_cr_prof_runs[i],
                    s_cr_qf ? s_cr_prof_wall[i] * 1000.0 / (double)s_cr_qf : 0.0,
                    s_cr_sel[i] ? " [selected]" : "", s_cr_cap_total[i]);
    fclose(f);
}

static void cr_pick_top(void)
{
    int top[32]; int n = 0;
    for (int i = 1; i < CPUREP_NIMG; i++) {
        if (!s_cr_prof_runs[i]) continue;
        int k;
        if (n < s_cr_top) k = n++;
        else if (s_cr_prof_wall[i] > s_cr_prof_wall[top[n - 1]]) k = n - 1;   /* evict the smallest */
        else continue;
        top[k] = i;
        while (k > 0 && s_cr_prof_wall[top[k]] > s_cr_prof_wall[top[k - 1]]) { int t = top[k]; top[k] = top[k - 1]; top[k - 1] = t; k--; }
    }
    for (int k = 0; k < n; k++) s_cr_sel[top[k]] = 1;
    fprintf(stderr, "[cpu-replay] spu: profiled %d frames, capturing top %d images:", s_cr_prof_frames, n);
    for (int k = 0; k < n; k++) fprintf(stderr, " %d (%.1f ms)", top[k], s_cr_prof_wall[top[k]] * 1000.0 / (double)s_cr_qf);
    fprintf(stderr, "\n");
}

/* Phase machine, advanced once per frame by whichever SPU thread notices. */
static void cr_tick(long frame)
{
    const long last = s_cr_last_frame;
    if (frame == last || _InterlockedCompareExchange(&s_cr_last_frame, frame, last) != last) return;
    switch (s_cr_phase) {
    case CR_WAIT: {
        char go[MAX_PATH + 8]; snprintf(go, sizeof go, "%s\\go", s_cr_dir);
        const int key = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                        (GetAsyncKeyState(VK_F8) & 0x8000);
        const int file = GetFileAttributesA(go) != INVALID_FILE_ATTRIBUTES;
        if (key || (s_cr_at_frame >= 0 && frame >= s_cr_at_frame) || file) {
            if (file) DeleteFileA(go);   /* one-shot: a leftover go would re-trigger the next run */
            s_cr_phase_frame = frame;
            s_cr_phase = s_cr_auto ? CR_PROFILE : CR_CAPTURE;
            fprintf(stderr, "[cpu-replay] spu: triggered at frame %ld (%s)\n", frame, s_cr_auto ? "profiling" : "capturing");
            Beep(880, 80);
        }
        break; }
    case CR_PROFILE:
        if (frame - s_cr_phase_frame >= s_cr_prof_frames) {
            cr_pick_top();
            s_cr_phase_frame = frame + 1;   /* start on a frame boundary */
            s_cr_phase = CR_CAPTURE;
        }
        break;
    case CR_CAPTURE:
        if (frame - s_cr_phase_frame >= s_cr_frames || s_cr_jobs >= s_cr_max_jobs) {
            s_cr_phase = CR_DONE;
            cr_session_txt("done", frame);
            fprintf(stderr, "[cpu-replay] spu: capture done: %ld jobs, %.1f MB in %s\n",
                    s_cr_jobs, s_cr_bytes / 1048576.0, s_cr_dir);
            Beep(1320, 120);
        }
        break;
    default: break;
    }
}

/* LS-window GETs (R3: EA 0xF0000000+, another SPU's LS) are served inside
 * mfc_do_transfer after the hook; their bytes are read back from the
 * destination LS at the next capture event. */
static void cr_fill_window(void)
{
    if (!t_cr_win_size || !t_cr_ctx) return;
    for (uint32_t i = 0; i < t_cr_win_size && t_cr_win_off + i < t_cr_job.n; i++)
        t_cr_job.p[t_cr_win_off + i] = t_cr_ctx->ls[(t_cr_win_lsa + i) & SPU_LS_MASK];
    t_cr_win_size = 0;
}

void cpurep_spu_job_begin(spu_context* ctx, int img, uint32_t link, long busy)
{
    long frame = g_port_frame_no;
    if (!frame) { LARGE_INTEGER q; QueryPerformanceCounter(&q); frame = (long)(q.QuadPart / (s_cr_qf > 60 ? s_cr_qf / 60 : 1)); }
    cr_tick(frame);
    const long ph = s_cr_phase;
    if (ph == CR_PROFILE && img > 0 && img < CPUREP_NIMG) {
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        t_cr_pimg = img; t_cr_pq0 = q.QuadPart;
        return;
    }
    if (ph != CR_CAPTURE || frame < s_cr_phase_frame || img <= 0 || img >= CPUREP_NIMG || !s_cr_sel[img]) return;
    if (t_cr_ctx) return;                                       /* nested */
    if (s_cr_cap_frame[img] != frame) { _InterlockedExchange(&s_cr_cap_frame[img], frame); _InterlockedExchange(&s_cr_cap_n[img], 0); }
    if (s_cr_per_frame > 0 && _InterlockedIncrement(&s_cr_cap_n[img]) > s_cr_per_frame) return;
    if (_InterlockedIncrement(&s_cr_jobs) > s_cr_max_jobs) return;
    cr_file* cf = cr_open();
    if (!cf) return;
    _InterlockedIncrement(&s_cr_cap_total[img]);

    t_cr_job.n = 0;
    crsj_begin b; memset(&b, 0, sizeof b);
    b.seq = (uint32_t)_InterlockedIncrement(&s_cr_seq);
    b.frame = (int32_t)frame; b.job_img = img;
    b.entry_pc = ctx->pc & SPU_LS_MASK; b.link = link & SPU_LS_MASK;
    b.image_id = ctx->image_id; b.resident_ovl = ctx->resident_ovl;
    b.resident_ovl_lsa = ctx->resident_ovl_lsa; b.ls_bias = ctx->ls_bias;
    b.policy_mode = ctx->policy_mode; b.resident_task = ctx->resident_task; b.module_img_a00 = ctx->module_img_a00;
    b.spu_id = ctx->spu_id; b.decrementer = ctx->decrementer; b.event_mask = ctx->event_mask;
    b.int_enable = ctx->int_enable; b.host_depth = ctx->host_depth;
    b.mfc_lsa = ctx->mfc_lsa; b.mfc_eah = ctx->mfc_eah; b.mfc_eal = ctx->mfc_eal; b.mfc_size = ctx->mfc_size;
    b.mfc_tag = ctx->mfc_tag; b.mfc_tag_mask = ctx->mfc_tag_mask; b.mfc_tag_status = ctx->mfc_tag_status;
    b.atomic_stat = ctx->atomic_stat; b.busy_at_start = (uint32_t)busy;
    for (unsigned s = 0; s < CRSJ_SLOTS && s < SPU_RESIDENT_CODE_SLOTS; s++) {
        b.slot[s].lsa = ctx->resident_code[s].lsa; b.slot[s].size = ctx->resident_code[s].size;
        b.slot[s].source_ea = ctx->resident_code[s].source_ea;
        b.slot[s].image_id = ctx->resident_code[s].image_id; b.slot[s].pic = ctx->resident_code[s].pic;
    }
    uint32_t idv[CRSJ_NCHUNK];
    cr_buf chunks = {0};
    cr_ls(cf, &chunks, ctx->ls, idv);
    if (chunks.n) cr_put(&t_cr_job, chunks.p, chunks.n);
    free(chunks.p);
    LARGE_INTEGER q; QueryPerformanceCounter(&q); b.qpc = (uint64_t)q.QuadPart;
    const uint32_t r[2] = { CRSJ_T_BEGIN, (uint32_t)(sizeof b + 2048 + sizeof idv) };
    cr_put(&t_cr_job, r, 8); cr_put(&t_cr_job, &b, sizeof b);
    cr_put(&t_cr_job, ctx->gpr, 2048); cr_put(&t_cr_job, idv, sizeof idv);
    t_cr_seq = b.seq; t_cr_ndma = t_cr_natom = 0; t_cr_win_size = 0;
    t_cr_ctx = ctx;
    ULONG64 c = 0; QueryThreadCycleTime(GetCurrentThread(), &c); t_cr_c0 = c;
    QueryPerformanceCounter(&q); t_cr_q0 = q.QuadPart;
    _InterlockedIncrement(&g_cpurep_spu_live);
}

void cpurep_spu_job_end(int how)
{
    if (t_cr_pimg) {
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        if (!how) {
            _InterlockedExchangeAdd64(&s_cr_prof_wall[t_cr_pimg], q.QuadPart - t_cr_pq0);
            _InterlockedIncrement(&s_cr_prof_runs[t_cr_pimg]);
        }
        t_cr_pimg = 0;
    }
    spu_context* ctx = t_cr_ctx;
    if (!ctx) return;
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    ULONG64 c = 0; QueryThreadCycleTime(GetCurrentThread(), &c);
    cr_fill_window();
    t_cr_ctx = NULL;
    _InterlockedDecrement(&g_cpurep_spu_live);
    cr_file* cf = t_cr_file;
    if (!cf) return;
    crsj_end e; memset(&e, 0, sizeof e);
    e.seq = t_cr_seq; e.how = (uint32_t)how; e.ndma = t_cr_ndma; e.natom = t_cr_natom;
    e.exit_pc = ctx->pc & SPU_LS_MASK;
    e.wall_qpc = (uint64_t)(q.QuadPart - t_cr_q0); e.cycles = c - t_cr_c0;
    uint32_t idv[CRSJ_NCHUNK];
    cr_buf chunks = {0};
    cr_ls(cf, &chunks, ctx->ls, idv);
    if (chunks.n) cr_put(&t_cr_job, chunks.p, chunks.n);
    free(chunks.p);
    const uint32_t r[2] = { CRSJ_T_END, (uint32_t)(sizeof e + 2048 + sizeof idv) };
    cr_put(&t_cr_job, r, 8); cr_put(&t_cr_job, &e, sizeof e);
    cr_put(&t_cr_job, ctx->gpr, 2048); cr_put(&t_cr_job, idv, sizeof idv);
    fwrite(t_cr_job.p, 1, t_cr_job.n, cf->f);
    fflush(cf->f);
    _InterlockedExchangeAdd64(&s_cr_bytes, (long long)t_cr_job.n);
    t_cr_job.n = 0;
    if (t_cr_job.cap > (64u << 20)) { free(t_cr_job.p); t_cr_job.p = NULL; t_cr_job.cap = 0; }
}

static int cr_committed(uint64_t ea, uint32_t size)
{
    if (!vm_base || (ea >> 32) || !size) return 0;
    const uint8_t* p = vm_base + (uint32_t)ea;
    const uint8_t* e = p + size;
    while (p < e) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
        p = (const uint8_t*)mi.BaseAddress + mi.RegionSize;
    }
    return 1;
}

static int cr_is_get(uint32_t c) { c &= 0xFF; return (c & 0xF0) == 0x40; }
static int cr_is_put(uint32_t c) { c &= 0xFF; return (c & 0xF0) == 0x20 || (c & 0xF0) == 0x30; }

/* mfc_do_transfer (spu_dma.h): every executed transfer, list elements one by one. */
void cpurep_spu_dma(spu_context* spu, uint32_t lsa, uint64_t ea, uint32_t size, uint32_t cmd)
{
    if (spu != t_cr_ctx) return;
    cr_fill_window();
    crsj_dma d; memset(&d, 0, sizeof d);
    d.cmd = cmd; d.lsa = lsa & SPU_LS_MASK; d.ea_lo = (uint32_t)ea; d.ea_hi = (uint32_t)(ea >> 32);
    d.size = size; d.pc = spu->pc & SPU_LS_MASK;
    const void* bytes = NULL; uint32_t n = 0;
    static const uint8_t zero[16384];
    if (size && size <= 16384 && cr_is_get(cmd)) {
        if ((uint32_t)ea >= 0xF0000000u && !(ea >> 32)) {
            d.flags = CRSJ_DMA_GET_BYTES | CRSJ_DMA_LS_WINDOW; bytes = zero; n = size;
        } else if (cr_committed(ea, size)) {
            d.flags = CRSJ_DMA_GET_BYTES; bytes = vm_base + (uint32_t)ea; n = size;
        } else d.flags = CRSJ_DMA_NO_DATA;
    } else if (size && size <= 16384 && cr_is_put(cmd) && s_cr_puts) {
        d.flags = CRSJ_DMA_PUT_BYTES; n = size;
        uint8_t tmp[16384];
        for (uint32_t i = 0; i < size; i++) tmp[i] = spu->ls[(lsa + i) & SPU_LS_MASK];
        cr_rec(&t_cr_job, CRSJ_T_DMA, &d, sizeof d, tmp, n);
        t_cr_ndma++;
        return;
    }
    if (d.flags & CRSJ_DMA_LS_WINDOW) {
        t_cr_win_off = t_cr_job.n + 8 + sizeof d; t_cr_win_lsa = lsa; t_cr_win_size = size;
    }
    cr_rec(&t_cr_job, CRSJ_T_DMA, &d, sizeof d, bytes, n);
    t_cr_ndma++;
}

/* spu_wrch MFC_Cmd (spu_channels.c): runs the atomic and records its outcome. */
int cpurep_spu_atomic(spu_context* ctx, uint32_t cmd, int (*fn)(spu_context*, uint32_t))
{
    const int rc = fn(ctx, cmd);
    if (!rc || ctx != t_cr_ctx) return rc;
    cr_fill_window();
    crsj_atom a; memset(&a, 0, sizeof a);
    a.cmd = cmd; a.ea = ctx->mfc_eal; a.lsa = ctx->mfc_lsa & SPU_LS_MASK; a.rc = rc;
    a.atomic_stat = ctx->atomic_stat; a.pc = ctx->pc & SPU_LS_MASK;
    uint8_t line[128];
    const uint32_t l = a.lsa & ~127u;
    for (uint32_t i = 0; i < 128; i++) line[i] = ctx->ls[(l + i) & SPU_LS_MASK];
    cr_rec(&t_cr_job, CRSJ_T_ATOM, &a, sizeof a, line, 128);
    t_cr_natom++;
    return rc;
}
#endif /* _WIN32 */
