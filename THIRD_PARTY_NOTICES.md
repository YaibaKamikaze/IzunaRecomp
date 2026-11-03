# Third-party notices

## How the licences fit together

- **This repository's own source** (IzunaRecomp, and Ninja's Dawn it forks) is
  under the MIT licence ([LICENSE](LICENSE)).
- **The Windows release** (`ng2.exe`, `ng2-compat.exe`) links in the libraries
  below, statically. Their licence texts are in the release's `licenses/`
  folder (`tools/windows/make_release.sh` copies them). `dxcompiler.dll` (the
  DirectX Shader Compiler, used at the first start to compile the game's
  shaders for your GPU) ships beside the executables under its own licence.
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

## Release-only files

- `dxcompiler.dll` — DirectX Shader Compiler (Microsoft), under its licence
  (`licenses/dxcompiler.txt` in the release).
