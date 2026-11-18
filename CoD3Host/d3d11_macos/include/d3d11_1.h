#pragma once

#include "d3d11.h"

typedef enum D3D11_COPY_FLAGS {
    D3D11_COPY_NO_OVERWRITE = 0x1,
    D3D11_COPY_DISCARD      = 0x2
} D3D11_COPY_FLAGS;

struct ID3D11DeviceContext1 : public ID3D11DeviceContext {
    virtual void UpdateSubresource1(
        ID3D11Resource* pDstResource,
        UINT DstSubresource,
        const D3D11_BOX* pDstBox,
        const void* pSrcData,
        UINT SrcRowPitch,
        UINT SrcDepthPitch,
        UINT CopyFlags) = 0;
    virtual void VSSetConstantBuffers1(
        UINT StartSlot,
        UINT NumBuffers,
        ID3D11Buffer* const* ppConstantBuffers,
        const UINT* pFirstConstant,
        const UINT* pNumConstants) = 0;
    virtual void PSSetConstantBuffers1(
        UINT StartSlot,
        UINT NumBuffers,
        ID3D11Buffer* const* ppConstantBuffers,
        const UINT* pFirstConstant,
        const UINT* pNumConstants) = 0;
    virtual void ClearView(
        ID3D11View* pView,
        const FLOAT Color[4],
        const D3D11_RECT* pRect,
        UINT NumRects) = 0;
};

template <> struct Cod3UuidTraits<ID3D11DeviceContext1> { static constexpr GUID guid = { 0x00000013, 0, 0, {0} }; };
