-- An input display: the buttons held and the sticks, bottom left.
-- A script mod's main.lua runs once when the game starts; on_frame() is then
-- called once a frame. The API is in docs/MODDING.md ("Script mods").

local shown = true
local layout = require("layout")  -- scripts/layout.lua

log("input display ready - L3 + R3 hides it")

function on_frame()
  if pad.held("l3") and pad.pressed("r3") then
    shown = not shown
    ui.notify(shown and "Input display on" or "Input display off", 2)
  end
  if not shown then return end

  local x, y = layout.x, layout.y
  ui.box(x - 12, y - 12, 380, 196, 0x000000A0)
  for _, b in ipairs(layout.buttons) do
    local on = pad.held(b.name)
    ui.box(x + b.x, y + b.y, 44, 44, on and 0xD6201CFF or 0x303034FF)
    ui.text(x + b.x + 8, y + b.y + 10, b.label, on and 0xFFFFFFFF or 0x9A968CFF, 22)
  end
  -- The sticks: a frame each, a dot where the stick is.
  local lx, ly, rx, ry = pad.sticks()
  for i, s in ipairs({{lx, ly}, {rx, ry}}) do
    local cx, cy = x + 196 + (i - 1) * 92, y + 40
    ui.frame(cx - 36, cy - 36, 72, 72, 0x9A968CFF)
    ui.box(cx + s[1] * 30 - 6, cy - s[2] * 30 - 6, 12, 12, 0xECE6D6FF)
  end
  local l2, r2 = pad.triggers()
  ui.box(x + 160, y + 100, 200 * l2, 14, 0xECE6D6FF)
  ui.box(x + 160, y + 128, 200 * r2, 14, 0xECE6D6FF)
  ui.text(x + 160, y + 146, "L2 / R2", 0x9A968CFF, 18)
end
