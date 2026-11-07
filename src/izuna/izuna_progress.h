// [IzunaRecomp] Start-up work the progress panel shows (izuna_shader_overlay.h)
// besides the shaders: the pipelines the renderer builds before the game
// draws (native_pipelines.cpp, WaitForPrecompile). Written by the renderer,
// read by the panel every paint.
#pragma once

#include <atomic>

namespace izuna::progress {

struct Stage {
  std::atomic<int> done{0};
  std::atomic<int> total{0};
  std::atomic<bool> active{false};  // the game is held until it is done
  std::atomic<bool> finished{false};
};

// The pipelines built ahead of the game's first frame.
inline Stage g_pipelines;

}  // namespace izuna::progress
