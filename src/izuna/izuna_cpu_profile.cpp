// [IzunaRecomp] A sampling CPU profiler (Linux, Android): where the game's
// threads spend their time, on the players' own devices.
//
// The process's CPU-time timer (ITIMER_PROF) sends SIGPROF about every
// millisecond of CPU used, to the thread using it; the handler stores that
// thread's id and program counter (and, on ARM64, its return address) in a
// ring. Every 15 seconds a thread counts the samples and logs, per busy
// thread, the hottest program counters as offsets into the program
// ("[cpuprof] ..."), with the program's build id - turned into function names
// afterwards against the unstripped build (llvm-symbolizer --obj=<ng2 or
// libmain.so> 0xOFFSET): no symbols ship, nothing is looked up on the device.
// The cvar izuna_cpu_profile (default on in test builds) switches it.
#include "izuna/izuna_cpu_profile.h"

#if defined(__linux__) && !defined(__PROSPERO__)

#include <dlfcn.h>
#include <link.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>

namespace izuna::cpu_profile {
namespace {

struct Sample {
  uint32_t tid;
  uintptr_t pc;
  uintptr_t lr;   // [IzunaRecomp] test69: the first return address in the program (ARM64: LR, then the frame chain)
  uintptr_t lr2;  // (the program frame above that one, if found)
};

constexpr size_t kRing = 1 << 17;
Sample g_ring[kRing];
std::atomic<uint64_t> g_head{0};
uintptr_t g_base = 0, g_end = 0;  // the program (or libmain.so) in memory
std::string g_build_id;

void OnProf(int, siginfo_t*, void* context) {
  const int saved_errno = errno;
  auto* uc = static_cast<ucontext_t*>(context);
  Sample s;
  s.tid = uint32_t(syscall(SYS_gettid));
#if defined(__aarch64__)
  s.pc = uintptr_t(uc->uc_mcontext.pc);
  s.lr = uintptr_t(uc->uc_mcontext.regs[30]);
  // (a library function that is not a leaf - libc's clock_gettime around the
  // vDSO's, SDL_Delay - has the program further up: the frame records [x29] =
  // the caller's x29, [x29 + 8] = its return address; followed only within
  // the 1 MB above the stack pointer, so a stray x29 is never read)
  if (s.pc < g_base || s.pc >= g_end) {
    const uintptr_t sp = uintptr_t(uc->uc_mcontext.sp);
    uintptr_t fp = uintptr_t(uc->uc_mcontext.regs[29]);
    uintptr_t found = (s.lr >= g_base && s.lr < g_end) ? s.lr : 0, found2 = 0;
    for (int i = 0; i < 12 && !found2 && fp >= sp && fp < sp + (1u << 20) && !(fp & 15); ++i) {
      const uintptr_t ret = reinterpret_cast<const uintptr_t*>(fp)[1];
      const uintptr_t next = reinterpret_cast<const uintptr_t*>(fp)[0];
      if (ret >= g_base && ret < g_end && ret != found) {
        if (!found) found = ret;
        else found2 = ret;
      }
      if (next <= fp) break;
      fp = next;
    }
    if (found) s.lr = found;
    s.lr2 = found2;
  }
#elif defined(__x86_64__)
  s.pc = uintptr_t(uc->uc_mcontext.gregs[REG_RIP]);
  s.lr = s.lr2 = 0;
#else
  s.pc = s.lr = s.lr2 = 0;
#endif
  const uint64_t i = g_head.fetch_add(1, std::memory_order_relaxed);
  g_ring[i & (kRing - 1)] = s;
  errno = saved_errno;
}

// Where this code is loaded, and its GNU build id (to match the build).
void FindProgram() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(&FindProgram), &info)) return;
  g_base = uintptr_t(info.dli_fbase);
  dl_iterate_phdr(
      [](dl_phdr_info* p, size_t, void*) -> int {
        if (p->dlpi_addr != g_base) return 0;
        for (int i = 0; i < p->dlpi_phnum; ++i) {
          const ElfW(Phdr)& ph = p->dlpi_phdr[i];
          if (ph.p_type == PT_LOAD) g_end = std::max<uintptr_t>(g_end, p->dlpi_addr + ph.p_vaddr + ph.p_memsz);
          if (ph.p_type == PT_NOTE) {
            const uint8_t* n = reinterpret_cast<const uint8_t*>(p->dlpi_addr + ph.p_vaddr);
            const uint8_t* end = n + ph.p_memsz;
            while (n + 12 <= end) {
              uint32_t namesz, descsz, type;
              std::memcpy(&namesz, n, 4);
              std::memcpy(&descsz, n + 4, 4);
              std::memcpy(&type, n + 8, 4);
              const uint8_t* desc = n + 12 + ((namesz + 3) & ~3u);
              if (type == 3 /* NT_GNU_BUILD_ID */ && desc + descsz <= end) {
                for (uint32_t k = 0; k < descsz; ++k) g_build_id += fmt::format("{:02x}", desc[k]);
              }
              n = desc + ((descsz + 3) & ~3u);
            }
          }
        }
        return 1;
      },
      nullptr);
}

std::string ThreadName(uint32_t tid) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/task/%u/comm", tid);
  FILE* f = std::fopen(path, "r");
  if (!f) return fmt::format("tid {}", tid);
  char buf[64] = {};
  const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  std::string s(buf, n);
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
  return s.empty() ? fmt::format("tid {}", tid) : s;
}

std::string Where(uintptr_t pc) {
  if (pc >= g_base && pc < g_end) return fmt::format("+{:x}", pc - g_base);
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
    const char* slash = std::strrchr(info.dli_fname, '/');
    return fmt::format("{}{}", slash ? slash + 1 : info.dli_fname,
                       info.dli_sname ? std::string(":") + info.dli_sname : std::string());
  }
  return "?";
}

void Loop() {
  uint64_t read = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(15));
    const uint64_t head = g_head.load(std::memory_order_acquire);
    if (head - read > kRing) read = head - kRing;  // (overrun: the newest)
    std::unordered_map<uint32_t, uint64_t> per_thread;
    std::map<std::pair<uint32_t, uintptr_t>, uint64_t> per_pc;
    // [IzunaRecomp] test64: samples in a library (libc, the driver) by where in
    // the program it was called from (the return address: ARM64 only, and
    // only for the library's leaf functions - still the usual case for a
    // clock read or a syscall stub)
    std::map<std::pair<uint32_t, uintptr_t>, uint64_t> per_caller;
    std::map<std::pair<uint32_t, uintptr_t>, std::map<uintptr_t, uint64_t>> per_caller2;  // (each caller's own callers)
    for (uint64_t i = read; i < head; ++i) {
      const Sample& s = g_ring[i & (kRing - 1)];
      ++per_thread[s.tid];
      // (64-byte buckets: a hot loop's few instructions together)
      ++per_pc[{s.tid, s.pc & ~uintptr_t(63)}];
      if ((s.pc < g_base || s.pc >= g_end) && s.lr >= g_base && s.lr < g_end)
      {
        ++per_caller[{s.tid, s.lr & ~uintptr_t(63)}];
        if (s.lr2) ++per_caller2[{s.tid, s.lr & ~uintptr_t(63)}][s.lr2 & ~uintptr_t(63)];
      }
    }
    const uint64_t total = head - read;
    read = head;
    if (!total) continue;
    std::vector<std::pair<uint32_t, uint64_t>> threads(per_thread.begin(), per_thread.end());
    std::sort(threads.begin(), threads.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::string out = fmt::format("[cpuprof] {} samples in 15 s (build {})", total, g_build_id);
    for (size_t t = 0; t < threads.size() && t < 4; ++t) {
      const uint32_t tid = threads[t].first;
      out += fmt::format("\n[cpuprof] {} - {:.1f}% of the samples; hottest:", ThreadName(tid),
                         100.0 * double(threads[t].second) / double(total));
      std::vector<std::pair<uintptr_t, uint64_t>> pcs;
      for (const auto& [key, n] : per_pc)
        if (key.first == tid) pcs.push_back({key.second, n});
      std::sort(pcs.begin(), pcs.end(), [](auto& a, auto& b) { return a.second > b.second; });
      for (size_t i = 0; i < pcs.size() && i < 12; ++i)
        out += fmt::format(" {} {:.1f}%", Where(pcs[i].first), 100.0 * double(pcs[i].second) / double(threads[t].second));
      std::vector<std::pair<uintptr_t, uint64_t>> callers;
      for (const auto& [key, n] : per_caller)
        if (key.first == tid) callers.push_back({key.second, n});
      if (!callers.empty()) {
        std::sort(callers.begin(), callers.end(), [](auto& a, auto& b) { return a.second > b.second; });
        out += "; library calls from:";
        for (size_t i = 0; i < callers.size() && i < 6; ++i) {
          out += fmt::format(" {} {:.1f}%", Where(callers[i].first),
                             100.0 * double(callers[i].second) / double(threads[t].second));
          auto up = per_caller2.find({tid, callers[i].first});
          if (up != per_caller2.end() && !up->second.empty()) {
            const auto top = std::max_element(up->second.begin(), up->second.end(),
                                              [](auto& a, auto& b) { return a.second < b.second; });
            out += fmt::format(" (<- {})", Where(top->first));
          }
        }
      }
    }
    REXLOG_INFO("{}", out);
  }
}

}  // namespace

void Start() {
  static bool started = false;
  if (started) return;
  started = true;
  FindProgram();
  struct sigaction sa = {};
  sa.sa_sigaction = OnProf;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGPROF, &sa, nullptr) != 0) return;
  itimerval it = {};
  it.it_interval.tv_usec = 1000;
  it.it_value.tv_usec = 1000;
  if (setitimer(ITIMER_PROF, &it, nullptr) != 0) return;
  std::thread(Loop).detach();
  REXLOG_INFO("[cpuprof] sampling every 1 ms of CPU; program at {:#x}-{:#x}, build {}", g_base, g_end, g_build_id);
}

}  // namespace izuna::cpu_profile

#else

namespace izuna::cpu_profile {
void Start() {}
}  // namespace izuna::cpu_profile

#endif
