#!/bin/bash
# [IzunaRecomp] The Windows build, on Linux (tools/windows/README.md):
#   tools/windows/build.sh [Release|RelWithDebInfo]   -> out/windows/ng2.exe
# NINJA_ARGS="-k 0" keeps going past errors.
#
# As the PS5 build (tools/native/ps5_build.sh): the codegen tool of this tree
# is a Windows program that cannot run here, so it is brought up to date and
# then replaced by a no-op; generated/ is made with the Linux tool (the
# codegen steps in CLAUDE.md).
set -euo pipefail
cd "$(dirname "$0")/../.."
cfg=${1:-Release}
# IZUNA_CPU=compat: ng2-compat.exe (x86-64-v2) in out/windows-compat; else
# ng2.exe (x86-64-v3) in out/windows (toolchain.cmake).
out=out/windows
if [ "${IZUNA_CPU:-}" = compat ]; then
  export IZUNA_MARCH=x86-64-v2
  out=out/windows-compat
else
  export IZUNA_MARCH=x86-64-v3
fi
export PATH="$HOME/ng2-ps5-deps/tools/wintc/bin:$PATH"  # (lld-link, llvm-rc, llvm-ar, llvm-mt)
if [ ! -f "$out/build.ninja" ]; then
  cmake -S . -B "$out" -G Ninja -DCMAKE_BUILD_TYPE="$cfg" \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/tools/windows/toolchain.cmake" \
    -DREXSDK_DIR="$PWD/third_party/rexglue-sdk" -DREXGLUE_OUTPUT_DIR="$PWD/$out/sdk"
fi
tool=$PWD/$out/sdk/rexglue.exe
# (the SDK and its tool - ninja names it relative to the tree; quiet unless it fails)
if ! ninja -C "$out" ${NINJA_ARGS:-} sdk/rexglue.exe > "$out/sdk_build.log" 2>&1; then
  grep -E "error|FAILED" "$out/sdk_build.log" | head -40
  exit 1
fi
if ! grep -q "ng2 no-op codegen" "$tool" 2>/dev/null; then
  printf '#!/bin/sh\n# ng2 no-op codegen: generated/ is made with the Linux tool (tools/windows/build.sh)\nexit 0\n' > "$tool"
  chmod +x "$tool"
fi
ninja -C "$out" ${NINJA_ARGS:-} ng2
