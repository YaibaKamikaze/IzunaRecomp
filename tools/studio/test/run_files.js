// gjs tools/studio/test/run_files.js : TOML and ZIP against the example mods.
const GLib = imports.gi.GLib;
const root = GLib.get_current_dir() + "/tools/studio/js/core/";
(0, eval)(new TextDecoder().decode(GLib.file_get_contents(root + "files.js")[1]));
const Izuna = globalThis.Izuna;
for (const m of ["Ryu Costume Swap (test)", "Input Display (script example)"]) {
  const text = new TextDecoder().decode(GLib.file_get_contents("examples/mods/" + m + "/mod.toml")[1]);
  const t = Izuna.toml.parse(text);
  print(m + ": " + JSON.stringify(t));
  const again = Izuna.toml.parse(Izuna.toml.write(t));
  print("  round trip same: " + (JSON.stringify(again) === JSON.stringify(t)));
}
const t = Izuna.toml.parse('name = "a \\"b\\" \\u00e9"\nhide_shaders = [\n "0123", # c\n "4567",\n]\n[replace]\n"char\\\\x" = \'char\\y\'\n');
print(JSON.stringify(t));
const z = Izuna.zip([{ path: "M/mod.toml", data: new TextEncoder().encode("name = \"x\"\n") }, { path: "M/files/a.bin", data: new Uint8Array([1, 2, 3]) }]);
GLib.file_set_contents("/tmp/claude-1000/-home-netro-ng2-ps5/e67dfabd-0350-4b59-9648-0bccbf728a29/scratchpad/t.zip", z);
print("zip " + z.length + " bytes, crc of 'abc' " + Izuna.crc32(new TextEncoder().encode("abc")).toString(16) + " (want 352441c2)");
