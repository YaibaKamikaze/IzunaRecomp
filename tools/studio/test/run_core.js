// gjs tools/studio/test/run_core.js <Game Data> : the core against a real copy of the game.
const GLib = imports.gi.GLib;
const root = GLib.get_current_dir() + "/tools/studio/js/core/";
for (const f of ["aes.js", "game.js"]) {
  const [, src] = GLib.file_get_contents(root + f);
  (0, eval)(new TextDecoder().decode(src));
}
const dir = ARGV[0];
const read = (n) => GLib.file_get_contents(dir + "/" + n)[1];
const t0 = Date.now();
const Izuna = globalThis.Izuna;
const idx = Izuna.GameIndex.build(read("default.xex"), read("load_img.bin"), read("pack_img.bin"));
print("built in " + (Date.now() - t0) + " ms: " + idx.files.length + " files, " + idx.files.filter(f => f.dlc).length + " DLC");
for (const id of [0, 3, 714, 715, 716, 720, 5098]) {
  const f = idx.files[id];
  print(id + " " + f.path + " size " + f.size + " " + f.archive + " @" + f.offset.toString(16) + " packed " + f.packed + (f.dlc ? " DLC" : ""));
}
print("find: " + idx.find("char/ryu_b/RYU_B.gmd").id);
