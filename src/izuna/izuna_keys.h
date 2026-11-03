// [IzunaRecomp] The keyboard as a controller on a PC: one row per pad input,
// each bound to a key (the hub's SETTINGS > Controls, Ng2Settings::pc_keys).
// Each becomes the runtime's keybind_* cvar (input/mnk/mnk_input_driver.cpp),
// read at every poll, so a change applies at once.
//
// The default layout plays with the keyboard alone and no mouse: the left hand
// moves (WASD), jumps (Space), blocks (Shift: the left trigger) and centres
// the camera (Q: the right trigger); the right hand attacks on J K
// L; the arrow keys are the d-pad, which several of the game's menus use.
// Mouse buttons are never offered (the hub's key capture takes keys only).
#pragma once

#include <array>
#include <string>
#include <string_view>

namespace izuna::keys {

struct Action {
  const char* id;     // the settings file's key_<id>
  const char* cvar;   // the runtime's keybind_* cvar
  const char* label;  // the hub's row
  const char* def;    // the default key (a name rex::ui::ParseVirtualKey knows)
};

inline constexpr Action kActions[] = {
    {"a", "keybind_a", "Jump (A / Cross)", "Space"},
    {"x", "keybind_x", "Attack (X / Square)", "J"},
    {"y", "keybind_y", "Strong attack (Y / Triangle)", "K"},
    {"b", "keybind_b", "Projectile (B / Circle)", "L"},
    {"lt", "keybind_left_trigger", "Block (LT / L2)", "Shift"},
    {"rt", "keybind_right_trigger", "Centre camera (RT / R2)", "Q"},
    {"rb", "keybind_right_shoulder", "RB / R1", "O"},
    {"lb", "keybind_left_shoulder", "LB / L1", "U"},
    {"ls_up", "keybind_lstick_up", "Move forward", "W"},
    {"ls_down", "keybind_lstick_down", "Move back", "S"},
    {"ls_left", "keybind_lstick_left", "Move left", "A"},
    {"ls_right", "keybind_lstick_right", "Move right", "D"},
    {"rs_up", "keybind_rstick_up", "Camera up", "T"},
    {"rs_down", "keybind_rstick_down", "Camera down", "G"},
    {"rs_left", "keybind_rstick_left", "Camera left", "F"},
    {"rs_right", "keybind_rstick_right", "Camera right", "H"},
    {"ls_press", "keybind_lstick_press", "Left stick press (L3)", "C"},
    {"rs_press", "keybind_rstick_press", "Right stick press (R3)", "V"},
    {"dpad_up", "keybind_dpad_up", "D-pad up", "Up"},
    {"dpad_down", "keybind_dpad_down", "D-pad down", "Down"},
    {"dpad_left", "keybind_dpad_left", "D-pad left", "Left"},
    {"dpad_right", "keybind_dpad_right", "D-pad right", "Right"},
    {"start", "keybind_start", "Start (pause)", "Return"},
    {"back", "keybind_back", "Back / Select", "Tab"},
};
inline constexpr int kCount = int(sizeof(kActions) / sizeof(kActions[0]));

using Keys = std::array<std::string, kCount>;

inline Keys Defaults() {
  Keys k;
  for (int i = 0; i < kCount; ++i) k[size_t(i)] = kActions[i].def;
  return k;
}

inline int Find(std::string_view id) {
  for (int i = 0; i < kCount; ++i)
    if (id == kActions[i].id) return i;
  return -1;
}

// How a bound key reads in the hub ("Space", "Up", "Numpad4"... "None").
inline std::string Shown(const std::string& key) { return key.empty() ? std::string("None") : key; }

}  // namespace izuna::keys
