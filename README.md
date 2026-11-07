# IzunaRecomp — The Izuna Project

> [!WARNING]
> **Played [NG2Recomp](https://github.com/gamboozino/ng2recomp) with the same game files?** Your game folder may now have a
> file called **`default.xexp`** next to `default.xex`. IzunaRecomp applies a `default.xexp` it finds as a title update,
> and that may stop the game from launching. Move `default.xexp` out of the game folder (on PS5:
> `PPSA94203/Game Data/`) before starting IzunaRecomp; put it back if NG2Recomp needs it.

## v0.2.1 adds a Linux AppImage (Steam Deck included), a PS5 build, smoother starts and accuracy fixes.
## This got released with the purpose of people playing around with it and seeing what can be improved, so if you have an issue with it please talk about it and send your logs, this will help the project. I am but one person using the water eating slop machine to develop this and don't have all the hardware possible to test it, I've only used a 4060 and an AMD CPU to try it so far, so reports from AMD and Intel Arc GPUs are especially welcome.


<img width="1280" height="720" alt="97089_dfcfff5534107b77" src="https://github.com/user-attachments/assets/aa900ec6-11f6-4c41-bf0e-e76c549b5be8" />


A static recompilation of **Ninja Gaiden II** (Xbox 360, title `544307D5`) for
Windows, Linux and the PS5, drawn natively by the **Kasumi Renderer** (Vulkan).

The game's PowerPC code is translated ahead of time into native x86-64, and
its Direct3D calls are answered by the Kasumi Renderer, which draws every frame
itself through Vulkan from the game's own shaders, recompiled for your GPU.
There is no emulator in it: no GPU emulation, no interpreter, no JIT.

> **AI disclosure.** The code, documents and tooling in this repository were
> written by an AI coding agent (Anthropic's Claude, through Claude Code),
> directed by the maintainer, who also tested every build on real hardware.
> Treat it like any other early-stage community project: expect bugs, read the
> code before relying on it, and report problems through issues.

IzunaRecomp started as a fork of
[Ninja's Dawn](https://github.com/FermataRest/Ninja-s-Dawn-A-Ninja-Gaiden-2-Recompilation-Project),
the PC recompilation of Ninja Gaiden II, and grew into its own project: the
emulated GPU was replaced by a native renderer, and the app, its settings,
its menus and its mod support were rebuilt around it. Gameplay and game logic
stay 1:1 with the Xbox 360 game.

---

## What you must bring

**This project ships no game data of any kind** — no textures, videos, sound,
saves, DLC or executable image from the disc. You provide all of it from your
own copy.

| | What | |
|---|---|---|
| **1** | **Your own copy of Ninja Gaiden II for Xbox 360**, extracted to a folder containing `default.xex` | Nothing works without it. |
| **2** | **Windows 10 or 11, 64-bit**, or **Linux** with glibc 2.38+ (SteamOS, Ubuntu 24.04+, Fedora 40+, Arch…), or **a PS5 that runs homebrew apps** (a jailbroken console with a homebrew launcher) | |
| **3** | On a PC, **a GPU with Vulkan** — NVIDIA GTX 900 / AMD RX 400 / Intel Arc or newer, with an up-to-date driver | |

Nothing else needs installing (no Visual C++ Redistributable).

## Getting started

**Linux / Steam Deck:** download `IzunaRecomp-<version>-x86_64.AppImage`, make
it executable (`chmod +x`) and run it — on a Steam Deck, add it to Steam as a
non-Steam game and run it as it is, without Proton. Settings, logs and mods
live in `~/.local/share/IzunaRecomp`. The rest of this section is the same.

**PS5:** download `IzunaRecomp-<version>-ps5.zip` and unzip it: you get a
folder named **`PPSA94203`**.

1. Copy your extracted game (`default.xex`, every `.ng2` archive, the videos
   and the rest of the disc's files) straight into **`PPSA94203/Game Data/`**,
   so that `default.xex` sits directly in it. If Ninja's Dawn for PS5
   (`PPSA94202`) is already installed with the game, you can skip this: the
   app uses that copy.
2. Copy the whole `PPSA94203` folder to **`/data/homebrew/`** on the console
   (FTP is the usual way).
3. Start it from your homebrew launcher; the hub opens, then **START GAME**.

Optional: DLC packages into `PPSA94203/DLC/`, Xbox 360 saves into
`PPSA94203/Saves Import/`. Logs are in `/data/homebrew/PPSA94203/logs/`. The
game's shaders are built into the PS5 app, so there is no first-start wait.

**Windows:**

1. Download `IzunaRecomp-<version>-windows.zip` from [Releases](../../releases)
   and unzip it anywhere.
2. Run `ng2.exe`. The hub asks for your game folder (the one with
   `default.xex`); it is remembered.
3. **START GAME.**

The first start takes longer: IzunaRecomp reads the game's shaders from your
copy and compiles them for your GPU (a progress bar shows how far along it
is). They are kept in `%LOCALAPPDATA%\IzunaRecomp\shaders`, so later starts
skip this.

`ng2.exe` is built for processors with AVX2 (Intel since 2013, every AMD
Ryzen); `ng2-compat.exe` is the same game for older processors, and `ng2.exe`
starts it by itself when needed. Keep both in the folder. The `README.txt` in
the release has the controls and the settings in detail.

## What's in it

- **The Kasumi Renderer** — native Vulkan rendering of the whole game, with
  the game's shaders recompiled from your copy at first start; any resolution
  (Auto: your screen's, or 720p to 2160p, each drawn at that true resolution),
  anti-aliasing, anisotropic filtering, output filters.
- **Frame pacing** — V-Sync Auto follows your monitor (60, 120, 144 Hz…,
  G-Sync / FreeSync), or locks the game's clock to 60 / 59.94 Hz; a Low input
  latency mode; the present mode of your choice (FIFO, FIFO relaxed, Mailbox,
  Immediate); in fullscreen, a 144-170 Hz screen switched to a refresh that
  60 fps divides, so frames show evenly.
- **No stutter from new pipelines at the start** — the game waits, behind a
  progress bar, for the pipelines it is about to use (a list of 480+ shipped
  with it, plus every one your own sessions met).
- **Skip cutscenes with Cross / A** (or Start), also the first time you see
  them — normally the game only lets you skip ones you've watched (on by
  default, Settings › Game).
- **A/V sync** — the sound held back to meet the picture (Settings › Sound).
- **A full app around the game** — a hub with settings, an in-game menu
  (Esc / F10, or hold View / Share), performance readouts, saves import, DLC.
- **Keyboard and controller** — play with either or both; every key can be
  rebound; PlayStation or Xbox button prompts.
- **Sound** — full-level stereo with a limiter, the 5.1 mix's bass brought
  into stereo, and a cutscene audio fix that keeps sound in step with the
  picture.
- **Mods** — a `Mods` folder and a MODS page in the hub: replace the game's
  files (models, motions, effects, stages, text, sound banks), replace
  textures with pictures up to 2048 × 2048, hide draws by their shader, Lua
  script mods, and hooks on any of the game's functions. See
  [docs/MODDING.md](docs/MODDING.md); examples are in [examples/mods](examples/mods).
- **Steadiness** — threaded command recording, pipelines linked from
  per-shader parts, asynchronous file reads off the game's main thread, crash
  minidumps, and a Logging switch (Settings › Developer) that turns all
  logging off.

## Known issues

- Glass reflections and glows in places (the tower in chapter 1) look off.
- There's occasional hitches and stutters, these will be improved upon.
- The Windows build cannot draw under Proton / Wine (its renderer cannot use
  the game's memory there): on Linux and the Steam Deck use the AppImage.
- The PS5 build is newer than the PC ones and less tested: report anything
  odd with the newest log from `/data/homebrew/PPSA94203/logs/`.

Report anything else through issues, with the `logs` folder from beside
`ng2.exe` (the newest `ng2_*.log`, and a `crash-*.dmp` if there is one).

## Building from source

See [docs/BUILDING.md](docs/BUILDING.md). In short: the ReXGlue SDK is fetched
and patched by `tools/setup_sdk.sh`, the game's code is recompiled from **your
own** `default.xex` (never committed — `generated/` is reproducible from it),
the Windows build is cross-compiled from Linux with Clang against the
headers and libraries of your own Build Tools for Visual Studio, and the PS5
build uses the ps5-plume-triangle toolchain.

## Credits

- **Ninja's Dawn** by TheSaltTrader — the PC recompilation IzunaRecomp forks.
- **ReXGlue SDK** by Tom Clay, derived from **Xenia** — the recompiler and
  runtime, as carried by the svr2011-ps5 project's `rexglue-ps5` branch.
- **XenosRecomp** (hedge-dev), through goliathret's reeot-XenosRecomp fork —
  the shader recompiler behind the Kasumi Renderer's shader translation.
- **The PS5 build** stands on [ps5-plume-triangle](https://github.com/Nazky/ps5-plume-triangle)
  by Nazky (the toolchain, the app startup and `libc.prx` of BlackBearReloaded's
  ps5-native-app-boilerplate), the PS5 payload SDK (John Törnblom; mihawk-99's fork) and mihawk-99's
  [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) (the RADV Vulkan driver),
  and elripalda's PS5 runtime; and on
  [Ninja's Dawn PS5](https://github.com/YaibaKamikaze/ninjas-dawn-ps5), the
  port it grew from.
- **[ng2recomp](https://github.com/gamboozino/ng2recomp)** by gamboozino —
  another Ninja Gaiden II recompilation, whose published research into the
  game's rendering (the Xenos multiply rule, predicate and control-flow
  behaviour) was a reference for IzunaRecomp's own fixes. No code of it is
  used: it is GPL-3.0, IzunaRecomp is MIT.
- Lua, Zstandard, smol-v, SDL, Dear ImGui, the Vulkan headers and Memory
  Allocator, DirectX Shader Compiler, and the Inter typeface — see
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Licence

IzunaRecomp's own source is under the MIT licence ([LICENSE](LICENSE)), as is
Ninja's Dawn, which it forks. Third-party components keep their own licences
([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). The PS5 **release binary**
also contains GPL-3.0-or-later components (the app startup and `libc.prx` from
ps5-native-app-boilerplate, via ps5-plume-triangle), so the PS5 release is
distributed under the terms of the **GNU GPL v3**; this repository, plus the
pinned upstream versions listed in THIRD_PARTY_NOTICES.md, is its source (the
PS5 zip's `licenses` folder has the details). No game material is covered by
any of these licences or included in this repository; the executable in each
release contains the game's code (on the PS5 also its shaders) in translated
form and is useful only with your own copy of the game.

Ninja Gaiden is a trademark of Koei Tecmo. IzunaRecomp is not affiliated with
Koei Tecmo, Microsoft or Sony. PlayStation and PS5 are trademarks of Sony
Interactive Entertainment.

<img width="1400" height="1000" alt="HWMupABXkAAt5eC" src="https://github.com/user-attachments/assets/79e3aff2-4e30-4cb7-a698-91b840723441" />

