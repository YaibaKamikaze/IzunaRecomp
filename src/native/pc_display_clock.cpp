// [IzunaRecomp] The display's own vblanks on Windows, for the guest vblank
// (izuna_gpu.cpp, VsyncThread; rex/ui/display_clock.h).
//
// On the PS5 the guest vblank follows VideoOut's vblanks, and the frames reach
// the screen once each, evenly. On a PC nothing told the timer when the
// screen refreshes - the swapchain trick the SDK has (an acquire that waited)
// only works while the swapchain is full, which a 60 fps game on a 120-165 Hz
// panel never is - so the guest ticked at 60 Hz on its own clock while the
// panel refreshed at its rate: frames held for 2 refreshes, then 3, unevenly
// (the "stutter" the PS5 had before its lock). Here a thread waits for each
// vblank of the monitor the game's window is on (DXGI, IDXGIOutput::
// WaitForVBlank - the output may belong to another GPU than the one drawing,
// as on a laptop whose panel hangs off the integrated GPU) and writes its time
// and the refresh period into display_clock.

#include "native/pc_display_clock.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/display_clock.h>

REXCVAR_DEFINE_BOOL(izuna_fullscreen_refresh_switch, true, "Native renderer",
                    "[IzunaRecomp] Fullscreen on a fixed-refresh screen whose rate 60 does not divide (144, 165, 170 "
                    "Hz): switch it to a multiple of 60 (120, 180, 240 Hz) so every frame shows equally long");

namespace izuna::pc {
namespace {

// [IzunaRecomp] test42: the refresh switch. A 60 fps game on a fixed 144-170
// Hz screen shows its frames 2, then 3 refreshes (issue #1: a 169.79 Hz
// monitor, "constant hiccups and bad frame pacing" with every frame on time).
// G-Sync / FreeSync shows them evenly; without it, in fullscreen, the monitor
// is switched to the best mode at the same size that 60 divides (240, 180,
// 120, else 60 Hz), with CDS_FULLSCREEN - Windows puts the desktop's back
// when the game exits - and back here when the window leaves fullscreen or
// the monitor.
struct RefreshSwitch {
  std::wstring device;  // the monitor switched ("\\.\DISPLAY1"), empty: none
  bool tried = false;   // (once per monitor: a refused switch is not retried)
};
RefreshSwitch g_switch;

bool CoversMonitor(HWND hwnd, HMONITOR monitor) {
  MONITORINFO mi = {sizeof(mi)};
  RECT r;
  if (!hwnd || !GetMonitorInfoW(monitor, &mi) || !GetWindowRect(hwnd, &r)) return false;
  return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
         r.bottom >= mi.rcMonitor.bottom;
}

// Whether a refresh rate shows a 60 fps game evenly (59.94-style rates count).
bool DividesBy60(double hz) {
  const double k = hz / 60.0;
  return k >= 0.95 && std::fabs(k - std::round(k)) < 0.02;
}

void RestoreRefresh() {
  if (g_switch.device.empty()) return;
  ChangeDisplaySettingsExW(g_switch.device.c_str(), nullptr, nullptr, 0, nullptr);
  REXLOG_INFO("[izuna] display: the monitor's own refresh rate restored");
  g_switch.device.clear();
}

// Returns true when the monitor was switched (the caller measures afresh).
bool SwitchRefresh(HMONITOR monitor, double hz) {
  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(monitor, &mi)) return false;
  DEVMODEW cur = {};
  cur.dmSize = sizeof(cur);
  if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur)) return false;
  DEVMODEW best = {};
  for (DWORD i = 0;; ++i) {
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, i, &dm)) break;
    if (dm.dmPelsWidth != cur.dmPelsWidth || dm.dmPelsHeight != cur.dmPelsHeight ||
        dm.dmBitsPerPel != cur.dmBitsPerPel || dm.dmDisplayFrequency > 245 ||
        !DividesBy60(double(dm.dmDisplayFrequency))) {
      continue;
    }
    if (dm.dmDisplayFrequency > best.dmDisplayFrequency) best = dm;
  }
  if (!best.dmDisplayFrequency) {
    REXLOG_INFO("[izuna] display: the monitor refreshes at {:.2f} Hz, which 60 fps does not divide, and offers no "
                "mode at {}x{} that does - G-Sync / FreeSync, or a 60 / 120 Hz desktop, shows the game evenly",
                hz, cur.dmPelsWidth, cur.dmPelsHeight);
    return false;
  }
  best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
  const LONG r = ChangeDisplaySettingsExW(mi.szDevice, &best, nullptr, CDS_FULLSCREEN, nullptr);
  if (r != DISP_CHANGE_SUCCESSFUL) {
    REXLOG_INFO("[izuna] display: switching the monitor to {} Hz was refused ({})", best.dmDisplayFrequency, r);
    return false;
  }
  g_switch.device = mi.szDevice;
  REXLOG_INFO("[izuna] display: the monitor refreshed at {:.2f} Hz, which 60 fps does not divide - switched it to {} "
              "Hz while the game is fullscreen (Settings > Display > Fullscreen refresh)",
              hz, best.dmDisplayFrequency);
  return true;
}

// The game's top-level window: the first visible one of this process.
HWND GameWindow() {
  struct Find {
    DWORD pid;
    HWND hwnd = nullptr;
  } find{GetCurrentProcessId()};
  EnumWindows(
      [](HWND hwnd, LPARAM p) -> BOOL {
        auto* f = reinterpret_cast<Find*>(p);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != f->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
        f->hwnd = hwnd;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&find));
  return find.hwnd;
}

// The DXGI output showing `monitor`, on whichever adapter drives it.
IDXGIOutput* OutputFor(HMONITOR monitor) {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return nullptr;
  IDXGIOutput* found = nullptr;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT a = 0; !found && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
    IDXGIOutput* output = nullptr;
    for (UINT o = 0; !found && adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
      DXGI_OUTPUT_DESC desc;
      if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
        found = output;
        // Which GPU drives the screen (a laptop's may be the integrated one,
        // with the frames drawn on the discrete one copied over to it).
        DXGI_ADAPTER_DESC1 ad;
        if (SUCCEEDED(adapter->GetDesc1(&ad))) {
          char name[128] = {};
          WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
          REXLOG_INFO("[izuna] display clock: the window's monitor is driven by {}", name);
        }
      } else {
        output->Release();
      }
    }
    adapter->Release();
  }
  factory->Release();
  return found;
}

std::atomic<bool> g_started{false};

void Run() {
  namespace dc = rex::ui::display_clock;
  HMONITOR monitor = nullptr;
  IDXGIOutput* output = nullptr;
  int64_t last = 0, checked = 0, logged_period = 0, measured = 0;
  int steady = 0, wobbles = 0;
  bool logged_variable = false;
  for (;;) {
    const int64_t now = dc::NowNs();
    // Which monitor the window is on: at the start and every 2 s (it can be moved).
    if (!output || now - checked > 2'000'000'000) {
      checked = now;
      HWND hwnd = GameWindow();
      HMONITOR m = MonitorFromWindow(hwnd ? hwnd : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
      // (left fullscreen, or another monitor: the switched one gets its rate back)
      if (!g_switch.device.empty() && (m != monitor || !CoversMonitor(hwnd, monitor) ||
                                       !REXCVAR_GET(izuna_fullscreen_refresh_switch))) {
        RestoreRefresh();
        g_switch.tried = false;
      }
      if (m != monitor || !output) {
        g_switch.tried = false;
        if (output) output->Release();
        output = OutputFor(m);
        monitor = m;
        last = 0;
        measured = 0;
        steady = 0;
        wobbles = 0;              // (another monitor: measured afresh)
        logged_variable = false;
        dc::period_ns.store(0, std::memory_order_relaxed);
        if (!output) {
          REXLOG_WARN("[izuna] display clock: no DXGI output for the window's monitor - the guest vblank runs on "
                      "its own timer");
          dc::external_source.store(false, std::memory_order_release);
          std::this_thread::sleep_for(std::chrono::seconds(2));
          continue;
        }
        dc::external_source.store(true, std::memory_order_release);
      }
    }
    if (FAILED(output->WaitForVBlank())) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    const int64_t t = dc::NowNs();
    const int64_t interval = last ? t - last : 0;
    last = t;
    // The period, measured here, is offered to the guest vblank only once it
    // has held for a second: a variable-refresh screen (G-Sync, FreeSync)
    // refreshes when a frame arrives, so locking the game to it chases its
    // own frames (test3's log: "60 Hz", then 72-126 Hz in the main menu).
    // Then the game keeps its own 60 Hz clock, which such a screen shows
    // evenly as it is. A late wake-up that skipped a refresh (an interval of
    // two or three periods) is not a change of rate.
    if (interval >= 4'000'000 && interval <= 70'000'000) {
      int64_t one = interval;
      if (measured) {
        const int64_t n = std::max<int64_t>(1, (interval + measured / 2) / measured);
        if (n > 1 && n <= 3 && std::llabs(interval - n * measured) * 25 < measured) one = interval / n;
      }
      if (one <= 20'000'000) {
        const bool agrees = measured && std::llabs(one - measured) * 33 < measured;  // (within 3%; not "near": a Windows macro)
        measured = measured ? measured + (one - measured) / 16 : one;
        steady = agrees ? steady + 1 : 0;
        if (!agrees && measured) wobbles = std::min(wobbles + 1, 1000);
      }
    }
    // A screen that has shown a variable refresh stays variable for the
    // session: test4's sat at a steady 120 Hz for a while (following the
    // game's 60 fps) and the lock that took it brought a 48 ms hitch.
    const bool trusted = steady >= 60 && !logged_variable;
    dc::last_vblank_ns.store(t, std::memory_order_release);
    dc::period_ns.store(trusted ? measured : 0, std::memory_order_relaxed);
    if (trusted && (!logged_period || std::llabs(measured - logged_period) * 50 > logged_period)) {
      logged_period = measured;
      REXLOG_INFO("[izuna] display clock: the window's monitor refreshes at {:.2f} Hz", 1e9 / double(measured));
    }
    if (trusted && !g_switch.tried && g_switch.device.empty() && REXCVAR_GET(izuna_fullscreen_refresh_switch) &&
        !DividesBy60(1e9 / double(measured)) && CoversMonitor(GameWindow(), monitor)) {
      g_switch.tried = true;
      if (SwitchRefresh(monitor, 1e9 / double(measured))) {
        last = 0;  // (the new rate, measured afresh)
        measured = 0;
        steady = 0;
        logged_period = 0;
        dc::period_ns.store(0, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::seconds(1));  // (the mode change settles)
        continue;
      }
    }
    if (!trusted && wobbles >= 30 && !logged_variable) {
      logged_variable = true;
      REXLOG_INFO("[izuna] display clock: the refresh rate keeps changing (a variable-refresh screen: G-Sync / "
                  "FreeSync) - the game's frames are paced by its own 60 Hz clock, which it shows evenly");
    }
  }
}

}  // namespace

void StartDisplayClock() {
  if (g_started.exchange(true)) return;
  std::thread(Run).detach();  // (lives as long as the app)
}

}  // namespace izuna::pc

#else

namespace izuna::pc {
void StartDisplayClock() {}
}  // namespace izuna::pc

#endif
