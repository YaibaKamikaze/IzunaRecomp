// [IzunaRecomp] The PS5's display as VideoOut reports it (ps5_videoout.h).
#include "ps5/ps5_videoout.h"

#if defined(__PROSPERO__) && !defined(NG2_PS5_HOST_TEST)

#include <atomic>

#include <rex/logging.h>
#include <rex/ui/display_clock.h>

extern "C" {
int __real_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void* parameter);
int __real_sceVideoOutSubmitFlip(int32_t handle, int32_t buffer_index, uint32_t mode, int64_t argument);
int sceVideoOutGetVblankStatus(int32_t handle, void* status);
int sceVideoOutGetFlipStatus(int32_t handle, uint64_t status[16]);
uint64_t sceKernelReadTsc(void);
uint64_t sceKernelGetTscFrequency(void);
}

namespace {

std::atomic<int> g_handle{-1};

// The WSI numbers its flips 1, 2, 3...; the flip status reports the latest
// shown (word 3, as the WSI itself reads it). The submit times of the last 16.
constexpr uint32_t kFlipSlots = 16;
std::atomic<int64_t> g_submitted{0};
std::atomic<int64_t> g_submit_ns[kFlipSlots];

}  // namespace

// The swapchain's sceVideoOutOpen (the WSI opens the main output once per
// process, bus 0): its handle is kept for the queries below.
extern "C" int __wrap_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void* parameter) {
  const int handle = __real_sceVideoOutOpen(user, bus, index, parameter);
  if (handle >= 0 && bus == 0) g_handle.store(handle, std::memory_order_release);
  REXLOG_INFO("[izuna] VideoOut opened: handle {} (bus {}, index {})", handle, bus, index);
  return handle;
}

// The WSI's flip thread, once a frame is done: the flip's argument and time.
extern "C" int __wrap_sceVideoOutSubmitFlip(int32_t handle, int32_t buffer_index, uint32_t mode, int64_t argument) {
  const int64_t now = rex::ui::display_clock::NowNs();
  const int result = __real_sceVideoOutSubmitFlip(handle, buffer_index, mode, argument);
  if (result == 0 && argument > 0) {
    g_submit_ns[uint64_t(argument) % kFlipSlots].store(now, std::memory_order_relaxed);
    g_submitted.store(argument, std::memory_order_release);
  }
  return result;
}

namespace ng2ps5::videoout {

// SceVideoOutVblankStatus as the PS4's SDK lays it out: the vblank count,
// the process time, then the time-stamp counter at the vblank (16 words
// read, so a longer PS5 struct is safe). An answer counts only when its
// counter reading is in the past and less than 100 ms old. (test45's
// console log: the count, then a counter reading just before "now", and the
// guest vblank's agreement checks trusted it within half a second.)
bool LastVblank(int64_t* ns, uint64_t* count) {
  const int handle = g_handle.load(std::memory_order_acquire);
  if (handle < 0) return false;
  uint64_t status[16] = {};
  const int result = sceVideoOutGetVblankStatus(handle, status);
  const uint64_t now_tsc = sceKernelReadTsc();
  const int64_t now_ns = rex::ui::display_clock::NowNs();
  if (result != 0 || status[2] == 0 || status[2] > now_tsc) return false;
  static const double ns_per_tick = 1e9 / double(sceKernelGetTscFrequency() ? sceKernelGetTscFrequency() : 1);
  const double age_ns = double(now_tsc - status[2]) * ns_per_tick;
  if (age_ns > 100e6) return false;
  *ns = now_ns - int64_t(age_ns);
  *count = status[0];
  return true;
}

bool PendingFlips(uint32_t* count, int64_t* oldest_ns) {
  const int handle = g_handle.load(std::memory_order_acquire);
  const int64_t submitted = g_submitted.load(std::memory_order_acquire);
  if (handle < 0 || submitted <= 0) return false;
  uint64_t status[16] = {};
  if (sceVideoOutGetFlipStatus(handle, status) != 0) return false;
  const int64_t shown = int64_t(status[3]);
  if (shown < 0 || shown > submitted || submitted - shown >= int64_t(kFlipSlots)) return false;
  *count = uint32_t(submitted - shown);
  *oldest_ns = *count ? g_submit_ns[uint64_t(shown + 1) % kFlipSlots].load(std::memory_order_relaxed) : 0;
  return true;
}

}  // namespace ng2ps5::videoout

#endif
