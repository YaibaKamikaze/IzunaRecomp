#!/usr/bin/env python3
"""Turn native_textures/*.bin dumps (hub: Native: save textures) into PNGs.

  tools/native/tex2png.py <dir or files...>

A dump is a texture's base level exactly as the native renderer uploaded it
(untiled, endian-swapped; BC blocks as they are), named
<address>_vk<VkFormat>_<w>x<h>_f<Xenos format>.bin. The channels are shown in
memory order (X Y Z W as R G B A): the fetch swizzle is not applied, which is
what a decoding check wants. Standard library only (slow for big BC
textures, fine for checks). The dumps come from the game: keep them out of
the repo.

The mods' texture dump (the hub's MODS page, R2) names each file by the
texture's id: <w>x<h>-<format>-<hash>_vk<VkFormat>_<w>x<h>.bin. With --ids the
PNGs are named <id>.png - the name a mod's replacement has in its textures/
folder (docs/MODDING.md):

  tools/native/tex2png.py --ids "Texture Dump"
"""
import pathlib, re, struct, sys, zlib


def half(h):
    s, e, m = (h >> 15) & 1, (h >> 10) & 31, h & 1023
    if e == 0:
        v = m / 1024.0 * 2.0 ** -14
    elif e == 31:
        v = float("inf") if m == 0 else 0.0
    else:
        v = (1 + m / 1024.0) * 2.0 ** (e - 15)
    return -v if s else v


def clamp8(v):
    return max(0, min(255, int(round(v * 255))))


def rgb565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31


def bc1_colors(c0, c1, alpha_ok):
    a, b = rgb565(c0), rgb565(c1)
    if c0 > c1 or not alpha_ok:
        return [a + (255,), b + (255,), tuple((2 * x + y) // 3 for x, y in zip(a, b)) + (255,),
                tuple((x + 2 * y) // 3 for x, y in zip(a, b)) + (255,)]
    return [a + (255,), b + (255,), tuple((x + y) // 2 for x, y in zip(a, b)) + (255,), (0, 0, 0, 0)]


def bc_alpha(block):  # BC3 alpha / BC4 / BC5 channel: 8 bytes -> 16 values
    a0, a1 = block[0], block[1]
    bits = int.from_bytes(block[2:8], "little")
    if a0 > a1:
        pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    else:
        pal = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
    return [pal[(bits >> (3 * i)) & 7] for i in range(16)]


def decode(fmt, w, h, d):
    px = bytearray(w * h * 4)

    def put(x, y, rgba):
        if x < w and y < h:
            o = (y * w + x) * 4
            px[o:o + 4] = bytes(rgba)

    if fmt in (133, 134, 131, 132, 135, 136, 137, 138, 139, 141):
        bw, bh = (w + 3) // 4, (h + 3) // 4
        size = 8 if fmt in (131, 132, 133, 134, 139) else 16
        for by in range(bh):
            for bx in range(bw):
                blk = d[(by * bw + bx) * size:(by * bw + bx + 1) * size]
                if len(blk) < size:
                    continue
                if fmt == 139:
                    r = bc_alpha(blk)
                    texels = [(v, v, v, 255) for v in r]
                elif fmt == 141:
                    r, g = bc_alpha(blk[:8]), bc_alpha(blk[8:])
                    texels = [(r[i], g[i], 0, 255) for i in range(16)]
                else:
                    cb = blk[size - 8:]
                    c0, c1, idx = struct.unpack("<HHI", cb)
                    cols = bc1_colors(c0, c1, size == 8)
                    texels = [cols[(idx >> (2 * i)) & 3] for i in range(16)]
                    if fmt in (135, 136):
                        a = int.from_bytes(blk[:8], "little")
                        texels = [t[:3] + (((a >> (4 * i)) & 15) * 17,) for i, t in enumerate(texels)]
                    elif fmt in (137, 138):
                        a = bc_alpha(blk[:8])
                        texels = [t[:3] + (a[i],) for i, t in enumerate(texels)]
                for i, t in enumerate(texels):
                    put(bx * 4 + i % 4, by * 4 + i // 4, t)
        return px
    for i in range(w * h):
        if fmt == 9:
            v = d[i]; t = (v, v, v, 255)
        elif fmt == 16:
            t = (d[i * 2], d[i * 2 + 1], 0, 255)
        elif fmt in (37, 43):
            t = tuple(d[i * 4:i * 4 + 4])
        elif fmt == 64:
            v = struct.unpack_from("<I", d, i * 4)[0]
            t = ((v & 1023) >> 2, ((v >> 10) & 1023) >> 2, ((v >> 20) & 1023) >> 2, (v >> 30) * 85)
        elif fmt == 70:
            v = struct.unpack_from("<H", d, i * 2)[0] >> 8; t = (v, v, v, 255)
        elif fmt == 77:
            a, b = struct.unpack_from("<HH", d, i * 4); t = (a >> 8, b >> 8, 0, 255)
        elif fmt == 91:
            t = tuple(c >> 8 for c in struct.unpack_from("<HHHH", d, i * 8))
        elif fmt == 76:
            v = clamp8(half(struct.unpack_from("<H", d, i * 2)[0])); t = (v, v, v, 255)
        elif fmt == 83:
            a, b = struct.unpack_from("<HH", d, i * 4); t = (clamp8(half(a)), clamp8(half(b)), 0, 255)
        elif fmt == 97:
            t = tuple(clamp8(half(c)) for c in struct.unpack_from("<HHHH", d, i * 8))
        elif fmt == 100:
            v = clamp8(struct.unpack_from("<f", d, i * 4)[0]); t = (v, v, v, 255)
        elif fmt == 103:
            a, b = struct.unpack_from("<ff", d, i * 8); t = (clamp8(a), clamp8(b), 0, 255)
        elif fmt == 109:
            t = tuple(clamp8(c) for c in struct.unpack_from("<ffff", d, i * 16))
        else:
            raise ValueError(f"VkFormat {fmt} not handled")
        px[i * 4:i * 4 + 4] = bytes(t)
    return px


def write_png(path, w, h, px):
    rows = b"".join(b"\0" + bytes(px[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


ids = "--ids" in sys.argv[1:]
for arg in [a for a in sys.argv[1:] if a != "--ids"]:
    p = pathlib.Path(arg)
    for f in sorted(p.glob("*.bin")) if p.is_dir() else [p]:
        m = re.search(r"_vk(\d+)_(\d+)x(\d+)", f.name)
        if not m:
            continue
        fmt, w, h = int(m.group(1)), int(m.group(2)), int(m.group(3))
        out = f.with_name(f.name[:m.start()] + ".png") if ids else f.with_suffix(".png")
        try:
            write_png(out, w, h, decode(fmt, w, h, f.read_bytes()))
            print(out.name)
        except ValueError as e:
            print(f.name, "-", e)
