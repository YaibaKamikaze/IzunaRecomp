// Izuna Mod Studio - a mod's folder (docs/MODDING.md), read and written as
// the game reads it (src/izuna/izuna_mods.cpp: the same rules, checked here
// before the mod reaches the console).
//
//   <folder>/mod.toml          name, author, version, description, script,
//                              hide_shaders; [replace] target = source
//   <folder>/files/<path>      game files, by their path in the game
//   <folder>/textures/<id>.png pictures for the game's textures
//   <folder>/scripts/main.lua  a script
"use strict";
var Izuna = (globalThis.Izuna = globalThis.Izuna || {});

Izuna.Mod = class Mod {
  constructor(workspace, folder) {
    this.workspace = workspace;  // the Folder of mods
    this.folder = folder;        // its folder's name
    this.tables = { "": {}, replace: {} };
    this.files = [];      // [{path (game path, backslashes), rel (in files/), size}]
    this.textures = [];   // [{id, rel}]
    this.scripts = [];    // paths under scripts/
    this.manifest = false;
  }

  get info() { return this.tables[""]; }
  get name() { return this.info.name || this.folder; }
  get replace() { return this.tables.replace || (this.tables.replace = {}); }
  get script() { return this.info.script || (this.scripts.includes("main.lua") ? "scripts/main.lua" : ""); }
  get hideShaders() { return Array.isArray(this.info.hide_shaders) ? this.info.hide_shaders : []; }

  static async load(workspace, folder) {
    const m = new Mod(workspace, folder);
    const dir = await workspace.sub(folder);
    if (await dir.exists("mod.toml")) {
      m.tables = Izuna.toml.parse(await dir.text("mod.toml"));
      m.tables.replace = m.tables.replace || {};
      if (m.tables.mod) {  // (keys under [mod]: as the game reads them)
        Object.assign(m.tables[""], m.tables.mod);
        delete m.tables.mod;
      }
      m.manifest = true;
    }
    for (const rel of await dir.walk("files")) {
      const f = await dir.file("files/" + rel);
      m.files.push({ path: rel.replace(/\//g, "\\"), rel, size: f.size });
    }
    for (const e of await dir.list("textures")) {
      if (e.kind !== "file" || !/\.(png|jpe?g|tga|bmp)$/i.test(e.name)) continue;
      m.textures.push({ id: e.name.replace(/\.[^.]+$/, "").toLowerCase(), rel: "textures/" + e.name });
    }
    m.scripts = (await dir.walk("scripts")).filter((p) => p.endsWith(".lua"));
    return m;
  }

  // Every mod in the workspace (folders not starting with '.').
  static async all(workspace) {
    const out = [];
    for (const e of await workspace.list()) {
      if (e.kind !== "directory" || e.name.startsWith(".")) continue;
      try {
        out.push(await Mod.load(workspace, e.name));
      } catch (err) {
        const m = new Mod(workspace, e.name);
        m.loadError = String(err.message || err);
        out.push(m);
      }
    }
    return out;
  }

  async dir() { return this.workspace.sub(this.folder, true); }

  async save() {
    const t = { "": {}, replace: this.replace };
    for (const k of ["name", "author", "version", "description", "script"]) if (this.info[k]) t[""][k] = this.info[k];
    if (this.hideShaders.length) t[""].hide_shaders = this.hideShaders;
    for (const [k, v] of Object.entries(this.info)) if (!(k in t[""]) && v !== "" && k !== "hide_shaders") t[""][k] = v;  // (keys the studio does not edit)
    await (await this.dir()).write("mod.toml", Izuna.toml.write(t, "# Made with Izuna Mod Studio (docs/MODDING.md).\n"));
    this.manifest = true;
  }

  // What the game will say about it: [{level: "error" | "warn" | "ok", text}].
  check(game) {
    const out = [];
    const err = (t) => out.push({ level: "error", text: t }), warn = (t) => out.push({ level: "warn", text: t });
    if (this.loadError) err("could not be read: " + this.loadError);
    if (!this.files.length && !Object.keys(this.replace).length && !this.script && !this.textures.length && !this.hideShaders.length) {
      warn("brings nothing yet: add game files, swaps, textures, a script or hidden shaders");
    }
    if (game) {
      for (const f of this.files) {
        const g = game.find(f.path);
        if (!g) err("files/" + f.rel + " is not one of the game's files - the game leaves it out");
        else if (!f.size) err("files/" + f.rel + " is empty");
        else if (g.size && f.size > g.size * 3) warn("files/" + f.rel + " is " + (f.size / g.size).toFixed(1) + " times the game's (" + Izuna.size(f.size) + " for " + Izuna.size(g.size) + "): the game may not have the memory for it");
      }
      for (const [target, source] of Object.entries(this.replace)) {
        const t = game.find(target), s = game.find(source);
        if (!t) err("swap: " + target + " is not one of the game's files");
        if (!s) err("swap: " + source + " is not one of the game's files");
        else if (s.dlc) err("swap: " + source + " is a DLC file - only the disc's files can be loaded in another's place");
        else if (t && t.size && s.size > t.size * 3) warn("swap: " + source + " is " + (s.size / t.size).toFixed(1) + " times " + target);
        if (this.files.some((f) => game.find(f.path) === t && t)) warn("swap: " + target + " is also in files/ - the file wins");
      }
    }
    for (const tx of this.textures) if (!Izuna.texture.isId(tx.id)) err(tx.rel + ": not named by a texture's id (<w>x<h>-<format>-<hash>)");
    for (const hsh of this.hideShaders) if (!/^[0-9a-fA-F]{16}$/.test(hsh)) err("hide_shaders: '" + hsh + "' is not a shader's hash (16 hex digits)");
    if (this.info.script && !this.scripts.includes(this.info.script.replace(/^scripts\//, ""))) err("script = '" + this.info.script + "' is not in the mod's folder");
    if (!out.length) out.push({ level: "ok", text: "ready: the game will load it as it is" });
    return out;
  }

  // All its files, for a ZIP: [{path: "<folder>/...", data}].
  async entries() {
    const dir = await this.dir(), out = [];
    for (const rel of await dir.walk()) out.push({ path: this.folder + "/" + rel, data: await dir.read(rel) });
    return out;
  }
};
