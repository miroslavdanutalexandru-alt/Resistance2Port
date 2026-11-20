/* rsx_d3d12_engine_ext.inc -- pure helpers (no device) for the RSX features
 * Direct3D 12 has no fixed-function equivalent of; part of rsx_d3d12_engine.c,
 * built with RSX_D3D12_HELPERS_ONLY too (project/tests/d3d12_helpers_test.c).
 *
 *   - Point sizes / point sprites and wide lines: a geometry shader expands
 *     each point into a screen-aligned quad (size from the VS's PSIZE, the
 *     sprite coordinate in RSX_PCOORD) and each line into a quad
 *     line_width pixels wide (GL's non-antialiased rule: extruded along the
 *     minor axis). DXBC links stages by register, so the GS's output
 *     signature is generated from the pixel shader's PSInput (in its order),
 *     filled from the VS's VSOutput by semantic.
 *   - Logic ops: D3D12 has them on UINT targets only; the engine's are UNORM,
 *     so the ops that are blends (CLEAR, COPY, NOOP) are exact and AND / OR /
 *     AND_REVERSE / AND_INVERTED are approximated (exact on 0 / 1 values).
 *   - R5G6B5 / A1R5G5B5 colour targets: the engine hands their texels over
 *     in the RGBA8 layout (rsx_draw_engine.c eng_host_color_format). */

/* ---- HLSL declaration parsing ---------------------------------------------- */

typedef struct d3g_decl {
    char mods[64];      /* interpolation modifiers + type */
    char name[48];
    char sem[48];
} d3g_decl;

#define D3G_MAX_DECLS 48

static int d3g_ieq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

static int d3g_ipre(const char* s, const char* pre)
{
    for (; *pre; s++, pre++) {
        char x = *s, y = *pre;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
    }
    return 1;
}

/* The body of `struct <name> {` ... `};` in src: [*b, *e). */
static int d3g_struct(const char* src, const char* name, const char** b, const char** e)
{
    char pat[64];
    snprintf(pat, sizeof pat, "struct %s {", name);
    const char* s = src ? strstr(src, pat) : NULL;
    if (!s) return 0;
    s += strlen(pat);
    const char* t = strstr(s, "\n};");
    if (!t) return 0;
    *b = s;
    *e = t + 1;
    return 1;
}

static void d3g_trim(char* s)
{
    size_t n = strlen(s), i = 0;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    while (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n') i++;
    if (i) memmove(s, s + i, n - i + 1);
}

/* Declarations of a struct body: preprocessor lines and [[...]] attributes
 * dropped (glslang-only spellings). Returns the count, -1 when one does not
 * parse or there are too many. */
static int d3g_parse(const char* b, const char* e, d3g_decl* d, int max)
{
    const size_t n = (size_t)(e - b);
    char* buf = (char*)malloc(n + 1);
    if (!buf) return -1;
    size_t o = 0;
    int bol = 1;
    for (const char* p = b; p < e; ) {
        if (bol) {
            const char* q = p;
            while (q < e && (*q == ' ' || *q == '\t')) q++;
            if (q < e && *q == '#') {
                while (p < e && *p != '\n') p++;
                continue;
            }
        }
        if (p + 1 < e && p[0] == '[' && p[1] == '[') {
            const char* q = strstr(p, "]]");
            if (!q || q >= e) break;
            p = q + 2;
            continue;
        }
        bol = *p == '\n';
        buf[o++] = *p++;
    }
    buf[o] = 0;
    int c = 0;
    for (char* s = buf; *s; ) {
        char* semi = strchr(s, ';');
        if (!semi) { d3g_trim(s); if (*s) c = -1; break; }
        *semi = 0;
        char item[192];
        snprintf(item, sizeof item, "%s", s);
        s = semi + 1;
        d3g_trim(item);
        if (!item[0]) continue;
        char* colon = strrchr(item, ':');
        if (!colon || c >= max) { c = -1; break; }
        *colon = 0;
        char* sem = colon + 1;
        d3g_trim(sem);
        d3g_trim(item);
        char* sp = item + strlen(item);
        while (sp > item && sp[-1] != ' ' && sp[-1] != '\t') sp--;
        if (sp == item || !*sp || !*sem) { c = -1; break; }
        snprintf(d[c].name, sizeof d[c].name, "%s", sp);
        *sp = 0;
        d3g_trim(item);
        snprintf(d[c].mods, sizeof d[c].mods, "%s", item);
        snprintf(d[c].sem, sizeof d[c].sem, "%s", sem);
        c++;
    }
    free(buf);
    return c;
}

typedef struct { char* p; size_t n, cap; int ok; } d3g_out;

static void d3g_puts(d3g_out* o, const char* s)
{
    const size_t l = strlen(s);
    if (!o->ok || o->n + l + 1 > o->cap) { o->ok = 0; return; }
    memcpy(o->p + o->n, s, l + 1);
    o->n += l;
}

static void d3g_printf(d3g_out* o, const char* fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    const int r = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= sizeof line) { o->ok = 0; return; }
    d3g_puts(o, line);
}

/* The part of the expansion GS shared by both kinds: VSOutput as the VS
 * declares it, GSOut laid out as the PS's PSInput (SV_IsFrontFace, which no
 * stage outputs, must come last there) plus the VS's clip distances,
 * gs_copy() filling one from the other by semantic, and the accessors the
 * bodies use. Returns the length, 0 when the shaders do not fit the scheme. */
size_t rsx_d3d12_gs_io(const char* vs_hlsl, const char* ps_hlsl, char* out, size_t cap)
{
    const char *vb, *ve, *pb, *pe;
    if (!out || !cap || !d3g_struct(vs_hlsl, "VSOutput", &vb, &ve) || !d3g_struct(ps_hlsl, "PSInput", &pb, &pe))
        return 0;
    d3g_decl* vd = (d3g_decl*)malloc(sizeof(d3g_decl) * 2u * D3G_MAX_DECLS);
    if (!vd) return 0;
    d3g_decl* pd = vd + D3G_MAX_DECLS;
    const int nv = d3g_parse(vb, ve, vd, D3G_MAX_DECLS);
    const int np = d3g_parse(pb, pe, pd, D3G_MAX_DECLS);
    size_t res = 0;
    if (nv <= 0 || np <= 0) goto done;
    int vpos = -1, vpsz = -1, ppos = -1, ppc = -1, front = 0;
    for (int i = 0; i < nv; i++) {
        if (d3g_ieq(vd[i].sem, "SV_Position")) vpos = i;
        else if (d3g_ieq(vd[i].sem, "PSIZE")) vpsz = i;
    }
    for (int i = 0; i < np; i++) {
        if (d3g_ieq(pd[i].sem, "SV_IsFrontFace")) { front = 1; continue; }
        if (front) goto done;           /* an input after the face flag: not a prefix */
        if (d3g_ieq(pd[i].sem, "SV_Position")) ppos = i;
        else if (d3g_ieq(pd[i].sem, "RSX_PCOORD")) ppc = i;
        else if (d3g_ipre(pd[i].sem, "SV_")) goto done;   /* SV_SampleIndex & co: not expandable */
    }
    if (vpos < 0 || ppos < 0) goto done;
    {
        d3g_out o = { out, 0, cap, 1 };
        out[0] = 0;
        d3g_puts(&o, "struct VSOutput {");
        {
            const size_t n = (size_t)(ve - vb);
            if (o.n + n + 1 > o.cap) o.ok = 0;
            else { memcpy(o.p + o.n, vb, n); o.n += n; o.p[o.n] = 0; }
        }
        d3g_puts(&o, "};\nstruct GSOut {\n");
        for (int i = 0; i < np; i++) {
            if (d3g_ieq(pd[i].sem, "SV_IsFrontFace")) continue;
            d3g_printf(&o, "    %s %s : %s;\n", pd[i].mods, pd[i].name, pd[i].sem);
        }
        for (int i = 0; i < nv; i++)
            if (d3g_ipre(vd[i].sem, "SV_ClipDistance"))
                d3g_printf(&o, "    %s _rsx_gcd%d : %s;\n", vd[i].mods, i, vd[i].sem);
        d3g_puts(&o, "};\nGSOut gs_copy(VSOutput v)\n{\n    GSOut o;\n");
        for (int i = 0; i < np; i++) {
            if (d3g_ieq(pd[i].sem, "SV_IsFrontFace")) continue;
            int m = -1;
            for (int j = 0; j < nv && m < 0; j++)
                if (d3g_ieq(vd[j].sem, pd[i].sem)) m = j;
            if (m >= 0) d3g_printf(&o, "    o.%s = v.%s;\n", pd[i].name, vd[m].name);
            else d3g_printf(&o, "    o.%s = 0;\n", pd[i].name);
        }
        for (int i = 0; i < nv; i++)
            if (d3g_ipre(vd[i].sem, "SV_ClipDistance"))
                d3g_printf(&o, "    o._rsx_gcd%d = v.%s;\n", i, vd[i].name);
        d3g_puts(&o, "    return o;\n}\n");
        d3g_printf(&o, "#define GS_VPOS(v) (v).%s\n", vd[vpos].name);
        if (vpsz >= 0) d3g_printf(&o, "#define GS_VPSIZE(v) (v).%s\n", vd[vpsz].name);
        else d3g_puts(&o, "#define GS_VPSIZE(v) 1.0\n");
        d3g_printf(&o, "#define GS_POS(o) (o).%s\n", pd[ppos].name);
        if (ppc >= 0) d3g_printf(&o, "#define GS_PC(o, c) (o).%s = (c)\n", pd[ppc].name);
        else d3g_puts(&o, "#define GS_PC(o, c)\n");
        d3g_puts(&o, "cbuffer RsxGsConst : register(b2) { float4 rsx_gs; };\n");
        if (o.ok) res = o.n;
    }
done:
    free(vd);
    return res;
}

/* kind 1: points -> quads of PSIZE pixels (1..2048) with the sprite
 * coordinate (0,0) at the top left; kind 2: lines -> quads rsx_gs.z pixels
 * wide. rsx_gs.xy is the viewport size in pixels, rsx_gs.w 1 when the front\n * face is counter-clockwise. */
const char* rsx_d3d12_gs_body(int kind)
{
    if (kind == 1)
        return
        "[maxvertexcount(4)]\n"
        "void main(point VSOutput i[1], inout TriangleStream<GSOut> s)\n"
        "{\n"
        "    const float4 p = GS_VPOS(i[0]);\n"
        "    if (!(p.w > 0.0)) return;\n"
        "    float sz = GS_VPSIZE(i[0]);\n"
        "    sz = sz >= 1.0 ? min(sz, 2048.0) : 1.0;\n"
        "    const float2 h = sz / max(rsx_gs.xy, 1.0) * p.w;\n"
        /* this order winds counter-clockwise: mirrored for a clockwise front
         * face (rsx_gs.w), points being front-facing as on RSX */
        "    const float fy = rsx_gs.w > 0.5 ? 1.0 : -1.0;\n"
        "    [unroll] for (int k = 0; k < 4; k++) {\n"
        "        const float2 c = float2(k < 2 ? -1.0 : 1.0, ((k & 1) ? -1.0 : 1.0) * fy);\n"
        "        GSOut o = gs_copy(i[0]);\n"
        "        GS_POS(o) = float4(p.xy + c * h, p.zw);\n"
        "        GS_PC(o, float2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5));\n"
        "        s.Append(o);\n"
        "    }\n"
        "}\n";
    if (kind == 2)
        return
        "[maxvertexcount(4)]\n"
        "void main(line VSOutput i[2], inout TriangleStream<GSOut> s)\n"
        "{\n"
        "    float4 a = GS_VPOS(i[0]), b = GS_VPOS(i[1]);\n"
        "    const float e = 1e-6;\n"
        "    if (!(a.w >= e) && !(b.w >= e)) return;\n"
        "    if (!(a.w >= e)) a = lerp(a, b, (e - a.w) / (b.w - a.w));\n"
        "    else if (!(b.w >= e)) b = lerp(b, a, (e - b.w) / (a.w - b.w));\n"
        "    const float2 hv = max(rsx_gs.xy, 1.0) * 0.5;\n"
        "    const float2 d = (b.xy / b.w - a.xy / a.w) * hv;\n"
        "    const float hw = max(rsx_gs.z, 1.0) * 0.5;\n"
        "    float2 n = (abs(d.x) >= abs(d.y) ? float2(0.0, hw) : float2(hw, 0.0)) / hv;\n"
        /* (a-n, a+n, b-n) winds counter-clockwise when cross(n, d) > 0: lines
         * are front-facing (two-sided colour) for either front face */
        "    if ((n.x * d.y - n.y * d.x > 0.0) != (rsx_gs.w > 0.5)) n = -n;\n"
        "    GSOut o = gs_copy(i[0]);\n"
        "    GS_POS(o) = float4(a.xy - n * a.w, a.zw); s.Append(o);\n"
        "    GS_POS(o) = float4(a.xy + n * a.w, a.zw); s.Append(o);\n"
        "    o = gs_copy(i[1]);\n"
        "    GS_POS(o) = float4(b.xy - n * b.w, b.zw); s.Append(o);\n"
        "    GS_POS(o) = float4(b.xy + n * b.w, b.zw); s.Append(o);\n"
        "}\n";
    return NULL;
}

/* The FP decompiler writes the sprite coordinate for the glslang paths only
 * (#ifdef RSX_FRAGCOORD_W_IS_RCP, a [[vk::location]] input). With the
 * expansion GS supplying RSX_PCOORD, D3D12 takes those blocks too: they are
 * unwrapped, the attribute dropped, and the input moved in front of the face
 * flag. A malloc'd copy, or NULL when ps reads no sprite coordinate. */
char* rsx_d3d12_ps_pcoord_fix(const char* ps)
{
    if (!ps || !strstr(ps, "_rsx_pcoord")) return NULL;
    const size_t n = strlen(ps);
    char* r = (char*)malloc(n + 64);
    if (!r) return NULL;
    static const char ifd[] = "#ifdef RSX_FRAGCOORD_W_IS_RCP\n";
    size_t o = 0;
    const char* skip_endif = NULL;
    for (const char* p = ps; *p; ) {
        if (p == skip_endif) {
            p += 6;
            if (*p == '\n') p++;
            skip_endif = NULL;
            continue;
        }
        if (!skip_endif && !strncmp(p, ifd, sizeof ifd - 1)) {
            const char* end = strstr(p, "#endif");
            const char* els = strstr(p, "#else");
            const char* nest = strstr(p + 1, "#if");
            const char* pc = strstr(p, "_rsx_pcoord");
            if (end && pc && pc < end && !(els && els < end) && !(nest && nest < end)) {
                skip_endif = end;
                p += sizeof ifd - 1;
                continue;
            }
        }
        if (p[0] == '[' && p[1] == '[' && !strncmp(p, "[[vk::location(", 15)) {
            const char* q = strstr(p, ")]]");
            if (q) {
                p = q + 3;
                if (*p == ' ') p++;
                continue;
            }
        }
        r[o++] = *p++;
    }
    r[o] = 0;
    /* RSX_PCOORD in front of SV_IsFrontFace, which no earlier stage writes */
    static const char pcl[] = "    float2 _rsx_pcoord : RSX_PCOORD;\n";
    char* fl = strstr(r, "    bool _rsx_front : SV_IsFrontFace;\n");
    char* pl = strstr(r, pcl);
    if (fl && pl && fl < pl) {
        const size_t L = sizeof pcl - 1;
        memmove(pl, pl + L, strlen(pl + L) + 1);
        memmove(fl + L, fl, strlen(fl) + 1);
        memcpy(fl, pcl, L);
    }
    return r;
}

/* ---- logic ops ------------------------------------------------------------- */

/* The blend that stands for GL logic op `op` (0x1500..0x150F) on a UNORM
 * target. 1: exact, 2: approximation (exact on 0 / 1 channel values), 0:
 * none (the caller draws without). NOOP leaves the write mask 0. */
int rsx_d3d12_logic_blend(uint32_t op, D3D12_RENDER_TARGET_BLEND_DESC* t)
{
    D3D12_BLEND s = D3D12_BLEND_ONE, d = D3D12_BLEND_ZERO, sa = D3D12_BLEND_ONE, da = D3D12_BLEND_ZERO;
    D3D12_BLEND_OP bo = D3D12_BLEND_OP_ADD;
    int r;
    switch (op) {
    case 0x1500: s = sa = D3D12_BLEND_ZERO; r = 1; break;                       /* CLEAR */
    case 0x1501: s = D3D12_BLEND_DEST_COLOR; sa = D3D12_BLEND_DEST_ALPHA; r = 2; break;   /* AND */
    case 0x1502: s = D3D12_BLEND_INV_DEST_COLOR; sa = D3D12_BLEND_INV_DEST_ALPHA; r = 2; break; /* AND_REVERSE */
    case 0x1503: t->BlendEnable = FALSE; return 1;                              /* COPY */
    case 0x1504: s = sa = D3D12_BLEND_ZERO; d = D3D12_BLEND_INV_SRC_COLOR; da = D3D12_BLEND_INV_SRC_ALPHA;
                 r = 2; break;                                                  /* AND_INVERTED */
    case 0x1505: t->BlendEnable = FALSE; t->RenderTargetWriteMask = 0; return 1; /* NOOP */
    case 0x1507: d = da = D3D12_BLEND_ONE; bo = D3D12_BLEND_OP_MAX; r = 2; break;  /* OR */
    default: return 0;
    }
    t->BlendEnable = TRUE;
    t->LogicOpEnable = FALSE;
    t->SrcBlend = s; t->DestBlend = d; t->BlendOp = bo;
    t->SrcBlendAlpha = sa; t->DestBlendAlpha = da; t->BlendOpAlpha = bo;
    return r;
}

/* ---- 16-bit colour targets in the RGBA8 layout ---------------------------
 * DXGI B5G6R5 / B5G5R5A1 pack like Vulkan's R5G6B5 / A1R5G5B5 PACK16 (blue in
 * the low bits); conversions as rsx_vulkan_backend.c vk_cv_*. kind 1: 565,
 * kind 2: 1555. */

static u8 d3g_ex(u32 v, u32 max) { return (u8)((v * 255u + max / 2u) / max); }
static u32 d3g_q(u32 v8, u32 max) { return (v8 * max + 127u) / 255u; }

void rsx_d3d12_cv_to_rgba(int kind, const uint8_t* s, uint8_t* d, uint32_t n)
{
    for (u32 i = 0; i < n; i++, d += 4) {
        if (kind == 3) { d[0] = d[1] = d[2] = d[3] = s[i]; continue; }
        if (kind == 4) { d[0] = d[2] = s[i * 2u]; d[1] = d[3] = s[i * 2u + 1u]; continue; }
        const u32 v = (u32)s[i * 2u] | ((u32)s[i * 2u + 1u] << 8);
        if (kind == 1) {
            d[0] = d3g_ex(v >> 11, 31u); d[1] = d3g_ex((v >> 5) & 63u, 63u);
            d[2] = d3g_ex(v & 31u, 31u); d[3] = 255u;
        } else {
            d[0] = d3g_ex((v >> 10) & 31u, 31u); d[1] = d3g_ex((v >> 5) & 31u, 31u);
            d[2] = d3g_ex(v & 31u, 31u); d[3] = (v & 0x8000u) ? 255u : 0u;
        }
    }
}

void rsx_d3d12_cv_from_rgba(int kind, const uint8_t* s, uint8_t* d, uint32_t n)
{
    for (u32 i = 0; i < n; i++, s += 4) {
        if (kind == 3) { d[i] = s[2]; continue; }
        if (kind == 4) { d[i * 2u] = s[2]; d[i * 2u + 1u] = s[1]; continue; }
        const u32 v = kind == 1
            ? (d3g_q(s[0], 31u) << 11) | (d3g_q(s[1], 63u) << 5) | d3g_q(s[2], 31u)
            : (s[3] >= 128u ? 0x8000u : 0u) | (d3g_q(s[0], 31u) << 10) | (d3g_q(s[1], 31u) << 5) | d3g_q(s[2], 31u);
        d[i * 2u] = (u8)v;
        d[i * 2u + 1u] = (u8)(v >> 8);
    }
}

/* ---- R8 / R8G8 colour targets (B8 / G8B8 surfaces) -------------------------
 * As rsx_vulkan_backend.c vk_cv_*: the image holds B8 in R, G8B8 as (B, G);
 * the engine still sees the RGBA8 lanes B8 = (B,B,B,B), G8B8 = (B,G,B,G).
 * kind 3: R8, kind 4: R8G8 (1 and 2 are the 16-bit kinds above). */

/* A D3D12 4-component mapping over the RGBA8 lanes, applied on top of the
 * native image's base mapping (R,R,R,R) / (R,G,R,G). */
uint32_t rsx_d3d12_cv_map(int kind, uint32_t map)
{
    static const u32 base3[4] = { 0, 0, 0, 0 }, base4[4] = { 0, 1, 0, 1 };
    const u32* b = kind == 3 ? base3 : kind == 4 ? base4 : NULL;
    if (!b) return map;
    u32 o[4];
    for (u32 c = 0; c < 4u; c++) {
        const u32 s = (map >> (3u * c)) & 7u;
        o[c] = s < 4u ? b[s] : s;
    }
    return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(o[0], o[1], o[2], o[3]);
}

/* An RGBA8-lane clear colour in the image's channels (RPCS3 get_b8_clear_color
 * / get_g8b8_clear_color). */
void rsx_d3d12_cv_clear(int kind, const float in[4], float out[4])
{
    memcpy(out, in, 4u * sizeof(float));
    if (kind == 3) out[0] = in[2];
    else if (kind == 4) { out[0] = in[2]; out[1] = in[1]; }
}

/* The colour write mask (bit0 R .. bit3 A over the RGBA8 lanes, D3D12's
 * order) in the image's channels (RPCS3 get_b8_colormask /
 * get_g8b8_r8g8_colormask); channels the format has not got follow. */
uint32_t rsx_d3d12_cv_write_mask(int kind, uint32_t m)
{
    if (kind == 3) return ((m >> 2) & 1u) ? 0xFu : 0u;
    if (kind == 4) { const u32 o = ((m >> 2) & 1u) | (m & 2u); return o ? (o | 0xCu) : 0u; }
    if (kind == 1 && (m & 7u)) return m | 8u;
    return m;
}

/* bytes per texel of a native kind */
uint32_t rsx_d3d12_cv_bpp(int kind) { return kind == 3 ? 1u : 2u; }

/* ---- render scale < 1 for the temporal upscalers ---------------------------
 * (rsx_d3d12_engine_up.inc) While the scene renders, its targets are drawn
 * into their top-left corner at RSX_UPSCALE x the guest size; textures read
 * from such a target get their coordinates scaled by the same factor. */

/* RSX_UPSCALE: the render scale in [0.25, 1); 1 = none. */
float rsx_d3d12_up_parse(const char* s)
{
    if (!s || !*s) return 1.0f;
    const double v = atof(s);
    if (!(v > 0.0) || v >= 0.999) return 1.0f;
    return v < 0.25 ? 0.25f : (float)v;
}

/* A guest extent at the render scale (never 0 for a non-zero extent). */
uint32_t rsx_d3d12_up_dim(uint32_t v, float u)
{
    if (!v) return 0;
    if (!(u > 0.0f) || u >= 1.0f) return v;
    const uint32_t r = (uint32_t)((double)v * u + 0.5);
    return r ? r : 1u;
}

/* A guest rectangle at the render scale: endpoints mapped, so neighbours
 * tile; never empty when it was not. */
void rsx_d3d12_up_rect(float u, uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h)
{
    if (!(u > 0.0f) || u >= 1.0f) return;
    const uint32_t x0 = rsx_d3d12_up_dim(*x, u), y0 = rsx_d3d12_up_dim(*y, u);
    uint32_t x1 = rsx_d3d12_up_dim(*x + *w, u), y1 = rsx_d3d12_up_dim(*y + *h, u);
    if (*w && x1 <= x0) x1 = x0 + 1u;
    if (*h && y1 <= y0) y1 = y0 + 1u;
    *x = x0; *y = y0; *w = *w ? x1 - x0 : 0u; *h = *h ? y1 - y0 : 0u;
}

/* Host MSAA (rsx_d3d12_engine_msaa.inc): the sample of the sx x sy twin that
 * holds widened pixel (x, y) -- RPCS3's / the Vulkan backend's row-major
 * order, which matches the D3D standard 4x pattern (0 TL, 1 TR, 2 BL, 3 BR). */
uint32_t rsx_d3d12_ms_sample(uint32_t sx, uint32_t sy, uint32_t x, uint32_t y)
{
    if (!sx || !sy) return 0u;
    return (y % sy) * sx + x % sx;
}

/* A widened scissor rectangle on the twin: floor the origin, ceil the end. */
void rsx_d3d12_ms_rect(uint32_t sx, uint32_t sy, uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h)
{
    if (!sx || !sy) return;
    const uint32_t x1 = (*x + *w + sx - 1u) / sx, y1 = (*y + *h + sy - 1u) / sy;
    *x /= sx; *y /= sy;
    *w = *w ? x1 - *x : 0u;     /* an empty scissor stays empty */
    *h = *h ? y1 - *y : 0u;
}

/* 1 when (sx, sy) has a host sample count (2x1 -> 2, 2x2 -> 4); *samples gets it. */
int rsx_d3d12_ms_samples(uint32_t sx, uint32_t sy, uint32_t* samples)
{
    if (sx < 1u || sy < 1u || sx > 2u || sy > 2u || sx * sy < 2u) return 0;
    if (samples) *samples = sx * sy;
    return 1;
}

/* Jitter phases: the DLSS / FSR / XeSS guidance, 8 x (output / render)^2. */
uint32_t rsx_d3d12_up_phases(float u)
{
    if (!(u > 0.0f) || u >= 0.999f) return 8u;
    const double n = ceil(8.0 / ((double)u * (double)u) - 1e-6);
    return n < 8.0 ? 8u : n > 64.0 ? 64u : (uint32_t)n;
}

/* Texture LOD bias for the scaled scene: log2(render / output) in quarter
 * steps, clamped to [-4, 0]. */
float rsx_d3d12_up_mip_bias(float u)
{
    if (!(u > 0.0f) || u >= 1.0f) return 0.0f;
    float b = floorf(log2f(u) * 4.0f + 0.5f) * 0.25f;
    return b < -4.0f ? -4.0f : b > 0.0f ? 0.0f : b;
}

/* XeSS 1.3+ quality setting (xess.h XESS_QUALITY_SETTING_*) for output / render =
 * ratio: the one with the largest fixed ratio not above it, so its render
 * size still covers the input (AA, 1:1, below 1.3). */
int rsx_d3d12_up_xess_quality(float ratio)
{
    static const struct { float r; int q; } t[] = {
        { 1.0f, 106 }, { 1.3f, 105 }, { 1.5f, 104 }, { 1.7f, 103 }, { 2.0f, 102 }, { 2.3f, 101 }, { 3.0f, 100 } };
    int best = 106;
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (t[i].r <= ratio + 0.01f) best = t[i].q;
    return best;
}

/* The PS with every 2D texture coordinate of the scaled sampling: the
 * argument after rsx_samp[N] becomes ((arg) * d3e_uvk[N/4].c), with the
 * factors in cbuffer D3EUp (b3). Cube and 3D units stay. malloc'd; NULL
 * when the shader samples nothing. */
static int d3g_unit_dim(const char* src, unsigned u)
{
    char pat[40];
    snprintf(pat, sizeof pat, "rsx_tex%u : register", u);
    const char* m = strstr(src, pat);
    if (!m) return 0;
    const char* ls = m;
    while (ls > src && ls[-1] != '\n') ls--;
    for (const char* q = ls; q < m; q++) {
        if (!strncmp(q, "TextureCube", 11)) return 1;
        if (!strncmp(q, "Texture3D", 9)) return 2;
    }
    return 0;
}

char* rsx_d3d12_ps_uv_scale(const char* ps)
{
    static const char decl[] = "SamplerState rsx_samp[16] : register(s0);\n";
    static const char cb[] = "cbuffer D3EUp : register(b3) { float4 d3e_uvk[4]; };\n";
    if (!ps) return NULL;
    const char* dp = strstr(ps, decl);
    if (!dp) return NULL;
    unsigned skip = 0;
    for (unsigned u = 0; u < 16u; u++)
        if (d3g_unit_dim(ps, u)) skip |= 1u << u;
    size_t n = strlen(ps), uses = 0;
    for (const char* p = strstr(ps, "rsx_samp["); p; p = strstr(p + 1, "rsx_samp[")) uses++;
    char* r = (char*)malloc(n + sizeof cb + uses * 40u + 1u);
    if (!r) return NULL;
    size_t o = 0;
    const char* p = ps;
    const char* after_decl = dp + sizeof decl - 1;
    while (*p) {
        if (p == after_decl) { memcpy(r + o, cb, sizeof cb - 1); o += sizeof cb - 1; }
        if (p > dp && !strncmp(p, "rsx_samp[", 9)) {
            char* e = NULL;
            const unsigned unit = (unsigned)strtoul(p + 9, &e, 10);
            const char* q = e;
            if (q && q[0] == ']' && unit < 16u && !((skip >> unit) & 1u)) {
                q++;
                while (*q == ' ') q++;
                if (*q == ',') {
                    q++;
                    while (*q == ' ') q++;
                    /* the argument: up to a top-level ',' or ')' */
                    const char* a = q;
                    int depth = 0;
                    while (*q && !(depth == 0 && (*q == ',' || *q == ')'))) {
                        if (*q == '(' || *q == '[') depth++;
                        else if (*q == ')' || *q == ']') depth--;
                        q++;
                    }
                    if (*q && q > a) {
                        const size_t head = (size_t)(a - p);
                        memcpy(r + o, p, head); o += head;
                        o += (size_t)sprintf(r + o, "((");
                        memcpy(r + o, a, (size_t)(q - a)); o += (size_t)(q - a);
                        o += (size_t)sprintf(r + o, ") * d3e_uvk[%u].%c)", unit >> 2, "xyzw"[unit & 3u]);
                        p = q;
                        continue;
                    }
                }
            }
        }
        r[o++] = *p++;
    }
    if (p == after_decl) { memcpy(r + o, cb, sizeof cb - 1); o += sizeof cb - 1; }
    r[o] = 0;
    return r;
}
