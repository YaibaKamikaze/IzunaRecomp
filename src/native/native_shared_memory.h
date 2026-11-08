// [IzunaRecomp] The guest's physical memory in a buffer the GPU shares with the
// CPU, for GPU drivers that cannot import the guest's own memory - no
// VK_EXT_external_memory_host: Qualcomm's and Turnip on Android, SteamOS's
// RADV (the Steam Deck), RADV and ANV refusing a shared-memory file. The PC
// way imports the guest's memory into Vulkan; this way the guest's memory is
// built on a Vulkan-shareable buffer instead, and the renderer reads that
// buffer in place exactly as it reads an imported one.
//   Linux:   Vulkan memory (CPU-cached) on the renderer's own device, exported
//            as a dma-buf, which the guest's physical views map.
//   Android: an AHardwareBuffer (BLOB); its dma-buf is what the views map, and
//            the renderer imports the buffer into Vulkan.
// Measured on the devices (test47/48): the CPU through it as fast as through
// plain memory, CPU -> GPU visible; GPU -> CPU too on the Deck.
#pragma once

#include <cstdint>

namespace ng2native::shared_memory {

// Registers the provider the SDK's Memory::Initialize asks (rex/memory/utils.h).
// Linux and Android; before the runtime is set up.
void Install();

// The shared buffer for the renderer, once the guest's memory is built on it:
// a VkBuffer over the whole physical memory with its VkDeviceMemory and memory
// type. False if the guest's memory is not in a shared buffer.
struct Guest {
  void* buffer = nullptr;  // VkBuffer
  void* memory = nullptr;  // VkDeviceMemory
  uint32_t type = 0;
};
bool RendererBuffer(Guest* out);

}  // namespace ng2native::shared_memory
