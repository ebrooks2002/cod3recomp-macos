#pragma once

#include "dxgi.h"

typedef enum DXGI_SCALING {
    DXGI_SCALING_STRETCH              = 0,
    DXGI_SCALING_NONE                 = 1,
    DXGI_SCALING_ASPECT_RATIO_STRETCH = 2
} DXGI_SCALING;

typedef enum DXGI_ALPHA_MODE {
    DXGI_ALPHA_MODE_UNSPECIFIED   = 0,
    DXGI_ALPHA_MODE_PREMULTIPLIED = 1,
    DXGI_ALPHA_MODE_STRAIGHT      = 2,
    DXGI_ALPHA_MODE_IGNORE        = 3
} DXGI_ALPHA_MODE;

typedef struct DXGI_SWAP_CHAIN_DESC1 {
    UINT             Width;
    UINT             Height;
    DXGI_FORMAT      Format;
    int32_t          Stereo;
    DXGI_SAMPLE_DESC SampleDesc;
    UINT             BufferUsage;
    UINT             BufferCount;
    DXGI_SCALING     Scaling;
    DXGI_SWAP_EFFECT SwapEffect;
    DXGI_ALPHA_MODE  AlphaMode;
    UINT             Flags;
} DXGI_SWAP_CHAIN_DESC1;

struct DXGI_SWAP_CHAIN_FULLSCREEN_DESC;
struct IDXGIOutput;

struct IDXGISwapChain1 : public IDXGISwapChain {
    virtual HRESULT GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) = 0;
};

struct IDXGISwapChain2 : public IDXGISwapChain1 {
    virtual HRESULT SetMaximumFrameLatency(UINT MaxLatency) = 0;
};

struct IDXGIFactory2 : public IDXGIFactory1 {
    virtual HRESULT CreateSwapChainForHwnd(
        IUnknown* pDevice,
        HWND hWnd,
        const DXGI_SWAP_CHAIN_DESC1* pDesc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
        IDXGIOutput* pRestrictToOutput,
        IDXGISwapChain1** ppSwapChain) = 0;
};

template <> struct Cod3UuidTraits<IDXGISwapChain1> { static constexpr GUID guid = { 0x00000008, 0, 0, {0} }; };
template <> struct Cod3UuidTraits<IDXGIFactory2>   { static constexpr GUID guid = { 0x00000009, 0, 0, {0} }; };
template <> struct Cod3UuidTraits<IDXGISwapChain2> { static constexpr GUID guid = { 0x0000000B, 0, 0, {0} }; };
