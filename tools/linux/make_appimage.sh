#!/bin/bash
# [IzunaRecomp] The Linux release: an AppImage of the desktop build.
#   tools/linux/make_appimage.sh [ng2 binary] [destination]
#     -> <destination>/IzunaRecomp-<version>-x86_64.AppImage
# The binary: out/linux-desktop/ng2 (tools/linux/build.sh). DXC's Linux
# libraries (libdxcompiler.so, libdxil.so) go beside it - the shaders are
# compiled from the player's copy at the first start, as on Windows. Licences:
# the Windows release's list (tools/windows/make_release.sh). appimagetool:
# APPIMAGETOOL, or downloaded once into ~/ng2-ps5-deps/tools.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
bin="${1:-$root/out/linux-desktop/ng2}"
dest="${2:-$root/out/linux-release}"
version="$(tr -d ' \r\n' < "$root/IZUNA_VERSION")"
dxc_dir="${DXC_LINUX_DIR:-$HOME/ng2-ps5-deps/tools/reeot-XenosRecomp/thirdparty/dxc-bin/lib/x64}"
[[ -f "$bin" ]] || { echo "no $bin (build it: tools/linux/build.sh)" >&2; exit 1; }
[[ -f "$dxc_dir/libdxcompiler.so" ]] || { echo "no $dxc_dir/libdxcompiler.so (set DXC_LINUX_DIR)" >&2; exit 1; }

app="$dest/IzunaRecomp.AppDir"
rm -rf "$app"
mkdir -p "$app/usr/bin" "$app/usr/share/izunarecomp"
cp "$bin" "$app/usr/bin/ng2"
strip --strip-unneeded "$app/usr/bin/ng2" 2>/dev/null || true
cp "$dxc_dir/libdxcompiler.so" "$app/usr/bin/"
[[ -f "$dxc_dir/libdxil.so" ]] && cp "$dxc_dir/libdxil.so" "$app/usr/bin/"
cp "$here/AppRun" "$app/AppRun"
chmod +x "$app/AppRun"
cp "$here/izunarecomp.desktop" "$app/izunarecomp.desktop"
cp "$root/resources/izuna_256.png" "$app/izunarecomp.png"

share="$app/usr/share/izunarecomp"
sed "s/@VERSION@/$version/" "$here/README.txt" > "$share/README.txt"
cp "$root/LICENSE" "$share/LICENSE.txt"
rel="$root/tools/windows/release"
mkdir -p "$share/mods" "$share/DLC" "$share/Saves Import" "$share/hub/music"
cp "$rel/mods.txt" "$share/mods/README.txt"
cp "$rel/dlc.txt" "$share/DLC/README.txt"
cp "$rel/saves.txt" "$share/Saves Import/README.txt"
cp "$rel/music.txt" "$share/hub/music/README.txt"
cp -r "$root/ps5/package/hub/button_prompts" "$share/hub/button_prompts"
lic="$share/licenses"
mkdir -p "$lic"
sdk="$root/third_party/rexglue-sdk"
copy_license() {  # name source
  [[ -f "$2" ]] || { echo "missing license: $2" >&2; exit 1; }
  cp "$2" "$lic/$1"
}
# (the same list as the Windows release, from its script)
(here="$root/tools/windows"; eval "$(grep -E '^copy_license ' "$root/tools/windows/make_release.sh")")

# Nothing of the game may be in it.
if find "$app" -iname '*.wav' -o -iname '*.xex' -o -iname '*.ng2' -o -iname '*.pdb' | grep -q .; then
  echo "refusing: game or debug files in $app" >&2
  exit 1
fi

tool="${APPIMAGETOOL:-$HOME/ng2-ps5-deps/tools/appimagetool-x86_64.AppImage}"
if [[ ! -x "$tool" ]]; then
  curl -sSL -o "$tool" https://github.com/AppImage/appimagetool/releases/download/1.9.0/appimagetool-x86_64.AppImage
  chmod +x "$tool"
fi
out="$dest/IzunaRecomp-$version-x86_64.AppImage"
rm -f "$out"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$tool" --no-appstream "$app" "$out" >/dev/null
(cd "$dest" && sha256sum "$(basename "$out")" > "$(basename "$out").sha256")
echo "$out ($(du -h "$out" | cut -f1))"
