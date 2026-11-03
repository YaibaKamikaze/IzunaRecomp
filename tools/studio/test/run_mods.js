// gjs tools/studio/test/run_mods.js <Game Data> <mods folder> : Mod load/check/save on a real folder.
const GLib = imports.gi.GLib, Gio = imports.gi.Gio;
const root = GLib.get_current_dir() + "/tools/studio/js/core/";
for (const f of ["aes.js", "game.js", "texture.js", "files.js", "mod.js"]) (0, eval)(new TextDecoder().decode(GLib.file_get_contents(root + f)[1]));
const Izuna = globalThis.Izuna;
Izuna.size = (n) => n + " B";
// A Folder over GLib (the interface of js/ui/fs.js).
class GFolder {
  constructor(path) { this.path = path; this.name = GLib.path_get_basename(path); this.readOnly = false; }
  p(rel) { return this.path + (rel ? "/" + rel.replace(/\\/g, "/") : ""); }
  async sub(rel, create) { if (create) GLib.mkdir_with_parents(this.p(rel), 0o755); return new GFolder(this.p(rel)); }
  async list(rel = "") {
    const out = [];
    if (!GLib.file_test(this.p(rel), GLib.FileTest.IS_DIR)) return out;
    const en = Gio.File.new_for_path(this.p(rel)).enumerate_children("standard::*", 0, null);
    let i; while ((i = en.next_file(null))) out.push({ name: i.get_name(), kind: i.get_file_type() === Gio.FileType.DIRECTORY ? "directory" : "file" });
    return out.sort((a, b) => a.name.localeCompare(b.name));
  }
  async walk(rel = "") {
    const out = [];
    const go = async (r, pre) => { for (const e of await this.list(r)) { if (e.name.startsWith(".")) continue; e.kind === "directory" ? await go((r ? r + "/" : "") + e.name, pre + e.name + "/") : out.push(pre + e.name); } };
    await go(rel, "");
    return out.sort();
  }
  async file(rel) { const d = GLib.file_get_contents(this.p(rel))[1]; return { size: d.length, data: d }; }
  async exists(rel) { return GLib.file_test(this.p(rel), GLib.FileTest.EXISTS); }
  async read(rel) { return GLib.file_get_contents(this.p(rel))[1]; }
  async text(rel) { return new TextDecoder().decode(await this.read(rel)); }
  async write(rel, data) { GLib.mkdir_with_parents(GLib.path_get_dirname(this.p(rel)), 0o755); GLib.file_set_contents(this.p(rel), typeof data === "string" ? new TextEncoder().encode(data) : data); }
}
(async () => {
  const g = (n) => GLib.file_get_contents(ARGV[0] + "/" + n)[1];
  const game = Izuna.GameIndex.build(g("default.xex"), g("load_img.bin"), g("pack_img.bin"));
  const ws = new GFolder(ARGV[1]);
  for (const m of await Izuna.Mod.all(ws)) {
    print("== " + m.name + " (" + m.folder + "): files " + m.files.length + " swaps " + Object.keys(m.replace).length + " script " + m.script + " textures " + m.textures.length);
    for (const c of m.check(game)) print("   " + c.level + ": " + c.text);
  }
  // A new mod, saved and read again.
  const m = new Izuna.Mod(ws, "Test Mod");
  Object.assign(m.info, { name: "Test Mod", author: "me", version: "1.0", description: "line \"one\"\nline two", hide_shaders: ["0123456789abcdef", "zz"] });
  m.replace["char/ryu_b/ryu_b.gmd"] = "char/ryu_dlc/ryu_dlc1.gmd";
  m.replace["char\\nope.gmd"] = "char/ryu_c/ryu_c.gmd";
  await m.save();
  await ws.write("Test Mod/files/char/ryu/ryu.gmd", new Uint8Array(10));
  await ws.write("Test Mod/files/not/a/file.bin", new Uint8Array(10));
  await ws.write("Test Mod/textures/bad name.png", new Uint8Array(4));
  print(new TextDecoder().decode(await ws.read("Test Mod/mod.toml")));
  const again = await Izuna.Mod.load(ws, "Test Mod");
  print("description round trip: " + (again.info.description === m.info.description));
  for (const c of again.check(game)) print("   " + c.level + ": " + c.text);
  const z = Izuna.zip(await again.entries());
  print("zip " + z.length + " bytes, " + (await again.entries()).length + " entries");
})().catch((e) => print("ERROR " + e + "\n" + e.stack));
