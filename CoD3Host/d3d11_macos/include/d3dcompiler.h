#pragma once

#include "d3dcommon.h"

#define D3DCOMPILER_DLL_A "d3dcompiler_47.dll"
#define D3DCOMPILE_DEBUG                (1 << 0)
#define D3DCOMPILE_SKIP_VALIDATION      (1 << 1)
#define D3DCOMPILE_SKIP_OPTIMIZATION    (1 << 2)
#define D3DCOMPILE_PACK_MATRIX_ROW_MAJOR (1 << 3)
#define D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR (1 << 4)
#define D3DCOMPILE_OPTIMIZATION_LEVEL0  (1 << 14)
#define D3DCOMPILE_OPTIMIZATION_LEVEL1  0
#define D3DCOMPILE_OPTIMIZATION_LEVEL2  ((1 << 14) | (1 << 15))
#define D3DCOMPILE_OPTIMIZATION_LEVEL3  (1 << 15)

typedef HRESULT (WINAPI *pD3DCompile)(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines,
    ID3DInclude* pInclude,
    LPCSTR pEntrypoint,
    LPCSTR pTarget,
    UINT Flags1,
    UINT Flags2,
    ID3DBlob** ppCode,
    ID3DBlob** ppErrorMsgs);

extern "C" HRESULT WINAPI D3DCompile(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines,
    ID3DInclude* pInclude,
    LPCSTR pEntrypoint,
    LPCSTR pTarget,
    UINT Flags1,
    UINT Flags2,
    ID3DBlob** ppCode,
    ID3DBlob** ppErrorMsgs);

extern "C" HRESULT WINAPI D3DCreateBlob(SIZE_T Size, ID3DBlob** ppBlob);
