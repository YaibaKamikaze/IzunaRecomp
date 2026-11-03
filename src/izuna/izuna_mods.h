// [IzunaRecomp] Mods: folders of game files that replace the game's own,
// found in Mods/ and loaded through the game's own loose-file path
// (izuna_mods.cpp; docs/MODDING.md).
//
//   Mods/
//     <a mod's folder>/
//       mod.toml        name, author, version, description; optional [replace]
//       files/          game files by their path in the game, e.g.
//                       files/char/ryu/ryu.gmd replaces char\ryu\ryu.gmd
//       scripts/main.lua  a script mod (izuna_scripts.h), optional
//       textures/<id>.png pictures drawn in place of the game's textures
//                       (by the texture's id: the texture dump names them)
//
// Which mods are on, and their order (the first one wins a file two mods
// both bring), are kept apart from the mods, in mods.toml in UserData: a mod's
// folder can be copied in or deleted without editing anything.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace izuna::mods {

struct Mod {
  std::string folder;  // its folder's name under Mods/: what the saved order refers to
  std::filesystem::path dir;
  std::string name, author, version, description;
  bool enabled = true;
  bool manifest = false;            // a mod.toml was read
  std::vector<std::string> files;   // the game files it brings, as paths in the game (backslashes)
  // [replace]: a game file loaded in place of another (target, source).
  std::vector<std::pair<std::string, std::string>> replace;
  std::string script;  // its script, from its folder (scripts/main.lua, or mod.toml's `script`), if any
  std::vector<std::string> textures;      // the ids its textures/ pictures replace
  std::vector<std::filesystem::path> texture_files;  // (the same order)
  std::vector<std::string> hide_shaders;  // mod.toml's hide_shaders: draws with these shaders left out
  std::vector<std::string> notes;  // what is wrong with it, or which of its files a mod above takes
};

struct Options {
  bool log_file_loads = false;  // every file the game loads, by name, in the log (to find what to replace)
  bool dump_textures = false;   // each texture the game draws saved to Texture Dump/, named by its id
};

std::filesystem::path Root();       // Mods/
std::filesystem::path StateFile();  // the order, what is on, the options
std::filesystem::path TextureDumpDir();

// The mods in Mods/, in their order (new folders at the end, on), with the
// conflicts between them noted.
std::vector<Mod> List(Options* options = nullptr);
bool Save(const std::vector<Mod>& mods, const Options& options);
// Deletes a mod's folder.
bool Remove(const Mod& mod, std::string* error);

// At the game's start, once its executable is in memory and before it runs:
// the files the enabled mods bring, mounted and planned (game_data: the
// game's own folder, for [replace]).
void Activate(const std::filesystem::path& game_data);

// The game's path per file id (empty until Activate has read them).
const std::vector<std::string>& FileNames();

// What Activate did, for the log and the overlay: mods on, files replaced.
struct Status {
  int mods = 0, files = 0;
};
Status Active();

}  // namespace izuna::mods
