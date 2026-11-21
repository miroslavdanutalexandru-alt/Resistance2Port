"""Tests for tools/hd_ui_pack.py (run: python tools/test_hd_ui_pack.py). No game data and no GPU needed."""
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hd_ui_pack as hp  # noqa: E402


def test_image(w, h, alpha):
    y, x = np.mgrid[0:h, 0:w]
    img = np.zeros((h, w, 4), np.uint8)
    img[:, :, 0] = (x * 255 // max(1, w - 1))
    img[:, :, 1] = (y * 255 // max(1, h - 1))
    img[:, :, 2] = ((x // 4 + y // 4) % 2) * 200
    img[:, :, 3] = alpha(x, y)
    return img


class DxtCodec(unittest.TestCase):
    def roundtrip(self, fmt, alpha, w=24, h=20):
        img = test_image(w, h, alpha)
        dec = hp.decode_dxt(hp.encode_dxt(img, fmt), w, h, fmt)
        self.assertEqual(dec.shape, img.shape)
        return img.astype(int), dec.astype(int)

    def test_dxt1_opaque(self):
        img, dec = self.roundtrip("DXT1", lambda x, y: 255)
        self.assertLess(np.abs(img[:, :, :3] - dec[:, :, :3]).mean(), 10)
        self.assertTrue((dec[:, :, 3] == 255).all())

    def test_dxt1_punch_through_keeps_transparency(self):
        img, dec = self.roundtrip("DXT1", lambda x, y: np.where((x + y) % 7 == 0, 0, 255))
        np.testing.assert_array_equal(dec[:, :, 3], img[:, :, 3])
        vis = img[:, :, 3] > 0
        self.assertLess(np.abs(img[vis][:, :3] - dec[vis][:, :3]).mean(), 14)

    def test_dxt3_and_dxt5_alpha(self):
        for fmt, tol in (("DXT3", 9), ("DXT5", 4)):
            img, dec = self.roundtrip(fmt, lambda x, y: x * 8 + y * 2)
            self.assertLessEqual(np.abs(img[:, :, 3] - dec[:, :, 3]).max(), tol, fmt)
            self.assertLess(np.abs(img[:, :, :3] - dec[:, :, :3]).mean(), 10, fmt)

    def test_dxt5_ignores_invisible_colours(self):
        img = np.zeros((4, 4, 4), np.uint8)
        img[:, 2:] = (230, 190, 40, 255)
        img[:, 2:, 0] -= np.arange(4, dtype=np.uint8)[:, None] * 20  # a ramp over the visible texels
        dec = hp.decode_dxt(hp.encode_dxt(img, "DXT5"), 4, 4, "DXT5").astype(int)
        self.assertLessEqual(np.abs(dec[:, 2:, :3] - img[:, 2:, :3].astype(int)).max(), 12)

    def test_flat_block(self):
        img = np.full((8, 8, 4), (10, 200, 30, 255), np.uint8)
        dec = hp.decode_dxt(hp.encode_dxt(img, "DXT5"), 8, 8, "DXT5")
        self.assertLessEqual(np.abs(dec.astype(int) - img).max(), 4)


class PackFiles(unittest.TestCase):
    def test_dds_has_every_mip_at_runtime_sizes(self):
        # The runtime expects level l to be max(1, (w*S) >> l) x max(1, (h*S) >> l), DXT padded to 4x4 blocks
        with tempfile.TemporaryDirectory() as d:
            for fmt in ("DXT1", "DXT5", "A8R8G8B8"):
                img = test_image(60, 36, lambda x, y: 255)
                p = Path(d) / f"t_{fmt}.dds"
                hp.write_pack_dds(p, img, fmt, 4)
                data = p.read_bytes()
                hdr = np.frombuffer(data[:128], "<u4")
                self.assertEqual((hdr[4], hdr[3], hdr[7]), (60, 36, 4))
                body = sum(hp.level0_bytes(w, h, fmt) for w, h in hp.mip_dims(60, 36, 4))
                self.assertEqual(len(data), 128 + body, fmt)
                back, f2 = hp.read_dds_level0(p)
                self.assertEqual(f2, fmt)
                self.assertLess(np.abs(back.astype(int) - img).mean(), 10)

    def test_uncompressed_dds_header_matches_runtime_check(self):
        hdr = np.frombuffer(hp.dds_header(16, 16, "A8R8G8B8", 1), "<u4")
        self.assertTrue(hdr[20] & 0x40)
        self.assertEqual((hdr[22], hdr[23], hdr[24], hdr[25], hdr[26]), (32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000))


class Planning(unittest.TestCase):
    def test_manifest_r3_and_r1_layouts(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "manifest.tsv"
            p.write_text("# key\twidth\n"
                         "00000000000000aa\t64\t32\tDXT5\t3\t0\t00000000000000aa_64x32_88_m3.dds\n"
                         "00000000000000BB\t16\t16\tRGBA8\t1\t00000000000000bb_16x16_RGBA8_m1.png\n"
                         "bad\t1\t1\tDXT1\t1\t0\tx.dds\n")
            m = hp.parse_manifest(p)
            self.assertEqual(set(m), {"00000000000000aa", "00000000000000bb"})
            self.assertEqual(m["00000000000000aa"]["file"], "00000000000000aa_64x32_88_m3.dds")
            self.assertEqual(m["00000000000000bb"]["fmt"], "A8R8G8B8")

    def test_plan_filters(self):
        e = {"a" * 16: dict(w=256, h=128, fmt="DXT5", mips=1, file="a"),
             "b" * 16: dict(w=8, h=8, fmt="DXT1", mips=1, file="b"),
             "c" * 16: dict(w=4096, h=4096, fmt="DXT1", mips=1, file="c"),
             "d" * 16: dict(w=256, h=256, fmt="A8R8G8B8", mips=1, file="d"),
             "e" * 16: dict(w=2048, h=2048, fmt="A8R8G8B8", mips=1, file="e")}
        chosen, skipped = hp.plan(e, {"a" * 16, "b" * 16, "c" * 16, "e" * 16}, 4)
        self.assertEqual([k for k, _ in chosen], ["a" * 16])
        self.assertEqual(sum(skipped.values()), 4)  # b small, c big, d not UI, e over 64 MB / 8192 at x4
        chosen, _ = hp.plan(e, None, 2)
        self.assertEqual([k for k, _ in chosen], ["a" * 16, "d" * 16, "e" * 16])

    def test_bleed_and_combine_keep_alpha(self):
        img = np.zeros((8, 8, 4), np.uint8)
        img[2:6, 2:6] = (250, 10, 10, 255)
        rgb = hp.bleed_rgb(img)
        self.assertTrue((rgb[0, 0] == (250, 10, 10)).all())  # no black fringe for the upscaler
        up = np.kron(rgb, np.ones((4, 4, 1), np.uint8))
        al = np.kron(img[:, :, 3], np.ones((4, 4), np.uint8))
        out = hp.combine(up, al, (24, 24))
        self.assertEqual(out.shape, (24, 24, 4))
        self.assertEqual(out[0, 0, 3], 0)
        self.assertEqual(out[12, 12, 3], 255)

    def test_commit_pack_is_atomic_and_keeps_scratch_out(self):
        with tempfile.TemporaryDirectory() as td:
            out, stage = Path(td) / "mods" / "hd_ui", Path(td) / "upscaler" / "stage"
            out.mkdir(parents=True)
            keep, stale = "a" * 16 + "_8x8_DXT1_m1_x2.dds", "d" * 16 + "_4x4_DXT1_m1.dds"  # stale: same key, replaced
            (out / keep).write_bytes(b"old")
            (out / stale).write_bytes(b"stale")
            (out / ".work" / "rgb").mkdir(parents=True)  # scratch left by an older version
            (out / ".work" / "rgb" / ("e" * 16 + ".png")).write_bytes(b"x")
            done = {"a" * 16: {"file": keep}, "d" * 16: {"file": "d" * 16 + "_8x8_DXT1_m1_x2.dds"}}
            img = test_image(16, 16, lambda x, y: 255 + 0 * x)
            hp.commit_pack(out, stage, [("d" * 16 + "_8x8_DXT1_m1_x2.dds", img, "DXT1", 1)], done)
            names = sorted(p.name for p in out.iterdir())
            self.assertEqual(names, sorted([keep, "d" * 16 + "_8x8_DXT1_m1_x2.dds", "pack.json"]))
            self.assertEqual((out / keep).read_bytes(), b"old")
            self.assertFalse(stage.exists())
            self.assertFalse(any(p.is_dir() for p in out.iterdir()))  # no .work / scratch folders in the pack


if __name__ == "__main__":
    unittest.main()
