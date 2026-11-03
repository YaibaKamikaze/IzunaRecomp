#!/usr/bin/env bash
# [native renderer] Build the PS5 app (out/ps5) without letting it run codegen.
#
#   tools/native/ps5_build.sh
#
# The generated rexglue.cmake makes the build re-run codegen whenever the
# codegen tool of the build tree is newer than generated/ - and in the PS5
# tree that tool (third_party/rexglue-sdk/out/ps5-amd64/rexglue) is built with
# the PS5 toolchain: it cannot run here (it crashes). Codegen is run by hand
# with the host tool (third_party/rexglue-sdk/out/linux-amd64/rexglue, then the
# three patch scripts - ps5/BUILD.md section 2), so the PS5 tree's tool is
# replaced by a no-op before building.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
tool=$root/third_party/rexglue-sdk/out/ps5-amd64/rexglue
# The tool is brought up to date first (an SDK change relinks it, which would
# otherwise happen during the build below and put the real one back), then
# replaced by the no-op whenever it is not that already.
ninja -C "$root/out/ps5" "$tool" > /dev/null
if ! grep -q "ng2 no-op codegen" "$tool" 2>/dev/null; then
  printf '#!/bin/sh\n# ng2 no-op codegen: generated/ is made with the host tool (tools/native/ps5_build.sh)\nexit 0\n' > "$tool"
  chmod +x "$tool"
fi
ninja -C "$root/out/ps5" -j6 ng2
