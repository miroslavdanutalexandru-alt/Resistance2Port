/* rsx_d3d12_engine_aa.inc -- included by rsx_d3d12_engine.c.
 *
 * Temporal upscalers through the engine's scene_aa hook (native AA at 1:1,
 * as the Vulkan engine runs them): the prep pass (scene colour/depth ->
 * RGBA16F colour, R32F depth, RG16F motion vectors from the game's camera
 * reprojection, R8 moving-object mask), one provider (NVIDIA DLSS through
 * r2_dlss.dll's D3D12 exports, AMD FSR 3.1 through the FidelityFX API DX12
 * runtime, Intel XeSS through libxess.dll), then the merge pass (result +
 * the original alpha, copied back into the scene target).
 *
 * Also FSR1 output scaling (EASU to the letterboxed output size, RCAS into
 * the back buffer), RSX_FILTER=fsr or rsx_d3d12_engine_force_output_fsr1. */

#define D3E_HAVE_FFX 0
#define D3E_HAVE_XESS 0
#if defined(__has_include)
#if __has_include("../../../tools/ffx_vk/include/ffx_api/ffx_upscale.h")
#undef D3E_HAVE_FFX
#define D3E_HAVE_FFX 1
#include "../../../tools/ffx_vk/include/ffx_api/ffx_api.h"
#include "../../../tools/ffx_vk/include/ffx_api/ffx_upscale.h"
#endif
#if __has_include("../../../tools/xess/inc/xess/xess_d3d12.h")
#undef D3E_HAVE_XESS
#define D3E_HAVE_XESS 1
#include "../../../tools/xess/inc/xess/xess_d3d12.h"
#endif
#endif

/* ---- shaders ------------------------------------------------------------------
 * Root signature: b0 = 32 root constants, t0..t2 = a 3-SRV pixel table.
 *   T      current NDC -> previous (prep), column-major as the Vulkan push block
 *   view   prep: jitter ndc xy, viewport wh   | EASU: con0
 *   opts   prep: zA, zB, w, h | merge: swap, w, h, debug view | FSR: source clamp rect
 *   opts2  prep: has previous depth, inverted | merge: render/output ratio | EASU: misc / RCAS: sharpness
 *   x0     RCAS: output origin xy, gamma on, gamma */
static const char s_aa_hlsl[] =
    "cbuffer A : register(b0) { float4x4 T; float4 view; float4 opts; float4 opts2; float4 x0; };\n"
    "Texture2D<float4> t0 : register(t0);\n"
    "Texture2D<float4> t1 : register(t1);\n"
    "Texture2D<float4> t2 : register(t2);\n"
    "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 t = float2((id << 1) & 2, id & 2);\n"
    "  return float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); }\n"
    "float lin(float raw) { return opts2.y > 0.5 ? raw : 1.0 - raw; }\n"
    "struct PO { float4 c : SV_Target0; float d : SV_Target1; float2 mv : SV_Target2; float m : SV_Target3; };\n"
    "PO prep(float4 sp : SV_Position) {\n"
    "  PO o; int2 p = int2(sp.xy);\n"
    "  o.c = t0.Load(int3(p, 0)); float d = t1.Load(int3(p, 0)).r; o.d = d;\n"
    "  float2 ndc = (float2(p) + 0.5) / view.zw * 2.0 - 1.0 - view.xy;\n"
    "  float4 q = mul(T, float4(ndc, d * opts.x + opts.y, 1.0));\n"
    "  float2 mv = float2(0.0, 0.0); float m = 0.0;\n"
    "  if (abs(q.w) > 1e-7) {\n"
    "    mv = (q.xy / q.w - ndc) * 0.5;\n"
    "    if (any(isnan(mv)) || any(isinf(mv)) || any(abs(mv) > 2.0)) mv = float2(0.0, 0.0);\n"
    "    else if (opts2.x > 0.5) {\n"
    "      int2 ip = int2(floor((q.xy / q.w + 1.0) * 0.5 * view.zw));\n"
    "      int2 lim = int2(opts.zw) - 1;\n"
    "      if (all(ip >= 0) && all(ip <= lim)) {\n"
    "        float lo = 1e30, hi = -1e30;\n"
    "        [unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x) {\n"
    "          float l = lin(t2.Load(int3(clamp(ip + int2(x, y), int2(0, 0), lim), 0)).r);\n"
    "          lo = min(lo, l); hi = max(hi, l); }\n"
    "        float lc = lin(((q.z / q.w) - opts.y) / opts.x);\n"
    "        float e = max(lo * 0.97 - lc, lc - hi * 1.03) / max(max(lc, hi), 1e-6);\n"
    "        m = 0.85 * smoothstep(0.0, 0.12, e); } } }\n"
    "  o.mv = mv; o.m = m; return o; }\n"
    "float4 merge(float4 sp : SV_Position) : SV_Target {\n"
    "  int2 p = int2(sp.xy); int2 q = int2((float2(p) + 0.5) * opts2.xy);\n"
    "  float3 c = t0.Load(int3(p, 0)).rgb; float4 o = t1.Load(int3(q, 0));\n"
    "  if (any(isnan(c))) c = o.rgb;\n"
    "  if (opts.w > 1.5) { if (p.x < int(opts.y) / 2) c = o.rgb; }\n"
    "  else if (opts.w > 0.5) c = min(abs(c - o.rgb) * 8.0, float3(1.0, 1.0, 1.0));\n"
    "  float4 r = float4(c, o.a); if (opts.x > 0.5) r = r.bgra; return r; }\n"
    /* FSR1 (AMD FidelityFX Super Resolution 1.0, MIT), the F32 path of the
     * Vulkan backend's rsx_vk_fsr1.comp. */
    "float3 ld(int2 p) { return t0.Load(int3(clamp(p, int2(opts.xy), int2(opts.zw)), 0)).rgb; }\n"
    "float rcpf(float x) { return 1.0 / max(x, 1e-20); }\n"
    "void tap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c) {\n"
    "  float2 v; v.x = off.x * dir.x + off.y * dir.y; v.y = off.x * -dir.y + off.y * dir.x; v *= len;\n"
    "  float d2 = min(v.x * v.x + v.y * v.y, clp);\n"
    "  float wB = 2.0 / 5.0 * d2 - 1.0; float wA = lob * d2 - 1.0; wB *= wB; wA *= wA;\n"
    "  wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0); float w = wB * wA; aC += c * w; aW += w; }\n"
    "void setf(inout float2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {\n"
    "  float dc = lD - lC, cb = lC - lB; float lenX = rcpf(max(abs(dc), abs(cb))); float dirX = lD - lB;\n"
    "  dir.x += dirX * w; lenX = saturate(abs(dirX) * lenX); lenX *= lenX; len += lenX * w;\n"
    "  float ec = lE - lC, ca = lC - lA; float lenY = rcpf(max(abs(ec), abs(ca))); float dirY = lE - lA;\n"
    "  dir.y += dirY * w; lenY = saturate(abs(dirY) * lenY); lenY *= lenY; len += lenY * w; }\n"
    "float luma(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }\n"
    "float4 easu(float4 sp : SV_Position) : SV_Target {\n"
    "  int2 ip = int2(sp.xy);\n"
    "  float2 pp = float2(ip) * view.xy + view.zw; float2 fp = floor(pp); pp -= fp; int2 f0 = int2(fp);\n"
    "  float3 bC = ld(f0 + int2( 0, -1)), cC = ld(f0 + int2( 1, -1));\n"
    "  float3 iC = ld(f0 + int2(-1,  1)), jC = ld(f0 + int2( 0,  1));\n"
    "  float3 fC = ld(f0 + int2( 0,  0)), eC = ld(f0 + int2(-1,  0));\n"
    "  float3 kC = ld(f0 + int2( 1,  1)), lC = ld(f0 + int2( 2,  1));\n"
    "  float3 hC = ld(f0 + int2( 2,  0)), gC = ld(f0 + int2( 1,  0));\n"
    "  float3 oC = ld(f0 + int2( 1,  2)), nC = ld(f0 + int2( 0,  2));\n"
    "  float bL = luma(bC), cL = luma(cC), iL = luma(iC), jL = luma(jC);\n"
    "  float fL = luma(fC), eL = luma(eC), kL = luma(kC), lL = luma(lC);\n"
    "  float hL = luma(hC), gL = luma(gC), oL = luma(oC), nL = luma(nC);\n"
    "  float2 dir = float2(0.0, 0.0); float len = 0.0;\n"
    "  setf(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
    "  setf(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
    "  setf(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
    "  setf(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
    "  float2 dir2 = dir * dir; float dirR = dir2.x + dir2.y; bool zro = dirR < 1.0 / 32768.0;\n"
    "  dirR = zro ? 1.0 : rsqrt(dirR); dir.x = zro ? 1.0 : dir.x; dir *= dirR;\n"
    "  len = len * 0.5; len *= len;\n"
    "  float stretch = (dir.x * dir.x + dir.y * dir.y) * rcpf(max(abs(dir.x), abs(dir.y)));\n"
    "  float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
    "  float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len; float clp = rcpf(lob);\n"
    "  float3 aC = float3(0.0, 0.0, 0.0); float aW = 0.0;\n"
    "  tap(aC, aW, float2( 0.0, -1.0) - pp, dir, len2, lob, clp, bC);\n"
    "  tap(aC, aW, float2( 1.0, -1.0) - pp, dir, len2, lob, clp, cC);\n"
    "  tap(aC, aW, float2(-1.0,  1.0) - pp, dir, len2, lob, clp, iC);\n"
    "  tap(aC, aW, float2( 0.0,  1.0) - pp, dir, len2, lob, clp, jC);\n"
    "  tap(aC, aW, float2( 0.0,  0.0) - pp, dir, len2, lob, clp, fC);\n"
    "  tap(aC, aW, float2(-1.0,  0.0) - pp, dir, len2, lob, clp, eC);\n"
    "  tap(aC, aW, float2( 1.0,  1.0) - pp, dir, len2, lob, clp, kC);\n"
    "  tap(aC, aW, float2( 2.0,  1.0) - pp, dir, len2, lob, clp, lC);\n"
    "  tap(aC, aW, float2( 2.0,  0.0) - pp, dir, len2, lob, clp, hC);\n"
    "  tap(aC, aW, float2( 1.0,  0.0) - pp, dir, len2, lob, clp, gC);\n"
    "  tap(aC, aW, float2( 1.0,  2.0) - pp, dir, len2, lob, clp, oC);\n"
    "  tap(aC, aW, float2( 0.0,  2.0) - pp, dir, len2, lob, clp, nC);\n"
    "  float3 min4 = min(min(fC, gC), min(jC, kC)); float3 max4 = max(max(fC, gC), max(jC, kC));\n"
    "  return float4(min(max4, max(min4, aC * (1.0 / aW))), 1.0); }\n"
    "float4 rcas(float4 sp : SV_Position) : SV_Target {\n"
    "  int2 ip = int2(sp.xy) - int2(x0.xy);\n"
    "  float3 b = ld(ip + int2(0, -1)), d = ld(ip + int2(-1, 0)), e = ld(ip), f = ld(ip + int2(1, 0)), h = ld(ip + int2(0, 1));\n"
    "  float3 mn4 = min(min(b, d), min(f, h)); float3 mx4 = max(max(b, d), max(f, h));\n"
    "  float3 hitMin = min(mn4, e) / max(4.0 * mx4, float3(1e-20, 1e-20, 1e-20));\n"
    "  float3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, float3(-1e-20, -1e-20, -1e-20));\n"
    "  float3 lobeRGB = max(-hitMin, hitMax);\n"
    "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * opts2.x;\n"
    "  float3 c = (lobe * (b + d + h + f) + e) / (4.0 * lobe + 1.0);\n"
    "  if (x0.z > 0.5) c = pow(max(c, 0.0), x0.w);\n"
    "  return float4(c, 1.0); }\n"
    /* HDR encode (R1 rsx_d3d12_backend.c): opts = mode (1 HDR10, 2 scRGB), paper, peak, saturation */
    "uint pcg(uint v) { uint s = v * 747796405u + 2891336453u; uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;\n"
    "  return (w >> 22u) ^ w; }\n"
    "float4 hdro(float4 sp : SV_Position) : SV_Target {\n"
    "  float3 c = saturate(t0.Load(int3(int2(sp.xy), 0)).rgb);\n"
    "  uint2 q = uint2(sp.xy); uint hh = pcg(q.x + pcg(q.y));\n"
    "  float t = (float(hh & 0xFFFFu) + float(hh >> 16)) / 65535.0 - 1.0;\n"
    "  c = saturate(c + t / 255.0 * saturate((max(c.r, max(c.g, c.b)) - 0.74) * 20.0));\n"
    "  float3 l = pow(c, 2.2); float y = dot(l, float3(0.2126, 0.7152, 0.0722));\n"
    "  const float kn = 0.6; float r = max(opts.z / opts.y, 1.0);\n"
    "  if (y > kn) { float u = saturate((y - kn) / (1.0 - kn)); float a = (r - 1.0) / (1.0 - kn);\n"
    "                l *= (kn + (1.0 - kn) * u * (1.0 + a * u * u * u)) / y; }\n"
    "  float3 n = float3(dot(l, float3(0.627404, 0.329283, 0.043313)), dot(l, float3(0.069097, 0.919540, 0.011362)),\n"
    "                    dot(l, float3(0.016391, 0.088013, 0.895595)));\n"
    "  float y2 = dot(n, float3(0.2627, 0.6780, 0.0593));\n"
    "  n = max(y2 + (n - y2) * opts.w, 0.0) * opts.y;\n"
    "  float m = max(n.r, max(n.g, n.b)); if (m > opts.z) n *= opts.z / m;\n"
    "  if (opts.x > 1.5) return float4(float3(dot(n, float3(1.660491, -0.587641, -0.072850)),\n"
    "    dot(n, float3(-0.124550, 1.132900, -0.008349)), dot(n, float3(-0.018151, -0.100579, 1.118730))) / 80.0, 1.0);\n"
    "  float3 p = pow(saturate(n / 10000.0), 0.1593017578125);\n"
    "  return float4(pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), 78.84375), 1.0); }\n";

enum { D3E_AA_PREP = 0, D3E_AA_MERGE, D3E_AA_EASU, D3E_AA_RCAS, D3E_AA_HDR, D3E_AA_PS_COUNT };
static const char* const s_aa_entry[D3E_AA_PS_COUNT] = { "prep", "merge", "easu", "rcas", "hdro" };

/* Images of the upscaler and of FSR1: CPU SRV + RTV slot = the enum value. */
enum { D3E_AI_COLOR = 0, D3E_AI_DEPTH0, D3E_AI_DEPTH1, D3E_AI_MV, D3E_AI_MASK, D3E_AI_OUT, D3E_AI_FIN,
       D3E_AI_MID, D3E_AI_SDR, D3E_AI_HUDLESS, D3E_AI_COUNT };

typedef struct d3e_aimg {
    ID3D12Resource* res;
    D3D12_RESOURCE_STATES st;
    DXGI_FORMAT fmt;
    u32 w, h, uav;
} d3e_aimg;

enum { D3E_UP_NONE = 0, D3E_UP_DLSS = 1, D3E_UP_FSR3 = 2, D3E_UP_XESS = 3 };
static const char* const s_up_name[4] = { "none", "NVIDIA DLSS", "AMD FSR 3.1", "Intel XeSS" };

static struct {
    int  kind;                       /* rsx_d3d12_engine_use_upscaler */
    float sharp;
    unsigned preset;
    int  loaded, ready, broken, created, prev_ok, eval_fail;
    u32  cw, ch, cflags, di;
    u32  crw, crh;                   /* render size of the context (render scale) */
    int  fsr1_forced, shaders_failed;
    ID3D12RootSignature* rs;
    ID3D12DescriptorHeap* srv;       /* CPU-only */
    ID3D12DescriptorHeap* rtv;
    u32  srv_inc, rtv_inc;
    ID3DBlob* vs;
    ID3DBlob* ps[D3E_AA_PS_COUNT];
    struct { u64 key; ID3D12PipelineState* pso; } pso[16];
    u32  npso;
    d3e_aimg img[D3E_AI_COUNT];
} s_ua;

static D3D12_CPU_DESCRIPTOR_HANDLE d3e_ua_srv(u32 i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(s_ua.srv, &h);
    h.ptr += (SIZE_T)i * s_ua.srv_inc;
    return h;
}

static D3D12_CPU_DESCRIPTOR_HANDLE d3e_ua_rtv(u32 i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(s_ua.rtv, &h);
    h.ptr += (SIZE_T)i * s_ua.rtv_inc;
    return h;
}

/* ---- providers ------------------------------------------------------------------ */

static wchar_t* d3e_exe_dir_w(void)
{
    static wchar_t dir[MAX_PATH + 2];
    if (!dir[0]) {
        const DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
        DWORD k = n < MAX_PATH ? n : 0;
        while (k && dir[k - 1] != L'\\' && dir[k - 1] != L'/') k--;
        if (k) dir[k - 1] = 0;
        else { dir[0] = L'.'; dir[1] = 0; }
    }
    return dir;
}

static HMODULE d3e_load_beside(const char* name)
{
    char p[MAX_PATH * 2];
    snprintf(p, sizeof p, "%s%s", d3e_exe_dir(), name);
    return LoadLibraryA(p);
}

/* NVIDIA DLSS: project/r2_dlss.cpp's D3D12 exports (r2_dlss.dll). */
static struct {
    HMODULE dll;
    int  (*init)(ID3D12Device*, const wchar_t*, void (*)(const char*));
    int  (*create)(ID3D12GraphicsCommandList*, unsigned, unsigned, unsigned, unsigned, int, unsigned);
    int  (*eval)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
                 ID3D12Resource*, unsigned, unsigned, float, float, float, float, int, float);
    void (*shutdown)(void);
} s_dl;

static void d3e_dl_log(const char* m) { D3E_LOG("DLSS: %s\n", m ? m : ""); }

static int d3e_dl_load(void)
{
    s_dl.dll = d3e_load_beside("r2_dlss.dll");
    if (!s_dl.dll) { D3E_LOG("r2_dlss.dll not found next to the game: DLSS unavailable\n"); return 0; }
    s_dl.init = (int (*)(ID3D12Device*, const wchar_t*, void (*)(const char*)))(void*)
        GetProcAddress(s_dl.dll, "r2_dlss_d3d12_init");
    s_dl.create = (int (*)(ID3D12GraphicsCommandList*, unsigned, unsigned, unsigned, unsigned, int, unsigned))(void*)
        GetProcAddress(s_dl.dll, "r2_dlss_d3d12_create");
    s_dl.eval = (int (*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
                         ID3D12Resource*, unsigned, unsigned, float, float, float, float, int, float))(void*)
        GetProcAddress(s_dl.dll, "r2_dlss_d3d12_eval");
    s_dl.shutdown = (void (*)(void))(void*)GetProcAddress(s_dl.dll, "r2_dlss_d3d12_shutdown");
    if (!s_dl.init || !s_dl.create || !s_dl.eval || !s_dl.shutdown) {
        D3E_LOG("r2_dlss.dll has no Direct3D 12 entry points (an older build?): DLSS unavailable\n");
        FreeLibrary(s_dl.dll);
        memset(&s_dl, 0, sizeof s_dl);
        return 0;
    }
    return 1;
}

#if D3E_HAVE_FFX
/* AMD FSR 3.1: the FidelityFX API (ffx_api, C) over its DX12 runtime. The
 * DX12 backend descriptor of ffx_api_dx12.h (C++ only, default arguments). */
#define D3E_FFX_BACKEND_DX12 0x0000002u
struct d3e_ffx_backend_dx12 { ffxCreateContextDescHeader header; ID3D12Device* device; };

static struct {
    HMODULE dll;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxDispatch dispatch;
    ffxContext ctx;
    int hdr;
    u32 w, h, rw, rh, msgs, errors;
} s_fx;

static void d3e_fx_message(uint32_t type, const wchar_t* message)
{
    if (s_fx.msgs >= 32u) return;
    if (++s_fx.msgs == 32u) { D3E_LOG("FSR 3.1: further runtime messages muted\n"); return; }
    char b[384];
    memset(b, 0, sizeof b);
    WideCharToMultiByte(CP_UTF8, 0, message ? message : L"", -1, b, (int)sizeof b - 1, NULL, NULL);
    D3E_LOG("FSR 3.1 %s: %s\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", b);
}

static int d3e_fx_load(void)
{
    static const char* const names[] = { "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_dx12.dll" };
    for (u32 i = 0; i < 2 && !s_fx.dll; i++) s_fx.dll = d3e_load_beside(names[i]);
    if (!s_fx.dll) {
        D3E_LOG("FSR 3.1 unavailable: amd_fidelityfx_loader_dx12.dll is not next to the game (error %lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }
    s_fx.create = (PfnFfxCreateContext)(void*)GetProcAddress(s_fx.dll, "ffxCreateContext");
    s_fx.destroy = (PfnFfxDestroyContext)(void*)GetProcAddress(s_fx.dll, "ffxDestroyContext");
    s_fx.dispatch = (PfnFfxDispatch)(void*)GetProcAddress(s_fx.dll, "ffxDispatch");
    if (!s_fx.create || !s_fx.destroy || !s_fx.dispatch) {
        D3E_LOG("FSR 3.1 unavailable: the FidelityFX DX12 runtime has no API exports\n");
        FreeLibrary(s_fx.dll);
        memset(&s_fx, 0, sizeof s_fx);
        return 0;
    }
    return 1;
}

static void d3e_fx_destroy(void)
{
    if (s_fx.ctx && s_fx.destroy) s_fx.destroy(&s_fx.ctx, NULL);
    s_fx.ctx = NULL;
}

static int d3e_fx_create(u32 rw, u32 rh, u32 w, u32 h, int flags)
{
    d3e_fx_destroy();
    struct d3e_ffx_backend_dx12 be;
    memset(&be, 0, sizeof be);
    be.header.type = D3E_FFX_BACKEND_DX12;
    be.device = s_d.dev;
    struct ffxCreateContextDescUpscale cd;
    memset(&cd, 0, sizeof cd);
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cd.header.pNext = &be.header;
    s_fx.hdr = (flags & 2) != 0;
    cd.flags = (s_fx.hdr ? (FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE)
                         : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE) |
               ((flags & 1) ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0);
    cd.maxRenderSize.width = rw; cd.maxRenderSize.height = rh;
    cd.maxUpscaleSize.width = w; cd.maxUpscaleSize.height = h;
    cd.fpMessage = d3e_fx_message;
    const ffxReturnCode_t rc = s_fx.create(&s_fx.ctx, &cd.header, NULL);
    if (rc != FFX_API_RETURN_OK || !s_fx.ctx) {
        s_fx.ctx = NULL;
        D3E_LOG("FSR 3.1 context creation failed (code %u)\n", (unsigned)rc);
        return 0;
    }
    s_fx.w = w; s_fx.h = h; s_fx.rw = rw; s_fx.rh = rh;
    D3E_LOG("FSR 3.1 (DX12) native AA context %ux%u -> %ux%u (%s depth, %s colour)\n", rw, rh, w, h,
            (flags & 1) ? "inverted" : "standard", s_fx.hdr ? "FP16 HDR" : "8-bit sRGB");
    return 1;
}

static uint32_t d3e_fx_format(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32_FLOAT:          return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16G16_FLOAT:       return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R8_UNORM:           return FFX_API_SURFACE_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM:     return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    default:                             return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

static struct FfxApiResource d3e_fx_res(const d3e_aimg* im, u32 w, u32 h)
{
    struct FfxApiResource r;
    memset(&r, 0, sizeof r);
    if (!im || !im->res) return r;
    r.resource = im->res;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = d3e_fx_format(im->fmt);
    r.description.width = w;
    r.description.height = h;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = im->uav ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    r.state = im->uav ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ;
    return r;
}

static int d3e_fx_eval(const d3e_aimg* c, const d3e_aimg* d, const d3e_aimg* mv, const d3e_aimg* mask,
                       const d3e_aimg* out, float jx, float jy, float mvx, float mvy, int reset, float ms)
{
    if (!s_fx.ctx) return 0;
    struct ffxDispatchDescUpscale dd;
    memset(&dd, 0, sizeof dd);
    dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dd.commandList = s_d.cl;
    dd.color = d3e_fx_res(c, c->w, c->h);
    dd.depth = d3e_fx_res(d, d->w, d->h);
    dd.motionVectors = d3e_fx_res(mv, mv->w, mv->h);
    if (mask) dd.reactive = d3e_fx_res(mask, mask->w, mask->h);
    dd.output = d3e_fx_res(out, out->w, out->h);
    dd.jitterOffset.x = jx;
    dd.jitterOffset.y = jy;
    dd.motionVectorScale.x = mvx;
    dd.motionVectorScale.y = mvy;
    dd.renderSize.width = s_fx.rw;
    dd.renderSize.height = s_fx.rh;
    dd.upscaleSize.width = s_fx.w;
    dd.upscaleSize.height = s_fx.h;
    dd.enableSharpening = s_ua.sharp > 0.0f;
    dd.sharpness = s_ua.sharp;
    dd.frameTimeDelta = ms > 0.0f ? ms : 16.7f;
    dd.preExposure = 1.0f;
    dd.reset = reset != 0;
    /* Unknown for a recompiled game; FSR 3.1 only uses them for its depth heuristics. */
    dd.cameraNear = 0.1f;
    dd.cameraFar = 1000.0f;
    dd.cameraFovAngleVertical = 1.0f;
    dd.viewSpaceToMetersFactor = 1.0f;
    dd.flags = s_fx.hdr ? 0u : (uint32_t)FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    const ffxReturnCode_t rc = s_fx.dispatch(&s_fx.ctx, &dd.header);
    if (rc != FFX_API_RETURN_OK) {
        if (s_fx.errors++ < 4u) D3E_LOG("FSR 3.1 dispatch failed (code %u)\n", (unsigned)rc);
        return 0;
    }
    return 1;
}
#endif /* D3E_HAVE_FFX */

#if D3E_HAVE_XESS
/* Intel XeSS: libxess.dll's D3D12 entry points, resolved at run time. */
static struct {
    HMODULE dll;
    xess_result_t (*create)(ID3D12Device*, xess_context_handle_t*);
    xess_result_t (*init)(xess_context_handle_t, const xess_d3d12_init_params_t*);
    xess_result_t (*exec)(xess_context_handle_t, ID3D12GraphicsCommandList*, const xess_d3d12_execute_params_t*);
    xess_result_t (*destroy)(xess_context_handle_t);
    xess_result_t (*vscale)(xess_context_handle_t, float, float);
    xess_result_t (*jscale)(xess_context_handle_t, float, float);
    xess_context_handle_t ctx;
    float mvx, mvy;
    u32 w, h, rw, rh, errors;
} s_xs;

static int d3e_xs_load(void)
{
    s_xs.dll = d3e_load_beside("libxess.dll");
    if (!s_xs.dll) { D3E_LOG("libxess.dll not found next to the game: XeSS unavailable\n"); return 0; }
    s_xs.create = (xess_result_t (*)(ID3D12Device*, xess_context_handle_t*))(void*)
        GetProcAddress(s_xs.dll, "xessD3D12CreateContext");
    s_xs.init = (xess_result_t (*)(xess_context_handle_t, const xess_d3d12_init_params_t*))(void*)
        GetProcAddress(s_xs.dll, "xessD3D12Init");
    s_xs.exec = (xess_result_t (*)(xess_context_handle_t, ID3D12GraphicsCommandList*, const xess_d3d12_execute_params_t*))(void*)
        GetProcAddress(s_xs.dll, "xessD3D12Execute");
    s_xs.destroy = (xess_result_t (*)(xess_context_handle_t))(void*)GetProcAddress(s_xs.dll, "xessDestroyContext");
    s_xs.vscale = (xess_result_t (*)(xess_context_handle_t, float, float))(void*)GetProcAddress(s_xs.dll, "xessSetVelocityScale");
    s_xs.jscale = (xess_result_t (*)(xess_context_handle_t, float, float))(void*)GetProcAddress(s_xs.dll, "xessSetJitterScale");
    if (!s_xs.create || !s_xs.init || !s_xs.exec || !s_xs.destroy || !s_xs.vscale) {
        D3E_LOG("libxess.dll has no Direct3D 12 entry points: XeSS unavailable\n");
        FreeLibrary(s_xs.dll);
        memset(&s_xs, 0, sizeof s_xs);
        return 0;
    }
    return 1;
}

static void d3e_xs_destroy(void)
{
    if (s_xs.ctx && s_xs.destroy) s_xs.destroy(s_xs.ctx);
    s_xs.ctx = NULL;
}

static float d3e_env_sign(const char* name)
{
    const char* e = getenv(name);
    return e && atof(e) < 0.0 ? -1.0f : 1.0f;
}

static int d3e_xs_create(u32 rw, u32 rh, u32 w, u32 h, int flags, int mask)
{
    d3e_xs_destroy();
    xess_result_t r = s_xs.create(s_d.dev, &s_xs.ctx);
    if (r != XESS_RESULT_SUCCESS || !s_xs.ctx) {
        s_xs.ctx = NULL;
        D3E_LOG("XeSS context creation failed (code %d)\n", (int)r);
        return 0;
    }
    xess_d3d12_init_params_t ip;
    memset(&ip, 0, sizeof ip);
    ip.outputResolution.x = w;
    ip.outputResolution.y = h;
    ip.qualitySetting = (xess_quality_settings_t)rsx_d3d12_up_xess_quality(rw && rh ? (float)w / (float)rw : 1.0f);
    ip.initFlags = ((flags & 1) ? XESS_INIT_FLAG_INVERTED_DEPTH : 0u) |
                   ((flags & 2) ? XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE : XESS_INIT_FLAG_LDR_INPUT_COLOR) |
                   (mask ? XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK : 0u);
    r = s_xs.init(s_xs.ctx, &ip);
    if (r != XESS_RESULT_SUCCESS) {
        d3e_xs_destroy();
        D3E_LOG("XeSS initialisation failed (code %d)\n", (int)r);
        return 0;
    }
    if (s_xs.jscale) {
        const float sx = d3e_env_sign("RSX_XESS_JITTER_SIGN");
        s_xs.jscale(s_xs.ctx, sx, sx * d3e_env_sign("RSX_XESS_JITTER_SIGN_Y"));
    }
    s_xs.mvx = s_xs.mvy = 0.0f;
    s_xs.w = w; s_xs.h = h; s_xs.rw = rw; s_xs.rh = rh;
    D3E_LOG("XeSS (D3D12) native AA %ux%u -> %ux%u, quality %d (%s depth, %s colour%s)\n", rw, rh, w, h, (int)ip.qualitySetting, (flags & 1) ? "inverted" : "standard",
            (flags & 2) ? "HDR" : "LDR", mask ? ", responsive mask" : "");
    return 1;
}

static int d3e_xs_eval(const d3e_aimg* c, const d3e_aimg* d, const d3e_aimg* mv, const d3e_aimg* mask,
                       const d3e_aimg* out, float jx, float jy, float mvx, float mvy, int reset)
{
    if (!s_xs.ctx) return 0;
    if (mvx != s_xs.mvx || mvy != s_xs.mvy) {
        s_xs.vscale(s_xs.ctx, mvx, mvy);
        s_xs.mvx = mvx; s_xs.mvy = mvy;
    }
    xess_d3d12_execute_params_t ep;
    memset(&ep, 0, sizeof ep);
    ep.pColorTexture = c->res;
    ep.pVelocityTexture = mv->res;
    ep.pDepthTexture = d->res;
    ep.pResponsivePixelMaskTexture = mask ? mask->res : NULL;
    ep.pOutputTexture = out->res;
    ep.jitterOffsetX = jx;
    ep.jitterOffsetY = jy;
    ep.exposureScale = 1.0f;
    ep.resetHistory = reset ? 1u : 0u;
    ep.inputWidth = s_xs.rw;
    ep.inputHeight = s_xs.rh;
    const xess_result_t r = s_xs.exec(s_xs.ctx, s_d.cl, &ep);
    if (r != XESS_RESULT_SUCCESS) {
        if (s_xs.errors++ < 4u) D3E_LOG("XeSS execution failed (code %d)\n", (int)r);
        return 0;
    }
    return 1;
}
#endif /* D3E_HAVE_XESS */

static int d3e_up_load(int kind)
{
    switch (kind) {
    case D3E_UP_DLSS: return d3e_dl_load();
#if D3E_HAVE_FFX
    case D3E_UP_FSR3: return d3e_fx_load();
#endif
#if D3E_HAVE_XESS
    case D3E_UP_XESS: return d3e_xs_load();
#endif
    default:
        D3E_LOG("%s is not built into this Direct3D 12 renderer\n", kind >= 0 && kind < 4 ? s_up_name[kind] : "?");
        return 0;
    }
}

static int d3e_up_init(void)
{
    switch (s_ua.kind) {
    case D3E_UP_DLSS: return s_dl.init(s_d.dev, d3e_exe_dir_w(), d3e_dl_log) == 1;
    default: return 1;           /* FSR 3.1 and XeSS bind the device at context creation */
    }
}

static int d3e_up_create(u32 rw, u32 rh, u32 w, u32 h, int flags, int mask)
{
    (void)mask;
    switch (s_ua.kind) {
    case D3E_UP_DLSS: return s_dl.create(s_d.cl, rw, rh, w, h, flags, s_ua.preset) == 1;
#if D3E_HAVE_FFX
    case D3E_UP_FSR3: return d3e_fx_create(rw, rh, w, h, flags);
#endif
#if D3E_HAVE_XESS
    case D3E_UP_XESS: return d3e_xs_create(rw, rh, w, h, flags, mask);
#endif
    default: return 0;
    }
}

static int d3e_up_eval(const d3e_aimg* c, const d3e_aimg* d, const d3e_aimg* mv, const d3e_aimg* mask,
                       const d3e_aimg* out, u32 w, u32 h, float jx, float jy, float mvx, float mvy,
                       int reset, float ms)
{
    switch (s_ua.kind) {
    case D3E_UP_DLSS:
        return s_dl.eval(s_d.cl, c->res, d->res, mv->res, mask ? mask->res : NULL, out->res, w, h,
                         jx, jy, mvx, mvy, reset, ms) == 1;
#if D3E_HAVE_FFX
    case D3E_UP_FSR3: return d3e_fx_eval(c, d, mv, mask, out, jx, jy, mvx, mvy, reset, ms);
#endif
#if D3E_HAVE_XESS
    case D3E_UP_XESS: return d3e_xs_eval(c, d, mv, mask, out, jx, jy, mvx, mvy, reset);
#endif
    default: return 0;
    }
}

static void d3e_up_shutdown(void)
{
    switch (s_ua.kind) {
    case D3E_UP_DLSS: if (s_dl.shutdown) s_dl.shutdown(); break;
#if D3E_HAVE_FFX
    case D3E_UP_FSR3: d3e_fx_destroy(); break;
#endif
#if D3E_HAVE_XESS
    case D3E_UP_XESS: d3e_xs_destroy(); break;
#endif
    default: break;
    }
}

/* ---- pipeline objects ---------------------------------------------------------- */

static int d3e_ua_objects(void)
{
    if (s_ua.rs) return 0;
    if (s_ua.shaders_failed) return -1;
    D3D12_DESCRIPTOR_RANGE r = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
    D3D12_ROOT_PARAMETER p[2];
    memset(p, 0, sizeof p);
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.ShaderRegister = 0;
    p[0].Constants.Num32BitValues = 32;
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges = &r;
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC d = { 2, p, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    s_ua.shaders_failed = 1;
    s_ua.vs = d3e_compile(s_aa_hlsl, "vs", "vs_5_0", "rsx_d3d12_aa");
    for (u32 i = 0; i < D3E_AA_PS_COUNT; i++)
        s_ua.ps[i] = d3e_compile(s_aa_hlsl, s_aa_entry[i], "ps_5_0", "rsx_d3d12_aa");
    for (u32 i = 0; i < D3E_AA_PS_COUNT; i++) if (!s_ua.ps[i]) return -1;
    if (!s_ua.vs) return -1;
    s_ua.srv = d3e_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3E_AI_COUNT, 0);
    s_ua.rtv = d3e_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3E_AI_COUNT, 0);
    if (!s_ua.srv || !s_ua.rtv) return -1;
    s_ua.srv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(s_d.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    s_ua.rtv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(s_d.dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (FAILED(d3e_serialize_rs(&d, &s_ua.rs))) { s_ua.rs = NULL; return -1; }
    s_ua.shaders_failed = 0;
    return 0;
}

static ID3D12PipelineState* d3e_ua_pso(u32 ps, u32 nrt, const DXGI_FORMAT* rtf)
{
    u64 key = (u64)ps;
    for (u32 i = 0; i < nrt; i++) key |= (u64)(rtf[i] & 0xFFu) << (8u + 8u * i);
    for (u32 i = 0; i < s_ua.npso; i++) if (s_ua.pso[i].key == key) return s_ua.pso[i].pso;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = s_ua.rs;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(s_ua.vs);
    pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(s_ua.vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(s_ua.ps[ps]);
    pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(s_ua.ps[ps]);
    for (u32 i = 0; i < 8; i++) {
        D3D12_RENDER_TARGET_BLEND_DESC* b = &pd.BlendState.RenderTarget[i];
        b->SrcBlend = b->SrcBlendAlpha = D3D12_BLEND_ONE;
        b->DestBlend = b->DestBlendAlpha = D3D12_BLEND_ZERO;
        b->BlendOp = b->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b->LogicOp = D3D12_LOGIC_OP_NOOP;
        b->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    pd.BlendState.IndependentBlendEnable = FALSE;
    pd.SampleMask = 0xFFFFFFFFu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = nrt;
    for (u32 i = 0; i < nrt; i++) pd.RTVFormats[i] = rtf[i];
    pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
    const HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(s_d.dev, &pd, &IID_ID3D12PipelineState, (void**)&pso);
    if (FAILED(hr)) {
        D3E_LOG("upscaler PSO %s failed 0x%08lX\n", s_aa_entry[ps], (unsigned long)hr);
        return NULL;
    }
    if (s_ua.npso == sizeof s_ua.pso / sizeof s_ua.pso[0]) {
        for (u32 i = 0; i < s_ua.npso; i++) d3e_defer_release(s_ua.pso[i].pso);
        s_ua.npso = 0;
    }
    s_ua.pso[s_ua.npso].key = key;
    s_ua.pso[s_ua.npso].pso = pso;
    s_ua.npso++;
    return pso;
}

/* One full-screen pass of the upscaler shaders into `nrt` targets. srv: CPU
 * descriptors for t0..t2 (ptr 0 = a null SRV). */
static int d3e_ua_run(u32 ps, u32 nrt, const DXGI_FORMAT* rtf, const D3D12_CPU_DESCRIPTOR_HANDLE* rtv,
                      const D3D12_CPU_DESCRIPTOR_HANDLE srv[3], const float c[32],
                      u32 x, u32 y, u32 w, u32 h)
{
    ID3D12PipelineState* pso = d3e_ua_pso(ps, nrt, rtf);
    if (!pso) return 0;
    d3e_reserve(0, 3);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    d3e_desc_alloc(3, &cpu, &gpu);
    for (u32 i = 0; i < 3; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE dst = cpu;
        dst.ptr += (SIZE_T)i * s_d.srv_inc;
        ID3D12Device_CopyDescriptorsSimple(s_d.dev, 1, dst, srv[i].ptr ? srv[i] : d3e_srv_cpu(D3E_SRV_NULL + 0),
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    d3e_query_close_open();
    ID3D12GraphicsCommandList* cl = s_d.cl;
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, s_ua.rs);
    s_d.cur_rs = s_ua.rs;
    ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
    s_d.cur_pso = pso;
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 32, c, 0);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, gpu);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, nrt, rtv, FALSE, NULL);
    s_d.om_valid = 0;
    D3D12_VIEWPORT vp = { (float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f };
    D3D12_RECT sc = { (LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h) };
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
    d3e_query_reopen();
    return 1;
}

/* Image `i` at w x h in format f (recreated on a change). 1 recreated, 0 kept, -1 failed. */
static int d3e_ua_img(u32 i, DXGI_FORMAT f, u32 w, u32 h, int uav)
{
    d3e_aimg* im = &s_ua.img[i];
    if (im->res && im->fmt == f && im->w == w && im->h == h) return 0;
    if (im->res) d3e_defer_release(im->res);
    memset(im, 0, sizeof *im);
    const D3D12_RESOURCE_FLAGS fl = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const D3D12_RESOURCE_STATES st = uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_RENDER_TARGET;
    im->res = d3e_texture_resource(D3D12_RESOURCE_DIMENSION_TEXTURE2D, f, w, h, 1, 1, fl, st, NULL);
    if (!im->res) return -1;
    im->st = st; im->fmt = f; im->w = w; im->h = h; im->uav = (u32)uav;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = f;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(s_d.dev, im->res, &sd, d3e_ua_srv(i));
    if (!uav) ID3D12Device_CreateRenderTargetView(s_d.dev, im->res, NULL, d3e_ua_rtv(i));
    return 1;
}

static void d3e_ua_release_images(void)
{
    for (u32 i = 0; i < D3E_AI_COUNT; i++) {
        if (s_ua.img[i].res) d3e_release(s_ua.img[i].res);
        memset(&s_ua.img[i], 0, sizeof s_ua.img[i]);
    }
}

/* ---- the scene_aa hook ------------------------------------------------------------ */

static int d3e_scene_aa(void* user, const rsx_be_scene_aa* q)
{
    (void)user;
    if (!s_ua.kind || !s_ua.ready || s_ua.broken || !q || !s_d.up) return 0;
    d3e_obj* c = d3e_get(q->color, D3E_COLOR);
    d3e_obj* d = d3e_get(q->depth, D3E_DEPTH);
    if (!c || !d || !c->res || !d->res || !c->has_srv || !d->has_srv) return 0;
    const int f16 = c->fmt == RSX_BE_FMT_R16G16B16A16F;
    if (!f16 && c->fmt != RSX_BE_FMT_R8G8B8A8) return 0;
    u32 w = q->w ? q->w : c->w, h = q->h ? q->h : c->h;
    if (w > c->w) w = c->w;
    if (w > d->w) w = d->w;
    if (h > c->h) h = c->h;
    if (h > d->h) h = d->h;
    if (w < 16u || h < 16u) return 0;
    /* render scale: the scene was drawn into the top-left rw x rh of its targets */
    const float us = d3e_up_marked(c) ? s_up.u : 1.0f;
    const u32 rw = us < 1.0f ? rsx_d3d12_up_dim(w, us) : w, rh = us < 1.0f ? rsx_d3d12_up_dim(h, us) : h;
    if (d3e_ua_objects() != 0) {
        D3E_LOG("temporal upscaler: shader/pipeline setup failed, disabled\n");
        s_ua.broken = 1;
        return 0;
    }
    const int flags = (q->inverted ? 1 : 0) | (f16 ? 2 : 0);
    const u32 ckey = (u32)flags | (q->use_mask ? 4u : 0u);     /* XeSS fixes the mask at creation */
    const int recreate = !s_ua.created || s_ua.cw != w || s_ua.ch != h || s_ua.crw != rw || s_ua.crh != rh ||
                         s_ua.cflags != ckey;
    /* The provider's old context may still be in flight: idle the GPU first. */
    if (recreate && s_ua.created) { d3e_submit(1); s_ua.created = 0; }
    int e, resized = 0;
    if ((e = d3e_ua_img(D3E_AI_COLOR, DXGI_FORMAT_R16G16B16A16_FLOAT, rw, rh, 0)) < 0) goto fail;
    resized |= e;
    if ((e = d3e_ua_img(D3E_AI_DEPTH0, DXGI_FORMAT_R32_FLOAT, rw, rh, 0)) < 0) goto fail;
    resized |= e;
    if ((e = d3e_ua_img(D3E_AI_DEPTH1, DXGI_FORMAT_R32_FLOAT, rw, rh, 0)) < 0) goto fail;
    resized |= e;
    if ((e = d3e_ua_img(D3E_AI_MV, DXGI_FORMAT_R16G16_FLOAT, rw, rh, 0)) < 0) goto fail;
    resized |= e;
    if ((e = d3e_ua_img(D3E_AI_MASK, DXGI_FORMAT_R8_UNORM, rw, rh, 0)) < 0) goto fail;
    resized |= e;
    if ((e = d3e_ua_img(D3E_AI_OUT, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, 1)) < 0) goto fail;
    resized |= e;
    if (d3e_ua_img(D3E_AI_FIN, c->rfmt, w, h, 0) < 0) goto fail;
    if (resized) s_ua.prev_ok = 0;

    const u32 cur = D3E_AI_DEPTH0 + (s_ua.di & 1u), prv = D3E_AI_DEPTH0 + ((s_ua.di & 1u) ^ 1u);
    const float vw = (float)(q->w ? q->w : w), vh = (float)(q->h ? q->h : h);
    const int reset = q->reset || !s_ua.prev_ok;

    /* prep: scene colour/depth (+ last run's depth) -> colour, depth, mv, mask */
    d3e_trans(c, d3e_srv_state(c));
    d3e_trans(d, d3e_srv_state(d));
    const u32 outs[4] = { D3E_AI_COLOR, cur, D3E_AI_MV, D3E_AI_MASK };
    DXGI_FORMAT rtf[4];
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4];
    for (u32 i = 0; i < 4; i++) {
        d3e_aimg* im = &s_ua.img[outs[i]];
        d3e_barrier_res(im->res, &im->st, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtf[i] = im->fmt;
        rtv[i] = d3e_ua_rtv(outs[i]);
    }
    d3e_barrier_res(s_ua.img[prv].res, &s_ua.img[prv].st, D3E_SRV_STATES);
    float pc[32];
    memset(pc, 0, sizeof pc);
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) pc[col * 4 + row] = q->reproject[row * 4 + col];
    pc[16] = 2.0f * q->jitter_px[0] / vw; pc[17] = 2.0f * q->jitter_px[1] / vh;
    pc[18] = vw * us; pc[19] = vh * us;
    pc[20] = q->zA; pc[21] = q->zB; pc[22] = (float)rw; pc[23] = (float)rh;
    pc[24] = (q->use_mask && !reset) ? 1.0f : 0.0f; pc[25] = q->inverted ? 1.0f : 0.0f;
    D3D12_CPU_DESCRIPTOR_HANDLE srv[3] = { d3e_srv_cpu(q->color), d3e_srv_cpu(q->depth), d3e_ua_srv(prv) };
    if (!d3e_ua_run(D3E_AA_PREP, 4, rtf, rtv, srv, pc, 0, 0, rw, rh)) {
        D3E_LOG("temporal upscaler: prep pass unavailable, disabled\n");
        s_ua.broken = 1;
        return 0;
    }

    /* the provider: inputs NON_PIXEL_SHADER_RESOURCE, output UNORDERED_ACCESS */
    const u32 ins[4] = { D3E_AI_COLOR, cur, D3E_AI_MV, D3E_AI_MASK };
    for (u32 i = 0; i < 4; i++) {
        d3e_aimg* im = &s_ua.img[ins[i]];
        d3e_barrier_res(im->res, &im->st, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    d3e_barrier_res(s_ua.img[D3E_AI_OUT].res, &s_ua.img[D3E_AI_OUT].st, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    d3e_query_close_open();
    int ok = 1;
    if (recreate) {
        ok = d3e_up_create(rw, rh, w, h, flags, q->use_mask != 0);
        s_ua.created = ok; s_ua.cw = w; s_ua.ch = h; s_ua.crw = rw; s_ua.crh = rh; s_ua.cflags = ckey;
        if (!ok) { D3E_LOG("temporal upscaler: %s create failed, disabled\n", s_up_name[s_ua.kind]); s_ua.broken = 1; }
    }
    if (ok) {
        ok = d3e_up_eval(&s_ua.img[D3E_AI_COLOR], &s_ua.img[cur], &s_ua.img[D3E_AI_MV],
                         q->use_mask ? &s_ua.img[D3E_AI_MASK] : NULL, &s_ua.img[D3E_AI_OUT], w, h,
                         q->eval_jitter[0] * us, q->eval_jitter[1] * us, vw * us, vh * us, reset,
                         q->frame_ms > 0.0f ? q->frame_ms : 16.7f);
        if (!ok && ++s_ua.eval_fail >= 8) {
            D3E_LOG("temporal upscaler: evaluation keeps failing, disabled\n");
            s_ua.broken = 1;
        }
        if (ok)
            d3e_fg_prepare(&s_ua.img[cur], &s_ua.img[D3E_AI_MV], rw, rh, q->eval_jitter[0] * us,
                           q->eval_jitter[1] * us, vw * us, vh * us, reset, q->inverted,
                           q->frame_ms > 0.0f ? q->frame_ms : 16.7f);
    }
    /* the provider set its own heaps, root signature and pipeline */
    d3e_list_begin_state();
    d3e_query_reopen();
    if (ok) {
        s_ua.eval_fail = 0;
        /* UAV writes of the provider before the merge reads them */
        D3D12_RESOURCE_BARRIER ub;
        memset(&ub, 0, sizeof ub);
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = s_ua.img[D3E_AI_OUT].res;
        ID3D12GraphicsCommandList_ResourceBarrier(s_d.cl, 1, &ub);
        d3e_aimg* out = &s_ua.img[D3E_AI_OUT];
        d3e_aimg* fin = &s_ua.img[D3E_AI_FIN];
        d3e_barrier_res(out->res, &out->st, D3E_SRV_STATES);
        d3e_barrier_res(fin->res, &fin->st, D3D12_RESOURCE_STATE_RENDER_TARGET);
        d3e_trans(c, d3e_srv_state(c));
        float mc[32];
        memset(mc, 0, sizeof mc);
        mc[20] = 0.0f; mc[21] = (float)w; mc[22] = (float)h; mc[23] = (float)q->debug_view;
        mc[24] = us; mc[25] = us;
        D3D12_CPU_DESCRIPTOR_HANDLE ms[3] = { d3e_ua_srv(D3E_AI_OUT), d3e_srv_cpu(q->color), { 0 } };
        const DXGI_FORMAT ff = fin->fmt;
        const D3D12_CPU_DESCRIPTOR_HANDLE fr = d3e_ua_rtv(D3E_AI_FIN);
        if (d3e_ua_run(D3E_AA_MERGE, 1, &ff, &fr, ms, mc, 0, 0, w, h)) {
            d3e_barrier_res(fin->res, &fin->st, D3D12_RESOURCE_STATE_COPY_SOURCE);
            d3e_trans(c, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dl, sl;
            memset(&dl, 0, sizeof dl); memset(&sl, 0, sizeof sl);
            dl.pResource = c->res; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            sl.pResource = fin->res; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_BOX box = { 0, 0, 0, w, h, 1 };
            ID3D12GraphicsCommandList_CopyTextureRegion(s_d.cl, &dl, 0, 0, 0, &sl, &box);
            d3e_fg_scene(q->color, w, h);
            s_ua.prev_ok = 1;
            s_ua.di ^= 1u;
        } else ok = 0;
    }
    if (!ok) s_ua.prev_ok = 0;
    return ok;
fail:
    D3E_LOG("temporal upscaler: out of memory for %ux%u, disabled\n", w, h);
    s_ua.broken = 1;
    return 0;
}

/* ---- FSR1 output scaling ----------------------------------------------------------- */

static float d3e_fsr1_sharpness(void)
{
    static float sharp = -1.0f;
    if (sharp < 0.0f) {
        sharp = 0.5f;
        const char* s = getenv("RSX_SHARPEN");
        if (s && *s) {
            float v = (float)atof(s);
            if (v > 1.0f) v *= 0.01f;          /* a percentage, as RPCS3's setting */
            sharp = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        }
    }
    return sharp;
}

/* Present rectangle (sx, sy, sw, sh) of `o` (CPU SRV `srv`) into the back
 * buffer rtv (format rtf) at (lx, ly, lw, lh) through EASU + RCAS. 0 when
 * FSR1 does not apply (not upscaling, off, or unavailable): the caller
 * presents with its plain pass. */
static int d3e_fsr1_present(d3e_obj* o, u32 srv, u32 sx, u32 sy, u32 sw, u32 sh,
                            D3D12_CPU_DESCRIPTOR_HANDLE rtv, DXGI_FORMAT rtf,
                            u32 lx, u32 ly, u32 lw, u32 lh, int gm, float g)
{
    if (!(s_ua.fsr1_forced || d3e_present_linear() == 2) || !o) return 0;
    if (!(sw < lw && sh < lh)) return 0;           /* FSR only upscales */
    if (d3e_ua_objects() != 0) {
        D3E_WARN_ONCE(28, "FSR1 output scaling unavailable (shader setup failed): bilinear\n");
        return 0;
    }
    if (d3e_ua_img(D3E_AI_MID, DXGI_FORMAT_R8G8B8A8_UNORM, lw, lh, 0) < 0) return 0;
    d3e_aimg* mid = &s_ua.img[D3E_AI_MID];
    d3e_trans(o, d3e_srv_state(o));
    d3e_barrier_res(mid->res, &mid->st, D3D12_RESOURCE_STATE_RENDER_TARGET);
    float c[32];
    memset(c, 0, sizeof c);
    const float kx = (float)sw / (float)lw, ky = (float)sh / (float)lh;
    c[16] = kx; c[17] = ky;
    c[18] = 0.5f * kx - 0.5f + (float)sx; c[19] = 0.5f * ky - 0.5f + (float)sy;
    c[20] = (float)sx; c[21] = (float)sy; c[22] = (float)(sx + sw - 1u); c[23] = (float)(sy + sh - 1u);
    c[24] = 0.0f; c[25] = (float)lw; c[26] = (float)lh;
    D3D12_CPU_DESCRIPTOR_HANDLE s1[3] = { d3e_srv_cpu(srv), { 0 }, { 0 } };
    const DXGI_FORMAT mf = mid->fmt;
    const D3D12_CPU_DESCRIPTOR_HANDLE mr = d3e_ua_rtv(D3E_AI_MID);
    if (!d3e_ua_run(D3E_AA_EASU, 1, &mf, &mr, s1, c, 0, 0, lw, lh)) return 0;
    d3e_barrier_res(mid->res, &mid->st, D3E_SRV_STATES);
    memset(c, 0, sizeof c);
    c[20] = 0.0f; c[21] = 0.0f; c[22] = (float)(lw - 1u); c[23] = (float)(lh - 1u);
    c[24] = exp2f(-(2.0f - 2.0f * d3e_fsr1_sharpness()));
    c[28] = (float)lx; c[29] = (float)ly; c[30] = gm ? 1.0f : 0.0f; c[31] = g;
    D3D12_CPU_DESCRIPTOR_HANDLE s2[3] = { d3e_ua_srv(D3E_AI_MID), { 0 }, { 0 } };
    return d3e_ua_run(D3E_AA_RCAS, 1, &rtf, &rtv, s2, c, lx, ly, lw, lh);
}

/* ---- lifetime ------------------------------------------------------------------------ */

/* End of d3e_device_create (not for the probe's throwaway device). */
static void d3e_ua_device_up(void)
{
    s_ua.ready = s_ua.broken = s_ua.created = s_ua.prev_ok = s_ua.eval_fail = 0;
    s_ua.di = 0;
    if (!s_ua.kind || !s_ua.loaded || s_d3e_probing) return;
    if (!d3e_up_init()) {
        D3E_LOG("%s unavailable on this device: no temporal AA\n", s_up_name[s_ua.kind]);
        s_ua.broken = 1;
        return;
    }
    s_ua.ready = 1;
    D3E_LOG("temporal upscaler: %s (native AA)\n", s_up_name[s_ua.kind]);
}

/* d3e_device_destroy, after the GPU went idle. */
static void d3e_ua_device_down(void)
{
    d3e_up_reset();
    if (s_ua.ready) d3e_up_shutdown();
    s_ua.ready = s_ua.created = 0;
    d3e_ua_release_images();
    for (u32 i = 0; i < s_ua.npso; i++) d3e_release(s_ua.pso[i].pso);
    s_ua.npso = 0;
    for (u32 i = 0; i < D3E_AA_PS_COUNT; i++) { d3e_release(s_ua.ps[i]); s_ua.ps[i] = NULL; }
    d3e_release(s_ua.vs); s_ua.vs = NULL;
    d3e_release(s_ua.rs); s_ua.rs = NULL;
    d3e_release(s_ua.srv); s_ua.srv = NULL;
    d3e_release(s_ua.rtv); s_ua.rtv = NULL;
    s_ua.shaders_failed = 0;
}

/* ---- public API ------------------------------------------------------------------------ */

int rsx_d3d12_engine_use_upscaler(int kind, float sharpness, unsigned dlss_preset)
{
    if (kind < D3E_UP_DLSS || kind > D3E_UP_XESS) return 0;
    s_ua.kind = kind;
    s_ua.sharp = sharpness < 0.0f ? 0.0f : sharpness > 1.0f ? 1.0f : sharpness;
    s_ua.preset = dlss_preset;
    s_ua.loaded = d3e_up_load(kind);
    if (!s_ua.loaded) s_ua.kind = D3E_UP_NONE;
    return s_ua.loaded;
}

int rsx_d3d12_engine_temporal_upscaler_ready(void)
{
    return s_ua.kind && s_ua.ready && !s_ua.broken;
}

void rsx_d3d12_engine_force_output_fsr1(void)
{
    if (!s_ua.fsr1_forced) D3E_LOG("output scaling: FSR1 (EASU + RCAS), RCAS sharpening %.2f\n", (double)d3e_fsr1_sharpness());
    s_ua.fsr1_forced = 1;
}
