// Ninja Gaiden II specific runtime tuning.
//
// The player never sees a config file. This header holds the settings that
// make *this* title behave, applied before the guest and the renderer start,
// so the shipped build is correct out of the box.
//
// Every entry cites why it is here. Two sources:
//
//   [compat]   Xenia's game-compatibility entry for 544307D5, which carries
//              the labels requires_clear_memory_page_state_true,
//              requires_protect_zero_false and vsync-off-speedup.
//              https://github.com/xenia-project/game-compatibility/issues/296
//   [measured] measured in this project - the number is in the comment.
//
// HOW IT IS APPLIED
//
// Some cvars are registered late (after OnPreSetup), so SetFlagByName there
// would miss them. cvar::LoadConfig defers values for cvars not registered
// yet and applies them at registration, so the tuning is written to a TOML in
// the cache root and loaded from OnPreSetup. (In the PC port this carried the
// emulated GPU plugin's settings; IzunaRecomp has no such plugin - the Kasumi
// renderer's own cvars come the same way.)

#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <algorithm>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "ng2_settings.h"

struct Ng2Tuning {
  struct Entry {
    const char* name;
    std::string value;
    const char* why;
  };

  // Fixed settings: correctness for this title, not preferences. These are not
  // exposed in the options UI because there is no sane reason to change them.
  static std::vector<Entry> Fixed() {
    return {
        {"protect_zero", "false",
         "[compat] requires_protect_zero_false - the game reads guest 0 during "
         "boot; matches the low-memory commit in OnPostSetup"},
        {"audio_maxqframes", "16",
         "[compat] queued audio frames; 16 is the documented floor and the value "
         "NG2 players use to kill the audio delay"},
#if !defined(__PROSPERO__)  // [ps5 port] D3D12 only
        {"d3d12_queue_priority", "1",
         "high priority command queue; 2 would need administrator rights"},
#endif
    };
  }

  // Settings the player controls, from the settings menu: cvars of the native
  // renderer, the presenter and the runtime, sent by the same deferred-config
  // route as the fixed settings above.
  static std::vector<Entry> FromSettings(const Ng2Settings& s,
                                         const std::string& mappings_file) {
    std::vector<Entry> out;

    // [IzunaRecomp] The Kasumi renderer draws at the preset's scale (1 720p,
    // 2 1440p, 3 4K). Lost in test47's tidy (an edit that did not apply):
    // test47-49 drew at 720p whatever the preset.
    {
      // [IzunaRecomp] A PC's may be 1.5 (a true 1080p).
      char scale[16];
      std::snprintf(scale, sizeof(scale), "%g", s.pc_scale_resolved > 0.0f ? double(s.pc_scale_resolved)
                                                                           : double(s.resolution_scale));
      out.push_back({"native_resolution_scale", scale, "the Kasumi renderer's resolution scale"});
    }

    // [compat] vsync-off-speedup: this title's logic runs off the display
    // rate, so vsync off does not just tear, it changes game speed. Sent as
    // "true" UNCONDITIONALLY rather than from the setting - the setting is no
    // longer offered in the menu, and a stale or hand-edited file must not be
    // able to reintroduce the speed-up through this path.
    out.push_back({"vsync", "true", "[compat] vsync-off-speedup - forced on"});



    // Post-process antialiasing in the native present: none / fxaa /
    // fxaa_extreme (native_present.cpp).
    out.push_back({"native_antialias", s.antialias, "the native present's anti-aliasing (IzunaRecomp)"});

    out.push_back({"present_dither", s.present_dither ? "true" : "false",
                   "dither the 10bpc output down to 8bpc"});
#if defined(__PROSPERO__)
    // [ps5 port] Always kept on the PS5: the hub no longer offers the toggle.
    out.push_back({"present_letterbox", "true", "keep the guest aspect ratio instead of stretching"});
#else
    out.push_back({"present_letterbox", s.letterbox ? "true" : "false",
                   "keep the guest aspect ratio instead of stretching"});
#endif

    // -1 leaves the game's samplers alone (the default).
    out.push_back({"native_anisotropic", std::to_string(std::clamp(s.anisotropic, -1, 4)),
                   "forced anisotropic filtering level, 0=1x .. 4=16x, -1 the game's"});

    // [IzunaRecomp] The Kasumi renderer as the hub's settings say, on the PS5
    // and on a PC alike (before the Windows build had the hub, the PC got the
    // renderer's own defaults: single-threaded recording, whose fences waited
    // on the GPU behind the present - the 30 ms fallbacks and its stutter).
#if defined(__PROSPERO__)
    out.push_back({"native_vblank_display_locked", s.ps5_display_locked_vblank ? "true" : "false",
                   "[IzunaRecomp] frame pacing, the native graphics system's"});
#else
    // [IzunaRecomp] V-Sync (the hub's Display tab): Auto, 60 Hz, 59.94 Hz.
    out.push_back({"native_vblank_display_locked", s.pc_vsync == 0 ? "true" : "false",
                   "[IzunaRecomp] V-Sync Auto: the guest vblank on the display's refresh"});
    out.push_back({"native_vblank_hz", s.pc_vsync == 2 ? "59.94" : s.pc_vsync == 1 ? "60" : "0",
                   "[IzunaRecomp] V-Sync: the guest vblank's own rate"});
    out.push_back({"present_low_latency", s.pc_low_latency ? "true" : "false",
                   "[IzunaRecomp] Input latency Low: one picture queued for the screen (present wait)"});
    {
      // [IzunaRecomp] Pipeline builders by the PC's CPU (the PS5 keeps 3 of
      // its 8 cores): half the hardware threads less the game's busiest
      // three, 2 to 6 - a new effect's pipelines come sooner.
      const int hw = int(std::thread::hardware_concurrency());
      const int builders = std::clamp(hw / 2 - 1, 2, 6);
      out.push_back({"native_pipeline_threads", std::to_string(builders),
                     "[IzunaRecomp] threads building pipelines, by the CPU"});
    }
#endif
    out.push_back({"audio_resync", s.ps5_audio_resync ? "true" : "false",
                   "[ps5] cutscene audio fix: drop the audio backlog after a stall"});

    // (-60: every second, keep all; -1: every 10 s kept in turn, plus a burst
    // of 30 consecutive frames a minute, kept all - for flicker; test23's 120
    // outran the console's storage and memory)
    const int save = s.ps5_native_save_frames;
    out.push_back({"native_dump_every", std::to_string(save == -1 ? 600 : std::abs(save)),
                   "[native renderer track] save a native frame every N frames (native_frames/)"});
    out.push_back({"native_dump_keep", save < -1 ? "0" : "20",
                   "[native renderer track] saved frames kept: 20 in turn, or all (0)"});
    out.push_back({"native_dump_burst", save == -1 ? "30" : "0",
                   "[native renderer track] bursts of consecutive saved frames (flicker)"});
    out.push_back({"native_auto_log", std::to_string(s.ps5_native_log_draws),
                   "[native renderer track] log the draws of heavy 3D frames"});
    out.push_back({"native_dump_textures", s.ps5_native_save_textures ? "true" : "false",
                   "[native renderer track] save each newly decoded texture (native_textures/)"});
    out.push_back({"native_pipeline_libraries", s.ps5_native_pipeline_libraries ? "true" : "false",
                   "[native renderer track] pipelines linked from per-shader parts"});
    out.push_back({"native_threaded", s.ps5_native_threaded ? "true" : "false",
                   "[native renderer track] the game's render thread records, a worker draws"});
    out.push_back({"native_dump_resolves", s.ps5_native_save_resolves ? "true" : "false",
                   "[native renderer track] save every resolve of each logged frame (native_resolves/)"});
    out.push_back({"native_ignore_pipeline_cache", s.ps5_native_fresh_pipelines ? "true" : "false",
                   "[native renderer track] build every pipeline afresh"});
    // [IzunaRecomp] Input latency Low: the guest vblank waits for the display's
    // pending flip (izuna_gpu.cpp, VsyncThread), so the swapchain stays shallow
    // and each frame shows at the next refresh. The swapchain keeps three
    // images either way: two only moved the wait (test44's paints waited a
    // whole refresh in acquire with three).
#if defined(__PROSPERO__)
    out.push_back({"present_low_latency", "false", "[ps5] three swapchain images"});
    out.push_back({"native_vblank_wait_flips", s.ps5_low_latency ? "true" : "false",
                   "[IzunaRecomp] Low input latency: guest vblanks wait for the display's pending flip"});
#endif
    {
      int after = s.ps5_frame_report_after;
      if (const char* env = std::getenv("NG2_FRAME_REPORT_AFTER"); env && *env)
        after = std::atoi(env);
      out.push_back({"native_capture_after_seconds", std::to_string(after),
                     "[ps5] diagnostic: a Kasumi frame capture this many seconds in (0 off)"});
    }

    // (Texture dumps and packs were the emulated GPU's texture-cache cvars on
    // the PC; with the Kasumi renderer the texture hook is the way in -
    // src/ps5/ps5_textures.cpp on the PS5 - and asset injection is to come.)

    // Keyboard-as-pad. The driver is in the runtime and defaults to off, so a
    // PC port that never sets this only works with a real controller.
    out.push_back({"mnk_mode", s.keyboard_control ? "true" : "false",
                   "drive the guest pad from the keyboard"});

    // The d-pad on plain arrow keys. The runtime binds it to Shift+Arrow,
    // which is awkward to press and, as it turns out, does not reach the game
    // at all - a bare Shift+Down does not move the main menu cursor, while the
    // unmodified left-stick keys do. Several of this title's menus navigate
    // with the d-pad, so on the default binding they cannot be used from the
    // keyboard. The arrow keys are free: the left stick is on WASD.
    // [IzunaRecomp] The player's layout (SETTINGS > Controls, izuna_keys.h) -
    // its d-pad on the plain arrows too.
    for (int i = 0; i < izuna::keys::kCount; ++i)
      out.push_back({izuna::keys::kActions[i].cvar, s.pc_keys[size_t(i)], "[IzunaRecomp] the keyboard layout"});

    if (!mappings_file.empty()) {
      // The runtime resolves this relative to the working directory, so a
      // shortcut that starts the game from anywhere else silently loses every
      // controller mapping. Give it an absolute path.
      out.push_back({"hid_mappings_file", mappings_file,
                     "absolute path - the default is resolved against the CWD"});
    }

    return out;
  }

  /// Write the entries to `path` and load them into the cvar registry.
  /// Returns false if the file could not be written; a parse failure is
  /// reported by the SDK's own log line.
  static bool Apply(const std::filesystem::path& path,
                    const std::vector<Entry>& entries) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
      REXLOG_WARN("Tuning: cannot write {}", path.string());
      return false;
    }
    out << "# Ninja Gaiden II tuning - generated every launch, do not edit.\n"
        << "# Player-facing settings live in the in-game options screen.\n\n";
    for (const auto& e : entries) {
      out << "# " << e.why << "\n";
      out << e.name << " = " << Quote(e.value) << "\n\n";
    }
    out.close();

    rex::cvar::LoadConfig(path);
    for (const auto& e : entries) {
      REXLOG_INFO("Tuning: {} = {} (now {})", e.name, e.value,
                  rex::cvar::GetFlagByName(e.name));
    }
    return true;
  }

 private:
  // Fixed precision rather than the shortest round-trip: these end up in a
  // TOML file a human may read, and "0.200" is easier to recognise than
  // "0.20000000000000001".
  static std::string ToString(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return buf;
  }

  // Numbers and booleans go in bare; everything else is a TOML string.
  static std::string Quote(const std::string& v) {
    if (v == "true" || v == "false") return v;
    bool numeric = !v.empty();
    for (char c : v) {
      if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.' && c != '-')
        numeric = false;
    }
    if (numeric) return v;
    std::string escaped;
    for (char c : v) {
      if (c == '\\' || c == '"') escaped += '\\';
      escaped += c;
    }
    return "\"" + escaped + "\"";
  }
};
