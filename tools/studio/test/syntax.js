// gjs tools/studio/test/syntax.js <files...> : each parses.
const GLib = imports.gi.GLib;
let bad = 0;
for (const f of ARGV) {
  try { new Function(new TextDecoder().decode(GLib.file_get_contents(f)[1])); }
  catch (e) { print(f + ": " + e); bad++; }
}
print(bad ? bad + " file(s) with errors" : ARGV.length + " files parse");
