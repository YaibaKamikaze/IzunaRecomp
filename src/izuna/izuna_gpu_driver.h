// [IzunaRecomp] Android: a Vulkan driver package the player chose - Turnip,
// Mesa's driver for Adreno - in place of the system's (Qualcomm's has no
// 64-bit integers in shaders, which every one of the game's translated shaders
// uses: test52). Packages are the AdrenoTools kind (a .zip with meta.json and
// the driver library), put in the app folder's drivers/ and installed by the
// activity into the app's private storage (IzunaActivity.installDrivers):
//   <internal>/gpu_drivers/<package>/meta.json + the library
// The setting gpu_driver names the package ("" = the system driver); it takes
// effect at the next start. libadrenotools opens it (the SDK's loader hook,
// rex/ui/vulkan/instance.h SetLoaderOpener).
#pragma once

#include <string>
#include <vector>

namespace izuna::gpu_driver {

struct Package {
  std::string id;       // its folder's name: what the setting stores
  std::string name;     // meta.json's name
  std::string library;  // meta.json's libraryName
  int min_api = 0;      // meta.json's minApi: the Android version (API level) it needs
  bool usable = true;   // this device's Android is new enough for it
};

// The installed packages, by name.
std::vector<Package> Installed();

// Before the Vulkan instance: use the package `id` names ("" = the system
// driver). If the last start died between loading a package and the renderer
// coming up, the system driver instead - returns the package that failed then
// (to tell the player), else "".
std::string Install(const std::string& id);

// The renderer is up: the chosen driver works (clears the start marker).
void RendererStarted();

// The package in use, or "" for the system driver.
std::string InUse();

}  // namespace izuna::gpu_driver
