# Building IzunaRecomp from source

The released executables are built exactly this way. Everything runs on a
**Linux** machine (the Windows build is cross-compiled); you need **your own
copy of Ninja Gaiden II** — its `default.xex` is the input the game's code is
recompiled from, and nothing derived from it is ever committed.

Tools: Clang/LLVM 19 (`clang`, `clang++`, `lld-link`, `llvm-rc`, `llvm-ar`,
`llvm-mt`), CMake 3.25+, Ninja, Python 3, Git. The scripts use
`~/ng2-ps5-deps` as their work folder for things that are not part of the
repository (the copied Windows SDK, toolchain links).

## 1. The ReXGlue SDK

```
tools/setup_sdk.sh
```

Clones the SDK at the pinned revision into `third_party/rexglue-sdk` (with its
submodules) and applies `patches/rexglue-izuna.patch` and
`patches/ffmpeg-izuna.patch`.

## 2. The game's code, recompiled

```
mkdir -p assets && cp "/path/to/your/game folder/default.xex" assets/
```

Build the codegen tool for this machine and run it as in
[ps5/BUILD.md](../ps5/BUILD.md), section 2 ("Generate the recompiled code"),
then the post-codegen steps:

```
third_party/rexglue-sdk/out/linux-amd64/rexglue codegen ng2_manifest.toml
python3 local/diag/patch_missed_regs.py
python3 local/diag/patch_scanguard.py
python3 ps5/tools/ps5_patch_generated.py generated
python3 tools/izuna/gen_hook_thunks.py
```

This writes `generated/` (about 580 MB). It is game-derived: it stays on your
machine (`.gitignore` keeps it out). For v0.2.0 these steps were checked to
reproduce, function for function, the code the release was built from.

## 3. The Windows build

Follow [tools/windows/README.md](../tools/windows/README.md): copy the x64
headers and libraries of your own *Build Tools for Visual Studio 2022*
("Desktop development with C++") to `~/ng2-ps5-deps/winsdk`, run
`tools/windows/case_links.py` on it once, then:

```
tools/windows/build.sh                     # out/windows/ng2.exe        (x86-64-v3)
IZUNA_CPU=compat tools/windows/build.sh    # out/windows-compat/ng2-compat.exe (x86-64-v2)
tools/windows/make_release.sh out/windows/ng2.exe <destination> out/windows-compat/ng2-compat.exe
```

`make_release.sh` assembles the release folder and zip: the two executables,
`dxcompiler.dll` (DXC 1.8, x64: `DXC_DLL=` its path), the README, the folders the hub reads
and the libraries' licences. It refuses to package any game file.

## 4. The Linux build (the AppImage)

```
tools/linux/sysroot/make_sysroot.sh        # once: ~/ng2-ps5-deps/linux-sysroot
tools/linux/build.sh                       # out/linux-desktop/ng2         (x86-64-v3)
IZUNA_CPU=compat tools/linux/build.sh      # out/linux-desktop-compat/ng2  (x86-64-v2)
tools/linux/make_appimage.sh               # out/linux-release/IzunaRecomp-<version>-x86_64.AppImage
```

The build compiles against an Ubuntu 22.04 sysroot that `make_sysroot.sh`
fetches from the Ubuntu archive (no root needed): glibc 2.35 and GCC 12's
libstdc++ (linked in), so the binaries run on any distribution from 2022 on,
plus the headers of X11, Wayland, PipeWire, PulseAudio, ALSA, udev and D-Bus,
so SDL builds all of those backends and loads whichever the player's system
has. `build.sh` refuses an SDL without them, or a binary that needs more than
glibc 2.35. The AppImage carries both binaries; its `AppRun` starts the compat
one on a processor without AVX2. It also needs DXC 1.8's Linux libraries
(`libdxcompiler.so`, `libdxil.so`; `DXC_LINUX_DIR=` their folder) and downloads
appimagetool 1.9.0 once.

The game's shaders are not built here: `ng2.exe` recompiles them from the
player's copy at the first start (`src/izuna/izuna_shaders.cpp`, with the
XenosRecomp fork in `third_party/xenosrecomp`).

## 5. The PS5 build

Set up the toolchain as in [ps5/BUILD.md](../ps5/BUILD.md), section 1
(`ps5/tools/setup-linux.sh` builds Nazky's ps5-plume-triangle in
`~/ng2-ps5-deps`: the PS5 payload SDK, RADV from mihawk-99's PS5_Mesa, the app
startup, `libc.prx` and the signer). Then:

```
tools/native/build_shaders.sh              # the game's shaders, from your default.xex
export PS5_PAYLOAD_SDK=~/ng2-ps5-deps/ps5-plume-triangle/.deps/native/ps5-payload-sdk
tools/native/ps5_build.sh                  # out/ps5/ng2
TITLE_NAME="Ninja Gaiden II - IzunaRecomp" \
  ps5/tools/ps5_package.sh out/ps5 ~/ng2-ps5-deps/ps5-plume-triangle PPSA94203
```

The app folder is `out/ps5/ps5-app/PPSA94203`, with the licences of what it
carries in `licenses/`. Unlike the PC builds, the PS5 app has the game's
shaders built in (translated by `build_shaders.sh` from your copy, like the
code): build output only, never committed. The packager refuses a package
that contains any game file.

## 6. The Android build

Needs the Android NDK r27c, the SDK's platform 35 and build tools 35, and a JDK 17
(`IZUNA_ANDROID_DEPS`, default `~/ng2-ps5-deps/android`, holds `jdk17/` and
`sdk/` with `ndk/27.2.12479018`). Like the PS5, the app has the game's shaders
built in (`tools/native/build_shaders.sh`, from your copy).

```
tools/android/build.sh                     # out/android/libmain.so (arm64-v8a, Snapdragon-tuned)
tools/android/make_apk.sh                  # out/android-release/IzunaRecomp-<version>-android.apk
```

`make_apk.sh` needs no Gradle: aapt2, javac, d8, zipalign and apksigner from the
build tools package `libmain.so`, libadrenotools' hook libraries, SDL's Java
glue (from the SDK tree) and `tools/android/app`. The APK is signed with the
key `IZUNA_KEYSTORE` names, made once if missing: an installed app only
updates from an APK signed with the same key, so keep it (and never commit it).

On Android the guest's physical memory is an AHardwareBuffer the GPU shares
(`src/native/native_shared_memory.cpp`), and a Turnip driver package is loaded
through libadrenotools (`third_party/libadrenotools`,
`src/izuna/izuna_gpu_driver.cpp`).
