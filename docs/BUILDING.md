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

The game's shaders are not built here: `ng2.exe` recompiles them from the
player's copy at the first start (`src/izuna/izuna_shaders.cpp`, with the
XenosRecomp fork in `third_party/xenosrecomp`).
