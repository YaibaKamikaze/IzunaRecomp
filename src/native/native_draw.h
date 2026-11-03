// [native renderer, M3] Drawing the game's Direct3D calls natively
// (native_draw.cpp). native_renderer.cpp owns the device setup and the frames
// shown; this draws into them.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <mutex>

#include <rex/ui/vulkan/api.h>

namespace ng2native {

struct DrawContext {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
  PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
  uint64_t guest_address = 0;  // device address of the imported guest physical memory
  bool graphics_pipeline_library = false;  // VK_EXT_graphics_pipeline_library enabled on the device
  bool dual_src_blend = false;             // [IzunaRecomp] dualSrcBlend enabled (alpha to mask on 2x MSAA)
  // The command buffer being recorded (begun on first use: a frame is a chain
  // of segments, one per Direct3D kickoff), its frame's index (0/1: which half
  // of the per-frame upload ring), and whether it starts a new frame.
  VkCommandBuffer (*frame_commands)(uint32_t* frame_index, bool* new_frame) = nullptr;
};

// After the guest memory import. False: the native path stays off (the test
// pattern shows instead).
bool DrawInit(const DrawContext& context);

// From the Direct3D hooks (game render thread).
void OnShaderCreateEnter(uint32_t container);  // D3D_CreateShaderFromContainer: r3
void OnShaderCreated(uint32_t object);         // ... its object, complete (r31 at 0x8373B4CC)
void OnDrawIndexed(uint32_t device, uint32_t primitive, uint32_t base_vertex, uint32_t start_index,
                   uint32_t index_count);  // 0x8373CB08
void OnDraw(uint32_t device, uint32_t primitive, uint32_t start, uint32_t count);      // 0x8373C720
// D3DDevice_BeginVertices: `count` vertices the game writes at `data` (guest,
// `dwords` long) after the call - drawn at the next Direct3D event.
void OnBeginVertices(uint32_t device, uint32_t primitive, uint32_t count, uint32_t data, uint32_t dwords);
// D3DDevice_BeginIndexedVertices: the same with indices (`index_data`,
// `index_dwords` long); `initiator`: the draw's VGT_DRAW_INITIATOR (primitive,
// 32-bit indices 0x800, index count << 16), `index_word`: endian in bits 30-31,
// `index_offset`: added to every index (the call writes it to VGT_INDX_OFFSET).
void OnBeginIndexedVertices(uint32_t device, uint32_t data, uint32_t dwords, uint32_t index_data,
                            uint32_t index_dwords, uint32_t initiator, uint32_t index_word, uint32_t index_offset);
void OnResolve(uint32_t device, uint32_t flags, uint32_t rect, uint32_t clear_color, double clear_z);
// D3DDevice_Clear, per rect: its arguments as it calls the function writing
// the clear (0x83738A38), then (OnClearEnd) once that returned - the cleared
// targets are in the register shadow by then.
void OnClearBegin(uint32_t device, uint32_t flags, uint32_t rect, uint32_t color, double z, uint32_t stencil);
void OnClearEnd();
// Direct3D's window scissor (PA_SC_WINDOW_SCISSOR_TL / _BR: x in bits 0-14, y
// in 16-30), which it writes straight into its command buffer, not the device
// shadow: SetScissorRect (0x83734530) and the clear's reset (0x837384D0;
// config/hooks/native.toml). Each draw is clipped to it and the screen scissor.
void OnWindowScissor(uint32_t tl, uint32_t br);

// [IzunaRecomp] What mods change in the drawing (native_mods.cpp).
// Draws whose pixel or vertex shader has this hash (the frame capture's draw
// log names them) are left out, or drawn again.
void HideShader(uint64_t hash, bool hidden);
// The mods' textures: a game texture whose id (TextureId: its size, format
// and the hash of its bytes) has a picture here is drawn with that picture
// instead, at the picture's own size.
void SetTextureReplacements(std::unordered_map<std::string, std::filesystem::path> by_id);
// Saves each texture the game loads, raw, named by its id
// (tools/native/tex2png.py --ids turns them into PNGs to edit).
void SetTextureDump(bool on, const std::filesystem::path& dir);
// Any Direct3D call (before it runs): draws a pending BeginVertices draw.
void OnD3DCall();

// At D3DDevice_Swap, if this frame recorded native work: finish it, copying
// the front buffer (the texture the game presents) into `target` (RGBA8,
// width x height; `target_used`: it holds an earlier frame, in
// SHADER_READ_ONLY_OPTIMAL). True: `cmd` is ready to end and submit.
bool DrawEndFrame(uint32_t device, uint32_t front_texture, VkImage target, bool target_used, uint32_t width,
                  uint32_t height);

// Time the render thread spent waiting for an earlier frame's GPU work.
void NoteFenceWait(uint64_t ns);

// Direct3D kicked commands off: end the segment being recorded (the
// renderer submits it); the next draw begins a new one.
void DrawEndSegment();


// One line of counts for the periodic log (draws drawn / skipped and why).
void DrawReport(uint64_t frame);

// Threaded recording (native_threaded): the game's render thread records each
// Direct3D event, a worker turns the records into Vulkan commands.
void StartDrawWorker();  // (after DrawInit)
bool DrawThreaded();
uint64_t ProducerNanoseconds();  // the game thread's recording time since the last call
// The kickoff and Swap as records (the game thread); the worker then calls the
// renderer's RendererKickoff / RendererSwap and RecordDone.
void QueueKickoff();
void QueueSwap(uint32_t device, uint32_t front_texture);
// The record sequence of the last kickoff or swap queued: GPU sync waits until
// the native work of every record up to it has finished on the GPU.
uint64_t LastKickoffRecord();
// The sequence the next record will have (the game thread's view): a side
// effect of a Direct3D call is due once a kickoff or swap record from it on
// has finished on the GPU.
uint64_t NextRecordSeq();

// Implemented by native_renderer.cpp, called on the worker.
void RendererKickoff();
void RendererSwap(uint32_t device, uint32_t front_texture);
// Records up to `seq` are recorded and submitted (or part of the open segment
// a later kickoff submits): their GPU work is done once what was submitted by
// now is (GPU sync).
void RecordDone(uint64_t seq);

// [diag] The frame being drawn is a logged one (native_log_frame, _on_ps,
// _on_tex, auto log): the Direct3D tracer lists every call too.
bool LoggingFrame();
// The resolution scale the native renderer draws at (1, 2 or 3: 720p, 1440p,
// 4K of the game's 1280x720): native_resolution_scale at the first call.
float RenderScale();  // [IzunaRecomp] 1, 1.5 (1080p), 2 or 3

}  // namespace ng2native
