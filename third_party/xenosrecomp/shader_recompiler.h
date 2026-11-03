#pragma once

#include "shader.h"
#include "shader_code.h"

struct StringBuffer
{
    std::string out;

    template<class... Args>
    void print(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
    }

    template<class... Args>
    void println(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
        out += '\n';
    }
};

#ifdef REEOT_RECOMP
//   w0: usage:4 | usageIndex:4 | isMiniFetch:1
//   w1: instrAddress:16 | parentFullFetchAddress:16  (96-bit instruction units,
//       relative to the physical code start = ShaderCacheEntry.vfetch_code_offset)
struct VertexFetchLayoutRecord
{
    uint32_t w0;
    uint32_t w1;
};
#endif

struct ShaderRecompiler : StringBuffer
{
    uint32_t missingVertexElements = 0;  // [ng2] (unused now: such vfetches read raw memory)
    uint32_t ng2FetchSlot = 0;           // [ng2] the last full vfetch's slot, index and stride,
    std::string ng2FetchIndex;           //       for the mini fetches after it (ng2VertexFetch)
    uint32_t ng2FetchStride = 0;
    uint64_t unsupportedExports = 0;     // [ng2] export registers with no destination (bit = register)
    std::unordered_set<uint32_t> ng2LiteralConstants;  // [ng2] registers the shader defines as literals (c<N> locals)
    bool usesMemExport = false;          // [ng2] writes guest memory (ng2MemExport)

    uint32_t indentation = 0;
    bool isPixelShader = false;
    const uint8_t* constantTableData = nullptr;
    std::unordered_map<uint32_t, VertexElement> vertexElements;
    std::unordered_map<uint32_t, std::string> interpolators;
    std::unordered_map<uint32_t, const ConstantInfo*> float4Constants;
    std::unordered_map<uint32_t, const char*> boolConstants;
    std::unordered_map<uint32_t, const char*> int4Constants;
    std::unordered_map<uint32_t, const char*> samplers;
    std::unordered_map<uint32_t, uint32_t> ifEndLabels;
    std::unordered_set<uint32_t> elseLabels;
    uint32_t specConstantsMask = 0;
    bool ng2CoverageOutput = false;  // [IzunaRecomp] this pixel shader declares oC0Coverage

#ifdef REEOT_RECOMP
    std::vector<VertexFetchLayoutRecord> vertexLayout;
    uint32_t lastFullVfetchAddress = 0;
    uint32_t physicalCodeOffset = 0;
    bool usesFloatConstants = false;
    uint32_t interpolantMask = 0;
    // [ng2] What the renderer must supply for this shader: float constant
    // registers 0..ng2FloatConstantRegs-1 (256 when any read is relative to
    // a0 / aL), and the texture fetch slots its tfetch instructions use.
    uint32_t ng2FloatConstantRegs = 0;
    uint32_t ng2TextureSlotMask = 0;
    // [ng2] Texture slots whose fetch instruction sets the filter itself
    // (mag/min filter fields: 0 point, 1 linear; 3 = the fetch constant's):
    // the native renderer's sampler for that slot follows the instruction.
    uint32_t ng2PointSlotMask = 0;
    uint32_t ng2LinearSlotMask = 0;
    // [ng2] ... and the mip filter (0 point, 1 linear, 2 base map only; 3 = the
    // fetch constant's) and anisotropy (0 off; 7 = the fetch constant's).
    uint32_t ng2MipPointSlotMask = 0;
    uint32_t ng2MipLinearSlotMask = 0;
    uint32_t ng2MipBaseSlotMask = 0;
    uint32_t ng2AnisoOffSlotMask = 0;
    enum class InterpolantVariant : uint8_t { Trimmed, Full, PositionOnly, Velocity };
    InterpolantVariant interpolantVariant = InterpolantVariant::Trimmed;
    bool velocityVertexShader() const { return !isPixelShader && interpolantVariant == InterpolantVariant::Velocity; }
    struct VertexAluWrite
    {
        uint32_t vectorDest;
        uint32_t vectorMask;
        uint64_t vectorSources;
        size_t dotOffset;
        uint32_t scalarDest;
        uint32_t scalarMask;
        bool scalarOp;
        bool scalarReadsPs;
        uint64_t scalarSources;
        bool conditional;
        uint64_t positionSources;
        bool positionReadsPs;
    };
    std::vector<VertexAluWrite> vertexAluWrites;
    size_t registerDeclOffsets[32]{};
    size_t psDeclOffset = 0;
    bool straightLineFlow = true;
#endif

#ifdef UNLEASHED_RECOMP
    bool hasMtxProjection = false;
    bool hasMtxPrevInvViewProjection = false;
#endif

    void indent()
    {
        for (uint32_t i = 0; i < indentation; i++)
            out += '\t';
    }

    void printDstSwizzle(uint32_t dstSwizzle, bool operand);
    void printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle);

    void recompile(const VertexFetchInstruction& instr, uint32_t address);
    void recompile(const TextureFetchInstruction& instr, bool bicubic);
    void recompile(const AluInstruction& instr);

    void recompile(const uint8_t* shaderData, const std::string_view& include);
};
