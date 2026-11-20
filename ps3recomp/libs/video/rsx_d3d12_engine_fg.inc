/* rsx_d3d12_engine_fg.inc -- included by rsx_d3d12_engine.c (after aa.inc / hdr.inc).
 *
 * AMD FSR 3 frame generation (frame interpolation, 2x) for the Direct3D 12
 * renderer, after R1's rsx_fsr3fg.inc:
 *
 *   R?_DLSS_FG   2 / 3 / 4 / fsr3, R?_FG_BACKEND, PORT_FG=fsr3|dlssg|off  (as the Vulkan path;
 *                DLSS Frame Generation is not ported to Direct3D 12: any request runs FSR 3 2x)
 *   PORT_FG_HUDLESS / R?_DLSS_FG_HUD  1 (default): the HUD-less scene goes to FidelityFX's UI
 *                extraction, 0: the whole frame is interpolated
 *   R?_DLSS_FG_DEBUG=2  FidelityFX's tear lines and reset indicators
 *
 * Needs a temporal AA / upscaler (DLSS, FSR 3.1 or XeSS: their depth, motion
 * vectors, jitter and reset are reused), SDR or HDR10 (not scRGB), and
 * amd_fidelityfx_loader_dx12.dll + amd_fidelityfx_framegeneration_dx12.dll
 * (AMD FidelityFX SDK 2.3.0, MIT) next to the game.
 *
 * Wiring: the swap chain is FidelityFX's frame-interpolation swap chain
 * (CreateSwapChainForHwnd replacement on the game queue). scene_aa dispatches
 * the "prepare" with the upscaler inputs; Present (on the game queue, no async
 * compute) interpolates. The HUD-less image is the scene right after the
 * upscaler (D3E_AI_FIN) run through the same present mapping; FidelityFX keeps
 * whatever differs from it (the HUD) out of generated frames. When that image
 * cannot match the presented frame (FSR1 output scaling, HDR, another surface)
 * the whole frame is interpolated. Unverified in game. */

#include "../../third_party/rpcs3/rpcs3/Emu/RSX/VK/VKR2FrameGenLogic.h"

#if D3E_HAVE_FFX
#include "../../../tools/ffx_vk/include/ffx_api/ffx_framegeneration.h"
#include "../../../tools/ffx_vk/include/ffx_api/dx12/ffx_api_dx12.h"

/* SDK 2.3.0 additions over the vendored 1.1.4 headers (ffx_framegeneration.h,
 * dx12/ffx_api_framegeneration_dx12.h of 2.3.0, MIT; layouts as R1 pins them). */
#define D3E_FG_PREPARE_V2   0x0002000cu
#define D3E_FG_CREATE_VER   0x0002000eu
#define D3E_FG_VERSION      ((4u << 22) | (0u << 12) | 1u)
#define D3E_FG_SC_VER       0x0003000bu
#define D3E_FG_SC_VERSION   ((3u << 22) | (1u << 12) | 7u)
struct d3e_fg_prepare_v2 {
    ffxDispatchDescHeader header;
    uint64_t frameID;
    uint32_t flags;
    void* commandList;
    struct FfxApiDimensions2D renderSize;
    struct FfxApiFloatCoords2D jitterOffset;
    struct FfxApiFloatCoords2D motionVectorScale;
    float frameTimeDelta;
    bool reset;
    float cameraNear, cameraFar, cameraFovAngleVertical, viewSpaceToMetersFactor;
    struct FfxApiResource depth;
    struct FfxApiResource motionVectors;
    float cameraPosition[3], cameraUp[3], cameraRight[3], cameraForward[3];
};
struct d3e_fg_ver { ffxCreateContextDescHeader header; uint32_t version; };

#define D3E_FG_SA(c) _Static_assert(c, #c)
D3E_FG_SA(sizeof(struct FfxApiResource) == 48);
D3E_FG_SA(sizeof(struct ffxCreateBackendDX12Desc) == 24);
D3E_FG_SA(sizeof(struct ffxCreateContextDescFrameGeneration) == 40);
D3E_FG_SA(sizeof(struct d3e_fg_ver) == 24);
D3E_FG_SA(sizeof(ffxDispatchDescFrameGeneration) == 312 && offsetof(ffxDispatchDescFrameGeneration, reset) == 268 &&
          offsetof(ffxDispatchDescFrameGeneration, frameID) == 304);
D3E_FG_SA(sizeof(struct ffxConfigureDescFrameGeneration) == 144 &&
          offsetof(struct ffxConfigureDescFrameGeneration, frameGenerationEnabled) == 56 &&
          offsetof(struct ffxConfigureDescFrameGeneration, HUDLessColor) == 64 &&
          offsetof(struct ffxConfigureDescFrameGeneration, flags) == 112 &&
          offsetof(struct ffxConfigureDescFrameGeneration, generationRect) == 120 &&
          offsetof(struct ffxConfigureDescFrameGeneration, frameID) == 136);
D3E_FG_SA(sizeof(struct d3e_fg_prepare_v2) == 232 && offsetof(struct d3e_fg_prepare_v2, commandList) == 32 &&
          offsetof(struct d3e_fg_prepare_v2, frameTimeDelta) == 64 && offsetof(struct d3e_fg_prepare_v2, reset) == 68 &&
          offsetof(struct d3e_fg_prepare_v2, viewSpaceToMetersFactor) == 84 &&
          offsetof(struct d3e_fg_prepare_v2, depth) == 88 && offsetof(struct d3e_fg_prepare_v2, motionVectors) == 136 &&
          offsetof(struct d3e_fg_prepare_v2, cameraForward) == 220);
D3E_FG_SA(sizeof(struct ffxCreateContextDescFrameGenerationSwapChainForHwndDX12) == 64 &&
          offsetof(struct ffxCreateContextDescFrameGenerationSwapChainForHwndDX12, gameQueue) == 56);
#undef D3E_FG_SA

static struct {
    int decided, want, hud, dbgflags;
    HMODULE dll;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxConfigure configure;
    PfnFfxDispatch dispatch;
    ffxContext sc_ctx, ctx;
    IDXGISwapChain4* sc4;           /* the frame-interpolation swap chain (one reference) */
    int ready, ctx_fail, on, prev_hl;
    volatile LONG reset_gen;      /* set on the render thread, consumed in the FFX generate callback */
    u32 streak, presents, gen_n;
    u64 fid;                        /* frame ID of the last configured present */
    /* the context: display size, largest render size, HDR10, inverted depth */
    u32 dw, dh, mw, mh, hdr, inv;
    /* requested by this frame's prepare when the context does not fit */
    int need;
    u32 need_rw, need_rh, need_inv;
    int tagged;                     /* this frame's prepare ran */
    u32 scene, scene_w, scene_h, scene_frame;
    int hl_ok, menu_off, why;
} s_fg;

/* game side, as VKR2FrameGen.cpp declares them (its defaults cover builds without them) */
extern volatile int g_r2pc_paused;
extern volatile int g_port_pause_ui;
extern volatile unsigned long long g_r2_look_ms, g_port_play_ms;
int port_local_players(void);
#pragma comment(linker, "/alternatename:g_r2_look_ms=r2fg_look_ms_default")
#pragma comment(linker, "/alternatename:g_r2pc_paused=r2fg_paused_default")
#pragma comment(linker, "/alternatename:port_local_players=r2fg_local_players_default")

static void d3e_fg_decide(void)
{
    s_fg.decided = 1;
    int want = 0, backend = 0, bad_mult = 0, bad_backend = 0;
    pfg_parse_vk_legacy(d3e_penv("DLSS_FG"), d3e_penv("FG_BACKEND"), &want, &backend, &bad_mult, &bad_backend);
    const char* sel = getenv("PORT_FG");
    const int s = pfg_parse_sel(sel);
    if (s != PFG_SEL_BAD && s != PFG_SEL_LEGACY) pfg_apply_sel(s, &want, &backend);
    if (!want) return;
    if (backend != PFG_BE_FSR3 || want != 2)
        D3E_LOG("frame generation: DLSS Frame Generation and 3x/4x are not available with Direct3D 12, using AMD FSR 3 2x\n");
    if (!s_ua.kind || !s_ua.loaded) {
        D3E_LOG("frame generation disabled: it needs a temporal anti-aliasing / upscaling mode (FSR 3.1, DLSS or XeSS)\n");
        return;
    }
    if (d3e_hdr_swap_format() == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        D3E_LOG("frame generation disabled: not available with scRGB HDR output (use HDR10 or SDR)\n");
        return;
    }
    char p[MAX_PATH * 2];
    snprintf(p, sizeof p, "%s%s", d3e_exe_dir(), "amd_fidelityfx_framegeneration_dx12.dll");
    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) {
        D3E_LOG("frame generation disabled: amd_fidelityfx_framegeneration_dx12.dll is not next to the game\n");
        return;
    }
    s_fg.dll = d3e_load_beside("amd_fidelityfx_loader_dx12.dll");
    if (!s_fg.dll) { D3E_LOG("frame generation disabled: amd_fidelityfx_loader_dx12.dll is missing\n"); return; }
    s_fg.create = (PfnFfxCreateContext)(void*)GetProcAddress(s_fg.dll, "ffxCreateContext");
    s_fg.destroy = (PfnFfxDestroyContext)(void*)GetProcAddress(s_fg.dll, "ffxDestroyContext");
    s_fg.configure = (PfnFfxConfigure)(void*)GetProcAddress(s_fg.dll, "ffxConfigure");
    s_fg.dispatch = (PfnFfxDispatch)(void*)GetProcAddress(s_fg.dll, "ffxDispatch");
    if (!s_fg.create || !s_fg.destroy || !s_fg.configure || !s_fg.dispatch) {
        D3E_LOG("frame generation disabled: amd_fidelityfx_loader_dx12.dll is too old\n");
        return;
    }
    const char* hd = d3e_penv("DLSS_FG_HUD");
    s_fg.hud = pfg_hudless(getenv("PORT_FG_HUDLESS"), hd ? atoi(hd) != 0 : 1);
    const char* mo = d3e_penv("DLSS_FG_MENU_OFF");
    s_fg.menu_off = mo ? atoi(mo) != 0 : 1;
    s_fg.why = -1;
    const char* dbg = d3e_penv("DLSS_FG_DEBUG");
    s_fg.dbgflags = (dbg && atoi(dbg) >= 2)
        ? (int)(FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS) : 0;
    s_fg.want = 1;
}

static void d3e_fg_wait_presents(void)
{
    if (!s_fg.sc_ctx) return;
    struct ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 w;
    memset(&w, 0, sizeof w);
    w.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
    s_fg.dispatch(&s_fg.sc_ctx, &w.header);
}

static ffxReturnCode_t d3e_fg_generate_cb(ffxDispatchDescFrameGeneration* d, void* user)
{
    (void)user;
    if (InterlockedExchange(&s_fg.reset_gen, 0)) d->reset = true;
    return s_fg.dispatch(&s_fg.ctx, &d->header);
}

static void d3e_fg_configure(int enabled, const struct FfxApiRect2D* rect, const d3e_aimg* hl)
{
    struct ffxConfigureDescFrameGeneration c;
    memset(&c, 0, sizeof c);
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = s_fg.sc4;
    c.frameGenerationCallback = enabled ? d3e_fg_generate_cb : NULL;
    c.frameGenerationEnabled = enabled != 0;
    c.allowAsyncWorkloads = false;
    if (hl && hl->res) {
        c.HUDLessColor = d3e_fx_res(hl, hl->w, hl->h);
        c.HUDLessColor.description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
        c.HUDLessColor.state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    }
    c.flags = (uint32_t)s_fg.dbgflags;
    if (rect) c.generationRect = *rect;
    else { c.generationRect.width = (int32_t)s_d.swap_w; c.generationRect.height = (int32_t)s_d.swap_h; }
    c.frameID = s_fg.fid;
    const ffxReturnCode_t rc = s_fg.configure(&s_fg.ctx, &c.header);
    if (rc != FFX_API_RETURN_OK) D3E_WARN_ONCE(40, "frame generation: configure failed (code %u)\n", (unsigned)rc);
}

static void d3e_fg_ctx_drop(void)
{
    if (!s_fg.ctx) return;
    d3e_fg_configure(0, NULL, NULL);       /* the swap chain must not call into the context any more */
    d3e_fg_wait_presents();
    s_fg.destroy(&s_fg.ctx, NULL);
    s_fg.ctx = NULL;
    s_fg.on = 0;
    s_fg.streak = 0;
}

static uint32_t d3e_fg_format(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R10G10B10A2_UNORM ? (uint32_t)FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM
                                              : (uint32_t)FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
}

static int d3e_fg_ctx_make(u32 rw, u32 rh, u32 inv)
{
    struct ffxCreateBackendDX12Desc be;
    memset(&be, 0, sizeof be);
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    be.device = s_d.dev;
    struct d3e_fg_ver ver;
    memset(&ver, 0, sizeof ver);
    ver.header.type = D3E_FG_CREATE_VER;
    ver.header.pNext = &be.header;
    ver.version = D3E_FG_VERSION;
    struct ffxCreateContextDescFrameGeneration cd;
    memset(&cd, 0, sizeof cd);
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    cd.header.pNext = &ver.header;
    const u32 hdr = s_hdr.active == 1;
    cd.flags = (hdr ? (uint32_t)FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0u) |
               (inv ? (uint32_t)FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0u);
    const u32 mw = rw > s_d.swap_w ? rw : s_d.swap_w, mh = rh > s_d.swap_h ? rh : s_d.swap_h;
    cd.displaySize.width = s_d.swap_w; cd.displaySize.height = s_d.swap_h;
    cd.maxRenderSize.width = mw; cd.maxRenderSize.height = mh;
    cd.backBufferFormat = d3e_fg_format(d3e_hdr_back_format());
    const ffxReturnCode_t rc = s_fg.create(&s_fg.ctx, &cd.header, NULL);
    if (rc != FFX_API_RETURN_OK || !s_fg.ctx) {
        s_fg.ctx = NULL;
        D3E_LOG("frame generation disabled: the frame generation context could not be created (code %u)\n", (unsigned)rc);
        return 0;
    }
    s_fg.dw = s_d.swap_w; s_fg.dh = s_d.swap_h; s_fg.mw = mw; s_fg.mh = mh; s_fg.hdr = hdr; s_fg.inv = inv;
    return 1;
}

/* d3e_swap_ensure, instead of CreateSwapChainForHwnd. 0: make the normal swap chain. */
static int d3e_fg_swapchain(DXGI_SWAP_CHAIN_DESC1* sd, IDXGISwapChain1** out)
{
    if (s_d3e_probing) return 0;
    if (!s_fg.decided) d3e_fg_decide();
    if (!s_fg.want) return 0;
    IDXGISwapChain4* sc4 = NULL;
    struct d3e_fg_ver ver;
    memset(&ver, 0, sizeof ver);
    ver.header.type = D3E_FG_SC_VER;
    ver.version = D3E_FG_SC_VERSION;
    struct ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 cd;
    memset(&cd, 0, sizeof cd);
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    cd.header.pNext = &ver.header;
    cd.swapchain = &sc4;
    cd.hwnd = s_d.hwnd;
    cd.desc = sd;
    cd.fullscreenDesc = NULL;
    cd.dxgiFactory = (IDXGIFactory*)s_d.factory;
    cd.gameQueue = s_d.queue;
    const ffxReturnCode_t rc = s_fg.create(&s_fg.sc_ctx, &cd.header, NULL);
    if (rc != FFX_API_RETURN_OK || !sc4) {
        if (s_fg.sc_ctx) s_fg.destroy(&s_fg.sc_ctx, NULL);
        s_fg.sc_ctx = NULL;
        s_fg.want = 0;
        D3E_LOG("frame generation disabled: the frame interpolation swap chain could not be created (code %u)\n",
                (unsigned)rc);
        return 0;
    }
    if (FAILED(IDXGISwapChain4_QueryInterface(sc4, &IID_IDXGISwapChain1, (void**)out)) || !*out) {
        IDXGISwapChain4_Release(sc4);
        s_fg.destroy(&s_fg.sc_ctx, NULL);
        s_fg.sc_ctx = NULL;
        s_fg.want = 0;
        D3E_LOG("frame generation disabled: the frame interpolation swap chain has no IDXGISwapChain1\n");
        return 0;
    }
    s_fg.sc4 = sc4;
    s_fg.ready = 1;
    s_fg.ctx_fail = 0;
    s_fg.fid = 0;
    D3E_LOG("frame generation: AMD FSR 3 2x ready (HUD-less input %s)\n", s_fg.hud ? "on" : "off");
    return 1;
}

/* Before ResizeBuffers (GPU idle). */
static void d3e_fg_before_resize(void)
{
    if (!s_fg.ready) return;
    d3e_fg_ctx_drop();
    d3e_fg_wait_presents();
    s_fg.ctx_fail = 0;
}

/* Before the swap chain is released (GPU idle). */
static void d3e_fg_swap_down(void)
{
    if (!s_fg.ready) return;
    d3e_fg_ctx_drop();
    struct ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 ru;
    memset(&ru, 0, sizeof ru);
    ru.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
    s_fg.configure(&s_fg.sc_ctx, &ru.header);
    d3e_fg_wait_presents();
    if (s_fg.sc4) { IDXGISwapChain4_Release(s_fg.sc4); s_fg.sc4 = NULL; }
    s_fg.destroy(&s_fg.sc_ctx, NULL);
    s_fg.sc_ctx = NULL;
    s_fg.ready = 0;
    s_fg.on = 0;
    s_fg.streak = 0;
    s_fg.tagged = 0;
}

/* scene_aa, after the provider ran (its inputs in NON_PIXEL_SHADER_RESOURCE),
 * on the frame's command list, before the engine state is restored. */
static void d3e_fg_prepare(const d3e_aimg* depth, const d3e_aimg* mv, u32 rw, u32 rh, float jx, float jy,
                           float mvx, float mvy, int reset, int inverted, float ms)
{
    if (!s_fg.ready || s_fg.tagged || s_fg.ctx_fail) return;
    const u32 inv = inverted ? 1u : 0u;
    if (!s_fg.ctx || s_fg.dw != s_d.swap_w || s_fg.dh != s_d.swap_h || rw > s_fg.mw || rh > s_fg.mh ||
        s_fg.hdr != (u32)(s_hdr.active == 1) || s_fg.inv != inv) {
        s_fg.need = 1;                     /* made at present, with the GPU idle */
        s_fg.need_rw = rw; s_fg.need_rh = rh; s_fg.need_inv = inv;
        return;
    }
    struct d3e_fg_prepare_v2 pd;
    memset(&pd, 0, sizeof pd);
    pd.header.type = D3E_FG_PREPARE_V2;
    pd.frameID = s_fg.fid + 1u;
    pd.commandList = s_d.cl;
    pd.renderSize.width = rw; pd.renderSize.height = rh;
    pd.jitterOffset.x = jx; pd.jitterOffset.y = jy;
    pd.motionVectorScale.x = mvx; pd.motionVectorScale.y = mvy;
    pd.frameTimeDelta = ms > 0.0f ? ms : 16.7f;
    pd.reset = reset != 0;
    pd.cameraNear = 0.1f; pd.cameraFar = 1000.0f;      /* unknown for a recompiled game (as the upscaler) */
    pd.cameraFovAngleVertical = 1.0f;
    pd.viewSpaceToMetersFactor = 1.0f;
    pd.depth = d3e_fx_res(depth, rw, rh);
    pd.motionVectors = d3e_fx_res(mv, rw, rh);
    const ffxReturnCode_t rc = s_fg.dispatch(&s_fg.ctx, &pd.header);
    if (rc != FFX_API_RETURN_OK) {
        D3E_WARN_ONCE(41, "frame generation: prepare failed (code %u)\n", (unsigned)rc);
        return;
    }
    s_fg.tagged = 1;
    if (reset) InterlockedExchange(&s_fg.reset_gen, 1);
}

/* scene_aa, after the merge: D3E_AI_FIN holds this frame's scene without what is drawn later (the HUD). */
static void d3e_fg_scene(u32 color, u32 w, u32 h)
{
    s_fg.scene = color; s_fg.scene_w = w; s_fg.scene_h = h;
    s_fg.scene_frame = s_d.presents + 1u;
}

/* d3e_present_region, after the present pass into the SDR back buffer: the
 * HUD-less image with the same mapping (rect r* of `surface` -> letterbox l*). */
static void d3e_fg_hudless(u32 surface, u32 rx, u32 ry, u32 rw, u32 rh, u32 lx, u32 ly, u32 lw, u32 lh,
                           int linear, int gm, float g, int other_path)
{
    s_fg.hl_ok = 0;
    if (!s_fg.ready || !s_fg.hud || !s_fg.ctx || other_path || s_hdr.active) return;
    const d3e_aimg* fin = &s_ua.img[D3E_AI_FIN];
    if (s_fg.scene != surface || s_fg.scene_frame != s_d.presents + 1u || !fin->res ||
        rx + rw > fin->w || ry + rh > fin->h || !rw || !rh) return;
    if (d3e_ua_img(D3E_AI_HUDLESS, d3e_hdr_back_format(), s_d.swap_w, s_d.swap_h, 0) < 0) return;
    d3e_aimg* hl = &s_ua.img[D3E_AI_HUDLESS];
    d3e_aimg* f = &s_ua.img[D3E_AI_FIN];
    d3e_barrier_res(f->res, &f->st, D3E_SRV_STATES);
    d3e_barrier_res(hl->res, &hl->st, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ID3D12GraphicsCommandList_ClearRenderTargetView(s_d.cl, d3e_ua_rtv(D3E_AI_HUDLESS), black, 0, NULL);
    d3e_pass_args a;
    d3e_pass_args_init(&a, linear ? D3E_PS_PRESENT_LINEAR : D3E_PS_PRESENT_POINT);
    a.rtf = hl->fmt;
    a.rtv = d3e_ua_rtv(D3E_AI_HUDLESS);
    a.srv0_cpu = d3e_ua_srv(D3E_AI_FIN);
    a.c[0] = (float)rx / (float)f->w; a.c[1] = (float)ry / (float)f->h;
    a.c[2] = (float)rw / (float)f->w; a.c[3] = (float)rh / (float)f->h;
    a.c[4] = gm ? 1.0f : 0.0f; a.c[5] = g;
    d3e_pass_target(&a, lx, ly, lw, lh);
    if (!d3e_pass_run(&a)) return;
    d3e_barrier_res(hl->res, &hl->st, D3E_SRV_STATES);
    s_fg.hl_ok = 1;
}

/* d3e_present_region, after the frame was submitted and before Present. */
static void d3e_fg_present(int presented, u32 lx, u32 ly, u32 lw, u32 lh)
{
    if (!s_fg.ready) return;
    if (s_fg.need && !s_fg.ctx_fail) {
        s_fg.need = 0;
        d3e_wait_fence(s_d.fence_val);
        d3e_fg_ctx_drop();
        if (!d3e_fg_ctx_make(s_fg.need_rw, s_fg.need_rh, s_fg.need_inv)) s_fg.ctx_fail = 1;
    }
    ++s_fg.fid;
    const HWND root = GetAncestor(s_d.hwnd, GA_ROOT);
    const HWND top = root ? root : s_d.hwnd;
    const int active = !IsIconic(top) && GetForegroundWindow() == top;
    /* the Vulkan path's gate (VKR2FrameGenLogic.h): pause, split screen, menus, warm-up */
    pfg_gate_in g;
    memset(&g, 0, sizeof g);
    g.scene = s_fg.tagged && s_fg.ctx && presented && !s_d3e_repost;   /* UI-only re-present: never generate */
    if (!s_d3e_repost) s_fg.streak = pfg_streak(s_fg.streak, g.scene);
    g.paused = g_r2pc_paused != 0 || !active;
    g.local_players = (g.scene && !g.paused) ? port_local_players() : 1;
    g.menu_off = s_fg.menu_off;
    g.pause_ui = g_port_pause_ui != 0;
    const unsigned long long look = g_r2_look_ms, play = g_port_play_ms;
    g.activity_ms = look > play ? look : play;
    g.now_ms = GetTickCount64();
    g.streak = s_fg.streak;
    const int why = pfg_gate(&g);
    const int on = why == PFG_GATE_ON;
    if (why != s_fg.why) {
        static u32 n;
        static const char* const txt[] = { "on", "warming up", "no 3D scene this frame (menu, movie or loading screen)",
                                           "paused / window in the background", "split screen", "scRGB", "sRGB",
                                           "swap chain", "menu (no look input)", "error" };
        if (n < 32u) {
            n++;
            if (on) D3E_LOG("frame generation on: 2x, %ux%u%s\n", s_d.swap_w, s_d.swap_h,
                            s_fg.hl_ok ? ", HUD kept out of generated frames" : "");
            else if (why != PFG_GATE_WARMUP)
                D3E_LOG("frame generation off: %s\n", (unsigned)why < sizeof txt / sizeof txt[0] ? txt[why] : "?");
        }
        s_fg.why = why;
    }
    if (on != s_fg.on) InterlockedExchange(&s_fg.reset_gen, 1);
    const int hl = on && s_fg.hl_ok;
    if (hl != s_fg.prev_hl) InterlockedExchange(&s_fg.reset_gen, 1);
    s_fg.prev_hl = hl;
    s_fg.on = on;
    if (s_fg.ctx) {
        struct FfxApiRect2D r;
        r.left = (int32_t)lx; r.top = (int32_t)ly; r.width = (int32_t)lw; r.height = (int32_t)lh;
        if (!presented || !lw || !lh) { r.left = r.top = 0; r.width = (int32_t)s_d.swap_w; r.height = (int32_t)s_d.swap_h; }
        d3e_fg_configure(on, &r, hl ? &s_ua.img[D3E_AI_HUDLESS] : NULL);
    }
    if (on) s_fg.gen_n++;
    if ((++s_fg.presents % 6000u) == 0u) {
        D3E_LOG("frame generation: %u of the last 6000 presents had a generated frame\n", s_fg.gen_n);
        s_fg.gen_n = 0;
    }
    s_fg.tagged = 0;
}

int rsx_d3d12_engine_fg_active(void) { return s_fg.on; }

#else  /* !D3E_HAVE_FFX */

static int  d3e_fg_swapchain(DXGI_SWAP_CHAIN_DESC1* sd, IDXGISwapChain1** out) { (void)sd; (void)out; return 0; }
static void d3e_fg_before_resize(void) {}
static void d3e_fg_swap_down(void) {}
static void d3e_fg_prepare(const d3e_aimg* depth, const d3e_aimg* mv, u32 rw, u32 rh, float jx, float jy,
                           float mvx, float mvy, int reset, int inverted, float ms)
{ (void)depth; (void)mv; (void)rw; (void)rh; (void)jx; (void)jy; (void)mvx; (void)mvy; (void)reset; (void)inverted; (void)ms; }
static void d3e_fg_scene(u32 color, u32 w, u32 h) { (void)color; (void)w; (void)h; }
static void d3e_fg_hudless(u32 surface, u32 rx, u32 ry, u32 rw, u32 rh, u32 lx, u32 ly, u32 lw, u32 lh,
                           int linear, int gm, float g, int other_path)
{ (void)surface; (void)rx; (void)ry; (void)rw; (void)rh; (void)lx; (void)ly; (void)lw; (void)lh; (void)linear; (void)gm; (void)g; (void)other_path; }
static void d3e_fg_present(int presented, u32 lx, u32 ly, u32 lw, u32 lh)
{ (void)presented; (void)lx; (void)ly; (void)lw; (void)lh; }
int rsx_d3d12_engine_fg_active(void) { return 0; }

#endif /* D3E_HAVE_FFX */
