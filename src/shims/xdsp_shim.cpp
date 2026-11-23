#include "core/logger.h"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#endif

// chad.dll is the Xbox audio DSP ("console hardware audio driver"). The title
// activates a DSP graph, connects streams to it and submits commands. There is
// no DSP on the host, so every entry point reports success and the audio
// engine falls back to its software mixing path (which the xaudio2 shim
// already implements).
//
// The exact parameter shapes are not public. Nothing is written through an
// argument unless the pointer is verifiably committed and writable, so a wrong
// guess cannot corrupt the guest's heap; the first call of each entry point is
// logged with its raw arguments so the real shapes can be read off the log.

namespace {

std::mutex& DspMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_set<std::string>& LoggedEntryPoints() {
    static std::unordered_set<std::string> logged;
    return logged;
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

void LogFirstCall(const char* entry, uint64_t a0, uint64_t a1, uint64_t a2,
                  uint64_t a3) {
    {
        std::lock_guard<std::mutex> lock(DspMutex());
        if (!LoggedEntryPoints().insert(entry).second) {
            return;
        }
    }
    scarlettx::LogMessage(scarlettx::LogLevel::Info, "chad",
                          std::string(entry) + " first call: a0=" + Hex(a0) +
                              " a1=" + Hex(a1) + " a2=" + Hex(a2) +
                              " a3=" + Hex(a3) +
                              " (DSP is stubbed; reporting success)");
}

// True only when [address, address+bytes) is committed and writable in this
// process. Used before writing through a parameter whose meaning is a guess.
bool WritablePointer(const void* address, size_t bytes) {
#if defined(_WIN32)
    if (!address || bytes == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) == 0) {
        return false;
    }
    if (info.State != MEM_COMMIT) {
        return false;
    }
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((info.Protect & writable) == 0) {
        return false;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t region_end =
        reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return start + bytes <= region_end;
#else
    (void)address;
    (void)bytes;
    return false;
#endif
}

// A stable non-null token the guest can treat as an opaque DSP handle.
std::uintptr_t& DspToken() {
    static std::uintptr_t token = 0xC0FFEE01u;
    return token;
}

}  // namespace

#if defined(_WIN32)
extern "C" {
#endif

// HRESULT XDspActivate(<activation params>, XDSP_HANDLE* out_handle)
long XDspActivate(void* a0, void* a1) {
    LogFirstCall("XDspActivate", reinterpret_cast<uint64_t>(a0),
                 reinterpret_cast<uint64_t>(a1), 0, 0);
    // The handle out-parameter is the last argument at every known call site.
    if (WritablePointer(a1, sizeof(void*))) {
        *static_cast<void**>(a1) = reinterpret_cast<void*>(DspToken());
    } else if (WritablePointer(a0, sizeof(void*))) {
        *static_cast<void**>(a0) = reinterpret_cast<void*>(DspToken());
    }
    return 0;  // S_OK
}

// HRESULT XDspDeactivate(XDSP_HANDLE handle)
long XDspDeactivate(void* a0) {
    LogFirstCall("XDspDeactivate", reinterpret_cast<uint64_t>(a0), 0, 0, 0);
    return 0;
}

// HRESULT XDspConnectWithMaximumStreamLimit(handle, params, limit, out_stream)
long XDspConnectWithMaximumStreamLimit(void* a0, void* a1, void* a2, void* a3) {
    LogFirstCall("XDspConnectWithMaximumStreamLimit",
                 reinterpret_cast<uint64_t>(a0), reinterpret_cast<uint64_t>(a1),
                 reinterpret_cast<uint64_t>(a2), reinterpret_cast<uint64_t>(a3));
    if (WritablePointer(a3, sizeof(void*))) {
        *static_cast<void**>(a3) = reinterpret_cast<void*>(DspToken() + 1);
    }
    return 0;
}

// HRESULT XDspDisconnect(stream handle)
long XDspDisconnect(void* a0) {
    LogFirstCall("XDspDisconnect", reinterpret_cast<uint64_t>(a0), 0, 0, 0);
    return 0;
}

// HRESULT XDspSubmitCommand(handle, command block, size)
long XDspSubmitCommand(void* a0, void* a1, uint32_t a2) {
    LogFirstCall("XDspSubmitCommand", reinterpret_cast<uint64_t>(a0),
                 reinterpret_cast<uint64_t>(a1), a2, 0);
    return 0;
}

#if defined(_WIN32)
}  // extern "C"
#endif
