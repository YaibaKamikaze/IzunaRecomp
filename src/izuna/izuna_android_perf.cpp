// [IzunaRecomp] Android: where the CPU time goes. Every 15 seconds, the
// threads that used the most CPU in that time, with the core each last ran on
// - a Snapdragon has small, middle and big cores, and a game thread left on a
// small one runs at a fraction of the speed (the Android performance work,
// test56). Read from /proc/self/task/*/stat; logged only.
#include "izuna/izuna_android_perf.h"

#if defined(__ANDROID__)

#include <android/native_window.h>
#include <dirent.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>

namespace izuna::android_perf {
namespace {

struct Sample {
  std::string name;
  int cpu = -1;
  uint64_t ticks = 0;  // utime + stime
};

std::map<int, Sample> Read() {
  std::map<int, Sample> out;
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return out;
  while (dirent* e = readdir(dir)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    const int tid = std::atoi(e->d_name);
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    FILE* f = std::fopen(path, "r");
    if (!f) continue;
    char buf[1024] = {};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    if (!n) continue;
    // "tid (name) state ..." - the name can hold spaces: fields after the last ')'
    const char* open = std::strchr(buf, '(');
    const char* close = std::strrchr(buf, ')');
    if (!open || !close || close < open) continue;
    Sample s;
    s.name.assign(open + 1, close);
    std::vector<std::string> fields;
    const char* p = close + 2;
    while (*p) {
      const char* q = std::strchr(p, ' ');
      fields.emplace_back(p, q ? q - p : std::strlen(p));
      if (!q) break;
      p = q + 1;
    }
    // fields[0] is field 3 (state): utime 14, stime 15, processor 39
    if (fields.size() > 36) {
      s.ticks = std::strtoull(fields[11].c_str(), nullptr, 10) + std::strtoull(fields[12].c_str(), nullptr, 10);
      s.cpu = std::atoi(fields[36].c_str());
    }
    out[tid] = s;
  }
  closedir(dir);
  return out;
}

void Loop() {
  const long hz = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
  auto before = Read();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(15));
    auto now = Read();
    struct Row {
      int tid;
      double percent;
      const Sample* s;
    };
    std::vector<Row> rows;
    for (const auto& [tid, s] : now) {
      auto it = before.find(tid);
      const uint64_t used = s.ticks - (it != before.end() && it->second.ticks <= s.ticks ? it->second.ticks : 0);
      rows.push_back({tid, 100.0 * double(used) / double(hz) / 15.0, &s});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.percent > b.percent; });
    std::string line;
    for (size_t i = 0; i < rows.size() && i < 10; ++i)
      line += fmt::format("{}{} {:.0f}% (core {})", i ? ", " : "", rows[i].s->name, rows[i].percent, rows[i].s->cpu);
    REXLOG_INFO("[perf] busiest threads, % of one core over 15 s: {}", line);
    before = std::move(now);
  }
}

}  // namespace

void PaceWindow() {
  using clock = std::chrono::steady_clock;
  static clock::time_point last{};
  static void* last_window = nullptr;
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  void* native = nullptr;
  if (windows && count > 0)
    native = SDL_GetPointerProperty(SDL_GetWindowProperties(windows[0]), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
  SDL_free(windows);
  const auto now = clock::now();
  if (!native || (native == last_window && now - last < std::chrono::seconds(5))) return;
  // (API 30: looked up, the library's floor is 29)
  using SetFrameRate = int32_t (*)(ANativeWindow*, float, int8_t);
  static auto set = reinterpret_cast<SetFrameRate>(dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate"));
  if (!set) return;
  const int32_t rc = set(static_cast<ANativeWindow*>(native), 60.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
  if (native != last_window) REXLOG_INFO("[perf] the game's window paced at 60 frames a second ({})", rc);
  last_window = native;
  last = now;
}

// --- ADPF ---------------------------------------------------------------------
namespace {
struct HintApi {
  void* (*get_manager)() = nullptr;
  void* (*create_session)(void*, const int32_t*, size_t, int64_t) = nullptr;
  int (*report)(void*, int64_t) = nullptr;
};

// A thread's CPU time (Linux's per-thread CPU clock: any thread of the process).
int64_t ThreadCpuNs(int tid) {
  const clockid_t id = clockid_t((~unsigned(tid) << 3) | 6);  // (MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED))
  timespec ts{};
  if (clock_gettime(id, &ts) != 0) return -1;
  return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
}  // namespace

void FrameDone() {
  static HintApi api;
  static void* session = nullptr;
  static bool given_up = false;
  static uint64_t frames = 0;
  static std::vector<int> tids;
  static std::vector<int64_t> last_cpu;
  if (given_up) return;
  ++frames;
  if (!session) {
    if (frames < 180) return;  // (the game's threads all exist by then)
    given_up = true;
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return;
    api.get_manager = reinterpret_cast<void* (*)()>(dlsym(lib, "APerformanceHint_getManager"));
    api.create_session =
        reinterpret_cast<void* (*)(void*, const int32_t*, size_t, int64_t)>(dlsym(lib, "APerformanceHint_createSession"));
    api.report = reinterpret_cast<int (*)(void*, int64_t)>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    if (!api.get_manager || !api.create_session || !api.report) {
      REXLOG_INFO("[perf] no performance hints on this Android (API 33+)");
      return;
    }
    // The frame's threads: this one (the game's render thread), the game's
    // main thread, the renderer's draw worker.
    tids.push_back(int(gettid()));
    for (const auto& [tid, s] : Read()) {
      if (tid == tids[0]) continue;
      if (s.name.rfind("Main XThread", 0) == 0 || s.name == "Izuna draw") tids.push_back(tid);
    }
    void* manager = api.get_manager();
    std::vector<int32_t> ids(tids.begin(), tids.end());
    session = manager ? api.create_session(manager, ids.data(), ids.size(), 16'666'667) : nullptr;
    REXLOG_INFO("[perf] performance hint session for {} thread(s): {}", ids.size(), session ? "on" : "refused");
    if (!session) return;
    given_up = false;
    for (int tid : tids) last_cpu.push_back(ThreadCpuNs(tid));
    return;
  }
  // The busiest of the frame's threads, this frame: its CPU time.
  int64_t work = 0;
  for (size_t i = 0; i < tids.size(); ++i) {
    const int64_t now = ThreadCpuNs(tids[i]);
    if (now >= 0 && last_cpu[i] >= 0) work = std::max(work, now - last_cpu[i]);
    last_cpu[i] = now;
  }
  if (work > 0) api.report(session, work);
}

void Start() {
  static bool started = false;
  if (started) return;
  started = true;
  std::thread(Loop).detach();
  REXLOG_INFO("[perf] {} CPU cores; thread report every 15 s", sysconf(_SC_NPROCESSORS_CONF));
}

}  // namespace izuna::android_perf

#else

namespace izuna::android_perf {
void Start() {}
void PaceWindow() {}
void FrameDone() {}
}  // namespace izuna::android_perf

#endif
