// [IzunaRecomp] Script mods (izuna_scripts.h; docs/MODDING.md).
//
// Lua 5.4 (third_party/lua), one state per mod, with:
//   the base library without dofile / loadfile (load takes text only),
//   string, table, math, utf8, coroutine - no io, os, package or debug;
//   require(name), from the mod's own scripts/ folder;
//   log(...) (print is the same), to the game's log;
//   mem.*   the game's memory: its own heaps and executable, not the GPU's
//           physical memory; a read of memory that is not there is nil, a
//           write to it false - never a crash;
//   pad.*   the controller as the game sees it (Xbox names, and the
//           PlayStation ones for the same buttons);
//   ui.*    text, boxes and notifications over the game (1920x1080 space);
//   game.*  the frame count, the time, the game's file names.
// A script gets a budget of instructions per call (an endless loop stops it
// rather than the game) and of memory; one that keeps failing is stopped.

#include "izuna/izuna_scripts.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include <imgui.h>

#include <rex/ppc/func.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/immediate_drawer.h>

#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "izuna/izuna_hooks.h"
#include "native/native_draw.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace fs = std::filesystem;

namespace izuna::scripts {
namespace {

constexpr uint64_t kCallBudget = 20'000'000;    // instructions per on_frame
constexpr uint64_t kStartBudget = 200'000'000;  // the script's first run
constexpr uint64_t kHookBudget = 5'000'000;     // instructions per hook call
constexpr size_t kMemoryLimit = 64u << 20;      // bytes of Lua memory per mod
constexpr int kMaxErrors = 5;                   // errors before a script is stopped

struct Script;
struct HookRef {  // a Lua function hooked on a game function
  Script* script;
  int ref;     // in the registry
  int id = 0;  // izuna::hooks'
};

struct Script {
  std::string mod;  // its name, for the log
  fs::path dir;      // its folder (ui.image)
  fs::path scripts;  // its scripts/ folder (require)
  // Its Lua is called from the game's render thread (on_frame) and from
  // whichever game thread calls a hooked function: one at a time.
  std::recursive_mutex mutex;
  std::vector<std::unique_ptr<HookRef>> hooks;
  lua_State* L = nullptr;
  size_t memory = 0;
  uint64_t ticks = 0, budget = 0;
  int errors = 0;
  bool stopped = false;
};
std::vector<std::unique_ptr<Script>> g_scripts;
std::atomic<int> g_running{0};

Script* Of(lua_State* L) { return *static_cast<Script**>(lua_getextraspace(L)); }

void* Alloc(void* ud, void* ptr, size_t old_size, size_t new_size) {
  auto* s = static_cast<Script*>(ud);
  const size_t old = ptr ? old_size : 0;
  if (new_size == 0) {
    std::free(ptr);
    s->memory -= old;
    return nullptr;
  }
  if (new_size > old && s->memory + (new_size - old) > kMemoryLimit) return nullptr;  // (Lua: "not enough memory")
  void* p = std::realloc(ptr, new_size);
  if (p) s->memory = s->memory - old + new_size;
  return p;
}

void CountHook(lua_State* L, lua_Debug*) {
  Script* s = Of(L);
  s->ticks += 10000;
  if (s->ticks > s->budget) luaL_error(L, "ran more than %d million instructions in one go (an endless loop?)",
                                        int(s->budget / 1000000));
}

// --- What the scripts draw ------------------------------------------------------

struct Shape {
  enum Kind { kText, kRect, kFrame, kImage } kind;
  float x, y, w, h, size;
  uint32_t colour;  // 0xRRGGBBAA
  std::string text;  // (kImage: the picture's path)
};
struct Notice {
  std::string text;
  double until;
};
std::mutex g_draw_mutex;
std::vector<Shape> g_pending, g_shown;  // being made this frame / on screen
std::vector<Notice> g_notices;

double Now() {
  static const auto t0 = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

uint32_t Colour(lua_State* L, int arg, uint32_t def) {
  return lua_isnoneornil(L, arg) ? def : uint32_t(luaL_checkinteger(L, arg));
}

// --- The game's memory -------------------------------------------------------------

rex::memory::Memory* Mem() {
  auto* kernel = REX_KERNEL_STATE();
  return kernel ? kernel->memory() : nullptr;
}

// The game's own memory only: not the physical (GPU) heaps, whose pages the
// renderer and the kernel's file reads treat specially.
uint8_t* Guest(uint32_t address, uint32_t size, bool write) {
  auto* memory = Mem();
  if (!memory || size == 0 || address < 0x10000 || uint64_t(address) + size > 0x100000000ull) return nullptr;
  auto* heap = memory->LookupHeap(address);
  if (!heap || heap != memory->LookupHeap(address + size - 1) ||
      heap->heap_type() == rex::memory::HeapType::kGuestPhysical ||
      heap->heap_type() == rex::memory::HeapType::kHostPhysical) {
    return nullptr;
  }
  const auto access = heap->QueryRangeAccess(address, address + size - 1);
  const uint32_t a = uint32_t(access);
  if (write ? (a & 2u) == 0 : (a & 1u) == 0) return nullptr;
  return memory->TranslateVirtual<uint8_t*>(address);
}

uint32_t Address(lua_State* L, int arg) { return uint32_t(luaL_checkinteger(L, arg) & 0xFFFFFFFF); }

template <typename T>
int Read(lua_State* L) {
  const uint8_t* p = Guest(Address(L, 1), sizeof(T), false);
  if (!p) {
    lua_pushnil(L);
    return 1;
  }
  const T v = rex::memory::load_and_swap<T>(p);
  if constexpr (std::is_floating_point_v<T>) {
    lua_pushnumber(L, lua_Number(v));
  } else {
    lua_pushinteger(L, lua_Integer(v));
  }
  return 1;
}

template <typename T>
int Write(lua_State* L) {
  uint8_t* p = Guest(Address(L, 1), sizeof(T), true);
  if (p) {
    if constexpr (std::is_floating_point_v<T>) {
      rex::memory::store_and_swap<T>(p, T(luaL_checknumber(L, 2)));
    } else {
      rex::memory::store_and_swap<T>(p, T(luaL_checkinteger(L, 2)));
    }
  }
  lua_pushboolean(L, p != nullptr);
  return 1;
}

int MemString(lua_State* L) {
  const uint32_t a = Address(L, 1);
  const uint32_t max = uint32_t(std::clamp<lua_Integer>(luaL_optinteger(L, 2, 256), 1, 65536));
  std::string s;
  for (uint32_t i = 0; i < max; ++i) {
    const uint8_t* p = Guest(a + i, 1, false);
    if (!p) {
      if (i == 0) {
        lua_pushnil(L);
        return 1;
      }
      break;
    }
    if (*p == 0) break;
    s.push_back(char(*p));
  }
  lua_pushlstring(L, s.data(), s.size());
  return 1;
}

int MemBytes(lua_State* L) {
  const uint32_t a = Address(L, 1);
  const uint32_t n = uint32_t(std::clamp<lua_Integer>(luaL_checkinteger(L, 2), 0, 1 << 20));
  const uint8_t* p = n ? Guest(a, n, false) : nullptr;
  if (!p) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, reinterpret_cast<const char*>(p), n);
  return 1;
}

int MemSetBytes(lua_State* L) {
  const uint32_t a = Address(L, 1);
  size_t n = 0;
  const char* s = luaL_checklstring(L, 2, &n);
  uint8_t* p = n ? Guest(a, uint32_t(std::min<size_t>(n, 1 << 20)), true) : nullptr;
  if (p) std::memcpy(p, s, std::min<size_t>(n, 1 << 20));
  lua_pushboolean(L, p != nullptr);
  return 1;
}

int MemReadable(lua_State* L) {
  const uint32_t n = uint32_t(std::clamp<lua_Integer>(luaL_optinteger(L, 2, 1), 1, 1 << 24));
  lua_pushboolean(L, Guest(Address(L, 1), n, lua_toboolean(L, 3)) != nullptr);
  return 1;
}

// --- The controller ----------------------------------------------------------------

struct PadSnapshot {
  uint64_t frame = UINT64_MAX;
  uint32_t buttons = 0, previous = 0;  // XInput's bits, and kL2 / kR2
  float lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;
} g_pad;
uint64_t g_frame = 0;
constexpr uint32_t kL2 = 1u << 16, kR2 = 1u << 17;  // (the triggers past half way, as buttons)

// Once a frame at most: the controller as the game's XInputGetState sees it.
void ReadPad() {
  if (g_pad.frame == g_frame) return;
  g_pad.previous = g_pad.frame + 1 == g_frame ? g_pad.buttons : 0;
  g_pad.frame = g_frame;
  g_pad.buttons = 0;
  g_pad.lt = g_pad.rt = g_pad.lx = g_pad.ly = g_pad.rx = g_pad.ry = 0;
  rex::input::X_INPUT_STATE state{};
  auto* runtime = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->emulator() : nullptr;
  auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  if (!input || input->GetState(0, &state) != 0) return;
  const auto& gp = state.gamepad;
  g_pad.lt = gp.left_trigger / 255.0f;
  g_pad.rt = gp.right_trigger / 255.0f;
  g_pad.buttons = uint16_t(gp.buttons) | (g_pad.lt > 0.5f ? kL2 : 0) | (g_pad.rt > 0.5f ? kR2 : 0);
  auto axis = [](int16_t v) { return std::max(-1.0f, float(v) / 32767.0f); };
  g_pad.lx = axis(int16_t(gp.thumb_lx));
  g_pad.ly = axis(int16_t(gp.thumb_ly));
  g_pad.rx = axis(int16_t(gp.thumb_rx));
  g_pad.ry = axis(int16_t(gp.thumb_ry));
}

// A button by its Xbox name or its PlayStation one.
uint32_t Button(lua_State* L, int arg) {
  struct Name {
    const char* name;
    uint32_t bit;
  };
  static const Name kNames[] = {
      {"up", 0x0001},      {"down", 0x0002},     {"left", 0x0004},     {"right", 0x0008}, {"start", 0x0010},
      {"options", 0x0010}, {"back", 0x0020},     {"touchpad", 0x0020}, {"ls", 0x0040},    {"l3", 0x0040},
      {"rs", 0x0080},      {"r3", 0x0080},       {"lb", 0x0100},       {"l1", 0x0100},    {"rb", 0x0200},
      {"r1", 0x0200},      {"a", 0x1000},        {"cross", 0x1000},    {"b", 0x2000},     {"circle", 0x2000},
      {"x", 0x4000},       {"square", 0x4000},   {"y", 0x8000},        {"triangle", 0x8000}, {"lt", kL2},
      {"l2", kL2},         {"rt", kR2},          {"r2", kR2}};
  const char* name = luaL_checkstring(L, arg);
  for (const Name& n : kNames)
    if (std::strcmp(n.name, name) == 0) return n.bit;
  luaL_argerror(L, arg, "not a button (a, b, x, y, lb, rb, lt, rt, ls, rs, start, back, up, down, left, right - "
                        "or cross, circle, square, triangle, l1, r1, l2, r2, l3, r3, options, touchpad)");
  return 0;
}

int PadHeld(lua_State* L) {
  ReadPad();
  lua_pushboolean(L, (g_pad.buttons & Button(L, 1)) != 0);
  return 1;
}
int PadPressed(lua_State* L) {
  ReadPad();
  const uint32_t b = Button(L, 1);
  lua_pushboolean(L, (g_pad.buttons & b) && !(g_pad.previous & b));
  return 1;
}
int PadReleased(lua_State* L) {
  ReadPad();
  const uint32_t b = Button(L, 1);
  lua_pushboolean(L, !(g_pad.buttons & b) && (g_pad.previous & b));
  return 1;
}
int PadButtons(lua_State* L) {
  ReadPad();
  lua_pushinteger(L, g_pad.buttons);
  return 1;
}
int PadSticks(lua_State* L) {
  ReadPad();
  for (float v : {g_pad.lx, g_pad.ly, g_pad.rx, g_pad.ry}) lua_pushnumber(L, v);
  return 4;
}
int PadTriggers(lua_State* L) {
  ReadPad();
  lua_pushnumber(L, g_pad.lt);
  lua_pushnumber(L, g_pad.rt);
  return 2;
}

// --- The screen, the log, the game ------------------------------------------------

int UiText(lua_State* L) {
  Shape s{Shape::kText, float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)), 0, 0,
          float(luaL_optnumber(L, 5, 28)), Colour(L, 4, 0xECE6D6FF), luaL_checkstring(L, 3)};
  std::lock_guard<std::mutex> lock(g_draw_mutex);
  if (g_pending.size() < 4096) g_pending.push_back(std::move(s));
  return 0;
}
int UiRect(lua_State* L, Shape::Kind kind) {
  Shape s{kind, float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3)),
          float(luaL_checknumber(L, 4)), 0, Colour(L, 5, 0x000000A0), {}};
  std::lock_guard<std::mutex> lock(g_draw_mutex);
  if (g_pending.size() < 4096) g_pending.push_back(std::move(s));
  return 0;
}
int UiBox(lua_State* L) { return UiRect(L, Shape::kRect); }
// ui.image(path, x, y, w, h [, tint]): a PNG or JPEG from the mod's folder.
int UiImage(lua_State* L) {
  const std::string rel = luaL_checkstring(L, 1);
  if (rel.find("..") != std::string::npos) return luaL_argerror(L, 1, "a path in the mod's folder");
  Shape s{Shape::kImage, float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3)), float(luaL_checknumber(L, 4)),
          float(luaL_checknumber(L, 5)), 0, Colour(L, 6, 0xFFFFFFFF), (Of(L)->dir / rel).string()};
  std::lock_guard<std::mutex> lock(g_draw_mutex);
  if (g_pending.size() < 4096) g_pending.push_back(std::move(s));
  return 0;
}
int UiFrame(lua_State* L) { return UiRect(L, Shape::kFrame); }
int UiNotify(lua_State* L) {
  const double seconds = std::clamp<double>(luaL_optnumber(L, 2, 4), 0.5, 60);
  std::lock_guard<std::mutex> lock(g_draw_mutex);
  if (g_notices.size() < 8) g_notices.push_back({luaL_checkstring(L, 1), Now() + seconds});
  return 0;
}

int Log(lua_State* L) {
  std::string line;
  const int n = lua_gettop(L);
  for (int i = 1; i <= n; ++i) {
    if (i > 1) line += "  ";
    size_t len = 0;
    const char* s = luaL_tolstring(L, i, &len);
    line.append(s, len);
    lua_pop(L, 1);
  }
  REXLOG_INFO("[script] {}: {}", Of(L)->mod, line);
  return 0;
}

int GameTime(lua_State* L) {
  lua_pushnumber(L, Now());
  return 1;
}
int GameFileName(lua_State* L) {
  const auto& names = mods::FileNames();
  const lua_Integer id = luaL_checkinteger(L, 1);
  if (id < 0 || size_t(id) >= names.size()) return lua_pushnil(L), 1;
  lua_pushstring(L, names[size_t(id)].c_str());
  return 1;
}

// load(text [, name]): text only (bytecode can break the Lua machine).
int Load(lua_State* L) {
  size_t n = 0;
  const char* text = luaL_checklstring(L, 1, &n);
  const char* name = luaL_optstring(L, 2, "=(load)");
  if (luaL_loadbufferx(L, text, n, name, "t") != LUA_OK) {
    lua_pushnil(L);
    lua_insert(L, -2);
    return 2;
  }
  return 1;
}

bool ReadText(const fs::path& p, std::string* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

// require(name): scripts/<name, dots as folders>.lua, once.
int Require(lua_State* L) {
  const std::string name = luaL_checkstring(L, 1);
  if (name.find("..") != std::string::npos || name.find('/') != std::string::npos ||
      name.find('\\') != std::string::npos) {
    return luaL_error(L, "require '%s': a module name has dots between its parts, nothing else", name.c_str());
  }
  lua_getfield(L, LUA_REGISTRYINDEX, "izuna.loaded");
  lua_getfield(L, -1, name.c_str());
  if (!lua_isnil(L, -1)) return 1;
  lua_pop(L, 1);
  std::string rel = name;
  std::replace(rel.begin(), rel.end(), '.', '/');
  std::string text;
  if (!ReadText(Of(L)->scripts / (rel + ".lua"), &text)) {
    return luaL_error(L, "require '%s': no scripts/%s.lua in the mod", name.c_str(), rel.c_str());
  }
  if (luaL_loadbufferx(L, text.data(), text.size(), ("@" + rel + ".lua").c_str(), "t") != LUA_OK) {
    return lua_error(L);
  }
  lua_pushstring(L, name.c_str());
  lua_call(L, 1, 1);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
  }
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, name.c_str());
  return 1;
}

// --- Hooks on the game's functions (izuna_hooks.h) ------------------------------

bool Call(Script& s, int args, uint64_t budget, bool* result);

// The registers at a hooked function's start (its arguments: r3-r10, f1-f13;
// r1 the stack) or end (its result: r3, f1), valid while the hook runs.
struct Regs {
  PPCContext* ctx;
  uint8_t* base;
};
constexpr const char* kRegsMeta = "izuna.regs";

PPCRegister* Gpr(PPCContext& c, int n) {
  static PPCRegister PPCContext::* const k[32] = {
      &PPCContext::r0,  &PPCContext::r1,  &PPCContext::r2,  &PPCContext::r3,  &PPCContext::r4,  &PPCContext::r5,
      &PPCContext::r6,  &PPCContext::r7,  &PPCContext::r8,  &PPCContext::r9,  &PPCContext::r10, &PPCContext::r11,
      &PPCContext::r12, &PPCContext::r13, &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17,
      &PPCContext::r18, &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
      &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28, &PPCContext::r29,
      &PPCContext::r30, &PPCContext::r31};
  return &(c.*k[n]);
}
PPCRegister* Fpr(PPCContext& c, int n) {
  static PPCRegister PPCContext::* const k[32] = {
      &PPCContext::f0,  &PPCContext::f1,  &PPCContext::f2,  &PPCContext::f3,  &PPCContext::f4,  &PPCContext::f5,
      &PPCContext::f6,  &PPCContext::f7,  &PPCContext::f8,  &PPCContext::f9,  &PPCContext::f10, &PPCContext::f11,
      &PPCContext::f12, &PPCContext::f13, &PPCContext::f14, &PPCContext::f15, &PPCContext::f16, &PPCContext::f17,
      &PPCContext::f18, &PPCContext::f19, &PPCContext::f20, &PPCContext::f21, &PPCContext::f22, &PPCContext::f23,
      &PPCContext::f24, &PPCContext::f25, &PPCContext::f26, &PPCContext::f27, &PPCContext::f28, &PPCContext::f29,
      &PPCContext::f30, &PPCContext::f31};
  return &(c.*k[n]);
}

Regs* CheckRegs(lua_State* L) {
  auto* r = static_cast<Regs*>(luaL_checkudata(L, 1, kRegsMeta));
  if (!r->ctx) luaL_error(L, "the registers of a hook are used after it returned");
  return r;
}

// "r3" -> 3 (gpr), "f1" -> 1 (fpr); -1 if neither.
int RegName(const char* k, bool* fp) {
  if ((k[0] != 'r' && k[0] != 'f') || !k[1]) return -1;
  char* end = nullptr;
  const long n = std::strtol(k + 1, &end, 10);
  if (*end || n < 0 || n > 31) return -1;
  *fp = k[0] == 'f';
  return int(n);
}

// regs:call(address, a1 ... a8): calls the game's function at `address` with
// those in r3-r10, on this thread, below the hooked function's stack; gives
// back its r3 and f1. The registers are as before afterwards.
int RegsCall(lua_State* L) {
  Regs* r = CheckRegs(L);
  const uint32_t address = uint32_t(luaL_checkinteger(L, 2));
  PPCFunc* fn = rex::runtime::ResolveIndirectFunction(address);
  if (!fn || !izuna::hooks::IsFunction(address)) {
    return luaL_error(L, "no game function at %s", fmt::format("0x{:08X}", address).c_str());
  }
  const int n = std::min(lua_gettop(L) - 2, 8);
  PPCContext& c = *r->ctx;
  const PPCContext saved = c;
  for (int i = 0; i < n; ++i) Gpr(c, 3 + i)->u64 = uint64_t(luaL_checkinteger(L, 3 + i));
  c.r1.u64 = (c.r1.u32 - 0x400) & ~uint64_t(0xF);  // (a frame below the hooked function's)
  fn(c, r->base);
  const uint64_t r3 = c.r3.u64;
  const double f1 = c.f1.f64;
  c = saved;
  lua_pushinteger(L, lua_Integer(r3));
  lua_pushnumber(L, f1);
  return 2;
}

int RegsIndex(lua_State* L) {
  Regs* r = CheckRegs(L);
  const char* k = luaL_checkstring(L, 2);
  bool fp = false;
  const int n = RegName(k, &fp);
  if (n >= 0) {
    if (fp) {
      lua_pushnumber(L, Fpr(*r->ctx, n)->f64);
    } else {
      lua_pushinteger(L, lua_Integer(Gpr(*r->ctx, n)->u64));
    }
  } else if (std::strcmp(k, "lr") == 0) {
    lua_pushinteger(L, lua_Integer(r->ctx->lr));
  } else if (std::strcmp(k, "call") == 0) {
    lua_pushcfunction(L, RegsCall);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

int RegsNewIndex(lua_State* L) {
  Regs* r = CheckRegs(L);
  bool fp = false;
  const int n = RegName(luaL_checkstring(L, 2), &fp);
  if (n < 0) return luaL_error(L, "registers are r0-r31 and f0-f31");
  if (fp) {
    Fpr(*r->ctx, n)->f64 = luaL_checknumber(L, 3);
  } else {
    Gpr(*r->ctx, n)->u64 = uint64_t(luaL_checkinteger(L, 3));
  }
  return 0;
}

bool LuaHook(void* user, PPCContext& ctx, uint8_t* base, bool after) {
  auto* h = static_cast<HookRef*>(user);
  Script& s = *h->script;
  if (s.stopped) return false;
  std::lock_guard<std::recursive_mutex> lock(s.mutex);
  if (s.stopped) return false;
  lua_rawgeti(s.L, LUA_REGISTRYINDEX, h->ref);
  auto* r = static_cast<Regs*>(lua_newuserdatauv(s.L, sizeof(Regs), 0));
  r->ctx = &ctx;
  r->base = base;
  luaL_setmetatable(s.L, kRegsMeta);
  lua_pushvalue(s.L, -1);
  const int keep = luaL_ref(s.L, LUA_REGISTRYINDEX);  // (the function, the registers: one kept to invalidate)
  bool skip = false;
  Call(s, 1, kHookBudget, &skip);
  lua_rawgeti(s.L, LUA_REGISTRYINDEX, keep);
  static_cast<Regs*>(lua_touserdata(s.L, -1))->ctx = nullptr;
  lua_pop(s.L, 1);
  luaL_unref(s.L, LUA_REGISTRYINDEX, keep);
  return !after && skip;
}

int HookAdd(lua_State* L, bool after) {
  const uint32_t address = uint32_t(luaL_checkinteger(L, 1));
  luaL_checktype(L, 2, LUA_TFUNCTION);
  Script* s = Of(L);
  lua_pushvalue(L, 2);
  auto ref = std::make_unique<HookRef>(HookRef{s, luaL_ref(L, LUA_REGISTRYINDEX)});
  const int id = izuna::hooks::Add(address, after, LuaHook, ref.get());
  if (id < 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, ref->ref);
    return luaL_error(L, "no game function starts at %s", fmt::format("0x{:08X}", address).c_str());
  }
  ref->id = id;
  s->hooks.push_back(std::move(ref));
  lua_pushinteger(L, id);
  return 1;
}
int HookBefore(lua_State* L) { return HookAdd(L, false); }
int HookAfter(lua_State* L) { return HookAdd(L, true); }
int HookRemove(lua_State* L) {
  izuna::hooks::Remove(int(luaL_checkinteger(L, 1)));
  return 0;
}
int HookIsFunction(lua_State* L) {
  lua_pushboolean(L, izuna::hooks::IsFunction(uint32_t(luaL_checkinteger(L, 1))));
  return 1;
}

// --- The renderer -------------------------------------------------------------------

// A shader's hash, as the frame capture's draw log writes it (16 hex digits),
// or as an integer.
uint64_t ShaderHash(lua_State* L, int arg) {
  if (lua_type(L, arg) == LUA_TNUMBER) return uint64_t(luaL_checkinteger(L, arg));
  const char* t = luaL_checkstring(L, arg);
  char* end = nullptr;
  const uint64_t v = std::strtoull(t, &end, 16);
  if (!*t || *end) luaL_argerror(L, arg, "a shader's hash: 16 hex digits");
  return v;
}
int RenderHide(lua_State* L) {
  ng2native::HideShader(ShaderHash(L, 1), true);
  return 0;
}
int RenderShow(lua_State* L) {
  ng2native::HideShader(ShaderHash(L, 1), false);
  return 0;
}

void Lib(lua_State* L, const char* name, const luaL_Reg* fns) {
  lua_newtable(L);  // (not luaL_newlib: it sizes the table from an array, and this is a pointer)
  luaL_setfuncs(L, fns, 0);
  lua_setglobal(L, name);
}

void Open(lua_State* L) {
  static const luaL_Reg kLibs[] = {{LUA_GNAME, luaopen_base},          {LUA_TABLIBNAME, luaopen_table},
                                   {LUA_STRLIBNAME, luaopen_string},   {LUA_MATHLIBNAME, luaopen_math},
                                   {LUA_UTF8LIBNAME, luaopen_utf8},    {LUA_COLIBNAME, luaopen_coroutine}};
  for (const luaL_Reg& lib : kLibs) {
    luaL_requiref(L, lib.name, lib.func, 1);
    lua_pop(L, 1);
  }
  for (const char* gone : {"dofile", "loadfile"}) {
    lua_pushnil(L);
    lua_setglobal(L, gone);
  }
  lua_register(L, "load", Load);
  lua_register(L, "log", Log);
  lua_register(L, "print", Log);
  lua_register(L, "require", Require);
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, "izuna.loaded");

  static const luaL_Reg kMem[] = {
      {"u8", Read<uint8_t>},          {"s8", Read<int8_t>},           {"u16", Read<uint16_t>},
      {"s16", Read<int16_t>},         {"u32", Read<uint32_t>},        {"s32", Read<int32_t>},
      {"f32", Read<float>},           {"f64", Read<double>},          {"set_u8", Write<uint8_t>},
      {"set_s8", Write<int8_t>},      {"set_u16", Write<uint16_t>},   {"set_s16", Write<int16_t>},
      {"set_u32", Write<uint32_t>},   {"set_s32", Write<int32_t>},    {"set_f32", Write<float>},
      {"set_f64", Write<double>},     {"string", MemString},          {"bytes", MemBytes},
      {"set_bytes", MemSetBytes},     {"readable", MemReadable},      {nullptr, nullptr}};
  static const luaL_Reg kPad[] = {{"held", PadHeld},       {"pressed", PadPressed}, {"released", PadReleased},
                                  {"buttons", PadButtons}, {"sticks", PadSticks},   {"triggers", PadTriggers},
                                  {nullptr, nullptr}};
  static const luaL_Reg kUi[] = {
      {"text", UiText}, {"box", UiBox}, {"frame", UiFrame}, {"image", UiImage}, {"notify", UiNotify},
      {nullptr, nullptr}};
  static const luaL_Reg kGame[] = {{"time", GameTime}, {"file_name", GameFileName}, {nullptr, nullptr}};
  static const luaL_Reg kHook[] = {{"before", HookBefore},
                                   {"after", HookAfter},
                                   {"remove", HookRemove},
                                   {"is_function", HookIsFunction},
                                   {nullptr, nullptr}};
  static const luaL_Reg kRender[] = {{"hide_shader", RenderHide}, {"show_shader", RenderShow}, {nullptr, nullptr}};
  static const luaL_Reg kRegs[] = {{"__index", RegsIndex}, {"__newindex", RegsNewIndex}, {nullptr, nullptr}};
  luaL_newmetatable(L, kRegsMeta);
  luaL_setfuncs(L, kRegs, 0);
  lua_pop(L, 1);
  Lib(L, "hook", kHook);
  Lib(L, "render", kRender);
  Lib(L, "mem", kMem);
  Lib(L, "pad", kPad);
  Lib(L, "ui", kUi);
  Lib(L, "game", kGame);
  lua_getglobal(L, "game");
  lua_pushinteger(L, 0);
  lua_setfield(L, -2, "frame");
  lua_pushstring(L, Of(L)->mod.c_str());
  lua_setfield(L, -2, "mod");
  lua_pop(L, 1);
}

int Traceback(lua_State* L) {
  const char* msg = lua_tostring(L, 1);
  luaL_traceback(L, L, msg ? msg : "(an error that is not a string)", 1);
  return 1;
}

// Calls what is on the stack (a function and `args` arguments) within the
// script's budget; false (and the error logged) if it failed.
bool Call(Script& s, int args, uint64_t budget, bool* result) {
  const uint64_t ticks = s.ticks, outer = s.budget;  // (a hook inside another call: its budget, then the outer's)
  s.ticks = 0;
  s.budget = budget;
  const int base = lua_gettop(s.L) - args;
  lua_pushcfunction(s.L, Traceback);
  lua_insert(s.L, base);
  const int status = lua_pcall(s.L, args, result ? 1 : 0, base);
  lua_remove(s.L, base);
  s.ticks = ticks;
  s.budget = outer;
  if (status == LUA_OK) {
    if (result) {
      *result = lua_toboolean(s.L, -1);
      lua_pop(s.L, 1);
    }
    return true;
  }
  const char* msg = lua_tostring(s.L, -1);
  REXLOG_WARN("[script] {}: {}", s.mod, msg ? msg : "error");
  lua_pop(s.L, 1);
  if (++s.errors >= kMaxErrors) {
    s.stopped = true;
    g_running.fetch_sub(1);
    for (const auto& h : s.hooks) izuna::hooks::Remove(h->id);
    REXLOG_WARN("[script] {}: stopped after {} errors", s.mod, s.errors);
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    g_notices.push_back({s.mod + ": its script stopped (see the log)", Now() + 8});
  }
  return false;
}

}  // namespace

void Start(const std::vector<mods::Mod>& mods) {
  for (const mods::Mod& mod : mods) {
    if (!mod.enabled || mod.script.empty()) continue;
    std::string text;
    const fs::path main = mod.dir / fs::path(mod.script);
    if (!ReadText(main, &text)) {
      REXLOG_WARN("[script] {}: {} could not be read", mod.name, main.string());
      continue;
    }
    auto s = std::make_unique<Script>();
    s->mod = mod.name;
    s->dir = mod.dir;
    s->scripts = mod.dir / "scripts";
    s->L = lua_newstate(Alloc, s.get());
    if (!s->L) continue;
    *static_cast<Script**>(lua_getextraspace(s->L)) = s.get();
    lua_sethook(s->L, CountHook, LUA_MASKCOUNT, 10000);
    Open(s->L);
    g_running.fetch_add(1);
    if (luaL_loadbufferx(s->L, text.data(), text.size(), ("@" + mod.script).c_str(), "t") != LUA_OK) {
      REXLOG_WARN("[script] {}: {}", mod.name, lua_tostring(s->L, -1));
      lua_close(s->L);
      g_running.fetch_sub(1);
      continue;
    }
    std::lock_guard<std::recursive_mutex> lock(s->mutex);  // (its hooks may fire as soon as it adds them)
    if (!Call(*s, 0, kStartBudget, nullptr)) {
      // (its first run failed: it stays loaded only if what it defined may still work)
      s->errors = std::max(s->errors, kMaxErrors - 1);
    }
    REXLOG_INFO("[script] {}: {} loaded", mod.name, mod.script);
    g_scripts.push_back(std::move(s));
  }
}

void OnFrame() {
  if (g_scripts.empty()) return;
  ++g_frame;
  for (auto& sp : g_scripts) {
    Script& s = *sp;
    if (s.stopped) continue;
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    lua_getglobal(s.L, "game");
    lua_pushinteger(s.L, lua_Integer(g_frame));
    lua_setfield(s.L, -2, "frame");
    lua_pop(s.L, 1);
    if (lua_getglobal(s.L, "on_frame") != LUA_TFUNCTION) {
      lua_pop(s.L, 1);
      continue;
    }
    Call(s, 0, kCallBudget, nullptr);
  }
  std::lock_guard<std::mutex> lock(g_draw_mutex);
  g_shown.swap(g_pending);
  g_pending.clear();
}

int Running() { return g_running.load(); }

Overlay::Overlay(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate)
    : rex::ui::ImGuiDialog(drawer), immediate_(immediate) {}
Overlay::~Overlay() = default;

// A picture for ui.image, read the first time it is drawn (on the UI thread).
rex::ui::ImmediateTexture* Overlay::Picture(const std::string& path) {
  auto it = pictures_.find(path);
  if (it != pictures_.end()) return it->second.get();
  std::unique_ptr<rex::ui::ImmediateTexture> texture;
  std::string text;
  if (immediate_ && pictures_.size() < 256 && ReadText(path, &text)) {
    int w = 0, h = 0;
    const std::vector<uint8_t> rgba =
        rex::ui::DecodeImageRGBA(reinterpret_cast<const uint8_t*>(text.data()), text.size(), w, h);
    if (!rgba.empty() && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
      texture = immediate_->CreateTexture(uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear, false,
                                          rgba.data());
    }
  }
  if (!texture) REXLOG_WARN("[script] ui.image: {} could not be read (PNG, JPEG, TGA or BMP, 4096 at most)", path);
  return (pictures_[path] = std::move(texture)).get();
}

void Overlay::OnDraw(ImGuiIO& io) {
  std::lock_guard<std::mutex> lock(g_draw_mutex);
  const double now = Now();
  g_notices.erase(std::remove_if(g_notices.begin(), g_notices.end(), [&](const Notice& n) { return n.until < now; }),
                  g_notices.end());
  if (g_shown.empty() && g_notices.empty()) return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  // The 1920x1080 space, fitted to the display.
  const float k = std::min(io.DisplaySize.x / 1920.0f, io.DisplaySize.y / 1080.0f);
  const float ox = (io.DisplaySize.x - 1920.0f * k) * 0.5f, oy = (io.DisplaySize.y - 1080.0f * k) * 0.5f;
  auto P = [&](float x, float y) { return ImVec2(ox + x * k, oy + y * k); };
  auto C = [](uint32_t rgba) {
    return IM_COL32((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, rgba & 0xFF);
  };
  for (const Shape& s : g_shown) {
    switch (s.kind) {
      case Shape::kText:
        dl->AddText(font, std::clamp(s.size, 8.0f, 200.0f) * k, P(s.x, s.y), C(s.colour), s.text.c_str());
        break;
      case Shape::kRect: dl->AddRectFilled(P(s.x, s.y), P(s.x + s.w, s.y + s.h), C(s.colour)); break;
      case Shape::kFrame: dl->AddRect(P(s.x, s.y), P(s.x + s.w, s.y + s.h), C(s.colour), 0, 0, 2.0f * k); break;
      case Shape::kImage:
        if (auto* picture = Picture(s.text)) {
          dl->AddImage(reinterpret_cast<ImTextureID>(picture), P(s.x, s.y), P(s.x + s.w, s.y + s.h), ImVec2(0, 0),
                       ImVec2(1, 1), C(s.colour));
        }
        break;
    }
  }
  float y = 120;
  for (const Notice& n : g_notices) {
    const float a = float(std::min(1.0, (n.until - now) * 2.0));
    const ImVec2 size = font->CalcTextSizeA(26 * k, FLT_MAX, 0, n.text.c_str());
    const ImVec2 at = P(1880, y);
    dl->AddRectFilled(ImVec2(at.x - size.x - 36 * k, at.y), ImVec2(at.x, at.y + 48 * k),
                      IM_COL32(10, 10, 12, int(220 * a)), 6 * k);
    dl->AddText(font, 26 * k, ImVec2(at.x - size.x - 18 * k, at.y + 10 * k), IM_COL32(236, 230, 214, int(255 * a)),
                n.text.c_str());
    y += 58;
  }
}

}  // namespace izuna::scripts
