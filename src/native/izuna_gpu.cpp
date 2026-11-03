// [IzunaRecomp] The native graphics system (izuna_gpu.h).
//
// What the game's Direct3D asks of a GPU besides drawing, done at the Direct3D
// calls that ask for it - the Kasumi renderer draws from the calls too, so
// nothing reads the GPU command stream Direct3D writes:
//
// - The command stream. Direct3D still writes it and kicks it (CP_RB_WPTR);
//   the kick only reports it consumed (the read pointer write-back), so
//   Direct3D always has room. No packet in it is looked at.
// - The timeline: what the hardware would do with the stream that the game
//   can see, in call order, each step once the native GPU work recorded
//   before its call has finished (native_renderer's RunAfterRecordedWork),
//   on our effect thread (a guest thread: interrupts call into the game).
//   The steps come from hooks on the Direct3D functions that ask for them
//   (config/hooks/native.toml; the M5 census of test48 found these to be all
//   NG2 asks for - see docs/NATIVE_RENDERER.md "M5"):
//     InsertFence (8373A088)    the fence counter and status word to the
//                               device's status block
//     the callback handshake    a CPU callback when the GPU gets there:
//       (8373A5A8)              scratch words, the interrupt, the game's
//                               handler clearing scratch 0, the poison
//     the flip (83744D90)       the flip-pending word set, then - after the
//                               swap callback's handshake - waited back to 0
//                               (the vblank handler clears it: the vsync wait)
//     the gamma ramp (8373F928) the display's 256-entry ramp
//     timestamps (83749AE0)     the swap / vblank counter (one device mode)
//     VdSwap (the SDK kernel)   the native frame to the presenter
//   A cache flush or coherency wait (MakeCoherent, EVENT_WRITE) has nothing
//   to do here: the renderer reads guest memory directly.
// - The vblank interrupts (VsyncThread: the video mode's rate, or locked to
//   the display - on the PS5 to VideoOut's own vblanks).

#include "native/izuna_gpu.h"
#include "native/pc_display_clock.h"
#include "izuna/izuna_pc.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/external_frame.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ps5_diag.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/ui/display_clock.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app_context.h>

#include "native/native_present.h"
#include "native/native_renderer.h"
#include "ps5/ps5_videoout.h"

// [IzunaRecomp] The guest vblank's own rate when it is not on the display's
// (V-Sync 60 Hz / 59.94 Hz on a PC): 0 is the video mode's.
REXCVAR_DEFINE_DOUBLE(native_vblank_hz, 0.0, "Native renderer",
                      "[IzunaRecomp] The guest vblank's rate on its own clock (0: the video mode's)");
REXCVAR_DEFINE_BOOL(native_vblank_display_locked, true, "Native renderer",
                    "[IzunaRecomp] Guest vblanks on the display's own refresh (frame pacing), else a timer at "
                    "the video mode's rate")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(native_vblank_display_phase, 50, "Native renderer",
                     "[IzunaRecomp] Where in the display's refresh a locked guest vblank falls, in % of it")
    .range(0, 99);
REXCVAR_DEFINE_BOOL(native_vblank_wait_flips, true, "Native renderer",
                    "[IzunaRecomp] Low latency: with the display's own vblanks and flips (PS5 VideoOut), hold a "
                    "guest vblank a refresh while the display is behind, so each frame shows at the next "
                    "refresh instead of queueing");

namespace ng2native {
namespace {

using rex::X_STATUS;

// The GPU registers the game's code touches (the M5 census: the CPU writes
// only the kick and reads only the interrupt status).
constexpr uint32_t CP_RB_WPTR = 0x01C5;

// The Direct3D device object's fields the timeline reads (guest 0x40001E80;
// big-endian words at these offsets).
constexpr uint32_t kDevFenceBlock = 10896;    // status block: +0 fence counter, +4 status word
constexpr uint32_t kDevScratchBlock = 10900;  // scratch write-back: scratch n at +4n
constexpr uint32_t kDevFenceCounter = 10908;  // the next fence
constexpr uint32_t kDevStatusBits = 14904;    // (& 3) | +48: the status word
constexpr uint32_t kDevStatusBase = 48;
constexpr uint32_t kDevTimestampMode = 21632, kDevTimestampTable = 21628;
constexpr uint32_t kDevTimestampLast = 21640, kDevTimestampRead = 21636;

class IzunaGraphicsSystem;
// The one graphics system (the hooks and SetGpuProgressCallback reach it here).
std::atomic<IzunaGraphicsSystem*> g_system{nullptr};

class IzunaGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  ~IzunaGraphicsSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    if (presenter_) return X_STATUS_SUCCESS;
    // (the provider's "GPU emulation" flag only asks for the device features
    // a renderer of guest frames needs - the native renderer's)
    if (!provider_) provider_ = rex::ui::vulkan::VulkanProvider::Create(true, true);
    if (!provider_) {
      REXLOG_ERROR("[izuna] no Vulkan device");
      return X_STATUS_UNSUCCESSFUL;
    }
    auto loss = [](bool, bool) { REXLOG_ERROR("[izuna] the Vulkan device was lost"); };
    if (app_context) {
      app_context->CallInUIThreadSynchronous([&] { presenter_ = provider_->CreatePresenter(loss); });
    } else {
      presenter_ = provider_->CreatePresenter(loss);
    }
    if (!presenter_) {
      REXLOG_ERROR("[izuna] no presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher, rex::system::KernelState* kernel) override {
    dispatcher_ = dispatcher;
    kernel_ = kernel;
    memory_ = dispatcher->memory();
    if (!provider_) provider_ = rex::ui::vulkan::VulkanProvider::Create(true, false);
    for (uint32_t i = 0; i < 256; ++i) {  // the linear ramp (the SDK's default: what games set for sRGB)
      const uint32_t v = i * 0x3FF / 0xFF;
      gamma_[i] = v << 20 | v << 10 | v;
    }
    // The GPU registers: 0x7FC80000-0x7FCFFFFF.
    memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this, &ReadRegisterThunk,
                                   &WriteRegisterThunk);
    running_ = true;
    g_system = this;
    SetGpuProgressCallback([] {
      if (IzunaGraphicsSystem* system = g_system.load(std::memory_order_acquire)) system->WakeEffects();
    });
    effect_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_, 128 * 1024, 0, [this] { return EffectThread(); }));
    effect_thread_->set_name("Izuna GPU effects");
    effect_thread_->Create();
    vsync_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_, 128 * 1024, 0, [this] { return VsyncThread(); }));
    vsync_thread_->set_name("Izuna VSync");
    vsync_thread_->Create();
    izuna::pc::StartDisplayClock();  // [IzunaRecomp] the window's monitor's vblanks (Windows; else nothing)
    REXLOG_INFO("[izuna] native graphics system up: Direct3D's GPU side natively, no command stream read");
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    interrupt_callback_ = callback;
    interrupt_data_ = user_data;
    REXLOG_INFO("[izuna] interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    REXLOG_INFO("[izuna] command stream at {:08X} ({} KB): reported consumed at each kick, never read", ptr,
                (uint32_t(1) << (size_log2 + 3)) >> 10);
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t) override {
    rptr_writeback_.store(ptr, std::memory_order_release);
  }

  // VdSwap (the SDK kernel), at its place in the game's calls: the native frame.
  void OnGuestSwap() override {
    Step([this] {
      counter_.fetch_add(1, std::memory_order_relaxed);
      if (++swaps_ % 600 == 0) LogCounts();
      Present();
    });
  }

  void Shutdown() override {
    if (!running_.exchange(false)) return;
    SetGpuProgressCallback(nullptr);
    g_system = nullptr;
    WakeEffects();
    WakeWaits();
    // (the guest threads end themselves at running_; the kernel owns them)
  }

  // --- The Direct3D calls (the hooks, on the game's render thread) ------------

  // InsertFence: the status word to the status block's +4, the fence counter
  // to +0 (two EVENT_WRITE_SHD packets on the hardware, in that order).
  void InsertFence(uint32_t device) {
    const uint32_t block = Field(device, kDevFenceBlock), counter = Field(device, kDevFenceCounter);
    const uint32_t word = (Field(device, kDevStatusBits) & 3) | Field(device, kDevStatusBase);
    Count(kFence);
    Step([this, block, word, counter] {
      Write32(block + 4, word);
      Write32(block, counter);
    });
  }

  // The callback handshake (8373A5A8: Direct3D's way to run a CPU callback
  // when the GPU reaches a point; ~3 a frame). flags bit 1: scratch 4 / 5 =
  // callback / argument, scratch 0 = the CPU mask (flags bits 24-29, 4 when
  // none: CPU 2), then the interrupt - the game's handler reads the scratch
  // words, runs the callback and clears scratch 0. Bit 2: wait for that, then
  // poison scratch 4 (0BADF00D). Neither bit: both.
  void CallbackHandshake(uint32_t device, uint32_t flags, uint32_t callback, uint32_t argument) {
    if (!(flags & 6)) flags |= 6;
    const uint32_t scratch = Field(device, kDevScratchBlock);
    uint32_t cpus = (flags >> 24) & 0x3F;
    if (!cpus) cpus = 4;
    Count(kInterrupt);
    Step([this, flags, scratch, callback, argument, cpus] {
      if (flags & 2) {
        Write32(scratch + 16, callback);
        Write32(scratch + 20, argument);
        Write32(scratch, cpus);
        for (uint32_t n = 0; n < 6; ++n) {
          if (cpus & (1u << n)) DispatchInterrupt(1, n);
        }
      }
      if (flags & 4) {
        WaitWord(scratch, 0);
        Write32(scratch + 16, 0x0BADF00Du);
      }
    });
  }

  // The flip (83744D90, once a frame): the flip-pending word (scratch 1)
  // set at its entry; after the swap callback's handshake (its own call,
  // CallbackHandshake), waited back to 0 - the vblank handler clears it once
  // the flip is shown: the vsync wait. Everything after it in the game's
  // calls - the swap itself, the next frame's fences - comes after the vblank.
  void FlipBegin(uint32_t device, uint32_t no_wait) {
    if (no_wait) return;
    const uint32_t scratch = Field(device, kDevScratchBlock);
    Step([this, scratch] { Write32(scratch + 4, 1); });
  }
  void FlipWait(uint32_t device, uint32_t no_wait) {
    if (no_wait) return;
    const uint32_t scratch = Field(device, kDevScratchBlock);
    Count(kFlip);
    Step([this, scratch] { WaitWord(scratch + 4, 0); });
  }

  // The gamma ramp (8373F928: DC_LUT_30_COLOR on the hardware, a COND_WRITE
  // stepping the index after each entry): three tables of 256 u16 - red,
  // green, blue - read at the call; the display takes 10 bits of each.
  // test41-44 missed it and showed every frame without the game's ramp.
  void GammaRamp(uint32_t ramp_address) {
    const uint8_t* p = memory_->TranslateVirtual<const uint8_t*>(ramp_address);
    if (!p) return;
    std::array<uint32_t, 256> ramp;
    auto u16 = [&](uint32_t at) { return uint32_t(p[at]) << 8 | p[at + 1]; };
    for (uint32_t i = 0; i < 256; ++i) {
      ramp[i] = (u16(i * 2) >> 6) << 20 | (u16(512 + i * 2) >> 6) << 10 | (u16(1024 + i * 2) >> 6);
    }
    Step([this, ramp] {
      {
        std::lock_guard<std::mutex> lock(gamma_mutex_);
        gamma_ = ramp;
      }
      LogRamp();
    });
  }

  // Timestamps (83749AE0, twice a frame): in device mode 2 only, the swap /
  // vblank counter into the next slot of the device's table (EVENT_WRITE_SHD
  // with initiator bit 31 on the hardware). NG2 never runs in that mode.
  void Timestamp(uint32_t device, uint32_t second) {
    if (Field(device, kDevTimestampMode) != 2) return;
    const uint32_t last = Field(device, kDevTimestampLast);
    const uint32_t next = last == 3 ? 0 : last + 1;
    if (next == Field(device, kDevTimestampRead)) return;
    const uint32_t at = Field(device, kDevTimestampTable) + (next * 2 + (second ? 0 : 1)) * 4;
    Step([this, at] { Write32(at, uint32_t(counter_.load(std::memory_order_relaxed))); });
  }

 private:
  // --- Registers --------------------------------------------------------------

  static uint32_t ReadRegisterThunk(void*, void* context, uint32_t addr) {
    return static_cast<IzunaGraphicsSystem*>(context)->ReadRegister(addr);
  }
  static void WriteRegisterThunk(void*, void* context, uint32_t addr, uint32_t value) {
    static_cast<IzunaGraphicsSystem*>(context)->WriteRegister(addr, value);
  }

  // The answers the SDK's GraphicsSystem::ReadRegister gives (NG2 reads only
  // the interrupt status).
  uint32_t ReadRegister(uint32_t addr) {
    const uint32_t r = (addr & 0xFFFF) / 4;
    switch (r) {
      case 0x0F00: return 0x08100748;  // RB_EDRAM_TIMING
      case 0x0F01: return 0x0000200E;  // RB_BC_CONTROL
      case 0x194C: {                   // R500_D1MODE_V_COUNTER
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case 0x1951: return 1;  // interrupt status: vblank
      case 0x1961: {          // AVIVO_D1MODE_VIEWPORT_SIZE
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_width), uint32_t(0x0FFF)) << 16 |
               std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      default: return 0;
    }
  }

  // The kick (CP_RB_WPTR): the stream is consumed as soon as it is written -
  // its read pointer write-back (Direct3D waits on it for room) moves to it.
  void WriteRegister(uint32_t addr, uint32_t value) {
    if ((addr & 0xFFFF) / 4 != CP_RB_WPTR) return;
    rex::ps5diag::Add(rex::ps5diag::kWritePointer);
    if (const uint32_t writeback = rptr_writeback_.load(std::memory_order_acquire)) {
      rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(writeback), value);
    }
  }

  // --- The timeline -----------------------------------------------------------

  uint32_t Field(uint32_t device, uint32_t offset) const {
    return rex::memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(device + offset));
  }

  // One step, in call order, once the native work recorded before it is done.
  void Step(std::function<void()> step) {
    RunAfterRecordedWork(std::move(step));
    WakeEffects();
  }

  // A big-endian word of guest memory (a guest virtual address).
  void Write32(uint32_t address, uint32_t value) {
    if (auto* p = memory_->TranslateVirtual<uint8_t*>(address)) rex::memory::store_and_swap<uint32_t>(p, value);
  }

  // Until a big-endian word of guest memory reads `value` (on the effect
  // thread: the steps after it wait too). Looked at again whenever something
  // we run may have changed it (WakeWaits: the vblank handler, the steps
  // before), else every 100 us (the game's own threads). At most 250 ms: a
  // word nothing will ever set must not hold the game for ever (logged).
  void WaitWord(uint32_t address, uint32_t value) {
    auto now_value = [&] { return rex::memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(address)); };
    if (now_value() == value) return;
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(waits_mutex_);
    while (running_) {
      const uint64_t seen = wakes_;
      if (now_value() == value) break;
      waits_cv_.wait_for(lock, std::chrono::microseconds(100), [&] { return wakes_ != seen || !running_; });
      if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(250)) {
        if (now_value() == value) break;
        REXLOG_WARN("[izuna] a wait gave up after 250 ms: {:08X} reads {:08X}, not {:08X}", address,
                    now_value(), value);
        break;
      }
    }
    waits_blocked_.fetch_add(1, std::memory_order_relaxed);
    wait_ns_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - t0).count()),
                       std::memory_order_relaxed);
  }

  // Something we ran may have met a wait: look again.
  void WakeWaits() {
    {
      std::lock_guard<std::mutex> lock(waits_mutex_);
      ++wakes_;
    }
    waits_cv_.notify_all();
  }

  // Runs the due steps (PumpSideEffects: on this guest thread), woken by new
  // steps and by finished GPU work (SetGpuProgressCallback); every 2 ms
  // regardless (the 30 ms fallback).
  int EffectThread() {
    izuna::pc::RaiseThreadPriority(0);  // [IzunaRecomp] (Windows: above normal)
    while (running_) {
      {
        std::unique_lock<std::mutex> lock(effect_mutex_);
        effect_cv_.wait_for(lock, std::chrono::milliseconds(2), [&] { return wake_effects_ || !running_; });
        wake_effects_ = false;
      }
      rex::external_frame::PumpSideEffects();
      WakeWaits();
    }
    return 0;
  }

  void WakeEffects() {
    {
      std::lock_guard<std::mutex> lock(effect_mutex_);
      wake_effects_ = true;
    }
    effect_cv_.notify_one();
  }

  // The game's graphics interrupt callback, on the calling guest thread
  // (the SDK's GraphicsSystem::DispatchInterruptCallback).
  void DispatchInterrupt(uint32_t source, uint32_t cpu) {
    if (!interrupt_callback_) return;
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) return;
    rex::ps5diag::Add(rex::ps5diag::kInterrupts);
    thread->SetActiveCpu(cpu == 0xFFFFFFFFu ? 2 : cpu);
    uint64_t args[] = {source, interrupt_data_};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args, 2);
  }

  // When the game has set a ramp: which (a few times).
  void LogRamp() {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) >= 6) return;
    std::lock_guard<std::mutex> lock(gamma_mutex_);
    auto red = [&](int e) { return gamma_[e] >> 20 & 1023; };
    REXLOG_INFO("[izuna] the game's gamma ramp: red at 0/32/64/96/128/160/192/224/255 = {} {} {} {} {} {} {} {} {} "
                "(of 1023; linear: 0 128 257 385 514 642 770 899 1023)",
                red(0), red(32), red(64), red(96), red(128), red(160), red(192), red(224), red(255));
  }

  // What the game asked for, logged every 600 swaps.
  enum Counter { kFence, kInterrupt, kFlip, kCounters };
  std::array<std::atomic<uint64_t>, kCounters> counts_{};
  void Count(Counter c) { counts_[c].fetch_add(1, std::memory_order_relaxed); }
  void LogCounts() {
    auto n = [&](Counter c) { return counts_[c].exchange(0, std::memory_order_relaxed); };
    REXLOG_INFO("[izuna] last 600 swaps: {} fences, {} callbacks, {} flips ({} waits had to wait, {:.1f} ms in "
                "all)",
                n(kFence), n(kInterrupt), n(kFlip), waits_blocked_.exchange(0), double(wait_ns_.exchange(0)) / 1e6);
    uint64_t pending[4];
    for (int i = 0; i < 4; ++i) pending[i] = pending_at_vblank_[i].exchange(0);
    if (pending[0] + pending[1] + pending[2] + pending[3]) {
      REXLOG_INFO("[izuna] flips waiting for the display at each guest vblank: none {}, one {}, two {}, more {}; "
                  "{} vblanks held (low latency)",
                  pending[0], pending[1], pending[2], pending[3], vblanks_held_.exchange(0));
    }
  }

  // --- Vblanks ----------------------------------------------------------------

  void MarkVblank() {
    rex::ps5diag::Add(rex::ps5diag::kVblanks);
    counter_.fetch_add(1, std::memory_order_relaxed);
    DispatchInterrupt(0, 2);
    WakeWaits();  // (the handler clears NG2's flip-pending word: the timeline's vsync wait)
  }

  // The display's refresh as VideoOut reports it (ps5_videoout.h): trusted
  // once 30 vblanks in a row agree - the same count always gives the same
  // time, and each step is 6-20 ms.
  struct VideoOutClock {
    int64_t ns = 0, period = 0;
    uint64_t count = 0;
    int agreed = 0;

    bool Update() {
      int64_t t;
      uint64_t c;
      if (!ng2ps5::videoout::LastVblank(&t, &c)) {
        agreed = 0;
      } else if (c == count) {
        if (std::llabs(t - ns) > 200'000) agreed = 0;
      } else {
        if (count && c > count && c - count <= 4) {
          const int64_t each = (t - ns) / int64_t(c - count);
          if (each >= 6'000'000 && each <= 20'000'000) {
            period = period ? period + (each - period) / 16 : each;
            ++agreed;
          } else {
            agreed = 0;
          }
        }
        count = c;
        ns = t;
      }
      return agreed >= 30 && period;
    }
  };

  // Low latency (native_vblank_wait_flips): whether to hold this guest vblank
  // a refresh because the display is behind - two flips waiting, or one
  // submitted before the display's last vblank (it missed it behind another).
  // Each frame then reaches the display at the next refresh instead of
  // queueing. Exact from the flips' own accounting (test45 asked
  // sceVideoOutIsFlipPending, which answered "pending" at every vblank: its
  // holds came at exactly the back-off's gaps, each a needless 33 ms frame).
  // Never two in a row, at most one a second.
  bool ShouldHold(int64_t target, int64_t display_vblank) {
    uint32_t pending;
    int64_t oldest;
    if (!ng2ps5::videoout::PendingFlips(&pending, &oldest)) return false;
    pending_at_vblank_[std::min<uint32_t>(pending, 3)].fetch_add(1, std::memory_order_relaxed);
    if (!REXCVAR_GET(native_vblank_wait_flips) || held_last_ || target - last_hold_ < 1'000'000'000) return false;
    return pending >= 2 || (pending == 1 && oldest < display_vblank - 1'000'000);
  }

  // As the SDK's vsync worker: the video mode's rate, or - with
  // native_vblank_display_locked and the display's refresh known and close
  // to it - a point (native_vblank_display_phase %) into each refresh of the
  // display, so no frame is shown twice or skipped. The display's refresh:
  // VideoOut's own vblanks on the PS5, else the presenter's acquires that had
  // to wait (rex/ui/display_clock.h - only while the swapchain is full).
  // The last part of a wait for a vblank, spun (yielding): what a sleep may overshoot by.
#if defined(_WIN32)
  static constexpr int64_t kSpinNs = 250'000;
#else
  static constexpr int64_t kSpinNs = 300'000;
#endif

  int VsyncThread() {
    izuna::pc::RaiseThreadPriority(1);  // [IzunaRecomp] (Windows: the guest vblank's clock, highest)
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double own_hz = REXCVAR_GET(native_vblank_hz);
    const double hz = std::max(1.0, own_hz > 0 ? own_hz : double(float(mode.refresh_rate)));
    REXLOG_INFO("[izuna] guest vblank: {:.3f} Hz{}", hz,
                REXCVAR_GET(native_vblank_display_locked) ? ", on the display's refresh when it is steady and divides it"
                                                          : ", on its own clock");
    const int64_t guest_period = int64_t(1e9 / hz);
    const uint64_t freq = rex::chrono::Clock::guest_tick_frequency();
    const uint64_t interval = std::max<uint64_t>(1, uint64_t(double(freq) / hz));
    uint64_t last = rex::chrono::Clock::QueryGuestTickCount();
    int64_t last_locked = 0;
    bool locked_logged = false, videoout_logged = false, uneven_logged = false;
    VideoOutClock videoout;
    namespace dc = rex::ui::display_clock;
    while (running_) {
      if (REXCVAR_GET(native_vblank_display_locked)) {
        int64_t period = dc::period_ns.load(std::memory_order_relaxed);
        int64_t last_vblank = dc::last_vblank_ns.load(std::memory_order_acquire);
        const bool from_videoout = videoout.Update();
        if (from_videoout) {
          period = videoout.period;
          last_vblank = videoout.ns;
          if (!videoout_logged) {
            videoout_logged = true;
            REXLOG_INFO("[izuna] guest vblank on VideoOut's vblanks: {:.3f} Hz", 1e9 / double(period));
          }
        }
        const int64_t now = dc::NowNs();
        // [IzunaRecomp] A whole number of the display's refreshes per guest
        // vblank: 1 on a 60 Hz screen, 2 on 120 Hz, 3 on 180 Hz, 4 on 240 Hz -
        // each frame then stays on screen the same number of refreshes.
        const int64_t every = period ? std::clamp<int64_t>((guest_period + period / 2) / period, 1, 4) : 1;
        const int64_t step = period * every;
        if (period && last_vblank && now - last_vblank < 4 * step &&
            std::llabs(step - guest_period) * 20 < guest_period) {
          if (!locked_logged) {
            locked_logged = true;
            REXLOG_INFO("[izuna] guest vblank locked to the display: {:.3f} Hz{}", 1e9 / double(period),
                        every > 1 ? fmt::format(", every {} refreshes", every) : std::string());
          }
          int64_t target = last_vblank + period * REXCVAR_GET(native_vblank_display_phase) / 100;
          while (target <= last_locked + step - period / 2) target += period;
          const int64_t left = target - now;
          if (left > kSpinNs + 100'000) {
            izuna::pc::SleepNs(left - kSpinNs);
            continue;
          }
          while (dc::NowNs() < target) rex::thread::MaybeYield();
          last_locked = target;
          last = rex::chrono::Clock::QueryGuestTickCount();
          const bool hold = from_videoout && ShouldHold(target, last_vblank);
          held_last_ = hold;
          if (hold) {
            last_hold_ = target;
            vblanks_held_.fetch_add(1, std::memory_order_relaxed);
          } else {
            MarkVblank();
          }
          continue;
        }
      }
      // [IzunaRecomp] A display the game's rate does not divide (144, 165 Hz):
      // said once - a 60 / 120 Hz mode or G-Sync / FreeSync shows it evenly.
      if (REXCVAR_GET(native_vblank_display_locked) && !uneven_logged) {
        const int64_t period = dc::period_ns.load(std::memory_order_relaxed);
        if (period && dc::NowNs() - dc::last_vblank_ns.load(std::memory_order_relaxed) < 4 * period) {
          uneven_logged = true;
          REXLOG_INFO("[izuna] the display refreshes at {:.2f} Hz, which 60 fps does not divide: frames stay on "
                      "screen 2 or 3 refreshes. A 60 or 120 Hz mode, or G-Sync / FreeSync, shows them evenly.",
                      1e9 / double(period));
        }
      }
      const uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
      if (now - last > interval * 4) last = now - interval;  // (fell far behind: no burst)
      if (now - last >= interval) {
        MarkVblank();
        last += interval;
        continue;
      }
      // Until the next one: asleep, then the last kSpinNs awake. [IzunaRecomp]
      // test32: a high-resolution timer (izuna::pc::SleepNs) - before, a 1 ms
      // sleep that lands anywhere in the next millisecond left 1-1.3 ms to
      // spin every vblank, ~6-8% of a core at 60 Hz (on a variable-refresh
      // screen this clock paces every frame) - CPU time and power a laptop on
      // battery takes from the game's own threads.
      const int64_t left_ns = int64_t(double(last + interval - now) * 1e9 / double(freq));
      if (left_ns > kSpinNs + 100'000) {
        izuna::pc::SleepNs(left_ns - kSpinNs);
      } else {
        rex::thread::MaybeYield();
      }
    }
    return 0;
  }

  // --- The swap ---------------------------------------------------------------

  // The swap's frame (once the native renderer has recorded and submitted it:
  // external_frame::SwapWait, at most 50 ms), through the game's gamma ramp
  // into the presenter.
  void Present() {
    rex::external_frame::SwapWait();
    std::array<uint32_t, 256> ramp;
    {
      std::lock_guard<std::mutex> lock(gamma_mutex_);
      ramp = gamma_;
    }
    if (presenter_ && PresentNativeFrame(*presenter_, ramp.data())) rex::external_frame::CountSwap();
  }

  std::unique_ptr<rex::ui::GraphicsProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  std::atomic<bool> running_{false};
  std::atomic<uint32_t> rptr_writeback_{0};
  std::mutex gamma_mutex_;
  std::array<uint32_t, 256> gamma_{};  // red << 20 | green << 10 | blue, 10 bits each
  uint32_t interrupt_callback_ = 0, interrupt_data_ = 0;
  std::atomic<uint64_t> counter_{0};  // swaps and vblanks (the hardware's, for timestamps)
  uint64_t swaps_ = 0;                // (the effect thread's)
  // WaitWord (waits_mutex_).
  std::mutex waits_mutex_;
  std::condition_variable waits_cv_;
  uint64_t wakes_ = 0;
  std::atomic<uint64_t> waits_blocked_{0}, wait_ns_{0};
  // VsyncThread's: holds (ShouldHold), the flips pending at each locked vblank (0, 1, 2, 3+).
  std::atomic<uint64_t> vblanks_held_{0}, pending_at_vblank_[4]{};
  bool held_last_ = false;
  int64_t last_hold_ = 0;
  // The effect thread (effect_mutex_).
  std::mutex effect_mutex_;
  std::condition_variable effect_cv_;
  bool wake_effects_ = false;
  rex::system::object_ref<rex::system::XHostThread> effect_thread_, vsync_thread_;
};

IzunaGraphicsSystem* System() { return g_system.load(std::memory_order_acquire); }

}  // namespace

std::unique_ptr<rex::system::IGraphicsSystem> CreateIzunaGraphicsSystem() {
  return std::make_unique<IzunaGraphicsSystem>();
}

void OnInsertFence(uint32_t device) {
  if (auto* s = System()) s->InsertFence(device);
}
void OnCallbackHandshake(uint32_t device, uint32_t flags, uint32_t callback, uint32_t argument) {
  if (auto* s = System()) s->CallbackHandshake(device, flags, callback, argument);
}
void OnFlipBegin(uint32_t device, uint32_t no_wait) {
  if (auto* s = System()) s->FlipBegin(device, no_wait);
}
void OnFlipWait(uint32_t device, uint32_t no_wait) {
  if (auto* s = System()) s->FlipWait(device, no_wait);
}
void OnGammaRamp(uint32_t ramp) {
  if (auto* s = System()) s->GammaRamp(ramp);
}
void OnGpuTimestamp(uint32_t device, uint32_t second) {
  if (auto* s = System()) s->Timestamp(device, second);
}

}  // namespace ng2native
