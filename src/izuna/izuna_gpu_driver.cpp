// [IzunaRecomp] A chosen Vulkan driver package (Android) - see izuna_gpu_driver.h.
// After EternalSonataReprise's android_gpu_driver.cpp (Marco Andronaco, MIT):
// libadrenotools' custom driver load, its hook libraries from the app's
// native library folder, and a start marker that falls back to the system
// driver when a package dies before the renderer comes up.
#include "izuna/izuna_gpu_driver.h"

#if defined(__ANDROID__)

#include <android/api-level.h>
#include <dlfcn.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include <SDL3/SDL_system.h>
#include <adrenotools/driver.h>

#include <rex/logging.h>

// (rex/ui/vulkan/instance.h's hook, declared here: that header pulls in the
// RenderDoc API header, which the game's target does not see)
namespace rex::ui::vulkan {
using LoaderOpener = void* (*)();
void SetLoaderOpener(LoaderOpener opener);
}  // namespace rex::ui::vulkan

namespace izuna::gpu_driver {
namespace {

namespace fs = std::filesystem;

std::string g_id;      // the package to open ("" = the system driver)
std::string g_in_use;  // what the loader opened

fs::path Root() {
  const char* internal = SDL_GetAndroidInternalStoragePath();
  return internal ? fs::path(internal) / "gpu_drivers" : fs::path();
}

fs::path Marker() {
  const fs::path root = Root();
  return root.empty() ? root : root / "starting";
}

// A string field of a small JSON object ("key": "value"), unescaped enough
// for the names drivers carry.
std::string JsonString(const std::string& json, const std::string& key) {
  const size_t k = json.find("\"" + key + "\"");
  if (k == std::string::npos) return {};
  size_t q = json.find('"', json.find(':', k) + 1);
  if (q == std::string::npos) return {};
  std::string out;
  for (size_t i = q + 1; i < json.size() && json[i] != '"'; ++i) {
    if (json[i] == '\\' && i + 1 < json.size()) {
      ++i;
      if (json[i] == 'u' && i + 4 < json.size()) {  // (a \\uXXXX: keep ASCII, else a space)
        const int code = std::stoi(json.substr(i + 1, 4), nullptr, 16);
        out += code < 0x80 ? char(code) : ' ';
        i += 4;
        continue;
      }
    }
    out += json[i];
  }
  return out;
}

// The library the native library folder holds (where libmain.so is).
std::string NativeLibraryDir() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(&NativeLibraryDir), &info) || !info.dli_fname) return {};
  return fs::path(info.dli_fname).parent_path().string() + "/";
}

void* OpenLoader() {
  if (g_id.empty()) return nullptr;
  const fs::path dir = Root() / g_id;
  Package chosen;
  for (const Package& p : Installed())
    if (p.id == g_id) chosen = p;
  if (chosen.library.empty()) {
    REXLOG_WARN("GPU driver: the package \"{}\" is not installed - the system driver", g_id);
    return nullptr;
  }
  if (!chosen.usable) {
    REXLOG_WARN("GPU driver: \"{}\" needs Android API {} (this device: {}) - the system driver", chosen.name,
                chosen.min_api, android_get_device_api_level());
    return nullptr;
  }
  std::error_code ec;
  std::ofstream(Marker()).put('\n');  // (cleared when the renderer is up)
  const char* cache = SDL_GetAndroidCachePath();
  const std::string tmp = cache ? std::string(cache) + "/" : std::string();
  const std::string hooks = NativeLibraryDir();
  const std::string driver_dir = dir.string() + "/";
  void* handle = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
                                            tmp.empty() ? nullptr : tmp.c_str(), hooks.c_str(), driver_dir.c_str(),
                                            chosen.library.c_str(), nullptr, nullptr);
  if (!handle) {
    const char* error = dlerror();
    REXLOG_ERROR("GPU driver: \"{}\" did not load ({}) - the system driver", chosen.name, error ? error : "?");
    fs::remove(Marker(), ec);
    return nullptr;
  }
  if (!dlsym(handle, "vkGetInstanceProcAddr")) {
    REXLOG_ERROR("GPU driver: \"{}\" has no vkGetInstanceProcAddr - the system driver", chosen.name);
    fs::remove(Marker(), ec);
    return nullptr;
  }
  g_in_use = g_id;
  REXLOG_INFO("GPU driver: {} ({}, {})", chosen.name, chosen.id, chosen.library);
  return handle;
}

}  // namespace

std::vector<Package> Installed() {
  std::vector<Package> out;
  std::error_code ec;
  const fs::path root = Root();
  if (root.empty() || !fs::is_directory(root, ec)) return out;
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    if (!entry.is_directory(ec)) continue;
    std::ifstream f(entry.path() / "meta.json");
    if (!f) continue;
    std::stringstream ss;
    ss << f.rdbuf();
    Package p;
    p.id = entry.path().filename().string();
    p.name = JsonString(ss.str(), "name");
    p.library = JsonString(ss.str(), "libraryName");
    {  // "minApi": a number
      const std::string& j = ss.str();
      const size_t k = j.find("\"minApi\"");
      if (k != std::string::npos) p.min_api = std::atoi(j.c_str() + j.find(':', k) + 1);
    }
    p.usable = p.min_api <= android_get_device_api_level();
    if (p.name.empty()) p.name = p.id;
    if (p.library.empty() || !fs::is_regular_file(entry.path() / p.library, ec)) continue;
    out.push_back(p);
  }
  std::sort(out.begin(), out.end(), [](const Package& a, const Package& b) { return a.name < b.name; });
  return out;
}

std::string Install(const std::string& id) {
  std::string failed;
  std::error_code ec;
  const fs::path marker = Marker();
  if (!marker.empty() && fs::exists(marker, ec)) {
    // The last start loaded a package and never reached the renderer.
    fs::remove(marker, ec);
    failed = id;
    REXLOG_ERROR("GPU driver: \"{}\" did not start the game last time - the system driver this time", id);
  }
  g_id = failed.empty() ? id : std::string();
  rex::ui::vulkan::SetLoaderOpener(OpenLoader);
  return failed;
}

void RendererStarted() {
  std::error_code ec;
  const fs::path marker = Marker();
  if (!marker.empty()) fs::remove(marker, ec);
}

std::string InUse() { return g_in_use; }

}  // namespace izuna::gpu_driver

#else  // not Android: the system's driver only

namespace izuna::gpu_driver {
std::vector<Package> Installed() { return {}; }
std::string Install(const std::string&) { return {}; }
void RendererStarted() {}
std::string InUse() { return {}; }
}  // namespace izuna::gpu_driver

#endif
