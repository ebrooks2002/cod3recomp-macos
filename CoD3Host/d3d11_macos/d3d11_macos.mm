#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Cocoa/Cocoa.h>

#include "d3d11_1.h"
#include "dxgi1_5.h"
#include "d3dcompiler.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_metal.h>

#define ENABLE_HLSL 1
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv_cross/spirv_msl.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mach/mach_time.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ============================================================================
// Blob Implementation
// ============================================================================
struct Cod3Blob final : public ID3DBlob {
    std::vector<uint8_t> data;

    explicit Cod3Blob(size_t size) : data(size, 0) {}
    Cod3Blob(const void* src, size_t size)
        : data(static_cast<const uint8_t*>(src), static_cast<const uint8_t*>(src) + size) {}

    LPVOID GetBufferPointer() override { return data.empty() ? nullptr : data.data(); }
    SIZE_T GetBufferSize() override { return data.size(); }
};

// ============================================================================
// Compiled Shader Bytecode Container ("CD3M")
// ============================================================================
static constexpr uint32_t kShaderMagic = 0x4D334443u; // "CD3M"
static constexpr uint32_t kShaderVersion = 2u;

struct ShaderBindingPair {
    uint16_t d3dSlot;
    uint16_t mslSlot;
};

struct ShaderInputBinding {
    char     semanticName[16];
    uint16_t semanticIndex;
    uint16_t location;
};

struct ShaderBlobHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t stage;       // 1 = VS, 2 = PS, 3 = GS
    uint32_t flags;       // bit 0 = has main0_rect
    uint32_t cbCount;
    uint32_t srvBufCount;
    uint32_t srvTexCount;
    uint32_t samplerCount;
    uint32_t inputCount;
    uint32_t mslLength;
};

// Transform a SPIRV-Cross vertex MSL shader into one that also provides `main0_rect`
// for Xenos rectangle-list (`g_rectangleShader`) 3-vertex -> 6-vertex expansion.
static bool InjectRectVertexEntry(std::string& msl) {
    const std::string marker = "vertex main0_out main0(";
    size_t pos = msl.find(marker);
    if (pos == std::string::npos) return false;
    if (msl.find("[[stage_in]]") != std::string::npos) return false;
    if (msl.find("gl_Position") == std::string::npos) return false;

    size_t paramsStart = pos + marker.size();
    int depth = 1;
    size_t paramsEnd = paramsStart;
    while (paramsEnd < msl.size() && depth > 0) {
        if (msl[paramsEnd] == '(') depth++;
        else if (msl[paramsEnd] == ')') depth--;
        if (depth > 0) paramsEnd++;
    }
    if (paramsEnd >= msl.size()) return false;

    std::string paramStr = msl.substr(paramsStart, paramsEnd - paramsStart);

    // Split parameters by top-level comma
    std::vector<std::string> rawParams;
    {
        size_t start = 0;
        int angle = 0;
        for (size_t i = 0; i <= paramStr.size(); ++i) {
            if (i == paramStr.size() || (paramStr[i] == ',' && angle == 0)) {
                std::string tok = paramStr.substr(start, i - start);
                while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\n')) tok.erase(tok.begin());
                while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\n')) tok.pop_back();
                if (!tok.empty()) rawParams.push_back(tok);
                start = i + 1;
            } else if (paramStr[i] == '<') {
                angle++;
            } else if (paramStr[i] == '>') {
                angle--;
            }
        }
    }

    struct ParsedParam {
        std::string fullWithAttr;
        std::string declNoAttr;
        std::string varName;
        bool isVertexId = false;
    };
    std::vector<ParsedParam> parsed;
    bool foundVertexId = false;

    for (const std::string& p : rawParams) {
        ParsedParam pp;
        pp.fullWithAttr = p;
        if (p.find("[[vertex_id]]") != std::string::npos) {
            pp.isVertexId = true;
            foundVertexId = true;
        }
        size_t attrPos = p.find("[[");
        std::string clean = (attrPos != std::string::npos) ? p.substr(0, attrPos) : p;
        while (!clean.empty() && clean.back() == ' ') clean.pop_back();
        pp.declNoAttr = clean;

        size_t lastSpace = clean.find_last_of(" &*");
        pp.varName = (lastSpace != std::string::npos) ? clean.substr(lastSpace + 1) : clean;
        parsed.push_back(pp);
    }

    if (!foundVertexId) {
        ParsedParam vp;
        vp.fullWithAttr = "uint gl_VertexIndex [[vertex_id]]";
        vp.declNoAttr = "uint gl_VertexIndex";
        vp.varName = "gl_VertexIndex";
        vp.isVertexId = true;
        parsed.push_back(vp);
    }

    std::string implDecls;
    std::string origParams;
    std::string callArgsOrig;
    std::string callArgs0, callArgs1, callArgs2;

    for (size_t i = 0; i < parsed.size(); ++i) {
        if (i > 0) {
            implDecls += ", ";
            origParams += ", ";
            callArgsOrig += ", ";
            callArgs0 += ", ";
            callArgs1 += ", ";
            callArgs2 += ", ";
        }
        implDecls += parsed[i].declNoAttr;
        origParams += parsed[i].fullWithAttr;
        callArgsOrig += parsed[i].varName;
        callArgs0 += parsed[i].isVertexId ? "i0" : parsed[i].varName;
        callArgs1 += parsed[i].isVertexId ? "i1" : parsed[i].varName;
        callArgs2 += parsed[i].isVertexId ? "i2" : parsed[i].varName;
    }

    std::string rectParams = origParams;
    if (!rectParams.empty()) rectParams += ", ";
    rectParams += "const device uint* spvRectIndices [[buffer(29)]]";

    // Replace `vertex main0_out main0(<paramStr>)` with `static inline main0_out main0_impl(<implDecls>)`
    msl.replace(pos, (paramsEnd + 1) - pos, "static inline main0_out main0_impl(" + implDecls + ")");

    // Append `main0` and `main0_rect`
    std::string extra = "\nvertex main0_out main0(" + origParams + ")\n{\n    return main0_impl(" + callArgsOrig + ");\n}\n\n";
    extra += "vertex main0_out main0_rect(" + rectParams + ")\n{\n";
    extra += "    uint rect = gl_VertexIndex / 6u;\n";
    extra += "    uint corner = gl_VertexIndex % 6u;\n";
    extra += "    uint i0 = spvRectIndices[rect * 3u + 0u];\n";
    extra += "    uint i1 = spvRectIndices[rect * 3u + 1u];\n";
    extra += "    uint i2 = spvRectIndices[rect * 3u + 2u];\n";
    extra += "    main0_out v0 = main0_impl(" + callArgs0 + ");\n";
    extra += "    main0_out v1 = main0_impl(" + callArgs1 + ");\n";
    extra += "    main0_out v2 = main0_impl(" + callArgs2 + ");\n";
    extra += "    float2 p0 = v0.gl_Position.xy / v0.gl_Position.w;\n";
    extra += "    float2 p1 = v1.gl_Position.xy / v1.gl_Position.w;\n";
    extra += "    float2 p2 = v2.gl_Position.xy / v2.gl_Position.w;\n";
    extra += "    float d01 = dot(p1 - p0, p1 - p0), d12 = dot(p2 - p1, p2 - p1), d02 = dot(p2 - p0, p2 - p0);\n";
    extra += "    uint cIdx = (d02 >= d01 && d02 >= d12) ? 1u : ((d01 >= d12) ? 2u : 0u);\n";
    extra += "    main0_out vc = (cIdx == 0u) ? v0 : ((cIdx == 1u) ? v1 : v2);\n";
    extra += "    main0_out va = (cIdx == 0u) ? v1 : ((cIdx == 1u) ? v2 : v0);\n";
    extra += "    main0_out vb = (cIdx == 0u) ? v2 : ((cIdx == 1u) ? v0 : v1);\n";
    extra += "    if (corner == 0u) return vc;\n";
    extra += "    if (corner == 1u || corner == 4u) return va;\n";
    extra += "    if (corner == 2u || corner == 3u) return vb;\n";
    extra += "    main0_out fourth = {};\n";
    extra += "    fourth.gl_Position = va.gl_Position + vb.gl_Position - vc.gl_Position;\n";
    for (int i = 0; i < 16; ++i) {
        std::string field = "_entryPointOutput_o" + std::to_string(i);
        if (msl.find(field + " ") != std::string::npos) {
            extra += "    fourth." + field + " = va." + field + " + vb." + field + " - vc." + field + ";\n";
        }
    }
    extra += "    return fourth;\n}\n";
    msl += extra;
    return true;
}

// ============================================================================
// Format Conversion Helpers
// ============================================================================
static MTLPixelFormat ToMtlPixelFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_B4G4R4A4_UNORM:
            return MTLPixelFormatRGBA8Unorm;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return MTLPixelFormatRGBA8Unorm_sRGB;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
            return MTLPixelFormatBGRA8Unorm;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return MTLPixelFormatBGRA8Unorm_sRGB;
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_A8_UNORM:
            return MTLPixelFormatR8Unorm;
        case DXGI_FORMAT_R8_SNORM:
            return MTLPixelFormatR8Snorm;
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8_UNORM:
            return MTLPixelFormatRG8Unorm;
        case DXGI_FORMAT_R8G8_SNORM:
            return MTLPixelFormatRG8Snorm;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_UNORM:
            return MTLPixelFormatR16Unorm;
        case DXGI_FORMAT_R16_SNORM:
            return MTLPixelFormatR16Snorm;
        case DXGI_FORMAT_R16_FLOAT:
            return MTLPixelFormatR16Float;
        case DXGI_FORMAT_D16_UNORM:
            return MTLPixelFormatDepth16Unorm;
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_UNORM:
            return MTLPixelFormatRG16Unorm;
        case DXGI_FORMAT_R16G16_SNORM:
            return MTLPixelFormatRG16Snorm;
        case DXGI_FORMAT_R16G16_FLOAT:
            return MTLPixelFormatRG16Float;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
            return MTLPixelFormatRGBA16Unorm;
        case DXGI_FORMAT_R16G16B16A16_SNORM:
            return MTLPixelFormatRGBA16Snorm;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return MTLPixelFormatRGBA16Float;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT:
            return MTLPixelFormatR32Float;
        case DXGI_FORMAT_D32_FLOAT:
            return MTLPixelFormatDepth32Float;
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:
            return MTLPixelFormatRG32Float;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return MTLPixelFormatRGBA32Float;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return MTLPixelFormatRGB10A2Unorm;
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return MTLPixelFormatRG11B10Float;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
            return MTLPixelFormatDepth32Float_Stencil8;
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
            return MTLPixelFormatBC1_RGBA;
        case DXGI_FORMAT_BC1_UNORM_SRGB:
            return MTLPixelFormatBC1_RGBA_sRGB;
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
            return MTLPixelFormatBC2_RGBA;
        case DXGI_FORMAT_BC2_UNORM_SRGB:
            return MTLPixelFormatBC2_RGBA_sRGB;
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
            return MTLPixelFormatBC3_RGBA;
        case DXGI_FORMAT_BC3_UNORM_SRGB:
            return MTLPixelFormatBC3_RGBA_sRGB;
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
            return MTLPixelFormatBC4_RUnorm;
        case DXGI_FORMAT_BC4_SNORM:
            return MTLPixelFormatBC4_RSnorm;
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
            return MTLPixelFormatBC5_RGUnorm;
        case DXGI_FORMAT_BC5_SNORM:
            return MTLPixelFormatBC5_RGSnorm;
        default:
            return MTLPixelFormatRGBA8Unorm;
    }
}

static bool IsBlockCompressed(DXGI_FORMAT fmt, uint32_t& blockBytes) {
    switch (fmt) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            blockBytes = 8;
            return true;
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
            blockBytes = 16;
            return true;
        default:
            blockBytes = 0;
            return false;
    }
}

static uint32_t BytesPerPixel(MTLPixelFormat fmt) {
    switch (fmt) {
        case MTLPixelFormatR8Unorm:
        case MTLPixelFormatR8Snorm:
            return 1;
        case MTLPixelFormatRG8Unorm:
        case MTLPixelFormatRG8Snorm:
        case MTLPixelFormatR16Unorm:
        case MTLPixelFormatR16Snorm:
        case MTLPixelFormatR16Float:
        case MTLPixelFormatDepth16Unorm:
            return 2;
        case MTLPixelFormatRGBA8Unorm:
        case MTLPixelFormatRGBA8Unorm_sRGB:
        case MTLPixelFormatBGRA8Unorm:
        case MTLPixelFormatBGRA8Unorm_sRGB:
        case MTLPixelFormatRG16Unorm:
        case MTLPixelFormatRG16Snorm:
        case MTLPixelFormatRG16Float:
        case MTLPixelFormatR32Float:
        case MTLPixelFormatDepth32Float:
        case MTLPixelFormatRGB10A2Unorm:
        case MTLPixelFormatRG11B10Float:
            return 4;
        case MTLPixelFormatRGBA16Unorm:
        case MTLPixelFormatRGBA16Snorm:
        case MTLPixelFormatRGBA16Float:
        case MTLPixelFormatRG32Float:
        case MTLPixelFormatDepth32Float_Stencil8:
            return 8;
        case MTLPixelFormatRGBA32Float:
            return 16;
        default:
            return 4;
    }
}

static MTLCompareFunction ToMtlCompareFunc(D3D11_COMPARISON_FUNC fn) {
    switch (fn) {
        case D3D11_COMPARISON_NEVER:         return MTLCompareFunctionNever;
        case D3D11_COMPARISON_LESS:          return MTLCompareFunctionLess;
        case D3D11_COMPARISON_EQUAL:         return MTLCompareFunctionEqual;
        case D3D11_COMPARISON_LESS_EQUAL:    return MTLCompareFunctionLessEqual;
        case D3D11_COMPARISON_GREATER:       return MTLCompareFunctionGreater;
        case D3D11_COMPARISON_NOT_EQUAL:     return MTLCompareFunctionNotEqual;
        case D3D11_COMPARISON_GREATER_EQUAL: return MTLCompareFunctionGreaterEqual;
        case D3D11_COMPARISON_ALWAYS:
        default:                             return MTLCompareFunctionAlways;
    }
}

static MTLStencilOperation ToMtlStencilOp(D3D11_STENCIL_OP op) {
    switch (op) {
        case D3D11_STENCIL_OP_KEEP:     return MTLStencilOperationKeep;
        case D3D11_STENCIL_OP_ZERO:     return MTLStencilOperationZero;
        case D3D11_STENCIL_OP_REPLACE:  return MTLStencilOperationReplace;
        case D3D11_STENCIL_OP_INCR_SAT: return MTLStencilOperationIncrementClamp;
        case D3D11_STENCIL_OP_DECR_SAT: return MTLStencilOperationDecrementClamp;
        case D3D11_STENCIL_OP_INVERT:   return MTLStencilOperationInvert;
        case D3D11_STENCIL_OP_INCR:     return MTLStencilOperationIncrementWrap;
        case D3D11_STENCIL_OP_DECR:     return MTLStencilOperationDecrementWrap;
        default:                        return MTLStencilOperationKeep;
    }
}

static MTLBlendFactor ToMtlBlendFactor(D3D11_BLEND b, bool isAlpha) {
    switch (b) {
        case D3D11_BLEND_ZERO:             return MTLBlendFactorZero;
        case D3D11_BLEND_ONE:              return MTLBlendFactorOne;
        case D3D11_BLEND_SRC_COLOR:        return isAlpha ? MTLBlendFactorSourceAlpha : MTLBlendFactorSourceColor;
        case D3D11_BLEND_INV_SRC_COLOR:    return isAlpha ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOneMinusSourceColor;
        case D3D11_BLEND_SRC_ALPHA:        return MTLBlendFactorSourceAlpha;
        case D3D11_BLEND_INV_SRC_ALPHA:    return MTLBlendFactorOneMinusSourceAlpha;
        case D3D11_BLEND_DEST_ALPHA:       return MTLBlendFactorDestinationAlpha;
        case D3D11_BLEND_INV_DEST_ALPHA:   return MTLBlendFactorOneMinusDestinationAlpha;
        case D3D11_BLEND_DEST_COLOR:       return isAlpha ? MTLBlendFactorDestinationAlpha : MTLBlendFactorDestinationColor;
        case D3D11_BLEND_INV_DEST_COLOR:   return isAlpha ? MTLBlendFactorOneMinusDestinationAlpha : MTLBlendFactorOneMinusDestinationColor;
        case D3D11_BLEND_SRC_ALPHA_SAT:    return MTLBlendFactorSourceAlphaSaturated;
        case D3D11_BLEND_BLEND_FACTOR:     return isAlpha ? MTLBlendFactorBlendAlpha : MTLBlendFactorBlendColor;
        case D3D11_BLEND_INV_BLEND_FACTOR: return isAlpha ? MTLBlendFactorOneMinusBlendAlpha : MTLBlendFactorOneMinusBlendColor;
        case D3D11_BLEND_SRC1_COLOR:       return isAlpha ? MTLBlendFactorSource1Alpha : MTLBlendFactorSource1Color;
        case D3D11_BLEND_INV_SRC1_COLOR:   return isAlpha ? MTLBlendFactorOneMinusSource1Alpha : MTLBlendFactorOneMinusSource1Color;
        case D3D11_BLEND_SRC1_ALPHA:       return MTLBlendFactorSource1Alpha;
        case D3D11_BLEND_INV_SRC1_ALPHA:   return MTLBlendFactorOneMinusSource1Alpha;
        default:                           return MTLBlendFactorOne;
    }
}

static MTLBlendOperation ToMtlBlendOp(D3D11_BLEND_OP op) {
    switch (op) {
        case D3D11_BLEND_OP_ADD:          return MTLBlendOperationAdd;
        case D3D11_BLEND_OP_SUBTRACT:     return MTLBlendOperationSubtract;
        case D3D11_BLEND_OP_REV_SUBTRACT: return MTLBlendOperationReverseSubtract;
        case D3D11_BLEND_OP_MIN:          return MTLBlendOperationMin;
        case D3D11_BLEND_OP_MAX:          return MTLBlendOperationMax;
        default:                          return MTLBlendOperationAdd;
    }
}

static MTLSamplerAddressMode ToMtlAddressMode(D3D11_TEXTURE_ADDRESS_MODE m) {
    switch (m) {
        case D3D11_TEXTURE_ADDRESS_WRAP:        return MTLSamplerAddressModeRepeat;
        case D3D11_TEXTURE_ADDRESS_MIRROR:      return MTLSamplerAddressModeMirrorRepeat;
        case D3D11_TEXTURE_ADDRESS_CLAMP:       return MTLSamplerAddressModeClampToEdge;
        case D3D11_TEXTURE_ADDRESS_BORDER:      return MTLSamplerAddressModeClampToBorderColor;
        case D3D11_TEXTURE_ADDRESS_MIRROR_ONCE: return MTLSamplerAddressModeMirrorClampToEdge;
        default:                                return MTLSamplerAddressModeClampToEdge;
    }
}

static MTLVertexFormat ToMtlVertexFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R32_FLOAT:          return MTLVertexFormatFloat;
        case DXGI_FORMAT_R32G32_FLOAT:       return MTLVertexFormatFloat2;
        case DXGI_FORMAT_R32G32B32_FLOAT:    return MTLVertexFormatFloat3;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return MTLVertexFormatFloat4;
        case DXGI_FORMAT_R8G8B8A8_UNORM:     return MTLVertexFormatUChar4Normalized;
        case DXGI_FORMAT_B8G8R8A8_UNORM:     return MTLVertexFormatUChar4Normalized_BGRA;
        case DXGI_FORMAT_R16G16_FLOAT:       return MTLVertexFormatHalf2;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return MTLVertexFormatHalf4;
        case DXGI_FORMAT_R16G16_SNORM:       return MTLVertexFormatShort2Normalized;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return MTLVertexFormatShort4Normalized;
        case DXGI_FORMAT_R16G16_UNORM:       return MTLVertexFormatUShort2Normalized;
        case DXGI_FORMAT_R16G16B16A16_UNORM: return MTLVertexFormatUShort4Normalized;
        default:                             return MTLVertexFormatFloat4;
    }
}

// ============================================================================
// Resource & State Implementations
// ============================================================================
struct Cod3Buffer final : public ID3D11Buffer {
    D3D11_BUFFER_DESC desc{};
    id<MTLDevice> mtlDevice = nil;
    id<MTLBuffer> mtlBuffer = nil;
    std::vector<uint8_t> cpuData;

    Cod3Buffer(id<MTLDevice> dev, const D3D11_BUFFER_DESC& d, const D3D11_SUBRESOURCE_DATA* init)
        : desc(d), mtlDevice(dev) {
        size_t sz = std::max<size_t>(d.ByteWidth, 16);
        cpuData.resize(sz, 0);
        if (init && init->pSysMem) {
            std::memcpy(cpuData.data(), init->pSysMem, d.ByteWidth);
            mtlBuffer = [dev newBufferWithBytes:cpuData.data() length:sz options:MTLResourceStorageModeShared];
        } else {
            mtlBuffer = [dev newBufferWithLength:sz options:MTLResourceStorageModeShared];
        }
    }

    void SyncFromCpu(bool allocateFreshBuffer) {
        size_t sz = std::max<size_t>(desc.ByteWidth, 16);
        if (allocateFreshBuffer || !mtlBuffer) {
            mtlBuffer = [mtlDevice newBufferWithBytes:cpuData.data() length:sz options:MTLResourceStorageModeShared];
        } else {
            std::memcpy([mtlBuffer contents], cpuData.data(), desc.ByteWidth);
        }
    }

    void GetDesc(D3D11_BUFFER_DESC* pDesc) override {
        if (pDesc) *pDesc = desc;
    }
};

struct Cod3Texture2D final : public ID3D11Texture2D {
    D3D11_TEXTURE2D_DESC desc{};
    MTLPixelFormat mtlFormat = MTLPixelFormatRGBA8Unorm;
    ID3D11Device* d3dDevice = nullptr;
    id<MTLDevice> mtlDevice = nil;
    id<MTLTexture> mtlTexture = nil;
    id<MTLBuffer> stagingBuffer = nil;
    uint32_t stagingRowPitch = 0;

    void GetDevice(ID3D11Device** ppDevice) override {
        if (!ppDevice) return;
        if (d3dDevice) d3dDevice->AddRef();
        *ppDevice = d3dDevice;
    }

    Cod3Texture2D(id<MTLDevice> dev, const D3D11_TEXTURE2D_DESC& d, const D3D11_SUBRESOURCE_DATA* init)
        : desc(d), mtlDevice(dev) {
        if (desc.MipLevels == 0) desc.MipLevels = 1;
        if (desc.ArraySize == 0) desc.ArraySize = 1;
        if (desc.SampleDesc.Count == 0) desc.SampleDesc.Count = 1;
        mtlFormat = ToMtlPixelFormat(desc.Format);

        if (desc.Usage == D3D11_USAGE_STAGING) {
            uint32_t bpp = BytesPerPixel(mtlFormat);
            stagingRowPitch = (desc.Width * bpp + 255u) & ~255u;
            size_t totalBytes = size_t(stagingRowPitch) * std::max<uint32_t>(1u, desc.Height);
            stagingBuffer = [dev newBufferWithLength:std::max<size_t>(totalBytes, 64) options:MTLResourceStorageModeShared];
            return;
        }

        MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
        const bool isCube = (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0 && desc.ArraySize == 6;
        const bool isMsaa = desc.SampleDesc.Count > 1;
        const bool isDepth = (mtlFormat == MTLPixelFormatDepth32Float_Stencil8 ||
                              mtlFormat == MTLPixelFormatDepth32Float ||
                              mtlFormat == MTLPixelFormatDepth16Unorm);

        td.textureType = isCube ? MTLTextureTypeCube : (isMsaa ? MTLTextureType2DMultisample : MTLTextureType2D);
        td.pixelFormat = mtlFormat;
        td.width = std::max<uint32_t>(1u, desc.Width);
        td.height = std::max<uint32_t>(1u, desc.Height);
        td.mipmapLevelCount = isMsaa ? 1 : std::max<uint32_t>(1u, desc.MipLevels);
        td.sampleCount = isMsaa ? desc.SampleDesc.Count : 1;
        td.arrayLength = 1;

        MTLTextureUsage usage = MTLTextureUsageShaderRead;
        if ((desc.BindFlags & D3D11_BIND_RENDER_TARGET) || (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) || isMsaa || isDepth) {
            usage |= MTLTextureUsageRenderTarget;
        }
        td.usage = usage;
        td.storageMode = (isMsaa || isDepth) ? MTLStorageModePrivate : MTLStorageModeShared;

        mtlTexture = [dev newTextureWithDescriptor:td];

        if (init && mtlTexture && td.storageMode == MTLStorageModeShared) {
            UploadInitialData(init);
        }
    }

    void UploadInitialData(const D3D11_SUBRESOURCE_DATA* init) {
        uint32_t blockBytes = 0;
        const bool compressed = IsBlockCompressed(desc.Format, blockBytes);
        const uint32_t faces = ((desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) && desc.ArraySize == 6) ? 6u : 1u;

        for (uint32_t slice = 0; slice < faces; ++slice) {
            for (uint32_t mip = 0; mip < desc.MipLevels; ++mip) {
                const D3D11_SUBRESOURCE_DATA& sub = init[slice * desc.MipLevels + mip];
                if (!sub.pSysMem) continue;
                uint32_t w = std::max<uint32_t>(1u, desc.Width >> mip);
                uint32_t h = std::max<uint32_t>(1u, desc.Height >> mip);
                MTLRegion region = MTLRegionMake2D(0, 0, w, h);

                if (desc.Format == DXGI_FORMAT_B5G6R5_UNORM ||
                    desc.Format == DXGI_FORMAT_B5G5R5A1_UNORM ||
                    desc.Format == DXGI_FORMAT_B4G4R4A4_UNORM) {
                    std::vector<uint8_t> rgba(size_t(w) * h * 4);
                    for (uint32_t y = 0; y < h; ++y) {
                        const uint16_t* srcRow = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(sub.pSysMem) + size_t(y) * sub.SysMemPitch);
                        uint8_t* dstRow = rgba.data() + size_t(y) * w * 4;
                        for (uint32_t x = 0; x < w; ++x) {
                            uint16_t px = srcRow[x];
                            if (desc.Format == DXGI_FORMAT_B5G6R5_UNORM) {
                                uint8_t b = uint8_t(((px >> 0) & 0x1F) * 255 / 31);
                                uint8_t g = uint8_t(((px >> 5) & 0x3F) * 255 / 63);
                                uint8_t r = uint8_t(((px >> 11) & 0x1F) * 255 / 31);
                                dstRow[x * 4 + 0] = r;
                                dstRow[x * 4 + 1] = g;
                                dstRow[x * 4 + 2] = b;
                                dstRow[x * 4 + 3] = 255;
                            } else if (desc.Format == DXGI_FORMAT_B5G5R5A1_UNORM) {
                                uint8_t b = uint8_t(((px >> 0) & 0x1F) * 255 / 31);
                                uint8_t g = uint8_t(((px >> 5) & 0x1F) * 255 / 31);
                                uint8_t r = uint8_t(((px >> 10) & 0x1F) * 255 / 31);
                                uint8_t a = (px & 0x8000) ? 255 : 0;
                                dstRow[x * 4 + 0] = r;
                                dstRow[x * 4 + 1] = g;
                                dstRow[x * 4 + 2] = b;
                                dstRow[x * 4 + 3] = a;
                            } else {
                                uint8_t b = uint8_t(((px >> 0) & 0xF) * 17);
                                uint8_t g = uint8_t(((px >> 4) & 0xF) * 17);
                                uint8_t r = uint8_t(((px >> 8) & 0xF) * 17);
                                uint8_t a = uint8_t(((px >> 12) & 0xF) * 17);
                                dstRow[x * 4 + 0] = r;
                                dstRow[x * 4 + 1] = g;
                                dstRow[x * 4 + 2] = b;
                                dstRow[x * 4 + 3] = a;
                            }
                        }
                    }
                    [mtlTexture replaceRegion:region mipmapLevel:mip slice:slice withBytes:rgba.data() bytesPerRow:w * 4 bytesPerImage:rgba.size()];
                } else {
                    uint32_t pitch = sub.SysMemPitch;
                    if (pitch == 0) {
                        pitch = compressed ? std::max<uint32_t>(1u, (w + 3) / 4) * blockBytes : w * BytesPerPixel(mtlFormat);
                    }
                    uint32_t rows = compressed ? std::max<uint32_t>(1u, (h + 3) / 4) : h;
                    [mtlTexture replaceRegion:region mipmapLevel:mip slice:slice withBytes:sub.pSysMem bytesPerRow:pitch bytesPerImage:pitch * rows];
                }
            }
        }
    }

    void GetDesc(D3D11_TEXTURE2D_DESC* pDesc) override {
        if (pDesc) *pDesc = desc;
    }
};

struct Cod3Texture1D final : public ID3D11Texture1D {
    D3D11_TEXTURE1D_DESC desc{};
    MTLPixelFormat mtlFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLDevice> mtlDevice = nil;
    id<MTLTexture> mtlTexture = nil;

    Cod3Texture1D(id<MTLDevice> dev, const D3D11_TEXTURE1D_DESC& d, const D3D11_SUBRESOURCE_DATA* init)
        : desc(d), mtlDevice(dev) {
        if (desc.MipLevels == 0) desc.MipLevels = 1;
        if (desc.ArraySize == 0) desc.ArraySize = 1;
        mtlFormat = ToMtlPixelFormat(desc.Format);

        MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
        // Backed by a Width x 1 2D texture so MSL sample(..., level(0)) works cleanly
        td.textureType = MTLTextureType2D;
        td.pixelFormat = mtlFormat;
        td.width = std::max<uint32_t>(1u, desc.Width);
        td.height = 1;
        td.mipmapLevelCount = std::max<uint32_t>(1u, desc.MipLevels);
        td.sampleCount = 1;
        td.arrayLength = 1;
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;

        mtlTexture = [dev newTextureWithDescriptor:td];
        if (init && init->pSysMem && mtlTexture) {
            uint32_t bpp = BytesPerPixel(mtlFormat);
            [mtlTexture replaceRegion:MTLRegionMake2D(0, 0, td.width, 1)
                          mipmapLevel:0
                            withBytes:init->pSysMem
                          bytesPerRow:td.width * bpp];
        }
    }

    void GetDesc(D3D11_TEXTURE1D_DESC* pDesc) override {
        if (pDesc) *pDesc = desc;
    }
};

struct Cod3Texture3D final : public ID3D11Texture3D {
    D3D11_TEXTURE3D_DESC desc{};
    MTLPixelFormat mtlFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLDevice> mtlDevice = nil;
    id<MTLTexture> mtlTexture = nil;

    Cod3Texture3D(id<MTLDevice> dev, const D3D11_TEXTURE3D_DESC& d, const D3D11_SUBRESOURCE_DATA* init)
        : desc(d), mtlDevice(dev) {
        if (desc.MipLevels == 0) desc.MipLevels = 1;
        mtlFormat = ToMtlPixelFormat(desc.Format);

        MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
        td.textureType = MTLTextureType3D;
        td.pixelFormat = mtlFormat;
        td.width = std::max<uint32_t>(1u, desc.Width);
        td.height = std::max<uint32_t>(1u, desc.Height);
        td.depth = std::max<uint32_t>(1u, desc.Depth);
        td.mipmapLevelCount = std::max<uint32_t>(1u, desc.MipLevels);
        td.sampleCount = 1;
        td.arrayLength = 1;
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;

        mtlTexture = [dev newTextureWithDescriptor:td];
        if (init && mtlTexture) {
            uint32_t blockBytes = 0;
            const bool compressed = IsBlockCompressed(desc.Format, blockBytes);
            for (uint32_t mip = 0; mip < desc.MipLevels; ++mip) {
                const D3D11_SUBRESOURCE_DATA& sub = init[mip];
                if (!sub.pSysMem) continue;
                uint32_t w = std::max<uint32_t>(1u, desc.Width >> mip);
                uint32_t h = std::max<uint32_t>(1u, desc.Height >> mip);
                uint32_t dp = std::max<uint32_t>(1u, desc.Depth >> mip);
                uint32_t pitch = sub.SysMemPitch ? sub.SysMemPitch : (compressed ? std::max<uint32_t>(1u, (w + 3) / 4) * blockBytes : w * BytesPerPixel(mtlFormat));
                uint32_t rows = compressed ? std::max<uint32_t>(1u, (h + 3) / 4) : h;
                uint32_t slicePitch = sub.SysMemSlicePitch ? sub.SysMemSlicePitch : pitch * rows;
                [mtlTexture replaceRegion:MTLRegionMake3D(0, 0, 0, w, h, dp)
                              mipmapLevel:mip
                                    slice:0
                                withBytes:sub.pSysMem
                              bytesPerRow:pitch
                            bytesPerImage:slicePitch];
            }
        }
    }

    void GetDesc(D3D11_TEXTURE3D_DESC* pDesc) override {
        if (pDesc) *pDesc = desc;
    }
};

struct Cod3ShaderResourceView final : public ID3D11ShaderResourceView {
    ID3D11Resource* resource = nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    Cod3Buffer* buffer = nullptr;
    Cod3Texture2D* texture = nullptr;
    Cod3Texture1D* texture1D = nullptr;
    Cod3Texture3D* texture3D = nullptr;
    id<MTLTexture> mtlTexture = nil;
    size_t bufferByteOffset = 0;

    Cod3ShaderResourceView(ID3D11Resource* res, const D3D11_SHADER_RESOURCE_VIEW_DESC* d)
        : resource(res) {
        if (resource) resource->AddRef();
        buffer = dynamic_cast<Cod3Buffer*>(res);
        texture = dynamic_cast<Cod3Texture2D*>(res);
        texture1D = dynamic_cast<Cod3Texture1D*>(res);
        texture3D = dynamic_cast<Cod3Texture3D*>(res);
        if (texture) mtlTexture = texture->mtlTexture;
        else if (texture1D) mtlTexture = texture1D->mtlTexture;
        else if (texture3D) mtlTexture = texture3D->mtlTexture;

        if (d) {
            desc = *d;
        } else if (texture) {
            desc.Format = texture->desc.Format;
            if ((texture->desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) && texture->desc.ArraySize == 6) {
                desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            } else if (texture->desc.SampleDesc.Count > 1) {
                desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
            } else {
                desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            }
        } else if (texture1D) {
            desc.Format = texture1D->desc.Format;
            desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
        } else if (texture3D) {
            desc.Format = texture3D->desc.Format;
            desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        }

        if (buffer && d) {
            if (d->ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX) {
                bufferByteOffset = size_t(d->BufferEx.FirstElement) * 4;
            } else if (d->ViewDimension == D3D11_SRV_DIMENSION_BUFFER) {
                bufferByteOffset = size_t(d->Buffer.FirstElement) * 4;
            }
        }
    }

    ~Cod3ShaderResourceView() override {
        if (resource) resource->Release();
    }

    void GetResource(ID3D11Resource** ppResource) override {
        if (!ppResource) return;
        if (resource) resource->AddRef();
        *ppResource = resource;
    }

    void GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc) override {
        if (pDesc) *pDesc = desc;
    }
};

struct Cod3RenderTargetView final : public ID3D11RenderTargetView {
    ID3D11Resource* resource = nullptr;
    Cod3Texture2D* texture = nullptr;
    UINT mipSlice = 0;

    Cod3RenderTargetView(ID3D11Resource* res, const D3D11_RENDER_TARGET_VIEW_DESC* d)
        : resource(res) {
        if (resource) resource->AddRef();
        texture = dynamic_cast<Cod3Texture2D*>(res);
        if (d && d->ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) {
            mipSlice = d->Texture2D.MipSlice;
        }
    }

    ~Cod3RenderTargetView() override {
        if (resource) resource->Release();
    }

    void GetResource(ID3D11Resource** ppResource) override {
        if (!ppResource) return;
        if (resource) resource->AddRef();
        *ppResource = resource;
    }
};

struct Cod3DepthStencilView final : public ID3D11DepthStencilView {
    ID3D11Resource* resource = nullptr;
    Cod3Texture2D* texture = nullptr;

    Cod3DepthStencilView(ID3D11Resource* res, const D3D11_DEPTH_STENCIL_VIEW_DESC*)
        : resource(res) {
        if (resource) resource->AddRef();
        texture = dynamic_cast<Cod3Texture2D*>(res);
    }

    ~Cod3DepthStencilView() override {
        if (resource) resource->Release();
    }

    void GetResource(ID3D11Resource** ppResource) override {
        if (!ppResource) return;
        if (resource) resource->AddRef();
        *ppResource = resource;
    }
};

struct ParsedShaderData {
    uint32_t stage = 0;
    uint32_t flags = 0;
    std::vector<ShaderBindingPair> cbBindings;
    std::vector<ShaderBindingPair> srvBufBindings;
    std::vector<ShaderBindingPair> srvTexBindings;
    std::vector<ShaderBindingPair> samplerBindings;
    std::vector<ShaderInputBinding> inputBindings;
    std::string mslSource;
    id<MTLLibrary> library = nil;
    id<MTLFunction> mainFunc = nil;
    id<MTLFunction> rectFunc = nil;

    bool ParseAndCompile(id<MTLDevice> dev, const void* bytecode, size_t length) {
        if (!bytecode || length < sizeof(ShaderBlobHeader)) return false;
        const auto* hdr = static_cast<const ShaderBlobHeader*>(bytecode);
        if (hdr->magic != kShaderMagic || hdr->version != kShaderVersion) return false;

        size_t expected = sizeof(ShaderBlobHeader) +
            size_t(hdr->cbCount + hdr->srvBufCount + hdr->srvTexCount + hdr->samplerCount) * sizeof(ShaderBindingPair) +
            size_t(hdr->inputCount) * sizeof(ShaderInputBinding) +
            hdr->mslLength;
        if (length < expected) return false;

        stage = hdr->stage;
        flags = hdr->flags;
        const uint8_t* ptr = static_cast<const uint8_t*>(bytecode) + sizeof(ShaderBlobHeader);

        auto readPairs = [&](uint32_t count, std::vector<ShaderBindingPair>& out) {
            out.resize(count);
            if (count > 0) {
                std::memcpy(out.data(), ptr, count * sizeof(ShaderBindingPair));
                ptr += count * sizeof(ShaderBindingPair);
            }
        };
        readPairs(hdr->cbCount, cbBindings);
        readPairs(hdr->srvBufCount, srvBufBindings);
        readPairs(hdr->srvTexCount, srvTexBindings);
        readPairs(hdr->samplerCount, samplerBindings);

        inputBindings.resize(hdr->inputCount);
        if (hdr->inputCount > 0) {
            std::memcpy(inputBindings.data(), ptr, hdr->inputCount * sizeof(ShaderInputBinding));
            ptr += hdr->inputCount * sizeof(ShaderInputBinding);
        }

        if (hdr->mslLength > 0) {
            mslSource.assign(reinterpret_cast<const char*>(ptr), hdr->mslLength - 1);
        }

        if (stage == 3) {
            return true; // Geometry shader handled via vertex rect expansion
        }

        NSError* err = nil;
        MTLCompileOptions* opts = [[MTLCompileOptions alloc] init];
        if (@available(macOS 15.0, *)) {
            opts.mathMode = MTLMathModeSafe;
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            opts.fastMathEnabled = NO;
#pragma clang diagnostic pop
        }
        NSString* src = [NSString stringWithUTF8String:mslSource.c_str()];
        library = [dev newLibraryWithSource:src options:opts error:&err];
        if (!library) {
            fprintf(stderr, "d3d11_macos: Metal shader compile failed: %s\n",
                    err ? [[err localizedDescription] UTF8String] : "unknown");
            return false;
        }
        mainFunc = [library newFunctionWithName:@"main0"];
        if (!mainFunc) {
            NSArray<NSString*>* names = [library functionNames];
            if ([names count] > 0) mainFunc = [library newFunctionWithName:names[0]];
        }
        if (flags & 1u) {
            rectFunc = [library newFunctionWithName:@"main0_rect"];
        }
        return mainFunc != nil;
    }
};

struct Cod3VertexShader final : public ID3D11VertexShader {
    ParsedShaderData data;
};

struct Cod3PixelShader final : public ID3D11PixelShader {
    ParsedShaderData data;
};

struct Cod3GeometryShader final : public ID3D11GeometryShader {
    ParsedShaderData data;
};

struct Cod3InputLayout final : public ID3D11InputLayout {
    std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
    std::vector<std::string> semanticStrings;
    std::vector<ShaderInputBinding> shaderInputs;

    Cod3InputLayout(const D3D11_INPUT_ELEMENT_DESC* descs, UINT count, const void* bytecode, SIZE_T len) {
        elements.assign(descs, descs + count);
        semanticStrings.resize(count);
        for (UINT i = 0; i < count; ++i) {
            semanticStrings[i] = descs[i].SemanticName ? descs[i].SemanticName : "";
            elements[i].SemanticName = semanticStrings[i].c_str();
        }
        if (bytecode && len >= sizeof(ShaderBlobHeader)) {
            const auto* hdr = static_cast<const ShaderBlobHeader*>(bytecode);
            if (hdr->magic == kShaderMagic && hdr->version == kShaderVersion) {
                const uint8_t* ptr = static_cast<const uint8_t*>(bytecode) + sizeof(ShaderBlobHeader) +
                    size_t(hdr->cbCount + hdr->srvBufCount + hdr->srvTexCount + hdr->samplerCount) * sizeof(ShaderBindingPair);
                shaderInputs.resize(hdr->inputCount);
                if (hdr->inputCount > 0) {
                    std::memcpy(shaderInputs.data(), ptr, hdr->inputCount * sizeof(ShaderInputBinding));
                }
            }
        }
    }
};

struct Cod3BlendState final : public ID3D11BlendState {
    D3D11_BLEND_DESC desc{};
    explicit Cod3BlendState(const D3D11_BLEND_DESC& d) : desc(d) {}
};

struct Cod3DepthStencilState final : public ID3D11DepthStencilState {
    D3D11_DEPTH_STENCIL_DESC desc{};
    id<MTLDepthStencilState> mtlState = nil;

    Cod3DepthStencilState(id<MTLDevice> dev, const D3D11_DEPTH_STENCIL_DESC& d) : desc(d) {
        MTLDepthStencilDescriptor* mdesc = [[MTLDepthStencilDescriptor alloc] init];
        mdesc.depthCompareFunction = d.DepthEnable ? ToMtlCompareFunc(d.DepthFunc) : MTLCompareFunctionAlways;
        mdesc.depthWriteEnabled = (d.DepthEnable && d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL) ? YES : NO;

        if (d.StencilEnable) {
            auto makeStencil = [&](const D3D11_DEPTH_STENCILOP_DESC& s) {
                MTLStencilDescriptor* sd = [[MTLStencilDescriptor alloc] init];
                sd.stencilCompareFunction = ToMtlCompareFunc(s.StencilFunc);
                sd.stencilFailureOperation = ToMtlStencilOp(s.StencilFailOp);
                sd.depthFailureOperation = ToMtlStencilOp(s.StencilDepthFailOp);
                sd.depthStencilPassOperation = ToMtlStencilOp(s.StencilPassOp);
                sd.readMask = d.StencilReadMask;
                sd.writeMask = d.StencilWriteMask;
                return sd;
            };
            mdesc.frontFaceStencil = makeStencil(d.FrontFace);
            mdesc.backFaceStencil = makeStencil(d.BackFace);
        }
        mtlState = [dev newDepthStencilStateWithDescriptor:mdesc];
    }
};

struct Cod3RasterizerState final : public ID3D11RasterizerState {
    D3D11_RASTERIZER_DESC desc{};
    explicit Cod3RasterizerState(const D3D11_RASTERIZER_DESC& d) : desc(d) {}
};

struct Cod3SamplerState final : public ID3D11SamplerState {
    D3D11_SAMPLER_DESC desc{};
    id<MTLSamplerState> mtlSampler = nil;

    Cod3SamplerState(id<MTLDevice> dev, const D3D11_SAMPLER_DESC& d) : desc(d) {
        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
        sd.sAddressMode = ToMtlAddressMode(d.AddressU);
        sd.tAddressMode = ToMtlAddressMode(d.AddressV);
        sd.rAddressMode = ToMtlAddressMode(d.AddressW);

        if (d.Filter == D3D11_FILTER_ANISOTROPIC) {
            sd.minFilter = MTLSamplerMinMagFilterLinear;
            sd.magFilter = MTLSamplerMinMagFilterLinear;
            sd.mipFilter = MTLSamplerMipFilterLinear;
            sd.maxAnisotropy = std::clamp<NSUInteger>(d.MaxAnisotropy, 1, 16);
        } else {
            sd.minFilter = (d.Filter & 0x10) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            sd.magFilter = (d.Filter & 0x04) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            sd.mipFilter = (d.Filter & 0x01) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
            sd.maxAnisotropy = 1;
        }
        sd.lodMinClamp = std::max(0.0f, d.MinLOD);
        sd.lodMaxClamp = d.MaxLOD;
        if (d.BorderColor[3] > 0.5f) {
            sd.borderColor = (d.BorderColor[0] > 0.5f) ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack;
        } else {
            sd.borderColor = MTLSamplerBorderColorTransparentBlack;
        }
        mtlSampler = [dev newSamplerStateWithDescriptor:sd];
    }
};

struct Cod3Query final : public ID3D11Query {
    D3D11_QUERY_DESC desc{};
    uint64_t timestampNs = 0;
    explicit Cod3Query(const D3D11_QUERY_DESC& d) : desc(d) {}
};

// ============================================================================
// Pipeline Cache Key
// ============================================================================
struct MtlPipelineKey {
    const Cod3VertexShader* vs = nullptr;
    const Cod3PixelShader* ps = nullptr;
    const Cod3InputLayout* layout = nullptr;
    const Cod3BlendState* blend = nullptr;
    uint32_t vertexStride0 = 0;
    uint16_t rtvFormats[4] = {};
    uint16_t dsvFormat = 0;
    uint8_t sampleCount = 1;
    uint8_t useRectFunc = 0;

    bool operator==(const MtlPipelineKey& o) const {
        return std::memcmp(this, &o, sizeof(MtlPipelineKey)) == 0;
    }
};

struct MtlPipelineKeyHash {
    size_t operator()(const MtlPipelineKey& k) const {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
        size_t h = 14695981039346656037ull;
        for (size_t i = 0; i < sizeof(MtlPipelineKey); ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return h;
    }
};

struct Cod3Device;
struct Cod3Factory;

// ============================================================================
// Device Context Implementation
// ============================================================================
struct Cod3DeviceContext final : public ID3D11DeviceContext1 {
    Cod3Device* parentDevice = nullptr;
    id<MTLDevice> mtlDevice = nil;
    id<MTLCommandQueue> cmdQueue = nil;
    id<MTLCommandBuffer> cmdBuffer = nil;
    id<MTLRenderCommandEncoder> renderEncoder = nil;
    id<MTLSamplerState> defaultSampler = nil;
    id<MTLTexture> dummyTexture2D = nil;
    id<MTLTexture> dummyTextureCube = nil;
    id<MTLBuffer> dummyBuffer = nil;

    // Bound IA State
    Cod3InputLayout* inputLayout = nullptr;
    Cod3Buffer* vertexBuffers[16] = {};
    UINT vertexStrides[16] = {};
    UINT vertexOffsets[16] = {};
    Cod3Buffer* indexBuffer = nullptr;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_R16_UINT;
    UINT indexOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // Bound Shaders & Resources
    Cod3VertexShader* vs = nullptr;
    Cod3PixelShader* ps = nullptr;
    Cod3GeometryShader* gs = nullptr;
    Cod3Buffer* vsCBs[16] = {};
    UINT vsCBFirstConstant[16] = {};
    UINT vsCBNumConstants[16] = {};
    Cod3Buffer* psCBs[16] = {};
    UINT psCBFirstConstant[16] = {};
    UINT psCBNumConstants[16] = {};
    Cod3ShaderResourceView* vsSRVs[64] = {};
    Cod3ShaderResourceView* psSRVs[64] = {};
    Cod3SamplerState* vsSamplers[16] = {};
    Cod3SamplerState* psSamplers[16] = {};

    // Bound OM / RS State
    Cod3RenderTargetView* rtvs[4] = {};
    UINT numRtvs = 0;
    Cod3DepthStencilView* dsv = nullptr;
    Cod3BlendState* blendState = nullptr;
    FLOAT blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    UINT sampleMask = 0xFFFFFFFFu;
    Cod3DepthStencilState* depthStencilState = nullptr;
    UINT stencilRef = 0;
    Cod3RasterizerState* rasterizerState = nullptr;
    D3D11_VIEWPORT viewports[16] = {};
    UINT numViewports = 0;
    D3D11_RECT scissorRects[16] = {};
    UINT numScissorRects = 0;

    std::unordered_map<MtlPipelineKey, id<MTLRenderPipelineState>, MtlPipelineKeyHash> pipelineCache;

    Cod3DeviceContext(Cod3Device* dev, id<MTLDevice> mdev)
        : parentDevice(dev), mtlDevice(mdev) {
        cmdQueue = [mdev newCommandQueue];
        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.mipFilter = MTLSamplerMipFilterNearest;
        defaultSampler = [mdev newSamplerStateWithDescriptor:sd];

        MTLTextureDescriptor* td2d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        dummyTexture2D = [mdev newTextureWithDescriptor:td2d];
        uint32_t zero = 0;
        [dummyTexture2D replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&zero bytesPerRow:4];

        MTLTextureDescriptor* tdcube = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm size:1 mipmapped:NO];
        dummyTextureCube = [mdev newTextureWithDescriptor:tdcube];

        dummyBuffer = [mdev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    }

    HRESULT QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_INVALIDARG;
        if (riid == __uuidof(ID3D11DeviceContext1) || riid == __uuidof(ID3D11DeviceContext)) {
            AddRef();
            *ppvObject = static_cast<ID3D11DeviceContext1*>(this);
            return S_OK;
        }
        return ID3D11DeviceContext1::QueryInterface(riid, ppvObject);
    }

    void EnsureCommandBuffer() {
        if (!cmdBuffer) {
            cmdBuffer = [cmdQueue commandBuffer];
        }
    }

    void EndRenderPass() {
        if (renderEncoder) {
            [renderEncoder endEncoding];
            renderEncoder = nil;
        }
    }

    void FlushAndWait() {
        EndRenderPass();
        if (cmdBuffer) {
            [cmdBuffer commit];
            [cmdBuffer waitUntilCompleted];
            cmdBuffer = nil;
        }
    }

    bool EnsureRenderPass() {
        if (renderEncoder) return true;
        if (numRtvs == 0 && !dsv) return false;

        EnsureCommandBuffer();
        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        bool hasAnyAttachment = false;

        for (UINT i = 0; i < numRtvs && i < 4; ++i) {
            if (rtvs[i] && rtvs[i]->texture && rtvs[i]->texture->mtlTexture) {
                rpd.colorAttachments[i].texture = rtvs[i]->texture->mtlTexture;
                rpd.colorAttachments[i].level = rtvs[i]->mipSlice;
                rpd.colorAttachments[i].loadAction = MTLLoadActionLoad;
                rpd.colorAttachments[i].storeAction = MTLStoreActionStore;
                hasAnyAttachment = true;
            }
        }

        if (dsv && dsv->texture && dsv->texture->mtlTexture) {
            rpd.depthAttachment.texture = dsv->texture->mtlTexture;
            rpd.depthAttachment.loadAction = MTLLoadActionLoad;
            rpd.depthAttachment.storeAction = MTLStoreActionStore;
            if (dsv->texture->mtlFormat == MTLPixelFormatDepth32Float_Stencil8) {
                rpd.stencilAttachment.texture = dsv->texture->mtlTexture;
                rpd.stencilAttachment.loadAction = MTLLoadActionLoad;
                rpd.stencilAttachment.storeAction = MTLStoreActionStore;
            }
            hasAnyAttachment = true;
        }

        if (!hasAnyAttachment) return false;
        renderEncoder = [cmdBuffer renderCommandEncoderWithDescriptor:rpd];
        return renderEncoder != nil;
    }

    id<MTLRenderPipelineState> GetOrCreatePipeline(bool useRectFunc) {
        if (!vs || !vs->data.mainFunc) return nil;

        MtlPipelineKey key{};
        key.vs = vs;
        key.ps = ps;
        key.layout = inputLayout;
        key.blend = blendState;
        key.vertexStride0 = inputLayout ? vertexStrides[0] : 0;
        key.useRectFunc = (useRectFunc && vs->data.rectFunc != nil) ? 1 : 0;

        uint32_t sampleCount = 1;
        for (UINT i = 0; i < numRtvs && i < 4; ++i) {
            if (rtvs[i] && rtvs[i]->texture) {
                key.rtvFormats[i] = static_cast<uint16_t>(rtvs[i]->texture->mtlFormat);
                sampleCount = std::max<uint32_t>(sampleCount, rtvs[i]->texture->desc.SampleDesc.Count);
            }
        }
        if (dsv && dsv->texture) {
            key.dsvFormat = static_cast<uint16_t>(dsv->texture->mtlFormat);
            sampleCount = std::max<uint32_t>(sampleCount, dsv->texture->desc.SampleDesc.Count);
        }
        key.sampleCount = static_cast<uint8_t>(sampleCount);

        auto it = pipelineCache.find(key);
        if (it != pipelineCache.end()) return it->second;

        MTLRenderPipelineDescriptor* pdesc = [[MTLRenderPipelineDescriptor alloc] init];
        pdesc.vertexFunction = key.useRectFunc ? vs->data.rectFunc : vs->data.mainFunc;
        pdesc.fragmentFunction = (ps && ps->data.mainFunc) ? ps->data.mainFunc : nil;
        pdesc.rasterSampleCount = sampleCount;

        if (inputLayout && !inputLayout->elements.empty()) {
            MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
            for (size_t i = 0; i < inputLayout->elements.size(); ++i) {
                const auto& el = inputLayout->elements[i];
                uint32_t loc = static_cast<uint32_t>(i);
                for (const auto& si : inputLayout->shaderInputs) {
                    if (std::strcmp(si.semanticName, el.SemanticName) == 0 && si.semanticIndex == el.SemanticIndex) {
                        loc = si.location;
                        break;
                    }
                }
                vd.attributes[loc].format = ToMtlVertexFormat(el.Format);
                vd.attributes[loc].offset = el.AlignedByteOffset;
                vd.attributes[loc].bufferIndex = 30 - el.InputSlot;
            }
            uint32_t stride = vertexStrides[0] ? vertexStrides[0] : 20;
            vd.layouts[30].stride = stride;
            vd.layouts[30].stepFunction = MTLVertexStepFunctionPerVertex;
            pdesc.vertexDescriptor = vd;
        }

        if (blendState && blendState->desc.AlphaToCoverageEnable) {
            pdesc.alphaToCoverageEnabled = YES;
        }

        for (UINT i = 0; i < 4; ++i) {
            MTLPixelFormat fmt = static_cast<MTLPixelFormat>(key.rtvFormats[i]);
            if (fmt == MTLPixelFormatInvalid) continue;
            auto* ca = pdesc.colorAttachments[i];
            ca.pixelFormat = fmt;
            if (!ps || !ps->data.mainFunc) {
                ca.writeMask = MTLColorWriteMaskNone;
            } else if (blendState) {
                const auto& rt = blendState->desc.RenderTarget[blendState->desc.IndependentBlendEnable ? i : 0];
                ca.blendingEnabled = rt.BlendEnable ? YES : NO;
                ca.sourceRGBBlendFactor = ToMtlBlendFactor(rt.SrcBlend, false);
                ca.destinationRGBBlendFactor = ToMtlBlendFactor(rt.DestBlend, false);
                ca.rgbBlendOperation = ToMtlBlendOp(rt.BlendOp);
                ca.sourceAlphaBlendFactor = ToMtlBlendFactor(rt.SrcBlendAlpha, true);
                ca.destinationAlphaBlendFactor = ToMtlBlendFactor(rt.DestBlendAlpha, true);
                ca.alphaBlendOperation = ToMtlBlendOp(rt.BlendOpAlpha);
                MTLColorWriteMask mask = MTLColorWriteMaskNone;
                if (rt.RenderTargetWriteMask & D3D11_COLOR_WRITE_ENABLE_RED)   mask |= MTLColorWriteMaskRed;
                if (rt.RenderTargetWriteMask & D3D11_COLOR_WRITE_ENABLE_GREEN) mask |= MTLColorWriteMaskGreen;
                if (rt.RenderTargetWriteMask & D3D11_COLOR_WRITE_ENABLE_BLUE)  mask |= MTLColorWriteMaskBlue;
                if (rt.RenderTargetWriteMask & D3D11_COLOR_WRITE_ENABLE_ALPHA) mask |= MTLColorWriteMaskAlpha;
                ca.writeMask = mask;
            } else {
                ca.writeMask = MTLColorWriteMaskAll;
            }
        }

        if (key.dsvFormat != 0) {
            MTLPixelFormat dfmt = static_cast<MTLPixelFormat>(key.dsvFormat);
            pdesc.depthAttachmentPixelFormat = dfmt;
            if (dfmt == MTLPixelFormatDepth32Float_Stencil8) {
                pdesc.stencilAttachmentPixelFormat = dfmt;
            }
        }

        NSError* err = nil;
        id<MTLRenderPipelineState> pso = [mtlDevice newRenderPipelineStateWithDescriptor:pdesc error:&err];
        if (!pso) {
            fprintf(stderr, "d3d11_macos: newRenderPipelineStateWithDescriptor failed: %s\n",
                    err ? [[err localizedDescription] UTF8String] : "unknown");
        }
        pipelineCache[key] = pso;
        return pso;
    }

    bool PrepareDrawState(bool useRectFunc) {
        if (!EnsureRenderPass()) return false;
        id<MTLRenderPipelineState> pso = GetOrCreatePipeline(useRectFunc);
        if (!pso) return false;

        [renderEncoder setRenderPipelineState:pso];

        if (depthStencilState && depthStencilState->mtlState && dsv) {
            [renderEncoder setDepthStencilState:depthStencilState->mtlState];
            [renderEncoder setStencilReferenceValue:stencilRef];
        }

        [renderEncoder setBlendColorRed:blendFactor[0] green:blendFactor[1] blue:blendFactor[2] alpha:blendFactor[3]];

        // Target dimensions for clamping viewport/scissor
        uint32_t targetW = 4096, targetH = 4096;
        if (numRtvs > 0 && rtvs[0] && rtvs[0]->texture) {
            targetW = std::max<uint32_t>(1u, rtvs[0]->texture->desc.Width >> rtvs[0]->mipSlice);
            targetH = std::max<uint32_t>(1u, rtvs[0]->texture->desc.Height >> rtvs[0]->mipSlice);
        } else if (dsv && dsv->texture) {
            targetW = dsv->texture->desc.Width;
            targetH = dsv->texture->desc.Height;
        }

        if (numViewports > 0) {
            MTLViewport vp;
            vp.originX = viewports[0].TopLeftX;
            vp.originY = viewports[0].TopLeftY;
            vp.width = viewports[0].Width;
            vp.height = viewports[0].Height;
            vp.znear = viewports[0].MinDepth;
            vp.zfar = viewports[0].MaxDepth;
            [renderEncoder setViewport:vp];
        }

        if (rasterizerState) {
            const auto& rd = rasterizerState->desc;
            [renderEncoder setTriangleFillMode:(rd.FillMode == D3D11_FILL_WIREFRAME) ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
            [renderEncoder setFrontFacingWinding:rd.FrontCounterClockwise ? MTLWindingCounterClockwise : MTLWindingClockwise];
            if (rd.CullMode == D3D11_CULL_NONE) [renderEncoder setCullMode:MTLCullModeNone];
            else if (rd.CullMode == D3D11_CULL_FRONT) [renderEncoder setCullMode:MTLCullModeFront];
            else [renderEncoder setCullMode:MTLCullModeBack];

            [renderEncoder setDepthBias:float(rd.DepthBias) slopeScale:rd.SlopeScaledDepthBias clamp:rd.DepthBiasClamp];
            [renderEncoder setDepthClipMode:rd.DepthClipEnable ? MTLDepthClipModeClip : MTLDepthClipModeClamp];

            if (rd.ScissorEnable && numScissorRects > 0) {
                int32_t x0 = std::clamp<int32_t>(scissorRects[0].left, 0, int32_t(targetW));
                int32_t y0 = std::clamp<int32_t>(scissorRects[0].top, 0, int32_t(targetH));
                int32_t x1 = std::clamp<int32_t>(scissorRects[0].right, x0, int32_t(targetW));
                int32_t y1 = std::clamp<int32_t>(scissorRects[0].bottom, y0, int32_t(targetH));
                if (x1 > x0 && y1 > y0) {
                    MTLScissorRect sr{ NSUInteger(x0), NSUInteger(y0), NSUInteger(x1 - x0), NSUInteger(y1 - y0) };
                    [renderEncoder setScissorRect:sr];
                }
            } else {
                MTLScissorRect sr{ 0, 0, targetW, targetH };
                [renderEncoder setScissorRect:sr];
            }
        }

        // Bind Vertex Shader resources
        if (vs) {
            for (const auto& b : vs->data.cbBindings) {
                Cod3Buffer* cb = (b.d3dSlot < 16) ? vsCBs[b.d3dSlot] : nullptr;
                size_t byteOff = (b.d3dSlot < 16) ? size_t(vsCBFirstConstant[b.d3dSlot]) * 16 : 0;
                size_t byteLen = (b.d3dSlot < 16 && vsCBNumConstants[b.d3dSlot] > 0) ? size_t(vsCBNumConstants[b.d3dSlot]) * 16 : (cb ? cb->cpuData.size() : 0);
                if (cb && byteOff < cb->cpuData.size()) {
                    size_t avail = std::min(byteLen, cb->cpuData.size() - byteOff);
                    if (avail <= 4096) {
                        [renderEncoder setVertexBytes:cb->cpuData.data() + byteOff length:avail atIndex:b.mslSlot];
                    } else if (cb->mtlBuffer) {
                        [renderEncoder setVertexBuffer:cb->mtlBuffer offset:byteOff atIndex:b.mslSlot];
                    } else {
                        [renderEncoder setVertexBuffer:dummyBuffer offset:0 atIndex:b.mslSlot];
                    }
                } else {
                    [renderEncoder setVertexBuffer:dummyBuffer offset:0 atIndex:b.mslSlot];
                }
            }
            for (const auto& b : vs->data.srvBufBindings) {
                Cod3ShaderResourceView* srv = (b.d3dSlot < 64) ? vsSRVs[b.d3dSlot] : nullptr;
                if (srv && srv->buffer && srv->buffer->mtlBuffer) {
                    [renderEncoder setVertexBuffer:srv->buffer->mtlBuffer offset:srv->bufferByteOffset atIndex:b.mslSlot];
                } else {
                    [renderEncoder setVertexBuffer:dummyBuffer offset:0 atIndex:b.mslSlot];
                }
            }
            for (const auto& b : vs->data.srvTexBindings) {
                Cod3ShaderResourceView* srv = (b.d3dSlot < 64) ? vsSRVs[b.d3dSlot] : nullptr;
                id<MTLTexture> tex = (srv && srv->mtlTexture) ? srv->mtlTexture : dummyTexture2D;
                [renderEncoder setVertexTexture:tex atIndex:b.mslSlot];
            }
            for (const auto& b : vs->data.samplerBindings) {
                Cod3SamplerState* samp = (b.d3dSlot < 16) ? vsSamplers[b.d3dSlot] : nullptr;
                [renderEncoder setVertexSamplerState:(samp && samp->mtlSampler) ? samp->mtlSampler : defaultSampler atIndex:b.mslSlot];
            }
        }

        // Bind InputLayout Vertex Buffers (e.g. ImGui)
        if (inputLayout) {
            for (UINT s = 0; s < 4; ++s) {
                if (vertexBuffers[s] && vertexBuffers[s]->mtlBuffer) {
                    [renderEncoder setVertexBuffer:vertexBuffers[s]->mtlBuffer offset:vertexOffsets[s] atIndex:(30 - s)];
                }
            }
        }

        // Bind Pixel Shader resources
        if (ps) {
            for (const auto& b : ps->data.cbBindings) {
                Cod3Buffer* cb = (b.d3dSlot < 16) ? psCBs[b.d3dSlot] : nullptr;
                size_t byteOff = (b.d3dSlot < 16) ? size_t(psCBFirstConstant[b.d3dSlot]) * 16 : 0;
                size_t byteLen = (b.d3dSlot < 16 && psCBNumConstants[b.d3dSlot] > 0) ? size_t(psCBNumConstants[b.d3dSlot]) * 16 : (cb ? cb->cpuData.size() : 0);
                if (cb && byteOff < cb->cpuData.size()) {
                    size_t avail = std::min(byteLen, cb->cpuData.size() - byteOff);
                    if (avail <= 4096) {
                        [renderEncoder setFragmentBytes:cb->cpuData.data() + byteOff length:avail atIndex:b.mslSlot];
                    } else if (cb->mtlBuffer) {
                        [renderEncoder setFragmentBuffer:cb->mtlBuffer offset:byteOff atIndex:b.mslSlot];
                    } else {
                        [renderEncoder setFragmentBuffer:dummyBuffer offset:0 atIndex:b.mslSlot];
                    }
                } else {
                    [renderEncoder setFragmentBuffer:dummyBuffer offset:0 atIndex:b.mslSlot];
                }
            }
            for (const auto& b : ps->data.srvBufBindings) {
                Cod3ShaderResourceView* srv = (b.d3dSlot < 64) ? psSRVs[b.d3dSlot] : nullptr;
                if (srv && srv->buffer && srv->buffer->mtlBuffer) {
                    [renderEncoder setFragmentBuffer:srv->buffer->mtlBuffer offset:srv->bufferByteOffset atIndex:b.mslSlot];
                } else {
                    [renderEncoder setFragmentBuffer:dummyBuffer offset:0 atIndex:b.mslSlot];
                }
            }
            for (const auto& b : ps->data.srvTexBindings) {
                Cod3ShaderResourceView* srv = (b.d3dSlot < 64) ? psSRVs[b.d3dSlot] : nullptr;
                id<MTLTexture> tex = (srv && srv->mtlTexture) ? srv->mtlTexture : dummyTexture2D;
                [renderEncoder setFragmentTexture:tex atIndex:b.mslSlot];
            }
            for (const auto& b : ps->data.samplerBindings) {
                Cod3SamplerState* samp = (b.d3dSlot < 16) ? psSamplers[b.d3dSlot] : nullptr;
                [renderEncoder setFragmentSamplerState:(samp && samp->mtlSampler) ? samp->mtlSampler : defaultSampler atIndex:b.mslSlot];
            }
        }
        return true;
    }

    MTLPrimitiveType CurrentMtlPrimitiveType() const {
        switch (topology) {
            case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:     return MTLPrimitiveTypePoint;
            case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:      return MTLPrimitiveTypeLine;
            case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:     return MTLPrimitiveTypeLineStrip;
            case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return MTLPrimitiveTypeTriangleStrip;
            case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
            default:                                     return MTLPrimitiveTypeTriangle;
        }
    }

    // --- ID3D11DeviceContext methods ---
    void VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override {
        VSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, nullptr, nullptr);
    }

    void PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override {
        PSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, nullptr, nullptr);
    }

    void VSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override {
        for (UINT i = 0; i < NumBuffers && (StartSlot + i) < 16; ++i) {
            Cod3Buffer* next = ppConstantBuffers ? static_cast<Cod3Buffer*>(ppConstantBuffers[i]) : nullptr;
            if (next) next->AddRef();
            if (vsCBs[StartSlot + i]) vsCBs[StartSlot + i]->Release();
            vsCBs[StartSlot + i] = next;
            vsCBFirstConstant[StartSlot + i] = pFirstConstant ? pFirstConstant[i] : 0;
            vsCBNumConstants[StartSlot + i] = pNumConstants ? pNumConstants[i] : 0;
        }
    }

    void PSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override {
        for (UINT i = 0; i < NumBuffers && (StartSlot + i) < 16; ++i) {
            Cod3Buffer* next = ppConstantBuffers ? static_cast<Cod3Buffer*>(ppConstantBuffers[i]) : nullptr;
            if (next) next->AddRef();
            if (psCBs[StartSlot + i]) psCBs[StartSlot + i]->Release();
            psCBs[StartSlot + i] = next;
            psCBFirstConstant[StartSlot + i] = pFirstConstant ? pFirstConstant[i] : 0;
            psCBNumConstants[StartSlot + i] = pNumConstants ? pNumConstants[i] : 0;
        }
    }

    void VSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override {
        for (UINT i = 0; i < NumViews && (StartSlot + i) < 64; ++i) {
            Cod3ShaderResourceView* next = ppShaderResourceViews ? static_cast<Cod3ShaderResourceView*>(ppShaderResourceViews[i]) : nullptr;
            if (next) next->AddRef();
            if (vsSRVs[StartSlot + i]) vsSRVs[StartSlot + i]->Release();
            vsSRVs[StartSlot + i] = next;
        }
    }

    void PSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override {
        for (UINT i = 0; i < NumViews && (StartSlot + i) < 64; ++i) {
            Cod3ShaderResourceView* next = ppShaderResourceViews ? static_cast<Cod3ShaderResourceView*>(ppShaderResourceViews[i]) : nullptr;
            if (next) next->AddRef();
            if (psSRVs[StartSlot + i]) psSRVs[StartSlot + i]->Release();
            psSRVs[StartSlot + i] = next;
        }
    }

    void VSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override {
        for (UINT i = 0; i < NumSamplers && (StartSlot + i) < 16; ++i) {
            Cod3SamplerState* next = ppSamplers ? static_cast<Cod3SamplerState*>(ppSamplers[i]) : nullptr;
            if (next) next->AddRef();
            if (vsSamplers[StartSlot + i]) vsSamplers[StartSlot + i]->Release();
            vsSamplers[StartSlot + i] = next;
        }
    }

    void PSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override {
        for (UINT i = 0; i < NumSamplers && (StartSlot + i) < 16; ++i) {
            Cod3SamplerState* next = ppSamplers ? static_cast<Cod3SamplerState*>(ppSamplers[i]) : nullptr;
            if (next) next->AddRef();
            if (psSamplers[StartSlot + i]) psSamplers[StartSlot + i]->Release();
            psSamplers[StartSlot + i] = next;
        }
    }

    void VSSetShader(ID3D11VertexShader* pVertexShader, ID3D11ClassInstance* const*, UINT) override {
        auto* next = static_cast<Cod3VertexShader*>(pVertexShader);
        if (next) next->AddRef();
        if (vs) vs->Release();
        vs = next;
    }

    void PSSetShader(ID3D11PixelShader* pPixelShader, ID3D11ClassInstance* const*, UINT) override {
        auto* next = static_cast<Cod3PixelShader*>(pPixelShader);
        if (next) next->AddRef();
        if (ps) ps->Release();
        ps = next;
    }

    void GSSetShader(ID3D11GeometryShader* pShader, ID3D11ClassInstance* const*, UINT) override {
        auto* next = static_cast<Cod3GeometryShader*>(pShader);
        if (next) next->AddRef();
        if (gs) gs->Release();
        gs = next;
    }

    void HSSetShader(ID3D11HullShader*, ID3D11ClassInstance* const*, UINT) override {}
    void DSSetShader(ID3D11DomainShader*, ID3D11ClassInstance* const*, UINT) override {}
    void CSSetShader(ID3D11ComputeShader*, ID3D11ClassInstance* const*, UINT) override {}

    void IASetInputLayout(ID3D11InputLayout* pInputLayout) override {
        auto* next = static_cast<Cod3InputLayout*>(pInputLayout);
        if (next) next->AddRef();
        if (inputLayout) inputLayout->Release();
        inputLayout = next;
    }

    void IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppVertexBuffers, const UINT* pStrides, const UINT* pOffsets) override {
        for (UINT i = 0; i < NumBuffers && (StartSlot + i) < 16; ++i) {
            Cod3Buffer* next = ppVertexBuffers ? static_cast<Cod3Buffer*>(ppVertexBuffers[i]) : nullptr;
            if (next) next->AddRef();
            if (vertexBuffers[StartSlot + i]) vertexBuffers[StartSlot + i]->Release();
            vertexBuffers[StartSlot + i] = next;
            vertexStrides[StartSlot + i] = pStrides ? pStrides[i] : 0;
            vertexOffsets[StartSlot + i] = pOffsets ? pOffsets[i] : 0;
        }
    }

    void IASetIndexBuffer(ID3D11Buffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset) override {
        auto* next = static_cast<Cod3Buffer*>(pIndexBuffer);
        if (next) next->AddRef();
        if (indexBuffer) indexBuffer->Release();
        indexBuffer = next;
        indexFormat = Format;
        indexOffset = Offset;
    }

    void IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) override {
        topology = Topology;
    }

    void OMSetRenderTargets(UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView) override {
        EndRenderPass();
        for (UINT i = 0; i < 4; ++i) {
            Cod3RenderTargetView* next = (i < NumViews && ppRenderTargetViews) ? static_cast<Cod3RenderTargetView*>(ppRenderTargetViews[i]) : nullptr;
            if (next) next->AddRef();
            if (rtvs[i]) rtvs[i]->Release();
            rtvs[i] = next;
        }
        numRtvs = std::min<UINT>(NumViews, 4);
        auto* nextDsv = static_cast<Cod3DepthStencilView*>(pDepthStencilView);
        if (nextDsv) nextDsv->AddRef();
        if (dsv) dsv->Release();
        dsv = nextDsv;
    }

    void OMSetBlendState(ID3D11BlendState* pBlendState, const FLOAT BlendFactor[4], UINT SampleMask) override {
        auto* next = static_cast<Cod3BlendState*>(pBlendState);
        if (next) next->AddRef();
        if (blendState) blendState->Release();
        blendState = next;
        if (BlendFactor) std::memcpy(blendFactor, BlendFactor, sizeof(blendFactor));
        else { blendFactor[0] = blendFactor[1] = blendFactor[2] = blendFactor[3] = 1.0f; }
        sampleMask = SampleMask;
    }

    void OMSetDepthStencilState(ID3D11DepthStencilState* pDepthStencilState, UINT StencilRef) override {
        auto* next = static_cast<Cod3DepthStencilState*>(pDepthStencilState);
        if (next) next->AddRef();
        if (depthStencilState) depthStencilState->Release();
        depthStencilState = next;
        stencilRef = StencilRef;
    }

    void RSSetState(ID3D11RasterizerState* pRasterizerState) override {
        auto* next = static_cast<Cod3RasterizerState*>(pRasterizerState);
        if (next) next->AddRef();
        if (rasterizerState) rasterizerState->Release();
        rasterizerState = next;
    }

    void RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports) override {
        numViewports = std::min<UINT>(NumViewports, 16);
        if (pViewports && numViewports > 0) {
            std::memcpy(viewports, pViewports, numViewports * sizeof(D3D11_VIEWPORT));
        }
    }

    void RSSetScissorRects(UINT NumRects, const D3D11_RECT* pRects) override {
        numScissorRects = std::min<UINT>(NumRects, 16);
        if (pRects && numScissorRects > 0) {
            std::memcpy(scissorRects, pRects, numScissorRects * sizeof(D3D11_RECT));
        }
    }

    void Draw(UINT VertexCount, UINT StartVertexLocation) override {
        if (VertexCount == 0) return;
        const bool useRect = (gs != nullptr && vs && vs->data.rectFunc != nil && VertexCount >= 3);
        if (!PrepareDrawState(useRect)) return;

        if (useRect) {
            uint32_t rectCount = VertexCount / 3;
            std::vector<uint32_t> rectIndices(rectCount * 3);
            for (uint32_t i = 0; i < rectCount * 3; ++i) {
                rectIndices[i] = StartVertexLocation + i;
            }
            if (rectIndices.size() * sizeof(uint32_t) <= 4096) {
                [renderEncoder setVertexBytes:rectIndices.data() length:rectIndices.size() * sizeof(uint32_t) atIndex:29];
            } else {
                id<MTLBuffer> idxBuf = [mtlDevice newBufferWithBytes:rectIndices.data() length:rectIndices.size() * sizeof(uint32_t) options:MTLResourceStorageModeShared];
                [renderEncoder setVertexBuffer:idxBuf offset:0 atIndex:29];
            }
            [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:rectCount * 6];
        } else {
            [renderEncoder drawPrimitives:CurrentMtlPrimitiveType() vertexStart:StartVertexLocation vertexCount:VertexCount];
        }
    }

    void DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) override {
        if (IndexCount == 0 || !indexBuffer || !indexBuffer->mtlBuffer) return;
        const bool useRect = (gs != nullptr && vs && vs->data.rectFunc != nil && IndexCount >= 3);
        if (!PrepareDrawState(useRect)) return;

        const bool is32 = (indexFormat == DXGI_FORMAT_R32_UINT);
        const size_t elemSize = is32 ? 4 : 2;
        const size_t byteOff = indexOffset + size_t(StartIndexLocation) * elemSize;

        if (useRect) {
            uint32_t rectCount = IndexCount / 3;
            std::vector<uint32_t> rectIndices(rectCount * 3);
            const uint8_t* raw = indexBuffer->cpuData.data() + byteOff;
            for (uint32_t i = 0; i < rectCount * 3; ++i) {
                uint32_t idx = is32 ? reinterpret_cast<const uint32_t*>(raw)[i] : reinterpret_cast<const uint16_t*>(raw)[i];
                rectIndices[i] = uint32_t(int32_t(idx) + BaseVertexLocation);
            }
            if (rectIndices.size() * sizeof(uint32_t) <= 4096) {
                [renderEncoder setVertexBytes:rectIndices.data() length:rectIndices.size() * sizeof(uint32_t) atIndex:29];
            } else {
                id<MTLBuffer> idxBuf = [mtlDevice newBufferWithBytes:rectIndices.data() length:rectIndices.size() * sizeof(uint32_t) options:MTLResourceStorageModeShared];
                [renderEncoder setVertexBuffer:idxBuf offset:0 atIndex:29];
            }
            [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:rectCount * 6];
        } else {
            [renderEncoder drawIndexedPrimitives:CurrentMtlPrimitiveType()
                                      indexCount:IndexCount
                                       indexType:is32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                                     indexBuffer:indexBuffer->mtlBuffer
                               indexBufferOffset:byteOff
                                   instanceCount:1
                                      baseVertex:BaseVertexLocation
                                    baseInstance:0];
        }
    }

    HRESULT Map(ID3D11Resource* pResource, UINT, D3D11_MAP MapType, UINT, D3D11_MAPPED_SUBRESOURCE* pMappedResource) override {
        if (!pResource || !pMappedResource) return E_INVALIDARG;
        if (auto* buf = dynamic_cast<Cod3Buffer*>(pResource)) {
            if (MapType == D3D11_MAP_READ) FlushAndWait();
            pMappedResource->pData = buf->cpuData.data();
            pMappedResource->RowPitch = buf->desc.ByteWidth;
            pMappedResource->DepthPitch = buf->desc.ByteWidth;
            return S_OK;
        }
        if (auto* tex = dynamic_cast<Cod3Texture2D*>(pResource)) {
            if (tex->stagingBuffer) {
                FlushAndWait();
                pMappedResource->pData = [tex->stagingBuffer contents];
                pMappedResource->RowPitch = tex->stagingRowPitch;
                pMappedResource->DepthPitch = tex->stagingRowPitch * tex->desc.Height;
                return S_OK;
            }
        }
        return E_FAIL;
    }

    void Unmap(ID3D11Resource* pResource, UINT) override {
        if (auto* buf = dynamic_cast<Cod3Buffer*>(pResource)) {
            buf->SyncFromCpu(true);
        }
    }

    void UpdateSubresource(ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) override {
        UpdateSubresource1(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch, 0);
    }

    void UpdateSubresource1(ID3D11Resource* pDstResource, UINT, const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT, UINT CopyFlags) override {
        if (!pDstResource || !pSrcData) return;
        if (auto* buf = dynamic_cast<Cod3Buffer*>(pDstResource)) {
            size_t offset = pDstBox ? pDstBox->left : 0;
            size_t length = pDstBox ? (pDstBox->right - pDstBox->left) : buf->desc.ByteWidth;
            if (offset + length <= buf->cpuData.size()) {
                std::memcpy(buf->cpuData.data() + offset, pSrcData, length);
                buf->SyncFromCpu((CopyFlags & D3D11_COPY_DISCARD) != 0 || (buf->desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0);
            }
            return;
        }
        if (auto* tex = dynamic_cast<Cod3Texture2D*>(pDstResource)) {
            if (tex->mtlTexture && tex->mtlTexture.storageMode == MTLStorageModeShared) {
                uint32_t x = pDstBox ? pDstBox->left : 0;
                uint32_t y = pDstBox ? pDstBox->top : 0;
                uint32_t w = pDstBox ? (pDstBox->right - pDstBox->left) : tex->desc.Width;
                uint32_t h = pDstBox ? (pDstBox->bottom - pDstBox->top) : tex->desc.Height;
                [tex->mtlTexture replaceRegion:MTLRegionMake2D(x, y, w, h) mipmapLevel:0 withBytes:pSrcData bytesPerRow:SrcRowPitch];
            }
            return;
        }
        if (auto* tex1 = dynamic_cast<Cod3Texture1D*>(pDstResource)) {
            if (tex1->mtlTexture) {
                uint32_t x = pDstBox ? pDstBox->left : 0;
                uint32_t w = pDstBox ? (pDstBox->right - pDstBox->left) : tex1->desc.Width;
                uint32_t pitch = SrcRowPitch ? SrcRowPitch : w * BytesPerPixel(tex1->mtlFormat);
                [tex1->mtlTexture replaceRegion:MTLRegionMake2D(x, 0, w, 1) mipmapLevel:0 withBytes:pSrcData bytesPerRow:pitch];
            }
        }
    }

    void ClearRenderTargetView(ID3D11RenderTargetView* pRenderTargetView, const FLOAT ColorRGBA[4]) override {
        auto* rtv = static_cast<Cod3RenderTargetView*>(pRenderTargetView);
        if (!rtv || !rtv->texture || !rtv->texture->mtlTexture) return;
        EndRenderPass();
        EnsureCommandBuffer();
        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = rtv->texture->mtlTexture;
        rpd.colorAttachments[0].level = rtv->mipSlice;
        rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        rpd.colorAttachments[0].clearColor = MTLClearColorMake(ColorRGBA[0], ColorRGBA[1], ColorRGBA[2], ColorRGBA[3]);
        id<MTLRenderCommandEncoder> enc = [cmdBuffer renderCommandEncoderWithDescriptor:rpd];
        [enc endEncoding];
    }

    void ClearView(ID3D11View* pView, const FLOAT Color[4], const D3D11_RECT* pRect, UINT NumRects) override {
        auto* rtv = dynamic_cast<Cod3RenderTargetView*>(pView);
        if (!rtv) return;
        if (!pRect || NumRects == 0) {
            ClearRenderTargetView(rtv, Color);
            return;
        }
        // If clearing the full target rect, use ClearRenderTargetView
        if (rtv->texture && NumRects == 1 &&
            pRect[0].left <= 0 && pRect[0].top <= 0 &&
            uint32_t(pRect[0].right) >= rtv->texture->desc.Width &&
            uint32_t(pRect[0].bottom) >= rtv->texture->desc.Height) {
            ClearRenderTargetView(rtv, Color);
            return;
        }
        ClearRenderTargetView(rtv, Color);
    }

    void GenerateMips(ID3D11ShaderResourceView* pShaderResourceView) override {
        auto* srv = static_cast<Cod3ShaderResourceView*>(pShaderResourceView);
        if (!srv || !srv->mtlTexture || srv->mtlTexture.mipmapLevelCount <= 1) return;
        EndRenderPass();
        EnsureCommandBuffer();
        id<MTLBlitCommandEncoder> blit = [cmdBuffer blitCommandEncoder];
        [blit generateMipmapsForTexture:srv->mtlTexture];
        [blit endEncoding];
    }

    void ClearDepthStencilView(ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, BYTE Stencil) override {
        auto* v = static_cast<Cod3DepthStencilView*>(pDepthStencilView);
        if (!v || !v->texture || !v->texture->mtlTexture) return;
        EndRenderPass();
        EnsureCommandBuffer();
        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        if (ClearFlags & D3D11_CLEAR_DEPTH) {
            rpd.depthAttachment.texture = v->texture->mtlTexture;
            rpd.depthAttachment.loadAction = MTLLoadActionClear;
            rpd.depthAttachment.storeAction = MTLStoreActionStore;
            rpd.depthAttachment.clearDepth = Depth;
        }
        if ((ClearFlags & D3D11_CLEAR_STENCIL) && v->texture->mtlFormat == MTLPixelFormatDepth32Float_Stencil8) {
            rpd.stencilAttachment.texture = v->texture->mtlTexture;
            rpd.stencilAttachment.loadAction = MTLLoadActionClear;
            rpd.stencilAttachment.storeAction = MTLStoreActionStore;
            rpd.stencilAttachment.clearStencil = Stencil;
        }
        id<MTLRenderCommandEncoder> enc = [cmdBuffer renderCommandEncoderWithDescriptor:rpd];
        [enc endEncoding];
    }

    void CopySubresourceRegion(ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT, ID3D11Resource* pSrcResource, UINT SrcSubresource, const D3D11_BOX* pSrcBox) override {
        if (!pDstResource || !pSrcResource) return;
        EndRenderPass();
        EnsureCommandBuffer();

        auto* dstBuf = dynamic_cast<Cod3Buffer*>(pDstResource);
        auto* srcBuf = dynamic_cast<Cod3Buffer*>(pSrcResource);
        if (dstBuf && srcBuf) {
            size_t srcOff = pSrcBox ? pSrcBox->left : 0;
            size_t len = pSrcBox ? (pSrcBox->right - pSrcBox->left) : std::min(dstBuf->desc.ByteWidth, srcBuf->desc.ByteWidth);
            if (DstX + len <= dstBuf->cpuData.size() && srcOff + len <= srcBuf->cpuData.size()) {
                std::memcpy(dstBuf->cpuData.data() + DstX, srcBuf->cpuData.data() + srcOff, len);
            }
            id<MTLBlitCommandEncoder> blit = [cmdBuffer blitCommandEncoder];
            [blit copyFromBuffer:srcBuf->mtlBuffer sourceOffset:srcOff toBuffer:dstBuf->mtlBuffer destinationOffset:DstX size:len];
            [blit endEncoding];
            return;
        }

        auto* dstTex = dynamic_cast<Cod3Texture2D*>(pDstResource);
        auto* srcTex = dynamic_cast<Cod3Texture2D*>(pSrcResource);
        if (!dstTex || !srcTex || !srcTex->mtlTexture) return;

        uint32_t srcMip = SrcSubresource % std::max<uint32_t>(1u, srcTex->desc.MipLevels);
        uint32_t srcSlice = SrcSubresource / std::max<uint32_t>(1u, srcTex->desc.MipLevels);
        uint32_t dstMip = DstSubresource % std::max<uint32_t>(1u, dstTex->desc.MipLevels);
        uint32_t dstSlice = DstSubresource / std::max<uint32_t>(1u, dstTex->desc.MipLevels);

        uint32_t srcX = pSrcBox ? pSrcBox->left : 0;
        uint32_t srcY = pSrcBox ? pSrcBox->top : 0;
        uint32_t w = pSrcBox ? (pSrcBox->right - pSrcBox->left) : std::max<uint32_t>(1u, srcTex->desc.Width >> srcMip);
        uint32_t h = pSrcBox ? (pSrcBox->bottom - pSrcBox->top) : std::max<uint32_t>(1u, srcTex->desc.Height >> srcMip);

        id<MTLBlitCommandEncoder> blit = [cmdBuffer blitCommandEncoder];
        if (dstTex->stagingBuffer) {
            [blit copyFromTexture:srcTex->mtlTexture
                      sourceSlice:srcSlice
                      sourceLevel:srcMip
                     sourceOrigin:MTLOriginMake(srcX, srcY, 0)
                       sourceSize:MTLSizeMake(w, h, 1)
                         toBuffer:dstTex->stagingBuffer
                destinationOffset:0
           destinationBytesPerRow:dstTex->stagingRowPitch
         destinationBytesPerImage:dstTex->stagingRowPitch * h];
        } else if (dstTex->mtlTexture) {
            [blit copyFromTexture:srcTex->mtlTexture
                      sourceSlice:srcSlice
                      sourceLevel:srcMip
                     sourceOrigin:MTLOriginMake(srcX, srcY, 0)
                       sourceSize:MTLSizeMake(w, h, 1)
                        toTexture:dstTex->mtlTexture
                 destinationSlice:dstSlice
                 destinationLevel:dstMip
                destinationOrigin:MTLOriginMake(DstX, DstY, 0)];
        }
        [blit endEncoding];
    }

    void CopyResource(ID3D11Resource* pDstResource, ID3D11Resource* pSrcResource) override {
        CopySubresourceRegion(pDstResource, 0, 0, 0, 0, pSrcResource, 0, nullptr);
    }

    void ResolveSubresource(ID3D11Resource* pDstResource, UINT DstSubresource, ID3D11Resource* pSrcResource, UINT, DXGI_FORMAT) override {
        auto* dstTex = dynamic_cast<Cod3Texture2D*>(pDstResource);
        auto* srcTex = dynamic_cast<Cod3Texture2D*>(pSrcResource);
        if (!dstTex || !srcTex || !dstTex->mtlTexture || !srcTex->mtlTexture) return;

        if (srcTex->desc.SampleDesc.Count <= 1) {
            CopySubresourceRegion(pDstResource, DstSubresource, 0, 0, 0, pSrcResource, 0, nullptr);
            return;
        }

        EndRenderPass();
        EnsureCommandBuffer();
        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = srcTex->mtlTexture;
        rpd.colorAttachments[0].resolveTexture = dstTex->mtlTexture;
        rpd.colorAttachments[0].resolveLevel = DstSubresource % std::max<uint32_t>(1u, dstTex->desc.MipLevels);
        rpd.colorAttachments[0].resolveSlice = DstSubresource / std::max<uint32_t>(1u, dstTex->desc.MipLevels);
        rpd.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
        id<MTLRenderCommandEncoder> enc = [cmdBuffer renderCommandEncoderWithDescriptor:rpd];
        [enc endEncoding];
    }

    void Begin(ID3D11Asynchronous*) override {}

    void End(ID3D11Asynchronous* pAsync) override {
        if (auto* q = dynamic_cast<Cod3Query*>(pAsync)) {
            static mach_timebase_info_data_t tb = []() {
                mach_timebase_info_data_t info{};
                mach_timebase_info(&info);
                return info;
            }();
            q->timestampNs = (mach_absolute_time() * tb.numer) / tb.denom;
        }
    }

    HRESULT GetData(ID3D11Asynchronous* pAsync, void* pData, UINT DataSize, UINT) override {
        auto* q = dynamic_cast<Cod3Query*>(pAsync);
        if (!q || !pData) return E_INVALIDARG;
        if (q->desc.Query == D3D11_QUERY_TIMESTAMP_DISJOINT && DataSize >= sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT)) {
            auto* out = static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT*>(pData);
            out->Frequency = 1000000000ULL;
            out->Disjoint = FALSE;
            return S_OK;
        }
        if (q->desc.Query == D3D11_QUERY_TIMESTAMP && DataSize >= sizeof(UINT64)) {
            *static_cast<UINT64*>(pData) = q->timestampNs;
            return S_OK;
        }
        return S_OK;
    }

    // --- State Getters ---
    void VSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override {
        if (!ppConstantBuffers) return;
        for (UINT i = 0; i < NumBuffers; ++i) {
            ID3D11Buffer* b = ((StartSlot + i) < 16) ? vsCBs[StartSlot + i] : nullptr;
            if (b) b->AddRef();
            ppConstantBuffers[i] = b;
        }
    }

    void PSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override {
        if (!ppShaderResourceViews) return;
        for (UINT i = 0; i < NumViews; ++i) {
            ID3D11ShaderResourceView* v = ((StartSlot + i) < 64) ? psSRVs[StartSlot + i] : nullptr;
            if (v) v->AddRef();
            ppShaderResourceViews[i] = v;
        }
    }

    void PSGetShader(ID3D11PixelShader** ppPixelShader, ID3D11ClassInstance**, UINT* pNumClassInstances) override {
        if (pNumClassInstances) *pNumClassInstances = 0;
        if (ppPixelShader) {
            if (ps) ps->AddRef();
            *ppPixelShader = ps;
        }
    }

    void PSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override {
        if (!ppSamplers) return;
        for (UINT i = 0; i < NumSamplers; ++i) {
            ID3D11SamplerState* s = ((StartSlot + i) < 16) ? psSamplers[StartSlot + i] : nullptr;
            if (s) s->AddRef();
            ppSamplers[i] = s;
        }
    }

    void VSGetShader(ID3D11VertexShader** ppVertexShader, ID3D11ClassInstance**, UINT* pNumClassInstances) override {
        if (pNumClassInstances) *pNumClassInstances = 0;
        if (ppVertexShader) {
            if (vs) vs->AddRef();
            *ppVertexShader = vs;
        }
    }

    void GSGetShader(ID3D11GeometryShader** ppGeometryShader, ID3D11ClassInstance**, UINT* pNumClassInstances) override {
        if (pNumClassInstances) *pNumClassInstances = 0;
        if (ppGeometryShader) {
            if (gs) gs->AddRef();
            *ppGeometryShader = gs;
        }
    }

    void IAGetInputLayout(ID3D11InputLayout** ppInputLayout) override {
        if (ppInputLayout) {
            if (inputLayout) inputLayout->AddRef();
            *ppInputLayout = inputLayout;
        }
    }

    void IAGetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppVertexBuffers, UINT* pStrides, UINT* pOffsets) override {
        for (UINT i = 0; i < NumBuffers; ++i) {
            UINT slot = StartSlot + i;
            ID3D11Buffer* b = (slot < 16) ? vertexBuffers[slot] : nullptr;
            if (ppVertexBuffers) {
                if (b) b->AddRef();
                ppVertexBuffers[i] = b;
            }
            if (pStrides) pStrides[i] = (slot < 16) ? vertexStrides[slot] : 0;
            if (pOffsets) pOffsets[i] = (slot < 16) ? vertexOffsets[slot] : 0;
        }
    }

    void IAGetIndexBuffer(ID3D11Buffer** pIndexBuffer, DXGI_FORMAT* Format, UINT* Offset) override {
        if (pIndexBuffer) {
            if (indexBuffer) indexBuffer->AddRef();
            *pIndexBuffer = indexBuffer;
        }
        if (Format) *Format = indexFormat;
        if (Offset) *Offset = indexOffset;
    }

    void IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* pTopology) override {
        if (pTopology) *pTopology = topology;
    }

    void OMGetBlendState(ID3D11BlendState** ppBlendState, FLOAT BlendFactor[4], UINT* pSampleMask) override {
        if (ppBlendState) {
            if (blendState) blendState->AddRef();
            *ppBlendState = blendState;
        }
        if (BlendFactor) std::memcpy(BlendFactor, blendFactor, sizeof(blendFactor));
        if (pSampleMask) *pSampleMask = sampleMask;
    }

    void OMGetDepthStencilState(ID3D11DepthStencilState** ppDepthStencilState, UINT* pStencilRef) override {
        if (ppDepthStencilState) {
            if (depthStencilState) depthStencilState->AddRef();
            *ppDepthStencilState = depthStencilState;
        }
        if (pStencilRef) *pStencilRef = stencilRef;
    }

    void RSGetState(ID3D11RasterizerState** ppRasterizerState) override {
        if (ppRasterizerState) {
            if (rasterizerState) rasterizerState->AddRef();
            *ppRasterizerState = rasterizerState;
        }
    }

    void RSGetViewports(UINT* pNumViewports, D3D11_VIEWPORT* pViewports) override {
        if (!pNumViewports) return;
        if (pViewports) {
            UINT n = std::min(*pNumViewports, numViewports);
            std::memcpy(pViewports, viewports, n * sizeof(D3D11_VIEWPORT));
        }
        *pNumViewports = numViewports;
    }

    void RSGetScissorRects(UINT* pNumRects, D3D11_RECT* pRects) override {
        if (!pNumRects) return;
        if (pRects) {
            UINT n = std::min(*pNumRects, numScissorRects);
            std::memcpy(pRects, scissorRects, n * sizeof(D3D11_RECT));
        }
        *pNumRects = numScissorRects;
    }
};

// ============================================================================
// DXGI Adapter, Device, Factory, SwapChain & D3D11Device
// ============================================================================
struct Cod3Adapter final : public IDXGIAdapter1 {
    IDXGIObject* parentFactory = nullptr;
    id<MTLDevice> mtlDevice = nil;

    Cod3Adapter(IDXGIObject* factory, id<MTLDevice> dev) : parentFactory(factory), mtlDevice(dev) {}

    HRESULT GetParent(REFIID riid, void** ppParent) override {
        if (!ppParent) return E_INVALIDARG;
        if (parentFactory) return parentFactory->QueryInterface(riid, ppParent);
        return CreateDXGIFactory1(riid, ppParent);
    }

    HRESULT GetDesc(DXGI_ADAPTER_DESC* pDesc) override {
        if (!pDesc) return E_INVALIDARG;
        DXGI_ADAPTER_DESC1 d1{};
        GetDesc1(&d1);
        std::memcpy(pDesc->Description, d1.Description, sizeof(d1.Description));
        pDesc->VendorId = d1.VendorId;
        pDesc->DeviceId = d1.DeviceId;
        pDesc->SubSysId = d1.SubSysId;
        pDesc->Revision = d1.Revision;
        pDesc->DedicatedVideoMemory = d1.DedicatedVideoMemory;
        pDesc->DedicatedSystemMemory = d1.DedicatedSystemMemory;
        pDesc->SharedSystemMemory = d1.SharedSystemMemory;
        pDesc->AdapterLuid = d1.AdapterLuid;
        return S_OK;
    }

    HRESULT GetDesc1(DXGI_ADAPTER_DESC1* pDesc) override {
        if (!pDesc) return E_INVALIDARG;
        std::memset(pDesc, 0, sizeof(*pDesc));
        const char* name = mtlDevice ? [[mtlDevice name] UTF8String] : "Apple Metal GPU";
        for (size_t i = 0; i < 127 && name[i] != '\0'; ++i) {
            pDesc->Description[i] = static_cast<wchar_t>(static_cast<unsigned char>(name[i]));
        }
        pDesc->VendorId = 0x106B; // Apple
        pDesc->DedicatedVideoMemory = mtlDevice ? [mtlDevice recommendedMaxWorkingSetSize] : (8ULL << 30);
        pDesc->Flags = DXGI_ADAPTER_FLAG_NONE;
        return S_OK;
    }
};

struct Cod3Device final : public ID3D11Device, public IDXGIDevice {
    id<MTLDevice> mtlDevice = nil;
    Cod3Adapter* adapter = nullptr;
    Cod3DeviceContext* immediateContext = nullptr;

    Cod3Device(id<MTLDevice> dev, Cod3Adapter* adp) : mtlDevice(dev), adapter(adp) {
        if (adapter) adapter->AddRef();
        immediateContext = new Cod3DeviceContext(this, dev);
    }

    ~Cod3Device() override {
        if (immediateContext) immediateContext->Release();
        if (adapter) adapter->Release();
    }

    uint32_t AddRef() override { return ID3D11Device::AddRef(); }
    uint32_t Release() override { return ID3D11Device::Release(); }

    HRESULT QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_INVALIDARG;
        if (riid == __uuidof(ID3D11InfoQueue)) {
            *ppvObject = nullptr;
            return E_NOINTERFACE;
        }
        if (riid == __uuidof(IDXGIDevice) || riid == __uuidof(IDXGIObject)) {
            AddRef();
            *ppvObject = static_cast<IDXGIDevice*>(this);
            return S_OK;
        }
        AddRef();
        *ppvObject = static_cast<ID3D11Device*>(this);
        return S_OK;
    }

    HRESULT GetParent(REFIID riid, void** ppParent) override {
        if (!ppParent || !adapter) return E_INVALIDARG;
        return adapter->QueryInterface(riid, ppParent);
    }

    HRESULT GetAdapter(IDXGIAdapter** pAdapter) override {
        if (!pAdapter || !adapter) return E_INVALIDARG;
        adapter->AddRef();
        *pAdapter = adapter;
        return S_OK;
    }

    HRESULT CreateBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer) override {
        if (!pDesc || !ppBuffer) return E_INVALIDARG;
        *ppBuffer = new Cod3Buffer(mtlDevice, *pDesc, pInitialData);
        return S_OK;
    }

    HRESULT CreateTexture1D(const D3D11_TEXTURE1D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture1D** ppTexture1D) override {
        if (!pDesc || !ppTexture1D) return E_INVALIDARG;
        auto* tex = new Cod3Texture1D(mtlDevice, *pDesc, pInitialData);
        if (!tex->mtlTexture) {
            tex->Release();
            return E_FAIL;
        }
        *ppTexture1D = tex;
        return S_OK;
    }

    HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D) override {
        if (!pDesc || !ppTexture2D) return E_INVALIDARG;
        auto* tex = new Cod3Texture2D(mtlDevice, *pDesc, pInitialData);
        tex->d3dDevice = this;
        if (pDesc->Usage != D3D11_USAGE_STAGING && !tex->mtlTexture) {
            tex->Release();
            return E_FAIL;
        }
        *ppTexture2D = tex;
        return S_OK;
    }

    HRESULT CreateTexture3D(const D3D11_TEXTURE3D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture3D** ppTexture3D) override {
        if (!pDesc || !ppTexture3D) return E_INVALIDARG;
        auto* tex = new Cod3Texture3D(mtlDevice, *pDesc, pInitialData);
        if (!tex->mtlTexture) {
            tex->Release();
            return E_FAIL;
        }
        *ppTexture3D = tex;
        return S_OK;
    }

    HRESULT CreateShaderResourceView(ID3D11Resource* pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc, ID3D11ShaderResourceView** ppSRView) override {
        if (!pResource || !ppSRView) return E_INVALIDARG;
        *ppSRView = new Cod3ShaderResourceView(pResource, pDesc);
        return S_OK;
    }

    HRESULT CreateRenderTargetView(ID3D11Resource* pResource, const D3D11_RENDER_TARGET_VIEW_DESC* pDesc, ID3D11RenderTargetView** ppRTView) override {
        if (!pResource || !ppRTView) return E_INVALIDARG;
        *ppRTView = new Cod3RenderTargetView(pResource, pDesc);
        return S_OK;
    }

    HRESULT CreateDepthStencilView(ID3D11Resource* pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc, ID3D11DepthStencilView** ppDepthStencilView) override {
        if (!pResource || !ppDepthStencilView) return E_INVALIDARG;
        *ppDepthStencilView = new Cod3DepthStencilView(pResource, pDesc);
        return S_OK;
    }

    HRESULT CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements, const void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, ID3D11InputLayout** ppInputLayout) override {
        if (!pInputElementDescs || !ppInputLayout) return E_INVALIDARG;
        *ppInputLayout = new Cod3InputLayout(pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature, BytecodeLength);
        return S_OK;
    }

    HRESULT CreateVertexShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage*, ID3D11VertexShader** ppVertexShader) override {
        if (!pShaderBytecode || !ppVertexShader) return E_INVALIDARG;
        auto* vs = new Cod3VertexShader();
        if (!vs->data.ParseAndCompile(mtlDevice, pShaderBytecode, BytecodeLength)) {
            vs->Release();
            return E_FAIL;
        }
        *ppVertexShader = vs;
        return S_OK;
    }

    HRESULT CreatePixelShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage*, ID3D11PixelShader** ppPixelShader) override {
        if (!pShaderBytecode || !ppPixelShader) return E_INVALIDARG;
        auto* ps = new Cod3PixelShader();
        if (!ps->data.ParseAndCompile(mtlDevice, pShaderBytecode, BytecodeLength)) {
            ps->Release();
            return E_FAIL;
        }
        *ppPixelShader = ps;
        return S_OK;
    }

    HRESULT CreateGeometryShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage*, ID3D11GeometryShader** ppGeometryShader) override {
        if (!pShaderBytecode || !ppGeometryShader) return E_INVALIDARG;
        auto* gs = new Cod3GeometryShader();
        if (!gs->data.ParseAndCompile(mtlDevice, pShaderBytecode, BytecodeLength)) {
            gs->Release();
            return E_FAIL;
        }
        *ppGeometryShader = gs;
        return S_OK;
    }

    HRESULT CreateBlendState(const D3D11_BLEND_DESC* pBlendStateDesc, ID3D11BlendState** ppBlendState) override {
        if (!pBlendStateDesc || !ppBlendState) return E_INVALIDARG;
        *ppBlendState = new Cod3BlendState(*pBlendStateDesc);
        return S_OK;
    }

    HRESULT CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* pDepthStencilDesc, ID3D11DepthStencilState** ppDepthStencilState) override {
        if (!pDepthStencilDesc || !ppDepthStencilState) return E_INVALIDARG;
        *ppDepthStencilState = new Cod3DepthStencilState(mtlDevice, *pDepthStencilDesc);
        return S_OK;
    }

    HRESULT CreateRasterizerState(const D3D11_RASTERIZER_DESC* pRasterizerDesc, ID3D11RasterizerState** ppRasterizerState) override {
        if (!pRasterizerDesc || !ppRasterizerState) return E_INVALIDARG;
        *ppRasterizerState = new Cod3RasterizerState(*pRasterizerDesc);
        return S_OK;
    }

    HRESULT CreateSamplerState(const D3D11_SAMPLER_DESC* pSamplerDesc, ID3D11SamplerState** ppSamplerState) override {
        if (!pSamplerDesc || !ppSamplerState) return E_INVALIDARG;
        *ppSamplerState = new Cod3SamplerState(mtlDevice, *pSamplerDesc);
        return S_OK;
    }

    HRESULT CreateQuery(const D3D11_QUERY_DESC* pQueryDesc, ID3D11Query** ppQuery) override {
        if (!pQueryDesc || !ppQuery) return E_INVALIDARG;
        *ppQuery = new Cod3Query(*pQueryDesc);
        return S_OK;
    }

    HRESULT CheckMultisampleQualityLevels(DXGI_FORMAT, UINT SampleCount, UINT* pNumQualityLevels) override {
        if (!pNumQualityLevels) return E_INVALIDARG;
        if (mtlDevice && [mtlDevice supportsTextureSampleCount:SampleCount]) {
            *pNumQualityLevels = 1;
        } else {
            *pNumQualityLevels = 0;
        }
        return S_OK;
    }

    HRESULT CheckFeatureSupport(D3D11_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) override {
        if (Feature == D3D11_FEATURE_D3D11_OPTIONS && pFeatureSupportData && FeatureSupportDataSize >= sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS)) {
            auto* opts = static_cast<D3D11_FEATURE_DATA_D3D11_OPTIONS*>(pFeatureSupportData);
            std::memset(opts, 0, sizeof(*opts));
            opts->ConstantBufferPartialUpdate = TRUE;
            opts->ConstantBufferOffsetting = TRUE;
            opts->ClearView = TRUE;
            return S_OK;
        }
        return E_FAIL;
    }

    void GetImmediateContext(ID3D11DeviceContext** ppImmediateContext) override {
        if (!ppImmediateContext) return;
        if (immediateContext) immediateContext->AddRef();
        *ppImmediateContext = immediateContext;
    }
};

struct Cod3SwapChain final : public IDXGISwapChain2 {
    Cod3Device* device = nullptr;
    HWND hwnd = nullptr;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    Cod3Texture2D* backBuffer = nullptr;
    SDL_MetalView metalView = nullptr;
    CAMetalLayer* metalLayer = nil;

    Cod3SwapChain(Cod3Device* dev, HWND h, const DXGI_SWAP_CHAIN_DESC1& d)
        : device(dev), hwnd(h), desc(d) {
        if (device) device->AddRef();
        if (desc.Width == 0) desc.Width = 1280;
        if (desc.Height == 0) desc.Height = 720;
        if (desc.Format == DXGI_FORMAT_UNKNOWN) desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (hwnd) {
            auto* sdlWin = static_cast<SDL_Window*>(hwnd);
            metalView = SDL_Metal_CreateView(sdlWin);
            if (metalView) {
                metalLayer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(metalView);
                metalLayer.device = device->mtlDevice;
                metalLayer.pixelFormat = ToMtlPixelFormat(desc.Format);
                metalLayer.drawableSize = CGSizeMake(desc.Width, desc.Height);
            }
        }
        CreateBackBuffer();
    }

    ~Cod3SwapChain() override {
        if (backBuffer) backBuffer->Release();
        if (metalView) SDL_Metal_DestroyView(metalView);
        if (device) device->Release();
    }

    void CreateBackBuffer() {
        if (backBuffer) {
            backBuffer->Release();
            backBuffer = nullptr;
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = std::max<UINT>(1u, desc.Width);
        td.Height = std::max<UINT>(1u, desc.Height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = desc.Format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        backBuffer = new Cod3Texture2D(device->mtlDevice, td, nullptr);
    }

    HRESULT GetParent(REFIID, void**) override { return E_FAIL; }

    HRESULT GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override {
        if (!pDesc) return E_INVALIDARG;
        *pDesc = desc;
        return S_OK;
    }

    HRESULT GetBuffer(UINT Buffer, REFIID, void** ppSurface) override {
        if (Buffer != 0 || !ppSurface || !backBuffer) return E_INVALIDARG;
        backBuffer->AddRef();
        *ppSurface = static_cast<ID3D11Texture2D*>(backBuffer);
        return S_OK;
    }

    HRESULT ResizeBuffers(UINT, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT) override {
        if (device && device->immediateContext) {
            device->immediateContext->FlushAndWait();
        }
        if (Width > 0) desc.Width = Width;
        if (Height > 0) desc.Height = Height;
        if (NewFormat != DXGI_FORMAT_UNKNOWN) desc.Format = NewFormat;
        if (metalLayer) {
            metalLayer.drawableSize = CGSizeMake(desc.Width, desc.Height);
        }
        CreateBackBuffer();
        return S_OK;
    }

    HRESULT Present(UINT, UINT) override {
        if (!device || !device->immediateContext) return E_FAIL;
        Cod3DeviceContext* ctx = device->immediateContext;
        ctx->EndRenderPass();
        ctx->EnsureCommandBuffer();

        if (metalLayer && backBuffer && backBuffer->mtlTexture) {
            @autoreleasepool {
                id<CAMetalDrawable> drawable = [metalLayer nextDrawable];
                if (drawable && drawable.texture) {
                    id<MTLBlitCommandEncoder> blit = [ctx->cmdBuffer blitCommandEncoder];
                    uint32_t w = std::min<uint32_t>(backBuffer->desc.Width, uint32_t(drawable.texture.width));
                    uint32_t h = std::min<uint32_t>(backBuffer->desc.Height, uint32_t(drawable.texture.height));
                    [blit copyFromTexture:backBuffer->mtlTexture
                              sourceSlice:0
                              sourceLevel:0
                             sourceOrigin:MTLOriginMake(0, 0, 0)
                               sourceSize:MTLSizeMake(w, h, 1)
                                toTexture:drawable.texture
                         destinationSlice:0
                         destinationLevel:0
                        destinationOrigin:MTLOriginMake(0, 0, 0)];
                    [blit endEncoding];
                    [ctx->cmdBuffer presentDrawable:drawable];
                }
            }
        }

        [ctx->cmdBuffer commit];
        [ctx->cmdBuffer waitUntilCompleted];
        ctx->cmdBuffer = nil;
        return S_OK;
    }

    HRESULT SetMaximumFrameLatency(UINT) override { return S_OK; }
};

struct Cod3Factory final : public IDXGIFactory5 {
    id<MTLDevice> mtlDevice = nil;
    Cod3Adapter* adapter = nullptr;

    Cod3Factory() {
        mtlDevice = MTLCreateSystemDefaultDevice();
        adapter = new Cod3Adapter(this, mtlDevice);
    }

    ~Cod3Factory() override {
        if (adapter) adapter->Release();
    }

    HRESULT GetParent(REFIID, void**) override { return E_FAIL; }

    HRESULT EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) override {
        if (Adapter > 0 || !ppAdapter || !adapter) return DXGI_ERROR_NOT_FOUND;
        adapter->AddRef();
        *ppAdapter = adapter;
        return S_OK;
    }

    HRESULT EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) override {
        if (Adapter > 0 || !ppAdapter || !adapter) return DXGI_ERROR_NOT_FOUND;
        adapter->AddRef();
        *ppAdapter = adapter;
        return S_OK;
    }

    HRESULT MakeWindowAssociation(HWND, UINT) override { return S_OK; }

    HRESULT CreateSwapChainForHwnd(
        IUnknown* pDevice,
        HWND hWnd,
        const DXGI_SWAP_CHAIN_DESC1* pDesc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
        IDXGIOutput*,
        IDXGISwapChain1** ppSwapChain) override {
        if (!pDevice || !pDesc || !ppSwapChain) return E_INVALIDARG;
        auto* dev = dynamic_cast<Cod3Device*>(pDevice);
        if (!dev) return E_INVALIDARG;
        *ppSwapChain = new Cod3SwapChain(dev, hWnd, *pDesc);
        return S_OK;
    }

    HRESULT CheckFeatureSupport(DXGI_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) override {
        if (Feature == DXGI_FEATURE_PRESENT_ALLOW_TEARING && pFeatureSupportData && FeatureSupportDataSize >= sizeof(BOOL)) {
            *static_cast<BOOL*>(pFeatureSupportData) = TRUE;
            return S_OK;
        }
        return E_FAIL;
    }
};

} // namespace

// ============================================================================
// Exported C Entrypoints
// ============================================================================
extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID, void** ppFactory) {
    if (!ppFactory) return E_INVALIDARG;
    *ppFactory = static_cast<IDXGIFactory1*>(new Cod3Factory());
    return S_OK;
}

extern "C" HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter* pAdapter,
    D3D_DRIVER_TYPE,
    HMODULE,
    UINT,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels,
    UINT,
    ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext) {
    id<MTLDevice> mdev = MTLCreateSystemDefaultDevice();
    if (!mdev) return E_FAIL;

    Cod3Adapter* adp = dynamic_cast<Cod3Adapter*>(pAdapter);
    bool ownAdapter = false;
    if (!adp) {
        adp = new Cod3Adapter(nullptr, mdev);
        ownAdapter = true;
    }

    auto* dev = new Cod3Device(mdev, adp);
    if (ownAdapter) adp->Release();

    if (pFeatureLevel) {
        *pFeatureLevel = (pFeatureLevels && FeatureLevels > 0) ? pFeatureLevels[0] : D3D_FEATURE_LEVEL_11_1;
    }
    if (ppImmediateContext) {
        dev->immediateContext->AddRef();
        *ppImmediateContext = dev->immediateContext;
    }
    if (ppDevice) {
        *ppDevice = dev;
    } else {
        dev->Release();
    }
    return S_OK;
}

extern "C" HRESULT WINAPI D3DCreateBlob(SIZE_T Size, ID3DBlob** ppBlob) {
    if (!ppBlob) return E_INVALIDARG;
    *ppBlob = new Cod3Blob(Size);
    return S_OK;
}

extern "C" HRESULT WINAPI D3DCompile(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    LPCSTR,
    const D3D_SHADER_MACRO* pDefines,
    ID3DInclude*,
    LPCSTR pEntrypoint,
    LPCSTR pTarget,
    UINT,
    UINT,
    ID3DBlob** ppCode,
    ID3DBlob** ppErrorMsgs) {
    if (ppErrorMsgs) *ppErrorMsgs = nullptr;
    if (!pSrcData || SrcDataSize == 0 || !pTarget || !ppCode) return E_INVALIDARG;

    static std::once_flag initFlag;
    std::call_once(initFlag, []() {
        glslang::InitializeProcess();
    });

    EShLanguage lang = EShLangVertex;
    uint32_t stage = 1;
    if ( std::strncmp(pTarget, "ps_", 3) == 0 ) {
        lang = EShLangFragment;
        stage = 2;
    } else if ( std::strncmp(pTarget, "gs_", 3) == 0 ) {
        lang = EShLangGeometry;
        stage = 3;
    }

    if (stage == 3) {
        // Geometry shader (e.g. RectangleSource): store stage=3 blob
        ShaderBlobHeader hdr{};
        hdr.magic = kShaderMagic;
        hdr.version = kShaderVersion;
        hdr.stage = 3;
        auto* blob = new Cod3Blob(&hdr, sizeof(hdr));
        *ppCode = blob;
        return S_OK;
    }

    const char* entry = (pEntrypoint && pEntrypoint[0]) ? pEntrypoint : "main";
    std::string combinedSource;
    if (pDefines) {
        for (const D3D_SHADER_MACRO* m = pDefines; m->Name != nullptr; ++m) {
            combinedSource += "#define ";
            combinedSource += m->Name;
            if (m->Definition) {
                combinedSource += " ";
                combinedSource += m->Definition;
            }
            combinedSource += "\n";
        }
    }
    combinedSource.append(static_cast<const char*>(pSrcData), SrcDataSize);
    while (!combinedSource.empty() && combinedSource.back() == '\0') {
        combinedSource.pop_back();
    }

    // MSL texture1d lacks sample(..., level(0)); rewrite QuadSource's Texture1D gammaTable to Texture2D
    if (combinedSource.find("Texture1D gammaTable") != std::string::npos) {
        auto replaceAll = [&](const std::string& from, const std::string& to) {
            size_t pos = 0;
            while ((pos = combinedSource.find(from, pos)) != std::string::npos) {
                combinedSource.replace(pos, from.size(), to);
                pos += to.size();
            }
        };
        replaceAll("Texture1D gammaTable", "Texture2D gammaTable");
        replaceAll("gammaTable.SampleLevel(smp, at.r, 0)", "gammaTable.SampleLevel(smp, float2(at.r, 0.5), 0)");
        replaceAll("gammaTable.SampleLevel(smp, at.g, 0)", "gammaTable.SampleLevel(smp, float2(at.g, 0.5), 0)");
        replaceAll("gammaTable.SampleLevel(smp, at.b, 0)", "gammaTable.SampleLevel(smp, float2(at.b, 0.5), 0)");
    }

    const char* srcPtr = combinedSource.c_str();
    int srcLen = static_cast<int>(combinedSource.size());

    glslang::TShader shader(lang);
    shader.setStringsWithLengths(&srcPtr, &srcLen, 1);
    shader.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    shader.setEnvTargetHlslFunctionality1();
    shader.setEntryPoint(entry);
    shader.setSourceEntryPoint(entry);
    shader.setAutoMapBindings(true);
    shader.setAutoMapLocations(true);
    shader.setHlslIoMapping(true);
    shader.setShiftCbufferBinding(0);
    shader.setShiftTextureBinding(64);
    shader.setShiftSsboBinding(64);
    shader.setShiftSamplerBinding(192);

    EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslLegalization);
    if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
        const char* log = shader.getInfoLog();
        if (ppErrorMsgs && log) *ppErrorMsgs = new Cod3Blob(log, std::strlen(log) + 1);
        return E_FAIL;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages) || !program.mapIO()) {
        const char* log = program.getInfoLog();
        if (ppErrorMsgs && log) *ppErrorMsgs = new Cod3Blob(log, std::strlen(log) + 1);
        return E_FAIL;
    }

    std::vector<uint32_t> spirv;
    glslang::SpvOptions spvOpts{};
    spvOpts.disableOptimizer = false;
    spvOpts.optimizeSize = false;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &spvOpts);

    try {
        spirv_cross::CompilerMSL msl(spirv);
        auto mslOpts = msl.get_msl_options();
        mslOpts.set_msl_version(2, 4);
        mslOpts.enable_decoration_binding = false;
        msl.set_msl_options(mslOpts);

        spirv_cross::ShaderResources resources = msl.get_shader_resources();
        std::string mslSource = msl.compile();

        std::vector<ShaderBindingPair> cbBindings;
        for (const auto& r : resources.uniform_buffers) {
            uint32_t spvBind = msl.get_decoration(r.id, spv::DecorationBinding);
            uint32_t mslBind = msl.get_automatic_msl_resource_binding(r.id);
            if (mslBind != uint32_t(-1)) {
                cbBindings.push_back({ uint16_t(spvBind), uint16_t(mslBind) });
            }
        }

        std::vector<ShaderBindingPair> srvBufBindings;
        for (const auto& r : resources.storage_buffers) {
            uint32_t spvBind = msl.get_decoration(r.id, spv::DecorationBinding);
            uint32_t mslBind = msl.get_automatic_msl_resource_binding(r.id);
            uint32_t d3dSlot = (spvBind >= 64) ? (spvBind - 64) : spvBind;
            if (mslBind != uint32_t(-1)) {
                srvBufBindings.push_back({ uint16_t(d3dSlot), uint16_t(mslBind) });
            }
        }

        std::vector<ShaderBindingPair> srvTexBindings;
        for (const auto& r : resources.separate_images) {
            uint32_t spvBind = msl.get_decoration(r.id, spv::DecorationBinding);
            uint32_t mslBind = msl.get_automatic_msl_resource_binding(r.id);
            uint32_t d3dSlot = (spvBind >= 64) ? (spvBind - 64) : spvBind;
            if (mslBind != uint32_t(-1)) {
                srvTexBindings.push_back({ uint16_t(d3dSlot), uint16_t(mslBind) });
            }
        }

        std::vector<ShaderBindingPair> samplerBindings;
        for (const auto& r : resources.separate_samplers) {
            uint32_t spvBind = msl.get_decoration(r.id, spv::DecorationBinding);
            uint32_t mslBind = msl.get_automatic_msl_resource_binding(r.id);
            uint32_t d3dSlot = (spvBind >= 192) ? (spvBind - 192) : spvBind;
            if (mslBind != uint32_t(-1)) {
                samplerBindings.push_back({ uint16_t(d3dSlot), uint16_t(mslBind) });
            }
        }

        std::vector<ShaderInputBinding> inputBindings;
        if (stage == 1) {
            for (const auto& r : resources.stage_inputs) {
                uint32_t loc = msl.get_decoration(r.id, spv::DecorationLocation);
                std::string sem = msl.get_decoration_string(r.id, spv::DecorationHlslSemanticGOOGLE);
                if (sem.empty()) sem = r.name;
                uint16_t semIdx = 0;
                while (!sem.empty() && std::isdigit(static_cast<unsigned char>(sem.back()))) {
                    semIdx = uint16_t(sem.back() - '0');
                    sem.pop_back();
                }
                ShaderInputBinding ib{};
                std::strncpy(ib.semanticName, sem.c_str(), sizeof(ib.semanticName) - 1);
                ib.semanticIndex = semIdx;
                ib.location = uint16_t(loc);
                inputBindings.push_back(ib);
            }
        }

        uint32_t flags = 0;
        if (stage == 1 && InjectRectVertexEntry(mslSource)) {
            flags |= 1u;
        }

        ShaderBlobHeader hdr{};
        hdr.magic = kShaderMagic;
        hdr.version = kShaderVersion;
        hdr.stage = stage;
        hdr.flags = flags;
        hdr.cbCount = uint32_t(cbBindings.size());
        hdr.srvBufCount = uint32_t(srvBufBindings.size());
        hdr.srvTexCount = uint32_t(srvTexBindings.size());
        hdr.samplerCount = uint32_t(samplerBindings.size());
        hdr.inputCount = uint32_t(inputBindings.size());
        hdr.mslLength = uint32_t(mslSource.size() + 1);

        size_t totalSize = sizeof(hdr) +
            (cbBindings.size() + srvBufBindings.size() + srvTexBindings.size() + samplerBindings.size()) * sizeof(ShaderBindingPair) +
            inputBindings.size() * sizeof(ShaderInputBinding) +
            hdr.mslLength;

        auto* blob = new Cod3Blob(totalSize);
        uint8_t* dst = blob->data.data();
        std::memcpy(dst, &hdr, sizeof(hdr));
        dst += sizeof(hdr);

        auto writePairs = [&](const std::vector<ShaderBindingPair>& v) {
            if (!v.empty()) {
                std::memcpy(dst, v.data(), v.size() * sizeof(ShaderBindingPair));
                dst += v.size() * sizeof(ShaderBindingPair);
            }
        };
        writePairs(cbBindings);
        writePairs(srvBufBindings);
        writePairs(srvTexBindings);
        writePairs(samplerBindings);

        if (!inputBindings.empty()) {
            std::memcpy(dst, inputBindings.data(), inputBindings.size() * sizeof(ShaderInputBinding));
            dst += inputBindings.size() * sizeof(ShaderInputBinding);
        }

        std::memcpy(dst, mslSource.c_str(), hdr.mslLength);
        *ppCode = blob;
        return S_OK;
    } catch (const std::exception& e) {
        if (ppErrorMsgs) {
            const char* msg = e.what();
            *ppErrorMsgs = new Cod3Blob(msg, std::strlen(msg) + 1);
        }
        return E_FAIL;
    }
}
