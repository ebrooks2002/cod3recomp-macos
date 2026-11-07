#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <atomic>

#ifndef WINAPI
#define WINAPI
#endif

#ifndef COD3_WIN32_TYPES_DEFINED
#define COD3_WIN32_TYPES_DEFINED
typedef int32_t HRESULT;
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint32_t UINT;
typedef int32_t INT;
typedef int32_t LONG;
typedef bool BOOL;
typedef float FLOAT;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef uint64_t UINT64;
typedef size_t SIZE_T;
typedef const char* LPCSTR;
typedef const void* LPCVOID;
typedef void* LPVOID;
typedef wchar_t WCHAR;
typedef void* HWND;
typedef void* HMODULE;
typedef void* HANDLE;
#endif

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#ifndef S_OK
#define S_OK ((HRESULT)0L)
#endif
#ifndef S_FALSE
#define S_FALSE ((HRESULT)1L)
#endif
#ifndef E_FAIL
#define E_FAIL ((HRESULT)0x80004005L)
#endif
#ifndef E_INVALIDARG
#define E_INVALIDARG ((HRESULT)0x80070057L)
#endif
#ifndef E_OUTOFMEMORY
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#endif
#ifndef E_NOINTERFACE
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#endif
#ifndef DXGI_ERROR_NOT_FOUND
#define DXGI_ERROR_NOT_FOUND ((HRESULT)0x887A0002L)
#endif

#ifndef SUCCEEDED
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#endif
#ifndef FAILED
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#endif

#ifndef ZeroMemory
#define ZeroMemory(Destination, Length) memset((Destination), 0, (Length))
#endif

#ifndef COD3_RECT_DEFINED
#define COD3_RECT_DEFINED
struct RECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
};
#endif

#ifndef COD3_LUID_DEFINED
#define COD3_LUID_DEFINED
struct LUID {
    DWORD LowPart;
    LONG HighPart;
};
#endif

#ifndef COD3_GUID_DEFINED
#define COD3_GUID_DEFINED
struct GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
    bool operator==(const GUID& other) const {
        return std::memcmp(this, &other, sizeof(GUID)) == 0;
    }
    bool operator!=(const GUID& other) const {
        return !(*this == other);
    }
};
typedef GUID IID;
typedef const IID& REFIID;
#endif

template <typename T>
struct Cod3UuidTraits {
    static const GUID guid;
};
template <typename T>
const GUID Cod3UuidTraits<T>::guid = { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

#ifndef __uuidof
#define __uuidof(T) (Cod3UuidTraits<T>::guid)
#endif

#ifndef IID_PPV_ARGS
template <typename T>
inline void** Cod3IidPpvArgsHelper(T** pp) {
    return reinterpret_cast<void**>(pp);
}
template <typename T>
inline const GUID& Cod3IidFromPpType(T**) {
    return Cod3UuidTraits<T>::guid;
}
#define IID_PPV_ARGS(ppType) Cod3IidFromPpType(static_cast<decltype(ppType)>(nullptr)), Cod3IidPpvArgsHelper(ppType)
#endif

struct IUnknown {
    std::atomic<uint32_t> m_refCount{ 1 };
    virtual ~IUnknown() = default;
    virtual HRESULT QueryInterface(REFIID riid, void** ppvObject) {
        (void)riid;
        if (!ppvObject) return E_INVALIDARG;
        AddRef();
        *ppvObject = this;
        return S_OK;
    }
    virtual uint32_t AddRef() {
        return ++m_refCount;
    }
    virtual uint32_t Release() {
        uint32_t r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }
};

struct ID3D10Blob : public IUnknown {
    virtual LPVOID GetBufferPointer() = 0;
    virtual SIZE_T GetBufferSize() = 0;
};
typedef ID3D10Blob ID3DBlob;

typedef enum D3D_DRIVER_TYPE {
    D3D_DRIVER_TYPE_UNKNOWN   = 0,
    D3D_DRIVER_TYPE_HARDWARE  = 1,
    D3D_DRIVER_TYPE_REFERENCE = 2,
    D3D_DRIVER_TYPE_NULL      = 3,
    D3D_DRIVER_TYPE_SOFTWARE  = 4,
    D3D_DRIVER_TYPE_WARP      = 5
} D3D_DRIVER_TYPE;

typedef enum D3D_FEATURE_LEVEL {
    D3D_FEATURE_LEVEL_9_1  = 0x9100,
    D3D_FEATURE_LEVEL_9_2  = 0x9200,
    D3D_FEATURE_LEVEL_9_3  = 0x9300,
    D3D_FEATURE_LEVEL_10_0 = 0xa000,
    D3D_FEATURE_LEVEL_10_1 = 0xa100,
    D3D_FEATURE_LEVEL_11_0 = 0xb000,
    D3D_FEATURE_LEVEL_11_1 = 0xb100
} D3D_FEATURE_LEVEL;

typedef enum D3D_PRIMITIVE_TOPOLOGY {
    D3D_PRIMITIVE_TOPOLOGY_UNDEFINED     = 0,
    D3D_PRIMITIVE_TOPOLOGY_POINTLIST     = 1,
    D3D_PRIMITIVE_TOPOLOGY_LINELIST      = 2,
    D3D_PRIMITIVE_TOPOLOGY_LINESTRIP     = 3,
    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST  = 4,
    D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP = 5,
    D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED     = 0,
    D3D11_PRIMITIVE_TOPOLOGY_POINTLIST     = 1,
    D3D11_PRIMITIVE_TOPOLOGY_LINELIST      = 2,
    D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP     = 3,
    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST  = 4,
    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP = 5
} D3D_PRIMITIVE_TOPOLOGY;
typedef D3D_PRIMITIVE_TOPOLOGY D3D11_PRIMITIVE_TOPOLOGY;

typedef enum D3D_SRV_DIMENSION {
    D3D_SRV_DIMENSION_UNKNOWN          = 0,
    D3D_SRV_DIMENSION_BUFFER           = 1,
    D3D_SRV_DIMENSION_TEXTURE1D        = 2,
    D3D_SRV_DIMENSION_TEXTURE1DARRAY   = 3,
    D3D_SRV_DIMENSION_TEXTURE2D        = 4,
    D3D_SRV_DIMENSION_TEXTURE2DARRAY   = 5,
    D3D_SRV_DIMENSION_TEXTURE2DMS      = 6,
    D3D_SRV_DIMENSION_TEXTURE2DMSARRAY = 7,
    D3D_SRV_DIMENSION_TEXTURE3D        = 8,
    D3D_SRV_DIMENSION_TEXTURECUBE      = 9,
    D3D_SRV_DIMENSION_BUFFEREX         = 11,
    D3D11_SRV_DIMENSION_UNKNOWN          = 0,
    D3D11_SRV_DIMENSION_BUFFER           = 1,
    D3D11_SRV_DIMENSION_TEXTURE1D        = 2,
    D3D11_SRV_DIMENSION_TEXTURE1DARRAY   = 3,
    D3D11_SRV_DIMENSION_TEXTURE2D        = 4,
    D3D11_SRV_DIMENSION_TEXTURE2DARRAY   = 5,
    D3D11_SRV_DIMENSION_TEXTURE2DMS      = 6,
    D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY = 7,
    D3D11_SRV_DIMENSION_TEXTURE3D        = 8,
    D3D11_SRV_DIMENSION_TEXTURECUBE      = 9,
    D3D11_SRV_DIMENSION_BUFFEREX         = 11
} D3D_SRV_DIMENSION;
typedef D3D_SRV_DIMENSION D3D11_SRV_DIMENSION;

struct D3D_SHADER_MACRO {
    LPCSTR Name;
    LPCSTR Definition;
};
typedef struct ID3DInclude* LPD3DINCLUDE;
