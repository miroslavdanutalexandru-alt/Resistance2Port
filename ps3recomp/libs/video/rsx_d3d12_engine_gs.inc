/* rsx_d3d12_engine_gs.inc -- device-side part of rsx_d3d12_engine_ext.inc:
 * the point / wide-line expansion geometry shaders, the logic-op blends and
 * the native R5G6B5 / A1R5G5B5 colour targets. Included by
 * rsx_d3d12_engine.c after the pass part.
 *
 *   RSX_D3D12_GS=0             no expansion GS: points 1 pixel, lines 1 pixel wide,
 *                              no point sprite coordinate (as before)
 *   RSX_D3D12_NATIVE_16BIT=0   R5G6B5 / A1R5G5B5 surfaces as RGBA8 targets
 *   RSX_D3D12_NATIVE_8BIT=0    B8 / G8B8 surfaces as RGBA8 targets (default R8 / R8G8)
 *   RSX_D3D12_LOGIC_OP=0       logic ops ignored (drawn without blending, as before) */

static int d3e_gs_on(void)
{
    static int v = -1;
    if (v < 0) v = d3e_env_on("RSX_D3D12_GS", 1);
    return v;
}

static int d3e_logic_on(void)
{
    static int v = -1;
    if (v < 0) v = d3e_env_on("RSX_D3D12_LOGIC_OP", 1);
    return v;
}

/* Interned GS I/O sources: a pipeline (and async jobs holding a snapshot of
 * it) points into this table, which is only ever appended to. */
#define D3E_GSIO_MAX 4096u
static struct {
    SRWLOCK lock;
    u64 key[D3E_GSIO_MAX];
    char* src[D3E_GSIO_MAX];
    u32 n;
} s_gsio = { SRWLOCK_INIT };

static const char* d3e_gsio_intern(const char* vs_hlsl, const char* ps_hlsl)
{
    if (!d3e_gs_on() || !vs_hlsl || !ps_hlsl) return NULL;
    static char buf[65536];       /* main thread only (pipeline_create) */
    const size_t n = rsx_d3d12_gs_io(vs_hlsl, ps_hlsl, buf, sizeof buf);
    if (!n) {
        D3E_WARN_ONCE(30, "a shader pair does not fit the point / line expansion GS: its points and wide lines stay 1 pixel\n");
        return NULL;
    }
    const u64 key = d3e_hash(buf, n, 0x47534F31ull);
    const char* r = NULL;
    AcquireSRWLockExclusive(&s_gsio.lock);
    for (u32 i = 0; i < s_gsio.n && !r; i++)
        if (s_gsio.key[i] == key && !strcmp(s_gsio.src[i], buf)) r = s_gsio.src[i];
    if (!r && s_gsio.n < D3E_GSIO_MAX) {
        char* c = (char*)malloc(n + 1);
        if (c) {
            memcpy(c, buf, n + 1);
            s_gsio.key[s_gsio.n] = key;
            s_gsio.src[s_gsio.n++] = c;
            r = c;
        }
    }
    ReleaseSRWLockExclusive(&s_gsio.lock);
    if (!r) D3E_WARN_ONCE(31, "expansion GS table full: further points / wide lines stay 1 pixel\n");
    return r;
}

/* The expansion GS of kind 1 (points) / 2 (lines) for gsio (any thread;
 * d3e_compile caches by source). */
static ID3DBlob* d3e_gs_compile(const char* gsio, u32 kind)
{
    const char* body = rsx_d3d12_gs_body((int)kind);
    if (!gsio || !body) return NULL;
    const size_t a = strlen(gsio), b = strlen(body);
    char* src = (char*)malloc(a + b + 1);
    if (!src) return NULL;
    memcpy(src, gsio, a);
    memcpy(src + a, body, b + 1);
    ID3DBlob* gs = d3e_compile(src, "main", "gs_5_0", kind == 1 ? "rsx_gs_point" : "rsx_gs_line");
    free(src);
    return gs;
}

static int d3e_be_point_sprite_supported(void* user)
{
    (void)user;
    return d3e_gs_on();
}

/* ---- R5G6B5 / A1R5G5B5 / R8 / R8G8 colour targets ------------------------------- */

static int d3e_native16_kind(rsx_be_format f) { return d3e_cv_kind(f); }

static int d3e_be_color_format_supported(void* user, rsx_be_format fmt)
{
    (void)user;
    static int cache[4] = { -1, -1, -1, -1 };
    static const DXGI_FORMAT hf[4] = { DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM,
                                       DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM };
    const int k = d3e_native16_kind(fmt);
    if (!k || !s_d.dev) return 0;
    if (cache[k - 1] < 0) {
        int ok = d3e_env_on(k <= 2 ? "RSX_D3D12_NATIVE_16BIT" : "RSX_D3D12_NATIVE_8BIT", 1);
        if (ok) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { hf[k - 1], D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
            const UINT need = D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE |
                              D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_TEXTURE2D;
            ok = SUCCEEDED(ID3D12Device_CheckFeatureSupport(s_d.dev, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) &&
                 (fs.Support1 & need) == need;
            if (!ok) D3E_LOG("%s render targets not supported here: such surfaces use RGBA8\n",
                             k == 1 ? "B5G6R5" : k == 2 ? "B5G5R5A1" : k == 3 ? "R8" : "R8G8");
        }
        cache[k - 1] = ok;
    }
    return cache[k - 1];
}

/* Upload rows given in the RGBA8 layout to a native 16/8-bit target. */
static void d3e_upload_rgba_to16(d3e_obj* o, u32 x, u32 y, u32 w, u32 h, const void* rows, u32 row_bytes)
{
    const int k = d3e_native16_kind(o->fmt);
    if (!k || !w || !h) return;
    const u32 bpp = rsx_d3d12_cv_bpp(k);
    u8* tmp = (u8*)malloc((size_t)w * bpp * h);
    if (!tmp) return;
    const u32 n = row_bytes / 4u < w ? row_bytes / 4u : w;
    for (u32 r = 0; r < h; r++) {
        u8* d = tmp + (size_t)r * w * bpp;
        rsx_d3d12_cv_from_rgba(k, (const u8*)rows + (size_t)r * row_bytes, d, n);
        if (n < w) memset(d + n * bpp, 0, (size_t)(w - n) * bpp);
    }
    d3e_upload_sub(o, 0, x, y, 0, w, h, tmp, w * bpp, h);
    free(tmp);
}

/* Read a native 16/8-bit target back in the RGBA8 layout. */
static int d3e_read_rect16(d3e_obj* o, u32 x, u32 y, u32 w, u32 h, void* out, u32 out_pitch)
{
    const int k = d3e_native16_kind(o->fmt);
    if (!k || !w || !h || out_pitch < 4u) return 0;
    if (x >= o->w || y >= o->h) return 0;
    if (w > o->w - x) w = o->w - x;
    if (h > o->h - y) h = o->h - y;
    const u32 bpp = rsx_d3d12_cv_bpp(k);
    u8* tmp = (u8*)malloc((size_t)w * bpp * h);
    if (!tmp) return 0;
    const int r = d3e_read_rect(o, x, y, w, h, tmp, w * bpp);
    if (r) {
        const u32 n = out_pitch / 4u < w ? out_pitch / 4u : w;
        for (u32 i = 0; i < h; i++)
            rsx_d3d12_cv_to_rgba(k, tmp + (size_t)i * w * bpp, (u8*)out + (size_t)i * out_pitch, n);
    }
    free(tmp);
    return r;
}
