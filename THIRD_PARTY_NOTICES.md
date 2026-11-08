# Third-party notices

## How the licences fit together

- **This repository's own source** (IzunaRecomp, and Ninja's Dawn it forks) is
  under the MIT licence ([LICENSE](LICENSE)).
- **The Windows release** (`ng2.exe`, `ng2-compat.exe`) links in the libraries
  below, statically. Their licence texts are in the release's `licenses/`
  folder (`tools/windows/make_release.sh` copies them). `dxcompiler.dll` (the
  DirectX Shader Compiler, used at the first start to compile the game's
  shaders for your GPU) ships beside the executables under its own licence.
- **The PS5 release** (the `PPSA94203` app folder) links in the same
  libraries (not DXC or the Vulkan loader), plus Mesa's RADV driver for the
  PS5 (MIT and Mesa's other licences). Its app startup (`app_crt.cpp`,
  `app_cpp_runtime.cpp`, linked into `eboot.bin`) and `sce_module/libc.prx`
  are BlackBearReloaded's ps5-native-app-boilerplate, as carried by Nazky's
  ps5-plume-triangle, under **GPL-3.0-or-later**: the PS5 release is therefore
  distributed under the terms of the GNU GPL v3, with this repository plus the
  pinned revisions below as its source. The licence texts and a notice are in
  the app's `licenses/` folder (`ps5/tools/ps5_package.sh` copies them).
- **FFmpeg** is under the LGPL v2.1 (no GPL parts are enabled). Its
  corresponding source is the FFmpeg submodule of the ReXGlue SDK at the
  commit below plus [patches/ffmpeg-izuna.patch](patches/ffmpeg-izuna.patch);
  this repository and `tools/setup_sdk.sh` rebuild the whole executable, so the
  release can be relinked against a modified FFmpeg.
- **No game material** is covered by any of these licences or included in the
  repository or its releases. The executables contain the game's code in
  translated form and are useful only with your own copy of the game.

## Pinned upstream revisions (v0.2.0)

| Component | Source | Revision |
|---|---|---|
| ReXGlue SDK (Tom Clay; derived from Xenia) | https://github.com/elripalda/svr2011-ps5, branch `rexglue-ps5` | `f9b13da60c76` + [patches/rexglue-izuna.patch](patches/rexglue-izuna.patch) |
| FFmpeg (LGPL v2.1) | SDK submodule `thirdparty/FFmpeg` | `0604b464c7cb` + [patches/ffmpeg-izuna.patch](patches/ffmpeg-izuna.patch) |
| SDL 3 (zlib) | SDK submodule `thirdparty/sdl3` | `8bf3b7215ad9` |
| Dear ImGui (MIT) | SDK submodule `thirdparty/imgui` | `6d910d5487d1` |
| glslang (BSD / Apache 2.0) | SDK submodule `thirdparty/glslang` | `f4f1d8a352ca` |
| SPIRV-Tools (Apache 2.0) | SDK submodule `thirdparty/spirv-tools` | `9a49b0883b9b` |
| Vulkan Loader (Apache 2.0) | SDK submodule `thirdparty/vulkan-loader` | `5f157b62e333` |
| Vulkan Memory Allocator (MIT) | SDK submodule `thirdparty/vulkan-memory-allocator` | `1d8f600fd424` |
| {fmt} (MIT) | SDK submodule `thirdparty/fmt` | `407c905e45ad` |
| spdlog (MIT) | SDK submodule `thirdparty/spdlog` | `79524ddd08a4` |
| xxHash (BSD 2-clause) | SDK submodule `thirdparty/xxHash` | `e626a72bc232` |
| toml++ (MIT) | SDK submodule `thirdparty/tomlplusplus` | `30172438cee6` |
| utfcpp (BSL 1.0) | SDK submodule `thirdparty/utfcpp` | `63d64de49fd6` |
| CLI11 (BSD 3-clause) | SDK submodule `thirdparty/cli11` | `bfffd37e1f80` |
| o1heap (MIT) | SDK submodule `thirdparty/o1heap` | `388a73fd9007` |
| SIMDe (MIT) | SDK submodule `thirdparty/simde` | `71fd833d9666` |
| libmspack (LGPL v2.1) | SDK submodule `thirdparty/libmspack` | `305907723a4e` |

## In this repository

| Component | Where | Licence |
|---|---|---|
| XenosRecomp (hedge-dev), from goliathret's reeot-XenosRecomp fork with IzunaRecomp's changes | [third_party/xenosrecomp](third_party/xenosrecomp) (fork commit in `FORK_COMMIT.txt`) | MIT |
| Lua 5.4 | [third_party/lua](third_party/lua) | MIT |
| Zstandard (decoder) | [third_party/zstd](third_party/zstd) | BSD |
| smol-v | [third_party/smol-v](third_party/smol-v) | MIT / public domain |
| stb_image | [src/third_party](src/third_party) | MIT / public domain |
| RenderDoc in-application API header | [src/third_party/renderdoc_app.h](src/third_party/renderdoc_app.h) | MIT |
| Inter, Cinzel typefaces | [ps5/assets/fonts](ps5/assets/fonts) | SIL Open Font Licence 1.1 |

### PS5 toolchain and runtime (v0.2.1)

| Component | Source | Revision |
|---|---|---|
| ps5-plume-triangle (Nazky): toolchain, app startup and `libc.prx` (GPL-3.0-or-later, BlackBearReloaded's ps5-native-app-boilerplate) | https://github.com/Nazky/ps5-plume-triangle | `d54d9adb084b` |
| PS5 payload SDK (John Törnblom; mihawk-99's fork, GPL-3.0; import stubs only are linked) | https://github.com/mihawk-99/PS5_PayloadSDK | `95c08f27386f` |
| Mesa RADV for the PS5 (mihawk-99; MIT and Mesa's licences) | https://github.com/mihawk-99/PS5_Mesa | `0b2d6d1a61d9` |

### Android (v0.2.2)

| Component | Where | Licence |
|---|---|---|
| libadrenotools (Billy Laws), with linkernsbypass | [third_party/libadrenotools](third_party/libadrenotools) (upstream `8fae8ce254df`) | BSD-2-Clause |
| SDL's Android Java glue | the SDK's `thirdparty/sdl3/android-project` | zlib |
| GPU driver loading after EternalSonataReprise (Marco Andronaco) | [src/izuna/izuna_gpu_driver.cpp](src/izuna/izuna_gpu_driver.cpp) | MIT ([tools/android/licenses](tools/android/licenses)) |

The APK carries these licences, and the ones of the libraries it links (as the
Windows release), in its `IzunaRecomp/licenses` folder. No GPU driver ships with
it: Turnip packages are the player's own download.

## Release-only files

- `dxcompiler.dll` (Windows), `libdxcompiler.so` and `libdxil.so` (the Linux
  AppImage) — DirectX Shader Compiler (Microsoft), under its licence
  (`licenses/dxcompiler.txt` in the release).

## Reference, not code

- [ng2recomp](https://github.com/gamboozino/ng2recomp) by gamboozino (GPL-3.0):
  its published notes on Ninja Gaiden II's rendering were a reference for
  IzunaRecomp's own fixes. None of its code is in IzunaRecomp.
