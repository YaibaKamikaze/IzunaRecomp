// The app's shared UI look and the settings applied while it runs: the fonts
// and ImGui theme every surface uses (the hub, the in-game menu, the
// readouts, the scripts' text), and the settings that take effect live.
//
// (The PC port's own setup screen and F10 menu lived here; the hub
// (src/ps5/hub) replaced both in every build.)

#pragma once

#include <imgui.h>

#include "ng2_settings.h"

namespace rex::ui {
class Window;
}

// The port's own version, baked in from the VERSION file by CMake.
#ifndef NG2_VERSION
#define NG2_VERSION "dev"
#endif

namespace ng2 {

// The UI's own fonts, loaded from ReXApp::OnConfigureFonts. Either may be null
// (the SDK's built-in font is the fallback), so every use tolerates it.
struct MenuFonts {
  ImFont* body = nullptr;
  ImFont* title = nullptr;
  float body_size = 18.0f;
  float title_size = 30.0f;
};
MenuFonts& Fonts();

// Loads them into the atlas the SDK is about to build (OnConfigureFonts: the
// atlas is finalised straight afterwards).
void LoadMenuFonts(ImFontAtlas* atlas);

// The look, applied globally so the SDK's own overlays share it.
void ApplyMenuStyle(ImGuiStyle& style);

// Pushes the live settings into their cvars, and the window state onto the
// window. A cvar this build does not have is logged and skipped.
void ApplyLiveSettings(const Ng2Settings& settings, rex::ui::Window* window);

// The sound settings onto the audio output stage (volume, the stereo fold's
// level and bass; rex/audio/downmix.h). Any time, any thread.
void ApplySound(const Ng2Settings& settings);

}  // namespace ng2
