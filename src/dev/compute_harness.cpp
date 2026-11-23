#include "compute_harness.h"

#include <bit>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>

#include "core/logger.h"

#if defined(_WIN32)
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

namespace vicewindev {

// Engine-side names the harness needs at every call site.
using scarlettx::LogLevel;
using scarlettx::LogMessage;
using scarlettx::StatusCode;

namespace {

#if defined(_WIN32)

using Microsoft::WRL::ComPtr;

// Upper bound for one GPU pass; WARP on a large particle set is slow.
constexpr DWORD kGpuWaitTimeoutMs = 120000;

std::string HexHresult(HRESULT hr) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    const uint32_t value = static_cast<uint32_t>(hr);
    std::string text = "0x";
    for (int shift = 28; shift >= 0; shift -= 4) {
        text.push_back(kDigits[(value >> shift) & 0xFu]);
    }
    return text;
}

Status HrError(StatusCode code, HRESULT hr, std::string_view what) {
    return Status::Error(code, std::string(what) + " failed (" + HexHresult(hr) + ")");
}

std::string WideToUtf8(const wchar_t* wide) {
    if (!wide) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    out.pop_back();
    return out;
}

// Closes the fence event on every exit path of RunComputeDxil.
class ScopedHandle {
public:
    ScopedHandle() = default;
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ~ScopedHandle() {
        if (handle != nullptr) {
            CloseHandle(handle);
        }
    }

    HANDLE handle = nullptr;
};

D3D12_HEAP_PROPERTIES MakeHeapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = type;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;
    return props;
}

// Plain buffer description, built by hand: d3dx12.h is not on the include path.
D3D12_RESOURCE_DESC MakeBufferDesc(uint64_t size_bytes, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = size_bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

Status CreateBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heap_type, uint64_t size_bytes,
                    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initial_state,
                    ComPtr<ID3D12Resource>& out_buffer) {
    const D3D12_HEAP_PROPERTIES heap = MakeHeapProps(heap_type);
    const D3D12_RESOURCE_DESC desc = MakeBufferDesc(size_bytes, flags);
    const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       initial_state, nullptr,
                                                       IID_PPV_ARGS(&out_buffer));
    if (FAILED(hr)) {
        return HrError(StatusCode::DeviceError, hr, "ID3D12Device::CreateCommittedResource");
    }
    if (!out_buffer) {
        return Status::Error(StatusCode::DeviceError, "CreateCommittedResource returned no resource");
    }
    return Status::Ok();
}

void TransitionResource(ID3D12GraphicsCommandList* command_list, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    command_list->ResourceBarrier(1, &barrier);
}

struct DeviceSelection {
    ComPtr<ID3D12Device> device;
    ComPtr<IDXGIAdapter1> adapter;  // null when DXGI picked its default adapter
    bool is_warp = false;
};

Status CreateD3D12Device(bool prefer_hardware, DeviceSelection& out_selection) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        factory.Reset();
    }

    const auto try_adapter = [&out_selection](IDXGIAdapter1* candidate, bool is_warp) -> bool {
        ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(candidate, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) ||
            !device) {
            return false;
        }
        out_selection.device = device;
        if (candidate) {
            out_selection.adapter = candidate;
        }
        out_selection.is_warp = is_warp;
        return true;
    };

    if (prefer_hardware && factory) {
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            if (FAILED(factory->EnumAdapters1(index, &candidate)) || !candidate) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(candidate->GetDesc1(&desc))) {
                continue;
            }
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                continue;
            }
            if (try_adapter(candidate.Get(), false)) {
                return Status::Ok();
            }
        }
        LogMessage(LogLevel::Warning, "vicewindev",
                   "No hardware D3D12 adapter accepted the request; falling back to WARP.");
    }

    if (factory) {
        ComPtr<IDXGIAdapter1> warp;
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && warp) {
            if (try_adapter(warp.Get(), true)) {
                return Status::Ok();
            }
        }
    }

    // Last resort: let DXGI choose. The adapter name is recovered from the LUID.
    {
        ComPtr<ID3D12Device> device;
        if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) &&
            device) {
            out_selection.device = device;
            out_selection.is_warp = false;
            return Status::Ok();
        }
    }

    return Status::Error(StatusCode::DeviceError,
                         "D3D12CreateDevice failed for both hardware and WARP adapters");
}

std::string ResolveAdapterName(const DeviceSelection& selection) {
    if (!selection.device) {
        return "unknown adapter";
    }
    if (selection.is_warp) {
        return "WARP";
    }
    if (selection.adapter) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(selection.adapter->GetDesc1(&desc))) {
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                return "WARP";
            }
            const std::string name = WideToUtf8(desc.Description);
            if (!name.empty()) {
                return name;
            }
        }
        return "unknown adapter";
    }

    ComPtr<IDXGIFactory4> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        ComPtr<IDXGIAdapter1> adapter;
        if (SUCCEEDED(factory->EnumAdapterByLuid(selection.device->GetAdapterLuid(),
                                                 IID_PPV_ARGS(&adapter))) &&
            adapter) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                    return "WARP";
                }
                const std::string name = WideToUtf8(desc.Description);
                if (!name.empty()) {
                    return name;
                }
            }
        }
    }
    return "unknown adapter";
}

#endif  // _WIN32

}  // namespace

Status RunComputeDxil(const std::vector<uint8_t>& dxil,
                      const std::vector<float>& input,
                      const ComputeRunDesc& desc,
                      ComputeRunResult& out_result) {
    out_result = ComputeRunResult{};

#if !defined(_WIN32)
    return Status::Error(StatusCode::NotImplemented,
                         "RunComputeDxil needs Windows and a D3D12 runtime");
#else
    if (dxil.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "DXIL container is empty");
    }
    if (input.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Input buffer is empty");
    }
    // The UAV is a StructuredBuffer with a 16-byte stride, so the input must
    // consist of whole float4 elements.
    if (input.size() % 4u != 0u) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Input size is not a multiple of 4 floats (16-byte elements)");
    }
    if (desc.particle_count == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "particle_count must be > 0");
    }
    if (desc.thread_group_x == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "thread_group_x must be > 0");
    }
    const uint64_t buffer_size = static_cast<uint64_t>(input.size()) * sizeof(float);
    const uint64_t element_count = buffer_size / 16u;
    if (desc.particle_count > element_count) {
        return Status::Error(StatusCode::InvalidArgument,
                             "particle_count exceeds the number of 16-byte elements in input");
    }
    const uint64_t group_count =
        (static_cast<uint64_t>(desc.particle_count) + desc.thread_group_x - 1u) / desc.thread_group_x;

    DeviceSelection selection;
    Status status = CreateD3D12Device(desc.prefer_hardware, selection);
    if (!status) {
        LogMessage(LogLevel::Error, "vicewindev", status.message);
        return status;
    }
    out_result.adapter = ResolveAdapterName(selection);
    if (desc.prefer_hardware && out_result.adapter == "WARP") {
        LogMessage(LogLevel::Warning, "vicewindev",
                   "Hardware adapter unavailable; the compute pass runs on WARP.");
    }
    LogMessage(LogLevel::Info, "vicewindev",
               "Compute adapter: " + out_result.adapter +
                   ", groups=" + std::to_string(group_count) +
                   ", steps=" + std::to_string(desc.steps));

    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_desc.NodeMask = 0;
    HRESULT hr = selection.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) {
        return HrError(StatusCode::DeviceError, hr, "ID3D12Device::CreateCommandQueue");
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    hr = selection.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                  IID_PPV_ARGS(&allocator));
    if (FAILED(hr)) {
        return HrError(StatusCode::DeviceError, hr, "ID3D12Device::CreateCommandAllocator");
    }

    ComPtr<ID3D12GraphicsCommandList> command_list;
    hr = selection.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator.Get(),
                                             nullptr, IID_PPV_ARGS(&command_list));
    if (FAILED(hr)) {
        return HrError(StatusCode::DeviceError, hr, "ID3D12Device::CreateCommandList");
    }

    ComPtr<ID3D12Fence> fence;
    hr = selection.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) {
        return HrError(StatusCode::DeviceError, hr, "ID3D12Device::CreateFence");
    }
    ScopedHandle fence_event;
    fence_event.handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event.handle) {
        return Status::Error(StatusCode::DeviceError, "CreateEventA failed for the fence event");
    }

    D3D12_ROOT_PARAMETER root_parameters[2]{};
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[0].Constants.ShaderRegister = 0;
    root_parameters[0].Constants.RegisterSpace = 0;
    root_parameters[0].Constants.Num32BitValues = 4;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    root_parameters[1].Descriptor.ShaderRegister = 0;
    root_parameters[1].Descriptor.RegisterSpace = 0;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC root_signature_desc{};
    root_signature_desc.NumParameters = 2;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = nullptr;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature_blob;
    ComPtr<ID3DBlob> signature_error;
    hr = D3D12SerializeRootSignature(&root_signature_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                     signature_blob.GetAddressOf(),
                                     signature_error.GetAddressOf());
    if (FAILED(hr) || !signature_blob) {
        std::string message = "D3D12SerializeRootSignature failed (" + HexHresult(hr) + ")";
        if (signature_error) {
            const char* text = static_cast<const char*>(signature_error->GetBufferPointer());
            size_t length = signature_error->GetBufferSize();
            while (length > 0 && text[length - 1] == '\0') {
                --length;
            }
            message += ": ";
            message.append(text, length);
        }
        LogMessage(LogLevel::Error, "vicewindev", message);
        return Status::Error(StatusCode::DeviceError, message);
    }

    ComPtr<ID3D12RootSignature> root_signature;
    hr = selection.device->CreateRootSignature(0, signature_blob->GetBufferPointer(),
                                              signature_blob->GetBufferSize(),
                                              IID_PPV_ARGS(&root_signature));
    if (FAILED(hr)) {
        return HrError(StatusCode::DeviceError, hr, "ID3D12Device::CreateRootSignature");
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root_signature.Get();
    pso_desc.CS = {dxil.data(), dxil.size()};
    pso_desc.NodeMask = 0;
    pso_desc.CachedPSO = {nullptr, 0};
    pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    ComPtr<ID3D12PipelineState> pipeline_state;
    hr = selection.device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pipeline_state));
    if (FAILED(hr)) {
        return HrError(StatusCode::ShaderError, hr, "ID3D12Device::CreateComputePipelineState");
    }

    ComPtr<ID3D12Resource> upload_buffer;
    status = CreateBuffer(selection.device.Get(), D3D12_HEAP_TYPE_UPLOAD, buffer_size,
                          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                          upload_buffer);
    if (!status) {
        return status;
    }

    // Raw byte buffer: the 16-byte StructuredBuffer stride lives in the shader,
    // the resource itself only needs D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS.
    ComPtr<ID3D12Resource> uav_buffer;
    status = CreateBuffer(selection.device.Get(), D3D12_HEAP_TYPE_DEFAULT, buffer_size,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COPY_DEST, uav_buffer);
    if (!status) {
        return status;
    }

    ComPtr<ID3D12Resource> readback_buffer;
    status = CreateBuffer(selection.device.Get(), D3D12_HEAP_TYPE_READBACK, buffer_size,
                          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                          readback_buffer);
    if (!status) {
        return status;
    }

    void* mapped_upload = nullptr;
    const D3D12_RANGE no_read{0, 0};
    hr = upload_buffer->Map(0, &no_read, &mapped_upload);
    if (FAILED(hr) || !mapped_upload) {
        return HrError(StatusCode::DeviceError, hr, "ID3D12Resource::Map (upload)");
    }
    std::memcpy(mapped_upload, input.data(), static_cast<size_t>(buffer_size));
    upload_buffer->Unmap(0, nullptr);

    command_list->CopyBufferRegion(uav_buffer.Get(), 0, upload_buffer.Get(), 0, buffer_size);
    TransitionResource(command_list.Get(), uav_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const uint32_t root_constants[4] = {
        desc.particle_count,
        desc.steps,
        std::bit_cast<uint32_t>(desc.dt),
        0u,
    };
    command_list->SetComputeRootSignature(root_signature.Get());
    command_list->SetPipelineState(pipeline_state.Get());
    command_list->SetComputeRoot32BitConstants(0, 4, root_constants, 0);
    command_list->SetComputeRootUnorderedAccessView(1, uav_buffer->GetGPUVirtualAddress());
    // The kernel loops `steps` times internally, so exactly one dispatch is issued.
    command_list->Dispatch(static_cast<UINT>(group_count), 1, 1);

    TransitionResource(command_list.Get(), uav_buffer.Get(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    command_list->CopyResource(readback_buffer.Get(), uav_buffer.Get());

    hr = command_list->Close();
    if (FAILED(hr)) {
        return HrError(StatusCode::DeviceError, hr, "ID3D12GraphicsCommandList::Close");
    }

    ID3D12CommandList* command_lists[] = {command_list.Get()};
    const auto start_time = std::chrono::steady_clock::now();
    queue->ExecuteCommandLists(1, command_lists);
    hr = queue->Signal(fence.Get(), 1);
    if (FAILED(hr)) {
        return HrError(StatusCode::DeviceError, hr, "ID3D12CommandQueue::Signal");
    }
    if (fence->GetCompletedValue() < 1) {
        hr = fence->SetEventOnCompletion(1, fence_event.handle);
        if (FAILED(hr)) {
            return HrError(StatusCode::DeviceError, hr, "ID3D12Fence::SetEventOnCompletion");
        }
        if (WaitForSingleObject(fence_event.handle, kGpuWaitTimeoutMs) != WAIT_OBJECT_0) {
            return Status::Error(StatusCode::DeviceError,
                                 "Timed out waiting for the compute pass to complete");
        }
    }
    const auto end_time = std::chrono::steady_clock::now();
    // Wall-clock time of ExecuteCommandLists plus the fence wait: no GPU
    // timestamp queries are used, so this includes submission overhead.
    out_result.dispatch_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());

    void* mapped_readback = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(buffer_size)};
    hr = readback_buffer->Map(0, &read_range, &mapped_readback);
    if (FAILED(hr) || !mapped_readback) {
        return HrError(StatusCode::DeviceError, hr, "ID3D12Resource::Map (readback)");
    }
    out_result.output.resize(input.size());
    std::memcpy(out_result.output.data(), mapped_readback, static_cast<size_t>(buffer_size));
    const D3D12_RANGE written_range{0, 0};
    readback_buffer->Unmap(0, &written_range);

    LogMessage(LogLevel::Info, "vicewindev",
               "Compute pass done: " + std::to_string(group_count) + " group(s), " +
                   std::to_string(desc.steps) + " step(s), " +
                   std::to_string(out_result.dispatch_ns) + " ns wall-clock.");
    return Status::Ok();
#endif
}

}  // namespace vicewindev
