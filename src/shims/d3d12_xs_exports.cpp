#include <cstdint>

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#endif

#include "scarlettx/gpu/d3d12_xs_abi.h"

extern "C" {

#if defined(_WIN32)
SCARLETTX_D3D12_API HRESULT WINAPI ViceWin_D3D12CreateDevice(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL minimumFeatureLevel,
    REFIID riid,
    void** ppDevice) {
    return static_cast<HRESULT>(D3D12XsCreateDevice(
        pAdapter, static_cast<uint32_t>(minimumFeatureLevel), &riid, ppDevice));
}
#else
SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12CreateDevice(
    void* pAdapter,
    uint32_t minimumFeatureLevel,
    const void* riid,
    void** ppDevice) {
    return D3D12XsCreateDevice(pAdapter, minimumFeatureLevel, riid, ppDevice);
}
#endif

#if defined(_WIN32)
SCARLETTX_D3D12_API HRESULT WINAPI ViceWin_D3D12GetDebugInterface(
    REFIID riid,
    void** ppvDebug) {
    return static_cast<HRESULT>(
        D3D12XsGetDebugInterface(&riid, ppvDebug));
}
#else
SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12GetDebugInterface(
    const void* riid,
    void** ppvDebug) {
    return D3D12XsGetDebugInterface(riid, ppvDebug);
}
#endif

#if defined(_WIN32)
SCARLETTX_D3D12_API HRESULT WINAPI ViceWin_D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC* pRootSignature,
    D3D_ROOT_SIGNATURE_VERSION version,
    ID3DBlob** ppBlob,
    ID3DBlob** ppErrorBlob) {
    return static_cast<HRESULT>(D3D12XsSerializeRootSignature(
        pRootSignature, static_cast<uint32_t>(version),
        reinterpret_cast<void**>(ppBlob),
        reinterpret_cast<void**>(ppErrorBlob)));
}
#else
SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12SerializeRootSignature(
    const void* pRootSignature,
    uint32_t version,
    void** ppBlob,
    void** ppErrorBlob) {
    return D3D12XsSerializeRootSignature(pRootSignature, version, ppBlob, ppErrorBlob);
}
#endif

#if defined(_WIN32)
SCARLETTX_D3D12_API HRESULT WINAPI ViceWin_D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
    ID3DBlob** ppBlob,
    ID3DBlob** ppErrorBlob) {
    return static_cast<HRESULT>(D3D12XsSerializeVersionedRootSignature(
        pRootSignature,
        reinterpret_cast<void**>(ppBlob),
        reinterpret_cast<void**>(ppErrorBlob)));
}
#else
SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12SerializeVersionedRootSignature(
    const void* pRootSignature,
    void** ppBlob,
    void** ppErrorBlob) {
    return D3D12XsSerializeVersionedRootSignature(pRootSignature, ppBlob, ppErrorBlob);
}
#endif

#if defined(_WIN32)
SCARLETTX_D3D12_API HRESULT WINAPI ViceWin_D3D12CreateRootSignatureDeserializer(
    LPCVOID pSrcData,
    SIZE_T srcDataSizeInBytes,
    REFIID rootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
    return static_cast<HRESULT>(D3D12XsCreateRootSignatureDeserializer(
        pSrcData, srcDataSizeInBytes,
        &rootSignatureDeserializerInterface, ppRootSignatureDeserializer));
}
#else
SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12CreateRootSignatureDeserializer(
    const void* pSrcData,
    size_t srcDataSizeInBytes,
    const void* pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
    return D3D12XsCreateRootSignatureDeserializer(
        pSrcData, srcDataSizeInBytes, pRootSignatureDeserializerInterface,
        ppRootSignatureDeserializer);
}
#endif

#if defined(_WIN32)
SCARLETTX_D3D12_API HRESULT WINAPI ViceWin_D3D12CreateVersionedRootSignatureDeserializer(
    LPCVOID pSrcData,
    SIZE_T srcDataSizeInBytes,
    REFIID rootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
    if (!ppRootSignatureDeserializer) {
        return E_POINTER;
    }
    *ppRootSignatureDeserializer = nullptr;
    HMODULE module = LoadLibraryW(L"d3d12.dll");
    if (!module) {
        return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    }
    using Fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    auto fn = reinterpret_cast<Fn>(
        GetProcAddress(module, "D3D12CreateVersionedRootSignatureDeserializer"));
    if (!fn) {
        return E_NOTIMPL;
    }
    return fn(pSrcData, srcDataSizeInBytes,
              rootSignatureDeserializerInterface,
              ppRootSignatureDeserializer);
}
#else
SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12CreateVersionedRootSignatureDeserializer(
    const void* pSrcData,
    size_t srcDataSizeInBytes,
    const void* pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
#if defined(_WIN32)
    if (!pRootSignatureDeserializerInterface || !ppRootSignatureDeserializer) {
        return static_cast<int32_t>(0x80004003u);
    }
    *ppRootSignatureDeserializer = nullptr;
    HMODULE module = LoadLibraryW(L"d3d12.dll");
    if (!module) {
        return static_cast<int32_t>(0x8007007eu);
    }
    using Fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    auto fn = reinterpret_cast<Fn>(
        GetProcAddress(module, "D3D12CreateVersionedRootSignatureDeserializer"));
    if (!fn) {
        return static_cast<int32_t>(0x80004005u);
    }
    return static_cast<int32_t>(fn(
        pSrcData,
        srcDataSizeInBytes,
        *reinterpret_cast<const IID*>(pRootSignatureDeserializerInterface),
        ppRootSignatureDeserializer));
#else
    (void)pSrcData;
    (void)srcDataSizeInBytes;
    (void)pRootSignatureDeserializerInterface;
    (void)ppRootSignatureDeserializer;
    return static_cast<int32_t>(0x80004001u);
#endif
}
#endif

// The console reserves address space for its graphics driver here. The host
// driver manages its own, so a valid request succeeds with nothing reserved.
// 0x10000000 is what the console returns for a successful request.
SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12ConfigureVirtualMemoryX(
    D3D12XS_VIRTUAL_MEMORY_CONFIG* pConfig) {
    if (!pConfig || (pConfig->Flags & ~1u) != 0 || pConfig->SizeIn4MbUnits > 33) {
        return static_cast<int32_t>(0x80070057u);
    }
    pConfig->Result0 = 0;
    pConfig->Result1 = 0;
    return 0x10000000;
}

// Data export: titles import the interface id itself. Same value as on PC.
#if defined(_WIN32)
SCARLETTX_D3D12_API extern const IID ViceWin_IID_ID3D12Device;
const IID ViceWin_IID_ID3D12Device = {
    0x189819f1u, 0x1db6, 0x4b57,
    {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
#else
struct ExportedGuid {
    uint32_t a;
    uint16_t b, c;
    uint8_t d[8];
};
SCARLETTX_D3D12_API extern const ExportedGuid IID_ID3D12Device;
const ExportedGuid IID_ID3D12Device = {
    0x189819f1u, 0x1db6, 0x4b57,
    {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
#endif

}  // extern "C"
