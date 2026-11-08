// Ninja Gaiden II for PS5 - the DualSense for the menus (ps5_pad.h).

#include "ps5/ps5_pad.h"
#include "izuna/izuna_touch.h"

#include <cstring>

#if defined(__PROSPERO__)
extern "C" {
int sceUserServiceInitialize(void* params);
int sceUserServiceGetInitialUser(int32_t* user_id);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void* param);
int scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
int scePadReadState(int32_t handle, void* data);
}
#else
#include <atomic>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <rex/logging.h>
#endif

namespace ng2ps5 {

namespace {

#if defined(__PROSPERO__)
// ScePadData's first bytes, as verified on hardware by ps5-plume-triangle and
// the SDK's input driver; the rest of the structure is not needed here.
struct ScePadData {
  uint32_t buttons;
  uint8_t lx, ly, rx, ry, l2, r2;
  uint8_t padding[2];
  uint8_t rest[240];
};

constexpr uint32_t kSceOptions = 0x00000008u, kSceUp = 0x00000010u, kSceRight = 0x00000020u,
                   kSceDown = 0x00000040u, kSceLeft = 0x00000080u, kSceL2 = 0x00000100u,
                   kSceR2 = 0x00000200u, kSceL1 = 0x00000400u, kSceR1 = 0x00000800u,
                   kSceTriangle = 0x00001000u, kSceCircle = 0x00002000u, kSceCross = 0x00004000u,
                   kSceSquare = 0x00008000u, kSceTouchpad = 0x00100000u;

// SCE_PAD_ERROR_ALREADY_OPENED: the SDK's input driver (or an earlier menu)
// has the pad; scePadGetHandle hands back the same handle.
constexpr int kAlreadyOpened = int(0x80920004u);

#endif
}  // namespace

#if defined(__PROSPERO__)
PromptStyle LastPromptStyle() { return PromptStyle::kPlayStation; }

// The DualSense's buttons as the menus' (false: no pad).
bool ReadRaw(int32_t& handle, uint32_t& b) {
  if (handle < 0) {
    sceUserServiceInitialize(nullptr);  // (already initialized is fine)
    scePadInit();
    int32_t user = -1;
    if (sceUserServiceGetInitialUser(&user) != 0 || user < 0)
      return false;
    handle = scePadOpen(user, 0, 0, nullptr);
    if (handle == kAlreadyOpened || handle < 0)
      handle = scePadGetHandle(user, 0, 0);
    if (handle < 0)
      return false;
  }
  ScePadData d;
  std::memset(&d, 0, sizeof(d));
  if (scePadReadState(handle, &d) != 0)
    return false;
  // The left stick moves like the d-pad, past half deflection.
  b = 0;
  if ((d.buttons & kSceUp) || d.ly < 64) b |= kPadUp;
  if ((d.buttons & kSceDown) || d.ly > 192) b |= kPadDown;
  if ((d.buttons & kSceLeft) || d.lx < 64) b |= kPadLeft;
  if ((d.buttons & kSceRight) || d.lx > 192) b |= kPadRight;
  if (d.buttons & kSceCross) b |= kPadCross;
  if (d.buttons & kSceCircle) b |= kPadCircle;
  if (d.buttons & kSceSquare) b |= kPadSquare;
  if (d.buttons & kSceTriangle) b |= kPadTriangle;
  if (d.buttons & kSceL1) b |= kPadL1;
  if (d.buttons & kSceR1) b |= kPadR1;
  if (d.buttons & kSceL2) b |= kPadL2;
  if (d.buttons & kSceR2) b |= kPadR2;
  if (d.buttons & kSceOptions) b |= kPadOptions;
  if (d.buttons & kSceTouchpad) b |= kPadTouchpad;
  return true;
}
#else
// [IzunaRecomp] A PC: the keyboard (through ImGui, which has the window's
// keys) and every controller SDL sees, at once.
//   Enter / Space: Cross (A)  Esc / Backspace: Circle (B)  R: Square (X)
//   T: Triangle (Y)  Q / E: L1 / R1  Z / C: L2 / R2  F1: Options (Start)
// Arrows or WASD move.
std::atomic<PromptStyle> g_style{PromptStyle::kKeyboard};
PromptStyle LastPromptStyle() { return g_style.load(std::memory_order_relaxed); }
std::vector<SDL_Gamepad*> g_opened;  // (the controllers the menus opened: closed on Release)

bool ReadRaw(int32_t& handle, uint32_t& b) {
  if (handle < 0) {
    handle = SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : 0;  // (no controllers: the keyboard still works)
  }
  b = 0;
  uint32_t keys = 0;
  if (ImGui::GetCurrentContext()) {
    auto k = [](ImGuiKey key) { return ImGui::IsKeyDown(key); };
    if (k(ImGuiKey_UpArrow) || k(ImGuiKey_W)) keys |= kPadUp;
    if (k(ImGuiKey_DownArrow) || k(ImGuiKey_S)) keys |= kPadDown;
    if (k(ImGuiKey_LeftArrow) || k(ImGuiKey_A)) keys |= kPadLeft;
    if (k(ImGuiKey_RightArrow) || k(ImGuiKey_D)) keys |= kPadRight;
    if (k(ImGuiKey_Enter) || k(ImGuiKey_KeypadEnter) || k(ImGuiKey_Space)) keys |= kPadCross;
    if (k(ImGuiKey_Escape) || k(ImGuiKey_Backspace)) keys |= kPadCircle;
    if (k(ImGuiKey_R)) keys |= kPadSquare;
    if (k(ImGuiKey_T)) keys |= kPadTriangle;
    if (k(ImGuiKey_Q)) keys |= kPadL1;
    if (k(ImGuiKey_E)) keys |= kPadR1;
    if (k(ImGuiKey_Z)) keys |= kPadL2;
    if (k(ImGuiKey_C)) keys |= kPadR2;
    if (k(ImGuiKey_F1)) keys |= kPadOptions;
  }
  uint32_t pad = 0;
  PromptStyle pad_style = PromptStyle::kXbox;
  if (handle > 0) {
    SDL_UpdateGamepads();
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
      SDL_Gamepad* g = SDL_GetGamepadFromID(ids[i]);
      if (!g) {
        g = SDL_OpenGamepad(ids[i]);  // (kept open while the menus read it)
        if (g) {
          g_opened.push_back(g);
          REXLOG_INFO("Hub: controller \"{}\" (type {})", SDL_GetGamepadName(g) ? SDL_GetGamepadName(g) : "?",
                      int(SDL_GetGamepadType(g)));
        }
      }
      if (!g) continue;
      auto on = [g](SDL_GamepadButton x) { return SDL_GetGamepadButton(g, x); };
      const int lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
      uint32_t p = 0;
      if (on(SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -16384) p |= kPadUp;
      if (on(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > 16384) p |= kPadDown;
      if (on(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -16384) p |= kPadLeft;
      if (on(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > 16384) p |= kPadRight;
      if (on(SDL_GAMEPAD_BUTTON_SOUTH)) p |= kPadCross;
      if (on(SDL_GAMEPAD_BUTTON_EAST)) p |= kPadCircle;
      if (on(SDL_GAMEPAD_BUTTON_WEST)) p |= kPadSquare;
      if (on(SDL_GAMEPAD_BUTTON_NORTH)) p |= kPadTriangle;
      if (on(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) p |= kPadL1;
      if (on(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) p |= kPadR1;
      if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384) p |= kPadL2;
      if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384) p |= kPadR2;
      if (on(SDL_GAMEPAD_BUTTON_START)) p |= kPadOptions;
      if (on(SDL_GAMEPAD_BUTTON_BACK) || on(SDL_GAMEPAD_BUTTON_TOUCHPAD)) p |= kPadTouchpad;
      if (p) {
        const SDL_GamepadType t = SDL_GetGamepadType(g);
        pad_style = (t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5)
                        ? PromptStyle::kPlayStation
                        : PromptStyle::kXbox;
      }
      pad |= p;
    }
    SDL_free(ids);
  }
  // The prompts follow whatever was pressed last.
  if (pad) {
    g_style.store(pad_style, std::memory_order_relaxed);
  } else if (keys) {
    g_style.store(PromptStyle::kKeyboard, std::memory_order_relaxed);
  }
#if defined(__ANDROID__)
  pad |= izuna::touch::PadBits();  // [IzunaRecomp] the on-screen controls, if on
#endif
  b = keys | pad;
  return true;
}
#endif

Pad::~Pad() { Release(); }

void Pad::Release() {
#if !defined(__PROSPERO__)
  if (handle_ > 0) {
    for (SDL_Gamepad* g : g_opened) SDL_CloseGamepad(g);
    g_opened.clear();
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }
  handle_ = -1;
#endif
}

PadState Pad::Read() {
  PadState out;
  uint32_t b = 0;
  if (!ReadRaw(handle_, b))
    return out;
  out.connected = true;
  const auto now = std::chrono::steady_clock::now();
  out.held = b;
  out.pressed = b & ~last_;
  out.released = last_ & ~b;
  for (int i = 0; i < 16; ++i) {
    if (out.pressed & (1u << i))
      down_since_[i] = now;
  }
  // Held directions repeat: after 0.4 s, then every 0.09 s - quick enough to
  // run down a long list, slow enough to stop on the row wanted.
  constexpr uint32_t kDirs = kPadUp | kPadDown | kPadLeft | kPadRight;
  if (b & kDirs) {
    if ((b & kDirs) != (last_ & kDirs)) {
      repeat_at_ = now + std::chrono::milliseconds(400);
    } else if (now >= repeat_at_) {
      out.pressed |= b & kDirs;
      repeat_at_ = now + std::chrono::milliseconds(90);
    }
  }
  last_ = b;
  if (first_) {
    first_ = false;
    out.pressed = 0;
    out.released = 0;
  }
  return out;
}

float Pad::HeldFor(uint32_t button) const {
  if (!(last_ & button))
    return 0.0f;
  for (int i = 0; i < 16; ++i) {
    if (button == (1u << i))
      return std::chrono::duration<float>(std::chrono::steady_clock::now() - down_since_[i]).count();
  }
  return 0.0f;
}

}  // namespace ng2ps5
