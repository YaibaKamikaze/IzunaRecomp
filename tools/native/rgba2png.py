#!/usr/bin/env python3
"""Convert native_frames/ dumps to PNG: raw *.rgba (named captures) and *.qoi
(the numbered frames, QOI since test30).

  tools/native/rgba2png.py <dir or files...>

Raw file names carry the size: cap_12_1280x720.rgba. Standard library only.
"""
import pathlib, re, struct, sys, zlib


def qoi_pixels(data: bytes):
    """Decodes a QOI file (qoiformat.org) to (width, height, RGBA bytes)."""
    if data[:4] != b"qoif":
        raise ValueError("not a QOI file")
    w, h = struct.unpack(">II", data[4:12])
    out = bytearray(w * h * 4)
    index = [(0, 0, 0, 0)] * 64
    r, g, b, a = 0, 0, 0, 255
    p, o, end = 14, 0, len(out)
    while o < end:
        op = data[p]
        p += 1
        if op == 0xFE:
            r, g, b = data[p], data[p + 1], data[p + 2]
            p += 3
        elif op == 0xFF:
            r, g, b, a = data[p], data[p + 1], data[p + 2], data[p + 3]
            p += 4
        elif op >> 6 == 0:
            r, g, b, a = index[op]
        elif op >> 6 == 1:
            r = (r + ((op >> 4) & 3) - 2) & 255
            g = (g + ((op >> 2) & 3) - 2) & 255
            b = (b + (op & 3) - 2) & 255
        elif op >> 6 == 2:
            vg = (op & 63) - 32
            second = data[p]
            p += 1
            r = (r + vg - 8 + (second >> 4)) & 255
            g = (g + vg) & 255
            b = (b + vg - 8 + (second & 15)) & 255
        else:
            run = (op & 63) + 1
            px = bytes((r, g, b, a)) * run
            out[o:o + len(px)] = px
            o += len(px)
            continue
        index[(r * 3 + g * 5 + b * 7 + a * 11) % 64] = (r, g, b, a)
        out[o:o + 4] = bytes((r, g, b, a))
        o += 4
    return w, h, bytes(out)


def png(path_in: pathlib.Path) -> pathlib.Path:
    data = path_in.read_bytes()
    if path_in.suffix == ".qoi":
        w, h, data = qoi_pixels(data)
    else:
        m = re.search(r"_(\d+)x(\d+)\.rgba$", path_in.name)
        w, h = int(m.group(1)), int(m.group(2))
    rows = b"".join(b"\0" + data[y * w * 4:(y + 1) * w * 4] for y in range(h))
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    out = path_in.with_suffix(".png")
    out.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                    chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))
    return out


for arg in sys.argv[1:]:
    p = pathlib.Path(arg)
    files = sorted(list(p.glob("*.rgba")) + list(p.glob("*.qoi"))) if p.is_dir() else [p]
    for f in files:
        print(png(f))
