#include "scarlettx/core/veh_interceptor.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/logger.h"
#include "scarlettx/core/hle_trace.h"
#include "scarlettx/core/iat_symbols.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace scarlettx {

#if defined(_WIN32)

namespace {

// ---------------------------------------------------------------------------
// NTSTATUS values the guest stubs compare against.
// ---------------------------------------------------------------------------
constexpr long kStatusSuccess = 0;
constexpr long kStatusNotImplemented = static_cast<long>(0xC0000002u);
constexpr long kStatusInvalidInfoClass = static_cast<long>(0xC0000003u);
constexpr long kStatusInfoLengthMismatch = static_cast<long>(0xC0000004u);
constexpr long kStatusInvalidHandle = static_cast<long>(0xC0000008u);
constexpr long kStatusInvalidParameter = static_cast<long>(0xC000000Du);
constexpr long kStatusNoMemory = static_cast<long>(0xC0000017u);
constexpr long kStatusAccessDenied = static_cast<long>(0xC0000022u);
constexpr long kStatusObjectNameInvalid = static_cast<long>(0xC0000033u);
constexpr long kStatusObjectNameCollision = static_cast<long>(0xC0000035u);
constexpr long kStatusObjectPathNotFound = static_cast<long>(0xC000003Au);
constexpr long kStatusInvalidPageProtection = static_cast<long>(0xC0000045u);
constexpr long kStatusNotSupported = static_cast<long>(0xC00000BBu);
constexpr long kStatusEndOfFile = static_cast<long>(0xC0000011u);
constexpr long kStatusTimeout = static_cast<long>(0x00000102u);
constexpr long kStatusUnsuccessful = static_cast<long>(0xC0000001u);

std::mutex& SiteMutex();  // defined below; needed by the syscall registry

// Same-site emulations past this count mean the guest is spinning on one of
// the emulated sites in a way that cannot converge; stop emulating so the
// failure surfaces instead of hanging the process forever.
constexpr uint32_t kMaxEmulationsPerSite = 100000;

std::atomic<uint64_t> g_emulated_instructions{0};
std::atomic<uint64_t> g_dispatched_syscalls{0};

// Guest `syscall` sites rewritten to ud2 by PatchSyscallSites: a plain
// `syscall` executes on Windows (it is the host's own user->kernel
// transition) and would reach the Windows kernel with an Xbox syscall
// number, so the instruction is neutralised in mapped guest code instead.
std::set<uintptr_t>& PatchedSyscalls() {
    static std::set<uintptr_t> sites;
    return sites;
}

bool IsPatchedSyscallSite(uintptr_t rip) {
    std::lock_guard<std::mutex> lock(SiteMutex());
    return PatchedSyscalls().count(rip) != 0;
}

struct EmulationSite {
    uint64_t count = 0;
    bool logged = false;
};

std::mutex& SiteMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<uint64_t, EmulationSite>& Sites() {
    static std::unordered_map<uint64_t, EmulationSite> sites;
    return sites;
}

enum class EmulationAction {
    Skip,     // advance Rip past the instruction, no visible state change
    ReadMsr,  // zero RAX:RDX, then advance
    Syscall,  // dispatch through the console syscall table, then advance
    NotOurs,  // unrecognised instruction: pass the exception on
};

struct DecodedInstruction {
    EmulationAction action = EmulationAction::NotOurs;
    size_t length = 0;
    const char* name = "";
};

bool IsPrefixByte(uint8_t byte) {
    switch (byte) {
        case 0x66:  // operand size
        case 0x67:  // address size
        case 0xF0:  // lock
        case 0xF2:  // repne
        case 0xF3:  // rep
        case 0x2E:  // cs
        case 0x36:  // ss
        case 0x3E:  // ds
        case 0x26:  // es
        case 0x64:  // fs
        case 0x65:  // gs
            return true;
        default:
            return byte >= 0x40 && byte <= 0x4F;  // REX
    }
}

// Decodes only the console/Retail-only encodings a Win32 user-mode process
// cannot execute. Everything else is NotOurs and the exception continues on
// to the host's regular SEH/VEH chain.
//
// cpuid is deliberately absent: it executes natively on the host, so console
// leaf numbers simply return host/hypervisor data and nothing here can (or
// needs to) intercept it.
DecodedInstruction DecodeFaultingInstruction(const uint8_t* code) {
    size_t prefixes = 0;
    while (prefixes < 4 && IsPrefixByte(code[prefixes])) {
        ++prefixes;
    }
    const size_t at = prefixes;
    const uint8_t opcode = code[at];

    if (opcode == 0xFA) return {EmulationAction::Skip, prefixes + 1, "cli"};
    if (opcode == 0xFB) return {EmulationAction::Skip, prefixes + 1, "sti"};
    if (opcode == 0xF4) return {EmulationAction::Skip, prefixes + 1, "hlt"};
    if (opcode != 0x0F) return {};

    switch (code[at + 1]) {
        case 0x08: return {EmulationAction::Skip, prefixes + 2, "invd"};
        case 0x09: return {EmulationAction::Skip, prefixes + 2, "wbinvd"};
        case 0x30: return {EmulationAction::Skip, prefixes + 2, "wrmsr"};
        case 0x32: return {EmulationAction::ReadMsr, prefixes + 2, "rdmsr"};
        case 0x05: return {EmulationAction::Syscall, prefixes + 2, "syscall"};
        default: return {};
    }
}

std::string HexText(uint64_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << value;
    return text.str();
}

// WDK-only flag: the guest stubs set it on synchronous file handles.
constexpr DWORD kFileSynchronousIoNonalert = 0x00000020;

std::string TrimText(const std::string& text) {
    size_t start = 0;
    while (start < text.size() &&
           (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' ||
            text[start] == '\n')) {
        ++start;
    }
    size_t end = text.size();
    while (end > start &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
            text[end - 1] == '\n')) {
        --end;
    }
    return text.substr(start, end - start);
}

// ---------------------------------------------------------------------------
// Guest ABI helpers
// ---------------------------------------------------------------------------

// NT/x64 argument order: RCX, RDX, R8, R9, then the stack. At the syscall
// instruction RSP still holds the stub's frame, so argument 5 is at
// [Rsp+0x28] (return address at [Rsp], shadow space above it).
uint64_t StackArgument(const CONTEXT& context, size_t index) {
    if (index < 4) {
        switch (index) {
            case 0: return context.Rcx;
            case 1: return context.Rdx;
            case 2: return context.R8;
            default: return context.R9;
        }
    }
    const uintptr_t address =
        static_cast<uintptr_t>(context.Rsp) + 0x28 + (index - 4) * sizeof(uint64_t);
    return *reinterpret_cast<const uint64_t*>(address);
}

long StatusFromWin32(DWORD error) {
    switch (error) {
        case ERROR_SUCCESS: return kStatusSuccess;
        case ERROR_INVALID_HANDLE: return kStatusInvalidHandle;
        case ERROR_INVALID_PARAMETER: return kStatusInvalidParameter;
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD: return kStatusAccessDenied;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME: return kStatusObjectPathNotFound;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS: return kStatusObjectNameCollision;
        case ERROR_HANDLE_EOF: return kStatusEndOfFile;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY: return kStatusNoMemory;
        case ERROR_NOT_SUPPORTED:
        case ERROR_CALL_NOT_IMPLEMENTED: return kStatusNotSupported;
        case ERROR_TIMEOUT: return kStatusTimeout;
        case ERROR_INVALID_DRIVE: return kStatusObjectPathNotFound;
        default: return kStatusUnsuccessful;
    }
}

// Guest OBJECT_ATTRIBUTES / UNICODE_STRING / IO_STATUS_BLOCK (x64 layout).
struct GuestUnicodeString {
    uint16_t length;
    uint16_t maximum_length;
    wchar_t* buffer;
};

struct GuestObjectAttributes {
    uint32_t length;
    void* root_directory;
    GuestUnicodeString* object_name;
    uint32_t attributes;
    void* security_descriptor;
    void* security_quality_of_service;
};

struct GuestIoStatusBlock {
    long status;
    uintptr_t information;
};

// Console paths: "\??\G:\..." / "\\?\G:\..." / bare "G:\...". The package
// root stands in for the console's G: drive, exactly like GuestCreateFileW.
std::wstring WidenGuestPath(const GuestUnicodeString& name) {
    if (!name.buffer || name.length < sizeof(wchar_t)) {
        return {};
    }
    std::wstring path(name.buffer, name.length / sizeof(wchar_t));
    if (path.rfind(L"\\??\\", 0) == 0) {
        path.erase(0, 4);
    } else if (path.rfind(L"\\\\?\\", 0) == 0) {
        path.erase(0, 4);
    }
    if (path.size() >= 2 && (path[0] == L'G' || path[0] == L'g') && path[1] == L':') {
        const char* root = std::getenv("SCARLETTX_PACKAGE_ROOT");
        if (root && *root) {
            std::wstring package_root =
                std::filesystem::path(root).wstring();
            path = package_root + path.substr(2);
        }
    }
    return path;
}

// ---------------------------------------------------------------------------
// Handler implementations. Each mirrors the NT contract the guest stub
// expects: arguments from the saved register state, NTSTATUS in the return
// value (placed in RAX by the dispatcher).
// ---------------------------------------------------------------------------

// NtAllocateVirtualMemory(ProcessHandle, PVOID* BaseAddress, ZeroBits,
//                         PSIZE_T RegionSize, AllocationType, Protect)
long SysNtAllocateVirtualMemory(CONTEXT& context) {
    auto* base_address = reinterpret_cast<void**>(context.Rdx);
    auto* region_size = reinterpret_cast<SIZE_T*>(context.R9);  // R8 is ZeroBits
    uint64_t allocation_type = StackArgument(context, 4);
    const DWORD protect = static_cast<DWORD>(StackArgument(context, 5) & 0xFFu);
    if (!base_address || !region_size || *region_size == 0) {
        return kStatusInvalidParameter;
    }

    const HANDLE process = reinterpret_cast<HANDLE>(context.Rcx);
    if (process != nullptr && process != reinterpret_cast<HANDLE>(-1) &&
        process != GetCurrentProcess()) {
        return kStatusInvalidHandle;  // the guest only ever allocates for itself
    }

    // Xbox GameOS sets MEM_64K_PAGES (0x00080000) alongside MEM_RESERVE;
    // on Win32 that bit is MEM_RESET, so it must not reach VirtualAlloc. A
    // genuine MEM_RESET is never combined with MEM_RESERVE, so the pairing
    // identifies the console flag unambiguously.
    constexpr uint64_t kMem64kPages = 0x00080000ull;
    constexpr uint64_t kMemReserve = 0x00002000ull;
    const bool sixty_four_k_pages =
        (allocation_type & kMem64kPages) != 0 && (allocation_type & kMemReserve) != 0;
    allocation_type &= ~kMem64kPages;

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const SIZE_T granularity = sixty_four_k_pages ? 0x10000u : system_info.dwPageSize;

    SIZE_T size = (*region_size + granularity - 1) & ~(granularity - 1);
    void* requested = *base_address;
    if (requested != nullptr) {
        requested = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(requested) & ~(granularity - 1));
    }
    void* result = VirtualAlloc(requested, size, static_cast<DWORD>(allocation_type), protect);
    if (!result) {
        const DWORD error = GetLastError();
        // A failed reservation with a heap hint usually means the hint is
        // taken; retry once without it, as the NT contract allows.
        if (requested != nullptr) {
            result = VirtualAlloc(nullptr, size,
                                  static_cast<DWORD>(allocation_type), protect);
        }
        if (!result) {
            LogMessage(LogLevel::Warning, "syscall",
                       "NtAllocateVirtualMemory failed: size=" +
                           HexText(size) + " type=" + HexText(allocation_type) +
                           " protect=" + HexText(protect) +
                           " error=" + std::to_string(error));
            return StatusFromWin32(error);
        }
    }
    *base_address = result;
    *region_size = size;
    return kStatusSuccess;
}

// NtFreeVirtualMemory(ProcessHandle, PVOID* BaseAddress, PSIZE_T RegionSize,
//                     FreeType)
long SysNtFreeVirtualMemory(CONTEXT& context) {
    auto* base_address = reinterpret_cast<void**>(context.Rdx);
    auto* region_size = reinterpret_cast<SIZE_T*>(context.R8);
    const DWORD free_type = static_cast<DWORD>(context.R9);
    if (!base_address || !*base_address) {
        return kStatusInvalidParameter;
    }
    SIZE_T size = region_size ? *region_size : 0;
    // MEM_RELEASE requires a zero size on Win32.
    if ((free_type & 0x8000u) != 0) {
        size = 0;
    }
    if (!VirtualFree(*base_address, size, free_type)) {
        const DWORD error = GetLastError();
        LogMessage(LogLevel::Warning, "syscall",
                   "NtFreeVirtualMemory failed: base=" +
                       HexText(reinterpret_cast<uint64_t>(*base_address)) +
                       " size=" + HexText(size) + " type=" + HexText(free_type) +
                       " error=" + std::to_string(error));
        return StatusFromWin32(error);
    }
    *base_address = nullptr;
    if (region_size) *region_size = 0;
    return kStatusSuccess;
}

// NtProtectVirtualMemory(ProcessHandle, PVOID* BaseAddress, PSIZE_T RegionSize,
//                        NewProtect, PULONG OldProtect)
long SysNtProtectVirtualMemory(CONTEXT& context) {
    auto* base_address = reinterpret_cast<void**>(context.Rdx);
    auto* region_size = reinterpret_cast<SIZE_T*>(context.R8);
    const DWORD new_protect = static_cast<DWORD>(context.R9 & 0xFFu);
    auto* old_protect = reinterpret_cast<DWORD*>(StackArgument(context, 4));
    if (!base_address || !*base_address || !region_size || *region_size == 0) {
        return kStatusInvalidParameter;
    }

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const uintptr_t page = system_info.dwPageSize;
    const uintptr_t start =
        reinterpret_cast<uintptr_t>(*base_address) & ~(page - 1);
    const uintptr_t end = (reinterpret_cast<uintptr_t>(*base_address) +
                           *region_size + page - 1) &
                          ~(page - 1);
    DWORD previous = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(start), end - start, new_protect,
                        &previous)) {
        const DWORD error = GetLastError();
        return error == ERROR_INVALID_PARAMETER ? kStatusInvalidPageProtection
                                                : StatusFromWin32(error);
    }
    // NT reports the page-rounded range back to the caller.
    *base_address = reinterpret_cast<void*>(start);
    *region_size = end - start;
    if (old_protect) *old_protect = previous;
    return kStatusSuccess;
}

// NtQuerySystemInformation(Class, PVOID Info, ULONG Length, PULONG ReturnLength)
long SysNtQuerySystemInformation(CONTEXT& context) {
    const uint32_t info_class = static_cast<uint32_t>(context.Rcx);
    void* buffer = reinterpret_cast<void*>(context.Rdx);
    const uint32_t length = static_cast<uint32_t>(context.R8);
    auto* returned = reinterpret_cast<uint32_t*>(context.R9);

    if (returned) *returned = 0;

    switch (info_class) {
        case 0: {  // SystemBasicInformation
            struct SystemBasicInformation {
                uint32_t reserved;
                uint32_t timer_resolution;
                uint32_t page_size;
                uint32_t number_of_physical_pages;
                uint32_t lowest_physical_page_number;
                uint32_t highest_physical_page_number;
                uint32_t allocation_granularity;
                uint64_t minimum_user_address;
                uint64_t maximum_user_address;
                uint64_t affinity_mask;
                uint32_t number_of_processors;
                uint8_t reserved_tail[4];
            };
            SYSTEM_INFO system_info{};
            GetSystemInfo(&system_info);
            MEMORYSTATUSEX memory{};
            memory.dwLength = sizeof(memory);
            GlobalMemoryStatusEx(&memory);

            SystemBasicInformation info{};
            info.timer_resolution = 156250;  // 100 ns units, the NT default
            info.page_size = system_info.dwPageSize;
            info.number_of_physical_pages = static_cast<uint32_t>(
                memory.ullTotalPhys / system_info.dwPageSize);
            info.highest_physical_page_number = info.number_of_physical_pages;
            info.allocation_granularity = system_info.dwAllocationGranularity;
            info.minimum_user_address = 0x10000ull;
            info.maximum_user_address = 0x7FFFFFFEFFFFull;
            info.affinity_mask = static_cast<uint64_t>(system_info.dwNumberOfProcessors);
            info.number_of_processors = system_info.dwNumberOfProcessors;
            if (returned) *returned = sizeof(info);
            if (!buffer || length < sizeof(info)) return kStatusInfoLengthMismatch;
            std::memcpy(buffer, &info, sizeof(info));
            return kStatusSuccess;
        }
        case 3: {  // SystemTimeOfDayInformation
            struct SystemTimeOfDayInformation {
                int64_t boot_time;
                int64_t current_time;
                int64_t time_zone_bias;
                int64_t time_zone_bias_stamp;
                uint64_t boot_time_bias;
                uint64_t time_zone_bias_delta;
            };
            FILETIME now{};
            GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER ticks{};
            ticks.LowPart = now.dwLowDateTime;
            ticks.HighPart = now.dwHighDateTime;
            SystemTimeOfDayInformation info{};
            info.current_time = static_cast<int64_t>(ticks.QuadPart);
            info.boot_time = info.current_time -
                             static_cast<int64_t>(GetTickCount64()) * 10000ll;
            if (returned) *returned = sizeof(info);
            if (!buffer || length < sizeof(info)) return kStatusInfoLengthMismatch;
            std::memcpy(buffer, &info, sizeof(info));
            return kStatusSuccess;
        }
        default: {
            static std::mutex log_mutex;
            static std::unordered_set<uint32_t> logged;
            {
                std::lock_guard<std::mutex> lock(log_mutex);
                if (logged.insert(info_class).second) {
                    LogMessage(LogLevel::Warning, "syscall",
                               "NtQuerySystemInformation class " +
                                   std::to_string(info_class) +
                                   " is not implemented; reporting "
                                   "STATUS_INVALID_INFO_CLASS");
                }
            }
            return kStatusInvalidInfoClass;
        }
    }
}

// NtDelayExecution(BOOLEAN Alertable, PLARGE_INTEGER Interval)
long SysNtDelayExecution(CONTEXT& context) {
    auto* interval = reinterpret_cast<const int64_t*>(context.Rdx);
    if (!interval) {
        Sleep(0);
        return kStatusSuccess;
    }
    const int64_t ticks = *interval;
    if (ticks < 0) {
        const uint64_t milliseconds =
            static_cast<uint64_t>(-ticks) / 10000ull;
        Sleep(static_cast<DWORD>(std::min<uint64_t>(milliseconds, 0xFFFFFFFFull)));
        return kStatusSuccess;
    }
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER now_ticks{};
    now_ticks.LowPart = now.dwLowDateTime;
    now_ticks.HighPart = now.dwHighDateTime;
    if (static_cast<uint64_t>(ticks) > now_ticks.QuadPart) {
        const uint64_t delta = (static_cast<uint64_t>(ticks) - now_ticks.QuadPart) / 10000ull;
        Sleep(static_cast<DWORD>(std::min<uint64_t>(delta, 0xFFFFFFFFull)));
    }
    return kStatusSuccess;
}

// NtSetEvent(HANDLE EventHandle, PLONG PreviousState)
long SysNtSetEvent(CONTEXT& context) {
    const HANDLE handle = reinterpret_cast<HANDLE>(context.Rcx);
    auto* previous = reinterpret_cast<LONG*>(context.Rdx);
    const DWORD wait_result = WaitForSingleObject(handle, 0);
    if (previous) *previous = (wait_result == WAIT_OBJECT_0) ? 1 : 0;
    if (!SetEvent(handle)) {
        return StatusFromWin32(GetLastError());
    }
    return kStatusSuccess;
}

// NtWaitForSingleObject(HANDLE, BOOLEAN Alertable, PLARGE_INTEGER Timeout)
long SysNtWaitForSingleObject(CONTEXT& context) {
    const HANDLE handle = reinterpret_cast<HANDLE>(context.Rcx);
    auto* timeout = reinterpret_cast<const int64_t*>(context.R8);
    DWORD milliseconds = INFINITE;
    if (timeout) {
        const int64_t ticks = *timeout;
        if (ticks == 0) {
            milliseconds = 0;
        } else if (ticks < 0) {
            milliseconds = static_cast<DWORD>(std::min<uint64_t>(
                static_cast<uint64_t>(-ticks) / 10000ull + 1ull, 0xFFFFFFFEull));
        } else {
            FILETIME now{};
            GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER now_ticks{};
            now_ticks.LowPart = now.dwLowDateTime;
            now_ticks.HighPart = now.dwHighDateTime;
            if (static_cast<uint64_t>(ticks) <= now_ticks.QuadPart) {
                milliseconds = 0;
            } else {
                milliseconds = static_cast<DWORD>(std::min<uint64_t>(
                    (static_cast<uint64_t>(ticks) - now_ticks.QuadPart) / 10000ull + 1ull,
                    0xFFFFFFFEull));
            }
        }
    }
    const DWORD wait_result = WaitForSingleObject(handle, milliseconds);
    if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED) {
        return kStatusSuccess;
    }
    if (wait_result == WAIT_TIMEOUT) {
        return kStatusTimeout;
    }
    return StatusFromWin32(GetLastError());
}

// NtQueryPerformanceCounter(PLARGE_INTEGER Counter, PLARGE_INTEGER Frequency)
long SysNtQueryPerformanceCounter(CONTEXT& context) {
    auto* counter = reinterpret_cast<LARGE_INTEGER*>(context.Rcx);
    auto* frequency = reinterpret_cast<LARGE_INTEGER*>(context.Rdx);
    LARGE_INTEGER value{};
    if (!QueryPerformanceCounter(&value)) {
        return StatusFromWin32(GetLastError());
    }
    if (counter) *counter = value;
    if (frequency && !QueryPerformanceFrequency(frequency)) {
        return StatusFromWin32(GetLastError());
    }
    return kStatusSuccess;
}

// NtClose(HANDLE)
long SysNtClose(CONTEXT& context) {
    const HANDLE handle = reinterpret_cast<HANDLE>(context.Rcx);
    if (handle == GetCurrentProcess() || handle == GetCurrentThread() ||
        handle == reinterpret_cast<HANDLE>(-1) ||
        handle == reinterpret_cast<HANDLE>(-2)) {
        return kStatusSuccess;  // pseudo-handles are not closable
    }
    if (!CloseHandle(handle)) {
        return StatusFromWin32(GetLastError());
    }
    return kStatusSuccess;
}

// NtCreateFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
//              PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
//              ULONG ShareAccess, ULONG CreateDisposition,
//              ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength)
long SysNtCreateFile(CONTEXT& context) {
    auto** out_handle = reinterpret_cast<void**>(context.Rcx);
    const DWORD desired_access = static_cast<DWORD>(context.Rdx);
    auto* attributes = reinterpret_cast<GuestObjectAttributes*>(context.R8);
    auto* io_status = reinterpret_cast<GuestIoStatusBlock*>(context.R9);
    const DWORD file_attributes = static_cast<DWORD>(StackArgument(context, 5));
    const DWORD share_access = static_cast<DWORD>(StackArgument(context, 6));
    const uint32_t create_disposition =
        static_cast<uint32_t>(StackArgument(context, 7));
    const DWORD create_options = static_cast<DWORD>(StackArgument(context, 8));

    if (!out_handle || !attributes || !attributes->object_name || !io_status) {
        return kStatusInvalidParameter;
    }
    const std::wstring path = WidenGuestPath(*attributes->object_name);
    if (path.empty()) {
        return kStatusObjectNameInvalid;
    }

    DWORD disposition = OPEN_EXISTING;
    uintptr_t information = 1;  // FILE_OPENED
    switch (create_disposition) {
        case 0: disposition = CREATE_ALWAYS; information = 3; break;      // FILE_SUPERSEDE
        case 1: disposition = OPEN_EXISTING; information = 1; break;      // FILE_OPEN
        case 2: disposition = CREATE_NEW; information = 2; break;         // FILE_CREATE
        case 3: disposition = OPEN_ALWAYS; information = 3; break;        // FILE_OPEN_IF
        case 4: disposition = TRUNCATE_EXISTING; information = 3; break;  // FILE_OVERWRITE
        case 5: disposition = CREATE_ALWAYS; information = 3; break;      // FILE_OVERWRITE_IF
        default: return kStatusInvalidParameter;
    }

    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (file_attributes != 0 && file_attributes != FILE_ATTRIBUTE_NORMAL) {
        flags = file_attributes & ~FILE_ATTRIBUTE_NORMAL;
    }
    if ((create_options & kFileSynchronousIoNonalert) == 0) {
        flags |= FILE_FLAG_OVERLAPPED;
    }

    HANDLE handle = CreateFileW(path.c_str(), desired_access, share_access, nullptr,
                                disposition, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        io_status->status = StatusFromWin32(error);
        io_status->information = 0;
        *out_handle = nullptr;
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            LogMessage(LogLevel::Warning, "syscall",
                       "NtCreateFile failed: error=" + std::to_string(error) +
                           " access=" + HexText(desired_access) +
                           " disposition=" + std::to_string(create_disposition));
        }
        return io_status->status;
    }
    io_status->status = kStatusSuccess;
    io_status->information = information;
    *out_handle = handle;
    return kStatusSuccess;
}

// NtReadFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID ApcContext,
//            PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
//            PLARGE_INTEGER ByteOffset, PULONG Key)
long SysNtReadFile(CONTEXT& context) {
    const HANDLE file = reinterpret_cast<HANDLE>(context.Rcx);
    auto* io_status = reinterpret_cast<GuestIoStatusBlock*>(StackArgument(context, 4));
    void* buffer = reinterpret_cast<void*>(StackArgument(context, 5));
    const DWORD length = static_cast<DWORD>(StackArgument(context, 6));
    auto* byte_offset = reinterpret_cast<const int64_t*>(StackArgument(context, 7));

    if (!io_status) {
        return kStatusInvalidParameter;
    }
    io_status->information = 0;
    if (length != 0 && !buffer) {
        io_status->status = kStatusInvalidParameter;
        return io_status->status;
    }

    DWORD read = 0;
    if (byte_offset) {
        OVERLAPPED overlapped{};
        const uint64_t offset = static_cast<uint64_t>(*byte_offset);
        overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        if (!ReadFile(file, buffer, length, &read, &overlapped)) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                io_status->status = StatusFromWin32(error);
                return io_status->status;
            }
            if (!GetOverlappedResult(file, &overlapped, &read, TRUE)) {
                io_status->status = StatusFromWin32(GetLastError());
                return io_status->status;
            }
        }
    } else {
        if (!ReadFile(file, buffer, length, &read, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER) {
                // The handle was opened for overlapped I/O by another path
                // (GuestCreateFileW forces FILE_FLAG_OVERLAPPED); retry at
                // offset zero through an explicit OVERLAPPED block.
                OVERLAPPED overlapped{};
                if (!ReadFile(file, buffer, length, &read, &overlapped) &&
                    GetLastError() != ERROR_IO_PENDING) {
                    io_status->status = StatusFromWin32(GetLastError());
                    return io_status->status;
                }
                if (read == 0 &&
                    !GetOverlappedResult(file, &overlapped, &read, TRUE)) {
                    const DWORD overlapped_error = GetLastError();
                    io_status->status = StatusFromWin32(overlapped_error);
                    return io_status->status;
                }
            } else if (error == ERROR_HANDLE_EOF) {
                read = 0;
            } else {
                io_status->status = StatusFromWin32(error);
                return io_status->status;
            }
        }
    }

    io_status->information = read;
    if (read == 0 && length != 0) {
        io_status->status = kStatusEndOfFile;
        return io_status->status;
    }
    io_status->status = kStatusSuccess;
    return kStatusSuccess;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

struct SyscallEntry {
    std::string name;
    XboxSyscallHandler handler = nullptr;
};

std::unordered_map<uint64_t, SyscallEntry>& SyscallTable() {
    static std::unordered_map<uint64_t, SyscallEntry> table;
    return table;
}

// Wraps a CONTEXT& handler into the void*-taking entry stored in the table.
template <long (*Handler)(CONTEXT&)>
long SyscallThunk(void* context) {
    return Handler(*static_cast<CONTEXT*>(context));
}

const std::unordered_map<std::string, XboxSyscallHandler>& BuiltinSyscalls() {
    static const std::unordered_map<std::string, XboxSyscallHandler> handlers = {
        {"NtAllocateVirtualMemory", &SyscallThunk<SysNtAllocateVirtualMemory>},
        {"NtFreeVirtualMemory", &SyscallThunk<SysNtFreeVirtualMemory>},
        {"NtProtectVirtualMemory", &SyscallThunk<SysNtProtectVirtualMemory>},
        {"NtQuerySystemInformation", &SyscallThunk<SysNtQuerySystemInformation>},
        {"NtDelayExecution", &SyscallThunk<SysNtDelayExecution>},
        {"NtSetEvent", &SyscallThunk<SysNtSetEvent>},
        {"NtWaitForSingleObject", &SyscallThunk<SysNtWaitForSingleObject>},
        {"NtQueryPerformanceCounter", &SyscallThunk<SysNtQueryPerformanceCounter>},
        {"NtClose", &SyscallThunk<SysNtClose>},
        {"NtCreateFile", &SyscallThunk<SysNtCreateFile>},
        {"NtReadFile", &SyscallThunk<SysNtReadFile>},
    };
    return handlers;
}

// Console syscall numbers are not published, so the table starts empty and is
// filled from SCARLETTX_SYSCALL_MAP (numbers observed in the log) or through
// RegisterXboxSyscallHandler().
void InitializeSyscallTableOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (const char* mapping = std::getenv("SCARLETTX_SYSCALL_MAP");
            mapping && *mapping) {
            LoadXboxSyscallMapFromString(mapping);
        }
    });
}

bool LookupSyscall(uint64_t number, SyscallEntry& out) {
    InitializeSyscallTableOnce();
    std::lock_guard<std::mutex> lock(SiteMutex());
    const auto it = SyscallTable().find(number);
    if (it == SyscallTable().end()) {
        return false;
    }
    out = it->second;
    return true;
}

// Unknown numbers are logged once each with the full argument set, so an
// operator can bind them through SCARLETTX_SYSCALL_MAP on the next run.
void LogUnknownSyscall(uint64_t number, const CONTEXT& context) {
    static std::mutex log_mutex;
    static std::unordered_set<uint64_t> logged;
    {
        std::lock_guard<std::mutex> lock(log_mutex);
        if (!logged.insert(number).second) {
            return;
        }
    }
    LogMessage(LogLevel::Warning, "syscall",
               "unknown console syscall rax=" + HexText(number) +
                   " rcx=" + HexText(context.Rcx) +
                   " rdx=" + HexText(context.Rdx) +
                   " r8=" + HexText(context.R8) +
                   " r9=" + HexText(context.R9) +
                   " rip=" + HexText(context.Rip) +
                   "; returning STATUS_NOT_IMPLEMENTED (bind with "
                   "SCARLETTX_SYSCALL_MAP=<rax>=<handler>)");
}

// Runs the bound handler for the syscall number the stub left in RAX and
// returns the NTSTATUS to place back in RAX.
long DispatchGuestSyscall(CONTEXT& context) {
    const uint64_t number = context.Rax;
    // Every distinct number is reported once with its argument registers: the
    // numbers are what SCARLETTX_SYSCALL_MAP needs, and the arguments say which
    // handler shape fits.
    {
        static std::mutex seen_mutex;
        static std::unordered_set<uint64_t> seen;
        bool first = false;
        {
            std::lock_guard<std::mutex> lock(seen_mutex);
            first = seen.insert(number).second;
        }
        if (first) {
            LogMessage(LogLevel::Info, "syscall",
                       "intercepted RAX=" + HexText(number) +
                           " RCX=" + HexText(context.Rcx) +
                           " RDX=" + HexText(context.Rdx) +
                           " R8=" + HexText(context.R8) +
                           " R9=" + HexText(context.R9) +
                           " rip=" + HexText(context.Rip));
        }
    }
    SyscallEntry entry{};
    if (!LookupSyscall(number, entry) || !entry.handler) {
        LogUnknownSyscall(number, context);
        return kStatusNotImplemented;
    }
    g_dispatched_syscalls.fetch_add(1, std::memory_order_relaxed);
    return entry.handler(&context);
}

LONG WINAPI VehHandler(EXCEPTION_POINTERS* info) {
    if (!info || !info->ContextRecord || !info->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    // Privileged instructions raise #GP (EXCEPTION_PRIV_INSTRUCTION); a
    // patched syscall site raises #UD (EXCEPTION_ILLEGAL_INSTRUCTION). The
    // breakpoint and access-violation branches below need to see their codes
    // too, so the filter lets those through instead of dropping them here.
    const DWORD code = record.ExceptionCode;
    const bool privileged =
        code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_ILLEGAL_INSTRUCTION;
    const bool diagnostic = code == EXCEPTION_BREAKPOINT ||
                            code == EXCEPTION_ACCESS_VIOLATION;
    if (!privileged && !diagnostic) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CONTEXT& context = *info->ContextRecord;

    // An access violation that no handler covers kills the host silently as far
    // as the log is concerned. Record where it happened once per site: the RIP
    // says whether it is guest code, a shim, or a call through a null pointer.
    // A guest-side assert is a real failure: report it with the call that fed
    // the null and the last HLE calls, then let the process stop here instead
    // of skipping the trap and dying later on [null + 0x10].
    if (record.ExceptionCode == EXCEPTION_BREAKPOINT &&
        *reinterpret_cast<const uint8_t*>(context.Rip) == 0xCC) {
        // Dump the bytes that led here and resolve the import behind them: the
        // assert site is `call <thunk>; mov [global],rax; test rax,rax; int3`,
        // so the last call before RIP names the API that returned null.
        const uint8_t* rip = reinterpret_cast<const uint8_t*>(context.Rip);
        std::string bytes;
        for (size_t back = 32; back > 0; --back) {
            char text[8] = {};
            std::snprintf(text, sizeof(text), "%02X ", rip[-static_cast<ptrdiff_t>(back)]);
            bytes += text;
        }

        std::string symbol = "unresolved";
        const uint8_t* call = nullptr;
        for (size_t back = 5; back < 34; ++back) {
            const uint8_t* candidate = rip - back;
            if (*candidate == 0xE8) {
                call = candidate;
                break;
            }
        }
        if (call) {
            int32_t displacement = 0;
            std::memcpy(&displacement, call + 1, sizeof(displacement));
            const uint64_t target = reinterpret_cast<uint64_t>(call) + 5 +
                                    static_cast<int64_t>(displacement);
            // The target is a jump thunk: FF 25 disp32 (or 48 FF 25 disp32).
            const auto* thunk = reinterpret_cast<const uint8_t*>(target);
            uint64_t slot = 0;
            if (thunk[0] == 0xFF && thunk[1] == 0x25) {
                int32_t thunk_disp = 0;
                std::memcpy(&thunk_disp, thunk + 2, sizeof(thunk_disp));
                slot = target + 6 + static_cast<int64_t>(thunk_disp);
            } else if (thunk[0] == 0x48 && thunk[1] == 0xFF && thunk[2] == 0x25) {
                int32_t thunk_disp = 0;
                std::memcpy(&thunk_disp, thunk + 3, sizeof(thunk_disp));
                slot = target + 7 + static_cast<int64_t>(thunk_disp);
            }
            const std::string bound = slot != 0 ? LookupIatBinding(slot)
                                                : LookupIatBinding(target);
            if (!bound.empty()) {
                symbol = bound;
            } else {
                symbol = "call 0x" + HexText(target) + " (slot 0x" +
                         HexText(slot) + ") not in the binding map";
            }
        }

        LogMessage(LogLevel::Error, "veh",
                   "guest assert (int3) at rip=" + HexText(context.Rip) +
                       " came from " + symbol + ", rax=" + HexText(context.Rax) +
                       " rcx=" + HexText(context.Rcx));
        LogMessage(LogLevel::Error, "veh", "bytes before rip: " + bytes);
        HleTraceFaultDump("the guest int3 assert");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        static std::mutex av_mutex;
        static std::unordered_set<uint64_t> av_reported;
        bool report = false;
        {
            std::lock_guard<std::mutex> lock(av_mutex);
            report = av_reported.insert(context.Rip).second;
        }
        if (report) {
            const ULONG_PTR access = record.NumberParameters > 0
                                         ? record.ExceptionInformation[0]
                                         : 0;
            const ULONG_PTR address = record.NumberParameters > 1
                                          ? record.ExceptionInformation[1]
                                          : 0;
            LogMessage(LogLevel::Error, "veh",
                       std::string("access violation: ") +
                           (access == 0 ? "read" : access == 1 ? "write" : "execute") +
                           " at " + HexText(address) + ", rip=" +
                           HexText(context.Rip));
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // A guest syscall site faults as ud2 on a registered address.
    if (record.ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION &&
        IsPatchedSyscallSite(static_cast<uintptr_t>(context.Rip))) {
        {
            std::lock_guard<std::mutex> lock(SiteMutex());
            EmulationSite& site = Sites()[context.Rip];
            ++site.count;
            if (!site.logged) {
                site.logged = true;
                LogMessage(LogLevel::Warning, "veh",
                           "console syscall site at rip=" +
                               HexText(context.Rip) +
                               " (number in rax, dispatched through the "
                               "console syscall table)");
            }
        }
        const long status = DispatchGuestSyscall(context);
        g_emulated_instructions.fetch_add(1, std::memory_order_relaxed);
        context.Rax = static_cast<uint64_t>(static_cast<int64_t>(status));
        context.Rip += 2;  // ud2 replaced the 2-byte syscall encoding
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // The instruction itself faulted, so its leading bytes are readable.
    const DecodedInstruction decoded =
        DecodeFaultingInstruction(reinterpret_cast<const uint8_t*>(context.Rip));
    if (decoded.action == EmulationAction::NotOurs) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    {
        std::lock_guard<std::mutex> lock(SiteMutex());
        EmulationSite& site = Sites()[context.Rip];
        ++site.count;
        if (!site.logged) {
            site.logged = true;
            LogMessage(LogLevel::Warning, "veh",
                       std::string("emulated console instruction ") + decoded.name +
                           " at rip=" + HexText(context.Rip));
        }
        if (site.count > kMaxEmulationsPerSite) {
            LogMessage(LogLevel::Error, "veh",
                       "guest spins on emulated instruction " +
                           std::string(decoded.name) + " at rip=" +
                           HexText(context.Rip) + "; no longer emulating");
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    g_emulated_instructions.fetch_add(1, std::memory_order_relaxed);

    long status = kStatusSuccess;
    switch (decoded.action) {
        case EmulationAction::ReadMsr:
            // Console MSR reads (timing/telemetry registers) report zero.
            context.Rax = 0;
            context.Rdx = 0;
            status = kStatusSuccess;
            break;
        case EmulationAction::Syscall:
            // A bare syscall that faults (rather than being patched) still
            // goes through the same console dispatch.
            status = DispatchGuestSyscall(context);
            break;
        case EmulationAction::Skip:
        case EmulationAction::NotOurs:
            break;
    }
    if (decoded.action == EmulationAction::Syscall) {
        context.Rax = static_cast<uint64_t>(static_cast<int64_t>(status));
    }
    context.Rip += decoded.length;
    return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

bool RegisterXboxSyscallHandler(uint32_t number, const char* name,
                                XboxSyscallHandler handler) {
    if (!handler) {
        return false;
    }
    std::lock_guard<std::mutex> lock(SiteMutex());
    auto& table = SyscallTable();
    if (table.count(number) != 0) {
        return false;
    }
    table.emplace(number, SyscallEntry{name ? name : "custom", handler});
    return true;
}

size_t LoadXboxSyscallMapFromString(const char* mapping) {
    if (!mapping || *mapping == '\0') {
        return 0;
    }
    const std::string text(mapping);
    size_t bound = 0;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const std::string raw_entry =
            text.substr(start, comma == std::string::npos ? std::string::npos
                                                          : comma - start);
        start = comma == std::string::npos ? text.size() + 1 : comma + 1;
        const std::string entry = TrimText(raw_entry);
        if (entry.empty()) {
            continue;
        }
        const size_t equals = entry.find('=');
        if (equals == std::string::npos) {
            LogMessage(LogLevel::Warning, "veh",
                       "SCARLETTX_SYSCALL_MAP entry '" + entry +
                           "' is not <number>=<handler>; ignored");
            continue;
        }
        const std::string number_text = TrimText(entry.substr(0, equals));
        const std::string handler_name = TrimText(entry.substr(equals + 1));
        char* number_end = nullptr;
        const unsigned long long number =
            std::strtoull(number_text.c_str(), &number_end, 0);
        if (number_end == number_text.c_str() || *number_end != '\0' ||
            number > 0xFFFFFFFFull) {
            LogMessage(LogLevel::Warning, "veh",
                       "SCARLETTX_SYSCALL_MAP number '" + number_text +
                           "' is not a valid 32-bit value; ignored");
            continue;
        }
        const auto& builtins = BuiltinSyscalls();
        const auto handler_it = builtins.find(handler_name);
        if (handler_it == builtins.end()) {
            LogMessage(LogLevel::Warning, "veh",
                       "SCARLETTX_SYSCALL_MAP handler '" + handler_name +
                           "' is unknown; ignored");
            continue;
        }
        if (RegisterXboxSyscallHandler(static_cast<uint32_t>(number),
                                      handler_it->first.c_str(),
                                      handler_it->second)) {
            ++bound;
        }
    }
    if (bound > 0) {
        LogMessage(LogLevel::Info, "veh",
                   "bound " + std::to_string(bound) +
                       " console syscall numbers from SCARLETTX_SYSCALL_MAP");
    }
    return bound;
}

// ---------------------------------------------------------------------------
// x86-64 instruction length decoder.
//
// Only the length is needed, so the table below covers exactly what decides it:
// legacy prefixes, REX, VEX/EVEX, one/two/three-byte opcodes, ModR/M, SIB and
// the displacement/immediate forms. Anything unknown returns 0 and the caller
// stops patching that region instead of guessing: a wrong length would restart
// the walk mid-instruction, which is the corruption this decoder exists to
// prevent.
// ---------------------------------------------------------------------------

struct ModRmResult {
    size_t extra = 0;  // SIB + displacement bytes after the ModR/M byte
    bool valid = false;
};

ModRmResult DecodeModRm(const uint8_t* code, size_t size, size_t at,
                        bool address_size_32) {
    ModRmResult result;
    if (at >= size) {
        return result;
    }
    const uint8_t modrm = code[at];
    const uint8_t mod = modrm >> 6;
    const uint8_t rm = modrm & 7;
    size_t extra = 0;
    if (mod != 3) {
        if (rm == 4) {  // SIB follows
            if (at + 1 >= size) {
                return result;
            }
            const uint8_t sib = code[at + 1];
            extra += 1;
            if (mod == 0 && (sib & 7) == 5) {
                extra += 4;  // disp32 with no base
            }
        } else if (mod == 0 && rm == 5) {
            extra += 4;  // RIP-relative disp32
        }
    }
    if (mod == 1) {
        extra += 1;
    } else if (mod == 2) {
        extra += 4;
    }
    (void)address_size_32;
    result.extra = extra;
    result.valid = true;
    return result;
}

// Returns the instruction length at `code`, or 0 when the encoding is outside
// the handled table.
size_t DecodeInstructionLength(const uint8_t* code, size_t size) {
    size_t i = 0;
    bool operand_size_16 = false;
    bool address_size_32 = false;
    uint8_t rex = 0;

    // Legacy prefixes.
    while (i < size) {
        switch (code[i]) {
            case 0x66: operand_size_16 = true; ++i; continue;
            case 0x67: address_size_32 = true; ++i; continue;
            case 0xF0: case 0xF2: case 0xF3:
            case 0x2E: case 0x36: case 0x3E: case 0x26: case 0x64: case 0x65:
                ++i; continue;
            default: break;
        }
        break;
    }
    if (i >= size) {
        return 0;
    }
    if ((code[i] & 0xF0) == 0x40) {  // REX
        rex = code[i];
        ++i;
    }
    if (i >= size) {
        return 0;
    }

    const uint8_t opcode = code[i++];
    const bool rex_w = (rex & 0x08) != 0;

    // VEX / EVEX: 2-byte (C5), 3-byte (C4), 4-byte (62). All carry ModR/M.
    if (opcode == 0xC5 || opcode == 0xC4 || opcode == 0x62) {
        size_t vex_bytes = opcode == 0xC5 ? 1u : (opcode == 0xC4 ? 2u : 3u);
        if (i + vex_bytes >= size) {
            return 0;
        }
        uint8_t map = 1;
        if (opcode == 0xC5) {
            ++i;
        } else if (opcode == 0xC4) {
            map = code[i] & 0x1F;
            i += 2;
        } else {
            map = code[i] & 0x07;
            i += 3;
        }
        if (i >= size) {
            return 0;
        }
        const uint8_t vop = code[i++];
        const ModRmResult modrm = DecodeModRm(code, size, i, address_size_32);
        if (!modrm.valid) {
            return 0;
        }
        i += 1 + modrm.extra;
        // Map 3 is the 0F3A family: every entry carries imm8.
        if (map == 3) {
            i += 1;
        } else if (map == 1) {
            switch (vop) {
                case 0x70: case 0x71: case 0x72: case 0x73:
                case 0xC2: case 0xC4: case 0xC5: case 0xC6:
                    i += 1;
                    break;
                default:
                    break;
            }
        }
        return i <= size ? i : 0;
    }

    if (opcode == 0x0F) {
        if (i >= size) {
            return 0;
        }
        uint8_t op2 = code[i++];
        uint8_t map = 1;
        if (op2 == 0x38 || op2 == 0x3A) {  // three-byte opcode
            if (i >= size) {
                return 0;
            }
            map = op2 == 0x38 ? 2 : 3;
            op2 = code[i++];
        }
        // Opcodes with no ModR/M at all.
        if (map == 1) {
            switch (op2) {
                case 0x04: case 0x05: case 0x06: case 0x07:
                case 0x08: case 0x09: case 0x0B: case 0x0E:
                case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
                case 0x37: case 0x77: case 0xA2:
                case 0xA8: case 0xA9: case 0xAA:
                    return i <= size ? i : 0;
                case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD:
                case 0xCE: case 0xCF:
                    return i <= size ? i : 0;
                default:
                    break;
            }
        }
        const ModRmResult modrm = DecodeModRm(code, size, i, address_size_32);
        if (!modrm.valid) {
            return 0;
        }
        i += 1 + modrm.extra;
        if (map == 3) {
            i += 1;  // every 0F3A entry takes imm8
        } else if (map == 1) {
            switch (op2) {
                case 0x70: case 0x71: case 0x72: case 0x73:
                case 0xA4: case 0xAC: case 0xBA: case 0xC2: case 0xC4:
                case 0xC5: case 0xC6:
                    i += 1;
                    break;
                default:
                    break;
            }
        }
        return i <= size ? i : 0;
    }

    // One-byte opcodes.
    size_t immediate = 0;
    bool has_modrm = false;
    bool modrm_imm8 = false;  // F6/F7 groups /0 and /1 take imm8
    switch (opcode) {
        // Arithmetic/logic groups: ModR/M, no immediate.
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x38: case 0x39: case 0x3A: case 0x3B:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        case 0x62: case 0x63:
        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        case 0xFE: case 0xFF:
            has_modrm = true;
            break;
        // ModR/M + imm8.
        case 0x80: case 0x82: case 0x83: case 0xC0: case 0xC1: case 0xC6:
            has_modrm = true;
            immediate = 1;
            break;
        // ModR/M + imm32 (imm16 with 0x66).
        case 0x81: case 0xC7:
            has_modrm = true;
            immediate = operand_size_16 ? 2u : 4u;
            break;
        case 0x69:
            has_modrm = true;
            immediate = operand_size_16 ? 2u : 4u;
            break;
        case 0x6B:
            has_modrm = true;
            immediate = 1;
            break;
        // test r/m, imm (F6 /0, F7 /0)
        case 0xF6: case 0xF7:
            has_modrm = true;
            modrm_imm8 = true;
            break;
        // No operands.
        case 0x06: case 0x07: case 0x0E: case 0x16: case 0x17: case 0x1E: case 0x1F:
        case 0x27: case 0x2F: case 0x37: case 0x3F:
        case 0x60: case 0x61: case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97: case 0x98: case 0x99:
        case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        case 0x6C: case 0x6D: case 0x6E: case 0x6F:  // ins/outs
        case 0xD7:                                  // xlat
        case 0xF1:                                  // int1
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        case 0xC3: case 0xC9: case 0xCB: case 0xCC: case 0xCE: case 0xCF:
        case 0xEC: case 0xED: case 0xEE: case 0xEF:
        case 0xF4: case 0xF5: case 0xF8: case 0xF9: case 0xFA: case 0xFB:
        case 0xFC: case 0xFD:
            break;
        // push/pop r64
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55:
        case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            break;
        // imm8
        case 0x6A: case 0xA8: case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        case 0xCD: case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        // accumulator short forms: ADD/OR/ADC/SBB/AND/SUB/XOR/CMP AL, imm8
        case 0x04: case 0x0C: case 0x14: case 0x1C:
        case 0x24: case 0x2C: case 0x34: case 0x3C:
            immediate = 1;
            break;
        // accumulator long forms: ... EAX, imm32 (imm16 with 0x66)
        case 0x05: case 0x0D: case 0x15: case 0x1D:
        case 0x25: case 0x2D: case 0x35: case 0x3D:
            immediate = operand_size_16 ? 2u : 4u;
            break;
        // rel8
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:
        case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xEB:
            immediate = 1;
            break;
        // rel32
        case 0xE8: case 0xE9:
            immediate = 4;
            break;
        // imm32 / imm64
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            immediate = rex_w ? 8u : (operand_size_16 ? 2u : 4u);
            break;
        case 0x68:
            immediate = operand_size_16 ? 2u : 4u;
            break;
        case 0xA9:
            immediate = operand_size_16 ? 2u : 4u;
            break;
        // moffs: address size only
        case 0xA0: case 0xA1: case 0xA2: case 0xA3:
            immediate = address_size_32 ? 4u : 8u;
            break;
        case 0xC2: case 0xCA:
            immediate = 2;
            break;
        case 0xC8:
            immediate = 3;  // imm16 + imm8
            break;
        default:
            return 0;  // not in the table: stop, never guess
    }

    if (has_modrm) {
        const ModRmResult modrm = DecodeModRm(code, size, i, address_size_32);
        if (!modrm.valid) {
            return 0;
        }
        if (modrm_imm8) {
            // F6/F7: only /0 and /1 carry an immediate.
            const uint8_t reg = (code[i] >> 3) & 7;
            if (reg <= 1) {
                immediate = 1;
            }
        }
        i += 1 + modrm.extra;
    }
    i += immediate;
    return i <= size ? i : 0;
}

size_t PatchSyscallSites(uint8_t* code, size_t size) {
    if (!code || size < 2) {
        return 0;
    }
    size_t patched = 0;
    size_t offset = 0;
    size_t resyncs = 0;
    // True while the walk has never lost instruction alignment. A continuous
    // decode chain is trusted on its own; after the first resynchronisation the
    // walk can land inside data, so only the stub-prologue test is allowed to
    // authorise a patch.
    bool chain_intact = true;
    std::lock_guard<std::mutex> lock(SiteMutex());

    const auto has_stub_prologue = [&](size_t at) {
        // x64 syscall stub: mov r10, rcx (4C 8B D1) followed by mov eax, imm32
        // (B8 ..). Both must appear in the 12 bytes before the syscall; that
        // rules out 0F 05 bytes belonging to a displacement or an immediate.
        const size_t window = at < 12 ? at : 12;
        bool mov_r10_rcx = false;
        bool mov_eax_imm = false;
        for (size_t k = at - window; k + 1 < at; ++k) {
            if (code[k] == 0x4C && code[k + 1] == 0x8B && k + 2 < at &&
                code[k + 2] == 0xD1) {
                mov_r10_rcx = true;
            }
            if (code[k] == 0xB8) {
                mov_eax_imm = true;
            }
        }
        return mov_r10_rcx && mov_eax_imm;
    };

    while (offset + 1 < size) {
        if (code[offset] == 0x0F && code[offset + 1] == 0x05) {
            if (chain_intact || has_stub_prologue(offset)) {
                code[offset + 1] = 0x0B;  // syscall -> ud2, same length
                PatchedSyscalls().insert(reinterpret_cast<uintptr_t>(code + offset));
                ++patched;
                offset += 2;
                continue;
            }
        }

        const size_t length =
            DecodeInstructionLength(code + offset, size - offset);
        if (length == 0) {
            // Unknown encoding: resynchronise one byte at a time instead of
            // abandoning the rest of .text.
            ++resyncs;
            chain_intact = false;
            ++offset;
            continue;
        }
        offset += length;
    }

    LogMessage(LogLevel::Info, "veh",
               "syscall scan finished: patched " + std::to_string(patched) +
                   " site(s), " + std::to_string(resyncs) +
                   " resynchronisation step(s) over " + std::to_string(size) +
                   " bytes");
    return patched;
}

void UnregisterPatchedSyscalls(const uint8_t* base, size_t size) {
    if (!base || size == 0) {
        return;
    }
    const uintptr_t low = reinterpret_cast<uintptr_t>(base);
    const uintptr_t high = low + size;
    std::lock_guard<std::mutex> lock(SiteMutex());
    auto& sites = PatchedSyscalls();
    for (auto it = sites.lower_bound(low); it != sites.end() && *it < high;) {
        it = sites.erase(it);
    }
}

ScopedVehInterceptor::ScopedVehInterceptor() {
    handle_ = AddVectoredExceptionHandler(1, &VehHandler);
    if (!handle_) {
        LogMessage(LogLevel::Error, "veh",
                   "AddVectoredExceptionHandler failed; privileged console "
                   "instructions will crash the guest");
    }
}

ScopedVehInterceptor::~ScopedVehInterceptor() {
    if (handle_) {
        RemoveVectoredExceptionHandler(handle_);
        handle_ = nullptr;
    }
}

uint64_t VehEmulatedInstructionCount() {
    return g_emulated_instructions.load(std::memory_order_relaxed);
}

#else  // !defined(_WIN32)

ScopedVehInterceptor::ScopedVehInterceptor() = default;
ScopedVehInterceptor::~ScopedVehInterceptor() = default;
uint64_t VehEmulatedInstructionCount() { return 0; }

bool RegisterXboxSyscallHandler(uint32_t /*number*/, const char* /*name*/,
                                XboxSyscallHandler /*handler*/) {
    return false;
}

size_t LoadXboxSyscallMapFromString(const char* /*mapping*/) { return 0; }

size_t PatchSyscallSites(uint8_t* /*code*/, size_t /*size*/) { return 0; }

void UnregisterPatchedSyscalls(const uint8_t* /*base*/, size_t /*size*/) {}

#endif

}  // namespace scarlettx
