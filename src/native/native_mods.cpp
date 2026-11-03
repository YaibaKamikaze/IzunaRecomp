// [IzunaRecomp] What mods change in the drawing (native_draw.h, "Mods";
// docs/MODDING.md): shaders left out, textures replaced, textures saved.
//
// A texture's id is its size, its Xenos format and the hash of its bytes as
// the renderer reads them (TextureId) - not its address, which the game's
// streaming reuses. A mod's picture for an id (textures/<id>.png) is decoded
// on a thread of its own the first time the texture is drawn; until it is
// ready the game's own is shown (native_textures.cpp, Texture2D).

#include "native/native_draw_internal.h"

#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_set>

#include <rex/ui/image_decode.h>

namespace ng2native::draw {
namespace {

// --- Hidden shaders ----------------------------------------------------------------

std::mutex g_hidden_mutex;
std::unordered_set<uint64_t> g_hidden;
std::atomic<uint32_t> g_hidden_count{0};

// --- Replacements and the dump --------------------------------------------------------

struct Replacement {
  std::filesystem::path path;
  int state = 0;  // 0 not read, 1 being read, 2 ready, 3 unusable
  std::shared_ptr<const TextureReplacement> pixels;
};
std::mutex g_mutex;
std::condition_variable g_cv;
std::unordered_map<std::string, Replacement> g_replacements;  // id -> picture
std::atomic<bool> g_any_replacement{false};
std::deque<std::string> g_decode;  // ids to read
struct Dump {
  std::filesystem::path file;
  std::vector<uint8_t> bytes;
};
std::deque<Dump> g_dumps;
std::atomic<bool> g_dump_on{false};
std::filesystem::path g_dump_dir;
std::unordered_set<std::string> g_dumped;
bool g_worker = false;
constexpr uint32_t kMaxSide = 2048;  // (a 2048x2048 picture and its mips fit a frame's upload ring)

void Worker() {
  for (;;) {
    std::string id;
    Dump dump;
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_cv.wait(lock, [] { return !g_decode.empty() || !g_dumps.empty(); });
      if (!g_decode.empty()) {
        id = std::move(g_decode.front());
        g_decode.pop_front();
      } else {
        dump = std::move(g_dumps.front());
        g_dumps.pop_front();
      }
    }
    if (!dump.file.empty()) {
      std::ofstream out(dump.file, std::ios::binary);
      out.write(reinterpret_cast<const char*>(dump.bytes.data()), std::streamsize(dump.bytes.size()));
      continue;
    }
    std::filesystem::path path;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      path = g_replacements[id].path;
    }
    auto pixels = std::make_shared<TextureReplacement>();
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int w = 0, h = 0;
    pixels->rgba = rex::ui::DecodeImageRGBA(file.data(), file.size(), w, h);
    pixels->w = uint32_t(std::max(w, 0));
    pixels->h = uint32_t(std::max(h, 0));
    const bool ok = !pixels->rgba.empty() && w > 0 && h > 0 && pixels->w <= kMaxSide && pixels->h <= kMaxSide;
    if (!ok) {
      REXLOG_WARN("[mods] texture {}: {} {}", id, path.string(),
                  pixels->rgba.empty() ? "could not be read (PNG, JPEG, TGA or BMP)"
                                       : fmt::format("is {}x{}: at most {}x{}", w, h, kMaxSide, kMaxSide));
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    Replacement& r = g_replacements[id];
    r.state = ok ? 2 : 3;
    if (ok) r.pixels = std::move(pixels);
  }
}

void StartWorker() {  // (under g_mutex)
  if (g_worker) return;
  g_worker = true;
  std::thread(Worker).detach();
}

}  // namespace

void HideShaderNow(uint64_t hash, bool hidden) {
  std::lock_guard<std::mutex> lock(g_hidden_mutex);
  if (hidden) {
    g_hidden.insert(hash);
  } else {
    g_hidden.erase(hash);
  }
  g_hidden_count.store(uint32_t(g_hidden.size()), std::memory_order_release);
}

bool ShaderHidden(uint64_t vs, uint64_t ps) {
  if (g_hidden_count.load(std::memory_order_acquire) == 0) return false;
  std::lock_guard<std::mutex> lock(g_hidden_mutex);
  return g_hidden.count(ps) || g_hidden.count(vs);
}

void SetReplacementsNow(std::unordered_map<std::string, std::filesystem::path> by_id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_replacements.clear();
  for (auto& [id, path] : by_id) g_replacements[id].path = std::move(path);
  g_any_replacement.store(!g_replacements.empty(), std::memory_order_release);
}

void SetDumpNow(bool on, const std::filesystem::path& dir) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_dump_dir = dir;
  if (on) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
  }
  g_dump_on.store(on, std::memory_order_release);
}

std::string TextureId(uint32_t w, uint32_t h, uint32_t format, uint64_t hash) {
  return fmt::format("{}x{}-{}-{:016x}", w, h, format, hash);
}

bool ReplacementsExist() { return g_any_replacement.load(std::memory_order_acquire); }

int TakeReplacement(const std::string& id, std::shared_ptr<const TextureReplacement>* out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_replacements.find(id);
  if (it == g_replacements.end()) return 0;
  Replacement& r = it->second;
  if (r.state == 0) {
    r.state = 1;
    g_decode.push_back(id);
    StartWorker();
    g_cv.notify_one();
  }
  if (r.state == 2) *out = r.pixels;
  return r.state == 3 ? 0 : r.state;
}

void ReplacementUploaded(const std::string& id) {
  // (its pixels are on the GPU now: read again only if the texture is evicted)
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_replacements.find(id);
  if (it == g_replacements.end() || it->second.state != 2) return;
  it->second.pixels.reset();
  it->second.state = 0;
}

bool TextureDumpOn() { return g_dump_on.load(std::memory_order_acquire); }

void DumpTexture(const std::string& id, VkFormat vk, uint32_t w, uint32_t h, const uint8_t* data, size_t bytes) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_dumps.size() > 256 || !g_dumped.insert(id).second) return;  // (each once; never more than 256 waiting)
  Dump d;
  d.file = g_dump_dir / fmt::format("{}_vk{}_{}x{}.bin", id, int(vk), w, h);
  d.bytes.assign(data, data + bytes);
  g_dumps.push_back(std::move(d));
  StartWorker();
  g_cv.notify_one();
}

}  // namespace ng2native::draw

namespace ng2native {
void HideShader(uint64_t hash, bool hidden) { draw::HideShaderNow(hash, hidden); }
void SetTextureReplacements(std::unordered_map<std::string, std::filesystem::path> by_id) {
  draw::SetReplacementsNow(std::move(by_id));
}
void SetTextureDump(bool on, const std::filesystem::path& dir) { draw::SetDumpNow(on, dir); }
}  // namespace ng2native
