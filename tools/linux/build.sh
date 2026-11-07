#!/bin/bash
# [IzunaRecomp] The Linux desktop build (the AppImage's binary):
#   tools/linux/build.sh  -> out/linux-desktop/ng2
# generated/ must exist (docs/BUILDING.md); x86-64-v3, as Windows' ng2.exe.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=out/linux-desktop
if [ ! -f "$out/build.ninja" ]; then
  cmake -S . -B "$out" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
    "-DCMAKE_CXX_FLAGS=-stdlib=libstdc++ -march=x86-64-v3" -DCMAKE_C_FLAGS=-march=x86-64-v3 \
    -DREXSDK_DIR="$PWD/third_party/rexglue-sdk" -DREXGLUE_OUTPUT_DIR="$PWD/$out/sdk" -DREXGLUE_STATIC_RUNTIME=ON
fi
ninja -C "$out" ${NINJA_ARGS:-} ng2
