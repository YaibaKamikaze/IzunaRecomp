// [IzunaRecomp] The native renderer's drawing, shared by its files: the
// state (State g), the types every part uses, and what each part offers the
// others. Internal to the renderer - native_draw.h is its interface.
//
//   native_draw.cpp       images, frames, uploads, shaders, targets, the draw,
//                         resolves, clears, the Direct3D hooks
//   native_pipelines.cpp  pipelines (whole, or linked from per-shader parts),
//                         their workers, the pipeline cache and precompile
//   native_textures.cpp   textures decoded from guest memory, samplers
//   native_worker.cpp     threaded recording (native_threaded)
//   native_capture.cpp    developer captures: resolve dumps, the pixel probe,
//                         the background file saver
#pragma once

#include "native/native_draw.h"
#include "native/native_renderer.h"

#include <cmath>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <string>
#include <fstream>
#include <functional>
#include <map>
#include <tuple>
#include <mutex>
#include <thread>
#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#include <immintrin.h>
#endif
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/external_frame.h>
#include "native/native_phase.h"
#include <rex/filesystem.h>
#include <rex/graphics/texture_load_hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/audio/host_stall.h>
#include <rex/system/xmemory.h>
#include <rex/graphics/xenos.h>
#define XXH_STATIC_LINKING_ONLY  // (XXH3_state_t)
#include <xxhash.h>

#include "native/shader_cache.h"
#include "ps5/ps5_paths.h"
#include "../../third_party/smol-v/smolv.h"
#include "../../third_party/zstd/zstd.h"

// The renderer's settings (defined in native_draw.cpp).
REXCVAR_DECLARE(int32_t, native_log_frame);
REXCVAR_DECLARE(int32_t, native_log_count);
REXCVAR_DECLARE(int32_t, native_debug);
REXCVAR_DECLARE(double, native_resolution_scale);
REXCVAR_DECLARE(int32_t, native_auto_log);
REXCVAR_DECLARE(int32_t, native_anisotropic);
REXCVAR_DECLARE(bool, native_dump_textures);
REXCVAR_DECLARE(std::string, native_log_on_ps);
REXCVAR_DECLARE(int32_t, native_log_on_delay);
REXCVAR_DECLARE(std::string, native_log_on_tex);
REXCVAR_DECLARE(std::string, native_watch_tex);
REXCVAR_DECLARE(std::string, native_probe);
REXCVAR_DECLARE(bool, native_dump_resolves);
REXCVAR_DECLARE(bool, native_ignore_pipeline_cache);
REXCVAR_DECLARE(int32_t, native_pipeline_threads);
REXCVAR_DECLARE(bool, native_pipeline_libraries);
REXCVAR_DECLARE(bool, native_threaded);
REXCVAR_DECLARE(bool, native_precompile_pipelines);
REXCVAR_DECLARE(int32_t, native_texture_recheck_frames);
REXCVAR_DECLARE(int32_t, native_texture_idle_frames);

namespace ng2native {
namespace draw {

// --- Vulkan functions -------------------------------------------------------

#define NG2_DEVICE_FUNCTIONS(X)                                                                      \
  X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory)   \
  X(GetBufferDeviceAddress) X(CreateImage) X(GetImageMemoryRequirements) X(BindImageMemory)          \
  X(AllocateMemory) X(FreeMemory) X(DestroyImage) X(DestroyImageView) X(CreateImageView) X(CreateShaderModule) X(CreatePipelineLayout)                 \
  X(CreateDescriptorSetLayout) X(CreateDescriptorPool) X(AllocateDescriptorSets)                     \
  X(UpdateDescriptorSets) X(CreateGraphicsPipelines) X(CreateSampler) X(CmdBindPipeline)             \
  X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw)         \
  X(CmdPipelineBarrier) X(CmdCopyImage) X(CmdBlitImage) X(CmdClearColorImage) X(CmdClearAttachments) \
  X(CmdClearDepthStencilImage) X(CmdSetBlendConstants) X(CmdSetStencilReference) X(CmdCopyBufferToImage)  \
  X(CreatePipelineCache) X(GetPipelineCacheData) X(CmdCopyImageToBuffer) X(CmdSetDepthBias)          \
  X(CreateQueryPool) X(CmdResetQueryPool) X(CmdWriteTimestamp) X(GetQueryPoolResults)                \
  X(CreateComputePipelines) X(CmdDispatch)

struct Fns {
#define NG2_DECLARE(name) PFN_vk##name name = nullptr;
  NG2_DEVICE_FUNCTIONS(NG2_DECLARE)
#undef NG2_DECLARE
  PFN_vkCmdBeginRendering CmdBeginRendering = nullptr;
  PFN_vkCmdEndRendering CmdEndRendering = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
};

// --- Guest memory helpers ---------------------------------------------------

// Big-endian words to native order, 32 bytes a step (the constants and fetch
// constants of every draw: ~5 KB at most).
inline void SwapCopy32(uint32_t* dst, const uint8_t* src, uint32_t words) {
  uint32_t i = 0;
#if defined(__AVX2__)
  const __m256i shuffle = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                                           3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  for (; i + 8 <= words; i += 8) {
    const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i * 4));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_shuffle_epi8(v, shuffle));
  }
#endif
  for (; i < words; ++i) {
    uint32_t w;
    std::memcpy(&w, src + i * 4, 4);
    dst[i] = __builtin_bswap32(w);
  }
}

inline uint32_t Be32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

inline const uint8_t* Guest(uint32_t address) {
  auto* memory = REX_KERNEL_MEMORY();
  return memory ? memory->TranslateVirtual<const uint8_t*>(address) : nullptr;
}

// A guest virtual address of physical memory -> the physical address (as the
// GPU sees it; the 0xE0000000 view is offset by 0x1000, as D3D computes it).
inline uint32_t Physical(uint32_t address) {
  return (address & 0x1FFFFFFFu) + (address >= 0xE0000000u ? 0x1000u : 0u);
}

// The device object's register shadow ([device_shadow] in
// config/d3d_functions.toml): dense runs of registers from a group's base.
struct ShadowGroup {
  uint32_t base, offset, count;
};
constexpr ShadowGroup kGroups[] = {
    {0x2000, 10368, 19}, {0x2100, 10444, 21}, {0x2180, 10528, 5},  {0x2200, 10548, 12},
    {0x2280, 10596, 21}, {0x2300, 10680, 38}, {0x2380, 10832, 8},  {0x4900, 10112, 40},
};
constexpr uint32_t kFetchConstants = 1152;  // 32 x 6 dwords (Xenos order)
constexpr uint32_t kVsConstants = 1920;     // 256 x float4
constexpr uint32_t kPsConstants = 6016;     // 256 x float4
constexpr uint32_t kPixelShader = 12684, kVertexShader = 12688, kIndexBuffer = 12428;

struct Device {
  const uint8_t* p = nullptr;
  uint32_t Reg(uint32_t reg) const {
    for (const ShadowGroup& g : kGroups) {
      if (reg >= g.base && reg < g.base + g.count) return Be32(p + g.offset + (reg - g.base) * 4);
    }
    return 0;
  }
  uint32_t Field(uint32_t offset) const { return Be32(p + offset); }
};

// Time in the hooks on the game's render thread (t_timing.hooks_ns, or the
// producer's own count when the worker does the drawing).
// The per-draw timers' clock: the CPU's time-stamp counter. Each draw read
// the clock 6-10 times (the stage marks, the texture and hook timers);
// steady_clock costs ~20 ns on Linux, but on the PS5 a clock_gettime may go
// through the kernel - DrawInit logs both costs ("[native] clock:").
#if defined(__PROSPERO__) && !defined(NG2_PS5_HOST_TEST)
extern "C" uint64_t sceKernelReadTsc(void);
extern "C" uint64_t sceKernelGetTscFrequency(void);
inline uint64_t Ticks() { return sceKernelReadTsc(); }
inline double NsPerTick() {
  static const double v = 1e9 / double(std::max<uint64_t>(sceKernelGetTscFrequency(), 1));
  return v;
}
#elif defined(__aarch64__)
// [IzunaRecomp] Android: the ARM generic timer - a constant-rate counter
// readable from user space, its rate in cntfrq_el0.
inline uint64_t Ticks() {
  uint64_t v;
  asm volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
}
inline double NsPerTick() {
  static const double v = [] {
    uint64_t f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return 1e9 / double(std::max<uint64_t>(f, 1));
  }();
  return v;
}
#else
inline uint64_t Ticks() { return __rdtsc(); }
inline double NsPerTick() {
  static const double v = [] {
    const auto c0 = std::chrono::steady_clock::now();
    const uint64_t t0 = __rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint64_t t1 = __rdtsc();
    const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - c0).count());
    return t1 > t0 ? ns / double(t1 - t0) : 1.0;
  }();
  return v;
}
#endif
inline uint64_t TicksToNs(uint64_t ticks) { return uint64_t(double(ticks) * NsPerTick()); }

struct ProducerTime {
  uint64_t t0 = g_timed ? Ticks() : 0;  // (g_timed: native_phase.h)
  ~ProducerTime();
};

// Xenos register numbers used here.
enum : uint32_t {
  RB_SURFACE_INFO = 0x2000, RB_COLOR_INFO = 0x2001, RB_DEPTH_INFO = 0x2002,
  RB_COLOR1_INFO = 0x2003, RB_COLOR2_INFO = 0x2004, RB_COLOR3_INFO = 0x2005,
  PA_SC_SCREEN_SCISSOR_TL = 0x200E, PA_SC_SCREEN_SCISSOR_BR = 0x200F,  // (0x200C/D before test50: COHER_DEST_BASE_6/7)
  VGT_INDX_OFFSET = 0x2102, VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103, RB_COLOR_MASK = 0x2104,
  RB_BLEND_RED = 0x2105, RB_STENCILREFMASK = 0x210D, RB_ALPHA_REF = 0x210E,
  PA_CL_VPORT_XSCALE = 0x210F, PA_CL_VPORT_XOFFSET = 0x2110, PA_CL_VPORT_YSCALE = 0x2111,
  PA_CL_VPORT_YOFFSET = 0x2112, PA_CL_VPORT_ZSCALE = 0x2113, PA_CL_VPORT_ZOFFSET = 0x2114,
  RB_DEPTHCONTROL = 0x2200, RB_BLENDCONTROL0 = 0x2201, RB_COLORCONTROL = 0x2202,
  PA_CL_CLIP_CNTL = 0x2204, PA_SU_SC_MODE_CNTL = 0x2205, PA_CL_VTE_CNTL = 0x2206, RB_MODECONTROL = 0x2208,
  RB_BLENDCONTROL1 = 0x2209, PA_SU_VTX_CNTL = 0x2302, PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380,
  RB_COPY_CONTROL = 0x2318, RB_COPY_DEST_BASE = 0x2319, RB_COPY_DEST_PITCH = 0x231A,
  RB_COPY_DEST_INFO = 0x231B, SQ_BOOL_CONSTANTS = 0x4900, SQ_LOOP_CONSTANTS = 0x4908,
};

inline float AsFloat(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// --- Shaders ----------------------------------------------------------------

struct ShaderInfo {
  const ShaderCacheEntry* entry = nullptr;
  VkShaderModule module = VK_NULL_HANDLE;
};

// --- Images -----------------------------------------------------------------

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0;
  bool depth = false;
  uint32_t descriptor = 0;  // index in the 2D texture table (0: none / placeholder)
  // A 1-texel-high texture is also an image of its own for the 1D table:
  // shaders may sample a 256x1 palette as 1D (tfetch1D) though its fetch
  // constant says 2D, and Vulkan can't view a 2D image as 1D.
  VkImage image_1d = VK_NULL_HANDLE;
  VkImageView view_1d = VK_NULL_HANDLE;
  VkDeviceMemory memory_1d = VK_NULL_HANDLE;
  uint32_t descriptor_1d = 0;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize bytes = 0;
  // Where its memory came from (AllocImageMemory): a block's range, or its
  // own allocation (block -1). The same for the 1D image.
  int32_t block = -1, block_1d = -1;
  VkDeviceSize mem_offset = 0, mem_size = 0, mem_offset_1d = 0, mem_size_1d = 0;
  // A resolved 8_8_8_8 image is also viewed as sRGB, for fetches marked gamma.
  VkImageView view_srgb = VK_NULL_HANDLE;
  uint32_t descriptor_srgb = 0;
  uint32_t mips = 1;
  uint32_t descriptor_cube = 0;  // a cube texture: its index in the cube table (descriptor is 0)
  uint64_t generation = 0;       // a resolved image: which resolve last wrote it (resolved_fit)
  uint64_t fit_used = 0;         // a fetch-sized copy: the last frame a draw sampled it (RefreshFits)
  // Host pixels per guest pixel: RenderScale() for EDRAM targets, resolved
  // images and their fetch-sized copies (their width / height are the host's),
  // 1 for textures decoded from memory. [IzunaRecomp] A float: 1.5 is a true
  // 1080p (ToHost below turns guest pixels into the host's).
  float scale = 1.0f;
  // [IzunaRecomp] ... vertically (scale is the horizontal): apart for the
  // game's 3D scene surfaces, sized so its own final stretch of 1120x584 to
  // the 1280x720 frame lands 1:1 on the output (Target, SceneFit).
  float scale_y = 1.0f;
};

// [IzunaRecomp] Guest pixels to the host's at `scale`: rounded to the nearest,
// the one rule for every edge and size, so rectangles that meet in the guest
// meet in the host (at 1.5 an odd guest coordinate is half a host pixel).
inline int32_t ToHost(int32_t v, float scale) { return int32_t(std::floor(double(v) * double(scale) + 0.5)); }
inline uint32_t ToHost(uint32_t v, float scale) { return uint32_t(std::floor(double(v) * double(scale) + 0.5)); }
// An image's size in host pixels for a guest size: rounded down, so every
// host pixel of it is inside the guest area and gets drawn (rounded up, an
// odd guest size at a fractional scale left an undrawn last row or column -
// test12's exposure strip). Coordinates use ToHost; copies clamp to sizes.
inline uint32_t ToHostSize(uint32_t v, float scale) {
  return std::max(1u, uint32_t(std::floor(double(v) * double(scale) + 1e-6)));
}
// Host pixels back to the guest's (an image's guest size).
inline uint32_t ToGuest(uint32_t v, float scale) { return uint32_t(std::floor(double(v) / double(scale) + 0.01)); }

constexpr uint32_t kRtHeight = 1024;  // EDRAM surfaces have no height of their own
constexpr uint32_t kRectList = 0x100;  // PipelineKey::topology flag: a triangle list expanded to rectangles
// PipelineKey::spec: the shaders' specialization constant - SPEC_CONSTANT_ALPHA_TEST
// (2), and the fork's SPEC_CONSTANT_FLOAT24_DEPTH: the depth target is the
// Xenos 20e4 format, the pixel shaders take depth to its precision (depth-only
// draws: shaders/depth24.frag).
constexpr uint32_t kSpecFloat24Depth = 1u << 8;
constexpr uint32_t kSpecAlphaToMask = 1u << 9;  // (the fork's SPEC_CONSTANT_ALPHA_TO_MASK: ng2AlphaToMask)
// [IzunaRecomp] ... on a 2x MSAA target: coverage as alpha, blended (the fork's
// SPEC_CONSTANT_ALPHA_TO_MASK_MSAA; native_draw.cpp, Draw).
constexpr uint32_t kSpecAlphaToMaskMsaa = 1u << 10;
// PipelineKey::raster bit 5: RT0 blended by the coverage output (dual source).
constexpr uint32_t kRasterCoverageBlend = 1u << 5;
constexpr uint32_t kMaxTextures = 8192, kMaxSamplers = 256;
constexpr VkDeviceSize kFrameUpload = 64u << 20;  // per frame (constants, BeginVertices data, texture uploads)
// [IzunaRecomp] kSharedBytes 2688 + 16: the texture scale at 2688 (the fork's
// g_TextureScale; the vertex layout region before it keeps its 1024 bytes).
// + 8 at 2696: the target's inverse scale per axis (g_RenderScaleInv2); + 256
// at 2704: each slot's scale per axis (g_SlotScale).
constexpr uint32_t kVsBytes = 4096, kPsBytes = 4096, kSharedBytes = 2960, kFetchBytes = 768;
constexpr uint32_t kSharedLayoutEnd = 2688;

inline VkFormat ColorFormat(uint32_t xenos) {
  switch (xenos) {
    case 0: return VK_FORMAT_R8G8B8A8_UNORM;                          // 8_8_8_8
    case 1: return VK_FORMAT_R8G8B8A8_SRGB;                           // 8_8_8_8_GAMMA: encoded on write (sRGB ~ Xenos PWL)
    case 2: case 10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;       // 2_10_10_10
    case 3: case 12: return VK_FORMAT_R16G16B16A16_SFLOAT;            // 2_10_10_10 float
    case 4: return VK_FORMAT_R16G16_SNORM;                             // 16_16
    case 5: return VK_FORMAT_R16G16B16A16_SNORM;                       // 16_16_16_16
    case 6: return VK_FORMAT_R16G16_SFLOAT;
    case 7: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 14: return VK_FORMAT_R32_SFLOAT;
    case 15: return VK_FORMAT_R32G32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
  }
}

// --- Pipelines --------------------------------------------------------------

struct PipelineKey {
  uint64_t vs, ps;
  uint32_t topology;
  uint32_t color_formats[4];
  uint32_t depth_format;
  uint32_t blend[4];
  uint32_t color_mask;
  uint32_t depth_control;
  uint32_t stencil_masks;
  uint32_t raster;  // bits 0-1 cull front/back, 2 front is clockwise, 3 depth bias on, 4 depth clamp
  uint32_t spec;    // the shaders' specialization constant (SPEC_CONSTANT_ALPHA_TEST)
  bool operator==(const PipelineKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
struct PipelineKeyHash {
  size_t operator()(const PipelineKey& k) const { return size_t(XXH3_64bits(&k, sizeof(k))); }
};

// --- State ------------------------------------------------------------------

struct Stats {
  uint32_t drawn = 0, no_vs = 0, no_ps = 0, unknown_shader = 0, rect = 0, copy_mode = 0, no_target = 0,
           bad_format = 0, upload_full = 0, pipeline_failed = 0, resolves = 0, depth_resolves = 0,
           shaders_seen = 0, shaders_unknown = 0, textures_uploaded = 0, texture_bytes = 0,
           textures_unsupported = 0, textures_deferred = 0, pipeline_pending = 0, clears = 0, indexed_up = 0,
           hidden = 0, textures_replaced = 0;  // ([IzunaRecomp] mods: native_mods.cpp)
  uint32_t msaa[4] = {};  // draws by RB_SURFACE_INFO's sample count (1x, 2x, 4x)
};

struct State {
  bool ready = false;
  DrawContext ctx;
  Fns vk;
  std::vector<uint8_t> spirv;
  // Guest shader object -> container hash. Shaders are created on loader
  // threads; everything else here runs on the game's render thread.
  std::mutex object_mutex;
  std::unordered_map<uint32_t, uint64_t> object_hash;
  std::atomic<uint64_t> object_generation{0};  // [IzunaRecomp] (each change: EntryForObject's memo)
  std::unordered_map<uint64_t, ShaderInfo> shaders;
  std::thread::id render_thread;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkShaderModule rect_gs = VK_NULL_HANDLE;  // rectangle lists (shaders/rectlist.geom)
  VkShaderModule depth24_fs = VK_NULL_HANDLE;  // depth-only draws on a 20e4 depth target (shaders/depth24.frag)
  std::array<VkDescriptorSetLayout, 5> set_layouts{};
  VkDescriptorPool pool = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, 5> sets{};
  // Pipelines are built on worker threads (a big shader can take seconds on
  // the console's driver - one never finished and froze the game); a draw
  // whose pipeline is not ready yet is skipped.
  struct PipelineSlot {
    std::atomic<VkPipeline> pipeline{VK_NULL_HANDLE};
    std::atomic<int> state{0};  // 0 queued, 1 ready, 2 failed
    std::atomic<bool> claimed{false};  // a worker took it (it may sit in both queues)
    bool urgent = false;               // (render thread) moved to the urgent queue
  };
  std::unordered_map<PipelineKey, std::shared_ptr<PipelineSlot>, PipelineKeyHash> pipelines;
  // Graphics pipeline libraries (native_pipeline_libraries): a pipeline is
  // linked from four parts - vertex input (topology), the vertex shader part
  // (VS, its rectangle geometry shader), the pixel shader part (PS,
  // alpha test, depth format) and the output part (formats, blend, masks).
  // Rasterisation and depth / stencil state are dynamic, so the shader parts
  // depend on the shaders only: a new render state is a link (fast), then a
  // link-time-optimised pipeline replaces it in the background.
  bool gpl = false;
  struct Lib {
    std::atomic<VkPipeline> pipeline{VK_NULL_HANDLE};
    std::atomic<int> state{0};  // 0 queued, 1 ready, 2 failed
    std::atomic<bool> claimed{false};
  };
  std::mutex lib_mutex;
  std::unordered_map<uint64_t, std::shared_ptr<Lib>> shader_libs;  // (lib_mutex)
  std::mutex state_lib_mutex;                                       // (the drawing thread and the pre-links)
  std::unordered_map<uint64_t, VkPipeline> state_libs;              // vertex input / output parts (state_lib_mutex)
  std::atomic<uint32_t> links{0}, optimized{0};
  // Links ahead of a pipeline's first draw (job kind 4: keys from earlier
  // runs), and the drawing thread's own links' time (the rest).
  std::atomic<uint32_t> prelinked{0};
  std::atomic<uint64_t> link_ns{0}, link_worst_ns{0};
  PFN_vkCmdSetCullMode CmdSetCullMode = nullptr;
  PFN_vkCmdSetFrontFace CmdSetFrontFace = nullptr;
  PFN_vkCmdSetDepthBiasEnable CmdSetDepthBiasEnable = nullptr;
  PFN_vkCmdSetDepthTestEnable CmdSetDepthTestEnable = nullptr;
  PFN_vkCmdSetDepthWriteEnable CmdSetDepthWriteEnable = nullptr;
  PFN_vkCmdSetDepthCompareOp CmdSetDepthCompareOp = nullptr;
  PFN_vkCmdSetStencilTestEnable CmdSetStencilTestEnable = nullptr;
  PFN_vkCmdSetStencilOp CmdSetStencilOp = nullptr;
  PFN_vkCmdSetStencilCompareMask CmdSetStencilCompareMask = nullptr;
  PFN_vkCmdSetStencilWriteMask CmdSetStencilWriteMask = nullptr;
  struct PipelineJob {
    PipelineKey key;
    VkShaderModule vs, ps;
    uint32_t vs_size, ps_size;
    std::shared_ptr<PipelineSlot> slot;
    bool precompile = false;
    // Graphics pipeline libraries: 1 a vertex shader part, 2 a pixel shader
    // part (into `lib`), 3 the optimised link of `libs` (into `slot`).
    int kind = 0;
    std::shared_ptr<Lib> lib;
    VkPipeline libs[4] = {};
    uint32_t tries = 0;  // (kind 4: looked at before its parts were built)
  };
  std::mutex job_mutex;
  std::condition_variable job_cv;
  // `urgent`: pipelines a draw is waiting for, taken first; `jobs`: the rest
  // (pipelines earlier runs used, queued at start - native_precompile_pipelines).
  std::deque<PipelineJob> jobs, urgent;
  std::atomic<uint32_t> jobs_queued{0}, pipelines_built{0}, precompile_left{0};
  // Every pipeline key queued, in first-use order, saved with the cache
  // (UserData/native_pipeline_keys.bin) for the next run's precompile.
  std::vector<PipelineKey> keys_seen;
  size_t keys_saved = 0;
  // Compiled pipelines kept across runs (UserData/native_pipelines.bin): a
  // pipeline costs 50-600 ms to build on the console the first time.
  VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
  size_t pipelines_saved = 0;
  std::unordered_map<uint64_t, Image> targets;   // EDRAM surface key -> image
  std::unordered_map<uint32_t, Image> resolved;  // destination physical address -> image
  // A resolved image is the copy destination's size (its pitch x height); a
  // texture fetching it may be smaller (a 1x1 exposure value in a 32x1 copy, a
  // 213x120 glow pass in 224x120): normalised coordinates must span the
  // texture, not the copy. Those fetch it through a copy of its own size
  // (destination | w-1 << 32 | h-1 << 48), redone when the resolve is.
  std::unordered_map<uint64_t, Image> resolved_fit;
  // The other shapes a destination was resolved in, kept for when it is
  // again (the game reuses addresses with alternating shapes - the shadow
  // map as R32F then R16G16, a glow pass as 320x720 RGBA8 then 320x180
  // RGBA16F - and remaking the image each time cost ~1.5 image creations a
  // frame). Keyed by XXH3 of {destination, format, width, height}.
  std::unordered_map<uint64_t, Image> resolved_variants;
  // Each texture slot's result for the rest of the frame, by its fetch
  // constant (most slots keep theirs from draw to draw); a resolve
  // invalidates them all (resolve_generation).
  // What a texture slot binds: its descriptors (and sampler), shared by the
  // slot cache and the frame table below.
  struct SlotResult {
    uint32_t texture = 0, one_d = 0, cube = 0, sampler = 0;
    bool write_tables = false;  // (a decoded texture: the 1D and cube tables too)
    bool scaled = false;        // (a resolved image drawn at RenderScale() > 1)
    float sx = 1.0f, sy = 1.0f; // [IzunaRecomp] (its scale, horizontal / vertical: the shader's texel offsets)
  };
  struct SlotCache {
    uint32_t fc[6] = {};
    uint64_t frame = UINT64_MAX, resolve_generation = 0;
    SlotResult r;
  };
  std::array<SlotCache, 32> slot_cache;
  // The command buffer's current pipeline, dynamic state and push constants:
  // a draw only records what changed (~20 Vulkan calls a draw before). The
  // rasterisation / depth group is only dynamic in linked pipelines: binding
  // a whole one (static state) invalidates it.
  struct DynCache {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool gpl_valid = false;
    uint32_t cull = 0, front = 0, bias_enable = 0, dtest = 0, dwrite = 0, dcompare = 0, stest = 0;
    uint32_t sop_front = 0, sop_back = 0, cmask = 0, wmask = 0;
    bool common_valid = false;
    VkViewport viewport{};
    VkRect2D scissor{};
    float blend[4] = {};
    uint32_t sref = 0;
    float bias[3] = {};
    uint64_t push[5] = {};
    bool push_valid = false;
  } dyn;
  // [diag] The last colour target written at each EDRAM base (format | pitch
  // << 8): a resolve reading a base in another format or pitch reads what the
  // hardware holds as different bits (EDRAM aliasing).
  std::unordered_map<uint32_t, uint32_t> edram_writer;
  uint64_t alias_reads = 0;
  // The frame's texture results by fetch constant, any slot, any draw (the
  // slot cache only knew the slot's last one): Texture2D + the resolved
  // lookup + Sampler once per texture a frame instead of once per use.
  struct FrameTex {
    uint32_t fc[6] = {};
    bool shader_gamma = false;
    uint64_t frame = UINT64_MAX, resolve_generation = 0;
    SlotResult r;
  };
  std::array<FrameTex, 1024> frame_tex;
  // The guest's vertex / pixel shader constants as last uploaded this frame
  // (equal bytes: the upload is reused without byte-swapping them again).
  std::array<std::vector<uint8_t>, 2> raw_constants;
  // Draw census: the draws the hooks received, by primitive (skipped ones too).
  std::array<uint64_t, 32> census{};
  // GPU time per pass of a logged frame: a timestamp at its start, after
  // each resolve and at its end; read 3 frames on, the costliest passes logged.
  VkQueryPool pass_queries = VK_NULL_HANDLE;
  double timestamp_ns = 0.0;
  uint32_t pass_count = 0;  // timestamps written in the logged frame
  struct PassLabel {
    uint32_t draws = 0, dest = 0, width = 0, height = 0;
  };
  std::array<PassLabel, 256> pass_labels{};
  uint64_t pass_frame = 0;  // the logged frame whose timestamps are pending (0: none)
  uint64_t resolve_generation = 0;
  std::vector<Image> placeholders;               // 2D, 3D, cube, 1D (descriptor 0 of each table)
  struct Texture {
    Image image;
    uint64_t hash = 0;
    uint64_t checked_frame = UINT64_MAX;  // the frame its bytes were last hashed
    bool dynamic = false;                 // its bytes changed after its first upload: checked every frame...
    uint32_t steady = 0;                  // ...until kSettleChecks checks in a row find them unchanged
    bool replaced = false;                // [IzunaRecomp] drawn with a mod's picture (native_mods.cpp)
    std::string awaiting;                 // the id of a mod's picture still being read
    uint32_t base = 0;                    // (diagnosis: native_show_texture)
    uint64_t last_used = 0;               // the frame it was last bound (eviction)
  };
  std::unordered_map<uint64_t, Texture> textures;  // fetch constant key -> texture
  std::unordered_map<uint64_t, uint32_t> samplers;  // sampler key -> index in the sampler table
  float max_anisotropy = 1.0f;                       // (1: anisotropic filtering unavailable)
  bool depth_clamp = false;                          // (the device feature: enabled when supported)
  float max_lod_bias = 0.0f;
  uint32_t next_descriptor = 1, next_descriptor_1d = 1, next_descriptor_cube = 1;
  // Freed images and descriptor slots wait until no frame in flight can still
  // use them (two frame slots: kept 3 frames to be safe).
  struct Retired {
    uint64_t frame;
    Image image;
  };
  std::vector<Retired> retired;
  // Image memory blocks (AllocImageMemory): free ranges by offset.
  struct MemBlock {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t type = 0;
    std::map<VkDeviceSize, VkDeviceSize> free;
  };
  std::vector<MemBlock> blocks;
  // [dev] Resolves of a logged frame waiting to be written (native_dump_resolves).
  struct ResolveDump {
    uint64_t frame = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    size_t bytes = 0;
    std::string name;
    bool capture = false;  // (an overlay capture: waits for the writer instead of being dropped)
  };
  std::vector<ResolveDump> resolve_dumps;
  uint32_t resolve_dumps_in_frame = 0;
  // [dev] native_probe: a pixel's colour and depth after each draw of the
  // logged frames, read back 3 frames on (ProbeAfterDraw).
  struct ProbeSample {
    uint64_t frame = 0, ps = 0;
    uint32_t draw = 0, target = 0, blend = 0, width = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool depth = false;
  };
  struct Probe {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    std::vector<ProbeSample> samples;
  } probe;
  std::vector<std::pair<uint64_t, uint32_t>> retired_descriptors, retired_descriptors_1d, retired_descriptors_cube;
  std::vector<uint32_t> free_descriptors, free_descriptors_1d, free_descriptors_cube;
  VkSampler sampler = VK_NULL_HANDLE;
  // Primitive-reset strips as triangle lists (ResetStripToList), converted
  // once per index buffer and kept in mapped arenas until the arenas fill
  // (then all are dropped, freed 3 frames on, and converted again).
  struct MappedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    uint64_t address = 0;
  };
  struct StripList {
    uint64_t hash = 0, address = 0, checked_frame = 0;
    uint32_t vertices = 0;
    bool dynamic = false;  // (its indices changed: checked every frame, until they settle)
    uint32_t steady = 0;   // (checks in a row without a change while dynamic)
  };
  std::unordered_map<uint64_t, StripList> strip_lists;
  // [IzunaRecomp] test29: a dynamic texture or index list unchanged this many
  // checks in a row (3 s at 60) goes back to the periodic checks - before, one
  // change (a level's load writing it) kept it hashed every frame for the rest
  // of the session (test28: ~70 textures, 2.3 MB hashed a frame).
  static constexpr uint32_t kSettleChecks = 180;
  std::vector<MappedBuffer> list_arenas;
  VkDeviceSize list_used = 0;  // (in the last arena)
  std::vector<std::pair<uint64_t, MappedBuffer>> retired_arenas;
  std::vector<MappedBuffer> free_arenas;  // [IzunaRecomp] (made at DrawInit, retired ones back here: never allocated in play)
  std::vector<uint32_t> list_scratch;
  // Upload ring: two frame halves, host-visible, device-addressed.
  VkBuffer upload = VK_NULL_HANDLE;
  // Depth resolves go depth image -> this buffer -> a float texture (Vulkan
  // copies no depth image into a colour one directly).
  VkBuffer depth_scratch = VK_NULL_HANDLE;
  uint8_t* upload_mapped = nullptr;
  uint64_t upload_address = 0;
  VkDeviceSize upload_used = 0;
  uint32_t frame_index = 0;
  // The frame being recorded.
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  bool rendering = false;
  std::array<const Image*, 4> bound_colors{};
  const Image* bound_depth = nullptr;
  uint32_t last_resolved = 0;
  // The window scissor (OnWindowScissor; the drawing thread's: the worker's
  // with native_threaded), as the GPU register holds it. Reset: unclipped.
  uint32_t window_scissor_tl = 0, window_scissor_br = 0x20002000;
  // A BeginVertices draw waiting for its vertices (drawn at the next event).
  struct {
    bool pending = false;
    uint32_t device = 0, primitive = 0, count = 0, data = 0, dwords = 0;
    // BeginIndexedVertices: the indices (index_dwords 0: not indexed)
    uint32_t index_data = 0, index_dwords = 0, index_flags = 0;  // flags: bit 0 32-bit, bits 1-2 endian
    uint32_t index_offset = 0;
  } begin;
  // The device as it was at the Begin*Vertices call: the hardware draws with
  // that state (the draw packet is written there; only the vertices come
  // later), and by the next traced call the game has often set the next
  // draw's constants already - inlined Direct3D code, no call to see. Drawn
  // from this, not from the device at the flush (test28: every such draw
  // took the next one's constants and textures, and the first was lost - the
  // chapter card's and the pause menu's backgrounds).
  uint8_t begin_device[12800] = {};
  uint8_t begin_layout[kSharedLayoutEnd - 1664] = {};
  uint32_t begin_layout_bytes = 0;
  bool begin_layout_ok = true;
  uint64_t frame_number = 0;  // native frames finished (DrawEndFrame)
  uint32_t frame_drawn = 0, last_frame_drawn = 0;  // draws drawn this / the last frame
  uint32_t frame_pending = 0, last_frame_pending = 0;  // ... skipped as their pipeline was still building
  uint32_t auto_logs = 0;     // frames logged automatically (the first heavy 3D frames)
  uint64_t auto_log_after = 0;
  bool log_frame = false;     // native_log_frame: this frame's draws are logged
  bool capture_frame = false;  // ... for RequestCapture: its resolves are saved too
  std::string capture_log;     // ... and its lines (FrameLog), saved at the frame's end
  uint64_t capture_log_frame = 0;
  uint64_t log_on_ps_frame = 0;  // native_log_on_ps: the frame to log
  // Last uploaded blocks (unchanged ones are reused).
  std::array<std::vector<uint8_t>, 4> last_block;
  std::array<uint64_t, 4> last_address{};
  Stats stats;
};
extern State g;
// Frames still to capture for the overlay (RequestCapture; any thread).
extern std::atomic<uint32_t> g_capture_frames;
// The title's look at each texture before it is decoded (SetTextureLoadHook).
extern std::atomic<rex::graphics::GuestTextureLoadHook> g_texture_load_hook;

// A line of a logged frame: to the log, or - in an overlay capture - to a
// file of its own (native_resolves/capture_<frame>.log, saved at the frame's
// end: SaveCaptureLog). A logged frame is ~15 lines a draw; through the log
// (5 MB files, two kept) the owner's test38 captures kept one frame's lines.
template <typename... Args>
void FrameLog(fmt::format_string<Args...> f, Args&&... args) {
  if (g.capture_frame) {
    fmt::format_to(std::back_inserter(g.capture_log), f, std::forward<Args>(args)...);
    g.capture_log += '\n';
    return;
  }
  REXLOG_INFO("[native] {}", fmt::format(f, std::forward<Args>(args)...));
}

// [timing] Where the render thread's time goes (reported every ~10 s, as
// per-frame averages): inside the native hooks, of that in textures (hash +
// decode), and waiting for a frame's GPU work (native_renderer.cpp).
struct Timing {
  uint64_t hooks_ns = 0, texture_ns = 0, fence_ns = 0, frames = 0;
  // Draw(), by stage: shaders + targets, constants, vertex fetches, texture
  // slots, pipeline lookup, command recording (diagnosis of the render
  // thread's time).
  uint64_t stage_ns[6] = {}, draws = 0;
  // Texture2D: lookups, content hashes (and their bytes and time), and
  // decodes (time from the hash to the end: untile, convert, copy).
  uint64_t tex_lookups = 0, tex_hashes = 0, tex_hash_bytes = 0, tex_hash_ns = 0, tex_decodes = 0, tex_decode_ns = 0;
  uint64_t tex_creates = 0, tex_create_ns = 0;
  uint64_t slot_resolved_ns = 0, slot_texture_ns = 0, slot_sampler_ns = 0, slot_frame_hits = 0, slot_misses = 0;
  uint64_t pass_ends = 0, fit_copies = 0;  // render passes ended (each a GPU flush), fetch-sized copies made
  uint64_t fit_copies_at_resolve = 0;      // ... of which made at the resolve (no pass break)
  uint64_t export_ends = 0;                // ... after a draw that exported to memory
  uint64_t create_part_ns[4] = {};  // CreateImage: vkCreateImage, memory + bind, view, layout barrier
  uint64_t creates_by[4] = {};      // images created: textures, render targets, resolves, fetch-sized copies  // new images (vkCreateImage + memory + view), part of the decodes
};
// Marks the end of one stage of Draw() (Timing::stage_ns).
struct StageClock {
  uint64_t t = g_timed ? Ticks() : 0;
  void Mark(int stage);
};
extern Timing t_timing;
extern std::atomic<uint64_t> g_image_bytes;  // GPU memory held by the native renderer's images
struct ScopedTime {
  uint64_t& out;
  uint64_t t0 = g_timed ? Ticks() : 0;
  explicit ScopedTime(uint64_t& o) : out(o) {}
  ~ScopedTime() {
    if (g_timed) out += TicksToNs(Ticks() - t0);
  }
};

// A clear between OnClearBegin and OnClearEnd (and the record the worker replays).
struct PendingClear {
  bool pending = false;
  uint32_t device = 0, flags = 0, stencil = 0;
  int32_t rect[4] = {};
  float color[4] = {};
  double z = 0;
};

struct TexFormat {
  VkFormat vk = VK_FORMAT_UNDEFINED;
  uint32_t log2_bpb = 0;   // bytes per block in guest memory
  uint32_t block = 1;      // block width/height in texels
  uint32_t components = 4; // before the swizzle (the last one is replicated)
  int convert = 0;         // CPU conversion to RGBA8: 1 1_5_5_5, 2 5_6_5, 3 4_4_4_4
};

// --- Each part's offer to the others ------------------------------------------

// native_draw.cpp
uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags want);
void Barrier(VkCommandBuffer cmd);
bool CreateImage(VkCommandBuffer cmd, Image& out, VkFormat format, uint32_t w, uint32_t h, bool depth,
                 VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D, VkComponentMapping components = {},
                 uint32_t mips = 1, VkImageUsageFlags extra_usage = 0);
void WriteTextureDescriptor(uint32_t set, uint32_t index, VkImageView view);
uint32_t AllocDescriptor(bool one_d);
uint32_t AllocCubeDescriptor();
void Retire(Image& img);

// [IzunaRecomp] Mods (native_mods.cpp).
bool ShaderHidden(uint64_t vs, uint64_t ps);
std::string TextureId(uint32_t w, uint32_t h, uint32_t format, uint64_t hash);
struct TextureReplacement {
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> rgba;
};
bool ReplacementsExist();
// 0 no picture for the id, 1 being read (the game's own is drawn meanwhile),
// 2 ready (*out).
int TakeReplacement(const std::string& id, std::shared_ptr<const TextureReplacement>* out);
void ReplacementUploaded(const std::string& id);
bool TextureDumpOn();
void DumpTexture(const std::string& id, VkFormat vk, uint32_t w, uint32_t h, const uint8_t* data, size_t bytes);
void EndRendering();
const ShaderInfo* ShaderByHash(uint64_t hash, bool* missing = nullptr);
bool FillVertexFetches(uint32_t object, const ShaderCacheEntry* e, uint8_t* out);
// Record one draw. `index_info`: SharedConstants + 960 (ng2VertexIndex);
// `up_data` / `up_fetch1`: a BeginVertices draw's copied vertices (fetch slot
// 95); `layout`: the vertex fetches as the producer read them (null: now).
void Draw(const Device& d, uint32_t xenos_primitive, uint32_t count, const uint32_t index_info[4],
          uint64_t up_data = 0, uint32_t up_fetch1 = 0, const uint8_t* layout = nullptr, bool layout_ok = true);
void DrawUp(const uint8_t* device_bytes, uint32_t primitive, uint32_t count, uint32_t dwords, const uint8_t* src,
            uint32_t bytes, const uint8_t* isrc, uint32_t ibytes, uint32_t index_flags, uint32_t index_offset,
            const uint8_t* layout = nullptr, bool layout_ok = true);
void FlushBeginVertices();

// native_pipelines.cpp
VkPipeline Pipeline(const PipelineKey& key, const ShaderInfo* vs, const ShaderInfo* ps, bool* pending,
                    bool* whole_pipeline);
void PipelineWorker();
void CreatePipelineCache();
void QueuePrecompile();
void WaitForPrecompile();  // [IzunaRecomp] test42 (native_pipelines.cpp)
void SavePipelineCache();

// native_textures.cpp
TexFormat Format(rex::graphics::xenos::TextureFormat f);
uint32_t Texture2D(const uint32_t fc[6], uint32_t* descriptor_1d = nullptr, uint32_t* descriptor_cube = nullptr,
                   bool shader_gamma = false);
uint32_t Sampler(const uint32_t fc[6], uint32_t filter = 0, uint32_t mip_override = 0, bool aniso_off = false);

// native_worker.cpp
bool Threaded();
bool OnWorker();
const ShaderCacheEntry* EntryForObject(uint32_t object);
// [IzunaRecomp] The shader table drawn with: the one built from the player's
// game at startup (izuna/izuna_shaders.h) when there is one, else the one
// built into the executable (out/native-shaders/shader_cache.cpp).
const ShaderCacheEntry* ShaderEntries();
size_t ShaderEntryCount();
const uint32_t* ShaderLayouts();
void QueueDraw(uint32_t device, uint32_t primitive, uint32_t count, const uint32_t index_info[4]);
void QueueDrawUp(const uint8_t* device_bytes, const uint8_t* layout, uint32_t layout_bytes, bool layout_ok,
                 uint32_t primitive, uint32_t count, uint32_t dwords, const uint8_t* src, uint32_t bytes,
                 const uint8_t* isrc, uint32_t ibytes, uint32_t index_flags, uint32_t index_offset);
void QueueResolve(uint32_t device, uint32_t flags, const int32_t* rect, const float* clear_color, double clear_z);
void QueueClear(const PendingClear& clear);
void QueueWindowScissor(uint32_t tl, uint32_t br);

// native_resolve_7e3.cpp: a resolve of a 2_10_10_10_FLOAT target as the EDRAM
// holds it (colour 0-31.875, 2-bit alpha), applied to the resolved rectangle
// of `image` (16-bit float, made with storage use) in place. False if it could not.
bool Saturate7e3(const Image& image, int32_t x, int32_t y, uint32_t w, uint32_t h);

// native_capture.cpp
void DumpResolve(const Image& img, uint32_t dest);
void WriteResolveDumps(bool all);
void ProbeAfterDraw(const Image* color, const Image* depth, uint32_t target, uint64_t ps, uint32_t blend);
void LogProbe();
void SaveFileLater(std::filesystem::path path, std::function<std::vector<char>()> data);

}  // namespace draw

// native_draw.cpp: the resolve and the clear themselves (the worker replays them).
void ResolveWork(const uint8_t* device_bytes, uint32_t flags, const int32_t* rect, const float* clear_color,
                 double clear_z);
void ClearWork(const uint8_t* device_bytes, const draw::PendingClear& clear);

using namespace draw;

}  // namespace ng2native
