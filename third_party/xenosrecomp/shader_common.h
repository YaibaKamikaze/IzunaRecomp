#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)

#ifdef UNLEASHED_RECOMP
    #define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_REVERSE_Z         (1 << 4)
#endif

#ifdef REEOT_RECOMP
    #define SPEC_CONSTANT_SINT_TEXCOORD     (1 << 2)
#endif

#ifdef NG2_RECOMP
    // [ng2] The depth target is the Xenos 24-bit float format (20e4): depth
    // is taken to its precision before the test (ng2DepthTo24).
    #define SPEC_CONSTANT_FLOAT24_DEPTH     (1 << 8)
    // [ng2] RB_COLORCONTROL's alpha to mask (ng2AlphaToMask).
    #define SPEC_CONSTANT_ALPHA_TO_MASK     (1 << 9)
    // [IzunaRecomp] Alpha to mask on a 2x MSAA target: the coverage of the
    // pixel's two samples (0, 1/2, 1) to the second output (oC0Coverage),
    // which the pipeline blends colour and alpha by (dual-source) - what the
    // hardware's resolve averages - instead of an on/off dither. In a cache
    // entry's specConstantsMask: the shader has that output.
    #define SPEC_CONSTANT_ALPHA_TO_MASK_MSAA (1 << 10)
#endif

#if !defined(__cplusplus) || defined(__INTELLISENSE__)

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

#ifdef __spirv__

struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
#ifdef NG2_RECOMP
    uint64_t GuestMemory;     // [ng2] the game's physical memory, read in place
    uint64_t FetchConstants;  // [ng2] the 32 fetch slots (6 dwords each), host-endian
#endif
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

#ifdef NG2_RECOMP
// [ng2] Vertex fetch from raw guest memory, for the shaders whose containers
// carry no vertex element table (most of Ninja Gaiden II's): decoded from what
// the vfetch instruction itself encodes, as the Xenos GPU does (and Xenia).
//   slot      vertex fetch constant (constIndex * 3 + constIndexSelect): dword 0
//             the buffer's physical address, dword 1 bits 0-1 the endian mode
//   flags     1 index rounded (else floored), 2 signed, 4 integer (else
//             normalised), 8 signed "no zero" mode (else clamp -1)
//   stride, offset in dwords; format the Xenos vertex format number
uint ng2Swap(uint v, uint endian)
{
    if (endian == 1) return ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);  // 8 in 16
    if (endian == 2) return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);  // 8 in 32
    if (endian == 3) return (v >> 16) | (v << 16);  // 16 in 32
    return v;
}

float ng2Unpack(uint v, uint bits, uint flags)
{
    if (flags & 2)  // signed
    {
        const int s = int(v << (32u - bits)) >> int(32u - bits);
        if (flags & 4) return float(s);
        const float range = float((1u << (bits - 1u)) - 1u);
        if (flags & 8) return (float(s) * 2.0 + 1.0) / (range * 2.0 + 1.0);
        return max(float(s) / range, -1.0);
    }
    if (flags & 4) return float(v);
    return float(v) / float(bits >= 32u ? 4294967295.0 : float((1u << bits) - 1u));
}

// [ng2] Memory export (63 of NG2's vertex shaders write guest memory from the
// GPU). eA's bits: x = stream base in dwords (bit 30 set), y = index of eM0,
// z = format info (endian bits 0-2, format bits 8-13, signed bit 16, integer
// bit 17, red/blue swap bit 19), w = element count; y, z, w carry the 2^23
// float exponent (0x96). As the emulator's SPIR-V translator does. Elements of
// 1 or 2 bytes are not stored yet (they would need a read-modify-write).
uint ng2PackNorm(float v, uint bits, bool isSigned, bool isInt)
{
    if (isSigned)
    {
        const float m = float((1u << (bits - 1u)) - 1u);
        const float c = isInt ? clamp(round(v), -m - 1.0, m) : round(clamp(v, -1.0, 1.0) * m);
        return uint(int(c)) & ((1u << bits) - 1u);
    }
    const float m = float((1u << bits) - 1u);
    return uint(isInt ? clamp(round(v), 0.0, m) : round(saturate(v) * m));
}

void ng2MemExport(float4 eA, uint element, float4 data)
{
    // SharedConstants + 976: nonzero = the native renderer does the memory
    // exports. Zero while the emulator still runs the memexport draws itself
    // (hybrid mode) - writing them twice corrupts guest memory.
    if (vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 976, 4) == 0)
        return;
    const uint4 a = asuint(eA);
    if ((a.x >> 30) != 1u || any((a.yzw >> 23) != 0x96u))
        return;
    const uint index = (a.y & 0x7FFFFFu) + element;
    if (index >= (a.w & 0x7FFFFFu))
        return;
    const uint info = a.z & 0x7FFFFFu;
    if (info & (1u << 19))
        data = data.zyxw;
    data = select(isnan(data), 0.0, data);
    const bool isSigned = (info & (1u << 16)) != 0;
    const bool isInt = (info & (1u << 17)) != 0;
    uint4 packed = 0;
    uint bytes = 0;
    switch ((info >> 8) & 0x3Fu)
    {
    case 50: case 6:  packed.x = ng2PackNorm(data.x, 8, isSigned, isInt) | (ng2PackNorm(data.y, 8, isSigned, isInt) << 8) |
                        (ng2PackNorm(data.z, 8, isSigned, isInt) << 16) | (ng2PackNorm(data.w, 8, isSigned, isInt) << 24); bytes = 4; break;
    case 54: case 7:  packed.x = ng2PackNorm(data.x, 10, isSigned, isInt) | (ng2PackNorm(data.y, 10, isSigned, isInt) << 10) |
                        (ng2PackNorm(data.z, 10, isSigned, isInt) << 20) | (ng2PackNorm(data.w, 2, isSigned, isInt) << 30); bytes = 4; break;
    case 55: case 16: packed.x = ng2PackNorm(data.x, 11, isSigned, isInt) | (ng2PackNorm(data.y, 11, isSigned, isInt) << 11) |
                        (ng2PackNorm(data.z, 10, isSigned, isInt) << 22); bytes = 4; break;
    case 56: case 17: packed.x = ng2PackNorm(data.x, 10, isSigned, isInt) | (ng2PackNorm(data.y, 11, isSigned, isInt) << 10) |
                        (ng2PackNorm(data.z, 11, isSigned, isInt) << 21); bytes = 4; break;
    case 25: packed.x = ng2PackNorm(data.x, 16, isSigned, isInt) | (ng2PackNorm(data.y, 16, isSigned, isInt) << 16); bytes = 4; break;
    case 26: packed.xy = uint2(ng2PackNorm(data.x, 16, isSigned, isInt) | (ng2PackNorm(data.y, 16, isSigned, isInt) << 16),
                               ng2PackNorm(data.z, 16, isSigned, isInt) | (ng2PackNorm(data.w, 16, isSigned, isInt) << 16)); bytes = 8; break;
    case 31: packed.x = f32tof16(data.x) | (f32tof16(data.y) << 16); bytes = 4; break;
    case 32: packed.xy = uint2(f32tof16(data.x) | (f32tof16(data.y) << 16), f32tof16(data.z) | (f32tof16(data.w) << 16)); bytes = 8; break;
    case 36: packed.x = asuint(data.x); bytes = 4; break;
    case 37: packed.xy = asuint(data.xy); bytes = 8; break;
    case 38: packed = asuint(data); bytes = 16; break;
    default: return;  // (1- and 2-byte formats, or unknown)
    }
    // Endian128 (the SDK's EndianSwap128Uint4): 8-in-64 and 8-in-128 reorder
    // the dwords, then swap bytes within each like 8-in-32.
    uint endian = info & 7u;
    if (endian == 4u) { packed = packed.yxwz; endian = 2u; }
    else if (endian == 5u) { packed = packed.wzyx; endian = 2u; }
    packed = uint4(ng2Swap(packed.x, endian), ng2Swap(packed.y, endian), ng2Swap(packed.z, endian), ng2Swap(packed.w, endian));
    const uint64_t p = g_PushConstants.GuestMemory + (((a.x << 2) + index * bytes) & 0x1FFFFFFFu);
    vk::RawBufferStore<uint>(p, packed.x, 4);
    if (bytes >= 8) vk::RawBufferStore<uint>(p + 4, packed.y, 4);
    if (bytes >= 16) { vk::RawBufferStore<uint>(p + 8, packed.z, 4); vk::RawBufferStore<uint>(p + 12, packed.w, 4); }
}

// [ng2] Vertex fetch k of the shader, with the fields the renderer decoded
// from its (Direct3D-patched) instruction at draw time, SharedConstants +
// 1664 + k * 16: x slot | stride << 8 (dwords), y offset (dwords, signed),
// z format, w flags | expAdjust << 8 (signed 8 bits).
float4 ng2VertexFetchK(uint k, float indexValue);

// [ng2] The vertex index the Xenos hardware loads into r0.x. The native
// renderer draws without a Vulkan index buffer (the game's are big-endian) and
// describes the draw at SharedConstants + 960:
//   x  index buffer physical address, 0xFFFFFFFF for a draw without indices
//   y  bit 0: 32-bit indices (else 16-bit); bits 1-2: endian mode (ng2Swap)
//   z  index offset (VGT_INDX_OFFSET), added to every index
//   w  primitive expansion: 0 none, 1 quad list drawn as triangles (6 per quad),
//      2 a primitive-reset strip the renderer turned into a triangle list
//      (little-endian indices, before the offset, at the device address at
//      SharedConstants + 1016) - each strip's winding then starts afresh
// Set by ng2VertexIndex when this vertex is the strip-restart index; the
// vertex shader then outputs a NaN position, so the triangles through it are
// dropped, as a primitive reset drops them on the hardware.
static bool ng2RestartVertex = false;

uint ng2VertexIndex(uint id)
{
    const uint4 ix = vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 960, 4);
    if (ix.w == 2u)
    {
        const uint2 list = vk::RawBufferLoad<uint2>(g_PushConstants.SharedConstants + 1016, 8);
        return vk::RawBufferLoad<uint>(((uint64_t(list.y) << 32) | list.x) + id * 4u, 4) + ix.z;
    }
    if (ix.w == 1u)
    {
        const uint k = id % 6u;
        id = (id / 6u) * 4u + (k < 3u ? k : (k == 3u ? 0u : k - 2u));  // triangles (0 1 2) (0 2 3)
    }
    uint v = id;
    if (ix.x != 0xFFFFFFFFu)
    {
        const uint endian = (ix.y >> 1) & 3u;
        if (ix.y & 1u)
        {
            v = ng2Swap(vk::RawBufferLoad<uint>(g_PushConstants.GuestMemory + ((ix.x + id * 4u) & 0x1FFFFFFCu), 4), endian);
        }
        else
        {
            const uint a = ix.x + id * 2u;
            const uint w = ng2Swap(vk::RawBufferLoad<uint>(g_PushConstants.GuestMemory + (a & 0x1FFFFFFCu), 4), endian);
            v = (a & 2u) ? (w >> 16) : (w & 0xFFFFu);
        }
        // y bit 3: primitive reset on (PA_SU_SC_MODE_CNTL), reset index at + 980
        if ((ix.y & 8u) && v == vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 980, 4))
            ng2RestartVertex = true;
    }
    return v + ix.z;
}

float4 ng2VertexFetch(uint slot, float indexValue, uint stride, int offset, uint format, uint flags, int expAdjust)
{
    const uint2 fc = vk::RawBufferLoad<uint2>(g_PushConstants.FetchConstants + slot * 8, 4);
    const uint base = fc.x & 0x1FFFFFFCu;
    const uint endian = fc.y & 3u;
    const int index = (flags & 1) ? int(round(indexValue)) : int(floor(indexValue));
    // (masked to the 512 MB: a garbage or restart index must not read outside
    // the guest's memory - a GPU page fault kills the app on the console; the
    // 16-byte read below stays inside it too)
    const uint at = min((base + uint(index * int(stride) + offset) * 4u) & 0x1FFFFFFCu, 0x1FFFFFF0u);
    uint4 w = vk::RawBufferLoad<uint4>(g_PushConstants.GuestMemory + at, 4);
    w = uint4(ng2Swap(w.x, endian), ng2Swap(w.y, endian), ng2Swap(w.z, endian), ng2Swap(w.w, endian));
    // [ng2] One decode for every format (a switch of whole decodes, inlined
    // for each fetch, made the shaders several times bigger and slow to
    // build): the format sets its layout - kind (0 fields read as normalised
    // or integer numbers, 1 half floats, 2 floats), component count and each
    // component's width; the components lie one after another from bit 0 of
    // the element, none crossing a dword.
    uint kind = 0, comps = 0;
    uint4 bits = uint4(32, 32, 32, 32);
    switch (format)
    {
    case 6:  comps = 4; bits = uint4(8, 8, 8, 8); break;        // 8_8_8_8
    case 7:  comps = 4; bits = uint4(10, 10, 10, 2); break;     // 2_10_10_10
    case 16: comps = 3; bits = uint4(11, 11, 10, 0); break;     // 10_11_11: x 11, y 11, z 10
    case 17: comps = 3; bits = uint4(10, 11, 11, 0); break;     // 11_11_10: x 10, y 11, z 11
    case 25: comps = 2; bits = uint4(16, 16, 0, 0); break;      // 16_16
    case 26: comps = 4; bits = uint4(16, 16, 16, 16); break;    // 16_16_16_16
    case 31: kind = 1; comps = 2; bits = uint4(16, 16, 16, 16); break;  // 16_16_FLOAT
    case 32: kind = 1; comps = 4; bits = uint4(16, 16, 16, 16); break;  // 16_16_16_16_FLOAT
    case 33: comps = 1; break;                                  // 32
    case 34: comps = 2; break;                                  // 32_32
    case 35: comps = 4; break;                                  // 32_32_32_32
    case 36: kind = 2; comps = 1; break;                        // 32_FLOAT
    case 37: kind = 2; comps = 2; break;                        // 32_32_FLOAT
    case 57: kind = 2; comps = 3; break;                        // 32_32_32_FLOAT
    case 38: kind = 2; comps = 4; break;                        // 32_32_32_32_FLOAT
    }
    float4 r = float4(0.0, 0.0, 0.0, 1.0);
    // (kind is the same for every vertex of the draw: these branches are cheap)
    if (kind == 2)
    {
        const float4 f = asfloat(w);
        r = float4(f.x, comps > 1 ? f.y : 0.0, comps > 2 ? f.z : 0.0, comps > 3 ? f.w : 1.0);
    }
    else if (kind == 1)
    {
        r = float4(f16tof32(w.x & 0xFFFFu), f16tof32(w.x >> 16), comps > 2 ? f16tof32(w.y & 0xFFFFu) : 0.0,
                   comps > 2 ? f16tof32(w.y >> 16) : 1.0);
    }
    else
    {
        uint o = 0;
        [unroll] for (uint i = 0; i < 4; ++i)
        {
            if (i < comps)
            {
                const uint d = o >> 5;
                const uint word = d == 0 ? w.x : d == 1 ? w.y : d == 2 ? w.z : w.w;
                const uint b = bits[i];
                const uint v = b >= 32u ? word : (word >> (o & 31u)) & ((1u << b) - 1u);
                r[i] = ng2Unpack(v, b, flags);
                o += b;
            }
        }
        if (expAdjust != 0)
            r *= exp2(float(expAdjust));
    }
    return r;
}

float4 ng2VertexFetchK(uint k, float indexValue)
{
    const uint4 p = vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 1664 + k * 16, 4);
    const float4 r = ng2VertexFetch(p.x & 0xFFu, indexValue, p.x >> 8, int(p.y), p.z, p.w & 0xFFu, int(p.w << 16) >> 24);
    // [ng2] The instruction's destination swizzle as it is in guest memory at
    // the draw (p.w bits 16-27, 3 bits a component: 0-3 X-W, 4 zero, 5 one):
    // Direct3D patches it with the stride, offset and format when a vertex
    // declaration is bound - a D3DCOLOR element's red and blue trade places
    // there - so the one the shader was compiled with can be stale (issue #4:
    // the health bar's red damage drawn blue). Applied here; the generated
    // code takes the components in order.
    const uint sw = p.w >> 16;
    float4 o = r;
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        const uint s = (sw >> (i * 3u)) & 7u;
        o[i] = s < 4u ? r[s] : s == 4u ? 0.0 : s == 5u ? 1.0 : r[i];
    }
    return o;
}
#endif

#ifdef REEOT_RECOMP
#define g_Booleans(i)              vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 320 + (i)*4)
#define g_SwappedTexcoords         vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 352)
#define g_HalfPixelOffset          vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 356)
#define g_AlphaThreshold           vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 364)
#ifdef NG2_RECOMP
#define g_AlphaFunc                vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 952)
#define g_AlphaToMaskOffsets       vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 956)
#endif
#define g_SwappedNormals           vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 368)
#define g_SwappedBinormals         vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 372)
#define g_SwappedTangents          vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 376)
#define g_SwappedBlendWeights      vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 380)
#define g_SwappedPositions         vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 384)
#define g_SintTexcoords            vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 388)
#define g_BiasedTextures           vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 392)
#define g_PackedDec3               vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 396)
#define g_LoopConstants(i)         vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 400 + (i)*16)
#define g_PosScale                 vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 912)
#define g_PosOffset                vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 928)
#ifdef NG2_RECOMP
// [ng2] Resolution scale: the native renderer draws its targets S times the
// guest's size. 1/S turns vPos back into guest pixels; the slots holding
// such images (resolves) take a fetch's texel offset - in guest texels - x S.
#define g_RenderScaleInv           vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 944)
#define g_ScaledSlots              vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 948)
// [IzunaRecomp] The scale of the scaled slots' images, apart from the target's
// (g_RenderScaleInv): at 1.5x the renderer keeps its small targets at 1x, and a
// draw into one samples the 1.5x scene (SharedConstants + 2688).
#define g_TextureScale             vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 2688)
// [IzunaRecomp] Per axis: the target's inverse scale (the game's 3D scene is
// scaled apart horizontally and vertically, so its final stretch lands 1:1),
// and each scaled slot's own scale (SharedConstants + 2704 + slot * 8).
#define g_RenderScaleInv2          vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 2696, 8)
// [IzunaRecomp] An interpolated input as the Xenos interpolator gives it: a
// value the same at a triangle's three corners comes back exactly that value.
// A PC GPU's perspective-correct interpolation returns it a few ulps off
// (1.0 as 0.99999994) on some pixels and not others - and NG2 tests such
// values. Its dismemberment (pixel shader 5D68..., SupDel) hides the joint
// triangles of a cut limb where bone 1's weight (TEXCOORD2.x) is below 1.0;
// a rigid vertex's 1.0 read low hid half the limb's pixels at random, the
// "see-through" severed limbs. A value within 2^-18 of a multiple of 2^-16 is
// snapped to it: exact constants come back exact, and a value that really
// varies moves by under four millionths.
// [IzunaRecomp] test28: on single components (the translator snaps only the
// components whose values reach a comparison: ng2NarrowInterpolantSnaps), and
// a fixed tolerance of 2^-18 (an interpolated constant's error is a few ulps
// for values up to ~8, where the snaps that matter are: weights of 0 and 1).
template <typename T>
T ng2Interpolated(T v)
{
    const T g = round(v * 65536.0) * (1.0 / 65536.0);
    return select(abs(g - v) <= (1.0 / 262144.0), g, v);
}

// [ng2] Depth as the Xenos 20e4 depth buffer holds it, truncated as the
// hardware stores it (the SDK's Float32To20e4 / Float20e4To32 round trip):
// from 2^-14 up the low 3 mantissa bits go, below that steps of 2^-34. A
// 32-bit float depth keeps differences 20e4 does not, so coplanar surfaces
// the game draws on top of each other (a light on a wall) z-fought there -
// the shop window's light flickering frame to frame.
float ng2DepthTo24(float z)
{
    z = saturate(z);
    const uint u = asuint(z);
    return u >= 0x38800000u ? asfloat(u & ~7u) : floor(z * 17179869184.0) * (1.0 / 17179869184.0);
}
// [ng2] Each texture slot's exponent adjust (fetch constant dword 3, bits
// 13-18, signed): the hardware multiplies a fetch's result by 2^adjust - a
// signed byte per slot at SharedConstants + 984.
float ng2ExpAdjust(uint slot)
{
    const uint w = vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 984 + (slot & ~3u), 4);
    const int e = int(w << (24u - 8u * (slot & 3u))) >> 24;
    return e != 0 ? exp2(float(e)) : 1.0;
}
float2 ng2OffsetScale(uint slot)
{
    return ((g_ScaledSlots >> slot) & 1u) != 0u
        ? vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 2704 + slot * 8, 8)
        : float2(1.0, 1.0);
}
#endif
#else
#define g_Booleans                 vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 256)
#define g_SwappedTexcoords         vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 260)
#define g_HalfPixelOffset          vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 264)
#define g_AlphaThreshold           vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 272)
#endif

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;

#define g_SpecConstants() g_SpecConstants

#else

#ifdef REEOT_RECOMP
#define DEFINE_SHARED_CONSTANTS() \
    uint4 g_BooleansArr[2] : packoffset(c20); \
    uint g_SwappedTexcoords : packoffset(c22.x); \
    float2 g_HalfPixelOffset : packoffset(c22.y); \
    float g_AlphaThreshold : packoffset(c22.w); \
    uint g_SwappedNormals : packoffset(c23.x); \
    uint g_SwappedBinormals : packoffset(c23.y); \
    uint g_SwappedTangents : packoffset(c23.z); \
    uint g_SwappedBlendWeights : packoffset(c23.w); \
    uint g_SwappedPositions : packoffset(c24.x); \
    uint g_SintTexcoords : packoffset(c24.y); \
    uint g_BiasedTextures : packoffset(c24.z); \
    uint g_PackedDec3 : packoffset(c24.w); \
    uint4 g_LoopConstantsArr[32] : packoffset(c25); \
    float4 g_PosScale : packoffset(c57); \
    float4 g_PosOffset : packoffset(c58);

#define g_Booleans(i) (g_BooleansArr[(i) / 4][(i) % 4])
#define g_LoopConstants(i) g_LoopConstantsArr[i]
#else
#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    uint g_SwappedTexcoords : packoffset(c16.y); \
    float2 g_HalfPixelOffset : packoffset(c16.z); \
    float g_AlphaThreshold : packoffset(c17.x);
#endif

#ifdef REEOT_RECOMP
cbuffer SpecConstants : register(b4, space4)
{
    uint g_SpecConstantsValue;
};
#define g_SpecConstants() g_SpecConstantsValue
#else
uint g_SpecConstants();
#endif

#endif

#ifdef REEOT_RECOMP
#define BOOL_BIT(n) ((g_Booleans((n) / 32u) & (1u << ((n) & 31u))) != 0)
#endif

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
Texture1D<float4> g_Texture1DDescriptorHeap[] : register(t0, space4);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

#ifdef REEOT_RECOMP
float pwlGammaToLinear1(float gamma)
{
    gamma = saturate(gamma);
    float scale, offset;
    if (gamma >= 96.0 / 255.0)
    {
        if (gamma >= 192.0 / 255.0) { scale = 8.0 / 1024.0; offset = -1024.0; }
        else                        { scale = 4.0 / 1024.0; offset = -256.0; }
    }
    else
    {
        if (gamma >= 64.0 / 255.0)  { scale = 2.0 / 1024.0; offset = -64.0; }
        else                        { scale = 1.0 / 1024.0; offset = 0.0; }
    }
    float lin = gamma * ((255.0 * 1024.0) * scale) + offset;
    lin += trunc(lin * scale);
    return lin * (1.0 / 1023.0);
}

float linearToPWLGamma1(float lin)
{
    lin = saturate(lin);
    float scale, offset;
    if (lin >= 128.0 / 1023.0)
    {
        if (lin >= 512.0 / 1023.0) { scale = 1023.0 / 8.0; offset = 128.0 / 255.0; }
        else                       { scale = 1023.0 / 4.0; offset = 64.0 / 255.0; }
    }
    else
    {
        if (lin >= 64.0 / 1023.0)  { scale = 1023.0 / 2.0; offset = 32.0 / 255.0; }
        else                       { scale = 1023.0;       offset = 0.0; }
    }
    return trunc(lin * scale) * (1.0 / 255.0) + offset;
}

float3 linearToPWLGamma(float3 lin)
{
    return float3(linearToPWLGamma1(lin.r), linearToPWLGamma1(lin.g),
                  linearToPWLGamma1(lin.b));
}

float4 applyFetchSign(float4 value, uint biasedMask, uint alphaMask, uint slotIndex)
{
    if (biasedMask & (1u << slotIndex))
        value.rgb = value.rgb * 2.0 - 1.0;
    if (alphaMask & (1u << (16u + slotIndex)))
        value.a = value.a * 2.0 - 1.0;
    if (biasedMask & (1u << (16u + slotIndex)))
        value.rgb = float3(pwlGammaToLinear1(value.r), pwlGammaToLinear1(value.g),
                           pwlGammaToLinear1(value.b));
#ifdef NG2_RECOMP
    value *= ng2ExpAdjust(slotIndex);  // (after the sign conversion, as Xenia applies it)
#endif
    return value;
}
#endif

#if defined(NG2_VERTEX_SHADER) && NG2_VERTEX_SHADER
// [ng2] Ninja Gaiden II samples textures in vertex shaders, where only an
// explicit LOD is allowed: the helpers below sample level 0 there.
#define Sample(samplerState, texCoord) SampleLevel(samplerState, texCoord, 0)
#endif

#ifdef NG2_RECOMP
// [ng2] How a texture fetch picks its mip, as the hardware does (the SDK's
// SPIR-V translator, ProcessTextureFetchInstruction): set by the recompiler
// before each fetch from the instruction's fields - the fetch constant's own
// LOD bias is in the sampler.
//   0 the computed LOD;        1 the computed LOD + ng2Lod;
//   2 the level ng2Lod (UseComputedLod off - and every vertex shader fetch);
//   3 the gradients set by setGradientH / V, scaled by 2^ng2Lod.
// ng2RegLod: setTexLOD's value, for fetches with UseRegisterLOD.
static uint ng2LodMode = 0;
static float ng2Lod = 0.0;
static float ng2RegLod = 0.0;
static float3 ng2GradH = 0.0;
static float3 ng2GradV = 0.0;

#if defined(NG2_VERTEX_SHADER) && NG2_VERTEX_SHADER
#define NG2_SAMPLE(t, s, c, gh, gv) t.SampleLevel(s, c, ng2LodMode == 0 ? 0.0 : ng2Lod)
#else
#define NG2_SAMPLE(t, s, c, gh, gv)                                                     \
    (ng2LodMode == 0 ? t.Sample(s, c) :                                                  \
     ng2LodMode == 1 ? t.SampleBias(s, c, ng2Lod) :                                      \
     ng2LodMode == 2 ? t.SampleLevel(s, c, ng2Lod) :                                     \
                       t.SampleGrad(s, c, (gh) * exp2(ng2Lod), (gv) * exp2(ng2Lod)))
#endif
#endif

float4 tfetch1D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float texCoord)
{
#ifdef NG2_RECOMP
    Texture1D<float4> t = g_Texture1DDescriptorHeap[resourceDescriptorIndex];
    return NG2_SAMPLE(t, g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord, ng2GradH.x, ng2GradV.x);
#endif
    return g_Texture1DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
#ifdef NG2_RECOMP
    return NG2_SAMPLE(texture, g_SamplerDescriptorHeap[samplerDescriptorIndex],
                      texCoord + offset / getTexture2DDimensions(texture), ng2GradH.xy, ng2GradV.xy);
#else
    return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + offset / getTexture2DDimensions(texture));
#endif
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}

float w0(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float w1(float a)
{
    return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float w2(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float w3(float a)
{
    return (1.0f / 6.0f) * (a * a * a);
}

float g0(float a)
{
    return w0(a) + w1(a);
}

float g1(float a)
{
    return w2(a) + w3(a);
}

float h0(float a)
{
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    uint2 dimensions = getTexture2DDimensions(texture);
    
    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return r;
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
#ifdef NG2_RECOMP
    Texture3D<float4> t = g_Texture3DDescriptorHeap[resourceDescriptorIndex];
    return NG2_SAMPLE(t, g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord, ng2GradH, ng2GradV);
#endif
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

#ifdef NG2_RECOMP
// [ng2] The alpha test (RB_COLORCONTROL) with its own compare function
// (SharedConstants + 952: never, <, ==, <=, >, !=, >=, always) against
// RB_ALPHA_REF. Only >= was done before test40 (clip(alpha - ref)): a test
// with any other function drew everything - the boss health bar, drawn
// whole and cut at the boss's health by its alpha, stayed full.
void ng2AlphaTest(float alpha)
{
    const float ref = g_AlphaThreshold;
    bool pass;
    switch (g_AlphaFunc)
    {
    case 0: pass = false; break;
    case 1: pass = alpha < ref; break;
    case 2: pass = alpha == ref; break;
    case 3: pass = alpha <= ref; break;
    case 4: pass = alpha > ref; break;
    case 5: pass = alpha != ref; break;
    case 6: pass = alpha >= ref; break;
    default: pass = true; break;
    }
    if (!pass) discard;
}

// [ng2] Alpha to mask on a single-sample target (RB_COLORCONTROL bit 4; the
// SDK's Xenia-derived translators, CompletePixelShader_AlphaToMask): the
// pixel's place in a 2x2 square picks one of four 2-bit offsets (the
// register's top byte, SharedConstants + 956), and the pixel is kept where
// alpha >= 1 - offset / 4 - an ordered dither the game chooses. In host
// pixels with resolution scaling, as the SDK does (the dither stays fine).
uint ng2AlphaToMaskOffsetIndex(float2 position)
{
    const uint2 p = uint2(position);
    return ((p.x & 1u) << 1) | (p.y & 1u);
}
// Single sample: the ordered dither, the pixel kept or not.
void ng2AlphaToMask(float alpha, float2 position)
{
    const float offset = float((g_AlphaToMaskOffsets >> (ng2AlphaToMaskOffsetIndex(position) * 2u)) & 3u);
    if (!(alpha >= 1.0 - offset * 0.25)) discard;
}
// [IzunaRecomp] 2x MSAA (the SDK's translators: sample thresholds 0.5 and 1.0,
// less offset / 8): the samples covered, each half the pixel - the resolve's
// weight for this draw's colour and alpha, which stay the shader's own.
float ng2AlphaToMaskCoverage(float alpha, float2 position)
{
    const float offset = float((g_AlphaToMaskOffsets >> (ng2AlphaToMaskOffsetIndex(position) * 2u)) & 3u);
    const float coverage = ((alpha >= 0.5 - offset * 0.125) ? 0.5 : 0.0) +
                           ((alpha >= 1.0 - offset * 0.125) ? 0.5 : 0.0);
    if (!(coverage > 0.0)) discard;
    return coverage;
}

// [ng2] The Xenos cube instruction as the hardware computes it (the SDK's
// Xenia-derived SPIR-V translator): of the direction (x, y, z), the face's T
// and S coordinates, twice the major axis and the face index (+X -X +Y -Y +Z
// -Z = 0-5). `operand` is the instruction's swizzled source, .z_xy of the
// direction. Unleashed's shortcut (keep the direction, return its index)
// kept only two per shader and broke on any math between cube and the
// fetch - reflections read from the wrong place.
float4 ng2Cube(float4 operand)
{
    const float3 v = float3(operand.z, operand.w, operand.x);
    const float3 a = abs(v);
    float4 r;
    if (a.z >= a.x && a.z >= a.y)
        r = float4(-v.y, v.z < 0.0 ? -v.x : v.x, v.z, v.z < 0.0 ? 5.0 : 4.0);
    else if (a.y >= a.x)
        r = float4(v.y < 0.0 ? -v.z : v.z, v.x, v.y, v.y < 0.0 ? 3.0 : 2.0);
    else
        r = float4(-v.y, v.x < 0.0 ? v.z : -v.z, v.x, v.x < 0.0 ? 1.0 : 0.0);
    r.z *= 2.0;
    return r;
}

// The direction a cube fetch's (S, T, face) stands for: S and T from the
// 1...2 range the shaders put them in back to -1...1, the axes remapped
// opposite to ng2Cube (as the SDK's translator does).
float3 ng2CubeDirection(float3 stf)
{
    const float s = stf.x * 2.0 - 3.0, t = stf.y * 2.0 - 3.0;
    const uint face = uint(clamp(stf.z, 0.0, 5.0));
    const bool negative = (face & 1u) != 0;
    const float sign = negative ? -1.0 : 1.0;
    switch (face >> 1)
    {
    case 0: return float3(sign, -t, negative ? s : -s);
    case 1: return float3(s, sign, negative ? -t : t);
    default: return float3(negative ? -s : s, -t, sign);
    }
}
#endif

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
#ifdef NG2_RECOMP
    TextureCube<float4> t = g_TextureCubeDescriptorHeap[resourceDescriptorIndex];
    return NG2_SAMPLE(t, g_SamplerDescriptorHeap[samplerDescriptorIndex], ng2CubeDirection(texCoord),
                      ng2GradH, ng2GradV);
#endif
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]);
}

#ifdef REEOT_RECOMP
float4 tfetchR11G11B10(uint dec3Mask, float4 value, uint slotCode)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        uint v = asuint(value.x);
        if (dec3Mask & (1u << slotCode))
        {
            int4 s = int4(v << 22, v << 12, v << 2, v) >> int4(22, 22, 22, 30);
            return max(float4(s) / float4(511.0, 511.0, 511.0, 1.0), -1.0);
        }
        return float4(
            (v & 0x00000400 ? -1.0 : 0.0) + ((v & 0x3FF) / 1024.0),
            (v & 0x00200000 ? -1.0 : 0.0) + (((v >> 11) & 0x3FF) / 1024.0),
            (v & 0x80000000 ? -1.0 : 0.0) + (((v >> 22) & 0x1FF) / 512.0),
            1.0);
    }
    return value;
}

float4 swapFloats(uint swappedMask, float4 value, uint semanticIndex)
{
    return (swappedMask & (1u << semanticIndex)) != 0 ? value.yxwz : value;
}

float4 sintTexcoord(uint mask, float4 value, uint semanticIndex)
{
    if ((mask & (1u << semanticIndex)) != 0)
    {
        int4 si = (int4(asuint(value)) << 16) >> 16;
        return float4(si);
    }
    return value;
}
#else
float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
    return (swappedTexcoords & (1ull << semanticIndex)) != 0 ? value.yxwz : value;
}
#endif

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;
    
    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

void eotTwoSum(float a, float b, out float s, out float e)
{
    precise float sum = a + b;
    precise float bv = sum - a;
    precise float err = (a - (sum - bv)) + (b - bv);
    s = sum;
    e = err;
}

void eotTwoProduct(float a, float b, out float p, out float e)
{
    precise float ca = 4097.0 * a;
    precise float ah = ca - (ca - a);
    precise float al = a - ah;
    precise float cb = 4097.0 * b;
    precise float bh = cb - (cb - b);
    precise float bl = b - bh;
    precise float prod = a * b;
    precise float err = ((ah * bh - prod) + ah * bl + al * bh) + al * bl;
    p = prod;
    e = err;
}

// [ng2] Direct3D 9's multiplication, as the Xbox 360's GPU does it: 0 (or a
// denormal, flushed) times anything is +0 - infinity and NaN included, where
// a PC GPU makes NaN (Xenia's shader interpreter: "Direct3D 9 behavior").
// Mul, Mad, Dp2Add / Dp3 / Dp4, Dst and the scalar Muls / Mulsc / MulsPrev(2)
// (shader_recompiler.cpp); a NaN from 0 x infinity spread through a
// lighting or fog term was the issue #6 suspect (ch. 4 rails, as on Xenia).
// (`precise` inside: DXC does not carry a caller's precise into a called
// function, and a product left free to fuse with the add after it would round
// differently from the Xbox's separate multiply and add - two passes computing
// one position apart, depth fighting. Noted by gamboozino's ng2recomp.)
template <typename T>
T ng2Mul(T a, T b)
{
    precise T p = a * b;
    return select(or(abs(a) < 1.175494351e-38, abs(b) < 1.175494351e-38), (T)0.0, p);
}
template <typename T>
float ng2Dot(T a, T b)
{
    precise float s = dot(ng2Mul(a, b), (T)1.0);
    return s;
}
float4 ng2Dst(float4 a, float4 b)
{
    return float4(1.0, ng2Mul(a.y, b.y), a.z, b.w);
}

float dotF(float4 a, float4 b) { return ng2Dot(a, b); }
float dotF(float3 a, float3 b) { return ng2Dot(a, b); }

float dotP(float4 a, float4 b)
{
    {  // [ng2] (D3D9's 0 x anything = 0, above)
        const bool4 z = or(abs(a) < 1.175494351e-38, abs(b) < 1.175494351e-38);
        a = select(z, 0.0, a);
        b = select(z, 0.0, b);
    }
    float p, s, h, r, q;
    eotTwoProduct(a.x, b.x, p, s);
    eotTwoProduct(a.y, b.y, h, r);
    eotTwoSum(p, h, p, q);
    precise float s1 = s + (q + r);
    eotTwoProduct(a.z, b.z, h, r);
    eotTwoSum(p, h, p, q);
    precise float s2 = s1 + (q + r);
    eotTwoProduct(a.w, b.w, h, r);
    eotTwoSum(p, h, p, q);
    precise float s3 = s2 + (q + r);
    precise float result = p + s3;
    return isfinite(result) ? result : dot(a, b);
}

float dotP(float3 a, float3 b)
{
    {  // [ng2] (D3D9's 0 x anything = 0, above)
        const bool3 z = or(abs(a) < 1.175494351e-38, abs(b) < 1.175494351e-38);
        a = select(z, 0.0, a);
        b = select(z, 0.0, b);
    }
    float p, s, h, r, q;
    eotTwoProduct(a.x, b.x, p, s);
    eotTwoProduct(a.y, b.y, h, r);
    eotTwoSum(p, h, p, q);
    precise float s1 = s + (q + r);
    eotTwoProduct(a.z, b.z, h, r);
    eotTwoSum(p, h, p, q);
    precise float s2 = s1 + (q + r);
    precise float result = p + s2;
    return isfinite(result) ? result : dot(a, b);
}

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]) * texCoord;
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif

#endif
