-- A hook on one of the game's functions: 0x823D6128 is the game's "how big
-- is this file" (it takes a file id in r3 and returns the size in r3). It is
-- asked before every file the game loads, so hooking it shows them coming in.
-- hook.before / hook.after work on any of the game's functions the same way
-- (docs/MODDING.md, "Hooks on the game's functions").

local GET_FILE_SIZE = 0x823D6128
local count, last, last_size = 0, "(none yet)", 0

hook.after(GET_FILE_SIZE, function(r)
  -- After: r3 is the result. The file id was the argument; the function
  -- keeps it in r31 - not something to count on in general, so it is taken
  -- in a before-hook instead (below) and remembered.
  last_size = r.r3
end)

hook.before(GET_FILE_SIZE, function(r)
  local id = r.r3 & 0xFFFF
  count = count + 1
  last = game.file_name(id) or ("#" .. id)
end)

function on_frame()
  ui.box(20, 20, 640, 64, 0x00000090)
  ui.text(36, 28, string.format("files asked for: %d", count), 0xECE6D6FF, 22)
  ui.text(36, 54, string.format("%s  (%d KB)", last, last_size // 1024), 0x9A968CFF, 20)
end
