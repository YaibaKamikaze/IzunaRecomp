// [dev tool] Renders the PS5 hub's pages to PNG files on the build machine,
// with the hub's own code and fonts: ImGui draws into its lists as on the
// console, and a small CPU rasteriser turns them into pixels - so a change to
// the hub's look can be checked without a console. Not part of the app.
//
//   tools/hub_preview/build.sh && out/hub_preview/hub_preview <out dir>   (PPM files)

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>


#include "ps5/hub/hub_pages.h"

namespace hub = ng2ps5::hub;

// --- The rasteriser ------------------------------------------------------------

struct Target {
  int w, h;
  std::vector<float> rgb;  // linear blending in 0..1 (as the display's sRGB bytes)
};

static void Sample(ImTextureData* tex, float u, float v, float out[4]) {
  if (!tex || !tex->Pixels) {
    out[0] = out[1] = out[2] = out[3] = 1.0f;
    return;
  }
  const int x = std::clamp(int(u * tex->Width), 0, tex->Width - 1);
  const int y = std::clamp(int(v * tex->Height), 0, tex->Height - 1);
  const unsigned char* p = (const unsigned char*)tex->Pixels + (size_t(y) * tex->Width + x) * tex->BytesPerPixel;
  if (tex->Format == ImTextureFormat_Alpha8) {
    out[0] = out[1] = out[2] = 1.0f;
    out[3] = p[0] / 255.0f;
  } else {
    for (int i = 0; i < 4; ++i) out[i] = p[i] / 255.0f;
  }
}

static void Triangle(Target& t, const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c, const ImVec4& clip,
                     ImTextureData* tex) {
  const float minx = std::max({clip.x, std::min({a.pos.x, b.pos.x, c.pos.x}), 0.0f});
  const float maxx = std::min({clip.z, std::max({a.pos.x, b.pos.x, c.pos.x}), float(t.w)});
  const float miny = std::max({clip.y, std::min({a.pos.y, b.pos.y, c.pos.y}), 0.0f});
  const float maxy = std::min({clip.w, std::max({a.pos.y, b.pos.y, c.pos.y}), float(t.h)});
  const float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
  if (std::fabs(area) < 1e-6f) return;
  auto col = [](ImU32 v, int i) { return float((v >> (i * 8)) & 0xFF) / 255.0f; };
  for (int y = int(miny); y < int(std::ceil(maxy)); ++y) {
    for (int x = int(minx); x < int(std::ceil(maxx)); ++x) {
      const float px = x + 0.5f, py = y + 0.5f;
      float w0 = ((b.pos.x - px) * (c.pos.y - py) - (b.pos.y - py) * (c.pos.x - px)) / area;
      float w1 = ((c.pos.x - px) * (a.pos.y - py) - (c.pos.y - py) * (a.pos.x - px)) / area;
      float w2 = 1.0f - w0 - w1;
      if (w0 < 0 || w1 < 0 || w2 < 0) continue;
      float s[4];
      Sample(tex, a.uv.x * w0 + b.uv.x * w1 + c.uv.x * w2, a.uv.y * w0 + b.uv.y * w1 + c.uv.y * w2, s);
      float rgba[4];
      for (int i = 0; i < 4; ++i) rgba[i] = (col(a.col, i) * w0 + col(b.col, i) * w1 + col(c.col, i) * w2) * s[i];
      float* d = &t.rgb[(size_t(y) * t.w + x) * 3];
      for (int i = 0; i < 3; ++i) d[i] = d[i] * (1.0f - rgba[3]) + rgba[i] * rgba[3];
    }
  }
}

static void Render(Target& t, ImDrawData* dd) {
  for (int n = 0; n < dd->CmdListsCount; ++n) {
    const ImDrawList* l = dd->CmdLists[n];
    for (const ImDrawCmd& cmd : l->CmdBuffer) {
      if (cmd.UserCallback) continue;
      ImTextureData* tex = cmd.TexRef._TexData;
      for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
        const ImDrawIdx* idx = l->IdxBuffer.Data + cmd.IdxOffset + i;
        Triangle(t, l->VtxBuffer[cmd.VtxOffset + idx[0]], l->VtxBuffer[cmd.VtxOffset + idx[1]],
                 l->VtxBuffer[cmd.VtxOffset + idx[2]], cmd.ClipRect, tex);
      }
    }
  }
}

// --- The pages ------------------------------------------------------------------

int main(int argc, char** argv) {
  const std::string out = argc > 1 ? argv[1] : ".";
  std::filesystem::create_directories(out);
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1920, 1080);
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  hub::ConfigureFonts(io.Fonts);

  Ng2Settings settings;
  hub::HubContext ctx;
  ctx.settings = &settings;
  ctx.game.folder = ctx.game.xex = true;
  ctx.game.title_id = 0x544307D5;
  ctx.game.archives = 20;
  ctx.game.message = "Game Data: Ninja Gaiden II found";
  ctx.version = "IzunaRecomp dev";
  hub::Hub h(ctx);

  // A script of presses: (frame, buttons, picture name to save at that frame).
  struct Step { int frame; uint32_t press; const char* shot; };
  const Step steps[] = {
      {40, 0, "home"},
      {41, ng2ps5::kPadDown, nullptr},
      {80, 0, "home_settings"},
      {81, ng2ps5::kPadCross, nullptr},
      {130, 0, "settings_display"},
      {131, ng2ps5::kPadDown, nullptr},
      {132, ng2ps5::kPadDown, nullptr},
      {170, 0, "settings_display_row3"},
      {171, ng2ps5::kPadR1, nullptr},
      {210, 0, "settings_graphics"},
      {211, ng2ps5::kPadR1, nullptr}, {212, ng2ps5::kPadR1, nullptr}, {213, ng2ps5::kPadR1, nullptr},
      {214, ng2ps5::kPadR1, nullptr},
      {250, 0, "settings_developer"},
      {251, ng2ps5::kPadCircle, nullptr},
      {252, ng2ps5::kPadDown, nullptr},
      {290, 0, "home_dlc"},
      {291, ng2ps5::kPadCross, nullptr},
      {330, 0, "dlc"},
      {331, ng2ps5::kPadCircle, nullptr},
      {332, ng2ps5::kPadDown, nullptr}, {333, ng2ps5::kPadDown, nullptr},
      {334, ng2ps5::kPadCross, nullptr},
      {380, 0, "about"},
  };
  int last = 0;
  for (const Step& s : steps) last = std::max(last, s.frame);
  for (int frame = 0; frame <= last; ++frame) {
    ng2ps5::PadState pad;
    const char* shot = nullptr;
    for (const Step& s : steps) {
      if (s.frame != frame) continue;
      pad.pressed |= s.press;
      if (s.shot) shot = s.shot;
    }
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    h.Draw(pad, io.DeltaTime, ImGui::GetForegroundDrawList(), io.DisplaySize);
    ImGui::Render();
    // (the font atlas's pages, as a renderer would upload them)
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
      if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
        tex->SetTexID((ImTextureID)(intptr_t)tex->UniqueID);
        tex->SetStatus(ImTextureStatus_OK);
      } else if (tex->Status == ImTextureStatus_WantDestroy) {
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
      }
    }
    if (shot) {
      Target t{1920, 1080, std::vector<float>(1920 * 1080 * 3, 0.0f)};
      Render(t, ImGui::GetDrawData());
      std::vector<unsigned char> px(1920 * 1080 * 3);
      for (size_t i = 0; i < px.size(); ++i) px[i] = (unsigned char)std::clamp(int(t.rgb[i] * 255.0f + 0.5f), 0, 255);
      const std::string path = out + "/" + shot + ".ppm";  // (tools/hub_preview/build.sh makes PNGs of them)
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P6\n1920 1080\n255\n");
        std::fwrite(px.data(), 1, px.size(), f);
        std::fclose(f);
      }
      std::printf("%s\n", path.c_str());
    }
  }
  return 0;
}

// --- What the hub calls that the console has and this tool does not -----------

namespace ng2ps5::hub {
void PlaySound(Sound) {}
void InitSounds(const std::string&) {}
}  // namespace ng2ps5::hub

#include "ng2_disc.h"
namespace ng2 {
DiscInfo InspectFolder(const std::filesystem::path&) { return DiscInfo{}; }
bool IsStfsPackage(const std::filesystem::path&) { return false; }
}  // namespace ng2
