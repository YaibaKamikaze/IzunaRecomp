#!/bin/bash
# [IzunaRecomp] The Android build (arm64-v8a), cross-compiled with the NDK:
#   tools/android/build.sh  -> out/android/libng2.so (inside the APK later)
# generated/ must exist (docs/BUILDING.md). Snapdragon-tuned: ARMv8.2-A with
# half-precision and dot product (Snapdragon 855 and newer), scheduled for the
# Cortex-A715 / X3 generation (Snapdragon 8 Gen 2, the test device).
# -fexperimental-library: the NDK's libc++ keeps std::jthread / stop_token
# (the SDK's timer queue) behind it.
set -euo pipefail
cd "$(dirname "$0")/../.."
deps=${IZUNA_ANDROID_DEPS:-$HOME/ng2-ps5-deps/android}
ndk=${ANDROID_NDK:-$deps/sdk/ndk/27.2.12479018}
out=out/android
arch="-march=armv8.2-a+fp16+dotprod+rcpc -mtune=cortex-a715"
if [ ! -f "$out/build.ninja" ]; then
  cmake -S . -B "$out" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
    "-DCMAKE_CXX_FLAGS=$arch -fexperimental-library" "-DCMAKE_C_FLAGS=$arch" \
    -DREXSDK_DIR="$PWD/third_party/rexglue-sdk" -DREXGLUE_OUTPUT_DIR="$PWD/$out/sdk" -DREXGLUE_STATIC_RUNTIME=ON
fi
# The SDK's codegen tool is built for the device and cannot run here; generated/
# is made with the host tool. Bring it up to date, then replace it with a no-op
# (as tools/native/ps5_build.sh does), so the build does not try to run it.
tool=$PWD/$out/sdk/rexglue
ninja -C "$out" sdk/rexglue > /dev/null
if ! grep -q "ng2 no-op codegen" "$tool" 2>/dev/null; then
  printf '#!/bin/sh\n# ng2 no-op codegen: generated/ is made with the host tool\nexit 0\n' > "$tool"
  chmod +x "$tool"
fi
ninja -C "$out" ${NINJA_ARGS:-} ng2
