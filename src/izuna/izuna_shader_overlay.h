// [IzunaRecomp] The on-screen progress of the startup shader build
// (izuna_shaders.h): a panel in the middle of the window - nothing else is on
// screen yet - with a bar and the count, shown from the first shader built
// until the game starts. A start that loads the saved build never shows it.
//
// The builder's worker threads report through Progress (atomics); the panel
// reads it every paint on the UI thread.
#pragma once

#include <atomic>
#include <cstdio>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>

#include "ng2_menu.h"  // Fonts()

namespace izuna::shaders {

struct Progress {
  std::atomic<int> done{0};
  std::atomic<int> total{0};
};

class ProgressOverlay final : public rex::ui::ImGuiDialog {
 public:
  ProgressOverlay(rex::ui::ImGuiDrawer* drawer, const Progress* progress)
      : rex::ui::ImGuiDialog(drawer), progress_(progress) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    const int total = progress_->total.load(std::memory_order_relaxed);
    const int done = progress_->done.load(std::memory_order_relaxed);
    if (total <= 0) return;
    const float fraction = float(done) / float(total);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width = vp->WorkSize.x < 620.0f ? vp->WorkSize.x - 40.0f : 560.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26.0f, 20.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    if (ImGui::Begin("##izuna_shaders", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove)) {
      if (ImFont* f = ng2::Fonts().title) ImGui::PushFont(f);
      ImGui::TextColored(ImVec4(0.45f, 0.70f, 1.00f, 1.0f), "PREPARING SHADERS");
      if (ng2::Fonts().title) ImGui::PopFont();
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.26f, 0.55f, 0.95f, 1.0f));
      char text[48];
      std::snprintf(text, sizeof(text), "%d / %d  (%.0f%%)", done, total, fraction * 100.0f);
      ImGui::ProgressBar(fraction, ImVec2(width, 0.0f), text);
      ImGui::PopStyleColor();
      ImGui::Spacing();
      ImGui::TextColored(ImVec4(0.80f, 0.86f, 0.82f, 0.85f),
                         "Compiling the game's shaders for your GPU from your copy of the game.");
      ImGui::TextColored(ImVec4(0.80f, 0.86f, 0.82f, 0.60f),
                         "Only on the first start (and after an update) - later starts skip this.");
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }

 private:
  const Progress* progress_;
};

}  // namespace izuna::shaders
