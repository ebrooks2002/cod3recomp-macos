#pragma once

#include "d3dcommon.h"
#include "dxgiformat.h"
#include "dxgi.h"

#define D3D11_SDK_VERSION (7)
#define D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE (16)
#define D3D11_FLOAT32_MAX (3.402823466e+38f)

typedef RECT D3D11_RECT;

typedef enum D3D11_CREATE_DEVICE_FLAG {
    D3D11_CREATE_DEVICE_SINGLETHREADED = 0x1,
    D3D11_CREATE_DEVICE_DEBUG          = 0x2,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT   = 0x20
} D3D11_CREATE_DEVICE_FLAG;

typedef enum D3D11_USAGE {
    D3D11_USAGE_DEFAULT   = 0,
    D3D11_USAGE_IMMUTABLE = 1,
    D3D11_USAGE_DYNAMIC   = 2,
    D3D11_USAGE_STAGING   = 3
} D3D11_USAGE;

typedef enum D3D11_BIND_FLAG {
    D3D11_BIND_VERTEX_BUFFER    = 0x1L,
    D3D11_BIND_INDEX_BUFFER     = 0x2L,
    D3D11_BIND_CONSTANT_BUFFER  = 0x4L,
    D3D11_BIND_SHADER_RESOURCE  = 0x8L,
    D3D11_BIND_STREAM_OUTPUT    = 0x10L,
    D3D11_BIND_RENDER_TARGET    = 0x20L,
    D3D11_BIND_DEPTH_STENCIL    = 0x40L,
    D3D11_BIND_UNORDERED_ACCESS = 0x80L
} D3D11_BIND_FLAG;

typedef enum D3D11_CPU_ACCESS_FLAG {
    D3D11_CPU_ACCESS_WRITE = 0x10000L,
    D3D11_CPU_ACCESS_READ  = 0x20000L
} D3D11_CPU_ACCESS_FLAG;

typedef enum D3D11_RESOURCE_MISC_FLAG {
    D3D11_RESOURCE_MISC_GENERATE_MIPS     = 0x1L,
    D3D11_RESOURCE_MISC_SHARED            = 0x2L,
    D3D11_RESOURCE_MISC_TEXTURECUBE       = 0x4L,
    D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS = 0x10L,
    D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS = 0x20L,
    D3D11_RESOURCE_MISC_BUFFER_STRUCTURED = 0x40L
} D3D11_RESOURCE_MISC_FLAG;

typedef enum D3D11_MAP {
    D3D11_MAP_READ               = 1,
    D3D11_MAP_WRITE              = 2,
    D3D11_MAP_READ_WRITE         = 3,
    D3D11_MAP_WRITE_DISCARD      = 4,
    D3D11_MAP_WRITE_NO_OVERWRITE = 5
} D3D11_MAP;

typedef enum D3D11_CLEAR_FLAG {
    D3D11_CLEAR_DEPTH   = 0x1L,
    D3D11_CLEAR_STENCIL = 0x2L
} D3D11_CLEAR_FLAG;

typedef enum D3D11_COMPARISON_FUNC {
    D3D11_COMPARISON_NEVER         = 1,
    D3D11_COMPARISON_LESS          = 2,
    D3D11_COMPARISON_EQUAL         = 3,
    D3D11_COMPARISON_LESS_EQUAL    = 4,
    D3D11_COMPARISON_GREATER       = 5,
    D3D11_COMPARISON_NOT_EQUAL     = 6,
    D3D11_COMPARISON_GREATER_EQUAL = 7,
    D3D11_COMPARISON_ALWAYS        = 8
} D3D11_COMPARISON_FUNC;

typedef enum D3D11_DEPTH_WRITE_MASK {
    D3D11_DEPTH_WRITE_MASK_ZERO = 0,
    D3D11_DEPTH_WRITE_MASK_ALL  = 1
} D3D11_DEPTH_WRITE_MASK;

typedef enum D3D11_STENCIL_OP {
    D3D11_STENCIL_OP_KEEP     = 1,
    D3D11_STENCIL_OP_ZERO     = 2,
    D3D11_STENCIL_OP_REPLACE  = 3,
    D3D11_STENCIL_OP_INCR_SAT = 4,
    D3D11_STENCIL_OP_DECR_SAT = 5,
    D3D11_STENCIL_OP_INVERT   = 6,
    D3D11_STENCIL_OP_INCR     = 7,
    D3D11_STENCIL_OP_DECR     = 8
} D3D11_STENCIL_OP;

typedef enum D3D11_BLEND {
    D3D11_BLEND_ZERO             = 1,
    D3D11_BLEND_ONE              = 2,
    D3D11_BLEND_SRC_COLOR        = 3,
    D3D11_BLEND_INV_SRC_COLOR    = 4,
    D3D11_BLEND_SRC_ALPHA        = 5,
    D3D11_BLEND_INV_SRC_ALPHA    = 6,
    D3D11_BLEND_DEST_ALPHA       = 7,
    D3D11_BLEND_INV_DEST_ALPHA   = 8,
    D3D11_BLEND_DEST_COLOR       = 9,
    D3D11_BLEND_INV_DEST_COLOR   = 10,
    D3D11_BLEND_SRC_ALPHA_SAT    = 11,
    D3D11_BLEND_BLEND_FACTOR     = 14,
    D3D11_BLEND_INV_BLEND_FACTOR = 15,
    D3D11_BLEND_SRC1_COLOR       = 16,
    D3D11_BLEND_INV_SRC1_COLOR   = 17,
    D3D11_BLEND_SRC1_ALPHA       = 18,
    D3D11_BLEND_INV_SRC1_ALPHA   = 19
} D3D11_BLEND;

typedef enum D3D11_BLEND_OP {
    D3D11_BLEND_OP_ADD          = 1,
    D3D11_BLEND_OP_SUBTRACT     = 2,
    D3D11_BLEND_OP_REV_SUBTRACT = 3,
    D3D11_BLEND_OP_MIN          = 4,
    D3D11_BLEND_OP_MAX          = 5
} D3D11_BLEND_OP;

typedef enum D3D11_COLOR_WRITE_ENABLE {
    D3D11_COLOR_WRITE_ENABLE_RED   = 1,
    D3D11_COLOR_WRITE_ENABLE_GREEN = 2,
    D3D11_COLOR_WRITE_ENABLE_BLUE  = 4,
    D3D11_COLOR_WRITE_ENABLE_ALPHA = 8,
    D3D11_COLOR_WRITE_ENABLE_ALL   = (D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE | D3D11_COLOR_WRITE_ENABLE_ALPHA)
} D3D11_COLOR_WRITE_ENABLE;

typedef enum D3D11_FILL_MODE {
    D3D11_FILL_WIREFRAME = 2,
    D3D11_FILL_SOLID     = 3
} D3D11_FILL_MODE;

typedef enum D3D11_CULL_MODE {
    D3D11_CULL_NONE  = 1,
    D3D11_CULL_FRONT = 2,
    D3D11_CULL_BACK  = 3
} D3D11_CULL_MODE;

typedef enum D3D11_FILTER {
    D3D11_FILTER_MIN_MAG_MIP_POINT        = 0,
    D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR = 0x1,
    D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT = 0x4,
    D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR = 0x5,
    D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT = 0x10,
    D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x11,
    D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT = 0x14,
    D3D11_FILTER_MIN_MAG_MIP_LINEAR       = 0x15,
    D3D11_FILTER_ANISOTROPIC              = 0x55
} D3D11_FILTER;

typedef enum D3D11_TEXTURE_ADDRESS_MODE {
    D3D11_TEXTURE_ADDRESS_WRAP        = 1,
    D3D11_TEXTURE_ADDRESS_MIRROR      = 2,
    D3D11_TEXTURE_ADDRESS_CLAMP       = 3,
    D3D11_TEXTURE_ADDRESS_BORDER      = 4,
    D3D11_TEXTURE_ADDRESS_MIRROR_ONCE = 5
} D3D11_TEXTURE_ADDRESS_MODE;

typedef enum D3D11_DSV_DIMENSION {
    D3D11_DSV_DIMENSION_UNKNOWN          = 0,
    D3D11_DSV_DIMENSION_TEXTURE1D        = 1,
    D3D11_DSV_DIMENSION_TEXTURE1DARRAY   = 2,
    D3D11_DSV_DIMENSION_TEXTURE2D        = 3,
    D3D11_DSV_DIMENSION_TEXTURE2DARRAY   = 4,
    D3D11_DSV_DIMENSION_TEXTURE2DMS      = 5,
    D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY = 6
} D3D11_DSV_DIMENSION;

typedef enum D3D11_RTV_DIMENSION {
    D3D11_RTV_DIMENSION_UNKNOWN          = 0,
    D3D11_RTV_DIMENSION_BUFFER           = 1,
    D3D11_RTV_DIMENSION_TEXTURE1D        = 2,
    D3D11_RTV_DIMENSION_TEXTURE1DARRAY   = 3,
    D3D11_RTV_DIMENSION_TEXTURE2D        = 4,
    D3D11_RTV_DIMENSION_TEXTURE2DARRAY   = 5,
    D3D11_RTV_DIMENSION_TEXTURE2DMS      = 6,
    D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY = 7,
    D3D11_RTV_DIMENSION_TEXTURE3D        = 8
} D3D11_RTV_DIMENSION;

typedef enum D3D11_BUFFEREX_SRV_FLAG {
    D3D11_BUFFEREX_SRV_FLAG_RAW = 0x1,
    D3D11_BUFFER_SRV_FLAG_RAW   = 0x1
} D3D11_BUFFEREX_SRV_FLAG;

typedef enum D3D11_FEATURE {
    D3D11_FEATURE_D3D11_OPTIONS = 7
} D3D11_FEATURE;

typedef struct D3D11_FEATURE_DATA_D3D11_OPTIONS {
    int32_t OutputMergerLogicOp;
    int32_t UAVOnlyRenderingForcedSampleCount;
    int32_t DiscardAPIsSeenByDriver;
    int32_t FlagsForUpdateAndCopySeenByDriver;
    int32_t ClearView;
    int32_t CopyWithOverlap;
    int32_t ConstantBufferPartialUpdate;
    int32_t ConstantBufferOffsetting;
    int32_t MapNoOverwriteOnDynamicConstantBuffer;
    int32_t MapNoOverwriteOnDynamicBufferSRV;
    int32_t MultisampleRTVWithForcedSampleCountOne;
    int32_t SAD4ShaderInstructions;
    int32_t ExtendedDoublesShaderInstructions;
    int32_t ExtendedResourceSharing;
} D3D11_FEATURE_DATA_D3D11_OPTIONS;

typedef enum D3D11_INPUT_CLASSIFICATION {
    D3D11_INPUT_PER_VERTEX_DATA   = 0,
    D3D11_INPUT_PER_INSTANCE_DATA = 1
} D3D11_INPUT_CLASSIFICATION;

typedef enum D3D11_QUERY {
    D3D11_QUERY_EVENT              = 0,
    D3D11_QUERY_OCCLUSION          = 1,
    D3D11_QUERY_TIMESTAMP          = 2,
    D3D11_QUERY_TIMESTAMP_DISJOINT = 3
} D3D11_QUERY;

typedef enum D3D11_ASYNC_GETDATA_FLAG {
    D3D11_ASYNC_GETDATA_DONOTFLUSH = 0x1
} D3D11_ASYNC_GETDATA_FLAG;

typedef struct D3D11_BOX {
    UINT left;
    UINT top;
    UINT front;
    UINT right;
    UINT bottom;
    UINT back;
} D3D11_BOX;

typedef struct D3D11_VIEWPORT {
    FLOAT TopLeftX;
    FLOAT TopLeftY;
    FLOAT Width;
    FLOAT Height;
    FLOAT MinDepth;
    FLOAT MaxDepth;
} D3D11_VIEWPORT;

typedef struct D3D11_SUBRESOURCE_DATA {
    const void* pSysMem;
    UINT        SysMemPitch;
    UINT        SysMemSlicePitch;
} D3D11_SUBRESOURCE_DATA;

typedef struct D3D11_MAPPED_SUBRESOURCE {
    void* pData;
    UINT  RowPitch;
    UINT  DepthPitch;
} D3D11_MAPPED_SUBRESOURCE;

typedef struct D3D11_BUFFER_DESC {
    UINT        ByteWidth;
    D3D11_USAGE Usage;
    UINT        BindFlags;
    UINT        CPUAccessFlags;
    UINT        MiscFlags;
    UINT        StructureByteStride;
} D3D11_BUFFER_DESC;

typedef struct D3D11_TEXTURE1D_DESC {
    UINT        Width;
    UINT        MipLevels;
    UINT        ArraySize;
    DXGI_FORMAT Format;
    D3D11_USAGE Usage;
    UINT        BindFlags;
    UINT        CPUAccessFlags;
    UINT        MiscFlags;
} D3D11_TEXTURE1D_DESC;

typedef struct D3D11_TEXTURE2D_DESC {
    UINT             Width;
    UINT             Height;
    UINT             MipLevels;
    UINT             ArraySize;
    DXGI_FORMAT      Format;
    DXGI_SAMPLE_DESC SampleDesc;
    D3D11_USAGE      Usage;
    UINT             BindFlags;
    UINT             CPUAccessFlags;
    UINT             MiscFlags;
} D3D11_TEXTURE2D_DESC;

typedef struct D3D11_TEXTURE3D_DESC {
    UINT        Width;
    UINT        Height;
    UINT        Depth;
    UINT        MipLevels;
    DXGI_FORMAT Format;
    D3D11_USAGE Usage;
    UINT        BindFlags;
    UINT        CPUAccessFlags;
    UINT        MiscFlags;
} D3D11_TEXTURE3D_DESC;

typedef struct D3D11_TEX2D_ARRAY_SRV {
    UINT MostDetailedMip;
    UINT MipLevels;
    UINT FirstArraySlice;
    UINT ArraySize;
} D3D11_TEX2D_ARRAY_SRV;

typedef struct D3D11_BUFFER_SRV {
    union {
        UINT FirstElement;
        UINT ElementOffset;
    };
    union {
        UINT NumElements;
        UINT ElementWidth;
    };
} D3D11_BUFFER_SRV;

typedef struct D3D11_BUFFEREX_SRV {
    UINT FirstElement;
    UINT NumElements;
    UINT Flags;
} D3D11_BUFFEREX_SRV;

typedef struct D3D11_TEX2D_SRV {
    UINT MostDetailedMip;
    UINT MipLevels;
} D3D11_TEX2D_SRV;

typedef struct D3D11_TEX2DMS_SRV {
    UINT UnusedField_NothingToDefine;
} D3D11_TEX2DMS_SRV;

typedef struct D3D11_TEXCUBE_SRV {
    UINT MostDetailedMip;
    UINT MipLevels;
} D3D11_TEXCUBE_SRV;

typedef struct D3D11_SHADER_RESOURCE_VIEW_DESC {
    DXGI_FORMAT         Format;
    D3D11_SRV_DIMENSION ViewDimension;
    union {
        D3D11_BUFFER_SRV      Buffer;
        D3D11_TEX2D_SRV       Texture2D;
        D3D11_TEX2D_ARRAY_SRV Texture2DArray;
        D3D11_TEX2DMS_SRV     Texture2DMS;
        D3D11_TEXCUBE_SRV     TextureCube;
        D3D11_BUFFEREX_SRV    BufferEx;
    };
} D3D11_SHADER_RESOURCE_VIEW_DESC;

typedef struct D3D11_TEX2D_RTV {
    UINT MipSlice;
} D3D11_TEX2D_RTV;

typedef struct D3D11_TEX2DMS_RTV {
    UINT UnusedField_NothingToDefine;
} D3D11_TEX2DMS_RTV;

typedef struct D3D11_RENDER_TARGET_VIEW_DESC {
    DXGI_FORMAT         Format;
    D3D11_RTV_DIMENSION ViewDimension;
    union {
        D3D11_TEX2D_RTV   Texture2D;
        D3D11_TEX2DMS_RTV Texture2DMS;
    };
} D3D11_RENDER_TARGET_VIEW_DESC;

typedef struct D3D11_TEX2D_DSV {
    UINT MipSlice;
} D3D11_TEX2D_DSV;

typedef struct D3D11_TEX2DMS_DSV {
    UINT UnusedField_NothingToDefine;
} D3D11_TEX2DMS_DSV;

typedef struct D3D11_DEPTH_STENCIL_VIEW_DESC {
    DXGI_FORMAT         Format;
    D3D11_DSV_DIMENSION ViewDimension;
    UINT                Flags;
    union {
        D3D11_TEX2D_DSV   Texture2D;
        D3D11_TEX2DMS_DSV Texture2DMS;
    };
} D3D11_DEPTH_STENCIL_VIEW_DESC;

typedef struct D3D11_DEPTH_STENCILOP_DESC {
    D3D11_STENCIL_OP      StencilFailOp;
    D3D11_STENCIL_OP      StencilDepthFailOp;
    D3D11_STENCIL_OP      StencilPassOp;
    D3D11_COMPARISON_FUNC StencilFunc;
} D3D11_DEPTH_STENCILOP_DESC;

typedef struct D3D11_DEPTH_STENCIL_DESC {
    int32_t                    DepthEnable;
    D3D11_DEPTH_WRITE_MASK     DepthWriteMask;
    D3D11_COMPARISON_FUNC      DepthFunc;
    int32_t                    StencilEnable;
    BYTE                       StencilReadMask;
    BYTE                       StencilWriteMask;
    D3D11_DEPTH_STENCILOP_DESC FrontFace;
    D3D11_DEPTH_STENCILOP_DESC BackFace;
} D3D11_DEPTH_STENCIL_DESC;

typedef struct D3D11_RENDER_TARGET_BLEND_DESC {
    int32_t        BlendEnable;
    D3D11_BLEND    SrcBlend;
    D3D11_BLEND    DestBlend;
    D3D11_BLEND_OP BlendOp;
    D3D11_BLEND    SrcBlendAlpha;
    D3D11_BLEND    DestBlendAlpha;
    D3D11_BLEND_OP BlendOpAlpha;
    BYTE           RenderTargetWriteMask;
} D3D11_RENDER_TARGET_BLEND_DESC;

typedef struct D3D11_BLEND_DESC {
    int32_t                        AlphaToCoverageEnable;
    int32_t                        IndependentBlendEnable;
    D3D11_RENDER_TARGET_BLEND_DESC RenderTarget[8];
} D3D11_BLEND_DESC;

typedef struct D3D11_RASTERIZER_DESC {
    D3D11_FILL_MODE FillMode;
    D3D11_CULL_MODE CullMode;
    int32_t         FrontCounterClockwise;
    INT             DepthBias;
    FLOAT           DepthBiasClamp;
    FLOAT           SlopeScaledDepthBias;
    int32_t         DepthClipEnable;
    int32_t         ScissorEnable;
    int32_t         MultisampleEnable;
    int32_t         AntialiasedLineEnable;
} D3D11_RASTERIZER_DESC;

typedef struct D3D11_SAMPLER_DESC {
    D3D11_FILTER               Filter;
    D3D11_TEXTURE_ADDRESS_MODE AddressU;
    D3D11_TEXTURE_ADDRESS_MODE AddressV;
    D3D11_TEXTURE_ADDRESS_MODE AddressW;
    FLOAT                      MipLODBias;
    UINT                       MaxAnisotropy;
    D3D11_COMPARISON_FUNC      ComparisonFunc;
    FLOAT                      BorderColor[4];
    FLOAT                      MinLOD;
    FLOAT                      MaxLOD;
} D3D11_SAMPLER_DESC;

typedef struct D3D11_INPUT_ELEMENT_DESC {
    LPCSTR                     SemanticName;
    UINT                       SemanticIndex;
    DXGI_FORMAT                Format;
    UINT                       InputSlot;
    UINT                       AlignedByteOffset;
    D3D11_INPUT_CLASSIFICATION InputSlotClass;
    UINT                       InstanceDataStepRate;
} D3D11_INPUT_ELEMENT_DESC;

typedef struct D3D11_QUERY_DESC {
    D3D11_QUERY Query;
    UINT        MiscFlags;
} D3D11_QUERY_DESC;

typedef struct D3D11_QUERY_DATA_TIMESTAMP_DISJOINT {
    UINT64  Frequency;
    int32_t Disjoint;
} D3D11_QUERY_DATA_TIMESTAMP_DISJOINT;

struct ID3D11Device;
struct ID3D11DeviceChild : public IUnknown {
    virtual void GetDevice(ID3D11Device** ppDevice) { if (ppDevice) *ppDevice = nullptr; }
};
struct ID3D11Asynchronous : public ID3D11DeviceChild {};
struct ID3D11Query : public ID3D11Asynchronous {};
struct ID3D11Resource : public ID3D11DeviceChild {};
struct ID3D11Buffer : public ID3D11Resource {
    virtual void GetDesc(D3D11_BUFFER_DESC* pDesc) = 0;
};
struct ID3D11Texture1D : public ID3D11Resource {
    virtual void GetDesc(D3D11_TEXTURE1D_DESC* pDesc) = 0;
};
struct ID3D11Texture2D : public ID3D11Resource {
    virtual void GetDesc(D3D11_TEXTURE2D_DESC* pDesc) = 0;
};
struct ID3D11Texture3D : public ID3D11Resource {
    virtual void GetDesc(D3D11_TEXTURE3D_DESC* pDesc) = 0;
};
typedef struct D3D11_MESSAGE {
    const char* pDescription;
    SIZE_T      DescriptionByteLength;
} D3D11_MESSAGE;
struct ID3D11InfoQueue : public IUnknown {
    virtual UINT64 GetNumStoredMessages() = 0;
    virtual HRESULT GetMessage(UINT64 MessageIndex, D3D11_MESSAGE* pMessage, SIZE_T* pMessageByteLength) = 0;
    virtual void ClearStoredMessages() = 0;
};
struct ID3D11View : public ID3D11DeviceChild {
    virtual void GetResource(ID3D11Resource** ppResource) = 0;
};
struct ID3D11ShaderResourceView : public ID3D11View {
    virtual void GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc) = 0;
};
struct ID3D11RenderTargetView : public ID3D11View {};
struct ID3D11DepthStencilView : public ID3D11View {};
struct ID3D11VertexShader : public ID3D11DeviceChild {};
struct ID3D11PixelShader : public ID3D11DeviceChild {};
struct ID3D11GeometryShader : public ID3D11DeviceChild {};
struct ID3D11HullShader : public ID3D11DeviceChild {};
struct ID3D11DomainShader : public ID3D11DeviceChild {};
struct ID3D11ComputeShader : public ID3D11DeviceChild {};
struct ID3D11ClassInstance : public ID3D11DeviceChild {};
struct ID3D11ClassLinkage : public ID3D11DeviceChild {};
struct ID3D11InputLayout : public ID3D11DeviceChild {};
struct ID3D11BlendState : public ID3D11DeviceChild {};
struct ID3D11DepthStencilState : public ID3D11DeviceChild {};
struct ID3D11RasterizerState : public ID3D11DeviceChild {};
struct ID3D11SamplerState : public ID3D11DeviceChild {};

struct ID3D11DeviceContext : public ID3D11DeviceChild {
    virtual void VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) = 0;
    virtual void PSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) = 0;
    virtual void PSSetShader(ID3D11PixelShader* pPixelShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) = 0;
    virtual void PSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) = 0;
    virtual void VSSetShader(ID3D11VertexShader* pVertexShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) = 0;
    virtual void DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) = 0;
    virtual void Draw(UINT VertexCount, UINT StartVertexLocation) = 0;
    virtual HRESULT Map(ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource) = 0;
    virtual void Unmap(ID3D11Resource* pResource, UINT Subresource) = 0;
    virtual void PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) = 0;
    virtual void IASetInputLayout(ID3D11InputLayout* pInputLayout) = 0;
    virtual void IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppVertexBuffers, const UINT* pStrides, const UINT* pOffsets) = 0;
    virtual void IASetIndexBuffer(ID3D11Buffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset) = 0;
    virtual void IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) = 0;
    virtual void VSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) = 0;
    virtual void VSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) = 0;
    virtual void Begin(ID3D11Asynchronous* pAsync) = 0;
    virtual void End(ID3D11Asynchronous* pAsync) = 0;
    virtual HRESULT GetData(ID3D11Asynchronous* pAsync, void* pData, UINT DataSize, UINT GetDataFlags) = 0;
    virtual void GSSetShader(ID3D11GeometryShader* pShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) = 0;
    virtual void HSSetShader(ID3D11HullShader* pHullShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) = 0;
    virtual void DSSetShader(ID3D11DomainShader* pDomainShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) = 0;
    virtual void CSSetShader(ID3D11ComputeShader* pComputeShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) = 0;
    virtual void OMSetRenderTargets(UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView) = 0;
    virtual void OMSetBlendState(ID3D11BlendState* pBlendState, const FLOAT BlendFactor[4], UINT SampleMask) = 0;
    virtual void OMSetDepthStencilState(ID3D11DepthStencilState* pDepthStencilState, UINT StencilRef) = 0;
    virtual void RSSetState(ID3D11RasterizerState* pRasterizerState) = 0;
    virtual void RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports) = 0;
    virtual void RSSetScissorRects(UINT NumRects, const D3D11_RECT* pRects) = 0;
    virtual void CopySubresourceRegion(ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource, const D3D11_BOX* pSrcBox) = 0;
    virtual void CopyResource(ID3D11Resource* pDstResource, ID3D11Resource* pSrcResource) = 0;
    virtual void UpdateSubresource(ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) = 0;
    virtual void ClearRenderTargetView(ID3D11RenderTargetView* pRenderTargetView, const FLOAT ColorRGBA[4]) = 0;
    virtual void ClearDepthStencilView(ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, BYTE Stencil) = 0;
    virtual void GenerateMips(ID3D11ShaderResourceView* pShaderResourceView) = 0;
    virtual void ResolveSubresource(ID3D11Resource* pDstResource, UINT DstSubresource, ID3D11Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) = 0;

    // State getters (used by imgui_impl_dx11.cpp)
    virtual void VSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) = 0;
    virtual void PSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) = 0;
    virtual void PSGetShader(ID3D11PixelShader** ppPixelShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) = 0;
    virtual void PSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) = 0;
    virtual void VSGetShader(ID3D11VertexShader** ppVertexShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) = 0;
    virtual void IAGetInputLayout(ID3D11InputLayout** ppInputLayout) = 0;
    virtual void IAGetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppVertexBuffers, UINT* pStrides, UINT* pOffsets) = 0;
    virtual void IAGetIndexBuffer(ID3D11Buffer** pIndexBuffer, DXGI_FORMAT* Format, UINT* Offset) = 0;
    virtual void GSGetShader(ID3D11GeometryShader** ppGeometryShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) = 0;
    virtual void IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* pTopology) = 0;
    virtual void OMGetBlendState(ID3D11BlendState** ppBlendState, FLOAT BlendFactor[4], UINT* pSampleMask) = 0;
    virtual void OMGetDepthStencilState(ID3D11DepthStencilState** ppDepthStencilState, UINT* pStencilRef) = 0;
    virtual void RSGetState(ID3D11RasterizerState** ppRasterizerState) = 0;
    virtual void RSGetViewports(UINT* pNumViewports, D3D11_VIEWPORT* pViewports) = 0;
    virtual void RSGetScissorRects(UINT* pNumRects, D3D11_RECT* pRects) = 0;
};

struct ID3D11Device : public IUnknown {
    virtual HRESULT CreateBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer) = 0;
    virtual HRESULT CreateTexture1D(const D3D11_TEXTURE1D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture1D** ppTexture1D) = 0;
    virtual HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D) = 0;
    virtual HRESULT CreateTexture3D(const D3D11_TEXTURE3D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture3D** ppTexture3D) = 0;
    virtual HRESULT CreateShaderResourceView(ID3D11Resource* pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc, ID3D11ShaderResourceView** ppSRView) = 0;
    virtual HRESULT CreateRenderTargetView(ID3D11Resource* pResource, const D3D11_RENDER_TARGET_VIEW_DESC* pDesc, ID3D11RenderTargetView** ppRTView) = 0;
    virtual HRESULT CreateDepthStencilView(ID3D11Resource* pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc, ID3D11DepthStencilView** ppDepthStencilView) = 0;
    virtual HRESULT CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements, const void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, ID3D11InputLayout** ppInputLayout) = 0;
    virtual HRESULT CreateVertexShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader) = 0;
    virtual HRESULT CreateGeometryShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11GeometryShader** ppGeometryShader) = 0;
    virtual HRESULT CreatePixelShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader) = 0;
    virtual HRESULT CreateBlendState(const D3D11_BLEND_DESC* pBlendStateDesc, ID3D11BlendState** ppBlendState) = 0;
    virtual HRESULT CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* pDepthStencilDesc, ID3D11DepthStencilState** ppDepthStencilState) = 0;
    virtual HRESULT CreateRasterizerState(const D3D11_RASTERIZER_DESC* pRasterizerDesc, ID3D11RasterizerState** ppRasterizerState) = 0;
    virtual HRESULT CreateSamplerState(const D3D11_SAMPLER_DESC* pSamplerDesc, ID3D11SamplerState** ppSamplerState) = 0;
    virtual HRESULT CreateQuery(const D3D11_QUERY_DESC* pQueryDesc, ID3D11Query** ppQuery) = 0;
    virtual HRESULT CheckMultisampleQualityLevels(DXGI_FORMAT Format, UINT SampleCount, UINT* pNumQualityLevels) = 0;
    virtual HRESULT CheckFeatureSupport(D3D11_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) = 0;
    virtual void GetImmediateContext(ID3D11DeviceContext** ppImmediateContext) = 0;
};

template <> struct Cod3UuidTraits<ID3D11Device>        { static constexpr GUID guid = { 0x00000010, 0, 0, {0} }; };
template <> struct Cod3UuidTraits<ID3D11DeviceContext> { static constexpr GUID guid = { 0x00000011, 0, 0, {0} }; };
template <> struct Cod3UuidTraits<ID3D11Texture2D>     { static constexpr GUID guid = { 0x00000012, 0, 0, {0} }; };
template <> struct Cod3UuidTraits<ID3D11InfoQueue>     { static constexpr GUID guid = { 0x00000014, 0, 0, {0} }; };

extern "C" HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter* pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext);
