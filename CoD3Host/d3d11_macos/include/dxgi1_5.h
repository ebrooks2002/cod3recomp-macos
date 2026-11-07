#pragma once

#include "dxgi1_2.h"

typedef enum DXGI_FEATURE {
    DXGI_FEATURE_PRESENT_ALLOW_TEARING = 0
} DXGI_FEATURE;

struct IDXGIFactory5 : public IDXGIFactory2 {
    virtual HRESULT CheckFeatureSupport(
        DXGI_FEATURE Feature,
        void* pFeatureSupportData,
        UINT FeatureSupportDataSize) = 0;
};

template <> struct Cod3UuidTraits<IDXGIFactory5> { static constexpr GUID guid = { 0x0000000A, 0, 0, {0} }; };
