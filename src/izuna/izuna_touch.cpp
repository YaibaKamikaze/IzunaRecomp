// [IzunaRecomp] On-screen touch controls (Android) - see izuna_touch.h.
#include "izuna/izuna_touch.h"

#include <rex/input/input_driver.h>

#if defined(__ANDROID__)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include <SDL3/SDL_events.h>
#include <imgui.h>

#include <rex/logging.h>

#include "ps5/ps5_pad.h"

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

namespace izuna::touch {
namespace {

// The Xbox 360 controller's button bits (XINPUT_GAMEPAD_*).
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010,
                   kBack = 0x0020, kR3 = 0x0080, kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000,
                   kX = 0x4000, kY = 0x8000;
constexpr uint16_t kLT = 0, kRT = 0;  // (triggers: their own fields)

struct Button {
  const char* label;
  float x, y, r;     // in pixels, for the current display
  uint16_t xinput;   // 0 for a trigger
  int trigger;       // 1 left, 2 right, 0 none
  uint32_t pad;      // ng2ps5::Pad's bit
};

// The layout, by the display's size: sizes in fractions of its height, so it
// looks the same on any screen; the right side measured from the right edge.
std::vector<Button> Layout(float w, float h) {
  const float face_x = w - 0.24f * h, face_y = 0.60f * h, d = 0.12f * h, fr = 0.068f * h;
  const float pad_x = 0.17f * h, pad_y = 0.36f * h, pd = 0.075f * h, pr = 0.045f * h;
  return {
      {"Y", face_x, face_y - d, fr, kY, 0, ng2ps5::kPadTriangle},
      {"A", face_x, face_y + d, fr, kA, 0, ng2ps5::kPadCross},
      {"X", face_x - d, face_y, fr, kX, 0, ng2ps5::kPadSquare},
      {"B", face_x + d, face_y, fr, kB, 0, ng2ps5::kPadCircle},
      {"LT", 0.11f * h, 0.11f * h, 0.065f * h, kLT, 1, ng2ps5::kPadL2},
      {"LB", 0.29f * h, 0.11f * h, 0.065f * h, kLB, 0, ng2ps5::kPadL1},
      {"RT", w - 0.11f * h, 0.11f * h, 0.065f * h, kRT, 2, ng2ps5::kPadR2},
      {"RB", w - 0.29f * h, 0.11f * h, 0.065f * h, kRB, 0, ng2ps5::kPadR1},
      {"BACK", w * 0.5f - 0.11f * h, 0.075f * h, 0.05f * h, kBack, 0, ng2ps5::kPadTouchpad},
      {"START", w * 0.5f + 0.11f * h, 0.075f * h, 0.05f * h, kStart, 0, ng2ps5::kPadOptions},
      {"^", pad_x, pad_y - pd, pr, kUp, 0, ng2ps5::kPadUp},
      {"v", pad_x, pad_y + pd, pr, kDown, 0, ng2ps5::kPadDown},
      {"<", pad_x - pd, pad_y, pr, kLeft, 0, ng2ps5::kPadLeft},
      {">", pad_x + pd, pad_y, pr, kRight, 0, ng2ps5::kPadRight},
      {"R3", w - 0.52f * h, 0.90f * h, 0.045f * h, kR3, 0, 0},
  };
}

constexpr int kNone = -1, kLeftStick = 100, kRightStick = 101;

struct Finger {
  SDL_FingerID id;
  int role;           // a button's index, a stick, or none
  float ox, oy, x, y; // where it landed and where it is, in pixels
};

std::atomic<bool> g_enabled{false};
std::mutex g_mutex;
std::vector<Finger> g_fingers;
float g_w = 1920.0f, g_h = 1080.0f;  // the display's size, from the last Draw

float StickRadius() { return 0.13f * g_h; }

struct Reading {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1, y up
  uint32_t pad = 0;
};

// The virtual controller now (under g_mutex).
Reading ReadLocked() {
  Reading out;
  if (!g_enabled.load(std::memory_order_relaxed)) return out;
  const auto layout = Layout(g_w, g_h);
  for (const Finger& f : g_fingers) {
    if (f.role >= 0 && f.role < int(layout.size())) {
      const Button& b = layout[size_t(f.role)];
      out.buttons |= b.xinput;
      if (b.trigger == 1) out.lt = 255;
      if (b.trigger == 2) out.rt = 255;
      out.pad |= b.pad;
    } else if (f.role == kLeftStick || f.role == kRightStick) {
      float dx = (f.x - f.ox) / StickRadius(), dy = (f.y - f.oy) / StickRadius();
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len > 1.0f) {
        dx /= len;
        dy /= len;
      }
      if (f.role == kLeftStick) {
        out.lx = dx;
        out.ly = -dy;
        // (the menus: the left stick also steers)
        if (dy < -0.5f) out.pad |= ng2ps5::kPadUp;
        if (dy > 0.5f) out.pad |= ng2ps5::kPadDown;
        if (dx < -0.5f) out.pad |= ng2ps5::kPadLeft;
        if (dx > 0.5f) out.pad |= ng2ps5::kPadRight;
      } else {
        out.rx = dx;
        out.ry = -dy;
      }
    }
  }
  return out;
}

int RoleAt(float x, float y) {
  const auto layout = Layout(g_w, g_h);
  for (size_t i = 0; i < layout.size(); ++i) {
    const Button& b = layout[i];
    const float dx = x - b.x, dy = y - b.y;
    if (dx * dx + dy * dy <= (b.r * 1.3f) * (b.r * 1.3f)) return int(i);
  }
  if (x < 0.42f * g_w && y > 0.45f * g_h) return kLeftStick;
  if (y > 0.30f * g_h) return kRightStick;
  return kNone;
}

bool SDLCALL OnEvent(void*, SDL_Event* e) {
  if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION && e->type != SDL_EVENT_FINGER_UP &&
      e->type != SDL_EVENT_FINGER_CANCELED)
    return true;
  std::lock_guard<std::mutex> lock(g_mutex);
  const float x = e->tfinger.x * g_w, y = e->tfinger.y * g_h;
  auto it = std::find_if(g_fingers.begin(), g_fingers.end(),
                         [&](const Finger& f) { return f.id == e->tfinger.fingerID; });
  if (e->type == SDL_EVENT_FINGER_DOWN) {
    if (it != g_fingers.end()) g_fingers.erase(it);
    g_fingers.push_back({e->tfinger.fingerID, RoleAt(x, y), x, y, x, y});
  } else if (e->type == SDL_EVENT_FINGER_MOTION) {
    if (it != g_fingers.end()) {
      it->x = x;
      it->y = y;
    }
  } else if (it != g_fingers.end()) {
    g_fingers.erase(it);
  }
  return true;
}

constexpr rex::input::DeviceId kTouchDevice = static_cast<rex::input::DeviceId>(0x4E473254);  // 'NG2T'

class TouchDriver final : public rex::input::InputDriver {
 public:
  TouchDriver() : InputDriver(nullptr, 0) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kTouchDevice;
    info.name = "Touch controls";
    info.synthetic = true;
    out.push_back(info);
  }
  X_RESULT GetDeviceState(rex::input::DeviceId id, X_INPUT_STATE* s) override {
    if (id != kTouchDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!s) return X_ERROR_BAD_ARGUMENTS;
    std::memset(s, 0, sizeof(*s));
    s->packet_number = ++packet_;
    Reading r;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      r = ReadLocked();
    }
    auto axis = [](float v) { return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f); };
    s->gamepad.buttons = r.buttons;
    s->gamepad.left_trigger = r.lt;
    s->gamepad.right_trigger = r.rt;
    s->gamepad.thumb_lx = axis(r.lx);
    s->gamepad.thumb_ly = axis(r.ly);
    s->gamepad.thumb_rx = axis(r.rx);
    s->gamepad.thumb_ry = axis(r.ry);
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t, X_INPUT_CAPABILITIES* caps) override {
    if (id != kTouchDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
      std::memset(caps, 0, sizeof(*caps));
      caps->type = 0x01;
      caps->sub_type = 0x01;
      caps->gamepad.buttons = 0xF3FF;
      caps->gamepad.left_trigger = 0xFF;
      caps->gamepad.right_trigger = 0xFF;
      caps->gamepad.thumb_lx = int16_t(0x7FFF);
      caps->gamepad.thumb_ly = int16_t(0x7FFF);
      caps->gamepad.thumb_rx = int16_t(0x7FFF);
      caps->gamepad.thumb_ry = int16_t(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(rex::input::DeviceId, X_INPUT_VIBRATION*) override { return X_ERROR_SUCCESS; }
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId, uint32_t, X_INPUT_KEYSTROKE*) override {
    return X_ERROR_EMPTY;
  }

 private:
  uint32_t packet_ = 0;
};

}  // namespace

void SetEnabled(bool enabled) {
  g_enabled.store(enabled, std::memory_order_release);
  if (!enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_fingers.clear();
  }
}

bool Enabled() { return g_enabled.load(std::memory_order_acquire); }

void Install() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  SDL_AddEventWatch(OnEvent, nullptr);
  REXLOG_INFO("Touch controls: watching the screen ({})", Enabled() ? "on" : "off");
}

uint32_t PadBits() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return ReadLocked().pad;
}

std::unique_ptr<rex::input::InputDriver> MakeDriver() { return std::make_unique<TouchDriver>(); }

void Draw(float width, float height) {
  if (!Enabled() || width <= 0 || height <= 0) return;
  std::vector<Finger> fingers;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_w = width;
    g_h = height;
    fingers = g_fingers;
  }
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  const auto layout = Layout(width, height);
  auto held = [&](int role) {
    return std::any_of(fingers.begin(), fingers.end(), [&](const Finger& f) { return f.role == role; });
  };
  auto circle = [&](float x, float y, float r, bool on, const char* label) {
    dl->AddCircleFilled(ImVec2(x, y), r, on ? IM_COL32(255, 255, 255, 110) : IM_COL32(20, 20, 24, 90), 40);
    dl->AddCircle(ImVec2(x, y), r, on ? IM_COL32(255, 255, 255, 230) : IM_COL32(236, 230, 214, 150), 40,
                  std::max(2.0f, r * 0.06f));
    if (label && *label) {
      const float size = r * (std::strlen(label) > 2 ? 0.55f : 0.9f);
      const ImVec2 t = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
      dl->AddText(font, size, ImVec2(x - t.x * 0.5f, y - t.y * 0.5f),
                  on ? IM_COL32(20, 20, 24, 255) : IM_COL32(236, 230, 214, 210), label);
    }
  };
  for (size_t i = 0; i < layout.size(); ++i) circle(layout[i].x, layout[i].y, layout[i].r, held(int(i)), layout[i].label);
  // The sticks: where the finger landed (or their resting place), and the knob.
  const float sr = 0.13f * height;
  auto stick = [&](int role, float rest_x, float rest_y) {
    const Finger* f = nullptr;
    for (const Finger& g : fingers)
      if (g.role == role) f = &g;
    const float cx = f ? f->ox : rest_x, cy = f ? f->oy : rest_y;
    dl->AddCircleFilled(ImVec2(cx, cy), sr, IM_COL32(20, 20, 24, f ? 90 : 55), 48);
    dl->AddCircle(ImVec2(cx, cy), sr, IM_COL32(236, 230, 214, f ? 170 : 90), 48, 3.0f);
    float kx = cx, ky = cy;
    if (f) {
      float dx = f->x - f->ox, dy = f->y - f->oy;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len > sr) {
        dx *= sr / len;
        dy *= sr / len;
      }
      kx += dx;
      ky += dy;
    }
    dl->AddCircleFilled(ImVec2(kx, ky), sr * 0.42f, IM_COL32(236, 230, 214, f ? 200 : 110), 32);
  };
  stick(kLeftStick, 0.24f * height, 0.76f * height);
  stick(kRightStick, width * 0.62f, 0.78f * height);
}

}  // namespace izuna::touch

#else  // not Android: none

namespace izuna::touch {
void SetEnabled(bool) {}
bool Enabled() { return false; }
void Install() {}
uint32_t PadBits() { return 0; }
std::unique_ptr<rex::input::InputDriver> MakeDriver() { return nullptr; }
void Draw(float, float) {}
}  // namespace izuna::touch

#endif
