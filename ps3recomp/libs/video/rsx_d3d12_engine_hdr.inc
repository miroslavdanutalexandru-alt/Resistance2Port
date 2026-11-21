/* rsx_d3d12_engine_hdr.inc -- included by rsx_d3d12_engine.c after _aa.inc.
 *
 * HDR output, R1 rsx_d3d12_backend.c's scheme: the frame is presented as
 * before (SDR, gamma-encoded) into an RGBA8 image at the swap chain size, then
 * one pass keeps it at paper white, expands only the highlights towards the
 * display peak and encodes HDR10 (PQ, R10G10B10A2 + ST2084/BT.2020) or scRGB
 * (linear FP16 + BT.709). Settings, as the Vulkan renderer reads them
 * (R3_/R2_/R1_/PORT_ prefixes):
 *   HDR        0/off (default), auto (only when Windows HDR is on), 1/on/hdr10, 2/scrgb
 *   HDR_PAPER  paper white nits (203, 80..500)
 *   HDR_PEAK   peak nits (else the display's, else 1000; never below paper)
 *   HDR_SAT    saturation 0..2 (1)
 * Windows HDR known to be off, no HDR colour space or a failed swap chain:
 * SDR, logged.
 *
 * Latency reduction (PORT_LATENCY + <game>_REFLEX, port_latency.h):
 *   NVIDIA Reflex through NVAPI (nvapi64.dll, loaded at run time):
 *     NvAPI_D3D_SetSleepMode once, NvAPI_D3D_Sleep at the game's frame start,
 *     latency markers as the Vulkan renderer places them (SIMULATION_START/END
 *     game thread, INPUT_SAMPLE first pad read, RENDERSUBMIT_START first draw,
 *     RENDERSUBMIT_END/PRESENT_START/PRESENT_END around Present).
 *   AMD Anti-Lag 2 through the AMD D3D12 driver extension (amdxc64.dll,
 *     ffx_antilag2_dx12.h's protocol, MIT): enabled once, the delay inserted
 *     at the game's frame start.
 * Neither is a frame limiter. Everything here is best effort: a missing
 * driver/DLL logs once and leaves the frame alone. */

#include "ps3emu/port_latency.h"

/* ---- settings ------------------------------------------------------------------- */

static const char* d3e_penv(const char* name)
{
    static const char* const pre[4] = { "R3_", "R2_", "R1_", "PORT_" };
    for (u32 i = 0; i < 4; i++) {
        char full[96];
        snprintf(full, sizeof full, "%s%s", pre[i], name);
        const char* v = getenv(full);
        if (v && *v) return v;
    }
    return NULL;
}

static float d3e_penv_f(const char* name, float def, float lo, float hi)
{
    const char* v = d3e_penv(name);
    if (!v) return def;
    const float f = (float)atof(v);
    if (!(f == f)) return def;
    return f < lo ? lo : f > hi ? hi : f;
}

/* ---- HDR -------------------------------------------------------------------------- */

static struct {
    int decided, mode;        /* wanted for the next swap chain: 0 SDR, 1 HDR10, 2 scRGB */
    int active;               /* mode of the current swap chain (0 = SDR) */
    float paper, peak, sat;
} s_hdr;

/* -1 auto, 0 off, 1 HDR10, 2 scRGB */
static int d3e_hdr_request(void)
{
    const char* v = d3e_penv("HDR");
    if (!v || rsx_vk_ieq(v, "0") || rsx_vk_ieq(v, "off") || rsx_vk_ieq(v, "sdr") || rsx_vk_ieq(v, "false")) return 0;
    if (rsx_vk_ieq(v, "auto")) return -1;
    if (rsx_vk_ieq(v, "2") || rsx_vk_ieq(v, "scrgb")) return 2;
    return 1;
}

/* The DXGI output showing most of the window: Windows HDR state and peak. 1 found. */
static int d3e_hdr_output(DXGI_COLOR_SPACE_TYPE* cs, float* max_nits)
{
    RECT wr;
    if (!s_d.factory || !s_d.hwnd || !GetWindowRect(s_d.hwnd, &wr)) return 0;
    long best = -1;
    for (UINT a = 0; a < 16; a++) {
        IDXGIAdapter1* ad = NULL;
        if (FAILED(IDXGIFactory4_EnumAdapters1(s_d.factory, a, &ad)) || !ad) break;
        for (UINT o = 0; o < 16; o++) {
            IDXGIOutput* out = NULL;
            if (FAILED(IDXGIAdapter1_EnumOutputs(ad, o, &out)) || !out) break;
            IDXGIOutput6* o6 = NULL;
            if (SUCCEEDED(IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput6, (void**)&o6)) && o6) {
                DXGI_OUTPUT_DESC1 d;
                if (SUCCEEDED(IDXGIOutput6_GetDesc1(o6, &d))) {
                    const RECT r = d.DesktopCoordinates;
                    const long ix = (wr.right < r.right ? wr.right : r.right) - (wr.left > r.left ? wr.left : r.left);
                    const long iy = (wr.bottom < r.bottom ? wr.bottom : r.bottom) - (wr.top > r.top ? wr.top : r.top);
                    const long area = ix > 0 && iy > 0 ? ix * iy : 0;
                    if (area > best) {
                        best = area;
                        *cs = d.ColorSpace;
                        *max_nits = d.MaxLuminance;
                    }
                }
                IDXGIOutput6_Release(o6);
            }
            IDXGIOutput_Release(out);
        }
        IDXGIAdapter1_Release(ad);
    }
    return best >= 0;
}

static void d3e_hdr_decide(void)
{
    s_hdr.decided = 1;
    s_hdr.mode = 0;
    const int want = d3e_hdr_request();
    if (!want) return;
    DXGI_COLOR_SPACE_TYPE cs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    float nits = 0.0f;
    const int known = d3e_hdr_output(&cs, &nits);
    const int win_hdr = known && cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    if (want < 0 && !win_hdr) { D3E_LOG("HDR auto: Windows HDR is off on this display, SDR output\n"); return; }
    if (want > 0 && known && !win_hdr) {
        D3E_LOG("HDR requested but Windows HDR is off on this display: SDR output (turn on \"Use HDR\" in Windows display settings)\n");
        return;
    }
    s_hdr.mode = want < 0 ? 1 : want;
    s_hdr.paper = d3e_penv_f("HDR_PAPER", 203.0f, 80.0f, 500.0f);
    const char* pk = d3e_penv("HDR_PEAK");
    float peak = pk ? (float)atof(pk) : 0.0f;
    if (!(peak >= 100.0f && peak <= 10000.0f)) peak = (nits >= 100.0f && nits <= 10000.0f) ? nits : 1000.0f;
    s_hdr.peak = peak < s_hdr.paper ? s_hdr.paper : peak;
    s_hdr.sat = d3e_penv_f("HDR_SAT", 1.0f, 0.0f, 2.0f);
}

/* The swap chain format to create (the probe's throwaway swap chain stays SDR). */
static DXGI_FORMAT d3e_hdr_swap_format(void)
{
    if (s_d3e_probing) return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!s_hdr.decided) d3e_hdr_decide();
    return s_hdr.mode == 2 ? DXGI_FORMAT_R16G16B16A16_FLOAT
         : s_hdr.mode == 1 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
}

static DXGI_FORMAT d3e_hdr_back_format(void)
{
    return s_hdr.active == 2 ? DXGI_FORMAT_R16G16B16A16_FLOAT
         : s_hdr.active == 1 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
}

/* After the swap chain was created or resized with d3e_hdr_swap_format():
 * colour space + HDR10 metadata. 0 = HDR could not be set up: the caller
 * drops the swap chain and the next one is SDR. */
static int d3e_hdr_apply(void)
{
    s_hdr.active = 0;
    if (s_d3e_probing || !s_hdr.mode || !s_d.swap) return 1;
    const DXGI_COLOR_SPACE_TYPE cs = s_hdr.mode == 2 ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                                     : DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    UINT support = 0;
    if (FAILED(IDXGISwapChain3_CheckColorSpaceSupport(s_d.swap, cs, &support)) ||
        !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ||
        FAILED(IDXGISwapChain3_SetColorSpace1(s_d.swap, cs))) {
        D3E_LOG("HDR: the swap chain cannot present %s, SDR output\n", s_hdr.mode == 2 ? "scRGB" : "HDR10");
        s_hdr.mode = 0;
        return 0;
    }
    if (s_hdr.mode == 1) {
        IDXGISwapChain4* s4 = NULL;
        if (SUCCEEDED(IDXGISwapChain3_QueryInterface(s_d.swap, &IID_IDXGISwapChain4, (void**)&s4)) && s4) {
            DXGI_HDR_METADATA_HDR10 md;
            memset(&md, 0, sizeof md);
            md.RedPrimary[0] = 35400; md.RedPrimary[1] = 14600;
            md.GreenPrimary[0] = 8500; md.GreenPrimary[1] = 39850;
            md.BluePrimary[0] = 6550; md.BluePrimary[1] = 2300;
            md.WhitePoint[0] = 15635; md.WhitePoint[1] = 16450;
            md.MaxMasteringLuminance = (UINT)s_hdr.peak;
            md.MinMasteringLuminance = 1;           /* 0.0001 nits */
            md.MaxContentLightLevel = (UINT16)s_hdr.peak;
            md.MaxFrameAverageLightLevel = (UINT16)s_hdr.paper;
            if (FAILED(IDXGISwapChain4_SetHDRMetaData(s4, DXGI_HDR_METADATA_TYPE_HDR10, sizeof md, &md)))
                D3E_WARN_ONCE(29, "HDR: SetHDRMetaData failed (display uses its defaults)\n");
            IDXGISwapChain4_Release(s4);
        }
    }
    s_hdr.active = s_hdr.mode;
    static int logged;
    if (!logged) {
        logged = 1;
        D3E_LOG("HDR output: %s, paper white %.0f nits, peak %.0f nits, saturation %.2f\n",
                s_hdr.mode == 2 ? "scRGB" : "HDR10 (PQ)", (double)s_hdr.paper, (double)s_hdr.peak, (double)s_hdr.sat);
    }
    return 1;
}

/* The present pass target: the SDR image when HDR is on (1), else the back
 * buffer the caller passed in (0). */
static int d3e_hdr_target(D3D12_CPU_DESCRIPTOR_HANDLE* rtv, DXGI_FORMAT* rtf)
{
    if (!s_hdr.active) return 0;
    if (d3e_ua_objects() != 0) {
        D3E_WARN_ONCE(29, "HDR: shader setup failed, presenting unencoded\n");
        return 0;
    }
    if (d3e_ua_img(D3E_AI_SDR, DXGI_FORMAT_R8G8B8A8_UNORM, s_d.swap_w, s_d.swap_h, 0) < 0) return 0;
    d3e_aimg* im = &s_ua.img[D3E_AI_SDR];
    d3e_barrier_res(im->res, &im->st, D3D12_RESOURCE_STATE_RENDER_TARGET);
    *rtv = d3e_ua_rtv(D3E_AI_SDR);
    *rtf = im->fmt;
    return 1;
}

/* The SDR image -> the HDR back buffer `rtv`. */
static void d3e_hdr_resolve(D3D12_CPU_DESCRIPTOR_HANDLE rtv)
{
    d3e_aimg* im = &s_ua.img[D3E_AI_SDR];
    if (!im->res) return;
    d3e_barrier_res(im->res, &im->st, D3E_SRV_STATES);
    float c[32];
    memset(c, 0, sizeof c);
    c[20] = (float)s_hdr.active; c[21] = s_hdr.paper; c[22] = s_hdr.peak; c[23] = s_hdr.sat;
    D3D12_CPU_DESCRIPTOR_HANDLE srv[3] = { d3e_ua_srv(D3E_AI_SDR), { 0 }, { 0 } };
    const DXGI_FORMAT f = d3e_hdr_back_format();
    if (!d3e_ua_run(D3E_AA_HDR, 1, &f, &rtv, srv, c, 0, 0, s_d.swap_w, s_d.swap_h))
        D3E_WARN_ONCE(29, "HDR: encode pass unavailable\n");
}

/* ---- latency reduction ------------------------------------------------------------ */

/* NVAPI (MIT, nvapi.h) declarations needed here; nvapi64.dll is resolved at run time. */
typedef struct d3e_nv_sleep_params {
    u32 version;
    u8  bLowLatencyMode, bLowLatencyBoost;
    u32 minimumIntervalUs;
    u8  bUseMarkersToOptimize, bUseMinQueueTime;
    u8  rsvd[30];
} d3e_nv_sleep_params;
typedef struct d3e_nv_marker_params {
    u32 version;
    u64 frameID;
    int markerType;
    u64 rsvd0;
    u8  rsvd[56];
} d3e_nv_marker_params;
#define D3E_NV_VER(t, v) ((u32)(sizeof(t) | ((v) << 16)))
enum { D3E_NV_SIM_START = 0, D3E_NV_SIM_END = 1, D3E_NV_RS_START = 2, D3E_NV_RS_END = 3,
       D3E_NV_PRESENT_START = 4, D3E_NV_PRESENT_END = 5, D3E_NV_INPUT = 6 };

/* AMD Anti-Lag 2 driver interface (ffx_antilag2_dx12.h, MIT) */
typedef struct d3e_al2_api d3e_al2_api;
typedef struct d3e_al2_vtbl {
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(d3e_al2_api*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE* AddRef)(d3e_al2_api*);
    ULONG   (STDMETHODCALLTYPE* Release)(d3e_al2_api*);
    HRESULT (STDMETHODCALLTYPE* UpdateAntiLagState)(d3e_al2_api*, void*);
} d3e_al2_vtbl;
struct d3e_al2_api { const d3e_al2_vtbl* lpVtbl; };
typedef struct d3e_al2_data_v1 {
    unsigned uiSize, uiVersion, eMode;
    const char* sControlStr;
    unsigned uiControlStrLength, maxFPS;
} d3e_al2_data_v1;
static const GUID s_iid_al2 = { 0x44085fbe, 0xe839, 0x40c5, { 0xbf, 0x38, 0x0e, 0xbc, 0x5a, 0xb4, 0xd0, 0xa6 } };

#define D3E_LAT_RING 8u
static struct {
    SRWLOCK lock;                /* device lifetime vs the game thread's sleep */
    int  kind;                   /* 0 none, 1 Reflex, 2 Anti-Lag 2 */
    int  markers;
    HMODULE nvapi;
    int  (*set_sleep)(IUnknown*, d3e_nv_sleep_params*);
    int  (*sleep)(IUnknown*);
    int  (*marker)(IUnknown*, d3e_nv_marker_params*);
    d3e_al2_api* al2;
    ID3D12Device* dev;
    volatile LONG64 sim;
    volatile LONG input_done;
    u64  ring[D3E_LAT_RING];
    volatile LONG head, tail;
    u64  render_id, render_last; /* RSX thread */
    int  render_open;
} s_lat = { SRWLOCK_INIT };

static void d3e_lat_emit(int m, u64 id)
{
    if (!id || s_lat.kind != 1 || !s_lat.markers || !s_lat.marker || !s_lat.dev) return;
    d3e_nv_marker_params p;
    memset(&p, 0, sizeof p);
    p.version = D3E_NV_VER(d3e_nv_marker_params, 1);
    p.frameID = id;
    p.markerType = m;
    s_lat.marker((IUnknown*)s_lat.dev, &p);
}

static int d3e_lat_reflex_up(int level)
{
    if (!s_lat.nvapi) s_lat.nvapi = LoadLibraryA("nvapi64.dll");
    if (!s_lat.nvapi) { D3E_LOG("Reflex: nvapi64.dll not found (not an NVIDIA driver)\n"); return 0; }
    void* (*qi)(u32) = (void* (*)(u32))(void*)GetProcAddress(s_lat.nvapi, "nvapi_QueryInterface");
    if (!qi) { D3E_LOG("Reflex: nvapi_QueryInterface missing\n"); return 0; }
    int (*init)(void) = (int (*)(void))qi(0x0150E828u);
    s_lat.set_sleep = (int (*)(IUnknown*, d3e_nv_sleep_params*))qi(0xAC1CA9E0u);
    s_lat.sleep = (int (*)(IUnknown*))qi(0x852CD1D2u);
    s_lat.marker = (int (*)(IUnknown*, d3e_nv_marker_params*))qi(0xD9984C05u);
    if (!init || !s_lat.set_sleep || !s_lat.sleep || init() != 0) {
        D3E_LOG("Reflex: NVAPI unavailable on this driver\n");
        return 0;
    }
    d3e_nv_sleep_params sp;
    memset(&sp, 0, sizeof sp);
    sp.version = D3E_NV_VER(d3e_nv_sleep_params, 1);
    sp.bLowLatencyMode = 1;
    sp.bLowLatencyBoost = level >= 2;
    sp.bUseMarkersToOptimize = (u8)(s_lat.markers && s_lat.marker);
    const int st = s_lat.set_sleep((IUnknown*)s_d.dev, &sp);
    if (st != 0) { D3E_LOG("Reflex: NvAPI_D3D_SetSleepMode failed (%d), off\n", st); return 0; }
    D3E_LOG("NVIDIA Reflex: on%s%s\n", level >= 2 ? " + boost" : "", sp.bUseMarkersToOptimize ? ", latency markers" : "");
    return 1;
}

static int d3e_lat_antilag_up(void)
{
    HMODULE m = GetModuleHandleA("amdxc64.dll");
    if (!m) { D3E_LOG("Anti-Lag 2: no AMD D3D12 driver extension (amdxc64.dll)\n"); return 0; }
    HRESULT (__cdecl* create)(IUnknown*, REFIID, void**) =
        (HRESULT (__cdecl*)(IUnknown*, REFIID, void**))(void*)GetProcAddress(m, "AmdExtD3DCreateInterface");
    if (!create) return 0;
    d3e_al2_api* api = NULL;
    if (FAILED(create((IUnknown*)s_d.dev, &s_iid_al2, (void**)&api)) || !api) {
        D3E_LOG("Anti-Lag 2: not supported by this GPU/driver\n");
        return 0;
    }
    d3e_al2_data_v1 d;
    memset(&d, 0, sizeof d);
    d.uiSize = sizeof d; d.uiVersion = 1; d.eMode = 1; d.maxFPS = 0;   /* on, no limiter */
    if (api->lpVtbl->UpdateAntiLagState(api, &d) != S_OK) {
        api->lpVtbl->Release(api);
        D3E_LOG("Anti-Lag 2: enabling failed\n");
        return 0;
    }
    s_lat.al2 = api;
    D3E_LOG("AMD Anti-Lag 2: on\n");
    return 1;
}

/* End of d3e_device_create (not for the probe's device). */
static void d3e_lat_device_up(void)
{
    if (s_d3e_probing) return;
    const char* r = d3e_penv("REFLEX");
    const int mode = port_latency_parse(getenv("PORT_LATENCY"));
    const int level = port_latency_reflex(mode, r ? atoi(r) : 0, s_d.vendor);
    const int al = port_latency_antilag(mode, s_d.vendor);
    const char* mk = d3e_penv("REFLEX_MARKERS");
    AcquireSRWLockExclusive(&s_lat.lock);
    s_lat.kind = 0;
    s_lat.markers = !mk || atoi(mk) != 0;
    s_lat.dev = s_d.dev;
    if (level > 0 && s_d.vendor != PORT_LAT_VENDOR_AMD && d3e_lat_reflex_up(level)) s_lat.kind = 1;
    else if (al && s_d.vendor != PORT_LAT_VENDOR_NVIDIA && d3e_lat_antilag_up()) s_lat.kind = 2;
    if (!s_lat.kind) s_lat.dev = NULL;
    s_lat.head = s_lat.tail = 0;
    s_lat.sim = 0;
    s_lat.render_id = s_lat.render_last = 0;
    s_lat.render_open = 0;
    ReleaseSRWLockExclusive(&s_lat.lock);
}

static void d3e_lat_device_down(void)
{
    if (s_d3e_probing) return;
    AcquireSRWLockExclusive(&s_lat.lock);
    if (s_lat.al2) {
        d3e_al2_data_v1 d;
        memset(&d, 0, sizeof d);
        d.uiSize = sizeof d; d.uiVersion = 1; d.eMode = 2;
        s_lat.al2->lpVtbl->UpdateAntiLagState(s_lat.al2, &d);
        s_lat.al2->lpVtbl->Release(s_lat.al2);
        s_lat.al2 = NULL;
    }
    s_lat.kind = 0;
    s_lat.dev = NULL;
    ReleaseSRWLockExclusive(&s_lat.lock);
    s_hdr.active = 0;
}

/* RSX thread: 0 first draw of a frame, 1 just before Present, 2 after it. */
static void d3e_lat_render(int ev)
{
    if (s_lat.kind != 1 || !s_lat.markers) return;
    switch (ev) {
    case 0: {
        if (s_lat.render_open) return;
        const LONG t = s_lat.tail;
        const int queued = t != s_lat.head;
        MemoryBarrier();
        s_lat.render_id = port_latency_render_id(queued, queued ? s_lat.ring[(u32)t % D3E_LAT_RING] : 0,
                                                 (unsigned long long)s_lat.sim, s_lat.render_last);
        s_lat.render_open = 1;
        d3e_lat_emit(D3E_NV_RS_START, s_lat.render_id);
        break;
    }
    case 1:
        if (!s_lat.render_open) d3e_lat_render(0);
        d3e_lat_emit(D3E_NV_RS_END, s_lat.render_id);
        d3e_lat_emit(D3E_NV_PRESENT_START, s_lat.render_id);
        break;
    case 2: {
        if (!s_lat.render_open) return;
        d3e_lat_emit(D3E_NV_PRESENT_END, s_lat.render_id);
        if (s_lat.render_id) s_lat.render_last = s_lat.render_id;
        s_lat.render_open = 0;
        LONG t = s_lat.tail;
        while (t != s_lat.head && s_lat.ring[(u32)t % D3E_LAT_RING] <= s_lat.render_last) t++;
        InterlockedExchange(&s_lat.tail, t);
        break;
    }
    default: break;
    }
}

/* ---- public API ------------------------------------------------------------------------ */

int rsx_d3d12_engine_latency_frame(void)
{
    if (!s_lat.kind) return 0;
    AcquireSRWLockShared(&s_lat.lock);
    int r = 0;
    if (s_lat.kind == 2 && s_lat.al2) {
        s_lat.al2->lpVtbl->UpdateAntiLagState(s_lat.al2, NULL);   /* the latency-reducing delay */
    } else if (s_lat.kind == 1 && s_lat.dev) {
        u64 id = (u64)s_lat.sim;
        if (s_lat.markers && id) {
            d3e_lat_emit(D3E_NV_SIM_END, id);
            const LONG h = s_lat.head;
            if ((u32)(h - s_lat.tail) < D3E_LAT_RING) {   /* else the render side is far behind: skip */
                s_lat.ring[(u32)h % D3E_LAT_RING] = id;
                MemoryBarrier();
                InterlockedExchange(&s_lat.head, h + 1);
            }
        }
        s_lat.sleep((IUnknown*)s_lat.dev);
        id++;
        InterlockedExchange64(&s_lat.sim, (LONG64)id);
        InterlockedExchange(&s_lat.input_done, 0);
        d3e_lat_emit(D3E_NV_SIM_START, id);
        r = s_lat.markers && s_lat.marker;
    }
    ReleaseSRWLockShared(&s_lat.lock);
    return r;
}

void rsx_d3d12_engine_latency_input(void)
{
    if (s_lat.kind != 1 || InterlockedExchange(&s_lat.input_done, 1)) return;
    AcquireSRWLockShared(&s_lat.lock);
    d3e_lat_emit(D3E_NV_INPUT, (u64)s_lat.sim);
    ReleaseSRWLockShared(&s_lat.lock);
}

int rsx_d3d12_engine_latency_kind(void) { return s_lat.kind; }

int rsx_d3d12_engine_hdr_active(void) { return s_hdr.active; }
