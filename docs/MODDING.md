# Modding IzunaRecomp

IzunaRecomp loads mods from a **Mods** folder: each mod is one folder, and
copying it in or deleting it is all it takes to add or remove it. The hub's
**MODS** page turns mods on and off, sets their order and removes them. A mod
can, in any mix:

- **replace the game's files** - models, motions, effects, stages, text,
  sound banks (asset injection);
- **replace the game's textures** with pictures of any size up to 2048 x 2048
  - the HUD, menus, models' skins - or hide one with a transparent picture;
- **hide draws** by their shader (an effect, a flash, a filter);
- **run a script** (Lua) with the game's memory, the controller, the screen,
  and **hooks on any of the game's 48,072 functions**: before or after it,
  its arguments and its result changed, or the function skipped - how the
  game and its mechanics work, changed at the source.

## Where mods go

| Build | Mods folder | Order and switches |
| --- | --- | --- |
| PS5 | `Mods/` in the app's folder, beside `Game Data/` (`/data/homebrew/<title id>/Mods/` over FTP) | `UserData/mods.toml` |
| PC / Linux | `mods/` beside the executable | `mods/mods.toml` |

A new mod folder is on by default. The order matters only when two mods
replace the same game file: the first one in the list wins (the MODS page
shows which files a mod loses this way). Changes take effect at START GAME.

## A mod's folder

```
Mods/
  Ryu's New Look/
    mod.toml
    files/
      char/ryu_b/ryu_b.gmd
    scripts/
      main.lua
    textures/
      512x256-18-3f9a01c2d4e5b607.png
```

`files/` holds game files **by their path in the game**: `files/char/ryu_b/
ryu_b.gmd` replaces the game's `char\ryu_b\ryu_b.gmd`. Any of the game's
5099 files can be replaced this way (models, textures, motions, effects,
stages, text, sound banks). A file that is not one of the game's is reported
in the log and left out.

`mod.toml`:

```toml
name = "Ryu's New Look"
author = "you"
version = "1.0"
description = "What it changes."

# Optional: load one of the game's own files in place of another, with
# nothing of the game's in the mod (forward slashes, single quotes).
[replace]
'char/ryu_b/ryu_b.gmd' = 'char/ryu_c/ryu_c.gmd'
```

Everything in `mod.toml` is optional; without one the mod is named after its
folder. `examples/mods/Ryu Costume Swap (test)` is a complete mod of the
`[replace]` kind.

## Izuna Mod Studio

`tools/studio/` is the mod maker: open its `index.html` in Edge or Chrome
(or build it as a Windows app: `tools/studio/README.md`). It browses the
game's files by name, makes and checks mods (files, swaps, textures, hidden
shaders, scripts), turns texture dumps into pictures, shows the game's
models and their textures, and exports mods as ZIPs.

## Finding and extracting the game's files

`tools/mods/ng2_files.py` reads the file table from your own copy of the
game (the folder with `default.xex` and the `.ng2` archives) - nothing of the
game ships with the tool:

```sh
python3 tools/mods/ng2_files.py "Game Data" list ryu            # id, size, archive, path
python3 tools/mods/ng2_files.py "Game Data" extract out 'char/ryu_b/*'
python3 tools/mods/ng2_files.py "Game Data" new-mod Mods "Ryu's New Look" 'char/ryu_b/ryu_b.gmd'
```

`new-mod` makes the mod's folder with a `mod.toml` and the files to edit in
`files/`. It needs Python 3.8+ and the `cryptography` package.

To see which files the game loads at a given moment (which model is the one
on screen), turn on **Log file loads** on the MODS page (Square): every file
the game loads is then in the log by id and path. Ryu's in-game model, for
example, is `char\ryu_b\ryu_b.gmd`: the console's logs show it read from
`cmn.ng2` at each chapter's start. The other `char\ryu*` models are most
likely his other costumes and the `r_ryu_*` ones the cutscenes' - the load log
will say.

## How it works

The game has a developer's way to load a file loose instead of from its
archives: each load request carries a flag that sends it to
`D:\databin\<the file's path>`. IzunaRecomp sets that flag for the files the
enabled mods bring and points the open at the mod's file, so **the game loads
a mod's file itself**, through its own file calls - nothing is patched into the
archives, and nothing is emulated. A mod's file may be larger than the game's:
the game is told the larger size before it makes room for it. Whether a much
larger file fits the game's memory for it is up to the game (a model a few
times the size of the original may not); the log says what was loaded from
where.

The hooks are in `config/hooks/mods.toml`, the loader in
`src/izuna/izuna_mods.cpp` (its header comment has the addresses and the
tables), the MODS page in `src/ps5/hub/hub_pages.cpp`.

`[replace]` copies the game file it names out of its archive once, into the
cache (`UserData/cache/mods/` on the PS5), and loads it from there.

## Script mods

A mod with a `scripts/main.lua` (or `script = "other.lua"` in its mod.toml)
is a script mod. The script is Lua 5.4. It runs once when the game starts;
the functions it defines are then called by the game:

| Function | When |
| --- | --- |
| `on_frame()` | once a frame, on the game's render thread, before the frame is shown |

Each mod has its own Lua, with these and nothing else - no files, no system:

**The game's memory** (`mem`). Addresses are the game's (Xbox 360) addresses,
values its big-endian ones, already turned round. A read of memory that is not
there gives `nil` and a write `false` - never a crash. Only the game's own
memory and executable; not the GPU's.

| | |
| --- | --- |
| `mem.u8 / s8 / u16 / s16 / u32 / s32 / f32 / f64(addr)` | a value, or nil |
| `mem.set_u8 ... set_f64(addr, value)` | true if written |
| `mem.string(addr [, max])` | a NUL-ended string (256 bytes at most by default) |
| `mem.bytes(addr, n)`, `mem.set_bytes(addr, s)` | raw bytes as a Lua string |
| `mem.readable(addr [, n [, for_writing]])` | whether it is there |

**The controller** (`pad`), as the game sees it. Buttons by their Xbox or
PlayStation name: `a`/`cross`, `b`/`circle`, `x`/`square`, `y`/`triangle`,
`lb`/`l1`, `rb`/`r1`, `lt`/`l2`, `rt`/`r2`, `ls`/`l3`, `rs`/`r3`,
`start`/`options`, `back`/`touchpad`, `up`, `down`, `left`, `right`.

| | |
| --- | --- |
| `pad.held(name)`, `pad.pressed(name)`, `pad.released(name)` | this frame (pressed / released: since the last) |
| `pad.sticks()` | `lx, ly, rx, ry`, -1 to 1 (up is +) |
| `pad.triggers()` | `l, r`, 0 to 1 |
| `pad.buttons()` | XInput's button bits |

**The screen** (`ui`), in a 1920 x 1080 space fitted to the display.
Colours are `0xRRGGBBAA`. What is drawn lasts one frame: draw it each frame.

| | |
| --- | --- |
| `ui.text(x, y, text [, colour [, size]])` | |
| `ui.box(x, y, w, h [, colour])`, `ui.frame(...)` | filled / outlined |
| `ui.notify(text [, seconds])` | a notice at the top right |

**The rest**: `log(...)` (and `print`) writes to the game's log as
`[script] <mod>: ...`; `game.frame` counts frames, `game.time()` is seconds
since the start, `game.mod` is the mod's name, `game.file_name(id)` the game's
path for a file id; `require("name")` loads `scripts/name.lua` from the mod
(`require("ui.menu")` loads `scripts/ui/menu.lua`), once. `load` takes text
only.

A script that runs too long in one call (20 million Lua instructions: an
endless loop) or uses more than 64 MB is stopped with an error, not the game;
after five errors it is stopped for the session and a notice says so. The
errors are in the log with their line.

`examples/mods/Input Display (script example)` is a complete script mod.

### Hooks on the game's functions

Every one of the game's functions can be hooked: `hook.before(address, fn)`
runs `fn` just before the game's function at `address` (its first
instruction, as the recompiled code names it: `sub_823D6128` is 0x823D6128),
`hook.after(address, fn)` just after it. `fn` gets the registers:

| | |
| --- | --- |
| `r.r3` ... `r.r10` | the integer arguments (before), `r.r3` the result (after) |
| `r.f1` ... `r.f13` | the float arguments, `r.f1` the float result |
| `r.r1` | the stack pointer (the game's memory: `mem.u32(r.r1 + 0x54)`) |
| `r.r3 = v` | changes it: an argument before, the result after |
| `return true` (before) | skips the game's function: its result is what the hook left in r3 / f1 |
| `r:call(address, a1, ..., a8)` | calls a game function, arguments in r3-r10; gives back its r3 and f1 |

`hook.before` / `hook.after` give back an id for `hook.remove(id)`;
`hook.is_function(address)` says whether a function starts there. A hook
runs on the game thread that called the function and has 5 million Lua
instructions; the registers are valid only while it runs. A hook on a
function the game calls thousands of times a frame costs that many Lua calls:
hook what you need. `examples/mods/File Watch (hook example)` hooks the
game's file-size function to show every file the game asks for.

Finding a function: the recompiled code (`generated/default/*.cpp`, made from
your own copy of the game) has every function by address, its instructions
as comments; the Windows modding tools will add a browser for it.

### The drawing

| | |
| --- | --- |
| `render.hide_shader(hash)`, `render.show_shader(hash)` | draws with that pixel or vertex shader left out / drawn again |
| `ui.image(path, x, y, w, h [, tint])` | a PNG / JPEG / TGA / BMP from the mod's folder, over the game |

## Textures

A mod's `textures/` folder holds pictures named by a **texture's id**:
`<width>x<height>-<format>-<hash>.png` - its size, its Xbox 360 format and
the hash of its contents, so the same texture has the same id wherever and
whenever the game loads it. The picture is drawn instead of the game's
texture, at the picture's own size (a 256 x 256 HUD icon can be 1024 x 1024),
with its mipmaps made from it; it is read in the background the first time
the texture is drawn, so the game's own shows for a frame or two. Pictures in
the dump's channel order (below); at most 2048 x 2048.

To find a texture's id, turn on **Save textures** on the MODS page (R2): every
texture the game draws is then saved, once, to `Texture Dump/` beside `Mods/`,
named by its id. Turn them into PNGs on a PC:

```sh
python3 tools/native/tex2png.py --ids "Texture Dump"
```

Edit the PNG and put it in the mod's `textures/` folder with the same name.
A HUD element is hidden by giving its texture a fully transparent picture.

## Hiding draws

```toml
hide_shaders = ["0123456789abcdef"]
```

in `mod.toml` leaves out every draw made with that pixel or vertex shader -
an effect, a flash, a full-screen filter. The hashes are in the Kasumi frame
capture's draw log (the overlay's Capture frame); a script can do the same
at the right moments with `render.hide_shader`.

## Not yet

- Names for the game's own functions and data (Ryu's health, the enemies,
  the Flying Swallow's flash) - they come as the game is mapped; until then
  hooks and `mem` take addresses.
- Modding tools for Windows with a window: a model exporter and importer, a
  mod maker.
- Replacing the pre-rendered videos and the streamed music (`.wmv`,
  `.xwb`), which the game reads outside its file table.
- DLC files as a `[replace]` source.
