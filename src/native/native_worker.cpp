// [IzunaRecomp] Threaded recording for the native renderer (native_draw_internal.h).

#include "izuna/izuna_pc.h"
#include "native/native_draw_internal.h"

namespace ng2native {

thread_local int64_t t_phase_ns[kPhaseCount] = {};  // (native_phase.h)
bool g_timed = true;                                // (native_phase.h)

// --- Threaded recording (native_threaded) -------------------------------------
//
// The game's render thread (the producer) only records each Direct3D event:
// the device as it changed since the event before (64-byte blocks of the
// window the drawing code reads), and what must be read at the call - the
// vertex fetches' fields Direct3D patches into the shader for this draw, the
// BeginVertices data (its command buffer space is reused), the resolve and
// clear arguments. A worker replays the records with the same code (Draw,
// DrawUp, ResolveWork, ClearWork, the renderer's kickoff and swap) against its
// mirror of the device. The game thread keeps only the copying; the Vulkan
// work runs beside it. The design of Edge of Time Recompiled's render thread
// (BSD-3: a queue of draw packets with a device snapshot), in our own code.

namespace draw {

constexpr uint32_t kWinBegin = 1152, kWinEnd = 12800, kWinBlock = 64;  // the device bytes the drawing reads
static_assert(kWinBegin % kWinBlock == 0 && kWinEnd % kWinBlock == 0);
static_assert(kVertexShader + 20 <= kWinEnd && kFetchConstants >= kWinBegin, "the window must hold what Draw reads");

enum class Rec : uint32_t { Draw, DrawUp, Resolve, Clear, Kickoff, Swap, Scissor, Wrap };
struct RecHeader {
  uint32_t type, bytes;  // (bytes: the whole record, header included, a multiple of 16)
  uint64_t seq;
};
struct DrawRec {
  uint32_t primitive, count, index_info[4];
};
struct DrawUpRec {
  uint32_t primitive, count, dwords, bytes, ibytes, index_flags, index_offset;
};
struct ResolveRec {
  uint32_t flags, has_rect, has_color;
  int32_t rect[4];
  float color[4];
  double clear_z;
};
struct SwapRec {
  uint32_t device, front;
};
struct ScissorRec {
  uint32_t tl, br;
};

struct Threading {
  std::atomic<bool> on{false};
  std::thread::id worker;
  static constexpr size_t kRing = size_t(64) << 20;
  std::unique_ptr<uint8_t[]> ring;
  size_t head = 0, tail = 0, used = 0;  // (mutex)
  bool worker_waiting = false;           // (mutex) [IzunaRecomp] the producer wakes it only then
  std::mutex mutex;
  std::condition_variable data_cv, space_cv, swap_cv;
  uint64_t enqueued = 0;                  // (producer)
  std::atomic<uint64_t> last_kickoff{0};  // the last kickoff / swap record (GPU sync)
  std::atomic<uint64_t> swaps_in{0}, swaps_done{0};
  std::atomic<uint64_t> producer_ns{0};
  // What the worker is on (the watchdog's report; seq 0: waiting for work).
  std::atomic<uint64_t> busy_seq{0};
  std::atomic<uint32_t> busy_type{0};
  std::atomic<int64_t> busy_since_ns{0};
  std::atomic<uint64_t> exceptions{0};
  std::vector<uint8_t> scratch;  // (producer) the record being built
  uint8_t producer_mirror[kWinEnd] = {};
  uint8_t worker_mirror[kWinEnd] = {};
} q;

bool Threaded() { return q.on.load(std::memory_order_relaxed); }

int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The game's render thread has waited `waited` seconds for the worker (a hang
// shows here rather than as a silent freeze - test23).
void WorkerStalled(const char* what, int waited) {
  const uint64_t seq = q.busy_seq.load();
  if (!seq) {
    REXLOG_WARN("[native] threaded: the game has waited {} s for {}; the worker is idle ({} records queued, {} frames done)",
                waited, what, q.enqueued, q.swaps_done.load());
    return;
  }
  REXLOG_WARN("[native] threaded: the game has waited {} s for {}; the worker is on record {} (type {}) for {:.0f} ms "
              "({} queued)",
              waited, what, seq, q.busy_type.load(), double(SteadyNs() - q.busy_since_ns.load()) / 1e6, q.enqueued);
}
bool OnWorker() { return Threaded() && std::this_thread::get_id() == q.worker; }

ProducerTime::~ProducerTime() {
  if (!g_timed) return;
  const uint64_t ns = TicksToNs(Ticks() - t0);
  if (Threaded()) {
    q.producer_ns.fetch_add(ns, std::memory_order_relaxed);
  } else {
    t_timing.hooks_ns += ns;
  }
}

void Put(const void* p, size_t n) {
  if (!n) return;
  const size_t at = q.scratch.size();
  q.scratch.resize(at + n);
  std::memcpy(q.scratch.data() + at, p, n);
}
template <class T>
void Put(const T& v) {
  Put(&v, sizeof(v));
}

void BeginRecord(Rec type) {
  q.scratch.clear();
  const RecHeader h = {uint32_t(type), 0, 0};
  Put(h);
}

// The device's 64-byte blocks that changed since the last record.
void PutDeltaBytes(const uint8_t* dev);
void PutDelta(uint32_t device) { PutDeltaBytes(device ? Guest(device) : nullptr); }
void PutDeltaBytes(const uint8_t* dev) {
  const size_t count_at = q.scratch.size();
  uint32_t n = 0;
  Put(n);
  if (dev) {
    for (uint32_t off = kWinBegin; off < kWinEnd; off += kWinBlock) {
      if (std::memcmp(dev + off, q.producer_mirror + off, kWinBlock) == 0) continue;
      std::memcpy(q.producer_mirror + off, dev + off, kWinBlock);
      Put(off);
      Put(dev + off, kWinBlock);
      ++n;
    }
  }
  std::memcpy(q.scratch.data() + count_at, &n, sizeof(n));
}

// The shader cache entry of a shader object (thread-safe: no Vulkan, no g.shaders).
// [IzunaRecomp] test29: the last answer remembered per thread - consecutive
// draws mostly use the same vertex shader, and each lookup took the shader
// table's mutex on the game's render thread - until the table changes
// (object_generation: a shader created, perhaps at a recycled address).
const ShaderCacheEntry* EntryForObject(uint32_t object) {
  thread_local uint32_t memo_object = 0;
  thread_local uint64_t memo_generation = UINT64_MAX;
  thread_local const ShaderCacheEntry* memo_entry = nullptr;
  const uint64_t generation = g.object_generation.load(std::memory_order_acquire);
  if (object == memo_object && generation == memo_generation) return memo_entry;
  memo_object = object;
  memo_generation = generation;
  memo_entry = nullptr;
  uint64_t hash;
  {
    std::lock_guard<std::mutex> lock(g.object_mutex);
    auto it = g.object_hash.find(object);
    if (it == g.object_hash.end()) return nullptr;
    hash = it->second;
  }
  const ShaderCacheEntry* begin = ShaderEntries();
  const ShaderCacheEntry* end = ShaderEntries() + ShaderEntryCount();
  const ShaderCacheEntry* e =
      std::lower_bound(begin, end, hash, [](const ShaderCacheEntry& a, uint64_t h) { return a.hash < h; });
  memo_entry = e != end && e->hash == hash ? e : nullptr;
  return memo_entry;
}

// The vertex fetches' fields as Direct3D patched them for this draw: ok flag,
// byte count, the bytes (FillVertexFetches' output).
void PutLayout(uint32_t device) {
  uint8_t buf[kSharedLayoutEnd - 1664] = {};
  uint32_t ok = 1, n = 0;
  if (const uint8_t* dev = Guest(device)) {
    const uint32_t object = Be32(dev + kVertexShader);
    if (const ShaderCacheEntry* e = EntryForObject(object)) {
      n = std::min<uint32_t>(e->vertexLayoutCount * 16u, sizeof(buf));
      ok = FillVertexFetches(object, e, buf) ? 1u : 0u;
    }
  }
  Put(ok);
  Put(n);
  Put(buf, n);
}

uint64_t CommitRecord() {
  q.scratch.resize((q.scratch.size() + 15) & ~size_t(15));
  const size_t bytes = q.scratch.size();
  RecHeader h;
  std::memcpy(&h, q.scratch.data(), sizeof(h));
  h.bytes = uint32_t(bytes);
  h.seq = ++q.enqueued;
  std::memcpy(q.scratch.data(), &h, sizeof(h));
  bool wake = false;
  {
    std::unique_lock<std::mutex> lock(q.mutex);
    for (;;) {
      const size_t room = Threading::kRing - q.head;
      const size_t needed = bytes <= room ? bytes : room + bytes;
      if (q.used + needed <= Threading::kRing) break;
      rex::audio::HostStallScope stall;
      for (int waited = 2; q.space_cv.wait_for(lock, std::chrono::seconds(2)) == std::cv_status::timeout; waited += 2) {
        WorkerStalled("room in the record ring", waited);
      }
    }
    if (bytes > Threading::kRing - q.head) {  // (a wrap marker to the end, then from the start)
      const RecHeader wrap = {uint32_t(Rec::Wrap), uint32_t(Threading::kRing - q.head), 0};
      std::memcpy(q.ring.get() + q.head, &wrap, sizeof(wrap));
      q.used += Threading::kRing - q.head;
      q.head = 0;
    }
    std::memcpy(q.ring.get() + q.head, q.scratch.data(), bytes);
    q.head = (q.head + bytes) % Threading::kRing;
    q.used += bytes;
    wake = q.worker_waiting;  // [IzunaRecomp] (a busy worker comes back for more by itself: no wake per draw)
  }
  if (wake) q.data_cv.notify_one();
  return h.seq;
}

// One record, on the worker.
void Execute(const RecHeader& h, const uint8_t* r) {
  uint32_t n;
  std::memcpy(&n, r, sizeof(n));
  r += sizeof(n);
  for (uint32_t i = 0; i < n; ++i, r += 4 + kWinBlock) {
    uint32_t off;
    std::memcpy(&off, r, sizeof(off));
    std::memcpy(q.worker_mirror + off, r + 4, kWinBlock);
  }
  auto layout = [&](const uint8_t*& at, bool* ok) {
    uint32_t okv, bytes;
    std::memcpy(&okv, at, 4);
    std::memcpy(&bytes, at + 4, 4);
    const uint8_t* p = at + 8;
    at += 8 + bytes;
    *ok = okv != 0;
    return p;
  };
  switch (Rec(h.type)) {
    case Rec::Draw: {
      DrawRec d;
      std::memcpy(&d, r, sizeof(d));
      r += sizeof(d);
      bool ok;
      const uint8_t* l = layout(r, &ok);
      ScopedTime timed(t_timing.hooks_ns);
      Draw(Device{q.worker_mirror}, d.primitive, d.count, d.index_info, 0, 0, l, ok);
      break;
    }
    case Rec::DrawUp: {
      DrawUpRec d;
      std::memcpy(&d, r, sizeof(d));
      r += sizeof(d);
      const uint8_t* src = r;
      const uint8_t* isrc = d.ibytes ? r + d.bytes : nullptr;
      r += d.bytes + d.ibytes;
      bool ok;
      const uint8_t* l = layout(r, &ok);
      ScopedTime timed(t_timing.hooks_ns);
      DrawUp(q.worker_mirror, d.primitive, d.count, d.dwords, src, d.bytes, isrc, d.ibytes, d.index_flags,
             d.index_offset, l, ok);
      break;
    }
    case Rec::Resolve: {
      ResolveRec d;
      std::memcpy(&d, r, sizeof(d));
      ScopedTime timed(t_timing.hooks_ns);
      ResolveWork(q.worker_mirror, d.flags, d.has_rect ? d.rect : nullptr, d.has_color ? d.color : nullptr,
                  d.clear_z);
      break;
    }
    case Rec::Clear: {
      PendingClear c;
      std::memcpy(&c, r, sizeof(c));
      ScopedTime timed(t_timing.hooks_ns);
      ClearWork(q.worker_mirror, c);
      break;
    }
    case Rec::Kickoff:
      RendererKickoff();
      RecordDone(h.seq);
      break;
    case Rec::Swap: {
      SwapRec d;
      std::memcpy(&d, r, sizeof(d));
      RendererSwap(d.device, d.front);
      RecordDone(h.seq);
      {
        // (under the mutex the game thread waits with: a notify between its
        // check and its wait would be lost, and it would hang at Swap)
        std::lock_guard<std::mutex> lock(q.mutex);
        q.swaps_done.fetch_add(1, std::memory_order_release);
      }
      q.swap_cv.notify_all();
      break;
    }
    case Rec::Scissor: {
      ScissorRec sc;
      std::memcpy(&sc, r, sizeof(sc));
      g.window_scissor_tl = sc.tl;
      g.window_scissor_br = sc.br;
      break;
    }
    case Rec::Wrap:
      break;
  }
}

void WorkerMain() {
  izuna::pc::RaiseThreadPriority(0);  // [IzunaRecomp] (Windows: above normal; elsewhere nothing)
  for (;;) {
    const uint8_t* p;
    RecHeader h;
    {
      std::unique_lock<std::mutex> lock(q.mutex);
      if (q.used == 0) {
        q.worker_waiting = true;
        q.data_cv.wait(lock, [] { return q.used != 0; });
        q.worker_waiting = false;
      }
      p = q.ring.get() + q.tail;
      std::memcpy(&h, p, sizeof(h));
      if (Rec(h.type) == Rec::Wrap) {
        q.used -= h.bytes;
        q.tail = 0;
        q.space_cv.notify_one();
        continue;
      }
    }
    q.busy_type.store(h.type, std::memory_order_relaxed);
    const int64_t record_t0 = g_timed ? SteadyNs() : 0;  // (only the log's reports read it)
    if (g_timed) {
      for (int64_t& v : t_phase_ns) v = 0;
    }
    q.busy_since_ns.store(record_t0, std::memory_order_relaxed);
    q.busy_seq.store(h.seq, std::memory_order_release);
    try {
      Execute(h, p + sizeof(h));  // (the region stays ours until `used` drops)
    } catch (const std::exception& e) {  // (logged, the record dropped: the game goes on)
      if (q.exceptions.fetch_add(1) < 10) {
        REXLOG_ERROR("[native] threaded: record {} (type {}) failed: {}", h.seq, h.type, e.what());
      }
      if (Rec(h.type) == Rec::Kickoff || Rec(h.type) == Rec::Swap) RecordDone(h.seq);
      if (Rec(h.type) == Rec::Swap) {  // (the game waits for it)
        {
          std::lock_guard<std::mutex> lock(q.mutex);
          q.swaps_done.fetch_add(1, std::memory_order_release);
        }
        q.swap_cv.notify_all();
      }
    }
    // [IzunaRecomp] test29: a record that held the worker past half a frame
    // (the game's fences wait behind it): what it was, for the next log.
    if (const int64_t now = g_timed ? SteadyNs() : 0, took = now - record_t0; took > 8'000'000) {
      static int slow_logged = 0;
      if (slow_logged < 40) {
        ++slow_logged;
        static const char* const kNames[] = {"draw", "draw (user vertices)", "resolve", "clear",
                                             "kickoff", "swap", "scissor", "wrap"};
        // (test30: what it was in, and whether the presenter's vkQueuePresentKHR overlapped it)
        REXLOG_INFO("[native] threaded: record {} ({}) took {:.1f} ms on the worker: submit {:.1f}, fence wait "
                    "{:.1f}, textures {:.1f}, pipelines {:.1f}, image creation {:.1f}; a present overlapped {:.1f} ms",
                    h.seq, h.type < 8 ? kNames[h.type] : "?", double(took) / 1e6, double(t_phase_ns[kPhaseSubmit]) / 1e6,
                    double(t_phase_ns[kPhaseFence]) / 1e6, double(t_phase_ns[kPhaseTexture]) / 1e6,
                    double(t_phase_ns[kPhasePipeline]) / 1e6, double(t_phase_ns[kPhaseImage]) / 1e6,
                    double(rex::external_frame::PresentOverlapNs(record_t0, now)) / 1e6);
      }
    }
    q.busy_seq.store(0, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(q.mutex);
      q.tail = (q.tail + h.bytes) % Threading::kRing;
      q.used -= h.bytes;
    }
    q.space_cv.notify_one();
  }
}

void QueueDraw(uint32_t device, uint32_t primitive, uint32_t count, const uint32_t index_info[4]) {
  BeginRecord(Rec::Draw);
  PutDelta(device);
  DrawRec d = {primitive, count, {index_info[0], index_info[1], index_info[2], index_info[3]}};
  Put(d);
  PutLayout(device);
  CommitRecord();
}

void QueueDrawUp(const uint8_t* device_bytes, const uint8_t* layout, uint32_t layout_bytes, bool layout_ok,
                 uint32_t primitive, uint32_t count, uint32_t dwords, const uint8_t* src, uint32_t bytes,
                 const uint8_t* isrc, uint32_t ibytes, uint32_t index_flags, uint32_t index_offset) {
  BeginRecord(Rec::DrawUp);
  PutDeltaBytes(device_bytes);  // (the device at the Begin*Vertices call: State::begin_device)
  const DrawUpRec d = {primitive, count, dwords, bytes, isrc ? ibytes : 0u, index_flags, index_offset};
  Put(d);
  Put(src, bytes);
  if (isrc) Put(isrc, ibytes);
  const uint32_t ok = layout_ok ? 1u : 0u;
  Put(ok);
  Put(layout_bytes);
  Put(layout, layout_bytes);
  CommitRecord();
}

void QueueResolve(uint32_t device, uint32_t flags, const int32_t* rect, const float* clear_color, double clear_z) {
  BeginRecord(Rec::Resolve);
  PutDelta(device);
  ResolveRec d = {};
  d.flags = flags;
  d.has_rect = rect != nullptr;
  d.has_color = clear_color != nullptr;
  for (int i = 0; i < 4; ++i) {
    if (rect) d.rect[i] = rect[i];
    if (clear_color) d.color[i] = clear_color[i];
  }
  d.clear_z = clear_z;
  Put(d);
  CommitRecord();
}

void QueueClear(const PendingClear& clear) {
  BeginRecord(Rec::Clear);
  PutDelta(clear.device);
  Put(clear);
  CommitRecord();
}

void QueueWindowScissor(uint32_t tl, uint32_t br) {
  BeginRecord(Rec::Scissor);
  PutDelta(0);
  const ScissorRec s = {tl, br};
  Put(s);
  CommitRecord();
}

}  // namespace

bool DrawThreaded() { return Threaded(); }

uint64_t LastKickoffRecord() { return q.last_kickoff.load(std::memory_order_acquire); }

uint64_t NextRecordSeq() { return q.enqueued + 1; }

void QueueKickoff() {
  ProducerTime timed;
  FlushBeginVertices();
  BeginRecord(Rec::Kickoff);
  PutDelta(0);
  q.last_kickoff.store(CommitRecord(), std::memory_order_release);
}

void QueueSwap(uint32_t device, uint32_t front_texture) {
  {
    ProducerTime timed;
    FlushBeginVertices();
    BeginRecord(Rec::Swap);
    PutDelta(device);
    const SwapRec d = {device, front_texture};
    Put(d);
    q.last_kickoff.store(CommitRecord(), std::memory_order_release);
  }
  // The game may run one frame ahead of the worker, no more (latency: a frame
  // queued behind another would show a frame late).
  const uint64_t in = q.swaps_in.fetch_add(1, std::memory_order_relaxed) + 1;
  std::unique_lock<std::mutex> lock(q.mutex);
  const auto caught_up = [in] { return q.swaps_done.load(std::memory_order_acquire) + 1 >= in; };
  for (int waited = 2; !q.swap_cv.wait_for(lock, std::chrono::seconds(2), caught_up); waited += 2) {
    WorkerStalled("the previous frame", waited);
  }
}

void StartDrawWorker() {
  if (q.on.load() || !REXCVAR_GET(native_threaded)) return;
  q.ring.reset(new uint8_t[Threading::kRing]);
  q.scratch.reserve(size_t(1) << 20);
  std::thread worker(WorkerMain);
  q.worker = worker.get_id();  // (records - and so OnWorker() - only come once `on` is set)
  worker.detach();
  q.on.store(true);
  REXLOG_INFO("[native] threaded recording on: the game's render thread records, a worker draws");
}

uint64_t ProducerNanoseconds() { return q.producer_ns.exchange(0, std::memory_order_relaxed); }

}  // namespace ng2native
