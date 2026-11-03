// [IzunaRecomp] Hooks on the game's functions (izuna_hooks.h).

#include "izuna/izuna_hooks.h"

#include <algorithm>
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
std::shared_mutex g_mutex;
std::unordered_map<uint32_t, std::vector<Hook>> g_hooks;  // function index -> its hooks
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
  g_hooks[uint32_t(index)].push_back({id, after, callback, user});
  g_index_of[id] = uint32_t(index);
  __atomic_store_n(&izuna_hook_flags[index], uint8_t(1), __ATOMIC_RELEASE);
  return id;
}

void Remove(int id) {
  std::unique_lock<std::shared_mutex> lock(g_mutex);
  auto at = g_index_of.find(id);
  if (at == g_index_of.end()) return;
  const uint32_t index = at->second;
  g_index_of.erase(at);
  auto& list = g_hooks[index];
  list.erase(std::remove_if(list.begin(), list.end(), [&](const Hook& h) { return h.id == id; }), list.end());
  if (list.empty()) {
    g_hooks.erase(index);
    __atomic_store_n(&izuna_hook_flags[index], uint8_t(0), __ATOMIC_RELEASE);
  }
}

}  // namespace izuna::hooks

// A hooked function's call (from its thunk): the hooks before it, the function
// unless one of them skips it, the hooks after it. The list is copied out of
// the lock, so a hook may add or remove hooks.
extern "C" void IzunaHookDispatch(uint32_t index, PPCContext& ctx, uint8_t* base,
                                  void (*original)(PPCContext&, uint8_t*)) {
  using namespace izuna::hooks;
  std::vector<Hook> hooks;
  {
    std::shared_lock<std::shared_mutex> lock(g_mutex);
    auto it = g_hooks.find(index);
    if (it != g_hooks.end()) hooks = it->second;
  }
  bool skip = false;
  for (const Hook& h : hooks) {
    if (!h.after && h.callback(h.user, ctx, base, false)) skip = true;
  }
  if (!skip) original(ctx, base);
  for (const Hook& h : hooks) {
    if (h.after) h.callback(h.user, ctx, base, true);
  }
}
