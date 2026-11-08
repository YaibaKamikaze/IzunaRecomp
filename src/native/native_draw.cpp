// [native renderer, M3] The game's Direct3D draws, drawn natively.
//
// Ninja Gaiden II's statically linked Direct3D (XDK 2.0.6683) keeps every
// piece of GPU state in a shadow inside its device object (guest 0x40001E80)
// and copies it into the command buffer at each draw. The draw hooks read
// that shadow - the shaders, the ALU and fetch constants, the render state
// registers - and record the draw with Vulkan:
//   - shaders: the game's own, translated ahead of time by our XenosRecomp
//     fork (out/native-shaders/shader_cache.cpp), found by the XXH3 hash of
//     the container D3D_CreateShaderFromContainer was given;
//   - vertices and indices: read by the vertex shader straight from the
//     imported guest memory (ng2VertexFetch / ng2VertexIndex), big-endian as
//     the game wrote them - no vertex or index buffers, no copies;
//   - render targets: one image per EDRAM surface (base, format, pitch);
//   - resolves (EDRAM -> texture copies): copied image to image, the copy
//     remembered by its destination address so later draws (and the swap)
//     that read that texture get the image.
//   - textures: decoded from guest memory (untiled, endian-swapped) into
//     images, cached by fetch constant and re-uploaded when their bytes
//     change; with their mips, cube maps, and 1-texel-high ones also as 1D
//     (NG2 samples no 3D texture).
//
// Everything stays in VK_IMAGE_LAYOUT_GENERAL, with one full memory barrier
// between passes: simple first, fast later.

#include "native/native_draw_internal.h"
#include "izuna/izuna_shaders.h"

REXCVAR_DEFINE_INT32(native_log_frame, 0, "Native renderer",
                     "[native renderer] Log every draw and resolve of this native frame (diagnosis), 0 = off");

REXCVAR_DEFINE_INT32(native_log_count, 1, "Native renderer",
                     "[native renderer] How many consecutive frames native_log_frame logs (flicker: 2+)")
    .range(1, 8);

REXCVAR_DEFINE_INT32(native_debug, 0, "Native renderer",
                     "[native renderer] Diagnosis bits: 1 skip draws, 2 show the first colour target instead of the "
                     "front buffer, 4 skip resolves");


REXCVAR_DEFINE_DOUBLE(native_resolution_scale, 1.0, "Native renderer",
                      "[native renderer] Draw at this multiple of the game's 1280x720 (1 720p, 1.5 1080p, 2 1440p, "
                      "3 4K; the hub's resolution setting sets it)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// [IzunaRecomp] (test12 wrote the coverage into the target's alpha - NG2
// keeps data there that bloom and exposure read: every scene broke; test13-14
// had it off. Now the coverage is a second blend source.)
REXCVAR_DEFINE_BOOL(native_alpha_to_mask_blend, true, "Native renderer",
                    "[IzunaRecomp] Alpha to mask on 2x MSAA surfaces as the resolve shows it: colour and alpha "
                    "blended by the samples' coverage (dual-source), not an on/off dither");
#if defined(__PROSPERO__)
#define IZUNA_SCENE_FIT_DEFAULT false  // (the console's 1440p as it was: the scene fit costs ~40% more pixels)
#else
#define IZUNA_SCENE_FIT_DEFAULT true
#endif
REXCVAR_DEFINE_BOOL(native_scene_fit, IZUNA_SCENE_FIT_DEFAULT, "Native renderer",
                    "[IzunaRecomp] Draw the game's 1120x584 3D scene at the size its final stretch maps 1:1 to "
                    "the output (1920x1080 at 1080p), not at the output's scale and stretched")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(native_fractional_min_pitch, 512, "Native renderer",
                     "[IzunaRecomp] At a fractional scale (1.5x), surfaces narrower than this stay 1x")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(native_auto_log, 1, "Native renderer",
                     "[native renderer] Log the draws of heavy 3D frames: 0 off, 1 the first two, 2 one a minute");
REXCVAR_DEFINE_INT32(native_anisotropic, -1, "Native renderer",
                     "[native renderer] Anisotropic filtering for every texture: -1 the game's own, 0 = 1x, 1 = 2x, "
                     "2 = 4x, 3 = 8x, 4 = 16x")
    .range(-1, 4)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(native_dump_textures, false, "Native renderer",
                    "[native renderer] Save each newly decoded texture once (native_textures/ beside the app; "
                    "tools/native/tex2png.py)");
REXCVAR_DEFINE_STRING(native_log_on_ps, "", "Native renderer",
                      "[native renderer, test] Log a frame 30 frames after the first draw of this pixel shader (hex)");
REXCVAR_DEFINE_INT32(native_log_on_delay, 30, "Native renderer",
                     "[native renderer, test] Frames from native_log_on_ps / _tex's first sight to the logged frame");
REXCVAR_DEFINE_STRING(native_log_on_tex, "", "Native renderer",
                      "[native renderer, test] As native_log_on_ps, on the first draw whose texture slot 0 is at "
                      "this guest address (hex)");
REXCVAR_DEFINE_STRING(native_watch_tex, "", "Native renderer",
                      "[native renderer, test] Note when a draw first uses this texture (slot 0, hex guest address) - "
                      "the host test's pad script \"untiltex\" waits for it");
REXCVAR_DEFINE_STRING(native_probe, "", "Native renderer",
                      "[native renderer, diagnosis] \"x,y\": log this guest pixel's colour and depth after each draw "
                      "of the logged frames (a pixel history)");
REXCVAR_DEFINE_BOOL(native_dump_resolves, false, "Native renderer",
                    "[native renderer] Save every resolve of each logged frame (native_auto_log / native_log_frame) "
                    "to native_resolves/ beside the app (tools/native/tex2png.py)");
REXCVAR_DEFINE_BOOL(native_ignore_pipeline_cache, false, "Native renderer",
                    "[native renderer] Build every pipeline afresh (ignore UserData/native_pipelines.bin)");
REXCVAR_DEFINE_INT32(native_pipeline_threads, 3, "Native renderer",
                     "[native renderer] Threads building pipelines (1-6)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(native_pipeline_libraries, true, "Native renderer",
                    "[native renderer] Link pipelines from per-shader parts (VK_EXT_graphics_pipeline_library) when "
                    "the driver has them: a new render state costs a link, not a compile")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(native_threaded, false, "Native renderer",
                    "[native renderer] The game's render thread only records each Direct3D event; a worker thread "
                    "turns them into Vulkan commands")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(native_precompile_pipelines, true, "Native renderer",
                    "[native renderer] At start, build every pipeline earlier runs used "
                    "(UserData/native_pipeline_keys.bin), so they are ready before the game draws with them")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(native_texture_recheck_frames, 8, "Native renderer",
                     "[native renderer] Look for changed bytes of a texture that never changed every N frames "
                     "(1: every frame; one that changed once is checked every frame)");
REXCVAR_DEFINE_INT32(native_texture_idle_frames, 1200, "Native renderer",
                     "[native renderer] Drop a texture not bound for this many frames (decoded again when used)");
REXCVAR_DEFINE_INT32(native_capture_after_seconds, 0, "Native renderer",
                     "[native renderer] Capture two frames (as the overlay's frame report) this many seconds after "
                     "the first native frame; 0 off (the hub's Developer > Frame report)");

namespace ng2native {
namespace draw {

// The rectangle-list geometry shader and the 20e4 depth-only pixel shader
// (shaders/rectlist.geom, shaders/depth24.frag), as SPIR-V.
constexpr uint32_t kRectListGs[] = {
#include "shaders/rectlist_gs.inc"
};
constexpr uint32_t kDepth24Fs[] = {
#include "shaders/depth24_fs.inc"
};

State g;
std::atomic<uint32_t> g_capture_frames{0};
std::atomic<rex::graphics::GuestTextureLoadHook> g_texture_load_hook{nullptr};
Timing t_timing;
std::atomic<uint64_t> g_image_bytes{0};

// --- Setup helpers ----------------------------------------------------------

uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties props;
  g.vk.GetPhysicalDeviceMemoryProperties(g.ctx.physical_device, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) return i;
  }
  return UINT32_MAX;
}

void Barrier(VkCommandBuffer cmd) {
  VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  g.vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0,
                          nullptr, 0, nullptr);
}

// --- Image memory -------------------------------------------------------------
//
// Images take their memory from 64 MB blocks: a vkAllocateMemory per image
// cost ~1 ms, and while an area streams in a new texture arrives about every
// frame. Ranges are first-fit, freed ones merged with their neighbours; an
// image over half a block gets an allocation of its own. [IzunaRecomp] (Half,
// not a quarter, since test25: at 1.5x the scene's 2x MSAA targets are 17-30
// MB, and an area's first frame made several at once, each with its own
// 15-40 ms allocation - test23's 85 ms frame at chapter 1's start. Targets
// live for the session, so they fragment nothing.)

constexpr VkDeviceSize kMemBlockSize = VkDeviceSize(64) << 20;

struct MemRange {
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize offset = 0, size = 0;
  int32_t block = -1;  // -1: an allocation of its own
};

// [IzunaRecomp] A spare block, allocated ahead on a thread of its own: a
// 64 MB vkAllocateMemory takes 15-40 ms on NVIDIA's Windows driver, and
// opening a block on the worker in the middle of a frame (an area's or a
// cutscene's first frames, with new targets and textures at once) held the
// frame and the game's fences behind it (izuna-windows-test23: 17 ms in the
// allocation, then eight fences at the 30 ms fallback and a 203 ms frame).
// The next block is ready before it is needed; the worker never allocates one.
// Per memory type (a depth target's memory type may not be a texture's).
// [IzunaRecomp] test28: and a reserve allocated before the first frame - a
// spare allocated on its own thread during play still held the worker 30 ms
// (test27: the driver serialises the allocation against the worker's image
// creation; 49 fences at the 30 ms fallback in one frame). A session's images
// fit the reserve (test23-27: 320-370 MB), so play allocates none; past it the
// spare takes over.
// [IzunaRecomp] test42: on a PC, by the card's video memory - an eighth of
// the heap, 512 MB to 1.5 GB (issue #1's 1440p session held 676 MB of images
// within a minute and went past the old fixed 512 MB, back to allocating in
// play; at 4K they take more).
#if defined(__PROSPERO__)
int ReserveBlocks(uint32_t) { return 1; }  // (the PS5's memory is the game's too)
#else
int ReserveBlocks(uint32_t type) {
  VkPhysicalDeviceMemoryProperties props;
  g.vk.GetPhysicalDeviceMemoryProperties(g.ctx.physical_device, &props);
  if (type >= props.memoryTypeCount) return 8;
  const VkDeviceSize heap = props.memoryHeaps[props.memoryTypes[type].heapIndex].size;
  return int(std::clamp<VkDeviceSize>(heap / 8 / kMemBlockSize, 8, 24));
}
#endif
struct SpareBlocks {
  std::mutex mutex;
  std::vector<VkDeviceMemory> memory[VK_MAX_MEMORY_TYPES];
  bool pending[VK_MAX_MEMORY_TYPES] = {};
};
SpareBlocks g_spare_blocks;

VkDeviceMemory AllocateBlock(uint32_t type) {
  VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = kMemBlockSize;
  mai.memoryTypeIndex = type;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  if (g.vk.AllocateMemory(g.ctx.device, &mai, nullptr, &memory) != VK_SUCCESS) memory = VK_NULL_HANDLE;
  return memory;
}

void PrepareSpareBlock(uint32_t type) {
  if (type >= VK_MAX_MEMORY_TYPES) return;
  {
    std::lock_guard<std::mutex> lock(g_spare_blocks.mutex);
    if (g_spare_blocks.pending[type] || !g_spare_blocks.memory[type].empty()) return;
    g_spare_blocks.pending[type] = true;
  }
  std::thread([type] {
    const VkDeviceMemory memory = AllocateBlock(type);
    std::lock_guard<std::mutex> lock(g_spare_blocks.mutex);
    if (memory) g_spare_blocks.memory[type].push_back(memory);
    g_spare_blocks.pending[type] = false;
  }).detach();
}

VkDeviceMemory TakeSpareBlock(uint32_t type) {
  if (type >= VK_MAX_MEMORY_TYPES) return VK_NULL_HANDLE;
  std::lock_guard<std::mutex> lock(g_spare_blocks.mutex);
  if (g_spare_blocks.memory[type].empty()) return VK_NULL_HANDLE;
  const VkDeviceMemory memory = g_spare_blocks.memory[type].back();
  g_spare_blocks.memory[type].pop_back();
  return memory;
}

// The reserve, allocated before the first frame, for each memory type a
// texture and a depth target take (two tiny images, asked and destroyed).
void PrewarmImageMemory() {
  uint32_t done[2] = {UINT32_MAX, UINT32_MAX};
  for (const bool depth : {false, true}) {
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = depth ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {64, 64, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    if (g.vk.CreateImage(g.ctx.device, &ici, nullptr, &image) != VK_SUCCESS) continue;
    VkMemoryRequirements req;
    g.vk.GetImageMemoryRequirements(g.ctx.device, image, &req);
    g.vk.DestroyImage(g.ctx.device, image, nullptr);
    const uint32_t type = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX || type >= VK_MAX_MEMORY_TYPES || type == done[0]) continue;
    done[depth ? 1 : 0] = type;
    int made = 0;
    const int blocks = depth ? 1 : ReserveBlocks(type);
    for (int i = 0; i < blocks; ++i) {
      const VkDeviceMemory memory = AllocateBlock(type);
      if (!memory) break;
      std::lock_guard<std::mutex> lock(g_spare_blocks.mutex);
      g_spare_blocks.memory[type].push_back(memory);
      ++made;
    }
    REXLOG_INFO("[native] image memory: {} block(s) of {} MB reserved ahead for memory type {} ({})", made,
                kMemBlockSize >> 20, type, depth ? "depth targets" : "textures and targets");
  }
}

bool CarveFrom(State::MemBlock& b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize* out) {
  for (auto it = b.free.begin(); it != b.free.end(); ++it) {
    const VkDeviceSize start = it->first, end = it->first + it->second;
    const VkDeviceSize at = (start + align - 1) / align * align;
    if (at + size > end) continue;
    b.free.erase(it);
    if (at > start) b.free.emplace(start, at - start);
    if (at + size < end) b.free.emplace(at + size, end - (at + size));
    *out = at;
    return true;
  }
  return false;
}

MemRange AllocImageMemory(const VkMemoryRequirements& req) {
  MemRange r;
  const uint32_t type = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (type == UINT32_MAX) return r;
  r.size = req.size;
  if (req.size > kMemBlockSize / 2) {
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    const auto t0 = std::chrono::steady_clock::now();
    if (g.vk.AllocateMemory(g.ctx.device, &mai, nullptr, &r.memory) != VK_SUCCESS) r.memory = VK_NULL_HANDLE;
    static int logged = 0;  // (the ones still made in a frame: a hitch's suspect)
    if (logged < 8) {
      ++logged;
      REXLOG_INFO("[native] image memory: a {} MB image's own allocation took {:.1f} ms", req.size >> 20,
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    return r;
  }
  const VkDeviceSize align = std::max<VkDeviceSize>(req.alignment, 256);
  for (size_t i = 0; i < g.blocks.size(); ++i) {
    if (g.blocks[i].type == type && CarveFrom(g.blocks[i], req.size, align, &r.offset)) {
      r.memory = g.blocks[i].memory;
      r.block = int32_t(i);
      return r;
    }
  }
  State::MemBlock b;
  b.type = type;
  b.memory = TakeSpareBlock(type);
  const bool spare = b.memory != VK_NULL_HANDLE;
  if (!spare) {
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = kMemBlockSize;
    mai.memoryTypeIndex = type;
    if (g.vk.AllocateMemory(g.ctx.device, &mai, nullptr, &b.memory) != VK_SUCCESS) return MemRange{};
  }
  PrepareSpareBlock(type);  // (the next one, ahead of its need - only once the reserve is used up)
  b.free.emplace(0, kMemBlockSize);
  g.blocks.push_back(std::move(b));
  State::MemBlock& nb = g.blocks.back();
  CarveFrom(nb, req.size, align, &r.offset);
  r.memory = nb.memory;
  r.block = int32_t(g.blocks.size() - 1);
  REXLOG_INFO("[native] image memory: block {} ({} MB in blocks){}", g.blocks.size(),
              (g.blocks.size() * kMemBlockSize) >> 20, spare ? " - from the reserve, allocated ahead" : "");
  return r;
}

void FreeImageMemory(VkDeviceMemory memory, int32_t block, VkDeviceSize offset, VkDeviceSize size) {
  if (!memory) return;
  if (block < 0) {
    g.vk.FreeMemory(g.ctx.device, memory, nullptr);
    return;
  }
  auto& free = g.blocks[size_t(block)].free;
  auto it = free.emplace(offset, size).first;
  if (auto next = std::next(it); next != free.end() && it->first + it->second == next->first) {
    it->second += next->second;
    free.erase(next);
  }
  if (it != free.begin()) {
    auto prev = std::prev(it);
    if (prev->first + prev->second == it->first) {
      prev->second += it->second;
      free.erase(it);
    }
  }
}

// An image in GENERAL layout (transitioned in `cmd`).
bool CreateImage(VkCommandBuffer cmd, Image& out, VkFormat format, uint32_t w, uint32_t h, bool depth,
                 VkImageViewType view_type, VkComponentMapping components, uint32_t mips,
                 VkImageUsageFlags extra_usage) {
  const PhaseTimer phase(kPhaseImage);
  const bool cube = view_type == VK_IMAGE_VIEW_TYPE_CUBE;
  VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.flags = (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) |
              (format == VK_FORMAT_R8G8B8A8_UNORM ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0);  // (sRGB views)
  ici.imageType = view_type == VK_IMAGE_VIEW_TYPE_3D   ? VK_IMAGE_TYPE_3D
                  : view_type == VK_IMAGE_VIEW_TYPE_1D ? VK_IMAGE_TYPE_1D
                                                       : VK_IMAGE_TYPE_2D;
  ici.format = format;
  ici.extent = {w, h, 1};
  ici.mipLevels = mips;
  ici.arrayLayers = cube ? 6 : 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  const bool compressed = format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK;
  ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
              (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                     : (view_type == VK_IMAGE_VIEW_TYPE_2D && !compressed ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0)) |
              extra_usage;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  StageClock part_clock;
  auto part = [&](int i) {
    if (!g_timed) return;
    const uint64_t now = Ticks();
    t_timing.create_part_ns[i] += TicksToNs(now - part_clock.t);
    part_clock.t = now;
  };
  if (g.vk.CreateImage(g.ctx.device, &ici, nullptr, &out.image) != VK_SUCCESS) return false;
  part(0);
  VkMemoryRequirements req;
  g.vk.GetImageMemoryRequirements(g.ctx.device, out.image, &req);
  const MemRange mem = AllocImageMemory(req);
  if (!mem.memory || g.vk.BindImageMemory(g.ctx.device, out.image, mem.memory, mem.offset) != VK_SUCCESS) {
    FreeImageMemory(mem.memory, mem.block, mem.offset, mem.size);
    g.vk.DestroyImage(g.ctx.device, out.image, nullptr);
    out.image = VK_NULL_HANDLE;
    return false;
  }
  g_image_bytes.fetch_add(req.size, std::memory_order_relaxed);
  out.memory = mem.memory;
  out.block = mem.block;
  out.mem_offset = mem.offset;
  out.mem_size = mem.size;
  out.bytes = req.size;
  part(1);
  const VkImageAspectFlags aspect =
      depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = out.image;
  vci.viewType = view_type;
  vci.format = format;
  vci.components = components;
  vci.subresourceRange = {depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : aspect, 0, mips, 0, cube ? 6u : 1u};
  if (g.vk.CreateImageView(g.ctx.device, &vci, nullptr, &out.view) != VK_SUCCESS) return false;
  part(2);
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = out.image;
  b.subresourceRange = {aspect, 0, mips, 0, cube ? 6u : 1u};
  g.vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                          0, nullptr, 1, &b);
  part(3);
  out.format = format;
  out.width = w;
  out.height = h;
  out.depth = depth;
  return true;
}

void WriteTextureDescriptor(uint32_t set, uint32_t index, VkImageView view) {
  VkDescriptorImageInfo ii = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = g.sets[set];
  w.dstBinding = 0;
  w.dstArrayElement = index;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  w.pImageInfo = &ii;
  g.vk.UpdateDescriptorSets(g.ctx.device, 1, &w, 0, nullptr);
}

// Hand out / give back slots in the 2D and 1D texture tables.
uint32_t AllocDescriptor(bool one_d) {
  auto& free = one_d ? g.free_descriptors_1d : g.free_descriptors;
  if (!free.empty()) {
    const uint32_t d = free.back();
    free.pop_back();
    return d;
  }
  uint32_t& next = one_d ? g.next_descriptor_1d : g.next_descriptor;
  return next < (one_d ? 1024u : kMaxTextures) ? next++ : 0;
}

// The cube table (set 2): its own slots, recycled like the others.
uint32_t AllocCubeDescriptor() {
  if (!g.free_descriptors_cube.empty()) {
    const uint32_t d = g.free_descriptors_cube.back();
    g.free_descriptors_cube.pop_back();
    return d;
  }
  return g.next_descriptor_cube < 1024 ? g.next_descriptor_cube++ : 0;
}

// Destroy an image once the frames in flight are done with it.
void Retire(Image& img) {
  if (!img.image) return;
  if (img.descriptor) g.retired_descriptors.push_back({g.frame_number, img.descriptor});
  if (img.descriptor_1d) g.retired_descriptors_1d.push_back({g.frame_number, img.descriptor_1d});
  if (img.descriptor_srgb) g.retired_descriptors.push_back({g.frame_number, img.descriptor_srgb});
  if (img.descriptor_cube) g.retired_descriptors_cube.push_back({g.frame_number, img.descriptor_cube});
  g.retired.push_back({g.frame_number, img});
  img = Image{};
}


void CollectRetired() {
  const uint64_t safe = g.frame_number >= 3 ? g.frame_number - 3 : 0;
  size_t keep = 0;
  for (auto& r : g.retired) {
    if (r.frame > safe) {
      g.retired[keep++] = r;
      continue;
    }
    Image& i = r.image;
    if (i.view) g.vk.DestroyImageView(g.ctx.device, i.view, nullptr);
    if (i.view_srgb) g.vk.DestroyImageView(g.ctx.device, i.view_srgb, nullptr);
    if (i.image) g.vk.DestroyImage(g.ctx.device, i.image, nullptr);
    FreeImageMemory(i.memory, i.block, i.mem_offset, i.mem_size);
    if (i.view_1d) g.vk.DestroyImageView(g.ctx.device, i.view_1d, nullptr);
    if (i.image_1d) g.vk.DestroyImage(g.ctx.device, i.image_1d, nullptr);
    FreeImageMemory(i.memory_1d, i.block_1d, i.mem_offset_1d, i.mem_size_1d);
    g_image_bytes.fetch_sub(i.bytes, std::memory_order_relaxed);
  }
  g.retired.resize(keep);
  auto release = [&](std::vector<std::pair<uint64_t, uint32_t>>& from, std::vector<uint32_t>& to, uint32_t set) {
    size_t k = 0;
    for (auto& [frame, d] : from) {
      if (frame > safe) {
        from[k++] = {frame, d};
        continue;
      }
      // (pointed back at the placeholder before it is handed out again)
      if (g.placeholders.size() == 4) WriteTextureDescriptor(set, d, g.placeholders[set == 4 ? 3 : set == 2 ? 2 : 0].view);
      to.push_back(d);
    }
    from.resize(k);
  };
  release(g.retired_descriptors, g.free_descriptors, 0);
  release(g.retired_descriptors_1d, g.free_descriptors_1d, 4);
  release(g.retired_descriptors_cube, g.free_descriptors_cube, 2);
  size_t k = 0;
  for (auto& [frame, a] : g.retired_arenas) {
    if (frame > safe) {
      g.retired_arenas[k++] = {frame, a};
      continue;
    }
    g.free_arenas.push_back(a);  // [IzunaRecomp] (kept for the next: an allocation in play stalls the worker)
  }
  g.retired_arenas.resize(k);
}

// Textures not bound for ~20 s are dropped (decoded again if used again).
void EvictTextures() {
  const uint64_t kIdleFrames = uint64_t(std::max(REXCVAR_GET(native_texture_idle_frames), 2));
  for (auto it = g.textures.begin(); it != g.textures.end();) {
    if (g.frame_number - it->second.last_used > kIdleFrames) {
      Retire(it->second.image);
      it = g.textures.erase(it);
    } else {
      ++it;
    }
  }
}

bool CreateLayout() {
  // Sets as the translated shaders declare them (shader_common.h): space0
  // Texture2D[], space1 Texture3D[], space2 TextureCube[], space3
  // SamplerState[], space4 Texture1D[] - each binding 0, bindless.
  for (uint32_t s = 0; s < 5; ++s) {
    VkDescriptorSetLayoutBinding b = {};
    b.binding = 0;
    b.descriptorType = s == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b.descriptorCount = s == 3 ? kMaxSamplers : (s == 0 ? kMaxTextures : (s == 4 || s == 2) ? 1024 : 16);
    b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorBindingFlags flags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bf.bindingCount = 1;
    bf.pBindingFlags = &flags;
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.pNext = &bf;
    ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    ci.bindingCount = 1;
    ci.pBindings = &b;
    if (g.vk.CreateDescriptorSetLayout(g.ctx.device, &ci, nullptr, &g.set_layouts[s]) != VK_SUCCESS) return false;
  }
  VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 5 * sizeof(uint64_t)};
  VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  li.setLayoutCount = 5;
  li.pSetLayouts = g.set_layouts.data();
  li.pushConstantRangeCount = 1;
  li.pPushConstantRanges = &pc;
  if (g.vk.CreatePipelineLayout(g.ctx.device, &li, nullptr, &g.layout) != VK_SUCCESS) return false;
  VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures + 2048 + 32},
                                  {VK_DESCRIPTOR_TYPE_SAMPLER, kMaxSamplers}};
  VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  pi.maxSets = 5;
  pi.poolSizeCount = 2;
  pi.pPoolSizes = sizes;
  if (g.vk.CreateDescriptorPool(g.ctx.device, &pi, nullptr, &g.pool) != VK_SUCCESS) return false;
  VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = g.pool;
  ai.descriptorSetCount = 5;
  ai.pSetLayouts = g.set_layouts.data();
  if (g.vk.AllocateDescriptorSets(g.ctx.device, &ai, g.sets.data()) != VK_SUCCESS) return false;
  VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_LINEAR;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.maxLod = VK_LOD_CLAMP_NONE;
  if (g.vk.CreateSampler(g.ctx.device, &si, nullptr, &g.sampler) != VK_SUCCESS) return false;
  VkDescriptorImageInfo ii = {g.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
  VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = g.sets[3];
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  w.pImageInfo = &ii;
  g.vk.UpdateDescriptorSets(g.ctx.device, 1, &w, 0, nullptr);
  return true;
}

// A host-written buffer the shaders read by device address.
bool CreateMappedBuffer(VkDeviceSize size, VkBufferUsageFlags usage, State::MappedBuffer& out) {
  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  if (g.vk.CreateBuffer(g.ctx.device, &bci, nullptr, &out.buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  g.vk.GetBufferMemoryRequirements(g.ctx.device, out.buffer, &req);
  VkMemoryAllocateFlagsInfo flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.pNext = &flags;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex =
      MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX || g.vk.AllocateMemory(g.ctx.device, &mai, nullptr, &out.memory) != VK_SUCCESS ||
      g.vk.BindBufferMemory(g.ctx.device, out.buffer, out.memory, 0) != VK_SUCCESS ||
      g.vk.MapMemory(g.ctx.device, out.memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&out.mapped)) !=
          VK_SUCCESS) {
    if (out.memory) g.vk.FreeMemory(g.ctx.device, out.memory, nullptr);
    g.vk.DestroyBuffer(g.ctx.device, out.buffer, nullptr);
    out = {};
    return false;
  }
  VkBufferDeviceAddressInfo ai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  ai.buffer = out.buffer;
  out.address = g.vk.GetBufferDeviceAddress(g.ctx.device, &ai);
  return true;
}

bool CreateUpload() {
  State::MappedBuffer upload;
  if (!CreateMappedBuffer(kFrameUpload * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, upload)) return false;
  g.upload = upload.buffer;
  g.upload_mapped = upload.mapped;
  g.upload_address = upload.address;
  // The depth resolve scratch buffer: one whole target (pitch x kRtHeight, 4 bytes).
  VkBufferCreateInfo sci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  sci.size = VkDeviceSize(ToHostSize(2048u, RenderScale())) * ToHostSize(kRtHeight, RenderScale()) * 4;
  sci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (g.vk.CreateBuffer(g.ctx.device, &sci, nullptr, &g.depth_scratch) == VK_SUCCESS) {
    VkMemoryRequirements sreq;
    g.vk.GetBufferMemoryRequirements(g.ctx.device, g.depth_scratch, &sreq);
    VkMemoryAllocateInfo smai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    smai.allocationSize = sreq.size;
    smai.memoryTypeIndex = MemoryType(sreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory smem;
    if (smai.memoryTypeIndex == UINT32_MAX || g.vk.AllocateMemory(g.ctx.device, &smai, nullptr, &smem) != VK_SUCCESS ||
        g.vk.BindBufferMemory(g.ctx.device, g.depth_scratch, smem, 0) != VK_SUCCESS) {
      g.depth_scratch = VK_NULL_HANDLE;
    }
  }
  return g.upload_address != 0;
}

// --- Per frame --------------------------------------------------------------

// A pass of the logged frame ends (a resolve, or the frame's end): its GPU
// end time, and what it was for the report.
void MarkPass(uint32_t dest, uint32_t width, uint32_t height) {
  if (!g.pass_queries || !g.pass_count || g.pass_count >= 256 || g.pass_frame) return;
  g.pass_labels[g.pass_count].dest = dest;
  g.pass_labels[g.pass_count].width = width;
  g.pass_labels[g.pass_count].height = height;
  g.vk.CmdWriteTimestamp(g.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.pass_queries, g.pass_count);
  ++g.pass_count;
}

// The logged frame's pass times, once its GPU work is surely done.
void ReadPassTimes() {
  if (!g.pass_frame || g.frame_number < g.pass_frame + 3) return;
  const uint32_t n = g.pass_count;
  g.pass_frame = 0;
  g.pass_count = 0;
  std::vector<uint64_t> t(n);
  if (n < 2 || g.vk.GetQueryPoolResults(g.ctx.device, g.pass_queries, 0, n, n * sizeof(uint64_t), t.data(),
                                        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
    return;
  }
  struct Pass {
    double ms;
    uint32_t index;
  };
  std::vector<Pass> passes;
  for (uint32_t i = 1; i < n; ++i) passes.push_back({double(t[i] - t[i - 1]) * g.timestamp_ns / 1e6, i});
  const double total = double(t[n - 1] - t[0]) * g.timestamp_ns / 1e6;
  std::sort(passes.begin(), passes.end(), [](const Pass& a, const Pass& b) { return a.ms > b.ms; });
  std::string top;
  for (size_t i = 0; i < passes.size() && i < 10; ++i) {
    const State::PassLabel& l = g.pass_labels[passes[i].index];
    top += fmt::format("\n[native]   {:.3f} ms: pass {} ({} draws) -> {}", passes[i].ms, passes[i].index, l.draws,
                       l.dest ? fmt::format("{:08X} {}x{}", l.dest, l.width, l.height) : std::string("frame end"));
  }
  REXLOG_INFO("[native] GPU time of the logged frame: {:.2f} ms in {} passes; the costliest:{}", total, n - 1, top);
}


// The frame's command buffer, begun on the first draw after a swap.
VkCommandBuffer Frame() {
  if (g.cmd) return g.cmd;
  uint32_t index = 0;
  bool new_frame = false;
  g.cmd = g.ctx.frame_commands(&index, &new_frame);
  if (!g.cmd) return VK_NULL_HANDLE;
  g.dyn = State::DynCache{};  // (a command buffer begun anew - recycled handles look the same)
  g.dyn.cmd = g.cmd;
  if (!new_frame) {  // a later segment of the frame: only the bindings to redo
    g.vk.CmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 5, g.sets.data(), 0, nullptr);
    return g.cmd;
  }
  g.frame_index = index & 1;
  g.upload_used = 0;
  CollectRetired();
  if (!g.resolve_dumps.empty()) WriteResolveDumps(false);
  g.resolve_dumps_in_frame = 0;
  LogProbe();
  if (g.frame_number % 300 == 0 || REXCVAR_GET(native_texture_idle_frames) < 300) EvictTextures();
  {
    const int64_t at = REXCVAR_GET(native_log_frame), n = REXCVAR_GET(native_log_count);
    const int64_t f = int64_t(g.frame_number + 1);
    g.log_frame = at > 0 && f >= at && f < at + std::max<int64_t>(n, 1);
  }
  if (g.log_frame) {
    RequestNamedDump(fmt::format("logged_{:06}", g.frame_number + 1).c_str());
  }
  if (!g.capture_log.empty()) {  // (the capture's lines, beside its resolves)
    const auto path = rex::filesystem::GetExecutableFolder() / "native_resolves" /
                      fmt::format("capture_{:06}.log", g.capture_log_frame);
    SaveFileLater(path, [text = std::move(g.capture_log)] { return std::vector<char>(text.begin(), text.end()); });
    g.capture_log.clear();
  }
  g.capture_frame = false;
  {  // (native_capture_after_seconds: a timed capture, once)
    static const auto first = std::chrono::steady_clock::now();
    static bool done = false;
    const int32_t after = REXCVAR_GET(native_capture_after_seconds);
    if (!done && after > 0 && std::chrono::steady_clock::now() - first >= std::chrono::seconds(after)) {
      done = true;
      g_capture_frames.store(2, std::memory_order_relaxed);
    }
  }
  if (uint32_t n = g_capture_frames.load(std::memory_order_relaxed); n && !g.log_frame) {  // (RequestCapture)
    g_capture_frames.store(n - 1, std::memory_order_relaxed);
    g.log_frame = g.capture_frame = true;
    g.capture_log_frame = g.frame_number + 1;
    RequestNamedDump(fmt::format("capture_{:06}", g.frame_number + 1).c_str());
    REXLOG_INFO("[native] capturing frame {} (the overlay's frame report; {} more): its draws go to "
                "native_resolves/capture_{:06}.log", g.frame_number + 1, n - 1, g.frame_number + 1);
  }
  if (g.log_on_ps_frame && g.frame_number + 1 == g.log_on_ps_frame) {  // (native_log_on_ps)
    g.log_frame = true;
    RequestNamedDump(fmt::format("logged_{:06}", g.frame_number + 1).c_str());
    REXLOG_INFO("[native] logging frame {} (native_log_on_ps)", g.frame_number + 1);
  }
  // [dev builds] Log the whole of the first heavy 3D frame (and one more two
  // minutes on), so a test run brings back a gameplay frame's draws - one
  // whose pipelines are all built (no draw of the one before it was skipped
  // for that), or it would show what is missing, not what is drawn wrong.
  const int32_t auto_log = REXCVAR_GET(native_auto_log);
  if (!g.log_frame && auto_log > 0 && (auto_log == 2 || g.auto_logs < 2) && g.last_frame_drawn > 300 &&
      g.last_frame_pending == 0 && g.frame_number >= g.auto_log_after) {
    g.log_frame = true;
    ++g.auto_logs;
    g.auto_log_after = g.frame_number + (auto_log == 2 ? 3600 : 7200);
    REXLOG_INFO("[native] logging frame {} ({} draws in the last one)", g.frame_number + 1, g.last_frame_drawn);
  }
  for (auto& b : g.last_block) b.clear();
  ReadPassTimes();
  if (g.log_frame && g.pass_queries && !g.pass_frame) {
    g.vk.CmdResetQueryPool(g.cmd, g.pass_queries, 0, 256);
    g.vk.CmdWriteTimestamp(g.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.pass_queries, 0);
    g.pass_count = 1;
    g.pass_labels = {};
  }
  // Placeholders and bindless sets are made once, in the first frame.
  if (g.placeholders.empty()) {
    const VkImageViewType types[] = {VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE,
                                     VK_IMAGE_VIEW_TYPE_1D};
    const uint32_t sets[] = {0, 1, 2, 4};
    for (int i = 0; i < 4; ++i) {
      Image img;
      if (!CreateImage(g.cmd, img, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, false, types[i])) {
        REXLOG_ERROR("[native] could not create a placeholder texture");
        break;
      }
      VkClearColorValue grey = {{0.5f, 0.5f, 0.5f, 1.0f}};
      VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, i == 2 ? 6u : 1u};
      g.vk.CmdClearColorImage(g.cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, &grey, 1, &r);
      WriteTextureDescriptor(sets[i], 0, img.view);
      g.placeholders.push_back(img);
    }
    Barrier(g.cmd);
  }
  g.vk.CmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 5, g.sets.data(), 0, nullptr);
  return g.cmd;
}

void EndRendering() {
  if (!g.rendering) return;
  ++t_timing.pass_ends;
  g.vk.CmdEndRendering(g.cmd);
  g.rendering = false;
  g.bound_colors = {};
  g.bound_depth = nullptr;
  Barrier(g.cmd);
}


// Upload a block (or reuse the last one if identical); its device address.
// Room for `size` bytes in this frame's half of the upload ring (null: full).
uint8_t* UploadSpace(uint32_t size, uint64_t* address) {
  const VkDeviceSize offset = (g.upload_used + 255) & ~VkDeviceSize(255);
  if (offset + size > kFrameUpload) return nullptr;
  g.upload_used = offset + size;
  const VkDeviceSize at = VkDeviceSize(g.frame_index) * kFrameUpload + offset;
  *address = g.upload_address + at;
  return g.upload_mapped + at;
}

uint64_t Upload(int which, const void* data, uint32_t size) {
  auto& last = g.last_block[which];
  if (last.size() == size && std::memcmp(last.data(), data, size) == 0) return g.last_address[which];
  uint64_t address;
  uint8_t* p = UploadSpace(size, &address);
  if (!p) return 0;
  std::memcpy(p, data, size);
  last.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
  return g.last_address[which] = address;
}

// An indexed triangle strip with primitive reset, as a triangle list for
// ng2VertexIndex's expansion 2: the guest's indices (before the index
// offset), little-endian; its device address, 0 if none. Each strip after a
// reset index starts its winding afresh, as on the hardware - drawn as one
// strip with the reset vertex's triangles dropped, the later strips kept the
// running parity, so these draws could not be culled, and the back faces of
// two-sided walls (the shop's paper windows) z-fought their fronts.
// Converted once per index buffer: converting each draw's indices every
// frame (test38) cost the worker ~5 ms a frame in heavy scenes. A cached list
// is checked against its indices' hash as textures are (every
// native_texture_recheck_frames frames; every frame once they changed).
constexpr VkDeviceSize kListArenaBytes = 16u << 20;
constexpr size_t kMaxListArenas = 4;

uint64_t ResetStripToList(const Device& d, const uint32_t index_info[4], uint32_t count, uint32_t* vertices) {
  const bool wide = index_info[1] & 1;
  const uint32_t endian = (index_info[1] >> 1) & 3;
  const uint32_t start = index_info[0] & 0x1FFFFFFFu, bytes = count * (wide ? 4u : 2u);
  if (!count || start + bytes + 4 > 0x20000000u) return 0;
  // (32-bit indices: the hardware compares their low 24 bits - the SDK's
  // primitive processor)
  const uint32_t compare_mask = wide ? 0xFFFFFFu : 0xFFFFu;
  const uint32_t reset = d.Reg(VGT_MULTI_PRIM_IB_RESET_INDX) & compare_mask;
  const uint8_t* src = REX_KERNEL_MEMORY()->TranslatePhysical<const uint8_t*>(start & ~3u);
  const uint32_t skew = start & 3u;
  const uint32_t key_words[4] = {start, count, index_info[1] & 0xF, reset};
  State::StripList& e = g.strip_lists[XXH3_64bits(key_words, sizeof(key_words))];
  const uint64_t every = e.dynamic ? 1 : uint64_t(std::max(REXCVAR_GET(native_texture_recheck_frames), 1));
  if (e.address && g.frame_number - e.checked_frame < every) {
    *vertices = e.vertices;
    return e.address;
  }
  const uint64_t hash = XXH3_64bits(src + skew, bytes);
  e.checked_frame = g.frame_number;
  if (e.address && e.hash == hash) {
    if (e.dynamic && ++e.steady >= State::kSettleChecks) {
      e.dynamic = false;  // (settled: the periodic checks again)
      e.steady = 0;
    }
    *vertices = e.vertices;
    return e.address;
  }
  if (e.address) {
    e.dynamic = true;
    e.steady = 0;
  }
  e.hash = hash;
  auto index = [&](uint32_t i) {  // (as ng2VertexIndex reads it)
    const uint32_t a = skew + i * (wide ? 4u : 2u);
    uint32_t w;
    std::memcpy(&w, src + (a & ~3u), 4);
    if (endian == 1) w = ((w >> 8) & 0x00FF00FFu) | ((w << 8) & 0xFF00FF00u);
    else if (endian == 2) w = __builtin_bswap32(w);
    else if (endian == 3) w = (w >> 16) | (w << 16);
    return wide ? w : (a & 2u) ? (w >> 16) : (w & 0xFFFFu);
  };
  auto& out = g.list_scratch;
  out.clear();
  uint32_t a = 0, b = 0, in_strip = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t v = index(i);
    if ((v & compare_mask) == reset) {
      in_strip = 0;
      continue;
    }
    if (in_strip >= 2) {  // triangle in_strip - 2 of this strip: odd ones swap to keep the winding
      const bool odd = in_strip & 1;
      out.push_back(a);
      out.push_back(odd ? v : b);
      out.push_back(odd ? b : v);
    }
    a = b;
    b = v;
    ++in_strip;
  }
  e.address = 0;
  e.vertices = uint32_t(out.size());
  const VkDeviceSize size = (VkDeviceSize(out.size()) * 4 + 255) & ~VkDeviceSize(255);
  if (!size || size > kListArenaBytes) return 0;
  if (g.list_arenas.empty() || g.list_used + size > kListArenaBytes) {
    if (g.list_arenas.size() >= kMaxListArenas) {  // all full: drop every list, convert again as drawn
      for (auto& arena : g.list_arenas) g.retired_arenas.emplace_back(g.frame_number, arena);
      g.list_arenas.clear();
      for (auto& [k, list] : g.strip_lists) list.address = 0;
    }
    State::MappedBuffer arena;
    if (!g.free_arenas.empty()) {
      arena = g.free_arenas.back();
      g.free_arenas.pop_back();
    } else if (!CreateMappedBuffer(kListArenaBytes, 0, arena)) {
      return 0;
    }
    g.list_arenas.push_back(arena);
    g.list_used = 0;
  }
  const State::MappedBuffer& arena = g.list_arenas.back();
  std::memcpy(arena.mapped + g.list_used, out.data(), out.size() * 4);
  e.address = arena.address + g.list_used;
  g.list_used += size;
  *vertices = e.vertices;
  return e.address;
}

// The shader module for a shader cache hash (made once, kept in g.shaders;
// `missing`: set when the hash was just found not to be in the cache).

const ShaderInfo* Shader(uint32_t object) {
  if (!object) return nullptr;
  uint64_t hash;
  {
    std::lock_guard<std::mutex> lock(g.object_mutex);
    auto it = g.object_hash.find(object);
    if (it == g.object_hash.end()) return nullptr;
    hash = it->second;
  }
  bool missing = false;
  const ShaderInfo* info = ShaderByHash(hash, &missing);
  if (missing) {
    ++g.stats.shaders_unknown;
    REXLOG_WARN("[native] shader {:016X} (object {:08X}) is not in the shader cache", hash, object);
  }
  return info;
}

// The shader table: on Windows the one built from the player's game at
// startup (izuna/izuna_shaders.h; DrawInit draws nothing without it), else
// the one built in at build time (tools/native/build_shaders.sh).
#if defined(IZUNA_SHADERS_FROM_GAME)
const ShaderCacheEntry* ShaderEntries() {
  const auto* t = izuna::shaders::Active();
  return t ? t->entries.data() : nullptr;
}
size_t ShaderEntryCount() {
  const auto* t = izuna::shaders::Active();
  return t ? t->entries.size() : 0;
}
const uint32_t* ShaderLayouts() {
  const auto* t = izuna::shaders::Active();
  return t ? t->layouts.data() : nullptr;
}
#else
const ShaderCacheEntry* ShaderEntries() { return g_shaderCacheEntries; }
size_t ShaderEntryCount() { return g_shaderCacheEntryCount; }
const uint32_t* ShaderLayouts() { return g_shaderVertexLayouts; }
#endif

const ShaderInfo* ShaderByHash(uint64_t hash, bool* missing) {
  auto s = g.shaders.find(hash);
  if (s != g.shaders.end()) return s->second.module ? &s->second : nullptr;
  ShaderInfo info;
  const ShaderCacheEntry* begin = ShaderEntries();
  const ShaderCacheEntry* end = ShaderEntries() + ShaderEntryCount();
  const ShaderCacheEntry* e = std::lower_bound(
      begin, end, hash, [](const ShaderCacheEntry& a, uint64_t h) { return a.hash < h; });
  if (e != end && e->hash == hash && e->spirvSize) {
    info.entry = e;
    // Each shader is smol-v encoded (as the fork writes it, like Unleashed).
    const uint8_t* encoded = g.spirv.data() + e->spirvOffset;
    std::vector<uint32_t> code(smolv::GetDecodedBufferSize(encoded, e->spirvSize) / 4);
    if (!code.empty() && smolv::Decode(encoded, e->spirvSize, code.data(), code.size() * 4)) {
      VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      ci.codeSize = code.size() * 4;
      ci.pCode = code.data();
      if (const VkResult r = g.vk.CreateShaderModule(g.ctx.device, &ci, nullptr, &info.module); r != VK_SUCCESS) {
        info.module = VK_NULL_HANDLE;
        static std::atomic<int> logged{0};  // [IzunaRecomp] (Adreno: which shaders a driver refuses, and why)
        if (logged.fetch_add(1) < 20) REXLOG_WARN("[native] shader {:016X}: module refused ({})", e->hash, int(r));
      }
    } else {
      REXLOG_WARN("[native] shader {:016X} did not decode (smol-v)", e->hash);
    }
  } else if (missing) {
    *missing = true;
  }
  auto& slot = g.shaders[hash] = info;
  return slot.module ? &slot : nullptr;
}

// Decode the vertex shader's vfetch instructions from its microcode in guest
// memory (object + 32: the microcode; the cache's layout records: each
// vfetch's instruction index, and whether it is a mini fetch) into the
// shader's per-draw table (16 bytes per fetch; ng2VertexFetchK).
bool FillVertexFetches(uint32_t object, const ShaderCacheEntry* e, uint8_t* out) {
  if (!e->vertexLayoutCount) return true;
  const uint8_t* o = Guest(object);
  const uint32_t code = o ? Be32(o + 32) : 0;
  const uint8_t* mc = code ? Guest(code) : nullptr;
  if (!mc) return false;
  uint32_t slot = 0, stride = 0;
  for (uint32_t k = 0; k < e->vertexLayoutCount && k < 64; ++k) {
    const uint32_t address = ShaderLayouts()[e->vertexLayoutOffset + k * 2];
    const uint8_t* ins = mc + e->vfetchCodeOffset + address * 12;
    const uint32_t d0 = Be32(ins), d1 = Be32(ins + 4), d2 = Be32(ins + 8);
    if (!((d1 >> 30) & 1)) {  // a full fetch: its slot and stride (mini fetches reuse them)
      slot = ((d0 >> 20) & 31) * 3 + ((d0 >> 25) & 3);
      stride = d2 & 0xFF;
    }
    const uint32_t flags = ((d1 >> 15) & 1) | ((d1 >> 12) & 1) << 1 | ((d1 >> 13) & 1) << 2 | ((d1 >> 14) & 1) << 3;
    const int32_t exp = int32_t(d1 << 2) >> 26;      // bits 24-29, signed
    const int32_t offset = int32_t(d2 << 1) >> 9;    // bits 8-30, signed
    // (p[3] bits 16-27: the destination swizzle as patched - ng2VertexFetchK)
    const uint32_t p[4] = {slot | stride << 8, uint32_t(offset), (d1 >> 16) & 63,
                           flags | (uint32_t(exp) & 0xFF) << 8 | (d1 & 0xFFF) << 16};
    std::memcpy(out + k * 16, p, 16);
  }
  return true;
}

// The image for an EDRAM surface (made on first use).
const Image* Target(uint32_t base, uint32_t format, uint32_t pitch, bool depth) {
  if (!pitch) return nullptr;
  // The _AS_ formats are the same EDRAM bits (only what a resolve of them
  // writes differs): 2_10_10_10_AS_10_10_10_10 is 2_10_10_10, and
  // 2_10_10_10_FLOAT_AS_16_16_16_16 is 2_10_10_10_FLOAT - one image each (the
  // game draws its HDR scene as 3 and resolves it as 12; Xenia's
  // GetStorageColorFormat).
  if (!depth && format == 10) format = 2;
  if (!depth && format == 12) format = 3;
  const uint64_t key = uint64_t(base & 0xFFF) | uint64_t(format & 0xF) << 12 | uint64_t(pitch) << 16 |
                       uint64_t(depth) << 40;
  auto it = g.targets.find(key);
  if (it != g.targets.end()) return &it->second;
  const VkFormat vf = depth ? VK_FORMAT_D32_SFLOAT_S8_UINT : ColorFormat(format);
  if (vf == VK_FORMAT_UNDEFINED) return nullptr;
  Image img;
  EndRendering();
  ++t_timing.creates_by[1];
  // [IzunaRecomp] At 1.5x (a true 1080p) only the large surfaces - the scene
  // (pitch 1280 / 1120 / 1040) and its half-size ones (560 / 640) - are drawn
  // scaled. The small ones (bloom levels, the 32x1 exposure strip) stay 1x: a
  // guest pixel is 1.5 of the host's, and an odd size left a half-covered
  // row or column never drawn - the exposure strip read it (izuna-windows-
  // test12's dark rooms and black / white blotches). Whole scales draw all.
  float scale = RenderScale(), scale_y = scale;
  if (scale != std::floor(scale) && pitch < uint32_t(REXCVAR_GET(native_fractional_min_pitch))) scale = scale_y = 1.0f;
  // [IzunaRecomp] The 3D scene: NG2 draws it at 1120x584 (2x MSAA fits the
  // 10 MB of EDRAM so) and stretches it to the 1280x720 frame at the end -
  // at 1.5x a 1680x876 scene stretched to 1920x1080. Drawn instead at
  // 1920 / 1120 by 1080 / 584 (and its half-size passes, 560 wide, likewise),
  // the stretch lands 1:1: the 3D picture at the output's true resolution.
  if ((pitch == 1120 || pitch == 560) && REXCVAR_GET(native_scene_fit)) {  // (at every scale: 720p too)
    scale = scale * (1280.0f / 1120.0f);
    scale_y = scale_y * (720.0f / 584.0f);
  }
  if (!CreateImage(g.cmd, img, vf, ToHostSize(pitch, scale), ToHostSize(kRtHeight, scale_y), depth)) return nullptr;
  img.scale = scale;
  img.scale_y = scale_y;
  VkImageSubresourceRange r = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                                        : VK_IMAGE_ASPECT_COLOR_BIT),
                               0, 1, 0, 1};
  if (depth) {
    VkClearDepthStencilValue v = {1.0f, 0};
    g.vk.CmdClearDepthStencilImage(g.cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
  } else {
    VkClearColorValue v = {};
    g.vk.CmdClearColorImage(g.cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
  }
  Barrier(g.cmd);
  REXLOG_INFO("[native] render target: EDRAM base {} format {} pitch {}{} -> {}x{}", base & 0xFFF, format, pitch,
              depth ? " (depth)" : "", img.width, img.height);
  return &g.targets.emplace(key, img).first->second;
}

// Begin rendering to these targets, unless already.
void Bind(const std::array<const Image*, 4>& colors, const Image* depth) {
  if (g.rendering && colors == g.bound_colors && depth == g.bound_depth) return;
  EndRendering();
  std::array<VkRenderingAttachmentInfo, 4> ca{};
  uint32_t count = 0, w = 0, h = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    ca[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (!colors[i]) continue;
    count = i + 1;
    ca[i].imageView = colors[i]->view;
    ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    w = w ? std::min(w, colors[i]->width) : colors[i]->width;
    h = h ? std::min(h, colors[i]->height) : colors[i]->height;
  }
  VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  if (depth) {
    da.imageView = depth->view;
    da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    w = w ? std::min(w, depth->width) : depth->width;
    h = h ? std::min(h, depth->height) : depth->height;
  }
  VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {w, h}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = count;
  ri.pColorAttachments = ca.data();
  ri.pDepthAttachment = depth ? &da : nullptr;
  ri.pStencilAttachment = depth ? &da : nullptr;
  g.vk.CmdBeginRendering(g.cmd, &ri);
  g.rendering = true;
  g.bound_colors = colors;
  g.bound_depth = depth;
}

bool CreateResolvedImage(Image& img, VkFormat format, uint32_t w, uint32_t h);

// A resolved image as a texture of w x h fetches it (see State::resolved_fit):
// itself if that is its size, else a copy of its top-left w x h.
const Image* ResolvedFit(uint32_t dest, const Image& src, uint32_t w, uint32_t h) {
  if (!w || !h || w > 8192 || h > 8192) return &src;
  w = ToHostSize(w, src.scale);  // (guest texels: the image's are the host's)
  h = ToHostSize(h, src.scale_y);
  if (w == src.width && h == src.height) return &src;
  const uint64_t key = uint64_t(dest) | uint64_t(w - 1) << 32 | uint64_t(h - 1) << 48;
  Image& fit = g.resolved_fit[key];
  if (fit.image && fit.format != src.format) Retire(fit);
  if (!fit.image) ++t_timing.creates_by[3];
  if (!fit.image && !CreateResolvedImage(fit, src.format, w, h)) {
    g.resolved_fit.erase(key);
    return &src;
  }
  fit.scale = src.scale;
  fit.scale_y = src.scale_y;
  fit.fit_used = g.frame_number;
  if (fit.generation != src.generation) {
    ++t_timing.fit_copies;
    EndRendering();
    VkImageCopy c = {};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {std::min(w, src.width), std::min(h, src.height), 1};
    g.vk.CmdCopyImage(g.cmd, src.image, VK_IMAGE_LAYOUT_GENERAL, fit.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    Barrier(g.cmd);
    fit.generation = src.generation;
  }
  return &fit;
}

// The fetch-sized copies of a destination just resolved, made now - outside
// any render pass - instead of at the draw that samples them, where each one
// ended the pass (~18 GPU flushes a frame in heavy scenes, test38's console
// log). Only those a draw sampled in the last two frames: the rest wait for
// their draw, as before. The same pixels either way: only resolves write
// resolved images.
void RefreshFits(uint32_t dest, const Image& src) {
  bool barrier = false;
  for (auto& [key, fit] : g.resolved_fit) {
    if (uint32_t(key) != dest || !fit.image || fit.format != src.format || fit.generation == src.generation ||
        g.frame_number - fit.fit_used > 2) {
      continue;
    }
    if (!barrier) {
      Barrier(g.cmd);  // (the resolve's write, before the copies read it)
      barrier = true;
    }
    const uint32_t w = uint32_t(key >> 32 & 0xFFFF) + 1, h = uint32_t(key >> 48) + 1;
    VkImageCopy c = {};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {std::min(w, src.width), std::min(h, src.height), 1};
    g.vk.CmdCopyImage(g.cmd, src.image, VK_IMAGE_LAYOUT_GENERAL, fit.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    fit.generation = src.generation;
    ++t_timing.fit_copies_at_resolve;
  }
  if (barrier) Barrier(g.cmd);
}

// Record one draw. `vertex_count`: vertices the shader will be invoked for;
// `index_info`: SharedConstants + 960 (ng2VertexIndex).
// `up_data`: the device address of vertices copied for a BeginVertices draw
// (vertex fetch slot 95 then reads them; `up_fetch1` its second dword).
void StageClock::Mark(int stage) {
  if (!g_timed) return;
  const uint64_t now = Ticks();
  t_timing.stage_ns[stage] += TicksToNs(now - t);
  t = now;
}

// `layout`: the vertex fetches' fields as the producer read them at the call
// (native_threaded: FillVertexFetches' output; `layout_ok` false: it failed),
// else (null) read now.
std::atomic<bool> g_watched_seen{false};  // native_watch_tex

void Draw(const Device& d, uint32_t xenos_primitive, uint32_t count, const uint32_t index_info[4],
          uint64_t up_data, uint32_t up_fetch1, const uint8_t* layout, bool layout_ok) {
  ++g.census[xenos_primitive & 31];
  if (!Frame()) return;
  StageClock clock;
  ++t_timing.draws;
  if (REXCVAR_GET(native_debug) & 1) return;
  const uint32_t mode = d.Reg(RB_MODECONTROL) & 7;
  if (mode == 6) {  // EDRAM copy: a resolve's own rectangle (handled at OnResolve)
    ++g.stats.copy_mode;
    return;
  }
  VkPrimitiveTopology topology;
  uint32_t vertices = count, expand = 0;
  uint64_t strip_list = 0;  // (expansion 2: ResetStripToList)
  switch (xenos_primitive) {
    case 1: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case 2: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 3: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case 4: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case 5: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case 6:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      // (primitive reset; a BeginVertices draw's indices are in the upload
      // ring, not guest memory: those keep the NaN reset vertex)
      if (index_info[0] != 0xFFFFFFFFu && (index_info[1] & 8) && !up_data) {
        strip_list = ResetStripToList(d, index_info, count, &vertices);
        if (!strip_list) return;
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        expand = 2;
      }
      break;
    case 13:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      vertices = count / 4 * 6;
      expand = 1;
      break;
    case 8:  // rectangle list: three corners each, the geometry shader adds the fourth
      if (!g.rect_gs) {
        ++g.stats.rect;
        return;
      }
      topology = VkPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST | kRectList);
      break;
    default:
      ++g.stats.rect;  // (line loops, polygons, ...: later)
      return;
  }
  const ShaderInfo* vs = Shader(d.Field(kVertexShader));
  if (!vs) {
    if (g.stats.no_vs < 3) {  // (diagnosis: what the device holds instead)
      const uint32_t o = d.Field(kVertexShader);
      const uint8_t* p = o ? Guest(o) : nullptr;
      REXLOG_WARN("[native] draw without a known vertex shader: object {:08X} [{:08X} {:08X} {:08X} {:08X}] "
                  "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                  o, p ? Be32(p) : 0, p ? Be32(p + 4) : 0, p ? Be32(p + 40) : 0, p ? Be32(p + 44) : 0,
                  d.Field(kVertexShader - 8), d.Field(kVertexShader - 4), d.Field(kVertexShader + 4),
                  d.Field(kVertexShader + 8), d.Field(kVertexShader + 12), d.Field(kVertexShader + 16));
    }
    ++g.stats.no_vs;
    return;
  }
  const uint32_t ps_object = d.Field(kPixelShader);
  const ShaderInfo* ps = Shader(ps_object);
  if (ps_object && !ps) {
    ++g.stats.unknown_shader;
    return;
  }
  // [IzunaRecomp] A mod's hidden shaders (native_mods.cpp): not drawn.
  if (ShaderHidden(vs->entry->hash, ps ? ps->entry->hash : 0)) {
    ++g.stats.hidden;
    return;
  }
  if (ps) {
    static const uint64_t log_on = std::strtoull(REXCVAR_GET(native_log_on_ps).c_str(), nullptr, 16);
    if (log_on && ps->entry->hash == log_on && !g.log_on_ps_frame) g.log_on_ps_frame = g.frame_number + uint64_t(std::max(REXCVAR_GET(native_log_on_delay), 1));
  }
  if (static const uint32_t watch = uint32_t(std::strtoul(REXCVAR_GET(native_watch_tex).c_str(), nullptr, 16));
      watch && !g_watched_seen.load(std::memory_order_relaxed)) {
    const uint8_t* f0 = d.p + kFetchConstants;
    if ((Be32(f0) & 3) == 2 && (Be32(f0 + 4) & 0x1FFFF000u) == (watch & 0x1FFFF000u)) g_watched_seen.store(true);
  }
  if (static const uint32_t log_tex = uint32_t(std::strtoul(REXCVAR_GET(native_log_on_tex).c_str(), nullptr, 16));
      log_tex && !g.log_on_ps_frame) {
    const uint8_t* f0 = d.p + kFetchConstants;
    if ((Be32(f0) & 3) == 2 && (Be32(f0 + 4) & 0x1FFFF000u) == (log_tex & 0x1FFFF000u)) {
      g.log_on_ps_frame = g.frame_number + uint64_t(std::max(REXCVAR_GET(native_log_on_delay), 1));
    }
  }
  // Render targets.
  const uint32_t surface = d.Reg(RB_SURFACE_INFO);
  const uint32_t pitch = surface & 0x3FFF;
  ++g.stats.msaa[(surface >> 16) & 3];
  const uint32_t color_mask = d.Reg(RB_COLOR_MASK);
  std::array<const Image*, 4> colors{};
  const uint32_t color_info[4] = {d.Reg(RB_COLOR_INFO), d.Reg(RB_COLOR1_INFO), d.Reg(RB_COLOR2_INFO),
                                  d.Reg(RB_COLOR3_INFO)};
  if (mode == 4) {  // color + depth (5: depth only)
    for (uint32_t i = 0; i < 4; ++i) {
      if (!((color_mask >> (i * 4)) & 0xF)) continue;
      colors[i] = Target(color_info[i] & 0xFFF, (color_info[i] >> 16) & 0xF, pitch, false);
      if (!colors[i]) {
        ++g.stats.bad_format;
        return;
      }
      g.edram_writer[color_info[i] & 0xFFF] = (((color_info[i] >> 16) & 0xF) == 12 ? 3u : ((color_info[i] >> 16) & 0xF) == 10 ? 2u : (color_info[i] >> 16) & 0xF) | pitch << 8;
    }
  }
  const uint32_t depth_control = d.Reg(RB_DEPTHCONTROL);
  const Image* depth = nullptr;
  if (depth_control & 0x7) {  // stencil, z test or z write
    const uint32_t di = d.Reg(RB_DEPTH_INFO);
    depth = Target(di & 0xFFF, 0, pitch, true);
  }
  if (!depth && !colors[0] && !colors[1] && !colors[2] && !colors[3]) {
    ++g.stats.no_target;
    return;
  }
  clock.Mark(0);
  // Constants (unchanged guest bytes this frame: the last upload again).
  static uint32_t block[kVsBytes / 4];
  auto constants = [&](int which, const uint8_t* src, uint32_t bytes) -> uint64_t {
    auto& raw = g.raw_constants[size_t(which)];
    if (!g.last_block[which].empty() && raw.size() == bytes && std::memcmp(raw.data(), src, bytes) == 0) {
      return g.last_address[which];
    }
    SwapCopy32(block, src, bytes / 4);
    const uint64_t address = Upload(which, block, bytes);
    if (address) {
      raw.assign(src, src + bytes);
    } else {
      raw.clear();  // (the ring was full: nothing to reuse)
    }
    return address;
  };
  // Only the registers each shader reads (the shader cache's
  // floatConstantRegs, at least 16 - Edge of Time Recompiled uploads its
  // constants the same way): most read far fewer than 256.
  auto constant_bytes = [](const ShaderInfo* sh) {
    return std::clamp<uint32_t>(sh ? sh->entry->floatConstantRegs : 16u, 16u, 256u) * 16u;
  };
  const uint8_t* vsc = d.p + kVsConstants;
  const uint64_t vs_address = constants(0, vsc, constant_bytes(vs));
  const uint8_t* psc = d.p + kPsConstants;
  const uint64_t ps_address = constants(1, psc, constant_bytes(ps));
  uint32_t fetch[kFetchBytes / 4];
  SwapCopy32(fetch, d.p + kFetchConstants, kFetchBytes / 4);
  if (up_data) {  // slot 95 -> the copied vertices (base 0 + the GuestMemory override below)
    fetch[190] = 3;
    fetch[191] = up_fetch1;
  }
  const uint64_t fetch_address = Upload(3, fetch, kFetchBytes);
  // Shared constants (shader_common.h, REEOT layout + NG2 additions).
  uint8_t shared[kSharedBytes] = {};
  auto put = [&](uint32_t offset, uint32_t v) { std::memcpy(shared + offset, &v, 4); };
  auto putf = [&](uint32_t offset, float v) { std::memcpy(shared + offset, &v, 4); };
  for (uint32_t i = 0; i < 8; ++i) put(320 + i * 4, d.Reg(SQ_BOOL_CONSTANTS + i));
  for (uint32_t i = 0; i < 32; ++i) {
    const uint32_t l = d.Reg(SQ_LOOP_CONSTANTS + i);
    put(400 + i * 16, l & 0xFF);                                 // count
    put(400 + i * 16 + 4, (l >> 8) & 0xFF);                      // start
    put(400 + i * 16 + 8, uint32_t(int32_t(int8_t(l >> 16))));   // step
  }
  putf(364, AsFloat(d.Reg(RB_ALPHA_REF)));
  put(952, d.Reg(RB_COLORCONTROL) & 7);  // (the alpha test's function: ng2AlphaTest)
  put(956, d.Reg(RB_COLORCONTROL) >> 24);  // (alpha to mask's four 2-bit offsets: ng2AlphaToMask)
  // Position: Xenos clip space -> its screen (VTE) -> Vulkan NDC over the
  // target; y pre-flipped because the vertex shaders end with -fvk-invert-y.
  const uint32_t vte = d.Reg(PA_CL_VTE_CNTL);
  const Image* sized = colors[0] ? colors[0] : colors[1] ? colors[1] : colors[2] ? colors[2] : colors[3] ? colors[3] : depth;
  // (guest pixels: the targets are RenderScale() times their size, the
  // viewport below covers all of the host image)
  const float w = float(sized->width / sized->scale), h = float(sized->height / sized->scale_y);
  const float host_w = float(sized->width), host_h = float(sized->height);
  const float sx = (vte & 1) ? AsFloat(d.Reg(PA_CL_VPORT_XSCALE)) : 1.0f;
  const float ox = (vte & 2) ? AsFloat(d.Reg(PA_CL_VPORT_XOFFSET)) : 0.0f;
  const float sy = (vte & 4) ? AsFloat(d.Reg(PA_CL_VPORT_YSCALE)) : 1.0f;
  const float oy = (vte & 8) ? AsFloat(d.Reg(PA_CL_VPORT_YOFFSET)) : 0.0f;
  const float sz = (vte & 16) ? AsFloat(d.Reg(PA_CL_VPORT_ZSCALE)) : 1.0f;
  const float oz = (vte & 32) ? AsFloat(d.Reg(PA_CL_VPORT_ZOFFSET)) : 0.0f;
  const float half = (d.Reg(PA_SU_VTX_CNTL) & 1) ? 0.0f : -0.5f;  // D3D pixel centres
  putf(912, sx * 2.0f / w);
  putf(916, -(sy * 2.0f / h));
  putf(920, sz);
  putf(928, (ox + half) * 2.0f / w - 1.0f);
  putf(932, -((oy + half) * 2.0f / h - 1.0f));
  putf(936, oz);
  for (uint32_t i = 0; i < 4; ++i) put(960 + i * 4, index_info[i]);
  put(976, 1);  // (ng2MemExport writes guest memory: the memory exports are ours)
  put(980, d.Reg(VGT_MULTI_PRIM_IB_RESET_INDX) & ((index_info[1] & 1) ? 0xFFFFFFFFu : 0xFFFFu));
  if (expand) put(972, expand);
  if (strip_list) std::memcpy(shared + 1016, &strip_list, 8);
  // Vertex fetches: the fields of each of the vertex shader's vfetch
  // instructions as they are now in guest memory (Direct3D patches them for
  // the bound vertex declaration), for ng2VertexFetchK.
  clock.Mark(1);
  if (layout) {
    if (!layout_ok) {
      ++g.stats.no_vs;
      return;
    }
    std::memcpy(shared + 1664, layout, std::min<size_t>(vs->entry->vertexLayoutCount * 16u, kSharedLayoutEnd - 1664));
  } else if (!FillVertexFetches(d.Field(kVertexShader), vs->entry, shared + 1664)) {
    ++g.stats.no_vs;
    return;
  }
  clock.Mark(2);
  // Textures: resolved images where a fetch constant points at one, else the
  // placeholder (descriptor 0).
  uint32_t biased_mask = 0, alpha_biased_mask = 0;
  uint32_t scaled_slots = 0;  // (slots holding images drawn at RenderScale(): g_ScaledSlots)
  int8_t exp_adjust[32] = {};  // (each slot's fetch exponent adjust: ng2ExpAdjust, SharedConstants + 984)
  // Only the slots the shaders fetch (the shader cache's textureSlotMask):
  // the device keeps the fetch constants of earlier draws in the others.
  const uint32_t slots = vs->entry->textureSlotMask | (ps ? ps->entry->textureSlotMask : 0u);
  const uint32_t point_slots = vs->entry->pointSlotMask | (ps ? ps->entry->pointSlotMask : 0u);
  const uint32_t linear_slots = vs->entry->linearSlotMask | (ps ? ps->entry->linearSlotMask : 0u);
  const uint32_t mip_point_slots = vs->entry->mipPointSlotMask | (ps ? ps->entry->mipPointSlotMask : 0u);
  const uint32_t mip_linear_slots = vs->entry->mipLinearSlotMask | (ps ? ps->entry->mipLinearSlotMask : 0u);
  const uint32_t mip_base_slots = vs->entry->mipBaseSlotMask | (ps ? ps->entry->mipBaseSlotMask : 0u);
  const uint32_t aniso_off_slots = vs->entry->anisoOffSlotMask | (ps ? ps->entry->anisoOffSlotMask : 0u);
  for (uint32_t m = slots; m; m &= m - 1) {
    const uint32_t slot = uint32_t(__builtin_ctz(m));
    const uint32_t* fc = fetch + slot * 6;
    if ((fc[0] & 3) != 2) continue;
    exp_adjust[slot] = int8_t(int32_t(fc[3] << 13) >> 26);  // (dword 3 bits 13-18, signed)
    // (the fetch instruction's own filter, when it sets one - a few shaders)
    const uint32_t filter = point_slots >> slot & 1 ? 1u : linear_slots >> slot & 1 ? 2u : 0u;
    const uint32_t mip = mip_point_slots >> slot & 1 ? 1u : mip_linear_slots >> slot & 1 ? 2u
                         : mip_base_slots >> slot & 1 ? 3u : 0u;
    const bool aniso_off = (aniso_off_slots >> slot) & 1;
    const bool own_sampler = filter || mip || aniso_off;  // (the instruction overrides the constant's sampler)
    // TextureSign per channel (fc[0] bits 2-9): 2 biased (x2-1), 3 gamma. The
    // shaders apply them for slots 0-15 (applyFetchSign: SharedConstants +
    // 392 rgb biased / gamma at bit 16 + slot, + 388 alpha biased at bit 16 +
    // slot), gamma with the exact Xenos curve.
    const uint32_t sx = (fc[0] >> 2) & 3, sw = (fc[0] >> 8) & 3;
    const bool gamma = ((fc[0] >> 2) & 0x3F) == 0x3F;  // sign_x/y/z all 3
    const bool shader_gamma = gamma && slot < 16;
    if (slot < 16) {
      if (sx == 2) biased_mask |= 1u << slot;
      if (shader_gamma) biased_mask |= 1u << (16 + slot);
      if (sw == 2) alpha_biased_mask |= 1u << (16 + slot);
    }
    // (the slot's descriptors and sampler into the shared constants)
    auto apply = [&](const State::SlotResult& r) {
      if (r.scaled) {
        scaled_slots |= 1u << slot;
        putf(2704 + slot * 8, r.sx);  // g_SlotScale (slot)
        putf(2708 + slot * 8, r.sy);
      }
      put(1024 + slot * 4, r.texture);
      if (r.write_tables) {
        put(1024 + 3 * 128 + slot * 4, r.one_d);  // (the 1D table)
        put(1024 + 2 * 128 + slot * 4, r.cube);   // (the cube table)
      }
      put(1536 + slot * 4, own_sampler ? Sampler(fc, filter, mip, aniso_off) : r.sampler);
    };
    State::SlotCache& cache = g.slot_cache[slot];
    if (cache.frame == g.frame_number && cache.resolve_generation == g.resolve_generation &&
        std::memcmp(cache.fc, fc, sizeof(cache.fc)) == 0) {
      apply(cache.r);
      continue;
    }
    std::memcpy(cache.fc, fc, sizeof(cache.fc));
    cache.frame = g.frame_number;
    cache.resolve_generation = g.resolve_generation;
    uint64_t mix = uint64_t(shader_gamma);
    for (int w = 0; w < 6; ++w) mix = (mix ^ fc[w]) * 0x9E3779B97F4A7C15ull;
    State::FrameTex& ft = g.frame_tex[(mix >> 54) & 1023];
    if (ft.frame == g.frame_number && ft.resolve_generation == g.resolve_generation &&
        ft.shader_gamma == shader_gamma && std::memcmp(ft.fc, fc, sizeof(ft.fc)) == 0) {
      cache.r = ft.r;
      ++t_timing.slot_frame_hits;
      apply(cache.r);
      continue;
    }
    ++t_timing.slot_misses;
    State::SlotResult& r = cache.r;
    r = {};
    const uint64_t slot_t0 = g_timed ? Ticks() : 0;
    auto it = g.resolved.find(fc[1] & 0x1FFFF000u);
    if (it != g.resolved.end() && it->second.descriptor) {
      const Image* img = &it->second;
      rex::graphics::xenos::xe_gpu_texture_fetch_t tf;
      std::memcpy(&tf, fc, sizeof(tf));
      if (tf.dimension == rex::graphics::xenos::DataDimension::k2DOrStacked) {
        img = ResolvedFit(it->first, it->second, tf.size_2d.width + 1, tf.size_2d.height + 1);
      }
      r.texture = gamma && !shader_gamma && img->descriptor_srgb ? img->descriptor_srgb : img->descriptor;
      r.scaled = img->scale != 1.0f || img->scale_y != 1.0f;
      r.sx = img->scale;
      r.sy = img->scale_y;
      if (g_timed) t_timing.slot_resolved_ns += TicksToNs(Ticks() - slot_t0);
    } else {
      r.texture = Texture2D(fc, &r.one_d, &r.cube, shader_gamma);
      r.write_tables = true;
      if (g_timed) t_timing.slot_texture_ns += TicksToNs(Ticks() - slot_t0);
    }
    const uint64_t sampler_t0 = g_timed ? Ticks() : 0;
    r.sampler = Sampler(fc);
    apply(r);
    std::memcpy(ft.fc, fc, sizeof(ft.fc));
    ft.shader_gamma = shader_gamma;
    ft.frame = g.frame_number;
    ft.resolve_generation = g.resolve_generation;
    ft.r = r;
    if (g_timed) t_timing.slot_sampler_ns += TicksToNs(Ticks() - sampler_t0);
  }
  {  // g_PackedDec3 bits 24-27: render target i is 2_10_10_10_FLOAT (or its _AS_16_16_16_16 view): the
     // shader clamps its output to what that EDRAM format holds (the fork's NG2 epilogue)
    // (16_16 / 16_16_16_16 targets - fixed point -32...32 in EDRAM - are
    // 16-bit SNORM here, values as written, and a resolve to a k_16_16 texture
    // converts the value, as the hardware's resolve does: NG2's variance
    // shadow blur reads its moments back with no scale of its own - test33's
    // bit-exact 1/32 storage left them 64x too small, the shadows wrong.)
    uint32_t packed = 0;
    if (mode == 4) {
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t f = (color_info[i] >> 16) & 0xF;
        if (colors[i] && (f == 3 || f == 12)) packed |= 0x01000000u << i;
      }
    }
    put(396, packed);
  }
  std::memcpy(shared + 984, exp_adjust, sizeof(exp_adjust));
  // [IzunaRecomp] The target's scale (vPos back to guest pixels) and the
  // scaled textures' (their texel offsets): apart at 1.5x, where small
  // targets stay 1x (Target).
  putf(944, 1.0f / sized->scale);  // g_RenderScaleInv
  putf(2688, RenderScale());       // g_TextureScale
  putf(2696, 1.0f / sized->scale);    // g_RenderScaleInv2 (vPos per axis)
  putf(2700, 1.0f / sized->scale_y);
  put(948, scaled_slots);                  // g_ScaledSlots
  put(392, biased_mask);        // g_BiasedTextures
  put(388, alpha_biased_mask);  // g_SintTexcoords (the alpha half: applyFetchSign)
  clock.Mark(3);
  const uint64_t shared_address = Upload(2, shared, kSharedBytes);
  if (!vs_address || !ps_address || !fetch_address || !shared_address) {
    ++g.stats.upload_full;
    return;
  }
  // Pipeline.
  PipelineKey key = {};
  key.vs = vs->entry->hash;
  key.ps = ps ? ps->entry->hash : 0;
  key.topology = topology;
  for (uint32_t i = 0; i < 4; ++i) key.color_formats[i] = colors[i] ? colors[i]->format : 0;
  key.depth_format = depth ? depth->format : 0;
  key.blend[0] = d.Reg(RB_BLENDCONTROL0);
  for (uint32_t i = 1; i < 4; ++i) key.blend[i] = d.Reg(RB_BLENDCONTROL1 + i - 1);
  key.color_mask = color_mask;
  key.depth_control = depth ? depth_control : 0;
  key.stencil_masks = depth ? (d.Reg(RB_STENCILREFMASK) & 0xFFFF00) : 0;
  const uint32_t mode_cntl = d.Reg(PA_SU_SC_MODE_CNTL);
  const bool polygonal = xenos_primitive >= 4 && xenos_primitive != 8;
  // No culling for rectangles, or with a primitive reset left to the reset
  // vertex's NaN position (fans: one running winding across the pieces);
  // reset strips are lists by now (ResetStripToList).
  const bool restart = ((index_info[1] >> 3) & 1) && expand != 2;
  if (polygonal && !restart) key.raster = mode_cntl & 3;
  if (polygonal) key.raster |= ((mode_cntl >> 2) & 1) << 2;
  // Depth bias (the SDK's GetPreferredFacePolygonOffset): the front offset
  // unless front faces are culled, else the back one; non-polygons use the
  // front one when poly_offset_para_enable is set.
  float bias_scale = 0.0f, bias_offset = 0.0f;
  if (polygonal) {
    if (((mode_cntl >> 11) & 1) && !(mode_cntl & 1)) {
      bias_scale = AsFloat(d.Reg(PA_SU_POLY_OFFSET_FRONT_SCALE));
      bias_offset = AsFloat(d.Reg(PA_SU_POLY_OFFSET_FRONT_SCALE + 1));
    }
    if (((mode_cntl >> 12) & 1) && !(mode_cntl & 2) && bias_scale == 0.0f && bias_offset == 0.0f) {
      bias_scale = AsFloat(d.Reg(PA_SU_POLY_OFFSET_FRONT_SCALE + 2));
      bias_offset = AsFloat(d.Reg(PA_SU_POLY_OFFSET_FRONT_SCALE + 3));
    }
  } else if ((mode_cntl >> 13) & 1) {
    bias_scale = AsFloat(d.Reg(PA_SU_POLY_OFFSET_FRONT_SCALE));
    bias_offset = AsFloat(d.Reg(PA_SU_POLY_OFFSET_FRONT_SCALE + 1));
  }
  if (depth && (bias_scale != 0.0f || bias_offset != 0.0f)) key.raster |= 8;
  // PA_CL_CLIP_CNTL clip_disable (bit 16): no clipping against the near / far
  // planes - depth clamped instead, as the SDK's backend does (a light volume
  // or sky drawn so is not cut by them; before test35 it was).
  if (g.depth_clamp && ((d.Reg(PA_CL_CLIP_CNTL) >> 16) & 1)) key.raster |= 16;
  // A 20e4 (float 24) depth target in use: depth at its precision (the pixel
  // shaders' epilogue; depth-only draws: depth24_fs).
  if (depth && ((d.Reg(RB_DEPTH_INFO) >> 16) & 1) && (depth_control & 6)) key.spec |= kSpecFloat24Depth;
  // Alpha test (RB_COLORCONTROL): with SPEC_CONSTANT_ALPHA_TEST the shaders
  // compare alpha with RB_ALPHA_REF (SharedConstants + 364) by the test's own
  // function (+ 952, the fork's ng2AlphaTest). Before test40 only GREATER /
  // GREATER_EQUAL were done (as >=) and any other function drew everything.
  const uint32_t color_control = d.Reg(RB_COLORCONTROL);
  if ((color_control & 8) && ps && (color_control & 7) != 7) key.spec |= 2;  // (7: always - no test)
  // Alpha to mask (bit 4): on our single-sample targets, the hardware's
  // ordered dither by alpha (the fork's ng2AlphaToMask; the offsets: + 956).
  if ((color_control & 0x10) && ps && colors[0]) {
    key.spec |= kSpecAlphaToMask;
    // [IzunaRecomp] On a 2x MSAA surface the hardware tests alpha per sample
    // (two thresholds, 0.5 and 1.0 less the dither offset / 8) and the resolve
    // averages the samples: a severed limb's cut surface or a fading mesh comes
    // out smoothly see-through. Our targets are single-sample, so the on/off
    // dither showed a screen-door instead (the owner's test11 screenshot).
    // Here the shader outputs the samples' coverage (0, 1/2, 1) as alpha and,
    // when the game drew RT0 opaque, the pipeline blends by it - the resolved
    // look. (A blended draw keeps the game's blend and the dither.)
    // [IzunaRecomp] test15: the coverage goes to the shader's second output
    // and the pipeline blends colour AND alpha by it (dual-source), so the
    // target keeps the game's own alpha, which NG2's bloom and exposure read
    // (test12 wrote the coverage into alpha and broke every scene). Only into
    // one colour target (Vulkan's dual-source limit), with a pixel shader that
    // has the output (its cache entry's mask), on a device with dualSrcBlend.
    const uint32_t b0 = key.blend[0];
    const bool opaque = (b0 & 31) == 1 && ((b0 >> 8) & 31) == 0 && ((b0 >> 5) & 7) == 0 &&
                        ((b0 >> 16) & 31) == 1 && ((b0 >> 24) & 31) == 0 && ((b0 >> 21) & 7) == 0;
    if (((d.Reg(RB_SURFACE_INFO) >> 16) & 3) == 1 && opaque && !colors[1] && !colors[2] && !colors[3] &&
        g.ctx.dual_src_blend && (ps->entry->specConstantsMask & kSpecAlphaToMaskMsaa) &&
        REXCVAR_GET(native_alpha_to_mask_blend)) {
      key.spec |= kSpecAlphaToMaskMsaa;
      key.raster |= kRasterCoverageBlend;
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_INFO("[native] alpha to mask on a 2x MSAA surface: blended by the samples' coverage (pixel shader "
                    "{:016X}, frame {})",
                    ps->entry->hash, g.frame_number);
      }
    }
  }
  bool pending = false;
  bool whole_pipeline = true;
  const VkPipeline pipeline = Pipeline(key, vs, ps, &pending, &whole_pipeline);
  clock.Mark(4);
  if (!pipeline) {
    ++(pending ? g.stats.pipeline_pending : g.stats.pipeline_failed);
    if (pending) ++g.frame_pending;
    return;
  }
  Bind(colors, depth);
  State::DynCache& dc = g.dyn;
  if (dc.cmd != g.cmd) {  // (a new command buffer: nothing set yet)
    dc = State::DynCache{};
    dc.cmd = g.cmd;
  }
  if (dc.pipeline != pipeline) {
    g.vk.CmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    dc.pipeline = pipeline;
    if (whole_pipeline) dc.gpl_valid = false;
  }
  if (g.gpl && !whole_pipeline) {  // (pipelines from parts: rasterisation and depth / stencil state are dynamic)
    const bool all = !dc.gpl_valid;
    dc.gpl_valid = true;
    const uint32_t cull = ((key.raster & 1) ? VK_CULL_MODE_FRONT_BIT : 0) | ((key.raster & 2) ? VK_CULL_MODE_BACK_BIT : 0);
    if (all || cull != dc.cull) g.CmdSetCullMode(g.cmd, dc.cull = cull);
    const uint32_t front = (key.raster & 4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if (all || front != dc.front) g.CmdSetFrontFace(g.cmd, VkFrontFace(dc.front = front));
    const uint32_t bias_enable = (key.raster & 8) ? 1u : 0u;
    if (all || bias_enable != dc.bias_enable) g.CmdSetDepthBiasEnable(g.cmd, dc.bias_enable = bias_enable);
    const uint32_t dctl = key.depth_control;
    const bool z = key.depth_format != 0;
    const uint32_t dtest = z && ((dctl >> 1) & 1), dwrite = z && ((dctl >> 2) & 1), dcompare = (dctl >> 4) & 7;
    const uint32_t stest = z && (dctl & 1);
    if (all || dtest != dc.dtest) g.CmdSetDepthTestEnable(g.cmd, dc.dtest = dtest);
    if (all || dwrite != dc.dwrite) g.CmdSetDepthWriteEnable(g.cmd, dc.dwrite = dwrite);
    if (all || dcompare != dc.dcompare) g.CmdSetDepthCompareOp(g.cmd, VkCompareOp(dc.dcompare = dcompare));
    if (all || stest != dc.stest) g.CmdSetStencilTestEnable(g.cmd, dc.stest = stest);
    auto face = [&](VkStencilFaceFlags f, uint32_t shift, uint32_t& last) {
      const uint32_t packed = (dctl >> shift) & 0xFFF;
      if (!all && packed == last) return;
      last = packed;
      g.CmdSetStencilOp(g.cmd, f, VkStencilOp((dctl >> (shift + 3)) & 7), VkStencilOp((dctl >> (shift + 6)) & 7),
                        VkStencilOp((dctl >> (shift + 9)) & 7), VkCompareOp((dctl >> shift) & 7));
    };
    face(VK_STENCIL_FACE_FRONT_BIT, 8, dc.sop_front);
    face(VK_STENCIL_FACE_BACK_BIT, ((dctl >> 7) & 1) ? 20 : 8, dc.sop_back);
    const uint32_t cmask = (key.stencil_masks >> 8) & 0xFF, wmask = (key.stencil_masks >> 16) & 0xFF;
    if (all || cmask != dc.cmask) g.CmdSetStencilCompareMask(g.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dc.cmask = cmask);
    if (all || wmask != dc.wmask) g.CmdSetStencilWriteMask(g.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dc.wmask = wmask);
  }
  const uint64_t push[5] = {vs_address, ps_address, shared_address, up_data ? up_data : g.ctx.guest_address,
                            fetch_address};
  if (!dc.push_valid || std::memcmp(push, dc.push, sizeof(push)) != 0) {
    g.vk.CmdPushConstants(g.cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                          sizeof(push), push);
    std::memcpy(dc.push, push, sizeof(push));
    dc.push_valid = true;
  }
  const bool common_all = !dc.common_valid;
  dc.common_valid = true;
  const VkViewport viewport = {0.0f, 0.0f, host_w, host_h, 0.0f, 1.0f};
  if (common_all || std::memcmp(&viewport, &dc.viewport, sizeof(viewport)) != 0) {
    g.vk.CmdSetViewport(g.cmd, 0, 1, &viewport);
    dc.viewport = viewport;
  }
  // The screen scissor (the device shadow; NG2 leaves it at 8192x8192) and
  // the window scissor (Direct3D's SetScissorRect: OnWindowScissor), as the
  // hardware clips: their intersection. (The boss's health bar is one red
  // layer the length of the bar, cut at the health by the window scissor.)
  const uint32_t tl = d.Reg(PA_SC_SCREEN_SCISSOR_TL), br = d.Reg(PA_SC_SCREEN_SCISSOR_BR);
  const uint32_t wtl = g.window_scissor_tl, wbr = g.window_scissor_br;
  int32_t x0 = std::max(int32_t(tl & 0x7FFF), int32_t(wtl & 0x7FFF));
  int32_t y0 = std::max(int32_t((tl >> 16) & 0x7FFF), int32_t((wtl >> 16) & 0x7FFF));
  int32_t x1 = int32_t(br & 0x7FFF), y1 = int32_t((br >> 16) & 0x7FFF);
  x1 = std::min<int32_t>(x1 ? x1 : int32_t(w), int32_t(w));
  y1 = std::min<int32_t>(y1 ? y1 : int32_t(h), int32_t(h));
  x1 = std::min(x1, int32_t(wbr & 0x7FFF));
  y1 = std::min(y1, int32_t((wbr >> 16) & 0x7FFF));
  x0 = std::min(x0, x1);
  y0 = std::min(y0, y1);
  {
    const float k = sized->scale, ky = sized->scale_y;
    x0 = ToHost(x0, k), y0 = ToHost(y0, ky), x1 = ToHost(x1, k), y1 = ToHost(y1, ky);
  }
  const VkRect2D scissor = {{x0, y0}, {uint32_t(std::max(x1 - x0, 0)), uint32_t(std::max(y1 - y0, 0))}};
  if (common_all || std::memcmp(&scissor, &dc.scissor, sizeof(scissor)) != 0) {
    g.vk.CmdSetScissor(g.cmd, 0, 1, &scissor);
    dc.scissor = scissor;
  }
  const float blend[4] = {AsFloat(d.Reg(RB_BLEND_RED)), AsFloat(d.Reg(RB_BLEND_RED + 1)),
                          AsFloat(d.Reg(RB_BLEND_RED + 2)), AsFloat(d.Reg(RB_BLEND_RED + 3))};
  if (common_all || std::memcmp(blend, dc.blend, sizeof(blend)) != 0) {
    g.vk.CmdSetBlendConstants(g.cmd, blend);
    std::memcpy(dc.blend, blend, sizeof(blend));
  }
  const uint32_t sref = d.Reg(RB_STENCILREFMASK) & 0xFF;
  if (common_all || sref != dc.sref) g.vk.CmdSetStencilReference(g.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dc.sref = sref);
  float bias[3] = {0.0f, 0.0f, 0.0f};
  if (key.raster & 8) {
    // The offset is in units of the depth format's precision; the scale is in
    // 1/16 subpixels (the SDK's factors). Our depth is D32_SFLOAT standing in
    // for the game's 24-bit formats.
    const bool float24 = (d.Reg(RB_DEPTH_INFO) >> 16) & 1;
    bias[0] = bias_offset * (float24 ? float(1u << 24) : float((1u << 24) - 1));
    // (per host pixel: a target RenderScale() times the guest's has that
    // fraction of the slope per pixel - scaled back, as Edge of Time does)
    bias[2] = bias_scale * (1.0f / 16.0f) * 0.5f * (sized->scale + sized->scale_y);
  }
  if (common_all || std::memcmp(bias, dc.bias, sizeof(bias)) != 0) {  // (dynamic in every pipeline: set)
    g.vk.CmdSetDepthBias(g.cmd, bias[0], bias[1], bias[2]);
    std::memcpy(dc.bias, bias, sizeof(bias));
  }
  g.vk.CmdDraw(g.cmd, vertices, 1, 0, 0);
  // A draw that exported to guest memory (NG2 skins its characters so: the
  // exports fill vertex buffers that later draws read): inside a render pass
  // the GPU overlaps consecutive draws and nothing orders those writes before
  // the next draws' reads - stale vertices for parts of a body. End the pass:
  // EndRendering's barrier makes the writes visible to everything after.
  if (vs->entry->memExport || (ps && ps->entry->memExport)) {
    ++t_timing.export_ends;
    EndRendering();
  }
  clock.Mark(5);
  if (g.pass_count && g.pass_count < 256) ++g.pass_labels[g.pass_count].draws;
  ++g.stats.drawn;
  ++g.frame_drawn;
  if (g.log_frame) {
    FrameLog("draw: prim {} count {} index {:08X}/{:X}/{} VS {:016X} PS {:016X} targets {:03X}/{:03X} "
                "fmt {} depth {:03X} mode {} mask {:04X} blend {:08X} dc {:08X} cc {:08X} aref {} clip {:08X} vte {:03X} "
                "vport {} {} {} {} scissor {:08X}-{:08X} window {:08X}-{:08X} pitch {} vsc0 {} {} {} {}",
                xenos_primitive, count, index_info[0], index_info[1], index_info[2], key.vs, key.ps,
                color_info[0] & 0xFFF, color_info[1] & 0xFFF, (color_info[0] >> 16) & 0xF,
                d.Reg(RB_DEPTH_INFO) & 0xFFF, mode, color_mask, key.blend[0], depth_control, d.Reg(RB_COLORCONTROL),
                AsFloat(d.Reg(RB_ALPHA_REF)), d.Reg(PA_CL_CLIP_CNTL), vte, sx, ox, sy, oy, tl,
                br, g.window_scissor_tl, g.window_scissor_br, pitch, AsFloat(Be32(vsc)), AsFloat(Be32(vsc + 4)), AsFloat(Be32(vsc + 8)), AsFloat(Be32(vsc + 12)));
    auto f4 = [](const uint8_t* p) {
      return fmt::format("({:.3g} {:.3g} {:.3g} {:.3g})", AsFloat(Be32(p)), AsFloat(Be32(p + 4)), AsFloat(Be32(p + 8)),
                         AsFloat(Be32(p + 12)));
    };
    std::string fetches;
    for (uint32_t k = 0; k < vs->entry->vertexLayoutCount && k < 4; ++k) {
      uint32_t q[4];
      std::memcpy(q, shared + 1664 + k * 16, 16);
      fetches += fmt::format(" [slot {} stride {} off {} fmt {} fl {:X}]", q[0] & 0xFF, q[0] >> 8, int32_t(q[1]), q[2], q[3]);
    }
    const uint32_t* vf = fetch + (vs->entry->vertexLayoutCount ? (*reinterpret_cast<const uint32_t*>(shared + 1664) & 0xFF) * 2 : 0);
    if (vs->entry->vertexLayoutCount) {
      auto* memory = REX_KERNEL_MEMORY();
      const uint8_t* vb = memory->TranslatePhysical<const uint8_t*>(vf[0] & 0x1FFFFFFCu);
      const uint32_t first = index_info[2] * ((*reinterpret_cast<const uint32_t*>(shared + 1664) >> 8) & 0xFFFFFF);
      std::string v;
      for (uint32_t i = 0; i < 6; ++i) v += fmt::format(" {:08X}", Be32(vb + (first + i) * 4));
      FrameLog("  first vertex (dwords, big-endian):{}", v);
    }
    for (uint32_t slot = 0; slot < 32; ++slot) {
      const uint32_t* fc = fetch + slot * 6;
      if ((fc[0] & 3) != 2) continue;
      rex::graphics::xenos::xe_gpu_texture_fetch_t tf;
      std::memcpy(&tf, fc, sizeof(tf));
      const uint32_t base = tf.base_address << 12;
      const uint8_t* tp = REX_KERNEL_MEMORY()->TranslatePhysical<const uint8_t*>(base);
      FrameLog("  tex slot {}: fmt {} sign {}{}{}{} {}x{} base {:08X} pitch {} tiled {} dim {} endian {} "
                  "swz {:03X} exp {} mips {:08X} {}-{}{} filter {}{}{} aniso {} bias {} resolved {} bytes {:08X} {:08X} {:08X}",
                  slot, uint32_t(tf.format), uint32_t(tf.sign_x), uint32_t(tf.sign_y), uint32_t(tf.sign_z),
                  uint32_t(tf.sign_w), tf.size_2d.width + 1, tf.size_2d.height + 1, base, tf.pitch * 32,
                  uint32_t(tf.tiled), uint32_t(tf.dimension), uint32_t(tf.endianness), uint32_t(tf.swizzle),
                  int32_t(tf.exp_adjust), uint32_t(tf.mip_address) << 12, uint32_t(tf.mip_min_level),
                  uint32_t(tf.mip_max_level), tf.packed_mips ? " packed" : "", uint32_t(tf.mag_filter),
                  uint32_t(tf.min_filter), uint32_t(tf.mip_filter), uint32_t(tf.aniso_filter),
                  int32_t(tf.lod_bias), g.resolved.count(fc[1] & 0x1FFFF000u), Be32(tp), Be32(tp + 4096), Be32(tp + 65536));
    }
    FrameLog("  ps c16-18 {} {} {} vs c21-24 {} {} {} {} fetch{} vfc {:08X} {:08X} tex0 {:08X} {:08X}",
                f4(psc + 256), f4(psc + 272), f4(psc + 288), f4(vsc + 336), f4(vsc + 352), f4(vsc + 368),
                f4(vsc + 384), fetches, vf[0], vf[1], fetch[0], fetch[1]);
    std::string vs_more;
    for (uint32_t c : {16u, 43u, 44u, 45u, 46u, 47u, 48u}) vs_more += fmt::format(" c{} {}", c, f4(vsc + c * 16));
    for (uint32_t c = 64; c < 72; ++c) vs_more += fmt::format(" c{} {}", c, f4(vsc + c * 16));
    FrameLog("  vs{} bools {:08X} {:08X}", vs_more, *reinterpret_cast<const uint32_t*>(shared + 320),
                *reinterpret_cast<const uint32_t*>(shared + 324));
    // [IzunaRecomp] Every float constant the pixel shader reads, and its bools
    // (pixel bools are 128-255: words 4-7) - the material's switches and values
    // (an alpha test's AlphaRef, a fade) for one draw, from a capture.
    if (ps) {
      std::string all;
      const uint32_t regs = std::min<uint32_t>(ps->entry->floatConstantRegs, 32u);
      for (uint32_t c = 0; c < regs; ++c) all += fmt::format(" c{} {}", c, f4(psc + c * 16));
      FrameLog("  ps consts{}; ps bools {:08X} {:08X} {:08X} {:08X}", all,
               *reinterpret_cast<const uint32_t*>(shared + 336), *reinterpret_cast<const uint32_t*>(shared + 340),
               *reinterpret_cast<const uint32_t*>(shared + 344), *reinterpret_cast<const uint32_t*>(shared + 348));
    }
    ProbeAfterDraw(colors[0], depth, color_info[0] & 0xFFF, key.ps, key.blend[0]);
  }
}

// Draw the pending BeginVertices draw: its vertices are written by now.
// The pending BeginVertices draw (its vertices are written by now): read now
// on the game's render thread; drawn there (DrawUp), or queued.
void FlushBeginVertices() {
  if (!g.begin.pending || OnWorker()) return;  // (the producer's state)
  g.begin.pending = false;
  const uint8_t* src = Guest(g.begin.data);
  const uint32_t bytes = g.begin.dwords * 4;
  const uint8_t* isrc = g.begin.index_dwords ? Guest(g.begin.index_data) : nullptr;
  const uint32_t ibytes = isrc ? g.begin.index_dwords * 4 : 0;
  if (!src || !bytes || (g.begin.index_dwords && !isrc)) return;
  uint32_t index_flags = 0;
  if (isrc) {
    Device d{g.begin_device};
    const bool reset = (d.Reg(PA_SU_SC_MODE_CNTL) >> 21) & 1;  // strip restart
    index_flags = g.begin.index_flags | (uint32_t(reset) << 3);
  }
  if (Threaded()) {
    QueueDrawUp(g.begin_device, g.begin_layout, g.begin_layout_bytes, g.begin_layout_ok, g.begin.primitive,
                g.begin.count, g.begin.dwords, src, bytes, isrc, ibytes, index_flags, g.begin.index_offset);
    return;
  }
  DrawUp(g.begin_device, g.begin.primitive, g.begin.count, g.begin.dwords, src, bytes, isrc, ibytes, index_flags,
         g.begin.index_offset, g.begin_layout, g.begin_layout_ok);
}

// A BeginVertices / BeginIndexedVertices draw: the vertices (and indices,
// right after them in the same block - the block stands in for guest memory,
// so the index address is their offset) copied to the upload ring and drawn.
void DrawUp(const uint8_t* device_bytes, uint32_t primitive, uint32_t count, uint32_t dwords, const uint8_t* src,
            uint32_t bytes, const uint8_t* isrc, uint32_t ibytes, uint32_t index_flags, uint32_t index_offset,
            const uint8_t* layout, bool layout_ok) {
  if (!Frame()) return;
  const uint32_t ioffset = (bytes + 15) & ~15u;
  const VkDeviceSize offset = (g.upload_used + 255) & ~VkDeviceSize(255);
  if (offset + ioffset + ibytes > kFrameUpload) {
    ++g.stats.upload_full;
    return;
  }
  g.upload_used = offset + ioffset + ibytes;
  const VkDeviceSize at = VkDeviceSize(g.frame_index) * kFrameUpload + offset;
  std::memcpy(g.upload_mapped + at, src, bytes);
  if (isrc) std::memcpy(g.upload_mapped + at + ioffset, isrc, ibytes);
  if (g.log_frame) {
    std::string v;
    for (uint32_t i = 0; i < std::min<uint32_t>(dwords, 24); ++i) v += fmt::format(" {:08X}", Be32(src + i * 4));
    FrameLog("begin-vertices prim {} count {} dwords {}{}:{}", primitive, count, dwords,
                isrc ? " (indexed)" : "", v);
  }
  Device d{device_bytes};
  uint32_t index_info[4] = {0xFFFFFFFFu, 0, 0, 0};
  if (isrc) {
    index_info[0] = ioffset;
    index_info[1] = index_flags;
    index_info[2] = index_offset;
    ++g.stats.indexed_up;
  }
  Draw(d, primitive, count, index_info, g.upload_address + at, 0x10000002u | (dwords << 2), layout, layout_ok);
}

}  // namespace draw

bool DrawInit(const DrawContext& context) {
  g_timed = rex::LoggingEnabled();  // (native_phase.h: before the worker and the builders start)
  if (!g_timed) {
    // (logging off: no report will print the renderer's timing - none is taken)
  } else {  // (what a clock read costs here: the per-draw timers use the TSC)
    constexpr int kReads = 20000;
    const uint64_t t0 = Ticks();
    uint64_t sink = 0;
    for (int i = 0; i < kReads; ++i) sink += uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    const uint64_t t1 = Ticks();
    for (int i = 0; i < kReads; ++i) sink += Ticks();
    const uint64_t t2 = Ticks();
    REXLOG_INFO("[native] clock: steady_clock {:.0f} ns a read, TSC {:.1f} ns a read ({:.3f} ns a tick){}",
                double(TicksToNs(t1 - t0)) / kReads, double(TicksToNs(t2 - t1)) / kReads, NsPerTick(),
                sink ? "" : " ");
  }
  g.ctx = context;
  if (!g.ctx.guest_address || !g.ctx.frame_commands) {
    REXLOG_WARN("[native] drawing off: no imported guest memory");
    return false;
  }
  auto load = [&](auto& fn, const char* name) {
    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(g.ctx.get_device_proc_addr(g.ctx.device, name));
    return fn != nullptr;
  };
  bool ok = true;
#define NG2_LOAD(name) ok &= load(g.vk.name, "vk" #name);
  NG2_DEVICE_FUNCTIONS(NG2_LOAD)
#undef NG2_LOAD
  if (!load(g.vk.CmdBeginRendering, "vkCmdBeginRendering")) load(g.vk.CmdBeginRendering, "vkCmdBeginRenderingKHR");
  if (!load(g.vk.CmdEndRendering, "vkCmdEndRendering")) load(g.vk.CmdEndRendering, "vkCmdEndRenderingKHR");
  g.vk.GetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
      g.ctx.get_instance_proc_addr(g.ctx.instance, "vkGetPhysicalDeviceMemoryProperties"));
  if (!ok || !g.vk.CmdBeginRendering || !g.vk.CmdEndRendering || !g.vk.GetPhysicalDeviceMemoryProperties) {
    REXLOG_ERROR("[native] drawing off: missing Vulkan functions (dynamic rendering?)");
    return false;
  }
#if defined(IZUNA_SHADERS_FROM_GAME)
  const auto* built = izuna::shaders::Active();
  if (!built) {
    REXLOG_ERROR("[native] drawing off: no shaders (the startup build from the game did not run)");
    return false;
  }
  g.spirv = built->spirv;
#else
  // The SPIR-V, decompressed once (~13 MB).
  g.spirv.resize(g_spirvCacheDecompressedSize);
  const size_t n = ZSTD_decompress(g.spirv.data(), g.spirv.size(), g_compressedSpirvCache, g_spirvCacheCompressedSize);
  if (ZSTD_isError(n) || n != g.spirv.size()) {
    REXLOG_ERROR("[native] drawing off: shader cache did not decompress");
    return false;
  }
#endif
  CreatePipelineCache();
  PrewarmImageMemory();  // [IzunaRecomp] (the first image blocks, before the first frame)
  for (size_t i = 0; i < kMaxListArenas; ++i) {  // [IzunaRecomp] (the strip lists' arenas too: test29)
    State::MappedBuffer arena;
    if (!CreateMappedBuffer(kListArenaBytes, 0, arena)) break;
    g.free_arenas.push_back(arena);
  }
  {  // Graphics pipeline libraries (with the dynamic states the parts leave out)
    auto load_dev = [&](auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(g.ctx.get_device_proc_addr(g.ctx.device, name));
      return fn != nullptr;
    };
    bool dyn = load_dev(g.CmdSetCullMode, "vkCmdSetCullMode") && load_dev(g.CmdSetFrontFace, "vkCmdSetFrontFace") &&
               load_dev(g.CmdSetDepthBiasEnable, "vkCmdSetDepthBiasEnable") &&
               load_dev(g.CmdSetDepthTestEnable, "vkCmdSetDepthTestEnable") &&
               load_dev(g.CmdSetDepthWriteEnable, "vkCmdSetDepthWriteEnable") &&
               load_dev(g.CmdSetDepthCompareOp, "vkCmdSetDepthCompareOp") &&
               load_dev(g.CmdSetStencilTestEnable, "vkCmdSetStencilTestEnable") &&
               load_dev(g.CmdSetStencilOp, "vkCmdSetStencilOp") &&
               load_dev(g.CmdSetStencilCompareMask, "vkCmdSetStencilCompareMask") &&
               load_dev(g.CmdSetStencilWriteMask, "vkCmdSetStencilWriteMask");
    g.gpl = g.ctx.graphics_pipeline_library && dyn && REXCVAR_GET(native_pipeline_libraries);
    REXLOG_INFO("[native] pipelines: {}", g.gpl ? "linked from per-shader parts (graphics pipeline libraries)"
                                                : fmt::format("whole (pipeline libraries {}, dynamic state {})",
                                                              g.ctx.graphics_pipeline_library ? "on the device" : "not on the device",
                                                              dyn ? "yes" : "no"));
  }
  {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = sizeof(kRectListGs);
    ci.pCode = kRectListGs;
    if (const VkResult r = g.vk.CreateShaderModule(g.ctx.device, &ci, nullptr, &g.rect_gs); r != VK_SUCCESS) {
      g.rect_gs = VK_NULL_HANDLE;
      REXLOG_WARN("[native] the rect-list geometry shader was refused ({})", int(r));
    }
    ci.codeSize = sizeof(kDepth24Fs);
    ci.pCode = kDepth24Fs;
    if (const VkResult r = g.vk.CreateShaderModule(g.ctx.device, &ci, nullptr, &g.depth24_fs); r != VK_SUCCESS) {
      g.depth24_fs = VK_NULL_HANDLE;
      REXLOG_WARN("[native] the 20e4 depth shader was refused ({})", int(r));
    }
  }
  if (!CreateLayout() || !CreateUpload()) {
    REXLOG_ERROR("[native] drawing off: could not create the pipeline layout or upload buffer");
    return false;
  }
  if (REXCVAR_GET(native_precompile_pipelines) && !REXCVAR_GET(native_ignore_pipeline_cache)) QueuePrecompile();
  REXLOG_INFO("[native] alpha to mask on 2x MSAA surfaces: {}",
              !REXCVAR_GET(native_alpha_to_mask_blend) ? "the on/off dither (native_alpha_to_mask_blend off)"
              : g.ctx.dual_src_blend                   ? "coverage-blended (dual-source blend on the device)"
                                                       : "the on/off dither (no dual-source blend on this device)");
  const int workers = std::clamp(REXCVAR_GET(native_pipeline_threads), 1, 6);
  for (int i = 0; i < workers; ++i) std::thread(PipelineWorker).detach();  // (they live as long as the app)
  WaitForPrecompile();  // [IzunaRecomp] test42: the game starts with its pipelines built
  {  // GPU time per pass of logged frames (ReadPassTimes)
    auto props_fn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        g.ctx.get_instance_proc_addr(g.ctx.instance, "vkGetPhysicalDeviceProperties"));
    VkPhysicalDeviceProperties props = {};
    if (props_fn) props_fn(g.ctx.physical_device, &props);
    if (props.limits.timestampComputeAndGraphics && props.limits.timestampPeriod > 0.0f) {
      VkQueryPoolCreateInfo qci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
      qci.queryCount = 256;
      if (g.vk.CreateQueryPool(g.ctx.device, &qci, nullptr, &g.pass_queries) != VK_SUCCESS) g.pass_queries = VK_NULL_HANDLE;
      g.timestamp_ns = props.limits.timestampPeriod;
    }
  }
  {  // Sampler limits: anisotropic filtering (enabled on the device when supported) and the LOD bias range
    auto props_fn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        g.ctx.get_instance_proc_addr(g.ctx.instance, "vkGetPhysicalDeviceProperties"));
    auto features_fn = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
        g.ctx.get_instance_proc_addr(g.ctx.instance, "vkGetPhysicalDeviceFeatures"));
    VkPhysicalDeviceProperties props = {};
    VkPhysicalDeviceFeatures features = {};
    if (props_fn) props_fn(g.ctx.physical_device, &props);
    if (features_fn) features_fn(g.ctx.physical_device, &features);
    g.max_anisotropy = features.samplerAnisotropy ? std::max(props.limits.maxSamplerAnisotropy, 1.0f) : 1.0f;
    g.max_lod_bias = props.limits.maxSamplerLodBias;
    g.depth_clamp = features.depthClamp;
    REXLOG_INFO("[native] samplers: anisotropy up to {}:1, LOD bias up to {}", g.max_anisotropy, g.max_lod_bias);
  }
  g.ready = true;
  REXLOG_INFO("[native] drawing on: {} shaders in the cache ({} MB of SPIR-V), {} shader objects seen so far",
              ShaderEntryCount(), g.spirv.size() >> 20, g.object_hash.size());
  return true;
}

// The container hashed at the creator's entry, until its object is complete
// (per thread: loader threads create shaders too).
thread_local uint64_t t_pending_hash = 0;

void OnShaderCreateEnter(uint32_t container) {
  t_pending_hash = 0;
  const uint8_t* p = Guest(container);
  if (!p || (Be32(p) & 0xFFFFFF00u) != 0x102A1100u) return;
  const uint32_t size = Be32(p + 4) + Be32(p + 8);
  t_pending_hash = XXH3_64bits(p, size);
}

void OnShaderCreated(uint32_t object) {
  if (!t_pending_hash) return;
  std::lock_guard<std::mutex> lock(g.object_mutex);
  g.object_hash[object] = t_pending_hash;
  g.object_generation.fetch_add(1, std::memory_order_acq_rel);
  t_pending_hash = 0;
}

void DrawEndSegment() {
  if (!g.ready || !g.cmd) return;
  EndRendering();
  g.cmd = VK_NULL_HANDLE;
}

void OnD3DCall() {
  if (g.begin.pending && std::this_thread::get_id() == g.render_thread) {
    ProducerTime timed;
    FlushBeginVertices();
  }
}


// The device and the vertex shader's fetch layout at a Begin*Vertices call
// (State::begin_device).
void CaptureBeginState(uint32_t device) {
  const uint8_t* dev = Guest(device);
  if (!dev) return;
  std::memcpy(g.begin_device, dev, sizeof(g.begin_device));
  g.begin_layout_bytes = 0;
  g.begin_layout_ok = true;
  const uint32_t object = Be32(dev + kVertexShader);
  if (const ShaderCacheEntry* e = EntryForObject(object)) {
    g.begin_layout_bytes = std::min<uint32_t>(e->vertexLayoutCount * 16u, sizeof(g.begin_layout));
    g.begin_layout_ok = FillVertexFetches(object, e, g.begin_layout);
  }
}

void OnBeginVertices(uint32_t device, uint32_t primitive, uint32_t count, uint32_t data, uint32_t dwords) {
  if (!g.ready) return;
  ProducerTime timed;
  FlushBeginVertices();
  g.render_thread = std::this_thread::get_id();
  g.begin = {true, device, primitive, count, data, dwords, 0, 0, 0, 0};
  CaptureBeginState(device);
}

void OnBeginIndexedVertices(uint32_t device, uint32_t data, uint32_t dwords, uint32_t index_data,
                            uint32_t index_dwords, uint32_t initiator, uint32_t index_word, uint32_t index_offset) {
  if (!g.ready) return;
  ProducerTime timed;
  FlushBeginVertices();
  g.render_thread = std::this_thread::get_id();
  const uint32_t flags = ((initiator >> 11) & 1) | (((index_word >> 30) & 3) << 1);
  g.begin = {true, device, initiator & 0x3F, initiator >> 16, data, dwords, index_data, index_dwords, flags,
             index_offset};
  CaptureBeginState(device);
}

void OnDrawIndexed(uint32_t device, uint32_t primitive, uint32_t base_vertex, uint32_t start_index,
                   uint32_t index_count) {
  if (!g.ready) return;
  ProducerTime timed;
  FlushBeginVertices();
  if (!index_count) return;
  Device d{Guest(device)};
  const uint32_t ib = d.Field(kIndexBuffer);
  const uint8_t* ibo = ib ? Guest(ib) : nullptr;
  if (!ibo) return;
  const uint32_t flags = Be32(ibo);
  const bool is32 = flags >> 31;
  const uint32_t endian = (flags >> 29) & 3;
  const bool reset = (d.Reg(PA_SU_SC_MODE_CNTL) >> 21) & 1;  // multi_prim_ib_ena: strip restart
  // (the base vertex from the call: the device's VGT_INDX_OFFSET is only
  // written by the call itself, after this hook)
  const uint32_t index_info[4] = {Physical(Be32(ibo + 24) + start_index * (is32 ? 4 : 2)),
                                  uint32_t(is32) | (endian << 1) | (uint32_t(reset) << 3), base_vertex, 0};
  if (Threaded()) {
    QueueDraw(device, primitive, index_count, index_info);
    return;
  }
  Draw(d, primitive, index_count, index_info);
}

void OnDraw(uint32_t device, uint32_t primitive, uint32_t start, uint32_t count) {
  if (!g.ready) return;
  ProducerTime timed;
  FlushBeginVertices();
  if (!count) return;
  Device d{Guest(device)};
  const uint32_t index_info[4] = {0xFFFFFFFFu, 0, start + d.Reg(VGT_INDX_OFFSET), 0};
  if (Threaded()) {
    QueueDraw(device, primitive, count, index_info);
    return;
  }
  Draw(d, primitive, count, index_info);
}

namespace draw {

// A sampled image a resolve (or ResolvedFit) writes: its descriptor, and an
// sRGB view of 8_8_8_8 for fetches marked gamma.
// A resolve that copies the target's bits as they are instead of converting
// values: 8_8_8_8 <-> 8_8_8_8_GAMMA (the hardware copies those bits; the R/B
// swap pairs with the fetch's swizzle).
bool RawResolve(VkFormat src, VkFormat dst) {
  auto rgba8 = [](VkFormat f) { return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB; };
  return rgba8(src) && rgba8(dst);
}

bool CreateResolvedImage(Image& img, VkFormat format, uint32_t w, uint32_t h) {
  // (16-bit float: also written by Saturate7e3 when resolved from a 7e3 target)
  const VkImageUsageFlags storage = format == VK_FORMAT_R16G16B16A16_SFLOAT ? VK_IMAGE_USAGE_STORAGE_BIT : 0;
  EndRendering();  // (the clear below: outside a pass)
  if (!CreateImage(g.cmd, img, format, w, h, false, VK_IMAGE_VIEW_TYPE_2D, {}, 1, storage)) {
    img = Image{};
    return false;
  }
  // Zero, as the render targets start: a resolve (or a fetch-sized copy)
  // writes only its rectangle, and the image's memory comes from a reused
  // block - what a pass sampled beyond the rectangle was an older image's
  // pixels (a bright one reads as glow).
  {
    const VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkClearColorValue v = {};
    g.vk.CmdClearColorImage(g.cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    Barrier(g.cmd);
  }
  img.descriptor = AllocDescriptor(false);
  if (img.descriptor) WriteTextureDescriptor(0, img.descriptor, img.view);
  if (format == VK_FORMAT_R8G8B8A8_UNORM) {
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_SRGB;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (g.vk.CreateImageView(g.ctx.device, &vci, nullptr, &img.view_srgb) == VK_SUCCESS) {
      img.descriptor_srgb = AllocDescriptor(false);
      if (img.descriptor_srgb) WriteTextureDescriptor(0, img.descriptor_srgb, img.view_srgb);
    }
  }
  return true;
}

}  // namespace

void OnResolve(uint32_t device, uint32_t flags, uint32_t rect, uint32_t clear_color, double clear_z) {
  if (!g.ready) return;
  int32_t r[4] = {};
  float c[4] = {};
  const uint8_t* rp = rect ? Guest(rect) : nullptr;
  const uint8_t* cp = clear_color ? Guest(clear_color) : nullptr;
  for (int i = 0; i < 4; ++i) {
    if (rp) r[i] = int32_t(Be32(rp + i * 4));
    if (cp) c[i] = AsFloat(Be32(cp + i * 4));
  }
  if (Threaded()) {
    ProducerTime timed;
    FlushBeginVertices();  // (a pending BeginVertices draw comes first)
    QueueResolve(device, flags, rp ? r : nullptr, cp ? c : nullptr, clear_z);
    return;
  }
  ScopedTime timed(t_timing.hooks_ns);
  FlushBeginVertices();
  ResolveWork(Guest(device), flags, rp ? r : nullptr, cp ? c : nullptr, clear_z);
}

// The resolve itself (on the game's render thread, or the worker with
// native_threaded): `device_bytes` the device (or its mirror), the source
// rect and clear colour as the call gave them (null: none).
void ResolveWork(const uint8_t* device_bytes, uint32_t flags, const int32_t* rect, const float* clear_color,
                 double clear_z) {
  if (!Frame()) return;
  if (REXCVAR_GET(native_debug) & 4) return;
  Device d{device_bytes};
  const uint32_t control = d.Reg(RB_COPY_CONTROL);
  const uint32_t source = control & 7;
  const uint32_t pitch = d.Reg(RB_SURFACE_INFO) & 0x3FFF;
  int32_t x0 = 0, y0 = 0, x1 = int32_t(pitch), y1 = int32_t(kRtHeight);
  if (rect) {
    x0 = rect[0], y0 = rect[1], x1 = rect[2], y1 = rect[3];
  }
  EndRendering();
  const Image* src = nullptr;
  bool src_7e3 = false;  // (a 2_10_10_10_FLOAT target: Saturate7e3 after the copy)
  if (source < 4) {
    const uint32_t regs[4] = {RB_COLOR_INFO, RB_COLOR1_INFO, RB_COLOR2_INFO, RB_COLOR3_INFO};
    const uint32_t info = d.Reg(regs[source]);
    src = Target(info & 0xFFF, (info >> 16) & 0xF, pitch, false);
    src_7e3 = ((info >> 16) & 0xF) == 3 || ((info >> 16) & 0xF) == 12;
    {  // [diag] EDRAM aliasing: what last wrote this base
      const uint32_t f = (info >> 16) & 0xF;
      const uint32_t want = (f == 12 ? 3u : f == 10 ? 2u : f) | pitch << 8;
      auto w = g.edram_writer.find(info & 0xFFF);
      if (w != g.edram_writer.end() && w->second != want) {
        if (++g.alias_reads <= 40 || g.log_frame) {
          REXLOG_INFO("[native] resolve reads EDRAM base {} as format {} pitch {}, last written as format {} pitch {} "
                      "(-> {:08X})",
                      info & 0xFFF, want & 0xFF, want >> 8, w->second & 0xFF, w->second >> 8, d.Reg(RB_COPY_DEST_BASE) & 0x1FFFF000u);
        }
      }
    }
  } else {
    src = Target(d.Reg(RB_DEPTH_INFO) & 0xFFF, 0, pitch, true);
  }
  if (!src) return;
  // (a GPU address: the GPU only masks it - unlike CPU pointers, no 0x1000
  // for the 0xE0000000 view; texture fetch constants hold the same form)
  const uint32_t dest = d.Reg(RB_COPY_DEST_BASE) & 0x1FFFF000u;
  const uint32_t dest_pitch_reg = d.Reg(RB_COPY_DEST_PITCH);
  const uint32_t dw = dest_pitch_reg & 0x3FFF, dh = (dest_pitch_reg >> 16) & 0x3FFF;
  // (the rect and the destination in guest pixels; the copies below in the
  // host's, RenderScale() times them)
  const float k = src->scale, ky = src->scale_y;
  x0 = std::clamp<int32_t>(x0, 0, int32_t(ToGuest(src->width, k)));
  y0 = std::clamp<int32_t>(y0, 0, int32_t(ToGuest(src->height, ky)));
  x1 = std::clamp<int32_t>(x1, x0, int32_t(ToGuest(src->width, k)));
  y1 = std::clamp<int32_t>(y1, y0, int32_t(ToGuest(src->height, ky)));
  if (src->depth && !g.depth_scratch) {
    ++g.stats.depth_resolves;  // (no scratch buffer: not copied)
  } else if (dest && dw && dh && x1 > x0 && y1 > y0) {
    // A depth resolve becomes a float texture holding the depth (what a
    // k_24_8 / k_24_8_FLOAT fetch of it reads in .x: shadow maps).
    // A colour resolve converts to the destination's format (RB_COPY_DEST_INFO
    // bits 7-12, a ColorFormat): a float target resolved to 8_8_8_8 is
    // clamped to 0-1 there, as the hardware does. (Its R/B swap, bit 24, pairs
    // with the swizzle the game's fetch of it applies; neither is applied to
    // resolved images, so they cancel as on the console.)
    const uint32_t dest_info = d.Reg(RB_COPY_DEST_INFO);
    // (the copy destination's format is numbered as texture formats are)
    VkFormat dst_format = VK_FORMAT_R32_SFLOAT;
    if (!src->depth) {
      const TexFormat df = Format(rex::graphics::xenos::TextureFormat((dest_info >> 7) & 0x3F));
      const bool compressed = df.block != 1;
      dst_format = (df.vk == VK_FORMAT_UNDEFINED || df.convert || compressed) ? src->format : df.vk;
    }
    Image& dst = g.resolved[dest];
    if (dst.image && (dst.width != ToHostSize(dw, k) || dst.height != ToHostSize(dh, ky) || dst.format != dst_format)) {
      auto variant_key = [&](VkFormat f, uint32_t vw, uint32_t vh) {
        const uint32_t k[4] = {dest, uint32_t(f), vw, vh};
        return XXH3_64bits(k, sizeof(k));
      };
      g.resolved_variants[variant_key(dst.format, dst.width, dst.height)] = dst;
      auto it = g.resolved_variants.find(variant_key(dst_format, ToHostSize(dw, k), ToHostSize(dh, ky)));
      if (it != g.resolved_variants.end()) {
        dst = it->second;
        g.resolved_variants.erase(it);
      } else {
        dst = Image{};
      }
    }
    if (!dst.image) {
      ++t_timing.creates_by[2];
      if (g.log_frame || t_timing.creates_by[2] < 64) {
        REXLOG_INFO("[native] resolve image {:08X}: {}x{} format {} (new)", dest, dw, dh, int(dst_format));
      }
    }
    if (!dst.image && !CreateResolvedImage(dst, dst_format, ToHostSize(dw, k), ToHostSize(dh, ky))) {
      g.resolved.erase(dest);
      return;
    }
    dst.scale = k;
    dst.scale_y = ky;
    dst.generation = ++g.resolve_generation;
    const uint32_t gcw = std::min<uint32_t>(uint32_t(x1 - x0), dw), gch = std::min<uint32_t>(uint32_t(y1 - y0), dh);
    const int32_t gdx = (uint32_t(x0) + gcw <= dw) ? x0 : 0, gdy = (uint32_t(y0) + gch <= dh) ? y0 : 0;
    // (host pixels from here; at a fractional scale the rect's size is rounded
    // once and kept inside both images)
    const int32_t dx = ToHost(gdx, k), dy = ToHost(gdy, ky);
    const int32_t sx0 = ToHost(x0, k), sy0 = ToHost(y0, ky);
    // (signed: at a fractional scale an offset rounded up may sit a pixel past
    // a size rounded down - ToHostSize)
    const uint32_t cw = uint32_t(std::max<int64_t>(
        0, std::min<int64_t>({int64_t(ToHost(gcw, k)), int64_t(src->width) - sx0, int64_t(dst.width) - dx})));
    const uint32_t ch = uint32_t(std::max<int64_t>(
        0, std::min<int64_t>({int64_t(ToHost(gch, ky)), int64_t(src->height) - sy0, int64_t(dst.height) - dy})));
    if (!cw || !ch) {
      // (nothing of the rect inside both images)
    } else if (src->depth) {
      VkBufferImageCopy r = {};
      r.bufferRowLength = cw;
      r.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      r.imageOffset = {sx0, sy0, 0};
      r.imageExtent = {cw, ch, 1};
      g.vk.CmdCopyImageToBuffer(g.cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, g.depth_scratch, 1, &r);
      Barrier(g.cmd);
      r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      r.imageOffset = {dx, dy, 0};
      g.vk.CmdCopyBufferToImage(g.cmd, g.depth_scratch, dst.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else if (dst_format != src->format && !RawResolve(src->format, dst_format)) {
      // converting (and clamping): a blit. (8_8_8_8 <-> 8_8_8_8_GAMMA copies the
      // bits as they are, as the hardware does - the copy below.)
      VkImageBlit b = {};
      b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      b.srcOffsets[0] = {sx0, sy0, 0};
      b.srcOffsets[1] = {sx0 + int32_t(cw), sy0 + int32_t(ch), 1};
      b.dstOffsets[0] = {dx, dy, 0};
      b.dstOffsets[1] = {dx + int32_t(cw), dy + int32_t(ch), 1};
      g.vk.CmdBlitImage(g.cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, dst.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                        VK_FILTER_NEAREST);
    } else {
      VkImageCopy c = {};
      c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      c.srcOffset = {sx0, sy0, 0};
      c.dstOffset = {dx, dy, 0};
      c.extent = {cw, ch, 1};
      g.vk.CmdCopyImage(g.cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, dst.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    // The EDRAM holds 7e3 targets saturated (0-31.875, 2-bit alpha); ours are
    // 16-bit float. A blit to a fixed-point format clamps by itself.
    if (src_7e3 && dst.format == VK_FORMAT_R16G16B16A16_SFLOAT && cw && ch) Saturate7e3(dst, dx, dy, cw, ch);
    if (((dest_info >> 16) & 0x3F) && !src->depth) {  // exponent bias: not applied yet
      static bool once = false;
      if (!once) {
        once = true;
        REXLOG_INFO("[native] resolve with an exponent bias (dest info {:08X}): not applied yet", dest_info);
      }
    }
    RefreshFits(dest, dst);
    g.last_resolved = dest;
    ++g.stats.resolves;
    if (g.log_frame && (g.capture_frame || REXCVAR_GET(native_dump_resolves))) DumpResolve(dst, dest);
    if (g.log_frame) MarkPass(dest, dw, dh);
    if (g.log_frame) {
      FrameLog("resolve: source {} ({}x{} fmt {}) rect {},{}-{},{} -> {:08X} {}x{} info {:08X} flags {:X}",
                  source, src->width, src->height, int(src->format), x0, y0, x1, y1, dest, dw, dh, dest_info, flags);
    }
  }
  // Clears that ride on the resolve (D3DRESOLVE_CLEARRENDERTARGET / ...DEPTHSTENCIL).
  if (flags & 0x100) {
    if (src && !src->depth && clear_color) {
      VkClearColorValue v;
      for (int i = 0; i < 4; ++i) v.float32[i] = clear_color[i];
      VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      g.vk.CmdClearColorImage(g.cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &sr);
    }
  }
  if (flags & 0x200) {
    if (const Image* z = Target(d.Reg(RB_DEPTH_INFO) & 0xFFF, 0, pitch, true)) {
      VkClearDepthStencilValue v = {float(clear_z), 0};
      VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
      g.vk.CmdClearDepthStencilImage(g.cmd, z->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &sr);
    }
  }
  Barrier(g.cmd);
}

namespace draw {
PendingClear g_clear;
}  // namespace draw

void OnClearBegin(uint32_t device, uint32_t flags, uint32_t rect, uint32_t color, double z, uint32_t stencil) {
  if (!g.ready) return;
  g_clear = PendingClear{};
  g_clear.pending = true;
  g_clear.device = device;
  g_clear.flags = flags;
  g_clear.z = z;
  g_clear.stencil = stencil;
  if (const uint8_t* r = rect ? Guest(rect) : nullptr) {
    for (int i = 0; i < 4; ++i) g_clear.rect[i] = int32_t(Be32(r + i * 4));
  }
  if (const uint8_t* c = color ? Guest(color) : nullptr) {
    for (int i = 0; i < 4; ++i) g_clear.color[i] = AsFloat(Be32(c + i * 4));
  }
}

void ClearWork(const uint8_t* device_bytes, const PendingClear& clear);

void OnClearEnd() {
  if (!g.ready || !g_clear.pending) return;
  g_clear.pending = false;
  if (Threaded()) {
    ProducerTime timed;
    FlushBeginVertices();
    QueueClear(g_clear);
    return;
  }
  ScopedTime timed(t_timing.hooks_ns);
  FlushBeginVertices();
  ClearWork(Guest(g_clear.device), g_clear);
}

void OnWindowScissor(uint32_t tl, uint32_t br) {
  if (!g.ready) return;
  if (Threaded()) {
    ProducerTime timed;
    FlushBeginVertices();  // (a pending BeginVertices draw has the scissor before this)
    QueueWindowScissor(tl, br);
    return;
  }
  FlushBeginVertices();
  g.window_scissor_tl = tl;
  g.window_scissor_br = br;
}

// The clear itself (see ResolveWork): `device_bytes` the device or its mirror.
void ClearWork(const uint8_t* device_bytes, const PendingClear& clear) {
  if (!Frame()) return;
  Device d{device_bytes};
  const uint32_t pitch = d.Reg(RB_SURFACE_INFO) & 0x3FFF;
  // The surfaces bound (device + 12432: render targets 0-3, + 12448 depth)
  // say which targets exist; the shadow says where they are in EDRAM.
  const uint8_t* dev = device_bytes;
  std::array<const Image*, 4> colors{};
  const uint32_t regs[4] = {RB_COLOR_INFO, RB_COLOR1_INFO, RB_COLOR2_INFO, RB_COLOR3_INFO};
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(clear.flags & (1u << i)) || !Be32(dev + 12432 + i * 4)) continue;
    const uint32_t info = d.Reg(regs[i]);
    colors[i] = Target(info & 0xFFF, (info >> 16) & 0xF, pitch, false);
  }
  const Image* depth = nullptr;
  if ((clear.flags & 0x30) && Be32(dev + 12448)) depth = Target(d.Reg(RB_DEPTH_INFO) & 0xFFF, 0, pitch, true);
  if (!depth && !colors[0] && !colors[1] && !colors[2] && !colors[3]) return;
  Bind(colors, depth);
  std::array<VkClearAttachment, 5> att{};
  uint32_t n = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!colors[i]) continue;
    att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    att[n].colorAttachment = i;
    for (int c = 0; c < 4; ++c) att[n].clearValue.color.float32[c] = clear.color[c];
    ++n;
  }
  if (depth) {
    // (D3DCLEAR_ZBUFFER 0x10, D3DCLEAR_STENCIL 0x20: the game clears its
    // GEQUAL-tested scene depth with 0x1F and Z 0)
    att[n].aspectMask = ((clear.flags & 0x10) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                        ((clear.flags & 0x20) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    att[n].clearValue.depthStencil = {float(std::clamp(clear.z, 0.0, 1.0)), clear.stencil & 0xFF};
    ++n;
  }
  // (the rect, within the targets: EDRAM surfaces are pitch x kRtHeight here)
  const Image* any = depth ? depth : colors[0] ? colors[0] : colors[1] ? colors[1] : colors[2] ? colors[2] : colors[3];
  const float ks = any->scale, ksy = any->scale_y;  // (the rect in guest pixels, the clear in the host's)
  const int32_t x0 = std::clamp<int32_t>(ToHost(int32_t(clear.rect[0]), ks), 0, int32_t(any->width));
  const int32_t y0 = std::clamp<int32_t>(ToHost(int32_t(clear.rect[1]), ksy), 0, int32_t(any->height));
  const int32_t x1 = std::clamp<int32_t>(ToHost(int32_t(clear.rect[2]), ks), x0, int32_t(any->width));
  const int32_t y1 = std::clamp<int32_t>(ToHost(int32_t(clear.rect[3]), ksy), y0, int32_t(any->height));
  if (x1 > x0 && y1 > y0) {
    VkClearRect cr = {{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1};
    g.vk.CmdClearAttachments(g.cmd, n, att.data(), 1, &cr);
  }
  ++g.stats.clears;
  if (g.log_frame) {
    FrameLog("clear: flags {:02X} rect {},{}-{},{} colour ({} {} {} {}) z {} stencil {} targets {:03X} "
                "{:03X} depth {:03X} pitch {}",
                clear.flags, x0, y0, x1, y1, clear.color[0], clear.color[1], clear.color[2], clear.color[3],
                clear.z, clear.stencil, d.Reg(RB_COLOR_INFO) & 0xFFF, d.Reg(RB_COLOR1_INFO) & 0xFFF,
                d.Reg(RB_DEPTH_INFO) & 0xFFF, pitch);
  }
  {  // (once: the first clear, to check the argument reading)
    static bool once = false;
    if (!once) {
      once = true;
      REXLOG_INFO("[native] first clear: flags {:02X} rect {},{}-{},{} colour ({} {} {} {}) z {} stencil {}",
                  clear.flags, clear.rect[0], clear.rect[1], clear.rect[2], clear.rect[3], clear.color[0],
                  clear.color[1], clear.color[2], clear.color[3], clear.z, clear.stencil);
    }
  }
}

bool DrawEndFrame(uint32_t device, uint32_t front_texture, VkImage target, bool target_used, uint32_t width,
                  uint32_t height) {
  (void)device;
  ScopedTime timed(t_timing.hooks_ns);
  ++t_timing.frames;
  if (g.ready) FlushBeginVertices();
  if (!g.ready || !g.cmd) return false;
  EndRendering();
  // The front buffer: the texture Swap presents, normally the last resolve.
  // NG2's front buffer textures start a page before the resolve that fills
  // them (texture 1EE46000, resolved to 1EE47000; 1F1DE000 / 1F1DF000): the
  // newest resolve inside the texture's first 64 KB is its picture. (Exact
  // lookups always missed, so every swap showed the frame's last resolve -
  // right only while the front buffer was resolved last.)
  const Image* front = nullptr;
  if (const uint8_t* t = front_texture ? Guest(front_texture) : nullptr) {
    const uint32_t base = Be32(t + 0x1C + 4) & 0x1FFFF000u;
    for (auto& [dest, img] : g.resolved) {
      if (dest >= base && dest < base + 0x10000 && img.image && (!front || img.generation > front->generation)) {
        front = &img;
      }
    }
  }
  if (!front) {
    auto it = g.resolved.find(g.last_resolved);
    if (it != g.resolved.end()) front = &it->second;
    static uint32_t logged = 0;
    if (front_texture && logged < 4) {
      ++logged;
      REXLOG_INFO("[native] swap: no resolve for the front texture {:08X}; showing the last resolve ({:08X})",
                  front_texture, g.last_resolved);
    }
  }
  if (REXCVAR_GET(native_debug) & 2) {
    for (auto& [key, img] : g.targets) {
      if (!img.depth && img.format == VK_FORMAT_R8G8B8A8_UNORM) front = &img;
    }
  }
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.oldLayout = target_used ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = target;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  g.vk.CmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                          nullptr, 1, &b);
  if (front) {
    VkImageBlit blit = {};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(front->width), int32_t(front->height), 1};
    blit.dstOffsets[1] = {int32_t(width), int32_t(height), 1};
    g.vk.CmdBlitImage(g.cmd, front->image, VK_IMAGE_LAYOUT_GENERAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                      &blit, VK_FILTER_LINEAR);
  } else {
    VkClearColorValue black = {};
    g.vk.CmdClearColorImage(g.cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &b.subresourceRange);
  }
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  g.vk.CmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                          0, nullptr, 1, &b);
  if (g.log_frame) {
    const uint8_t* ft = front_texture ? Guest(front_texture) : nullptr;
    REXLOG_INFO("[native] swap: front texture {:08X} (fetch {:08X} {:08X} {:08X}) -> {}", front_texture,
                ft ? Be32(ft + 0x1C) : 0, ft ? Be32(ft + 0x20) : 0, ft ? Be32(ft + 0x24) : 0,
                front ? fmt::format("{}x{} image", front->width, front->height) : std::string("none"));
  }
  if (g.log_frame && g.pass_count) {  // (the logged frame's last pass, then its times pending)
    MarkPass(0, 0, 0);
    g.pass_frame = g.frame_number + 1;
  }
  ++g.frame_number;
  g.last_frame_drawn = g.frame_drawn;
  g.frame_drawn = 0;
  g.last_frame_pending = g.frame_pending;
  g.frame_pending = 0;
  g.cmd = VK_NULL_HANDLE;
  return true;
}

void NoteFenceWait(uint64_t ns) { t_timing.fence_ns += ns; }

float RenderScale() {
  // (decided once: the targets, resolves and frames are made at this size)
  // [IzunaRecomp] Any scale from 1 to 3: the picture is 1280x720 times it
  // (1.5: 1920x1080, 1.25: 1600x900, 2.5: 3200x1800 - each a true resolution,
  // the frame shown 1:1). Whole ones as before; at a fractional one the small
  // surfaces stay 1x (Target) and sizes round down (ToHostSize).
  static const float scale = [] {
    const double want = REXCVAR_GET(native_resolution_scale);
    return float(std::clamp(std::isfinite(want) ? want : 1.0, 1.0, 3.0));
  }();
  return scale;
}

void DrawReport(uint64_t frame) {
  SavePipelineCache();  // (every ~10 s, when it grew)
  if (Threaded() && t_timing.frames) {
    REXLOG_INFO("[native] threaded: the game's render thread spent {:.2f} ms a frame recording (the times below "
                "are the worker's)",
                double(ProducerNanoseconds()) / double(t_timing.frames) / 1e6);
  }
  if (t_timing.frames) {
    const double n = double(t_timing.frames) * 1e6;
    REXLOG_INFO("[native] time per frame: {:.2f} ms in the native hooks (textures {:.2f} ms), {:.2f} ms waiting for "
                "the GPU; pipelines: {} queued, {} built; images hold {} MB",
                t_timing.hooks_ns / n, t_timing.texture_ns / n, t_timing.fence_ns / n,
                g.jobs_queued.load(), g.pipelines_built.load(), g_image_bytes.load() >> 20);
    if (g.gpl) {
      const uint32_t links = g.links.load();
      REXLOG_INFO("[native] pipeline libraries: {} shader parts, {} pipelines linked ahead of their first draw, {} "
                  "at it ({:.2f} ms each on average, {:.2f} ms at worst), {} optimised so far",
                  g.shader_libs.size(), g.prelinked.load(), links,
                  links ? double(g.link_ns.load()) / 1e6 / links : 0.0, double(g.link_worst_ns.load()) / 1e6,
                  g.optimized.load());
    }
    const uint64_t* st = t_timing.stage_ns;
    REXLOG_INFO("[native] draw stages, ms a frame ({:.0f} draws a frame): shaders+targets {:.2f}, constants {:.2f}, "
                "vertex fetches {:.2f}, texture slots {:.2f}, shared upload+pipeline {:.2f}, recording {:.2f}",
                double(t_timing.draws) * 1e6 / n, st[0] / n, st[1] / n, st[2] / n, st[3] / n, st[4] / n, st[5] / n);
    const double frames = double(t_timing.frames);
    REXLOG_INFO("[native] render passes a frame: {:.1f} ended ({:.1f} for fetch-sized copies at a draw; {:.1f} "
                "made at their resolve instead; {:.1f} after memory-export draws)",
                t_timing.pass_ends / frames, t_timing.fit_copies / frames, t_timing.fit_copies_at_resolve / frames,
                t_timing.export_ends / frames);
    REXLOG_INFO("[native] texture slots a frame: {:.0f} slot-cache misses ({:.0f} found in the frame's table); "
                "resolved lookups {:.2f} ms, textures {:.2f} ms, samplers {:.2f} ms",
                t_timing.slot_misses / frames, t_timing.slot_frame_hits / frames, t_timing.slot_resolved_ns / n,
                t_timing.slot_texture_ns / n, t_timing.slot_sampler_ns / n);
    REXLOG_INFO("[native] textures a frame: {:.0f} lookups, {:.1f} content hashes ({:.0f} KB, {:.2f} ms), {:.2f} "
                "decodes ({:.2f} ms, of which {:.2f} new images: {:.2f} ms)",
                t_timing.tex_lookups / frames, t_timing.tex_hashes / frames,
                t_timing.tex_hash_bytes / frames / 1024.0, t_timing.tex_hash_ns / n, t_timing.tex_decodes / frames,
                t_timing.tex_decode_ns / n, t_timing.tex_creates / frames, t_timing.tex_create_ns / n);
    REXLOG_INFO("[native] image creation, ms a frame: vkCreateImage {:.2f}, memory {:.2f}, view {:.2f}, barrier "
                "{:.2f}; created in the period: {} textures, {} render targets, {} resolves, {} fetch-sized copies",
                t_timing.create_part_ns[0] / n, t_timing.create_part_ns[1] / n, t_timing.create_part_ns[2] / n,
                t_timing.create_part_ns[3] / n, t_timing.tex_creates, t_timing.creates_by[1], t_timing.creates_by[2],
                t_timing.creates_by[3]);
  }
  t_timing = Timing{};
  const Stats& s = g.stats;
  REXLOG_INFO("[native] frame {}: {} draws drawn; skipped: {} no vertex shader, {} unknown pixel shader, {} rect "
              "lists, {} EDRAM copies, {} no target, {} target format, {} upload full, {} pipeline failed, {} pipeline "
              "not built yet, {} hidden by mods; {} clears, {} indexed vertex-buffer-less draws, {} resolves "
              "(+{} depth, not copied); {} shader objects ({} not in the cache), {} pipelines, {} targets, {} resolved "
              "textures; textures: {} uploaded ({} KB, {} from mods), {} unsupported format, {} deferred, {} cached",
              frame, s.drawn, s.no_vs, s.unknown_shader, s.rect, s.copy_mode, s.no_target, s.bad_format,
              s.upload_full, s.pipeline_failed, s.pipeline_pending, s.hidden, s.clears, s.indexed_up, s.resolves, s.depth_resolves, g.object_hash.size(),
              s.shaders_unknown, g.pipelines.size(), g.targets.size(), g.resolved.size(), s.textures_uploaded,
              s.texture_bytes >> 10, s.textures_replaced, s.textures_unsupported, s.textures_deferred, g.textures.size());
  {  // Draw census (see State::census): the draws the hooks got, by primitive.
    std::string by_prim;
    for (uint32_t p = 0; p < 32; ++p) {
      if (g.census[p]) by_prim += fmt::format(" prim {}: {}", p, g.census[p]);
    }
    REXLOG_INFO("[native] draw census:{}; {} clears, {} resolves; draws by MSAA 1x/2x/4x: {}/{}/{}", by_prim,
                s.clears, s.resolves + s.depth_resolves, s.msaa[0], s.msaa[1], s.msaa[2]);
    g.census = {};
  }
  g.stats = Stats{};
  g.stats.shaders_unknown = s.shaders_unknown;
}

}  // namespace ng2native

namespace ng2native {
bool WatchedTextureSeen() { return g_watched_seen.load(std::memory_order_relaxed); }
}  // namespace ng2native

namespace ng2native {
bool LoggingFrame() { return g.log_frame; }

bool RequestCapture(uint32_t frames) {
  if (!g.ready) return false;
  g_capture_frames.store(std::min<uint32_t>(frames, 8), std::memory_order_relaxed);
  return true;
}

void SetTextureLoadHook(rex::graphics::GuestTextureLoadHook hook) {
  g_texture_load_hook.store(hook, std::memory_order_release);
}
}  // namespace ng2native
