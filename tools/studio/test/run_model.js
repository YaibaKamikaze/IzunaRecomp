// gjs tools/studio/test/run_model.js <model.gmd> <out dir> : a model's textures, raw RGBA.
const GLib = imports.gi.GLib;
const root = GLib.get_current_dir() + "/tools/studio/js/core/";
for (const f of ["game.js", "texture.js", "model.js"]) (0, eval)(new TextDecoder().decode(GLib.file_get_contents(root + f)[1]));
const Izuna = globalThis.Izuna;
const d = GLib.file_get_contents(ARGV[0])[1];
const m = Izuna.model.inspect(d);
print(m.magic + " " + JSON.stringify(m.counts) + " parts " + JSON.stringify(m.parts));
for (const t of m.textures.slice(0, 31)) {
  let ok = "";
  try {
    const px = Izuna.model.texturePixels(d, t);
    GLib.file_set_contents(ARGV[1] + "/tex" + t.index + "_" + t.w + "x" + t.h + ".rgba", px);
    ok = "ok";
  } catch (e) { ok = String(e.message); }
  print(t.index + " " + t.name + " " + t.w + "x" + t.h + " tiled " + t.tiled + " " + ok);
}
