// [native renderer] The shader cache our XenosRecomp fork writes
// (out/native-shaders/shader_cache.cpp, tools/native/build_shaders.sh): one
// entry per unique shader container in the game's executable, keyed by the
// XXH3 hash of the whole container (header + microcode), and all the SPIR-V,
// zstd-compressed. Made from the game: build output only.
#pragma once

#include <cstddef>
#include <cstdint>

// The fork's REEOT layout: the primary blob, then the vertex shader variants
// (full interpolants, position only, velocity), each as DXIL offset/size and
// SPIR-V offset/size. NG2 builds only the primary (variants are disabled).
struct ShaderCacheEntry {
  uint64_t hash;
  uint32_t dxilOffset, dxilSize, spirvOffset, spirvSize;
  uint32_t specConstantsMask;
  uint32_t vertexLayoutOffset, vertexLayoutCount, vfetchCodeOffset;
  uint32_t usesFloatConstants, interpolantMask;
  uint32_t fullDxilOffset, fullDxilSize, fullSpirvOffset, fullSpirvSize;
  uint32_t posDxilOffset, posDxilSize, posSpirvOffset, posSpirvSize;
  uint32_t velDxilOffset, velDxilSize, velSpirvOffset, velSpirvSize;
  // [ng2] What a draw must supply (the fork's NG2 epilogue of the walk):
  // float constant registers 0..floatConstantRegs-1 (256 if read relatively),
  // and the texture fetch slots the shader's tfetch instructions use.
  uint32_t floatConstantRegs, textureSlotMask;
  // Texture slots whose fetch instruction sets the filter (point / linear)
  // instead of leaving it to the fetch constant: the slot's sampler follows.
  uint32_t pointSlotMask, linearSlotMask;
  // ... the mip filter (point / linear / base map only) and anisotropy off,
  // likewise (test38: the instruction's mip filter was ignored - a fetch
  // constant's base-map-only then kept a mipped texture at level 0).
  uint32_t mipPointSlotMask, mipLinearSlotMask, mipBaseSlotMask, anisoOffSlotMask;
  // The shader writes guest memory (memory export, ng2MemExport): what later
  // draws read must wait for it (test40).
  uint32_t memExport;
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const uint32_t g_shaderVertexLayouts[];
extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
extern const size_t g_shaderCacheEntryCount;
