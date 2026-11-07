#pragma once

#include "d3dcommon.h"
#include "dxgiformat.h"

#ifndef DXGI_ERROR_NOT_FOUND
#define DXGI_ERROR_NOT_FOUND ((HRESULT)0x887A0002L)
#endif
#ifndef DXGI_ERROR_DEVICE_REMOVED
#define DXGI_ERROR_DEVICE_REMOVED ((HRESULT)0x887A0005L)
#endif
#ifndef DXGI_ERROR_DEVICE_RESET
#define DXGI_ERROR_DEVICE_RESET ((HRESULT)0x887A0007L)
#endif

#define DXGI_USAGE_RENDER_TARGET_OUTPUT (1L << (1 + 4))
#define DXGI_MWA_NO_ALT_ENTER           (1 << 1)

typedef struct DXGI_SAMPLE_DESC {
    UINT Count;
    UINT Quality;
} DXGI_SAMPLE_DESC;

typedef struct DXGI_RATIONAL {
    UINT Numerator;
    UINT Denominator;
} DXGI_RATIONAL;

typedef enum DXGI_SWAP_EFFECT {
    DXGI_SWAP_EFFECT_DISCARD         = 0,
    DXGI_SWAP_EFFECT_SEQUENTIAL      = 1,
    DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL = 3,
    DXGI_SWAP_EFFECT_FLIP_DISCARD    = 4
} DXGI_SWAP_EFFECT;

typedef enum DXGI_SWAP_CHAIN_FLAG {
    DXGI_SWAP_CHAIN_FLAG_NONPREROTATED   = 1,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH = 2,
    DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE  = 4,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING   = 2048
} DXGI_SWAP_CHAIN_FLAG;

typedef enum DXGI_PRESENT {
    DXGI_PRESENT_ALLOW_TEARING = 0x00000200UL
} DXGI_PRESENT;

typedef enum DXGI_ADAPTER_FLAG {
    DXGI_ADAPTER_FLAG_NONE     = 0,
    DXGI_ADAPTER_FLAG_REMOTE   = 1,
    DXGI_ADAPTER_FLAG_SOFTWARE = 2
} DXGI_ADAPTER_FLAG;

typedef struct DXGI_ADAPTER_DESC {
    wchar_t Description[128];
    UINT    VendorId;
    UINT    DeviceId;
    UINT    SubSysId;
    UINT    Revision;
    SIZE_T  DedicatedVideoMemory;
    SIZE_T  DedicatedSystemMemory;
    SIZE_T  SharedSystemMemory;
    LUID    AdapterLuid;
} DXGI_ADAPTER_DESC;

typedef struct DXGI_ADAPTER_DESC1 {
    wchar_t Description[128];
    UINT    VendorId;
    UINT    DeviceId;
    UINT    SubSysId;
    UINT    Revision;
    SIZE_T  DedicatedVideoMemory;
    SIZE_T  DedicatedSystemMemory;
    SIZE_T  SharedSystemMemory;
    LUID    AdapterLuid;
    UINT    Flags;
} DXGI_ADAPTER_DESC1;

struct IDXGIObject : public IUnknown {
    virtual HRESULT GetParent(REFIID riid, void** ppParent) = 0;
};

struct IDXGIAdapter : public IDXGIObject {
    virtual HRESULT GetDesc(DXGI_ADAPTER_DESC* pDesc) = 0;
};

struct IDXGIAdapter1 : public IDXGIAdapter {
    virtual HRESULT GetDesc1(DXGI_ADAPTER_DESC1* pDesc) = 0;
};

struct IDXGIDevice : public IDXGIObject {
    virtual HRESULT GetAdapter(IDXGIAdapter** pAdapter) = 0;
};

struct IDXGISwapChain : public IDXGIObject {
    virtual HRESULT Present(UINT SyncInterval, UINT Flags) = 0;
    virtual HRESULT GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) = 0;
    virtual HRESULT ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) = 0;
};

struct IDXGIFactory : public IDXGIObject {
    virtual HRESULT EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) = 0;
    virtual HRESULT MakeWindowAssociation(HWND WindowHandle, UINT Flags) = 0;
};

struct IDXGIFactory1 : public IDXGIFactory {
    virtual HRESULT EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) = 0;
};

template <> struct Cod3UuidTraits<IDXGIObject>    { static constexpr GUID guid = { 0x00000001, 0, 0, {0} }; };
template <> struct Cod3UuidTraits<IDXGIAdapter>   { static constexpr GUID guid = { 0x00000002, 0, 0, {0} }; };
template <> struct Cod3UuidTraits<IDXGIAdapter1>  { static constexpr GUID guid = { 0x00000003, 0, 0, {0} }; };
template <> struct Cod3UuidTraits<IDXGIDevice>    { static constexpr GUID guid = { 0x00000004, 0, 0, {0} }; };
template <> struct Cod3UuidTraits<IDXGISwapChain> { static constexpr GUID guid = { 0x00000005, 0, 0, {0} }; };
template <> struct Cod3UuidTraits<IDXGIFactory>   { static constexpr GUID guid = { 0x00000006, 0, 0, {0} }; };
template <> struct Cod3UuidTraits<IDXGIFactory1>  { static constexpr GUID guid = { 0x00000007, 0, 0, {0} }; };

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory);
