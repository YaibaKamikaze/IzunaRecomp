// The game folder and the content packages beside it: is this Ninja Gaiden II
// (default.xex's title id), and is a file an Xbox 360 content package.
//
// (The PC port's setup screen also read and extracted disc images here; the
// hub takes a folder of the disc's files, so that went in the clean-up after
// izuna-windows-test22.)

#pragma once

#include <cstdint>
#include <filesystem>

namespace ng2 {

inline constexpr uint32_t kNg2TitleId = 0x544307D5;

struct DiscInfo {
  bool opened = false;    // the folder exists
  bool has_xex = false;   // default.xex is at its root
  uint32_t title_id = 0;  // 0 when the XEX could not be read
  uint32_t media_id = 0;

  bool IsNg2() const { return title_id == kNg2TitleId; }
  bool Usable() const { return opened && has_xex; }
};

// Reads default.xex's header only (not the folder's files).
DiscInfo InspectFolder(const std::filesystem::path& folder);

// An Xbox 360 content package (STFS: LIVE, PIRS or CON), by its magic - the
// packages have no extension.
bool IsStfsPackage(const std::filesystem::path& path);

}  // namespace ng2
