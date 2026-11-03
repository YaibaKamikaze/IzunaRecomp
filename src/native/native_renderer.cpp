// [IzunaRecomp] The Kasumi renderer's frames: the Vulkan device it draws on
// (the SDK's UI layer: rex/external_frame.h's VulkanDevice), the command
// buffer segments of each frame (one per Direct3D kickoff, the last at the
// swap), the frames shown (the native graphics system presents the latest at
// each guest swap), and GPU sync: the guest-visible steps of Direct3D calls
// (izuna_gpu.cpp, RunAfterRecordedWork) wait until the native work recorded
// before them has finished on the GPU. native_draw.cpp draws into the
// segments. (At milestone M2 this was the test pattern: a cycling colour.)

#include "native/native_phase.h"
#include "native/native_renderer.h"
#include "izuna/izuna_pc.h"
#include "native/native_draw.h"

#include <array>
#include <initializer_list>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <thread>
#include <memory>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <new>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>  // (VirtualProtect)
#else
#include <sys/mman.h>
#endif
#include <vector>
#include <mutex>
#include <string>

#include <fstream>

#include <fmt/format.h>
#include <rex/filesystem.h>
#include <rex/cvar.h>
#include <rex/external_frame.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/audio/host_stall.h>
#include <rex/ps5_diag.h>
#include <rex/ui/vulkan/api.h>

#if defined(NG2_PS5_HOST_TEST)
constexpr int32_t kDumpEveryDefault = 0;
#else
constexpr int32_t kDumpEveryDefault = 600;  // [dev builds] a frame every ~10 s, for the owner's test runs
#endif
REXCVAR_DEFINE_INT32(native_dump_keep, 20, "Native renderer",
                     "[native renderer] How many saved frames to keep (they are reused in turn); 0 = keep all, "
                     "named by frame number");
REXCVAR_DEFINE_INT32(native_dump_burst, 0, "Native renderer",
                     "[native renderer] Also save every frame for this many frames in a row, every "
                     "native_dump_burst_every frames (0 off) - for flicker; kept all, named by frame number");
REXCVAR_DEFINE_INT32(native_dump_burst_every, 3600, "Native renderer",
                     "[native renderer] How often a burst of saved frames starts (native_dump_burst)");
REXCVAR_DEFINE_BOOL(native_sync_frames, false, "Native renderer",
                    "[native renderer, diagnosis] Each frame waits for its GPU work to finish before the game "
                    "goes on (the game's data can then never be rewritten under a frame still drawing)");
REXCVAR_DEFINE_INT32(native_dump_every, kDumpEveryDefault, "Native renderer",
                     "[native renderer] Save every Nth native frame as QOI, lossless (native_frames/ beside the app; "
                     "tools/native/rgba2png.py makes PNGs), 0 = off - for the desktop test build");

namespace ng2native {
namespace {

// The frames shown: the game's 1280x720 at the native renderer's resolution
// scale (1440p / 4K: RenderScale() 2 / 3).
// [IzunaRecomp] (1.5: 1920x1080; rounded down as the images are: ToHostSize)
uint32_t FrameWidth() { return uint32_t(std::floor(1280.0 * RenderScale() + 1e-6)); }
uint32_t FrameHeight() { return uint32_t(std::floor(720.0 * RenderScale() + 1e-6)); }

struct Vk {
  PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkCreateImage CreateImage = nullptr;
  PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
  PFN_vkAllocateMemory AllocateMemory = nullptr;
  PFN_vkBindImageMemory BindImageMemory = nullptr;
  PFN_vkCreateImageView CreateImageView = nullptr;
  PFN_vkCreateCommandPool CreateCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
  PFN_vkCreateFence CreateFence = nullptr;
  PFN_vkWaitForFences WaitForFences = nullptr;
  PFN_vkResetFences ResetFences = nullptr;
  PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
  PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
  PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;
  PFN_vkQueueSubmit QueueSubmit = nullptr;
  PFN_vkCreateBuffer CreateBuffer = nullptr;
  PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
  PFN_vkBindBufferMemory BindBufferMemory = nullptr;
  PFN_vkMapMemory MapMemory = nullptr;
  PFN_vkCmdCopyBuffer CmdCopyBuffer = nullptr;
  PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
  PFN_vkGetBufferDeviceAddress GetBufferDeviceAddress = nullptr;  // (optional)
};

struct Slot {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool used = false;
};

struct State {
  bool ready = false;
  Vk vk;
  const rex::external_frame::VulkanDevice* dev = nullptr;
  VkDevice device = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  std::array<Slot, 2> slots;
  uint32_t next = 0;
  uint64_t frames = 0;
  std::mutex frame_mutex;  // the provider reads `shown` from the GPU thread
  rex::external_frame::Frame shown;
  bool have_shown = false;
  bool recording = false;  // the next slot's command buffer is begun (FrameCommands)
  bool drawing = false;    // native_draw is up
  // Saved frames waiting for their GPU work (written by a thread of their own
  // once done: the drawing thread neither waits for them nor writes 3.6 MB);
  // four, so a burst of consecutive frames fits.
  struct DumpSlot {
    VkBuffer buffer = VK_NULL_HANDLE;
    void* mapped = nullptr;
    uint64_t seq = 0;  // the segment it waits for (0: free)
    std::string name;
  };
  std::array<DumpSlot, 4> dumps;
  uint64_t dumps_skipped = 0;
  std::mutex named_mutex;
  std::string named_dump;  // (named_mutex) RequestNamedDump
  // GPU sync: the Direct3D calls' guest-visible steps wait for the native
  // work recorded before them (RunAfterRecordedWork).
  PFN_vkGetFenceStatus GetFenceStatus = nullptr;
  // A frame is a chain of command buffer segments, one per Direct3D kickoff
  // and the last at the swap; segment `seq` (1, 2, ...) uses pool[seq % N].
  struct Segment {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
  };
  static constexpr uint64_t kSegments = 64;
  std::array<Segment, kSegments> segs;
  std::array<uint64_t, 2> slot_last_seq{};  // the last segment of the frame each slot holds
  // GPU time per frame: timestamps at the start of a frame's first segment and
  // the end of its last (queries 2 * slot, 2 * slot + 1).
  VkQueryPool timestamps = VK_NULL_HANDLE;
  double timestamp_ns = 0.0;  // per tick
  std::array<bool, 2> timestamps_valid{};
  // Submission cost (the PS5 driver can block in vkQueueSubmit while the GPU
  // is busy - R-comp's driver reports): per-report counts.
  uint64_t submits = 0, submit_ns = 0;
  uint64_t frames_repeated = 0;  // swaps that showed the previous frame again (no native work)
  // Frames finished (FinishFrame), for the graphics system's swap: it shows
  // the latest one, so it waits for the next (rex::external_frame::SwapWait).
  std::mutex finished_mutex;
  std::condition_variable finished_cv;
  uint64_t finished = 0;
  std::atomic<uint64_t> swap_waits_late{0};
  double gpu_ns = 0.0;
  uint64_t gpu_frames = 0;
  PFN_vkCmdWriteTimestamp CmdWriteTimestamp = nullptr;
  PFN_vkCmdResetQueryPool CmdResetQueryPool = nullptr;
  PFN_vkGetQueryPoolResults GetQueryPoolResults = nullptr;
  bool frame_open = false;                  // a frame has begun recording (its first segment)
  std::thread::id render_thread;
  std::atomic<uint64_t> submitted{0}, completed{0};  // segments
  // native_threaded: GPU sync by record (ng2native::RecordDone): each ticket
  // is a record sequence and the segments submitted when it was done; the
  // completion thread moves done_records on as those segments finish.
  struct Ticket {
    uint64_t record, segment;
  };
  std::mutex ticket_mutex;
  std::deque<Ticket> tickets;
  std::atomic<uint64_t> done_records{0};
  struct Pending {
    uint64_t frame;
    std::chrono::steady_clock::time_point at;
    rex::external_frame::SideEffect effect;
  };
  std::mutex side_mutex;
  std::condition_variable side_cv;
  std::deque<Pending> pending;
  std::atomic<uint64_t> effects_late{0}, effects_run{0};
  // The guest's physical memory, imported (TestGuestMemoryImport), and a
  // host-visible buffer for the read checks.
  VkBuffer guest = VK_NULL_HANDLE;
  uint64_t guest_address = 0;
  VkBuffer readback = VK_NULL_HANDLE;
  void* readback_mapped = nullptr;
};
State g;

template <typename T>
bool Load(T& fn, const char* name) {
  fn = reinterpret_cast<T>(g.vk.GetDeviceProcAddr(g.device, name));
  if (!fn) REXLOG_ERROR("[native] missing Vulkan function {}", name);
  return fn != nullptr;
}

// The first memory type of `bits` with all the flags of a preference, the
// preferences in order; UINT32_MAX if none fits.
uint32_t MemoryType(uint32_t bits, std::initializer_list<VkMemoryPropertyFlags> preferences) {
  VkPhysicalDeviceMemoryProperties props;
  g.vk.GetPhysicalDeviceMemoryProperties(VkPhysicalDevice(g.dev->physical_device), &props);
  for (const VkMemoryPropertyFlags want : preferences) {
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) return i;
    }
  }
  return UINT32_MAX;
}

// A host-visible buffer the GPU copies into for the CPU to read: cached
// memory when there is any (reading the write-combined kind runs at a
// fraction of the speed - a saved frame held the drawing thread ~100 ms on
// the console, test29), mapped.
bool CreateReadback(VkDeviceSize size, VkBuffer* buffer, void** mapped) {
  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (g.vk.CreateBuffer(g.device, &bci, nullptr, buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  g.vk.GetBufferMemoryRequirements(g.device, *buffer, &req);
  constexpr VkMemoryPropertyFlags kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = MemoryType(req.memoryTypeBits, {kHost | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, kHost});
  VkDeviceMemory memory = VK_NULL_HANDLE;
  if (mai.memoryTypeIndex == UINT32_MAX || g.vk.AllocateMemory(g.device, &mai, nullptr, &memory) != VK_SUCCESS ||
      g.vk.BindBufferMemory(g.device, *buffer, memory, 0) != VK_SUCCESS ||
      g.vk.MapMemory(g.device, memory, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS) {
    *buffer = VK_NULL_HANDLE;  // (left for the process: a failure here is at start, once)
    return false;
  }
  return true;
}

bool CreateSlot(Slot& s) {
  VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R8G8B8A8_UNORM;
  ici.extent = {FrameWidth(), FrameHeight(), 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (g.vk.CreateImage(g.device, &ici, nullptr, &s.image) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  g.vk.GetImageMemoryRequirements(g.device, s.image, &req);
  const uint32_t type = MemoryType(req.memoryTypeBits, {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});
  if (type == UINT32_MAX) return false;
  VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type;
  if (g.vk.AllocateMemory(g.device, &mai, nullptr, &s.memory) != VK_SUCCESS) return false;
  if (g.vk.BindImageMemory(g.device, s.image, s.memory, 0) != VK_SUCCESS) return false;
  VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = s.image;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = VK_FORMAT_R8G8B8A8_UNORM;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (g.vk.CreateImageView(g.device, &vci, nullptr, &s.view) != VK_SUCCESS) return false;
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = g.pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  if (g.vk.AllocateCommandBuffers(g.device, &cai, &s.cmd) != VK_SUCCESS) return false;
  VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  return g.vk.CreateFence(g.device, &fci, nullptr, &s.fence) == VK_SUCCESS;
}

// [M3 groundwork] Can the GPU read the game's physical memory in place? That
// needs VK_EXT_external_memory_host and an importable pointer. Logged once,
// so a test run answers it (the extension is not enabled yet: this only asks).
void ProbeGuestMemoryImport(PFN_vkGetInstanceProcAddr gipa, VkInstance instance) {
  const VkPhysicalDevice pd = VkPhysicalDevice(g.dev->physical_device);
  auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
      gipa(instance, "vkEnumerateDeviceExtensionProperties"));
  auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
      gipa(instance, "vkGetPhysicalDeviceProperties2"));
  bool host = false, bda = false;
  if (enumerate) {
    uint32_t n = 0;
    enumerate(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> ext(n);
    enumerate(pd, nullptr, &n, ext.data());
    for (const auto& e : ext) {
      host |= strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) == 0;
      bda |= strcmp(e.extensionName, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) == 0;
    }
  }
  uint64_t alignment = 0;
  if (host && properties2) {
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &hp;
    properties2(pd, &p2);
    alignment = hp.minImportedHostPointerAlignment;
  }
  const uint8_t* base = nullptr;
  if (auto* memory = REX_KERNEL_MEMORY()) base = memory->physical_membase();
  REXLOG_INFO("[native] memory probe: VK_EXT_external_memory_host {} (min alignment {}), "
              "VK_KHR_buffer_device_address {}, guest physical memory at host {} ({}aligned to it)",
              host ? "supported" : "NOT supported", alignment, bda ? "listed" : "core/absent",
              static_cast<const void*>(base),
              (alignment && base && (uintptr_t(base) % alignment) == 0) ? "" : "not ");
  // The import check itself needs the extension enabled on the device
  // (vkGetMemoryHostPointerPropertiesEXT): the next step if it is supported.
}

// [M3 groundwork] Import the guest's physical memory (512 MB) as one buffer
// and check the GPU reads the same bytes the CPU sees: a GPU copy of a few
// 4 KB blocks into a host-visible buffer, compared. If it holds, the native
// renderer reads vertices, indices and textures in place - no uploads, no
// write watches. Once, at start; logged.
void TestGuestMemoryImport() {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  uint8_t* base = memory->physical_membase();
  constexpr VkDeviceSize kSize = 0x20000000;  // 512 MB
  auto getHostProps = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
      g.vk.GetDeviceProcAddr(g.device, "vkGetMemoryHostPointerPropertiesEXT"));
  if (!getHostProps) {
    REXLOG_WARN("[native] memory import: vkGetMemoryHostPointerPropertiesEXT unavailable (extension not enabled)");
    return;
  }
  // [test9] Freeze the range's protection first. The emulator changes it when
  // the game frees and reallocates physical memory (no-access, then read/write
  // again); on the PS5 that breaks the GPU's mapping of those pages and the
  // next GPU read kills the app (tests 6-8 died exactly in a GPU copy, after
  // the game had loaded). Guest code reaches memory through the virtual views,
  // so leaving this physical mapping read/write changes nothing for the game.
  rex::ps5diag::frozen_hi.store(reinterpret_cast<uintptr_t>(base) + kSize, std::memory_order_release);
  rex::ps5diag::frozen_lo.store(reinterpret_cast<uintptr_t>(base), std::memory_order_release);
#if defined(_WIN32)
  DWORD old_protect = 0;
  if (!VirtualProtect(base, kSize, PAGE_READWRITE, &old_protect)) {
    REXLOG_WARN("[native] memory import: could not make the guest memory read/write (error {})", GetLastError());
  }
#else
  if (mprotect(base, kSize, PROT_READ | PROT_WRITE) != 0) {
    REXLOG_WARN("[native] memory import: could not make the guest memory read/write (errno {})", errno);
  }
#endif
  // [IzunaRecomp] The handle type: a host allocation first (NVIDIA, RADV, the
  // PS5), then "mapped foreign memory" - the type for memory mapped from
  // elsewhere, which the guest's file mapping is, should a driver refuse it as
  // an allocation (a Windows driver other than NVIDIA's: AMD, Intel).
  VkResult r = VK_SUCCESS;
  VkBuffer guest = VK_NULL_HANDLE;
  VkDeviceMemory guest_memory = VK_NULL_HANDLE;
  uint32_t type = 0;
  // (for a failed try's cleanup)
  const auto destroy_buffer =
      reinterpret_cast<PFN_vkDestroyBuffer>(g.vk.GetDeviceProcAddr(g.device, "vkDestroyBuffer"));
  const auto free_memory = reinterpret_cast<PFN_vkFreeMemory>(g.vk.GetDeviceProcAddr(g.device, "vkFreeMemory"));
  const VkExternalMemoryHandleTypeFlagBits kTypes[] = {VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                       VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT};
  for (const VkExternalMemoryHandleTypeFlagBits handle : kTypes) {
    const char* handle_name =
        handle == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT ? "host allocation" : "mapped foreign memory";
    VkMemoryHostPointerPropertiesEXT hpp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    r = getHostProps(g.device, handle, base, &hpp);
    if (r != VK_SUCCESS || !hpp.memoryTypeBits) {
      REXLOG_WARN("[native] memory import ({}): the guest's physical memory is not importable (result {}, types {:X})",
                  handle_name, int(r), hpp.memoryTypeBits);
      continue;
    }
    // The imported buffer.
    VkExternalMemoryBufferCreateInfo ebci = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ebci.handleTypes = handle;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext = &ebci;
    bci.size = kSize;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if ((r = g.vk.CreateBuffer(g.device, &bci, nullptr, &guest)) != VK_SUCCESS) {
      REXLOG_WARN("[native] memory import ({}): vkCreateBuffer failed ({})", handle_name, int(r));
      guest = VK_NULL_HANDLE;
      continue;
    }
    VkMemoryRequirements req;
    g.vk.GetBufferMemoryRequirements(g.device, guest, &req);
    const uint32_t bits = req.memoryTypeBits & hpp.memoryTypeBits;
    type = 0;
    while (type < 32 && !(bits & (1u << type))) ++type;
    if (type == 32) {
      REXLOG_WARN("[native] memory import ({}): no memory type fits (buffer {:X}, host pointer {:X})", handle_name,
                  req.memoryTypeBits, hpp.memoryTypeBits);
      if (destroy_buffer) destroy_buffer(g.device, guest, nullptr);
      guest = VK_NULL_HANDLE;
      continue;
    }
    VkMemoryAllocateFlagsInfo flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkImportMemoryHostPointerInfoEXT import = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    import.pNext = &flags;
    import.handleType = handle;
    import.pHostPointer = base;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &import;
    mai.allocationSize = kSize;
    mai.memoryTypeIndex = type;
    if ((r = g.vk.AllocateMemory(g.device, &mai, nullptr, &guest_memory)) != VK_SUCCESS) {
      REXLOG_WARN("[native] memory import ({}): vkAllocateMemory of the guest memory failed ({})", handle_name,
                  int(r));
      if (destroy_buffer) destroy_buffer(g.device, guest, nullptr);
      guest = VK_NULL_HANDLE;
      guest_memory = VK_NULL_HANDLE;
      continue;
    }
    if ((r = g.vk.BindBufferMemory(g.device, guest, guest_memory, 0)) != VK_SUCCESS) {
      REXLOG_WARN("[native] memory import ({}): bind failed ({})", handle_name, int(r));
      if (destroy_buffer) destroy_buffer(g.device, guest, nullptr);
      if (free_memory) free_memory(g.device, guest_memory, nullptr);
      guest = VK_NULL_HANDLE;
      guest_memory = VK_NULL_HANDLE;
      continue;
    }
    if (handle != kTypes[0]) REXLOG_INFO("[native] memory import: as {}", handle_name);
    break;
  }
  if (!guest || !guest_memory) {
    REXLOG_ERROR("[native] memory import failed with every handle type: the Kasumi renderer cannot draw on this "
                 "GPU's driver (update it; the log above says which step refused)");
    return;
  }
  uint64_t address = 0;
  if (g.vk.GetBufferDeviceAddress) {
    VkBufferDeviceAddressInfo bai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    bai.buffer = guest;
    address = g.vk.GetBufferDeviceAddress(g.device, &bai);
  }
  // A readback buffer for the check: 4 blocks of 4 KB.
  constexpr uint32_t kBlocks = 4, kBlock = 4096;
  const VkDeviceSize offsets[kBlocks] = {0x00100000, 0x04000000, 0x0A000000, 0x1E000000};
  VkBuffer readback = VK_NULL_HANDLE;
  void* mapped = nullptr;
  if (!CreateReadback(kBlocks * kBlock, &readback, &mapped)) return;
  (void)offsets;
  g.guest = guest;
  g.guest_address = address;
  g.readback = readback;
  g.readback_mapped = mapped;
  REXLOG_INFO("[native] memory import: the guest's 512 MB imported (memory type {}, device address {:#x}); "
              "read checks at frame 300, then every 1800", type, address);
}

// The read check, on pages that hold data: the first four 4 KB pages (on a
// 1 MB stride) with over 512 nonzero bytes, copied by the GPU from the
// imported buffer and compared with the CPU's view.
// Whether the guest has physical page range [physical, physical + size) committed
// and readable. Freed guest memory is decommitted with mprotect(PROT_NONE), so
// touching it from the CPU faults (native-m2-test6 crashed exactly so at its
// frame-3000 check). A physical page is mapped through three guest views -
// 0xA0000000 (64 KB pages), 0xC0000000 (16 MB) and 0xE0000000 (4 KB, offset by
// 0x1000) - and each view's heap only knows what was allocated through it, so
// the page counts as backed if any of them has it readable.
bool PhysicalRangeReadable(rex::memory::Memory* memory, uint32_t physical, uint32_t size) {
  constexpr uint32_t kViews[] = {0xA0000000u, 0xC0000000u, 0xE0000000u - 0x1000u};
  for (uint32_t view : kViews) {
    if (view == kViews[2] && physical < 0x1000) continue;
    const uint32_t first = view + physical, last = first + size - 1;
    auto* heap = memory->LookupHeap(first);
    uint32_t protect = 0;
    if (heap && heap == memory->LookupHeap(last) && heap->QueryProtect(first, &protect) &&
        (protect & rex::memory::kMemoryProtectRead) && heap->QueryProtect(last, &protect) &&
        (protect & rex::memory::kMemoryProtectRead)) {
      return true;
    }
  }
  return false;
}

// Log a line and get it onto the disk now (see VerifyGuestReads).
template <typename... Args>
void Step(fmt::format_string<Args...> format, Args&&... args) {
  REXLOG_INFO("[native] {}", fmt::format(format, std::forward<Args>(args)...));
  rex::FlushLogging();
}

void VerifyGuestReads() {
  if (!g.guest || !g.readback_mapped) return;
  auto* memory = REX_KERNEL_MEMORY();
  const uint8_t* base = memory->physical_membase();
  constexpr uint32_t kBlocks = 4, kBlock = 4096;
  VkDeviceSize offsets[kBlocks];
  uint32_t found = 0;
  for (VkDeviceSize o = 0x100000; o < 0x20000000 && found < kBlocks; o += 0x100000) {
    if (!PhysicalRangeReadable(memory, uint32_t(o), kBlock)) continue;
    uint32_t nonzero = 0;
    for (uint32_t b = 0; b < kBlock; ++b) nonzero += base[o + b] != 0;
    if (nonzero > 512) offsets[found++] = o;
  }
  if (!found) {
    REXLOG_INFO("[native] memory read check: no page with data found yet");
    return;
  }
  // [test8] Each step is logged and flushed: the PS5 app only flushes warnings
  // at once, so a crash used to take the last lines with it.
  Step("read check at frame {}: CPU scan done, {} pages; GPU copy next", g.frames, found);
  Slot& s = g.slots[g.next];
  g.vk.WaitForFences(g.device, 1, &s.fence, VK_TRUE, UINT64_MAX);
  g.vk.ResetFences(g.device, 1, &s.fence);
  g.vk.ResetCommandBuffer(s.cmd, 0);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  g.vk.BeginCommandBuffer(s.cmd, &bi);
  VkBufferCopy regions[kBlocks];
  for (uint32_t i = 0; i < found; ++i) regions[i] = {offsets[i], i * kBlock, kBlock};
  g.vk.CmdCopyBuffer(s.cmd, g.guest, g.readback, found, regions);
  g.vk.EndCommandBuffer(s.cmd);
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &s.cmd;
  VkResult r;
  {
    std::lock_guard<std::recursive_mutex> lock(*g.dev->queue_mutex);
    r = g.vk.QueueSubmit(VkQueue(g.dev->queue), 1, &si, s.fence);
  }
  if (r != VK_SUCCESS) {
    Step("read check: submit failed ({})", int(r));
    return;
  }
  r = g.vk.WaitForFences(g.device, 1, &s.fence, VK_TRUE, UINT64_MAX);
  Step("read check: GPU copy finished ({}); comparing", int(r));
  if (r != VK_SUCCESS) return;
  uint32_t equal = 0, nonzero = 0;
  std::string where;
  for (uint32_t i = 0; i < found; ++i) {
    const uint8_t* gpu = static_cast<const uint8_t*>(g.readback_mapped) + i * kBlock;
    const uint8_t* cpu = base + offsets[i];
    if (!PhysicalRangeReadable(memory, uint32_t(offsets[i]), kBlock)) {  // freed since the scan
      where += fmt::format(" {:#x}(freed)", offsets[i]);
      continue;
    }
    // (the game may write a page between the copy and this compare: a mismatch
    // in a busy page is possible; most pages are static)
    equal += memcmp(gpu, cpu, kBlock) == 0;
    for (uint32_t b = 0; b < kBlock; ++b) nonzero += cpu[b] != 0;
    where += fmt::format(" {:#x}", offsets[i]);
  }
  REXLOG_INFO("[native] memory read check at frame {}: {}/{} pages with data identical on the GPU ({} nonzero "
              "bytes; pages{}){}", g.frames, equal, found, nonzero, where,
              equal == found ? " - zero-copy reads work" : " - some differ (a page the game was writing?)");
  rex::FlushLogging();
}

VkCommandBuffer FrameCommands(uint32_t* index, bool* new_frame);

// The gate runs each side effect on the graphics system's effect thread (a
// guest thread: interrupts call into the game - izuna_gpu.cpp), once the
// native work recorded before it came has finished on the GPU, or after 30 ms
// - the guest may wait on a fence in the frame still being recorded, which
// only finishes at its swap: never a deadlock, at worst a late fence.
std::atomic<void (*)()> g_gpu_progress{nullptr};

// Waits on the oldest unfinished segment's fence (2 ms at most, then looks
// again), so finished work is seen as the GPU finishes it - test45 looked
// every 0.5 ms and the effect thread every 1 ms, up to 1.5 ms between the
// GPU finishing and the game's fence. Tells SetGpuProgressCallback's callback.
void CompletionThread() {  // (only watches fences: never calls into the guest)
  izuna::pc::RaiseThreadPriority(0);  // [IzunaRecomp] (Windows: above normal; elsewhere nothing)
  for (;;) {
    bool progress = false;
    // Segments finish in submission order (one queue): advance while done.
    for (uint64_t seq = g.completed.load() + 1; seq <= g.submitted.load(); ++seq) {
      if (g.GetFenceStatus(g.device, g.segs[seq % State::kSegments].fence) != VK_SUCCESS) break;
      uint64_t c = g.completed.load();
      while (seq > c && !g.completed.compare_exchange_weak(c, seq)) {
      }
      progress = true;
    }
    {
      std::lock_guard<std::mutex> lock(g.ticket_mutex);
      while (!g.tickets.empty() && g.tickets.front().segment <= g.completed.load()) {
        g.done_records.store(g.tickets.front().record);
        g.tickets.pop_front();
        progress = true;
      }
    }
    if (progress) {
      if (auto callback = g_gpu_progress.load(std::memory_order_acquire)) callback();
    }
    const uint64_t next = g.completed.load() + 1;
    if (next <= g.submitted.load()) {
      // (a segment's fence is reset only once it has finished and 64 more
      // were begun: never this one)
      g.vk.WaitForFences(g.device, 1, &g.segs[next % State::kSegments].fence, VK_TRUE, 2'000'000);
    } else {
      std::unique_lock<std::mutex> lock(g.side_mutex);
      g.side_cv.wait_for(lock, std::chrono::milliseconds(2));  // (woken at each submit)
    }
  }
}

// The side effects' pump (rex::external_frame::PumpSideEffects, called by the
// graphics system's effect thread): runs the steps RunAfterRecordedWork queued
// whose native work is done, in order. Installed by the first step - before
// any native work exists every step is due at once.
void InstallEffectPump() {
  rex::external_frame::SetSideEffectPump([] {
    using clock = std::chrono::steady_clock;
    for (;;) {
      rex::external_frame::SideEffect effect;
      {
        std::lock_guard<std::mutex> lock(g.side_mutex);
        if (g.pending.empty()) return;
        State::Pending& p = g.pending.front();
        const bool done = p.frame <= (DrawThreaded() ? g.done_records.load() : g.completed.load());
        const bool late = clock::now() - p.at > std::chrono::milliseconds(30);
        if (!done && !late) return;
        if (!done) {
          g.effects_late.fetch_add(1, std::memory_order_relaxed);
          static int late_logged = 0;  // (which work it waited for, and where the records were)
          if (late_logged < 8) {
            ++late_logged;
            REXLOG_INFO("[native] a side effect ran late (30 ms): waited for record {}; records done {}, last "
                        "kickoff or swap {}; segments done {} of {}",
                        p.frame, g.done_records.load(), DrawThreaded() ? LastKickoffRecord() : 0,
                        g.completed.load(), g.submitted.load());
          }
        }
        effect = std::move(p.effect);
        g.pending.pop_front();
      }
      static int logged = 0;
      if (logged < 6) {
        ++logged;
        REXLOG_INFO("[native] side effect run (completed {})", g.completed.load());
      }
      effect();  // (in order, on the effect thread)
      g.effects_run.fetch_add(1, std::memory_order_relaxed);
    }
  });
}

void InstallGpuSync() {
  g.GetFenceStatus = reinterpret_cast<PFN_vkGetFenceStatus>(g.vk.GetDeviceProcAddr(g.device, "vkGetFenceStatus"));
  if (!g.GetFenceStatus) return;
  std::thread(CompletionThread).detach();  // (lives as long as the app)
  REXLOG_INFO("[native] GPU sync on: fences and interrupts wait for the native frames");
}

bool Initialize() {
  g.dev = rex::external_frame::GetVulkanDevice();
  if (!g.dev || !g.dev->device || !g.dev->get_instance_proc_addr) {
    REXLOG_WARN("[native] no Vulkan device from the emulator yet");
    return false;
  }
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(g.dev->get_instance_proc_addr);
  const VkInstance instance = VkInstance(g.dev->instance);
  g.device = VkDevice(g.dev->device);
  g.vk.GetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
  g.vk.GetPhysicalDeviceMemoryProperties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
  if (!g.vk.GetDeviceProcAddr || !g.vk.GetPhysicalDeviceMemoryProperties) return false;
  bool ok = Load(g.vk.CreateImage, "vkCreateImage") &&
            Load(g.vk.GetImageMemoryRequirements, "vkGetImageMemoryRequirements") &&
            Load(g.vk.AllocateMemory, "vkAllocateMemory") && Load(g.vk.BindImageMemory, "vkBindImageMemory") &&
            Load(g.vk.CreateImageView, "vkCreateImageView") && Load(g.vk.CreateCommandPool, "vkCreateCommandPool") &&
            Load(g.vk.AllocateCommandBuffers, "vkAllocateCommandBuffers") && Load(g.vk.CreateFence, "vkCreateFence") &&
            Load(g.vk.WaitForFences, "vkWaitForFences") && Load(g.vk.ResetFences, "vkResetFences") &&
            Load(g.vk.ResetCommandBuffer, "vkResetCommandBuffer") &&
            Load(g.vk.BeginCommandBuffer, "vkBeginCommandBuffer") && Load(g.vk.EndCommandBuffer, "vkEndCommandBuffer") &&
            Load(g.vk.CmdPipelineBarrier, "vkCmdPipelineBarrier") &&
            Load(g.vk.CmdClearColorImage, "vkCmdClearColorImage") && Load(g.vk.QueueSubmit, "vkQueueSubmit") &&
            Load(g.vk.CreateBuffer, "vkCreateBuffer") &&
            Load(g.vk.GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements") &&
            Load(g.vk.BindBufferMemory, "vkBindBufferMemory") && Load(g.vk.MapMemory, "vkMapMemory") &&
            Load(g.vk.CmdCopyBuffer, "vkCmdCopyBuffer") &&
            Load(g.vk.CmdCopyImageToBuffer, "vkCmdCopyImageToBuffer");
  if (!ok) return false;
  g.vk.GetBufferDeviceAddress =
      reinterpret_cast<PFN_vkGetBufferDeviceAddress>(g.vk.GetDeviceProcAddr(g.device, "vkGetBufferDeviceAddress"));
  VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = g.dev->queue_family;
  if (g.vk.CreateCommandPool(g.device, &pci, nullptr, &g.pool) != VK_SUCCESS) return false;
  for (State::Segment& seg : g.segs) {
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (g.vk.AllocateCommandBuffers(g.device, &cai, &seg.cmd) != VK_SUCCESS ||
        g.vk.CreateFence(g.device, &fci, nullptr, &seg.fence) != VK_SUCCESS) {
      REXLOG_ERROR("[native] could not create the command buffer segments");
      return false;
    }
  }
  {  // GPU timestamps (optional)
    auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        gipa(instance, "vkGetPhysicalDeviceProperties"));
    auto createQueryPool = reinterpret_cast<PFN_vkCreateQueryPool>(g.vk.GetDeviceProcAddr(g.device, "vkCreateQueryPool"));
    g.CmdWriteTimestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(g.vk.GetDeviceProcAddr(g.device, "vkCmdWriteTimestamp"));
    g.CmdResetQueryPool = reinterpret_cast<PFN_vkCmdResetQueryPool>(g.vk.GetDeviceProcAddr(g.device, "vkCmdResetQueryPool"));
    g.GetQueryPoolResults =
        reinterpret_cast<PFN_vkGetQueryPoolResults>(g.vk.GetDeviceProcAddr(g.device, "vkGetQueryPoolResults"));
    VkPhysicalDeviceProperties props = {};
    if (properties) properties(VkPhysicalDevice(g.dev->physical_device), &props);
    VkQueryPoolCreateInfo qci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qci.queryCount = 4;
    if (props.limits.timestampComputeAndGraphics && props.limits.timestampPeriod > 0.0f && createQueryPool &&
        g.CmdWriteTimestamp && g.CmdResetQueryPool && g.GetQueryPoolResults &&
        createQueryPool(g.device, &qci, nullptr, &g.timestamps) == VK_SUCCESS) {
      g.timestamp_ns = props.limits.timestampPeriod;
    } else {
      g.timestamps = VK_NULL_HANDLE;
    }
  }
  for (Slot& s : g.slots) {
    if (!CreateSlot(s)) {
      REXLOG_ERROR("[native] could not create a frame image");
      return false;
    }
  }
  ProbeGuestMemoryImport(gipa, instance);
  TestGuestMemoryImport();
  DrawContext dc;
  dc.instance = instance;
  dc.physical_device = VkPhysicalDevice(g.dev->physical_device);
  dc.device = g.device;
  dc.get_instance_proc_addr = gipa;
  dc.get_device_proc_addr = g.vk.GetDeviceProcAddr;
  dc.guest_address = g.guest_address;
  dc.graphics_pipeline_library = g.dev->graphics_pipeline_library;
  dc.dual_src_blend = g.dev->dual_src_blend;
  dc.frame_commands = &FrameCommands;
  g.drawing = DrawInit(dc);
  if (g.drawing) {
    StartDrawWorker();
    InstallGpuSync();
    // The graphics system shows the latest native frame at the guest's swap:
    // with threaded recording the worker may still be finishing that swap's
    // frame - wait for it (else the previous frame shows again: a frame late,
    // a repeated frame). Each guest swap has its frame; 50 ms at most.
    rex::external_frame::SetSwapWait([] {
      static uint64_t seen = 0;
      std::unique_lock<std::mutex> lock(g.finished_mutex);
      if (!g.finished_cv.wait_for(lock, std::chrono::milliseconds(50), [] { return g.finished > seen; })) {
        g.swap_waits_late.fetch_add(1, std::memory_order_relaxed);
      }
      ++seen;
      if (g.finished > seen + 1) seen = g.finished - 1;  // (fell behind: catch up, never wait on old frames)
    });
  }
  rex::external_frame::SetProvider([](rex::external_frame::Frame& frame) {
    std::lock_guard<std::mutex> lock(g.frame_mutex);
    if (!g.have_shown) return false;
    frame = g.shown;
    return true;
  });
  REXLOG_INFO("[native] renderer up: {}x{} frames on the Vulkan device (app renderer features {})", FrameWidth(),
              FrameHeight(), g.dev->app_renderer_features);
  return true;
}

// Wait for segment `seq` to finish on the GPU (and say so for GPU sync).
void WaitSegment(uint64_t seq) {
  if (!seq || g.completed.load() >= seq) return;
  const PhaseTimer phase(kPhaseFence);
  const auto t0 = std::chrono::steady_clock::now();
  g.vk.WaitForFences(g.device, 1, &g.segs[seq % State::kSegments].fence, VK_TRUE, UINT64_MAX);
  NoteFenceWait(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
                             .count()));
  uint64_t c = g.completed.load();
  while (seq > c && !g.completed.compare_exchange_weak(c, seq)) {
  }
  g.side_cv.notify_all();
}

// The command buffer being recorded: a pool segment, begun on first use (by
// native_draw's first draw after a swap or kickoff, or the test pattern). A
// new frame first waits for the frame that last used its slot (two back).
VkCommandBuffer FrameCommands(uint32_t* index, bool* new_frame) {
  if (index) *index = g.next;
  if (new_frame) *new_frame = false;
  if (g.recording) return g.segs[(g.submitted.load() + 1) % State::kSegments].cmd;
  bool first_segment = false;
  if (!g.frame_open) {
    WaitSegment(g.slot_last_seq[g.next]);
    if (g.timestamps && g.timestamps_valid[g.next]) {  // the slot's previous frame: done, so readable
      uint64_t t[2] = {};
      if (g.GetQueryPoolResults(g.device, g.timestamps, g.next * 2, 2, sizeof(t), t, sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0]) {
        g.gpu_ns += double(t[1] - t[0]) * g.timestamp_ns;
        ++g.gpu_frames;
      }
      g.timestamps_valid[g.next] = false;
    }
    g.frame_open = true;
    first_segment = true;
    if (new_frame) *new_frame = true;
  }
  const uint64_t seq = g.submitted.load() + 1;
  State::Segment& seg = g.segs[seq % State::kSegments];
  if (seq > State::kSegments) WaitSegment(seq - State::kSegments);  // (its last use: long done)
  g.vk.ResetFences(g.device, 1, &seg.fence);
  g.vk.ResetCommandBuffer(seg.cmd, 0);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  g.vk.BeginCommandBuffer(seg.cmd, &bi);
  if (first_segment && g.timestamps) {
    g.CmdResetQueryPool(seg.cmd, g.timestamps, g.next * 2, 2);
    g.CmdWriteTimestamp(seg.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.timestamps, g.next * 2);
  }
  g.recording = true;
  return seg.cmd;
}

// End and submit the segment being recorded.
bool SubmitSegment() {
  const PhaseTimer phase(kPhaseSubmit);
  const uint64_t seq = g.submitted.load() + 1;
  State::Segment& seg = g.segs[seq % State::kSegments];
  g.vk.EndCommandBuffer(seg.cmd);
  g.recording = false;
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &seg.cmd;
  VkResult r;
  const auto t0 = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::recursive_mutex> lock(*g.dev->queue_mutex);
    r = g.vk.QueueSubmit(VkQueue(g.dev->queue), 1, &si, seg.fence);
  }
  g.submit_ns += uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  ++g.submits;
  if (r != VK_SUCCESS) {
    REXLOG_ERROR("[native] vkQueueSubmit failed ({})", int(r));
    return false;
  }
  g.submitted.store(seq);
  g.side_cv.notify_all();
  return true;
}

// The test pattern: clear the slot's image to a slowly cycling colour.
void RecordTestPattern(Slot& s, VkCommandBuffer cmd) {
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.oldLayout = s.used ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = s.image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  g.vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                          0, nullptr, 1, &b);
  // A slow trip round the colour wheel (~6 s), dimmed: easy on the eyes.
  const float t = float(g.frames % 360) / 360.0f * 6.2831853f;
  VkClearColorValue c = {};
  c.float32[0] = 0.35f + 0.25f * std::cos(t);
  c.float32[1] = 0.35f + 0.25f * std::cos(t - 2.094f);
  c.float32[2] = 0.35f + 0.25f * std::cos(t + 2.094f);
  c.float32[3] = 1.0f;
  g.vk.CmdClearColorImage(cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &b.subresourceRange);
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  g.vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                          0, nullptr, 1, &b);
}

// [desktop test build] Copy the slot's image to the dump buffer (in `cmd`).
// A free dump buffer (made on first use), or null.
State::DumpSlot* FreeDump() {
  for (State::DumpSlot& d : g.dumps) {
    if (d.seq) continue;
    if (!d.buffer && !CreateReadback(VkDeviceSize(FrameWidth()) * FrameHeight() * 4, &d.buffer, &d.mapped)) {
      return nullptr;
    }
    return &d;
  }
  return nullptr;
}

// Copies the slot's image into a dump buffer (in `cmd`).
bool RecordDump(Slot& s, VkCommandBuffer cmd, State::DumpSlot& dump) {
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = s.image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  g.vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                          nullptr, 1, &b);
  VkBufferImageCopy r = {};
  r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  r.imageExtent = {FrameWidth(), FrameHeight(), 1};
  g.vk.CmdCopyImageToBuffer(cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dump.buffer, 1, &r);
  b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  g.vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                          0, nullptr, 1, &b);
  return true;
}

// Saved frames are written by one thread of their own, from a fixed set of
// buffers: no allocation on the drawing thread, and a bounded amount held.
// (test23 made a thread and a 3.6 MB copy per frame; a 120-frame burst kept
// ~430 MB waiting on slow storage - the first level's load then ran the
// console out of memory, and a burst in a cutscene hung the game.)
// A frame with no free buffer is dropped and counted; once the queue drains,
// all but two buffers are given back.
struct DumpWriter {
  static constexpr size_t kBuffers = 32;  // 118 MB at 1280x720: a whole 30-frame burst (the console writes ~17 MB/s)
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::unique_ptr<char[]>> free;
  size_t made = 0;
  std::deque<std::pair<std::unique_ptr<char[]>, std::string>> queue;
  bool started = false;
  uint64_t written = 0, dropped = 0;
  double write_ms = 0;  // since the last report
  uint64_t write_bytes = 0;
};
DumpWriter& Writer() {
  static DumpWriter* w = new DumpWriter;  // (lives as long as the app)
  return *w;
}

// A saved frame as QOI (lossless; "The Quite OK Image Format", Dominic
// Szablewski, qoiformat.org - written here from its specification): ~3x
// smaller than the raw pixels for the game's frames, so the console's slow
// storage keeps up (test29 wrote 3.6 MB a second, raw, and its writes went
// from 0.2 s to 3 s each). Pillow and ffmpeg read it.
size_t EncodeQoi(const uint8_t* rgba, uint32_t width, uint32_t height, std::vector<uint8_t>& out) {
  const size_t pixels = size_t(width) * height;
  out.resize(14 + pixels * 5 + 8);
  uint8_t* o = out.data();
  auto be32 = [&](uint32_t v) {
    *o++ = uint8_t(v >> 24);
    *o++ = uint8_t(v >> 16);
    *o++ = uint8_t(v >> 8);
    *o++ = uint8_t(v);
  };
  *o++ = 'q';
  *o++ = 'o';
  *o++ = 'i';
  *o++ = 'f';
  be32(width);
  be32(height);
  *o++ = 4;  // RGBA
  *o++ = 0;  // sRGB with linear alpha
  uint32_t index[64] = {};
  uint8_t pr = 0, pg = 0, pb = 0, pa = 255;
  uint32_t run = 0;
  for (size_t i = 0; i < pixels; ++i) {
    const uint8_t* px = rgba + i * 4;
    const uint8_t r = px[0], g = px[1], b = px[2], a = px[3];
    if (r == pr && g == pg && b == pb && a == pa) {
      if (++run == 62 || i + 1 == pixels) {
        *o++ = uint8_t(0xC0 | (run - 1));
        run = 0;
      }
      continue;
    }
    if (run) {
      *o++ = uint8_t(0xC0 | (run - 1));
      run = 0;
    }
    const uint32_t value = uint32_t(r) << 24 | uint32_t(g) << 16 | uint32_t(b) << 8 | a;
    const uint32_t slot = (r * 3u + g * 5u + b * 7u + a * 11u) % 64u;
    if (index[slot] == value) {
      *o++ = uint8_t(slot);
    } else {
      index[slot] = value;
      if (a == pa) {
        const int8_t vr = int8_t(r - pr), vg = int8_t(g - pg), vb = int8_t(b - pb);
        const int8_t vg_r = int8_t(vr - vg), vg_b = int8_t(vb - vg);
        if (vr > -3 && vr < 2 && vg > -3 && vg < 2 && vb > -3 && vb < 2) {
          *o++ = uint8_t(0x40 | (vr + 2) << 4 | (vg + 2) << 2 | (vb + 2));
        } else if (vg_r > -9 && vg_r < 8 && vg > -33 && vg < 32 && vg_b > -9 && vg_b < 8) {
          *o++ = uint8_t(0x80 | (vg + 32));
          *o++ = uint8_t((vg_r + 8) << 4 | (vg_b + 8));
        } else {
          *o++ = 0xFE;
          *o++ = r;
          *o++ = g;
          *o++ = b;
        }
      } else {
        *o++ = 0xFF;
        *o++ = r;
        *o++ = g;
        *o++ = b;
        *o++ = a;
      }
    }
    pr = r;
    pg = g;
    pb = b;
    pa = a;
  }
  for (int i = 0; i < 7; ++i) *o++ = 0;
  *o++ = 1;
  return size_t(o - out.data());
}

void DumpWriterMain() {
  DumpWriter& w = Writer();
  const size_t size = size_t(FrameWidth()) * FrameHeight() * 4;
  const auto dir = rex::filesystem::GetExecutableFolder() / "native_frames";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::vector<uint8_t> qoi;
  std::unique_lock<std::mutex> lock(w.mutex);
  for (;;) {
    w.cv.wait(lock, [&] { return !w.queue.empty(); });
    uint64_t bytes = 0;
    auto item = std::move(w.queue.front());
    w.queue.pop_front();
    lock.unlock();
    const auto t0 = std::chrono::steady_clock::now();
    {
      std::ofstream f(dir / item.second, std::ios::binary);
      if (item.second.ends_with(".qoi")) {
        const size_t n = EncodeQoi(reinterpret_cast<const uint8_t*>(item.first.get()), FrameWidth(), FrameHeight(), qoi);
        f.write(reinterpret_cast<const char*>(qoi.data()), std::streamsize(n));
        bytes += n;
      } else {
        f.write(item.first.get(), std::streamsize(size));
        bytes += size;
      }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    lock.lock();
    ++w.written;
    w.write_ms += ms;
    w.write_bytes += bytes;
    w.free.push_back(std::move(item.first));
    while (w.queue.empty() && w.free.size() > 2) {
      w.free.pop_back();
      --w.made;
    }
  }
}

// The saved frame, once its GPU work is done: copied out and queued for the
// writer.
void CollectDump() {
  DumpWriter& w = Writer();
  const size_t size = size_t(FrameWidth()) * FrameHeight() * 4;
  for (State::DumpSlot& d : g.dumps) {
    if (!d.seq || g.completed.load() < d.seq) continue;
    d.seq = 0;
    std::unique_lock<std::mutex> lock(w.mutex);
    // (at 1440p / 4K a frame is 4x / 9x the bytes: as many fewer buffers, 4 at least)
    const size_t max_buffers =
        std::max<size_t>(4, size_t(double(DumpWriter::kBuffers) / (double(RenderScale()) * RenderScale())));
    if (w.free.empty() && w.made < max_buffers) {
      w.free.emplace_back(new (std::nothrow) char[size]);
      if (w.free.back()) {
        ++w.made;
      } else {
        w.free.pop_back();
      }
    }
    if (w.free.empty()) {
      ++w.dropped;
      continue;
    }
    if (!w.started) {
      try {
        std::thread(DumpWriterMain).detach();
        w.started = true;
      } catch (const std::exception&) {
        ++w.dropped;
        continue;
      }
    }
    std::unique_ptr<char[]> buffer = std::move(w.free.back());
    w.free.pop_back();
    lock.unlock();  // (the copy without the lock: the writer may finish a file meanwhile)
    std::memcpy(buffer.get(), d.mapped, size);
    lock.lock();
    w.queue.emplace_back(std::move(buffer), d.name);
    w.cv.notify_one();
  }
}

// "N written (X ms each), M dropped", since the last call; empty if none.
std::string DumpWriterReport() {
  DumpWriter& w = Writer();
  std::lock_guard<std::mutex> lock(w.mutex);
  if (!w.written && !w.dropped) return {};
  std::string r = fmt::format("{} frames written ({:.1f} ms, {:.2f} MB each, {} waiting)", w.written,
                              w.written ? w.write_ms / double(w.written) : 0.0,
                              w.written ? double(w.write_bytes) / double(w.written) / 1048576.0 : 0.0,
                              w.queue.size());
  if (w.dropped) r += fmt::format(", {} dropped (storage too slow)", w.dropped);
  w.written = w.dropped = 0;
  w.write_ms = 0;
  w.write_bytes = 0;
  return r;
}

// One frame at the guest's swap: the native drawing if this frame recorded
// any, else the test pattern; then hand the slot's image to the presenter
// once submitted (queue order: the presenter's work comes after it on the
// same queue, so no semaphore).
// A frame is finished (shown or kept): the graphics system's swap may go on.
void NoteFrameFinished() {
  {
    std::lock_guard<std::mutex> lock(g.finished_mutex);
    ++g.finished;
  }
  g.finished_cv.notify_all();
}

void FinishFrame(uint32_t device, uint32_t front_texture) {
  struct Finished {
    ~Finished() { NoteFrameFinished(); }
  } finished;
  const bool native = g.frame_open && g.drawing;
  Slot& s = g.slots[g.next];
  // A swap with no native work, once native frames are on screen: the last
  // one stays (the test pattern here flashed a colour - test15's green frame).
  const bool keep_last = g.drawing && g.have_shown;
  if (keep_last && !g.frame_open) {
    ++g.frames;
    ++g.frames_repeated;
    return;
  }
  const VkCommandBuffer cmd = FrameCommands(nullptr, nullptr);
  if (!native || !DrawEndFrame(device, front_texture, s.image, s.used, FrameWidth(), FrameHeight())) {
    if (keep_last) {  // (work was recorded but nothing to show: run it, keep the picture)
      if (SubmitSegment()) g.slot_last_seq[g.next] = g.submitted.load();
      g.frame_open = false;
      ++g.frames;
      ++g.frames_repeated;
      return;
    }
    RecordTestPattern(s, cmd);
  }
  const int32_t every = REXCVAR_GET(native_dump_every);
  CollectDump();
  // (a burst: every frame for native_dump_burst frames, each
  // native_dump_burst_every - flicker shows between consecutive frames)
  const int32_t burst = REXCVAR_GET(native_dump_burst);
  const uint64_t burst_every = uint64_t(std::max(REXCVAR_GET(native_dump_burst_every), burst + 1));
  const bool in_burst = burst > 0 && g.frames % burst_every < uint64_t(burst);
  std::string named;
  {
    std::lock_guard<std::mutex> lock(g.named_mutex);
    named.swap(g.named_dump);
  }
  const bool want_dump = in_burst || !named.empty() || (every > 0 && g.frames % uint64_t(every) == 0);
  State::DumpSlot* dump_slot = want_dump ? FreeDump() : nullptr;
  if (want_dump && !dump_slot) ++g.dumps_skipped;
  const bool dump = dump_slot && RecordDump(s, cmd, *dump_slot);
  if (g.timestamps) {
    g.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.timestamps, g.next * 2 + 1);
    g.timestamps_valid[g.next] = true;
  }
  if (!SubmitSegment()) return;
  g.slot_last_seq[g.next] = g.submitted.load();
  if (REXCVAR_GET(native_sync_frames)) WaitSegment(g.submitted.load());
  g.frame_open = false;
  g.next = (g.next + 1) % g.slots.size();
  s.used = true;
  ++g.frames;
  if (dump) {
    // Kept in turn (native_dump_keep slots; the log says which frame went
    // where), or all of them by frame number (native_dump_keep 0).
    // (a burst's frames are all kept, by frame number)
    const int32_t keep = in_burst ? 0 : REXCVAR_GET(native_dump_keep);
    if (!named.empty()) {
      dump_slot->name = named + ".rgba";
    } else if (keep <= 0) {
      dump_slot->name = fmt::format("frame_{:06}_{}x{}.qoi", g.frames, FrameWidth(), FrameHeight());
    } else {
      const uint64_t slot = (g.frames / uint64_t(std::max(every, 1))) % uint64_t(keep);
      dump_slot->name = fmt::format("frame_{:02}_{}x{}.qoi", slot, FrameWidth(), FrameHeight());
    }
    if (!in_burst || g.frames % burst_every == 1) {
      REXLOG_INFO("[native] frame {} saved as native_frames/{}{}", g.frames, dump_slot->name,
                  in_burst ? fmt::format(" (a burst of {} frames begins)", burst) : std::string());
    }
    dump_slot->seq = g.submitted.load();
  }
  std::lock_guard<std::mutex> lock(g.frame_mutex);
  g.shown.vk_image = uint64_t(s.image);
  g.shown.vk_image_view = uint64_t(s.view);
  g.shown.width = FrameWidth();
  g.shown.height = FrameHeight();
  g.have_shown = true;
}

}  // namespace

void RunAfterRecordedWork(std::function<void()> effect) {
  // What has been recorded so far: in threaded recording, the records before
  // the next one (due once a kickoff or swap record from there on is done) -
  // or, when the last record is a kickoff or swap, that one: Direct3D inserts
  // its fences inside the kick (8373A3D0 -> 8373A170 -> InsertFence), after
  // the kick's record. Waiting for the kick after it held every fence one
  // kick late, which showed where the game waits on a fence straight after
  // its kick: the game over screen met the 30 ms fallback each frame (test52:
  // 15 fps). Else the open segment, if one is being recorded, or the last
  // submitted.
  static std::once_flag pump_installed;
  std::call_once(pump_installed, InstallEffectPump);
  uint64_t token;
  if (DrawThreaded()) {
    const uint64_t next = NextRecordSeq();
    token = LastKickoffRecord() == next - 1 ? next - 1 : next;
  } else {
    token = g.submitted.load() + (g.recording ? 1 : 0);
  }
  std::lock_guard<std::mutex> lock(g.side_mutex);
  g.pending.push_back({token, std::chrono::steady_clock::now(), std::move(effect)});
}

void SetGpuProgressCallback(void (*callback)()) { g_gpu_progress.store(callback, std::memory_order_release); }

bool RequestNamedDump(const char* name) {
  if (!g.ready || !g.drawing) return false;
  std::lock_guard<std::mutex> lock(g.named_mutex);
  g.named_dump = name;
  return true;
}

void OnGuestKickoff() {
  if (!g.ready || !g.drawing || std::this_thread::get_id() != g.render_thread) return;
  if (DrawThreaded()) {
    QueueKickoff();
    return;
  }
  RendererKickoff();
}

// The kickoff's work: the segment being recorded is submitted (the worker,
// with native_threaded; else the game's render thread).
void RendererKickoff() {
  if (!g.recording) return;
  DrawEndSegment();
  SubmitSegment();
}

void RecordDone(uint64_t seq) {
  std::lock_guard<std::mutex> lock(g.ticket_mutex);
  g.tickets.push_back({seq, g.submitted.load()});
  g.side_cv.notify_all();
}

void OnGuestSwap(uint32_t device, uint32_t front_texture) {
  rex::audio::HostStallScope stall;  // (a long wait here holds the game's audio with it)
  g.render_thread = std::this_thread::get_id();
  static bool tried = false;
  if (!g.ready && !tried) {
    tried = true;
    g.ready = Initialize();
  }
  if (!g.ready) return;
  if (g.drawing && DrawThreaded()) {
    QueueSwap(device, front_texture);
    return;
  }
  RendererSwap(device, front_texture);
}

// The swap's work: the frame finished and shown, the periodic reports.
void RendererSwap(uint32_t device, uint32_t front_texture) {
  FinishFrame(device, front_texture);
  if (!g.drawing && (g.frames == 300 || g.frames % 1800 == 0)) {
    Step("read check at frame {}: starting", g.frames);
    VerifyGuestReads();
  }
  if (g.frames == 1 || g.frames % 600 == 0) {
    std::string gpu = g.gpu_frames ? fmt::format("; GPU span {:.2f} ms a frame (first native command to last)", g.gpu_ns / g.gpu_frames / 1e6) : "";
    if (g.frames > 1 && g.submits) {
      const double n = 600.0;  // (frames since the last report)
      gpu += fmt::format("; {:.1f} submits a frame, {:.2f} ms a frame in vkQueueSubmit (worst-case blocking: "
                         "driver), {:.3f} ms each",
                         double(g.submits) / n, double(g.submit_ns) / n / 1e6, double(g.submit_ns) / g.submits / 1e6);
    }
    if (g.frames_repeated) gpu += fmt::format("; {} swaps kept the previous frame", g.frames_repeated);
    if (g.dumps_skipped) {
      gpu += fmt::format("; {} frames not saved (the save buffers were busy)", g.dumps_skipped);
      g.dumps_skipped = 0;
    }
    if (const std::string saved = DumpWriterReport(); !saved.empty()) gpu += "; saved frames: " + saved;
    if (const uint64_t late = g.swap_waits_late.exchange(0)) {
      gpu += fmt::format("; {} swaps waited 50 ms for their frame", late);
    }
    g.frames_repeated = 0;
    g.submits = g.submit_ns = 0;
    g.gpu_ns = 0.0;
    g.gpu_frames = 0;
    Step("{} {} frames presented ({} protection changes kept off the imported memory; GPU side effects: {} run, "
         "{} by the 30 ms fallback{})",
         g.frames, g.drawing ? "native" : "test", rex::ps5diag::frozen_skips.load(std::memory_order_relaxed),
         g.effects_run.load(), g.effects_late.load(), gpu);
    if (g.drawing) DrawReport(g.frames);
  }
}

}  // namespace ng2native
