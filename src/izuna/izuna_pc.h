// [IzunaRecomp] What the Windows build does for steadiness (izuna_pc.cpp);
// each is a no-op elsewhere.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace izuna::pc {

// Once, at the start: Windows' power throttling off for this process (a
// laptop's EcoQoS slows a process it thinks is in the background, and its
// timers with it), 1 ms timer resolution, the process a step above normal.
void TuneProcess();

// The calling thread above normal (1: highest - the guest vblank's clock).
void RaiseThreadPriority(int level);

// Asleep for about `ns` nanoseconds, to within ~0.1 ms on Windows 10 1803+ (a
// high-resolution waitable timer, one per thread): a timing loop can then
// sleep until just before its moment instead of spinning through the last
// millisecond, which a 1 ms Sleep would overshoot. Elsewhere: sleep_for.
void SleepNs(int64_t ns);

// One copy of IzunaRecomp at a time on this PC (two would write the same
// pipeline cache in %LOCALAPPDATA%, and a folder's saves). A copy that finds
// another running waits up to 5 s for it (the hub's relaunch of itself
// starts the next before this one has gone), then says so and quits.
void EnsureSingleInstance();

// The primary display's mode in pixels (its real resolution, not the
// DPI-scaled one); false when unknown (and off Windows).
bool ScreenSize(int* width, int* height);

// A problem the player must see before the app quits: a message box on
// Windows (and the log line everywhere).
void ShowError(const std::string& text);

// A crash writes logs/crash-<time>.dmp (a minidump: the threads' stacks and the
// memory they point at), to be read with the build's ng2.pdb.
void InstallCrashDumps(const std::filesystem::path& logs);

}  // namespace izuna::pc
