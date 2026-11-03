// [native renderer] The native renderer's entry points (native_renderer.cpp).
#pragma once

#include <cstdint>
#include <functional>

#include <rex/graphics/texture_load_hook.h>

namespace ng2native {

// The game called D3DDevice_Swap(device, front buffer texture) (from the
// Direct3D call tracer's hook, on the game's render thread). With
// native_renderer on: finish and present the native frame.
void OnGuestSwap(uint32_t device, uint32_t front_texture);

// Direct3D kicks commands off to the GPU (0x8373A3D0, the segment flush): the
// native work recorded so far is submitted, so everything the command
// processor will see has its native work on the GPU already (GPU sync).
void OnGuestKickoff();

// [test] Save the next native frame as native_frames/<name>.rgba (the host
// test's NG2_CAPTURE, next to the emulated renderer's capture). False when
// the native renderer is not drawing.
bool RequestNamedDump(const char* name);

// The overlay's "Capture frame report": log the next `frames` native frames
// whole (every draw, resolve and render pass, saved to native_resolves/) and
// save each one shown (native_frames/capture_<frame>.rgba) - what the player
// sees, for the developer. False when the native renderer is not drawing.
bool RequestCapture(uint32_t frames);

// The title's look at each texture whose bytes are new or changed, before
// it is decoded (rex/graphics/texture_load_hook.h's contract: bytes the hook
// writes into guest memory are what the texture is made from) - the PS5
// port's PlayStation button prompts and HUD picture dump. Any thread.
void SetTextureLoadHook(rex::graphics::GuestTextureLoadHook hook);

// The guest-visible side of a Direct3D call - a fence, an interrupt, a wait,
// the swap (izuna_gpu.cpp) - run in call order on the graphics system's effect
// thread (PumpSideEffects), once the native GPU work recorded before the call
// has finished there, or after 30 ms (the game may wait on a fence in a frame
// it has not kicked off yet). Called on the game's render thread, at the call.
void RunAfterRecordedWork(std::function<void()> effect);

// Called (on the renderer's completion thread) whenever native GPU work has
// finished: side effects waiting for it may be due now (izuna_gpu.cpp wakes
// its effect thread). One callback; null removes it.
void SetGpuProgressCallback(void (*callback)());

// [test] native_watch_tex: whether a draw has used that texture (slot 0's
// guest address) yet - the host test's pad script waits for it ("untiltex").
bool WatchedTextureSeen();

}  // namespace ng2native
