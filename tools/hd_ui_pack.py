#!/usr/bin/env python3
"""Make an HD UI pack: AI-upscale the menu / HUD textures dumped from your own game.

1. Run the game with Dump textures = Menus / HUD only (RSX_TEXDUMP=ui) and visit the menus, HUD, map, loading screens.
   This writes texdump\\manifest.tsv, texdump\\ui.tsv and the textures.
2. python tools\\hd_ui_pack.py       (the port's Real-ESRGAN in tools\\hdui_upscaler, on the CPU; never the graphics card
   unless you pass --upscaler with a GPU build: on an RTX 50 series card that made the driver reset and the PC restart)
3. Turn on HD UI pack (RSX_HD_UI=1): the game loads mods\\hd_ui\\<key>_*.dds instead of the listed textures.

Everything is made and stays on this PC; the generated pack contains game-derived art and is not for sharing.
Needs Python 3.8+, Pillow and numpy.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import urllib.request
from pathlib import Path

import numpy as np
from PIL import Image

TOOL_VERSION = 1
HERE = Path(__file__).resolve().parent
REPO = HERE.parent

# the upscaler that comes with the port (CPU only: no Vulkan code), pinned like in project\hd_ui_pack.cpp
BUNDLED = REPO / "tools" / "hdui_upscaler"
CPU_EXE = ("realesrgan-ncnn-cpu.exe", "87d64acf7ca426ca3bc190171b59145f53bad837d61d8f1027bdb2c54b7b1bf5")
UPSCAYL = "https://raw.githubusercontent.com/upscayl/upscayl/a00d55fee90e0f9435d5eaa86e76700df8199af8/resources/models/"
# name -> (model name, {file: sha256}, bundled in tools\hdui_upscaler\models)
MODELS = {
    # Real-ESRGAN's realesr-animevideov3 (BSD-3), comes with the port: about 30x faster than remacri on the CPU
    "fast": ("realesr-animevideov3-x4", {"realesr-animevideov3-x4.param": "850a248e7c14c27e5bd8cf7265113a9441036a7db63963bb8aa5169d788a435e",
                                         "realesr-animevideov3-x4.bin": "548a36f9c3f4ab8da56cd3b13badf23968bee207b396dad14d04b830e5f2ab2d"}, True),
    # remacri: the ESRGAN model the Bloodborne "AI upscaled UI" mod used; good on flat UI art (CC BY-NC-SA)
    "remacri": ("remacri-4x", {"remacri-4x.param": "859ecba5b3592ecf3e76c93bed65e9f627b5236dd696aae5a84ecf8c93ab65ce",
                               "remacri-4x.bin": "a43be595c0d743314c30b50fe7ef188be0c61cc55c46ce81adb79ba4b3c3fb7a"}, False),
    # 4x-UltraSharp: sharper, can over-sharpen gradients (CC BY-NC-SA)
    "ultrasharp": ("ultrasharp-4x", {"ultrasharp-4x.param": "0136ca83686809a8f17f7111f11b951e8db93610e24b7f4137c9ffe4dbc4a806",
                                     "ultrasharp-4x.bin": "fb3e279d40d4cddb44db4e684d59e68d0aa39852c8cc14dc3f23ccc7e6eee9c1"}, False),
}
MODEL_SCALE = 4
MAX_SIDE = 8192
MAX_LEVEL0 = 64 << 20


# ---------------------------------------------------------------------------------------------------------------- manifest

def read_tsv(path):
    rows = []
    if not path.is_file():
        return rows
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("#"):
            continue
        rows.append(line.split("\t"))
    return rows


def parse_manifest(path):
    """manifest.tsv -> {key: dict(w, h, fmt, mips, file)}. R2/R3: key w h fmt mips swizzled file; R1: key w h fmt mips file."""
    out = {}
    for f in read_tsv(path):
        if len(f) < 6 or len(f[0]) != 16:
            continue
        try:
            int(f[0], 16)
            w, h, mips = int(f[1]), int(f[2]), int(f[4])
        except ValueError:
            continue
        fmt = f[3].upper()
        if fmt == "RGBA8":
            fmt = "A8R8G8B8"
        out[f[0].lower()] = {"w": w, "h": h, "fmt": fmt, "mips": max(1, mips), "file": f[-1]}
    return out


def parse_ui_keys(path):
    return {f[0].lower() for f in read_tsv(path) if f and len(f[0]) == 16}


def plan(entries, ui_keys, scale, min_size=16, max_size=2048):
    """Pick the textures to upscale. Returns (list of (key, entry), {reason: count})."""
    chosen, skipped = [], {}
    for key, e in sorted(entries.items()):
        why = None
        if ui_keys is not None and key not in ui_keys:
            why = "not a menu / HUD texture"
        elif e["fmt"] not in ("A8R8G8B8", "DXT1", "DXT3", "DXT5"):
            why = "unsupported format"
        elif min(e["w"], e["h"]) < min_size:
            why = f"smaller than {min_size} px"
        elif max(e["w"], e["h"]) > max_size:
            why = f"larger than {max_size} px"
        elif max(e["w"], e["h"]) * scale > MAX_SIDE or level0_bytes(e["w"] * scale, e["h"] * scale, e["fmt"]) > MAX_LEVEL0:
            why = "too large at this scale"
        if why:
            skipped[why] = skipped.get(why, 0) + 1
        else:
            chosen.append((key, e))
    return chosen, skipped


def level0_bytes(w, h, fmt):
    if fmt == "A8R8G8B8":
        return w * h * 4
    return ((w + 3) // 4) * ((h + 3) // 4) * (8 if fmt == "DXT1" else 16)


def mip_dims(w, h, mips):
    return [(max(1, w >> l), max(1, h >> l)) for l in range(mips)]


# ---------------------------------------------------------------------------------------------------------------- DXT codec

def _565_to_rgb(c):
    c = c.astype(np.uint32)
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    b = c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.int32)


def _color_palette(c0, c1, four):
    """c0, c1: (N,) uint16 -> (N, 4, 3) palettes; four: (N,) bool (4-colour mode)."""
    p0, p1 = _565_to_rgb(c0), _565_to_rgb(c1)
    p2 = np.where(four[:, None], (2 * p0 + p1 + 1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1 + 1) // 3, 0)
    return np.stack([p0, p1, p2, p3], 1)


def decode_dxt(data, w, h, fmt):
    """Level 0 of a DXT1/3/5 image -> (h, w, 4) uint8 RGBA."""
    bw, bh = (w + 3) // 4, (h + 3) // 4
    bs = 8 if fmt == "DXT1" else 16
    blk = np.frombuffer(data, np.uint8, bw * bh * bs).reshape(-1, bs)
    col = blk[:, bs - 8:]
    c0 = col[:, 0:2].copy().view("<u2")[:, 0]
    c1 = col[:, 2:4].copy().view("<u2")[:, 0]
    idx = col[:, 4:8].copy().view("<u4")[:, 0]
    four = (c0 > c1) | (fmt != "DXT1")
    pal = _color_palette(c0, c1, four)
    sel = (idx[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    rgb = np.take_along_axis(pal, sel[:, :, None].astype(np.int64), 1)
    if fmt == "DXT1":
        a = np.where((~four[:, None]) & (sel == 3), 0, 255)
    elif fmt == "DXT3":
        bits = blk[:, 0:8].copy().view("<u8")[:, 0]
        a = ((bits[:, None] >> (4 * np.arange(16, dtype=np.uint64))) & 15).astype(np.int32) * 17
    else:
        a0, a1 = blk[:, 0].astype(np.int32), blk[:, 1].astype(np.int32)
        bits = np.zeros(len(blk), np.uint64)
        for i in range(6):
            bits |= blk[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
        ai = ((bits[:, None] >> (3 * np.arange(16, dtype=np.uint64))) & 7).astype(np.int32)
        eight = (a0 > a1)[:, None]
        k = np.arange(8)
        pal8 = np.where(k == 0, a0[:, None], np.where(k == 1, a1[:, None],
                        ((8 - k) * a0[:, None] + (k - 1) * a1[:, None] + 3) // 7))
        pal6 = np.where(k == 0, a0[:, None], np.where(k == 1, a1[:, None], np.where(k == 6, 0, np.where(k == 7, 255,
                        ((6 - k) * a0[:, None] + (k - 1) * a1[:, None] + 2) // 5))))
        a = np.take_along_axis(np.where(eight, pal8, pal6), ai, 1)
    px = np.concatenate([rgb, a[:, :, None]], 2).astype(np.uint8)  # (N, 16, 4)
    img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return np.ascontiguousarray(img[:h, :w])


def _blocks(img):
    """(h, w, 4) -> (N, 16, 4) int32, edges clamped to whole 4x4 blocks."""
    h, w = img.shape[:2]
    ph, pw = (-h) % 4, (-w) % 4
    if ph or pw:
        img = np.pad(img, ((0, ph), (0, pw), (0, 0)), mode="edge")
    bh, bw = img.shape[0] // 4, img.shape[1] // 4
    return img.reshape(bh, 4, bw, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4).astype(np.int32)


def _to565(rgb):
    rgb = np.clip(np.rint(rgb), 0, 255).astype(np.uint32)
    return ((rgb[..., 0] * 31 + 127) // 255 << 11) | ((rgb[..., 1] * 63 + 127) // 255 << 5) | ((rgb[..., 2] * 31 + 127) // 255)


def _encode_color(px, punch, fit_visible=False):
    """px (N, 16, 4); punch: (N,) bool blocks that use DXT1 3-colour mode with transparent texels -> (N, 8) uint8.
    fit_visible: fit the endpoints to texels with alpha > 0 only (DXT3/5: invisible colours must not pull them)."""
    rgb = px[:, :, :3].astype(np.float64)
    opaque = ~(punch[:, None] & (px[:, :, 3] < 128))
    fit = opaque
    if fit_visible:
        vis = px[:, :, 3] > 0
        fit = np.where(vis.any(1)[:, None], vis, True)
    wgt = fit.astype(np.float64)[:, :, None]
    cnt = np.maximum(wgt.sum(1), 1)
    mean = (rgb * wgt).sum(1) / cnt
    d = (rgb - mean[:, None]) * wgt
    cov = np.einsum("nki,nkj->nij", d, d)
    axis = np.ones((len(px), 3)) / np.sqrt(3)
    for _ in range(8):  # power iteration: principal colour axis per block
        axis = np.einsum("nij,nj->ni", cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-9)
    t = np.einsum("nki,ni->nk", rgb - mean[:, None], axis)
    big = 1e9
    tmin = np.where(fit, t, big).min(1)
    tmax = np.where(fit, t, -big).max(1)
    tmin = np.where(tmin > tmax, 0, tmin)
    tmax = np.where(tmax < tmin, 0, tmax)
    c0 = _to565(mean + tmax[:, None] * axis)
    c1 = _to565(mean + tmin[:, None] * axis)
    # 4-colour mode needs c0 > c1, 3-colour (punch-through) mode c0 <= c1
    swap = np.where(punch, c0 > c1, c0 < c1)
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    four = ~punch
    pal = _color_palette(c0.astype(np.uint16), c1.astype(np.uint16), four)  # (N, 4, 3)
    dist = ((px[:, :, None, :3] - pal[:, None, :, :]) ** 2).sum(-1)  # (N, 16, 4)
    dist[:, :, 3] = np.where(four[:, None], dist[:, :, 3], 1 << 30)
    sel = dist.argmin(-1).astype(np.uint32)
    sel = np.where(opaque, sel, 3)
    # c0 == c1 in 4-colour mode decodes as 3-colour mode: index 0 only
    sel = np.where((four & (c0 == c1))[:, None], 0, sel)
    idx = (sel << (2 * np.arange(16, dtype=np.uint32))).sum(1).astype(np.uint32)
    out = np.zeros((len(px), 8), np.uint8)
    out[:, 0:2] = c0.astype("<u2").view(np.uint8).reshape(-1, 2)
    out[:, 2:4] = c1.astype("<u2").view(np.uint8).reshape(-1, 2)
    out[:, 4:8] = idx.astype("<u4").view(np.uint8).reshape(-1, 4)
    return out


def _encode_alpha5(a):
    """a (N, 16) int -> (N, 8) uint8 DXT5 alpha blocks (8-value mode)."""
    a0, a1 = a.max(1), a.min(1)
    k = np.arange(8)
    pal = np.where(k == 0, a0[:, None], np.where(k == 1, a1[:, None], ((8 - k) * a0[:, None] + (k - 1) * a1[:, None] + 3) // 7))
    sel = np.abs(a[:, :, None] - pal[:, None, :]).argmin(-1).astype(np.uint64)
    sel = np.where((a0 == a1)[:, None], 0, sel).astype(np.uint64)
    bits = (sel << (3 * np.arange(16, dtype=np.uint64))).sum(1, dtype=np.uint64)
    out = np.zeros((len(a), 8), np.uint8)
    out[:, 0], out[:, 1] = a0, a1
    for i in range(6):
        out[:, 2 + i] = ((bits >> np.uint64(8 * i)) & np.uint64(255)).astype(np.uint8)
    return out


def encode_dxt(img, fmt):
    """(h, w, 4) uint8 RGBA -> DXT1/3/5 bytes."""
    px = _blocks(img)
    if fmt == "DXT1":
        punch = (px[:, :, 3] < 128).any(1)
        return _encode_color(px, punch).tobytes()
    color = _encode_color(px, np.zeros(len(px), bool), fit_visible=True)
    if fmt == "DXT3":
        a4 = ((px[:, :, 3] * 15 + 127) // 255).astype(np.uint64)
        bits = (a4 << (4 * np.arange(16, dtype=np.uint64))).sum(1, dtype=np.uint64)
        alpha = bits.astype("<u8").view(np.uint8).reshape(-1, 8)
    else:
        alpha = _encode_alpha5(px[:, :, 3])
    return np.concatenate([alpha, color], 1).tobytes()


# ---------------------------------------------------------------------------------------------------------------- DDS I/O

FOURCC = {"DXT1": 0x31545844, "DXT2": 0x32545844, "DXT3": 0x33545844, "DXT4": 0x34545844, "DXT5": 0x35545844}


def dds_header(w, h, fmt, levels):
    hdr = [0] * 32
    hdr[0] = 0x20534444
    hdr[1] = 124
    hdr[2] = 0x1 | 0x2 | 0x4 | 0x1000 | (0x20000 if levels > 1 else 0)
    hdr[3], hdr[4] = h, w
    hdr[7] = levels
    hdr[19] = 32
    if fmt == "A8R8G8B8":
        hdr[2] |= 0x8
        hdr[5] = w * 4
        hdr[20] = 0x40 | 0x1  # DDPF_RGB | DDPF_ALPHAPIXELS
        hdr[22] = 32
        hdr[23], hdr[24], hdr[25], hdr[26] = 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000
    else:
        hdr[2] |= 0x80000
        hdr[5] = level0_bytes(w, h, fmt)
        hdr[20] = 0x4
        hdr[21] = FOURCC[fmt]
    hdr[27] = 0x1000 | (0x400008 if levels > 1 else 0)
    return struct.pack("<32I", *hdr)


def read_dds_level0(path):
    """-> (RGBA uint8 array, fmt name)."""
    data = Path(path).read_bytes()
    if len(data) < 128 or data[:4] != b"DDS ":
        raise ValueError(f"{path}: not a DDS file")
    hdr = struct.unpack("<32I", data[:128])
    h, w = hdr[3], hdr[4]
    if hdr[20] & 0x4:
        fmt = {v: k for k, v in FOURCC.items()}.get(hdr[21])
        if fmt is None:
            raise ValueError(f"{path}: unsupported DDS format")
        fmt = {"DXT2": "DXT3", "DXT4": "DXT5"}.get(fmt, fmt)
        return decode_dxt(data[128:], w, h, fmt), fmt
    if (hdr[20] & 0x40) and hdr[22] == 32:
        px = np.frombuffer(data, np.uint8, w * h * 4, 128).reshape(h, w, 4)
        order = [2, 1, 0, 3] if hdr[23] == 0x00FF0000 else [0, 1, 2, 3]
        return np.ascontiguousarray(px[:, :, order]), "A8R8G8B8"
    raise ValueError(f"{path}: unsupported DDS format")


def build_mips(img, mips):
    """RGBA uint8 level 0 -> list of mips levels (Pillow box filter on premultiplied colour)."""
    w, h = img.shape[1], img.shape[0]
    a = img[:, :, 3:4].astype(np.float32) / 255
    pm = np.concatenate([img[:, :, :3].astype(np.float32) * a, img[:, :, 3:4].astype(np.float32)], 2)
    out = [img]
    for lw, lh in mip_dims(w, h, mips)[1:]:
        ch = [np.asarray(Image.fromarray(pm[:, :, c], "F").resize((lw, lh), Image.BOX)) for c in range(4)]
        al = ch[3]
        rgb = np.stack(ch[:3], -1) / np.maximum(al[:, :, None] / 255, 1e-6)
        lvl = np.concatenate([np.where(al[:, :, None] > 0, rgb, 0), al[:, :, None]], 2)
        out.append(np.clip(np.rint(lvl), 0, 255).astype(np.uint8))
    return out


def write_pack_dds(path, img, fmt, mips):
    levels = build_mips(img, mips)
    body = []
    for lvl in levels:
        if fmt == "A8R8G8B8":
            body.append(np.ascontiguousarray(lvl[:, :, [2, 1, 0, 3]]).tobytes())
        else:
            body.append(encode_dxt(lvl, fmt))
    tmp = Path(str(path) + ".part")
    with open(tmp, "wb") as f:
        f.write(dds_header(img.shape[1], img.shape[0], fmt, len(levels)))
        for b in body:
            f.write(b)
    os.replace(tmp, path)


# ---------------------------------------------------------------------------------------------------------------- alpha

def bleed_rgb(img, steps=16):
    """Spread the colour of visible texels into fully transparent ones so the upscaler sees no black fringes."""
    rgb = img[:, :, :3].astype(np.float32)
    known = img[:, :, 3] > 0
    if known.all() or not known.any():
        return img[:, :, :3].copy()
    for _ in range(steps):
        if known.all():
            break
        acc = np.zeros_like(rgb)
        cnt = np.zeros(known.shape, np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            k = np.roll(known, (dy, dx), (0, 1))
            acc += np.roll(rgb, (dy, dx), (0, 1)) * k[:, :, None]
            cnt += k
        new = (~known) & (cnt > 0)
        rgb[new] = acc[new] / cnt[new][:, None]
        known = known | new
    rgb[~known] = rgb[known].mean(0) if known.any() else 0
    return np.clip(np.rint(rgb), 0, 255).astype(np.uint8)


def combine(rgb_up, alpha_up, size):
    """Model output (4x) -> RGBA at the pack size (Lanczos)."""
    rgb = Image.fromarray(rgb_up[:, :, :3]).resize(size, Image.LANCZOS)
    out = np.dstack([np.asarray(rgb), np.full((size[1], size[0]), 255, np.uint8)])
    if alpha_up is not None:
        a = alpha_up.astype(np.float32).mean(-1) if alpha_up.ndim == 3 else alpha_up.astype(np.float32)
        a = np.asarray(Image.fromarray(a, "F").resize(size, Image.LANCZOS))
        out[:, :, 3] = np.clip(np.rint(a), 0, 255).astype(np.uint8)
    return out


# ---------------------------------------------------------------------------------------------------------------- upscaler

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def download(url, dest, want):
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.is_file() and sha256(dest) == want:
        return
    print(f"downloading {url}")
    tmp = Path(str(dest) + ".download")
    with urllib.request.urlopen(url, timeout=120) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f)
    got = sha256(tmp)
    if got != want:
        tmp.unlink()
        raise SystemExit(f"{dest.name}: SHA-256 mismatch ({got}); not used")
    os.replace(tmp, dest)


def ensure_upscaler(exe, model, cache):
    """-> (exe path, model name passed to -n, model folder, on the CPU). The port's CPU upscaler (checked) unless --upscaler;
    a model that does not come with the port is downloaded (pinned) into cache\\models."""
    name, files, bundled = MODELS[model]
    if exe:
        exe, cpu = Path(exe), False
        if not exe.is_file():
            raise SystemExit(f"{exe}: not found")
    else:
        exe, cpu = BUNDLED / "bin" / CPU_EXE[0], True
        if not exe.is_file() or sha256(exe) != CPU_EXE[1]:
            raise SystemExit(f"{exe}: missing or not the file that comes with the port (get the port's files again)")
    mdir = BUNDLED / "models" if bundled else cache / "models"
    for fn, want in files.items():
        p = mdir / fn
        if bundled:
            if not p.is_file() or sha256(p) != want:
                raise SystemExit(f"{p}: missing or damaged (comes with the port)")
        else:
            download(UPSCAYL + fn, p, want)
    return exe, name, mdir, cpu


def run_upscaler(exe, model_name, mdir, cpu, src, dst, gpu=None, tile=None):
    dst.mkdir(parents=True, exist_ok=True)
    cmd = [str(exe), "-i", str(src.resolve()), "-o", str(dst.resolve()), "-n", model_name, "-s", str(MODEL_SCALE), "-f", "png",
           "-m", str(mdir.resolve())]
    if cpu:
        cmd += ["-g", "-1", "-j", f"1:{max(1, min(16, (os.cpu_count() or 2) // 2))}:1", "-t", str(tile or 200)]
    else:
        if gpu is not None:
            cmd += ["-g", str(gpu)]
        if tile:
            cmd += ["-t", str(tile)]
    r = subprocess.run(cmd, cwd=exe.parent, capture_output=True, text=True,
                       creationflags=getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0) if cpu else 0)
    if r.returncode != 0:
        raise SystemExit(f"upscaler failed ({r.returncode}):\n{r.stderr[-2000:]}")


# ---------------------------------------------------------------------------------------------------------------- main

def load_dump(path):
    if path.suffix.lower() == ".png":
        return np.asarray(Image.open(path).convert("RGBA"))
    return read_dds_level0(path)[0]


def out_name(key, e, scale):
    return f"{key}_{e['w'] * scale}x{e['h'] * scale}_{e['fmt']}_m{e['mips']}.dds"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump-dir", type=Path, default=None, help="texture dump folder (default: texdump\\ next to the game)")
    ap.add_argument("--out", type=Path, default=None, help="pack folder (default: mods\\hd_ui)")
    ap.add_argument("--scale", type=int, default=3, choices=(2, 3, 4), help="size factor (default 3: 720p UI art for 4K)")
    ap.add_argument("--model", default="fast", choices=sorted(MODELS),
                    help="upscaling model (default fast, comes with the port; remacri / ultrasharp are downloaded once)")
    ap.add_argument("--upscaler", default=None,
                    help="use this realesrgan-ncnn-vulkan.exe (on the GPU, at your own risk) instead of the port's CPU upscaler")
    ap.add_argument("--all", action="store_true", help="every dumped texture, not only those listed in ui.tsv")
    ap.add_argument("--max-size", type=int, default=2048, help="skip game textures larger than this (default 2048)")
    ap.add_argument("--gpu", type=int, default=None, help="Vulkan GPU index for an --upscaler")
    ap.add_argument("--tile", type=int, default=None, help="upscaler tile size (default 200 on the CPU)")
    ap.add_argument("--force", action="store_true", help="redo textures already in the pack")
    ap.add_argument("--dry-run", action="store_true", help="only list what would be done")
    a = ap.parse_args(argv)

    dump = a.dump_dir or next((p for p in (Path.cwd() / "texdump", REPO / "texdump", REPO / "build" / "texdump") if p.is_dir()), REPO / "texdump")
    out = a.out or (dump.parent / "mods" / "hd_ui")
    entries = parse_manifest(dump / "manifest.tsv")
    if not entries:
        raise SystemExit(f"{dump}\\manifest.tsv: no textures. Run the game with Dump textures = Menus / HUD only (RSX_TEXDUMP=ui) first.")
    ui = None
    if not a.all:
        ui = parse_ui_keys(dump / "ui.tsv")
        if not ui:
            raise SystemExit(f"{dump}\\ui.tsv: no menu / HUD textures listed (dump with RSX_TEXDUMP=ui, or pass --all)")
    chosen, skipped = plan(entries, ui, a.scale, max_size=a.max_size)
    state_path = out / "pack.json"
    state = json.loads(state_path.read_text()) if state_path.is_file() else {}
    done = state.get("textures", {})
    todo = []
    for key, e in chosen:
        name = out_name(key, e, a.scale)
        prev = done.get(key)
        if not a.force and prev and prev.get("file") == name and prev.get("model") == a.model and (out / name).is_file():
            continue
        if not (dump / e["file"]).is_file():
            skipped["dump file missing"] = skipped.get("dump file missing", 0) + 1
            continue
        todo.append((key, e, name))
    print(f"{len(chosen)} menu / HUD texture(s) eligible, {len(todo)} to upscale x{a.scale} with {a.model} into {out}")
    for why, n in sorted(skipped.items()):
        print(f"  skipped {n}: {why}")
    if a.dry_run or not todo:
        return 0

    exe, model_name, mdir, cpu = ensure_upscaler(a.upscaler, a.model, HERE / "upscaler")
    # Scratch PNGs never live in the pack folder: the game loads every <16 hex digit>*.png/.dds found there.
    work = HERE / "upscaler" / "work"
    try:
        shutil.rmtree(work, ignore_errors=True)
        for d in ("rgb", "alpha", "rgb_up", "alpha_up"):
            (work / d).mkdir(parents=True)
        has_alpha = {}
        for key, e, _ in todo:
            img = load_dump(dump / e["file"])
            if img.shape[:2] != (e["h"], e["w"]):
                raise SystemExit(f"{e['file']}: {img.shape[1]}x{img.shape[0]}, manifest says {e['w']}x{e['h']}")
            Image.fromarray(bleed_rgb(img)).save(work / "rgb" / f"{key}.png")
            has_alpha[key] = bool((img[:, :, 3] < 255).any())
            if has_alpha[key]:
                Image.fromarray(np.repeat(img[:, :, 3:4], 3, 2)).save(work / "alpha" / f"{key}.png")
        print(f"upscaling {len(todo)} texture(s) ({sum(has_alpha.values())} with alpha) on the {'CPU' if cpu else 'GPU'} ...")
        run_upscaler(exe, model_name, mdir, cpu, work / "rgb", work / "rgb_up", a.gpu, a.tile)
        if any(has_alpha.values()):
            run_upscaler(exe, model_name, mdir, cpu, work / "alpha", work / "alpha_up", a.gpu, a.tile)
        made = []
        for key, e, name in todo:
            up = work / "rgb_up" / f"{key}.png"
            if not up.is_file():
                print(f"  {key}: no upscaler output, skipped")
                continue
            rgb = np.asarray(Image.open(up).convert("RGB"))
            al = np.asarray(Image.open(work / "alpha_up" / f"{key}.png").convert("L")) if has_alpha[key] else None
            made.append((key, name, combine(rgb, al, (e["w"] * a.scale, e["h"] * a.scale)), e))
        for key, name, _, e in made:
            done[key] = {"file": name, "model": a.model, "scale": a.scale, "source": f"{e['w']}x{e['h']} {e['fmt']} m{e['mips']}"}
        commit_pack(out, HERE / "upscaler" / "stage", [(name, img, e["fmt"], e["mips"]) for _, name, img, e in made], done)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"wrote {len(made)} texture(s); {len(done)} in {out}. Turn on HD UI pack (RSX_HD_UI=1).")
    return 0


def commit_pack(out, stage, new_files, done):
    """Atomically replace the pack folder: the finished pack is built in `stage` (outside the pack, holding the
    files kept from the old pack plus the new ones) and renamed into place, so an interrupted run never leaves
    partial or scratch files where the game looks for replacements."""
    out, stage = Path(out), Path(stage)
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir(parents=True)
    replaced = {name.split("_", 1)[0] for name, *_ in new_files}
    if out.is_dir():
        for f in out.iterdir():  # files the launcher's builder (pack.tsv) made are kept too
            if f.is_file() and f.name != "pack.json" and f.name.split("_", 1)[0] not in replaced:
                try:
                    os.link(f, stage / f.name)
                except OSError:
                    shutil.copy2(f, stage / f.name)
    for name, img, fmt, mips in new_files:
        write_pack_dds(stage / name, img, fmt, mips)
    state = {"tool_version": TOOL_VERSION, "note": "generated locally from your game; do not distribute",
             "textures": {k: v for k, v in done.items() if (stage / v["file"]).is_file()}}
    (stage / "pack.json").write_text(json.dumps(state, indent=1))
    old = stage.with_name(stage.name + ".old")
    shutil.rmtree(old, ignore_errors=True)
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        os.replace(out, old)
    os.replace(stage, out)
    shutil.rmtree(old, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
