// [IzunaRecomp] Script mods: Lua that runs with the game (izuna_scripts.cpp;
// docs/MODDING.md, "Script mods").
//
// A mod's script is scripts/main.lua in its folder (or the file mod.toml
// names with `script = "..."`). It runs once when the game starts; the
// functions it defines are then called by the game's events - on_frame()
// once a frame. Each mod has its own Lua, with the game's memory, the
// controller, the screen and the log to work with, and nothing else (no files,
// no system calls).
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>

#include "izuna/izuna_mods.h"

namespace izuna::scripts {

// At the game's start (izuna::mods::Activate): the enabled mods' scripts
// loaded and run.
void Start(const std::vector<mods::Mod>& mods);
// Once a frame, on the game's render thread (its Direct3D Swap).
void OnFrame();
int Running();  // scripts loaded and not stopped

// What the scripts draw (ui.text, ui.box, ui.image, ui.notify), over the game.
class Overlay final : public rex::ui::ImGuiDialog {
 public:
  Overlay(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate);
  ~Overlay() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  rex::ui::ImmediateTexture* Picture(const std::string& path);
  rex::ui::ImmediateDrawer* immediate_;
  std::unordered_map<std::string, std::unique_ptr<rex::ui::ImmediateTexture>> pictures_;  // (null: unreadable)
};

}  // namespace izuna::scripts
