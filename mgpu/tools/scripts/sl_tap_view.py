# /// script
# dependencies = ["numpy", "pillow"]
# ///
"""Converts an sl_tap capture (capture.json + .raw) into PNG previews and prints per-resource stats.

uv run mgpu/tools/scripts/sl_tap_view.py data/sl_tap/<run>
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

BPP = {2: 16, 10: 8, 16: 8, 20: 8, 24: 4, 26: 4, 27: 4, 28: 4, 29: 4, 34: 4, 39: 4, 40: 4, 41: 4, 87: 4, 88: 4, 91: 4}


def r11g11b10(u):
    def f(bits, mbits):
        e = (bits >> mbits) & 0x1F
        m = bits & ((1 << mbits) - 1)
        v = np.where(e == 0, m / (1 << mbits) * 2.0**-14, (1 + m / (1 << mbits)) * 2.0 ** (e.astype(np.float64) - 15))
        return v.astype(np.float32)
    return np.stack([f(u & 0x7FF, 6), f((u >> 11) & 0x7FF, 6), f((u >> 22) & 0x3FF, 5)], -1)


def decode(t, raw):
    w, h, pitch, fmt = t["width"], t["height"], t["row_pitch"], t["format"]
    rows = np.frombuffer(raw, np.uint8)[: pitch * h].reshape(h, pitch)[:, : w * BPP[fmt]]
    if fmt in (24,):  # R10G10B10A2_UNORM
        u = rows.copy().view(np.uint32).reshape(h, w)
        return np.stack([(u >> s) & 0x3FF for s in (0, 10, 20)], -1) / 1023.0, ((u >> 30) & 3) / 3.0, "unorm10"
    if fmt == 26:
        return r11g11b10(rows.copy().view(np.uint32).reshape(h, w)), None, "float"
    if fmt in (10, 2):
        a = rows.copy().view(np.float16 if fmt == 10 else np.float32).reshape(h, w, 4).astype(np.float32)
        return a[..., :3], a[..., 3], "float"
    if fmt in (27, 28, 29, 87, 88, 91):
        a = rows.reshape(h, w, 4) / 255.0
        if fmt in (87, 88, 91):
            a = a[..., [2, 1, 0, 3]]
        return a[..., :3], a[..., 3], "unorm8"
    if fmt in (39, 40, 41):
        return rows.copy().view(np.float32).reshape(h, w), None, "depth"
    if fmt == 20:
        return rows.copy().view(np.float32).reshape(h, w, 2)[..., 0], None, "depth"
    if fmt == 34:
        return rows.copy().view(np.float16).reshape(h, w, 2).astype(np.float32), None, "mv"
    raise ValueError(f"format {fmt}")


def to_png(kind, a):
    if kind == "float":
        a = np.clip(a / (1 + a), 0, 1) ** (1 / 2.2)
    elif kind == "depth":
        lo, hi = np.percentile(a[a > 0], [1, 99]) if (a > 0).any() else (0, 1)
        a = np.clip((a - lo) / max(hi - lo, 1e-9), 0, 1)
    elif kind == "mv":
        k = max(np.percentile(np.abs(a), 99), 1e-6)
        a = np.concatenate([0.5 + 0.5 * np.clip(a / k, -1, 1), np.full(a.shape[:2] + (1,), 0.5)], -1)
    return Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8))


def main(d):
    d = Path(d)
    cap = json.loads((d / "capture.json").read_text())
    imgs = {}
    for t in cap["tags"]:
        rgb, alpha, kind = decode(t, (d / t["file"]).read_bytes())
        imgs[t["name"]] = (rgb, alpha, kind)
        stats = f"min {rgb.min():.4g} max {rgb.max():.4g} mean {rgb.mean():.4g}"
        if alpha is not None:
            stats += f" alpha mean {np.mean(alpha):.3f} nonzero {np.mean(alpha > 0):.3f}"
        print(f"{t['name']:18} fmt {t['format']:3} {t['width']}x{t['height']} {kind:7} {stats}")
        to_png(kind, rgb).save(d / f"{t['name']}.png")
        if alpha is not None and np.ptp(alpha) > 0:
            to_png("unorm8", alpha).save(d / f"{t['name']}_alpha.png")
    # Without a separate UI tag the HUD is the back buffer minus the HUD-less colour.
    if "backbuffer" in imgs and "ffx_hudless" in imgs and imgs["backbuffer"][0].shape == imgs["ffx_hudless"][0].shape:
        diff = np.abs(imgs["backbuffer"][0] - imgs["ffx_hudless"][0]).max(-1)
        mask = diff > 2 / 1023
        print(f"backbuffer vs hudless: {mask.mean() * 100:.2f}% of pixels differ (max {diff.max():.3f})")
        ui = np.where(mask[..., None], imgs["backbuffer"][0], 0)
        to_png("unorm8", ui).save(d / "ui_from_diff.png")
        to_png("unorm8", mask.astype(np.float32)).save(d / "ui_mask.png")


if __name__ == "__main__":
    main(sys.argv[1])
