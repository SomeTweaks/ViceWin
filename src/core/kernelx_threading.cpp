#include "scarlettx/kernel/kernelx_hle.h"

#include "core/logger.h"
#include "scarlettx/loader/pe_mapper.h"

#include <cstring>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace scarlettx::kernelx {
namespace {

constexpr uint32_t kXboxLogicalProcessorCount = 16;

#if defined(_WIN32)
uintptr_t CurrentProcessAffinityMask() {
    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
        return 0;
    }
    return static_cast<uintptr_t>(process_mask);
}

uint64_t MapHostMaskToXbox(uintptr_t host_mask, uintptr_t available_mask) {
    uint64_t xbox_mask = 0;
    uint32_t xbox_index = 0;
    for (uint32_t host_index = 0;
         host_index < sizeof(uintptr_t) * 8 &&
         xbox_index < kXboxLogicalProcessorCount;
         ++host_index) {
        const uintptr_t bit = uintptr_t{1} << host_index;
        if ((available_mask & bit) == 0) {
            continue;
        }
        if ((host_mask & bit) != 0) {
            xbox_mask |= uint64_t{1} << xbox_index;
        }
        ++xbox_index;
    }
    return xbox_mask;
}

BOOL WINAPI GetLogicalProcessorInformationExXbox(
    LOGICAL_PROCESSOR_RELATIONSHIP relationship,
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX information,
    PDWORD returned_length) {
    if (!returned_length) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (relationship != RelationProcessorCore && relationship != RelationAll) {
        return ::GetLogicalProcessorInformationEx(relationship, information,
                                                  returned_length);
    }

    constexpr DWORD kXboxCoreCount = 8;
    constexpr size_t kRecordSize =
        offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor) +
        offsetof(PROCESSOR_RELATIONSHIP, GroupMask) + sizeof(GROUP_AFFINITY);
    constexpr DWORD kRequiredLength = static_cast<DWORD>(kXboxCoreCount * kRecordSize);
    if (!information || *returned_length < kRequiredLength) {
        *returned_length = kRequiredLength;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    auto* output = reinterpret_cast<uint8_t*>(information);
    for (DWORD core = 0; core < kXboxCoreCount; ++core) {
        auto* record = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
            output + core * kRecordSize);
        std::memset(record, 0, kRecordSize);
        record->Relationship = RelationProcessorCore;
        record->Size = static_cast<DWORD>(kRecordSize);
        record->Processor.Flags = LTP_PC_SMT;
        record->Processor.GroupCount = 1;
        record->Processor.GroupMask[0].Group = 0;
        record->Processor.GroupMask[0].Mask = KAFFINITY{3} << (core * 2);
    }
    *returned_length = kRequiredLength;
    return TRUE;
}

DWORD_PTR WINAPI SetThreadAffinityMaskXbox(HANDLE thread, DWORD_PTR xbox_mask) {
    const uintptr_t available_mask = CurrentProcessAffinityMask();
    const uintptr_t host_mask =
        MapXboxThreadMaskToHost(static_cast<uint64_t>(xbox_mask), available_mask);
    if (host_mask == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const DWORD_PTR previous = ::SetThreadAffinityMask(
        thread, static_cast<DWORD_PTR>(host_mask));
    if (previous == 0) {
        return 0;
    }
    return static_cast<DWORD_PTR>(
        MapHostMaskToXbox(static_cast<uintptr_t>(previous), available_mask));
}

DWORD WINAPI SetThreadIdealProcessorXbox(HANDLE thread, DWORD xbox_processor) {
    const uintptr_t available_mask = CurrentProcessAffinityMask();
    const uint32_t host_processor =
        MapXboxProcessorIndexToHost(xbox_processor, available_mask);
    if (host_processor == std::numeric_limits<uint32_t>::max()) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return MAXDWORD;
    }
    const DWORD previous = ::SetThreadIdealProcessor(thread, host_processor);
    if (previous == MAXDWORD) {
        return MAXDWORD;
    }
    if (previous >= sizeof(uintptr_t) * 8) {
        return MAXDWORD;
    }
    const uint64_t previous_mask = MapHostMaskToXbox(
        uintptr_t{1} << previous, available_mask);
    for (uint32_t index = 0; index < 64; ++index) {
        if ((previous_mask & (uint64_t{1} << index)) != 0) {
            return index;
        }
    }
    return MAXDWORD;
}
#else
bool GetLogicalProcessorInformationExXbox(
    uint32_t,
    void*,
    uint32_t*) {
    return false;
}

uintptr_t SetThreadAffinityMaskXbox(void*, uintptr_t) {
    return 0;
}

uint32_t SetThreadIdealProcessorXbox(void*, uint32_t) {
    return std::numeric_limits<uint32_t>::max();
}
#endif

}  // namespace

uintptr_t MapXboxThreadMaskToHost(uint64_t xbox_mask,
                                 uintptr_t host_available_mask) noexcept {
    if (xbox_mask == 0 ||
        (xbox_mask >> kXboxLogicalProcessorCount) != 0) {
        return 0;
    }
    uintptr_t host_mask = 0;
    uint32_t xbox_index = 0;
    for (uint32_t host_index = 0;
         host_index < sizeof(uintptr_t) * 8 &&
         xbox_index < kXboxLogicalProcessorCount;
         ++host_index) {
        const uintptr_t host_bit = uintptr_t{1} << host_index;
        if ((host_available_mask & host_bit) == 0) {
            continue;
        }
        if ((xbox_mask & (uint64_t{1} << xbox_index)) != 0) {
            host_mask |= host_bit;
        }
        ++xbox_index;
    }
    return host_mask;
}

uint32_t MapXboxProcessorIndexToHost(uint32_t xbox_index,
                                     uintptr_t host_available_mask) noexcept {
    if (xbox_index >= kXboxLogicalProcessorCount) {
        return std::numeric_limits<uint32_t>::max();
    }
    uint32_t current_xbox_index = 0;
    for (uint32_t host_index = 0;
         host_index < sizeof(uintptr_t) * 8;
         ++host_index) {
        if ((host_available_mask & (uintptr_t{1} << host_index)) == 0) {
            continue;
        }
        if (current_xbox_index == xbox_index) {
            return host_index;
        }
        ++current_xbox_index;
    }
    return std::numeric_limits<uint32_t>::max();
}

void RegisterKernelxThreadingSymbols(HleSymbolRegistry& registry) {
    registry.RegisterSymbol("kernelx.dll", "GetLogicalProcessorInformationEx",
                            reinterpret_cast<void*>(&GetLogicalProcessorInformationExXbox));
    registry.RegisterSymbol("kernelx.dll", "SetThreadAffinityMask",
                            reinterpret_cast<void*>(&SetThreadAffinityMaskXbox));
    registry.RegisterSymbol("kernelx.dll", "SetThreadIdealProcessor",
                            reinterpret_cast<void*>(&SetThreadIdealProcessorXbox));
    LogMessage(LogLevel::Info, "kernelx_threading",
               "Registered an 8-core/16-thread Xbox topology with host affinity mapping");
}

}  // namespace scarlettx::kernelx
