// Ninja Gaiden II for PS5 - the hub's pages (hub_pages.h).

#include "ps5/hub/hub_pages.h"

#include <algorithm>
#include <cmath>

#include "ps5/hub/hub_backdrop.h"

namespace ng2ps5::hub {

namespace {

const char* const kHomeItems[] = {"START GAME", "SETTINGS", "MODS", "DOWNLOADABLE CONTENT", "SAVES", "ABOUT", "QUIT"};
constexpr int kHomeCount = 7;

// [IzunaRecomp] What differs between the PS5 and a PC in the pages' words.
#if defined(__PROSPERO__)
const char* const kQuitAsk = "Back to the PS5 home screen?";
const char* const kQuitWhat = "Back to the PS5 home screen.";
const char* const kRemoveFrom = "from the console";
const char* const kModsWhere = "copy it into the Mods folder in the app's folder (beside Game Data) over FTP";
const char* const kDlcWhere = "into /data/homebrew/PPSA94203/DLC/";
const char* const kSavesWhere = "/data/homebrew/PPSA94203/Saves Import/.";
const char* const kResetTo = "Put every setting back to the PS5 defaults?";
const char* const kPlatformName = "Ninja Gaiden II for PS5";
#else
const char* const kQuitAsk = "Close IzunaRecomp?";
const char* const kQuitWhat = "Close IzunaRecomp.";
const char* const kRemoveFrom = "from this PC";
const char* const kModsWhere = "copy it into the mods folder beside IzunaRecomp's .exe";
const char* const kDlcWhere = "into the DLC folder beside IzunaRecomp's .exe";
const char* const kSavesWhere = "the Saves Import folder beside IzunaRecomp's .exe.";
const char* const kResetTo = "Put every setting back to its default?";
const char* const kPlatformName = "Ninja Gaiden II for Windows";
#endif

const char* ResolutionName(int preset) {
  switch (preset) {
    case 1: return "1440p";
    case 2: return "720p";
    default: return "4K";
  }
}

}  // namespace

Hub::Hub(HubContext context) : ctx_(std::move(context)) {
  SetTab(0);
  ReloadMods();
}

void Hub::SetTab(int tab) {
  // [IzunaRecomp] Past a tab with no rows on this platform, in the direction moved.
  const int dir = tab < tab_ ? -1 : 1;
  tab_ = (tab + int(Tab::kCount)) % int(Tab::kCount);
  for (int k = 0; k < int(Tab::kCount) && !TabShown(Tab(tab_)); ++k)
    tab_ = (tab_ + dir + int(Tab::kCount)) % int(Tab::kCount);
  std::vector<const Setting*> rows;
  for (const Setting& s : Settings())
    if (int(s.tab) == tab_)
      rows.push_back(&s);
  list_.SetRows(std::move(rows));
}

void Hub::GoTo(Page page) {
  page_ = page;
  page_in_ = 0;
}

void Hub::Toast(std::string text) {
  toast_ = std::move(text);
  toast_time_ = 4.0f;
}

void Hub::Confirm(std::string title, std::string text, std::function<void()> yes) {
  confirm_title_ = std::move(title);
  confirm_text_ = std::move(text);
  confirm_yes_ = std::move(yes);
  confirm_open_ = true;
}

std::string Hub::StartCaption() const {
  const Ng2Settings& s = *ctx_.settings;
#if defined(__PROSPERO__)
  return std::string(ResolutionName(s.ps5_resolution)) + "  ·  " + std::to_string(s.fps) + " fps";
#else
  return (s.pc_resolution <= 0 ? std::string("Auto resolution") : std::to_string(s.pc_resolution) + "p") + "  ·  " +
         (s.fullscreen ? "fullscreen" : "windowed") + "  ·  " +
         std::to_string(s.fps) + " fps";
#endif
}

Hub::Result Hub::Draw(const PadState& pad_in, float dt, ImDrawList* dl, const ImVec2& display) {
  dt_ = std::min(dt, 0.1f);
  time_ += dt_;
  std::vector<Hit> last = std::move(c_.hits);  // [IzunaRecomp] (the mouse regions the last frame marked)
  c_.Begin(dl, display);
  c_.alpha = 1.0f - leave_;
  // The display outside the 16:9 canvas, if any, is plain black.
  dl->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 255));

  // Input stops the moment START GAME (or QUIT) is chosen: the fade is not
  // interruptible.
  PadState pad = leaving_ ? PadState{} : pad_in;
  if (!leaving_)
    ApplyMouse(ImGui::GetIO(), c_, last, pad, confirm_open_);  // [IzunaRecomp] (a PC's mouse)
  const bool modal = confirm_open_ && ConfirmOpen(pad);
  const PadState page_pad = modal ? PadState{} : pad;

  DrawBackdrop(c_, time_);
  Ease(page_in_, 1.0f, dt_, 10.0f);
  switch (page_) {
    case Page::kHome: Home(page_pad); break;
    case Page::kSettings: SettingsPage(page_pad); break;
    case Page::kMods: ModsPage(page_pad); break;
    case Page::kDlc: DlcPage(page_pad); break;
    case Page::kSaves: SavesPage(page_pad); break;
    case Page::kAbout: AboutPage(page_pad); break;
  }

  if (confirm_open_) {
    c_.Rect(0, 0, 1920, 1080, IM_COL32(0, 0, 0, 150));
    Panel(560, 380, 800, 300);
    c_.Embossed(GetFonts().menu, 44, 960, 410, colour::kIvory, confirm_title_, 0.5f, 0.0f);
    c_.Wrapped(GetFonts().body, 28, 620, 490, 680, colour::kIvoryDim, confirm_text_);
    c_.Hint(1340, 640, kPadCircle, "No");
    c_.Hint(1180, 640, kPadCross, "Yes");
  }

  if (toast_time_ > 0) {
    toast_time_ -= dt_;
    const float a = std::min(1.0f, toast_time_ * 2.0f);
    const float w = c_.TextWidth(GetFonts().body, 26, toast_) + 60;
    c_.Rect(960 - w / 2, 950, w, 52, IM_COL32(10, 10, 12, int(220 * a)), 8);
    c_.Text(GetFonts().body, 26, 960, 962, IM_COL32(236, 230, 214, int(255 * a)), toast_, 0.5f);
  }

  if (settings_dirty_) {
    ctx_.settings->Save();
    settings_dirty_ = false;
  }

  if (leaving_) {
    leave_ = std::min(1.0f, leave_ + dt_ / 0.6f);
    if (leave_ >= 1.0f)
      return quitting_ ? Result::kQuit : Result::kStart;
  }
  return Result::kNone;
}

bool Hub::ConfirmOpen(const PadState& pad) {
  if (pad.pressed & kPadCross) {
    confirm_open_ = false;
    PlaySound(Sound::kSelect);
    if (confirm_yes_)
      confirm_yes_();
  } else if (pad.pressed & kPadCircle) {
    confirm_open_ = false;
    PlaySound(Sound::kBack);
  }
  return true;
}

void Hub::Hints(std::initializer_list<std::pair<uint32_t, const char*>> hints) {
  float right = 1840;
  for (const auto& [button, label] : hints)
    right = c_.Hint(right, 1046, button, label);
}

void Hub::Header(const std::string& right, ImU32 right_col, const std::string& right_sub) {
  // Darker behind the header, so the logo and the rules read over the sky.
  c_.Gradient(0, 0, 1920, 270, IM_COL32(0, 0, 0, 190), IM_COL32(0, 0, 0, 70));
  // The rules: a short one before the plate, a long one after it, and the
  // full-width one under the header.
  c_.Line(40, 92, 128, 92, colour::kIvory, 3);
  c_.Line(776, 92, 1880, 92, colour::kIvory, 3);
  c_.Line(40, 254, 1880, 254, colour::kIvory, 3);
  // The logo plate: a bar pointing left, its tip on the short rule.
  const ImVec2 plate[5] = {ImVec2(196, 22), ImVec2(776, 22), ImVec2(776, 196), ImVec2(196, 196), ImVec2(134, 109)};
  c_.Poly({ImVec2(204, 30), ImVec2(784, 30), ImVec2(784, 204), ImVec2(204, 204), ImVec2(142, 117)},
          IM_COL32(0, 0, 0, 120));  // (its shadow)
  c_.Poly({plate[0], plate[1], plate[2], plate[3], plate[4]}, IM_COL32(12, 12, 14, 240));
  c_.PolyLine({plate[0], plate[1], plate[2], plate[3], plate[4]}, colour::kIvory, 3);
  c_.Line(214, 186, 760, 186, colour::kRedDeep, 3);
  DrawLogo(c_, 468, 40, 0.42f, time_);
  c_.Text(GetFonts().heading, 28, 468, 136, colour::kIvory, "The Izuna Project", 0.5f);
  // The page (or the state) on the right, as large slanted letters.
  c_.Slanted(GetFonts().menu, 60, 1880, 116, right_col, right, 1.0f, 0.18f);
  if (!right_sub.empty())
    c_.Text(GetFonts().body, 24, 1880, 196, colour::kIvoryDim, right_sub, 1.0f);
}

void Hub::PageTitle(const char* title, const std::string& subtitle) {
  Header(title, colour::kIvory, subtitle);
}

void Hub::Panel(float x, float y, float w, float h) {
  c_.Rect(x, y, w, h, colour::kPanel, 10);
  c_.Frame(x, y, w, h, IM_COL32(120, 20, 18, 160), 2, 10);
}

// --- Home --------------------------------------------------------------------
//
// Big slanted bands down the left, each a little shorter than the one above,
// pointing right; the focused one turns ivory and slides out. Under them a
// framed line says what the focused one does.

namespace {
const Icon kHomeIcons[] = {Icon::kPlay, Icon::kGear, Icon::kShuriken, Icon::kBox, Icon::kSave, Icon::kInfo, Icon::kPower};
}  // namespace

void Hub::Home(const PadState& pad) {
  const bool ready = ctx_.game.Ready();
  if (pad.pressed & kPadUp) {
    home_sel_ = (home_sel_ + kHomeCount - 1) % kHomeCount;
    wipe_ = 0;
    PlaySound(Sound::kMove);
  }
  if (pad.pressed & kPadDown) {
    home_sel_ = (home_sel_ + 1) % kHomeCount;
    wipe_ = 0;
    PlaySound(Sound::kMove);
  }
  const bool start = (pad.pressed & kPadOptions) || ((pad.pressed & kPadCross) && home_sel_ == 0);
  if (start) {
    if (!ready && ctx_.choose_game_folder) {
      // [IzunaRecomp] A PC: the game's folder is wherever the player keeps it.
      PlaySound(Sound::kSelect);
      ctx_.game = ctx_.choose_game_folder();
      if (ctx_.game.Ready()) {
        Toast(ctx_.game.message);
      } else if (ctx_.game.folder) {
        PlaySound(Sound::kError);
        Confirm("GAME DATA", ctx_.game.message, nullptr);
      }
    } else if (!ready) {
      PlaySound(Sound::kError);
      Confirm("GAME DATA", ctx_.game.message + "\n\nThe folder is /data/homebrew/PPSA94203/Game Data (or Ninja's Dawn PS5's, PPSA94202, if it is installed).", nullptr);
    } else {
      PlaySound(Sound::kStart);
      leaving_ = true;
    }
  } else if (pad.pressed & kPadCross) {
    PlaySound(Sound::kSelect);
    switch (home_sel_) {
      case 1: GoTo(Page::kSettings); break;
      case 2:
        ReloadMods();  // (folders copied in since)
        GoTo(Page::kMods);
        break;
      case 3: GoTo(Page::kDlc); break;
      case 4: GoTo(Page::kSaves); break;
      case 5: GoTo(Page::kAbout); break;
      case 6:
        Confirm("QUIT", kQuitAsk, [this] {
          quitting_ = true;
          leaving_ = true;
        });
        break;
      default: break;
    }
  }

  // The left of the scene darkened, under the bands.
  c_.dl->AddRectFilledMultiColor(c_.P(0, 254), c_.P(1300, 1080), c_.C(IM_COL32(0, 0, 0, 170)),
                                 c_.C(IM_COL32(0, 0, 0, 0)), c_.C(IM_COL32(0, 0, 0, 0)),
                                 c_.C(IM_COL32(0, 0, 0, 170)));
  Header(ready ? "Ready" : "No game data", ready ? colour::kIvory : colour::kWarn,
         ready ? StartCaption() + "   ·   " + ctx_.version : std::string("Game Data needs your copy of the game"));

  const float top = 288, h = 78, step = 89;
  Ease(wipe_, 1.0f, dt_, 6.0f);
  for (int i = 0; i < kHomeCount; ++i) {
    const bool on = i == home_sel_;
    Ease(home_slide_[i], on ? 1.0f : 0.0f, dt_, 14.0f);
    const float slide = home_slide_[i];
    const float y = top + float(i) * step;
    const float tip = 1010.0f - float(i) * 52.0f + slide * 40.0f;  // the band's right end, before its point
    const float point = h * 0.42f;
    const bool muted = i == 0 && !ready;
    // [IzunaRecomp] The mouse: over a band focuses it, a click chooses it.
    c_.Region(0, y, tip + point, h, kPadCross, [this, i] {
      if (home_sel_ != i) {
        home_sel_ = i;
        wipe_ = 0;
        PlaySound(Sound::kMove);
      }
    }, [this, i] { home_sel_ = i; });
    // The band's shadow, then the band: ink, or ivory when focused.
    c_.Poly({ImVec2(0, y + 8), ImVec2(tip + 10, y + 8), ImVec2(tip + 10 + point, y + h * 0.5f + 8),
             ImVec2(tip + 10, y + h + 8), ImVec2(0, y + h + 8)},
            IM_COL32(0, 0, 0, 110));
    const ImU32 fill = on ? IM_COL32(236, 230, 214, 250) : IM_COL32(10, 10, 12, 228);
    c_.Poly({ImVec2(0, y), ImVec2(tip, y), ImVec2(tip + point, y + h * 0.5f), ImVec2(tip, y + h), ImVec2(0, y + h)},
            fill);
    // A red edge along the point, and on the focused band a red underline.
    c_.PolyLine({ImVec2(tip, y), ImVec2(tip + point, y + h * 0.5f), ImVec2(tip, y + h)},
                on ? colour::kRed : IM_COL32(120, 20, 18, 220), on ? 5.0f : 3.0f, false);
    if (on)
      c_.Rect(0, y + h - 6, tip, 6, colour::kRed);
    // The icon on the left, the name right-aligned against the point.
    const ImU32 ink = on ? colour::kInk : (muted ? colour::kMuted : colour::kIvory);
    DrawIcon(c_, kHomeIcons[i], 96 + slide * 40.0f, y + h * 0.5f, 26, on ? colour::kRedDeep : ink);
    c_.Slanted(GetFonts().menu, 52, tip - 26, y + 9, ink, kHomeItems[i], 1.0f, 0.2f);
    if (on && wipe_ < 0.999f) {
      // The slash: a bright diagonal that crosses the band once on focus.
      const float x = 140 + wipe_ * (tip - 140);
      const float a = std::sin(wipe_ * 3.14159f);
      c_.Line(x - 22, y + h + 4, x + 22, y - 4, IM_COL32(214, 32, 28, int(230 * a)), 4);
    }
  }

  // What the focused entry does, in a framed box.
  static const char* const kWhat[] = {
      "Play Ninja Gaiden II. Your own copy of the game, in Game Data, is required.",
      "Display, graphics, game, textures, performance and developer settings.",
      "Mods from the Mods folder: turn them on or off, set their order, remove them.",
      "Costumes and challenge packs from your Xbox 360: installed when the game starts.",
      "Import Xbox 360 saves, and back up your saves and profile.",
      "The folders, credits and diagnostics.",
      kQuitWhat};
  std::string what = kWhat[home_sel_];
  ImU32 what_col = colour::kIvory;
  if (home_sel_ == 0 && !ready) {
    what = ctx_.game.message;
    what_col = colour::kWarn;
  }
  c_.Rect(250, 930, 1420, 76, IM_COL32(10, 10, 12, 215));
  c_.Frame(250, 930, 1420, 76, colour::kIvory, 3);
  c_.Fitted(GetFonts().bold, 30, 960, 950, 1360, what_col, what, 0.5f);

  Hints({{kPadCross, "Select"}, {kPadOptions, "Start game"}});
}

// --- Settings ----------------------------------------------------------------
//
// After the game's own pause menu: a chrome bar of tab plates with L1 / R1
// at its ends, a strip saying what the tab holds, the settings as plates in a
// column over a dark blade, the focused one's detail beside them, and an
// "Information" bar at the foot. Over the scene, a deep red tint.

void Hub::SettingsPage(const PadState& pad) {
  const bool capturing = list_.Capturing();  // [IzunaRecomp] (a key binding waiting: its keys are its own)
  if (!capturing && (pad.pressed & kPadCircle)) {
    PlaySound(Sound::kBack);
    GoTo(Page::kHome);
    return;
  }
  if (!capturing && (pad.pressed & kPadL1)) {
    SetTab(tab_ - 1);
    PlaySound(Sound::kTab);
  }
  if (!capturing && (pad.pressed & kPadR1)) {
    SetTab(tab_ + 1);
    PlaySound(Sound::kTab);
  }
  if (list_.Update(pad, *ctx_.settings, false))
    settings_dirty_ = true;

  // The red tint over the scene, darker at the edges.
  c_.Rect(0, 0, 1920, 1080, IM_COL32(48, 4, 6, 205));
  c_.Gradient(0, 0, 1920, 300, IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 0));
  c_.Gradient(0, 780, 1920, 300, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 170));
  c_.dl->AddRectFilledMultiColor(c_.P(0, 0), c_.P(420, 1080), c_.C(IM_COL32(0, 0, 0, 160)),
                                 c_.C(IM_COL32(0, 0, 0, 0)), c_.C(IM_COL32(0, 0, 0, 0)),
                                 c_.C(IM_COL32(0, 0, 0, 160)));

  // The chrome bar, then the tab plates on it.
  c_.Gradient(60, 44, 1800, 84, IM_COL32(150, 150, 158, 255), IM_COL32(40, 40, 46, 255));
  c_.Line(60, 46, 1860, 46, IM_COL32(230, 230, 236, 220), 2);
  c_.Line(60, 126, 1860, 126, IM_COL32(10, 10, 12, 255), 2);
  c_.Glyph(kPadL1, 112, 86, 20);
  c_.Glyph(kPadR1, 1808, 86, 20);
  {
    const Fonts& f = GetFonts();
    // [IzunaRecomp] The tabs this platform has, smaller when they would not
    // fit between the L1 and R1 glyphs.
    float fs = 34, pad_w = 64, total = 0;
    for (int t = 0; t < int(Tab::kCount); ++t)
      if (TabShown(Tab(t))) total += c_.TextWidth(f.heading, fs, TabName(Tab(t))) + pad_w + 12;
    if (total > 1560) {
      const float k = 1560 / total;
      fs *= k;
      pad_w *= k;
      total = 0;
      for (int t = 0; t < int(Tab::kCount); ++t)
        if (TabShown(Tab(t))) total += c_.TextWidth(f.heading, fs, TabName(Tab(t))) + pad_w + 12;
    }
    float x = 960 - total * 0.5f;
    for (int t = 0; t < int(Tab::kCount); ++t) {
      if (!TabShown(Tab(t)))
        continue;
      const char* name = TabName(Tab(t));
      const float w = c_.TextWidth(f.heading, fs, name) + pad_w;
      const bool on = t == tab_;
      const float y0 = on ? 50 : 56, y1 = on ? 122 : 116;
      if (on) {
        c_.Gradient(x, y0, w, y1 - y0, IM_COL32(220, 40, 34, 255), IM_COL32(110, 10, 12, 255));
      } else {
        c_.Gradient(x, y0, w, y1 - y0, IM_COL32(84, 84, 90, 255), IM_COL32(26, 26, 30, 255));
      }
      c_.Frame(x, y0, w, y1 - y0, on ? colour::kIvory : IM_COL32(200, 200, 206, 200), on ? 2.5f : 1.5f, 8);
      c_.Embossed(f.heading, fs, x + w * 0.5f, y0 + (y1 - y0) * 0.5f - fs * 0.65f,
                  on ? colour::kIvory : colour::kIvoryDim, name, 0.5f, on ? 0.6f : 0.0f);
      if (!on) {
        c_.Region(x, y0, w, y1 - y0, 0, {}, [this, t] {  // [IzunaRecomp] (a tab clicked)
          SetTab(t);
          PlaySound(Sound::kTab);
        });
      }
      x += w + 12;
    }
  }
  // The strip under the tabs: the page's name, then what the tab holds.
  c_.Rect(300, 144, 1320, 54, IM_COL32(8, 6, 6, 200));
  c_.Line(300, 144, 1620, 144, IM_COL32(150, 18, 16, 255), 2);
  c_.Line(300, 198, 1620, 198, IM_COL32(150, 18, 16, 255), 2);
  c_.Slanted(GetFonts().heading, 30, 330, 152, colour::kIvory, "SETTINGS", 0.0f, 0.18f);
  c_.Text(GetFonts().body, 26, 1590, 157, colour::kIvoryDim, TabBlurb(Tab(tab_)), 1.0f);

  // The blade behind the list: a long dark shape with chrome edges.
  c_.Poly({ImVec2(150, 214), ImVec2(330, 214), ImVec2(344, 820), ImVec2(240, 912), ImVec2(136, 820)},
          IM_COL32(14, 14, 16, 200));
  c_.PolyLine({ImVec2(150, 214), ImVec2(330, 214), ImVec2(344, 820), ImVec2(240, 912), ImVec2(136, 820)},
              IM_COL32(170, 170, 178, 160), 2);
  c_.Line(240, 232, 240, 890, IM_COL32(60, 60, 66, 200), 3);

  list_.DrawPlates(c_, *ctx_.settings, 80, 236, 900, 672, 1030, 830, dt_);

  // The information bar at the foot.
  c_.Rect(40, 930, 1840, 4, IM_COL32(150, 18, 16, 255));
  c_.Slanted(GetFonts().heading, 26, 70, 952, colour::kIvoryDim, "Information", 0.0f, 0.2f);
  c_.Text(GetFonts().body, 22, 70, 990, colour::kMuted,
          "Changes are saved at once; those marked to apply at the next start do so at START GAME.");
  const Setting* cur = list_.Current();
  if (capturing) {
    Hints({{kPadCircle, "Cancel"}});
  } else if (cur && cur->key >= 0) {
    Hints({{kPadCircle, "Back"}, {kPadSquare, "Default"}, {kPadCross, "Change key"}, {kPadL1, "Tab"}});
  } else {
    Hints({{kPadCircle, "Back"}, {kPadSquare, "Default"}, {kPadLeft | kPadRight, "Change"}, {kPadL1, "Tab"}});
  }
}

// --- Mods ----------------------------------------------------------------------
//
// The mods in Mods/ (izuna_mods.h) in their order, down the left; the focused
// one's details on the right. Each change is saved at once and takes effect
// at START GAME.

void Hub::ReloadMods() {
  mods_ = izuna::mods::List(&mod_options_);
  mods_sel_ = std::clamp(mods_sel_, 0, std::max(0, int(mods_.size()) - 1));
}

void Hub::SaveMods() {
  if (!izuna::mods::Save(mods_, mod_options_)) Toast("Could not save the mods' order.");
  ReloadMods();  // (the conflicts between them, again)
}

void Hub::ModsPage(const PadState& pad) {
  if (pad.pressed & kPadCircle) {
    PlaySound(Sound::kBack);
    GoTo(Page::kHome);
    return;
  }
  const int n = int(mods_.size());
  if (n && (pad.pressed & kPadUp)) mods_sel_ = (mods_sel_ + n - 1) % n, PlaySound(Sound::kMove);
  if (n && (pad.pressed & kPadDown)) mods_sel_ = (mods_sel_ + 1) % n, PlaySound(Sound::kMove);
  if (n && (pad.pressed & kPadCross)) {
    mods_[size_t(mods_sel_)].enabled = !mods_[size_t(mods_sel_)].enabled;
    PlaySound(Sound::kChange);
    SaveMods();
  }
  // L1 / R1: the focused mod up or down the order (up wins).
  if (n > 1 && (pad.pressed & kPadL1) && mods_sel_ > 0) {
    std::swap(mods_[size_t(mods_sel_)], mods_[size_t(mods_sel_ - 1)]);
    --mods_sel_;
    PlaySound(Sound::kTab);
    SaveMods();
  }
  if (n > 1 && (pad.pressed & kPadR1) && mods_sel_ < n - 1) {
    std::swap(mods_[size_t(mods_sel_)], mods_[size_t(mods_sel_ + 1)]);
    ++mods_sel_;
    PlaySound(Sound::kTab);
    SaveMods();
  }
  if (pad.pressed & kPadSquare) {
    mod_options_.log_file_loads = !mod_options_.log_file_loads;
    PlaySound(Sound::kChange);
    SaveMods();
    Toast(mod_options_.log_file_loads ? "Every file the game loads will be in the log, by name."
                                      : "The game's file loads are no longer logged.");
  }
  if (pad.pressed & kPadR2) {
    mod_options_.dump_textures = !mod_options_.dump_textures;
    PlaySound(Sound::kChange);
    SaveMods();
    Toast(mod_options_.dump_textures ? "Each texture the game draws will be saved to Texture Dump/, by its id."
                                     : "Textures are no longer saved.");
  }
  if (n && (pad.pressed & kPadTriangle)) {
    const izuna::mods::Mod mod = mods_[size_t(mods_sel_)];
    Confirm("REMOVE MOD", "Delete " + mod.name + " (the folder Mods/" + mod.folder + ") " + kRemoveFrom + "?",
            [this, mod] {
              std::string error;
              if (izuna::mods::Remove(mod, &error)) {
                Toast(mod.name + " removed.");
              } else {
                Toast("Could not remove it: " + error);
              }
              ReloadMods();
            });
  }

  int on = 0;
  for (const auto& m : mods_) on += m.enabled ? 1 : 0;
  PageTitle("MODS", std::to_string(n) + " mod(s), " + std::to_string(on) +
                        " on. Changes take effect at START GAME.");
  const Fonts& f = GetFonts();

  // The list.
  Panel(80, 286, 860, 620);
  if (mods_.empty()) {
    c_.Wrapped(f.body, 26, 120, 320, 780, colour::kIvoryDim,
               std::string("No mods yet. Each mod is a folder: ") + kModsWhere +
                   ", then come back here. A mod's folder holds a mod.toml (its name and "
               "details) and a files folder with the game files it replaces, by their path in the game - "
               "files/char/ryu/ryu.gmd replaces Ryu's model.");
  }
  const float row_h = 64, list_top = 306, list_h = 580;
  const float want = std::clamp(float(mods_sel_) * row_h - list_h * 0.5f + row_h, 0.0f,
                                std::max(0.0f, float(n) * row_h - list_h));
  Ease(mods_scroll_, want, dt_, 12.0f);
  for (int i = 0; i < n; ++i) {
    const float y = list_top + float(i) * row_h - mods_scroll_;
    if (y < list_top - 4 || y > list_top + list_h - row_h + 4) continue;
    const auto& m = mods_[size_t(i)];
    if (i == mods_sel_) c_.Rect(96, y - 6, 828, row_h - 6, IM_COL32(150, 18, 16, 110));
    // [IzunaRecomp] The mouse: a row focuses; its On / Off switches it.
    c_.Region(96, y - 6, 700, row_h - 6, 0, [this, i] { mods_sel_ = i; }, [this, i] { mods_sel_ = i; });
    c_.Region(796, y - 6, 128, row_h - 6, kPadCross, {}, [this, i] { mods_sel_ = i; });
    c_.Text(f.body, 22, 116, y + 8, colour::kMuted, std::to_string(i + 1));
    c_.Fitted(f.bold, 28, 160, y, 600, m.enabled ? colour::kIvory : colour::kMuted, m.name);
    const bool trouble = m.enabled && !m.notes.empty();
    c_.Text(f.body, 26, 900, y + 2, m.enabled ? (trouble ? colour::kWarn : colour::kGood) : colour::kMuted,
            m.enabled ? "On" : "Off", 1.0f);
  }

  // The focused mod's details.
  Panel(960, 286, 880, 620);
  if (n) {
    const auto& m = mods_[size_t(mods_sel_)];
    float y = 312;
    y += c_.Wrapped(f.heading, 34, 990, y, 820, colour::kIvory, m.name) + 4;
    std::string by;
    if (!m.author.empty()) by = "by " + m.author;
    if (!m.version.empty()) by += (by.empty() ? "version " : "  ·  version ") + m.version;
    if (!by.empty()) y += c_.Wrapped(f.body, 24, 990, y, 820, colour::kIvoryDim, by) + 8;
    if (!m.description.empty()) y += c_.Wrapped(f.body, 24, 990, y, 820, colour::kIvory, m.description) + 12;
    if (!m.manifest) y += c_.Wrapped(f.body, 22, 990, y, 820, colour::kMuted, "No mod.toml: named after its folder.") + 8;
    const size_t count = m.files.size() + m.replace.size();
    if (count) y += c_.Wrapped(f.heading, 24, 990, y, 820, colour::kIvoryDim,
                    "REPLACES " + std::to_string(count) + " GAME FILE" + (count == 1 ? "" : "S")) + 4;
    size_t shown = 0;
    for (const auto& file : m.files) {
      if (shown == 6 || y > 820) break;
      y += c_.Wrapped(f.body, 22, 1010, y, 800, colour::kIvoryDim, file) + 2;
      ++shown;
    }
    for (const auto& [target, source] : m.replace) {
      if (shown == 6 || y > 820) break;
      y += c_.Wrapped(f.body, 22, 1010, y, 800, colour::kIvoryDim, target + "  <-  " + source) + 2;
      ++shown;
    }
    if (count > shown) y += c_.Wrapped(f.body, 22, 1010, y, 800, colour::kMuted, "and " + std::to_string(count - shown) + " more") + 2;
    if (!m.script.empty()) {
      y += 8;
      y += c_.Wrapped(f.heading, 24, 990, y, 820, colour::kIvoryDim, "SCRIPT") + 4;
      y += c_.Wrapped(f.body, 22, 1010, y, 800, colour::kIvoryDim, m.script) + 2;
    }
    if (!m.textures.empty() || !m.hide_shaders.empty()) {
      y += 8;
      std::string what;
      if (!m.textures.empty()) what = std::to_string(m.textures.size()) + " texture(s) replaced";
      if (!m.hide_shaders.empty()) {
        what += (what.empty() ? "" : ",  ") + std::to_string(m.hide_shaders.size()) + " shader(s) hidden";
      }
      y += c_.Wrapped(f.heading, 24, 990, y, 820, colour::kIvoryDim, "DRAWING") + 4;
      y += c_.Wrapped(f.body, 22, 1010, y, 800, colour::kIvoryDim, what) + 2;
    }
    y += 10;
    for (const auto& note : m.notes) {
      if (y > 860) break;
      y += c_.Wrapped(f.body, 22, 990, y, 820, colour::kWarn, note) + 4;
    }
  }

  c_.Text(f.body, 22, 80, 930, colour::kMuted,
          std::string("The first mod in the list wins a file two mods both replace.    Log file loads: ") +
              (mod_options_.log_file_loads ? "on" : "off") + "    Save textures: " +
              (mod_options_.dump_textures ? "on" : "off"));
  Hints({{kPadCircle, "Back"},
         {kPadTriangle, "Remove"},
         {kPadSquare, "Log file loads"},
         {kPadR2, "Save textures"},
         {kPadL1, "Move up"},
         {kPadR1, "Move down"},
         {kPadCross, "On / off"}});
}

// --- Downloadable content ------------------------------------------------------

void Hub::DlcPage(const PadState& pad) {
  if (pad.pressed & kPadCircle) {
    PlaySound(Sound::kBack);
    GoTo(Page::kHome);
    return;
  }
  const int n = int(ctx_.dlc.size());
  if (n && (pad.pressed & kPadUp)) dlc_sel_ = (dlc_sel_ + n - 1) % n, PlaySound(Sound::kMove);
  if (n && (pad.pressed & kPadDown)) dlc_sel_ = (dlc_sel_ + 1) % n, PlaySound(Sound::kMove);

  int valid = 0;
  for (const auto& d : ctx_.dlc) valid += d.valid ? 1 : 0;
  PageTitle("DOWNLOADABLE CONTENT", std::to_string(valid) + " package(s) in the DLC folder. They are installed when "
                                                           "you press START GAME.");
  Panel(340, 286, 1240, 714);
  if (ctx_.dlc.empty()) {
    c_.Wrapped(GetFonts().body, 28, 400, 330, 1120, colour::kIvoryDim,
               std::string("No packages yet. Copy the DLC files from your Xbox 360 (Content/<profile>/544307D5/00000002/) ") +
                   kDlcWhere + " - as they are, without extensions.");
  }
  for (int i = 0; i < n; ++i) {
    const float y = 316 + float(i) * 60;
    if (y > 880)
      break;
    const auto& d = ctx_.dlc[size_t(i)];
    c_.Region(360, y - 8, 1200, 56, 0, [this, i] { dlc_sel_ = i; }, [this, i] { dlc_sel_ = i; });  // [IzunaRecomp]
    if (i == dlc_sel_)
      c_.Rect(360, y - 8, 1200, 56, IM_COL32(150, 18, 16, 110));
    c_.Text(GetFonts().bold, 28, 390, y, colour::kIvory, d.name);
    c_.Text(GetFonts().body, 26, 1530, y, d.valid ? colour::kGood : colour::kWarn,
            d.valid ? "ready" : "not a DLC package", 1.0f);
  }
  c_.Wrapped(GetFonts().body, 22, 380, 936, 1160, colour::kMuted,
             "Mission Mode needs the game's title update, which this version does not load yet. The other "
             "packs (costumes, Fiend Challenges) work without it.");
  Hints({{kPadCircle, "Back"}});
}

// --- Saves ---------------------------------------------------------------------

void Hub::SavesPage(const PadState& pad) {
  if (pad.pressed & kPadCircle) {
    PlaySound(Sound::kBack);
    GoTo(Page::kHome);
    return;
  }
  // Row 0: import on START GAME; row 1: back up now.
  if (pad.pressed & kPadUp) saves_sel_ = (saves_sel_ + 1) % 2, PlaySound(Sound::kMove);
  if (pad.pressed & kPadDown) saves_sel_ = (saves_sel_ + 1) % 2, PlaySound(Sound::kMove);
  int usable = 0;
  for (const auto& s : ctx_.saves) usable += s.usable ? 1 : 0;
  if (pad.pressed & (kPadCross | kPadLeft | kPadRight)) {
    if (saves_sel_ == 0) {
      if (usable == 0) {
        PlaySound(Sound::kError);
        Toast("No Ninja Gaiden II saves in the Saves Import folder.");
      } else {
        import_saves_ = !import_saves_;
        PlaySound(Sound::kChange);
      }
    } else if (pad.pressed & kPadCross) {
      PlaySound(Sound::kSelect);
      Toast(ctx_.backup ? ctx_.backup() : std::string());
    }
  }

#if defined(__PROSPERO__)
  PageTitle("SAVES", "Your saves and profile are in the app's UserData folder.");
#else
  PageTitle("SAVES", "Your saves and profile are in the user folder beside ng2.exe.");
#endif
  Panel(340, 286, 1240, 714);
  const char* rows[2] = {"Import these saves on START GAME", "Back up UserData now"};
  for (int i = 0; i < 2; ++i) {
    const float y = 316 + float(i) * 64;
    c_.Region(360, y - 8, 1200, 58, kPadCross, [this, i] { saves_sel_ = i; }, [this, i] { saves_sel_ = i; });  // [IzunaRecomp]
    if (i == saves_sel_)
      c_.Rect(360, y - 8, 1200, 58, IM_COL32(150, 18, 16, 110));
    c_.Text(GetFonts().bold, 30, 390, y, colour::kIvory, rows[i]);
    if (i == 0)
      c_.Text(GetFonts().body, 30, 1530, y, colour::kIvory, import_saves_ ? "Yes" : "No", 1.0f);
  }
  c_.Text(GetFonts().heading, 28, 390, 476, colour::kIvoryDim, "IN SAVES IMPORT");
  if (ctx_.saves.empty())
    c_.Wrapped(GetFonts().body, 26, 390, 526, 1140, colour::kMuted,
               std::string("Empty. Copy Xbox 360 save packages (Content/<profile>/544307D5/00000001/) into ") +
                   kSavesWhere);
  for (size_t i = 0; i < ctx_.saves.size(); ++i) {
    const float y = 526 + float(i) * 46;
    if (y > 940)
      break;
    const auto& s = ctx_.saves[i];
    c_.Text(GetFonts().body, 26, 390, y, s.usable ? colour::kIvory : colour::kMuted, s.name);
    c_.Text(GetFonts().body, 22, 1530, y + 3, s.usable ? colour::kGood : colour::kWarn, s.message, 1.0f);
  }
  Hints({{kPadCircle, "Back"}, {kPadCross, "Select"}});
}

// --- About ---------------------------------------------------------------------

void Hub::AboutPage(const PadState& pad) {
  if (pad.pressed & kPadCircle) {
    PlaySound(Sound::kBack);
    GoTo(Page::kHome);
    return;
  }
  if (pad.pressed & kPadCross) {
    PlaySound(Sound::kSelect);
    Toast(ctx_.write_diagnostics ? ctx_.write_diagnostics() : std::string());
  }
  if (pad.pressed & kPadSquare) {
    Confirm("RESET SETTINGS", kResetTo, [this] {
      ResetAll(*ctx_.settings);
      settings_dirty_ = true;
      Toast("Settings reset.");
    });
  }

  PageTitle("ABOUT", std::string("The Izuna Project  ·  ") + kPlatformName + "  " + ctx_.version);
  Panel(340, 286, 1240, 714);
  float y = 316;
  auto line = [&](ImFont* f, float size, ImU32 col, const std::string& t) {
    y += c_.Wrapped(f, size, 390, y, 1140, col, t) + 6;
  };
  const Fonts& f = GetFonts();
  line(f.heading, 28, colour::kIvory, "FOLDERS");
  line(f.body, 24, ctx_.game.Ready() ? colour::kGood : colour::kWarn, "Game Data: " + ctx_.game.message);
  line(f.body, 24, colour::kIvoryDim, "DLC: " + std::to_string(ctx_.dlc.size()) + " file(s)    Saves Import: " +
                                          std::to_string(ctx_.saves.size()) + " file(s)    Mods: " +
                                          std::to_string(mods_.size()) + " folder(s)");
  y += 14;
  line(f.heading, 28, colour::kIvory, "CREDITS");
  line(f.body, 22, colour::kIvoryDim,
       "Ninja's Dawn, the Ninja Gaiden II recompilation, by TheSaltTrader (MIT). ReXGlue and Xenia "
       "(BSD). The PS5 runtime by elripalda (SvR 2011 and Skate 3 PS5 ports). ps5-plume-triangle and "
       "UnleashedRecomp-Prospero by Nazky. PS5_Vulkan, PS5_Mesa and the payload SDK fork by mihawk-99; "
       "the payload SDK by John Törnblom. The hub's design draws on ps5-homebrew-ui by BlackBearReloaded; "
       "its menus on Project M's launcher and the game's own pause menu. The native renderer follows Edge "
       "of Time Recompiled and the WWE All Stars recomp (BSD-3). Fonts: Cinzel and Inter (SIL Open Font "
       "License).");
  line(f.body, 22, colour::kIvoryDim,
       "No game files ship with this app: it runs from the copy of the game you put in Game Data. Ninja "
       "Gaiden is a trademark of Koei Tecmo; this project is not affiliated with them.");
  y += 14;
  line(f.body, 22, colour::kMuted, "Write diagnostics puts the log, settings and system details in logs/ for a "
                                   "report.");
  Hints({{kPadCircle, "Back"}, {kPadSquare, "Reset settings"}, {kPadCross, "Write diagnostics"}});
}

}  // namespace ng2ps5::hub
