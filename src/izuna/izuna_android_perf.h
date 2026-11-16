// [IzunaRecomp] Android: a CPU report every 15 s - the busiest threads and the
// cores they run on (izuna_android_perf.cpp). No-op elsewhere.
#pragma once

namespace izuna::android_perf {
void Start();
// The game's window shows 60 frames a second: tells Android, so a 90 / 120 /
// 144 Hz screen paces them evenly (or switches to 60 Hz) instead of showing
// each for one, two or three refreshes by chance - the "frame-skippy" 60 fps
// (test56, the AYN Thor's 120 Hz screen). From the UI thread; cheap; called
// every frame, it acts only when the window changes and every few seconds.
void PaceWindow();
// [IzunaRecomp] test68: once per game frame, from the game's render thread
// (its swap): Android's performance hints (ADPF, Android 13+) - the frame's
// target, 16.7 ms, and the CPU time the frame's threads took, so the system
// keeps them on cores and clocks that make the target rather than guessing.
void FrameDone();
}  // namespace izuna::android_perf
