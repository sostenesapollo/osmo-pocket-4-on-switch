#!/usr/bin/env python3
"""Gera o ícone 256x256 do Osmo Remote (formato exigido pelo hbmenu)."""

from __future__ import annotations

import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

SIZE = 256
OUT = Path(__file__).resolve().parents[1] / "switch" / "icon.jpg"


def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def gradient_bg(size: int) -> Image.Image:
    img = Image.new("RGB", (size, size))
    px = img.load()
    top = (26, 34, 46)
    bottom = (8, 11, 16)
    for y in range(size):
        t = y / (size - 1)
        r = lerp(top[0], bottom[0], t)
        g = lerp(top[1], bottom[1], t)
        b = lerp(top[2], bottom[2], t)
        for x in range(size):
            # subtle radial lift from the upper-left
            d = math.hypot(x - size * 0.28, y - size * 0.22) / size
            lift = max(0.0, 0.32 - d) * 46
            px[x, y] = (
                int(min(255, r + lift)),
                int(min(255, g + lift * 1.05)),
                int(min(255, b + lift * 1.15)),
            )
    return img


def draw_gimbal(img: Image.Image) -> None:
    d = ImageDraw.Draw(img, "RGBA")
    cx, cy = SIZE * 0.5, SIZE * 0.55
    R = SIZE * 0.215

    # gimbal arm: open ring around the lens
    d.arc(
        [cx - R * 1.72, cy - R * 1.72, cx + R * 1.72, cy + R * 1.72],
        start=205,
        end=505,
        fill=(120, 214, 255, 235),
        width=int(SIZE * 0.042),
    )

    # arm pivot blocks
    for ang in (205, 325):
        rad = math.radians(ang)
        px = cx + math.cos(rad) * R * 1.72
        py = cy + math.sin(rad) * R * 1.72
        r = SIZE * 0.036
        d.ellipse([px - r, py - r, px + r, py + r], fill=(150, 228, 255, 255))

    # lens body
    d.ellipse([cx - R, cy - R, cx + R, cy + R], fill=(16, 20, 27, 255))
    d.ellipse(
        [cx - R, cy - R, cx + R, cy + R],
        outline=(150, 228, 255, 255),
        width=int(SIZE * 0.026),
    )

    # inner glass
    inner = R * 0.62
    d.ellipse(
        [cx - inner, cy - inner, cx + inner, cy + inner],
        fill=(38, 132, 178, 255),
    )
    # specular highlight
    hl = R * 0.24
    d.ellipse(
        [cx - R * 0.44 - hl, cy - R * 0.46 - hl, cx - R * 0.44 + hl, cy - R * 0.46 + hl],
        fill=(226, 246, 255, 210),
    )

    # stream arcs (wifi-ish), lower right
    ax, ay = SIZE * 0.79, SIZE * 0.80
    for i, rad in enumerate((SIZE * 0.10, SIZE * 0.155)):
        box = [ax - rad, ay - rad, ax + rad, ay + rad]
        d.arc(box, start=200, end=340, fill=(150, 228, 255, 255 - i * 60),
              width=int(SIZE * 0.024))


def main() -> None:
    img = gradient_bg(SIZE)
    glow = img.filter(ImageFilter.GaussianBlur(0))
    glow = glow.copy()

    layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    draw_gimbal(layer)

    # soft glow behind the graphics
    halo = layer.filter(ImageFilter.GaussianBlur(SIZE * 0.045))
    img = Image.alpha_composite(img.convert("RGBA"), halo).convert("RGB")
    img = Image.alpha_composite(img.convert("RGBA"), layer).convert("RGB")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    img.save(OUT, "JPEG", quality=94, optimize=True)
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
