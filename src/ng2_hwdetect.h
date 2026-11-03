// The GPU this machine draws with, for the diagnostics report: its name and
// its own memory (DXGI on Windows; the PS5's is fixed).
//
// (This also sized the emulated GPU's texture cache on the first run; with no
// GPU emulation in the build that went in the clean-up after
// izuna-windows-test22.)

#pragma once

#include <cstdint>
#include <string>

namespace ng2 {

struct GpuInfo {
  bool valid = false;
  std::string name;
  uint64_t dedicated_bytes = 0;  // the adapter's own memory
  uint64_t budget_bytes = 0;     // what this process may use right now
  uint64_t used_bytes = 0;       // what this process is using right now
};

// Queries the adapter. Never throws; `valid` is false if DXGI is unavailable,
// in which case callers should leave the settings alone rather than guess.
GpuInfo DetectGpu();


}  // namespace ng2
