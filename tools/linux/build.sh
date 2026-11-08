#!/bin/bash
# [IzunaRecomp] The Linux desktop build (the AppImage's binaries):
#   tools/linux/build.sh                    -> out/linux-desktop/ng2         (x86-64-v3, AVX2)
#   IZUNA_CPU=compat tools/linux/build.sh   -> out/linux-desktop-compat/ng2  (x86-64-v2)
# generated/ must exist (docs/BUILDING.md).
#
# Built against an Ubuntu 22.04 sysroot (~/ng2-ps5-deps/linux-sysroot, made by
# its fetch.py from the Ubuntu archive): glibc 2.35 and GCC 12's libstdc++
# (linked in), so the binary runs on any distribution from 2022 on (Ubuntu
# 22.04+, Debian 12, Fedora 36+, Arch, SteamOS, Bazzite ...). Its headers for
# X11, Wayland, PipeWire, PulseAudio, ALSA, udev and D-Bus let SDL build all of
# those backends - it silently leaves out any it cannot find (v0.2.1's AppImage
# had "offscreen" only: "No available video device"). SDL loads them from the
# player's system at run time, whichever the desktop has.
set -euo pipefail
cd "$(dirname "$0")/../.."
sysroot_dir=${IZUNA_LINUX_SYSROOT:-$HOME/ng2-ps5-deps/linux-sysroot}
toolchain=$sysroot_dir/jammy.cmake
[ -f "$toolchain" ] || { echo "error: no sysroot toolchain at $toolchain (make it: tools/linux/sysroot/make_sysroot.sh)" >&2; exit 1; }
if [ "${IZUNA_CPU:-}" = compat ]; then
  out=out/linux-desktop-compat; march=x86-64-v2
else
  out=out/linux-desktop; march=x86-64-v3
fi
if [ ! -f "$out/build.ninja" ]; then
  cmake -S . -B "$out" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    "-DCMAKE_CXX_FLAGS=-stdlib=libstdc++ -march=$march" "-DCMAKE_C_FLAGS=-march=$march" \
    -DREXSDK_DIR="$PWD/third_party/rexglue-sdk" -DREXGLUE_OUTPUT_DIR="$PWD/$out/sdk" -DREXGLUE_STATIC_RUNTIME=ON
fi
# Refuse an SDL that cannot open a window, play sound or see controllers on a
# Linux desktop.
sdl_config=$(find "$out" -name SDL_build_config.h | head -1)
need() { grep -q "^#define $1 1" "$sdl_config" || { echo "error: SDL was configured without $1 ($sdl_config)" >&2; exit 1; }; }
need SDL_VIDEO_DRIVER_X11
need SDL_VIDEO_DRIVER_WAYLAND
need SDL_AUDIO_DRIVER_PIPEWIRE
need SDL_AUDIO_DRIVER_PULSEAUDIO
need SDL_AUDIO_DRIVER_ALSA
need HAVE_LIBUDEV_H        # controllers: udev hot-plug and detection
grep -q '^#define SDL_UDEV_DYNAMIC "libudev.so' "$sdl_config" || { echo "error: SDL was configured without udev" >&2; exit 1; }
need HAVE_DBUS_DBUS_H      # D-Bus: screensaver inhibit, desktop settings
ninja -C "$out" ${NINJA_ARGS:-} ng2
# Refuse a binary that needs a newer glibc than the sysroot's.
newest=$(objdump -T "$out/ng2" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
case $newest in
  GLIBC_2.[0-9]|GLIBC_2.[12][0-9]|GLIBC_2.3[0-5]) echo "==> $out/ng2: needs $newest at most" ;;
  *) echo "error: $out/ng2 needs $newest (more than glibc 2.35)" >&2; exit 1 ;;
esac
