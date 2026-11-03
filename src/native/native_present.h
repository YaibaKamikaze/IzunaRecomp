// [IzunaRecomp, M4] The native swap: the native renderer's latest frame into
// the presenter's guest output, through the game's gamma ramp
// (native_present.cpp). What the emulated GPU's swap did with the frames the
// native renderer provided, without it.
#pragma once

#include <cstdint>

namespace rex::ui {
class Presenter;
}

namespace ng2native {

// The latest frame (rex::external_frame::GetFrame) through `gamma_ramp` (256
// entries, red << 20 | green << 10 | blue, 10 bits each) into `presenter`'s
// guest output; the work is submitted on the shared queue. False when there
// is no frame yet or the Vulkan objects could not be made. Any one thread.
bool PresentNativeFrame(rex::ui::Presenter& presenter, const uint32_t* gamma_ramp);

}  // namespace ng2native
