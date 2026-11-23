// Class-ABI surface of XboxBvhBuilder.dll.
//
// The retail title imports the C++ class methods (mangled names), while our
// synthetic games use the flat XboxBvh* exports. Both live in this DLL: the
// class below is declared with the exact member signatures the imports encode,
// so the compiler emits matching symbols, and the bodies forward to the real
// D3D12 API wherever the ABI is known.
//
// The Xbox-only descriptor types stay opaque: they are only ever passed by
// pointer, and the mangled name depends on the type name, not its layout.

#include "core/logger.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#endif

struct D3D12XBOX_XDXR_STREAM_DESC;
struct D3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2;

namespace {

std::mutex& LogMutex() {
    static std::mutex mutex;
    return mutex;
}

bool& BuildWarningEmitted() {
    static bool emitted = false;
    return emitted;
}

std::string Hex(uint64_t value) {
    char buffer[32] = {};
#if defined(_WIN32)
    std::snprintf(buffer, sizeof(buffer), "0x%llX",
                  static_cast<unsigned long long>(value));
#else
    std::snprintf(buffer, sizeof(buffer), "0x%llx",
                  static_cast<unsigned long long>(value));
#endif
    return buffer;
}

}  // namespace

class __declspec(dllexport) XboxBvhBuilder {
public:
    XboxBvhBuilder(ID3D12Device11* device, bool use_64bit_morton_codes);
    ~XboxBvhBuilder();

    void Set64BitMortonCodes(bool enabled);

    void GetRaytracingAccelerationStructurePrebuildInfo(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* inputs,
        const D3D12XBOX_XDXR_STREAM_DESC* stream,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* info);

    void BuildRaytracingAccelerationStructureBatched2X(
        ID3D12GraphicsCommandList6* command_list,
        unsigned int count,
        const D3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2* descs);

    void CopyRaytracingAccelerationStructure(
        ID3D12GraphicsCommandList4* command_list,
        unsigned __int64 destination,
        unsigned __int64 source,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode);

    long RehydrateRaytracingAccelerationStructureBatched2X(
        ID3D12GraphicsCommandList8* command_list,
        unsigned int count,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* descs,
        const unsigned __int64* reserved);

private:
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D12Device11> device_;
    Microsoft::WRL::ComPtr<ID3D12Device5> device5_;
#endif
    bool use_64bit_morton_codes_ = false;
};

#if defined(_WIN32)

XboxBvhBuilder::XboxBvhBuilder(ID3D12Device11* device,
                               bool use_64bit_morton_codes)
    : use_64bit_morton_codes_(use_64bit_morton_codes) {
    if (device) {
        device_ = device;
        // Prebuild information comes straight from the real device; without it
        // the title would size its scratch buffers from zeros.
        device->QueryInterface(IID_PPV_ARGS(&device5_));
    }
    scarlettx::LogMessage(scarlettx::LogLevel::Info, "bvh",
                          "XboxBvhBuilder constructed (device=" +
                              Hex(reinterpret_cast<uint64_t>(device)) +
                              ", description backed by D3D12=" +
                              (device5_ ? "yes" : "no") +
                              ", 64-bit morton codes=" +
                              (use_64bit_morton_codes ? "yes" : "no") + ")");
}

XboxBvhBuilder::~XboxBvhBuilder() = default;

void XboxBvhBuilder::Set64BitMortonCodes(bool enabled) {
    use_64bit_morton_codes_ = enabled;
}

void XboxBvhBuilder::GetRaytracingAccelerationStructurePrebuildInfo(
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* inputs,
    const D3D12XBOX_XDXR_STREAM_DESC* stream,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* info) {
    if (!info) {
        return;
    }
    std::memset(info, 0, sizeof(*info));
    if (!inputs) {
        return;
    }
    if (device5_) {
        // The stream descriptor only selects Xbox-side memory streaming; the
        // desktop device computes the same sizes from the build inputs.
        device5_->GetRaytracingAccelerationStructurePrebuildInfo(inputs, info);
        return;
    }
    scarlettx::LogMessage(
        scarlettx::LogLevel::Warning, "bvh",
        "GetRaytracingAccelerationStructurePrebuildInfo without a D3D12 "
        "device behind it (stream=" +
            Hex(reinterpret_cast<uint64_t>(stream)) +
            "); reporting zero sizes, the title will allocate nothing");
}

void XboxBvhBuilder::BuildRaytracingAccelerationStructureBatched2X(
    ID3D12GraphicsCommandList6* command_list,
    unsigned int count,
    const D3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2* descs) {
    {
        std::lock_guard<std::mutex> lock(LogMutex());
        if (!BuildWarningEmitted()) {
            BuildWarningEmitted() = true;
            scarlettx::LogMessage(
                scarlettx::LogLevel::Warning, "bvh",
                "BuildRaytracingAccelerationStructureBatched2X: the Xbox batched "
                "descriptor layout is not decoded yet, so the build is skipped "
                "(list=" + Hex(reinterpret_cast<uint64_t>(command_list)) +
                    ", count=" + std::to_string(count) + ", descs=" +
                    Hex(reinterpret_cast<uint64_t>(descs)) +
                    "). Ray-traced effects stay disabled; the frame loop continues.");
        }
    }
}

void XboxBvhBuilder::CopyRaytracingAccelerationStructure(
    ID3D12GraphicsCommandList4* command_list,
    unsigned __int64 destination,
    unsigned __int64 source,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) {
    if (!command_list) {
        return;
    }
    command_list->CopyRaytracingAccelerationStructure(destination, source, mode);
}

long XboxBvhBuilder::RehydrateRaytracingAccelerationStructureBatched2X(
    ID3D12GraphicsCommandList8* command_list,
    unsigned int count,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* descs,
    const unsigned __int64* reserved) {
    scarlettx::LogMessage(
        scarlettx::LogLevel::Warning, "bvh",
        "RehydrateRaytracingAccelerationStructureBatched2X is not implemented "
        "(list=" + Hex(reinterpret_cast<uint64_t>(command_list)) +
            ", count=" + std::to_string(count) + ", descs=" +
            Hex(reinterpret_cast<uint64_t>(descs)) + ", reserved=" +
            Hex(reinterpret_cast<uint64_t>(reserved)) + ")");
    (void)use_64bit_morton_codes_;
    return 0;  // S_OK: the caller continues with a non-rehydrated structure
}

#else  // !defined(_WIN32)

XboxBvhBuilder::XboxBvhBuilder(ID3D12Device11*, bool use_64bit_morton_codes)
    : use_64bit_morton_codes_(use_64bit_morton_codes) {}
XboxBvhBuilder::~XboxBvhBuilder() = default;
void XboxBvhBuilder::Set64BitMortonCodes(bool enabled) {
    use_64bit_morton_codes_ = enabled;
}
void XboxBvhBuilder::GetRaytracingAccelerationStructurePrebuildInfo(
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS*, 
    const D3D12XBOX_XDXR_STREAM_DESC*,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO*) {}
void XboxBvhBuilder::BuildRaytracingAccelerationStructureBatched2X(
    ID3D12GraphicsCommandList6*, unsigned int,
    const D3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2*) {}
void XboxBvhBuilder::CopyRaytracingAccelerationStructure(
    ID3D12GraphicsCommandList4*, unsigned __int64, unsigned __int64,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE) {}
long XboxBvhBuilder::RehydrateRaytracingAccelerationStructureBatched2X(
    ID3D12GraphicsCommandList8*, unsigned int,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*,
    const unsigned __int64*) {
    return 0;
}

#endif
