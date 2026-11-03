// Izuna Mod Studio - the game's models (.gmd), as far as they are read yet.
//
// A .gmd is a chunk file ("GMD", Team Ninja's format of the time) whose third
// part is an XPR2 bundle - the Xbox 360 SDK's resource package:
//   USER  the model's own description (MdlGeo / ObjGeo / GeoDecl: meshes and
//         vertex layouts; MdlInfo / ObjInfo: the nodes - the skeleton)
//   TX2D  textures: a Direct3D texture header, its GPU fetch constant at +28
//   VBUF  vertex buffers, IBUF index buffers (headers; the data after them)
// The textures are read here (untiled and decoded as the Kasumi renderer
// does: src/native/native_textures.cpp). The meshes and the skeleton - the
// model exporter (glTF) and importer - come next.
"use strict";
var Izuna = (globalThis.Izuna = globalThis.Izuna || {});

Izuna.model = (function () {
  const be32 = (b, o) => Izuna.be32(b, o);

  // Xenos tiled address of block (x, y), in bytes (native_textures.cpp, TiledOffset2D).
  function tiledOffset(x, y, pitch, log2bpb) {
    pitch = (pitch + 31) & ~31;
    const macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log2bpb + 7);
    const micro = ((x & 7) + ((y & 0xe) << 2)) << log2bpb;
    const offset = macro + ((micro & ~0xf) << 1) + (micro & 0xf) + ((y & 1) << 4);
    return ((offset & ~0x1ff) << 3) + ((y & 16) << 7) + ((offset & 0x1c0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3f);
  }

  // Xenos texture formats -> {vk (the dump decoder's), block, bpb}.
  const FORMATS = {
    18: { vk: 133, block: 4, bpb: 8, name: "DXT1" },
    19: { vk: 135, block: 4, bpb: 16, name: "DXT2/3" },
    20: { vk: 137, block: 4, bpb: 16, name: "DXT4/5" },
    49: { vk: 141, block: 4, bpb: 16, name: "DXN" },
    59: { vk: 139, block: 4, bpb: 8, name: "DXT3A" },
    60: { vk: 139, block: 4, bpb: 8, name: "DXT5A" },
    6: { vk: 37, block: 1, bpb: 4, name: "8888", argb: true },
    2: { vk: 9, block: 1, bpb: 1, name: "8" },
  };

  // The XPR2 bundle in a .gmd: {resources: [{type, header, size}], data (offset of its data)}.
  function bundle(d) {
    for (let o = 0; o + 16 <= Math.min(d.length, 0x100000); o += 0x10) {
      if (d[o] !== 0x58 || d[o + 1] !== 0x50 || d[o + 2] !== 0x52 || d[o + 3] !== 0x32) continue;  // "XPR2"
      const headerSize = be32(d, o + 4), count = be32(d, o + 12), base = o + 12;
      if (count > 4096) continue;
      const resources = [];
      for (let i = 0; i < count; i++) {
        const e = o + 16 + 16 * i;
        resources.push({ type: String.fromCharCode(d[e], d[e + 1], d[e + 2], d[e + 3]), header: base + be32(d, e + 4), size: be32(d, e + 8) });
      }
      return { at: o, resources, data: base + headerSize };
    }
    return null;
  }

  // Its textures: [{index, w, h, format, name, tiled, address, pitch}].
  function textures(d, b) {
    const out = [];
    b.resources.forEach((r, i) => {
      if (r.type !== "TX2D") return;
      const fc = [0, 1, 2, 3, 4, 5].map((k) => be32(d, r.header + 28 + 4 * k));
      const format = fc[1] & 0x3f;
      out.push({
        index: i, format, name: (FORMATS[format] || {}).name || "format " + format,
        w: (fc[2] & 0x1fff) + 1, h: ((fc[2] >>> 13) & 0x1fff) + 1,
        tiled: fc[0] >>> 31, pitch: (fc[0] >>> 22) & 0x1ff, endian: (fc[1] >>> 6) & 3,
        address: b.data + ((fc[1] >>> 12) << 12),
      });
    });
    return out;
  }

  // A texture's top level as RGBA8.
  function texturePixels(d, t) {
    const f = FORMATS[t.format];
    if (!f) throw new Error(t.name + " textures are not shown yet");
    const wb = Math.ceil(t.w / f.block), hb = Math.ceil(t.h / f.block);
    const log2 = Math.log2(f.bpb);
    const pitchBlocks = (t.pitch * 32) / f.block;
    const out = new Uint8Array(wb * hb * f.bpb);
    for (let y = 0; y < hb; y++) {
      for (let x = 0; x < wb; x++) {
        const src = t.address + (t.tiled ? tiledOffset(x, y, pitchBlocks, log2) : (y * ((pitchBlocks + 31) & ~31) + x) * f.bpb);
        const dst = (y * wb + x) * f.bpb;
        for (let k = 0; k < f.bpb; k++) out[dst + k] = d[src + k] || 0;
      }
    }
    // The endian swap (1: 8 in 16, 2: 16 in 32, 3: 8 in 32).
    if (t.endian === 1) for (let i = 0; i + 1 < out.length; i += 2) [out[i], out[i + 1]] = [out[i + 1], out[i]];
    else if (t.endian === 3) for (let i = 0; i + 3 < out.length; i += 4) [out[i], out[i + 1], out[i + 2], out[i + 3]] = [out[i + 3], out[i + 2], out[i + 1], out[i]];
    else if (t.endian === 2) for (let i = 0; i + 3 < out.length; i += 4) [out[i], out[i + 1], out[i + 2], out[i + 3]] = [out[i + 2], out[i + 3], out[i], out[i + 1]];
    if (f.argb) for (let i = 0; i + 3 < out.length; i += 4) [out[i], out[i + 1], out[i + 2], out[i + 3]] = [out[i + 1], out[i + 2], out[i + 3], out[i]];
    return Izuna.texture.decode(f.vk, t.w, t.h, out);
  }

  // A summary: what the model holds.
  function inspect(d) {
    const magic = String.fromCharCode(...d.slice(0, 8)).replace(/\0+$/, "");
    const b = bundle(d);
    if (!b) return { magic, bundle: null, textures: [], counts: {} };
    const counts = {};
    for (const r of b.resources) counts[r.type] = (counts[r.type] || 0) + 1;
    // The USER parts' own chunk names (MdlGeo, MdlInfo, ...).
    const parts = b.resources.filter((r) => r.type === "USER").map((r) => {
      let s = "";
      for (let k = 0; k < 8 && d[r.header + k] >= 32 && d[r.header + k] < 127; k++) s += String.fromCharCode(d[r.header + k]);
      return s || "?";
    });
    return { magic, bundle: b, textures: textures(d, b), counts, parts };
  }

  return { inspect, texturePixels, tiledOffset, FORMATS };
})();
