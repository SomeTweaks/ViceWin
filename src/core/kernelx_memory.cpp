#include "scarlettx/kernel/kernelx_hle.h"

#include "core/logger.h"
#include "scarlettx/loader/pe_mapper.h"
#include "scarlettx/core/hle_trace.h"

#include <cstring>
#include <cstdio>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <cstdlib>
#endif

namespace scarlettx::kernelx {
namespace {

constexpr size_t kTitlePageSize = 64u * 1024u;

std::mutex& AllocationMutex() {
    static std::mutex mutex;
    return mutex;
}

struct AllocationRecord {
    size_t size;
    uint32_t memory_type;
};

std::unordered_map<void*, AllocationRecord>& Allocations() {
    static std::unordered_map<void*, AllocationRecord> allocations;
    return allocations;
}

uint64_t TrackedAllocationBytes() {
    std::lock_guard<std::mutex> lock(AllocationMutex());
    uint64_t total = 0;
    for (const auto& allocation : Allocations()) {
        const uint64_t size = allocation.second.size;
        total = size > std::numeric_limits<uint64_t>::max() - total
                    ? std::numeric_limits<uint64_t>::max()
                    : total + size;
    }
    return total;
}

bool RoundUp(size_t value, size_t alignment, size_t& result) {
    if (value > std::numeric_limits<size_t>::max() - (alignment - 1)) {
        return false;
    }
    result = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

#if defined(_WIN32)
DWORD FileMapAccess(DWORD protection) {
    switch (protection & 0xFFu) {
        case PAGE_READONLY:
            return FILE_MAP_READ;
        case PAGE_READWRITE:
            return FILE_MAP_READ | FILE_MAP_WRITE;
        case PAGE_WRITECOPY:
            return FILE_MAP_COPY;
        case PAGE_EXECUTE_READ:
            return FILE_MAP_READ | FILE_MAP_EXECUTE;
        case PAGE_EXECUTE_READWRITE:
            return FILE_MAP_READ | FILE_MAP_WRITE | FILE_MAP_EXECUTE;
        case PAGE_EXECUTE_WRITECOPY:
            return FILE_MAP_COPY | FILE_MAP_EXECUTE;
        default:
            SetLastError(ERROR_INVALID_PARAMETER);
            return 0;
    }
}
#endif

}  // namespace

void* XMemAlloc(size_t size, uint64_t attributes) {
    const uint32_t memory_type = static_cast<uint32_t>((attributes >> 29) & 0xFu);
    if (memory_type > 12) {
#if defined(_WIN32)
        SetLastError(ERROR_INVALID_PARAMETER);
#endif
        LogMessage(LogLevel::Error, "kernelx_memory",
                   "XMemAlloc received an unsupported memory type");
        return nullptr;
    }
    if (size == 0) {
        size = 1;
    }
    size_t reserved_size = 0;
    if (!RoundUp(size, kTitlePageSize, reserved_size)) {
        return nullptr;
    }

#if defined(_WIN32)
    void* memory = VirtualAlloc(nullptr, reserved_size, MEM_RESERVE | MEM_COMMIT,
                                PAGE_READWRITE);
#else
    void* memory = nullptr;
    const int allocation_error =
        posix_memalign(&memory, kTitlePageSize, reserved_size);
    if (allocation_error != 0) {
        memory = nullptr;
    }
#endif
    if (!memory) {
#if defined(_WIN32)
        const std::string message =
            "VirtualAlloc failed for " + std::to_string(reserved_size) +
            " bytes (GetLastError=" + std::to_string(GetLastError()) + ")";
#else
        const std::string message =
            "posix_memalign failed for XMemAlloc with error " +
            std::to_string(allocation_error);
#endif
        LogMessage(LogLevel::Error, "kernelx_memory", message);
        return nullptr;
    }
    std::memset(memory, 0, reserved_size);
    try {
        std::lock_guard<std::mutex> lock(AllocationMutex());
        Allocations().emplace(memory, AllocationRecord{reserved_size, memory_type});
    } catch (const std::bad_alloc&) {
#if defined(_WIN32)
        VirtualFree(memory, 0, MEM_RELEASE);
#else
        std::free(memory);
#endif
        LogMessage(LogLevel::Error, "kernelx_memory",
                   "Could not track an XMemAlloc allocation");
        HleTrace("XMemAlloc(failed)", 0, size, attributes);
        return nullptr;
    }
    HleTrace("XMemAlloc", reinterpret_cast<uint64_t>(memory), size, attributes);
    return memory;
}

void XMemFree(void* address, uint64_t /*attributes*/) {
    if (!address) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(AllocationMutex());
        const auto it = Allocations().find(address);
        if (it == Allocations().end()) {
            LogMessage(LogLevel::Error, "kernelx_memory",
                       "XMemFree received an address not allocated by XMemAlloc");
            return;
        }
        Allocations().erase(it);
    }
#if defined(_WIN32)
    if (!VirtualFree(address, 0, MEM_RELEASE)) {
        LogMessage(LogLevel::Error, "kernelx_memory",
                   "VirtualFree failed for an XMemAlloc allocation");
    }
#else
    std::free(address);
#endif
}

void* XMemSet(void* destination, int value, size_t size) {
    if (!destination && size != 0) {
        return nullptr;
    }
    return size == 0 ? destination : std::memset(destination, value, size);
}

void* XMemCpy(void* destination, const void* source, size_t size) {
    if ((!destination || !source) && size != 0) {
        return nullptr;
    }
    return size == 0 ? destination : std::memcpy(destination, source, size);
}

namespace {

std::mutex& PhysicalPagesMutex() {
    static std::mutex mutex;
    return mutex;
}

// Aligned block -> reservation base, for the over-aligned case where the
// handed-out pointer is not the VirtualAlloc base.
std::unordered_map<void*, void*>& PhysicalPageBases() {
    static std::unordered_map<void*, void*> bases;
    return bases;
}

}  // namespace

void* XMemAllocatePhysicalPages(size_t size, size_t alignment) {
    if (size == 0) {
        size = 1;
    }
    size_t resolved_alignment = alignment != 0 ? alignment : 0x10000ull;
    if (resolved_alignment < 0x1000ull) {
        resolved_alignment = 0x1000ull;
    }
    size_t rounded = 0;
    if (!RoundUp(size, resolved_alignment, rounded)) {
        return nullptr;
    }

#if defined(_WIN32)
    // Win32 cannot hand out physically contiguous pages. A 64 KiB-aligned
    // commit is the closest semantic match: the guest only maps and transfers
    // through it, and XMemMapPhysicalPages keeps the address unchanged.
    if (resolved_alignment <= 0x10000ull) {
        void* memory = VirtualAlloc(nullptr, rounded, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_READWRITE);
        if (memory) {
            std::memset(memory, 0, rounded);
        }
        return memory;
    }
    // Alignments above 64 KiB need an over-reservation whose aligned interior
    // is what the guest sees; the base is remembered so the free path works.
    void* reservation = VirtualAlloc(nullptr, rounded + resolved_alignment,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!reservation) {
        return nullptr;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(reservation);
    const uintptr_t aligned =
        (base + resolved_alignment - 1) & ~(resolved_alignment - 1);
    void* memory = reinterpret_cast<void*>(aligned);
    std::memset(memory, 0, rounded);
    {
        std::lock_guard<std::mutex> lock(PhysicalPagesMutex());
        PhysicalPageBases().emplace(memory, reservation);
    }
    return memory;
#else
    void* memory = nullptr;
    if (posix_memalign(&memory, resolved_alignment, rounded) != 0) {
        return nullptr;
    }
    std::memset(memory, 0, rounded);
    return memory;
#endif
}

void XMemFreePhysicalPages(void* address, size_t /*size*/) {
    if (!address) {
        return;
    }
    void* base = address;
    {
        std::lock_guard<std::mutex> lock(PhysicalPagesMutex());
        auto& bases = PhysicalPageBases();
        const auto it = bases.find(address);
        if (it != bases.end()) {
            base = it->second;
            bases.erase(it);
        }
    }
#if defined(_WIN32)
    VirtualFree(base, 0, MEM_RELEASE);
#else
    std::free(base);
#endif
}

void* XMemMapPhysicalPages(void* physical_address_base, size_t /*virtual_size*/) {
    // "Physical" pages are ordinary commits here, so the mapping is the
    // identity and the base the guest passed in is already valid.
    return physical_address_base;
}

void* XMemMapPhysicalPagesScatter(void** physical_page_array,
                                  size_t number_of_pages,
                                  size_t /*virtual_size*/) {
    if (!physical_page_array || number_of_pages == 0) {
        return nullptr;
    }
    // The first page is contiguous with the rest because the allocator handed
    // out one block per call; returning its base keeps the identity mapping.
    return physical_page_array[0];
}

void XMemTransferMemory(void* destination, const void* source, size_t size) {
    if (size != 0 && destination && source) {
        std::memcpy(destination, source, size);
    }
}

void* XMemVirtualAlloc(void* address, size_t size, uint32_t allocation_type,
                       uint32_t protect, uint64_t xmem_flags) {
#if defined(_WIN32)
    // Two call sites in this title disagree about the last two arguments: one
    // passes 3 / 0x801, the other 4 (PAGE_READWRITE) on the stack. Neither order
    // is knowable from the header, so pick whichever value is a real PAGE_*
    // code and default to PAGE_READWRITE when neither is.
    const auto valid_protect = [](uint64_t value) {
        switch (value) {
            case PAGE_NOACCESS: case PAGE_READONLY: case PAGE_READWRITE:
            case PAGE_WRITECOPY: case PAGE_EXECUTE: case PAGE_EXECUTE_READ:
            case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
                return true;
            default:
                return false;
        }
    };
    DWORD safe_protect = PAGE_READWRITE;
    if (valid_protect(protect)) {
        safe_protect = protect;
    } else if (valid_protect(xmem_flags)) {
        safe_protect = static_cast<DWORD>(xmem_flags);
    }
    // The allocation type may arrive in either of the two words as well.
    constexpr DWORD kSafeType = MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN;
    DWORD type_word = allocation_type;
    if ((type_word & (MEM_COMMIT | MEM_RESERVE | MEM_RESET)) == 0) {
        const uint32_t other = static_cast<uint32_t>(xmem_flags);
        if ((other & (MEM_COMMIT | MEM_RESERVE | MEM_RESET)) != 0) {
            type_word = other;
        }
    }
    constexpr DWORD kSafe = MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN;
    DWORD sanitized = type_word & kSafe;
    if (sanitized == 0) {
        sanitized = MEM_COMMIT | MEM_RESERVE;
    }
    if (size != 0) {
        size = (size + 0xFFFFull) & ~0xFFFFull;
    }
    void* result = ::VirtualAlloc(address, size, sanitized, safe_protect);
    if (!result && address != nullptr) {
        result = ::VirtualAlloc(nullptr, size, sanitized, safe_protect);
    }
    if (sanitized != allocation_type) {
        static std::atomic<uint32_t> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
            char text[128] = {};
            std::snprintf(text, sizeof(text),
                          "XMemVirtualAlloc stripped console allocation bits "
                          "(0x%lX -> 0x%lX)",
                          static_cast<unsigned long>(allocation_type),
                          static_cast<unsigned long>(sanitized));
            LogMessage(LogLevel::Info, "kernelx_memory", text);
        }
    }
    (void)xmem_flags;
    return result;
#else
    (void)xmem_flags;
    (void)allocation_type;
    (void)protect;
    if (size == 0 || address) {
        return nullptr;
    }
    void* memory = nullptr;
    if (posix_memalign(&memory, 0x1000, size) != 0) {
        return nullptr;
    }
    return memory;
#endif
}

int32_t XMemGetWorkingSetStatistics(uint32_t /*kind*/, void* statistics) {
    if (!statistics) {
        return 0;
    }
#if defined(_WIN32)
    // Resolved at runtime from kernel32 so the core library does not have to
    // link psapi.lib just for this one query.
    using GetProcessMemoryInfoFn = BOOL(WINAPI*)(HANDLE, PROCESS_MEMORY_COUNTERS*, DWORD);
    static const GetProcessMemoryInfoFn query = []() -> GetProcessMemoryInfoFn {
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (!kernel) {
            return nullptr;
        }
        return reinterpret_cast<GetProcessMemoryInfoFn>(
            reinterpret_cast<void*>(GetProcAddress(kernel, "K32GetProcessMemoryInfo")));
    }();
    PROCESS_MEMORY_COUNTERS counters{};
    if (!query || !query(GetCurrentProcess(), &counters, sizeof(counters))) {
        return 0;
    }
    auto* bytes = static_cast<uint8_t*>(statistics);
    std::memset(bytes, 0, 16);
    const uint64_t working_set = counters.WorkingSetSize;
    std::memcpy(bytes + 8, &working_set, sizeof(working_set));
    return 1;
#else
    auto* bytes = static_cast<uint8_t*>(statistics);
    std::memset(bytes, 0, 16);
    return 1;
#endif
}

bool XMemIsToolsMemAvailable() {
    // No tools memory is carved out on the host; titles use this to decide
    // whether to take the profiling path, and false keeps them on the retail
    // path they ship with.
    return false;
}

int32_t XMemGetAllocationStatistics(
    uint32_t memory_type,
    int32_t alternate_pool,
    XMemAllocationStatistics* statistics) {
    if (!statistics || statistics->size != 0x20u) {
#if defined(_WIN32)
        SetLastError(ERROR_BAD_LENGTH);
#endif
        return 0;
    }
    if (memory_type > 15) {
#if defined(_WIN32)
        SetLastError(ERROR_INVALID_PARAMETER);
#endif
        statistics->allocated_bytes = 0;
        statistics->cached_bytes = 0;
        statistics->allocation_count = 0;
        statistics->released_pages = 0;
#if defined(_WIN32)
        SetLastError(ERROR_SUCCESS);
#endif
        return 1;
    }

    uint64_t allocated_bytes = 0;
    uint32_t allocation_count = 0;
    if (alternate_pool == 0) {
        std::lock_guard<std::mutex> lock(AllocationMutex());
        for (const auto& allocation : Allocations()) {
            if (allocation.second.memory_type != memory_type) {
                continue;
            }
            const uint64_t size = allocation.second.size;
            allocated_bytes = size > std::numeric_limits<uint64_t>::max() -
                                         allocated_bytes
                                  ? std::numeric_limits<uint64_t>::max()
                                  : allocated_bytes + size;
            if (allocation_count != std::numeric_limits<uint32_t>::max()) {
                ++allocation_count;
            }
        }
    }

    statistics->allocated_bytes = allocated_bytes;
    statistics->cached_bytes = 0;
    statistics->allocation_count = allocation_count;
    statistics->released_pages = 0;
#if defined(_WIN32)
    SetLastError(ERROR_SUCCESS);
#endif
    return 1;
}

#if defined(_WIN32)
namespace {

BOOL WINAPI TitleGlobalMemoryStatusEx(MEMORYSTATUSEX* status) {
    if (!status || status->dwLength < sizeof(MEMORYSTATUSEX)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    MEMORYSTATUSEX host_status{};
    host_status.dwLength = static_cast<DWORD>(sizeof(host_status));
    if (!::GlobalMemoryStatusEx(&host_status)) {
        return FALSE;
    }

    constexpr uint64_t kTitleMemoryBytes =
        13ull * 1024ull * 1024ull * 1024ull + 512ull * 1024ull * 1024ull;
    const uint64_t allocated_bytes = TrackedAllocationBytes();
    const uint64_t title_available =
        allocated_bytes >= kTitleMemoryBytes ? 0 : kTitleMemoryBytes - allocated_bytes;
    host_status.ullTotalPhys = kTitleMemoryBytes;
    host_status.ullAvailPhys =
        host_status.ullAvailPhys < title_available ? host_status.ullAvailPhys
                                                   : title_available;
    host_status.dwMemoryLoad = static_cast<DWORD>(
        ((kTitleMemoryBytes - host_status.ullAvailPhys) * 100) / kTitleMemoryBytes);
    *status = host_status;
    return TRUE;
}

}  // namespace
#endif

void* VirtualAllocX(void* address,
                    size_t size,
                    uint32_t allocation_type,
                    uint64_t /*xmem_flags*/,
                    uint32_t protection) {
#if defined(_WIN32)
    if (size == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    constexpr DWORD kLargePages = 0x20000000u;
    constexpr DWORD kSixtyFourKiBPages = 0x00400000u;
    const DWORD console_flags = allocation_type;
    size_t page_size = 0x1000;
    if ((console_flags & (kLargePages | kSixtyFourKiBPages)) ==
        (kLargePages | kSixtyFourKiBPages)) {
        page_size = kTitlePageSize;
    } else if ((console_flags & kLargePages) != 0) {
        page_size = 2u * 1024u * 1024u;
    }

    DWORD host_flags = console_flags & ~(kLargePages | kSixtyFourKiBPages);
    if (host_flags == 0) {
        host_flags = MEM_RESERVE | MEM_COMMIT;
    }
    if (!address && (host_flags & MEM_COMMIT) != 0) {
        host_flags |= MEM_RESERVE;
    }
    if (!protection) {
        protection = PAGE_READWRITE;
    }

    size_t rounded_size = 0;
    if (!RoundUp(size, page_size, rounded_size)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (address) {
        if ((reinterpret_cast<uintptr_t>(address) & (page_size - 1)) != 0) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return nullptr;
        }
        return VirtualAlloc(address, rounded_size, host_flags, protection);
    }
    if (page_size <= kTitlePageSize) {
        return VirtualAlloc(nullptr, rounded_size, host_flags, protection);
    }

    if (rounded_size > std::numeric_limits<size_t>::max() - page_size) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        void* probe = VirtualAlloc(nullptr, rounded_size + page_size,
                                   MEM_RESERVE, PAGE_NOACCESS);
        if (!probe) {
            return nullptr;
        }
        const uintptr_t aligned =
            (reinterpret_cast<uintptr_t>(probe) + page_size - 1) & ~(page_size - 1);
        VirtualFree(probe, 0, MEM_RELEASE);
        if (void* memory = VirtualAlloc(reinterpret_cast<void*>(aligned), rounded_size,
                                        host_flags, protection)) {
            return memory;
        }
    }
    return nullptr;
#else
    (void)address;
    (void)size;
    (void)allocation_type;
    (void)protection;
    return nullptr;
#endif
}

void* MapTitleMemory(void* file_handle,
                     uint64_t offset,
                     size_t size,
                     void* base_address,
                     uint32_t protection) {
#if defined(_WIN32)
    if (size == 0 || (offset & (kTitlePageSize - 1)) != 0 ||
        (base_address &&
         (reinterpret_cast<uintptr_t>(base_address) & (kTitlePageSize - 1)) != 0)) {
        SetLastError(ERROR_MAPPED_ALIGNMENT);
        return nullptr;
    }
    if (offset > std::numeric_limits<uint64_t>::max() - size) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const DWORD access = FileMapAccess(protection);
    if (access == 0) {
        return nullptr;
    }

    HANDLE backing_file = file_handle
                              ? static_cast<HANDLE>(file_handle)
                              : INVALID_HANDLE_VALUE;
    const uint64_t mapping_size = offset + size;
    HANDLE mapping = CreateFileMappingW(
        backing_file, nullptr, protection,
        static_cast<DWORD>(mapping_size >> 32),
        static_cast<DWORD>(mapping_size & 0xFFFFFFFFu), nullptr);
    if (!mapping) {
        return nullptr;
    }
    void* view = MapViewOfFileEx(
        mapping, access, static_cast<DWORD>(offset >> 32),
        static_cast<DWORD>(offset & 0xFFFFFFFFu), size, base_address);
    const DWORD map_error = view ? ERROR_SUCCESS : GetLastError();
    CloseHandle(mapping);
    if (!view) {
        SetLastError(map_error);
    }
    return view;
#else
    (void)file_handle;
    (void)offset;
    (void)size;
    (void)base_address;
    (void)protection;
    return nullptr;
#endif
}

bool UnmapTitleMemory(void* address) {
#if defined(_WIN32)
    if (!address) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return false;
    }
    return UnmapViewOfFile(address) != FALSE;
#else
    (void)address;
    return false;
#endif
}

namespace {

// A retail title that decides "cannot continue" calls ExitProcess and the host
// evaporates without a trace. These hooks log the code first, which is the only
// evidence of *why* the guest bailed out.
#if defined(_WIN32)
void WINAPI GuestExitProcessHook(UINT exit_code) {
    LogMessage(LogLevel::Error, "kernelx",
               "guest called ExitProcess(" + std::to_string(exit_code) + ")");
    ::ExitProcess(exit_code);
}

BOOL WINAPI GuestTerminateProcessHook(HANDLE process, UINT exit_code) {
    LogMessage(LogLevel::Error, "kernelx",
               "guest called TerminateProcess(" + std::to_string(exit_code) + ")");
    return ::TerminateProcess(process, exit_code);
}

void WINAPI GuestExitThreadHook(DWORD exit_code) {
    LogMessage(LogLevel::Warning, "kernelx",
               "guest called ExitThread(" + std::to_string(exit_code) + ")");
    ::ExitThread(exit_code);
}
#endif

}  // namespace

// GetProcAddress is how late-bound entry points are found; a null result there
// is a common precursor to the title's null-pointer assert.
#if defined(_WIN32)
// The console passes Xbox-only allocation bits (observed: 0x20402000) in
// VirtualAlloc's flAllocationType. Win32 rejects the whole call, the title gets
// NULL, asserts and then writes through the null object. Strip everything that
// is not a documented Win32 flag and keep the rest intact.
LPVOID WINAPI GuestVirtualAllocHook(LPVOID address, SIZE_T size, DWORD type,
                                    DWORD protect) {
    // Only MEM_COMMIT / MEM_RESERVE / MEM_RESET / MEM_TOP_DOWN are safe to pass
    // through. The console adds MEM_64K_PAGES (0x80000), MEM_PHYSICAL
    // (0x400000) and MEM_LARGE_PAGES (0x20000000), and Win32 rejects the whole
    // request, which the title turns into a null-pointer assert.
    constexpr DWORD kSafe = MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN;
    DWORD sanitized = type & kSafe;
    if (sanitized == 0) {
        sanitized = MEM_COMMIT | MEM_RESERVE;
    }
    const bool had_console_bits = sanitized != type;
    if (size != 0) {
        size = (size + 0xFFFFull) & ~0xFFFFull;  // 64 KiB granularity
    }
    const DWORD sanitized_protect = protect & 0xFFu;

    LPVOID result = ::VirtualAlloc(address, size, sanitized, sanitized_protect);
    if (!result && address != nullptr) {
        // Console code often passes a preferred address that the host already
        // uses. Retry without the hint before reporting failure.
        result = ::VirtualAlloc(nullptr, size, sanitized, sanitized_protect);
        char text[128] = {};
        std::snprintf(text, sizeof(text),
                      "VirtualAlloc at preferred address 0x%llX failed; retried "
                      "without it -> 0x%llX",
                      reinterpret_cast<unsigned long long>(address),
                      reinterpret_cast<unsigned long long>(result));
        LogMessage(result ? LogLevel::Info : LogLevel::Error, "kernelx", text);
    }
    if (had_console_bits) {
        static std::atomic<uint32_t> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
            char text[128] = {};
            std::snprintf(text, sizeof(text),
                          "VirtualAlloc type 0x%lX carried console-only bits; "
                          "passed 0x%lX",
                          static_cast<unsigned long>(type),
                          static_cast<unsigned long>(sanitized));
            LogMessage(LogLevel::Info, "kernelx", text);
        }
    }
    HleTrace(result ? "VirtualAlloc" : "VirtualAlloc(failed)",
             reinterpret_cast<uint64_t>(result), size, type);
    return result;
}

BOOL WINAPI GuestVirtualFreeHook(LPVOID address, SIZE_T size, DWORD type) {
    const BOOL result = ::VirtualFree(address, size, type);
    HleTrace(result ? "VirtualFree" : "VirtualFree(failed)",
             static_cast<uint64_t>(result), size, type);
    return result;
}

// A query that answers 0 makes the title believe the page is not there and it
// then walks into it. Report our allocations (and anything committed) as
// MEM_COMMIT/PAGE_READWRITE over a plausible region.
SIZE_T WINAPI GuestVirtualQueryExHook(HANDLE process, LPCVOID address,
                                      PMEMORY_BASIC_INFORMATION info,
                                      SIZE_T length) {
    if (!info || length < sizeof(MEMORY_BASIC_INFORMATION)) {
        return ::VirtualQueryEx(process, address, info, length);
    }
    const SIZE_T result = ::VirtualQueryEx(process, address, info, length);
    if (result != 0) {
        return result;
    }
    std::memset(info, 0, sizeof(*info));
    info->BaseAddress = const_cast<void*>(address);
    info->AllocationBase = const_cast<void*>(address);
    info->AllocationProtect = PAGE_READWRITE;
    info->RegionSize = 0x10000;
    info->State = MEM_COMMIT;
    info->Protect = PAGE_READWRITE;
    info->Type = MEM_PRIVATE;
    HleTrace("VirtualQueryEx(synthesised)",
             reinterpret_cast<uint64_t>(address), 0, 0);
    return sizeof(MEMORY_BASIC_INFORMATION);
}

SIZE_T WINAPI GuestVirtualQueryHook(LPCVOID address,
                                    PMEMORY_BASIC_INFORMATION info,
                                    SIZE_T length) {
    return GuestVirtualQueryExHook(GetCurrentProcess(), address, info, length);
}

FARPROC WINAPI GuestGetProcAddressHook(HMODULE module, LPCSTR name) {
    // The title delay-loads kernel32 by name, so this is the only place where
    // an override can take effect for those imports.
    static const std::unordered_map<std::string, void*> overrides = {
        {"VirtualAlloc", reinterpret_cast<void*>(&GuestVirtualAllocHook)},
    };
    if (name) {
        const auto it = overrides.find(name);
        if (it != overrides.end()) {
            HleTrace("GetProcAddress(override)",
                     reinterpret_cast<uint64_t>(it->second),
                     reinterpret_cast<uint64_t>(module),
                     reinterpret_cast<uint64_t>(name));
            return reinterpret_cast<FARPROC>(it->second);
        }
    }
    const FARPROC proc = ::GetProcAddress(module, name);
    HleTrace(proc ? "GetProcAddress" : "GetProcAddress(miss)",
             reinterpret_cast<uint64_t>(proc),
             reinterpret_cast<uint64_t>(module),
             reinterpret_cast<uint64_t>(name));
    return proc;
}
#endif

void RegisterKernelxMemorySymbols(HleSymbolRegistry& registry) {
    registry.RegisterSymbol("kernelx.dll", "XMemAlloc",
                            reinterpret_cast<void*>(&XMemAlloc));
    registry.RegisterSymbol("kernelx.dll", "XMemFree",
                            reinterpret_cast<void*>(&XMemFree));
    registry.RegisterSymbol("kernelx.dll", "XMemSet",
                            reinterpret_cast<void*>(&XMemSet));
    registry.RegisterSymbol("kernelx.dll", "XMemCpy",
                            reinterpret_cast<void*>(&XMemCpy));
    registry.RegisterSymbol("kernelx.dll", "XMemGetAllocationStatistics",
                            reinterpret_cast<void*>(&XMemGetAllocationStatistics));
    registry.RegisterSymbol("xmem.dll", "XMemGetAllocationStatistics",
                            reinterpret_cast<void*>(&XMemGetAllocationStatistics));
    // The retail dump imports every allocator entry point from xmem.dll, not
    // from kernelx.dll; register the whole surface under that name as well.
    registry.RegisterSymbol("xmem.dll", "XMemAlloc",
                            reinterpret_cast<void*>(&XMemAlloc));
    registry.RegisterSymbol("xmem.dll", "XMemFree",
                            reinterpret_cast<void*>(&XMemFree));
    registry.RegisterSymbol("xmem.dll", "XMemAllocDefault",
                            reinterpret_cast<void*>(&XMemAlloc));
    registry.RegisterSymbol("xmem.dll", "XMemFreeDefault",
                            reinterpret_cast<void*>(&XMemFree));
    registry.RegisterSymbol("xmem.dll", "XMemAllocatePhysicalPages",
                            reinterpret_cast<void*>(&XMemAllocatePhysicalPages));
    registry.RegisterSymbol("xmem.dll", "XMemFreePhysicalPages",
                            reinterpret_cast<void*>(&XMemFreePhysicalPages));
    registry.RegisterSymbol("xmem.dll", "XMemMapPhysicalPages",
                            reinterpret_cast<void*>(&XMemMapPhysicalPages));
    registry.RegisterSymbol("xmem.dll", "XMemMapPhysicalPagesScatter",
                            reinterpret_cast<void*>(&XMemMapPhysicalPagesScatter));
    registry.RegisterSymbol("xmem.dll", "XMemTransferMemory",
                            reinterpret_cast<void*>(&XMemTransferMemory));
    registry.RegisterSymbol("xmem.dll", "XMemVirtualAlloc",
                            reinterpret_cast<void*>(&XMemVirtualAlloc));
    registry.RegisterSymbol("xmem.dll", "XMemGetWorkingSetStatistics",
                            reinterpret_cast<void*>(&XMemGetWorkingSetStatistics));
    registry.RegisterSymbol("xmem.dll", "XMemIsToolsMemAvailable",
                            reinterpret_cast<void*>(&XMemIsToolsMemAvailable));
    registry.RegisterSymbol("xgameruntime.dll", "XMemGetAllocationStatistics",
                            reinterpret_cast<void*>(&XMemGetAllocationStatistics));
#if defined(_WIN32)
    registry.RegisterSymbol("kernel32.dll", "GlobalMemoryStatusEx",
                            reinterpret_cast<void*>(&TitleGlobalMemoryStatusEx));
    registry.RegisterSymbol("kernelx.dll", "GlobalMemoryStatusEx",
                            reinterpret_cast<void*>(&TitleGlobalMemoryStatusEx));
#endif
    registry.RegisterSymbol("kernelx.dll", "VirtualAllocX",
                            reinterpret_cast<void*>(&VirtualAllocX));
    registry.RegisterSymbol("kernelx.dll", "MapTitleMemory",
                            reinterpret_cast<void*>(&MapTitleMemory));
    registry.RegisterSymbol("kernelx.dll", "UnmapTitleMemory",
                            reinterpret_cast<void*>(&UnmapTitleMemory));
#if defined(_WIN32)
    // Exit paths: log first, then do exactly what the guest asked for.
    registry.RegisterSymbol("kernel32.dll", "ExitProcess",
                            reinterpret_cast<void*>(&GuestExitProcessHook));
    registry.RegisterSymbol("kernel32.dll", "TerminateProcess",
                            reinterpret_cast<void*>(&GuestTerminateProcessHook));
    registry.RegisterSymbol("kernel32.dll", "ExitThread",
                            reinterpret_cast<void*>(&GuestExitThreadHook));
    registry.RegisterSymbol("kernel32.dll", "GetProcAddress",
                            reinterpret_cast<void*>(&GuestGetProcAddressHook));
    registry.RegisterSymbol("kernel32.dll", "VirtualAlloc",
                            reinterpret_cast<void*>(&GuestVirtualAllocHook));
    registry.RegisterSymbol("kernelbase.dll", "VirtualAlloc",
                            reinterpret_cast<void*>(&GuestVirtualAllocHook));
    // The title imports this through the memory API set, so the registry lookup
    // has to hit on those names as well (the api-set redirect only rewrites the
    // load target, not the symbol name the lookup starts with).
    registry.RegisterSymbol("api-ms-win-core-memory-l1-1-0.dll", "VirtualAlloc",
                            reinterpret_cast<void*>(&GuestVirtualAllocHook));
    registry.RegisterSymbol("api-ms-win-core-memory-l1-1-1.dll", "VirtualAlloc",
                            reinterpret_cast<void*>(&GuestVirtualAllocHook));
    registry.RegisterSymbol("api-ms-win-core-memory-l1-1-2.dll", "VirtualAlloc",
                            reinterpret_cast<void*>(&GuestVirtualAllocHook));
    // Module-agnostic keys: they win regardless of which name the redirect
    // table produced, which is what the retail api-set imports needed.
    registry.RegisterSymbolAnyModule("VirtualAlloc",
                                     reinterpret_cast<void*>(&GuestVirtualAllocHook));
    registry.RegisterSymbolAnyModule("VirtualFree",
                                     reinterpret_cast<void*>(&GuestVirtualFreeHook));
    registry.RegisterSymbolAnyModule("VirtualQueryEx",
                                     reinterpret_cast<void*>(&GuestVirtualQueryExHook));
    registry.RegisterSymbolAnyModule("VirtualQuery",
                                     reinterpret_cast<void*>(&GuestVirtualQueryHook));
    for (const char* dll : {"kernel32.dll", "kernelbase.dll",
                            "api-ms-win-core-memory-l1-1-0.dll",
                            "api-ms-win-core-memory-l1-1-1.dll",
                            "api-ms-win-core-memory-l1-1-2.dll"}) {
        registry.RegisterSymbol(dll, "VirtualFree",
                                reinterpret_cast<void*>(&GuestVirtualFreeHook));
        registry.RegisterSymbol(dll, "VirtualQueryEx",
                                reinterpret_cast<void*>(&GuestVirtualQueryExHook));
        registry.RegisterSymbol(dll, "VirtualQuery",
                                reinterpret_cast<void*>(&GuestVirtualQueryHook));
    }
    LogMessage(LogLevel::Info, "kernelx",
               "VirtualAlloc/VirtualFree/VirtualQuery(Ex) sanitizers bound "
               "(any module + kernel32/kernelbase/api-ms-win-core-memory)");
#endif
}

}  // namespace scarlettx::kernelx
