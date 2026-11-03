// [IzunaRecomp] Where the game's shaders are not built at startup (the PS5:
// no DXC; Linux for now): the built-in table only (izuna_shaders.h).
#include "izuna/izuna_shaders.h"

#include <cstring>

namespace izuna::shaders {

Report Prepare(const uint8_t*, size_t, const std::filesystem::path&, const std::filesystem::path&, Table&,
               const std::function<void(size_t, size_t)>&) {
  Report r;
  r.error = "not built at startup on this platform";
  return r;
}

std::vector<uint8_t> CopyReadable(const uint8_t* src, size_t size) {
  std::vector<uint8_t> out(size);
  if (src) std::memcpy(out.data(), src, size);
  return out;
}

void SetActive(Table*) {}
const Table* Active() { return nullptr; }

}  // namespace izuna::shaders
