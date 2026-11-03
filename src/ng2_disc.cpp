#include "ng2_disc.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace ng2 {
namespace {

uint32_t ReadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) |
         uint32_t(p[3]);
}

// XEX2 identity, straight out of the optional header table.
//
//   0x00  'XEX2'
//   0x14  optional header count
//   0x18  {key, value} pairs, big-endian
//
// Key 0x00040006 is XEX_HEADER_EXECUTION_INFO. Its low byte (6) is the size in
// dwords, which means `value` is a file offset rather than an inline value.
// The struct is media_id, version, base_version, title_id, then a byte each of
// platform / executable table / disc number / disc count.
//
// Verified against this disc: media 69F555CB, title 544307D5, disc 1 of 1.
struct XexIdentity {
  bool ok = false;
  uint32_t title_id = 0;
  uint32_t media_id = 0;
};

XexIdentity ParseXexIdentity(const uint8_t* data, size_t size) {
  XexIdentity id;
  if (size < 0x18 || std::memcmp(data, "XEX2", 4) != 0)
    return id;
  const uint32_t count = ReadBE32(data + 0x14);
  // A plausible table only; a corrupt count must not walk off the buffer.
  if (count > 256 || 0x18 + size_t(count) * 8 > size)
    return id;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* entry = data + 0x18 + size_t(i) * 8;
    if (ReadBE32(entry) != 0x00040006u)
      continue;
    const uint32_t offset = ReadBE32(entry + 4);
    if (size_t(offset) + 20 > size)
      return id;
    id.media_id = ReadBE32(data + offset);
    id.title_id = ReadBE32(data + offset + 0x0C);
    id.ok = true;
    return id;
  }
  return id;
}

// How much of the XEX has to be in hand for the header walk. The execution
// info on this disc sits at 0x5B44, well past the first page - reading only
// 0x1000 bytes would find the table and then fall off the end of the buffer.
constexpr size_t kXexHeaderBytes = 0x20000;

}  // namespace

DiscInfo InspectFolder(const fs::path& folder) {
  DiscInfo info;
  std::error_code ec;
  if (!fs::is_directory(folder, ec)) return info;
  info.opened = true;
  const auto xex = folder / "default.xex";
  if (!fs::is_regular_file(xex, ec)) return info;
  info.has_xex = true;
  std::ifstream in(xex, std::ios::binary);
  std::vector<uint8_t> buffer(kXexHeaderBytes);
  in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
  const auto id = ParseXexIdentity(buffer.data(), static_cast<size_t>(in.gcount()));
  info.title_id = id.title_id;
  info.media_id = id.media_id;
  return info;
}

bool IsStfsPackage(const fs::path& path) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec))
    return false;
  if (fs::file_size(path, ec) < 0x1000)
    return false;
  std::ifstream in(path, std::ios::binary);
  char magic[4] = {};
  if (!in.read(magic, sizeof(magic)))
    return false;
  return std::memcmp(magic, "LIVE", 4) == 0 || std::memcmp(magic, "PIRS", 4) == 0 ||
         std::memcmp(magic, "CON ", 4) == 0;
}

}  // namespace ng2
