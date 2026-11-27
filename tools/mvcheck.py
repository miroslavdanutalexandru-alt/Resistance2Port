#!/usr/bin/env python3
"""Frame-generation / upscaler motion-vector quality check on RSX_MV_DUMP bursts.

For every pair of consecutive dumped frames (N-1, N) it follows each pixel of frame N along its motion vector into
frame N-1 and measures how far the content actually is from where the vector points: a 7x7 zero-mean normalised
cross-correlation (luminance, log-compressed HDR, so global brightness pops do not count) searched over +-R px around
the predicted position, with sub-pixel refinement. Per pixel:
  ok       best match within 1 px of where the vector points
  err1     1..3 px off          err3   more than 3 px off (still found within R)
  lost     no good match within R (disocclusion, lighting change, transparency, motion larger than the vector + R)
  flat     not measurable (no texture: sky, flat walls, dark)   off   the vector points off-screen
Percentages are of the measurable pixels (ok + err1 + err3 + lost); 'within 1 px' = ok / measurable.

Pixel classes (from the dump, later ones win): cam (camera-only motion), deform (vertex data changed since last frame,
"vh" != "vh_prev"), objmv (ID-redrawn draw that wrote its own velocity, draws.jsonl "op"), edge (within ~2 px of a 5 %
depth step: one vector cannot describe both layers), fx (effect coverage / fxid), hud (fg_ui alpha), disocc (where the
vector points, last frame's depth was all nearer: the surface was hidden), vm (first-person view model, depth < 0.5),
sky (cleared depth).

Vector conventions (r2aa_prep_pass): mv is in view UV units, previous position = current + mv, both unjittered
(ndc = pixel ndc - jndc). --mv fg_mv checks what frame generation was handed (same units: Streamline mvecScale =
view / image size). --cam recomputes camera-only vectors from the frame's T / zA / zB to compare.

Usage:  python tools/mvcheck.py <burst dir or mvdump dir> [--mv mv|fg_mv] [--cam] [--radius 6] [--png outdir]
        python tools/mvcheck.py --selftest
"""
import argparse
import json
import os
import sys

import numpy as np

try:
    import cv2  # optional: ~50x faster box filter / bilinear lookup
except ImportError:
    cv2 = None

VK = {97: ('<f2', 4), 100: ('<f4', 1), 83: ('<f2', 2), 9: ('u1', 1), 70: ('<u2', 1), 37: ('u1', 4), 44: ('u1', 4), 64: ('<u4', 1)}
CLASSES = ('cam', 'objmv', 'deform', 'fx', 'hud', 'sky', 'vm', 'edge', 'disocc')
BINS = ('ok', 'err1', 'err3', 'lost', 'flat', 'off')


def load_img(path, w, h, fmt):
    dt, ch = VK[fmt]
    a = np.fromfile(path, dtype=dt)
    a = a.reshape(h, w, ch) if ch > 1 else a.reshape(h, w)
    if fmt == 64:  # A2B10G10R10: alpha in the top 2 bits
        a = np.stack([(a & 1023), (a >> 10) & 1023, (a >> 20) & 1023, (a >> 30) * 341], -1).astype(np.float32) / 1023.0
    elif fmt in (37, 44):
        a = a.astype(np.float32) / 255.0
    elif fmt == 9:
        a = a.astype(np.float32) / 255.0
    return a


def read_jsonl(path):
    if not os.path.exists(path):
        return []
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        return [json.loads(l) for l in f if l.strip()]


class Burst:
    def __init__(self, d):
        self.dir = d
        self.meta = read_jsonl(os.path.join(d, 'frames.jsonl'))
        self.imgs = {}
        for e in read_jsonl(os.path.join(d, 'images.jsonl')):
            self.imgs.setdefault(e['f'], {})[e['name']] = e

    def frames(self):
        return sorted(self.imgs)

    def img(self, f, name):
        e = self.imgs.get(f, {}).get(name)
        if not e or e['vk_format'] not in VK:
            return None
        p = os.path.join(self.dir, 'f%02u_%s.bin' % (f, name))
        return load_img(p, e['w'], e['h'], e['vk_format']) if os.path.exists(p) else None

    def draws(self, f):
        return {d['i']: d for d in read_jsonl(os.path.join(self.dir, 'f%02u_draws.jsonl' % f))}


def fit(a, h, w):
    """crop / nearest-resample a map to h x w (ID maps follow the depth surface, which can be larger)"""
    if a is None:
        return None
    if a.shape[0] >= h and a.shape[1] >= w and (a.shape[0] < 2 * h and a.shape[1] < 2 * w):
        return a[:h, :w]
    ys = (np.arange(h) * a.shape[0] // h).clip(0, a.shape[0] - 1)
    xs = (np.arange(w) * a.shape[1] // w).clip(0, a.shape[1] - 1)
    return a[ys][:, xs]


def luma(c):
    y = 0.2126 * c[..., 0] + 0.7152 * c[..., 1] + 0.0722 * c[..., 2]
    y = np.nan_to_num(y.astype(np.float32), nan=0.0, posinf=64.0, neginf=0.0).clip(0, 64.0)
    return np.log1p(16.0 * y)


def box(a, r):
    """sum over a (2r+1)^2 window, edges clamped"""
    if cv2 is not None:
        return cv2.boxFilter(a.astype(np.float32), -1, (2 * r + 1, 2 * r + 1), normalize=False, borderType=cv2.BORDER_REPLICATE)
    p = np.pad(a, ((r + 1, r), (r + 1, r)), mode='edge').astype(np.float64)
    s = p.cumsum(0).cumsum(1)
    k = 2 * r + 1
    return (s[k:, k:] - s[:-k, k:] - s[k:, :-k] + s[:-k, :-k]).astype(np.float32)


def bilinear(img, x, y):
    h, w = img.shape
    if cv2 is not None:
        return cv2.remap(img.astype(np.float32), np.asarray(x, np.float32), np.asarray(y, np.float32), cv2.INTER_LINEAR,
                         borderMode=cv2.BORDER_REPLICATE)
    x = x.clip(0, w - 1.001)
    y = y.clip(0, h - 1.001)
    x0 = np.floor(x).astype(np.int32)
    y0 = np.floor(y).astype(np.int32)
    fx = x - x0
    fy = y - y0
    a = img[y0, x0] * (1 - fx) + img[y0, x0 + 1] * fx
    b = img[y0 + 1, x0] * (1 - fx) + img[y0 + 1, x0 + 1] * fx
    return a * (1 - fy) + b * fy


def cam_prev_lin(m, depth, h, w):
    """camera-predicted previous linear depth (lin = 1 - z, or z when inverted) of every pixel"""
    T = np.array(m['T_d'] if 'T_d' in m else m['T'], dtype=np.float64).reshape(4, 4).T
    vw, vh = m.get('vw', w), m.get('vh', h)
    jx, jy = m.get('jndc', [0.0, 0.0])
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float64)
    nx = (xs + 0.5) / vw * 2 - 1 - jx
    ny = (ys + 0.5) / vh * 2 - 1 - jy
    z = depth.astype(np.float64) * m['zA'] + m['zB']
    with np.errstate(all='ignore'):
        zp = (T[0, 2] * nx + T[1, 2] * ny + T[2, 2] * z + T[3, 2]) / (T[0, 3] * nx + T[1, 3] * ny + T[2, 3] * z + T[3, 3])
        zp = (zp - m['zB']) / (m['zA'] or 1.0)
    lp = zp if m.get('inverted', 0) else 1.0 - zp
    return np.where(np.isfinite(lp), lp, 0.0)


def cam_mv(m, depth, h, w):
    """camera-only vectors as r2aa_prep_pass computes them: q = T (ndc, d zA + zB, 1), mv = (q.xy / q.w - ndc) / 2"""
    T = np.array(m['T_d'] if 'T_d' in m else m['T'], dtype=np.float64).reshape(4, 4).T  # dumped row by row: T[c][r] after .T
    vw, vh = m.get('vw', w), m.get('vh', h)
    jx, jy = m.get('jndc', [0.0, 0.0])
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float64)
    nx = (xs + 0.5) / vw * 2 - 1 - jx
    ny = (ys + 0.5) / vh * 2 - 1 - jy
    z = depth.astype(np.float64) * m['zA'] + m['zB']
    q = [T[0, r] * nx + T[1, r] * ny + T[2, r] * z + T[3, r] for r in range(4)]
    with np.errstate(all='ignore'):
        mx = (q[0] / q[3] - nx) * 0.5
        my = (q[1] / q[3] - ny) * 0.5
    bad = ~np.isfinite(mx) | ~np.isfinite(my) | (np.abs(mx) > 2) | (np.abs(my) > 2) | (np.abs(q[3]) <= 1e-7)
    mx[bad] = 0
    my[bad] = 0
    return np.stack([mx, my], -1).astype(np.float32)


def classify(b, f, m, h, w, depth, mv=None, cam=None):
    """cam: the camera-predicted previous linear depth (disocclusion test), else the current one"""
    cls = np.zeros((h, w), np.uint8)  # 0 cam
    dr = b.draws(f)
    idm = fit(b.img(f, 'id'), h, w)
    if idm is not None and dr:
        ids = idm.astype(np.int64)
        n = max(dr) + 1
        om = np.zeros(n + 1, bool)
        de = np.zeros(n + 1, bool)
        for i, d in dr.items():
            if i <= n:
                om[i] = d.get('op', 0) == 1 or d.get('om', -1) > 0
                de[i] = bool(d.get('vh_prev')) and d.get('vh') != d.get('vh_prev')
        ids = ids.clip(0, n)
        cls[de[ids]] = 2
        cls[om[ids]] = 1
    fx = fit(b.img(f, 'fx'), h, w)
    fxid = fit(b.img(f, 'fxid'), h, w)
    if fx is not None:
        cls[fx > 0.02] = 3
    if fxid is not None:
        cls[fxid > 0] = 3
    ui = b.img(f, 'fg_ui')
    if ui is not None and ui.ndim == 3:
        cls[fit(ui[..., 3], h, w) > 0.02] = 4
    inv = bool(m.get('inverted', 0))
    lin = depth.astype(np.float64) if inv else 1.0 - depth.astype(np.float64)  # ~ 1 / view distance
    # depth edge: a 3x3 neighbour more than 5 % nearer or farther (one vector per pixel cannot describe both layers)
    pad = np.pad(lin, 1, mode='edge')
    lo = hi = lin
    for oy in range(3):
        for ox in range(3):
            n_ = pad[oy:oy + h, ox:ox + w]
            lo = np.minimum(lo, n_)
            hi = np.maximum(hi, n_)
    edge = (hi > lin * 1.05 + 1e-6) | (lo < lin * 0.95 - 1e-6)
    edge = np.pad(edge, 1, mode='edge')
    edge = edge[:-2, 1:-1] | edge[2:, 1:-1] | edge[1:-1, :-2] | edge[1:-1, 2:] | edge[1:-1, 1:-1]
    cls[edge] = 7
    # disocclusion: where the vector points, last frame's 3x3 depth was all nearer than this surface (it was hidden)
    dp = b.img(f, 'depth_prev')
    if mv is not None and dp is not None:
        dp = fit(dp, h, w).astype(np.float64)
        lp = dp if inv else 1.0 - dp
        if cam is not None:
            want = cam
        else:
            want = lin
        ys, xs = np.mgrid[0:h, 0:w]
        ix = np.floor(xs + 0.5 + mv[..., 0] * m.get('vw', w)).astype(np.int64)
        iy = np.floor(ys + 0.5 + mv[..., 1] * m.get('vh', h)).astype(np.int64)
        near = np.full((h, w), True)
        for oy in (-1, 0, 1):
            for ox in (-1, 0, 1):
                near &= lp[np.clip(iy + oy, 0, h - 1), np.clip(ix + ox, 0, w - 1)] > want * 1.05 + 1e-6
        cls[near] = 8
    cls[lin > 0.5] = 6  # first-person view model: depth range 0.15 .. 0.18 (world >= 0.89)
    clear = 0.0 if inv else 1.0
    cls[np.abs(depth - clear) < 1e-7] = 5
    return cls


def dilate(mv, depth, inverted=False):
    """closest-depth 3x3 dilation, as DLSS / FSR apply internally to render-resolution (undilated) vectors"""
    h, w = depth.shape
    d = -depth if inverted else depth
    pd = np.pad(d, 1, mode='edge')
    pm = np.pad(mv, ((1, 1), (1, 1), (0, 0)), mode='edge')
    best, out = d.copy(), mv.copy()
    for oy in range(3):
        for ox in range(3):
            n_ = pd[oy:oy + h, ox:ox + w]
            u = n_ < best
            best = np.where(u, n_, best)
            out = np.where(u[..., None], pm[oy:oy + h, ox:ox + w], out)
    return out


def measure(cur, prev, mv, m_cur, m_prev, radius, patch=3, margin=0.02):
    """per-pixel error (px), bin index, best ZNCC"""
    h, w = cur.shape
    vw, vh = m_cur.get('vw', w), m_cur.get('vh', h)
    jc = m_cur.get('jndc', [0.0, 0.0])
    jp = m_prev.get('jndc', [0.0, 0.0])
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    # unjittered sample point of this pixel, moved by the vector, re-jittered with the previous frame's offset
    px = xs + mv[..., 0] * vw + (jp[0] - jc[0]) * vw * 0.5
    py = ys + mv[..., 1] * vh + (jp[1] - jc[1]) * vh * 0.5
    off = (px < 0) | (py < 0) | (px > w - 1) | (py > h - 1)
    n = float((2 * patch + 1) ** 2)
    sc = box(cur, patch)
    mc = sc / n
    vc = box(cur * cur, patch) / n - mc * mc
    # measurable: textured in both directions (structure tensor, smaller eigenvalue) - flat / 1-D edges are ambiguous
    gx = np.zeros_like(cur)
    gy = np.zeros_like(cur)
    gx[:, 1:-1] = (cur[:, 2:] - cur[:, :-2]) * 0.5
    gy[1:-1] = (cur[2:] - cur[:-2]) * 0.5
    a, bb, c = box(gx * gx, patch) / n, box(gx * gy, patch) / n, box(gy * gy, patch) / n
    lmin = (a + c) * 0.5 - np.sqrt(((a - c) * 0.5) ** 2 + bb * bb)
    meas = (vc > 4e-3) & (lmin > 4e-4)
    best = np.full((h, w), -2.0, np.float32)
    bx = np.zeros((h, w), np.int8)
    by = np.zeros((h, w), np.int8)
    zero = None
    scores = {}
    for oy in range(-radius, radius + 1):
        for ox in range(-radius, radius + 1):
            wp = bilinear(prev, px + ox, py + oy)
            sw = box(wp, patch) / n
            vwp = box(wp * wp, patch) / n - sw * sw
            cov = box(cur * wp, patch) / n - mc * sw
            z = cov / np.sqrt(np.maximum(vc * vwp, 1e-12))
            if ox == 0 and oy == 0:
                zero = z
            if abs(ox) <= 1 and abs(oy) <= 1:
                scores[(ox, oy)] = z
            upd = z > best
            best = np.where(upd, z, best)
            bx = np.where(upd, ox, bx).astype(np.int8)
            by = np.where(upd, oy, by).astype(np.int8)
    # ties / near-ties with the predicted position count as on target (repeating texture, aperture)
    on = (best - zero) < margin

    def zn(ox, oy):
        wp = bilinear(prev, px + ox, py + oy)
        sw = box(wp, patch) / n
        vwp = box(wp * wp, patch) / n - sw * sw
        return (box(cur * wp, patch) / n - mc * sw) / np.sqrt(np.maximum(vc * vwp, 1e-12))

    def vertex(l, c, r):
        den = l - 2 * c + r
        with np.errstate(all='ignore'):
            return np.where(den < -1e-6, np.clip(0.5 * (l - r) / den, -0.5, 0.5), 0)

    # sub-pixel around the best integer offset (parabola through its 4 neighbours)
    fbx, fby = bx.astype(np.float32), by.astype(np.float32)
    ex = np.where(on, 0, fbx + vertex(zn(fbx - 1, fby), best, zn(fbx + 1, fby)))
    ey = np.where(on, 0, fby + vertex(zn(fbx, fby - 1), best, zn(fbx, fby + 1)))
    err = np.hypot(ex, ey)
    measure.last = (ex, ey, on)
    # on target: sub-pixel residual at the predicted position (parabola vertex, at most half a pixel per axis)
    zl, zr, zu, zd = scores[(-1, 0)], scores[(1, 0)], scores[(0, -1)], scores[(0, 1)]
    err = np.where(on, np.hypot(vertex(zl, zero, zr), vertex(zu, zero, zd)), err)
    bins = np.where(err <= 1.0, 0, np.where(err <= 3.0, 1, 2)).astype(np.uint8)
    bins[best < 0.7] = 3
    bins[~meas] = 4
    bins[off] = 5
    return err, bins, best


def summarise(bins, cls):
    out = {}
    for ci, cn in enumerate(CLASSES + ('all',)):
        sel = bins if cn == 'all' else bins[cls == ci]
        cnt = np.bincount(sel.ravel(), minlength=6).astype(np.float64)
        mcount = cnt[:4].sum()
        out[cn] = {'px': int(sel.size), 'measurable': int(mcount),
                   **{BINS[i]: (100.0 * cnt[i] / mcount if mcount and i < 4 else 100.0 * cnt[i] / max(sel.size, 1)) for i in range(6)}}
    return out


def heatmap(path, cur, bins, cls):
    from PIL import Image
    g = (cur / max(float(cur.max()), 1e-6) * 90).astype(np.uint8)
    rgb = np.stack([g, g, g], -1)
    rgb[bins == 0] = (rgb[bins == 0] * 0.6 + np.array([0, 60, 0])).astype(np.uint8)
    rgb[bins == 1] = (255, 200, 0)
    rgb[bins == 2] = (255, 0, 0)
    rgb[bins == 3] = (40, 90, 255)
    rgb[bins == 5] = (120, 0, 120)
    Image.fromarray(rgb).save(path)
    pal = np.array([[90, 90, 90], [0, 220, 0], [220, 220, 0], [255, 0, 255], [255, 255, 255], [0, 160, 255], [140, 70, 20],
                    [255, 120, 0], [255, 0, 0]], np.uint8)
    Image.fromarray(pal[cls]).save(path.replace('.png', '_cls.png'))


def run_burst(d, args, report):
    b = Burst(d)
    fr = b.frames()
    meta = {i: m for i, m in enumerate(b.meta)}
    tot = {}
    for a, c in zip(fr, fr[1:]):
        ma, mc = meta.get(a, {}), meta.get(c, {})
        if mc.get('frame', 0) != ma.get('frame', -1) + 1 or mc.get('reset') or mc.get('fg_reset'):
            continue
        rw, rh = mc.get('rw'), mc.get('rh')
        col_c, col_p = b.img(c, 'color'), b.img(a, 'color')
        dep = b.img(c, 'depth')
        mv = b.img(c, args.mv)
        if col_c is None or col_p is None or dep is None or (mv is None and not args.cam):
            continue
        h, w = rh or col_c.shape[0], rw or col_c.shape[1]
        cur, prev = luma(col_c[:h, :w]), luma(col_p[:h, :w])
        dep = fit(dep, h, w).astype(np.float32)
        mv0 = fit(mv, h, w).astype(np.float32) if mv is not None else cam_mv(mc, dep, h, w)
        cls = classify(b, c, mc, h, w, dep, mv0, cam_prev_lin(mc, dep, h, w))
        sets = []
        if mv is not None:
            sets.append((args.mv, fit(mv, h, w).astype(np.float32)))
        if args.cam:
            sets.append(('cam', cam_mv(mc, dep, h, w)))
        if args.dilate:
            sets = [(n_, dilate(v_, dep, bool(mc.get('inverted', 0)))) for n_, v_ in sets]
        mcm, mam, clsm = mc, ma, cls
        if args.domain == 'out':
            # the upscaler's output (what frame generation interpolates): unjittered and anti-aliased, so the aliasing of the
            # jittered render-resolution input does not count as motion error; vectors / classes scaled up (nearest)
            oc, op = b.img(c, 'out'), b.img(a, 'out')
            if oc is None or op is None:
                continue
            oh, ow = mc.get('oh') or oc.shape[0], mc.get('ow') or oc.shape[1]
            cur, prev = luma(oc[:oh, :ow]), luma(op[:oh, :ow])
            sets = [(n_, fit(v_, oh, ow)) for n_, v_ in sets]
            clsm = fit(cls, oh, ow)
            mcm = dict(mc, jndc=[0.0, 0.0], vw=float(ow), vh=float(oh))
            mam = dict(ma, jndc=[0.0, 0.0])
        for name, v in sets:
            err, bins, _ = measure(cur, prev, v, mcm, mam, args.radius, margin=args.margin)
            cls = clsm
            s = summarise(bins, cls)
            key = (os.path.basename(d), name)
            t = tot.setdefault(name, np.zeros((len(CLASSES) + 1, 6)))
            for ci in range(len(CLASSES)):
                t[ci] += np.bincount(bins[cls == ci].ravel(), minlength=6)
            t[-1] += np.bincount(bins.ravel(), minlength=6)
            report.append({'burst': key[0], 'pair': [a, c], 'frame': mc.get('frame'), 'var': mc.get('var'), 'mv': name, 'stats': s})
            al = s['all']
            print('%s f%02u->f%02u %-5s var %s  within1 %6.2f%%  >1 %5.2f%%  >3 %5.2f%%  lost %5.2f%%  (measurable %d)  %s' % (
                key[0], a, c, name, mc.get('var', '?'), al['ok'], al['err1'] + al['err3'], al['err3'], al['lost'], al['measurable'],
                ' '.join('%s:%.1f' % (cn, s[cn]['ok']) for cn in CLASSES if s[cn]['measurable'] > 200)))
            if args.png:
                os.makedirs(args.png, exist_ok=True)
                heatmap(os.path.join(args.png, '%s_f%02u_%s.png' % (key[0], c, name)), cur, bins, cls)
    return tot


def print_totals(label, tot):
    for name, t in tot.items():
        print('== %s %s' % (label, name))
        for ci, cn in enumerate(CLASSES + ('all',)):
            m = t[ci][:4].sum()
            if m < 1:
                continue
            print('  %-6s measurable %9d  within1 %6.2f%%  >1 %5.2f%%  >3 %5.2f%%  lost %5.2f%%' % (
                cn, m, 100 * t[ci][0] / m, 100 * (t[ci][1] + t[ci][2]) / m, 100 * t[ci][2] / m, 100 * t[ci][3] / m))


def selftest():
    """synthetic burst: textured plane moving right 2 px/frame; the vectors are correct except in one block (4 px off)"""
    import tempfile
    rng = np.random.default_rng(1)
    w, h, k = 160, 96, 3
    base = rng.random((h + 8, w + 40)).astype(np.float32)
    base = (base + np.roll(base, 1, 0) + np.roll(base, 1, 1)) / 3
    d = tempfile.mkdtemp(prefix='mvcheck_', dir=os.path.dirname(os.path.abspath(__file__)))
    try:
        man, meta = [], []
        for f in range(k):
            x0 = 20 - 2 * f  # content moves right: this pixel showed (x - 2) last frame
            y = base[4:4 + h, x0:x0 + w]
            col = np.stack([y, y, y, np.ones_like(y)], -1).astype('<f2')
            mv = np.zeros((h, w, 2), '<f2')
            mv[..., 0] = -2.0 / w
            mv[30:60, 40:80, 0] = -6.0 / w  # wrong by 4 px
            mv[70:90, 100:140, 0] = -3.5 / w  # wrong by 1.5 px
            dep = np.full((h, w), 0.5, '<f4')
            for name, a, fmt, bpp in (('color', col, 97, 8), ('mv', mv, 83, 4), ('depth', dep, 100, 4)):
                a.tofile(os.path.join(d, 'f%02u_%s.bin' % (f, name)))
                man.append({'f': f, 'frame': 100 + f, 'name': name, 'w': w, 'h': h, 'bpp': bpp, 'vk_format': fmt})
            meta.append({'frame': 100 + f, 'rw': w, 'rh': h, 'vw': w, 'vh': h, 'jndc': [0, 0], 'reset': 0, 'fg_reset': 0,
                         'zA': 1, 'zB': 0, 'inverted': 0, 'T': [1, 0, 0, -4.0 / w, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]})
        with open(os.path.join(d, 'images.jsonl'), 'w') as o:
            o.write('\n'.join(json.dumps(e) for e in man) + '\n')
        with open(os.path.join(d, 'frames.jsonl'), 'w') as o:
            o.write('\n'.join(json.dumps(e) for e in meta) + '\n')
        a = argparse.Namespace(mv='mv', cam=True, radius=6, png=None, margin=0.02, domain='render', dilate=False)
        rep = []
        tot = run_burst(d, a, rep)
        t = tot['mv'][-1]
        m = t[:4].sum()
        bad = 100 * (t[2]) / m
        exp = 100 * 30 * 40 / (h * w)
        assert abs(bad - exp) < 3.0, (bad, exp)
        mid = 100 * t[1] / m
        exp1 = 100 * 20 * 40 / (h * w)
        assert abs(mid - exp1) < 2.5, (mid, exp1)
        tc = tot['cam'][-1]
        assert 100 * tc[0] / tc[:4].sum() > 97, tc  # T says 2 px right everywhere: correct
        print('selftest ok: wrong block %.1f%% >3 px (expected ~%.1f%%), camera T %.1f%% within 1 px' % (bad, exp, 100 * tc[0] / tc[:4].sum()))
    finally:
        for f in os.listdir(d):
            os.remove(os.path.join(d, f))
        os.rmdir(d)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('path', nargs='?')
    ap.add_argument('--mv', default='mv', help='vector image to check: mv (upscaler) or fg_mv (frame generation)')
    ap.add_argument('--cam', action='store_true', help='also check camera-only vectors recomputed from T')
    ap.add_argument('--radius', type=int, default=6)
    ap.add_argument('--domain', default='out', choices=('out', 'render'), help='images compared: the upscaler output (default) or its jittered input')
    ap.add_argument('--margin', type=float, default=0.05, help='ZNCC gain a shifted match needs over the predicted position')
    ap.add_argument('--png', help='write heatmaps here (green ok, yellow >1, red >3, blue lost, purple off-screen)')
    ap.add_argument('--json', help='write the per-pair report here')
    ap.add_argument('--dilate', action='store_true', help='score the vectors after the closest-depth 3x3 dilation DLSS / FSR apply')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    if args.selftest:
        selftest()
        return
    if not args.path:
        ap.error('path required')
    dirs = [args.path] if os.path.exists(os.path.join(args.path, 'images.jsonl')) else sorted(
        os.path.join(args.path, x) for x in os.listdir(args.path) if os.path.exists(os.path.join(args.path, x, 'images.jsonl')))
    if not dirs:
        sys.exit('no bursts (images.jsonl) under %s' % args.path)
    report, grand = [], {}
    for d in dirs:
        tot = run_burst(d, args, report)
        print_totals(os.path.basename(d), tot)
        for k, v in tot.items():
            grand[k] = grand.get(k, 0) + v
    if len(dirs) > 1:
        print_totals('ALL', grand)
    if args.json:
        with open(args.json, 'w') as o:
            json.dump(report, o, indent=1)


if __name__ == '__main__':
    main()
