# Izuna Mod Studio

The mod maker for IzunaRecomp (docs/MODDING.md). A page that runs in a
browser - Microsoft Edge or Google Chrome on Windows - or, wrapped in
Electron, as a Windows app. Nothing to install for the browser: open
`index.html`.

What it does (v0.1):

- **Start** - your copy of the game (the folder with `default.xex`; read,
  never changed) and a folder for your mods. Both are remembered.
- **Game files** - the game's 5099 files by name, searchable: what each is,
  where it is; copy them out, into a mod to edit, or into a swap.
- **Mods** - the mod maker: a mod's details, the game files it replaces,
  swaps of the game's own files, textures, hidden shaders and its script
  (an editor with examples), checked as the game will check it, exported as
  a ZIP.
- **Textures** - a texture dump from the console (the hub's MODS page, R2) as
  pictures: replace one in a mod (its picture goes in the mod to edit) or
  hide it.
- **Models** - the game's models: what each holds, its textures as pictures.
  The glTF exporter and importer (Blender) are next.
- **Scripting** - the script API, with examples.

Nothing of the game is in the studio: the game's file names come from your
`default.xex` each time (decrypted here: `js/core/aes.js`, `js/core/game.js`).

## As a Windows app

```powershell
powershell -ExecutionPolicy Bypass -File tools\studio\electron\build-windows.ps1
```

downloads Electron (its official Windows build, checked against the
release's checksums) and puts the studio in it:
`tools\studio\out\Izuna Mod Studio\Izuna Mod Studio.exe`.

## Layout

```
index.html, studio.css
js/core/   no browser needed: aes, game (the file index), texture (the dump's
           pictures), files (ZIP, TOML), mod (a mod's folder), model (.gmd)
js/ui/     the pages; fs.js (folders through the File System Access API)
electron/  the Windows app
test/      run_core.js, run_files.js, run_mods.js, run_model.js (gjs, against
           a copy of the game), serve.py + shots.py (the pages in Firefox)
```

Tests: `gjs tools/studio/test/run_core.js "<Game Data>"` and the others; the
pages: `python3 tools/studio/test/serve.py "<Game Data>" &` then
`python3 tools/studio/test/shots.py <out dir> files models`.
