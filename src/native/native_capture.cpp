// [IzunaRecomp] Developer captures of the native renderer (native_draw_internal.h):
// resolve dumps, the pixel probe, and the background file saver.

#include "native/native_draw_internal.h"

namespace ng2native::draw {

// --- Resolve dumps (native_dump_resolves) -----------------------------------
//
// Each resolve of a logged frame is copied to a host buffer of its own, and
// written out once its frame is surely done (3 frames on, as images retire).

uint32_t BytesPerTexel(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R5G6B5_UNORM_PACK16: return 2;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT: return 4;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;
  }
}

// A mapped buffer the GPU copies into for the CPU to read.
bool CreateReadback(size_t bytes, VkBuffer& buffer, VkDeviceMemory& memory, void*& mapped) {
  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (g.vk.CreateBuffer(g.ctx.device, &bci, nullptr, &buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  g.vk.GetBufferMemoryRequirements(g.ctx.device, buffer, &req);
  VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  // (read back by the CPU: cached memory if there is any - reading the
  // write-combined kind runs at a fraction of the speed)
  mai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                           VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX) {
    mai.memoryTypeIndex =
        MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  }
  memory = VK_NULL_HANDLE;
  if (mai.memoryTypeIndex == UINT32_MAX || g.vk.AllocateMemory(g.ctx.device, &mai, nullptr, &memory) != VK_SUCCESS ||
      g.vk.BindBufferMemory(g.ctx.device, buffer, memory, 0) != VK_SUCCESS ||
      g.vk.MapMemory(g.ctx.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
    if (memory) g.vk.FreeMemory(g.ctx.device, memory, nullptr);
    g.vk.DestroyBuffer(g.ctx.device, buffer, nullptr);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

// Saved at the game's own size (1280x720 for the frame), scaled down from the
// renderer's: test50's 4K capture copied ~30 resolves of up to 66 MB each
// and the drawing waited on the storage until the game froze; the pixels
// asked of a capture are the game's (one host pixel of each guest one).
void DumpResolve(const Image& img, uint32_t dest) {
  const uint32_t bpp = BytesPerTexel(img.format);
  if (!bpp || g.resolve_dumps.size() >= 256) return;
  const float k = std::max(img.scale, 1.0f), ky = std::max(img.scale_y, 1.0f);
  const uint32_t w = std::max(ToGuest(img.width, k), 1u), h = std::max(ToGuest(img.height, ky), 1u);
  State::ResolveDump r;
  r.frame = g.frame_number;
  r.capture = g.capture_frame;
  r.bytes = size_t(w) * h * bpp;
  if (!CreateReadback(r.bytes, r.buffer, r.memory, r.mapped)) return;
  VkImage source = img.image;
  if (k != 1.0f || ky != 1.0f) {
    Image small;
    if (!CreateImage(g.cmd, small, img.format, w, h, false)) {
      g.vk.DestroyBuffer(g.ctx.device, r.buffer, nullptr);
      g.vk.FreeMemory(g.ctx.device, r.memory, nullptr);
      return;
    }
    Barrier(g.cmd);
    VkImageBlit b = {};
    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {int32_t(img.width), int32_t(img.height), 1};
    b.dstOffsets[1] = {int32_t(w), int32_t(h), 1};
    g.vk.CmdBlitImage(g.cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, small.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                      VK_FILTER_NEAREST);
    source = small.image;
    Retire(small);  // (freed once this frame is done)
  }
  Barrier(g.cmd);
  VkBufferImageCopy c = {};
  c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  c.imageExtent = {w, h, 1};
  g.vk.CmdCopyImageToBuffer(g.cmd, source, VK_IMAGE_LAYOUT_GENERAL, r.buffer, 1, &c);
  r.name = fmt::format("f{}_{:03}_{:08X}_vk{}_{}x{}.bin", g.frame_number + 1, g.resolve_dumps_in_frame++, dest,
                       int(img.format), w, h);
  g.resolve_dumps.push_back(std::move(r));
}

// Written by a thread of their own: a logged frame's ~100 MB took the drawing
// thread 2 s on the console (test24), and the game froze soon after one.
// The writer frees each buffer once written.
// At most kMaxBytes wait (the buffers are device memory): past that, on
// storage too slow to keep up, a resolve is dropped and counted - test29's
// slow storage with a logged frame a minute would otherwise grow it ~100 MB
// a minute.
struct ResolveWriter {
  static constexpr size_t kMaxBytes = 256u << 20;
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<State::ResolveDump> queue;
  size_t bytes = 0;
  uint32_t dropped = 0;
  bool started = false;
};
ResolveWriter& Resolves() {
  static ResolveWriter* w = new ResolveWriter;  // (lives as long as the app)
  return *w;
}

void ResolveWriterMain() {
  ResolveWriter& w = Resolves();
  const auto dir = rex::filesystem::GetExecutableFolder() / "native_resolves";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  uint32_t written = 0;
  std::unique_lock<std::mutex> lock(w.mutex);
  for (;;) {
    w.cv.wait(lock, [&] { return !w.queue.empty(); });
    State::ResolveDump r = std::move(w.queue.front());
    w.queue.pop_front();
    const bool last = w.queue.empty();
    const uint32_t dropped = last ? std::exchange(w.dropped, 0) : 0;
    lock.unlock();
    {
      std::ofstream out(dir / r.name, std::ios::binary);
      out.write(static_cast<const char*>(r.mapped), std::streamsize(r.bytes));
      if (!out) REXLOG_WARN("[native] could not write native_resolves/{}", r.name);
    }
    g.vk.DestroyBuffer(g.ctx.device, r.buffer, nullptr);
    g.vk.FreeMemory(g.ctx.device, r.memory, nullptr);
    ++written;
    if (last) {
      REXLOG_INFO("[native] {} resolves saved to native_resolves/{}", written,
                  dropped ? fmt::format(" ({} dropped: the storage was too slow)", dropped) : "");
      written = 0;
    }
    lock.lock();
    w.bytes -= std::min(w.bytes, r.bytes);
    w.cv.notify_all();  // (a capture's resolve may wait for room)
  }
}

void WriteResolveDumps(bool all) {
  ResolveWriter& w = Resolves();
  size_t keep = 0;
  for (size_t i = 0; i < g.resolve_dumps.size(); ++i) {
    auto& r = g.resolve_dumps[i];
    if (!all && r.frame + 3 > g.frame_number) {
      if (keep != i) g.resolve_dumps[keep] = std::move(r);  // (never onto itself: that empties the name)
      ++keep;
      continue;
    }
    std::unique_lock<std::mutex> lock(w.mutex);
    if (!w.started) {
      w.started = true;
      std::thread(ResolveWriterMain).detach();
    }
    // (A capture's resolves no longer wait for room: at the game's size a
    // frame's fit the backlog, and a wait held the drawing - test50's freeze.)
    if (w.bytes + r.bytes > ResolveWriter::kMaxBytes) {
      g.vk.DestroyBuffer(g.ctx.device, r.buffer, nullptr);
      g.vk.FreeMemory(g.ctx.device, r.memory, nullptr);
      ++w.dropped;
      continue;
    }
    w.bytes += r.bytes;
    w.queue.push_back(std::move(r));
    w.cv.notify_one();
  }
  g.resolve_dumps.resize(keep);
}

// [dev] native_probe "x,y": the guest pixel's colour (first colour target)
// and depth after each draw of a logged frame - a pixel history, for the
// draw that makes a pixel differ between two frames (with native_log_count
// 2+, the draw numbers match the log's "draw:" lines). Each sample ends the
// render pass, so logged frames only.
constexpr uint32_t kProbeSamples = 4096, kProbeStride = 32;  // (colour at +0, depth at +16)

void ProbeAfterDraw(const Image* color, const Image* depth, uint32_t target, uint64_t ps, uint32_t blend) {
  static const std::pair<int, int> at = [] {
    int x = -1, y = -1;
    if (std::sscanf(REXCVAR_GET(native_probe).c_str(), "%d,%d", &x, &y) != 2) x = y = -1;
    return std::pair<int, int>{x, y};
  }();
  auto& p = g.probe;
  if (at.first < 0 || at.second < 0 || p.samples.size() >= kProbeSamples) return;
  if (!p.buffer && !CreateReadback(kProbeSamples * kProbeStride, p.buffer, p.memory, p.mapped)) return;
  const VkDeviceSize offset = VkDeviceSize(p.samples.size()) * kProbeStride;
  State::ProbeSample s;
  s.frame = g.frame_number;
  s.draw = g.frame_drawn;
  s.target = target;
  s.ps = ps;
  s.blend = blend;
  EndRendering();
  auto copy = [&](const Image* img, VkImageAspectFlags aspect, VkDeviceSize at_offset) {
    const uint32_t x = ToHost(uint32_t(at.first), img->scale), y = ToHost(uint32_t(at.second), img->scale_y);
    if (x >= img->width || y >= img->height) return false;
    VkBufferImageCopy c = {};
    c.bufferOffset = at_offset;
    c.imageSubresource = {aspect, 0, 0, 1};
    c.imageOffset = {int32_t(x), int32_t(y), 0};
    c.imageExtent = {1, 1, 1};
    g.vk.CmdCopyImageToBuffer(g.cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, p.buffer, 1, &c);
    return true;
  };
  if (color && BytesPerTexel(color->format) && copy(color, VK_IMAGE_ASPECT_COLOR_BIT, offset)) {
    s.format = color->format;
    s.width = color->width;
  }
  s.depth = depth && copy(depth, VK_IMAGE_ASPECT_DEPTH_BIT, offset + 16);
  if (!s.format && !s.depth) return;  // (a target the pixel is outside of)
  Barrier(g.cmd);  // (the next draw writes what was just read)
  p.samples.push_back(s);
}

std::string ProbeColor(VkFormat format, const uint8_t* b) {
  auto half = [](uint16_t h) {
    const uint32_t e = (h >> 10) & 31, m = h & 1023;
    const float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(m | 1024), int(e) - 25);
    return (h & 0x8000) ? -v : v;
  };
  uint16_t h[4];
  float f[4];
  std::memcpy(h, b, 8);
  std::memcpy(f, b, 16);
  switch (format) {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
      return fmt::format("({:.4g} {:.4g} {:.4g} {:.4g})", half(h[0]), half(h[1]), half(h[2]), half(h[3]));
    case VK_FORMAT_R16G16_SFLOAT: return fmt::format("({:.4g} {:.4g})", half(h[0]), half(h[1]));
    case VK_FORMAT_R32_SFLOAT: return fmt::format("({:.6g})", f[0]);
    case VK_FORMAT_R32G32_SFLOAT: return fmt::format("({:.6g} {:.6g})", f[0], f[1]);
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM:
      return fmt::format("({} {} {} {})", b[0], b[1], b[2], b[3]);
    default: return fmt::format("{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}", b[0], b[1], b[2], b[3], b[4],
                                b[5], b[6], b[7]);
  }
}

// The samples, once their frames' GPU work is surely done: each draw that
// changed the pixel of its target.
void LogProbe() {
  auto& p = g.probe;
  if (p.samples.empty() || g.log_frame || p.samples.back().frame + 3 > g.frame_number) return;
  std::map<uint64_t, std::array<uint8_t, 20>> last;  // (per target image: the pixel before)
  uint64_t frame = UINT64_MAX;
  for (size_t i = 0; i < p.samples.size(); ++i) {
    const State::ProbeSample& s = p.samples[i];
    if (s.frame != frame) last.clear(), frame = s.frame;
    const uint8_t* b = static_cast<const uint8_t*>(p.mapped) + i * kProbeStride;
    std::array<uint8_t, 20> v{};
    std::memcpy(v.data(), b, 8);
    if (s.format) std::memcpy(v.data(), b, std::min<uint32_t>(16, BytesPerTexel(s.format)));
    if (s.depth) std::memcpy(v.data() + 16, b + 16, 4);
    const uint64_t key = uint64_t(s.target) | uint64_t(s.width) << 12 | uint64_t(s.format) << 32;
    auto it = last.find(key);
    if (it != last.end() && it->second == v) continue;
    last[key] = v;
    float z = 0.0f;
    std::memcpy(&z, b + 16, 4);
    REXLOG_INFO("[native] probe frame {} draw {} PS {:016X} blend {:08X} target {:03X} ({} wide): {}{}", s.frame + 1,
                s.draw, s.ps, s.blend, s.target, s.width, s.format ? ProbeColor(s.format, b) : std::string("-"),
                s.depth ? fmt::format(" z {:.9g}", z) : std::string());
  }
  p.samples.clear();
}

// The pipeline files are written by a thread of their own, newest content
// per file: written in place every ~10 s, one save behind saved frames on
// the console's busy storage held the drawing thread 5 s (test29's freezes).
struct FileSaver {
  std::mutex mutex;
  std::condition_variable cv;
  // (each file's content, or what makes it there - the pipeline cache's
  // serialisation is ~100 ms of the drawing thread on the console, test29)
  std::vector<std::pair<std::filesystem::path, std::function<std::vector<char>()>>> pending;
  bool started = false;
};
FileSaver& Saver() {
  static FileSaver* s = new FileSaver;  // (lives as long as the app)
  return *s;
}

void FileSaverMain() {
  FileSaver& s = Saver();
  std::unique_lock<std::mutex> lock(s.mutex);
  for (;;) {
    s.cv.wait(lock, [&] { return !s.pending.empty(); });
    auto item = std::move(s.pending.front());
    s.pending.erase(s.pending.begin());
    lock.unlock();
    const std::vector<char> data = item.second();
    if (data.empty()) {
      lock.lock();
      continue;
    }
    std::error_code ec;
    std::filesystem::create_directories(item.first.parent_path(), ec);
    bool written;
    {
      std::ofstream f(item.first.string() + ".tmp", std::ios::binary | std::ios::trunc);
      f.write(data.data(), std::streamsize(data.size()));
      f.close();
      written = !f.fail();
    }
    // [IzunaRecomp] Only a whole file replaces the old one (a full disk wrote
    // part of it and the part became the cache).
    if (written) {
      std::filesystem::rename(item.first.string() + ".tmp", item.first, ec);
    } else {
      std::filesystem::remove(item.first.string() + ".tmp", ec);
    }
    lock.lock();
  }
}

// Queues a whole file's new content, made on the saver thread (replacing any
// not yet written; empty: nothing written).
void SaveFileLater(std::filesystem::path path, std::function<std::vector<char>()> data) {
  FileSaver& s = Saver();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.started) {
    try {
      std::thread(FileSaverMain).detach();
    } catch (const std::exception&) {
      return;
    }
    s.started = true;
  }
  for (auto& item : s.pending) {
    if (item.first == path) {
      item.second = std::move(data);
      return;
    }
  }
  s.pending.emplace_back(std::move(path), std::move(data));
  s.cv.notify_one();
}

}  // namespace ng2native::draw
