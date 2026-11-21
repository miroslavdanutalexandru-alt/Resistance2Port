/* rsx_d3d12_engine_osd.inc -- included by rsx_d3d12_engine.c (after hdr.inc / fg.inc).
 *
 * Port UI ("OSD", project/port_osd.h) and screenshots for the Direct3D 12 renderer.
 *
 * OSD: the r2rsx_osd_* calls of the port (rpcs3_bridge/r2rsx_osd.cpp) are
 * forwarded here while this renderer runs (rsx_d3d12_engine_osd_api), so the
 * menus, toasts and the F3 overlay are drawn into the swap chain image like on
 * the RPCS3 renderer instead of as layered windows (which keep DWM composing
 * and are missing from captures). The 16 slots hold straight-alpha BGRA
 * panels placed in client coordinates (ref_w x ref_h -> the swap chain). They
 * are blended into the SDR image after the game frame: under HDR that is the
 * SDR image the HDR encode reads (paper-white scaled with the game, as the
 * Vulkan path's encode_hdr), and frame generation's HUD-less image excludes
 * them (FidelityFX keeps them out of generated frames). PORT_OSD=layered keeps
 * the layered windows.
 *
 * Screenshots: rsx_d3d12_engine_capture copies the next presented frame before
 * the OSD is blended in. Under HDR that is the SDR image (the frame before the
 * HDR encode), so a PNG matches what SDR shows; with frame generation it is a
 * real (not generated) frame. */

#define D3E_OSD_SLOTS 16

typedef struct d3e_osd_slot {
    /* port side, under s_osd.lock */
    u8* px;                       /* BGRA, w * 4 pitch, straight alpha */
    int w, h;
    float x, y, ref_w, ref_h, alpha;
    int visible;
    u32 gen;
    /* render side */
    ID3D12Resource* tex;
    D3D12_RESOURCE_STATES st;
    int tw, th;
    u32 shown_gen;
} d3e_osd_slot;

static struct {
    SRWLOCK lock;
    d3e_osd_slot s[D3E_OSD_SLOTS];
    volatile LONG suppress, presents, ready;
    ID3D12DescriptorHeap* srv;    /* CPU-only, one SRV per slot */
    u32 srv_inc, warned;
    /* capture: the caller's request, the render thread's answer */
    int cap_req, cap_done, cap_w, cap_h;
    u8* cap_px;
} s_osd = { SRWLOCK_INIT };

/* UI changed (or a capture waits) since the last present: rsx_d3d12_engine_osd_repost shows it
 * while the game is parked. The last present's arguments are what it presents again. */
static volatile LONG s_osd_dirty;
static struct { u32 surface, sx, sy, sw, sh, aw, ah; int have; ULONGLONG ms; } s_osd_last;

static int d3e_osd_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char* v = getenv("PORT_OSD");
        mode = (v && (!_stricmp(v, "layered") || !_stricmp(v, "window") || !_stricmp(v, "off") || !strcmp(v, "0"))) ? 0 : 1;
    }
    return mode;
}

static int  d3e_osd_wanted(void) { return s_d.up && !s_d3e_probing && d3e_osd_mode(); }
static int  d3e_osd_ready(void) { return d3e_osd_wanted() && s_osd.ready; }

static void d3e_osd_layer(int slot, const void* bgra, int w, int h, int x, int y, int ref_w, int ref_h, float alpha)
{
    if (slot < 0 || slot >= D3E_OSD_SLOTS || !d3e_osd_wanted()) return;
    u8* px = NULL;
    if (bgra && w > 0 && h > 0 && w <= 16384 && h <= 16384) {
        px = (u8*)malloc((size_t)w * (size_t)h * 4u);
        if (!px) return;
        memcpy(px, bgra, (size_t)w * (size_t)h * 4u);
    }
    AcquireSRWLockExclusive(&s_osd.lock);
    d3e_osd_slot* p = &s_osd.s[slot];
    if (px) { free(p->px); p->px = px; p->w = w; p->h = h; p->gen++; }
    p->x = (float)x; p->y = (float)y;
    p->ref_w = (float)(ref_w > 1 ? ref_w : 1); p->ref_h = (float)(ref_h > 1 ? ref_h : 1);
    p->alpha = alpha;
    p->visible = p->px != NULL;
    ReleaseSRWLockExclusive(&s_osd.lock);
    InterlockedExchange(&s_osd_dirty, 1);
}

static void d3e_osd_hide(int slot)
{
    if (slot < 0 || slot >= D3E_OSD_SLOTS) return;
    AcquireSRWLockExclusive(&s_osd.lock);
    const int was = s_osd.s[slot].visible;
    s_osd.s[slot].visible = 0;
    ReleaseSRWLockExclusive(&s_osd.lock);
    if (was) InterlockedExchange(&s_osd_dirty, 1);
}

static int d3e_osd_visible(int slot)
{
    int v = 0;
    AcquireSRWLockShared(&s_osd.lock);
    if (slot < 0) { for (int i = 0; i < D3E_OSD_SLOTS - 1; i++) v |= s_osd.s[i].visible; }
    else if (slot < D3E_OSD_SLOTS) v = s_osd.s[slot].visible;
    ReleaseSRWLockShared(&s_osd.lock);
    return v != 0;
}

static void d3e_osd_suppress(int on)
{
    if (InterlockedExchange(&s_osd.suppress, on ? 1 : 0) != (on ? 1 : 0)) InterlockedExchange(&s_osd_dirty, 1);
}
static unsigned d3e_osd_presents(void) { return (unsigned)s_osd.presents; }
static void d3e_osd_refresh(void) {}   /* the game keeps flipping in its menus; nothing to kick */

static const rsx_d3d12_osd_api s_osd_api = {
    d3e_osd_wanted, d3e_osd_ready, d3e_osd_layer, d3e_osd_hide, d3e_osd_visible,
    d3e_osd_suppress, d3e_osd_presents, d3e_osd_refresh
};

const rsx_d3d12_osd_api* rsx_d3d12_engine_osd_api(void)
{
    return (s_d.up && !s_d3e_probing && d3e_osd_mode()) ? &s_osd_api : NULL;
}

/* Device lost / shutdown: the GPU copies go; the port's pixels stay for the next device. */
static void d3e_osd_device_down(void)
{
    for (int i = 0; i < D3E_OSD_SLOTS; i++) {
        d3e_osd_slot* p = &s_osd.s[i];
        if (p->tex) d3e_release(p->tex);
        p->tex = NULL; p->tw = p->th = 0; p->shown_gen = 0;
    }
    if (s_osd.srv) d3e_release(s_osd.srv);
    s_osd.srv = NULL;
    InterlockedExchange(&s_osd.ready, 0);
}

static D3D12_CPU_DESCRIPTOR_HANDLE d3e_osd_srv(int i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(s_osd.srv, &h);
    h.ptr += (SIZE_T)i * s_osd.srv_inc;
    return h;
}

/* Under the lock: upload slot i's pixels when they changed. 0: not drawable. */
static int d3e_osd_upload(int i)
{
    d3e_osd_slot* p = &s_osd.s[i];
    if (!p->px || p->w <= 0 || p->h <= 0) return 0;
    if (p->tex && p->shown_gen == p->gen && p->tw == p->w && p->th == p->h) return 1;
    if (!p->tex || p->tw != p->w || p->th != p->h) {
        if (p->tex) d3e_defer_release(p->tex);
        p->tex = d3e_texture_resource(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_B8G8R8A8_UNORM, (u32)p->w, (u32)p->h,
                                      1, 1, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, NULL);
        p->tw = p->th = 0;
        if (!p->tex) return 0;
        p->st = D3D12_RESOURCE_STATE_COPY_DEST;
        p->tw = p->w; p->th = p->h;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        ID3D12Device_CreateShaderResourceView(s_d.dev, p->tex, &sd, d3e_osd_srv(i));
    }
    const u32 pitch = ((u32)p->w * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    ID3D12Resource* up = d3e_buffer((u64)pitch * (u32)p->h, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!up) return 0;
    void* m = NULL;
    D3D12_RANGE none = { 0, 0 };
    if (FAILED(ID3D12Resource_Map(up, 0, &none, &m)) || !m) { d3e_release(up); return 0; }
    for (int r = 0; r < p->h; r++) memcpy((u8*)m + (size_t)r * pitch, p->px + (size_t)r * (size_t)p->w * 4u, (size_t)p->w * 4u);
    ID3D12Resource_Unmap(up, 0, NULL);
    D3D12_TEXTURE_COPY_LOCATION dl, sl;
    memset(&dl, 0, sizeof dl); memset(&sl, 0, sizeof sl);
    dl.pResource = p->tex; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.pResource = up; sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sl.PlacedFootprint.Footprint.Width = (u32)p->w;
    sl.PlacedFootprint.Footprint.Height = (u32)p->h;
    sl.PlacedFootprint.Footprint.Depth = 1;
    sl.PlacedFootprint.Footprint.RowPitch = pitch;
    d3e_barrier_res(p->tex, &p->st, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyTextureRegion(s_d.cl, &dl, 0, 0, 0, &sl, NULL);
    d3e_defer_release(up);
    p->shown_gen = p->gen;
    return 1;
}

/* d3e_present_region: blend the visible slots into the present target (the SDR image under HDR). */
static void d3e_osd_draw(D3D12_CPU_DESCRIPTOR_HANDLE rtv, DXGI_FORMAT rtf)
{
    if (!d3e_osd_mode()) return;
    InterlockedExchange(&s_osd_dirty, 0);
    InterlockedExchange(&s_osd.presents, s_osd.presents + 1);
    if (!s_osd.srv) {
        s_osd.srv = d3e_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3E_OSD_SLOTS, 0);
        if (!s_osd.srv) { D3E_WARN_ONCE(42, "OSD: descriptor heap failed, port UI stays in layered windows\n"); return; }
        s_osd.srv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(s_d.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    if (!s_osd.ready) {
        InterlockedExchange(&s_osd.ready, 1);
        D3E_LOG("OSD: port UI drawn into the swap chain image (PORT_OSD=layered for windows)\n");
    }
    if (!s_d.swap_w || !s_d.swap_h) return;
    const int hide = s_osd.suppress != 0;
    AcquireSRWLockExclusive(&s_osd.lock);
    for (int i = 0; i < D3E_OSD_SLOTS; i++) {
        d3e_osd_slot* p = &s_osd.s[i];
        if (!p->visible || hide || p->alpha <= 0.0f) continue;
        if (!d3e_osd_upload(i)) continue;
        const float sx = (float)s_d.swap_w / p->ref_w, sy = (float)s_d.swap_h / p->ref_h;
        float x0 = p->x * sx, y0 = p->y * sy, x1 = (p->x + (float)p->w) * sx, y1 = (p->y + (float)p->h) * sy;
        /* the visible part, in whole pixels (the viewport maps the whole panel; the scissor clips it) */
        const float cx0 = x0 < 0.0f ? 0.0f : x0, cy0 = y0 < 0.0f ? 0.0f : y0;
        const float cx1 = x1 > (float)s_d.swap_w ? (float)s_d.swap_w : x1, cy1 = y1 > (float)s_d.swap_h ? (float)s_d.swap_h : y1;
        if (cx1 <= cx0 || cy1 <= cy0) continue;
        d3e_barrier_res(p->tex, &p->st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        d3e_pass_args a;
        d3e_pass_args_init(&a, D3E_PS_OSD);
        a.mode |= D3E_PM_BLEND;
        a.rtf = rtf;
        a.rtv = rtv;
        a.srv0_cpu = d3e_osd_srv(i);
        a.c[4] = p->alpha > 1.0f ? 1.0f : p->alpha;
        a.vp.TopLeftX = x0; a.vp.TopLeftY = y0;
        a.vp.Width = x1 - x0; a.vp.Height = y1 - y0;
        a.vp.MinDepth = 0.0f; a.vp.MaxDepth = 1.0f;
        a.sc.left = (LONG)cx0; a.sc.top = (LONG)cy0;
        a.sc.right = (LONG)(cx1 + 0.5f); a.sc.bottom = (LONG)(cy1 + 0.5f);
        d3e_pass_run(&a);
    }
    ReleaseSRWLockExclusive(&s_osd.lock);
}

/* ---- screenshots ------------------------------------------------------------------- */

static ID3D12Resource* s_cap_rb;
static u32 s_cap_pitch, s_cap_w, s_cap_h;

/* d3e_present_region, after the frame is in `res` (RGBA8, state *st) and before the OSD. */
static void d3e_cap_record(ID3D12Resource* res, D3D12_RESOURCE_STATES* st, u32 x, u32 y, u32 w, u32 h)
{
    if (s_cap_rb) return;
    AcquireSRWLockShared(&s_osd.lock);
    const int want = s_osd.cap_req && !s_osd.cap_done;
    ReleaseSRWLockShared(&s_osd.lock);
    if (!want || !res || !w || !h) return;
    const u32 pitch = (w * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    s_cap_rb = d3e_buffer((u64)pitch * h, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!s_cap_rb) return;
    D3D12_TEXTURE_COPY_LOCATION dl, sl;
    memset(&dl, 0, sizeof dl); memset(&sl, 0, sizeof sl);
    dl.pResource = s_cap_rb;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dl.PlacedFootprint.Footprint.Width = w;
    dl.PlacedFootprint.Footprint.Height = h;
    dl.PlacedFootprint.Footprint.Depth = 1;
    dl.PlacedFootprint.Footprint.RowPitch = pitch;
    sl.pResource = res;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = { x, y, 0, x + w, y + h, 1 };
    const D3D12_RESOURCE_STATES back = *st;
    d3e_barrier_res(res, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyTextureRegion(s_d.cl, &dl, 0, 0, 0, &sl, &box);
    d3e_barrier_res(res, st, back);
    s_cap_pitch = pitch; s_cap_w = w; s_cap_h = h;
}

/* d3e_present_region, before the OSD: the SDR image under HDR, else the back buffer. */
static void d3e_cap_present(int hdr, u32 bi, u32 x, u32 y, u32 w, u32 h)
{
    if (hdr) d3e_cap_record(s_ua.img[D3E_AI_SDR].res, &s_ua.img[D3E_AI_SDR].st, x, y, w, h);
    else d3e_cap_record(s_d.back[bi], &s_d.back_st[bi], x, y, w, h);
}

/* d3e_present_region, after the frame was submitted. */
static void d3e_cap_finish(void)
{
    if (!s_cap_rb) return;
    d3e_wait_fence(s_d.fence_val);
    u8* px = (u8*)malloc((size_t)s_cap_w * s_cap_h * 4u);
    void* m = NULL;
    D3D12_RANGE rr = { 0, (SIZE_T)s_cap_pitch * s_cap_h };
    if (px && SUCCEEDED(ID3D12Resource_Map(s_cap_rb, 0, &rr, &m)) && m) {
        for (u32 r = 0; r < s_cap_h; r++) {
            const u8* s = (const u8*)m + (size_t)r * s_cap_pitch;
            u8* o = px + (size_t)r * s_cap_w * 4u;
            for (u32 x = 0; x < s_cap_w; x++, s += 4, o += 4) { o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = 255; }
        }
        D3D12_RANGE none = { 0, 0 };
        ID3D12Resource_Unmap(s_cap_rb, 0, &none);
    } else { free(px); px = NULL; }
    d3e_release(s_cap_rb);
    s_cap_rb = NULL;
    AcquireSRWLockExclusive(&s_osd.lock);
    if (s_osd.cap_req && !s_osd.cap_done && px) {
        free(s_osd.cap_px);
        s_osd.cap_px = px; px = NULL;
        s_osd.cap_w = (int)s_cap_w; s_osd.cap_h = (int)s_cap_h;
        s_osd.cap_done = 1;
    }
    ReleaseSRWLockExclusive(&s_osd.lock);
    free(px);
}

int rsx_d3d12_engine_capture(void** bgra, int* w, int* h, unsigned timeout_ms)
{
    if (!bgra || !w || !h) return 0;
    *bgra = NULL; *w = *h = 0;
    if (!s_d.up || s_d3e_probing || !s_d.swap) return 0;
    AcquireSRWLockExclusive(&s_osd.lock);
    free(s_osd.cap_px);
    s_osd.cap_px = NULL;
    s_osd.cap_done = 0;
    s_osd.cap_req = 1;
    ReleaseSRWLockExclusive(&s_osd.lock);
    InterlockedExchange(&s_osd_dirty, 1);     /* parked game: the re-present serves the capture */
    const ULONGLONG t0 = GetTickCount64();
    int ok = 0;
    for (;;) {
        AcquireSRWLockExclusive(&s_osd.lock);
        if (s_osd.cap_done) {
            *bgra = s_osd.cap_px; *w = s_osd.cap_w; *h = s_osd.cap_h;
            s_osd.cap_px = NULL;
            ok = 1;
        }
        if (ok || GetTickCount64() - t0 >= timeout_ms) {
            s_osd.cap_req = 0; s_osd.cap_done = 0;
            ReleaseSRWLockExclusive(&s_osd.lock);
            break;
        }
        ReleaseSRWLockExclusive(&s_osd.lock);
        Sleep(2);
    }
    return ok;
}

void rsx_d3d12_engine_capture_free(void* p) { free(p); }

int rsx_d3d12_engine_capture_available(void) { return s_d.up && !s_d3e_probing && s_d.swap; }

static void d3e_osd_note_present(u32 surface, u32 sx, u32 sy, u32 sw, u32 sh, u32 aw, u32 ah)
{
    s_osd_last.ms = GetTickCount64();
    if (!surface) return;
    s_osd_last.surface = surface; s_osd_last.sx = sx; s_osd_last.sy = sy; s_osd_last.sw = sw; s_osd_last.sh = sh;
    s_osd_last.aw = aw; s_osd_last.ah = ah; s_osd_last.have = 1;
}

/* The game does not flip while the port pauses it, so a changed UI would stay frozen: show the
 * last presented guest frame again with the current UI (frame generation gated off for it). */
int rsx_d3d12_engine_osd_repost(unsigned idle_ms, int parked)
{
    if (!s_d.up || s_d3e_probing || !s_osd_dirty || !s_osd_last.have) return 0;
    if (!parked && !(s_osd.cap_req && !s_osd.cap_done)) return 0;   /* running game: the next flip shows it */
    if (GetTickCount64() - s_osd_last.ms < (ULONGLONG)idle_ms) return 0;
    if (!d3e_get(s_osd_last.surface, D3E_COLOR)) {   /* the surface is gone: wait for the next flip */
        s_osd_last.have = 0;
        return 0;
    }
    s_d3e_repost = 1;
    d3e_present_region(s_osd_last.surface, s_osd_last.sx, s_osd_last.sy, s_osd_last.sw, s_osd_last.sh,
                       s_osd_last.aw, s_osd_last.ah);
    s_d3e_repost = 0;
    InterlockedExchange(&s_osd_dirty, 0);
    return 1;
}
