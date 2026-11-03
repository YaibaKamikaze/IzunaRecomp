#include "ng2_perf_hud.h"

#include <algorithm>
#include <thread>

#include "ng2_perf.h"
#include "ng2_settings.h"

namespace ng2 {

namespace {
// The app's own settings object: the readouts follow its switches directly.
const Ng2Settings* g_hud_settings = nullptr;
}  // namespace

void SetHudSettings(const Ng2Settings* settings) { g_hud_settings = settings; }

void PerfHudOverlay::OnDraw(ImGuiIO& io) {
  (void)io;
  // FPS is NOT counted here: this callback runs once per HOST present, which on a
  // high-refresh monitor with vsync is the display's rate, not the game's (it
  // read 165 on a 165 Hz panel while the game ran at 60). The counter is driven
  // instead from ng2DiagFrameTick - the guest main-loop frame hook - so it shows
  // the rate the game actually renders at.
  const Ng2Settings* s = g_hud_settings;
  const bool shown =
      s && s->hud_enabled && (s->hud_fps || s->hud_gpu || s->hud_vram || s->hud_cpu || s->hud_ram);
  SetPerfSamplingWanted(shown);
  if (!shown)
    return;

  const PerfSample p = GetPerfSample();

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(vp->WorkPos.x + vp->WorkSize.x - 18.0f, vp->WorkPos.y + 18.0f),
      ImGuiCond_Always, ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.42f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 7.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
  if (ImGui::Begin("##ng2_perfhud", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoMove)) {
    const ImVec4 good(0.55f, 0.95f, 0.60f, 1.0f);
    const ImVec4 warn(0.98f, 0.82f, 0.35f, 1.0f);
    const ImVec4 bad(0.98f, 0.45f, 0.40f, 1.0f);
    const ImVec4 label(0.72f, 0.78f, 0.74f, 1.0f);
    // Fixed per-stat bar colours so each bar is told apart at a glance,
    // independent of the value: FPS blue, CPU and GPU red, VRAM green.
    const ImVec4 blue(0.40f, 0.66f, 0.98f, 1.0f);
    const ImVec4 red = bad;
    const ImVec4 green = good;

    // A single-value bar under a readout, coloured to match the number on a dim
    // track - the instantaneous style of the Fable II overlay. Only drawn when
    // the player has bars on.
    const bool bars = s->hud_bars;
    auto Bar = [&](float fraction, const ImVec4& col) {
      if (!bars)
        return;
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);  // ProgressBar fill
      ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.16f, 0.18f, 0.17f, 0.85f));
      ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), ImVec2(176.0f, 7.0f), "");
      ImGui::PopStyleColor(2);
    };

    if (s->hud_fps) {
      // Against 60, which is what this title targets.
      const ImVec4 c = p.fps >= 57.0f ? good : (p.fps >= 45.0f ? warn : bad);
      ImGui::TextColored(label, "FPS");
      ImGui::SameLine();
      ImGui::TextColored(c, "%5.1f", p.fps);
      Bar(p.fps / 60.0f, blue);
    }
    if (s->hud_cpu) {
      static const float cores =
          float(std::max(1u, std::thread::hardware_concurrency()));
      const float busy = p.cpu_percent / 100.0f * cores;
      const ImVec4 c =
          p.cpu_percent < 50.0f ? good : (p.cpu_percent < 80.0f ? warn : bad);
      ImGui::TextColored(label, "CPU");
      ImGui::SameLine();
      ImGui::TextColored(c, "%5.0f%%", p.cpu_percent);
      ImGui::SameLine();
      ImGui::TextColored(label, " %.1f of %.0f", busy, cores);
      Bar(p.cpu_percent / 100.0f, red);
    }
    if (s->hud_ram) {
      ImGui::TextColored(label, "RAM");
      ImGui::SameLine();
      if (p.ram_valid) {
        const float frac = p.ram_total_mb > 0.0f ? p.ram_mb / p.ram_total_mb : 0.0f;
        const ImVec4 c = frac < 0.7f ? good : (frac < 0.9f ? warn : bad);
        ImGui::TextColored(c, "%.1f / %.1f GB", p.ram_mb / 1024.0f,
                           p.ram_total_mb / 1024.0f);
        Bar(frac, green);  // fixed green, as requested
      } else {
        ImGui::TextColored(label, "n/a");
      }
    }
    if (s->hud_gpu) {
      ImGui::TextColored(label, "GPU");
      ImGui::SameLine();
      if (p.gpu_valid) {
        const ImVec4 c = p.gpu_percent < 80.0f ? good : (p.gpu_percent < 95.0f ? warn : bad);
        ImGui::TextColored(c, "%5.0f%%", p.gpu_percent);
        Bar(p.gpu_percent / 100.0f, red);
      } else {
        // Never a zero that looks like an idle GPU.
        ImGui::TextColored(label, "    n/a");
      }
    }
    if (s->hud_vram) {
      ImGui::TextColored(label, "VRAM");
      ImGui::SameLine();
      if (p.vram_valid) {
        const float frac = p.vram_total_mb > 0.0f ? p.vram_mb / p.vram_total_mb : 0.0f;
        const ImVec4 c = frac < 0.7f ? good : (frac < 0.9f ? warn : bad);
        ImGui::TextColored(c, "%.1f / %.1f GB", p.vram_mb / 1024.0f,
                           p.vram_total_mb / 1024.0f);
        Bar(frac, green);
      } else {
        ImGui::TextColored(label, "n/a");
      }
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

}  // namespace ng2
