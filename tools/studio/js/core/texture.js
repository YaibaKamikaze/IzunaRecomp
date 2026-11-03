// Izuna Mod Studio - the texture dump (the hub's MODS page, R2: Save textures)
// as pictures. A dump is a texture's top level as the Kasumi renderer uploaded
// it, named <id>_vk<VkFormat>_<w>x<h>.bin, the id <w>x<h>-<format>-<hash>.
// Channels in memory order (as tools/native/tex2png.py, which this follows):
// the order a mod's replacement picture is read in.
"use strict";
var Izuna = (globalThis.Izuna = globalThis.Izuna || {});

Izuna.texture = (function () {
  // <id>_vk<n>_<w>x<h>.bin -> {id, vk, w, h, format}
  function parseDumpName(name) {
    const m = /^((\d+)x(\d+)-(\d+)-([0-9a-f]{16}))_vk(\d+)_(\d+)x(\d+)\.bin$/i.exec(name);
    if (!m) return null;
    return { id: m[1].toLowerCase(), w: +m[2], h: +m[3], format: +m[4], vk: +m[6] };
  }

  function isId(id) {
    return /^\d+x\d+-\d+-[0-9a-f]{16}$/.test(id);
  }

  const half = (h) => {
    const s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    const v = e === 0 ? (m / 1024) * 2 ** -14 : e === 31 ? (m ? 0 : Infinity) : (1 + m / 1024) * 2 ** (e - 15);
    return s ? -v : v;
  };
  const c8 = (v) => Math.max(0, Math.min(255, Math.round(v * 255)));
  const rgb565 = (c) => [(((c >> 11) & 31) * 255 / 31) | 0, (((c >> 5) & 63) * 255 / 63) | 0, ((c & 31) * 255 / 31) | 0];

  function bcColours(c0, c1, alphaOk) {
    const a = rgb565(c0), b = rgb565(c1);
    if (c0 > c1 || !alphaOk) {
      return [[...a, 255], [...b, 255], a.map((x, i) => ((2 * x + b[i]) / 3) | 0).concat(255),
        a.map((x, i) => ((x + 2 * b[i]) / 3) | 0).concat(255)];
    }
    return [[...a, 255], [...b, 255], a.map((x, i) => ((x + b[i]) / 2) | 0).concat(255), [0, 0, 0, 0]];
  }

  // BC3 alpha / BC4 / BC5: 8 bytes -> 16 values.
  function bcAlpha(d, o) {
    const a0 = d[o], a1 = d[o + 1];
    let lo = d[o + 2] | (d[o + 3] << 8) | (d[o + 4] << 16), hi = d[o + 5] | (d[o + 6] << 8) | (d[o + 7] << 16);
    const pal = [a0, a1];
    if (a0 > a1) for (let i = 1; i < 7; i++) pal.push((((7 - i) * a0 + i * a1) / 7) | 0);
    else {
      for (let i = 1; i < 5; i++) pal.push((((5 - i) * a0 + i * a1) / 5) | 0);
      pal.push(0, 255);
    }
    const out = new Array(16);
    for (let i = 0; i < 8; i++) out[i] = pal[(lo >> (3 * i)) & 7];
    for (let i = 0; i < 8; i++) out[8 + i] = pal[(hi >> (3 * i)) & 7];
    return out;
  }

  // VkFormat numbers of the dumps -> RGBA8 pixels (w * h * 4).
  function decode(vk, w, h, d) {
    const px = new Uint8ClampedArray(w * h * 4);
    const put = (x, y, r, g, b, a) => {
      if (x < w && y < h) {
        const o = (y * w + x) * 4;
        px[o] = r; px[o + 1] = g; px[o + 2] = b; px[o + 3] = a;
      }
    };
    const bc = [131, 132, 133, 134, 135, 136, 137, 138, 139, 141];
    const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
    if (bc.includes(vk)) {
      const bw = (w + 3) >> 2, bh = (h + 3) >> 2;
      const size = [131, 132, 133, 134, 139].includes(vk) ? 8 : 16;
      for (let by = 0; by < bh; by++) {
        for (let bx = 0; bx < bw; bx++) {
          const o = (by * bw + bx) * size;
          if (o + size > d.length) continue;
          let tex;
          if (vk === 139) {
            tex = bcAlpha(d, o).map((v) => [v, v, v, 255]);
          } else if (vk === 141) {
            const r = bcAlpha(d, o), g = bcAlpha(d, o + 8);
            tex = r.map((v, i) => [v, g[i], 0, 255]);
          } else {
            const cb = o + size - 8;
            const c0 = dv.getUint16(cb, true), c1 = dv.getUint16(cb + 2, true), idx = dv.getUint32(cb + 4, true);
            const cols = bcColours(c0, c1, size === 8);
            tex = [];
            for (let i = 0; i < 16; i++) tex.push(cols[(idx >>> (2 * i)) & 3].slice());
            if (vk === 135 || vk === 136) {
              for (let i = 0; i < 16; i++) tex[i][3] = ((d[o + (i >> 1)] >> (4 * (i & 1))) & 15) * 17;
            } else if (vk === 137 || vk === 138) {
              const a = bcAlpha(d, o);
              for (let i = 0; i < 16; i++) tex[i][3] = a[i];
            }
          }
          for (let i = 0; i < 16; i++) put(bx * 4 + (i & 3), by * 4 + (i >> 2), ...tex[i]);
        }
      }
      return px;
    }
    for (let i = 0; i < w * h; i++) {
      const x = i % w, y = (i / w) | 0;
      switch (vk) {
        case 9: put(x, y, d[i], d[i], d[i], 255); break;
        case 16: put(x, y, d[i * 2], d[i * 2 + 1], 0, 255); break;
        case 37: case 43: put(x, y, d[i * 4], d[i * 4 + 1], d[i * 4 + 2], d[i * 4 + 3]); break;
        case 64: {
          const v = dv.getUint32(i * 4, true);
          put(x, y, (v & 1023) >> 2, ((v >> 10) & 1023) >> 2, ((v >> 20) & 1023) >> 2, (v >>> 30) * 85);
          break;
        }
        case 70: { const v = dv.getUint16(i * 2, true) >> 8; put(x, y, v, v, v, 255); break; }
        case 77: put(x, y, dv.getUint16(i * 4, true) >> 8, dv.getUint16(i * 4 + 2, true) >> 8, 0, 255); break;
        case 91: put(x, y, ...[0, 1, 2, 3].map((c) => dv.getUint16(i * 8 + c * 2, true) >> 8)); break;
        case 76: { const v = c8(half(dv.getUint16(i * 2, true))); put(x, y, v, v, v, 255); break; }
        case 83: put(x, y, c8(half(dv.getUint16(i * 4, true))), c8(half(dv.getUint16(i * 4 + 2, true))), 0, 255); break;
        case 97: put(x, y, ...[0, 1, 2, 3].map((c) => c8(half(dv.getUint16(i * 8 + c * 2, true))))); break;
        case 100: { const v = c8(dv.getFloat32(i * 4, true)); put(x, y, v, v, v, 255); break; }
        case 103: put(x, y, c8(dv.getFloat32(i * 8, true)), c8(dv.getFloat32(i * 8 + 4, true)), 0, 255); break;
        case 109: put(x, y, ...[0, 1, 2, 3].map((c) => c8(dv.getFloat32(i * 16 + c * 4, true)))); break;
        default: throw new Error("a texture format this does not show yet (VkFormat " + vk + ")");
      }
    }
    return px;
  }

  // A human name for the Xenos format number in an id.
  const FORMATS = { 2: "A8", 6: "8888", 4: "565", 3: "1555", 15: "4444", 18: "DXT1", 19: "DXT2/3", 20: "DXT4/5",
    49: "DXN", 59: "DXT3A", 60: "DXT5A", 10: "8", 11: "16", 22: "16 16" };
  const formatName = (f) => FORMATS[f] || "format " + f;

  return { parseDumpName, isId, decode, formatName };
})();
