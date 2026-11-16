// [IzunaRecomp] Hooks on the game's functions at runtime (izuna_hooks.cpp).
//
// Every recompiled game function is reached through a thunk
// (tools/izuna/gen_hook_thunks.py, generated/default/izuna_hooks.*.cpp) that
// calls its body straight away unless a hook is set on it - so a script mod
// can run before or after any of the game's functions, read and change its
// arguments and its result, or skip it.
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace izuna::hooks {

// A hook: before the function (return true to skip it - its result is then
// whatever the hook left in r3 / f1), or after it (r3 / f1 hold its result).
using Callback = bool (*)(void* user, PPCContext& ctx, uint8_t* base, bool after);

// A hook on the game function at `address` (its first instruction), or -1 if
// none starts there. Hooks run in the order added.
int Add(uint32_t address, bool after, Callback callback, void* user);
void Remove(int id);
// The function at `address` is never run again (each call returns at once,
// registers as they were): for the engine's own work whose result nothing
// uses. Mods' hooks on it still run. False if no function starts there.
bool Skip(uint32_t address);
// Whether a game function starts at `address`.
bool IsFunction(uint32_t address);
uint32_t FunctionCount();

}  // namespace izuna::hooks
