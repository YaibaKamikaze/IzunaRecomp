#!/usr/bin/env bash
# [native renderer, M1] Build NG2's shader cache.
#
#   tools/native/build_shaders.sh
#
# 1. Decrypt and decompress the game's XEX (tools/xex_image.py) into
#    out/native-shaders/input/image.bin - Ninja Gaiden II keeps all of its
#    shaders (625 unique containers) in the executable image.
# 2. Translate every container with our XenosRecomp fork
#    (~/ng2-ps5-deps/tools/reeot-XenosRecomp, branch ng2, NG2_RECOMP mode) and
#    compile it to SPIR-V: out/native-shaders/shader_cache.cpp.
#
# Both outputs are made from the game: build output only, never committed or
# packaged as files of their own.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
xex=$root/assets/default.xex   # your own copy (assets/ is never committed)
xr_src=${XENOSRECOMP_SRC:-$HOME/ng2-ps5-deps/tools/reeot-XenosRecomp}
xr=$xr_src/build/XenosRecomp/XenosRecomp
out=$root/out/native-shaders

[[ -f $xex ]] || { echo "missing $xex" >&2; exit 2; }
[[ -x $xr ]] || { echo "missing $xr (build the XenosRecomp fork: cmake -S $xr_src -B $xr_src/build -G Ninja && ninja -C $xr_src/build)" >&2; exit 2; }
mkdir -p "$out/input"
rm -f "$out/input/image.bin"  # (an old symlink into the main copy)
python3 - "$root/tools" "$xex" "$out/input/image.bin" <<'EOF'
import sys
sys.path.insert(0, sys.argv[1])
from xex_image import XexImage
img = XexImage.load(sys.argv[2], sys.argv[3])
print(f"XEX image: {len(img.data)} bytes at 0x{img.image_base:08X}")
EOF
"$xr" "$out/input" "$out/shader_cache.cpp" "$xr_src/XenosRecomp/shader_common.h" | tail -3
ls -la "$out/shader_cache.cpp"
