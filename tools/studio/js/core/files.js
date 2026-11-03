// Izuna Mod Studio - small file formats: a ZIP writer (stored, for mods to
// share), and the part of TOML that mod.toml uses (the game reads it with
// toml++; this reads and writes what the studio puts in it, and keeps the
// rest of a hand-written file's keys).
"use strict";
var Izuna = (globalThis.Izuna = globalThis.Izuna || {});

Izuna.crc32 = (function () {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return (d) => {
    let c = 0xffffffff;
    for (let i = 0; i < d.length; i++) c = t[(c ^ d[i]) & 255] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  };
})();

// [{path: "Mod/files/a.gmd", data: Uint8Array}] -> a ZIP's bytes (no compression:
// the game's files are mostly compressed already, and it keeps this small).
Izuna.zip = function (entries) {
  const enc = new TextEncoder(), parts = [], central = [];
  let offset = 0;
  const now = new Date();
  const time = (now.getHours() << 11) | (now.getMinutes() << 5) | (now.getSeconds() >> 1);
  const date = ((now.getFullYear() - 1980) << 9) | ((now.getMonth() + 1) << 5) | now.getDate();
  for (const e of entries) {
    const name = enc.encode(e.path.replace(/\\/g, "/"));
    const crc = Izuna.crc32(e.data), size = e.data.length;
    const h = new DataView(new ArrayBuffer(30));
    h.setUint32(0, 0x04034b50, true); h.setUint16(4, 20, true); h.setUint16(6, 0x0800, true);  // (UTF-8 names)
    h.setUint16(10, time, true); h.setUint16(12, date, true);
    h.setUint32(14, crc, true); h.setUint32(18, size, true); h.setUint32(22, size, true); h.setUint16(26, name.length, true);
    parts.push(new Uint8Array(h.buffer), name, e.data);
    const c = new DataView(new ArrayBuffer(46));
    c.setUint32(0, 0x02014b50, true); c.setUint16(4, 20, true); c.setUint16(6, 20, true); c.setUint16(8, 0x0800, true);
    c.setUint16(12, time, true); c.setUint16(14, date, true);
    c.setUint32(16, crc, true); c.setUint32(20, size, true); c.setUint32(24, size, true); c.setUint16(28, name.length, true);
    c.setUint32(42, offset, true);
    central.push(new Uint8Array(c.buffer), name);
    offset += 30 + name.length + size;
  }
  const cdSize = central.reduce((n, p) => n + p.length, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true); end.setUint16(8, entries.length, true); end.setUint16(10, entries.length, true);
  end.setUint32(12, cdSize, true); end.setUint32(16, offset, true);
  const all = [...parts, ...central, new Uint8Array(end.buffer)];
  const out = new Uint8Array(all.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of all) { out.set(p, o); o += p.length; }
  return out;
};

Izuna.toml = (function () {
  // Values: strings (basic, literal, multi-line basic), booleans, numbers,
  // arrays of those; tables [name]. Keys bare or quoted. -> {"": {...}, name: {...}}
  function parse(text) {
    const tables = { "": {} };
    let cur = tables[""], i = 0;
    const n = text.length;
    const ws = () => { while (i < n && (text[i] === " " || text[i] === "\t")) i++; };
    const comment = () => { ws(); if (text[i] === "#") while (i < n && text[i] !== "\n") i++; };
    const err = (m) => { throw new Error("mod.toml, line " + (text.slice(0, i).split("\n").length) + ": " + m); };
    function str() {
      if (text.startsWith('"""', i)) {
        i += 3;
        if (text[i] === "\n") i++;
        let s = "";
        while (i < n && !text.startsWith('"""', i)) s += text[i] === "\\" ? esc() : text[i++];
        i += 3;
        return s;
      }
      const q = text[i++];
      let s = "";
      while (i < n && text[i] !== q) {
        if (text[i] === "\n") err("a string is not closed");
        s += q === '"' && text[i] === "\\" ? esc() : text[i++];
      }
      i++;
      return s;
    }
    function esc() {
      const c = text[i + 1];
      i += 2;
      const map = { n: "\n", t: "\t", r: "\r", '"': '"', "\\": "\\", b: "\b", f: "\f" };
      if (c in map) return map[c];
      if (c === "u" || c === "U") {
        const len = c === "u" ? 4 : 8;
        const cp = parseInt(text.substr(i, len), 16);
        i += len;
        return String.fromCodePoint(cp);
      }
      err("an unknown escape \\" + c);
    }
    function value() {
      ws();
      const c = text[i];
      if (c === '"' || c === "'") return str();
      if (c === "[") {
        i++;
        const arr = [];
        for (;;) {
          while (i < n && /[\s,]/.test(text[i])) i++;
          if (text[i] === "#") { comment(); continue; }
          if (text[i] === "]") { i++; return arr; }
          arr.push(value());
        }
      }
      const m = /^(true|false|[-+]?[0-9_]+(\.[0-9_]+)?([eE][-+]?\d+)?|0x[0-9a-fA-F_]+)/.exec(text.slice(i, i + 64));
      if (!m) err("a value this does not read");
      i += m[0].length;
      if (m[0] === "true") return true;
      if (m[0] === "false") return false;
      return Number(m[0].replace(/_/g, ""));
    }
    function key() {
      ws();
      if (text[i] === '"' || text[i] === "'") return str();
      const m = /^[A-Za-z0-9_-]+/.exec(text.slice(i, i + 256));
      if (!m) err("a key this does not read");
      i += m[0].length;
      return m[0];
    }
    while (i < n) {
      while (i < n && /\s/.test(text[i])) i++;
      if (i >= n) break;
      if (text[i] === "#") { comment(); continue; }
      if (text[i] === "[") {
        i++;
        const name = key();
        ws();
        if (text[i] !== "]") err("a table name is not closed");
        i++;
        cur = tables[name] = tables[name] || {};
        comment();
        continue;
      }
      const k = key();
      ws();
      if (text[i] !== "=") err("'=' expected after " + k);
      i++;
      cur[k] = value();
      comment();
    }
    return tables;
  }

  const q = (s) => '"' + String(s).replace(/\\/g, "\\\\").replace(/"/g, '\\"').replace(/\n/g, "\\n") + '"';
  // A path key or value: literal ('...') so backslashes stay as they are.
  const lit = (s) => (/['\n]/.test(s) ? q(s) : "'" + s + "'");
  function val(v) {
    if (Array.isArray(v)) return "[" + v.map(val).join(", ") + "]";
    if (typeof v === "boolean" || typeof v === "number") return String(v);
    return q(v);
  }
  function write(tables, comments) {
    let out = comments || "";
    for (const [k, v] of Object.entries(tables[""] || {})) out += k + " = " + val(v) + "\n";
    for (const [name, t] of Object.entries(tables)) {
      if (name === "" || !Object.keys(t).length) continue;
      out += "\n[" + name + "]\n";
      for (const [k, v] of Object.entries(t)) out += (/^[A-Za-z0-9_-]+$/.test(k) ? k : lit(k)) + " = " + (typeof v === "string" ? lit(v) : val(v)) + "\n";
    }
    return out;
  }
  return { parse, write };
})();
