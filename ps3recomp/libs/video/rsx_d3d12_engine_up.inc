/* rsx_d3d12_engine_up.inc - render scale < 1 for the temporal upscalers
 * (RSX_UPSCALE; included by rsx_d3d12_engine.c after the draw path).
 *
 * The guest renders at its own resolution; the scene window (opened by the
 * game hooks at the first scene draw, closed by scene_aa or a present) makes
 * every target of the scene colour's size, and everything drawn together with
 * one, render into its top-left corner at u x its size ("marked" targets).
 * Viewports / scissors of those draws are scaled in d3e_draw, texture units
 * that read a marked target sample its corner (root constants b3,
 * rsx_d3d12_ps_uv_scale), and the copies / snapshots / clears below work on
 * the corner. scene_aa feeds the corner to DLSS / FSR 3.1 / XeSS at render
 * size and gets the full size back; any other marked target is resampled to
 * full size when the window closes or the guest reads it.
 *
 * Experimental and unverified in game: bilinear edges at the corner border,
 * unscaled SampleGrad gradients, partial-coverage targets stretched on
 * write-back and region views of surfaces are approximations. */

static int d3e_up_marked(const d3e_obj* o)
{
    return o && s_up.on && o->rsc_gen == s_up.gen;
}

static u32 d3e_up_cw(const d3e_obj* o) { return rsx_d3d12_up_dim(o->w, s_up.u); }
static u32 d3e_up_ch(const d3e_obj* o) { return rsx_d3d12_up_dim(o->h, s_up.u); }

/* The depth copy source for an in-place rescale of z (same size and class). */
static d3e_obj* d3e_up_twin(const d3e_obj* z)
{
    d3e_obj* t = d3e_get(s_up.twin, D3E_DEPTH);
    if (t && t->w == z->w && t->h == z->h && t->zcls == z->zcls) return t;
    if (t) d3e_release_obj(s_up.twin);
    s_up.twin = d3e_depth_new(z->w, z->h, z->zcls);
    return d3e_get(s_up.twin, D3E_DEPTH);
}

/* Depth (and stencil) rectangle (sx, sy, sw, sh) of z rescaled onto (dx, dy, dw, dh) of itself. */
static int d3e_up_depth_resample(u32 h, u32 sx, u32 sy, u32 sw, u32 sh, u32 dx, u32 dy, u32 dw, u32 dh)
{
    d3e_obj* z = d3e_get(h, D3E_DEPTH);
    if (!z) return 0;
    d3e_obj* t = d3e_up_twin(z);
    if (!t) {
        D3E_WARN_ONCE(34, "render scale: no depth copy target, depth left as it was\n");
        return 0;
    }
    z = &s_obj[h];
    d3e_query_close_open();
    d3e_trans(z, D3D12_RESOURCE_STATE_COPY_SOURCE);
    d3e_trans(t, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyResource(s_d.cl, t->res, z->res);
    return d3e_be_depth_target_copy(NULL, s_up.twin, sx, sy, sw, sh, h, dx, dy, dw, dh);
}

/* Full size -> corner (resample) or just the mark (the content is about to be
 * replaced). 0: the list is full, the window was closed. */
static int d3e_up_mark(u32 h, int resample)
{
    d3e_obj* o = d3e_get(h, 0);
    if (!o || (o->kind != D3E_COLOR && o->kind != D3E_DEPTH) || d3e_up_marked(o)) return 1;
    if (s_up.n >= D3E_UP_LIST) {
        D3E_WARN_ONCE(33, "render scale: more than %u targets in one scene window, closed early\n", D3E_UP_LIST);
        d3e_up_close(0);
        return 0;
    }
    const u32 W = o->w, H = o->h, cw = d3e_up_cw(o), ch = d3e_up_ch(o);
    if (resample) {
        if (o->kind == D3E_COLOR) d3e_blit(h, 0, 0, W, H, h, 0, 0, cw, ch, 1, 0);
        else if (!d3e_up_depth_resample(h, 0, 0, W, H, 0, 0, cw, ch)) return 1;   /* stays full size, unmarked */
    }
    o = &s_obj[h];
    o->rsc_gen = s_up.gen;
    s_up.list[s_up.n++] = h;
    return 1;
}

/* Corner -> full size and unmark. */
static void d3e_up_unmark(u32 h, int writeback)
{
    d3e_obj* o = d3e_get(h, 0);
    if (!d3e_up_marked(o)) return;
    const u32 W = o->w, H = o->h, cw = d3e_up_cw(o), ch = d3e_up_ch(o);
    o->rsc_gen = 0;
    if (!writeback) return;
    if (o->kind == D3E_COLOR) d3e_blit(h, 0, 0, cw, ch, h, 0, 0, W, H, 1, 0);
    else d3e_up_depth_resample(h, 0, 0, cw, ch, 0, 0, W, H);
}

static void d3e_up_close(u32 keep)
{
    if (!s_up.on) return;
    for (u32 i = 0; i < s_up.n; i++)
        d3e_up_unmark(s_up.list[i], s_up.list[i] != keep);
    s_up.n = 0;
    s_up.open = 0;
    if (++s_up.gen == 0u) s_up.gen = 1u;
}

/* The guest reads a target: it gets its full-size content back (and is
 * resampled again if drawn in the window later). */
static void d3e_up_flush(u32 h)
{
    if (s_up.on && s_up.n) d3e_up_unmark(h, 1);
}

static void d3e_up_reset(void)
{
    s_up.open = 0;
    s_up.n = 0;
    s_up.twin = 0;
    s_up.scaled = 0;
    if (++s_up.gen == 0u) s_up.gen = 1u;
}

static int d3e_up_qualifies(const d3e_obj* o)
{
    return o && (d3e_up_marked(o) || (o->w == s_up.ww && o->h == s_up.wh));
}

static int d3e_up_draw_begin(d3e_obj* const* rt, u32 n, d3e_obj* zo)
{
    if (!s_up.on || !s_up.open) return 0;
    int q = 0;
    for (u32 i = 0; i < n && !q; i++)
        if (rt[i] && (d3e_up_marked(rt[i]) || (i == 0 && d3e_up_qualifies(rt[i])))) q = 1;
    if (!q && !n && d3e_up_qualifies(zo)) q = 1;
    if (!q && zo && d3e_up_marked(zo)) q = 1;
    if (!q) return 0;
    for (u32 i = 0; i < n; i++)
        if (rt[i] && !d3e_up_mark((u32)(rt[i] - s_obj), 1)) return 0;
    if (zo && !d3e_up_mark((u32)(zo - s_obj), 1)) return 0;
    return 1;
}

/* ---- backend wrappers ------------------------------------------------------------- */

static u32 d3e_up_color_snapshot(void* user, u32 surface)
{
    d3e_obj* t = d3e_get(surface, D3E_COLOR);
    if (!d3e_up_marked(t)) return d3e_be_color_snapshot(user, surface);
    const u32 sn = d3e_snapshot(t, 0, 0, d3e_up_cw(t), d3e_up_ch(t));
    if (sn) s_obj[sn].rsc_rs = s_up.u;
    return sn;
}

static u32 d3e_up_color_region_snapshot(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h)
{
    d3e_obj* t = d3e_get(surface, D3E_COLOR);
    if (!d3e_up_marked(t)) return d3e_be_color_region_snapshot(user, surface, x, y, w, h);
    if (!w || !h || x >= t->w || y >= t->h || w > t->w - x || h > t->h - y) return 0;
    rsx_d3d12_up_rect(s_up.u, &x, &y, &w, &h);
    const u32 sn = d3e_be_color_region_snapshot(user, surface, x, y, w, h);
    if (sn) s_obj[sn].rsc_rs = s_up.u;
    return sn;
}

static u32 d3e_up_depth_snapshot(void* user, u32 depth, u32 w, u32 h)
{
    d3e_obj* z = d3e_get(depth, D3E_DEPTH);
    const int m = d3e_up_marked(z);
    if (m) { w = rsx_d3d12_up_dim(w, s_up.u); h = rsx_d3d12_up_dim(h, s_up.u); }
    const u32 sn = d3e_be_depth_snapshot(user, depth, w, h);
    if (sn) s_obj[sn].rsc_rs = m ? s_up.u : 0.0f;
    return sn;
}

static void d3e_up_rects(u32 src, u32* sx, u32* sy, u32* sw, u32* sh,
                         u32 dst, u32* dx, u32* dy, u32* dw, u32* dh)
{
    if (d3e_up_marked(d3e_get(src, 0))) rsx_d3d12_up_rect(s_up.u, sx, sy, sw, sh);
    if (d3e_up_marked(d3e_get(dst, 0))) rsx_d3d12_up_rect(s_up.u, dx, dy, dw, dh);
}

static void d3e_up_color_target_blit(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                                     u32 dst, u32 dx, u32 dy, u32 dw, u32 dh)
{
    d3e_up_rects(src, &sx, &sy, &sw, &sh, dst, &dx, &dy, &dw, &dh);
    d3e_be_color_target_blit(user, src, sx, sy, sw, sh, dst, dx, dy, dw, dh);
}

static void d3e_up_color_target_blit_filtered(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                                              u32 dst, u32 dx, u32 dy, u32 dw, u32 dh, int linear)
{
    d3e_up_rects(src, &sx, &sy, &sw, &sh, dst, &dx, &dy, &dw, &dh);
    d3e_be_color_target_blit_filtered(user, src, sx, sy, sw, sh, dst, dx, dy, dw, dh, linear);
}

static void d3e_up_color_target_blit_flip(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                                          u32 dst, u32 dx, u32 dy, u32 dw, u32 dh, int linear, u32 flip)
{
    d3e_up_rects(src, &sx, &sy, &sw, &sh, dst, &dx, &dy, &dw, &dh);
    d3e_be_color_target_blit_flip(user, src, sx, sy, sw, sh, dst, dx, dy, dw, dh, linear, flip);
}

static int d3e_up_depth_target_copy(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                                    u32 dst, u32 dx, u32 dy, u32 dw, u32 dh)
{
    d3e_up_rects(src, &sx, &sy, &sw, &sh, dst, &dx, &dy, &dw, &dh);
    return d3e_be_depth_target_copy(user, src, sx, sy, sw, sh, dst, dx, dy, dw, dh);
}

static void d3e_up_clear_color(void* user, u32 surface, const float rgba[4])
{
    if (s_up.open && d3e_up_qualifies(d3e_get(surface, D3E_COLOR))) d3e_up_mark(surface, 0);
    d3e_be_clear_color(user, surface, rgba);
}

static void d3e_up_clear_depth_stencil(void* user, u32 depth, u32 flags, float depth_value, u8 stencil)
{
    /* a depth-only clear keeps stencil: only a full clear can skip the resample */
    const int full = (flags & RSX_BE_CLEAR_DEPTH) && (flags & RSX_BE_CLEAR_STENCIL);
    if (s_up.open && d3e_up_qualifies(d3e_get(depth, D3E_DEPTH))) d3e_up_mark(depth, !full);
    d3e_be_clear_depth_stencil(user, depth, flags, depth_value, stencil);
}

static void d3e_up_clear_rect(void* user, const u32* rts, u32 n_rts, u32 depth, const float rgba[4],
                              u32 color_mask, u32 ds_flags, float depth_value, u32 stencil, u32 stencil_mask,
                              u32 x, u32 y, u32 w, u32 h)
{
    if (s_up.open) {
        if (n_rts > RSX_BE_MAX_COLOR_TARGETS) n_rts = RSX_BE_MAX_COLOR_TARGETS;
        const d3e_obj* lead = rts && n_rts ? d3e_get(rts[0], D3E_COLOR) : d3e_get(depth, D3E_DEPTH);
        int q = d3e_up_qualifies(lead);
        for (u32 i = 0; rts && i < n_rts && !q; i++) q = d3e_up_marked(d3e_get(rts[i], 0));
        if (!q && depth) q = d3e_up_marked(d3e_get(depth, 0));
        if (q) {
            int ok = 1;
            for (u32 i = 0; rts && i < n_rts && ok; i++) {
                const d3e_obj* o = d3e_get(rts[i], D3E_COLOR);
                const int whole = o && !x && !y && w >= o->w && h >= o->h && (color_mask & 0xFu) == 0xFu;
                ok = d3e_up_mark(rts[i], !whole);
            }
            if (ok && depth) {
                const d3e_obj* z = d3e_get(depth, D3E_DEPTH);
                const int whole = z && !x && !y && w >= z->w && h >= z->h &&
                                  (ds_flags & RSX_BE_CLEAR_DEPTH) && (ds_flags & RSX_BE_CLEAR_STENCIL) &&
                                  (stencil_mask & 0xFFu) == 0xFFu;
                ok = d3e_up_mark(depth, !whole);
            }
            if (ok) rsx_d3d12_up_rect(s_up.u, &x, &y, &w, &h);
        }
    }
    d3e_be_clear_rect(user, rts, n_rts, depth, rgba, color_mask, ds_flags, depth_value, stencil, stencil_mask,
                      x, y, w, h);
}

static void d3e_up_color_target_write(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                                      const void* rows, u32 row_bytes)
{
    d3e_up_flush(surface);
    d3e_be_color_target_write(user, surface, x, y, w, h, rows, row_bytes);
}

static void d3e_up_readback(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h, void* out, u32 out_pitch)
{
    d3e_up_flush(surface);
    d3e_be_readback(user, surface, x, y, w, h, out, out_pitch);
}

static int d3e_up_depth_readback(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h, float* out, u32 out_pitch)
{
    d3e_up_flush(depth);
    return d3e_be_depth_readback(user, depth, x, y, w, h, out, out_pitch);
}

static int d3e_up_stencil_readback(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h, u8* out, u32 out_pitch)
{
    d3e_up_flush(depth);
    return d3e_be_stencil_readback(user, depth, x, y, w, h, out, out_pitch);
}

static int d3e_up_zeta_readback(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h,
                                float* out_f, u32* out_raw, u32* raw_bits, u8* out_stencil, int* have_stencil)
{
    d3e_up_flush(depth);
    return d3e_be_zeta_readback(user, depth, x, y, w, h, out_f, out_raw, raw_bits, out_stencil, have_stencil);
}

static int d3e_up_texture_copy_from_color(void* user, u32 texture, u32 layer, u32 mip,
                                          u32 surface, u32 sx, u32 sy, u32 w, u32 h)
{
    d3e_up_flush(surface);
    return d3e_be_texture_copy_from_color(user, texture, layer, mip, surface, sx, sy, w, h);
}

static void d3e_up_present(void* user, u32 surface)
{
    d3e_up_close(0);
    d3e_be_present(user, surface);
}

static void d3e_up_present_region(void* user, u32 surface, u32 sx, u32 sy, u32 sw, u32 sh,
                                  u32 aspect_w, u32 aspect_h)
{
    d3e_up_close(0);
    d3e_be_present_region(user, surface, sx, sy, sw, sh, aspect_w, aspect_h);
}

static int d3e_up_scene_aa(void* user, const rsx_be_scene_aa* q)
{
    const int ok = d3e_scene_aa(user, q);
    d3e_up_close(ok && q ? q->color : 0u);
    return ok;
}

static void d3e_up_backend(rsx_draw_backend* b)
{
    b->color_snapshot = d3e_up_color_snapshot;
    b->color_region_snapshot = d3e_up_color_region_snapshot;
    b->depth_snapshot = d3e_up_depth_snapshot;
    b->color_target_blit = d3e_up_color_target_blit;
    b->color_target_blit_filtered = d3e_up_color_target_blit_filtered;
    b->color_target_blit_flip = d3e_up_color_target_blit_flip;
    b->depth_target_copy = d3e_up_depth_target_copy;
    b->clear_color = d3e_up_clear_color;
    b->clear_depth_stencil = d3e_up_clear_depth_stencil;
    b->clear_rect = d3e_up_clear_rect;
    b->color_target_write = d3e_up_color_target_write;
    b->readback = d3e_up_readback;
    b->depth_readback = d3e_up_depth_readback;
    b->stencil_readback = d3e_up_stencil_readback;
    b->zeta_readback = d3e_up_zeta_readback;
    b->texture_copy_from_color = d3e_up_texture_copy_from_color;
    b->present = d3e_up_present;
    b->present_region = d3e_up_present_region;
    b->scene_aa = d3e_up_scene_aa;
}

/* ---- API ---------------------------------------------------------------------------- */

float rsx_d3d12_engine_set_render_scale(float u)
{
    s_up.on = 0;
    s_up.u = 1.0f;
    s_up.bias = 0.0f;
    if (s_d.up) {
        D3E_LOG("render scale: must be set before init, staying 1:1\n");
        return 1.0f;
    }
    if (!(u > 0.0f && u < 1.0f)) return 1.0f;
    if (!d3e_env_on("RSX_D3D12_UPSCALE", 1)) {
        D3E_LOG("render scale %.3f: RSX_D3D12_UPSCALE=0, staying 1:1\n", (double)u);
        return 1.0f;
    }
    if (s_ua.kind < D3E_UP_DLSS || s_ua.kind > D3E_UP_XESS) {
        D3E_LOG("render scale %.3f: needs DLSS / FSR 3.1 / XeSS, staying 1:1\n", (double)u);
        return 1.0f;
    }
    if (u < 0.25f) u = 0.25f;
    s_up.on = 1;
    s_up.u = u;
    s_up.bias = rsx_d3d12_up_mip_bias(u);
    const char* mb = getenv("RSX_D3D12_UPSCALE_MIPBIAS");
    if (mb && *mb) {
        const float b = (float)atof(mb);
        s_up.bias = b < -4.0f ? -4.0f : b > 0.0f ? 0.0f : b;
    }
    d3e_up_reset();
    D3E_LOG("render scale %.3f (%s, texture LOD bias %.2f; experimental, unverified)\n",
            (double)s_up.u, s_up_name[s_ua.kind], (double)s_up.bias);
    return s_up.u;
}

void rsx_d3d12_engine_aa_window(int open, unsigned color_handle)
{
    if (!s_up.on || !s_d.up) return;
    if (!open) { d3e_up_close(0); return; }
    if (s_up.open) return;
    const d3e_obj* c = d3e_get(color_handle, 0);   /* the scene colour, or its depth for depth-only draws */
    if (!c || (c->kind != D3E_COLOR && c->kind != D3E_DEPTH)) return;
    s_up.ww = c->w;
    s_up.wh = c->h;
    s_up.open = 1;
}
