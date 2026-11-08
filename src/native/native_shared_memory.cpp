// [IzunaRecomp] The guest's physical memory in a buffer the GPU shares with the
// CPU - see native_shared_memory.h.
#include "native/native_shared_memory.h"

#if defined(__linux__)  // (Android too)

#if defined(__ANDROID__)
#include <android/hardware_buffer.h>
#include <sys/socket.h>
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <initializer_list>
#include <vector>

#include <rex/external_frame.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ui/vulkan/api.h>

namespace ng2native::shared_memory {
namespace {

// The guest's physical memory, and the 4 KB the 0xE0000000 view reaches past
// it (it maps 4 KB in), rounded to 64 KB.
constexpr VkDeviceSize kSize = 0x20000000ull + 0x10000ull;

struct State {
  bool active = false;      // the guest's memory is built on it
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint32_t type = 0;
#if defined(__ANDROID__)
  AHardwareBuffer* ahb = nullptr;
#endif
} g;

struct Vk {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  PFN_vkGetDeviceProcAddr gdpa = nullptr;
  PFN_vkGetInstanceProcAddr gipa = nullptr;
  template <typename T>
  T Device(const char* name) const {
    return reinterpret_cast<T>(gdpa(device, name));
  }
  template <typename T>
  T Instance(const char* name) const {
    return reinterpret_cast<T>(gipa(instance, name));
  }
};

bool GetVk(Vk& vk) {
  const auto* dev = rex::external_frame::GetVulkanDevice();
  if (!dev || !dev->device || !dev->get_instance_proc_addr) return false;
  vk.instance = VkInstance(dev->instance);
  vk.physical = VkPhysicalDevice(dev->physical_device);
  vk.device = VkDevice(dev->device);
  vk.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dev->get_instance_proc_addr);
  vk.gdpa = vk.Instance<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
  return vk.gdpa != nullptr;
}

// The first memory type of `bits` with every flag of a preference, in order.
uint32_t MemoryType(const Vk& vk, uint32_t bits, std::initializer_list<VkMemoryPropertyFlags> preferences,
                    VkMemoryPropertyFlags* flags_out = nullptr) {
  auto props_fn = vk.Instance<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties");
  VkPhysicalDeviceMemoryProperties props;
  props_fn(vk.physical, &props);
  for (const VkMemoryPropertyFlags want : preferences) {
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) {
        if (flags_out) *flags_out = props.memoryTypes[i].propertyFlags;
        return i;
      }
    }
  }
  return UINT32_MAX;
}

// Two CPU mappings of fd show one memory (the guest maps it several times).
bool MapsAndAliases(int fd) {
  auto* a = static_cast<uint8_t*>(mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  if (a == MAP_FAILED) {
    REXLOG_WARN("[native] shared memory: the CPU cannot map it (errno {})", errno);
    return false;
  }
  // (4 KB in, as the guest's 0xE0000000 view, and no further than the buffer's
  // end: a dma-buf refuses a mapping past it - test49 on the Deck)
  auto* b = static_cast<uint8_t*>(mmap(nullptr, kSize - 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x1000));
  bool same = false;
  if (b != MAP_FAILED) {
    a[0x3000] = 0x77;
    same = b[0x2000] == 0x77;  // (b starts 4 KB in, as the 0xE0000000 view)
    a[0x3000] = 0;
    munmap(b, kSize - 0x1000);
  }
  munmap(a, kSize);
  if (b == MAP_FAILED) {
    REXLOG_WARN("[native] shared memory: a second CPU mapping of it failed (errno {})", errno);
  } else if (!same) {
    REXLOG_WARN("[native] shared memory: two CPU mappings of it do not show one memory");
  }
  return same;
}

#if defined(__ANDROID__)
// The fds an AHardwareBuffer's handle carries (its dma-buf among them).
std::vector<int> AhbFds(AHardwareBuffer* ahb) {
  std::vector<int> fds;
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return fds;
  if (AHardwareBuffer_sendHandleToUnixSocket(ahb, sv[0]) == 0) {
    char data[4096];
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 32)];
    iovec iov = {data, sizeof(data)};
    msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    if (recvmsg(sv[1], &msg, 0) > 0) {
      for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
          const int n = int((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
          for (int i = 0; i < n; ++i) fds.push_back(reinterpret_cast<int*>(CMSG_DATA(c))[i]);
        }
      }
    }
  }
  close(sv[0]);
  close(sv[1]);
  return fds;
}

// Android: an AHardwareBuffer; the renderer imports it later (RendererBuffer).
rex::memory::FileMappingHandle Provide(size_t* size) {
  AHardwareBuffer_Desc d = {};
  d.width = uint32_t(kSize);
  d.height = 1;
  d.layers = 1;
  d.format = AHARDWAREBUFFER_FORMAT_BLOB;
  d.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
            AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
  AHardwareBuffer* ahb = nullptr;
  if (const int rc = AHardwareBuffer_allocate(&d, &ahb); rc != 0 || !ahb) {
    REXLOG_ERROR("[native] shared memory: no hardware buffer for the guest's memory ({})", rc);
    return rex::memory::kFileMappingHandleInvalid;
  }
  int fd = -1;
  for (int f : AhbFds(ahb)) {
    if (fd < 0 && lseek(f, 0, SEEK_END) >= off_t(kSize)) {
      fd = f;
    } else {
      close(f);
    }
  }
  if (fd < 0 || !MapsAndAliases(fd)) {
    REXLOG_ERROR("[native] shared memory: the hardware buffer's memory is not mappable");
    if (fd >= 0) close(fd);
    AHardwareBuffer_release(ahb);
    return rex::memory::kFileMappingHandleInvalid;
  }
  g.ahb = ahb;
  g.active = true;
  *size = size_t(kSize);
  REXLOG_INFO("[native] shared memory: the guest's physical memory is a hardware buffer the GPU shares");
  return fd;
}

#else

// Linux: Vulkan memory on the renderer's own device, exported as a dma-buf.
// Tried first on every GPU (RADV and ANV refuse to import a shared-memory
// file even where they have VK_EXT_external_memory_host); if any step fails the
// guest keeps its mapping file and the renderer imports it the PC's way
// (NVIDIA's driver).
rex::memory::FileMappingHandle Provide(size_t* size) {
  Vk vk;
  if (!GetVk(vk)) {
    REXLOG_WARN("[native] shared memory: no Vulkan device yet - the guest's memory stays as it is");
    return rex::memory::kFileMappingHandleInvalid;
  }
  auto get_fd = vk.Device<PFN_vkGetMemoryFdKHR>("vkGetMemoryFdKHR");
  auto create_buffer = vk.Device<PFN_vkCreateBuffer>("vkCreateBuffer");
  auto destroy_buffer = vk.Device<PFN_vkDestroyBuffer>("vkDestroyBuffer");
  auto requirements = vk.Device<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements");
  auto allocate = vk.Device<PFN_vkAllocateMemory>("vkAllocateMemory");
  auto free_memory = vk.Device<PFN_vkFreeMemory>("vkFreeMemory");
  auto bind = vk.Device<PFN_vkBindBufferMemory>("vkBindBufferMemory");
  if (!get_fd || !create_buffer || !requirements || !allocate || !bind) {
    REXLOG_INFO("[native] shared memory: the device cannot export memory (VK_KHR_external_memory_fd)");
    return rex::memory::kFileMappingHandleInvalid;
  }
  for (const VkExternalMemoryHandleTypeFlagBits handle :
       {VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT}) {
    const char* handle_name = handle == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT ? "dma-buf" : "opaque fd";
    VkExternalMemoryBufferCreateInfo ebci = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ebci.handleTypes = handle;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext = &ebci;
    bci.size = kSize;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (create_buffer(vk.device, &bci, nullptr, &buffer) != VK_SUCCESS) continue;
    VkMemoryRequirements req;
    requirements(vk.device, buffer, &req);
    // CPU-cached: the game reads its memory all the time, and uncached memory
    // reads at a hundredth of the speed (167 MB/s on the Deck, test48).
    constexpr VkMemoryPropertyFlags kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryPropertyFlags flags = 0;
    const uint32_t type = MemoryType(vk, req.memoryTypeBits, {kHost | VK_MEMORY_PROPERTY_HOST_CACHED_BIT}, &flags);
    if (type == UINT32_MAX) {
      REXLOG_INFO("[native] shared memory: no CPU-cached memory type to export as {}", handle_name);
      destroy_buffer(vk.device, buffer, nullptr);
      continue;
    }
    VkMemoryAllocateFlagsInfo address = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    address.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.pNext = &address;
    exp.handleTypes = handle;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &exp;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult r = allocate(vk.device, &mai, nullptr, &memory);
    if (r != VK_SUCCESS || (r = bind(vk.device, buffer, memory, 0)) != VK_SUCCESS) {
      REXLOG_INFO("[native] shared memory: {} allocation failed ({})", handle_name, int(r));
      if (memory) free_memory(vk.device, memory, nullptr);
      destroy_buffer(vk.device, buffer, nullptr);
      continue;
    }
    VkMemoryGetFdInfoKHR gfi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    gfi.memory = memory;
    gfi.handleType = handle;
    int fd = -1;
    if (get_fd(vk.device, &gfi, &fd) != VK_SUCCESS || fd < 0 || !MapsAndAliases(fd)) {
      if (fd >= 0) close(fd);
      free_memory(vk.device, memory, nullptr);
      destroy_buffer(vk.device, buffer, nullptr);
      continue;
    }
    g.buffer = buffer;
    g.memory = memory;
    g.type = type;
    g.active = true;
    *size = size_t(kSize);
    REXLOG_INFO("[native] shared memory: the guest's physical memory is Vulkan memory (type {}, {}) shared as a {}",
                type, (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "device-local" : "system", handle_name);
    return fd;
  }
  REXLOG_INFO("[native] shared memory: not available on this device - the guest's memory stays as it is");
  return rex::memory::kFileMappingHandleInvalid;
}
#endif  // __ANDROID__

}  // namespace

void Install() { rex::memory::SetExternalPhysicalProvider(Provide); }

bool RendererBuffer(Guest* out) {
  if (!g.active) return false;
#if defined(__ANDROID__)
  if (!g.buffer) {  // the hardware buffer, imported into the renderer's device once
    Vk vk;
    if (!GetVk(vk)) return false;
    auto props_fn = vk.Device<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
        "vkGetAndroidHardwareBufferPropertiesANDROID");
    auto create_buffer = vk.Device<PFN_vkCreateBuffer>("vkCreateBuffer");
    auto allocate = vk.Device<PFN_vkAllocateMemory>("vkAllocateMemory");
    auto bind = vk.Device<PFN_vkBindBufferMemory>("vkBindBufferMemory");
    if (!props_fn) {
      REXLOG_ERROR("[native] shared memory: the driver cannot import hardware buffers");
      return false;
    }
    VkAndroidHardwareBufferPropertiesANDROID hp = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    VkResult r = props_fn(vk.device, g.ahb, &hp);
    if (r != VK_SUCCESS || !hp.memoryTypeBits) {
      REXLOG_ERROR("[native] shared memory: the hardware buffer is not importable ({})", int(r));
      return false;
    }
    VkExternalMemoryBufferCreateInfo ebci = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ebci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext = &ebci;
    bci.size = kSize;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (create_buffer(vk.device, &bci, nullptr, &buffer) != VK_SUCCESS) return false;
    // A host-visible coherent type if the driver offers one for it (Qualcomm:
    // types 1 and 4, test47): the GPU's own writes then reach the CPU.
    constexpr VkMemoryPropertyFlags kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = MemoryType(vk, hp.memoryTypeBits, {kHost});
    if (type == UINT32_MAX) type = uint32_t(__builtin_ctz(hp.memoryTypeBits));
    VkMemoryAllocateFlagsInfo address = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    address.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    imp.pNext = &address;
    imp.buffer = g.ahb;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &imp;
    mai.allocationSize = hp.allocationSize;
    mai.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if ((r = allocate(vk.device, &mai, nullptr, &memory)) != VK_SUCCESS ||
        (r = bind(vk.device, buffer, memory, 0)) != VK_SUCCESS) {
      REXLOG_ERROR("[native] shared memory: importing the hardware buffer failed ({}, type {})", int(r), type);
      return false;
    }
    g.buffer = buffer;
    g.memory = memory;
    g.type = type;
    REXLOG_INFO("[native] shared memory: hardware buffer imported (memory type {} of {:#x})", type,
                hp.memoryTypeBits);
  }
#endif
  out->buffer = g.buffer;
  out->memory = g.memory;
  out->type = g.type;
  return true;
}

}  // namespace ng2native::shared_memory

#else  // not Linux / Android: the PC's import only

namespace ng2native::shared_memory {
void Install() {}
bool RendererBuffer(Guest*) { return false; }
}  // namespace ng2native::shared_memory

#endif
