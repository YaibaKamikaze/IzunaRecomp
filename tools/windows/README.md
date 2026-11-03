# The Windows build (made on Linux)

IzunaRecomp for Windows is cross-compiled here: Clang 19 for
x86_64-pc-windows-msvc, LLVM's lld-link, llvm-rc, llvm-ar and llvm-mt, against
a copy of MSVC's headers and libraries and the Windows SDK from the player's
own Build Tools for Visual Studio 2022.

1. On the Windows PC: install Build Tools for Visual Studio 2022 with
   "Desktop development with C++", then run copy-winsdk.ps1 (it copies the
   x64 headers and libraries to C:\ng2recomp-v1.3.0\winsdk, the VM's
   /media/sf_ng2/winsdk).
2. Here: copy it to ~/ng2-ps5-deps/winsdk (never into the repo) and run
   `tools/windows/case_links.py ~/ng2-ps5-deps/winsdk src third_party` once:
   Windows' file names ignore case, Linux's do not.
3. `tools/windows/build.sh` -> out/windows/ng2.exe. `NINJA_ARGS="-k 0"` keeps
   going past errors. Codegen is not run here (the tree's codegen tool is a
   Windows program): generated/ comes from the Linux tool, as for the PS5.

What differs from the PS5 / Linux builds: the SDK's Vulkan side on and its
Direct3D 12 side off (Kasumi is Vulkan everywhere), the runtime linked in
statically (REXGLUE_STATIC_RUNTIME: one .exe), x86-64-v2, the discrete GPU
preferred over an integrated one (a laptop's Optimus pair).

Since izuna-windows-test9 the C and C++ runtime is linked in
(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded, toolchain.cmake): ng2.exe needs only
Windows and a Vulkan driver, no Visual C++ Redistributable. The link writes
out/windows/ng2.pdb beside it: archive it with the build (it reads the
logs\crash-*.dmp a crash writes - src/izuna/izuna_pc.cpp) and never ship it.

Two CPU levels since izuna-windows-test11: `tools/windows/build.sh` makes
ng2.exe for x86-64-v3 (AVX2, FMA, BMI, MOVBE) in out/windows, and
`IZUNA_CPU=compat tools/windows/build.sh` ng2-compat.exe for x86-64-v2 in
out/windows-compat (each tree builds its own SDK into <tree>/sdk:
REXGLUE_OUTPUT_DIR). ng2.exe checks the CPU in a TLS callback before any of
its code runs and starts ng2-compat.exe on one without AVX2
(src/izuna/izuna_cpu_redirect.cpp, compiled for plain x86-64). The generated
code is no longer built with -fasync-exceptions on Windows (NG2 has no guest
SEH scopes; IZUNA_RECOMP_ASYNC_EH=ON puts it back).

A release: `tools/windows/make_release.sh [ng2.exe] [dest] [ng2-compat.exe]` -> the folder
IzunaRecomp-<IZUNA_VERSION>-windows and its .zip: ng2.exe, README.txt
(tools/windows/release/), the folders the hub reads with their READMEs,
hub/button_prompts, and licenses/. No game files and no game music (the PS5
package's hub/music/theme.wav is the soundtrack's: never in a release).

Since izuna-windows-test3 the Windows build opens on the PS5's hub (src/ps5/hub)
instead of the PC port's setup screen: ps5_pad.cpp reads the keyboard (ImGui)
and controllers (SDL) off the PS5, ps5_sound.cpp plays through SDL, ps5_paths.cpp
keeps everything beside the .exe and the game where the player chose
(START GAME asks). Its version is IZUNA_VERSION; its icon resources/izuna.ico
(tools/make_izuna_icon.py). Frame pacing: src/native/pc_display_clock.cpp
(the window's monitor's vblanks through DXGI) and the renderer as the PS5 runs
it (threaded recording - ng2_tuning.h, NG2_HUB).
