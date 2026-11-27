#!/usr/bin/env python3
"""Offline motion-vector dilation experiment on RSX_MV_DUMP bursts (scored with mvcheck.py's matcher).

Every pair of consecutive frames is scored once per vector variant against the same images, classes and measurable mask:
  base   the vectors as dumped (--mv fg_mv: what frame generation was handed)
  d3     each pixel takes the vector of the nearest-to-camera pixel of its 3x3 neighbourhood (FSR 2/3 / DLSS style)
  d5     the same over 5x5
  d3e    3x3, but only where that nearest neighbour is clearly in front (linear depth step > --edge, default 5 %)
  d5e    5x5, edge-limited the same way
Besides the usual totals it reports:
  grass  pixels of alpha-tested / discarding scene draws (draws.jsonl at / kil) that write depth: grass, foliage, fences
  chg    only the pixels whose vector the variant changed by more than 0.25 render px, scored before and after: dilation
         cannot win by moving the matcher - an unchanged vector scores identically, so all of the difference is here
  still  pairs whose camera did not move (game_vp == game_vp_prev): anything left is object / vertex-animation motion

Usage:  python tools/mvdilate.py <mvdump dir or burst> [--mv fg_mv] [--variants base,d3,d5,d3e,d5e] [--jobs 4] [--json out]
"""
import argparse
import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mvcheck as M  # noqa: E402


def lin_depth(depth, inverted):
    return depth.astype(np.float32) if inverted else 1.0 - depth.astype(np.float32)  # ~ 1 / view distance


def dilate(mv, depth, inverted, r, edge=0.0):
    """closest-depth (2r+1)^2 dilation; edge > 0: only where the closest neighbour is > edge nearer (linear depth)"""
    h, w = depth.shape
    lin = lin_depth(depth, inverted)
    pl = np.pad(lin, r, mode='edge')
    pm = np.pad(mv, ((r, r), (r, r), (0, 0)), mode='edge')
    best, out = lin.copy(), mv.copy()
    for oy in range(2 * r + 1):
        for ox in range(2 * r + 1):
            n_ = pl[oy:oy + h, ox:ox + w]
            u = n_ > best
            best = np.where(u, n_, best)
            out = np.where(u[..., None], pm[oy:oy + h, ox:ox + w], out)
    if edge > 0:
        keep = best > lin * (1.0 + edge) + 1e-7
        out = np.where(keep[..., None], out, mv)
    return out


def grass_mask(b, f, h, w, depth):
    """pixels whose ID-map draw is an alpha-tested / kill depth-writing scene draw"""
    dr = b.draws(f)
    idm = M.fit(b.img(f, 'id'), h, w)
    if idm is None or not dr:
        return None
    n = max(dr) + 1
    g = np.zeros(n + 1, bool)
    for i, d in dr.items():
        if i <= n and d.get('scene', 1) and d.get('dw', 0) and (d.get('at', 0) or d.get('kil', 0)) and not d.get('fx', 0):
            g[i] = True
    return g[idm.astype(np.int64).clip(0, n)]


def make_variants(names, mv, dep, inv, edge):
    out = {}
    for n_ in names:
        if n_ == 'base':
            out[n_] = mv
        elif n_ in ('d3', 'd5', 'd7'):
            out[n_] = dilate(mv, dep, inv, int(n_[1]) // 2)
        elif n_ in ('d3e', 'd5e'):
            out[n_] = dilate(mv, dep, inv, int(n_[1]) // 2, edge)
        else:
            raise SystemExit('unknown variant %s' % n_)
    return out


def tally(bins, sel):
    return np.bincount(bins[sel].ravel(), minlength=6)[:6].astype(np.int64) if sel is not None else np.zeros(6, np.int64)


def run_burst(d, args):
    b = M.Burst(d)
    fr = b.frames()
    meta = {i: m for i, m in enumerate(b.meta)}
    names = args.variants.split(',')
    res = []
    for a, c in zip(fr, fr[1:]):
        ma, mc = meta.get(a, {}), meta.get(c, {})
        if mc.get('frame', 0) != ma.get('frame', -1) + 1 or mc.get('reset') or mc.get('fg_reset'):
            continue
        rw, rh = mc.get('rw'), mc.get('rh')
        dep = b.img(c, 'depth')
        mv = b.img(c, args.mv)
        oc, op = b.img(c, 'out'), b.img(a, 'out')
        if dep is None or mv is None or oc is None or op is None:
            continue
        h, w = rh, rw
        inv = bool(mc.get('inverted', 0))
        dep = M.fit(dep, h, w).astype(np.float32)
        mv0 = M.fit(mv, h, w).astype(np.float32)
        cls = M.classify(b, c, mc, h, w, dep, mv0, M.cam_prev_lin(mc, dep, h, w))
        grass = grass_mask(b, c, h, w, dep)
        still = mc.get('game_vp') is not None and mc.get('game_vp') == mc.get('game_vp_prev')
        oh, ow = mc.get('oh') or oc.shape[0], mc.get('ow') or oc.shape[1]
        cur, prev = M.luma(oc[:oh, :ow]), M.luma(op[:oh, :ow])
        mcm = dict(mc, jndc=[0.0, 0.0], vw=float(ow), vh=float(oh))
        mam = dict(ma, jndc=[0.0, 0.0])
        clso = M.fit(cls, oh, ow)
        grasso = M.fit(grass, oh, ow) if grass is not None else None
        vs = make_variants(names, mv0, dep, inv, args.edge)
        pix = np.array([w, h], np.float32)
        binsd = {}
        for n_, v in vs.items():
            _, bins, _ = M.measure(cur, prev, M.fit(v, oh, ow), mcm, mam, args.radius, margin=args.margin)
            binsd[n_] = bins
        row = {'burst': os.path.basename(d), 'pair': [a, c], 'frame': mc.get('frame'), 'still': bool(still),
               'cam_px': float(np.median(np.hypot(*(M.cam_mv(mc, dep, h, w) * pix).transpose(2, 0, 1)))), 'v': {}}
        for n_, bins in binsd.items():
            e = {'all': tally(bins, np.ones_like(clso, bool)).tolist(),
                 'grass': tally(bins, grasso).tolist() if grasso is not None else None,
                 'cls': {cn: tally(bins, clso == ci).tolist() for ci, cn in enumerate(M.CLASSES)}}
            if n_ != 'base' and 'base' in binsd:
                chg = np.hypot(*((vs[n_] - mv0) * pix).transpose(2, 0, 1)) > 0.25
                chgo = M.fit(chg, oh, ow)
                e['chg_frac'] = float(chg.mean())
                e['chg_before'] = tally(binsd['base'], chgo).tolist()
                e['chg_after'] = tally(bins, chgo).tolist()
                if grasso is not None:
                    e['chg_grass_before'] = tally(binsd['base'], chgo & grasso).tolist()
                    e['chg_grass_after'] = tally(bins, chgo & grasso).tolist()
            row['v'][n_] = e
        res.append(row)
        print('%s f%02u->f%02u %s cam %.1f px  %s' % (row['burst'], a, c, 'STILL' if still else 'moving', row['cam_px'], '  '.join(
            '%s %.2f%%' % (n_, w1(e['all'])) + (' (g %.1f)' % w1(e['grass']) if e['grass'] else '') for n_, e in row['v'].items())), flush=True)
    return res


def w1(t):
    t = np.asarray(t, np.float64)
    m = t[:4].sum()
    return 100.0 * t[0] / m if m else float('nan')


def meas(t):
    return int(np.asarray(t)[:4].sum())


def report(rows, names):
    bursts = sorted({r['burst'] for r in rows})

    def agg(sel, key, sub=None):
        t = np.zeros(6, np.int64)
        for r in sel:
            for n_ in [key]:
                e = r['v'][n_]
                x = e.get(sub) if sub and not sub.startswith('cls:') else (e['cls'][sub[4:]] if sub else e['all'])
                if x is not None:
                    t += np.asarray(x, np.int64)
        return t

    print('\nwithin 1 px (output domain) per burst; grass = alpha-tested depth-writing draws')
    print('%-8s %5s %6s | ' % ('burst', 'still', 'cam') + ' | '.join('%-15s' % n_ for n_ in names))
    for bu in bursts + ['ALL']:
        sel = [r for r in rows if bu == 'ALL' or r['burst'] == bu]
        st = sum(r['still'] for r in sel)
        cam = np.median([r['cam_px'] for r in sel])
        cells = []
        for n_ in names:
            cells.append('%6.2f g%6.2f' % (w1(agg(sel, n_)), w1(agg(sel, n_, 'grass'))))
        print('%-8s %2d/%-2d %6.1f | ' % (bu, st, len(sel), cam) + ' | '.join(cells))
    print('\nchanged pixels only (vector moved > 0.25 px): within 1 px before -> after, share of render pixels changed')
    for bu in bursts + ['ALL']:
        sel = [r for r in rows if bu == 'ALL' or r['burst'] == bu]
        cells = []
        for n_ in names:
            if n_ == 'base':
                continue
            bf, af = agg(sel, n_, 'chg_before'), agg(sel, n_, 'chg_after')
            fr = np.mean([r['v'][n_]['chg_frac'] for r in sel]) * 100
            cells.append('%s %5.1f->%5.1f (%4.1f%% px, %d meas)' % (n_, w1(bf), w1(af), fr, meas(bf)))
        print('%-8s ' % bu + ' | '.join(cells))
    print('\nper class, all bursts (within 1 px)')
    for cn in M.CLASSES:
        cells = ['%s %6.2f' % (n_, w1(agg(rows, n_, 'cls:' + cn))) for n_ in names]
        print('  %-6s meas %9d  ' % (cn, meas(agg(rows, 'base', 'cls:' + cn))) + '  '.join(cells))
    stl = [r for r in rows if r['still']]
    if stl:
        print('\ncamera still pairs (%d): grass within 1 px %s, all %s, grass measurable %d' % (
            len(stl), '/'.join('%.2f' % w1(agg(stl, n_, 'grass')) for n_ in names),
            '/'.join('%.2f' % w1(agg(stl, n_)) for n_ in names), meas(agg(stl, 'base', 'grass'))))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('path', nargs='?')
    ap.add_argument('--mv', default='mv', help='mv (identical to fg_mv in every dumped frame that has both)')
    ap.add_argument('--variants', default='base,d3,d5,d3e,d5e')
    ap.add_argument('--edge', type=float, default=0.05)
    ap.add_argument('--radius', type=int, default=6)
    ap.add_argument('--margin', type=float, default=0.05)
    ap.add_argument('--jobs', type=int, default=1)
    ap.add_argument('--json')
    ap.add_argument('--load', help='re-print the report of an earlier --json')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    if args.selftest:
        selftest()
        return
    if args.load:
        with open(args.load) as f:
            rows = json.load(f)
        report(rows, list(rows[0]['v']))
        return
    if not args.path:
        ap.error('path required')
    dirs = [args.path] if os.path.exists(os.path.join(args.path, 'images.jsonl')) else sorted(
        os.path.join(args.path, x) for x in os.listdir(args.path) if os.path.exists(os.path.join(args.path, x, 'images.jsonl')))
    rows = []
    if args.jobs > 1:
        with ProcessPoolExecutor(args.jobs) as ex:
            for r in ex.map(run_burst, dirs, [args] * len(dirs)):
                rows += r
    else:
        for d in dirs:
            rows += run_burst(d, args)
    if args.json:
        with open(args.json, 'w') as o:
            json.dump(rows, o)
    report(rows, args.variants.split(','))


def selftest():
    """a 1 px wide near blade moving 3 px over a still far plane, whose vectors are ground-only beside it"""
    h, w = 9, 9
    dep = np.full((h, w), 0.99, np.float32)
    dep[:, 4] = 0.5
    mv = np.zeros((h, w, 2), np.float32)
    mv[:, 4, 0] = 3.0
    o = dilate(mv, dep, False, 1)
    assert (o[:, 3:6, 0] == 3.0).all() and (o[:, :3, 0] == 0).all() and (o[:, 6:, 0] == 0).all()
    o = dilate(mv, dep, False, 2)
    assert (o[:, 2:7, 0] == 3.0).all() and o[0, 1, 0] == 0
    dep[:, 6] = 0.985  # a 1.5 % step (lin 0.015 vs 0.01 = 50 % nearer in 1 / distance): still an edge at 5 %
    dep[:, 7] = 0.9899  # a tiny step: not an edge
    mv[:, 7, 0] = 1.0
    assert dilate(mv, dep, False, 1)[0, 8, 0] == 1.0  # plain dilation follows any nearer neighbour
    o = dilate(mv, dep, False, 1, 0.05)
    assert o[0, 8, 0] == 0.0 and o[0, 7, 0] == 0.0 and o[0, 5, 0] == 3.0  # 1 % step ignored, 48 % step and the blade kept
    print('selftest ok')


if __name__ == '__main__':
    main()
