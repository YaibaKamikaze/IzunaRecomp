# The Kasumi Renderer: IzunaRecomp's native renderer for Ninja Gaiden II (PS5)

**Status (2026-10-26):** the game is drawn natively through Vulkan by the Kasumi
Renderer, on IzunaRecomp's own graphics system - no GPU emulation is built or
linked (test47). The sections below are the history, oldest first.

Status: **M3 - the game is playable on the PS5 with the native renderer (test13, owner); graphical bugs remain (missing effects, flicker, parts of models); performance work under way: threaded recording, pipelines linked from per-shader parts, smaller shaders (test17-21)** (2026-10-24). Still hybrid: the emulator's command processor parses the ring for sync (fences, interrupts, the swap) with its drawing off. Track: `~/ng2-ps5-native` only (see CLAUDE.md).

## Goal

Draw the game directly through Vulkan (RADV on the PS5) instead of emulating
the Xbox 360 GPU, as Unleashed Recompiled, Edge of Time Recompiled and Skate 3
Recomp do. Today every frame goes through the Xenos emulator: the game's
Direct3D library writes GPU command packets into a ring buffer, and the GPU
command thread decodes them one register at a time, emulates EDRAM render
targets (FSI), resolves, untiles textures and translates shaders at runtime,
while memory watches and a per-frame refresh keep its copy of guest memory in
sync. That thread is the frame-rate limit in heavy scenes. A native renderer
removes all of it: the game's Direct3D calls become Vulkan calls.

The emulated renderer stays, selectable, as the fallback (Skate 3 does the
same: Settings > Video > Renderer, Native or Emulated).

## Which approach

| Project | How | Portable to NG2? |
|---|---|---|
| Unleashed Recompiled, Edge of Time Recompiled | Replace the game's statically linked **Xbox 360 Direct3D library** (D3DDevice_SetTexture, DrawIndexedVertices, Swap, ...) with native implementations; shaders translated ahead of time by **XenosRecomp** | **Yes** - every XDK game links the same library. The game's own code is untouched. |
| Skate 3 Recomp | Read the game's own scene data (entities, materials) from guest memory and redraw it with hand-written ports of its shaders | No - specific to Skate 3's engine; NG2's would have to be reverse-engineered from nothing |

So: the **Unleashed / Edge of Time approach**, reusing Edge of Time's renderer
(BSD-3) where it fits.

## What we can reuse (and under what terms)

| Source | Licence | Use |
|---|---|---|
| KongaTime/EdgeOfTimeRecomp `src/gpu` (~15k lines: device, draw, textures, surfaces, resolve, pipeline cache, present, hooks) | BSD-3 | Reuse with credit; adapt to NG2's Direct3D layout |
| zolaware/plume (render abstraction, Vulkan + D3D12) | check before import (Unleashed's, MIT) | The Vulkan backend; already shown on a PS5 (KongaTime PS5_EdgeOfTimeTitle test app 5: 4K swapchain on RADV) |
| hedge-dev/XenosRecomp, goliathret/reeot-XenosRecomp | MIT | Shader translation; we keep our own fork |
| rexglue/rexglue-sdk upstream (IGraphicsSystem: the Xenos emulator is a plugin, a game can supply its own graphics system) | BSD-style | The seam where the native renderer plugs in |
| Nazky/UnleashedRecomp-Prospero | **GPL-3** | Read for design only; copying would make those parts GPL |
| mchughalex/skate3recomp, elripalda/skate3-ps5 | **no licence** | Read only, never copy |

## Milestones

**M0 - map NG2's Direct3D library.** The game has no symbols. Anchors found so
far (build 16.19's generated code):

- `sub_837452C0` calls `VdSwap`, `VdPersistDisplay`, `VdGetCurrentDisplayInformation`: D3DDevice_Swap (or its present path).
- `sub_8373A918` calls `VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack`, `VdSetSystemCommandBufferGpuIdentifierAddress`: the device's GPU initialisation.
- `sub_83741F00`, `sub_83742128`: `VdSetGraphicsInterruptCallback`.
- Every function that writes GPU command packets or calls a GPU kernel export lies in **0x83734000-0x83750000 (~550 functions)**; 253 of them in the first 16 KB, the pattern of the per-state SetRenderState/GetRenderState stubs Edge of Time's list (217 named Direct3D functions) shows.

Findings so far:

- NG2 was built with **XDK 2.0.6683** (xboxkrnl/xam import version, mid-2008).
  Edge of Time (2011) used a much later XDK, so its D3DDevice offsets will not
  match; Sonic Unleashed (late 2008) is far closer - its layout (from
  UnleashedRecomp, GPL: facts only, no code) is the better guide for offsets.
- `tools/native/d3d_map.py` profiles the block -> `docs/native/d3d_map.txt`:
  **491 functions, 101 called from outside the block** (the game's Direct3D
  entry points - the list to name and replace).
- Recognised so far: `8373BD50` (49 call sites) builds DRAW_INDX packets: the
  draw; `8373D570` writes shader constants and WAIT_REG_MEM: the constant /
  command-buffer path; `837341F0` (67 call sites) works device fields
  10908/10912/13508 (state-dirty bookkeeping shared by setters);
  `837452C0` keeps the device in r31 and counts at +16544 every call: Swap.
- The device pointer arrives in r3; Swap reads +48, +10896, +13592, +14828,
  +21600..21608.
- **Most render and sampler states are not calls.** XDK SetRenderState_* /
  SetSamplerState_* are mostly header inlines: the game writes the device's
  copy of the GPU registers and sets a dirty bit. A native renderer must read
  that copy at draw time (as Edge of Time's state.cpp does). The copy and its
  flush are mapped (config/d3d_functions.toml [device_shadow]): dirty masks at
  device+0..+32, the draw flushes them via 0x83743548 (16 registers per bit:
  shader float constants 0x4000/0x4400 at +1920/+6016), 0x837431A8 (one per
  register: render states 0x2000-0x23FF at +10368..+10832, bool/loop
  constants 0x4900 at +10112), 0x837433E8 (fetch constants 0x4800, 6 dwords
  per slot, 32 slots; indexed with 1152 - Edge of Time's kDevConstantArea is
  also 1152, so some of its offsets may hold: verify each), 0x83743268
  (control registers near 0x2388).
- **Diagnostic build native-m0-trace1** (title PPSA94203, "Ninja Gaiden II
  (native dev)", installs beside the main app and uses its Game Data): a hook
  on each of the 101 public functions (tools/native/gen_d3d_trace.py ->
  config/hooks/d3d_trace.toml, src/native/d3d_trace.cpp, cvar
  native_d3d_trace): logs each function's first two calls with r3-r8 and 8
  words behind each pointer argument, and call counts every 600 swaps.

Method: classify each function in that block by the GPU registers and packet
opcodes it writes (a SetTexture writes texture fetch constants, a draw writes
DRAW_INDX packets, a SetRenderState_X one register), the device-structure
offsets it touches (the device pointer is reachable from Swap), and who calls
it; confirm on the console with a diagnostic build that logs calls to the
candidates. Output: `config/d3d_functions.toml`, the name of every Direct3D
function NG2 uses, and the layout of its D3DDevice and resource structures.

**M1 findings (2026-10-18).** NG2's shaders are **in the executable**: the
decompressed XEX image holds 663 Xbox 360 shader containers (magic
0x102A11xx) - 625 unique, 411 vertex and 214 pixel; none in the .ng2
archives. Translation tool: Edge of Time's XenosRecomp fork
(goliathret/reeot-XenosRecomp, MIT), cloned to
`~/ng2-ps5-deps/tools/reeot-XenosRecomp`, our changes on its branch `ng2`
(it bundles DXC for Linux). It crashed at once on NG2: **83% of the vertex
shader containers (343/411) carry no vertex-element table and 70 no
interpolator table** - Unleashed's and Edge of Time's were built with them,
NG2's (XDK 2.0.6683) mostly not. Two consequences, now design decisions:

- **Interpolators link positionally** (vertex export register i -> location
  i, pixel input register i <- location i), as the Xenos hardware and Xenia
  do. Checked: of the shaders the GPU actually ran in the owner's logs, 65/75
  vertex and 86/87 pixel shaders hash (XXH3 of the microcode, the emulator's
  ucode_data_hash) identical to a container - Direct3D does not rewrite NG2's
  shaders at link time, so the containers' microcode is what runs and
  positional linkage is exact. The few unmatched are likely Direct3D's own
  internal shaders (clear, resolve) - to identify.
- **Vertex fetch reads raw memory in the shader**: the vfetch instruction
  carries format, offset, stride, signedness and exponent; the shader reads
  the vertex buffer through the fetch constant (slot from constIndex /
  constIndexSelect) and decodes it (Xenia's approach) instead of relying on
  D3D input-layout semantics the containers lack.
- **Memory design**: on the PS5 the guest's physical memory is ordinary memory
  the GPU can read. Exposed to Vulkan as one buffer (e.g.
  VK_EXT_external_memory_host, to verify on the console's RADV), vertex,
  index and texture data are read in place - no copies, no write watches, no
  per-frame refresh/hash/mprotect (the emulator's biggest costs today).
  **Verified on the console (native-m2-test5/6, 2026-10-18):** the extension
  is there (16 KB alignment); all 512 MB import as one buffer (memory type 5)
  whose device address equals the host address 0x1100000000; GPU copies of
  pages holding data match the CPU's view byte for byte. Open problem: test6 and test7
  were killed outright (at ~frame 3000 and ~5 s), with no unhandled-fault
  line, so not a plain host segfault. Suspects: the emulator still decommits
  (mprotect PROT_NONE + MADV_DONTNEED) and write-protects pages inside the
  imported range, which Vulkan host-pointer import does not allow; or the GPU
  copy itself. test8 showed the app dies inside the GPU copy. test9 freezes
  the physical mapping read/write before importing (ps5diag::frozen_lo/hi).
  If it lasts, the rule for the native renderer: nothing may mprotect the
  imported range. If the import is
  the cause: once the emulator's GPU side is gone it stops touching those
  pages, or we import committed ranges only.

**M1 result (2026-10-18): all 625 unique shaders translate and compile to
SPIR-V** (`tools/native/build_shaders.sh` -> out/native-shaders/
shader_cache.cpp, 12.4 MB, ~11 s). Fork changes (branch `ng2`, commits
1b1f5e1, 80deebc; NG2_RECOMP): positional interpolators; raw vertex fetch
(ng2VertexFetch: every vertex format, endian modes, signed / normalised /
integer, both signed-normalisation modes, exponent adjust; mini fetches reuse
the last full fetch); containers without a constant table; unnamed and
relatively-indexed constants read by register; **memory export** - 63 vertex
shaders write guest memory (eA/eM0-eM4, ng2MemExport: 4/8/16-byte formats;
1-2-byte elements not yet); 32 texture fetch slots (descriptor indices at
SharedConstants + 1024); vertex shaders sample at LOD 0; no Edge of Time
TAA/trimming variants. Push-constant contract adds GuestMemory and
FetchConstants (buffer device addresses). Not yet validated on screen - that
needs M2-M3.

**M1 - NG2's shaders.** Find where the game keeps its compiled shaders (inside
its .ng2 archives or the XEX), extract them at build time (they are game data:
generated, never committed, like the recompiled code), translate with our
XenosRecomp fork, compile HLSL -> SPIR-V with DXC (to install on the build
machine). Measure how many translate cleanly.

**Architecture (decided 2026-10-18): the SDK's app-renderer seam, not a
second device.** SVR 2011's runtime (our SDK base) already has
`rex/external_frame.h`: an app renderer gets the emulator's own Vulkan device
and queue (GetVulkanDevice; submit holding queue_mutex), provides each
finished frame as an RGBA8 image the presenter shows instead of the emulated
front buffer (SetProvider - hub, overlay, pacing untouched), and can switch
the emulated GPU's drawing off while it keeps running the game's command
stream - fences, interrupts and memory-export draws still run
(SetHostDrawingDisabled). So the native renderer runs **hybrid**: the
emulator keeps the game's GPU synchronisation and GPU-computed data alive
cheaply, the native renderer draws. Raw Vulkan on that device (the shaders
are SPIR-V already); plume stays an option for reusing Edge of Time code.
The native track's SDK enables bufferDeviceAddress and shaderInt64 for app
renderers (the shader contract needs them).

**SVR 2011 has a native renderer of its own** (elripalda/svr2011-ps5,
`port/src/native/`, docs `port/docs/native_renderer_phase{1,2}.md`; **no
licence: design and facts only, no code**). Its D3D device register mirror is
**identical to NG2's** (fetch constants +0x480, VS/PS float constants
+0x780/+0x1780, booleans +0x2780, registers 0x2000-0x2325 at
+0x2880..+0x29B8) - independent confirmation of our map - and it documents
the resource objects (24-byte header, Common & 0xF = type: 1 vertex buffer
(+0x18 fetch constant), 2 index buffer (+0x18 address, +0x1C size), 3 texture
(+0x1C fetch constant), 4 surface, 5 vertex declaration, 6/7 vertex/pixel
shader objects embedding the container header) and the current shaders at
**device+0x3244 (pixel) / +0x3248 (vertex)** - to verify on NG2.

**M2 skeleton (native-m2-test1):** `src/native/native_renderer.cpp` - with
Developer > Renderer (dev) = Native test (cvar native_renderer, restart): at
the first guest swap it takes the device from external_frame, makes two
1280x720 RGBA8 images, registers the provider, turns emulated drawing off;
every D3DDevice_Swap clears the next image to a slowly cycling colour and
submits it on the shared queue. Proves the path on the console.

**M2 - plume on the PS5 in this app.** Bring plume into the build beside the
Xenos emulator; a native device that can clear and present, switched by a hub
setting (Renderer: Emulated / Native, default Emulated).

**M3 - first native frames.** Hook Swap, Clear, SetRenderTarget, the draws,
shaders, constants, vertex/index buffers and textures (Edge of Time's
`gpu/hooks` as the model); get the title screen and menus drawn natively.

**M4 - gameplay.** Render targets and resolves (NG2's post-processing chain),
depth, MSAA, every texture format, the HDR/tone-map path; then parity passes
chapter by chapter, with the emulator as the reference.

**M5 - default.** When a full playthrough matches, make Native the default and
keep Emulated as the fallback.

Audio emulation is a separate, later question: XMA decoding is not the frame
limit, and the cutscene audio fix covers the visible symptom.

## M3: first native frames (2026-10-18)

`src/native/native_draw.cpp` draws the game's Direct3D calls with Vulkan on
the emulator's device. Result on the VM (desktop test build, lavapipe): the
**title screen renders correctly** with the game's own shaders and textures.
First console build: `native-m3-test1`.

What the game's Direct3D (XDK 2.0.6683) turned out to do - each of these cost
a wrong frame before it was understood:

- **Two shader creators.** Pixel shaders: `0x8373B3C8` (object complete at
  `0x8373B4CC`, r31). Vertex shaders: `0x8373B790` (object at `0x8373B868`,
  r30; it keeps the container header at +872). Both take the container
  (word 1 header size, word 2 microcode size); the cache key is XXH3 of the
  whole container, hashed at entry, linked to the object at the end.
- **Vertex declarations are patched into the microcode.** Shaders built
  against a declaration have stride/offset/format zero in their vfetch
  instructions; Direct3D fills them in when a declaration is bound. So every
  vfetch reads its fields at draw time: the fork gives fetch k slot k
  (`ng2VertexFetchK`), the cache's layout records hold each fetch's
  instruction index, and the renderer decodes the instruction from the
  object's microcode (object + 32, + `vfetchCodeOffset`, + 12 x index) into
  SharedConstants + 1664.
- **No Vulkan vertex or index buffers.** r0.x is loaded by
  `ng2VertexIndex(SV_VertexID)` from SharedConstants + 960 (index address,
  16/32-bit, endian, index offset, quad-list expansion), as the hardware does.
- **`0x8373BD50` is BeginVertices** (device, prim, count, stride): it reserves
  vertex space in the command buffer, points fetch slot 95 at it with a
  register write (not the shadow), and the game writes the vertices after it
  returns. The hook at `0x8373C018` records the block; it is drawn at the next
  Direct3D call **on the render thread** (loader threads call Direct3D too),
  from a copy in the upload ring.
- **`0x8373CB08` is the indexed draw** (always a triangle strip; index buffer
  object at device+12428; base vertex from VGT_INDX_OFFSET).
- **Resolves** set RB_COPY_* in the shadow; hooked right after
  (`0x8373D918`; r25 flags 0x100/0x200 clear colour/depth, r26 rect, r21
  clear colour, f30 clear Z). GPU addresses in registers and fetch constants
  are only masked (no +0x1000 for the 0xE0000000 view - that is for CPU
  pointers such as the index buffer's).
- **Shader cache blobs are smol-v**, then zstd over the whole cache.
- **`-fvk-invert-y`**: the vertex shaders negate y at the end; the NDC
  scale/offset constants (from PA_CL_VTE_CNTL / VPORT, 912/928) pre-flip it.
- Fork fixes found by rendering: literal constants (`def`) were read from the
  constant buffer instead of the shader's locals; pixel constants were
  clamped to 223; the quad-list triangle order.

Textures: 2D base levels decoded on the CPU (untile + endian XOR + BCn as
is), cached by fetch constant, re-uploaded when their (sampled) hash changes;
the fetch swizzle is the view's component mapping. Samplers from the clamp
and filter fields. Resolved images take priority by address.

Not done yet (next): rect lists (clears / some full-screen passes), depth
resolves, culling (off), EDRAM aliasing (one image per base+format; the
title's float target and 8-bit target at the same base are separate), mips,
3D / cube textures, alpha test (spec constant), MSAA, gamma formats,
render-target heights (fixed 1024), texture invalidation by writes.

### M3 tests on the console (2026-10-18)

- test1: title screen and the main menu drawn natively, 50-56 fps; stalls
  of 100-600 ms per new pipeline (now kept in UserData/native_pipelines.bin).
- test2: title complete (rect lists via the geometry shader; palettes
  sampled as 1D get a 1D image); the menus, difficulty select and the
  chapter 1 loading screen all draw - then a crash in game code
  (sub_83823B30, a heap function, reading 0x300D7FF0) as gameplay began.
  Not resolves (the emulated build never reads them back either:
  readback_resolve=none). Most likely our own **memexport**: the emulator
  still runs the memexport draws, and our translated shaders wrote the same
  exports again (through our own, unverified decode) - so test3 turns native
  memexport off (ng2MemExport returns unless SharedConstants + 976 is set).
  **Rule: while the emulator runs the memexport draws, the native renderer
  must not write guest memory.**
- `native_keep_emulated_drawing` (default off): keeps the emulated GPU
  drawing too, unseen - a fallback to tell native-mode side effects apart.
- The VM can't get past the title yet: the scripted pad's presses are
  produced but the game doesn't act on them there (cause not found); menus
  and gameplay are checked on the console.

### M3 continued: tests 4-8 (2026-10-18)

Console findings:
- test4: no crash; reached gameplay (~400 draws a frame), then froze inside
  vkCreateGraphicsPipelines for a 135 KB vertex shader (>30 s on the
  console's compiler). -> pipelines are built on two worker threads; a draw
  whose pipeline is not ready is skipped (test5).
- test5 timing: the native hooks cost 0.4-4.7 ms a frame and never wait for
  the GPU, yet gameplay ran at 3.6 fps (~276 ms a frame) with the emulator's
  command thread idle too - a timeout. The emulated GPU's occlusion queries
  count *its* draws, which are off: every query read "hidden" and was waited
  out. -> in native mode `occlusion_query_enable=false` (ng2_tuning.h): the
  queries answer at once with the fallback count, until the native renderer
  runs them itself.
- The heap crash at the menu video (0x300D7FF0) recurred in test2/3 but not
  in tests 4/5; the heap watch (xmemory.cpp, PS5 only) stays in to catch it.
- A run that ended with the app closed and no log line: suspected GPU page
  fault -> vertex fetch addresses are now masked to the 512 MB.

Added since (tests 6-8), each from the hardware's rules (the SDK's
Xenia-derived code as the reference):
- depth resolves (shadow maps): depth image -> scratch buffer -> R32 float
  texture under the destination address;
- resolves convert to RB_COPY_DEST_INFO's format (numbered as texture
  formats): a float target into 8_8_8_8 is clamped; 8_8_8_8 <-> _GAMMA copy
  bits; the R/B swap bit and the fetch swizzle cancel (neither applied);
- strip restart (PA_SU_SC_MODE_CNTL.multi_prim_ib_ena,
  VGT_MULTI_PRIM_IB_RESET_INDX): the restart vertex gets a NaN position;
- culling (face 0 = CCW front, as the SDK; none for rect lists; reset
  strips culled since test38, as lists - see test38),
  depth bias (GetPreferredFacePolygonOffset rules), alpha test (the shaders'
  SPEC_CONSTANT_ALPHA_TEST for GREATER / GEQUAL);
- gamma: 8_8_8_8_GAMMA targets are sRGB images; textures with gamma signs
  use sRGB formats; resolved 8_8_8_8 images have an sRGB view too;
- texture mips (2D, the GetGuestTextureLayout rules: next_pow2 >> L pitch,
  4 KB-aligned levels from mip_address, packed tail) and cube textures (six
  faces per level) with their own descriptor table;
- GPU memory: images are destroyed 3 frames after release; textures unbound
  for ~20 s are dropped; descriptor slots are recycled the same way;
- per-frame timing and image memory in the 10 s report.
- tools: framediff.py compares a frame dump with a reference
  (~/ng2-ps5-deps/hosttest/reference/, game-derived: outside the repo).

Still on the way to "no GPU emulation": the emulator's command processor
still reads the ring (fences, interrupts, memexport draws, the occlusion
fallback). Removing it means answering the game's GPU synchronisation
natively (fences / interrupts / swap) and running memexport ourselves.

### GPU sync (test10): fences follow the native GPU

Before: the emulator's command processor, with its drawing off, ran through
the command ring as fast as it arrived and wrote the guest's fences / raised
its interrupts long before the native frame was even submitted (at the swap).
The game then reused vertex, index and texture memory that the zero-copy
native draws still read - wrong geometry, flicker.

Now (native_gpu_sync, on):
- The command processor's guest-visible side effects - EVENT_WRITE_SHD
  (NG2's fences: opcode 0x58, two per fence, from 0x8373A088), MEM_WRITE,
  REG_TO_MEM, COND_WRITE, EVENT_WRITE_EXT extents, the occlusion fallback,
  INTERRUPT - go through rex::external_frame::RunSideEffect (SDK) into a
  queue, and run in order on the command processor's own thread (interrupts
  call into the guest and need its context) from PumpSideEffects - called
  between batches, while idle, and inside WAIT_REG_MEM (which may wait for
  a write still queued).
- A native frame is a chain of command buffer segments (64 in a pool,
  segment seq uses pool[seq % 64]); Direct3D's segment flush (0x8373A3D0,
  44 callers, every kickoff) submits the native work recorded so far.
  Whatever the command processor sees was kicked off, so its native work is
  already submitted: each side effect waits for the segments submitted when
  it came (exact, no guess), plus a 30 ms fallback (never a deadlock).
- First attempts that failed: running the effects on a host thread (the
  interrupt needs a guest thread: hang); blocking the command processor
  until the frame ends (circular: the ring fills, the render thread can't
  reach its swap); tagging with the frame being recorded (every effect waited
  out the fallback).
- On lavapipe the title runs at ~5 fps with it (the game now really waits for
  a slow software GPU); on the console the waits should be short.

### Developer tooling (test11)

- Hub Developer rows (ps5_native_* settings -> native_* cvars): GPU sync,
  memory exports emulated/native (native: SharedConstants + 976 = 1 and
  rex::external_frame::SetMemexportDrawsDisabled - the emulated GPU skips
  every draw), keep emulated drawing, save frames (every 10 s / 1 s, 20 kept),
  log draws (native_auto_log), save textures (native_dump_textures: base
  level as uploaded, <address>_vk<format>_<w>x<h>_f<xenos format>.bin ->
  tools/native/tex2png.py, BC1-5 included), ignore pipeline cache.
- Native memexport now follows the SDK (Endian128 8-in-64 / 8-in-128 dword
  reordering, the _AS_16_16_16_16 aliases); still unvalidated on gameplay.

### GPU timing and texture signs (test12)

- GPU timestamps: a 4-query pool around each frame's native commands; the
  600-frame Step line adds "GPU span X ms a frame (first native command to
  last)" when the device supports timestamps (lavapipe: ~28 ms at the title).
- Texture signs (TextureSign per channel, fc[0] bits 2-9), as the XenosRecomp
  shaders expect (applyFetchSign, slots 0-15):
  - biased (2): SharedConstants + 392 (g_BiasedTextures) bit `slot` for rgb,
    + 388 bit 16 + slot for alpha - the shader does x*2-1;
  - gamma (3 on x/y/z): + 392 bit 16 + slot, decoded in the shader with the
    exact Xenos piecewise-linear curve; the image is then uploaded as UNORM
    (the cache key carries this; resolved targets get their UNORM view).
    Slots 16-31, which the shaders don't cover, still use the sRGB format /
    view (an approximation of the Xenos curve);
  - signed (1 on x): an SNORM format (R8, RG8, RGBA8, R16, RG16, RGBA16,
    BC4, BC5).
  The title frame is unchanged (framediff 5.6% as before: none there).

### The 3D scene draws (test13, 2026-10-18)

The VM now reaches gameplay: the title was waiting on a system message box
nobody could answer headless (`--headless=true` in hosttest.sh). That made
the chapter 1 opening cutscene (black on the VM, grey on the console)
reproducible, and three bugs were found with a new dump of every resolve of
a logged frame (`native_dump_resolves`, hub "Native: save render passes"):

1. **Clears were never seen.** D3DDevice_Clear (0x837390C8) clears each
   rect through 0x83739188, which writes the clear into the command buffer
   itself (0x83738A38): no Resolve, no draw call. Two midasm hooks
   (config/hooks/native.toml): at the call (0x83739294: flags, rect,
   colour, Z, stencil) and right after it (0x83739298), where the cleared
   targets are in the register shadow; the clear is a vkCmdClearAttachments
   over the rect. Flags: 0xF targets 0-3, **0x10 depth, 0x20 stencil** (the
   scene's GEQUAL depth is cleared with 0x1F, Z 0). Without them depth stayed
   at the initial 1.0 and every GEQUAL draw failed.
2. **EDRAM aliasing of the _AS_ formats.** The HDR scene is drawn as
   2_10_10_10_FLOAT (3) and resolved as 2_10_10_10_FLOAT_AS_16_16_16_16
   (12): same bits, two images. Target() maps 12 -> 3 and 10 -> 2 (Xenia's
   GetStorageColorFormat).
3. **Resolved images fetched at a smaller size.** A resolved image is the
   copy destination's size (pitch x height); a fetch of it may be smaller -
   the 1x1 exposure values live in 32x1 copies, the glow passes are 213x120
   in 224x120. Normalised coordinates must span the fetch's size, so such
   fetches bind a fetch-sized copy of the top-left (State::resolved_fit,
   recopied when the resolve's generation changes). Before, the composite
   read exposure 0 and showed only the glow.

After the three, the cutscene renders (cherry trees, moon, skyline, HDR
glow) on lavapipe. Also new: the auto draw log waits for a heavy frame in
which no draw was skipped for a pipeline still building (the first heavy
frame showed missing draws, not wrong ones); the draw log adds VS c16,
c43-48, c64-71 and the boolean constants.

Still open from this run: shadow-map resolves (R32F 128x128) read 0 - to
check against the emulated renderer; the pipeline cache saved nothing on the
VM ("0 KB loaded" each run - lavapipe keeps no pipeline cache). (A
suspected recompiler bug in the fog height path was a display filter of mine
hiding the `p0 = ...` line: the scalar setp ops are translated right.)

### test13 on the console, and test14 (2026-10-18)

test13 on the PS5: **the game is playable with the native renderer** (owner):
title, menus, the chapter 1 cutscenes and combat draw, mostly near 60 fps
once warm. Graphical bugs remain (to be listed with the owner's help), and
two measurable problems:

- **Pipelines built late.** The console's driver takes a median 283 ms, p90
  3.9 s, worst 10.7 s to build a pipeline (101 builds in the run); draws
  waiting for one are skipped - 30-55 thousand in some 600-frame windows of
  the cutscenes (frame 03's black sky). test14: every pipeline key queued is
  recorded (UserData/native_pipeline_keys.bin, versioned, saved with the
  pipeline cache) and at start all recorded ones are queued on the workers
  (`native_precompile_pipelines`, default on; `native_pipeline_threads`, 3);
  a draw waiting for one moves it to an urgent queue taken first. VM: the
  second run skipped 0 draws where the first skipped 10,328.
- **Render-thread time.** 4-9 ms a frame in the hooks, 1.7-3.5 ms of it
  textures - each bound texture's bytes were hashed every frame. test14:
  one that never changed is rechecked every 8 frames
  (`native_texture_recheck_frames`); one that changed once (video frames,
  CPU-written) every frame.

Also in test14: submission counters in the 600-frame line (submits a frame,
time in vkQueueSubmit). R-comp's PS5 driver reports (GPL-3, facts only:
`~/ng2-ps5-deps/refs/360-rcomp/gpu/vulkan/driver-reports/`) found
vkQueueSubmit blocking for milliseconds in sceAgcSuspendPoint while the GPU is
busy; on the VM we submit 1-1.3 times a frame - the console's figure decides
whether a submit thread is worth it. The GPU span read 0.29 ms a frame on the
console, which is implausible for these frames: the timestamp period is to
be checked before trusting it.

### test14 on the console, and test15 (2026-10-18)

The owner reports missing effects, several kinds of flickering and parts
missing from models. From test14's log and passes:

- vkQueueSubmit is not a cost here: ~2 submits a frame, 0.1 ms. GPU span
  ~6.7 ms a frame in gameplay. The render thread is the limit: 9.5-10 ms a
  frame in the hooks.
- The HDR scene target held **negative** colours and alpha outside 0-1. The
  game's 2_10_10_10_FLOAT holds unsigned 7e3 (0-31.875) and a 2-bit alpha;
  the 16-bit float stand-in kept whatever the shader wrote, and it spread
  through the glow and luminance chain. test15: SharedConstants + 396
  (g_PackedDec3) bits 24-27 mark the 2_10_10_10_FLOAT targets and the fork's
  NG2 epilogue clamps those outputs (XenosRecomp fork, uncommitted; the diff
  is in the test15 backup). Blending can still go past (Vulkan has no 7e3).
- Render-thread time, measured on the VM with new counters ("draw stages",
  "textures a frame", "image creation" lines every 600 frames):
  - texture decodes ~0.8 ms each: the untile copied byte by byte. Now whole
    16-byte runs (8/8/4/2/1 blocks for 1/2/4/8/16-byte blocks - the micro
    tile keeps x & 7 together within 16 bytes), word-wide endian swaps, into
    ordinary memory, then one copy into the write-combined upload ring.
    Checked equal to the old loop on 3000 random layouts; 2-6x faster
    (menus: 2.3 -> 0.15 ms a frame);
  - images created ~1.5 a frame: the game resolves the same address in
    alternating shapes (the shadow map R32F / R16G16, a glow pass 320x720
    RGBA8 / 320x180 RGBA16F), and each switch remade the image (empty).
    Other shapes are now kept aside and swapped back (State::
    resolved_variants): 898 -> 26 creations in 600 frames;
  - image memory comes from 64 MB blocks (sub-allocated; over 16 MB alone).
    On lavapipe that was not the cost - the image views are - the console's
    figures will say.
- Hub "Native: save frames" adds "Every second, keep all" (frames named by
  frame number, none deleted: ~13 GB an hour); saving no longer waits for
  the GPU or writes on the render thread.

### test15 on the console, and test16 (2026-10-24)

test15 (owner: memory exports Native - works; log draws every minute; one
run): the precompile built the 158 recorded pipelines at start, no draw was
skipped for a pipeline; texture decodes and image creation cost ~0 on the
console. The render thread still spends 9-10 ms a frame in the hooks:
texture slots 4.3 ms (1,700 lookups a frame), constants ~1 ms, vertex
fetches ~0.45 ms. Findings in the 154 saved frames (one a second, kept):

- **A green frame (6241)**: the test pattern, shown when a swap had no
  native work. Now the last frame stays on screen ("swaps kept the previous
  frame" in the 600-frame line). A flicker source.
- **Missing draws: D3DDevice_BeginIndexedVertices (0x8373C1F0)**, ~2 a
  frame, never drawn: like BeginVertices it reserves vertex and index space
  in the command buffer and the game fills it after the call. Hooked at the
  join 0x8373C6CC (config/hooks/native.toml) and drawn at the next Direct3D
  call with BeginVertices' path: vertices and indices in one upload block
  that stands in for guest memory (the index address is their offset).
  Signature from the code: r4 primitive, r5 min index, r6 vertex count, r7
  index count, r8 index format (0x4: 32-bit), r9 stride, r10 the index
  pointer's out address, the vertex pointer's at caller stack + 0x54.
- DrawIndexedVertices now passes its own primitive (r4) and base vertex
  (r5): the hook runs at the call's entry, before D3D writes VGT_INDX_OFFSET.
- A logged gameplay frame (8237) drew 42 draws after 876: its hooks got
  only 42. To find such gaps directly, a **draw census** (the method of the
  WWE All Stars recomp, BSD-3): the command processor - still parsing the
  ring for sync - counts every guest draw by primitive and EDRAM mode
  (rex::external_frame::CountGuestDraw), the renderer counts what its hooks
  received, and the 600-frame report prints both ("draw census").
- Render thread: each texture slot's result is kept for the rest of the
  frame by its fetch constant (any resolve invalidates them) - lookups
  halved on the VM; unchanged shader constants are no longer byte-swapped.
- **Shipped pipeline keys**: data/native/pipeline_keys.bin (163 keys from
  the owner's run; shader hashes and render state only, no game data) is
  built in (tools/native/pipeline_keys.py merge / inc) and queued with the
  player's own at start, so a first run has its pipelines too (the WWE All
  Stars recomp's ADR-0019 idea).

Next (planned): take the per-draw work off the game's render thread - it
only copies each draw's state into a stream, a worker turns the stream into
Vulkan commands (the SvR2008 PS5 port's threaded command recording, an idea
only: GPL-3). The render thread is the limit now, for frame rate and latency.

### test17: smaller shaders, less per-draw work (2026-10-24)

From Edge of Time Recompiled's renderer (BSD-3: it computes, per shader,
the float constant registers used and the texture fetch slots, and uploads /
binds only those): the XenosRecomp fork now records both for every shader in
the cache (ShaderCacheEntry::floatConstantRegs, textureSlotMask, from the
recompiler's own operand walk - relative reads count as the whole file,
literal constants c252-255 as nothing):

- constants: the median shader reads 81 registers (16 read all 256, 58
  none): the upload and byte swap per draw are ~3x smaller;
- texture slots: 1.06 per shader on average, against every slot holding a
  fetch constant (stale ones from earlier draws included) before.

Shader size: every vertex fetch inlined a switch of whole per-format
decodes; one table-driven decode now (the format sets kind, component count
and widths; one 16-byte read kept inside the 512 MB; uniform branches on the
kind). SPIR-V median 57 -> 30 KB, p90 213 -> 98 KB, largest 504 -> 209 KB;
DXC builds all shaders in 44 s instead of ~2 min; the startup precompile of
the 163 shipped pipelines on lavapipe 40-46 s -> 8.4 s, with the same frame
rate (a first version computing every decode and selecting one was 25% slower
on lavapipe). The console's 4.2 s pipeline (VS C0A01B99..., 287 KB) was one
of these.

The draw census also showed ~600 point-list draws a frame in cutscenes that
no hook receives: D3D's own no-ops (one point each, colour mask 0, no depth,
one shader, from a fixed buffer at 1EE39980) - counted apart now ("writing
nothing"); the rest balances.

### test18: threaded recording (2026-10-24)

The render thread was the limit (9-10 ms a frame in the hooks on the console
in gameplay, the game's own work on the same thread). With
`native_threaded` (hub "Native: threaded recording", **on by default**) the
game's render thread only records each Direct3D event, and a worker thread
does all the native work at the same time - the design of Edge of Time
Recompiled's render thread (BSD-3), in our own code (native_draw.cpp, the
"Threaded recording" section):

- A record holds the device's 64-byte blocks that changed since the record
  before, in the window the drawing reads (device + 1152 .. 12800: fetch
  constants, both constant files, the register shadow, the shader / surface
  pointers - a few hundred bytes a draw), plus what must be read at the call:
  the vertex fetches' fields Direct3D patches into the shader for the draw,
  BeginVertices / BeginIndexedVertices data, resolve and clear arguments.
  The worker applies the blocks to its mirror of the device and runs the same
  code as before against it (Draw, DrawUp, ResolveWork, ClearWork,
  RendererKickoff, RendererSwap). Records go through a 64 MB ring.
- GPU sync by record: a side effect waits until the native work of every
  record up to the last kickoff queued has finished on the GPU (the game
  thread queues the kickoff before the command processor can see what it
  kicked off). The worker leaves a ticket (record, segments submitted) at
  each kickoff / swap; the completion thread moves done_records on.
- The game runs at most one frame ahead of the worker (a wait at Swap).
- On the VM (lavapipe, ~820 draws a frame at the heaviest): the game's
  render thread spends 0.5-1.2 ms a frame recording; frames identical to
  unthreaded; the draw census balances; 7.5-minute run stable.

Also: GPU time per pass of logged frames ("GPU time of the logged frame":
timestamps at the frame's start, after each resolve and at its end; the ten
costliest passes with their draw counts) - where to spend shader work.

### test19: pipelines linked from per-shader parts (2026-10-24)

The console's driver is Mesa RADV 26.2 on NAVI21, which has
VK_EXT_graphics_pipeline_library; the SDK now enables it (with
VK_KHR_pipeline_library; vulkan_device.cpp, external_frame's
graphics_pipeline_library) and the renderer uses it when there
(`native_pipeline_libraries`, hub "Native: pipeline libraries", on by
default; else whole pipelines as before). The idea is the WWE All Stars
recomp's (ADR-0019's library path).

A pipeline is linked from four parts: vertex input (topology), the vertex
shader part (VS, alpha-test constant, the rectangle geometry shader), the
pixel shader part (PS, alpha-test constant, depth format) and the output
part (formats, blend, colour masks). Cull mode, winding, depth-bias enable
and all depth / stencil state are dynamic (core 1.3, set each draw), so the
shader parts depend on the shaders only: a render state never seen before
costs a link on the drawing thread (no draw skipped), and a
link-time-optimised pipeline is built in the background and replaces it.
Only a shader never seen before still compiles - once. The precompile now
builds the shader parts of the shipped / recorded keys (134 parts for the
163 keys). A part or link the driver refuses falls back to the whole
pipeline. On lavapipe: no draw ever waited for a pipeline; every linked
pipeline was optimised in the background; frames identical.

### test20: each swap shows its own frame (2026-10-24)

The command processor shows the latest native frame when it reaches the
guest's swap packet (vulkan/command_processor.cpp IssueSwap, from
rex::external_frame::GetFrame). Unthreaded, that frame is always finished
(FinishFrame runs in the game thread's Swap hook, before Direct3D writes the
packet); with threaded recording the worker may still be finishing it, and
the previous frame would show again - a frame of latency and a repeated
frame (judder). The SDK now calls rex::external_frame::SwapWait() before
IssueSwap; the renderer waits there until one more frame has finished than
at the previous swap (50 ms at most - counted in the 600-frame line as
"swaps waited 50 ms"), catching up if it fell behind. Unthreaded it never
waits. The wait blocks only the command processor's thread, which the worker
never waits on.

### test21: bursts of consecutive frames, for flicker (2026-10-24)

Hub "Native: save frames" → "Bursts (flicker)": a frame every 10 s as
before, plus every frame for 2 s once a minute (native_dump_burst 120,
native_dump_burst_every 3600), kept all and named by frame number - flicker
shows between consecutive frames, which one a second cannot catch. Frame
saving uses four readback buffers now (a frame whose buffers are all still
waiting on the GPU is not saved, and counted). Also: draws by MSAA sample
count in the census line - none multisampled in menus and cutscenes (the
game renders at 1x there).

## M4 plan: removing the command processor (research, 2026-10-24)

The renderer still runs the emulator's command processor (CP) with its
drawing off: it parses the guest's ring only for what the guest can see -
fences, the ring read pointer, interrupts, a few memory writes. Native means
none of it (owner: no GPU emulation). Edge of Time Recompiled never consumes
the ring: it stubs D3D's waits (BlockOnFence, IsFencePending -> 0,
WaitPrimaryRingSpace, BlockUntilIdle) and copies vertex data at draw time.
Ours reads guest memory in place on the GPU (zero-copy), so its fences must
stay truthful - they must follow our GPU work, not be skipped.

NG2's D3D, from the code (device + 10896 points to a status block the GPU
writes: + 4 the fence value - EVENT_WRITE_SHD - and + 60 the ring read
pointer; device + 10908 the fence counter; + 14884 the ring size mask):

| Address | What | From |
|---|---|---|
| 0x8373A088 | InsertFence: next value from + 10908, EVENT_WRITE_SHD to status + 4 | known (GPU sync) |
| 0x83739520 | the fence wait (BlockOnFence-like): spins until status + 4 reaches the fence | reads status + 4, loops on 0x83737AE0 |
| 0x83739B38 | the ring-space wait: spins on status + 60 against the ring size | reads status + 60, + 14884 |
| 0x83737AE0 | the shared spin helper (db16cyc pause, 5000-unit hang timeout) | called by both |
| 0x8373A3D0 | kickoff (segment flush) | known |

Functions touching the fence fields (+ 10896 / 10908 / 10900 / 10912), to
sort: 822F16D0, 8280EB48, 8348E458, 836552E0, 836E2BA0, 837341F0, 83736F48,
837370F0, 83737AE0, 83739520, 83739B38, 8373A088, 8373A3D0, 8373A4D0,
8373A5A8, 8373A718, 8373A7D0, 8373A848, 8373A918, 8373B4D8, 8373B880,
8373D570, 83740290, 83742A68, 837441A0, 837442E0, 83744AF8, 83744BF8,
83744D90, 837452C0, 83747940, 837483E0, 837492A8, 8374BAC8, 8374CA98,
8374D1E0, 83771600, 83775DB0, 837808A0, 837C6680.

Steps (each tested with the CP still running, then the CP off):
1. Fences natively: hook InsertFence (its value); the worker tags the record
   stream; when the GPU work recorded before it completes, write the value to
   status + 4 (big-endian) - what the gated EVENT_WRITE_SHD does now.
2. The ring: at each kickoff write status + 60 to the write position (no
   one reads the ring any more; BeginVertices data is copied at the next
   Direct3D call already).
3. Interrupts: the swap interrupt to the guest's graphics callback
   (D3D_SetInterruptCallback) when our frame is done; vblank already comes
   from the SDK's VSync thread.
4. The other memory writes the CP does (MEM_WRITE, REG_TO_MEM, COND_WRITE,
   EVENT_WRITE_EXT, occlusion queries' ZPD): find the D3D functions emitting
   them; do each natively.
5. Then the CP is not started (native renderer on); memexport is native only.

### test22 (2026-10-24)

A review of the threaded recording found a lost wakeup: the worker counted a
finished swap and notified the game thread without the mutex the game thread
waits under - a notify between its check and its wait would leave the game
hanging at Swap. Fixed (the count changes under the mutex). test22 = test21
with that fix; it carries everything since test16.

### test23: the new hub (2026-10-24)

Home after Project M's launcher (slanted arrow bands with drawn icons, a
framed description box, the logo plate with "The Izuna Project" under
NINJA GAIDEN II); settings after NG2's pause menu (chrome tab plates with
L1/R1, a red tint, setting plates beside a detail panel with every choice,
an Information bar); DLC / Saves / About under the same header. Colours as
before. `tools/hub_preview/build.sh` builds a tool rendering the hub's pages
to images on the build machine (the hub's own code and fonts, a CPU
rasteriser) - for checking its look without a console.

### test24: frame-save bursts fixed (2026-10-24)

Owner's test23 runs (logs in `~/ng2-ps5-deps/console/test23/logs`): a
7.5-minute run with "save frames: every second" was fine (log 001); with
"Bursts (flicker)" on, the game froze in the first cutscene the moment a
burst began (log 004: "frame 3601 saved ... a burst of 120 frames begins",
then no GPU batches, the presenter still painting, the stall report), and
every later run died at START GAME's load, a few seconds after the burst at
frame 1 (logs 005-008, no error line). The burst made a detached thread and a
3.6 MB copy for each of its 120 frames: ~430 MB held while slow storage wrote
them, and 120 threads at once. A failed thread start or allocation throws on
the draw worker, which dies; the game then waits for it for ever - the freeze
of log 004. The level load on top of the held memory explains 005-008.

Fix: one writer thread, at most 16 frame buffers (59 MB, given back down to 2
once written), no allocation that can throw on the drawing thread; a frame
with no free buffer is dropped and counted ("saved frames: N frames written
(X ms each, W waiting), D dropped" in the 600-frame line). Bursts are 30
frames now. Also new: the worker catches an exception from a record (logged,
the record dropped, its frame/sync counts still advanced), and a watchdog -
when the game's render thread waits over 2 s for the worker, "[native]
threaded: the game has waited N s for ..." says what the worker is on. On the
VM: four 30-frame bursts written complete, 23.8 ms a frame.

### test25: the cutscene freeze was the SDK's semaphore (2026-10-24)

test24 still froze in the first cutscene. Its new watchdog showed the draw
worker was not the cause (a 2 s wait for a logged frame's resolve dumps, then
the game went on), and the stall report showed the game's main thread
(guest F80000D4, start 822F16D0) waiting on event F8000064 with its four job
workers (F800006C..84, start 822F27B8) idle on semaphore F8000060.

NG2's job system (from the recompiled code): the dispatcher
(sub_822F2688) waits for all four "worker idle" events (table at 83BF1814),
resets the counters at 83BF1C70, then releases the semaphore by 4. Each
worker atomically counts itself idle, NtSignalAndWaitForSingleObjectEx(its
idle event, the semaphore), then runs its pinned job and pops shared ones
(sub_822F2870 / sub_822F2750); the one whose finished count reaches the total
sets "jobs done" (83BF1C3C = F8000064), which the main thread waits on.

Two differences from the Xbox kernel in threading_posix.cpp made a pinned job
never run: (1) a semaphore release went to whichever thread took the mutex
first, not to the threads already waiting - a worker done with its job and
back on the semaphore took the count meant for one not yet scheduled; (2)
SignalAndWait signalled, then started waiting - the dispatcher could release
before a worker that had signalled "idle" was waiting. Fixed: a release is
handed to the queued waiters oldest first (only the rest raises the count),
and SignalAndWait queues on a semaphore before it signals.
`tools/native/semaphore_stress.cpp` reproduces the pattern: the old code hung
at batch 19192 of 20000, the fixed one passes 4 x 60000. **Main track: the
same code is in ~/ng2-ps5's SDK** - the released port can freeze the same way
(rarer with the emulated renderer's timing).

Also in test25: resolve dumps ("save render passes") are written by a thread
of their own (they took the draw worker 2 s); the stall report's host stacks
keep only return addresses inside the eboot's code that follow a call
instruction, from up to 1 MB of stack (the old scan stopped at 64 KB and was
mostly data words); `diag_watch_event=<handle>` logs who sets / waits on an
event; `native_test_stall=<frame>` stalls the drawing 2.5 s (test).

### test26: fetch-instruction filters, whole bursts, audio counters (2026-10-24)

- A tfetch instruction can set its own mag/min filter (point / linear)
  instead of the fetch constant's; the fork records the slots
  (ShaderCacheEntry pointSlotMask / linearSlotMask) and the slot's sampler
  follows (Sampler(fc, filter)). 28 of 625 shaders force point on a slot, 1
  forces linear.
- Saved frames: up to 32 buffers (a whole 30-frame burst; the console's
  storage writes ~215 ms a frame - test24 kept 18 of 30).
- PS5 audio: the 10 s line now gives the frames the game made vs the frames
  played and the credits the full semaphore refused - the console drops ~11
  frames per 10 s with no underruns, i.e. the game makes more than it is
  asked for; this tells where.
- Host test: `NG2_CAPTURE` pairs (CLAUDE.md), `native_skip_ps=<hash>`.

Main menu vs the emulated renderer (host test): the native picture is
brighter, with white mist over the grass and sky. The menu is 2D: palette
layers (k_8 index images through 256x1 colour tables; shader 276BCC... point,
7020466... its own bilinear with 1D table fetches). Skipping 7020466 (the two
mist layers) brings the native ground to 6.5 (emulated 12); simulating those
two layers offline from the native renderer's decoded textures and the draw
log's quads/constants gives exactly the native result (ground 91.6, sky
127.5). So the native renderer draws what the textures and constants say;
the emulated (Xenia-derived) picture has almost no mist. Which is right
needs the real console's picture (the title is known for rolling mist) -
not changed.

### test26 on the console, and test27 (2026-10-24)

Owner's test26 run (video + logs in `~/ng2-ps5-deps/console/test26/`, and a
video of the emulated build as the reference for how the game should look):
**the cutscene freeze is gone** (semaphore fix, test25) - the first cutscene
played into gameplay. Compared with the reference, native still has:

- **Pause menu**: black background (reference: the paused scene, red-tinted,
  blood smoke) and the blood strips as opaque cyan (reference: dark red).
- **Chapter card** (before the first cutscene): the red city background
  disappears once the card is up (reference: it stays). Host test
  reproduces it (native: visible at the card's start, then black; emulated
  renderer: stays). Ruled out: the clear order (the ring has the colour
  clear - a prim-8 draw, mask FFFF - before the copy too; Xenos clears are
  draws, the copies carry no clear bits), the clears themselves
  (native_test_no_color_clear: still black), format aliasing (the city is
  drawn to EDRAM 0 as format 3/12, the card resolves format 3), skipped
  draws (none), missing resolved images (every sampled texture found). The
  card's frames have no 3D draws in the ring either, so the city must reach
  the reference through EDRAM/memory left from earlier frames - next: compare
  EDRAM contents across the card's first frames (emulated vs native).
- **Main menu**: no red glow on the highlighted item, and darker overall -
  next: the game's display gamma ramp (the emulated presenter applies it,
  native does not), and the highlight's draws.
- The mod textures of the main port (PlayStation prompts, "Downloadable
  Content", PRESS OPTIONS) are NOT wanted here (owner): IzunaRecomp shows the
  game's own textures. (A texture-hook call was tried and removed.)
- Performance: gameplay 52-59 fps, hitches to 244 ms; the draw worker spent
  13.4 ms a frame at ~970 draws (texture slots 4.4 ms of it).
- Audio: 1887 frames from the game per 1875 played (game ~0.6% fast) - 11
  dropped every 10 s (an audible skip about once a second).

test27:
- Per-draw timers read the TSC (sceKernelReadTsc on the PS5, rdtsc on the
  host) instead of steady_clock; "[native] clock:" logs both costs (VM: 37 vs
  7 ns a read) - on the PS5 a clock_gettime may be a syscall.
- A per-frame texture table (1024 entries by fetch constant): Texture2D, the
  resolved lookup and Sampler once per texture a frame, not once per use (VM:
  164 of 200 slot-cache misses served). "[native] texture slots a frame:"
  line splits the stage.
- PS5 audio output: rate matching instead of dropping - the game's frames go
  through a stereo FIFO read at 0.99-1.02x (linear interpolation) steered by
  the queue depth; a stall's backlog (> target + 24 frames) is still dropped
  at once. The 10 s line now always prints, with the mean playback rate.
- Diagnosis tools: `native_log_on_ps` / `native_log_on_tex` (log + save a
  frame 30 frames after a shader / texture first shows, and have the command
  processor log the same frame: `cp draw constants`, `cp draw state`, `cp
  copy`), `native_watch_tex` + pad script `untiltex` / `+N:button` and
  `NG2_CAPTURE=+N` (follow the game, not the clock), `cp_log_draw_every`,
  `native_log_frame` saves its frame, `native_test_no_color_clear`.
  hosttest.sh copies the newer GPU plugin (out/linux's copy goes stale).
- Fetch-instruction filters (test26) kept; `native_skip_ps` kept.

Chapter card, more (2026-10-24, later): a card frame with the black
background (native frame 975, `[d3d call]` trace of every D3D call in a
logged frame) has NO 3D draws: clears, the frame-start resolve 1F577000
(black after the 1F clear), the post chain, the 2D card, resolve to
1F1DF000, swap (front texture 40001C80 = 1F1DF000). The 3D city is drawn in
other frames (presented from 1EE47000): the game alternates front buffers.
So the background has to come from how 3D and card-only frames combine -
next: dump consecutive frames' resolves (1F577000, 1CC10000, 1F1DF000,
1EE47000) over ~10 frames of the card, native vs emulated (the emulated
guest-memory dump route needs resolution_scale 1, which the VM's emulated
renderer cannot set up: 4.6 GB scaled resolve buffer).

Backgrounds (chapter card, pause, menu mid-tones), more: every frame-start
resolve 1F577000 reads EDRAM base 0 as format 3 while the last draw there
was format 0 (the previous frame's final 8888 composite) - "[native] resolve
reads EDRAM base ... last written as format ..." (39 of 40 aliasing reads).
The emulated path's clear before it does write colour (cp draw ps: the clear
PS 2E372EA2 writes RT0, mask FFFF, needed = true), so its copy is black too;
the menu's raw sky is ~195 emulated vs ~130 native (presentation maps both
the same: 0.84x - 71 on the VM capture). Not solved; the diagnostics stay.

### test28 (2026-10-24)

- Draw recording only records what changed: the pipeline bind, the dynamic
  state (the rasterisation / depth group only while linked pipelines are
  bound - a whole fallback pipeline has it static and invalidates it) and
  the push constants are compared with the command buffer's current ones
  (State::DynCache, reset with each command buffer). VM: recording 0.48 us a
  draw (644 draws a frame) from ~1.7 us.
- Constants and fetch constants are byte-swapped 32 bytes a step (AVX2:
  Zen 2 on the PS5, x86-64-v3 on the host).
- "[native] render passes a frame:" - passes ended (each a GPU flush) and
  fetch-sized copies of resolves; the opening: ~20 a frame, ~10 of them at a
  fetch-sized copy. (Making those copies at the resolve instead did not lower
  the pass count - they fall between passes already; reverted.)
- Diagnostics: EDRAM aliasing on resolves ("resolve reads EDRAM base ... last
  written as format ..."), `present_ignore_external` (show the emulated front
  buffer), `NG2_CAPTURE_PRESENTER=1` (the presenter's picture and the native
  frame of one moment), `NG2_DUMP_MEM=addr:size`, `[d3d call]` list in logged
  frames, `cp draw ps` / gamma-ramp logs in the command processor (the game
  sets no gamma ramp: both renderers use the default one).

### test29: BeginVertices draws use the state of their call (2026-10-24)

**Fixed: the chapter card's and the main menu's backgrounds (and very likely
the pause menu's), the main menu's highlight, its darkness.** A
BeginVertices / BeginIndexedVertices draw was drawn at the next traced D3D
call (when its vertices are written) with the device state of THAT moment.
On the Xbox the draw packet is written inside BeginVertices: the GPU draws
with the state then. By the next traced call the game had already set the
next draw's constants and textures (inlined Direct3D code - no call to see),
so every such draw took the next one's state and the first of a run was
effectively lost. Found by aligning one frame's draws, native vs the ring
(`native_log_on_tex` + `cp draw constants`, unthreaded): native had every
2D layer shifted by one (layer i drawn with layer i+1's c17) and the card's
background layer (the post-processed scene, c17 0.637 0.637 0.727) missing.
Now the device bytes the drawing reads (0-12800) and the vertex fetch layout
are captured at the Begin*Vertices hook (State::begin_device / begin_layout)
and drawn from (threaded: recorded from them). VM: the card keeps its red
city background, the menu highlights NEW GAME with a sky of ~198 (emulated
~195, native was ~130), native and ring draw lists identical (64/64).

After test29: a 3D frame of the opening aligned native vs ring (unthreaded):
761 draws identical in order and constants (the logs' frame boundaries
differ by 254 + 6 draws). The ring's ~144 single-point draws a frame
(prim 1 count 1, VS C049A8C9, PS 2E372EA2, colour mask 0) are Direct3D's
own state draws - no memexport, nothing the native renderer misses.
Host-test note: the hub settings (UserData/ng2_settings.cfg) override the
command line for the native_* cvars they map (ps5_native_threaded,
ps5_native_memexport, ...): set them in the file for a test.

### test29 on the console, and test30: no file writes in the game's way (2026-10-25)

**test29 on the console** (frames saved every second, keep all; a logged
frame a minute; resolves saved for logged frames): the 2D layers are right;
small freezes that grew more frequent, two 5 s freezes, a 7 s stall, then
the game stopped. The audio kept playing through the freezes (out of sync).
Light flicker in the shop after the cutscene (not yet looked at).

What the log showed: both 5 s freezes started exactly at frames 30000 and
30600, the 10 s report, with the worker on that swap. The report saves the
pipeline cache (2.7 MB) when it grew - on the worker. Saved frames had gone
from 0.22 s to 3.3 s a frame (3.6 MB raw each, the storage filling up), so
that save waited seconds behind them. The log file was the same: a
synchronous sink with one lock, flushed every second and at each warning -
when a write stalls, every thread that logs (the render worker included)
waits. A logged frame also printed every Direct3D call (`[d3d call]`, ~20000
lines; 84754 in the run), and its saved resolves (~100 MB of device memory a
logged frame) queued without a bound behind the slow storage.

test30:
- **The log file is written by a thread of its own** (SDK, core/logging.cpp
  QueuedSink): a thread that logs copies its line into memory. Past 32 MB
  waiting, lines are dropped and counted ("[log] N lines dropped"), never
  waited for. FlushLogging (the crash hook, the exit to the hub) waits up to
  2 s for the lines so far, so a crash still keeps its last lines.
- **The pipeline cache and key files are written by a thread of their own**
  (native_draw.cpp SaveFileLater: newest content per file).
- **Saved resolves wait at most 256 MB**; past that one is dropped and
  counted in the "resolves saved" line.
- **`[d3d call]` lines only with `native_log_d3d_calls`** (default off).
- **Saved frames are QOI** (lossless, "The Quite OK Image Format",
  qoiformat.org; encoder written from the spec in native_renderer.cpp):
  37-59% of the raw size on the VM's frames, ~5.6 ms each on the writer
  thread. Pillow and ffmpeg read them; tools/native/rgba2png.py decodes them
  too (named captures stay raw .rgba). The 10 s line gives MB a frame.
- **Audio follows host stalls** (SDK include/rex/audio/host_stall.h,
  `audio_follow_game_ms` 50, 0 off): while the game's render thread has been
  blocked in the native renderer (its swap, or waiting for room in the record
  ring) for 50 ms, the PS5 audio output plays silence, keeps the queued sound
  and grants no credit, so the game's audio stops with the picture and
  resumes with it. Only host stalls count: holding audio whenever the game is
  slow could deadlock a game that waits on its own sound. (The owner's Xenia
  build was only looked at for its audio settings: apu_max_queued_frames 4,
  the new XMA decoder; nothing taken.)

VM: native frames, log and QOI frames checked (0.68 MB a frame, decoded
identical to the raw pixels). The audio hold needs the console (the desktop
test has no PS5 audio).

### test30 on the console, test31 and test32: resolution scale, cleanup (2026-10-25)

**test30 on the console** (10 minutes): 5 slow frames, worst 281 ms once, no
stalls; saved frames 0.1 s and 1.8 MB each (QOI). The freezes are gone.

**The native renderer draws at the selected resolution** (test31). It drew
1280x720 whatever the preset said. Now every EDRAM target, resolved image and
fetch-sized copy is RenderScale() times the guest size (Image::scale: 2 at
1440p, 3 at 4K), as Edge of Time scales its surfaces:
- guest pixels stay the unit everywhere the game speaks: the clip-space
  conversion (912-936) uses the guest size and the Vulkan viewport covers
  the whole host image; scissors, clear rects and resolve rects are scaled;
- `native_resolution_scale` (set from the preset) decides it; with the native
  renderer on, the emulated GPU's draw_resolution_scale stays 1 (its scaled
  EDRAM would only take memory, several GB at 4K);
- shaders (XenosRecomp fork): vPos is `iPos * g_RenderScaleInv - 0.5` (guest
  pixels; SharedConstants + 944); a fetch's texel offset is in guest texels,
  so on a scaled image (g_ScaledSlots, + 948: a bit per slot, set by the slot
  caches) it is multiplied by S - the post-processing blurs sample at 0.5 /
  1.5 texel offsets;
- the slope-scaled depth bias is multiplied by S (per host pixel the slope is
  1/S; Edge of Time's targetScale);
- frames shown and saved are at the scaled size; saved-frame buffers shrink by
  S^2 (at least 4).
VM at 1440p: the loading screen at 2560x1440, layout and text in place.

**The front buffer**: the swap looked the front texture's base up exactly,
but NG2's front textures start a page before their resolve (1EE46000 /
1EE47000, 1F1DE000 / 1F1DF000): every swap showed the frame's last resolve
(right only while the front buffer was resolved last). Now the newest resolve
within the texture's first 64 KB; a fallback is logged.

**Hitches**: every 600 frames the report serialised the pipeline cache
(vkGetPipelineCacheData, 2.7 MB) on the worker: a ~100 ms hitch every 10 s in
test29. It runs on the file saver thread now. Frame and resolve readbacks
use cached host memory (CPU reads of the write-combined kind were slow; a
saved frame's copy held the worker ~100 ms on busy storage).

**Cleanup** (test32): the test-only knobs from the VM hunts are gone
(native_test_stall, native_test_no_color_clear, native_skip_ps,
native_max_draws, native_show_texture); native_renderer.cpp loads its buffer
functions once with the rest and shares MemoryType / CreateReadback instead
of four hand-written memory-type loops and per-call function lookups.

Checked from the logs, nothing to fix there: no exponent-biased resolves, no
gamma render targets, signed fetch flags only on float formats.

**Open: occlusion queries are still answered by the command processor with a
fixed count** (query_occlusion_fake_sample_count 1000 - every queried light
glow / flare counts as visible, even behind walls; likely part of "the lights
look different", and its gated write may be the shop flicker). The native
answer is a Vulkan occlusion query around the native draws between the
game's query begin and end, written to the sample-count memory when that GPU
work completes. Missing: where NG2 begins / ends its queries - Direct3D's
query code is inlined (no traced call; the packet and register constants do
not appear as immediates in the recompiled code).

### test33: samplers, exponent adjust, fixed-point targets (2026-10-25)

Owner (test32 at 1440p: works, runs fine): lights, shadows and glows need
work most; a hall floor shows a "LOD / foveated" look where the light has a
cut-off. Three hardware behaviours the native renderer did not reproduce:

- **Samplers took only the filters and clamps** of a fetch constant. Now all
  that a sampler holds (Xenia's semantics; Eternal Sonata Reprise takes the
  LOD bias the same way): the mip filter's base-map-only mode (2: level 0
  only - was sampled with mips), min / max mip level, the LOD bias (dword 4,
  10 bits with 5 fractional), anisotropic filtering (aniso_filter 1-5 =
  1:1...16:1, with linear filtering; up to the device's limit) and the white
  border colour. The table holds 256 samplers (was 64). Floors at a glancing
  angle had no anisotropy: blurred with distance, with a hard line where the
  mip changed - the floor "cut-off".
- **The fetch exponent adjust** (dword 3 bits 13-18, signed: the result x
  2^adjust) was ignored. Now in every texture fetch (the fork's
  applyFetchSign, ng2ExpAdjust; a signed byte per slot at SharedConstants +
  984). Logged frames print it ("exp N").
- **16_16 / 16_16_16_16 render targets are fixed point -32...32** in EDRAM
  (xenos.h). They are SNORM images here; the shader now scales its output
  by 1/32 into them (g_FixedTargets, + 952) and clears are scaled alike, so
  they hold the hardware's bits, and their resolves copy the bits as they
  are (RawResolve) instead of converting values. NG2's variance shadow maps
  go through them: PS 1A99B35E writes the moments (1-d, (1-d)^2) of the
  shadow depth into 16_16, PS 1B603BC7 blurs them, the lighting reads them as
  k_16_16 - raw bits read as unsigned are ~v/64 there, which the fetch's
  exponent adjust is for. Before, the value-converting resolve gave v and
  the adjust was dropped: right only for one adjust value.

VM: the chapter card at 1440p with the new shaders and samplers; "samplers:
anisotropy up to 16:1, LOD bias up to 16". Not yet seen in a 3D scene.

Eternal Sonata Reprise (MIT, Marco Andronaco; ~/ng2-ps5-deps/refs/
EternalSonataReprise): a native renderer split by concern (d3d state, draw,
frame, texture, pipeline, readback, deferred command lists, batching) - the
layout to follow when native_draw.cpp is split. No guest occlusion queries or
exponent adjust there either.

### test34: capturing a frame from the overlay (2026-10-25)

The owner's reports come as videos and screenshots; a frame the player
chooses could not be logged on the console (logged frames came from
native_auto_log, a minute apart). The in-game overlay's "Capture frame
report" now, with the native renderer drawing, logs the next two frames
whole (ng2native::RequestCapture: every draw with its textures and
constants, every resolve and clear) and saves their render passes
(native_resolves/, whatever the save setting) and the frames shown
(native_frames/capture_<frame>.rgba). Two consecutive frames: a flicker
shows as a difference between them. Without the native renderer it is the
GPU backend's frame report, as before.

Occlusion queries in logged / captured frames: the command processor logs
each ZPD event ("cp occlusion query begin|end at <sample block>", and the
ZPass words found there) - whether the shop's light goes through queries,
and whether its fixed answer was there in time.

Owner's screenshots (test33): the hall floor's light pool still ends along
an edge; the tower shows the night sky as flat grey with a large dark
straight-edged wedge over it (top right) - to be looked at in a capture.

### test35: clip control (2026-10-25)

PA_CL_CLIP_CNTL was not read at all: draws with clip_disable (bit 16) were
clipped against the near and far planes anyway - a light volume or a sky
drawn so is cut by a plane (a candidate for the owner's light "cut-off").
Now those draws clamp depth instead (depthClampEnable, when the device has
the feature), as the SDK's backend does; pipeline key raster bit 4, and the
vertex library part is keyed by it (rasterisation state is static in it).
Not done: user clip planes (ucp_ena 0-5, clip distances in the shaders).

### test36: textures keep their mips; fetch LOD modes (2026-10-25)

Owner (test35): the clip-control change made no visible difference; the
"top right" bug is a building's texture glitching close to Ryu (the tiled
wall: shimmering light mortar lines); the floor issue is less at higher
resolutions. Both read as aliasing - textures sampled without their mips.

- **A texture's mips were decoded only when the fetch's mip filter was not
  base-map-only**, and decoded textures are cached by a key without the
  filter: a texture first met through a base-map-only fetch kept one level
  for every later fetch of it. Now every level the fetch constant has is
  decoded; base-map-only is the sampler's (maxLod 0.25, test33).
- **Fetch LOD modes** (the fork: ng2LodMode / ng2Lod / ng2RegLod / ng2GradH,V,
  static shader variables set before each fetch; NG2_SAMPLE): UseComputedLOD
  off -> SampleLevel at the register LOD (setTexLOD) + the instruction's bias
  (signed 3.4); computed with a bias -> SampleBias; register gradients
  (setGradientH/V) -> SampleGrad scaled by 2^bias - the SDK's SPIR-V
  translator's semantics. In NG2: 69 shaders have explicit-LOD fetches, all
  vertex shaders (level 0 before too) but one pixel shader, 360A4E3F (a
  position weight map / stamp: setTexLOD), which now samples its set level.

### The shop flicker: what the VM showed (2026-10-25)

VM run (new game, 720p, `--native_log_frame=4815`): frame 4815 is the shop
cutscene (Sonia by the shoji). The command processor logged the same frame:
- **no occlusion query (ZPD) in it** - the queries' fixed answer is not the
  shop light's flicker;
- the native draw list matches the ring's 1:1 in order (302 draws: prim 6
  283, prim 13 17, prim 8 2); the ring's extra draws are Direct3D's own
  (360 prim-1 points, 4 three-vertex rects = its clears).
Next: constants per draw (native "consts vs/ps" vs "cp draw consts", XXH3 of
all 256 + 256 float4, host order; tools/native/compare_constants.py) and
consecutive frames (`native_log_count`, 1-8, with native_log_frame): the
flicker alternates frame to frame, so one logged frame cannot show it. One
candidate: NG2 resolves its shadow address 1C784000 at 1024x1024 in some
frames and 512x512 in others; the native renderer keeps a variant per size
where the Xbox has one memory - a fetch of the other size gets a partly
stale fit copy.

Also in: the texture-slot loop's three paths share one SlotResult and one
apply step (no behaviour change).

### test37: depth at the Xenos 20e4 precision - the shop window flicker (2026-10-25)

The cause, from the console video (test26, 103.6 s, two consecutive 60 fps
frames, steady camera): only one region changes from one frame to the next -
the paper behind the shop's left lattice window, bright in one frame, grey in
the next; everything else is still. A whole flat region toggling while the
camera barely moves is two coplanar surfaces z-fighting (the window and the
light drawn on it). NG2's depth buffer is the Xenos 24-bit float format
(20e4, RB_DEPTH_INFO bit 16); the native renderer's is 32-bit float, which
keeps depth differences 20e4 does not - on the Xbox the second surface ties
and passes GEQUAL every frame, here it passes or fails with the camera's
sub-pixel motion. (Not occlusion queries: the VM's logged shop frame has no
ZPD at all; and its draws match the ring's 1:1, pixel constants c16-18
identical on every draw that could be matched.)

Fix, as the SDK's backend does it (Xenia's depth_float24 conversion in the
pixel shader, truncating): with a 20e4 depth target in use, every pixel
shader writes its depth at 20e4 precision - asuint(z) & ~7 from 2^-14 up,
steps of 2^-34 below (exactly the Float32To20e4 / Float20e4To32 round trip).
- the fork: SPEC_CONSTANT_FLOAT24_DEPTH (1 << 8), ng2DepthTo24; shaders that
  do not export depth declare `oDepth : SV_DepthLessEqual` (the conversion
  only lowers depth and NG2's test is GEQUAL, so early rejection stays
  valid); those that export it convert their own value;
- depth-only draws (no pixel shader of the game's: pre-passes, shadow maps)
  get shaders/depth24.frag (depth24_fs.inc), so a pre-pass and the colour
  pass after it compare as on the Xbox;
- the renderer sets the spec bit when the draw's depth target is 20e4 and it
  tests or writes depth; pipeline parts are keyed by the spec already.
VM: the 3D opening (canal, walls, blossoms, towers) renders whole.

Also in test37: `native_log_count` (consecutive logged frames) and the
texture-slot SlotResult refactor. (test37 also hashed all 512 constants on
both draw logs; removed after: whole-array hashes never match - Direct3D
sends the GPU only the constants a shader uses.)

### test37 on the console (2026-10-25)

Owner: a crash; 3D objects invisible for a second before appearing (new);
the shop light flicker still there; the building texture bug still there.

- **Objects appearing late**: test37's SPEC_CONSTANT_FLOAT24_DEPTH changed the
  key of every pipeline testing depth, so none of the saved / shipped keys
  matched and each was built at its first draw (a draw whose pipeline is not
  ready is skipped). Loaded keys that test or write depth now get the bit
  (QueueKeys), and the shipped list has the owner's 41 newer keys (204).
- **The flicker is texture aliasing, not depth**: the VM's 30-frame burst of
  the shop shot (frames 4801-4830, before the mip fix) - an alternation map
  over the burst puts it in one strip at the right edge: the outer lattice
  window seen through the shoji, whose fine bamboo-blind pattern shows, goes
  blank and shows again frame to frame (moire: a fine, brightly lit texture
  sampled from too fine a mip). On the Xbox the right mip blurs it. The
  owner's test37 still flickers, so that texture still samples the wrong mip
  even with test36's mip fix: logged frames now print each slot's mip
  address, mip range, packed flag, filters, aniso and LOD bias to find why.
  (Float 24 depth stays - it is the hardware's behaviour - but was not this.)
- **The crash** (ng2_017): an unhandled guest read of 0x3012BFF0 (never
  allocated - the heap watch saw nothing there) in game code sub_835CFA80
  (from sub_829DB630 / sub_8300BBA0), right after the game opened
  aurora12.wmv for a chapter load. The same family as test2/3's and test13's
  0x300D7FF0 read at the menu video (16 bytes before a page end each time).
  Not memory exports (they write physical memory; this is the 4 KB virtual
  heap) nor the chapter-12 site guard (it only reads). Open.

### test38: the shop flicker (back faces of reset strips), test33's fixed-point targets reverted, instruction filters

- **Reverted: test33's "16_16 fixed-point" storage.** The VM's logged shop
  frames show what it did: the shadow moments after NG2's moment pass read
  130-1024 (raw 16-bit), after its blur 2-16 - the blur reads them back with
  no scale and the fetch's exponent adjust is 0. So the hardware's resolve
  of a 16_16 EDRAM target to a k_16_16 texture converts the value (as the
  value-converting blit did before test33); storing 1/32 and copying bits
  left the moments 64x too small from test33 on - wrong shadows. The
  exponent adjust (test33) stays: it is the hardware's, and 0 here.
- **The fetch instruction's mip filter and anisotropy** now override the
  fetch constant's, as its mag / min filter already did (the fork's
  ShaderCacheEntry gains mipPoint / mipLinear / mipBase / anisoOff slot
  masks). In NG2: 19 shaders set the mip filter to point.
- Logged frames print each texture's mip address, mip range, packed flag,
  filters, aniso and LOD bias. NG2: every fetch constant carries a LOD bias
  of -48 (-1.5 levels; applied since test33, as the SDK's shaders apply it);
  many of its textures have no mip chain at all (mip address 0).
- **The shop window flicker: found and fixed.** A new diagnosis cvar,
  `native_probe=x,y`, logs one guest pixel's colour and depth after every
  draw of the logged frames (a pixel history; each sample ends the render
  pass, so logged frames only). On the VM's shop shot (frames 4807-4810,
  pixel 1265,150) it showed the paper of the outer window written twice at
  exactly the same depth: draw 42 (dark, the side facing the room) and draw
  129 (bright, lit from outside) - two meshes of one two-sided wall, both
  triangle strips with a primitive reset. The second is the back face seen
  from the shop: the hardware culls it. We did not: the reset vertex got a
  NaN position and the strip ran on as one, so after the first reset every
  strip's winding parity was off, and reset strips were drawn without
  culling. The back face then z-fought the front (GEQUAL, equal depth):
  solid bright on one frame, a one-pixel checkerboard on the next, and the
  bloom / depth-of-field passes (their 1/4-size blur of the sharp scene
  alternated 173 / 119 / 178 / 110 at that pixel) spread it into the
  flickering glow. (The earlier notes here - texture aliasing, then data
  read late - were wrong turns: the probe settles it.)
  Fix: an indexed strip with a reset is turned into a triangle list on the
  CPU when the draw is recorded (`ResetStripToList`: each strip after a
  reset index starts its winding afresh, as on the hardware; the indices go
  to the upload ring) - the fork's ng2VertexIndex reads it (expansion 2,
  list address at SharedConstants + 1016) - and those draws are culled as
  the game asks. Any two-sided geometry built from reset strips was
  affected, not only this window.
- `native_sync_frames` (each frame waits for its GPU work) stays as a
  diagnosis switch: it showed the flicker was not a guest-memory race.
- Built 2026-10-25 (eboot sha256 0ea5d1ce...e684cb; archive
  ~/ng2-ps5-deps/builds/native-m3-test38). VM check of the shop shot with
  the fix: the right-edge alternation over the 30-frame burst fell from 70.9
  to 4.8 (the rest of the frame 6.9, camera motion); the probed pixel stays
  at the dark paper (~0.29, as the emulated build) in every frame.

### test38 on the console, test39: the strip lists cached; native only (2026-10-25)

The owner's test38 run: the shop light flicker is gone, and the building
texture near Ryu is fixed too (the same back-face z-fight). New: 55-58 fps in
heavy gameplay, stutter at 60.
- **The cause was test38's own fix.** The worker's "shaders+targets" stage
  went from 0.27-0.29 ms a frame (test37) to 4.2-5.9 ms: ResetStripToList
  read and rewrote every index of every reset strip at every draw (~680
  strip draws a frame, ~17 MB a frame through the upload ring). Now each
  index buffer is converted once: lists are cached by {address, count,
  format, reset index} in 16 MB mapped arenas (at most 4; when all are
  full they are dropped together, freed 3 frames on, and converted again
  as drawn), and checked against their indices' XXH3 hash on the textures'
  schedule (every native_texture_recheck_frames frames; every frame once
  they changed). The reset compare uses the low 24 bits of 32-bit indices
  (the SDK's primitive processor: the hardware's rule).
- **No GPU emulation left to choose** (owner): the hub's Renderer (dev)
  switch is gone - the native renderer always draws - and so are the rows
  that only set the emulated GPU (render target path, memory refresh, skip
  unchanged memory, fast memory checks, async submission, descriptor reuse,
  shader compilation, texture cache size, the fuzzy alpha / accurate depth
  workarounds, GPU timing / profiling, texture dump and packs, and the
  native renderer's own GPU sync / memory exports / keep emulated drawing
  A/B switches). The cvars native_renderer, native_gpu_sync,
  native_memexport and native_keep_emulated_drawing are removed: memory
  exports are always ours (SharedConstants + 976), the command processor
  never draws or exports, and fences always follow our GPU work. ng2_tuning
  sends the command processor fixed values (1x, occlusion fallback) until
  M4 removes it.
- **Anisotropic filtering** (Graphics) now reaches the native renderer
  (native_anisotropic: -1 the game's, 0-4 = 1x-16x, overriding each fetch
  constant unless the fetch instruction turns anisotropy off); it went to
  the emulated GPU only.
- **PlayStation button prompts** work natively: the PS5 port's texture
  hook (ps5_textures.cpp) was the emulated texture cache's; the native
  texture loader now calls it (ng2native::SetTextureLoadHook) for each
  texture whose bytes are new or changed, then hashes what the hook left.
- **M4, step 1 studied:** InsertFence (0x8373A088) writes a NOP pair and two
  EVENT_WRITE_SHD packets: a status word ((device + 14904 & 3) | device +
  48) to the status block (device + 10896) + 4, and the fence counter
  (device + 10908) to + 0 - and, when device + 21516 is 0 and a flag of
  + 10941 is set, writes both from the CPU at once. Native fences: hook it,
  and write the two values when the GPU work recorded before it is done.
- **M4: who writes what the guest can see** (type-3 headers built as
  `lis/oris 0xC00n` + `ori <opcode << 8>` in the generated code): fences
  (EVENT_WRITE_SHD, 0x58) from InsertFence 8373A088, Swap 837452C0,
  83747940, 83749AE0; COND_WRITE (0x45) from 8373F928 / 8373FA08;
  REG_TO_MEM (0x3E) from 83740290; INTERRUPT (0x54) from 8373A5A8 (~3 a
  frame: the graphics interrupt callback, source 1); WAIT_REG_MEM (0x3C,
  GPU-side waits, nothing to write) from 8 functions. MEM_WRITE,
  EVENT_WRITE_EXT and EVENT_WRITE_ZPD are not built this way (a table or
  another form): still to find. The swap packet (XE_SWAP) is the SDK
  kernel's VdSwap, not the game's - the native present replaces it.
- **M4 design (next):** the command processor cannot go piece by piece -
  while it still runs the ring it writes the same fences (an older value
  after a newer native one would stall the game) - so everything switches
  together, behind a switch first, then as the only path:
  1. An Izuna GraphicsSystem without a command processor (the SDK's is 514
     lines: registers, ring setup, read pointer write-back, the interrupt
     callback, vblank). CP_RB_WPTR writes (the game's kick) move the read
     pointer write-back to the write pointer at once: nothing reads the
     ring any more.
  2. Hooks on the Direct3D functions above queue their guest-visible
     writes, each tagged with the native work recorded before it, and a
     guest-context thread of ours (an XHostThread: interrupts run guest code,
     DispatchInterruptCallback needs an XThread) runs them in order as
     that work completes - what PumpSideEffects does now on the CP's thread.
     InsertFence: status + 0 = fence counter, + 4 = status word (above).
  3. The present: the CP's IssueSwap path (gamma ramp, FXAA, the
     presenter) replaced by ours at the guest's VdSwap.
  4. Occlusion queries (now the CP's fallback count) answered by our own
     queries or the same fallback.
  5. Then the GPU plugin is not created at all (the Vulkan device and the
     presenter come from the SDK's UI layer, not the emulated GPU).
- **test39 built** (eboot sha256 9e8109b2...dc3474; archive
  ~/ng2-ps5-deps/builds/native-m3-test39). VM check (shop shot, 720p):
  "shaders+targets" 0.22-0.34 ms a frame at 300-530 draws (test38 on the
  same scenes: 2.8-5.1 ms); the shop window stays steady (right-edge
  alternation 4.5 over the burst); "PS5 prompts: first PlayStation picture
  in place" - the prompts work natively; no errors. The fork's cube
  instruction / cube fetch now follow the hardware (ng2Cube /
  ng2CubeDirection), though none of NG2's 625 shaders use either - the
  tower's glass reflections are 2D textures (still to find, from the
  owner's frame reports).

### test40: the owner's test38 captures - alpha test functions, memory-export ordering, capture files (2026-10-25)

The owner's overlay captures (test38, 4K): three of the tower's glass, three
of the first boss fight, one of Lew (its picture only partly written). Only
the last two rotated logs survived (5 MB each, ~15 lines a draw, and the
log's queue drops past its cap), so most captures lost their draw lines; and
the resolve writer dropped most of the second frames' resolves (one 4K HDR
target is 66 MB against its 256 MB backlog).
- **Captures are complete now:** a capture's draw lines go to
  native_resolves/capture_<frame>.log (FrameLog; written at the frame's end
  on the file saver thread, which now makes the folder), not the rotating
  log, and without the command processor's per-draw lines; its resolves
  wait for the writer (10 s at most) instead of being dropped. The draw
  line now carries RB_COLORCONTROL (alpha test / alpha-to-mask), the alpha
  reference and PA_CL_CLIP_CNTL.
- **Alpha test, every function** (the boss health bar: drawn full length in
  both boss captures after six hits, while the player's bar works). The
  shaders did clip(alpha - ref) - >= - and only for GREATER / GEQUAL: any
  other function (LESS, LEQUAL, EQUAL, NOTEQUAL, NEVER) drew everything. Now
  the fork's ng2AlphaTest compares by RB_COLORCONTROL's function
  (SharedConstants + 952), for every enabled test but ALWAYS - exact for >
  too. The boss bar's own draws were among the lost lines, so this is the
  likely cause, not yet a seen one.
- **Memory exports are ordered.** 64 of NG2's shaders export to memory
  (character skinning: the exports fill vertex buffers that later draws
  read), and nothing ordered those writes before the next draws' vertex
  reads inside a render pass - consecutive draws overlap on the GPU, so
  parts of a body could be drawn from stale vertices (the owner's missing
  pieces of Sonia's back and Lew's leg are the suspects). The fork marks
  exporting shaders (ShaderCacheEntry::memExport), and the renderer ends
  the render pass after each such draw (EndRendering's barrier); the
  report counts them ("after memory-export draws").
  **Then disproved as the cause:** no memory-export draw has run in any
  log (VM or console, with the exports native or the command processor's),
  and none of the 64 exporting shaders appears in any logged frame - NG2
  does not issue them in the scenes reached (title, chapter 1, the tower,
  the first boss). The ordering stays (it is the hardware's rule, and
  free when unused); the missing body parts are something else.
- The glass: the scene resolve shows the tower glass's reflection (soft
  vertical bars and a band - the room behind the camera) before any
  post-processing, steady between the captured frames; no fault found in
  it yet without a reference of the same spot.
- **M4: the seam is rex::system::IGraphicsSystem** (include/rex/system/
  interfaces/graphics.h): the kernel's Vd* exports only reach the GPU
  through it, and it is meant for custom systems - SetupPresentation
  (provider + presenter), SetupGuestGpu (the GPU register MMIO, the vblank
  worker), SetInterruptCallback, InitializeRingBuffer,
  EnableReadPointerWriteBack, Shutdown. The Vulkan device comes from the
  UI layer (ui/vulkan/vulkan_provider.cpp, which also fills
  external_frame::VulkanDevice) and so does the presenter - neither is the
  emulated GPU. So M4 is an Izuna IGraphicsSystem: the SDK's Vulkan
  provider and presenter, GPU registers (CP_RB_WPTR: read pointer = write
  pointer at once; the reads GraphicsSystem::ReadRegister answers), the
  vblank interrupts, the swap from our D3DDevice_Swap hook straight to the
  presenter, fences / interrupts / memory writes from the Direct3D hooks -
  and the GPU plugin (librexgpu-xenos: the command processor, the emulated
  backends) is never loaded.
  The SDK takes one directly: RuntimeConfig::graphics
  (`config.graphics = REX_GRAPHICS_BACKEND(...)`, include/rex/runtime.h) is
  used instead of loading `gpu_plugin` (Ng2App::OnPreSetup sets "xenos"
  today). Pieces to carry over from GraphicsSystem: SetupPresentation
  (graphics_system.cpp:105), the MMIO / vsync setup (:144), ReadRegister's
  answers (:353), DispatchInterruptCallback (:418, needs an XThread: the
  vsync worker and our effect thread are XHostThreads), and the swap's gamma
  ramp pass (the CP's IssueSwap: the DC_LUT registers the guest writes).

### M4: the native graphics system - no emulated GPU (2026-10-25)

`native_graphics_system` (src/native/izuna_gpu.{h,cpp}, native_present.
{h,cpp}): Ng2App::OnPreSetup gives the runtime our IGraphicsSystem
(RuntimeConfig::graphics) instead of the Xenos plugin - no command
processor, no emulated backend, nothing of the GPU plugin is created.
- **Presentation:** the SDK's UI-layer Vulkan provider and presenter (the
  same device the native renderer always drew on).
- **The GPU registers** (MMIO 0x7FC80000): the reads GraphicsSystem
  answered; CP_RB_WPTR (the game's kick) wakes the sync scan.
- **The sync scan** (its own thread, as the command processor's was): reads
  the ring from the read to the write pointer for what the game waits on and
  nothing else, then writes the read pointer back. Register writes (type 0/1,
  SET_CONSTANT's register range, REG_RMW) are kept for the few registers the
  sync packets read and the gamma ramp - shader constants are skipped by
  their length without being read; draws, state and waits are skipped. The
  guest-visible effects - EVENT_WRITE_SHD (fences; initiator bit 31 writes
  the swap + vblank counter), MEM_WRITE, REG_TO_MEM, COND_WRITE (a memory
  poll decided in the effect, in order with the writes before it),
  EVENT_WRITE_EXT (the whole surface), EVENT_WRITE_ZPD (every object
  visible: native_query_sample_count, the emulated GPU's fallback), the
  scratch register write-back and INTERRUPT - go in ring order to an effect
  thread of ours (a guest XHostThread: interrupts call the game) that hands
  each to the native renderer's gate and runs the due ones: the same gate,
  the same timing rules as with the command processor.
- **Found on the first run:** Direct3D passes its interrupt handler a
  callback through scratch registers 4 / 5 (8373A5A8: write them, INTERRUPT,
  then a WAIT_REG_MEM the command processor sat in until the handler ran).
  Written back at once, the scan ran on to the next handshake's values
  before the interrupt ran: the game called a stale pointer (abort in
  sub_8373A718). Scratch write-backs are ordered effects now.
- **Vblanks:** the SDK's vsync worker, ours (native_vblank_display_locked /
  _phase: the hub's Frame pacing).
- **The swap:** at each XE_SWAP the present thread waits for that swap's
  native frame (external_frame::SwapWait, as the command processor did) and
  draws it into the presenter's guest output (native_present.cpp: one
  full-screen triangle; the game's 256-entry gamma ramp from DC_LUT_RW_INDEX /
  SEQ_COLOR, linear until set; the hub's Anti-aliasing as FXAA / FXAA
  extreme - the SDK's swap FXAA, before the ramp - native_antialias).
- **VM checks:** 400 s from boot through the title into chapter 1 (no
  errors; frame 1800 in 4:13 against 4:35 with the command processor); the
  presenter's picture of the chapter 1 screen equals the native frame
  (channel means 48.3 / 24.6 / 24.8 against 48.4 / 24.6 / 24.7 - no swap,
  the linear ramp; the rest is the screen's animation); the PlayStation ✕
  prompt shows.
- **test40 built** (eboot sha256 ac03e72f...80edcb7; archive
  ~/ng2-ps5-deps/builds/native-m3-test40): the alpha test functions, the
  memory-export ordering, complete captures, the cube instruction, and the
  M4 native graphics system built in but off (native_graphics_system
  false: the command processor still runs the sync) until its VM gameplay
  run and a console run have passed. VM check of test40 itself (the shop
  shot): no errors, the window steady (4.6), stage times as test39.

### test41: the native graphics system on - no emulated GPU (2026-10-25)

native_graphics_system defaults to true: the GPU plugin is not created; the
game runs on izuna_gpu.cpp + native_present.cpp (M4 above). VM gameplay
run (chapter 1 to the shop and past, 7200+ frames): no errors; the logged
shop frames have the same draws as with the command processor (594 / 592);
the window steady (4.4); stage times as before. What NG2 asks of a GPU there
(600 swaps): ~5.5 fences and 3 interrupts a frame, the swaps, ~56-75
indirect buffers a frame - no memory writes, conditional writes, register
copies, screen extents or occlusion queries. The cvar stays for diagnosis
(false: the plugin's command processor runs the sync); it is not in the hub.
test40's eboot is kept on the share too (ps5-test-native/backup-test40).
Built: eboot sha256 0d2b6583... (/home/netro/ng2-ps5-deps/builds/native-m3-test41).

### test41 on the console, test42: the GPU's waits (2026-10-26)

The owner's test41: 160+ fps (the game ran fast), the picture washed out
(lifted blacks, low contrast), the intro video split into three strips now
and then, FXAA changing nothing.
- **The waits.** Direct3D puts WAIT_REG_MEM in its stream - the GPU waits
  until a memory word or register matches; NG2's is a memory word at
  1EE3B004 that must read 0 (the flip still pending: its vblank handler
  clears it). The command processor sat in those waits, so the fences after
  them - which the game waits on - came no sooner: that is what kept the
  game at the display's rate. test41's scan skipped them. Now each is an
  effect (WaitRegMem: polls at the packet's interval, register reads live,
  COHER_STATUS_HOST's dirty bit cleared as MakeCoherent did, 250 ms at most
  with a warning); everything after it waits with it. VM: 10883 waits in 600
  swaps at the title / menu, 218 of them had to wait, none gave up.
  The washed-out picture and the split video are most likely the same
  cause (time-based effects at 160 fps; a video texture read while the
  decoder writes it): the present pass itself matched the native frame on
  the VM - to be confirmed on the console.
- **FXAA** was read once, at the first present, and the overlay's live path
  set only the plugin's swap_post_effect: now native_antialias is set there
  too and read at every present.
- Built: eboot sha256 db589304...76cd305e (~/ng2-ps5-deps/builds/
  native-m3-test42); test41's eboot kept on the share (backup-test41).

### test43: alpha to mask; fetch-sized copies at the resolve (2026-10-26)

- **Nothing of the emulated GPU's is missing natively:** every setting the
  app still sends to the plugin is either native now (FXAA, frame pacing,
  resolution scale, anisotropy, occlusion answers) or was the emulated GPU's
  own (its scale, texture cache, readbacks, frame report). The Flying
  Swallow flash filter never worked there either (the hub's "Coming soon");
  natively it would be skipping that draw - it needs a capture of one.
- **Alpha to mask** (RB_COLORCONTROL bit 4) was ignored. On a single-sample
  target the hardware dithers by alpha: the pixel's place in a 2x2 square
  picks one of four 2-bit offsets (the register's top byte) and the pixel is
  kept where alpha >= 1 - offset / 4 (the SDK's translators,
  CompletePixelShader_AlphaToMask). The fork's ng2AlphaToMask, under its own
  specialization constant (SPEC_CONSTANT_ALPHA_TO_MASK, 1 << 9: pipelines
  without it have no discard, so early depth stays), the offsets at
  SharedConstants + 956. 214 of the 625 shaders carry it (those with the
  alpha test). The shop does not use it; the logged cc field shows where.
- **Fetch-sized copies at the resolve.** A resolved image sampled at a
  smaller size (the 213x120 glow inside a 224x120 resolve) gets a copy of
  its top-left part; it was made at the draw that sampled it, ending the
  render pass - 18 of the 32 passes ended a frame in the owner's heavy
  scene. The report's "made at their resolve instead" was never written.
  Now RefreshFits makes them right after their resolve (already outside a
  pass), for the copies a draw sampled in the last two frames; the draw then
  finds them current. The same pixels: only resolves write resolved images.
- **VM checks** (two runs to the shop and past, 8400 / 6000 frames): no
  errors, no wait gave up; the logged shop frame has the same draws (596 /
  592); the window steady (4.0 / 1.3); copies made at a draw 0.0-0.1 a
  frame against 6-11 at their resolve (18 at a draw in test38's console
  log); the frames' mean brightness the same (within 1 of 255) - the
  washed-out console picture of test41 is not a rendering difference the VM
  shows. (The two runs' pixels differ a little: the VM's timing moves the
  cutscene camera between runs.)
- Built: eboot sha256 c92a6733...b0d7ec93 (~/ng2-ps5-deps/builds/
  native-m3-test43); test42's eboot kept on the share (backup-test42).

### test43 on the console, test44: the waits where they stand (2026-10-26)

The owner's test43 (ng2_017): 60 fps (p50 16.7 ms, p99 ~17.7 ms) but it
feels choppy, as the first builds did; still washed out (the mid-tones
brighter: median 21 -> 37 of 255 in the two shop screenshots, the blacks
not lifted); FXAA hardly visible.
- **The cause found in the code:** the command processor waited out each
  WAIT_REG_MEM inline - NG2's vsync wait: every frame waited ~16.5 ms there
  (the log: 600 of 600 swaps) - and only then read on, wrote the fences
  after it (some carry the swap + vblank counter, initiator bit 31, which
  the game takes its timing from) and presented the swap. test42 / 43 made
  the wait an effect and read on: the counter values and the present ran
  ahead of the vblank - frames evenly made but shown and timed unevenly (the
  choppiness), and the game's timing from that counter uneven too (most
  likely the exposure as well: NG2 adapts its exposure over time).
- Now the scan waits where the wait stands and presents where the swap
  stands (Present, on the scan thread, after SwapWait as the command
  processor's swap) - the command processor's order and timing, with none
  of its emulation. The present thread is gone.
- FXAA is the SDK's algorithm unchanged; at 4K (the owner's 3x) its effect
  on edges is small - as it was with the emulated GPU's.
- A log line if the game ever writes a whole 256-entry gamma ramp
  ("the game's gamma ramp: red at ..."), against the linear one.
- Built: eboot sha256 b7e1919b...4b3e17 (~/ng2-ps5-deps/builds/
  native-m3-test44); test43's eboot kept on the share (backup-test43).
  Not run on the VM (the owner: build and test on the console).

### test44 on the console, test45: the game's gamma ramp; latency (2026-10-26)

The owner's test44 (ng2_018): performance good again (59.9 fps, p50
16.9 ms, p99 17.1 ms; one 33 ms hitch); still washed out; input latency to
improve.
- **The washed-out look was the game's gamma ramp, never applied since
  test41.** Matching the two shop screenshots' percentiles (test38's
  command processor against test41's M4) gives one power curve: new ~
  old^0.78 (16 -> 30, 26 -> 43, 47 -> 70, 123 -> 142; blacks stay black),
  the game's own HUD as well (the emblem's 95th percentile 175 -> 190) - so
  the whole guest image, the swap's ramp, not the renderer. NG2 uploads its
  ramp in sub_8373F928 (the function test38 found writing COND_WRITE):
  RW_MODE / RW_INDEX = 0, WRITE_EN_MASK = 7, then for each of the 256
  entries a type-0 write of **DC_LUT_30_COLOR** (0x1925: red / green / blue,
  10 bits each, from three u16 tables) and a COND_WRITE setting RW_INDEX to
  the next entry. The command processor handled DC_LUT_30_COLOR; Izuna only
  had DC_LUT_SEQ_COLOR (the per-component form), so the ramp stayed linear.
  Now SetRegister applies it (the write-enable mask, the index step as the
  command processor's) and logs the ramp once written ("the game's gamma
  ramp (30-bit): ..."). The COND_WRITE polls a register and targets one, so
  it runs inline on the scan thread, in order with the entries. (The VM
  comparison that found no difference between the two paths compared the
  renderer's frames, before the ramp.)
- **Input latency.** test44's presenter line "acquire 16.45 ms" a paint:
  each paint waited a whole refresh for a free swapchain image - two flips
  queued ahead - so a frame reached the screen ~2.5 refreshes (~42 ms)
  after its guest vblank. The guest vblank was locked to the display by
  those very waits (display_clock: an acquire that waited returned at a
  vblank), so the queue had to stay full for the lock to hold; two images
  (the old "Low") broke the lock instead - why it felt uneven in 16.13.
  Now, on the PS5:
  - src/ps5/ps5_videoout.cpp: the WSI's sceVideoOutOpen is wrapped at link
    time (ps5_package.sh and CMake: --wrap=sceVideoOutOpen) to keep the
    handle; LastVblank reads sceVideoOutGetVblankStatus (count, process
    time, TSC as the PS4 SDK lays it out - its first four answers are
    logged raw), FlipPending sceVideoOutIsFlipPending.
  - VsyncThread trusts VideoOut's vblanks once 30 in a row agree (the same
    count gives the same time, each step 6-20 ms; "[izuna] guest vblank on
    VideoOut's vblanks"), and with native_vblank_wait_flips holds a guest
    vblank one refresh while a flip still waits for the display: the queue
    drains, then each frame shows at the next refresh (~0.5 refresh after
    its guest vblank at phase 50). Never two holds in a row; at most one a
    second, the gap doubling (to 64 s) whenever the queue fills again soon
    after (a load the GPU cannot finish in time), back to 1 s after a quiet
    minute. LogCounts reports holds. Without trusted VideoOut times
    everything is as in test44.
  - The hub's Input latency: Low (default, defaults round 7) is this;
    Standard is test44's pacing. Three swapchain images either way
    (present_low_latency is no longer set). Live.
- Built: eboot sha256 c554e239...7c7dde (~/ng2-ps5-deps/builds/
  native-m3-test45); test44's eboot kept on the share (backup-test44).
  Not run on the VM (the owner: build and test on the console).

### test45 on the console, test46: holds that never drained; the renderer in parts (2026-10-26)

The owner's test45 runs (ng2_019, ng2_020: a whole chapter, 27 min): the
colours are right (the game's ramp, 0 66 193 326 461 ... of 1023, written
once at the start and never again - so the tower's look, which the owner
thought maybe still washed out while chapter 2 looks right, is the scene,
not the ramp); input latency "as good as it can be"; tiny hitches.
- **The hitches were test45's own holds:** 91 holds in ng2_020 against 59
  hitch reports, each window's worst frame 34-45 ms. sceVideoOutIsFlipPending
  answered "pending" at every vblank - the holds came at exactly the
  back-off's gaps (8, 16, 32 s) - and the presenter's acquire waited
  16.4 ms a paint throughout: the UI thread painted back to back, paced by
  the swapchain. Only the first few holds drained anything.
- Why it painted back to back: the overlay's ImGui dialog asks for a repaint
  at the end of every paint; the presenter's UI-only throttle then polled
  for the next guest frame in 4 ms steps. Now ImGuiDrawer asks only when no
  guest frame came for 100 ms (Presenter::GuestOutputLive): each guest frame
  requests its own paint, at once.
- Holds from exact accounting: ps5_videoout.cpp also wraps
  sceVideoOutSubmitFlip (each flip's argument and time) and reads the shown
  argument from sceVideoOutGetFlipStatus (word 3, as the WSI does).
  ShouldHold: two flips waiting, or one submitted before the display's last
  vblank; never two in a row, at most one a second. LogCounts reports the
  flips waiting at each guest vblank (none / one / two / more) and the holds;
  the presenter line now counts acquires that waited.
- The PS5 WSI always makes at least three images (MAX2(minImageCount, 3)):
  present_low_latency's two never happened there.
- **Waits and fences without polling:** the completion thread waits on the
  oldest unfinished segment's fence (2 ms at most) instead of looking every
  0.5 ms, and tells the effect thread (SetGpuProgressCallback) - fences
  reached the game up to 1.5 ms after the GPU finished. WAIT_REG_MEM waits on
  a condition woken by what we run (the vblank handler, interrupts and
  fences, MMIO writes; every 100 us otherwise) instead of 50 us sleeps.
- **native_draw.cpp in parts** (4909 lines; moved, not rewritten):
  native_draw_internal.h (the state, the shared types, each part's offer),
  native_draw.cpp (images, frames, uploads, shaders, targets, the draw,
  resolves, clears, the hooks), native_pipelines.cpp, native_textures.cpp,
  native_worker.cpp (threaded recording), native_capture.cpp (resolve dumps,
  the probe, the file saver). The anonymous namespace became
  ng2native::draw.
- What only the command processor needed, gone from Izuna: the draw census's
  command-stream half (always 0), RequestCpDrawLog calls, RenderScale's
  fallback to draw_resolution_scale_x (native_resolution_scale defaults to
  1), the M0 call tracer's counting and log lines (native_d3d_trace off by
  default). ng2_tuning.h keeps the emulated GPU's cvars in one block, for the
  diagnosis mode only.
- Still open (owner): the first boss's health bar (the priority), Sonia's
  missing parts, the tower's glass and glow. The health bar needs a frame
  capture of the fight: its draws were never in a log.
- Built: eboot sha256 063b1a0c...74c4d1 (~/ng2-ps5-deps/builds/
  native-m3-test46). The ng2_tuning.h tidy came after the build (no change
  of values: the next build has it). Not run on the VM.

### test47: no GPU emulation in the build; the Kasumi Renderer (2026-10-26)

The owner named the native renderer **the Kasumi Renderer** (readmes,
GitHub, release notes), and asked for the emulated GPU's library to go.
- The SDK has an option, REXGLUE_GPU_EMULATION (default on, for other
  apps); IzunaRecomp's CMakeLists.txt turns it off. Off, src/graphics (the
  Xenos plugin rexgpu-xenos: the command processor, the emulated Vulkan /
  D3D backends, the shader translators) is not built, the install list
  leaves it out, and LoadGpuPlugin reports that there is none
  (REXGLUE_NO_GPU_EMULATION). The app no longer asks for GPU_PLUGINS xenos.
- The app: OnPreSetup always installs the native graphics system (the
  native_graphics_system cvar and its diagnosis mode are gone);
  the overlay's live settings set native_vblank_display_locked instead of
  the plugin's pacing and stop setting the plugin's submission, descriptor
  and memory-refresh cvars; the frame-report fallback to the plugin is gone
  (the capture is the Kasumi renderer's); the startup check logs "Kasumi
  renderer: internal scale Nx" instead of the plugin's scale; ng2_tuning.h
  drops clear_memory_page_state, render_target_path_d3d12, the PC texture
  pack cvars, the plugin's diagnosis block and test levers; the
  native renderer no longer sets the command processor's drawing flags.
- Adapted rather than dropped: the hub's Developer > Frame report (it was the
  plugin's frame_report_after_seconds) is now native_capture_after_seconds,
  the Kasumi frame capture N seconds after the first frame. The Flying
  Swallow flash filter stays a greyed "coming soon" row: its filter was the
  plugin's.
- The eboot: 190.5 MB (test46: 195.6 MB), no command-processor symbols.
  tools/native/hosttest.sh no longer stages librexgpu-xenos.so.
- Left for later: the PC F10 menu (ng2_menu.cpp, not shown on the PS5) still
  has rows for the emulated GPU's texture cache, accurate depth, fuzzy alpha
  and texture dump / packs; the settings file keeps fields only those used.
- Built: eboot sha256 f0d43497...794a46b (~/ng2-ps5-deps/builds/
  native-m3-test47; it carries test46's changes too); test46 kept on the
  share (backup-test46). Not run on the VM.

### M5, test48: no GPU command stream read - Direct3D's GPU side at its calls (2026-10-26)

The owner asked for IzunaRecomp to be emulation-free end to end. What was
left after test47 was the ring scan: the native graphics system read the
command stream Direct3D writes for the packets the game can see (fences,
interrupts, waits, the gamma ramp, the swap) - a GPU command processor in a
narrow role. Now nothing reads the stream: the kick (CP_RB_WPTR) reports it
consumed at once, and what the game can see is done at the Direct3D calls
that ask for it.
- **The census first** (VM, with the ring scan as the oracle: every sync
  packet by signature, three frames in order, the CPU's register accesses).
  All of NG2's sync packets are in Direct3D's command-buffer segments, and
  per frame they are: InsertFence's two EVENT_WRITE_SHD (the status block
  1EE3C000: +4 the status word, +0 the counter); three callback handshakes
  (8373A5A8: scratch 4 / 5 = callback / argument - 837C59C8 twice, the
  swap callback 83744BF8 once - scratch 0 = the CPU mask 4, a wait for its
  write-back, INTERRUPT on CPU 2, a wait for the handler to clear scratch
  0, scratch 4 = 0BADF00D); the flip (83744D90: scratch 1 = 1, its
  write-back waited for, the swap callback's handshake, then a wait for
  scratch 1 back to 0 - the vsync wait); XE_SWAP (the SDK's VdSwap);
  cache-coherency writes and waits (COHER_*: nothing to do natively) and
  cache-flush EVENT_WRITEs; the gamma ramp (8373F928) in menus and at
  start. Nothing else in a whole chapter on the console (test44/45 logs: no
  memory writes, register copies, screen extents, occlusion queries). The
  CPU only ever reads register 0x1951 (interrupt status) and writes only the
  kick: the register file is not needed at all.
- **How the flip works** (from the game's code): the swap callback
  (83744BF8, run by the handshake's interrupt) measures the GPU's time
  against a refresh (mftb) and queues a flip for a target vblank (late:
  one more); the interrupt handler's vblank path (8373A718 -> 83744AF8)
  counts vblanks and, for each queued entry whose target has come, flips the
  front buffer (D1GRPH_PRIMARY_SURFACE_ADDRESS) or - entry 0 - clears the
  flip-pending word: what the flip's last wait waits for.
- **The timeline** (izuna_gpu.cpp): each of those calls adds steps in call
  order - hooks at the calls (InsertFence 8373A088, the flip's wait after
  its handshake 83744EC4, the gamma ramp 8373F928, timestamps 83749AE0 in
  config/hooks/native.toml; the handshake 8373A5A8 and the flip's entry
  83744D90 through the call tracer; VdSwap through a new
  IGraphicsSystem::OnGuestSwap). Each step runs on the effect thread once
  the native GPU work recorded before its call has finished
  (native_renderer's RunAfterRecordedWork: in threaded recording, due when a
  kickoff or swap record from the call on is done; else the open segment),
  or after 30 ms. A wait blocks the steps after it, as the hardware's did.
  The gate is stricter than the ring scan's (that one took the last kick
  before the scan saw a packet): on the VM's software GPU (~100 ms a frame)
  most steps reach the 30 ms fallback, which the console's ~5 ms frames
  should not.
- Removed: the ring scan, the PM4 packet parsing, the register file, the
  gamma registers' decoding (the ramp now comes from the game's own tables
  at the call), native_query_sample_count (NG2 asks no occlusion query),
  the external_frame side-effect gate.
- VM check (ng2_161, 900 s): boot, the title, chapter 1's cutscenes render
  as before (frame 08 of the run: Sonia's cutscene); the ramp arrives
  through the hook; 600 flips / 1800 callbacks / ~1400 fences per 600 swaps;
  three flip waits gave up after 250 ms in the first 15 s of boot (the VM's
  slow frames), none after. To check on the console: "a wait gave up"
  lines and "by the 30 ms fallback" in the native renderer's line.
- Built: eboot sha256 e2716a0b...f31eef94 (~/ng2-ps5-deps/builds/
  native-m3-test48); the ring scan's last version with the census is kept in
  ~/ng2-ps5-deps/backups/native-m3-test48/izuna_gpu.ringscan-census.cpp.

### test48 on the console, test49: the chapter-load crash; the blood's cyan dots (2026-10-31)

The owner's test48 run (findings/logs/ng2_027, 14 min): pacing as it should
be - 59.9 fps, p99 16.7 ms, no hitches; the presenter never waited for an
image (0 of 600 paints), no flip ever queued ("none 600"), no holds; 110
steps by the 30 ms fallback in the whole run; no wait gave up. Then a crash
entering the next chapter, and blood showing blue / green / red dots.
- **The crash** (unhandled guest read of 0x300E3FF0 in sub_83823B30, a heap
  function, from the 0x8386xxxx video code): the same family as test2/3/13's
  0x300D7FF0 and test37's 0x3012BFF0 - a heap block header read 16 bytes
  before a page end, right after the game released the pages around it (the
  heap watch logged 300E0000-300E3000 released just before). The PS5 has 16
  KB pages and the game's low heap 4 KB ones: BaseHeap::SyncHostPageAccess
  gave a host page no access once none of its four guest pages was
  committed. A 4 KB-page host never does that (Release and Decommit leave
  the host's protection alone - upstream's "games are using memory after
  releasing it"), and NG2 relies on it. Now a host page with nothing
  committed is left as it is (released: still readable; never committed:
  its reservation's no-access); protect_on_release still protects.
  Resolved with the anchor rule (address = anchor-relative - 0x10000000 +
  NowMs in the build's llvm-pie.elf; addr2line). Builds archive their
  llvm-pie.elf from now on (test40-48's symbols.txt came from out/ps5/ng2,
  another link).
- **The blood's dots** (captures 25756 / 26223): the dots are cyan - red 0,
  green and blue high. The frame's scene resolve (2_10_10_10_FLOAT, ours
  R16G16B16A16_SFLOAT) holds 116248 negative components there (green / blue
  to -1.3) where the EDRAM, unsigned 7e3, stops at 0: the blood darkens the
  floor with a subtracting blend, and a post pass dividing by the brightness
  flipped the signs. The shaders' 7e3 clamp (the fork) covers their output,
  not the blend. Now each resolve of a 7e3 target is saturated in place as
  the EDRAM holds it (native_resolve_7e3.cpp, shaders/resolve7e3.comp: NaN
  to 0, colour 0-31.875, alpha to 2 bits) - a compute pass on the resolved
  rectangle (resolved 16-bit float images get storage use). On the VM's
  software GPU it costs ~200 ms a frame; on the console's GPU a fraction of
  a millisecond.
- VM check (ng2_162, 600 s): boots, chapter 1's city flyover renders as
  before; the saturate pass runs; one flip wait gave up at boot (as test48 on
  the VM; none on the console).
- Built: eboot sha256 aabbf54d...294acb31 (~/ng2-ps5-deps/builds/
  native-m3-test49, with llvm-pie.elf).

### test49 on the console, test50: 720p since test47; the scissor (2026-10-31)

The owner's test49 runs (findings/logs/ng2_028-029): the crash and the blood
are fixed; regressions they put at test48: the picture at 720p with the
preset at 4K, the tower (chapter 1, phase 2) greyish; the first boss's health
bar still full (captures 36741 / 37028).
- **720p** - not the renderer: test47's ng2_tuning.h tidy removed the
  native_resolution_scale entry in one place to add it in another, and the
  add's match failed silently (an unchecked replace in my edit script), so
  test47-49 sent no scale and the renderer drew at its default, 1x ("Kasumi
  renderer: internal scale 1x" beside "renders 3840x2160 (3x)"). Restored
  (VM: preset 1440p -> "internal scale 2x", 2560x1440 frames). Every other
  change of that tidy was checked against test46's files: intended.
- **The boss's health bar** - the renderer took the scissor from 0x200C /
  0x200D, which are COHER_DEST_BASE_6 / 7 (always 0: no scissor at all);
  the screen scissor is 0x200E / 0x200F (register_table.inc), in the
  device shadow's 0x2000 group. Wrong since the first gameplay build
  (test13). NG2's boss bar (from the capture): the frame and dark interior
  (PS 276BCCBD, a 1038x19 texture), the red layer added over the whole bar
  (PS 70204660: a 640x75 index texture through a 256x1 palette, SRC_ALPHA /
  ONE), and the pink recent-damage segment (PS 276BCCBD, a quad sized to
  the damage) - the red layer has no length of its own, so it is cut at the
  health by the scissor. Every other register number the renderer uses was
  checked against register_table.inc: only the scissor pair was wrong. Other
  scissored passes are affected too.
- **The tower's grey look** - the game's ramp is the same in every build since
  test45 (0 66 193 326 461 599 739 881 1020), and test38's capture of the
  tower (newthings/native_frames, before M4) is just as hazy: that area's
  look, made flatter by test47-49's 720p.
- Built: eboot sha256 fb8ca451...12aa5ec6 (~/ng2-ps5-deps/builds/
  native-m3-test50). Not run on the console yet.

### test50 on the console, test51: the window scissor; captures at the game's size (2026-10-31)

The owner's test50 (findings/logs/ng2_030): the resolution is right again;
the boss's health bar still full; the overlay's frame capture froze the game.
- **The bar's scissor is the window scissor, not the screen one.** NG2's
  Direct3D sets the screen scissor once, at the device reset (0x83748F18:
  BR 0x20002000), so test50's register fix changed nothing; SetScissorRect
  (0x83734530: device, left, top, right, bottom) writes PA_SC_WINDOW_OFFSET /
  WINDOW_SCISSOR_TL / BR (a type-0 packet from 0x2080) straight into the
  command buffer - not into the device shadow, so the Kasumi renderer never
  saw it, before or after M5. Two other writers: the clear path (0x837384D0
  at 0x83738608: offset 0, 0,0 - 0x2000,0x2000) and an internal draw
  (0x8373AD78: 16x16, not modelled - Direct3D must set its own rect again
  after, or the Xbox would show it). Now hooks keep the window scissor
  (native_draw.h OnWindowScissor; a Scissor record with native_threaded)
  and each draw clips to it and the screen scissor; the draw log shows it
  ("window TL-BR"). VM (ng2_164): the game keeps it at its target's size
  (02D00500 for 1280x720, 00010001 for the 1x1 brightness) and narrows it
  where it means to; chapter 1 renders as before.
- **The capture freeze**: a captured frame's ~30 resolves were copied at the
  renderer's size (4K: up to 66 MB each) and each waited for the file writer
  (10 s at most) - the drawing waited minutes. Captures now save resolves at
  the game's size (a blit down from the renderer's scale; 1280x720 for the
  frame) and never wait: past the writer's 256 MB a resolve is dropped and
  counted.
- Built: eboot sha256 611da3dd...1deba35f (~/ng2-ps5-deps/builds/
  native-m3-test51).

### test51 on the console, test52: a refinement pass (2026-10-31)

The owner's test51: the boss's health bar works, the resolution is right -
the best build so far. One report: after chapter 1's last cutscene, a large
white glow in the top right corner (not captured).
- **New resolve images start cleared.** A resolve's image was created and
  only the resolved rectangle written; the rest held whatever the memory had,
  and a pass sampling past the rectangle (a bloom's blur, a downsample) could
  read it - a likely cause of the glow, unconfirmed. Each is now cleared to
  zero when created (CreateResolvedImage).
- **Pipelines linked ahead of their first draw.** The shader parts of earlier
  runs' keys were already compiled at start; their pipelines were linked at
  the first draw that needed them (test51's log: ~100 at one area load). Now
  a worker links each key's pipeline once both its parts are ready (a job
  kind 4, PreLink), so the draw finds it. The draw report says how many were
  linked ahead, how many at the draw, and their time each and at worst. VM
  (ng2_165): 346 linked ahead, 1 at the draw. The state library is now
  created under a lock (two workers could make it at once).
- **Dead settings removed**: the live settings no longer set the emulated
  GPU's cvars (the FOV shader patch, the texture pack path); CAS sharpness
  only where the cvar exists.
- VM (ng2_165, 600 s): no new errors; draws a frame and the GPU's time as
  ng2_164.
- Built: eboot sha256 72f5fa20...0f817c17 (~/ng2-ps5-deps/builds/
  native-m3-test52).

### test52 on the console, test53: fences on time after their kick (2026-10-31)

The owner's test52 (new finding/logs/ng2_032): at the game over screen the
frame rate fell to exactly 15 fps (66.7 ms frames, ~15 s each death), and the
side effects run by the 30 ms fallback went from 49 to 361 in those stretches
- about one a frame. Elsewhere 60 fps; pipelines: 492 linked ahead of their
first draw, 30 at it (0.01 ms each), so the area-load hitches are gone.
- **Every fence waited for the kick after its own.** Direct3D inserts its
  fences inside the kick (8373A3D0 -> 8373A170, the only caller of
  InsertFence 8373A088), and the kick hook records its Kickoff at the entry -
  so with native_threaded the fence's step was given the next record's
  sequence, due only once a later kickoff or swap had finished. In play the
  next kick comes within the millisecond; the game over screen waits on its
  fence right after the kick, so nothing came and each frame waited the 30 ms
  fallback. RunAfterRecordedWork now takes the last record when it is a
  kickoff or swap. The first eight late steps are logged (the record waited
  for, the records and segments done).
- VM (ng2_166): runs as before (the VM's GPU is too slow for the fallback
  count to mean anything there).
- Built: eboot sha256 38574899...6530cba380 (~/ng2-ps5-deps/builds/
  native-m3-test53).
