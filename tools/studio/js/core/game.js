// Izuna Mod Studio - the game's files, from the player's own copy of the game
// (as src/izuna/izuna_mods.cpp and tools/mods/ng2_files.py read them):
//
//   default.xex   the executable: a path per file id (0x83AEBC98, 5099 of them)
//   load_img.bin  each file's size
//   pack_img.bin  where each sits in the .ng2 archives (archive, offset, size)
//
// Nothing of the game is in the studio: these are read from the folder the
// player picks, each time.
"use strict";
var Izuna = (globalThis.Izuna = globalThis.Izuna || {});

Izuna.be32 = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
Izuna.be16 = (b, o) => (b[o] << 8) | b[o + 1];

// --- default.xex ------------------------------------------------------------------

Izuna.xex = (function () {
  const RETAIL_KEY = new Uint8Array([0x20, 0xb1, 0x85, 0xa5, 0x9d, 0x28, 0xfd, 0xc3, 0x40, 0x58, 0x3f, 0xbb, 0x08, 0x96, 0xbf, 0x91]);
  const HDR_FILE_FORMAT_INFO = 0x3ff, HDR_IMAGE_BASE = 0x10201;

  // The executable's memory image: decrypted, decompressed ("basic" only - a
  // list of data runs and zero runs). {image, base, offset(va)}.
  function decode(d) {
    const be32 = Izuna.be32;
    if (d[0] !== 0x58 || d[1] !== 0x45 || d[2] !== 0x58 || d[3] !== 0x32) throw new Error("default.xex is not an XEX2 file");
    const peOff = be32(d, 8), secOff = be32(d, 16), count = be32(d, 20);
    const hdrs = {};
    for (let i = 0, o = 0x18; i < count; i++, o += 8) hdrs[be32(d, o)] = be32(d, o + 4);
    const fi = hdrs[HDR_FILE_FORMAT_INFO];
    const infoSize = be32(d, fi), encryption = Izuna.be16(d, fi + 4), compression = Izuna.be16(d, fi + 6);
    if (compression > 1) throw new Error("default.xex uses a compression this does not read (" + compression + ")");
    let body = d.subarray(peOff);
    if (encryption) {
      const session = Izuna.aes.decryptEcb(RETAIL_KEY, d.subarray(secOff + 0x150, secOff + 0x160));
      body = Izuna.aes.decryptCbc(session, body);
    }
    let image = body;
    if (compression === 1) {
      const runs = [];
      let total = 0;
      for (let i = 0; i < (infoSize - 8) / 8; i++) {
        const data = be32(d, fi + 8 + i * 8), zeros = be32(d, fi + 12 + i * 8);
        runs.push([data, zeros]);
        total += data + zeros;
      }
      image = new Uint8Array(total);
      let p = 0, q = 0;
      for (const [data, zeros] of runs) {
        image.set(body.subarray(p, p + data), q);
        p += data;
        q += data + zeros;
      }
    }
    const base = hdrs[HDR_IMAGE_BASE] || 0x82000000;
    return { image, base, offset: (va) => va - base };
  }

  return { decode };
})();

// --- The file table ----------------------------------------------------------------

Izuna.GameIndex = (function () {
  const FILE_NAMES = 0x83aebc98, FILE_COUNT = 5099;
  // pack_img.bin's first six slots (the executable's table at 0x83AF0D40;
  // slots 6-19 hold the chapters again).
  const SLOTS = [["cmn.ng2"], ["char.ng2"], [], ["rtm.ng2"], ["other.ng2"], ["other_rtm.ng2"]];
  for (let c = 1; c <= 14; c++) SLOTS[2].push("s_chap_" + String(c).padStart(2, "0") + ".ng2");

  function cString(b, o) {
    let e = o;
    while (e < b.length && b[e]) e++;
    let s = "";
    for (let i = o; i < e; i++) s += String.fromCharCode(b[i]);
    return s;
  }

  // From the three files' bytes: the files, by id.
  function build(xexBytes, loadImg, packImg) {
    const be32 = Izuna.be32;
    const x = Izuna.xex.decode(xexBytes);
    const files = [];
    for (let id = 0; id < FILE_COUNT; id++) {
      const va = be32(x.image, x.offset(FILE_NAMES + 4 * id));
      if (va < x.base || x.offset(va) >= x.image.length) throw new Error("the file table ends early at " + id + " - not Ninja Gaiden II's default.xex?");
      files.push({ id, path: cString(x.image, x.offset(va)), size: 16 + 8 * id + 4 <= loadImg.length ? be32(loadImg, 16 + 8 * id) : 0, archive: null, offset: 0, packed: 0 });
    }
    const tables = [0, 1, 2, 3].map((i) => be32(packImg, 4 * i));
    for (let slot = 0; slot < SLOTS.length; slot++) {
      for (let a = 0; a < SLOTS[slot].length; a++) {
        const p = tables.map((t) => be32(packImg, t + 4 * slot));
        const count = be32(packImg, p[1] + 4 * a), ids = be32(packImg, p[0] + 4 * a);
        const offs = be32(packImg, p[2] + 4 * a), sizes = be32(packImg, p[3] + 4 * a);
        if (count > FILE_COUNT) throw new Error("pack_img.bin is not as expected");
        for (let k = 0; k < count; k++) {
          const f = files[be32(packImg, ids + 4 * k)];
          if (f && !f.archive) {
            f.archive = SLOTS[slot][a];
            f.offset = be32(packImg, offs + 4 * k);
            f.packed = be32(packImg, sizes + 4 * k);
          }
        }
      }
    }
    // A DLC file: no size of its own (the archives keep a placeholder).
    for (const f of files) f.dlc = !f.archive || !f.size || f.size > f.packed;
    const byKey = new Map(files.map((f) => [Izuna.GameIndex.key(f.path), f]));
    return { files, byKey, find: (path) => byKey.get(Izuna.GameIndex.key(path)) || null };
  }

  // A path as the game writes it, for comparing (backslashes, lower case).
  function key(path) {
    return path.replace(/\//g, "\\").replace(/^\\+/, "").toLowerCase();
  }

  // What kind of file it is, from the first bytes (chunk magic) or its name.
  function kindOf(path) {
    const ext = (path.match(/\.([a-z0-9_]+)$/i) || [, ""])[1].toLowerCase();
    const kinds = {
      gmd: "Model", chr: "Character data", mtn: "Motion set", mot: "Motion", sty: "Story script", tdp: "Textures",
      emc: "Effect", bin: "Data", cmn: "Common data", itm: "Item", stg: "Stage", xwb: "Sound bank", xsb: "Sound cues",
    };
    return kinds[ext] || (ext ? ext.toUpperCase() + " file" : "File");
  }

  return { build, key, kindOf, FILE_COUNT };
})();
