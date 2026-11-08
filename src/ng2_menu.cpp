#include "ng2_menu.h"
#include "ng2_autoskip.h"

#include <algorithm>
#include <filesystem>
#include <string>

#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/window.h>

namespace ng2 {
namespace {

// The accent: the red of the II in the game's logo.
const ImVec4 kAccent(0.78f, 0.16f, 0.18f, 1.0f);

bool CvarExists(const char* name) {
  return rex::cvar::GetFlagInfo(name) != nullptr;
}

// Not a silent no-op: a setting whose cvar this build does not have is a
// finding worth a line in the log.
void SetCvar(const char* name, const std::string& value) {
  if (!CvarExists(name)) {
    REXLOG_WARN("Settings: cvar '{}' is not registered; '{}' not applied", name, value);
    return;
  }
  if (!rex::cvar::SetFlagByName(name, value))
    REXLOG_WARN("Settings: cvar '{}' rejected value '{}'", name, value);
}

}  // namespace

MenuFonts& Fonts() {
  static MenuFonts fonts;
  return fonts;
}

void LoadMenuFonts(ImFontAtlas* atlas) {
  if (atlas == nullptr)
    return;
  // Segoe UI ships with every supported Windows version, so this is not a
  // bundled asset that can go missing from a release - and if it somehow is
  // missing, both pointers stay null and the SDK's default font is used.
  const char* kBody = "C:\\Windows\\Fonts\\segoeui.ttf";
  const char* kTitle = "C:\\Windows\\Fonts\\seguibl.ttf";  // Segoe UI Black
  std::error_code ec;
  if (std::filesystem::exists(kBody, ec)) {
    Fonts().body = atlas->AddFontFromFileTTF(kBody, Fonts().body_size);
  }
  const char* title_path =
      std::filesystem::exists(kTitle, ec) ? kTitle
      : std::filesystem::exists(kBody, ec) ? kBody
                                           : nullptr;
  if (title_path != nullptr) {
    Fonts().title = atlas->AddFontFromFileTTF(title_path, Fonts().title_size);
  }
  REXLOG_INFO("Menu fonts: body={} title={}", Fonts().body != nullptr,
              Fonts().title != nullptr);
}

void ApplyMenuStyle(ImGuiStyle& s) {
  // Start from a known base rather than patching whatever the SDK left, so the
  // result does not depend on the SDK's own theme changing under us.
  ImGui::StyleColorsDark(&s);

  s.WindowRounding = 6.0f;
  s.ChildRounding = 4.0f;
  s.FrameRounding = 4.0f;
  s.GrabRounding = 3.0f;
  s.TabRounding = 4.0f;
  s.PopupRounding = 4.0f;
  s.ScrollbarRounding = 6.0f;
  s.WindowBorderSize = 1.0f;
  s.FrameBorderSize = 0.0f;
  s.WindowPadding = ImVec2(22.0f, 18.0f);
  s.FramePadding = ImVec2(10.0f, 6.0f);
  s.ItemSpacing = ImVec2(10.0f, 7.0f);
  s.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
  s.ScrollbarSize = 13.0f;
  s.GrabMinSize = 12.0f;
  s.SeparatorTextBorderSize = 2.0f;

  ImVec4* c = s.Colors;
  const ImVec4 accent = kAccent;
  const ImVec4 accent_hot(0.90f, 0.22f, 0.24f, 1.00f);

  c[ImGuiCol_Text] = ImVec4(0.91f, 0.91f, 0.93f, 1.00f);
  c[ImGuiCol_TextDisabled] = ImVec4(0.44f, 0.44f, 0.48f, 1.00f);
  c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.075f, 0.085f, 0.97f);
  c[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.115f, 0.99f);
  c[ImGuiCol_Border] = ImVec4(0.24f, 0.24f, 0.27f, 0.85f);
  c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.25f, 1.00f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.27f, 0.27f, 0.30f, 1.00f);
  c[ImGuiCol_TitleBg] = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.10f, 0.11f, 1.00f);
  c[ImGuiCol_MenuBarBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0.09f, 0.09f, 0.10f, 0.60f);
  c[ImGuiCol_ScrollbarGrab] = ImVec4(0.28f, 0.28f, 0.31f, 1.00f);
  c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.36f, 0.36f, 0.39f, 1.00f);
  c[ImGuiCol_ScrollbarGrabActive] = accent;
  c[ImGuiCol_CheckMark] = accent_hot;
  c[ImGuiCol_SliderGrab] = accent;
  c[ImGuiCol_SliderGrabActive] = accent_hot;
  c[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
  c[ImGuiCol_ButtonHovered] = accent;
  c[ImGuiCol_ButtonActive] = accent_hot;
  c[ImGuiCol_Header] = ImVec4(0.24f, 0.16f, 0.17f, 1.00f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.35f, 0.18f, 0.19f, 1.00f);
  c[ImGuiCol_HeaderActive] = accent;
  c[ImGuiCol_Separator] = ImVec4(0.24f, 0.24f, 0.27f, 1.00f);
  c[ImGuiCol_Tab] = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
  c[ImGuiCol_TabHovered] = ImVec4(0.35f, 0.18f, 0.19f, 1.00f);
  c[ImGuiCol_TabSelected] = ImVec4(0.28f, 0.13f, 0.14f, 1.00f);
  c[ImGuiCol_TabSelectedOverline] = accent_hot;
  c[ImGuiCol_TabDimmed] = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
  c[ImGuiCol_TabDimmedSelected] = ImVec4(0.20f, 0.12f, 0.13f, 1.00f);
  c[ImGuiCol_PlotHistogram] = accent;
  c[ImGuiCol_TextSelectedBg] = ImVec4(0.40f, 0.16f, 0.17f, 0.75f);
}

void ApplySound(const Ng2Settings& s) {
  rex::audio::StereoFold fold;  // (centre and surrounds at -3 dB: ITU-R BS.775)
  // Full: the front channels at the level the game mixed them, the limiter
  // (rex/audio/output_stage.h) catching what the fold adds up past full
  // scale - as a 5.1 game sounds through Windows' own stereo fold. The SDK's
  // normalized fold (1 / 1.707) never reaches full scale, so it plays the
  // game 4.6 dB quieter than its mix to avoid a clip the limiter now prevents.
  if (s.audio_full_stereo) fold.scale = 1.0f;
  // Bass: the LFE channel (explosions, impacts) at -6 dB into both sides;
  // stereo folds drop it by convention, which loses it on headphones.
  if (s.audio_stereo_bass) fold.lfe = 0.5f;
  rex::audio::SetStereoFold(fold);
  // A perceived-loudness curve: half way on the row is about half as loud.
  const float v = std::clamp(s.audio_volume, 0, 100) / 100.0f;
  rex::audio::SetOutputGain(v * v);
  static int logged_volume = -1, logged_full = -1, logged_bass = -1;
  if (logged_volume != s.audio_volume || logged_full != int(s.audio_full_stereo) ||
      logged_bass != int(s.audio_stereo_bass)) {
    logged_volume = s.audio_volume;
    logged_full = int(s.audio_full_stereo);
    logged_bass = int(s.audio_stereo_bass);
    REXLOG_INFO("Sound: volume {}%, stereo level {}, bass in stereo {}", s.audio_volume,
                s.audio_full_stereo ? "full" : "reduced", s.audio_stereo_bass ? "on" : "off");
  }
}

void ApplyLiveSettings(const Ng2Settings& s, rex::ui::Window* window) {
  ApplySound(s);
#if !defined(__PROSPERO__)
  // [IzunaRecomp] The keyboard layout (read by the keyboard driver at every poll).
  for (int i = 0; i < izuna::keys::kCount; ++i) SetCvar(izuna::keys::kActions[i].cvar, s.pc_keys[size_t(i)]);
#endif
  // FXAA is the native present's post-process (read at every present), so it
  // changes with the next frame: the antialiasing row is not restart-bound.
  SetCvar("native_antialias", s.antialias);
  ng2::SetSkipAnyCutscene(s.skip_cutscenes_x);  // [IzunaRecomp] test44
  // [IzunaRecomp] test43: the A/V sync delay, live (the audio driver reads it every callback)
  SetCvar("audio_resync_target_frames", std::to_string(6 + (std::clamp(s.audio_delay_ms, 0, 120) * 3 + 8) / 16));
#if !defined(__PROSPERO__)
#if defined(_WIN32)
  SetCvar("izuna_fullscreen_refresh_switch", s.pc_refresh_switch ? "true" : "false");  // [IzunaRecomp] (read every 2 s)
#endif
#endif
#if defined(__PROSPERO__)  // (the PS5's VideoOut flips: no such setting on a PC)
  SetCvar("native_vblank_wait_flips", s.ps5_low_latency ? "true" : "false");  // [IzunaRecomp] (read every vblank)
#endif
  SetCvar("present_effect", s.present_effect);
  if (CvarExists("present_cas_additional_sharpness"))  // (builds with FidelityFX only)
    SetCvar("present_cas_additional_sharpness", std::to_string(s.cas_sharpness));
  SetCvar("present_dither", s.present_dither ? "true" : "false");
  SetCvar("present_letterbox", s.letterbox ? "true" : "false");
  // Unconditional, not from the setting: vsync off is a game-speed change on this
  // title, the row is no longer offered, and this live path must not be able to
  // reintroduce it from a stale value.
  SetCvar("vsync", "true");
  SetCvar("mnk_mode", s.keyboard_control ? "true" : "false");
  // Read afresh every time the guest opens a video, so turning this on mid-run
  // takes effect at the next one rather than at the next launch.
  //
  // Passed through as-is. This used to fold 0 into 1, from when the guest's own
  // decoder produced garbled video and replacing the picture was the only way
  // to watch anything. The v0.5.0 codegen fix repaired that decoder, 0 became
  // the right default and the setting says so - but this line still forced 1,
  // so the replacement decoder ran on every launch no matter what was chosen,
  // and mode 0 was unreachable from the only path that runs at startup.
  SetCvar("ng2_video_mode", std::to_string(s.video_mode));

  if (window != nullptr) {
    window->SetFullscreen(s.fullscreen);
    // Auto-hide is a *mode*, not just a delay. The window only ever hides the
    // pointer while its visibility is kAutoHidden; the delay alone does
    // nothing while it stays kVisible - which is why "Hide the pointer after"
    // never hid anything: the delay was set, the mode never was. So drive the
    // mode from the setting. >0 arms auto-hide with that idle delay (the
    // pointer shows on motion and hides once it stops); 0 means never hide,
    // i.e. plain kVisible.
    if (s.cursor_hide_seconds > 0) {
      window->SetCursorAutoHideDelayMs(
          static_cast<uint32_t>(s.cursor_hide_seconds) * 1000u);
      window->SetCursorVisibility(
          rex::ui::Window::CursorVisibility::kAutoHidden);
    } else {
      window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    }
  }
}

}  // namespace ng2
