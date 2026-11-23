#include <algorithm>
#include "sx_check.h"
#include <bit>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <vector>

#include "scarlettx/kernel/kernelx_hle.h"
#include "scarlettx/loader/pe_loader.h"
#include "scarlettx/loader/pe_mapper.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

}  // namespace

int main() {
    // Memory APIs use host allocations aligned to the console's 64 KiB page.
    scarlettx::HleSymbolRegistry kernelx_symbols;
    scarlettx::kernelx::RegisterKernelxMemorySymbols(kernelx_symbols);
    scarlettx::kernelx::RegisterKernelxThreadingSymbols(kernelx_symbols);
    using XMemAllocFn = void* (*)(size_t, uint64_t);
    using XMemFreeFn = void (*)(void*, uint64_t);
    using XMemSetFn = void* (*)(void*, int, size_t);
    using XMemCpyFn = void* (*)(void*, const void*, size_t);
    using XMemGetAllocationStatisticsFn =
        int32_t (*)(uint32_t, int32_t, XMemAllocationStatistics*);
    using VirtualAllocXFn = void* (*)(void*, size_t, uint32_t, uint64_t, uint32_t);
    using MapTitleMemoryFn = void* (*)(void*, uint64_t, size_t, void*, uint32_t);
    using UnmapTitleMemoryFn = bool (*)(void*);
    const auto alloc = reinterpret_cast<XMemAllocFn>(
        kernelx_symbols.LookupSymbol("kernelx.dll", "XMemAlloc"));
    const auto free_mem = reinterpret_cast<XMemFreeFn>(
        kernelx_symbols.LookupSymbol("kernelx.dll", "XMemFree"));
    const auto set_mem = reinterpret_cast<XMemSetFn>(
        kernelx_symbols.LookupSymbol("kernelx.dll", "XMemSet"));
    const auto copy_mem = reinterpret_cast<XMemCpyFn>(
        kernelx_symbols.LookupSymbol("kernelx.dll", "XMemCpy"));
    const auto get_allocation_statistics =
        reinterpret_cast<XMemGetAllocationStatisticsFn>(
            kernelx_symbols.LookupSymbol(
                "xmem.dll", "XMemGetAllocationStatistics"));
    const auto virtual_alloc_x = reinterpret_cast<VirtualAllocXFn>(
        kernelx_symbols.LookupSymbol("kernelx.dll", "VirtualAllocX"));
    const auto map_title_memory = reinterpret_cast<MapTitleMemoryFn>(
        kernelx_symbols.LookupSymbol("kernelx.dll", "MapTitleMemory"));
    const auto unmap_title_memory = reinterpret_cast<UnmapTitleMemoryFn>(
        kernelx_symbols.LookupSymbol("kernelx.dll", "UnmapTitleMemory"));
    SX_REQUIRE(alloc && free_mem && set_mem && copy_mem &&
               get_allocation_statistics && virtual_alloc_x &&
               map_title_memory && unmap_title_memory);
    constexpr uint32_t kTrackedMemoryType = 3;
    auto* memory = static_cast<uint8_t*>(
        alloc(257, uint64_t{kTrackedMemoryType} << 29));
    SX_REQUIRE(memory != nullptr);
    SX_REQUIRE((reinterpret_cast<uintptr_t>(memory) & 0xFFFFu) == 0);
    XMemAllocationStatistics memory_statistics{};
    memory_statistics.size =
        static_cast<uint32_t>(sizeof(memory_statistics));
    SX_REQUIRE(get_allocation_statistics(
                   kTrackedMemoryType, 0, &memory_statistics) == 1);
    SX_REQUIRE(memory_statistics.allocated_bytes >= 64u * 1024u);
    SX_REQUIRE(memory_statistics.allocation_count == 1);
    for (size_t i = 0; i < 257; ++i) SX_REQUIRE(memory[i] == 0);
    SX_REQUIRE(set_mem(memory, 0xA5, 257) == memory);
    uint8_t copy[257]{};
    SX_REQUIRE(copy_mem(copy, memory, sizeof(copy)) == copy);
    for (uint8_t byte : copy) SX_REQUIRE(byte == 0xA5);
    free_mem(memory, 0);
    SX_REQUIRE(get_allocation_statistics(
                   kTrackedMemoryType, 0, &memory_statistics) == 1);
    SX_REQUIRE(memory_statistics.allocated_bytes == 0);
    SX_REQUIRE(memory_statistics.allocation_count == 0);

    // Xbox logical processor indexes map by rank into the host's available mask.
    const uintptr_t host_mask =
        (uintptr_t{1} << 2) | (uintptr_t{1} << 5) | (uintptr_t{1} << 7);
    SX_REQUIRE(scarlettx::kernelx::MapXboxThreadMaskToHost(0x5, host_mask) ==
               ((uintptr_t{1} << 2) | (uintptr_t{1} << 7)));
    SX_REQUIRE(scarlettx::kernelx::MapXboxProcessorIndexToHost(1, host_mask) == 5);
    SX_REQUIRE(scarlettx::kernelx::MapXboxProcessorIndexToHost(3, host_mask) ==
               UINT32_MAX);
    SX_REQUIRE(scarlettx::kernelx::MapXboxThreadMaskToHost(0x10000, host_mask) == 0);
    SX_REQUIRE(scarlettx::kernelx::MapXboxProcessorIndexToHost(16, host_mask) ==
               UINT32_MAX);
    SX_REQUIRE(kernelx_symbols.LookupSymbol(
                   "kernelx.dll", "GetLogicalProcessorInformationEx") != nullptr);
    SX_REQUIRE(kernelx_symbols.LookupSymbol(
                   "kernelx.dll", "SetThreadAffinityMask") != nullptr);
    SX_REQUIRE(kernelx_symbols.LookupSymbol(
                   "kernelx.dll", "SetThreadIdealProcessor") != nullptr);

#if defined(_WIN32)
    using GlobalMemoryStatusExFn = BOOL(WINAPI*)(LPMEMORYSTATUSEX);
    const auto global_memory_status = reinterpret_cast<GlobalMemoryStatusExFn>(
        kernelx_symbols.LookupSymbol("kernel32.dll", "GlobalMemoryStatusEx"));
    SX_REQUIRE(global_memory_status != nullptr);
    MEMORYSTATUSEX memory_status{};
    memory_status.dwLength = static_cast<DWORD>(sizeof(memory_status));
    SX_REQUIRE(global_memory_status(&memory_status));
    SX_REQUIRE(memory_status.ullTotalPhys ==
               13ull * 1024ull * 1024ull * 1024ull +
                   512ull * 1024ull * 1024ull);
    SX_REQUIRE(memory_status.ullAvailPhys <= memory_status.ullTotalPhys);

    constexpr uint32_t kXmem64KiBPages = 0x20400000u;
    void* page_aligned_reservation = virtual_alloc_x(
        nullptr, 64u * 1024u, MEM_RESERVE | kXmem64KiBPages, 0,
        PAGE_READWRITE);
    SX_REQUIRE(page_aligned_reservation != nullptr);
    SX_REQUIRE((reinterpret_cast<uintptr_t>(page_aligned_reservation) &
                0xFFFFu) == 0);
    SX_REQUIRE(VirtualFree(page_aligned_reservation, 0, MEM_RELEASE));

    wchar_t temporary_directory[MAX_PATH]{};
    wchar_t temporary_file[MAX_PATH]{};
    SX_REQUIRE(GetTempPathW(MAX_PATH, temporary_directory) != 0);
    SX_REQUIRE(GetTempFileNameW(temporary_directory, L"kxm", 0,
                               temporary_file) != 0);
    std::vector<uint8_t> mapping_contents(64u * 1024u, 0x37);
    const HANDLE file = CreateFileW(temporary_file, GENERIC_READ | GENERIC_WRITE,
                                    0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    SX_REQUIRE(file != INVALID_HANDLE_VALUE);
    DWORD file_bytes_written = 0;
    SX_REQUIRE(WriteFile(file, mapping_contents.data(),
                         static_cast<DWORD>(mapping_contents.size()),
                         &file_bytes_written, nullptr));
    SX_REQUIRE(file_bytes_written == mapping_contents.size());
    void* mapped_title_memory = map_title_memory(
        file, 0, mapping_contents.size(), nullptr, PAGE_READONLY);
    SX_REQUIRE(mapped_title_memory != nullptr);
    SX_REQUIRE(static_cast<const uint8_t*>(mapped_title_memory)[0] == 0x37);
    SX_REQUIRE(unmap_title_memory(mapped_title_memory));
    SX_REQUIRE(CloseHandle(file));
    SX_REQUIRE(DeleteFileW(temporary_file));

    using QueryTopologyFn = BOOL(WINAPI*)(
        LOGICAL_PROCESSOR_RELATIONSHIP,
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
        PDWORD);
    const auto query_topology = reinterpret_cast<QueryTopologyFn>(
        kernelx_symbols.LookupSymbol(
            "kernelx.dll", "GetLogicalProcessorInformationEx"));
    DWORD required_topology_bytes = 0;
    SX_REQUIRE(!query_topology(RelationProcessorCore, nullptr,
                               &required_topology_bytes));
    SX_REQUIRE(GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    SX_REQUIRE(required_topology_bytes > 0);
    std::vector<uint8_t> topology(required_topology_bytes);
    DWORD topology_bytes = required_topology_bytes;
    SX_REQUIRE(query_topology(
        RelationProcessorCore,
        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(topology.data()),
        &topology_bytes));
    size_t core_records = 0;
    constexpr size_t minimum_core_record_size =
        offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor) +
        offsetof(PROCESSOR_RELATIONSHIP, GroupMask) + sizeof(GROUP_AFFINITY);
    for (size_t offset = 0; offset < topology_bytes;) {
        const auto* record =
            reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                topology.data() + offset);
        SX_REQUIRE(record->Relationship == RelationProcessorCore);
        SX_REQUIRE(record->Size >= minimum_core_record_size);
        SX_REQUIRE(offset + record->Size <= topology_bytes);
        SX_REQUIRE(record->Processor.GroupCount == 1);
        SX_REQUIRE(std::popcount(static_cast<uint64_t>(
                       record->Processor.GroupMask[0].Mask)) == 2);
        ++core_records;
        offset += record->Size;
    }
    SX_REQUIRE(core_records == 8);
#endif

    scarlettx::DllRedirectionTable redirects;
    std::string redirected_target;
    bool is_hle = true;
    SX_REQUIRE(redirects.Resolve("KERNELX.DLL", redirected_target, is_hle));
    SX_REQUIRE(redirected_target == "kernel32.dll");
    SX_REQUIRE(!is_hle);
    SX_REQUIRE(redirects.Resolve("dstorage_xs.dll", redirected_target, is_hle));
    SX_REQUIRE(redirected_target == "dstorage_xs.dll");
    SX_REQUIRE(is_hle);
    SX_REQUIRE(redirects.Resolve("gameinput.dll", redirected_target, is_hle));
    SX_REQUIRE(redirected_target == "GameInput.dll" && is_hle);
    SX_REQUIRE(redirects.Resolve("xaudio2_9redist.dll", redirected_target, is_hle));
    SX_REQUIRE(redirected_target == "xaudio2_9.dll" && is_hle);
    SX_REQUIRE(redirects.Resolve("xgameruntime.dll", redirected_target, is_hle));
    SX_REQUIRE(redirected_target == "xgameruntime.dll" && is_hle);
    SX_REQUIRE(redirects.Resolve("xcrdapi.dll", redirected_target, is_hle));
    SX_REQUIRE(redirected_target == "xcrdapi.dll" && is_hle);
    SX_REQUIRE(redirects.Resolve("mfplat.dll", redirected_target, is_hle));
    SX_REQUIRE(redirected_target == "mfplat.dll" && !is_hle);
    SX_REQUIRE(redirects.Resolve("mfreadwrite.dll", redirected_target, is_hle));
    SX_REQUIRE(redirected_target == "mfreadwrite.dll" && !is_hle);

    const std::filesystem::path rt_exe =
        std::filesystem::path(SCARLETTX_BINARY_DIR) / "packages" / "rt_showcase" / "rt_showcase.exe";
    SX_REQUIRE(std::filesystem::exists(rt_exe));

    scarlettx::PeImageInfo info;
    auto st = scarlettx::InspectPeFile(rt_exe.string(), redirects, info);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(info.is_pe64);
    SX_REQUIRE(!info.sections.empty());
    SX_REQUIRE(!info.imports.empty());

    bool found_d3d12_xs = false;
    bool found_xgr = false;
    bool found_bvh = false;
    for (const auto& mod : info.imports) {
        const std::string name = ToLower(mod.dll_name);
        if (name == "d3d12_xs.dll") {
            found_d3d12_xs = true;
            SX_REQUIRE(mod.is_scarlett_hle);
            SX_REQUIRE(!mod.symbols.empty());
        } else if (name == "xgameruntime.dll") {
            found_xgr = true;
            SX_REQUIRE(mod.is_scarlett_hle);
        } else if (name == "xboxbvhbuilder.dll") {
            found_bvh = true;
            SX_REQUIRE(mod.is_scarlett_hle);
        }
    }

    SX_REQUIRE(found_d3d12_xs);
    SX_REQUIRE(found_xgr);
    SX_REQUIRE(found_bvh);

    std::cout << "[test_pe_loader] PASSED (imports=" << info.imports.size() << ")\n";
    return 0;
}
