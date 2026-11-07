// Hooks that watch the game without changing it (config/hooks/diagnostics.toml):
// every file it opens - the chapter being loaded, and the chapter-start
// auto-skip's arming - and once per game frame, the frame-rate report and the
// HUD's FPS readout.
//
// (The main port's investigation tools that lived here - the guest profiler,
// the write trap, the oscillation scan, the projection-matrix hunt, the
// RenderDoc trigger, the plane dumps and the video injector - and the init
// crash's parser hooks were removed in the clean-up after izuna-windows-test22.)

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <string>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include "ng2_autoskip.h"
#include "ng2_chapter.h"
#include "ng2_perf.h"

// The NG2FIX-SCANGUARD crash patch (local/diag/patch_scanguard.py, re-applied
// to generated/ after every codegen) reports each guarded call through this:
// ids >= 10 carry a per-frame value and log only on a change; lower ids are
// rare events and log every time.
extern "C" void ng2_diag_xtrace(int id, unsigned val) {
  if (id < 0 || id >= 128) return;
  if (id >= 10) {
    static unsigned last[128];
    static bool seen[128];
    if (seen[id] && last[id] == val) return;
    seen[id] = true;
    last[id] = val;
  }
  REXLOG_INFO("[xtrace] pt {} = 0x{:08X}", id, val);
}

namespace {

// A NUL-terminated guest string, bounded; empty for a null or unmapped one.
std::string ReadGuestString(uint32_t va, size_t limit = 128) {
  std::string out;
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory || va < 0x1000) return out;
  const auto* p = memory->TranslateVirtual<const char*>(va);
  if (!p) return out;
  for (size_t i = 0; i < limit && p[i]; ++i) out.push_back(p[i]);
  return out;
}

// Which chapter is loading, from the file the game opens for it. Loading
// chapter N opens s_chap_NN.ng2 - and in the same millisecond enumerates every
// other chapter file, so the answer is the FIRST file of a burst (taking the
// last reported chapter 14 every time).
std::atomic<int> g_current_chapter{0};

void NoteChapterFile(const std::string& path) {
  const auto at = path.find("s_chap_");
  if (at == std::string::npos || at + 9 > path.size()) return;
  if (!std::isdigit(static_cast<unsigned char>(path[at + 7])) ||
      !std::isdigit(static_cast<unsigned char>(path[at + 8])))
    return;
  const int chapter = (path[at + 7] - '0') * 10 + (path[at + 8] - '0');

  using clock = std::chrono::steady_clock;
  static clock::time_point last{};
  const auto now = clock::now();
  const bool new_burst = last == clock::time_point{} || now - last > std::chrono::seconds(1);
  last = now;
  if (!new_burst) return;  // (the enumeration that follows the real load)

  if (g_current_chapter.exchange(chapter) != chapter)
    REXLOG_INFO("[diag] chapter {} is loading (from {})", chapter, path);
}

// The median, 99th percentile and worst frame over 5 s windows, and how many
// frames took more than twice the median (a hitch is one long frame, not a
// lower average).
void ReportFrameRate() {
  if (!rex::LoggingEnabled()) return;  // (logging off: nothing to report it to)
  using clock = std::chrono::steady_clock;
  static clock::time_point last_frame_at = clock::now();
  static clock::time_point window_start = last_frame_at;
  static std::array<float, 1536> ms{};  // (fixed: per frame, no allocation; 5 s at 300 fps)
  static size_t count = 0;

  const auto now = clock::now();
  const float dt = std::chrono::duration<float, std::milli>(now - last_frame_at).count();
  last_frame_at = now;
  if (count < ms.size()) ms[count++] = dt;

  const double secs = std::chrono::duration<double>(now - window_start).count();
  if (secs < 5.0) return;
  window_start = now;
  if (count < 2) {
    count = 0;
    return;
  }
  std::array<float, 1536> sorted = ms;
  std::sort(sorted.begin(), sorted.begin() + count);
  const float p50 = sorted[count / 2], p99 = sorted[(count * 99) / 100], worst = sorted[count - 1];
  int hitches = 0;
  for (size_t i = 0; i < count; ++i)
    if (ms[i] > p50 * 2.0f) ++hitches;
  REXLOG_INFO("[diag] {:.1f} fps ({} frames in {:.1f}s)  frame ms: p50 {:.1f}  p99 {:.1f}  worst {:.1f}  hitches {}",
              double(count) / secs, count, secs, p50, p99, worst, hitches);
  count = 0;
}

}  // namespace

namespace ng2 {
int CurrentChapter() { return g_current_chapter.load(std::memory_order_relaxed); }
}  // namespace ng2

// (The generated code declares these hooks with C++ linkage.)
void ng2DiagFileOpen(PPCRegister& r3) {
  static int opened = 0;
  const std::string path = ReadGuestString(r3.u32);
  REXLOG_INFO("[diag] open #{}: {}", ++opened, path);
  // A chapter's story data opening IS the chapter starting: arm the cinematic
  // auto-skip, and note the chapter (ng2_chapter.h).
  if (path.find("s_chap_") != std::string::npos) {
    ng2::ArmAutoSkip();
    NoteChapterFile(path);
  }
}

void ng2DiagFrameTick() {
  // The HUD's FPS from the game's own frame (sub_822F3A88, once per frame),
  // not the presenter's - on a high-refresh screen those differ.
  ng2::PerfFrameTick();
  ng2::PollCutsceneSkip();  // [IzunaRecomp] test44: every cutscene skippable
  ReportFrameRate();
}
