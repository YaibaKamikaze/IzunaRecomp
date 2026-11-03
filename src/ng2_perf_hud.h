// The on-screen readouts, top right: FPS, CPU, RAM, GPU and video memory, as
// switched on in the settings (F8 shows or hides them all). Always present,
// drawing only what is on.

#pragma once

#include <imgui.h>
#include <rex/ui/imgui_dialog.h>

// (Global namespace, like its definition in ng2_settings.h.)
struct Ng2Settings;

namespace ng2 {

// Points the HUD at the live settings, so a switch takes effect at once.
void SetHudSettings(const Ng2Settings* settings);

class PerfHudOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit PerfHudOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace ng2
