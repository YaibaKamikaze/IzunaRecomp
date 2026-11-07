// Ninja Gaiden II for PS5 - every setting the hub and the overlay offer, as
// rows (hub_ui.h). The table is CLAUDE.md 5.6: each PC setting that makes
// sense on a console, mapped onto the same Ng2Settings field the PC menus
// write, so the rest of the port (ng2_tuning.h, ApplyLiveSettings) applies it
// exactly as it does on PC.

#include <algorithm>

#include <rex/cvar.h>

#include "ps5/hub/hub_ui.h"
#include "izuna/izuna_keys.h"

namespace ng2ps5::hub {

const char* TabName(Tab tab) {
  switch (tab) {
    case Tab::kDisplay: return "Display";
    case Tab::kGraphics: return "Graphics";
    case Tab::kSound: return "Sound";
    case Tab::kControls: return "Controls";
    case Tab::kGame: return "Game";
    case Tab::kPerformance: return "Performance";
    case Tab::kDeveloper: return "Developer";
    default: return "";
  }
}

const char* TabBlurb(Tab tab) {
  switch (tab) {
    case Tab::kDisplay: return "Resolution, frame rate, the picture on your screen";
    case Tab::kGraphics: return "How the game's frames are drawn";
    case Tab::kSound: return "Volume, the stereo mix, sync with the picture";
    case Tab::kControls: return "The keyboard as a controller: every button on a key";
    case Tab::kGame: return "Prompts, videos and play";
    case Tab::kPerformance: return "Smoothness and loading";
    case Tab::kDeveloper: return "Diagnostics and the native renderer's tests";
    default: return "";
  }
}

bool TabShown(Tab tab) {
  for (const Setting& s : Settings())
    if (s.tab == tab) return true;
  return false;
}

namespace {

// A two-way switch over a bool field.
Setting Toggle(Tab tab, const char* label, const char* help, bool Ng2Settings::*field, bool def, bool restart,
               bool live, const char* off = "Off", const char* on = "On") {
  Setting s;
  s.tab = tab;
  s.label = label;
  s.help = help;
  s.choices = {off, on};
  s.get = [field](const Ng2Settings& v) { return (v.*field) ? 1 : 0; };
  s.set = [field](Ng2Settings& v, int i) { v.*field = i != 0; };
  s.def = def ? 1 : 0;
  s.restart = restart;
  s.live = live;
  return s;
}

// A choice over string values.
Setting Strings(Tab tab, const char* label, const char* help, std::string Ng2Settings::*field,
                std::vector<std::string> values, std::vector<std::string> names, int def, bool restart, bool live) {
  Setting s;
  s.tab = tab;
  s.label = label;
  s.help = help;
  s.choices = std::move(names);
  s.get = [field, values](const Ng2Settings& v) {
    auto it = std::find(values.begin(), values.end(), v.*field);
    return it == values.end() ? 0 : int(it - values.begin());
  };
  s.set = [field, values](Ng2Settings& v, int i) { v.*field = values[size_t(i)]; };
  s.def = def;
  s.restart = restart;
  s.live = live;
  return s;
}

// A choice over int values.
Setting Ints(Tab tab, const char* label, const char* help, int Ng2Settings::*field, std::vector<int> values,
             std::vector<std::string> names, int def, bool restart, bool live) {
  Setting s;
  s.tab = tab;
  s.label = label;
  s.help = help;
  s.choices = std::move(names);
  s.get = [field, values](const Ng2Settings& v) {
    auto it = std::find(values.begin(), values.end(), v.*field);
    return it == values.end() ? 0 : int(it - values.begin());
  };
  s.set = [field, values](Ng2Settings& v, int i) { v.*field = values[size_t(i)]; };
  s.def = def;
  s.restart = restart;
  s.live = live;
  return s;
}

std::vector<Setting> Build() {
  std::vector<Setting> t;

  // --- Display ---
#if defined(__PROSPERO__)
  t.push_back(Ints(Tab::kDisplay, "Resolution",
                   "What the game renders at: an exact 3x, 2x or 1x of its 1280x720 frame. 1080p is not "
                   "offered: it would be 1.5x, which the renderer cannot do without blurring.",
                   &Ng2Settings::ps5_resolution, {0, 1, 2}, {"4K  (3840 x 2160)", "1440p  (2560 x 1440)", "720p  (1280 x 720)"},
                   0, true, false));
  t.push_back(Ints(Tab::kDisplay, "Frame rate",
                   "The game paces itself from this. Above 60 it runs fast, not smooth, so 60 is the top.",
                   &Ng2Settings::fps, {30, 60}, {"30", "60"}, 1, true, false));
  {
    Setting s = Toggle(Tab::kDisplay, "Base render size",
                       "1280x720 is the community 720p patch: sharper, and what the resolution presets scale. "
                       "The original 1120x584 is the game as shipped.",
                       &Ng2Settings::internal_720p, true, true, false, "1120 x 584 (original)", "1280 x 720");
    t.push_back(std::move(s));
  }
  t.push_back(Ints(Tab::kDisplay, "Render scale (advanced)",
                   "Overrides the scale the resolution picked. Higher is sharper and costs more GPU time.",
                   &Ng2Settings::ps5_render_scale, {0, 1, 2, 3}, {"From resolution", "1x", "2x", "3x"}, 0, true,
                   false));
#else
  // [IzunaRecomp] A PC: a window or the whole screen, its size, and how many
  // times the game's frame the renderer draws.
  t.push_back(Toggle(Tab::kDisplay, "Window", "Fullscreen covers the monitor the window is on.",
                     &Ng2Settings::fullscreen, false, false, true, "Windowed", "Fullscreen"));
  {
    static const int kSizes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    Setting s;
    s.tab = Tab::kDisplay;
    s.label = "Window size";
    s.help = "The window's size when it is not fullscreen.";
    s.choices = {"1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
    s.get = [](const Ng2Settings& v) {
      for (int i = 0; i < 5; ++i)
        if (v.window_width == kSizes[i][0] && v.window_height == kSizes[i][1]) return i;
      return 0;
    };
    s.set = [](Ng2Settings& v, int i) {
      v.window_width = kSizes[i][0];
      v.window_height = kSizes[i][1];
    };
    s.def = 0;
    s.restart = true;
    t.push_back(std::move(s));
  }
  t.push_back(Ints(Tab::kDisplay, "Resolution",
                   "The resolution the game is drawn at - each one a true resolution, shown pixel for pixel. "
                   "Auto matches your screen exactly (on a 16:10 or ultrawide screen the largest 16:9 picture "
                   "that fits, with bars). One above your screen is sharper still and costs more GPU time.",
                   &Ng2Settings::pc_resolution, {0, 720, 768, 900, 1080, 1440, 1800, 2160},
                   {"Auto", "1280 x 720", "1366 x 768", "1600 x 900", "1920 x 1080", "2560 x 1440", "3200 x 1800",
                    "3840 x 2160"},
                   0, true, false));
  t.push_back(Toggle(Tab::kDisplay, "Keep aspect ratio", "Bars instead of stretching the picture to the window.",
                     &Ng2Settings::letterbox, true, false, true));
  t.push_back(Ints(Tab::kDisplay, "V-Sync",
                   "When the game's frames are shown. Auto follows the screen's refresh when it is steady and 60 "
                   "divides it (60, 120, 240 Hz). 60 Hz and 59.94 Hz keep the game's own clock: what a G-Sync / "
                   "FreeSync screen shows evenly. 59.94 is the Xbox 360's rate.",
                   &Ng2Settings::pc_vsync, {0, 1, 2}, {"Auto", "60 Hz", "59.94 Hz"}, 0, true, false));
#endif
  // (No "Keep aspect ratio" row: changing it from the in-game menu went with
  // a crash; the picture always keeps its aspect - see OnConfigurePaths.)
  t.push_back(Toggle(Tab::kDisplay, "Dither output", "Adds faint noise to hide banding in dark gradients.",
                     &Ng2Settings::present_dither, false, false, true));
  {
    Setting s;
    s.tab = Tab::kDisplay;
    s.label = "Field of view";
    s.help = "Widens or narrows the 3D view.";
    s.choices = {"Default"};
    s.get = [](const Ng2Settings&) { return 0; };
    s.set = [](Ng2Settings&, int) {};
    s.unavailable = [](const Ng2Settings&) {
      return std::string("Not in this build: it needs the PC renderer's projection hook, which the PS5 "
                         "renderer does not have yet.");
    };
    t.push_back(std::move(s));
  }

  // --- Graphics ---
  t.push_back(Strings(Tab::kGraphics, "Anti-aliasing", "Smooths jagged edges after the frame is drawn.",
                      &Ng2Settings::antialias, {"none", "fxaa", "fxaa_extreme"}, {"Off", "FXAA", "FXAA Extreme"}, 0,
                      false, true));
  {
    Setting s = Strings(Tab::kGraphics, "Output filter",
                        "How the frame is scaled to the screen. CAS sharpens; FSR upscales.",
                        &Ng2Settings::present_effect, {"bilinear", "cas", "fsr"}, {"Bilinear", "CAS", "FSR"}, 0, true,
                        false);
    // Offered only as far as this build declares it (the presenter lists what
    // it implements); a filter that would do nothing is not a choice.
    s.unavailable = [](const Ng2Settings&) {
      const auto* info = rex::cvar::GetFlagInfo("present_effect");
      if (info && info->constraints.allowed_values.size() > 1)
        return std::string();
      return std::string("This build's presenter has bilinear only.");
    };
    t.push_back(std::move(s));
  }
  t.push_back(Ints(Tab::kGraphics, "Anisotropic filtering", "Sharper textures at a glancing angle.",
                   &Ng2Settings::anisotropic, {-1, 0, 1, 2, 3, 4}, {"Game default", "1x", "2x", "4x", "8x", "16x"}, 0,
                   true, false));

  // --- Game ---
  t.push_back(Toggle(Tab::kGame, "Button prompts",
                     "The game's button pictures as PlayStation buttons or the original ones.",
                     &Ng2Settings::ps5_playstation_prompts, true, true, false, "Original", "PlayStation"));
#if !defined(__PROSPERO__)
  // [IzunaRecomp] A PC: Auto follows the controller (the row above is the PS5's).
  t.pop_back();
  t.push_back(Ints(Tab::kGame, "Button prompts",
                   "The game's button pictures. Auto shows PlayStation buttons when you play with a PlayStation "
                   "controller (the one the hub last saw), the game's own Xbox ones otherwise.",
                   &Ng2Settings::pc_prompts, {0, 1, 2}, {"Auto", "Original", "PlayStation"}, 0, true, false));
#endif
  {
    Setting s = Toggle(Tab::kGame, "Flying Swallow flash",
                       "The white flash the screen gives on a Flying Swallow, as in the original. Removed "
                       "spares the eyes; nothing else about the move changes.",
                       &Ng2Settings::ps5_remove_screen_flash, false, false, true, "Original", "Removed");
    // The emulated GPU's filter (ng2_remove_screen_flash) never caught the
    // game's flash on the console, and it went with that GPU: shown, greyed,
    // until the Kasumi renderer has one.
    s.unavailable = [](const Ng2Settings&) { return std::string("Coming soon."); };
    t.push_back(std::move(s));
  }
  t.push_back(Ints(Tab::kGame, "Skip intro videos", "Goes straight to the title screen.", &Ng2Settings::video_mode,
                   {0, 2}, {"Off", "On"}, 0, false, true));
#if !defined(__PROSPERO__)
  // [IzunaRecomp] --- Controls (a PC): the keyboard, every pad input on a key ---
  t.push_back(Toggle(Tab::kControls, "Keyboard controls",
                     "Play with the keyboard as well as a controller (both drive player 1). The keys are below; "
                     "mouse buttons are never used.",
                     &Ng2Settings::keyboard_control, true, true, false));
  for (int i = 0; i < izuna::keys::kCount; ++i) {
    Setting s;
    s.tab = Tab::kControls;
    s.label = izuna::keys::kActions[i].label;
    s.help = "Cross / Enter, then press the key for it (Esc cancels). A key already in use moves to the key "
             "this one had. Square puts the default back.";
    s.get = [](const Ng2Settings&) { return 0; };
    s.set = [](Ng2Settings&, int) {};
    s.key = i;
    s.live = true;
    s.unavailable = [](const Ng2Settings& v) {
      return v.keyboard_control ? std::string() : std::string("Turn on Keyboard controls first.");
    };
    t.push_back(std::move(s));
  }
#endif
  t.push_back(Toggle(Tab::kGame, "Workaround: chapter 12",
                     "The community fix for the chapter 12 crash. Turn it on only if you are stuck there: it "
                     "causes problems elsewhere.",
                     &Ng2Settings::chapter12_workaround, false, true, false));
  // [IzunaRecomp] --- Sound ---
  {
    std::vector<int> values;
    std::vector<std::string> names;
    for (int v = 0; v <= 100; v += 10) {
      values.push_back(v);
      names.push_back(std::to_string(v) + "%");
    }
    t.push_back(Ints(Tab::kSound, "Volume", "The game's sound as a whole (the hub's own sounds are not changed).",
                     &Ng2Settings::audio_volume, values, names, 10, false, true));
  }
  t.push_back(Toggle(Tab::kSound, "Stereo level",
                     "Full plays the game's mix at the level it was made, with a limiter that keeps the loudest "
                     "moments clean instead of letting them crackle. Reduced is the older, quieter fold.",
                     &Ng2Settings::audio_full_stereo, true, false, true, "Reduced", "Full"));
  t.push_back(Toggle(Tab::kSound, "Bass on stereo",
                     "The game's 5.1 mix has a bass channel (explosions, impacts) that a stereo mix leaves out. "
                     "On brings it into both sides: fuller on headphones and speakers with bass.",
                     &Ng2Settings::audio_stereo_bass, false, false, true));
  t.push_back(Toggle(Tab::kSound, "Cutscene audio fix",
                     "Keeps sound in time with the picture: the game is given sound to make by the clock, so a "
                     "moment it falls behind is caught up instead of leaving voices late for the rest of the "
                     "cutscene, and sound pauses when the picture does. Turn off for the original behaviour.",
                     &Ng2Settings::ps5_audio_resync, true, false, true));
  t.push_back(Ints(Tab::kSound, "Audio delay (A/V sync)",
                   "Holds the game's sound back so it meets the picture, which reaches your screen a frame or two "
                   "after the game makes the sound. Raise it if sounds come early (a hit heard before it lands); "
                   "lower it on a fast monitor. Needs the Cutscene audio fix on.",
                   &Ng2Settings::audio_delay_ms, {0, 20, 40, 60, 80}, {"Off", "20 ms", "40 ms", "60 ms", "80 ms"}, 2,
                   false, true));
  t.push_back(Toggle(Tab::kSound, "Hub music", "Plays the theme (hub/music/*.wav) behind this menu.",
                     &Ng2Settings::ps5_menu_music, true, false, false));


  // --- Performance ---
  t.push_back(Toggle(Tab::kPerformance, "Performance readouts", "Frame rate and CPU in the corner of the screen.",
                     &Ng2Settings::hud_enabled, false, false, true));
  t.push_back(Toggle(Tab::kPerformance, "Readout: frame rate", "", &Ng2Settings::hud_fps, true, false, true));
  t.push_back(Toggle(Tab::kPerformance, "Readout: CPU", "", &Ng2Settings::hud_cpu, true, false, true));
  t.push_back(Toggle(Tab::kPerformance, "Readout: bars", "A small bar under each number.", &Ng2Settings::hud_bars,
                     true, false, true));

#if !defined(__PROSPERO__)
  // [IzunaRecomp] test42: a fixed 144-170 Hz screen shows 60 fps unevenly.
  // [IzunaRecomp] test43: the present mode (the game keeps its own pace in all).
  t.push_back(Strings(Tab::kDisplay, "Present mode",
                      "How finished frames reach the screen; none changes the game's speed. FIFO: each waits for "
                      "the screen's refresh (no tearing). FIFO relaxed: a late frame may tear instead of waiting. "
                      "Mailbox: the newest frame at each refresh, lower latency, no tearing. Immediate: at once, "
                      "lowest latency, tears. Auto is FIFO. From the next start.",
                      &Ng2Settings::pc_present_mode, {"auto", "fifo", "fifo_relaxed", "mailbox", "immediate"},
                      {"Auto", "FIFO (V-Sync)", "FIFO relaxed", "Mailbox", "Immediate (tearing)"}, 0, true, false));
  t.push_back(Toggle(Tab::kDisplay, "Fullscreen refresh",
                     "In fullscreen on a screen whose refresh rate 60 does not divide (144, 165, 170 Hz) and without "
                     "G-Sync / FreeSync, a multiple of 60 switches it to 120, 180 or 240 Hz, so every frame stays on "
                     "screen equally long. Your desktop's rate comes back when the game closes.",
                     &Ng2Settings::pc_refresh_switch, true, false, true, "Monitor's own", "A multiple of 60"));
#endif
  // [IzunaRecomp] test44: every cutscene skippable, watched or not.
  t.push_back(Toggle(Tab::kGame, "Skip cutscenes with Cross / A",
                     "Press Cross (A on an Xbox controller, or Start / Options) to skip a cutscene, also the first time "
                     "you see it - the game normally lets you skip only the ones you have already watched.",
                     &Ng2Settings::skip_cutscenes_x, true, false, true));
  // --- Developer ---
  t.push_back(Toggle(Tab::kDeveloper, "Logging",
                     "Writes what happens in each session to the logs folder, for reporting a problem. Off "
                     "writes no log at all and skips the timing measured only for it. From the next start.",
                     &Ng2Settings::logging, true, true, false));
  t.push_back(Toggle(Tab::kDeveloper, "Dump HUD pictures",
                     "Writes every HUD picture the game loads to logs/hud_pictures, once each. This is how "
                     "the original button pictures are found.",
                     &Ng2Settings::ps5_dump_hud_pictures, false, true, false));
#if !defined(__PROSPERO__)
  t.back().unavailable = [](const Ng2Settings&) { return std::string("On the PS5 only, for now."); };
#endif
#if defined(__PROSPERO__)  // (a PC: V-Sync on the Display tab)
  t.push_back(Toggle(Tab::kPerformance, "Frame pacing",
                     "Display refresh times the game's frames by the screen's own refresh, so none is shown "
                     "twice or skipped (on a 120 or 240 Hz screen, each the same number of times). Timer is the "
                     "runtime's 60 Hz clock, which drifts against the screen.",
                     &Ng2Settings::ps5_display_locked_vblank, true, false, true, "Timer", "Display refresh"));
#endif
  t.push_back(Toggle(Tab::kPerformance, "Input latency",
                     "Low shows each frame at the next refresh of the TV, about 30 ms sooner than Standard: "
                     "the game waits for the TV to take the last picture before it starts the next. Standard "
                     "lets two finished pictures queue ahead of the screen.",
                     &Ng2Settings::ps5_low_latency, true, false, true, "Standard", "Low"));
#if !defined(__PROSPERO__)
  // [IzunaRecomp] A PC: one picture waiting for the screen instead of two (the row above is the PS5's).
  t.pop_back();
  t.push_back(Toggle(Tab::kPerformance, "Input latency",
                     "Low paints each picture only once the one before it is on the screen, so at most one "
                     "waits for the screen instead of two: about a frame sooner. Standard keeps a picture of "
                     "slack, steadier if your PC sometimes misses a frame.",
                     &Ng2Settings::pc_low_latency, false, true, false, "Standard", "Low"));
#endif
  // [native renderer track] Developer options for the native renderer: each
  // logs or saves something specific, or switches one part for an A/B test.
  t.push_back(Ints(Tab::kDeveloper, "Native: save frames",
                   "Native renderer: saves the frame shown to native_frames/ in the app folder. Every 10 s / every "
                   "second keep the last 20 (the log says which is which). Every second, keep all: one file per "
                   "second named by frame number, none deleted - about 13 GB an hour of play. Bursts (flicker): "
                   "every 10 s, plus every frame for half a second once a minute (30 frames in a row, ~110 MB a "
                   "burst; frames the storage can't keep up with are skipped, the log says how many) - for "
                   "flickering. Copy the folder over with the log.",
                   &Ng2Settings::ps5_native_save_frames, {0, 600, 60, -60, -1},
                   {"Off", "Every 10 s", "Every second", "Every second, keep all", "Bursts (flicker)"}, 1, true,
                   false));
  t.push_back(Ints(Tab::kDeveloper, "Native: log draws",
                   "Native renderer: writes every draw of a frame to the log - shaders, textures, render state. "
                   "First 3D frames: the first two heavy frames of the run. Every minute: one heavy frame a "
                   "minute (bigger logs).",
                   &Ng2Settings::ps5_native_log_draws, {0, 1, 2}, {"Off", "First 3D frames", "Every minute"}, 1,
                   true, false));
  t.push_back(Toggle(Tab::kDeveloper, "Native: save textures",
                     "Native renderer: saves each texture once as it is first decoded, to native_textures/ in the "
                     "app folder, to check the decoding. Can be thousands of files: turn on for one short test.",
                     &Ng2Settings::ps5_native_save_textures, false, true, false));
  t.push_back(Toggle(Tab::kDeveloper, "Native: threaded recording",
                     "Native renderer: the game's own render thread only records what it draws, and a second "
                     "thread turns that into GPU work at the same time - the game thread is freed for the game "
                     "(faster, less input lag). Turn off if the picture breaks or the game crashes, and say so.",
                     &Ng2Settings::ps5_native_threaded, true, true, false));
  t.push_back(Toggle(Tab::kDeveloper, "Native: pipeline libraries",
                     "Native renderer: builds each shader once and links the GPU pipelines from those parts, so "
                     "a new effect or angle costs a link instead of a compile (less stutter). Turn off if the "
                     "picture breaks, and say so.",
                     &Ng2Settings::ps5_native_pipeline_libraries, true, true, false));
  t.push_back(Toggle(Tab::kDeveloper, "Native: save render passes",
                     "Native renderer: with 'log draws' on, also saves every image a logged frame renders "
                     "(shadows, reflections, glow, the final picture: each resolve) to native_resolves/ in the app "
                     "folder. About 100 MB a frame: copy the folder over with the log.",
                     &Ng2Settings::ps5_native_save_resolves, false, true, false));
  t.push_back(Toggle(Tab::kDeveloper, "Native: ignore pipeline cache",
                     "Native renderer: builds every shader pipeline afresh instead of loading the saved ones. "
                     "Slower to start; only if a stale cache is suspected.",
                     &Ng2Settings::ps5_native_fresh_pipelines, false, true, false));
  t.push_back(Ints(Tab::kDeveloper, "Frame report",
                   "Diagnostic only: captures two frames whole (every draw and image, to native_frames and "
                   "native_resolves) this long after the game starts. Or capture one from the in-game menu.",
                   &Ng2Settings::ps5_frame_report_after, {0, 30, 60, 120},
                   {"Off", "30 s after start", "60 s after start", "120 s after start"}, 0, true, false));
  return t;
}

}  // namespace

const std::vector<Setting>& Settings() {
  static const std::vector<Setting> table = Build();
  return table;
}

void ApplyPs5Defaults(Ng2Settings& s) {
  // What suits a PS5 on a TV, over the PC defaults: 1440p from the 720p base
  // with the exact (FSI) render targets - the combination that holds 60 fps
  // steadily in testing - PlayStation buttons, the texture cache the hardware
  // detection picks on PS5. Applied once; after that the player's choices
  // stand.
  s.ps5_resolution = 1;
  s.ps5_menu_music = true;
  s.ps5_low_latency = true;
  s.ps5_render_scale = 0;
  s.internal_720p = true;
  s.fps = 60;
  s.letterbox = true;
  s.ps5_playstation_prompts = true;
  s.configured = true;
  s.ps5_defaults_applied = true;
  s.ps5_defaults_version = kPs5DefaultsVersion;
}

void UpdatePs5Defaults(Ng2Settings& s) {
  if (!s.ps5_defaults_applied) {
    ApplyPs5Defaults(s);
    return;
  }
  if (s.ps5_defaults_version < 2) {
    // Round 2: 4K was the first default and hitches; 1440p with FSI is the
    // tested one. Moved once, then the player's choice stands again.
    s.ps5_resolution = 1;
    s.ps5_render_scale = 0;
    s.ps5_menu_music = true;  // (the hub has its theme from this round on)
  }
  // (The rounds run oldest first, so a later round's choice wins.)
  if (s.ps5_defaults_version < 3) {
    // Round 3: build 14 made Low input latency the default; Standard is again
    // until Low has been compared on the console.
    s.ps5_low_latency = false;
  }
  // (Round 4 set the emulated GPU's memory refresh, gone since test47.)
  // (Round 5 set the emulated GPU's asynchronous submission.)
  if (s.ps5_defaults_version < 6) {
    // Round 6: Low input latency (round 5, build 16.13) made jump timing
    // feel uneven: with two pictures a frame that misses a refresh waits for
    // the next one. Standard again; Low stays a choice.
    s.ps5_low_latency = false;
  }
  if (s.ps5_defaults_version < 7) {
    // Round 7 [IzunaRecomp]: Low is now the guest vblank waiting for the
    // display's pending flip (three images still), not two images: each
    // frame shows at the next refresh instead of ~2.5 refreshes on (test45).
    s.ps5_low_latency = true;
  }
  s.ps5_defaults_version = kPs5DefaultsVersion;
}

void ResetAll(Ng2Settings& s) {
  s = Ng2Settings{};
#if defined(__PROSPERO__)
  ApplyPs5Defaults(s);  // (a PC keeps the PC defaults)
#endif
  s.Clamp();
}

}  // namespace ng2ps5::hub
