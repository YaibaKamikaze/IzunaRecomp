// Izuna Mod Studio - Scripting: the script mods' API (docs/MODDING.md) and
// examples to start from (the Script tab inserts them).
"use strict";
var Izuna = (globalThis.Izuna = globalThis.Izuna || {});

Izuna.pageScripting = {
  title: "Scripting",
  icon: "script",
  templates: {
    "Every frame": `function on_frame()
  -- game.frame counts frames; game.time() is seconds since the start.
end
`,
    "Text on screen": `function on_frame()
  -- A 1920 x 1080 space, fitted to the display. Colours are 0xRRGGBBAA.
  ui.box(20, 20, 360, 48, 0x00000090)
  ui.text(36, 30, "Hello from a mod", 0xECE6D6FF, 24)
end
`,
    "A button combination": `function on_frame()
  -- Buttons by their PlayStation or Xbox name: cross / a, l1 / lb, r2 / rt ...
  if pad.held("l1") and pad.pressed("triangle") then
    ui.notify("L1 + Triangle", 2)
  end
end
`,
    "Hook a game function": `-- Runs before the game's function at that address (its arguments in r3-r10,
-- f1-f13). Return true to skip the function; its result is then r3 / f1.
local ADDRESS = 0x823D6128  -- the game's "file size" function: a file id in r3

hook.before(ADDRESS, function(r)
  log("asked for", game.file_name(r.r3 & 0xFFFF))
end)

-- After it: r3 / f1 are its result, and can be changed.
hook.after(ADDRESS, function(r)
  -- r.r3 = r.r3  -- (the size it gave back)
end)
`,
    "Read and write memory": `-- The game's memory, by its (Xbox 360) addresses. A read of memory that is
-- not there is nil, a write false: never a crash.
local ADDRESS = 0x00000000  -- a value you found

function on_frame()
  local v = mem.u32(ADDRESS)
  if v then
    ui.text(40, 120, string.format("0x%08X = %d", ADDRESS, v), 0xECE6D6FF, 22)
  end
  -- mem.set_u32(ADDRESS, 99)  -- to change it
end
`,
    "Hide a shader at times": `-- Leave out the draws of one shader (its hash from the frame capture's draw
-- log) while a button is held.
local SHADER = "0123456789abcdef"

function on_frame()
  if pad.held("l3") then render.hide_shader(SHADER) else render.show_shader(SHADER) end
end
`,
    "A picture on screen": `-- A PNG from the mod's own folder (here: pictures/logo.png).
function on_frame()
  ui.image("pictures/logo.png", 1600, 40, 280, 140)
end
`,
    "A module": `-- scripts/settings.lua: return a table, and require("settings") gives it.
local settings = require("settings")
`,
  },
  render(el) {
    const h = Izuna.h;
    const table = (rows) => h("div.table-wrap", { style: { marginBottom: "16px" } }, h("table", h("tbody", rows.map(([a, b]) => h("tr", h("td.mono", { style: { width: "42%" } }, a), h("td", b))))));
    el.append(
      h("h1", "Scripting"),
      h("p.lead", "A script mod is Lua 5.4 in scripts/main.lua. It runs once when the game starts; the functions it defines are then called by the game. Each mod has its own Lua with the game's memory, the controller, the screen, hooks on any of the game's 48,072 functions, and nothing else - no files, no system."),
      h("h3", "Events"),
      table([["on_frame()", "Every frame, on the game's render thread, before the frame is shown."]]),
      h("h3", "Hooks on the game's functions"),
      table([
        ["hook.before(address, fn)", "fn(r) just before the game's function at address. return true skips the function (its result: r.r3 / r.f1). Gives an id."],
        ["hook.after(address, fn)", "fn(r) just after it: r.r3 / r.f1 are its result, and can be changed."],
        ["r.r3 ... r.r10, r.f1 ... r.f13", "Its arguments (read and write); r.r1 the stack pointer."],
        ["r:call(address, a1, ... a8)", "Calls a game function (arguments in r3-r10); gives its r3 and f1."],
        ["hook.remove(id), hook.is_function(address)", ""],
      ]),
      h("h3", "Memory"),
      table([
        ["mem.u8 / s8 / u16 / s16 / u32 / s32 / f32 / f64(addr)", "A value, or nil where there is no memory."],
        ["mem.set_u8 ... set_f64(addr, v)", "true if written."],
        ["mem.string(addr [, max]), mem.bytes(addr, n), mem.set_bytes(addr, s)", ""],
        ["mem.readable(addr [, n [, for_writing]])", ""],
      ]),
      h("h3", "The controller"),
      table([
        ["pad.held(name), pad.pressed(name), pad.released(name)", "a / cross, b / circle, x / square, y / triangle, lb / l1, rb / r1, lt / l2, rt / r2, ls / l3, rs / r3, start / options, back / touchpad, up, down, left, right"],
        ["pad.sticks(), pad.triggers(), pad.buttons()", "lx, ly, rx, ry (-1 to 1); l, r (0 to 1); XInput's bits"],
      ]),
      h("h3", "The screen and the drawing"),
      table([
        ["ui.text(x, y, text [, colour [, size]])", "In a 1920 x 1080 space; colours 0xRRGGBBAA. Lasts one frame."],
        ["ui.box / ui.frame(x, y, w, h [, colour])", "Filled / outlined."],
        ["ui.image(path, x, y, w, h [, tint])", "A picture from the mod's folder."],
        ["ui.notify(text [, seconds])", "A notice at the top right."],
        ["render.hide_shader(hash), render.show_shader(hash)", "Leave out the draws of a shader, or draw them again."],
      ]),
      h("h3", "The rest"),
      table([
        ["log(...), print(...)", "To the game's log: [script] <mod>: ..."],
        ["game.frame, game.time(), game.mod, game.file_name(id)", ""],
        ['require("name")', "scripts/name.lua, once."],
      ]),
      h("p.muted", "Limits: 20 million Lua instructions per on_frame, 5 million per hook call, 64 MB of memory; five errors stop a script for the session. Errors are in the game's log with their line."),
      h("h3", "Examples"),
      Object.entries(this.templates).map(([name, code]) => h("div", h("h2", { style: { fontSize: "14px", marginTop: "14px" } }, name), h("pre.code", code))));
  },
};
