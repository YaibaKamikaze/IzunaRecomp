#!/usr/bin/env bash
# [IzunaRecomp] The Windows release folder (and its .zip) from a built ng2.exe:
#
#   tools/windows/make_release.sh [ng2.exe] [destination folder] [ng2-compat.exe]
#
# ng2.exe is the x86-64-v3 build (out/windows), ng2-compat.exe the x86-64-v2
# one for older CPUs (IZUNA_CPU=compat tools/windows/build.sh ->
# out/windows-compat): ng2.exe starts it by itself on a CPU without AVX2.
#
# Makes <destination>/IzunaRecomp-<version>-windows/ and
# <destination>/IzunaRecomp-<version>-windows.zip. Nothing of the game is in
# it: no game files, no music from the game (hub/music has only its README;
# the PS5 package's theme.wav is the game's soundtrack and stays out), and not
# ng2.pdb (kept with the build in ~/ng2-ps5-deps/builds to read crash dumps).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
exe="${1:-$root/out/windows/ng2.exe}"
dest="${2:-$root/out/release}"
compat="${3:-$root/out/windows-compat/ng2-compat.exe}"
version="$(tr -d ' \r\n' < "$root/IZUNA_VERSION")"
name="IzunaRecomp-$version-windows"
out="$dest/$name"

[[ -f "$exe" ]] || { echo "no $exe (build it: tools/windows/build.sh)" >&2; exit 1; }
[[ -f "$compat" ]] || { echo "no $compat (build it: IZUNA_CPU=compat tools/windows/build.sh)" >&2; exit 1; }
rm -rf "$out" "$dest/$name.zip"
mkdir -p "$out"

cp "$exe" "$out/ng2.exe"
cp "$compat" "$out/ng2-compat.exe"
# DXC, which ng2.exe loads at startup to compile the shaders it translates
# from the player's game (src/izuna/izuna_shaders.cpp). Not in the repo: the
# unmodified release build, kept with the XenosRecomp fork.
dxc="${DXC_DLL:-$HOME/ng2-ps5-deps/tools/reeot-XenosRecomp/thirdparty/dxc-bin/bin/x64/dxcompiler.dll}"
[[ -f "$dxc" ]] || { echo "no $dxc (set DXC_DLL to DXC 1.8's x64 dxcompiler.dll)" >&2; exit 1; }
cp "$dxc" "$out/dxcompiler.dll"
sed "s/@VERSION@/$version/" "$here/release/README.txt" > "$out/README.txt"
cp "$root/LICENSE" "$out/LICENSE.txt"

# The folders the hub reads, each with its README.
mkdir -p "$out/mods" "$out/DLC" "$out/Saves Import" "$out/hub/music"
cp "$here/release/mods.txt" "$out/mods/README.txt"
cp "$here/release/dlc.txt" "$out/DLC/README.txt"
cp "$here/release/saves.txt" "$out/Saves Import/README.txt"
cp "$here/release/music.txt" "$out/hub/music/README.txt"
cp -r "$root/ps5/package/hub/button_prompts" "$out/hub/button_prompts"

# The libraries' licenses (what ng2.exe has linked in).
lic="$out/licenses"
mkdir -p "$lic"
sdk="$root/third_party/rexglue-sdk"
copy_license() {  # name source
  [[ -f "$2" ]] || { echo "missing license: $2" >&2; exit 1; }
  cp "$2" "$lic/$1"
}
copy_license rexglue-sdk.txt "$sdk/LICENSE"
copy_license lua.txt "$root/third_party/lua/LICENSE"
copy_license zstd.txt "$root/third_party/zstd/LICENSE"
copy_license smol-v.txt "$root/third_party/smol-v/LICENSE.txt"
copy_license XenosRecomp.md "$root/third_party/xenosrecomp/LICENSE.md"
copy_license dxcompiler.txt "$here/release/dxcompiler-license.txt"
copy_license SDL3.txt "$sdk/thirdparty/sdl3/LICENSE.txt"
copy_license imgui.txt "$sdk/thirdparty/imgui/LICENSE.txt"
copy_license fmt.txt "$sdk/thirdparty/fmt/LICENSE"
copy_license spdlog.txt "$sdk/thirdparty/spdlog/LICENSE"
copy_license xxHash.txt "$sdk/thirdparty/xxHash/LICENSE"
copy_license tomlplusplus.txt "$sdk/thirdparty/tomlplusplus/LICENSE"
copy_license utfcpp.txt "$sdk/thirdparty/utfcpp/LICENSE"
copy_license cli11.txt "$sdk/thirdparty/cli11/LICENSE"
copy_license o1heap.txt "$sdk/thirdparty/o1heap/LICENSE"
copy_license glslang.txt "$sdk/thirdparty/glslang/LICENSE.txt"
copy_license spirv-tools.txt "$sdk/thirdparty/spirv-tools/LICENSE"
copy_license vulkan-loader.txt "$sdk/thirdparty/vulkan-loader/LICENSE.txt"
copy_license vulkan-memory-allocator.txt "$sdk/thirdparty/vulkan-memory-allocator/LICENSE.txt"
copy_license FFmpeg.md "$sdk/thirdparty/FFmpeg/LICENSE.md"
copy_license FFmpeg-LGPLv2.1.txt "$sdk/thirdparty/FFmpeg/COPYING.LGPLv2.1"
copy_license font-Cinzel-OFL.txt "$root/ps5/assets/fonts/OFL-Cinzel.txt"
copy_license font-Inter-OFL.txt "$root/ps5/assets/fonts/OFL-Inter.txt"

# Windows line endings for the text files (Notepad shows them either way; older
# editors do not).
find "$out" -name '*.txt' -print0 | while IFS= read -r -d '' f; do
  sed -i 's/\r$//; s/$/\r/' "$f"
done

# Nothing of the game may be in it.
if find "$out" -iname '*.wav' -o -iname '*.xex' -o -iname '*.ng2' -o -iname '*.pdb' | grep -q .; then
  echo "refusing: game or debug files in $out" >&2
  exit 1
fi

(cd "$dest" && rm -f "$name.zip" && zip -qr9 "$name.zip" "$name")
(cd "$out" && sha256sum ng2.exe ng2-compat.exe dxcompiler.dll) > "$dest/$name.sha256"
echo "$out"
echo "$dest/$name.zip ($(du -h "$dest/$name.zip" | cut -f1))"
