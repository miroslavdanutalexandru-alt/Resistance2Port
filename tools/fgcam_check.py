"""Offline check of R2_DLSS_FG_REAL_CAMERA / R3_DLSS_FG_REAL_CAMERA (VKR2FrameGen.cpp fill_constants).

For every RSX_MV_DUMP frame with the game's view-projection (frames.jsonl game_vp / game_vp_prev), this derives the
Streamline camera the way pfg_camera_from_vp (VKR2FrameGenLogic.h) does and checks:
  * orthonormality of [right up fwd] after the float cast (DLSS-G's scene change detection runs only if it holds),
    handedness (det) and how level the camera is (up . world y);
  * jitter: the view-projection of consecutive frames (game_vp_prev of frame k+1 == game_vp of frame k) and of still
    frames must not carry a per-frame offset;
  * reprojection: Streamline's clipToPrevClip rebuilt from the constants (cameraViewToClip with the real x / y scales,
    both camera poses: clipToView * viewToViewPrev * viewToClipPrev) against the clipToPrevClip actually sent (from T)
    and against the frame-generation motion vectors (fg_mv), in render pixels, at every 4th pixel with its depth.
Usage: python tools/fgcam_check.py <mvdump dir> [<mvdump dir> ...]
"""
import json
import math
import os
import sys

import numpy as np


def cam_from_vp(V):
    V = np.asarray(V, dtype=np.float64).reshape(4, 4)
    fw = V[3, :3].copy()
    lw = np.linalg.norm(fw)
    fw /= lw
    rt = V[0, :3] - np.dot(V[0, :3], fw) * fw
    lr = np.linalg.norm(rt)
    rt /= lr
    up = V[1, :3] - np.dot(V[1, :3], fw) * fw
    up -= np.dot(up, rt) * rt
    lu = np.linalg.norm(up)
    up /= lu
    M = np.array([V[0, :3], V[1, :3], V[3, :3]])
    pos = np.linalg.solve(M, -np.array([V[0, 3], V[1, 3], V[3, 3]]))
    return dict(pos=pos, right=rt, up=up, fwd=fw, xs=lr / lw, ys=lu / lw)


def view_of(c):
    """world -> camera view (x right, y up, z forward), column vectors"""
    Rm = np.array([c["right"], c["up"], c["fwd"]])
    W = np.eye(4)
    W[:3, :3] = Rm
    W[:3, 3] = -Rm @ c["pos"]
    return W


def proj(xs, ys, n, f, inverted):
    P = np.zeros((4, 4))
    P[0, 0] = xs
    P[1, 1] = ys
    P[3, 2] = 1.0
    if inverted:
        P[2, 2] = -n / (f - n)
        P[2, 3] = n * f / (f - n)
    else:
        P[2, 2] = f / (f - n)
        P[2, 3] = -n * f / (f - n)
    return P


def sent_clip_to_prev(m, yflip=True):
    T = np.array(m["T_d"], dtype=np.float64).reshape(4, 4)
    zA = m["zA"] if abs(m["zA"]) > 1e-9 else 1.0
    Z = np.eye(4); Zi = np.eye(4); F = np.eye(4)
    Z[2, 2] = zA; Z[2, 3] = m["zB"]
    Zi[2, 2] = 1.0 / zA; Zi[2, 3] = -m["zB"] / zA
    if yflip:
        F[1, 1] = -1.0
    return F @ Zi @ T @ Z @ F


def load(d, f, name, w, h, dt):
    p = os.path.join(d, "f%02d_%s.bin" % (f, name))
    if not os.path.exists(p):
        return None
    a = np.fromfile(p, dtype=dt)
    return a.reshape(h, w, -1) if a.size != w * h else a.reshape(h, w)


def reproject(A, x, y, z):
    v = np.stack([x, y, z, np.ones_like(x)])
    o = A @ v
    return o[0] / o[3], o[1] / o[3], o[3]


def main(dirs):
    orth, dets, level, rep_sent, rep_mv_cam, rep_mv_sent, jit = [], [], [], [], [], [], []
    nfr = 0
    for d in dirs:
        frames = [json.loads(l) for l in open(os.path.join(d, "frames.jsonl"))]
        imgs = {}
        for l in open(os.path.join(d, "images.jsonl")):
            r = json.loads(l)
            imgs[(r["f"], r["name"])] = r
        prev_vp = None
        for fi, m in enumerate(frames):
            if not m.get("game_vp_ok") or "game_vp" not in m:
                prev_vp = None
                continue
            nfr += 1
            Vc = np.array(m["game_vp"]).reshape(4, 4)
            Vp = np.array(m["game_vp_prev"]).reshape(4, 4)
            if prev_vp is not None:
                # consecutive frames: last frame's VP must be this frame's previous VP exactly (no per-frame jitter in it)
                jit.append(float(np.max(np.abs(prev_vp - Vp))))
            prev_vp = Vc
            c, cp = cam_from_vp(Vc), cam_from_vp(Vp)
            Mf = np.array([c["right"], c["up"], c["fwd"]], dtype=np.float32).T.astype(np.float64)
            orth.append(float(np.max(np.abs(Mf.T @ Mf - np.eye(3)))))
            dets.append(float(np.linalg.det(Mf)))
            level.append(float(c["up"][1]))
            n, f, inv = m["near"], m["far"], bool(m["inverted"])
            P, Pp = proj(c["xs"], c["ys"], n, f, inv), proj(cp["xs"], cp["ys"], n, f, inv)
            A_cam = Pp @ view_of(cp) @ np.linalg.inv(view_of(c)) @ np.linalg.inv(P)
            A_sent = sent_clip_to_prev(m)
            rw, rh = m["rw"], m["rh"]
            if (fi, "fg_depth") not in imgs:
                continue
            dep = load(d, fi, "fg_depth", rw, rh, np.float32)
            mv = load(d, fi, "fg_mv", rw, rh, np.float16)
            if dep is None or mv is None:
                continue
            ys_, xs_ = np.mgrid[0:rh:4, 0:rw:4]
            z = dep[ys_, xs_].astype(np.float64).ravel()
            nx = ((xs_.ravel() + 0.5) / rw) * 2 - 1
            ny = 1 - ((ys_.ravel() + 0.5) / rh) * 2  # Streamline clip: y up
            ok = (z > 0) & (z < 1) if not inv else (z > 0) & (z < 1)
            x1, y1, w1 = reproject(A_cam, nx, ny, z)
            x2, y2, w2 = reproject(A_sent, nx, ny, z)
            ok &= (w1 > 1e-6) & (w2 > 1e-6) & np.isfinite(x1) & np.isfinite(x2)
            dpx = np.hypot((x1 - x2) * rw / 2, (y1 - y2) * rh / 2)[ok]
            rep_sent.append(np.median(dpx))
            mvu = mv[ys_, xs_, 0].astype(np.float64).ravel() * m["mvscale_fg"][0]
            mvv = mv[ys_, xs_, 1].astype(np.float64).ravel() * m["mvscale_fg"][1]
            # fg mv: (prev - cur) in uv (y down)
            for (px, py), acc in (((x1, y1), rep_mv_cam), ((x2, y2), rep_mv_sent)):
                du = (px - nx) * 0.5
                dv = -(py - ny) * 0.5
                e = np.hypot((du - mvu) * rw, (dv - mvv) * rh)[ok & np.isfinite(mvu)]
                acc.append((np.median(e), np.mean(e < 1.0)))
        print("%s: %d frames" % (d, len(frames)))
    print("frames with the game VP: %d" % nfr)
    if orth:
        print("orthonormality |M^T M - I| (float): max %.2e median %.2e" % (max(orth), float(np.median(orth))))
        print("det [right up fwd]: min %.6f max %.6f (constant sign = consistent handedness)" % (min(dets), max(dets)))
        print("up . world y: min %.4f median %.4f" % (min(level), float(np.median(level))))
    if jit:
        print("VP(k) vs VP_prev(k+1) max abs difference: max %.3e median %.3e (0 = no per-frame jitter in the VP)" % (max(jit), float(np.median(jit))))
    if rep_sent:
        print("clipToPrevClip from the camera constants vs the one sent (T): median px per frame: median %.4f max %.4f" % (float(np.median(rep_sent)), max(rep_sent)))
        mc = np.array(rep_mv_cam); ms = np.array(rep_mv_sent)
        print("vs fg motion vectors, median px error: camera %.4f / sent %.4f; pixels within 1 px: camera %.1f%% / sent %.1f%%" % (
            float(np.median(mc[:, 0])), float(np.median(ms[:, 0])), 100 * float(np.mean(mc[:, 1])), 100 * float(np.mean(ms[:, 1]))))


if __name__ == "__main__":
    main(sys.argv[1:])
