// [IzunaRecomp] The game's shaders, built from the player's own copy of the
// game when it starts, instead of shipped inside ng2.exe.
//
// Ninja Gaiden II keeps all of its shaders (625 Xenos shader containers) in
// its executable image. Once the runtime has loaded that image into guest
// memory - and before the game's first instruction runs - Prepare scans it
// for the containers, translates each with our XenosRecomp fork
// (third_party/xenosrecomp: Xenos microcode -> HLSL) and compiles it with
// DXC (dxcompiler.dll beside the executable: HLSL -> SPIR-V), on every core.
// The result is saved in the shader cache folder, named by the image's hash;
// a later start with the same game loads it in a moment.
//
// The Kasumi renderer then draws with these (native_draw.cpp, ShaderEntries):
// the same table layout tools/native/build_shaders.sh writes at build time,
// which is built into the PS5 and Linux host-test builds instead (no DXC
// there). On Windows this is the only source of shaders.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "native/shader_cache.h"

namespace izuna::shaders {

struct Table {
  std::vector<ShaderCacheEntry> entries;  // sorted by hash
  std::vector<uint32_t> layouts;          // the vertex fetch layout records (w0, w1)
  std::vector<uint8_t> spirv;             // every shader's SPIR-V, smol-v encoded, end to end
  uint64_t image_hash = 0;
};

// What Prepare did, for the log and the hub.
struct Report {
  bool ok = false;
  bool from_cache = false;
  size_t found = 0, built = 0, failed = 0;
  double seconds = 0;
  std::string error;
};

// The image (guest memory, `size` bytes from `image`): the table from
// `cache_dir` if one was saved for this image and this translator, else built
// and saved. `progress(done, total)` is called from the worker threads.
Report Prepare(const uint8_t* image, size_t size, const std::filesystem::path& cache_dir,
               const std::filesystem::path& dxc_dll, Table& out,
               const std::function<void(size_t, size_t)>& progress);

// `size` bytes from `src` (guest memory) copied, a page that cannot be read
// (left without access) as zeros instead of a fault.
std::vector<uint8_t> CopyReadable(const uint8_t* src, size_t size);

// The table the renderer draws with, once Prepare has made it (null: the
// built-in one). Set before the game's first frame; read-only after.
void SetActive(Table* table);
const Table* Active();

}  // namespace izuna::shaders
