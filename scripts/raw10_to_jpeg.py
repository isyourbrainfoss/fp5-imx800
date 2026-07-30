#!/usr/bin/env python3
"""Unpack MIPI RAW10 packed Bayer (SBGGR10) → simple demosaic JPEG/PNG.

Usage:
  python3 raw10_to_jpeg.py input.bin width height [-o out.jpg] [--scale N] [--rotate 270]
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np


def unpack_raw10_packed(raw: bytes, w: int, h: int) -> np.ndarray:
    """MIPI packed 10-bit: 4 pixels in 5 bytes → uint16 (0..1023), shape (h,w).

    Vectorized: each group of 5 bytes → 4 pixels.
    """
    stride = w * 5 // 4
    expected = stride * h
    if len(raw) < expected:
        raise ValueError(f"raw size {len(raw)} < expected {expected} for {w}x{h}")
    arr = np.frombuffer(raw, dtype=np.uint8, count=expected).reshape(h, stride)
    # groups of 5 bytes along width: w/4 groups
    n = w // 4
    g = arr[:, : n * 5].reshape(h, n, 5).astype(np.uint16)
    out = np.empty((h, w), dtype=np.uint16)
    out[:, 0::4] = (g[:, :, 0] << 2) | (g[:, :, 4] & 0x3)
    out[:, 1::4] = (g[:, :, 1] << 2) | ((g[:, :, 4] >> 2) & 0x3)
    out[:, 2::4] = (g[:, :, 2] << 2) | ((g[:, :, 4] >> 4) & 0x3)
    out[:, 3::4] = (g[:, :, 3] << 2) | ((g[:, :, 4] >> 6) & 0x3)
    return out


def demosaic_sbggr(bayer: np.ndarray, scale: int = 2) -> np.ndarray:
    """Vectorized 2x2 cell demosaic for SBGGR + box downscale → uint8 RGB."""
    h, w = bayer.shape
    bl = 64  # 10-bit black level (matches soft IPA 4096 in 16-bit)
    b10 = np.clip(bayer.astype(np.int32) - bl, 0, 1023)
    scale = max(2, scale & ~1)
    # sample 2x2 cells on a grid of step `scale`
    ys = np.arange(0, h - 1, scale)
    xs = np.arange(0, w - 1, scale)
    # ensure even starts (SBGGR phase)
    ys = ys[ys % 2 == 0]
    xs = xs[xs % 2 == 0]
    yy, xx = np.meshgrid(ys, xs, indexing="ij")
    b = b10[yy, xx]
    g = (b10[yy, xx + 1] + b10[yy + 1, xx]) // 2
    r = b10[yy + 1, xx + 1]
    stretch = 255.0 / 500.0
    rgb = np.stack(
        [
            np.clip(r * stretch, 0, 255),
            np.clip(g * stretch, 0, 255),
            np.clip(b * stretch, 0, 255),
        ],
        axis=-1,
    ).astype(np.uint8)
    return rgb


def rotate_cw(rgb: np.ndarray, deg: int) -> np.ndarray:
    k = (deg // 90) % 4
    if k:
        rgb = np.ascontiguousarray(np.rot90(rgb, k=-k))  # CW
    return rgb


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("width", type=int)
    ap.add_argument("height", type=int)
    ap.add_argument("-o", "--output", type=Path, default=None)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--rotate", type=int, default=270, help="sensor CW rotation")
    args = ap.parse_args()

    raw_path = Path(args.raw)
    data = raw_path.read_bytes()
    bayer = unpack_raw10_packed(data, args.width, args.height)
    rgb = demosaic_sbggr(bayer, scale=args.scale)
    rgb = rotate_cw(rgb, args.rotate)

    out = args.output
    if out is None:
        out = raw_path.with_suffix(".jpg")

    try:
        from PIL import Image

        Image.fromarray(rgb, "RGB").save(out, quality=92)
    except ImportError:
        ppm = out.with_suffix(".ppm")
        h, w = rgb.shape[:2]
        with open(ppm, "wb") as f:
            f.write(f"P6\n{w} {h}\n255\n".encode())
            f.write(np.ascontiguousarray(rgb).tobytes())
        import subprocess

        subprocess.run(["convert", str(ppm), str(out)], check=True)
        ppm.unlink(missing_ok=True)

    print(f"wrote {out} ({rgb.shape[1]}x{rgb.shape[0]}) from {args.width}x{args.height}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
