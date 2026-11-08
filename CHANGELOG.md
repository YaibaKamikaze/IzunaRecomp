# Changelog

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
