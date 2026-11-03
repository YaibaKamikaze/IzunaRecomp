#!/usr/bin/env bash
# [native renderer] Run the Linux desktop build of the PS5 app (out/linux,
# NG2_PS5_HOST_TEST=ON) on the build VM: headless (SDL offscreen + Mesa's
# lavapipe), straight past the hub, with scripted button presses. System
# dialogs (XamShowMessageBoxUI - the game asks one at the title) answer
# themselves with their default button (--headless=true): headless, nobody
# could, and the title would stay at NOW LOADING for ever.
#
#   tools/native/hosttest.sh [seconds] [extra cvar flags...]
#   e.g. tools/native/hosttest.sh 120 --native_renderer=true
#
# Runs in ~/ng2-ps5-deps/hosttest (outside the repo): a copy of the binary and
# its two libraries, and a "Game Data" symlink to the disc folder - a link,
# never a copy (the game files stay out of the repo, CLAUDE.md). The log lands
# in ~/ng2-ps5-deps/hosttest/logs/. Slow (software rendering): a few frames a
# second at best - for checking what the native renderer draws, not for play.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
run=${NG2_HOSTTEST_DIR:-$HOME/ng2-ps5-deps/hosttest}
disc=${NG2_DISC:-/media/sf_ng2/Game data}
seconds=${1:-90}
shift || true

mkdir -p "$run"
cp "$root/out/linux/ng2" "$run/ng2"
# (no GPU plugin: IzunaRecomp builds the SDK without GPU emulation)
rm -f "$run/librexgpu-xenos.so"
cp "$root/third_party/rexglue-sdk/out/linux-amd64/librexruntime.so" "$run/"
[[ -e "$run/Game Data" ]] || ln -sfn "$disc" "$run/Game Data"

cd "$run"
status=0
SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy LD_LIBRARY_PATH="$run" VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  NG2_AUTOSTART=1 NG2_PAD_SCRIPT="${NG2_PAD_SCRIPT:-autoskip:$((seconds - 10))}" \
  timeout "$seconds" ./ng2 --headless=true "$@" > stdout.txt 2>&1 || status=$?
# A run the timer stops leaves its guest memory file (4.5 GB) in /dev/shm;
# a full /dev/shm makes the next run die with a bus error.
pgrep -x ng2 > /dev/null || find /dev/shm -maxdepth 1 -name 'xenia_memory_*' -user "$USER" -delete
log=$(ls -t logs/*.log | head -1)
echo "exit $status (124 = stopped by the timer); log: $run/$log"
