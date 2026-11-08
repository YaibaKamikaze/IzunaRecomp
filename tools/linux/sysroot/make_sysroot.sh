#!/bin/bash
# [IzunaRecomp] The Ubuntu 22.04 (jammy) sysroot the Linux build compiles
# against (tools/linux/build.sh): glibc 2.35, GCC 12's libstdc++, and the
# headers of the desktop libraries SDL loads at run time. Packages come from
# the Ubuntu archive (fetch.py); no root access is needed.
#   tools/linux/sysroot/make_sysroot.sh [folder]   (default ~/ng2-ps5-deps/linux-sysroot)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
dir="${1:-$HOME/ng2-ps5-deps/linux-sysroot}"
mkdir -p "$dir"
cp "$here/fetch.py" "$dir/fetch.py"
cd "$dir"
python3 fetch.py \
  libc6 libc6-dev linux-libc-dev libcrypt-dev libcrypt1 \
  libgcc-12-dev libstdc++-12-dev libgcc-s1 gcc-12-base libstdc++6 libatomic1 \
  libx11-dev libx11-6 libx11-data libxcb1-dev libxcb1 libx11-xcb-dev libx11-xcb1 libxau-dev libxau6 \
  libxdmcp-dev libxdmcp6 libbsd0 libmd0 x11proto-dev xtrans-dev libxext-dev libxext6 libxrandr-dev libxrandr2 \
  libxcursor-dev libxcursor1 libxi-dev libxi6 libxfixes-dev libxfixes3 libxss-dev libxss1 \
  libxrender-dev libxrender1 libxtst-dev libxtst6 libxkbcommon-dev libxkbcommon0 \
  libwayland-dev libwayland-client0 libwayland-cursor0 libwayland-egl1 libwayland-server0 wayland-protocols \
  libffi-dev libffi8 libdecor-0-dev libdecor-0-0 libegl-dev libegl1 libgl-dev libglx-dev libglvnd0 \
  libasound2-dev libasound2 libpulse-dev libpulse0 libpulse-mainloop-glib0 \
  libpipewire-0.3-dev libpipewire-0.3-0 libspa-0.2-dev libudev-dev libudev1 libdbus-1-dev libdbus-1-3 >/dev/null
root="$dir/root"
# A merged /usr, as on the real system.
for d in lib lib64; do
  if [ -d "$root/$d" ] && [ ! -L "$root/$d" ]; then
    mkdir -p "$root/usr/$d"; cp -a "$root/$d/." "$root/usr/$d/"; rm -rf "${root:?}/$d"; ln -s "usr/$d" "$root/$d"
  fi
done
# Absolute symlinks would point at the build machine's own libraries.
python3 - "$root" <<'PY'
import os, sys
root = sys.argv[1]
for dp, dns, fns in os.walk(root):
    for name in dns + fns:
        p = os.path.join(dp, name)
        if os.path.islink(p) and os.readlink(p).startswith('/'):
            t = os.readlink(p); os.remove(p)
            os.symlink(os.path.relpath(os.path.join(root, t.lstrip('/')), dp), p)
PY
cat > "$dir/jammy.cmake" <<CMAKE
# Linux build against the Ubuntu 22.04 sysroot (tools/linux/sysroot/make_sysroot.sh).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_SYSROOT $root)
set(CMAKE_C_COMPILER clang-19)
set(CMAKE_CXX_COMPILER clang++-19)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_SYSROOT_DIR} $root)
set(ENV{PKG_CONFIG_LIBDIR} $root/usr/lib/x86_64-linux-gnu/pkgconfig:$root/usr/share/pkgconfig)
set(ENV{PKG_CONFIG_PATH} "")
CMAKE
echo "==> $dir (toolchain: $dir/jammy.cmake)"
