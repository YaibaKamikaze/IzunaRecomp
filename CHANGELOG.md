# Changelog

## v0.2.2.1 — Windows and Linux performance update

A performance update for **Windows** and **Linux** (the Steam Deck included), from a rework of how the game
is recompiled and drawn. Android and PS5 are unchanged in this release: use v0.2.2 for Android (its
optimisation is still in progress) and v0.2.1 for the PS5.

**Recompiler**
- The game's memory reads and writes are now ordinary ones the C++ compiler can optimise (before, every
  one was forced to happen exactly as written), with the waits between threads kept correct.
- The recompiler finds the game's busy-wait loops (41 of them, the main loop's wait for the render thread
  among them) and makes them pause, then let other threads run, instead of spinning flat out. They do not
  sleep: a sleep woke too late for the game's handoffs between its threads, so frames missed the screen's
  refresh and 60 fps looked like 30.
- Linux: calls between the game's functions are direct calls again (a "large code model" setting had made
  every one an indirect call).
- Linux: timed waits no longer spin through their last millisecond.
- Direct3D's copies of its state into a command stream that the renderer never reads are skipped.

**Renderer**
- The game's shaders are built into the program, as on Android and the PS5: no shader build at the first
  start, and no `dxcompiler.dll` / DXC libraries in the release.
- The game's shaders read its memory and constants without 64-bit addresses, as plain uniform and storage
  buffers — lighter for the GPU (the Steam Deck's 3D scene pass went from 5–17 ms to about 3–4 ms).
- Strip index lists are kept in ordinary memory (no slow reads back from GPU-shared memory).
- The game's index buffers are drawn as Vulkan index buffers, so the GPU reuses transformed vertices.
- Whole-surface clears at the start of a pass are done as the pass's own clear.
- The renderer hands its work to the GPU every 128 draws as well as at the end of a frame, so each
  frame is ready in time for the screen's next refresh.

**Linux**
- RenderDoc is no longer loaded just because it is installed (SteamOS has it): it was hooking every
  Vulkan call of the session.

**Testing**
- An `izuna_overrides.toml` file beside the program (optional) sets any setting by name, for trying a
  switch without a new build; each one is logged. The log also counts the vblanks between frames.

## v0.2.2 — Android, Linux on AMD and Intel GPUs, Windows fixes

> [!WARNING]
> **Android performance is poor at the moment.** The Android build runs the game, but not well yet — it
> still has to be optimised. Treat it as an early preview.

**Android (new)**
- An APK for Android 11 and newer (64-bit ARM), tuned for Snapdragon — tested on an AYN Thor
  (Snapdragon 8 Gen 2). The game's shaders are built in.
- **Turnip drivers:** put a driver package (an AdrenoTools `.zip`, e.g. Balemuni's Aurora "Universal" or
  Purple Turnip) in `IzunaRecomp/drivers/`, start the app again and choose it in Settings › Display ›
  GPU driver. A package that needs a newer Android than the device's is listed but never loaded; one that
  keeps the game from starting is replaced by the system's driver at the next start. Qualcomm's own
  driver cannot run the game's shaders (they use 64-bit integers) — the app says so instead of staying black.
- The game's memory is shared with the GPU through an Android hardware buffer, as the renderer reads it
  in place.
- Full screen (no status bar over the picture), "All files access" for your copy of the game, Android's
  own folder picker in the hub, and a visible `IzunaRecomp` folder for settings, logs, mods, DLC, saves
  and drivers.
- **Touch controls**, off by default: TOUCH CONTROLS in the hub's main menu turns them on (after asking
  whether you really want to play Ninja Gaiden II that way). Moving and resizing them is still to come.

**Linux**
- **Draws on AMD and Intel GPUs, the Steam Deck included.** Their drivers cannot import the game's memory
  the way the renderer did on Windows (the picture stayed a rainbow); the game's memory is now built on
  Vulkan memory shared with the GPU instead — tested on the Steam Deck at a steady 60 fps.
- Auto resolution reads the real screen size: the Steam Deck draws its own 1280×800 instead of 1920×1080
  at 2×.
- (Since the replaced v0.2.1 AppImage:) any distribution from 2022 on, any desktop (X11 or Wayland), sound
  through PipeWire, PulseAudio or ALSA, controllers through udev, and a second build for processors
  without AVX2.

**Windows**
- A `default.xexp` in the game folder (another recompilation can leave one there) is ignored instead of
  applied as a title update, so the game no longer fails to start because of it.

**All**
- The renderer enables the "storage images without a format" features every translated shader declares,
  on any driver that offers them.

**PS5**
- Left unchanged: this release carries the same `IzunaRecomp-0.2.1-ps5.zip` as v0.2.1 (its source is
  the `v0.2.1-ps5` tag). The `default.xexp` fix and the rest of v0.2.2 are not in it.

**Known issues**
- **Android performance is poor** — the game runs, but not well yet; the Android build still has to be
  optimised.
- Android needs a Turnip driver: Qualcomm's own driver cannot run the game's shaders.
- Android's touch controls cannot be moved or resized yet.

## v0.2.1 — Linux, PS5, smoother starts, accuracy fixes

**Linux AppImage replaced (2026-11-07).** The first v0.2.1 AppImage could not
open a window ("No available video device"): its SDL had been built without
X11, Wayland, sound or udev support. The replacement has all of them, runs on
any Linux from 2022 on (glibc 2.35: Ubuntu 22.04+, Debian 12, Fedora 36+,
SteamOS, Bazzite, Arch...) under any desktop, carries a second build for
processors without AVX2, and ignores a `default.xexp` left in the game folder.
Please download it again.

**New**
- **PS5:** the app folder `PPSA94203` for consoles that run homebrew apps,
  with the game's shaders built in (no first-start wait). It can use the game
  files of an installed Ninja's Dawn for PS5 (`PPSA94202`).
- **Linux:** an AppImage (x86-64, glibc 2.38+). On the Steam Deck it runs
  natively, without Proton — the Windows build cannot draw under Proton.
- **Skip cutscenes with Cross / A** (or Start / Options) the first time you
  see them too — the game normally lets you skip only cutscenes you have
  already watched. On by default (Settings › Game).
- **Present mode:** FIFO (V-Sync), FIFO relaxed, Mailbox or Immediate
  (Settings › Display). None of them changes the game's speed.
- **Fullscreen refresh:** a fixed 144-170 Hz screen (no G-Sync / FreeSync) is
  switched to 120, 180 or 240 Hz in fullscreen, so 60 fps frames all show
  equally long; the desktop's rate comes back on exit (issue #1).
- **Audio delay (A/V sync):** the sound held back to meet the picture, 40 ms by
  default (issue #5: sounds heard before what makes them).

**Stutter**
- The game no longer starts while its pipelines are still being built: it
  waits, behind a "Preparing pipelines" bar, for the ones it is about to use.
  That is what made "skip intro videos" and first sessions stutter.
- The shipped pipeline list grew from 204 to 484.
- Video memory reserved up front by the card's size (512 MB to 1.5 GB): 1440p
  and 4K sessions no longer allocate in play.

**Accuracy** (the game's shaders are rebuilt once on the first start)
- Vertex colours in the order the console's Direct3D sets them at draw time:
  the fix for the health bar's damage drawn blue instead of red (issue #4).
- Direct3D 9's multiplication: 0 times anything is 0, infinity and NaN
  included, as on the Xbox 360's GPU — a NaN no longer erases geometry or
  colours (the cause of the missing part of Sonia's model, and a likely one
  for the chapter 4 rails, issue #6).
- Predicated exec blocks run only when their predicate says so.
- No fused multiply-adds where the console rounds the multiply first.


## v0.2.0 — the first public release (Windows)

IzunaRecomp's first release: Ninja Gaiden II recompiled for Windows and drawn
natively by the Kasumi Renderer. Compared with Ninja's Dawn, which it forks:

**Rendering**
- The Kasumi Renderer: the game's Direct3D calls drawn through Vulkan — no GPU
  emulation is built or linked. The game's shaders are recompiled from your
  copy at the first start (a progress bar shows it) and kept for later starts.
- Any resolution: Auto (your screen's) or 720p to 2160p, each a true
  resolution; anti-aliasing, anisotropic filtering, output filters.
- Threaded command recording and pipelines linked from per-shader parts, built
  ahead of their first draw; video memory reserved up front, so allocations do
  not stall play.

**Pacing and latency**
- V-Sync Auto follows the monitor (60, 120, 144 Hz…, G-Sync / FreeSync), or
  60 / 59.94 Hz; a high-resolution timer instead of spinning.
- Input latency Standard / Low; controllers polled on a thread of their own.

**The app**
- A hub (from the PS5 work) with every setting, an in-game menu, performance
  readouts, saves import and DLC; keyboard play with rebindable keys;
  PlayStation or Xbox prompts.
- Two builds in one: `ng2.exe` (AVX2) starts `ng2-compat.exe` on older CPUs.
- Crash minidumps; a Logging switch that turns all logging off.

**Steadiness**
- The game's file reads — including its large archive loads — run on an I/O
  thread instead of the game's main thread.
- Audio (XMA) and clock fixes; the cutscene audio fix keeps sound in step.

**Mods**
- A `Mods` folder and a MODS page: file replacement, texture replacement up to
  2048 × 2048, hiding draws by shader, Lua script mods, hooks on any game
  function (docs/MODDING.md).

**Known issues:** see the README.
