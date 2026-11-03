# Changelog

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
