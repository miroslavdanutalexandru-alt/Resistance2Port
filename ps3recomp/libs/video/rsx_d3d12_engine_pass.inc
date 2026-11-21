/* rsx_d3d12_engine_pass.inc -- part of rsx_d3d12_engine.c (included there).
 *
 * Shader compilation (FXC, with an in-memory and an on-disk DXBC cache), the
 * internal full-screen passes (blits, present with display gamma, depth
 * resolve and copy, masked clears), the scratch depth target, GPU readbacks
 * and the swap chain. */

static u32 s_draws, s_last_draws;   /* guest draws this frame / last frame */

/* ---- shader compilation ---------------------------------------------------- */

typedef struct d3e_blob_ent { u64 key; ID3DBlob* b; } d3e_blob_ent;
#define D3E_BLOB_CACHE 8192u
static d3e_blob_ent s_blobs[D3E_BLOB_CACHE];
static u32 s_blob_n;
static SRWLOCK s_blob_lock = SRWLOCK_INIT;   /* the compiler pool shares the cache */
static char s_exe_dir[MAX_PATH + 2];

static const char* d3e_exe_dir(void)
{
    if (!s_exe_dir[0]) {
        char p[MAX_PATH + 2];
        const DWORD n = GetModuleFileNameA(NULL, p, MAX_PATH);
        if (n && n < MAX_PATH) {
            p[n] = 0;
            char* s = strrchr(p, '\\');
            if (s) { s[1] = 0; memcpy(s_exe_dir, p, strlen(p) + 1u); }
        }
        if (!s_exe_dir[0]) { s_exe_dir[0] = '.'; s_exe_dir[1] = '\\'; s_exe_dir[2] = 0; }
    }
    return s_exe_dir;
}

static void d3e_fail_dump(const char* src, size_t len, const char* target, u64 key, const char* err)
{
    const char* dir = getenv("RSX_D3D12_FAIL_DUMP");
    if (!dir || !*dir) return;
    CreateDirectoryA(dir, NULL);
    char path[MAX_PATH * 2];
    snprintf(path, sizeof path, "%s\\fail_%016llX_%s.hlsl", dir, (unsigned long long)key, target);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(src, 1, len, f);
    if (err) fprintf(f, "\n/* FXC:\n%s\n*/\n", err);
    fclose(f);
}

/* Compile (or fetch) `src` for `target`. The caller owns one reference. */
static ID3DBlob* d3e_compile(const char* src, const char* entry, const char* target, const char* name)
{
    if (!src) return NULL;
    const size_t len = strlen(src);
    u64 key = d3e_hash(src, len, 0);
    key = d3e_hash(entry, strlen(entry), key);
    key = d3e_hash(target, strlen(target), key);
    if (!key) key = 1;
    const u32 h0 = (u32)(key % D3E_BLOB_CACHE);
    d3e_exe_dir();
    AcquireSRWLockShared(&s_blob_lock);
    for (u32 p = 0; p < D3E_BLOB_CACHE; p++) {
        d3e_blob_ent* e = &s_blobs[(h0 + p) % D3E_BLOB_CACHE];
        if (!e->b) break;
        if (e->key == key) {
            ID3D10Blob_AddRef(e->b);
            ID3DBlob* r = e->b;
            ReleaseSRWLockShared(&s_blob_lock);
            return r;
        }
    }
    ReleaseSRWLockShared(&s_blob_lock);
    static int disk = -1;
    if (disk < 0) disk = d3e_env_on("RSX_SHADERCACHE", 1);
    char path[MAX_PATH * 2];
    ID3DBlob* b = NULL;
    if (disk) {
        snprintf(path, sizeof path, "%sshadercache\\d3d12\\%016llX.dxbc", d3e_exe_dir(), (unsigned long long)key);
        FILE* f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            const long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 32 && n < (64l << 20) && SUCCEEDED(D3DCreateBlob((SIZE_T)n, &b))) {
                u8* d = (u8*)ID3D10Blob_GetBufferPointer(b);
                if (fread(d, 1, (size_t)n, f) != (size_t)n || memcmp(d, "DXBC", 4)) {
                    ID3D10Blob_Release(b);
                    b = NULL;
                }
            }
            fclose(f);
        }
    }
    if (!b) {
        ID3DBlob* err = NULL;
        HRESULT hr = D3DCompile(src, len, name, NULL, NULL, entry, target, 0, 0, &b, &err);
        if (FAILED(hr) || !b) {
            const char* et = err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "?";
            D3E_LOG("FXC %s (%s) failed 0x%08lX: %.1200s\n", name, target, (unsigned long)hr, et);
            d3e_fail_dump(src, len, target, key, err ? et : NULL);
            if (err) ID3D10Blob_Release(err);
            if (b) ID3D10Blob_Release(b);
            return NULL;
        }
        if (err) ID3D10Blob_Release(err);
        if (disk) {
            char dir[MAX_PATH * 2];
            snprintf(dir, sizeof dir, "%sshadercache", d3e_exe_dir());
            CreateDirectoryA(dir, NULL);
            snprintf(dir, sizeof dir, "%sshadercache\\d3d12", d3e_exe_dir());
            CreateDirectoryA(dir, NULL);
            char tmp[MAX_PATH * 2 + 8];
            snprintf(tmp, sizeof tmp, "%s.%lu.%lu.tmp", path, (unsigned long)GetCurrentProcessId(),
                     (unsigned long)GetCurrentThreadId());
            FILE* f = fopen(tmp, "wb");
            if (f) {
                const size_t n = ID3D10Blob_GetBufferSize(b);
                const int ok = fwrite(ID3D10Blob_GetBufferPointer(b), 1, n, f) == n;
                fclose(f);
                if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp);
            }
        }
    }
    AcquireSRWLockExclusive(&s_blob_lock);
    if (s_blob_n < D3E_BLOB_CACHE * 3u / 4u) {
        for (u32 p = 0; p < D3E_BLOB_CACHE; p++) {
            d3e_blob_ent* e = &s_blobs[(h0 + p) % D3E_BLOB_CACHE];
            if (e->b && e->key == key) break;     /* another thread got there */
            if (e->b) continue;
            e->key = key; e->b = b;
            ID3D10Blob_AddRef(b);
            s_blob_n++;
            break;
        }
    }
    ReleaseSRWLockExclusive(&s_blob_lock);
    return b;
}

static void d3e_blob_cache_clear(void)
{
    AcquireSRWLockExclusive(&s_blob_lock);
    for (u32 i = 0; i < D3E_BLOB_CACHE; i++)
        if (s_blobs[i].b) ID3D10Blob_Release(s_blobs[i].b);
    memset(s_blobs, 0, sizeof s_blobs);
    s_blob_n = 0;
    ReleaseSRWLockExclusive(&s_blob_lock);
}

/* ---- internal passes --------------------------------------------------------
 * One full-screen triangle; the 16 root constants are
 *   c[0..3]   uvr: source uv origin and extent
 *   c[4..7]   p1:  present (gamma on, gamma), stencil copy (source w, h)
 *   c[8..11]  p2:  clear colour
 *   c[12..15] p3:  x = output z (clears) */

static const char s_pass_hlsl[] =
    "cbuffer P : register(b0) { float4 uvr; float4 p1; float4 p2; float4 p3; };\n"
    "Texture2D<float4> t0 : register(t0);\n"
    "Texture2D<uint2> ts : register(t1);\n"
    "SamplerState sp : register(s0);\n"
    "SamplerState sl : register(s1);\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) {\n"
    "  V o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "  o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), p3.x, 1.0);\n"
    "  o.uv = uvr.xy + t * uvr.zw; return o; }\n"
    "float4 blit_point(V i) : SV_Target { return t0.SampleLevel(sp, i.uv, 0); }\n"
    "float4 blit_linear(V i) : SV_Target { return t0.SampleLevel(sl, i.uv, 0); }\n"
    "float4 gamma(float4 c) { if (p1.x > 0.5) c.rgb = pow(max(c.rgb, 0.0), p1.y); c.a = 1.0; return c; }\n"
    "float4 present_point(V i) : SV_Target { return gamma(t0.SampleLevel(sp, i.uv, 0)); }\n"
    "float4 present_linear(V i) : SV_Target { return gamma(t0.SampleLevel(sl, i.uv, 0)); }\n"
    "float depth_r32f(V i) : SV_Target { return t0.SampleLevel(sp, i.uv, 0).r; }\n"
    "float depth_copy(V i) : SV_Depth { return t0.SampleLevel(sp, i.uv, 0).r; }\n"
    "struct DS { float d : SV_Depth; uint s : SV_StencilRef; };\n"
    "DS depth_stencil_copy(V i) { DS o; o.d = t0.SampleLevel(sp, i.uv, 0).r;\n"
    "  int2 p = int2(clamp(i.uv * p1.zw, float2(0.0, 0.0), p1.zw - 1.0));\n"
    "  o.s = ts.Load(int3(p, 0)).g; return o; }\n"
    "float4 clear_color(V i) : SV_Target { return p2; }\n"
    "float4 osd(V i) : SV_Target { float4 c = t0.SampleLevel(sl, i.uv, 0); c.a *= p1.x; return c; }\n";

enum {
    D3E_PS_BLIT_POINT = 0, D3E_PS_BLIT_LINEAR, D3E_PS_PRESENT_POINT, D3E_PS_PRESENT_LINEAR,
    D3E_PS_DEPTH_R32F, D3E_PS_DEPTH_COPY, D3E_PS_DEPTH_STENCIL_COPY, D3E_PS_CLEAR, D3E_PS_OSD, D3E_PS_COUNT
};
static const char* const s_pass_entry[D3E_PS_COUNT] = {
    "blit_point", "blit_linear", "present_point", "present_linear",
    "depth_r32f", "depth_copy", "depth_stencil_copy", "clear_color", "osd"
};
_Static_assert(D3E_PS_COUNT <= sizeof s_d.pass_ps / sizeof s_d.pass_ps[0], "s_d.pass_ps too small");

/* Pass PSO mode bits. */
#define D3E_PM_WRITE_MASK   0x0Fu      /* D3D12_COLOR_WRITE_ENABLE_* */
#define D3E_PM_DEPTH        0x10u      /* depth test ALWAYS, write     */
#define D3E_PM_STENCIL      0x20u      /* stencil ALWAYS, REPLACE      */
#define D3E_PM_BLEND        0x40u      /* straight-alpha "over" blend  */
#define D3E_PM_SMASK_SHIFT  8u         /* stencil write mask [8:15]    */

static HRESULT d3e_serialize_rs(const D3D12_ROOT_SIGNATURE_DESC* d, ID3D12RootSignature** out)
{
    ID3DBlob* sig = NULL;
    ID3DBlob* err = NULL;
    HRESULT hr = D3D12SerializeRootSignature(d, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) {
        D3E_LOG("root signature: %s\n", err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return hr;
    }
    if (err) ID3D10Blob_Release(err);
    hr = ID3D12Device_CreateRootSignature(s_d.dev, 0, ID3D10Blob_GetBufferPointer(sig), ID3D10Blob_GetBufferSize(sig),
                                          &IID_ID3D12RootSignature, (void**)out);
    ID3D10Blob_Release(sig);
    return hr;
}

static int d3e_pass_init(void)
{
    D3D12_DESCRIPTOR_RANGE r = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
    D3D12_ROOT_PARAMETER p[2];
    memset(p, 0, sizeof p);
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.ShaderRegister = 0;
    p[0].Constants.Num32BitValues = 16;
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges = &r;
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC ss[2];
    memset(ss, 0, sizeof ss);
    for (u32 i = 0; i < 2; i++) {
        ss[i].Filter = i ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        ss[i].AddressU = ss[i].AddressV = ss[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss[i].MaxAnisotropy = 1;
        ss[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        ss[i].MaxLOD = D3D12_FLOAT32_MAX;
        ss[i].ShaderRegister = i;
        ss[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC d = { 2, p, 2, ss, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    if (FAILED(d3e_serialize_rs(&d, &s_d.rs_pass))) return -1;
    s_d.pass_vs = d3e_compile(s_pass_hlsl, "vs", "vs_5_0", "rsx_d3d12_pass");
    if (!s_d.pass_vs) return -1;
    for (u32 i = 0; i < D3E_PS_COUNT; i++) {
        /* SV_StencilRef needs shader model 5.1; the rest is 5.0. Only the
         * stencil copy may fail (it is optional). */
        s_d.pass_ps[i] = d3e_compile(s_pass_hlsl, s_pass_entry[i],
                                     i == D3E_PS_DEPTH_STENCIL_COPY ? "ps_5_1" : "ps_5_0", "rsx_d3d12_pass");
        if (!s_d.pass_ps[i] && i != D3E_PS_DEPTH_STENCIL_COPY) return -1;
    }
    return 0;
}

typedef struct d3e_pass_pso { u64 key; ID3D12PipelineState* pso; } d3e_pass_pso;
#define D3E_PASS_PSOS 128u
static d3e_pass_pso s_ppso[D3E_PASS_PSOS];
static u32 s_ppso_n;

static ID3D12PipelineState* d3e_pass_pso_get(u32 ps, DXGI_FORMAT rtf, DXGI_FORMAT dsf, u32 mode)
{
    const u64 key = (u64)ps | ((u64)rtf << 8) | ((u64)dsf << 20) | ((u64)mode << 32);
    for (u32 i = 0; i < s_ppso_n; i++) if (s_ppso[i].key == key) return s_ppso[i].pso;
    if (ps >= D3E_PS_COUNT || !s_d.pass_ps[ps] || !s_d.pass_vs) return NULL;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = s_d.rs_pass;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(s_d.pass_vs);
    pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(s_d.pass_vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(s_d.pass_ps[ps]);
    pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(s_d.pass_ps[ps]);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = (UINT8)(mode & D3E_PM_WRITE_MASK);
    pd.BlendState.RenderTarget[0].SrcBlend = pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlend = pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOp = pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    if (mode & D3E_PM_BLEND) {
        D3D12_RENDER_TARGET_BLEND_DESC* b = &pd.BlendState.RenderTarget[0];
        b->BlendEnable = TRUE;
        b->SrcBlend = D3D12_BLEND_SRC_ALPHA; b->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b->SrcBlendAlpha = D3D12_BLEND_ONE; b->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    }
    pd.SampleMask = 0xFFFFFFFFu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    if (dsf != DXGI_FORMAT_UNKNOWN) {
        D3D12_DEPTH_STENCIL_DESC* ds = &pd.DepthStencilState;
        if (mode & D3E_PM_DEPTH) {
            ds->DepthEnable = TRUE;
            ds->DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            ds->DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        }
        if (mode & D3E_PM_STENCIL) {
            ds->StencilEnable = TRUE;
            ds->StencilReadMask = 0xFF;
            ds->StencilWriteMask = (UINT8)(mode >> D3E_PM_SMASK_SHIFT);
            D3D12_DEPTH_STENCILOP_DESC f = { D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_REPLACE,
                                             D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS };
            ds->FrontFace = f;
            ds->BackFace = f;
        }
    }
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = rtf != DXGI_FORMAT_UNKNOWN ? 1u : 0u;
    pd.RTVFormats[0] = rtf;
    pd.DSVFormat = dsf;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(s_d.dev, &pd, &IID_ID3D12PipelineState, (void**)&pso);
    if (FAILED(hr)) {
        D3E_LOG("pass PSO %s rt %d ds %d mode 0x%X failed 0x%08lX\n", s_pass_entry[ps], (int)rtf, (int)dsf,
                mode, (unsigned long)hr);
        return NULL;
    }
    if (s_ppso_n == D3E_PASS_PSOS) {     /* full: start over (rare: a handful of formats) */
        for (u32 i = 0; i < s_ppso_n; i++) d3e_defer_release(s_ppso[i].pso);
        s_ppso_n = 0;
    }
    s_ppso[s_ppso_n].key = key;
    s_ppso[s_ppso_n].pso = pso;
    s_ppso_n++;
    return pso;
}

typedef struct d3e_pass_args {
    u32 ps, mode;
    DXGI_FORMAT rtf, dsf;                    /* UNKNOWN: none */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv;
    u32 srv0, srv1;                          /* CPU heap indices; 0xFFFFFFFF: none */
    D3D12_CPU_DESCRIPTOR_HANDLE srv0_cpu;    /* non-zero: t0 from this CPU descriptor instead of srv0 */
    float c[16];
    D3D12_VIEWPORT vp;
    D3D12_RECT sc;
    u32 stencil_ref;
} d3e_pass_args;

static void d3e_pass_args_init(d3e_pass_args* a, u32 ps)
{
    memset(a, 0, sizeof *a);
    a->ps = ps;
    a->mode = D3D12_COLOR_WRITE_ENABLE_ALL;
    a->rtf = a->dsf = DXGI_FORMAT_UNKNOWN;
    a->srv0 = a->srv1 = 0xFFFFFFFFu;
    a->c[2] = a->c[3] = 1.0f;
}

static void d3e_pass_target(d3e_pass_args* a, u32 x, u32 y, u32 w, u32 h)
{
    a->vp.TopLeftX = (float)x; a->vp.TopLeftY = (float)y;
    a->vp.Width = (float)w; a->vp.Height = (float)h;
    a->vp.MinDepth = 0.0f; a->vp.MaxDepth = 1.0f;
    a->sc.left = (LONG)x; a->sc.top = (LONG)y;
    a->sc.right = (LONG)(x + w); a->sc.bottom = (LONG)(y + h);
}

/* Record one internal pass. Occlusion queries do not count it. */
static int d3e_pass_run(const d3e_pass_args* a)
{
    ID3D12PipelineState* pso = d3e_pass_pso_get(a->ps, a->rtf, a->dsf, a->mode);
    if (!pso) return 0;
    d3e_reserve(0, 2);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    d3e_desc_alloc(2, &cpu, &gpu);
    D3D12_CPU_DESCRIPTOR_HANDLE c1 = cpu;
    c1.ptr += s_d.srv_inc;
    ID3D12Device_CopyDescriptorsSimple(s_d.dev, 1, cpu,
                                       a->srv0_cpu.ptr ? a->srv0_cpu
                                                       : d3e_srv_cpu(a->srv0 != 0xFFFFFFFFu ? a->srv0 : D3E_SRV_NULL + 0),
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12Device_CopyDescriptorsSimple(s_d.dev, 1, c1, d3e_srv_cpu(a->srv1 != 0xFFFFFFFFu ? a->srv1 : D3E_SRV_NULL + 3),
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    d3e_query_close_open();
    ID3D12GraphicsCommandList* cl = s_d.cl;
    if (s_d.cur_rs != s_d.rs_pass) {
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, s_d.rs_pass);
        s_d.cur_rs = s_d.rs_pass;
    }
    ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
    s_d.cur_pso = pso;
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 16, a->c, 0);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, gpu);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, a->rtf != DXGI_FORMAT_UNKNOWN ? 1u : 0u,
                                                 a->rtf != DXGI_FORMAT_UNKNOWN ? &a->rtv : NULL, FALSE,
                                                 a->dsf != DXGI_FORMAT_UNKNOWN ? &a->dsv : NULL);
    s_d.om_valid = 0;
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &a->vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &a->sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_OMSetStencilRef(cl, a->stencil_ref);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
    d3e_query_reopen();
    return 1;
}

/* ---- scratch depth ----------------------------------------------------------
 * A draw whose zeta is missing or smaller than its colour target still needs
 * a depth attachment of the colour target's size (as the Vulkan backend's). */
static int d3e_scratch_depth(u32 w, u32 h)
{
    if (s_d.scratch && s_d.scratch_w >= w && s_d.scratch_h >= h) return 1;
    const u32 nw = w > s_d.scratch_w ? w : s_d.scratch_w, nh = h > s_d.scratch_h ? h : s_d.scratch_h;
    if (s_d.scratch) { d3e_defer_release(s_d.scratch); s_d.scratch = NULL; }
    s_d.scratch_w = s_d.scratch_h = 0;
    D3D12_CLEAR_VALUE cv;
    memset(&cv, 0, sizeof cv);
    cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    cv.DepthStencil.Depth = 1.0f;
    cv.DepthStencil.Stencil = 0xFF;
    s_d.scratch = d3e_texture_resource(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_D24_UNORM_S8_UINT, nw, nh, 1, 1,
                                       D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE,
                                       D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv);
    if (!s_d.scratch) return 0;
    s_d.scratch_st = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    s_d.scratch_w = nw; s_d.scratch_h = nh;
    ID3D12Device_CreateDepthStencilView(s_d.dev, s_d.scratch, NULL, d3e_dsv_cpu(D3E_DSV_SCRATCH));
    ID3D12GraphicsCommandList_ClearDepthStencilView(s_d.cl, d3e_dsv_cpu(D3E_DSV_SCRATCH),
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0xFF, 0, NULL);
    s_d.om_valid = 0;
    return 1;
}

/* ---- readback ---------------------------------------------------------------- */

static int d3e_rb_ensure(u64 bytes)
{
    if (s_d.rb && s_d.rb_size >= bytes) return 1;
    u64 n = s_d.rb_size ? s_d.rb_size : (4u << 20);
    while (n < bytes) n *= 2u;
    if (s_d.rb) { d3e_defer_release(s_d.rb); s_d.rb = NULL; s_d.rb_size = 0; }
    s_d.rb = d3e_buffer(n, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!s_d.rb) { D3E_LOG("readback buffer of %llu bytes failed\n", (unsigned long long)n); return 0; }
    s_d.rb_size = n;
    return 1;
}

/* Copy rectangle (x, y, w, h) of subresource 0 of a colour or texture object
 * into `out` (bpp bytes per pixel). Submits and waits. */
static int d3e_read_rect(d3e_obj* o, u32 x, u32 y, u32 w, u32 h, void* out, u32 out_pitch)
{
    if (!o || !o->res || o->bc || !o->bpp || !w || !h || x >= o->w || y >= o->h ||
        w > o->w - x || h > o->h - y) return 0;
    const u32 tight = w * o->bpp;
    const u32 pitch = (tight + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    if (!d3e_rb_ensure((u64)pitch * h)) return 0;
    D3D12_TEXTURE_COPY_LOCATION dl, sl;
    memset(&dl, 0, sizeof dl); memset(&sl, 0, sizeof sl);
    dl.pResource = s_d.rb;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint.Footprint.Format = o->rfmt;
    dl.PlacedFootprint.Footprint.Width = w;
    dl.PlacedFootprint.Footprint.Height = h;
    dl.PlacedFootprint.Footprint.Depth = 1;
    dl.PlacedFootprint.Footprint.RowPitch = pitch;
    sl.pResource = o->res;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = { x, y, 0, x + w, y + h, 1 };
    d3e_trans(o, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyTextureRegion(s_d.cl, &dl, 0, 0, 0, &sl, &box);
    d3e_submit(1);
    void* p = NULL;
    D3D12_RANGE rr = { 0, (SIZE_T)pitch * h };
    if (FAILED(ID3D12Resource_Map(s_d.rb, 0, &rr, &p)) || !p) return 0;
    const u32 n = tight < out_pitch || !out_pitch ? tight : out_pitch;
    for (u32 r = 0; r < h; r++)
        memcpy((u8*)out + (size_t)r * (out_pitch ? out_pitch : tight), (const u8*)p + (size_t)r * pitch, n);
    D3D12_RANGE none = { 0, 0 };
    ID3D12Resource_Unmap(s_d.rb, 0, &none);
    return 1;
}

/* Copy a whole plane of a depth target (depth-stencil copies cannot take a
 * box) and map it. *row_pitch / *texel get the footprint's row pitch and the
 * bytes per texel; the caller unmaps s_d.rb. */
static const u8* d3e_read_depth_plane(d3e_obj* o, u32 plane, u32* row_pitch, u32* texel)
{
    if (!o || !o->res) return NULL;
    D3D12_RESOURCE_DESC rd;
    ID3D12Resource_GetDesc(o->res, &rd);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows = 0;
    UINT64 row_size = 0, total = 0;
    ID3D12Device_GetCopyableFootprints(s_d.dev, &rd, plane, 1, 0, &fp, &rows, &row_size, &total);
    if (!total || !fp.Footprint.Width) return NULL;
    if (!d3e_rb_ensure(total)) return NULL;
    D3D12_TEXTURE_COPY_LOCATION dl, sl;
    memset(&dl, 0, sizeof dl); memset(&sl, 0, sizeof sl);
    dl.pResource = s_d.rb;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint = fp;
    sl.pResource = o->res;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.SubresourceIndex = plane;
    d3e_trans(o, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyTextureRegion(s_d.cl, &dl, 0, 0, 0, &sl, NULL);
    d3e_submit(1);
    void* p = NULL;
    D3D12_RANGE rr = { 0, (SIZE_T)total };
    if (FAILED(ID3D12Resource_Map(s_d.rb, 0, &rr, &p)) || !p) return NULL;
    *row_pitch = fp.Footprint.RowPitch;
    *texel = (u32)(row_size / fp.Footprint.Width);
    return (const u8*)p;
}

static void d3e_rb_unmap(void)
{
    D3D12_RANGE none = { 0, 0 };
    ID3D12Resource_Unmap(s_d.rb, 0, &none);
}

/* ---- swap chain and present --------------------------------------------------- */

static void d3e_swap_release_buffers(void)
{
    for (u32 i = 0; i < D3E_SWAP_BUFFERS; i++) {
        if (s_d.back[i]) { ID3D12Resource_Release(s_d.back[i]); s_d.back[i] = NULL; }
    }
}

static int d3e_swap_get_buffers(void)
{
    for (u32 i = 0; i < D3E_SWAP_BUFFERS; i++) {
        if (FAILED(IDXGISwapChain3_GetBuffer(s_d.swap, i, &IID_ID3D12Resource, (void**)&s_d.back[i]))) return 0;
        ID3D12Device_CreateRenderTargetView(s_d.dev, s_d.back[i], NULL, d3e_rtv_cpu(D3E_RTV_SWAP + i));
        s_d.back_st[i] = D3D12_RESOURCE_STATE_PRESENT;
    }
    return 1;
}

/* The swap chain at the window's client size; 0 when there is nothing to
 * present into (no window, minimised, or creation failed). */
static int d3e_swap_ensure(void)
{
    static int broken;
    if (!s_d.hwnd || broken || !IsWindow(s_d.hwnd)) return 0;
    RECT rc;
    if (!GetClientRect(s_d.hwnd, &rc)) return 0;
    const u32 w = (u32)(rc.right - rc.left), h = (u32)(rc.bottom - rc.top);
    if (!w || !h) return 0;
    const UINT flags = s_d.tearing_ok ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    if (!s_d.swap) {
        DXGI_SWAP_CHAIN_DESC1 sd;
        memset(&sd, 0, sizeof sd);
        sd.Width = w; sd.Height = h;
        sd.Format = d3e_hdr_swap_format();
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = D3E_SWAP_BUFFERS;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        sd.Flags = flags;
        IDXGISwapChain1* sc1 = NULL;
        HRESULT hr = d3e_fg_swapchain(&sd, &sc1) ? S_OK
                   : IDXGIFactory4_CreateSwapChainForHwnd(s_d.factory, (IUnknown*)s_d.queue, s_d.hwnd, &sd,
                                                          NULL, NULL, &sc1);
        if (FAILED(hr) || !sc1) {
            D3E_LOG("CreateSwapChainForHwnd failed 0x%08lX: presenting nothing\n", (unsigned long)hr);
            broken = 1;
            return 0;
        }
        hr = IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void**)&s_d.swap);
        IDXGISwapChain1_Release(sc1);
        if (FAILED(hr)) { s_d.swap = NULL; d3e_fg_swap_down(); broken = 1; return 0; }
        IDXGIFactory4_MakeWindowAssociation(s_d.factory, s_d.hwnd, DXGI_MWA_NO_ALT_ENTER);
        if (!d3e_hdr_apply() || !d3e_swap_get_buffers()) {
            d3e_swap_release_buffers();
            d3e_fg_swap_down();
            IDXGISwapChain3_Release(s_d.swap);
            s_d.swap = NULL;
            if (sd.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d3e_hdr_swap_format() == DXGI_FORMAT_R8G8B8A8_UNORM)
                return 0;                        /* HDR refused: an SDR swap chain next frame */
            broken = 1;
            return 0;
        }
        s_d.swap_w = w; s_d.swap_h = h;
        D3E_LOG("swap chain %ux%u, %u buffers%s\n", w, h, D3E_SWAP_BUFFERS, flags ? ", tearing allowed" : "");
        return 1;
    }
    if (w != s_d.swap_w || h != s_d.swap_h) {
        d3e_wait_fence(s_d.fence_val);           /* every frame using the buffers is done */
        d3e_swap_release_buffers();
        d3e_fg_before_resize();
        HRESULT hr = IDXGISwapChain3_ResizeBuffers(s_d.swap, D3E_SWAP_BUFFERS, w, h, DXGI_FORMAT_UNKNOWN, flags);
        if (SUCCEEDED(hr) && !d3e_hdr_apply()) {
            d3e_swap_release_buffers();
            d3e_fg_swap_down();
            IDXGISwapChain3_Release(s_d.swap);
            s_d.swap = NULL;                     /* HDR refused: an SDR swap chain next frame */
            return 0;
        }
        if (FAILED(hr) || !d3e_swap_get_buffers()) {
            D3E_LOG("ResizeBuffers %ux%u failed 0x%08lX\n", w, h, (unsigned long)hr);
            d3e_swap_release_buffers();
            d3e_fg_swap_down();
            IDXGISwapChain3_Release(s_d.swap);
            s_d.swap = NULL;
            broken = 1;
            return 0;
        }
        s_d.swap_w = w; s_d.swap_h = h;
    }
    return 1;
}

/* RSX_FILTER / PS3RECOMP_VK_OUTPUT_SCALING, as the Vulkan backend reads them:
 * 0 point|nearest, 2 fsr* (FSR1 when upscaling, rsx_d3d12_engine_aa.inc), 1 linear. */
static int d3e_present_linear(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char* e = getenv("PS3RECOMP_VK_OUTPUT_SCALING");
        if (!e || !*e) e = getenv("RSX_FILTER");
        mode = 1;
        if (e && (rsx_vk_ieq(e, "point") || rsx_vk_ieq(e, "nearest"))) mode = 0;
        else if (e && (e[0] == 'f' || e[0] == 'F') && (e[1] == 's' || e[1] == 'S') && (e[2] == 'r' || e[2] == 'R'))
            mode = 2;
    }
    return mode;
}

static int d3e_gamma(float* g)
{
    static int legacy = -1;
    if (legacy < 0) {
        const char* e = getenv("RSX_CFG_LEGACY");
        legacy = e && e[0] && e[0] != '0';
    }
    *g = cellAvconfExt_display_gamma();
    if (legacy || !(*g == *g) || *g <= 0.0f) return 0;
    return rsx_vk_present_calibrate(*g, 1);
}

/* RSX_D3D12_DUMP=<prefix>: every RSX_D3D12_DUMP_EVERY-th presented region
 * (an R,G,B,A target) as <prefix>NNNNNN.ppm. */
static void d3e_dump(d3e_obj* o, u32 sx, u32 sy, u32 sw, u32 sh)
{
    static const char* prefix;
    static int every = -1;
    if (every < 0) {
        prefix = getenv("RSX_D3D12_DUMP");
        const char* e = getenv("RSX_D3D12_DUMP_EVERY");
        every = e && atoi(e) > 0 ? atoi(e) : 60;
    }
    if (!prefix || !*prefix || !o || o->fmt != RSX_BE_FMT_R8G8B8A8) return;
    if (s_d.presents % (u32)every) return;
    u8* px = (u8*)malloc((size_t)sw * sh * 4u);
    if (!px) return;
    if (d3e_read_rect(o, sx, sy, sw, sh, px, sw * 4u)) {
        const u8* c = px + ((size_t)(sh / 2u) * sw + sw / 2u) * 4u;
        s_d.last_center = (u32)c[0] << 16 | (u32)c[1] << 8 | c[2];
        char path[MAX_PATH * 2];
        snprintf(path, sizeof path, "%s%06u.ppm", prefix, s_d.presents);
        FILE* f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", sw, sh);
            for (size_t i = 0; i < (size_t)sw * sh; i++) fwrite(px + i * 4u, 1, 3, f);
            fclose(f);
            D3E_LOG("dumped %s (centre %06X)\n", path, s_d.last_center);
        }
    }
    free(px);
}

/* Present rectangle (sx, sy, sw, sh) of colour target `surface`, letterboxed
 * to aw:ah, then move to the next frame. A missing surface keeps the last
 * frame on screen (the Vulkan backend's rule). */
static void d3e_present_region(u32 surface, u32 sx, u32 sy, u32 sw, u32 sh, u32 aw, u32 ah)
{
    if (!s_d.up) return;
    d3e_osd_note_present(surface, sx, sy, sw, sh, aw, ah);
    d3e_async_frame();
    d3e_obj* o = d3e_get(surface, D3E_COLOR);
    u32 rx = 0, ry = 0, rw = 0, rh = 0;
    if (o) {
        rx = sx < o->w ? sx : 0u;
        ry = sy < o->h ? sy : 0u;
        rw = sw && rx + sw <= o->w ? sw : o->w - rx;
        rh = sh && ry + sh <= o->h ? sh : o->h - ry;
        d3e_dump(o, rx, ry, rw, rh);
    }
    int presented = 0;
    u32 fl_x = 0, fl_y = 0, fl_w = 0, fl_h = 0;
    if (o && d3e_swap_ensure()) {
        const u32 bi = IDXGISwapChain3_GetCurrentBackBufferIndex(s_d.swap) % D3E_SWAP_BUFFERS;
        d3e_barrier_res(s_d.back[bi], &s_d.back_st[bi], D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE prtv = d3e_rtv_cpu(D3E_RTV_SWAP + bi);
        DXGI_FORMAT prtf = d3e_hdr_back_format();
        const int hdr = d3e_hdr_target(&prtv, &prtf);   /* HDR: present into the SDR image first */
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        ID3D12GraphicsCommandList_ClearRenderTargetView(s_d.cl, prtv, black, 0, NULL);
        u32 lx, ly, lw, lh;
        rsx_d3d12_letterbox(s_d.swap_w, s_d.swap_h, aw, ah, &lx, &ly, &lw, &lh);
        const int linear = (rw == lw && rh == lh) ? 0 : d3e_present_linear();
        float g = 1.0f;
        const int gm = d3e_gamma(&g);
        d3e_trans(o, d3e_srv_state(o));
        const int f1 = d3e_fsr1_present(o, surface, rx, ry, rw, rh, prtv, prtf, lx, ly, lw, lh, gm, g);
        if (!f1) {
        d3e_pass_args a;
        d3e_pass_args_init(&a, linear ? D3E_PS_PRESENT_LINEAR : D3E_PS_PRESENT_POINT);
        a.rtf = prtf;
        a.rtv = prtv;
        a.srv0 = surface;
        a.c[0] = (float)rx / (float)o->w; a.c[1] = (float)ry / (float)o->h;
        a.c[2] = (float)rw / (float)o->w; a.c[3] = (float)rh / (float)o->h;
        a.c[4] = gm ? 1.0f : 0.0f; a.c[5] = g;
        d3e_pass_target(&a, lx, ly, lw, lh);
        d3e_pass_run(&a);
        }
        d3e_fg_hudless(surface, rx, ry, rw, rh, lx, ly, lw, lh, linear, gm, g, f1);
        d3e_cap_present(hdr, bi, lx, ly, lw, lh);
        d3e_osd_draw(prtv, prtf);
        fl_x = lx; fl_y = ly; fl_w = lw; fl_h = lh;
        if (hdr) d3e_hdr_resolve(d3e_rtv_cpu(D3E_RTV_SWAP + bi));
        d3e_barrier_res(s_d.back[bi], &s_d.back_st[bi], D3D12_RESOURCE_STATE_PRESENT);
        presented = 1;
    }
    d3e_frame_end();
    d3e_cap_finish();
    d3e_fg_present(presented, fl_x, fl_y, fl_w, fl_h);
    if (presented) {
        /* RSX_VSYNC is settled at boot (stubs.cpp r2_fps_target) before the first present:
         * read it once instead of a CRT getenv (environment lock + scan) every frame. */
        static int s_vs = -1;
        if (s_vs < 0) s_vs = rsx_vk_vsync_mode(getenv("RSX_VSYNC"));
        const int vs = s_vs;
        const UINT interval = vs == RSX_VK_VSYNC_OFF ? 0u : 1u;
        const UINT pf = (vs == RSX_VK_VSYNC_OFF && s_d.tearing_ok) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
        d3e_lat_render(1);
        const HRESULT hr = IDXGISwapChain3_Present(s_d.swap, interval, pf);
        d3e_lat_render(2);
        if (FAILED(hr)) {
            const HRESULT why = ID3D12Device_GetDeviceRemovedReason(s_d.dev);
            D3E_WARN_ONCE(7, "Present failed 0x%08lX (device removed reason 0x%08lX)\n",
                          (unsigned long)hr, (unsigned long)why);
        }
    }
    d3e_frame_begin_next();
    s_d.presents++;
    s_last_draws = s_draws;
    s_draws = 0;
}
