// [IzunaRecomp] The hook table when the generated thunks are not there yet
// (izuna_hooks.cpp): weak, so generated/default/izuna_hooks.0.cpp's wins.
#include <cstdint>
extern "C" __attribute__((weak)) uint8_t izuna_hook_flags[1] = {};
extern "C" __attribute__((weak)) const uint32_t izuna_hook_count = 0;
extern "C" __attribute__((weak)) const uint32_t izuna_hook_addresses[1] = {};
