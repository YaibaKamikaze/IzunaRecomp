Ninja Gaiden II - IzunaRecomp @VERSION@ for Linux (AppImage)
============================================================

A native recompilation of Ninja Gaiden II (Xbox 360), drawn by the Kasumi
Renderer (Vulkan). No emulator and no game files come with it: you need your
own copy of the game.

Needs: a 64-bit Linux with glibc 2.39 or newer (SteamOS 3.6+, Ubuntu 24.04+,
Fedora 40+, Arch, ...), a GPU with a Vulkan driver (Mesa RADV / ANV / NVK, or
NVIDIA's), a processor with AVX2 (Intel since 2013, every AMD Ryzen, the Steam
Deck).


Starting
--------
1. Make the AppImage executable (chmod +x IzunaRecomp-*.AppImage) and run it.
2. The hub asks for your game folder: choose the folder of your Ninja Gaiden II
   copy (the one with default.xex). It is remembered.
3. START GAME.

The first start takes a little longer: the game's shaders are compiled for
your GPU from your copy (kept in ~/.cache/IzunaRecomp/shaders, so later starts
skip this).

Settings, logs, mods, DLC and saves to import live in
~/.local/share/IzunaRecomp (the AppImage itself is read-only).

Steam Deck: add the AppImage to Steam as a non-Steam game and run it as it is
- no Proton. (Under Proton, the Windows build cannot draw: its renderer cannot
use the game's memory there.)

The controls and settings are the Windows build's: see the Windows README, or
SETTINGS in the hub.


When something goes wrong
-------------------------
Send ~/.local/share/IzunaRecomp/logs (the newest ng2_*.log).

IzunaRecomp is not affiliated with Koei Tecmo or Microsoft. Ninja Gaiden is a
trademark of Koei Tecmo. The licenses of the libraries inside are in licenses/.
