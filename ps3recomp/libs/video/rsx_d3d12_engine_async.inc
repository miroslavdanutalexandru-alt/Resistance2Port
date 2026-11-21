/* rsx_d3d12_engine_async.inc -- part of rsx_d3d12_engine.c (included there,
 * after rsx_d3d12_engine_draw.inc).
 *
 * The asynchronous pipeline compiler and the persisted PSO library.
 *
 * Async compile (RPCS3's async recompiler, the Vulkan engine backend's
 * RSX_VK_ASYNC_PIPELINES): pipeline_create only queues the FXC compile on a
 * worker pool and returns a handle at once; draws that use a pipeline (or a
 * PSO variant) that is not built yet are skipped, as on the Vulkan path.
 * Workers never touch s_pipe (it can be reallocated): a job carries a
 * snapshot of the pipeline and its results land on a done list that the
 * render thread adopts (d3e_async_poll), checking the pipeline's generation
 * so a slot released and reused meanwhile drops a stale result.
 *   R2_SHADER_MODE=sync          compile on the render thread (no skipped draws)
 *   R2_SHADER_MODE=async|interp  asynchronous (default; there is no interpreter)
 *   RSX_D3D12_ASYNC=0|1          overrides R2_SHADER_MODE
 *   RSX_D3D12_PIPELINE_THREADS=n worker count (default from the core count)
 *
 * PSO library (ID3D12PipelineLibrary): every guest PSO is stored by a hash of
 * its full description and loaded from it on later runs, so a second run does
 * not pay the driver's compile again. Persisted to cache\d3d12_pso.bin next
 * to the executable; the header carries the adapter LUID, the driver (UMD)
 * version and the executable's size and timestamp, and a checksum of the
 * payload -- any mismatch, a damaged file or a driver refusal starts empty.
 *   RSX_D3D12_PSO_CACHE=0        disables it */

/* ---- PSO library ------------------------------------------------------------ */

#define D3E_PL_MAGIC   0x4C503344u      /* "D3PL" */
#define D3E_PL_VERSION 1u
#define D3E_PL_MAX     (512ull << 20)

typedef struct {
    u32 magic, version;
    u64 luid, umd, exe_size, exe_mtime;
    u64 size, sum;
} d3e_pl_hdr;

static struct {
    int  inited;                 /* cs initialised */
    CRITICAL_SECTION cs;
    ID3D12PipelineLibrary*  lib;
    ID3D12PipelineLibrary1* lib1;
    void* blob;                  /* must outlive lib */
    u32  stored, saved;          /* pipelines stored / at the last save */
    u32  loads, misses;
    DWORD last_save;
    d3e_pl_hdr want;
} s_pl;

/* Library access: loads / stores of different names run in parallel (the
 * library synchronises itself, except for loads of one name -- a stripe per
 * name hash); Serialize takes the library exclusively. s_pl.cs only guards
 * the counters now.
 *   RSX_D3D12_PSO_LIB_PARALLEL=0   one stripe: every load / store serialised (the old way) */
#define D3E_PL_STRIPES 16u
static SRWLOCK s_pl_big = SRWLOCK_INIT;
static SRWLOCK s_pl_stripe[D3E_PL_STRIPES];      /* zero == SRWLOCK_INIT */

static u32 d3e_pl_stripe(const WCHAR* name)
{
    static int par = -1;
    if (par < 0) par = d3e_env_on("RSX_D3D12_PSO_LIB_PARALLEL", 1);
    if (!par) return 0;
    u32 h = 2166136261u;
    for (; *name; name++) { h ^= (u32)*name; h *= 16777619u; }
    return h % D3E_PL_STRIPES;
}

static void d3e_pl_path(char* out, size_t n, int dir_only)
{
    if (dir_only) snprintf(out, n, "%scache", d3e_exe_dir());
    else snprintf(out, n, "%scache\\d3d12_pso.bin", d3e_exe_dir());
}

static void d3e_pl_header(d3e_pl_hdr* h)
{
    memset(h, 0, sizeof *h);
    h->magic = D3E_PL_MAGIC;
    h->version = D3E_PL_VERSION;
    DXGI_ADAPTER_DESC1 ad;
    memset(&ad, 0, sizeof ad);
    if (s_d.adapter && SUCCEEDED(IDXGIAdapter1_GetDesc1(s_d.adapter, &ad)))
        h->luid = ((u64)(u32)ad.AdapterLuid.HighPart << 32) | ad.AdapterLuid.LowPart;
    LARGE_INTEGER umd;
    umd.QuadPart = 0;
    if (s_d.adapter && SUCCEEDED(IDXGIAdapter1_CheckInterfaceSupport(s_d.adapter, &IID_IDXGIDevice, &umd)))
        h->umd = (u64)umd.QuadPart;
    char path[MAX_PATH + 2];
    const DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (n && n < MAX_PATH) {
        path[n] = 0;
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &a)) {
            h->exe_size = ((u64)a.nFileSizeHigh << 32) | a.nFileSizeLow;
            h->exe_mtime = ((u64)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
        }
    }
}

static void d3e_plib_open(void)
{
    if (s_pl.lib || !s_d.dev || s_d3e_probing || !d3e_env_on("RSX_D3D12_PSO_CACHE", 1)) return;
    ID3D12Device1* d1 = NULL;
    if (FAILED(ID3D12Device_QueryInterface(s_d.dev, &IID_ID3D12Device1, (void**)&d1)) || !d1) {
        D3E_LOG("PSO cache: ID3D12Device1 missing, pipelines are not persisted\n");
        return;
    }
    if (!s_pl.inited) { InitializeCriticalSection(&s_pl.cs); s_pl.inited = 1; }
    d3e_pl_header(&s_pl.want);
    char path[MAX_PATH * 2];
    d3e_pl_path(path, sizeof path, 0);
    const char* why = "no cache yet";
    FILE* f = fopen(path, "rb");
    if (f) {
        d3e_pl_hdr h;
        if (fread(&h, sizeof h, 1, f) != 1) why = "truncated header";
        else if (h.magic != D3E_PL_MAGIC || h.version != D3E_PL_VERSION) why = "unknown format";
        else if (h.luid != s_pl.want.luid) why = "another adapter";
        else if (h.umd != s_pl.want.umd) why = "driver changed";
        else if (h.exe_size != s_pl.want.exe_size || h.exe_mtime != s_pl.want.exe_mtime) why = "executable changed";
        else if (!h.size || h.size > D3E_PL_MAX) why = "bad size";
        else {
            void* b = malloc((size_t)h.size);
            if (!b) why = "out of memory";
            else if (fread(b, 1, (size_t)h.size, f) != (size_t)h.size) { why = "truncated"; free(b); }
            else if (d3e_hash(b, (size_t)h.size, 0) != h.sum) { why = "checksum mismatch"; free(b); }
            else {
                const HRESULT hr = ID3D12Device1_CreatePipelineLibrary(d1, b, (SIZE_T)h.size,
                                                                      &IID_ID3D12PipelineLibrary, (void**)&s_pl.lib);
                if (SUCCEEDED(hr) && s_pl.lib) {
                    s_pl.blob = b;
                    why = NULL;
                } else {
                    s_pl.lib = NULL;
                    free(b);
                    why = hr == D3D12_ERROR_DRIVER_VERSION_MISMATCH ? "driver refused (version)"
                        : hr == D3D12_ERROR_ADAPTER_NOT_FOUND ? "driver refused (adapter)"
                        : "driver refused (corrupt)";
                }
            }
        }
        fclose(f);
    }
    if (why) {
        if (FAILED(ID3D12Device1_CreatePipelineLibrary(d1, NULL, 0, &IID_ID3D12PipelineLibrary, (void**)&s_pl.lib)))
            s_pl.lib = NULL;
        if (s_pl.lib) D3E_LOG("PSO cache: starting empty (%s)\n", why);
        else D3E_LOG("PSO cache: the driver offers no pipeline library, pipelines are not persisted\n");
    } else {
        D3E_LOG("PSO cache: %s loaded\n", path);
    }
    ID3D12Device1_Release(d1);
    if (s_pl.lib)
        ID3D12PipelineLibrary_QueryInterface(s_pl.lib, &IID_ID3D12PipelineLibrary1, (void**)&s_pl.lib1);
    s_pl.stored = s_pl.saved = 0;
    s_pl.last_save = GetTickCount();
}

/* Write the library out when it holds pipelines the file does not. */
static void d3e_plib_save(void)
{
    if (!s_pl.lib) return;
    AcquireSRWLockExclusive(&s_pl_big);      /* no load / store meanwhile: `stored` matches the library */
    EnterCriticalSection(&s_pl.cs);
    const u32 stored = s_pl.stored;
    const int same = stored == s_pl.saved;
    LeaveCriticalSection(&s_pl.cs);
    if (same) { ReleaseSRWLockExclusive(&s_pl_big); return; }
    const SIZE_T n = ID3D12PipelineLibrary_GetSerializedSize(s_pl.lib);
    void* b = (n && n <= D3E_PL_MAX) ? malloc(n) : NULL;
    HRESULT hr = b ? ID3D12PipelineLibrary_Serialize(s_pl.lib, b, n) : E_OUTOFMEMORY;
    ReleaseSRWLockExclusive(&s_pl_big);
    if (FAILED(hr)) {
        D3E_WARN_ONCE(27, "PSO cache: serialising %llu bytes failed 0x%08lX\n", (unsigned long long)n, (unsigned long)hr);
        free(b);
        return;
    }
    d3e_pl_hdr h = s_pl.want;
    h.size = (u64)n;
    h.sum = d3e_hash(b, n, 0);
    char dir[MAX_PATH * 2], path[MAX_PATH * 2], tmp[MAX_PATH * 2 + 16];
    d3e_pl_path(dir, sizeof dir, 1);
    CreateDirectoryA(dir, NULL);
    d3e_pl_path(path, sizeof path, 0);
    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, (unsigned long)GetCurrentProcessId());
    FILE* f = fopen(tmp, "wb");
    int ok = 0;
    if (f) {
        ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(b, 1, n, f) == n;
        ok = fclose(f) == 0 && ok;
    }
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp);
        D3E_WARN_ONCE(27, "PSO cache: %s could not be written\n", path);
    } else {
        EnterCriticalSection(&s_pl.cs);
        s_pl.saved = stored;
        LeaveCriticalSection(&s_pl.cs);
    }
    free(b);
}

static void d3e_plib_close(void)
{
    if (s_pl.lib) {
        d3e_plib_save();
        if (s_pl.loads || s_pl.misses)
            D3E_LOG("PSO cache: %u pipelines loaded, %u built\n", s_pl.loads, s_pl.misses);
    }
    if (s_pl.lib1) ID3D12PipelineLibrary1_Release(s_pl.lib1);
    if (s_pl.lib) ID3D12PipelineLibrary_Release(s_pl.lib);
    free(s_pl.blob);
    s_pl.lib = NULL; s_pl.lib1 = NULL; s_pl.blob = NULL;
    s_pl.stored = s_pl.saved = s_pl.loads = s_pl.misses = 0;
}

/* The name a guest PSO is stored under: everything that defines it except
 * the root signature (the executable identity in the header covers it). */
static void d3e_pso_name(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, const void* ds2, size_t ds2n,
                         int stream, WCHAR out[24])
{
    u64 h = d3e_hash(pd->VS.pShaderBytecode, pd->VS.BytecodeLength, 0);
    h = d3e_hash(pd->PS.pShaderBytecode, pd->PS.BytecodeLength, h);
    if (pd->GS.BytecodeLength) h = d3e_hash(pd->GS.pShaderBytecode, pd->GS.BytecodeLength, h ^ 0x4753ull);
    h = d3e_hash(&pd->BlendState, sizeof pd->BlendState, h);
    h = d3e_hash(&pd->SampleMask, sizeof pd->SampleMask, h);
    h = d3e_hash(&pd->RasterizerState, sizeof pd->RasterizerState, h);
    h = d3e_hash(&pd->DepthStencilState, sizeof pd->DepthStencilState, h);
    for (u32 i = 0; i < pd->InputLayout.NumElements; i++) {
        const D3D12_INPUT_ELEMENT_DESC* e = &pd->InputLayout.pInputElementDescs[i];
        const u32 v[6] = { e->SemanticIndex, (u32)e->Format, e->InputSlot, e->AlignedByteOffset,
                           (u32)e->InputSlotClass, e->InstanceDataStepRate };
        h = d3e_hash(v, sizeof v, h);
    }
    const u32 t[5] = { (u32)pd->PrimitiveTopologyType, pd->NumRenderTargets, (u32)pd->DSVFormat,
                       pd->SampleDesc.Count, pd->SampleDesc.Quality };
    h = d3e_hash(t, sizeof t, h);
    if (s_up.on) h ^= 0x5550ull;     /* render scale: the root signature has b3 */
    h = d3e_hash(pd->RTVFormats, sizeof pd->RTVFormats, h);
    if (stream) h = d3e_hash(ds2, ds2n, h ^ 0x5354524541ull);
    swprintf(out, 24, L"g%016llX", (unsigned long long)h);
}

/* Create a guest PSO through the library: load it when stored, else build
 * it and store it. `sd` non-NULL: the pipeline-state-stream path. */
static HRESULT d3e_plib_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, const D3D12_PIPELINE_STATE_STREAM_DESC* sd,
                            const void* ds2, size_t ds2n, ID3D12PipelineState** pso)
{
    *pso = NULL;
    WCHAR name[24];
    const int lib = s_pl.lib && (!sd || s_pl.lib1);
    if (lib) {
        d3e_pso_name(pd, ds2, ds2n, sd != NULL, name);
        HRESULT hr;
        SRWLOCK* stripe = &s_pl_stripe[d3e_pl_stripe(name)];
        AcquireSRWLockShared(&s_pl_big);
        AcquireSRWLockExclusive(stripe);
        if (sd) hr = ID3D12PipelineLibrary1_LoadPipeline(s_pl.lib1, name, sd, &IID_ID3D12PipelineState, (void**)pso);
        else hr = ID3D12PipelineLibrary_LoadGraphicsPipeline(s_pl.lib, name, pd, &IID_ID3D12PipelineState, (void**)pso);
        ReleaseSRWLockExclusive(stripe);
        if (SUCCEEDED(hr) && *pso) { EnterCriticalSection(&s_pl.cs); s_pl.loads++; LeaveCriticalSection(&s_pl.cs); }
        ReleaseSRWLockShared(&s_pl_big);
        if (SUCCEEDED(hr) && *pso) return S_OK;
        *pso = NULL;
    }
    HRESULT hr = sd ? ID3D12Device2_CreatePipelineState(s_d.dev2, sd, &IID_ID3D12PipelineState, (void**)pso)
                    : ID3D12Device_CreateGraphicsPipelineState(s_d.dev, pd, &IID_ID3D12PipelineState, (void**)pso);
    if (SUCCEEDED(hr) && *pso && lib) {
        SRWLOCK* stripe = &s_pl_stripe[d3e_pl_stripe(name)];
        AcquireSRWLockShared(&s_pl_big);
        AcquireSRWLockExclusive(stripe);
        /* E_INVALIDARG: the name is taken by a description that no longer
         * loads (a stale entry, or another worker stored it first); the new PSO still works. */
        const int ok = SUCCEEDED(ID3D12PipelineLibrary_StorePipeline(s_pl.lib, name, *pso));
        ReleaseSRWLockExclusive(stripe);
        EnterCriticalSection(&s_pl.cs);
        s_pl.misses++;
        if (ok) s_pl.stored++;
        LeaveCriticalSection(&s_pl.cs);
        ReleaseSRWLockShared(&s_pl_big);
    }
    return hr;
}

/* ---- the compiler pool ---------------------------------------------------------- */

enum { D3E_JOB_COMPILE = 0, D3E_JOB_VARIANT, D3E_JOB_PREWARM, D3E_JOB_SAVE };

#define D3E_JOB_OUT 6u
typedef struct d3e_job {
    struct d3e_job* next;
    u32  kind, idx, gen;
    char* vs_src;
    char* ps_src;
    d3e_pipe p;                     /* snapshot; p.vs / p.ps are the job's references */
    d3e_vkey key;                   /* D3E_JOB_VARIANT */
    u32  topo_mask, dsf;            /* D3E_JOB_PREWARM */
    u32  nout;
    u64  out_key[D3E_JOB_OUT];
    ID3D12PipelineState* out_pso[D3E_JOB_OUT];
    volatile LONG cancel;
    int  failed;
} d3e_job;

#define D3E_ASYNC_MAX_THREADS 16u
static struct {
    int  state;                     /* 0 untried, 1 on, -1 off */
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    d3e_job *hi, *hi_tail, *lo, *lo_tail;     /* queued: draws wait on hi */
    d3e_job *done;
    volatile LONG ndone;
    volatile LONG busy;             /* queued + running */
    int  stop;
    HANDLE th[D3E_ASYNC_MAX_THREADS];
    u32  nth;
    volatile LONG last_dsf;         /* the depth format draws use most recently */
    u32  skipped;                   /* draws skipped this run */
    u32  save_pending;
} s_as;

static void d3e_job_free(d3e_job* j)
{
    if (!j) return;
    if (j->p.vs) ID3D10Blob_Release(j->p.vs);
    if (j->p.ps) ID3D10Blob_Release(j->p.ps);
    for (u32 i = 0; i < j->nout; i++) d3e_release(j->out_pso[i]);
    free(j->vs_src);
    free(j->ps_src);
    free(j);
}

static void d3e_job_out(d3e_job* j, const d3e_vkey* k)
{
    if (j->nout >= D3E_JOB_OUT || j->cancel) return;
    ID3D12PipelineState* pso = d3e_build_pso(&j->p, k);
    j->out_key[j->nout] = d3e_hash(k, sizeof *k, 0) | 1u;
    j->out_pso[j->nout] = pso;      /* NULL: a failure, cached as on the sync path */
    j->nout++;
}

/* Prewarm: the topology classes (bit n = rsx_topology n) with no bias, no
 * wireframe and the depth format draws used last -- the common case. */
static void d3e_job_prewarm(d3e_job* j)
{
    u32 cls = 0;
    for (u32 t = RSX_TOPOLOGY_POINTS; t <= RSX_TOPOLOGY_TRIANGLE_STRIP; t++) {
        if (!((j->topo_mask >> t) & 1u)) continue;
        cls |= 1u << (t == RSX_TOPOLOGY_POINTS ? 0u : t >= RSX_TOPOLOGY_TRIANGLES ? 2u : 1u);
    }
    static const u32 topo[3] = { D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,
                                 D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE };
    for (u32 c = 0; c < 3; c++) {
        if (!((cls >> c) & 1u)) continue;
        d3e_vkey k;
        memset(&k, 0, sizeof k);
        k.topo = topo[c];
        k.dsf = j->dsf;
        k.stencil = j->dsf == DXGI_FORMAT_D24_UNORM_S8_UINT || j->dsf == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        if (c == 0 && j->p.gsio) k.gs = 1;      /* points draw through the expansion GS (d3e_draw) */
        d3e_job_out(j, &k);
    }
}

static void d3e_job_run(d3e_job* j)
{
    if (j->cancel) return;
    switch (j->kind) {
    case D3E_JOB_COMPILE:
        j->p.vs = d3e_compile(j->vs_src, "main", "vs_5_0", "rsx_vp");
        j->p.ps = j->p.vs ? d3e_compile(j->ps_src, "main", "ps_5_0", "rsx_fp") : NULL;
        if (!j->p.vs || !j->p.ps) j->failed = 1;
        break;
    case D3E_JOB_VARIANT:
        d3e_job_out(j, &j->key);
        break;
    case D3E_JOB_PREWARM:
        d3e_job_prewarm(j);
        break;
    case D3E_JOB_SAVE:
        d3e_plib_save();
        break;
    default:
        break;
    }
}

static DWORD WINAPI d3e_async_main(LPVOID arg)
{
    (void)arg;
    EnterCriticalSection(&s_as.cs);
    for (;;) {
        while (!s_as.stop && !s_as.hi && !s_as.lo) SleepConditionVariableCS(&s_as.cv, &s_as.cs, INFINITE);
        if (s_as.stop) break;
        d3e_job* j = s_as.hi ? s_as.hi : s_as.lo;
        if (j == s_as.hi) { s_as.hi = j->next; if (!s_as.hi) s_as.hi_tail = NULL; }
        else { s_as.lo = j->next; if (!s_as.lo) s_as.lo_tail = NULL; }
        j->next = NULL;
        LeaveCriticalSection(&s_as.cs);
        d3e_job_run(j);
        EnterCriticalSection(&s_as.cs);
        j->next = s_as.done;
        s_as.done = j;
        InterlockedIncrement(&s_as.ndone);
        InterlockedDecrement(&s_as.busy);
    }
    LeaveCriticalSection(&s_as.cs);
    return 0;
}

static u32 d3e_async_threads(void)
{
    const char* e = getenv("RSX_D3D12_PIPELINE_THREADS");
    int n = (e && *e) ? atoi(e) : 0;
    if (n <= 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const u32 hw = (u32)si.dwNumberOfProcessors;
        n = hw >= 24 ? 12 : hw >= 16 ? 8 : hw > 12 ? 6 : hw > 8 ? 4 : hw == 8 ? 2 : 1;
    }
    return (u32)n > D3E_ASYNC_MAX_THREADS ? D3E_ASYNC_MAX_THREADS : (u32)n;
}

/* The policy (see the top of the file); starts the pool on first use. */
static int d3e_async_on(void)
{
    if (s_as.state) return s_as.state > 0;
    s_as.state = -1;
    const char* e = getenv("RSX_D3D12_ASYNC");
    int on;
    if (e && *e) on = *e != '0';
    else {
        const char* m = getenv("R2_SHADER_MODE");
        on = !(m && !_stricmp(m, "sync"));
    }
    if (!on) {
        D3E_LOG("pipelines: synchronous (%s)\n", (e && *e) ? "RSX_D3D12_ASYNC=0" : "R2_SHADER_MODE=sync");
        return 0;
    }
    InitializeCriticalSection(&s_as.cs);
    InitializeConditionVariable(&s_as.cv);
    s_as.stop = 0;
    s_as.hi = s_as.hi_tail = s_as.lo = s_as.lo_tail = s_as.done = NULL;
    s_as.ndone = s_as.busy = 0;
    s_as.last_dsf = DXGI_FORMAT_D24_UNORM_S8_UINT;
    const u32 want = d3e_async_threads();
    s_as.nth = 0;
    for (u32 i = 0; i < want; i++) {
        /* FXC recurses on long programs: a roomy stack */
        s_as.th[i] = CreateThread(NULL, 8u << 20, d3e_async_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!s_as.th[i]) break;
        s_as.nth++;
    }
    if (!s_as.nth) {
        DeleteCriticalSection(&s_as.cs);
        D3E_LOG("pipelines: no compiler thread could start, synchronous\n");
        return 0;
    }
    s_as.state = 1;
    D3E_LOG("pipelines: asynchronous, %u compiler threads (draws wait for their pipeline)\n", s_as.nth);
    return 1;
}

static void d3e_async_push(d3e_job* j, int hi)
{
    InterlockedIncrement(&s_as.busy);
    EnterCriticalSection(&s_as.cs);
    j->next = NULL;
    if (hi) { if (s_as.hi_tail) s_as.hi_tail->next = j; else s_as.hi = j; s_as.hi_tail = j; }
    else    { if (s_as.lo_tail) s_as.lo_tail->next = j; else s_as.lo = j; s_as.lo_tail = j; }
    WakeConditionVariable(&s_as.cv);
    LeaveCriticalSection(&s_as.cs);
}

static d3e_job* d3e_job_new(u32 kind, const d3e_pipe* P, u32 idx)
{
    d3e_job* j = (d3e_job*)calloc(1, sizeof *j);
    if (!j) return NULL;
    j->kind = kind;
    j->idx = idx;
    if (P) {
        j->p = *P;
        j->gen = P->gen;
        j->p.job = NULL;
        j->p.nvar = 0;
        if (j->p.vs) ID3D10Blob_AddRef(j->p.vs);
        if (j->p.ps) ID3D10Blob_AddRef(j->p.ps);
    }
    return j;
}

/* Queue the compile of a pipeline pipeline_create just set up (P->pending). */
static int d3e_async_compile(d3e_pipe* P, u32 idx, const char* vs_src, const char* ps_src)
{
    d3e_job* j = d3e_job_new(D3E_JOB_COMPILE, P, idx);
    if (!j) return 0;
    j->vs_src = _strdup(vs_src);
    j->ps_src = _strdup(ps_src);
    if (!j->vs_src || !j->ps_src) { d3e_job_free(j); return 0; }
    P->job = j;
    d3e_async_push(j, 1);
    return 1;
}

static void d3e_async_prewarm_job(d3e_pipe* P, u32 idx, u32 mask)
{
    d3e_job* j = d3e_job_new(D3E_JOB_PREWARM, P, idx);
    if (!j) return;
    j->topo_mask = mask;
    j->dsf = (u32)s_as.last_dsf;
    d3e_async_push(j, 0);
}

/* d3e_variant_get's miss in async mode: queue the PSO, the slot is pending. */
static int d3e_async_variant(d3e_pipe* P, const d3e_vkey* k)
{
    const u32 idx = (u32)(P - s_pipe);
    d3e_job* j = d3e_job_new(D3E_JOB_VARIANT, P, idx);
    if (!j) return 0;
    j->key = *k;
    d3e_async_push(j, 1);
    return 1;
}

static void d3e_async_adopt_pso(d3e_pipe* P, u64 key, ID3D12PipelineState* pso)
{
    for (u32 i = 0; i < P->nvar; i++) {
        if (P->var[i].key != key) continue;
        if (P->var[i].pending) { P->var[i].pso = pso; P->var[i].pending = 0; }
        else d3e_release(pso);          /* built meanwhile */
        return;
    }
    if (P->nvar < D3E_PIPE_VARIANTS) {
        d3e_variant* v = &P->var[P->nvar++];
        v->key = key; v->pso = pso; v->pending = 0;
    } else d3e_release(pso);
}

static void d3e_async_adopt(d3e_job* j)
{
    d3e_pipe* P = (j->idx < s_pipe_cap) ? &s_pipe[j->idx] : NULL;
    if (!P || !P->used || P->gen != j->gen || j->cancel) return;   /* d3e_job_free drops it all */
    if (j->kind == D3E_JOB_COMPILE && P->pending && P->job == j) {
        P->job = NULL;
        P->pending = 0;
        if (j->failed) {
            P->failed = 1;      /* FXC logged why; draws with it are skipped */
        } else {
            P->vs = j->p.vs; j->p.vs = NULL;
            P->ps = j->p.ps; j->p.ps = NULL;
            if (P->prewarm) { d3e_async_prewarm_job(P, j->idx, P->prewarm); P->prewarm = 0; }
        }
    }
    for (u32 i = 0; i < j->nout; i++) {
        if (j->out_pso[i] || j->kind == D3E_JOB_VARIANT) d3e_async_adopt_pso(P, j->out_key[i], j->out_pso[i]);
        j->out_pso[i] = NULL;
    }
    j->nout = 0;
}

/* Render thread: adopt what the workers finished. */
static void d3e_async_poll(void)
{
    if (s_as.state <= 0 || !s_as.ndone) return;
    EnterCriticalSection(&s_as.cs);
    d3e_job* l = s_as.done;
    s_as.done = NULL;
    s_as.ndone = 0;
    LeaveCriticalSection(&s_as.cs);
    while (l) {
        d3e_job* n = l->next;
        d3e_async_adopt(l);
        d3e_job_free(l);
        l = n;
    }
}

static int d3e_async_active(void)
{
    return s_as.state > 0;
}

static void d3e_async_draw_skipped(void)
{
    s_as.skipped++;
}

static void d3e_async_note_dsf(u32 dsf)
{
    s_as.last_dsf = (LONG)dsf;
}

/* pipeline_release of a pipeline whose compile is still queued. */
static void d3e_async_forget(d3e_pipe* P)
{
    if (P->job) { InterlockedExchange(&P->job->cancel, 1); P->job = NULL; }
}

/* Once per presented frame: the PSO library is saved off the render thread
 * at most every 20 s while new pipelines arrive. */
static void d3e_async_frame(void)
{
    d3e_async_poll();
    if (!s_pl.lib || s_pl.stored == s_pl.saved) return;
    const DWORD now = GetTickCount();
    if (now - s_pl.last_save < 20000u) return;
    s_pl.last_save = now;
    if (s_as.state > 0) {
        d3e_job* j = d3e_job_new(D3E_JOB_SAVE, NULL, 0);
        if (j) { d3e_async_push(j, 0); return; }
    }
    d3e_plib_save();
}

/* Device teardown: join the workers (they use the device), drop every job. */
static void d3e_async_stop(void)
{
    if (s_as.state <= 0) { s_as.state = 0; return; }
    EnterCriticalSection(&s_as.cs);
    s_as.stop = 1;
    WakeAllConditionVariable(&s_as.cv);
    LeaveCriticalSection(&s_as.cs);
    for (u32 i = 0; i < s_as.nth; i++) {
        WaitForSingleObject(s_as.th[i], INFINITE);
        CloseHandle(s_as.th[i]);
        s_as.th[i] = NULL;
    }
    s_as.nth = 0;
    d3e_job* lists[3] = { s_as.hi, s_as.lo, s_as.done };
    for (u32 i = 0; i < 3; i++)
        for (d3e_job* j = lists[i]; j; ) { d3e_job* n = j->next; d3e_job_free(j); j = n; }
    s_as.hi = s_as.hi_tail = s_as.lo = s_as.lo_tail = s_as.done = NULL;
    s_as.ndone = s_as.busy = 0;
    for (u32 i = 0; i < s_pipe_cap; i++) s_pipe[i].job = NULL;
    if (s_as.skipped) D3E_LOG("pipelines: %u draws waited for a pipeline\n", s_as.skipped);
    s_as.skipped = 0;
    DeleteCriticalSection(&s_as.cs);
    s_as.state = 0;                 /* a re-created device reads the policy again */
}

/* ---- vtable: pipeline_async / pipeline_prewarm ------------------------------------ */

static int d3e_pipeline_async(void* user)
{
    (void)user;
    return s_d.up && d3e_async_on();
}

static void d3e_pipeline_prewarm(void* user, u32 pipeline, u32 topology_mask)
{
    (void)user;
    if (s_as.state <= 0 || !pipeline || pipeline > s_pipe_cap || !topology_mask) return;
    d3e_pipe* P = &s_pipe[pipeline - 1u];
    if (!P->used || P->failed) return;
    if (P->pending) P->prewarm |= topology_mask;
    else d3e_async_prewarm_job(P, pipeline - 1u, topology_mask);
}
