// Ninja Gaiden II for PS5 - the hub's drawing, fonts and settings list
// (hub_ui.h).

#include "ps5/hub/hub_ui.h"
#include "izuna/izuna_keys.h"

#include <algorithm>
#include <cstring>

#include <algorithm>
#include <cmath>

namespace ng2ps5::hub {

// --- Fonts -------------------------------------------------------------------

// Built into the app so the hub works with nothing copied but the package:
// SIL Open Font License faces, licences beside them in ps5/assets/fonts.
#if defined(_WIN32)  // [IzunaRecomp] (COFF: .rdata, and no .previous - back to .text)
#define NG2PS5_INCBIN_SECTION ".section .rdata,\"dr\"\n"
#define NG2PS5_INCBIN_BACK ".text\n"
#else
#define NG2PS5_INCBIN_SECTION ".section .rodata\n"
#define NG2PS5_INCBIN_BACK ".previous\n"
#endif
#define NG2PS5_INCBIN(name, file)                                                         \
  __asm__(NG2PS5_INCBIN_SECTION ".global " #name "\n.balign 16\n" #name ":\n.incbin \"" file \
          "\"\n.global " #name "_end\n" #name "_end:\n.byte 0\n" NG2PS5_INCBIN_BACK);   \
  extern "C" const unsigned char name[];                                                \
  extern "C" const unsigned char name##_end[];
NG2PS5_INCBIN(ng2ps5_font_cinzel_bold, NG2_PS5_FONTS_DIR "/cinzel-700.ttf")
NG2PS5_INCBIN(ng2ps5_font_cinzel_semibold, NG2_PS5_FONTS_DIR "/cinzel-600.ttf")
NG2PS5_INCBIN(ng2ps5_font_inter, NG2_PS5_FONTS_DIR "/inter-400.ttf")
NG2PS5_INCBIN(ng2ps5_font_inter_semibold, NG2_PS5_FONTS_DIR "/inter-600.ttf")

Fonts& GetFonts() {
  static Fonts fonts;
  return fonts;
}

namespace {
ImFont* AddFont(ImFontAtlas* atlas, const unsigned char* begin, const unsigned char* end, float size) {
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;  // (built into the app, not the heap)
  cfg.OversampleH = 2;
  return atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(begin), int(end - begin), size, &cfg);
}
}  // namespace

void ConfigureFonts(ImFontAtlas* atlas) {
  // Rasterised for a 4K screen; the canvas draws them at whatever size the
  // display needs (ImGui 1.92 re-rasterises per size on demand).
  Fonts& f = GetFonts();
  f.logo = AddFont(atlas, ng2ps5_font_cinzel_bold, ng2ps5_font_cinzel_bold_end, 96.0f);
  f.menu = f.logo;
  f.heading = AddFont(atlas, ng2ps5_font_cinzel_semibold, ng2ps5_font_cinzel_semibold_end, 48.0f);
  f.body = AddFont(atlas, ng2ps5_font_inter, ng2ps5_font_inter_end, 40.0f);
  f.bold = AddFont(atlas, ng2ps5_font_inter_semibold, ng2ps5_font_inter_semibold_end, 40.0f);
}

// --- Canvas ------------------------------------------------------------------

void Canvas::Begin(ImDrawList* list, const ImVec2& display) {
  dl = list;
  hits.clear();
  s = std::min(display.x / 1920.0f, display.y / 1080.0f);
  ox = (display.x - 1920.0f * s) * 0.5f;
  oy = (display.y - 1080.0f * s) * 0.5f;
}

void ApplyMouse(const ImGuiIO& io, const Canvas& c, const std::vector<Hit>& last, PadState& pad,
                bool buttons_only) {
  static ImVec2 previous(-1, -1);
  const ImVec2 m = io.MousePos;
  const bool inside = m.x >= 0 && m.y >= 0 && c.s > 0;
  const bool moved = inside && (std::fabs(m.x - previous.x) > 0.5f || std::fabs(m.y - previous.y) > 0.5f);
  previous = m;
  if (!inside)
    return;
  const float vx = (m.x - c.ox) / c.s, vy = (m.y - c.oy) / c.s;
  // The last region marked wins (drawn on top).
  const Hit* over = nullptr;
  for (const Hit& h : last) {
    if (buttons_only && !h.button)
      continue;
    if (vx >= h.x && vx < h.x + h.w && vy >= h.y && vy < h.y + h.h)
      over = &h;
  }
  if (over && moved && over->hover)
    over->hover();
  if (over && io.MouseClicked[0]) {
    if (over->click)
      over->click();
    if (over->button)
      pad.pressed |= over->button;
  }
  if (io.MouseClicked[1])
    pad.pressed |= kPadCircle;
  if (io.MouseWheel > 0.0f)
    pad.pressed |= kPadUp;
  else if (io.MouseWheel < 0.0f)
    pad.pressed |= kPadDown;
}

ImU32 Canvas::C(ImU32 col) const {
  if (alpha >= 1.0f)
    return col;
  const uint32_t a = uint32_t(float((col >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

void Canvas::Rect(float x, float y, float w, float h, ImU32 col, float rounding) const {
  dl->AddRectFilled(P(x, y), P(x + w, y + h), C(col), rounding * s);
}

void Canvas::Frame(float x, float y, float w, float h, ImU32 col, float thickness, float rounding) const {
  dl->AddRect(P(x, y), P(x + w, y + h), C(col), rounding * s, 0, thickness * s);
}

void Canvas::Gradient(float x, float y, float w, float h, ImU32 top, ImU32 bottom) const {
  dl->AddRectFilledMultiColor(P(x, y), P(x + w, y + h), C(top), C(top), C(bottom), C(bottom));
}

float Canvas::TextWidth(ImFont* f, float size, const std::string& t) const {
  if (!f)
    f = ImGui::GetFont();
  return f->CalcTextSizeA(size, FLT_MAX, 0.0f, t.c_str()).x;
}

float Canvas::Text(ImFont* f, float size, float x, float y, ImU32 col, const std::string& t, float align) const {
  if (!f)
    f = ImGui::GetFont();
  const float w = TextWidth(f, size, t);
  dl->AddText(f, size * s, P(x - w * align, y), C(col), t.c_str());
  return w;
}

float Canvas::Wrapped(ImFont* f, float size, float x, float y, float width, ImU32 col, const std::string& t,
                      float line) const {
  if (!f)
    f = ImGui::GetFont();
  // Greedy word wrap in virtual units, so the break points do not move with
  // the display's size.
  float yy = y;
  size_t start = 0;
  while (start < t.size()) {
    size_t end = start, last_fit = std::string::npos;
    while (end <= t.size()) {
      if (end == t.size() || t[end] == ' ' || t[end] == '\n') {
        if (TextWidth(f, size, t.substr(start, end - start)) > width && last_fit != std::string::npos)
          break;
        last_fit = end;
        if (end == t.size() || t[end] == '\n')
          break;
      }
      ++end;
    }
    if (last_fit == std::string::npos)
      last_fit = t.size();
    Text(f, size, x, yy, col, t.substr(start, last_fit - start));
    yy += size * line;
    start = last_fit + 1;
  }
  return yy - y;
}

void Canvas::Embossed(ImFont* f, float size, float x, float y, ImU32 face, const std::string& t, float align,
                      float glow) const {
  if (!f)
    f = ImGui::GetFont();
  const float w = TextWidth(f, size, t);
  const float left = x - w * align;
  if (glow > 0.01f) {
    // A soft red bloom: the same text, larger offsets, falling alpha.
    for (int ring = 3; ring >= 1; --ring) {
      const float o = 2.5f * float(ring);
      const ImU32 g = IM_COL32(220, 30, 24, int(46.0f * glow / float(ring)));
      for (int k = 0; k < 8; ++k) {
        const float a = float(k) * 0.785398f;
        dl->AddText(f, size * s, P(left + std::cos(a) * o, y + std::sin(a) * o), C(g), t.c_str());
      }
    }
  }
  // The dark outline, then the bevel: a light top-left edge and a dark
  // bottom-right edge around the face.
  const ImU32 outline = IM_COL32(8, 8, 10, 230);
  for (int k = 0; k < 8; ++k) {
    const float a = float(k) * 0.785398f;
    dl->AddText(f, size * s, P(left + std::cos(a) * 2.2f, y + std::sin(a) * 2.2f), C(outline), t.c_str());
  }
  dl->AddText(f, size * s, P(left + 1.2f, y + 1.6f), C(IM_COL32(40, 36, 32, 255)), t.c_str());
  dl->AddText(f, size * s, P(left - 0.8f, y - 1.0f), C(IM_COL32(255, 252, 240, 200)), t.c_str());
  dl->AddText(f, size * s, P(left, y), C(face), t.c_str());
}

namespace {

// [IzunaRecomp] A key for each of the menus' buttons on a PC keyboard (ps5_pad.cpp).
const char* KeyLabel(uint32_t button) {
  switch (button) {
    case kPadCross: return "Enter";
    case kPadCircle: return "Esc";
    case kPadSquare: return "R";
    case kPadTriangle: return "T";
    case kPadL1: return "Q";
    case kPadR1: return "E";
    case kPadL2: return "Z";
    case kPadR2: return "C";
    case kPadOptions: return "F1";
    case kPadTouchpad: return "Tab";
    default: return nullptr;
  }
}

// The Xbox names for the shoulder and menu buttons.
const char* XboxPill(uint32_t button) {
  switch (button) {
    case kPadL1: return "LB";
    case kPadR1: return "RB";
    case kPadL2: return "LT";
    case kPadR2: return "RT";
    case kPadOptions: return "MENU";
    case kPadTouchpad: return "VIEW";
    default: return nullptr;
  }
}

}  // namespace

// How wide a button's glyph is drawn (Hint lays the row out with it).
float Canvas::GlyphWidth(uint32_t button, float r) const {
  const PromptStyle style = LastPromptStyle();
  if (button == (kPadUp | kPadDown) || button == (kPadLeft | kPadRight)) return r * 2.0f;
  if (style == PromptStyle::kKeyboard) {
    if (const char* k = KeyLabel(button)) return std::max(r * 2.0f, TextWidth(GetFonts().bold, r * 0.95f, k) + r * 1.1f);
  }
  if (style == PromptStyle::kXbox && XboxPill(button)) return r * (std::strlen(XboxPill(button)) > 2 ? 3.6f : 2.6f);
  const bool pill = button == kPadL1 || button == kPadR1 || button == kPadL2 || button == kPadR2 ||
                    button == kPadOptions || button == kPadTouchpad;
  return pill ? r * 2.6f : r * 2.0f;
}

void Canvas::Glyph(uint32_t button, float cx, float cy, float r) const {
  const ImVec2 c = P(cx, cy);
  const float R = r * s;
  const ImU32 ring = C(IM_COL32(232, 232, 232, 255));
  const ImU32 fill = C(IM_COL32(30, 30, 34, 235));
  auto pill = [&](const char* label) {
    const float w = GlyphWidth(button, r), h = r * 1.5f;
    dl->AddRectFilled(P(cx - w / 2, cy - h / 2), P(cx + w / 2, cy + h / 2), fill, h * 0.5f * s);
    dl->AddRect(P(cx - w / 2, cy - h / 2), P(cx + w / 2, cy + h / 2), ring, h * 0.5f * s, 0, 1.6f * s);
    Text(GetFonts().bold, r * 0.95f, cx, cy - r * 0.55f, IM_COL32(240, 240, 240, 255), label, 0.5f);
  };
  const PromptStyle style = LastPromptStyle();
  const bool dpad = button == (kPadUp | kPadDown) || button == (kPadLeft | kPadRight);
  if (style == PromptStyle::kKeyboard && !dpad) {
    if (const char* k = KeyLabel(button)) {
      // A keycap: a rounded square, the key's name on it.
      const float w = GlyphWidth(button, r), h = r * 1.8f;
      dl->AddRectFilled(P(cx - w / 2, cy - h / 2), P(cx + w / 2, cy + h / 2), fill, 5 * s);
      dl->AddRect(P(cx - w / 2, cy - h / 2), P(cx + w / 2, cy + h / 2), ring, 5 * s, 0, 1.6f * s);
      dl->AddLine(P(cx - w / 2 + 4, cy + h / 2 - 3), P(cx + w / 2 - 4, cy + h / 2 - 3), C(IM_COL32(110, 110, 116, 255)),
                  1.4f * s);
      Text(GetFonts().bold, r * 0.95f, cx, cy - r * 0.62f, IM_COL32(240, 240, 240, 255), k, 0.5f);
      return;
    }
  }
  if (style == PromptStyle::kXbox && !dpad) {
    if (const char* p = XboxPill(button)) {
      pill(p);
      return;
    }
    // The face buttons: a letter in its colour.
    const char* letter = nullptr;
    ImU32 col = 0;
    switch (button) {
      case kPadCross: letter = "A", col = IM_COL32(110, 200, 90, 255); break;
      case kPadCircle: letter = "B", col = IM_COL32(235, 85, 75, 255); break;
      case kPadSquare: letter = "X", col = IM_COL32(80, 150, 240, 255); break;
      case kPadTriangle: letter = "Y", col = IM_COL32(240, 200, 60, 255); break;
      default: break;
    }
    if (letter) {
      dl->AddCircleFilled(c, R, fill, 32);
      dl->AddCircle(c, R, C(col), 32, 2.0f * s);
      Text(GetFonts().bold, r * 1.15f, cx, cy - r * 0.68f, col, letter, 0.5f);
      return;
    }
  }
  switch (button) {
    case kPadL1: pill("L1"); return;
    case kPadR1: pill("R1"); return;
    case kPadL2: pill("L2"); return;
    case kPadR2: pill("R2"); return;
    case kPadOptions: pill("OPT"); return;
    case kPadTouchpad: pill("TP"); return;
    default: break;
  }
  if (button == (kPadUp | kPadDown) || button == (kPadLeft | kPadRight)) {
    // A d-pad cross with the two arms that matter lit.
    const bool vertical = button == (kPadUp | kPadDown);
    const float a = r * 0.36f, l = r * 0.95f;
    dl->AddRectFilled(P(cx - a, cy - l), P(cx + a, cy + l), C(vertical ? IM_COL32(235, 235, 235, 255)
                                                                         : IM_COL32(110, 110, 110, 255)), 2 * s);
    dl->AddRectFilled(P(cx - l, cy - a), P(cx + l, cy + a), C(vertical ? IM_COL32(110, 110, 110, 255)
                                                                         : IM_COL32(235, 235, 235, 255)), 2 * s);
    return;
  }
  dl->AddCircleFilled(c, R, fill, 32);
  dl->AddCircle(c, R, ring, 32, 1.6f * s);
  const float g = R * 0.48f, t = std::max(1.5f, 2.6f * s);
  switch (button) {
    case kPadCross: {
      const ImU32 col = C(IM_COL32(124, 178, 232, 255));
      dl->AddLine(ImVec2(c.x - g, c.y - g), ImVec2(c.x + g, c.y + g), col, t);
      dl->AddLine(ImVec2(c.x - g, c.y + g), ImVec2(c.x + g, c.y - g), col, t);
      break;
    }
    case kPadCircle:
      dl->AddCircle(c, g * 1.05f, C(IM_COL32(255, 102, 102, 255)), 24, t);
      break;
    case kPadSquare:
      dl->AddRect(ImVec2(c.x - g * 0.9f, c.y - g * 0.9f), ImVec2(c.x + g * 0.9f, c.y + g * 0.9f),
                  C(IM_COL32(255, 105, 248, 255)), 0, 0, t);
      break;
    case kPadTriangle: {
      const ImU32 col = C(IM_COL32(64, 226, 160, 255));
      dl->AddTriangle(ImVec2(c.x, c.y - g * 1.05f), ImVec2(c.x - g * 1.05f, c.y + g * 0.8f),
                      ImVec2(c.x + g * 1.05f, c.y + g * 0.8f), col, t);
      break;
    }
    default: break;
  }
}

float Canvas::Hint(float right, float y, uint32_t button, const char* label) const {
  const float size = 24.0f;
  const float w = TextWidth(GetFonts().body, size, label);
  Text(GetFonts().body, size, right - w, y - size * 0.62f, colour::kIvory, label);
  const float r = 15.0f;
  const float gw = GlyphWidth(button, r);
  Glyph(button, right - w - 12.0f - gw * 0.5f, y, r);
  Region(right - w - 12.0f - gw - 6.0f, y - 24.0f, w + gw + 18.0f, 48.0f, button);  // [IzunaRecomp] (clickable)
  return right - w - 12.0f - gw - 34.0f;
}

void Canvas::Poly(std::initializer_list<ImVec2> pts, ImU32 col) const {
  ImVec2 q[16];
  int n = 0;
  for (const ImVec2& p : pts) q[n++] = P(p.x, p.y);
  dl->AddConvexPolyFilled(q, n, C(col));
}

void Canvas::PolyLine(std::initializer_list<ImVec2> pts, ImU32 col, float thickness, bool closed) const {
  ImVec2 q[16];
  int n = 0;
  for (const ImVec2& p : pts) q[n++] = P(p.x, p.y);
  dl->AddPolyline(q, n, C(col), closed ? ImDrawFlags_Closed : 0, thickness * s);
}

void Canvas::Line(float x0, float y0, float x1, float y1, ImU32 col, float thickness) const {
  dl->AddLine(P(x0, y0), P(x1, y1), C(col), thickness * s);
}

float Canvas::Slanted(ImFont* f, float size, float x, float y, ImU32 col, const std::string& t, float align,
                      float skew) const {
  if (!f)
    f = ImGui::GetFont();
  const float w = TextWidth(f, size, t);
  const int first = dl->VtxBuffer.Size;
  dl->AddText(f, size * s, P(x - w * align, y), C(col), t.c_str());
  // Shear about the baseline: the tops lean right, the feet stay put.
  const float base = P(0, y + size * 0.82f).y;
  for (int i = first; i < dl->VtxBuffer.Size; ++i) dl->VtxBuffer[i].pos.x += (base - dl->VtxBuffer[i].pos.y) * skew;
  return w;
}

float Canvas::Fitted(ImFont* f, float size, float x, float y, float max_width, ImU32 col, const std::string& t,
                     float align) const {
  const float w = TextWidth(f, size, t);
  const float fit = w > max_width && w > 0 ? size * max_width / w : size;
  return Text(f, fit, x, y + (size - fit) * 0.5f, col, t, align);
}

void DrawIcon(const Canvas& c, Icon icon, float cx, float cy, float r, ImU32 col) {
  const float t = std::max(2.0f, r * 0.16f);
  switch (icon) {
    case Icon::kPlay:
      c.Poly({ImVec2(cx - r * 0.55f, cy - r * 0.8f), ImVec2(cx + r * 0.85f, cy), ImVec2(cx - r * 0.55f, cy + r * 0.8f)},
             col);
      break;
    case Icon::kGear: {
      // Eight teeth around a ring, a hole in the middle.
      for (int k = 0; k < 8; ++k) {
        const float a = float(k) * 0.785398f, ca = std::cos(a), sa = std::sin(a);
        const float w = r * 0.2f, r0 = r * 0.55f, r1 = r * 0.95f;
        c.Poly({ImVec2(cx + ca * r0 - sa * w, cy + sa * r0 + ca * w), ImVec2(cx + ca * r1 - sa * w, cy + sa * r1 + ca * w),
                ImVec2(cx + ca * r1 + sa * w, cy + sa * r1 - ca * w), ImVec2(cx + ca * r0 + sa * w, cy + sa * r0 - ca * w)},
               col);
      }
      c.dl->AddCircle(c.P(cx, cy), r * 0.52f * c.s, c.C(col), 24, r * 0.3f * c.s);
      break;
    }
    case Icon::kShuriken: {
      // Four blades round a hole: a throwing star.
      for (int k = 0; k < 4; ++k) {
        const float a = float(k) * 1.570796f + 0.3f, ca = std::cos(a), sa = std::sin(a);
        const float cb = std::cos(a + 1.1f), sb = std::sin(a + 1.1f);
        c.Poly({ImVec2(cx + ca * r * 0.25f, cy + sa * r * 0.25f), ImVec2(cx + ca * r, cy + sa * r),
                ImVec2(cx + cb * r * 0.42f, cy + sb * r * 0.42f)},
               col);
      }
      c.dl->AddCircle(c.P(cx, cy), r * 0.3f * c.s, c.C(col), 16, t * c.s);
      break;
    }
    case Icon::kBox: {
      // A package seen from above a corner: three faces of a cube.
      const float h = r * 0.5f;
      c.PolyLine({ImVec2(cx, cy - r), ImVec2(cx + r * 0.9f, cy - h), ImVec2(cx + r * 0.9f, cy + h),
                  ImVec2(cx, cy + r), ImVec2(cx - r * 0.9f, cy + h), ImVec2(cx - r * 0.9f, cy - h)},
                 col, t);
      c.Line(cx - r * 0.9f, cy - h, cx, cy, col, t);
      c.Line(cx + r * 0.9f, cy - h, cx, cy, col, t);
      c.Line(cx, cy, cx, cy + r, col, t);
      break;
    }
    case Icon::kSave:
      // A floppy disk: the body with a cut corner, the shutter and the label.
      c.PolyLine({ImVec2(cx - r * 0.85f, cy - r * 0.85f), ImVec2(cx + r * 0.55f, cy - r * 0.85f),
                  ImVec2(cx + r * 0.85f, cy - r * 0.55f), ImVec2(cx + r * 0.85f, cy + r * 0.85f),
                  ImVec2(cx - r * 0.85f, cy + r * 0.85f)},
                 col, t);
      c.Rect(cx - r * 0.45f, cy - r * 0.85f, r * 0.8f, r * 0.5f, col);
      c.Frame(cx - r * 0.55f, cy + r * 0.1f, r * 1.1f, r * 0.62f, col, t * 0.8f);
      break;
    case Icon::kInfo:
      c.dl->AddCircle(c.P(cx, cy), r * 0.9f * c.s, c.C(col), 32, t * c.s);
      c.dl->AddCircleFilled(c.P(cx, cy - r * 0.42f), r * 0.13f * c.s, c.C(col), 12);
      c.Rect(cx - r * 0.1f, cy - r * 0.16f, r * 0.2f, r * 0.62f, col);
      break;
    case Icon::kPower: {
      // A ring open at the top, a bar through the gap.
      const int n = 24;
      ImVec2 arc[n + 1];
      for (int k = 0; k <= n; ++k) {
        const float a = 2.17f + float(k) / float(n) * 5.08f;  // (from just left of the top round to just right)
        arc[k] = c.P(cx + std::cos(a) * r * 0.78f, cy - std::sin(a) * r * 0.78f);
      }
      c.dl->AddPolyline(arc, n + 1, c.C(col), 0, t * c.s);
      c.Rect(cx - t * 0.5f, cy - r * 0.95f, t, r * 0.8f, col);
      break;
    }
  }
}

// --- The settings page's plates ----------------------------------------------

void SettingsList::DrawPlates(const Canvas& c, const Ng2Settings& s, float x, float y, float w, float h, float dx,
                              float dw, float dt) {
  const Fonts& f = GetFonts();
  const float rh = 66.0f, gap = 10.0f, step = rh + gap;
  const int n = int(rows_.size());
  const float want = std::clamp(float(sel_) * step - h * 0.5f + step * 0.5f, 0.0f,
                                std::max(0.0f, float(n) * step - h));
  Ease(scroll_, want, dt, 12.0f);
  Ease(nudge_, 0.0f, dt, 10.0f);
  c.dl->PushClipRect(c.P(x - 20, y), c.P(x + w + 40, y + h), true);
  for (int i = 0; i < n; ++i) {
    const float ry = y + float(i) * step - scroll_;
    if (ry + rh < y || ry > y + h)
      continue;
    const Setting& row = *rows_[size_t(i)];
    const bool on = i == sel_;
    const std::string why = row.unavailable ? row.unavailable(s) : std::string();
    const bool greyed = !why.empty();
    // [IzunaRecomp] The mouse: over a plate focuses it, a click steps its value.
    c.Region(x, std::max(ry, y), w, std::min(ry + rh, y + h) - std::max(ry, y),
             on ? (row.key >= 0 ? kPadCross : kPadRight) : 0, [this, i] { if (!capturing_) sel_ = i; }, [this, i] { if (!capturing_) sel_ = i; });
    // A plate with clipped corners, chrome-edged; the focused one red and
    // pushed out a little, as the pause menu's chosen item.
    const float px = x + (on ? 16.0f : 0.0f), cut = 14.0f;
    const ImVec2 pts[6] = {ImVec2(px + cut, ry), ImVec2(px + w - cut, ry), ImVec2(px + w, ry + rh * 0.5f),
                           ImVec2(px + w - cut, ry + rh), ImVec2(px + cut, ry + rh), ImVec2(px, ry + rh * 0.5f)};
    c.Poly({pts[0], pts[1], pts[2], pts[3], pts[4], pts[5]}, on ? IM_COL32(150, 18, 16, 250) : IM_COL32(16, 16, 18, 248));
    if (on) {  // a lighter top half: a polished face
      c.Poly({pts[0], pts[1], pts[2], pts[5]}, IM_COL32(214, 32, 28, 90));
    }
    c.PolyLine({pts[0], pts[1], pts[2], pts[3], pts[4], pts[5]},
               on ? colour::kIvory : IM_COL32(150, 150, 158, 200), on ? 2.5f : 1.5f);
    const ImU32 label_col = greyed ? colour::kMuted : (on ? colour::kIvory : colour::kIvoryDim);
    const std::string value = ValueText(row, s, on && capturing_);
    const float vw = std::min(c.TextWidth(f.body, 24.0f, value), w * 0.4f);
    c.Fitted(f.heading, 30.0f, px + 34.0f, ry + 14.0f, w - vw - 100.0f, label_col, row.label);
    c.Fitted(f.body, 24.0f, px + w - 34.0f, ry + 20.0f, w * 0.4f, greyed ? colour::kMuted : colour::kIvory, value,
             1.0f);
  }
  c.dl->PopClipRect();

  // The detail panel: the focused setting's name, its value with arrows,
  // every choice (the current one lit), when it applies, and its help.
  const Setting* row = Current();
  if (!row)
    return;
  const std::string why = row->unavailable ? row->unavailable(s) : std::string();
  c.Rect(dx, y, dw, h, IM_COL32(8, 6, 6, 170), 6);
  c.Frame(dx, y, dw, h, IM_COL32(150, 150, 158, 160), 1.5f, 6);
  float yy = y + 26;
  yy += c.Wrapped(f.menu, 44, dx + 30, yy, dw - 60, colour::kIvory, row->label, 1.15f) + 10;
  c.Line(dx + 30, yy, dx + dw - 30, yy, colour::kRedDeep, 3);
  yy += 22;
  const int count = int(row->choices.size());
  const int cur = std::clamp(row->get(s), 0, std::max(0, count - 1));
  if (row->key >= 0) {  // [IzunaRecomp] a key binding: its key, or the wait for one
    const std::string value = ValueText(*row, s, capturing_);
    c.Fitted(f.bold, 40, dx + dw * 0.5f + nudge_ * 10.0f, yy, dw - 160,
             capturing_ ? colour::kRed : (why.empty() ? colour::kIvory : colour::kMuted), value, 0.5f);
    yy += 66;
    c.Text(f.body, 22, dx + 30, yy, colour::kIvoryDim,
           std::string("Default: ") + izuna::keys::kActions[row->key].def);
    yy += 44;
  } else if (count) {
    const std::string value = row->choices[size_t(cur)];
    const float vx = dx + dw * 0.5f + nudge_ * 10.0f;
    c.Fitted(f.bold, 40, vx, yy, dw - 160, why.empty() ? colour::kIvory : colour::kMuted, value, 0.5f);
    if (why.empty() && count > 1) {
      const float vw = std::min(c.TextWidth(f.bold, 40, value), dw - 160);
      c.Text(f.bold, 40, vx - vw * 0.5f - 34, yy, colour::kRed, "<", 1.0f);
      c.Text(f.bold, 40, vx + vw * 0.5f + 34, yy, colour::kRed, ">");
    }
    yy += 66;
    if (count > 1 && count <= 8) {  // the choices as chips, wrapping
      float cx = dx + 30;
      for (int i = 0; i < count; ++i) {
        const std::string& ch = row->choices[size_t(i)];
        const float cw = std::min(c.TextWidth(f.body, 22, ch), dw - 100) + 28;
        if (cx + cw > dx + dw - 30) {
          cx = dx + 30;
          yy += 46;
        }
        const bool lit = i == cur;
        c.Rect(cx, yy, cw, 38, lit ? IM_COL32(150, 18, 16, 230) : IM_COL32(30, 30, 34, 220), 4);
        c.Frame(cx, yy, cw, 38, lit ? colour::kIvory : IM_COL32(110, 110, 116, 200), 1.5f, 4);
        c.Fitted(f.body, 22, cx + 14, yy + 6, cw - 28, lit ? colour::kIvory : colour::kIvoryDim, ch);
        cx += cw + 10;
      }
      yy += 60;
    }
  }
  if (row->restart) {
    c.Text(f.bold, 22, dx + 30, yy, colour::kWarn, "Applies the next time the game starts");
    yy += 40;
  }
  c.Wrapped(f.body, 26, dx + 30, yy, dw - 60, why.empty() ? colour::kIvoryDim : colour::kWarn,
            why.empty() ? std::string(row->help) : why);
}

float Ease(float& v, float target, float dt, float speed) {
  v += (target - v) * (1.0f - std::exp(-speed * dt));
  if (std::fabs(target - v) < 0.001f)
    v = target;
  return v;
}

// --- SettingsList ------------------------------------------------------------

// [IzunaRecomp] The name the keyboard driver knows (rex/ui/keybinds.cpp) for
// a key the player pressed; empty for one that cannot be bound: Esc and the
// F-keys (the app's own), and the mouse buttons (never: no attacks on clicks).
static std::string BindableKeyName(ImGuiKey k) {
  if (k >= ImGuiKey_A && k <= ImGuiKey_Z) return std::string(1, char('A' + (k - ImGuiKey_A)));
  if (k >= ImGuiKey_0 && k <= ImGuiKey_9) return std::string(1, char('0' + (k - ImGuiKey_0)));
  if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9) return "Numpad" + std::to_string(k - ImGuiKey_Keypad0);
  switch (k) {
    case ImGuiKey_Space: return "Space";
    case ImGuiKey_Enter: return "Return";
    case ImGuiKey_KeypadEnter: return "NumpadEnter";
    case ImGuiKey_Tab: return "Tab";
    case ImGuiKey_Backspace: return "Backspace";
    case ImGuiKey_Delete: return "Delete";
    case ImGuiKey_Insert: return "Insert";
    case ImGuiKey_Home: return "Home";
    case ImGuiKey_End: return "End";
    case ImGuiKey_PageUp: return "PageUp";
    case ImGuiKey_PageDown: return "PageDown";
    case ImGuiKey_LeftArrow: return "Left";
    case ImGuiKey_RightArrow: return "Right";
    case ImGuiKey_UpArrow: return "Up";
    case ImGuiKey_DownArrow: return "Down";
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift: return "Shift";
    case ImGuiKey_LeftCtrl:
    case ImGuiKey_RightCtrl: return "Control";
    case ImGuiKey_LeftAlt:
    case ImGuiKey_RightAlt: return "Alt";
    case ImGuiKey_Apostrophe: return "Quote";
    case ImGuiKey_Comma: return "Comma";
    case ImGuiKey_Minus: return "Minus";
    case ImGuiKey_Period: return "Period";
    case ImGuiKey_Slash: return "Slash";
    case ImGuiKey_Semicolon: return "Semicolon";
    case ImGuiKey_Equal: return "Plus";
    case ImGuiKey_LeftBracket: return "LBracket";
    case ImGuiKey_Backslash: return "Backslash";
    case ImGuiKey_RightBracket: return "RBracket";
    case ImGuiKey_GraveAccent: return "Backtick";
    case ImGuiKey_CapsLock: return "CapsLock";
    case ImGuiKey_KeypadAdd: return "NumpadPlus";
    case ImGuiKey_KeypadSubtract: return "NumpadMinus";
    case ImGuiKey_KeypadMultiply: return "NumpadStar";
    case ImGuiKey_KeypadDivide: return "NumpadSlash";
    default: return {};
  }
}

// Binds `key` to action `i`; an action that had it takes i's old key.
static void BindKey(Ng2Settings& s, int i, const std::string& key) {
  const std::string old = s.pc_keys[size_t(i)];
  for (int j = 0; j < izuna::keys::kCount; ++j)
    if (j != i && s.pc_keys[size_t(j)] == key) s.pc_keys[size_t(j)] = old;
  s.pc_keys[size_t(i)] = key;
}

std::string SettingsList::ValueText(const Setting& row, const Ng2Settings& s, bool capturing) {
  if (row.key >= 0) return capturing ? std::string("Press a key...") : izuna::keys::Shown(s.pc_keys[size_t(row.key)]);
  return row.choices.empty() ? std::string()
                             : row.choices[size_t(std::clamp(row.get(s), 0, int(row.choices.size()) - 1))];
}

bool SettingsList::UpdateCapture(const PadState& pad, Ng2Settings& s) {
  const Setting& row = *rows_[size_t(sel_)];
  // (two frames' grace: the press that started the wait is not its key)
  if (++capture_age_ < 2.5f) return false;
  for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
    if (!ImGui::IsKeyPressed(ImGuiKey(k), false)) continue;
    const std::string name = BindableKeyName(ImGuiKey(k));
    if (name.empty()) continue;
    capturing_ = false;
    if (row.key < 0) return false;
    BindKey(s, row.key, name);
    PlaySound(Sound::kChange);
    return true;
  }
  if (pad.pressed & kPadCircle) {  // (Esc, or the controller's Circle)
    capturing_ = false;
    PlaySound(Sound::kBack);
  }
  return false;
}

bool SettingsList::Update(const PadState& pad, Ng2Settings& s, bool in_game) {
  if (rows_.empty())
    return false;
  if (capturing_)
    return UpdateCapture(pad, s);
  const int n = int(rows_.size());
  if (pad.pressed & kPadUp) {
    sel_ = (sel_ + n - 1) % n;
    help_ = false;
    PlaySound(Sound::kMove);
  }
  if (pad.pressed & kPadDown) {
    sel_ = (sel_ + 1) % n;
    help_ = false;
    PlaySound(Sound::kMove);
  }
  if (pad.pressed & kPadTriangle) {
    help_ = !help_;
    PlaySound(Sound::kSelect);
  }
  const Setting& row = *rows_[size_t(sel_)];
  const bool locked = (row.unavailable && !row.unavailable(s).empty()) || (in_game && row.restart);
  if (row.key >= 0) {  // [IzunaRecomp] a key binding: Cross waits for a key, Square the default
    if (!(pad.pressed & (kPadCross | kPadSquare)))
      return false;
    if (locked) {
      PlaySound(Sound::kError);
      nudge_ = 1;
      return false;
    }
    if (pad.pressed & kPadSquare) {
      const std::string def = izuna::keys::kActions[row.key].def;
      if (s.pc_keys[size_t(row.key)] == def)
        return false;
      BindKey(s, row.key, def);
      PlaySound(Sound::kChange);
      return true;
    }
    capturing_ = true;
    capture_age_ = 0;
    PlaySound(Sound::kSelect);
    return false;
  }
  int dir = 0;
  // Left/Right change a value; Cross does not, so "selecting" a row can never
  // flip a setting by accident.
  if (pad.pressed & kPadRight) dir = 1;
  if (pad.pressed & kPadLeft) dir = -1;
  if (dir == 0 && !(pad.pressed & kPadSquare))
    return false;
  if (locked) {
    PlaySound(Sound::kError);
    nudge_ = float(dir ? dir : 1);
    return false;
  }
  const int count = int(row.choices.size());
  const int current = std::clamp(row.get(s), 0, count - 1);
  const int next = (pad.pressed & kPadSquare) ? row.def : (current + dir + count) % count;
  if (next == current)
    return false;
  row.set(s, next);
  s.Clamp();
  PlaySound(Sound::kChange);
  return true;
}

void SettingsList::Draw(const Canvas& c, const Ng2Settings& s, float x, float y, float w, float h, bool in_game,
                        float dt) {
  const Fonts& f = GetFonts();
  const float rh = 64.0f;
  const float help_h = 92.0f;
  const float list_h = h - help_h;
  const int n = int(rows_.size());
  // Keep the focused row in view, eased so the list glides rather than jumps.
  const float want = std::clamp(float(sel_) * rh - list_h * 0.5f + rh * 0.5f, 0.0f,
                                std::max(0.0f, float(n) * rh - list_h));
  Ease(scroll_, want, dt, 12.0f);
  if (focus_y_ < 0)
    focus_y_ = float(sel_) * rh;
  Ease(focus_y_, float(sel_) * rh, dt, 18.0f);
  Ease(nudge_, 0.0f, dt, 10.0f);

  c.dl->PushClipRect(c.P(x, y), c.P(x + w, y + list_h), true);
  const float fy = y + focus_y_ - scroll_;
  c.Rect(x, fy, w, rh, IM_COL32(150, 18, 16, 120));
  c.Rect(x, fy, 6.0f, rh, colour::kRed);
  for (int i = 0; i < n; ++i) {
    const float ry = y + float(i) * rh - scroll_;
    if (ry + rh < y || ry > y + list_h)
      continue;
    const Setting& row = *rows_[size_t(i)];
    const std::string why = row.unavailable ? row.unavailable(s) : std::string();
    const bool greyed = !why.empty() || (in_game && row.restart);
    c.Region(x, std::max(ry, y), w, std::min(ry + rh, y + list_h) - std::max(ry, y),
             i == sel_ ? (row.key >= 0 ? kPadCross : kPadRight) : 0, [this, i] { if (!capturing_) sel_ = i; },
             [this, i] { if (!capturing_) sel_ = i; });  // [IzunaRecomp] (the mouse)
    const ImU32 label_col = greyed ? colour::kMuted : (i == sel_ ? colour::kIvory : colour::kIvoryDim);
    c.Text(f.bold, 30.0f, x + 28.0f, ry + 15.0f, label_col, row.label);
    std::string value = ValueText(row, s, i == sel_ && capturing_);
    const float vx = x + w - 40.0f + (i == sel_ ? nudge_ * 8.0f : 0.0f);
    c.Text(f.body, 30.0f, vx, ry + 15.0f, greyed ? colour::kMuted : colour::kIvory, value, 1.0f);
    if (i == sel_ && !greyed && row.choices.size() > 1) {
      const float vw = c.TextWidth(f.body, 30.0f, value);
      c.Text(f.body, 30.0f, vx - vw - 18.0f, ry + 15.0f, colour::kRed, "<", 1.0f);
      c.Text(f.body, 30.0f, vx + 8.0f, ry + 15.0f, colour::kRed, ">");
    }
    std::string tag;
    if (in_game && row.restart)
      tag = "restart";
    else if (!in_game && row.restart)
      tag = "next start";
    if (!tag.empty())
      c.Text(f.body, 20.0f, x + 28.0f + c.TextWidth(f.bold, 30.0f, row.label) + 16.0f, ry + 22.0f,
             colour::kMuted, tag);
  }
  c.dl->PopClipRect();

  // The focused row's description (Triangle: the longer help).
  if (const Setting* row = Current()) {
    const std::string why = row->unavailable ? row->unavailable(s) : std::string();
    std::string text = !why.empty() ? why
                       : (in_game && row->restart)
                           ? std::string("Changed from the hub before START GAME; this one cannot change mid-game.")
                           : std::string(row->help);
    c.Rect(x, y + list_h + 8.0f, w, help_h - 8.0f, IM_COL32(0, 0, 0, 120), 6.0f);
    c.Wrapped(f.body, 24.0f, x + 24.0f, y + list_h + 22.0f, w - 48.0f, why.empty() ? colour::kIvoryDim : colour::kWarn,
              text);
  }
}

}  // namespace ng2ps5::hub
