// [IzunaRecomp] Textures for the native renderer (native_draw_internal.h):
// decoded from guest memory (untiled, endian-swapped, converted), cached by
// fetch constant and re-decoded when their bytes change; samplers.

#include "native/native_draw_internal.h"

namespace ng2native::draw {

// --- Textures ---------------------------------------------------------------

// Xenos tiled address of block (x, y) in a 2D surface, in bytes - as the SDK's
// GetTiledOffset2D (src/graphics/pipeline/texture/util.cpp, from UModel's
// UnTexture.cpp), copied: the GPU code lives in the runtime-loaded plugin.
int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bpb) {
  pitch = (pitch + 31) & ~31u;
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bpb + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bpb;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// Copy `bytes` (a multiple of the swap unit, from a unit-aligned address)
// applying the guest endian swap (x_or: 1 8-in-16, 2 16-in-32, 3 8-in-32).
void CopySwapped(uint8_t* d, const uint8_t* s, uint32_t bytes, uint32_t x_or) {
  switch (x_or) {
    case 0: std::memcpy(d, s, bytes); return;
    case 3:
      for (uint32_t i = 0; i < bytes; i += 4) {
        uint32_t v;
        std::memcpy(&v, s + i, 4);
        v = __builtin_bswap32(v);
        std::memcpy(d + i, &v, 4);
      }
      return;
    case 2:
      for (uint32_t i = 0; i < bytes; i += 4) {
        uint32_t v;
        std::memcpy(&v, s + i, 4);
        v = (v >> 16) | (v << 16);
        std::memcpy(d + i, &v, 4);
      }
      return;
    default:
      for (uint32_t i = 0; i < bytes; i += 2) {
        uint16_t v;
        std::memcpy(&v, s + i, 2);
        v = uint16_t((v >> 8) | (v << 8));
        std::memcpy(d + i, &v, 2);
      }
      return;
  }
}

// One level (or cube face) of a texture, untiled and endian-swapped into
// `dst` (wb x hb blocks, tightly packed). Tiled surfaces keep 16-byte runs
// together (the micro tile's low 4 address bits, x & 7), so whole runs are
// copied: 8, 8, 4, 2 or 1 blocks for 1, 2, 4, 8 and 16-byte blocks; linear
// rows whole.
void UntileLevel(const uint8_t* lsrc, uint8_t* dst, uint32_t wb, uint32_t hb, uint32_t ox, uint32_t oy,
                        uint32_t pitch_blocks, uint32_t log2_bpb, uint32_t x_or, bool tiled) {
  const uint32_t sbpb = 1u << log2_bpb;
  const uint32_t unit = x_or == 0 ? 1 : x_or == 1 ? 2 : 4;
  const uint32_t run_blocks = log2_bpb >= 4 ? 1u : std::min(8u, 16u >> log2_bpb);
  for (uint32_t y = 0; y < hb; ++y) {
    const int32_t row = int32_t(y + oy);
    uint8_t* out = dst + size_t(y) * wb * sbpb;
    uint32_t x = 0;
    while (x < wb) {
      const uint32_t gx = x + ox;
      uint32_t n, so;
      if (tiled) {
        so = uint32_t(TiledOffset2D(int32_t(gx), row, pitch_blocks, log2_bpb));
        n = std::min(run_blocks - (gx & (run_blocks - 1)), wb - x);
      } else {
        so = (uint32_t(row) * pitch_blocks + gx) << log2_bpb;
        n = wb - x;
      }
      const uint32_t bytes = n << log2_bpb;
      if ((so & (unit - 1)) == 0 && (bytes & (unit - 1)) == 0) {
        CopySwapped(out + (size_t(x) << log2_bpb), lsrc + so, bytes, x_or);
      } else {
        for (uint32_t i = 0; i < bytes; ++i) out[(size_t(x) << log2_bpb) + i] = lsrc[(so + i) ^ x_or];
      }
      x += n;
    }
  }
}

uint32_t Log2Ceil(uint32_t v) { return v <= 1 ? 0 : 32 - __builtin_clz(v - 1); }

uint32_t NextPow2(uint32_t v) { return v <= 1 ? 1 : 1u << Log2Ceil(v); }

// The mip level holding the packed tail (the SDK's GetPackedMipLevel).
uint32_t PackedMipLevel(uint32_t w, uint32_t h) {
  const uint32_t l = Log2Ceil(std::min(w, h));
  return l > 4 ? l - 4 : 0;
}

// Where mip `mip` sits in the packed tail tile, in texels (the SDK's
// GetPackedMipOffset, 2D).
void PackedMipOffset(uint32_t w, uint32_t h, uint32_t mip, uint32_t& x, uint32_t& y) {
  const uint32_t lw = Log2Ceil(w), lh = Log2Ceil(h);
  x = y = 0;
  if (std::min(lw, lh) > 4 + mip) return;
  const uint32_t base = std::min(lw, lh) > 4 ? std::min(lw, lh) - 4 : 0;
  const uint32_t packed = mip - base;
  if (packed < 3) {
    if (lw > lh) y = 16 >> packed; else x = 16 >> packed;
  } else if (lw > lh) {
    x = (1u << (lw - base)) >> (packed - 2);
  } else {
    y = (1u << (lh - base)) >> (packed - 2);
  }
}

TexFormat Format(rex::graphics::xenos::TextureFormat f) {
  using F = rex::graphics::xenos::TextureFormat;
  switch (f) {
    case F::k_8: case F::k_8_A: case F::k_8_B: return {VK_FORMAT_R8_UNORM, 0, 1, 1};
    case F::k_1_5_5_5: return {VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 4, 1};
    case F::k_5_6_5: return {VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 3, 2};
    case F::k_4_4_4_4: return {VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 4, 3};
    case F::k_8_8: return {VK_FORMAT_R8G8_UNORM, 1, 1, 2};
    case F::k_8_8_8_8: case F::k_8_8_8_8_A: case F::k_8_8_8_8_AS_16_16_16_16:
      return {VK_FORMAT_R8G8B8A8_UNORM, 2, 1, 4};
    case F::k_2_10_10_10: case F::k_2_10_10_10_AS_16_16_16_16:
      return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 2, 1, 4};
    case F::k_DXT1: case F::k_DXT1_AS_16_16_16_16: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 3, 4, 4};
    case F::k_DXT2_3: case F::k_DXT2_3_AS_16_16_16_16: return {VK_FORMAT_BC2_UNORM_BLOCK, 4, 4, 4};
    case F::k_DXT4_5: case F::k_DXT4_5_AS_16_16_16_16: return {VK_FORMAT_BC3_UNORM_BLOCK, 4, 4, 4};
    case F::k_DXN: return {VK_FORMAT_BC5_UNORM_BLOCK, 4, 4, 2};
    case F::k_DXT5A: return {VK_FORMAT_BC4_UNORM_BLOCK, 3, 4, 1};
    case F::k_16: return {VK_FORMAT_R16_UNORM, 1, 1, 1};
    case F::k_16_16: return {VK_FORMAT_R16G16_UNORM, 2, 1, 2};
    case F::k_16_16_16_16: return {VK_FORMAT_R16G16B16A16_UNORM, 3, 1, 4};
    case F::k_16_FLOAT: return {VK_FORMAT_R16_SFLOAT, 1, 1, 1};
    case F::k_16_16_FLOAT: return {VK_FORMAT_R16G16_SFLOAT, 2, 1, 2};
    case F::k_16_16_16_16_FLOAT: return {VK_FORMAT_R16G16B16A16_SFLOAT, 3, 1, 4};
    case F::k_32_FLOAT: return {VK_FORMAT_R32_SFLOAT, 2, 1, 1};
    case F::k_32_32_FLOAT: return {VK_FORMAT_R32G32_SFLOAT, 3, 1, 2};
    case F::k_32_32_32_32_FLOAT: return {VK_FORMAT_R32G32B32A32_SFLOAT, 4, 1, 4};
    default: return {};
  }
}

// The fetch swizzle as an image view mapping: 0-3 X..W (a component the
// format lacks repeats its last one), 4 zero, 5 one.
VkComponentMapping Swizzle(uint32_t swizzle, uint32_t components) {
  const VkComponentSwizzle comp[4] = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                                      VK_COMPONENT_SWIZZLE_A};
  VkComponentSwizzle out[4];
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t s = (swizzle >> (i * 3)) & 7;
    out[i] = s < 4 ? comp[std::min(s, components - 1)] : s == 4 ? VK_COMPONENT_SWIZZLE_ZERO : VK_COMPONENT_SWIZZLE_ONE;
  }
  return {out[0], out[1], out[2], out[3]};
}

// A hash of the texture's bytes: all of them when small, else a sample
// (enough to see a video frame or a reloaded texture change).
uint64_t HashBytes(const uint8_t* p, size_t size) {
  if (size <= (64u << 10)) return XXH3_64bits(p, size);
  XXH3_state_t st;
  XXH3_64bits_reset(&st);
  XXH3_64bits_update(&st, p, 64u << 10);
  for (size_t at = 64u << 10; at + 4096 <= size; at += 128u << 10) XXH3_64bits_update(&st, p + at, 4096);
  XXH3_64bits_update(&st, p + size - (64u << 10), 64u << 10);
  return XXH3_64bits_digest(&st);
}

// The descriptor (2D table index) of the texture a fetch constant
// describes, uploading or refreshing it; 0: the placeholder.
// [IzunaRecomp] A mod's picture as a texture's image (RGBA8, in the texture's
// gamma, its swizzle), with mips made by halving it.
bool UploadReplacement(State::Texture& t, const TextureReplacement& picture, VkFormat guest, VkComponentMapping swizzle) {
  const bool srgb = guest == VK_FORMAT_R8G8B8A8_SRGB || guest == VK_FORMAT_BC1_RGBA_SRGB_BLOCK ||
                    guest == VK_FORMAT_BC2_SRGB_BLOCK || guest == VK_FORMAT_BC3_SRGB_BLOCK;
  const uint32_t w = picture.w, h = picture.h;
  const uint32_t mips = 32 - __builtin_clz(std::max(w, h));
  const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
  const VkDeviceSize offset = (g.upload_used + 15) & ~VkDeviceSize(15);
  if (offset + bytes > kFrameUpload || picture.rgba.size() < bytes) return false;
  const VkDeviceSize ring = VkDeviceSize(g.frame_index) * kFrameUpload;
  std::memcpy(g.upload_mapped + ring + offset, picture.rgba.data(), size_t(bytes));
  g.upload_used = offset + bytes;
  Retire(t.image);
  EndRendering();
  if (!CreateImage(g.cmd, t.image, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, w, h, false,
                   VK_IMAGE_VIEW_TYPE_2D, swizzle, mips)) {
    t.image = Image{};
    return false;
  }
  t.image.mips = mips;
  t.image.descriptor = AllocDescriptor(false);
  if (t.image.descriptor) WriteTextureDescriptor(0, t.image.descriptor, t.image.view);
  VkBufferImageCopy r = {};
  r.bufferOffset = ring + offset;
  r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  r.imageExtent = {w, h, 1};
  g.vk.CmdCopyBufferToImage(g.cmd, g.upload, t.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
  for (uint32_t m = 1; m < mips; ++m) {
    Barrier(g.cmd);
    VkImageBlit b = {};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
    b.srcOffsets[1] = {int32_t(std::max(w >> (m - 1), 1u)), int32_t(std::max(h >> (m - 1), 1u)), 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
    b.dstOffsets[1] = {int32_t(std::max(w >> m, 1u)), int32_t(std::max(h >> m, 1u)), 1};
    g.vk.CmdBlitImage(g.cmd, t.image.image, VK_IMAGE_LAYOUT_GENERAL, t.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                      VK_FILTER_LINEAR);
  }
  Barrier(g.cmd);
  return true;
}

uint32_t Texture2D(const uint32_t fc[6], uint32_t* descriptor_1d, uint32_t* descriptor_cube, bool shader_gamma) {
  // (the frame's texture time and the worker record's, from one pair of clock reads)
  struct TextureTime {
    uint64_t t0 = g_timed ? Ticks() : 0;
    ~TextureTime() {
      if (!g_timed) return;
      const uint64_t ns = TicksToNs(Ticks() - t0);
      t_timing.texture_ns += ns;
      t_phase_ns[kPhaseTexture] += int64_t(ns);
    }
  } timed;
  if (descriptor_1d) *descriptor_1d = 0;
  if (descriptor_cube) *descriptor_cube = 0;
  rex::graphics::xenos::xe_gpu_texture_fetch_t f;
  std::memcpy(&f, fc, sizeof(f));
  const bool cube = f.dimension == rex::graphics::xenos::DataDimension::kCube;
  if (f.dimension != rex::graphics::xenos::DataDimension::k2DOrStacked && !cube) return 0;
  const uint32_t faces = cube ? 6 : 1;
  const TexFormat tf = Format(f.format);
  if (tf.vk == VK_FORMAT_UNDEFINED) {
    ++g.stats.textures_unsupported;
    static uint64_t logged = 0;  // each format once
    if (!(logged & (1ull << uint32_t(f.format)))) {
      logged |= 1ull << uint32_t(f.format);
      REXLOG_INFO("[native] texture format {} not supported yet ({}x{}, fetch {:08X} {:08X} {:08X} {:08X})",
                  uint32_t(f.format), f.size_2d.width + 1, f.size_2d.height + 1, fc[0], fc[1], fc[2], fc[3]);
    }
    return 0;
  }
  // Colour channels marked gamma (TextureSign 3) are decoded to linear when
  // sampled: an sRGB format for 8-bit and BC formats.
  // (shader_gamma: the shader decodes it with the exact Xenos curve instead -
  // applyFetchSign, g_BiasedTextures bit 16 + slot)
  TexFormat tfv = tf;
  if (!shader_gamma && uint32_t(f.sign_x) == 3 && uint32_t(f.sign_y) == 3 && uint32_t(f.sign_z) == 3) {
    switch (tf.vk) {
      case VK_FORMAT_R8G8B8A8_UNORM: tfv.vk = VK_FORMAT_R8G8B8A8_SRGB; break;
      case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: tfv.vk = VK_FORMAT_BC1_RGBA_SRGB_BLOCK; break;
      case VK_FORMAT_BC2_UNORM_BLOCK: tfv.vk = VK_FORMAT_BC2_SRGB_BLOCK; break;
      case VK_FORMAT_BC3_UNORM_BLOCK: tfv.vk = VK_FORMAT_BC3_SRGB_BLOCK; break;
      default: break;
    }
  }
  // Signed channels (TextureSign 1): two's-complement data, read as SNORM.
  if (uint32_t(f.sign_x) == 1) {
    switch (tf.vk) {
      case VK_FORMAT_R8_UNORM: tfv.vk = VK_FORMAT_R8_SNORM; break;
      case VK_FORMAT_R8G8_UNORM: tfv.vk = VK_FORMAT_R8G8_SNORM; break;
      case VK_FORMAT_R8G8B8A8_UNORM: tfv.vk = VK_FORMAT_R8G8B8A8_SNORM; break;
      case VK_FORMAT_R16_UNORM: tfv.vk = VK_FORMAT_R16_SNORM; break;
      case VK_FORMAT_R16G16_UNORM: tfv.vk = VK_FORMAT_R16G16_SNORM; break;
      case VK_FORMAT_R16G16B16A16_UNORM: tfv.vk = VK_FORMAT_R16G16B16A16_SNORM; break;
      case VK_FORMAT_BC4_UNORM_BLOCK: tfv.vk = VK_FORMAT_BC4_SNORM_BLOCK; break;
      case VK_FORMAT_BC5_UNORM_BLOCK: tfv.vk = VK_FORMAT_BC5_SNORM_BLOCK; break;
      default: break;
    }
  }
  const uint32_t w = f.size_2d.width + 1, h = f.size_2d.height + 1;
  const uint32_t key_words[6] = {fc[0], fc[1], fc[2], fc[3] & 0x1FFE, (fc[4] & 0x3FC) | (fc[5] & 0xFFFFF800),
                                 uint32_t(shader_gamma)};
  const uint64_t key = XXH3_64bits(key_words, sizeof(key_words));
  auto& t = g.textures[key];
  t.last_used = g.frame_number;
  ++t_timing.tex_lookups;
  auto done = [&]() {
    if (descriptor_1d) *descriptor_1d = t.image.descriptor_1d;
    if (descriptor_cube) *descriptor_cube = t.image.descriptor_cube;
    return t.image.descriptor;
  };
  if (t.image.image && t.checked_frame != UINT64_MAX) {
    // Hashing every bound texture each frame cost the render thread 2-3 ms;
    // the game's textures rarely change once loaded, so those that never did
    // are looked at every native_texture_recheck_frames frames.
    const uint64_t every = t.dynamic || !t.awaiting.empty()  // (a mod's picture being read: looked at each frame)
                               ? 1
                               : uint64_t(std::max(REXCVAR_GET(native_texture_recheck_frames), 1));
    if (g.frame_number - t.checked_frame < every) return done();
  }
  // The levels: the base at base_address, mips 1.. at mip_address (the
  // SDK's GetGuestTextureLayout, 2D): mip L's pitch and height are
  // next_pow2(size) >> L in 32x32-block tiles, each level 4 KB aligned; the
  // levels from the packed one on share one tile at their PackedMipOffset.
  const uint32_t b = tf.block, sbpb = 1u << tf.log2_bpb;
  const uint32_t base = f.base_address << 12, mip_base = f.mip_address << 12;
  const uint32_t packed_level = f.packed_mips ? PackedMipLevel(w, h) : UINT32_MAX;
  uint32_t max_level = 0;
  // (all the levels the fetch constant has, whatever its mip filter: the image
  // is cached by a key without the filter, so a texture first seen through a
  // base-map-only fetch had no mips for every later fetch of it - shimmering
  // walls and floors; base-map-only is the sampler's job, maxLod)
  if (mip_base) max_level = std::min<uint32_t>(f.mip_max_level, 31 - __builtin_clz(std::max(w, h)));
  struct Level {
    uint32_t address, pitch_blocks, ox, oy, wb, hb;
    uint32_t face_stride;  // a cube's faces: one after another, 4 KB aligned
  };
  std::array<Level, 14> levels{};
  uint32_t mip_offset = 0;
  auto layout_pitch = [&](uint32_t level, bool is_base) {
    const uint32_t texels = is_base ? f.pitch * 32 : std::max(NextPow2(w) >> level, 1u);
    uint32_t blocks = ((texels + b - 1) / b + 31) & ~31u;
    if (!f.tiled && !is_base) blocks = ((blocks * sbpb + 255) & ~255u) / sbpb;  // (linear mips: 256-byte rows)
    return std::max(blocks, 1u);
  };
  size_t total_bytes = 0;
  uint32_t level_count = 0;
  for (uint32_t level = 0; level <= max_level && level < levels.size(); ++level) {
    Level& lv = levels[level];
    const uint32_t lw = std::max(w >> level, 1u), lh = std::max(h >> level, 1u);
    lv.wb = (lw + b - 1) / b;
    lv.hb = (lh + b - 1) / b;
    uint32_t px = 0, py = 0;
    if (level >= packed_level) PackedMipOffset(w, h, level, px, py);
    lv.ox = px / b;
    lv.oy = py / b;
    if (level == 0) {
      lv.address = base;
      lv.pitch_blocks = layout_pitch(0, true);
      const uint32_t rows = ((h + b - 1) / b + 31) & ~31u;
      lv.face_stride = (lv.pitch_blocks * sbpb * rows + 4095) & ~4095u;
    } else {
      // The level whose layout holds this one: itself, or the packed tail.
      // (with the whole texture packed, packed_level 0, the mips use the
      // level-0 mip layout at mip_address)
      const uint32_t holder = level < packed_level ? level : packed_level;
      lv.address = mip_base + mip_offset;
      lv.pitch_blocks = layout_pitch(holder, false);
      const uint32_t rows = ((std::max(NextPow2(h) >> holder, 1u) + b - 1) / b + 31) & ~31u;
      lv.face_stride = (lv.pitch_blocks * sbpb * rows + 4095) & ~4095u;
      if (level < packed_level) mip_offset += lv.face_stride * faces;
    }
    total_bytes += size_t(lv.wb) * lv.hb * (tf.convert ? 4u : sbpb) * faces;
    ++level_count;
  }
  // Change detection: the base level's bytes (and the start of the mips).
  const uint32_t base_rows = ((levels[0].hb + levels[0].oy + 31) & ~31u);
  const size_t size = size_t((levels[0].pitch_blocks + 31) & ~31u) * base_rows << tf.log2_bpb;
  if (size_t(base) + size > 0x20000000u) return done();
  auto* memory = REX_KERNEL_MEMORY();
  const uint8_t* src = memory->TranslatePhysical<const uint8_t*>(base);
  const uint64_t hash_t0 = g_timed ? Ticks() : 0;
  uint64_t hash = HashBytes(src, size);
  if (level_count > 1 && size_t(mip_base) + 4096 <= 0x20000000u) {
    hash ^= XXH3_64bits(memory->TranslatePhysical<const uint8_t*>(mip_base), 4096) * 31;
  }
  t.checked_frame = g.frame_number;
  const uint64_t hash_t1 = g_timed ? Ticks() : 0;
  ++t_timing.tex_hashes;
  t_timing.tex_hash_bytes += std::min<size_t>(size, (64u << 10) * 2 + size / 32);
  if (g_timed) t_timing.tex_hash_ns += TicksToNs(hash_t1 - hash_t0);
  struct DecodeTime {
    uint64_t t0;
    bool on = false;
    ~DecodeTime() {
      if (!on) return;
      ++t_timing.tex_decodes;
      if (g_timed) t_timing.tex_decode_ns += TicksToNs(Ticks() - t0);
    }
  } decode_time{hash_t1};
  if (t.image.image && hash == t.hash && t.awaiting.empty()) return done();
  // New or changed bytes: the title's look first (the PS5 port's PlayStation
  // button prompts write their picture over the guest's here, as the
  // emulated GPU's texture cache let them), then the hash of what is drawn.
  if (const auto hook = g_texture_load_hook.load(std::memory_order_acquire)) {
    rex::graphics::GuestTextureLoad load = {};
    load.base_address = base;
    load.base_size = uint32_t(size);
    load.width = w;
    load.height = h;
    load.pitch = f.pitch;
    load.format = uint32_t(f.format);
    load.endianness = uint32_t(f.endianness);
    load.tiled = f.tiled;
    load.dimension = uint32_t(f.dimension);
    load.depth_or_array_size = faces;
    load.mip_max_level = f.mip_max_level;
    load.packed_mips = f.packed_mips;
    hook(load);
    hash = HashBytes(src, size);
    if (level_count > 1 && size_t(mip_base) + 4096 <= 0x20000000u) {
      hash ^= XXH3_64bits(memory->TranslatePhysical<const uint8_t*>(mip_base), 4096) * 31;
    }
    if (t.image.image && hash == t.hash && t.awaiting.empty()) return done();
  }
  // [IzunaRecomp] A mod's picture for this texture (native_mods.cpp): drawn
  // instead, once read. 2D colour textures (not signed, not 1D, not cubes).
  if (ReplacementsExist() && faces == 1 && h > 1 && uint32_t(f.sign_x) != 1) {
    if (t.replaced && t.image.image && hash == t.hash) return done();
    const std::string id = TextureId(w, h, uint32_t(f.format), hash);
    std::shared_ptr<const TextureReplacement> picture;
    const int state = TakeReplacement(id, &picture);
    if (state == 2 && UploadReplacement(t, *picture, tfv.vk, Swizzle(f.swizzle, tf.components))) {
      ReplacementUploaded(id);
      t.hash = hash;
      t.base = base;
      t.replaced = true;
      t.awaiting.clear();
      ++g.stats.textures_replaced;
      return done();
    }
    t.awaiting = state ? id : std::string();  // (2 with a full upload ring: the next frame)
  } else {
    t.awaiting.clear();
  }
  if (t.image.image && hash == t.hash) {  // (the game's own, while the picture is read)
    if (t.dynamic && ++t.steady >= State::kSettleChecks) {
      t.dynamic = false;  // (settled: the periodic checks again)
      t.steady = 0;
    }
    return done();
  }
  if (t.replaced) {  // (new bytes, with no picture for them: the game's own again)
    Retire(t.image);
    t.replaced = false;
  }
  decode_time.on = true;
  if (t.image.image) {
    t.dynamic = true;
    t.steady = 0;
  }
  // Convert into the upload ring, then copy into the image.
  const uint32_t dst_bpb = tf.convert ? 4u : sbpb;
  const VkDeviceSize bytes = VkDeviceSize(total_bytes) + 16 * level_count * faces;
  VkDeviceSize offset = (g.upload_used + 15) & ~VkDeviceSize(15);
  if (offset + bytes > kFrameUpload) {
    ++g.stats.textures_deferred;  // next frame
    t.checked_frame = UINT64_MAX;
    return done();
  }
  const VkDeviceSize ring = VkDeviceSize(g.frame_index) * kFrameUpload;
  const uint32_t x_or = f.endianness == rex::graphics::xenos::Endian::k8in16   ? 1
                        : f.endianness == rex::graphics::xenos::Endian::k8in32 ? 3
                        : f.endianness == rex::graphics::xenos::Endian::k16in32 ? 2
                                                                                  : 0;
  std::array<VkBufferImageCopy, 14 * 6> regions{};
  uint32_t region_count = 0;
  for (uint32_t level = 0; level < level_count; ++level) {
   const Level& lv = levels[level];
   bool level_ok = true;
   for (uint32_t face = 0; face < faces; ++face) {
    const uint32_t rows_needed = ((lv.hb + lv.oy + 31) & ~31u);
    const uint32_t face_address = lv.address + face * lv.face_stride;
    if (size_t(face_address) + (size_t((lv.pitch_blocks + 31) & ~31u) * rows_needed << tf.log2_bpb) > 0x20000000u) {
      level_ok = false;  // (a level outside the guest's memory: stop at the last good one)
      break;
    }
    const uint8_t* lsrc = memory->TranslatePhysical<const uint8_t*>(face_address);
    uint8_t* dst = g.upload_mapped + ring + offset;
    // Untiled in ordinary memory first, then copied to the upload ring in one
    // go (it is write-combined: small scattered writes there are slow).
    static std::vector<uint8_t> staging;
    const size_t level_bytes = size_t(lv.wb) * lv.hb * sbpb;
    if (staging.size() < level_bytes) staging.resize(level_bytes);
    UntileLevel(lsrc, staging.data(), lv.wb, lv.hb, lv.ox, lv.oy, lv.pitch_blocks, tf.log2_bpb, x_or, f.tiled);
    if (!tf.convert) std::memcpy(dst, staging.data(), level_bytes);
    for (uint32_t y = 0; tf.convert && y < lv.hb; ++y) {
      for (uint32_t x = 0; x < lv.wb; ++x) {
        const uint8_t* e = staging.data() + (size_t(y) * lv.wb + x) * sbpb;
        uint8_t* d = dst + (size_t(y) * lv.wb + x) * dst_bpb;
        const uint32_t v = uint32_t(e[0]) | uint32_t(e[1]) << 8;
        auto scale = [](uint32_t c, uint32_t bits) { return uint8_t((c * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1)); };
        if (tf.convert == 1) {
          d[0] = scale(v & 31, 5), d[1] = scale((v >> 5) & 31, 5), d[2] = scale((v >> 10) & 31, 5), d[3] = (v >> 15) ? 255 : 0;
        } else if (tf.convert == 2) {
          d[0] = scale(v & 31, 5), d[1] = scale((v >> 5) & 63, 6), d[2] = scale((v >> 11) & 31, 5), d[3] = 255;
        } else {
          d[0] = scale(v & 15, 4), d[1] = scale((v >> 4) & 15, 4), d[2] = scale((v >> 8) & 15, 4), d[3] = scale(v >> 12, 4);
        }
      }
    }
    VkBufferImageCopy& r = regions[region_count++];
    r.bufferOffset = ring + offset;
    r.bufferRowLength = lv.wb * b;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, face, 1};
    r.imageExtent = {std::max(w >> level, 1u), std::max(h >> level, 1u), 1};
    offset = (offset + VkDeviceSize(lv.wb) * lv.hb * dst_bpb + 15) & ~VkDeviceSize(15);
   }
   if (!level_ok) {
     region_count = level * faces;
     level_count = level;
     break;
   }
  }
  if (!level_count) return done();
  g.upload_used = offset;
  if (t.image.image && t.image.mips != level_count) Retire(t.image);
  if (!t.image.image && TextureDumpOn()) {  // [IzunaRecomp] for mods: named by the texture's id
    DumpTexture(TextureId(w, h, uint32_t(f.format), hash), tfv.vk, w, h, g.upload_mapped + regions[0].bufferOffset,
                size_t(levels[0].wb) * levels[0].hb * dst_bpb);
  }
  if (!t.image.image && REXCVAR_GET(native_dump_textures)) {
    // [dev] The base level as uploaded (untiled, endian-swapped; BC blocks as
    // they are): tools/native/tex2png.py reads the name.
    static uint32_t saved = 0;
    if (saved < 4000) {
      ++saved;
      const auto dir = rex::filesystem::GetExecutableFolder() / "native_textures";
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      std::ofstream out(dir / fmt::format("{:08X}_vk{}_{}x{}_f{}.bin", base, int(tfv.vk), w, h, uint32_t(f.format)),
                        std::ios::binary);
      out.write(reinterpret_cast<const char*>(g.upload_mapped + regions[0].bufferOffset),
                std::streamsize(size_t(levels[0].wb) * levels[0].hb * dst_bpb));
    }
  }
  if (!t.image.image) {
    EndRendering();
    ScopedTime created(t_timing.tex_create_ns);
    ++t_timing.tex_creates;
    if (!CreateImage(g.cmd, t.image, tfv.vk, w, h, false, cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
                     Swizzle(f.swizzle, tf.components), level_count)) {
      t.image = Image{};
      return 0;
    }
    t.image.mips = level_count;
    if (cube) {
      t.image.descriptor_cube = AllocCubeDescriptor();
      if (t.image.descriptor_cube) WriteTextureDescriptor(2, t.image.descriptor_cube, t.image.view);
    } else {
      t.image.descriptor = AllocDescriptor(false);
      if (t.image.descriptor) WriteTextureDescriptor(0, t.image.descriptor, t.image.view);
    }
  }
  if (h == 1 && !t.image.image_1d && b == 1 && level_count == 1 && !cube) {
    Image one;
    if (CreateImage(g.cmd, one, tfv.vk, w, 1, false, VK_IMAGE_VIEW_TYPE_1D, Swizzle(f.swizzle, tf.components))) {
      t.image.image_1d = one.image;
      t.image.view_1d = one.view;
      t.image.memory_1d = one.memory;
      t.image.block_1d = one.block;
      t.image.mem_offset_1d = one.mem_offset;
      t.image.mem_size_1d = one.mem_size;
      t.image.bytes += one.bytes;
      t.image.descriptor_1d = AllocDescriptor(true);
      if (t.image.descriptor_1d) WriteTextureDescriptor(4, t.image.descriptor_1d, one.view);
    }
  }
  EndRendering();
  g.vk.CmdCopyBufferToImage(g.cmd, g.upload, t.image.image, VK_IMAGE_LAYOUT_GENERAL, region_count, regions.data());
  if (t.image.image_1d) g.vk.CmdCopyBufferToImage(g.cmd, g.upload, t.image.image_1d, VK_IMAGE_LAYOUT_GENERAL, 1, regions.data());
  Barrier(g.cmd);
  t.hash = hash;
  t.base = base;
  ++g.stats.textures_uploaded;
  g.stats.texture_bytes += uint32_t(total_bytes);
  return done();
}

// The sampler table index for a fetch constant's addressing and filtering.
// `filter`: 0 the fetch constant's, 1 point, 2 linear - the fetch
// instruction's own mag/min filter, when it sets one (the shader cache's
// pointSlotMask / linearSlotMask).
// `filter`: the fetch instruction's own mag / min filter (0: the fetch
// constant's, 1 point, 2 linear); `mip_override`: its mip filter (0: the constant's, 1
// point, 2 linear, 3 base map only); `aniso_off`: it turns anisotropy off.
uint32_t Sampler(const uint32_t fc[6], uint32_t filter, uint32_t mip_override, bool aniso_off) {
  using rex::graphics::xenos::TextureFilter;
  rex::graphics::xenos::xe_gpu_texture_fetch_t f;
  std::memcpy(&f, fc, sizeof(f));
  if (filter) f.mag_filter = f.min_filter = filter == 1 ? TextureFilter::kPoint : TextureFilter::kLinear;
  if (mip_override) {
    f.mip_filter = mip_override == 1 ? TextureFilter::kPoint : mip_override == 2 ? TextureFilter::kLinear
                                                                                  : TextureFilter::kBaseMap;
  }
  if (aniso_off) {
    f.aniso_filter = rex::graphics::xenos::AnisoFilter::kDisabled;
  } else if (const int32_t forced = REXCVAR_GET(native_anisotropic); forced >= 0) {  // (the player's setting)
    f.aniso_filter = rex::graphics::xenos::AnisoFilter(std::min(forced, 4) + 1);  // (0 = 1x ... 4 = 16x)
  }
  // Everything of the fetch constant a sampler holds (as Xenia's samplers
  // do): the filters, the mip range and LOD bias, anisotropy, the border.
  // Before test33 only the filters and the clamps were taken: the mip filter's
  // base-map-only mode, the game's LOD bias and its anisotropic filtering were
  // lost (floors blurred with distance, a hard mip line across them).
  const uint32_t mip = uint32_t(f.mip_filter), aniso = uint32_t(f.aniso_filter);
  const uint32_t bias_bits = uint32_t(f.lod_bias) & 0x3FF;
  const uint64_t key = uint64_t(f.clamp_x) | uint64_t(f.clamp_y) << 3 | uint64_t(f.clamp_z) << 6 |
                       uint64_t(f.mag_filter) << 9 | uint64_t(f.min_filter) << 11 | uint64_t(mip) << 13 |
                       uint64_t(aniso) << 15 | uint64_t(f.border_color) << 18 | uint64_t(bias_bits) << 20 |
                       uint64_t(f.mip_min_level) << 30 | uint64_t(f.mip_max_level) << 34;
  auto it = g.samplers.find(key);
  if (it != g.samplers.end()) return it->second;
  const uint32_t index = uint32_t(g.samplers.size()) + 1;  // (0: the default)
  if (index >= kMaxSamplers) {
    static bool once = false;
    if (!once) {
      once = true;
      REXLOG_WARN("[native] sampler table full ({}): further sampler states use the default", kMaxSamplers);
    }
    return 0;
  }
  auto address = [](uint32_t c) {
    switch (c) {
      case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
      case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
      case 3: case 5: case 7: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
      case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
      default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
  };
  VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = uint32_t(f.mag_filter) == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  si.minFilter = uint32_t(f.min_filter) == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  si.mipmapMode = mip == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = address(uint32_t(f.clamp_x));
  si.addressModeV = address(uint32_t(f.clamp_y));
  si.addressModeW = address(uint32_t(f.clamp_z));
  si.borderColor = f.border_color == rex::graphics::xenos::BorderColor::k_ABGR_White
                       ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                       : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  // (lod_bias: 10 bits signed, 5 fractional; base map: level 0 only)
  si.mipLodBias = std::clamp(float(int32_t(bias_bits << 22) >> 22) / 32.0f, -g.max_lod_bias, g.max_lod_bias);
  si.minLod = mip == 2 ? 0.0f : float(f.mip_min_level);
  si.maxLod = mip == 2 ? 0.25f : std::max(float(f.mip_max_level), si.minLod);
  // Anisotropy 1:1 ... 16:1 (aniso_filter 1-5), with linear filtering (as
  // Xenia: the hardware only filters anisotropically then).
  if (aniso >= 2 && aniso <= 5 && g.max_anisotropy > 1.0f && si.minFilter == VK_FILTER_LINEAR) {
    si.anisotropyEnable = VK_TRUE;
    si.maxAnisotropy = std::min(float(1u << (aniso - 1)), g.max_anisotropy);
  }
  VkSampler sampler;
  if (g.vk.CreateSampler(g.ctx.device, &si, nullptr, &sampler) != VK_SUCCESS) return 0;
  VkDescriptorImageInfo ii = {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
  VkWriteDescriptorSet wds = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  wds.dstSet = g.sets[3];
  wds.dstArrayElement = index;
  wds.descriptorCount = 1;
  wds.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  wds.pImageInfo = &ii;
  g.vk.UpdateDescriptorSets(g.ctx.device, 1, &wds, 0, nullptr);
  g.samplers.emplace(key, index);
  return index;
}

}  // namespace ng2native::draw
