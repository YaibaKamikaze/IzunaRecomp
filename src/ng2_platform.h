// Host OS bits: the folder picker, the foreground check, the monitors, the
// relaunch and the exit (the PS5's versions at the top of ng2_platform.cpp).
//
// The picker talks to IFileOpenDialog directly rather than SDL's dialog: the
// SDK's SDL is linked inside its runtime and does not export it.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ng2 {

// Win32-style filter: a display name and a ';'-separated pattern list.
struct FileFilter {
  const char* name;
  const char* pattern;  // e.g. "*.iso;*.img"
};

// A modal folder dialog: nullopt when the user cancels or it cannot be shown
// (a cancel is not an error and is not logged).
std::optional<std::filesystem::path> PickFolder(
    const std::string& title, const std::filesystem::path& start_in);

// Whether the window with focus belongs to this process. Used to keep guest
// input from being driven by a background application, without needing the
// window handle - which the SDK does not hand out.
bool ThisProcessIsForeground();

// An attached display: the space it can actually show a window in - its work
// area, so the taskbar is already subtracted - in PHYSICAL pixels, plus the
// scaling Windows applies to it.
//
// Both halves are needed together. The runtime's window size is LOGICAL, so a
// window asked for in physical pixels has to be divided by `scale` before it is
// handed over, and a display at 150% can only show a 2560-logical window across
// its 3840 physical pixels.
struct MonitorInfo {
  int index = 0;
  // The whole display - what a person calls its resolution, and what the menu
  // shows beside it.
  int full_width = 0;
  int full_height = 0;
  // What a window can actually occupy: the display minus the taskbar. Smaller
  // than the full size, and the reason a "4K" window opens 72 pixels short.
  int width = 0;
  int height = 0;
  float scale = 1.0f;   // 1.25 at 125%
  bool primary = false;
};

// Every attached display, ordered LEFT TO RIGHT by position. Not primary-first:
// that is what this did before, and it disagreed with the runtime's own display
// indices for every display that was not the primary. Empty if they cannot be
// enumerated, in which case nothing should be clamped - refusing to guess beats
// clamping to a number that came from nowhere.
std::vector<MonitorInfo> Monitors();

// The scaling Windows will ACTUALLY apply to this process's windows.
//
// Not the target monitor's: measured, the window comes out at the PRIMARY
// display's scale wherever it is put, which is what a System-DPI-aware process
// gets. Asking for a size converted with the 4K monitor's own 150% therefore
// produced a window a fifth too small on it.
float SystemScale();

// The PHYSICAL work area of one monitor, and its scaling. Falls back to the
// primary when the index is out of range. False if nothing was determined.
bool MonitorWorkArea(int index, int& width, int& height, float& scale);

// Starts another copy of this application and does NOT wait for it. Used by
// the game's own "Quit Game", which returns the player to the hub by
// relaunching - see Ng2App::ReturnToMenu for why it is a relaunch and not a
// teardown. Returns false if the process could not be started, in which case
// the caller should quit rather than pretend a menu is coming.
bool LaunchDetached(const std::filesystem::path& exe, const std::string& args);

// [ps5 port] Ends the process now, without running destructors - what
// std::_Exit did at every call site. On the PS5 a title must ask the system
// shell to close it: _Exit, exit() and returning from main all kill the
// process, and the console then reports a crash over a run that worked. On
// the PS5, LaunchDetached above reloads the app (into its hub) the same way
// and does not return when it succeeds.
[[noreturn]] void ExitNow(int code);

}  // namespace ng2
