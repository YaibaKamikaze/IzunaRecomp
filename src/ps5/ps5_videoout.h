// [IzunaRecomp] The PS5's display, as VideoOut reports it (ps5_videoout.cpp):
// when it last refreshed, and which flips still wait for it. The swapchain
// (the PS5 Mesa's VideoOut WSI) opens VideoOut and submits the flips itself;
// both calls are wrapped at link time (--wrap=sceVideoOutOpen,
// --wrap=sceVideoOutSubmitFlip in ps5/tools/ps5_package.sh and CMakeLists.txt)
// to keep the handle and each flip's argument and time. Elsewhere - or before
// the swapchain exists - nothing is known and callers keep their own clock.
#pragma once

#include <cstdint>

namespace ng2ps5::videoout {

#if defined(__PROSPERO__) && !defined(NG2_PS5_HOST_TEST)

// The display's last vblank (steady_clock ns, as rex::ui::display_clock) and
// VideoOut's count of them. False when VideoOut is not open or its answer is
// not a plausible vblank (each answer is checked: see the .cpp).
bool LastVblank(int64_t* ns, uint64_t* count);

// The flips submitted and not shown yet, and when the oldest of them was
// submitted (steady_clock ns). False when unknown.
bool PendingFlips(uint32_t* count, int64_t* oldest_ns);

#else

inline bool LastVblank(int64_t*, uint64_t*) { return false; }
inline bool PendingFlips(uint32_t*, int64_t*) { return false; }

#endif

}  // namespace ng2ps5::videoout
