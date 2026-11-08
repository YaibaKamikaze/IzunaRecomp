#!/bin/bash
# [IzunaRecomp] The Android app: tools/android/build.sh's libmain.so in an APK.
#   tools/android/make_apk.sh [destination]  -> <destination>/IzunaRecomp-<version>-android.apk
# No Gradle: the SDK's build tools directly - aapt2 (manifest, icons), javac and
# d8 (SDL's Java glue from the SDK tree, and tools/android/app/java), zipalign
# and apksigner. Signed with the project's key (IZUNA_KEYSTORE, made once in
# ~/ng2-ps5-deps/android if missing): an installed app updates only from an
# APK signed with the same key, so keep it.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
dest="${1:-$root/out/android-release}"
deps=${IZUNA_ANDROID_DEPS:-$HOME/ng2-ps5-deps/android}
sdk=$deps/sdk
bt=$sdk/build-tools/35.0.0
jar=$sdk/platforms/android-35/android.jar
ndk=${ANDROID_NDK:-$sdk/ndk/27.2.12479018}
export JAVA_HOME=${JAVA_HOME:-$deps/jdk17}
export PATH=$JAVA_HOME/bin:$PATH
lib=$root/out/android/libmain.so
[[ -f "$lib" ]] || { echo "no $lib (build it: tools/android/build.sh)" >&2; exit 1; }
version="$(tr -d ' \r\n' < "$root/IZUNA_VERSION")"
IFS=. read -r vmaj vmin vpat <<< "$version"
version_code=$(( vmaj * 10000 + vmin * 100 + ${vpat:-0} ))

work=$root/out/android-apk
rm -rf "$work"; mkdir -p "$work/res" "$work/classes" "$work/dex" "$work/lib/arm64-v8a" "$dest"

# Manifest and resources.
sed "s/@VERSION@/$version/; s/@VERSION_CODE@/$version_code/" "$here/app/AndroidManifest.xml" > "$work/AndroidManifest.xml"
cp -r "$here/app/res/." "$work/res/" 2>/dev/null || true
python3 - "$root/resources/izuna_256.png" "$work/res" <<'PY'
import os, sys
from PIL import Image
src = Image.open(sys.argv[1]).convert("RGBA")
for name, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
    d = os.path.join(sys.argv[2], f"mipmap-{name}"); os.makedirs(d, exist_ok=True)
    src.resize((size, size), Image.LANCZOS).save(os.path.join(d, "ic_launcher.png"))
PY
"$bt/aapt2" compile --dir "$work/res" -o "$work/res.zip"

# The app folder's starting files, as the AppImage's (tools/linux/make_appimage.sh):
# copied there by IzunaActivity, never over a file already there.
share=$work/assets/izuna
rel=$root/tools/windows/release
mkdir -p "$share/mods" "$share/DLC" "$share/Saves Import" "$share/hub/music" "$share/licenses"
sed "s/@VERSION@/$version/" "$here/README.txt" > "$share/README.txt"
cp "$root/LICENSE" "$share/LICENSE.txt"
cp "$rel/mods.txt" "$share/mods/README.txt"
cp "$rel/dlc.txt" "$share/DLC/README.txt"
cp "$rel/saves.txt" "$share/Saves Import/README.txt"
cp "$rel/music.txt" "$share/hub/music/README.txt"
mkdir -p "$share/drivers"
cp "$here/drivers.txt" "$share/drivers/README.txt"
cp -r "$root/ps5/package/hub/button_prompts" "$share/hub/button_prompts"
lic="$share/licenses"; sdk_src="$root/third_party/rexglue-sdk"
copy_license() { [[ -f "$2" ]] || { echo "missing license: $2" >&2; exit 1; }; cp "$2" "$lic/$1"; }
(here="$root/tools/windows"; sdk="$sdk_src"; eval "$(grep -E '^copy_license ' "$root/tools/windows/make_release.sh" | grep -v dxcompiler)")
copy_license libadrenotools.txt "$root/third_party/libadrenotools/LICENSE"
copy_license EternalSonataReprise-MIT.txt "$here/licenses/EternalSonataReprise-MIT.txt"
if find "$work/assets" -iname '*.wav' -o -iname '*.xex' -o -iname '*.ng2' | grep -q .; then
  echo "refusing: game files in the APK's assets" >&2; exit 1
fi

"$bt/aapt2" link -I "$jar" --manifest "$work/AndroidManifest.xml" -A "$work/assets" -o "$work/base.apk" \
  --min-sdk-version 30 --target-sdk-version 34 --version-code "$version_code" --version-name "$version" "$work/res.zip"

# Java: SDL's activity glue (its version matches the SDL linked into libmain.so) + ours.
sdl_java=$root/third_party/rexglue-sdk/thirdparty/sdl3/android-project/app/src/main/java
find "$sdl_java" "$here/app/java" -name '*.java' > "$work/sources.txt"
# (core-lambda-stubs.jar: what lambdas compile against with android.jar as the
# class library; d8 then rewrites them for Android, as Android's own build does)
if ! javac -nowarn -Xlint:none -source 8 -target 8 -bootclasspath "$jar:$bt/core-lambda-stubs.jar" \
     -d "$work/classes" @"$work/sources.txt" > "$work/javac.log" 2>&1; then
  grep -v "^warning: \[options\]" "$work/javac.log" >&2
  echo "error: the Java side did not compile" >&2
  exit 1
fi
find "$work/classes" -name '*.class' > "$work/classes.txt"
"$bt/d8" --release --min-api 30 --lib "$jar" --output "$work/dex" @"$work/classes.txt"

# The game.
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded -o "$work/lib/arm64-v8a/libmain.so" "$lib"
# libadrenotools' hook libraries: opened by name from the app's native library
# folder when a GPU driver package is in use (src/izuna/izuna_gpu_driver.cpp).
for hook in libmain_hook.so libfile_redirect_hook.so libgsl_alloc_hook.so libhook_impl.so; do
  [[ -f "$root/out/android/$hook" ]] || { echo "error: no $hook (tools/android/build.sh)" >&2; exit 1; }
  "$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded -o "$work/lib/arm64-v8a/$hook" "$root/out/android/$hook"
done

# Assemble, align, sign.
cp "$work/base.apk" "$work/unsigned.apk"
(cd "$work/dex" && zip -q -X "$work/unsigned.apk" classes*.dex)
(cd "$work" && zip -q -X "$work/unsigned.apk" lib/arm64-v8a/*.so)
"$bt/zipalign" -P 16 -f 4 "$work/unsigned.apk" "$work/aligned.apk"
keystore=${IZUNA_KEYSTORE:-$deps/izunarecomp-release.keystore}
passfile=${IZUNA_KEYSTORE_PASS:-$deps/izunarecomp-release.pass}
if [[ ! -f "$keystore" ]]; then
  umask 077
  head -c 24 /dev/urandom | base64 | tr -d '/+=' > "$passfile"
  keytool -genkeypair -keystore "$keystore" -storepass "$(cat "$passfile")" -alias izunarecomp \
    -keyalg RSA -keysize 4096 -validity 10000 -dname "CN=IzunaRecomp, O=The Izuna Project" >/dev/null 2>&1
  echo "==> made the signing key $keystore (password in $passfile) - back both up"
fi
apk="$dest/IzunaRecomp-$version-android.apk"
"$bt/apksigner" sign --ks "$keystore" --ks-pass "file:$passfile" --ks-key-alias izunarecomp --out "$apk" "$work/aligned.apk"
"$bt/apksigner" verify "$apk"
(cd "$dest" && sha256sum "$(basename "$apk")" > "$(basename "$apk").sha256")
echo "$apk ($(du -h "$apk" | cut -f1))"
