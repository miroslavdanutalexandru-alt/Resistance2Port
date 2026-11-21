/* rsx_d3d12_engine_msaa.inc -- part of rsx_d3d12_engine.c (included there).
 *
 * Host MSAA, RSX_D3D12_HW_MSAA=1 (default off, unverified in game): RPCS3's
 * "MSAA: Auto" on real multisampled twins, the Vulkan backend's "ems" design
 * (rsx_vulkan_backend.c). The engine keeps drawing an antialiased surface
 * widened by aa_sx x aa_sy; target_set_aa gives such a colour / depth target
 * a twin of the pixel size with aa_sx * aa_sy samples. Draws whose targets all
 * have matching twins rasterise into the twins (viewport and scissor divided
 * by the factors); every read of the widened image (sampling, snapshots,
 * blits, readbacks, present, the upscalers) first resolves the twin into it,
 * sample s of twin pixel q going to widened pixel q * (sx, sy) + (s % sx, s / sx)
 * (rsx_d3d12_ms_sample). Writes outside draws land in the widened image and
 * mark it newer; the next multisampled draw "unresolves" it into the twin.
 * Twin states: 0 in step, 1 twin newer, 2 widened image newer.
 *
 * With host MSAA the engine leaves alpha-to-coverage to the hardware on every
 * pipeline (eng_hw_aa_caps): SET_ANTI_ALIASING_CONTROL bit 4 is applied in
 * each PSO, its sample mask (bits 16..31) on multisampled ones. Alpha-to-one
 * has no D3D12 state and stays in the shader. Unlike the Vulkan path (forced
 * per-sample shading) draws shade per pixel. Not with render scale < 1. */

#define D3E_MS_MAX 256u

typedef struct d3e_twin {
    ID3D12Resource* res;
    D3D12_RESOURCE_STATES st;
    u32 owner;              /* engine handle; 0: free */
    u32 sx, sy, samples, pw, ph;
    u32 state;              /* 0 in step, 1 twin newer, 2 widened image newer */
    u8  depth, has_stencil, zcls;
    DXGI_FORMAT rfmt;
} d3e_twin;

enum { D3E_MS_RC = 0, D3E_MS_RD, D3E_MS_RS, D3E_MS_UC, D3E_MS_UD, D3E_MS_US, D3E_MS_OPS };

static int s_ms_on;         /* set by d3e_ms_setup before any pipeline exists */
static struct {
    d3e_twin tw[D3E_MS_MAX];
    u16 slot[D3E_MAX_OBJ];  /* engine handle -> twin index + 1 */
    ID3D12DescriptorHeap* rtv;
    ID3D12DescriptorHeap* dsv;
    ID3D12DescriptorHeap* srv;       /* CPU only: 2 per twin (colour / depth, stencil) */
    u32 rtv_inc, dsv_inc, srv_inc;
    int heaps_failed;
    ID3DBlob* ps[D3E_MS_OPS];
    struct { u64 key; ID3D12PipelineState* pso; } pso[96];
    u32 npso;
    u32 sx, sy;             /* the current multisampled draw */
    u32 warned;
    rsx_draw_backend inner;
} s_ms;

#define D3E_MS_WARN(bit, ...) do { if (!(s_ms.warned & (1u << (bit)))) { s_ms.warned |= 1u << (bit); D3E_LOG(__VA_ARGS__); } } while (0)

#define D3E_MS_HDR \
    "cbuffer P : register(b0) { float4 k; float4 k1; float4 k2; float4 k3; };\n" \
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n" \
    "uint2 msf() { return uint2(max(k.xy, float2(1.0, 1.0))); }\n"

/* k.xy: sx, sy; k.z: the stencil bit (1 << n) of a stencil pass */
static const char* const s_ms_src[D3E_MS_OPS] = {
    /* resolve colour: widened pixel p <- sample of twin pixel p / (sx, sy) */
    D3E_MS_HDR
    "Texture2DMS<float4> t : register(t0);\n"
    "float4 main(V i) : SV_Target { uint2 s = msf(); uint2 p = uint2(i.pos.xy);\n"
    "  return t.Load(int2(p / s), (p.y % s.y) * s.x + p.x % s.x); }\n",
    /* resolve depth */
    D3E_MS_HDR
    "Texture2DMS<float4> t : register(t0);\n"
    "float main(V i) : SV_Depth { uint2 s = msf(); uint2 p = uint2(i.pos.xy);\n"
    "  return t.Load(int2(p / s), (p.y % s.y) * s.x + p.x % s.x).r; }\n",
    /* resolve one stencil bit (stencil REPLACE 0xFF through write mask k.z) */
    D3E_MS_HDR
    "Texture2DMS<uint2> ts : register(t1);\n"
    "void main(V i) { uint2 s = msf(); uint2 p = uint2(i.pos.xy);\n"
    "  uint v = ts.Load(int2(p / s), (p.y % s.y) * s.x + p.x % s.x).g;\n"
    "  if ((v & (uint)k.z) == 0u) discard; }\n",
    /* unresolve colour: per sample (SV_SampleIndex) */
    D3E_MS_HDR
    "Texture2D<float4> t : register(t0);\n"
    "float4 main(V i, uint si : SV_SampleIndex) : SV_Target { uint2 s = msf();\n"
    "  int2 q = int2(i.pos.xy) * int2(s) + int2(si % s.x, si / s.x); return t.Load(int3(q, 0)); }\n",
    /* unresolve depth */
    D3E_MS_HDR
    "Texture2D<float4> t : register(t0);\n"
    "float main(V i, uint si : SV_SampleIndex) : SV_Depth { uint2 s = msf();\n"
    "  int2 q = int2(i.pos.xy) * int2(s) + int2(si % s.x, si / s.x); return t.Load(int3(q, 0)).r; }\n",
    /* unresolve one stencil bit */
    D3E_MS_HDR
    "Texture2D<uint2> ts : register(t1);\n"
    "void main(V i, uint si : SV_SampleIndex) { uint2 s = msf();\n"
    "  int2 q = int2(i.pos.xy) * int2(s) + int2(si % s.x, si / s.x);\n"
    "  if ((ts.Load(int3(q, 0)).g & (uint)k.z) == 0u) discard; }\n",
};

static int d3e_ms_live(void) { return s_ms_on && !s_up.on && s_d.up; }

static d3e_twin* d3e_ms_twin(u32 h)
{
    if (!s_ms_on || !h || h >= D3E_MAX_OBJ || !s_ms.slot[h]) return NULL;
    d3e_twin* t = &s_ms.tw[s_ms.slot[h] - 1u];
    return t->owner == h && t->res ? t : NULL;
}

static D3D12_CPU_DESCRIPTOR_HANDLE d3e_ms_heap_cpu(ID3D12DescriptorHeap* hp, u32 inc, u32 i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(hp, &h);
    h.ptr += (SIZE_T)i * inc;
    return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE d3e_ms_rtv(const d3e_twin* t)
{ return d3e_ms_heap_cpu(s_ms.rtv, s_ms.rtv_inc, (u32)(t - s_ms.tw)); }
static D3D12_CPU_DESCRIPTOR_HANDLE d3e_ms_dsv(const d3e_twin* t)
{ return d3e_ms_heap_cpu(s_ms.dsv, s_ms.dsv_inc, (u32)(t - s_ms.tw)); }
static D3D12_CPU_DESCRIPTOR_HANDLE d3e_ms_srv(const d3e_twin* t, u32 stencil)
{ return d3e_ms_heap_cpu(s_ms.srv, s_ms.srv_inc, 2u * (u32)(t - s_ms.tw) + stencil); }

static int d3e_ms_heaps(void)
{
    if (s_ms.rtv && s_ms.dsv && s_ms.srv) return 1;
    if (s_ms.heaps_failed || !s_d.dev) return 0;
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    memset(&hd, 0, sizeof hd);
    hd.NumDescriptors = D3E_MS_MAX;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    HRESULT hr = ID3D12Device_CreateDescriptorHeap(s_d.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_ms.rtv);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateDescriptorHeap(s_d.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_ms.dsv);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2u * D3E_MS_MAX;
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateDescriptorHeap(s_d.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_ms.srv);
    if (FAILED(hr)) {
        D3E_LOG("host MSAA: descriptor heaps failed 0x%08lX; the widened emulation stays\n", (unsigned long)hr);
        s_ms.heaps_failed = 1;
        return 0;
    }
    s_ms.rtv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(s_d.dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    s_ms.dsv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(s_d.dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    s_ms.srv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(s_d.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return 1;
}

static void d3e_ms_forget(u32 h)
{
    if (!h || h >= D3E_MAX_OBJ || !s_ms.slot[h]) return;
    d3e_twin* t = &s_ms.tw[s_ms.slot[h] - 1u];
    s_ms.slot[h] = 0;
    if (t->owner != h) return;
    if (t->res) d3e_defer_release(t->res);
    memset(t, 0, sizeof *t);
    s_d.om_valid = 0;
}

/* 1 when `samples` multisampled targets of rt_or_ds load in a shader as srv. */
static int d3e_ms_fmt_ok(DXGI_FORMAT rt_or_ds, DXGI_FORMAT srv, u32 samples, int depth)
{
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q;
    memset(&q, 0, sizeof q);
    q.Format = rt_or_ds;
    q.SampleCount = samples;
    if (FAILED(ID3D12Device_CheckFeatureSupport(s_d.dev, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof q)) ||
        !q.NumQualityLevels) return 0;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs;
    memset(&fs, 0, sizeof fs);
    fs.Format = srv;
    if (FAILED(ID3D12Device_CheckFeatureSupport(s_d.dev, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs))) return 0;
    if (!(fs.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD)) return 0;
    /* float loads only: integer colour formats keep the emulation */
    if (!depth && !(fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE)) return 0;
    return 1;
}

static void d3e_ms_clear_twin(d3e_twin* t, D3D12_CLEAR_FLAGS f, const float* rgba, float z, u8 s)
{
    if (t->depth) {
        d3e_barrier_res(t->res, &t->st, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        if (!t->has_stencil) f &= ~D3D12_CLEAR_FLAG_STENCIL;
        if (f) ID3D12GraphicsCommandList_ClearDepthStencilView(s_d.cl, d3e_ms_dsv(t), f, d3e_clamp01(z), s, 0, NULL);
    } else {
        d3e_barrier_res(t->res, &t->st, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12GraphicsCommandList_ClearRenderTargetView(s_d.cl, d3e_ms_rtv(t), rgba, 0, NULL);
    }
}

/* ---- the twin of an antialiased target (backend target_set_aa) ----------------------- */

static u32 d3e_ms_target_set_aa(void* user, u32 h, u32 sx, u32 sy)
{
    (void)user;
    if (!d3e_ms_live()) return 0u;
    if (!h) return RSX_BE_AA_HW_MSAA;            /* the backend-wide capability */
    d3e_obj* o = d3e_get(h, 0);
    u32 samples = 0;
    if (!o || (o->kind != D3E_COLOR && o->kind != D3E_DEPTH) || !rsx_d3d12_ms_samples(sx, sy, &samples) ||
        (o->kind == D3E_COLOR && d3e_cv_kind(o->fmt)) || o->w < sx || o->h < sy) {
        d3e_ms_forget(h);
        return 0u;
    }
    const int depth = o->kind == D3E_DEPTH;
    const u32 pw = o->w / sx, ph = o->h / sy;
    d3e_twin* t = d3e_ms_twin(h);
    if (t && t->sx == sx && t->sy == sy && t->pw == pw && t->ph == ph && t->depth == depth &&
        (depth ? t->zcls == o->zcls : t->rfmt == o->rfmt))
        return RSX_BE_AA_HW_MSAA;
    d3e_ms_forget(h);
    if (!d3e_ms_heaps()) return 0u;
    const DXGI_FORMAT vf = depth ? d3e_dsv_format(o->zcls) : o->rfmt;
    const DXGI_FORMAT sf = depth ? d3e_depth_srv_format(o->zcls) : o->vfmt;
    if (!d3e_ms_fmt_ok(vf, sf, samples, depth)) {
        D3E_MS_WARN(0, "host MSAA: format %d x%u not multisample-loadable; that target keeps the emulation\n",
                    (int)vf, samples);
        return 0u;
    }
    u32 idx = D3E_MS_MAX;
    for (u32 i = 0; i < D3E_MS_MAX; i++) if (!s_ms.tw[i].owner) { idx = i; break; }
    if (idx == D3E_MS_MAX) {
        D3E_MS_WARN(1, "host MSAA: more than %u antialiased targets; the rest keep the emulation\n", D3E_MS_MAX);
        return 0u;
    }
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
    D3D12_RESOURCE_DESC rd;
    memset(&rd, 0, sizeof rd);
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = pw; rd.Height = ph;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = depth ? d3e_depth_res_format(o->zcls) : o->rfmt;
    rd.SampleDesc.Count = samples;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv;
    memset(&cv, 0, sizeof cv);
    cv.Format = vf;
    if (depth) { cv.DepthStencil.Depth = 1.0f; cv.DepthStencil.Stencil = 0xFF; }
    else cv.Color[3] = 1.0f;
    const D3D12_RESOURCE_STATES st0 = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12Resource* r = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(s_d.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, st0, &cv,
                                                      &IID_ID3D12Resource, (void**)&r);
    if (FAILED(hr) || !r) {
        D3E_MS_WARN(2, "host MSAA: twin %ux%u x%u failed 0x%08lX; that target keeps the emulation\n",
                    pw, ph, samples, (unsigned long)hr);
        return 0u;
    }
    t = &s_ms.tw[idx];
    memset(t, 0, sizeof *t);
    t->res = r; t->st = st0; t->owner = h;
    t->sx = sx; t->sy = sy; t->samples = samples; t->pw = pw; t->ph = ph;
    t->state = 2;                       /* the widened image is what the guest has */
    t->depth = (u8)depth; t->zcls = o->zcls; t->rfmt = o->rfmt;
    t->has_stencil = (u8)(depth && o->has_stencil);
    s_ms.slot[h] = (u16)(idx + 1u);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = sf;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ID3D12Device_CreateShaderResourceView(s_d.dev, r, &sd, d3e_ms_srv(t, 0));
    if (depth) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.Format = vf;
        dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        ID3D12Device_CreateDepthStencilView(s_d.dev, r, &dd, d3e_ms_dsv(t));
        if (t->has_stencil) {
            /* the plane is implied by the format for TEXTURE2DMS (unverified on every driver) */
            sd.Format = o->zcls == D3E_ZD32S8 ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_X24_TYPELESS_G8_UINT;
            ID3D12Device_CreateShaderResourceView(s_d.dev, r, &sd, d3e_ms_srv(t, 1));
        }
    } else {
        ID3D12Device_CreateRenderTargetView(s_d.dev, r, NULL, d3e_ms_rtv(t));
    }
    /* render targets start defined (D3D12 requires a clear / discard before partial writes) */
    const float c0[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    d3e_ms_clear_twin(t, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, c0, 1.0f, 0xFF);
    return RSX_BE_AA_HW_MSAA;
}

/* ---- resolve / unresolve passes ------------------------------------------------------ */

static ID3D12PipelineState* d3e_ms_pso(u32 op, DXGI_FORMAT rtf, DXGI_FORMAT dsf, u32 samples, u32 smask)
{
    const u64 key = (u64)op | ((u64)rtf << 4) | ((u64)dsf << 12) | ((u64)samples << 20) | ((u64)smask << 28);
    for (u32 i = 0; i < s_ms.npso; i++) if (s_ms.pso[i].key == key) return s_ms.pso[i].pso;
    if (op >= D3E_MS_OPS || !s_d.pass_vs || !s_d.rs_pass) return NULL;
    if (!s_ms.ps[op]) s_ms.ps[op] = d3e_compile(s_ms_src[op], "main", "ps_5_0", "rsx_d3d12_msaa");
    if (!s_ms.ps[op]) return NULL;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = s_d.rs_pass;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(s_d.pass_vs);
    pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(s_d.pass_vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(s_ms.ps[op]);
    pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(s_ms.ps[op]);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.BlendState.RenderTarget[0].SrcBlend = pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlend = pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOp = pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    pd.SampleMask = 0xFFFFFFFFu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.RasterizerState.MultisampleEnable = samples > 1u;
    D3D12_DEPTH_STENCIL_DESC* ds = &pd.DepthStencilState;
    if (op == D3E_MS_RD || op == D3E_MS_UD) {
        ds->DepthEnable = TRUE;
        ds->DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        ds->DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    } else if (op == D3E_MS_RS || op == D3E_MS_US) {
        ds->StencilEnable = TRUE;
        ds->StencilReadMask = 0xFF;
        ds->StencilWriteMask = (UINT8)smask;
        D3D12_DEPTH_STENCILOP_DESC f = { D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_REPLACE,
                                         D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS };
        ds->FrontFace = f;
        ds->BackFace = f;
    }
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = rtf != DXGI_FORMAT_UNKNOWN ? 1u : 0u;
    pd.RTVFormats[0] = rtf;
    pd.DSVFormat = dsf;
    pd.SampleDesc.Count = samples ? samples : 1u;
    ID3D12PipelineState* pso = NULL;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(s_d.dev, &pd, &IID_ID3D12PipelineState, (void**)&pso);
    if (FAILED(hr)) {
        D3E_LOG("host MSAA: pass PSO op %u rt %d ds %d x%u failed 0x%08lX\n", op, (int)rtf, (int)dsf, samples,
                (unsigned long)hr);
        return NULL;
    }
    if (s_ms.npso == (u32)(sizeof s_ms.pso / sizeof s_ms.pso[0])) {
        for (u32 i = 0; i < s_ms.npso; i++) d3e_defer_release(s_ms.pso[i].pso);
        s_ms.npso = 0;
    }
    s_ms.pso[s_ms.npso].key = key;
    s_ms.pso[s_ms.npso].pso = pso;
    s_ms.npso++;
    return pso;
}

/* One full-screen pass over w x h; src0 / src1: CPU SRVs for t0 / t1 (ptr 0: null). */
static void d3e_ms_run(ID3D12PipelineState* pso, D3D12_CPU_DESCRIPTOR_HANDLE src0, D3D12_CPU_DESCRIPTOR_HANDLE src1,
                       const D3D12_CPU_DESCRIPTOR_HANDLE* rtv, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv,
                       u32 w, u32 h, const d3e_twin* t, u32 bit)
{
    if (!pso || !w || !h) return;
    d3e_reserve(0, 2);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    d3e_desc_alloc(2, &cpu, &gpu);
    D3D12_CPU_DESCRIPTOR_HANDLE c1 = cpu;
    c1.ptr += s_d.srv_inc;
    ID3D12Device_CopyDescriptorsSimple(s_d.dev, 1, cpu, src0.ptr ? src0 : d3e_srv_cpu(D3E_SRV_NULL + 0),
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12Device_CopyDescriptorsSimple(s_d.dev, 1, c1, src1.ptr ? src1 : d3e_srv_cpu(D3E_SRV_NULL + 3),
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    float c[16];
    memset(c, 0, sizeof c);
    c[0] = (float)t->sx; c[1] = (float)t->sy; c[2] = (float)bit;
    d3e_query_close_open();
    ID3D12GraphicsCommandList* cl = s_d.cl;
    if (s_d.cur_rs != s_d.rs_pass) {
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, s_d.rs_pass);
        s_d.cur_rs = s_d.rs_pass;
    }
    ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
    s_d.cur_pso = pso;
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 16, c, 0);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, gpu);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, rtv ? 1u : 0u, rtv, FALSE, dsv);
    s_d.om_valid = 0;
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_OMSetStencilRef(cl, 0xFFu);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
    d3e_query_reopen();
}

/* Before the widened image of h is read or written outside a draw: bring the
 * twin's rendering into it. */
static void d3e_ms_ss(u32 h)
{
    d3e_twin* t = d3e_ms_twin(h);
    if (!t || t->state != 1u) return;
    d3e_obj* o = d3e_get(h, 0);
    if (!o) return;
    t->state = 0;
    const D3D12_CPU_DESCRIPTOR_HANDLE none = { 0 };
    const u32 w = t->pw * t->sx, hh = t->ph * t->sy;
    d3e_barrier_res(t->res, &t->st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (!t->depth) {
        d3e_trans(o, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = d3e_rtv_cpu(h);
        d3e_ms_run(d3e_ms_pso(D3E_MS_RC, o->rfmt, DXGI_FORMAT_UNKNOWN, 1, 0), d3e_ms_srv(t, 0), none,
                   &rtv, NULL, w, hh, t, 0);
        return;
    }
    d3e_trans(o, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const DXGI_FORMAT dsf = d3e_dsv_format(o->zcls);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = d3e_dsv_cpu(h);
    d3e_ms_run(d3e_ms_pso(D3E_MS_RD, DXGI_FORMAT_UNKNOWN, dsf, 1, 0), d3e_ms_srv(t, 0), none, NULL, &dsv, w, hh, t, 0);
    if (t->has_stencil && o->has_stencil) {
        D3D12_RECT r = { 0, 0, (LONG)w, (LONG)hh };
        ID3D12GraphicsCommandList_ClearDepthStencilView(s_d.cl, dsv, D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 1, &r);
        for (u32 b = 0; b < 8u; b++)
            d3e_ms_run(d3e_ms_pso(D3E_MS_RS, DXGI_FORMAT_UNKNOWN, dsf, 1, 1u << b), none, d3e_ms_srv(t, 1),
                       NULL, &dsv, w, hh, t, 1u << b);
    }
}

/* Before a multisampled draw into h: bring the widened image into the twin. */
static void d3e_ms_ms(u32 h)
{
    d3e_twin* t = d3e_ms_twin(h);
    if (!t || t->state != 2u) return;
    d3e_obj* o = d3e_get(h, 0);
    if (!o || !o->has_srv) return;
    t->state = 0;
    const D3D12_CPU_DESCRIPTOR_HANDLE none = { 0 };
    d3e_trans(o, d3e_srv_state(o));
    if (!t->depth) {
        d3e_barrier_res(t->res, &t->st, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = d3e_ms_rtv(t);
        d3e_ms_run(d3e_ms_pso(D3E_MS_UC, t->rfmt, DXGI_FORMAT_UNKNOWN, t->samples, 0), d3e_srv_cpu(h), none,
                   &rtv, NULL, t->pw, t->ph, t, 0);
        return;
    }
    d3e_barrier_res(t->res, &t->st, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const DXGI_FORMAT dsf = d3e_dsv_format(t->zcls);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = d3e_ms_dsv(t);
    d3e_ms_run(d3e_ms_pso(D3E_MS_UD, DXGI_FORMAT_UNKNOWN, dsf, t->samples, 0), d3e_srv_cpu(h), none,
               NULL, &dsv, t->pw, t->ph, t, 0);
    if (t->has_stencil && o->has_ssrv) {
        ID3D12GraphicsCommandList_ClearDepthStencilView(s_d.cl, dsv, D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
        for (u32 b = 0; b < 8u; b++)
            d3e_ms_run(d3e_ms_pso(D3E_MS_US, DXGI_FORMAT_UNKNOWN, dsf, t->samples, 1u << b), none,
                       d3e_srv_cpu(D3E_SRV_STENCIL + h), NULL, &dsv, t->pw, t->ph, t, 1u << b);
    }
}

static void d3e_ms_wrote_ss(u32 h)
{
    d3e_twin* t = d3e_ms_twin(h);
    if (t) t->state = 2;
}

/* ---- d3e_draw hooks ------------------------------------------------------------------ */

/* The sample count this draw rasterises at: every colour target has a twin of
 * one shape and the zeta (if any, of its own) a matching one; else 0. */
static u32 d3e_ms_plan(u32 n, int own, int scratch)
{
    if (!d3e_ms_live() || scratch || !n) return 0u;
    const d3e_twin* t0 = d3e_ms_twin(s_e.rt[0]);
    if (!t0) return 0u;
    for (u32 i = 1; i < n; i++) {
        const d3e_twin* t = d3e_ms_twin(s_e.rt[i]);
        if (!t || t->sx != t0->sx || t->sy != t0->sy || t->pw != t0->pw || t->ph != t0->ph) return 0u;
    }
    if (own) {
        const d3e_twin* z = d3e_ms_twin(s_e.depth);
        if (!z || z->sx != t0->sx || z->sy != t0->sy || z->pw < t0->pw || z->ph < t0->ph) return 0u;
    }
    s_ms.sx = t0->sx;
    s_ms.sy = t0->sy;
    return t0->samples;
}

static void d3e_ms_ss_unit(u32 h)
{
    const d3e_obj* b = d3e_base(d3e_get(h, 0));
    if (b) d3e_ms_ss((u32)(b - s_obj));
}

/* Before the draw's units are resolved: sampled surfaces get their twin's
 * rendering; the targets get the side the draw renders into up to date. */
static void d3e_ms_draw_prep(u32 samples, u32 n, int own)
{
    if (!d3e_ms_live()) return;
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++)
        if ((s_e.tex_mask >> u) & 1u) d3e_ms_ss_unit(s_e.tex[u]);
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++)
        if ((s_e.vtex_mask >> u) & 1u) d3e_ms_ss_unit(s_e.vtex[u]);
    for (u32 i = 0; i < n; i++) {
        if (samples) d3e_ms_ms(s_e.rt[i]);
        else d3e_ms_ss(s_e.rt[i]);
    }
    if (own) {
        if (samples) d3e_ms_ms(s_e.depth);
        else d3e_ms_ss(s_e.depth);
    }
}

/* A multisampled draw's attachments: the twins. */
static void d3e_ms_om(u32 n, int own)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[RSX_BE_MAX_COLOR_TARGETS], dsv;
    for (u32 i = 0; i < n; i++) {
        d3e_twin* t = d3e_ms_twin(s_e.rt[i]);
        if (!t) return;                       /* d3e_ms_plan checked them */
        d3e_barrier_res(t->res, &t->st, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtv[i] = d3e_ms_rtv(t);
    }
    d3e_twin* z = own ? d3e_ms_twin(s_e.depth) : NULL;
    if (z) {
        d3e_barrier_res(z->res, &z->st, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        dsv = d3e_ms_dsv(z);
    }
    ID3D12GraphicsCommandList_OMSetRenderTargets(s_d.cl, n, rtv, FALSE, z ? &dsv : NULL);
    s_d.om_valid = 0;
}

/* Widened viewport / scissor onto the twin. */
static void d3e_ms_viewport(D3D12_VIEWPORT* v, D3D12_RECT* sc)
{
    const float fx = (float)s_ms.sx, fy = (float)s_ms.sy;
    v->TopLeftX /= fx; v->Width /= fx;
    v->TopLeftY /= fy; v->Height /= fy;
    u32 x = (u32)sc->left, y = (u32)sc->top, w = (u32)(sc->right - sc->left), h = (u32)(sc->bottom - sc->top);
    rsx_d3d12_ms_rect(s_ms.sx, s_ms.sy, &x, &y, &w, &h);
    sc->left = (LONG)x; sc->top = (LONG)y; sc->right = (LONG)(x + w); sc->bottom = (LONG)(y + h);
}

static void d3e_ms_draw_done(u32 samples, u32 n, int own)
{
    if (!d3e_ms_live()) return;
    for (u32 i = 0; i < n; i++) {
        d3e_twin* t = d3e_ms_twin(s_e.rt[i]);
        if (t) t->state = samples ? 1u : 2u;
    }
    if (own) {
        d3e_twin* z = d3e_ms_twin(s_e.depth);
        if (z) z->state = samples ? 1u : 2u;
    }
}

/* d3e_build_pso: SET_ANTI_ALIASING_CONTROL (RPCS3 get_pipeline_properties). A
 * register never written reads as its reset value 0xFFFF0000. */
static void d3e_ms_pso_state(const d3e_pipe* P, const struct d3e_vkey* k, D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd)
{
    if (!s_ms_on) return;
    const u32 aac = P->rs.aa_control ? P->rs.aa_control : 0xFFFF0000u;
    pd->BlendState.AlphaToCoverageEnable = (aac & 0x10u) ? TRUE : FALSE;
    if (k->samples > 1u) {
        pd->SampleDesc.Count = k->samples;
        pd->SampleMask = (aac >> 16) & 0xFFFFu;
        pd->RasterizerState.MultisampleEnable = TRUE;
    }
}

/* ---- backend wrap: reads resolve first, writes outside draws mark the widened image -- */

static void d3e_ms_be_texture_release(void* u, u32 h) { d3e_ms_forget(h); s_ms.inner.texture_release(u, h); }
static void d3e_ms_be_color_release(void* u, u32 h) { d3e_ms_forget(h); s_ms.inner.color_target_release(u, h); }
static void d3e_ms_be_depth_release(void* u, u32 h) { d3e_ms_forget(h); s_ms.inner.depth_target_release(u, h); }

static void d3e_ms_be_color_write(void* u, u32 s, u32 x, u32 y, u32 w, u32 h, const void* rows, u32 rb)
{
    d3e_ms_ss(s);
    s_ms.inner.color_target_write(u, s, x, y, w, h, rows, rb);
    d3e_ms_wrote_ss(s);
}

static u32 d3e_ms_be_depth_snapshot(void* u, u32 d, u32 w, u32 h)
{
    d3e_ms_ss(d);
    return s_ms.inner.depth_snapshot(u, d, w, h);
}

static void d3e_ms_be_clear_color(void* u, u32 s, const float rgba[4])
{
    s_ms.inner.clear_color(u, s, rgba);
    d3e_twin* t = d3e_ms_twin(s);
    if (!t || !rgba) return;
    float cv[4];
    rsx_d3d12_cv_clear(0, rgba, cv);
    d3e_ms_clear_twin(t, (D3D12_CLEAR_FLAGS)0, cv, 0.0f, 0);
    t->state = 0;
}

static void d3e_ms_be_clear_ds(void* u, u32 d, u32 flags, float z, u8 s)
{
    d3e_twin* t = d3e_ms_twin(d);
    if (!t) { s_ms.inner.clear_depth_stencil(u, d, flags, z, s); return; }
    const int full = (flags & RSX_BE_CLEAR_DEPTH) && (!t->has_stencil || (flags & RSX_BE_CLEAR_STENCIL));
    if (!full) d3e_ms_ss(d);           /* the other aspect must agree on both sides */
    s_ms.inner.clear_depth_stencil(u, d, flags, z, s);
    D3D12_CLEAR_FLAGS f = (D3D12_CLEAR_FLAGS)0;
    if (flags & RSX_BE_CLEAR_DEPTH) f |= D3D12_CLEAR_FLAG_DEPTH;
    if (flags & RSX_BE_CLEAR_STENCIL) f |= D3D12_CLEAR_FLAG_STENCIL;
    d3e_ms_clear_twin(t, f, NULL, z, s);
    if (full) t->state = 0;            /* else unchanged: 0 stays 0, 2 stays 2 */
}

static void d3e_ms_be_clear_rect(void* u, const u32* rts, u32 n_rts, u32 depth, const float rgba[4],
                                 u32 color_mask, u32 ds_flags, float z, u32 stencil, u32 stencil_mask,
                                 u32 x, u32 y, u32 w, u32 h)
{
    if (n_rts > RSX_BE_MAX_COLOR_TARGETS) n_rts = RSX_BE_MAX_COLOR_TARGETS;
    for (u32 i = 0; rts && i < n_rts; i++) d3e_ms_ss(rts[i]);
    if (depth) d3e_ms_ss(depth);
    s_ms.inner.clear_rect(u, rts, n_rts, depth, rgba, color_mask, ds_flags, z, stencil, stencil_mask, x, y, w, h);
    if (rgba && (color_mask & 0xFu))
        for (u32 i = 0; rts && i < n_rts; i++) d3e_ms_wrote_ss(rts[i]);
    if (depth && (ds_flags & (RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL))) d3e_ms_wrote_ss(depth);
}

static void d3e_ms_be_present(void* u, u32 s) { d3e_ms_ss(s); s_ms.inner.present(u, s); }

static void d3e_ms_be_present_region(void* u, u32 s, u32 sx, u32 sy, u32 sw, u32 sh, u32 aw, u32 ah)
{
    d3e_ms_ss(s);
    s_ms.inner.present_region(u, s, sx, sy, sw, sh, aw, ah);
}

static void d3e_ms_be_readback(void* u, u32 s, u32 x, u32 y, u32 w, u32 h, void* out, u32 pitch)
{
    d3e_ms_ss(s);
    s_ms.inner.readback(u, s, x, y, w, h, out, pitch);
}

static int d3e_ms_be_depth_readback(void* u, u32 d, u32 x, u32 y, u32 w, u32 h, float* out, u32 pitch)
{
    d3e_ms_ss(d);
    return s_ms.inner.depth_readback(u, d, x, y, w, h, out, pitch);
}

static int d3e_ms_be_stencil_readback(void* u, u32 d, u32 x, u32 y, u32 w, u32 h, u8* out, u32 pitch)
{
    d3e_ms_ss(d);
    return s_ms.inner.stencil_readback(u, d, x, y, w, h, out, pitch);
}

static int d3e_ms_be_zeta_readback(void* u, u32 d, u32 x, u32 y, u32 w, u32 h, float* of, u32* oraw,
                                   u32* raw_bits, u8* os, int* have_stencil)
{
    d3e_ms_ss(d);
    return s_ms.inner.zeta_readback(u, d, x, y, w, h, of, oraw, raw_bits, os, have_stencil);
}

static u32 d3e_ms_be_color_snapshot(void* u, u32 s) { d3e_ms_ss(s); return s_ms.inner.color_snapshot(u, s); }

static u32 d3e_ms_be_color_region_snapshot(void* u, u32 s, u32 x, u32 y, u32 w, u32 h)
{
    d3e_ms_ss(s);
    return s_ms.inner.color_region_snapshot(u, s, x, y, w, h);
}

static int d3e_ms_be_tex_copy_from_color(void* u, u32 tex, u32 layer, u32 mip, u32 s, u32 sx, u32 sy, u32 w, u32 h)
{
    d3e_ms_ss(s);
    return s_ms.inner.texture_copy_from_color(u, tex, layer, mip, s, sx, sy, w, h);
}

static void d3e_ms_be_blit(void* u, u32 src, u32 sx, u32 sy, u32 sw, u32 sh, u32 dst, u32 dx, u32 dy, u32 dw, u32 dh)
{
    d3e_ms_ss(src);
    d3e_ms_ss(dst);
    s_ms.inner.color_target_blit(u, src, sx, sy, sw, sh, dst, dx, dy, dw, dh);
    d3e_ms_wrote_ss(dst);
}

static void d3e_ms_be_blit_filtered(void* u, u32 src, u32 sx, u32 sy, u32 sw, u32 sh, u32 dst, u32 dx, u32 dy,
                                    u32 dw, u32 dh, int linear)
{
    d3e_ms_ss(src);
    d3e_ms_ss(dst);
    s_ms.inner.color_target_blit_filtered(u, src, sx, sy, sw, sh, dst, dx, dy, dw, dh, linear);
    d3e_ms_wrote_ss(dst);
}

static void d3e_ms_be_blit_flip(void* u, u32 src, u32 sx, u32 sy, u32 sw, u32 sh, u32 dst, u32 dx, u32 dy,
                                u32 dw, u32 dh, int linear, u32 flip)
{
    d3e_ms_ss(src);
    d3e_ms_ss(dst);
    s_ms.inner.color_target_blit_flip(u, src, sx, sy, sw, sh, dst, dx, dy, dw, dh, linear, flip);
    d3e_ms_wrote_ss(dst);
}

static int d3e_ms_be_depth_copy(void* u, u32 src, u32 sx, u32 sy, u32 sw, u32 sh, u32 dst, u32 dx, u32 dy,
                                u32 dw, u32 dh)
{
    d3e_ms_ss(src);
    d3e_ms_ss(dst);
    const int r = s_ms.inner.depth_target_copy(u, src, sx, sy, sw, sh, dst, dx, dy, dw, dh);
    if (r) d3e_ms_wrote_ss(dst);
    return r;
}

static int d3e_ms_be_scene_aa(void* u, const rsx_be_scene_aa* q)
{
    if (q) { d3e_ms_ss(q->color); d3e_ms_ss(q->depth); }
    const int r = s_ms.inner.scene_aa(u, q);
    if (r && q) d3e_ms_wrote_ss(q->color);
    return r;
}

static void d3e_ms_backend(rsx_draw_backend* b)
{
    s_ms.inner = *b;
#define D3E_MS_WRAP(field, fn) do { if (b->field) b->field = fn; } while (0)
    D3E_MS_WRAP(texture_release, d3e_ms_be_texture_release);
    D3E_MS_WRAP(color_target_release, d3e_ms_be_color_release);
    D3E_MS_WRAP(depth_target_release, d3e_ms_be_depth_release);
    D3E_MS_WRAP(color_target_write, d3e_ms_be_color_write);
    D3E_MS_WRAP(depth_snapshot, d3e_ms_be_depth_snapshot);
    D3E_MS_WRAP(clear_color, d3e_ms_be_clear_color);
    D3E_MS_WRAP(clear_depth_stencil, d3e_ms_be_clear_ds);
    D3E_MS_WRAP(clear_rect, d3e_ms_be_clear_rect);
    D3E_MS_WRAP(present, d3e_ms_be_present);
    D3E_MS_WRAP(present_region, d3e_ms_be_present_region);
    D3E_MS_WRAP(readback, d3e_ms_be_readback);
    D3E_MS_WRAP(depth_readback, d3e_ms_be_depth_readback);
    D3E_MS_WRAP(stencil_readback, d3e_ms_be_stencil_readback);
    D3E_MS_WRAP(zeta_readback, d3e_ms_be_zeta_readback);
    D3E_MS_WRAP(color_snapshot, d3e_ms_be_color_snapshot);
    D3E_MS_WRAP(color_region_snapshot, d3e_ms_be_color_region_snapshot);
    D3E_MS_WRAP(texture_copy_from_color, d3e_ms_be_tex_copy_from_color);
    D3E_MS_WRAP(color_target_blit, d3e_ms_be_blit);
    D3E_MS_WRAP(color_target_blit_filtered, d3e_ms_be_blit_filtered);
    D3E_MS_WRAP(color_target_blit_flip, d3e_ms_be_blit_flip);
    D3E_MS_WRAP(depth_target_copy, d3e_ms_be_depth_copy);
    D3E_MS_WRAP(scene_aa, d3e_ms_be_scene_aa);
#undef D3E_MS_WRAP
    b->target_set_aa = d3e_ms_target_set_aa;
}

/* rsx_d3d12_engine_init, after the render scale is known: 1 when the backend
 * is to be wrapped (RSX_D3D12_HW_MSAA=1 and no render scale). */
static int d3e_ms_setup(void)
{
    s_ms_on = 0;
    if (!d3e_env_on("RSX_D3D12_HW_MSAA", 0)) return 0;
    if (s_up.on) {
        D3E_LOG("host MSAA: off with a render scale < 1 (the widened emulation runs)\n");
        return 0;
    }
    s_ms_on = 1;
    D3E_LOG("host MSAA on (RSX_D3D12_HW_MSAA=1, experimental): antialiased surfaces draw into multisampled twins\n");
    return 1;
}

static void d3e_ms_device_down(void)
{
    for (u32 i = 0; i < D3E_MS_MAX; i++)
        if (s_ms.tw[i].res) d3e_defer_release(s_ms.tw[i].res);
    memset(s_ms.tw, 0, sizeof s_ms.tw);
    memset(s_ms.slot, 0, sizeof s_ms.slot);
    for (u32 i = 0; i < s_ms.npso; i++) d3e_defer_release(s_ms.pso[i].pso);
    s_ms.npso = 0;
    for (u32 i = 0; i < D3E_MS_OPS; i++)
        if (s_ms.ps[i]) { ID3D10Blob_Release(s_ms.ps[i]); s_ms.ps[i] = NULL; }
    if (s_ms.rtv) { ID3D12DescriptorHeap_Release(s_ms.rtv); s_ms.rtv = NULL; }
    if (s_ms.dsv) { ID3D12DescriptorHeap_Release(s_ms.dsv); s_ms.dsv = NULL; }
    if (s_ms.srv) { ID3D12DescriptorHeap_Release(s_ms.srv); s_ms.srv = NULL; }
    s_ms.heaps_failed = 0;
}
