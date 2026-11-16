// [IzunaRecomp] Hooks on the game's functions (izuna_hooks.h).

#include "izuna/izuna_hooks.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>

// The thunks' table (generated/default/izuna_hooks.0.cpp; izuna_hooks_none.cpp
// stands in before the first codegen). Declared without their sizes here: a
// definition seen in this file would let the compiler fold them.
extern "C" uint8_t izuna_hook_flags[];
extern "C" const uint32_t izuna_hook_count;
extern "C" const uint32_t izuna_hook_addresses[];

namespace izuna::hooks {
namespace {

struct Hook {
  int id;
  bool after;
  Callback callback;
  void* user;
};
// izuna_hook_flags[i]: kHooked - hooks are set (g_hooks); kSkipped - the
// function is never run (Skip: the engine's own, for work nothing uses).
constexpr uint8_t kHooked = 1, kSkipped = 2;
using HookList = std::shared_ptr<const std::vector<Hook>>;
std::shared_mutex g_mutex;
// function index -> its hooks: an immutable list, replaced on each change, so
// a call takes a reference to it rather than a copy
std::unordered_map<uint32_t, HookList> g_hooks;
std::unordered_map<int, uint32_t> g_index_of;             // hook id -> function index
int g_next_id = 1;

int IndexOf(uint32_t address) {
  const uint32_t* begin = izuna_hook_addresses;
  const uint32_t* end = izuna_hook_addresses + izuna_hook_count;
  const uint32_t* it = std::lower_bound(begin, end, address);
  return it != end && *it == address ? int(it - begin) : -1;
}

}  // namespace

bool IsFunction(uint32_t address) { return IndexOf(address) >= 0; }
uint32_t FunctionCount() { return izuna_hook_count; }

int Add(uint32_t address, bool after, Callback callback, void* user) {
  const int index = IndexOf(address);
  if (index < 0) return -1;
  std::unique_lock<std::shared_mutex> lock(g_mutex);
  const int id = g_next_id++;
  HookList& slot = g_hooks[uint32_t(index)];
  auto list = slot ? std::make_shared<std::vector<Hook>>(*slot) : std::make_shared<std::vector<Hook>>();
  list->push_back({id, after, callback, user});
  slot = std::move(list);
  g_index_of[id] = uint32_t(index);
  __atomic_fetch_or(&izuna_hook_flags[index], kHooked, __ATOMIC_RELEASE);
  return id;
}

void Remove(int id) {
  std::unique_lock<std::shared_mutex> lock(g_mutex);
  auto at = g_index_of.find(id);
  if (at == g_index_of.end()) return;
  const uint32_t index = at->second;
  g_index_of.erase(at);
  HookList& slot = g_hooks[index];
  auto list = std::make_shared<std::vector<Hook>>(slot ? *slot : std::vector<Hook>());
  list->erase(std::remove_if(list->begin(), list->end(), [&](const Hook& h) { return h.id == id; }), list->end());
  if (list->empty()) {
    g_hooks.erase(index);
    __atomic_fetch_and(&izuna_hook_flags[index], uint8_t(~kHooked), __ATOMIC_RELEASE);
  } else {
    slot = std::move(list);
  }
}

bool Skip(uint32_t address) {
  const int index = IndexOf(address);
  if (index < 0) return false;
  __atomic_fetch_or(&izuna_hook_flags[index], kSkipped, __ATOMIC_RELEASE);
  return true;
}

}  // namespace izuna::hooks

// A hooked function's call (from its thunk): the hooks before it, the function
// unless one of them skips it, the hooks after it. The list is copied out of
// the lock (a reference to the immutable list), so a hook may add or remove
// hooks.
extern "C" void IzunaHookDispatch(uint32_t index, PPCContext& ctx, uint8_t* base,
                                  void (*original)(PPCContext&, uint8_t*)) {
  using namespace izuna::hooks;
  const uint8_t flags = __atomic_load_n(&izuna_hook_flags[index], __ATOMIC_ACQUIRE);
  if (flags == kSkipped) return;  // (the common case of a skipped function: no lock)
  HookList hooks;
  {
    std::shared_lock<std::shared_mutex> lock(g_mutex);
    auto it = g_hooks.find(index);
    if (it != g_hooks.end()) hooks = it->second;
  }
  bool skip = (flags & kSkipped) != 0;
  if (hooks) {
    for (const Hook& h : *hooks) {
      if (!h.after && h.callback(h.user, ctx, base, false)) skip = true;
    }
  }
  if (!skip) original(ctx, base);
  if (hooks) {
    for (const Hook& h : *hooks) {
      if (h.after) h.callback(h.user, ctx, base, true);
    }
  }
}
