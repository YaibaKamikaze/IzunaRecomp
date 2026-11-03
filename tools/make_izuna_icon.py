"""Draw IzunaRecomp's icon: a four-bladed throwing star on the hub's ink tile.

    python3 tools/make_izuna_icon.py      # writes resources/izuna.ico (+ resources/izuna_256.png)

The Windows build's ng2.exe carries it (CMakeLists.txt, NG2_ICON_PATH). The
colours are the hub's: ink, ivory and the red of its logo plate. Small sizes
are drawn simpler, not shrunk: below 32 px the tile loses its edge line and
the star its hole, which would otherwise blur into one grey pixel.
"""

import math
import os

from PIL import Image, ImageDraw

INK = (14, 14, 16, 255)
IVORY = (236, 230, 214, 255)
RED = (214, 32, 28, 255)
RED_DEEP = (140, 12, 14, 255)
SIZES = [16, 24, 32, 48, 64, 128, 256]


def star(draw, cx, cy, r, inner, twist, fill):
    """Four blades: each a kite from near the centre out to a point."""
    for k in range(4):
        a = k * math.pi / 2 + twist
        tip = (cx + math.cos(a) * r, cy + math.sin(a) * r)
        side1 = (cx + math.cos(a + 0.55) * inner, cy + math.sin(a + 0.55) * inner)
        side2 = (cx + math.cos(a - 0.95) * inner * 0.55, cy + math.sin(a - 0.95) * inner * 0.55)
        draw.polygon([side2, tip, side1, (cx, cy)], fill=fill)


def draw_icon(size):
    s = 4  # drawn 4x, then reduced: smooth edges at every size
    n = size * s
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    small = size < 32
    pad = n * (0.04 if small else 0.06)
    radius = n * 0.2
    d.rounded_rectangle([pad, pad, n - pad, n - pad], radius=radius, fill=INK)
    if not small:
        w = max(s, int(n * 0.035))
        d.rounded_rectangle([pad, pad, n - pad, n - pad], radius=radius, outline=RED, width=w)
    c = n / 2
    r = n * (0.40 if small else 0.36)
    star(d, c, c, r, r * 0.42, -0.35, IVORY)
    # The centre: a red ring with a hole (a solid red dot when small).
    rr = r * (0.26 if small else 0.22)
    d.ellipse([c - rr, c - rr, c + rr, c + rr], fill=RED)
    if not small:
        hole = rr * 0.45
        d.ellipse([c - hole, c - hole, c + hole, c + hole], fill=INK)
        # A thin deep-red line under the star's lower edge, as under the hub's plate.
        d.line([(n * 0.28, n * 0.86), (n * 0.72, n * 0.86)], fill=RED_DEEP, width=max(s, int(n * 0.02)))
    return img.resize((size, size), Image.LANCZOS)


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "resources")
    images = [draw_icon(sz) for sz in SIZES]
    out = os.path.join(root, "izuna.ico")
    images[-1].save(out, format="ICO", sizes=[(sz, sz) for sz in SIZES], append_images=images[:-1])
    images[-1].save(os.path.join(root, "izuna_256.png"))
    print(out)


if __name__ == "__main__":
    main()
