#!/usr/bin/env bash
# Builds tools/hub_preview (the hub's pages to PNG on the build machine) against
# the desktop test build's runtime library (ninja -C out/linux first).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdk=$root/third_party/rexglue-sdk
mkdir -p "$root/out/hub_preview"
flags=(-std=gnu++23 -O2 -march=x86-64-v3 -DNG2_PS5_FONTS_DIR="\"$root/ps5/assets/fonts\"" -DNG2_PS5_HOST_TEST=1
       -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -D__PROSPERO__=1 -DREX_HAS_VULKAN=1 -DNG2_VERSION="\"preview\""
       -DNG2_PS5_VERSION="\"preview\"" -I"$root" -I"$root/src" -I"$sdk/thirdparty/imgui" -I"$sdk/thirdparty/stb"
       -I"$sdk/include" -I"$root/out/linux/rexglue-sdk/include" -I"$sdk/thirdparty/fmt/include"
       -I"$sdk/thirdparty/spdlog/include" -I"$sdk/thirdparty/simde" -isystem "$sdk/thirdparty/tomlplusplus/include")
srcs=("$root/tools/hub_preview/hub_preview.cpp" "$root/src/ps5/hub/hub_ui.cpp" "$root/src/ps5/hub/hub_backdrop.cpp"
      "$root/src/ps5/hub/hub_pages.cpp" "$root/src/ps5/hub/hub_settings.cpp" "$root/src/ps5/ps5_paths.cpp")
clang++-19 "${flags[@]}" "${srcs[@]}" -o "$root/out/hub_preview/hub_preview" \
  -L"$sdk/out/linux-amd64" -lrexruntime -Wl,-rpath,"$sdk/out/linux-amd64" -lpthread
echo "$root/out/hub_preview/hub_preview"
