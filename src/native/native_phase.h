// [IzunaRecomp] Where a worker record's time went (native_worker.cpp's
// slow-record report), shared by the renderer's parts.
#pragma once

#include <chrono>
#include <cstdint>

namespace ng2native {
// [IzunaRecomp] test30: where a worker record's time went - the slow-record
// report (native_worker.cpp) prints these when one record takes over 8 ms.
// (Nested: texture time includes the image creations it makes.)
enum WorkPhase : int { kPhaseSubmit, kPhaseFence, kPhaseTexture, kPhasePipeline, kPhaseImage, kPhaseCount };
extern thread_local int64_t t_phase_ns[kPhaseCount];

// [IzunaRecomp] test33: whether the renderer times its work - the clock reads
// per draw, texture and record that only the log's reports print (the
// slow-record line, "time per frame", "draw stages" ...). rex::LoggingEnabled()
// at DrawInit, before any thread that reads it: with logging off, none of
// them is taken. Never changes after.
extern bool g_timed;

struct PhaseTimer {
  WorkPhase phase;
  std::chrono::steady_clock::time_point t0;
  explicit PhaseTimer(WorkPhase p) : phase(p) {
    if (g_timed) t0 = std::chrono::steady_clock::now();
  }
  ~PhaseTimer() {
    if (!g_timed) return;
    t_phase_ns[phase] +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
  }
};

}  // namespace ng2native
