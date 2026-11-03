#!/bin/bash
# [IzunaRecomp] Fetches the ReXGlue SDK at the revision IzunaRecomp is built
# on and applies IzunaRecomp's changes to it:
#   tools/setup_sdk.sh            -> third_party/rexglue-sdk
#
# The SDK: ReXGlue (Tom Clay, derived from Xenia), as carried by the
# svr2011-ps5 project's rexglue-ps5 branch, at commit f9b13da. IzunaRecomp's
# changes to it are patches/rexglue-izuna.patch (the native graphics side,
# the Vulkan presenter's pacing, asynchronous file reads, audio and input
# fixes - see CHANGELOG.md) and patches/ffmpeg-izuna.patch (its FFmpeg
# submodule: a PS5 configuration header).
set -euo pipefail
cd "$(dirname "$0")/.."
url=https://github.com/elripalda/svr2011-ps5.git
rev=f9b13da60c766020a67adbf45a7e96caad00b02e
dir=third_party/rexglue-sdk
if [ -e "$dir/.git" ]; then
  echo "$dir exists already - remove it to fetch it again" >&2
  exit 1
fi
git clone --no-checkout "$url" "$dir"
git -C "$dir" checkout --quiet "$rev"
git -C "$dir" submodule update --init --recursive
git -C "$dir" apply --whitespace=nowarn "$PWD/patches/rexglue-izuna.patch"
git -C "$dir/thirdparty/FFmpeg" apply --whitespace=nowarn "$PWD/patches/ffmpeg-izuna.patch"
echo "ReXGlue SDK ready in $dir ($rev + IzunaRecomp's patches)"
