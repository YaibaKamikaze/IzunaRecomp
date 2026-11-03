// [IzunaRecomp] The native graphics system: what the game's Direct3D asks of
// a GPU besides drawing - fences, CPU callbacks, the flip and its vsync wait,
// the gamma ramp, the swap - done at the Direct3D calls that ask for it, in
// their order, as the native GPU work before each call finishes; the vblank
// interrupts; the presenter and the Vulkan device (the SDK's UI layer: the
// Kasumi renderer draws on it). No GPU command stream is read and nothing is
// emulated: the SDK is built without its GPU plugin (REXGLUE_GPU_EMULATION
// off). izuna_gpu.cpp lists each call and what it does.
#pragma once

#include <cstdint>
#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace ng2native {

// The graphics system for RuntimeConfig::graphics (Ng2App::OnPreSetup).
std::unique_ptr<rex::system::IGraphicsSystem> CreateIzunaGraphicsSystem();

// The Direct3D calls it implements (hooks: d3d_trace.cpp, config/hooks/
// native.toml), on the game's render thread at the call.
void OnInsertFence(uint32_t device);                                   // 8373A088
void OnCallbackHandshake(uint32_t device, uint32_t flags, uint32_t callback,
                         uint32_t argument);                           // 8373A5A8
void OnFlipBegin(uint32_t device, uint32_t no_wait);                   // 83744D90 at entry
void OnFlipWait(uint32_t device, uint32_t no_wait);                    // 83744D90 after the swap callback
void OnGammaRamp(uint32_t ramp);                                       // 8373F928 (r4: the three tables)
void OnGpuTimestamp(uint32_t device, uint32_t second);                 // 83749AE0

}  // namespace ng2native
