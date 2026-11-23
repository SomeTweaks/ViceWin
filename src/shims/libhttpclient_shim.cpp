#define SCARLETTX_BUILDING_LIBHTTPCLIENT 1
#include "scarlettx/net/httpclient_abi.h"
#include "core/logger.h"
#include "scarlettx/core/hle_trace.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

struct HC_CALL {
    std::string method = "GET";
    std::string url;
    std::unordered_map<std::string, std::string> req_headers;
    std::vector<uint8_t> req_body;
    uint32_t timeout_seconds = 30;

    uint32_t status_code = 0;
    SX_HRESULT network_error = SX_S_OK;
    uint32_t platform_error = 0;
    std::vector<uint8_t> resp_body;
    std::string resp_string;
    std::unordered_map<std::string, std::string> resp_headers;
};

namespace {

struct HttpMockRule {
    std::string method;
    std::string url_prefix;
    uint32_t status_code = 200;
    std::vector<uint8_t> body;
    std::string content_type = "application/json";
};

std::mutex g_mutex;
bool g_initialized = false;
std::unordered_map<HC_CALL*, std::unique_ptr<HC_CALL>> g_calls;
std::vector<HttpMockRule> g_mocks;

std::mutex& EntryLogMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_set<std::string>& LoggedEntryPoints() {
    static std::unordered_set<std::string> logged;
    return logged;
}

// Trace level recorded by HCHttpCallSetTracing and read back by
// HCSettingsGetTraceLevel. Zero is both the initial value and the libHttpClient
// default, so a title that never opts in keeps tracing off.
std::atomic<uint32_t>& TraceLevel() {
    static std::atomic<uint32_t> level{0};
    return level;
}

// Handed out by the URL and platform-error getters. Nothing writes through
// these pointers and the array outlives the process, so the address stays valid
// for as long as the title keeps it.
const char kEmptyString[] = "";

std::string Hex(uint64_t value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llX",
                  static_cast<unsigned long long>(value));
    return buffer;
}

// The offline client has no headers, no request URL and no platform network
// error to report, so most of the introspection entry points answer with empty
// values. The first call of each one is logged with its raw arguments so the
// call order and the real argument shapes can be read off the log.
void LogFirstCall(const char* entry, uint64_t a0, uint64_t a1, uint64_t a2,
                  uint64_t a3, uint64_t a4 = 0) {
    {
        std::lock_guard<std::mutex> lock(EntryLogMutex());
        if (!LoggedEntryPoints().insert(entry).second) {
            return;
        }
    }
    scarlettx::LogMessage(scarlettx::LogLevel::Info, "libhttpclient",
                          std::string(entry) + " first call: a0=" + Hex(a0) +
                              " a1=" + Hex(a1) + " a2=" + Hex(a2) +
                              " a3=" + Hex(a3) + " a4=" + Hex(a4));
}

// True only when [address, address + bytes) is committed and writable in this
// process. Every out-parameter is written through this check first, so a call
// site that passes something unexpected cannot corrupt the guest heap.
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
    if ((info.Protect & PAGE_GUARD) != 0) {
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

// Copies a NUL-terminated string only after VirtualQuery confirmed the bytes up
// to the terminator (capped at max_bytes) live in one committed, readable
// region. HCTraceImplMessage receives strings straight from the guest, so they
// are never dereferenced unchecked.
std::string ReadableCString(const void* address, size_t max_bytes) {
#if defined(_WIN32)
    if (!address || max_bytes == 0) {
        return std::string();
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) == 0) {
        return std::string();
    }
    if (info.State != MEM_COMMIT) {
        return std::string();
    }
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY;
    if ((info.Protect & readable) == 0) {
        return std::string();
    }
    if ((info.Protect & PAGE_GUARD) != 0) {
        return std::string();
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t region_end =
        reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    const size_t available = static_cast<size_t>(region_end - start);
    const size_t limit = std::min(available, max_bytes);
    const char* text = static_cast<const char*>(address);
    size_t length = 0;
    while (length < limit && text[length] != '\0') {
        ++length;
    }
    return std::string(text, length);
#else
    (void)address;
    (void)max_bytes;
    return std::string();
#endif
}

}  // namespace

extern "C" {

SX_HRESULT HCInitialize(void* args) {
    (void)args;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_initialized = true;
    return SX_S_OK;
}

void HCCleanup(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_calls.clear();
    g_initialized = false;
}

uint32_t HCIsInitialized(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_initialized ? 1u : 0u;
}

SX_HRESULT HCGetLibVersion(const char** version) {
    if (!version) return SX_E_INVALIDARG;
    *version = "ScarlettX-libHttpClient-GDK-1.0";
    return SX_S_OK;
}

SX_HRESULT HCHttpCallCreate(HCCallHandle* call) {
    if (!call) return SX_E_INVALIDARG;
    *call = nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto obj = std::make_unique<HC_CALL>();
    HC_CALL* raw = obj.get();
    g_calls[raw] = std::move(obj);
    *call = raw;
    scarlettx::HleTrace("HCHttpCallCreate", reinterpret_cast<uint64_t>(raw), 0, 0);
    return SX_S_OK;
}

SX_HRESULT HCHttpCallCloseHandle(HCCallHandle call) {
    if (!call) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_calls.erase(call) > 0 ? SX_S_OK : SX_E_INVALIDARG;
}

SX_HRESULT HCHttpCallRequestSetUrl(HCCallHandle call, const char* method, const char* url) {
    if (!call || !method || !url) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    call->method = method;
    call->url = url;
    return SX_S_OK;
}

SX_HRESULT HCHttpCallRequestSetHeader(
    HCCallHandle call,
    const char* header_name,
    const char* header_value,
    uint32_t allow_tracing) {
    (void)allow_tracing;
    if (!call || !header_name || !header_value) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    call->req_headers[header_name] = header_value;
    return SX_S_OK;
}

SX_HRESULT HCHttpCallRequestSetRequestBodyBytes(
    HCCallHandle call,
    const uint8_t* request_body_bytes,
    uint32_t request_body_size) {
    if (!call || (!request_body_bytes && request_body_size > 0)) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    call->req_body.assign(request_body_bytes, request_body_bytes + request_body_size);
    return SX_S_OK;
}

SX_HRESULT HCHttpCallRequestSetTimeout(HCCallHandle call, uint32_t timeout_in_seconds) {
    if (!call) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    call->timeout_seconds = timeout_in_seconds;
    return SX_S_OK;
}

SX_HRESULT HCHttpCallPerformAsync(HCCallHandle call, XAsyncBlock* async_block) {
    if (!call) return SX_E_INVALIDARG;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;

        bool matched = false;
        for (const auto& rule : g_mocks) {
            const bool method_ok = rule.method.empty() || rule.method == "*" || rule.method == call->method;
            const bool url_ok = call->url.rfind(rule.url_prefix, 0) == 0;
            if (method_ok && url_ok) {
                call->status_code = rule.status_code;
                call->network_error = SX_S_OK;
                call->platform_error = 0;
                call->resp_body = rule.body;
                call->resp_string.assign(
                    reinterpret_cast<const char*>(rule.body.data()), rule.body.size());
                call->resp_headers["Content-Type"] = rule.content_type;
                call->resp_headers["X-ScarlettX-Offline"] = "1";
                matched = true;
                break;
            }
        }
        if (!matched) {
            // Deterministic offline fallback so titles probing telemetry/auth don't hang
            const std::string fallback = "{\"status\":\"offline\",\"emulator\":\"ScarlettX\"}";
            call->status_code = 503;
            call->network_error = SX_S_OK;
            call->platform_error = 0;
            call->resp_string = fallback;
            call->resp_body.assign(fallback.begin(), fallback.end());
            call->resp_headers["Content-Type"] = "application/json";
            call->resp_headers["X-ScarlettX-Offline"] = "1";
        }
    }

    if (async_block) {
        if (async_block->callback) {
            async_block->callback(async_block);
        }
    }
    return SX_S_OK;
}

SX_HRESULT HCHttpCallResponseGetStatusCode(HCCallHandle call, uint32_t* status_code) {
    if (!call || !status_code) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    *status_code = call->status_code;
    return SX_S_OK;
}

SX_HRESULT HCHttpCallResponseGetNetworkErrorCode(
    HCCallHandle call,
    SX_HRESULT* network_error_code,
    uint32_t* platform_network_error_code) {
    if (!call || !network_error_code) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    *network_error_code = call->network_error;
    if (platform_network_error_code) {
        *platform_network_error_code = call->platform_error;
    }
    return SX_S_OK;
}

SX_HRESULT HCHttpCallResponseGetResponseString(HCCallHandle call, const char** response_string) {
    if (!call || !response_string) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    *response_string = call->resp_string.c_str();
    return SX_S_OK;
}

SX_HRESULT HCHttpCallResponseGetResponseBodyBytesSize(HCCallHandle call, size_t* buffer_size) {
    if (!call || !buffer_size) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    *buffer_size = call->resp_body.size();
    return SX_S_OK;
}

SX_HRESULT HCHttpCallResponseGetResponseBodyBytes(
    HCCallHandle call,
    size_t buffer_size,
    uint8_t* buffer,
    size_t* buffer_used) {
    if (!call || (!buffer && buffer_size > 0)) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    const size_t n = std::min(buffer_size, call->resp_body.size());
    if (n > 0) {
        std::memcpy(buffer, call->resp_body.data(), n);
    }
    if (buffer_used) {
        *buffer_used = n;
    }
    return SX_S_OK;
}

SX_HRESULT HCHttpCallResponseGetHeader(
    HCCallHandle call,
    const char* header_name,
    const char** header_value) {
    if (!call || !header_name || !header_value) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    auto it = call->resp_headers.find(header_name);
    *header_value = (it != call->resp_headers.end()) ? it->second.c_str() : nullptr;
    return SX_S_OK;
}

SX_HRESULT HCHttpCallRegisterMock(
    const char* method,
    const char* url_prefix,
    uint32_t status_code,
    const uint8_t* body,
    uint32_t body_size,
    const char* content_type) {
    if (!url_prefix) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    HttpMockRule rule{};
    rule.method = method ? method : "*";
    rule.url_prefix = url_prefix;
    rule.status_code = status_code;
    if (body && body_size > 0) {
        rule.body.assign(body, body + body_size);
    }
    rule.content_type = content_type ? content_type : "application/octet-stream";
    g_mocks.push_back(std::move(rule));
    return SX_S_OK;
}

void HCHttpCallClearMocks(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_mocks.clear();
}

// ---------------------------------------------------------------------------
// Handle lifetime: duplication and async cleanup
// ---------------------------------------------------------------------------

// HRESULT HCHttpCallDuplicateHandle(HCCallHandle call, HCCallHandle* duplicated)
// The offline client hands the same handle back. Every other entry point then
// keeps working against the one registry entry; the reference client calls
// HCHttpCallCloseHandle once per handle it holds, which stays correct here
// because a duplicated handle is still the same live entry.
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallDuplicateHandle(
    HCCallHandle call,
    HCCallHandle* duplicated) {
    LogFirstCall("HCHttpCallDuplicateHandle", reinterpret_cast<uint64_t>(call),
                 reinterpret_cast<uint64_t>(duplicated), 0, 0);
    if (!call || !duplicated) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    if (WritablePointer(duplicated, sizeof(*duplicated))) {
        *duplicated = call;
    }
    return SX_S_OK;
}

// HRESULT HCCleanupAsync(XAsyncBlock* async)
// Nothing is torn down asynchronously: HCCleanup already drops the call registry
// synchronously, so the request is only acknowledged.
SCARLETTX_HC_API SX_HRESULT __stdcall HCCleanupAsync(XAsyncBlock* async_block) {
    LogFirstCall("HCCleanupAsync", reinterpret_cast<uint64_t>(async_block), 0, 0,
                 0);
    if (!async_block) return SX_E_INVALIDARG;
    return SX_S_OK;
}

// ---------------------------------------------------------------------------
// Request introspection
// ---------------------------------------------------------------------------

// HRESULT HCHttpCallRequestGetNumHeaders(HCCallHandle call, uint32_t* numHeaders)
// Headers set through HCHttpCallRequestSetHeader stay inside the offline client;
// the title only needs a count to iterate, and zero short-circuits that loop.
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallRequestGetNumHeaders(
    HCCallHandle call,
    uint32_t* num_headers) {
    LogFirstCall("HCHttpCallRequestGetNumHeaders",
                 reinterpret_cast<uint64_t>(call),
                 reinterpret_cast<uint64_t>(num_headers), 0, 0);
    if (!call || !num_headers) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    if (WritablePointer(num_headers, sizeof(*num_headers))) {
        *num_headers = 0;
    }
    return SX_S_OK;
}

// HRESULT HCHttpCallRequestGetHeaderAtIndex(HCCallHandle call, uint32_t index,
//                                          const char** headerName,
//                                          const char** headerValue)
// Only index 0 is answered (with empty values) because the count above is zero;
// anything past it is out of range.
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallRequestGetHeaderAtIndex(
    HCCallHandle call,
    uint32_t index,
    const char** header_name,
    const char** header_value) {
    LogFirstCall("HCHttpCallRequestGetHeaderAtIndex",
                 reinterpret_cast<uint64_t>(call), index,
                 reinterpret_cast<uint64_t>(header_name),
                 reinterpret_cast<uint64_t>(header_value));
    if (!call || !header_name || !header_value) return SX_E_INVALIDARG;
    if (index != 0) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    if (WritablePointer(header_name, sizeof(*header_name))) {
        *header_name = nullptr;
    }
    if (WritablePointer(header_value, sizeof(*header_value))) {
        *header_value = nullptr;
    }
    return SX_S_OK;
}

// HRESULT HCHttpCallRequestGetUrl(HCCallHandle call, const char** url)
// The offline client has no outbound URL to report, so the answer is the empty
// string. HCHttpCallGetRequestUrl is the older name for the same query.
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallRequestGetUrl(HCCallHandle call,
                                                              const char** url) {
    LogFirstCall("HCHttpCallRequestGetUrl", reinterpret_cast<uint64_t>(call),
                 reinterpret_cast<uint64_t>(url), 0, 0);
    if (!call || !url) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    if (WritablePointer(url, sizeof(*url))) {
        *url = kEmptyString;
    }
    return SX_S_OK;
}

// HRESULT HCHttpCallGetRequestUrl(HCCallHandle call, const char** url)
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallGetRequestUrl(HCCallHandle call,
                                                              const char** url) {
    LogFirstCall("HCHttpCallGetRequestUrl", reinterpret_cast<uint64_t>(call),
                 reinterpret_cast<uint64_t>(url), 0, 0);
    if (!call || !url) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    if (WritablePointer(url, sizeof(*url))) {
        *url = kEmptyString;
    }
    return SX_S_OK;
}

// HRESULT HCHttpCallRequestGetRequestBodyBytes(HCCallHandle call,
//                                             const uint8_t** requestBodyBytes,
//                                             uint32_t* requestBodySize)
// No request body is exposed back to the title; both out-parameters are emptied.
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallRequestGetRequestBodyBytes(
    HCCallHandle call,
    const uint8_t** request_body_bytes,
    uint32_t* request_body_size) {
    LogFirstCall("HCHttpCallRequestGetRequestBodyBytes",
                 reinterpret_cast<uint64_t>(call),
                 reinterpret_cast<uint64_t>(request_body_bytes),
                 reinterpret_cast<uint64_t>(request_body_size), 0);
    if (!call || !request_body_bytes || !request_body_size) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    if (WritablePointer(request_body_bytes, sizeof(*request_body_bytes))) {
        *request_body_bytes = nullptr;
    }
    if (WritablePointer(request_body_size, sizeof(*request_body_size))) {
        *request_body_size = 0;
    }
    return SX_S_OK;
}

// HRESULT HCHttpCallRequestSetRequestBodyString(HCCallHandle call,
//                                              const char* requestBodyString)
// Accepted and dropped: the offline handler answers from the mock table or the
// synthetic fallback and never forwards a request body.
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallRequestSetRequestBodyString(
    HCCallHandle call,
    const char* request_body_string) {
    LogFirstCall("HCHttpCallRequestSetRequestBodyString",
                 reinterpret_cast<uint64_t>(call),
                 reinterpret_cast<uint64_t>(request_body_string), 0, 0);
    if (!call || !request_body_string) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    return SX_S_OK;
}

// ---------------------------------------------------------------------------
// Retry and timeout knobs
// ---------------------------------------------------------------------------
// There is no network, so no call is ever retried and no timeout is ever armed.
// The setters accept the values, the getters report the libHttpClient defaults
// (retry off, 60 s timeout and a 60 s timeout window).

// HRESULT HCHttpCallRequestGetRetryAllowed(HCCallHandle call, bool* retryAllowed)
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallRequestGetRetryAllowed(
    HCCallHandle call,
    bool* retry_allowed) {
    LogFirstCall("HCHttpCallRequestGetRetryAllowed",
                 reinterpret_cast<uint64_t>(call),
                 reinterpret_cast<uint64_t>(retry_allowed), 0, 0);
    if (!call || !retry_allowed) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    if (WritablePointer(retry_allowed, sizeof(*retry_allowed))) {
        *retry_allowed = false;
    }
    return SX_S_OK;
}

// HRESULT HCHttpCallRequestSetRetryAllowed(HCCallHandle call, bool retryAllowed)
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallRequestSetRetryAllowed(
    HCCallHandle call,
    bool retry_allowed) {
    LogFirstCall("HCHttpCallRequestSetRetryAllowed",
                 reinterpret_cast<uint64_t>(call), retry_allowed ? 1u : 0u, 0, 0);
    if (!call) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    return SX_S_OK;
}

// HRESULT HCHttpCallRequestGetRetryCacheId(HCCallHandle call,
//                                         uint32_t* retryCacheId)
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallRequestGetRetryCacheId(
    HCCallHandle call,
    uint32_t* retry_cache_id) {
    LogFirstCall("HCHttpCallRequestGetRetryCacheId",
                 reinterpret_cast<uint64_t>(call),
                 reinterpret_cast<uint64_t>(retry_cache_id), 0, 0);
    if (!call || !retry_cache_id) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    if (WritablePointer(retry_cache_id, sizeof(*retry_cache_id))) {
        *retry_cache_id = 0;
    }
    return SX_S_OK;
}

// HRESULT HCHttpCallRequestSetRetryCacheId(HCCallHandle call,
//                                         uint32_t retryAfterCacheId)
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallRequestSetRetryCacheId(
    HCCallHandle call,
    uint32_t retry_cache_id) {
    LogFirstCall("HCHttpCallRequestSetRetryCacheId",
                 reinterpret_cast<uint64_t>(call), retry_cache_id, 0, 0);
    if (!call) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    return SX_S_OK;
}

// HRESULT HCHttpCallRequestGetRetryDelay(HCCallHandle call,
//                                       uint32_t* retryDelayInSeconds)
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallRequestGetRetryDelay(
    HCCallHandle call,
    uint32_t* retry_delay_in_seconds) {
    LogFirstCall("HCHttpCallRequestGetRetryDelay",
                 reinterpret_cast<uint64_t>(call),
                 reinterpret_cast<uint64_t>(retry_delay_in_seconds), 0, 0);
    if (!call || !retry_delay_in_seconds) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    if (WritablePointer(retry_delay_in_seconds, sizeof(*retry_delay_in_seconds))) {
        *retry_delay_in_seconds = 0;
    }
    return SX_S_OK;
}

// HRESULT HCHttpCallRequestSetRetryDelay(HCCallHandle call,
//                                       uint32_t retryDelayInSeconds)
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallRequestSetRetryDelay(
    HCCallHandle call,
    uint32_t retry_delay_in_seconds) {
    LogFirstCall("HCHttpCallRequestSetRetryDelay",
                 reinterpret_cast<uint64_t>(call), retry_delay_in_seconds, 0, 0);
    if (!call) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    return SX_S_OK;
}

// HRESULT HCHttpCallRequestGetTimeout(HCCallHandle call,
//                                    uint32_t* timeoutInMilliseconds)
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallRequestGetTimeout(
    HCCallHandle call,
    uint32_t* timeout_in_milliseconds) {
    LogFirstCall("HCHttpCallRequestGetTimeout", reinterpret_cast<uint64_t>(call),
                 reinterpret_cast<uint64_t>(timeout_in_milliseconds), 0, 0);
    if (!call || !timeout_in_milliseconds) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    if (WritablePointer(timeout_in_milliseconds,
                        sizeof(*timeout_in_milliseconds))) {
        *timeout_in_milliseconds = 60000;
    }
    return SX_S_OK;
}

// HRESULT HCHttpCallRequestGetTimeoutWindow(HCCallHandle call,
//                                          uint32_t* timeoutWindowInMilliseconds)
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallRequestGetTimeoutWindow(
    HCCallHandle call,
    uint32_t* timeout_window_in_milliseconds) {
    LogFirstCall("HCHttpCallRequestGetTimeoutWindow",
                 reinterpret_cast<uint64_t>(call),
                 reinterpret_cast<uint64_t>(timeout_window_in_milliseconds), 0,
                 0);
    if (!call || !timeout_window_in_milliseconds) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    if (WritablePointer(timeout_window_in_milliseconds,
                        sizeof(*timeout_window_in_milliseconds))) {
        *timeout_window_in_milliseconds = 60000;
    }
    return SX_S_OK;
}

// HRESULT HCHttpCallRequestSetTimeoutWindow(HCCallHandle call,
//                                          uint32_t timeoutWindowInMilliseconds)
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallRequestSetTimeoutWindow(
    HCCallHandle call,
    uint32_t timeout_window_in_milliseconds) {
    LogFirstCall("HCHttpCallRequestSetTimeoutWindow",
                 reinterpret_cast<uint64_t>(call), timeout_window_in_milliseconds,
                 0, 0);
    if (!call) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    return SX_S_OK;
}

// ---------------------------------------------------------------------------
// Response introspection
// ---------------------------------------------------------------------------

// HRESULT HCHttpCallResponseGetNumHeaders(HCCallHandle call,
//                                        uint32_t* numHeaders)
// HCHttpCallPerformAsync fills Content-Type and X-ScarlettX-Offline, but those
// are reachable by name through HCHttpCallResponseGetHeader; exposing them by
// index would need stable per-call name/value storage, so the count stays zero.
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallResponseGetNumHeaders(
    HCCallHandle call,
    uint32_t* num_headers) {
    LogFirstCall("HCHttpCallResponseGetNumHeaders",
                 reinterpret_cast<uint64_t>(call),
                 reinterpret_cast<uint64_t>(num_headers), 0, 0);
    if (!call || !num_headers) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    if (WritablePointer(num_headers, sizeof(*num_headers))) {
        *num_headers = 0;
    }
    return SX_S_OK;
}

// HRESULT HCHttpCallResponseGetHeaderAtIndex(HCCallHandle call, uint32_t index,
//                                           const char** headerName,
//                                           const char** headerValue)
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallResponseGetHeaderAtIndex(
    HCCallHandle call,
    uint32_t index,
    const char** header_name,
    const char** header_value) {
    LogFirstCall("HCHttpCallResponseGetHeaderAtIndex",
                 reinterpret_cast<uint64_t>(call), index,
                 reinterpret_cast<uint64_t>(header_name),
                 reinterpret_cast<uint64_t>(header_value));
    if (!call || !header_name || !header_value) return SX_E_INVALIDARG;
    if (index != 0) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    if (WritablePointer(header_name, sizeof(*header_name))) {
        *header_name = nullptr;
    }
    if (WritablePointer(header_value, sizeof(*header_value))) {
        *header_value = nullptr;
    }
    return SX_S_OK;
}

// HRESULT HCHttpCallResponseGetPlatformNetworkErrorMessage(
//     HCCallHandle call, const char** platformNetworkErrorMessage)
// Every offline answer is synthesized locally, so there is no platform error.
SCARLETTX_HC_API SX_HRESULT __stdcall
HCHttpCallResponseGetPlatformNetworkErrorMessage(HCCallHandle call,
                                                 const char** message) {
    LogFirstCall("HCHttpCallResponseGetPlatformNetworkErrorMessage",
                 reinterpret_cast<uint64_t>(call),
                 reinterpret_cast<uint64_t>(message), 0, 0);
    if (!call || !message) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    if (WritablePointer(message, sizeof(*message))) {
        *message = kEmptyString;
    }
    return SX_S_OK;
}

// ---------------------------------------------------------------------------
// Tracing and settings
// ---------------------------------------------------------------------------

// HRESULT HCHttpCallSetTracing(uint32_t traceLevel)  (deprecated ABI kept alive)
// The level is recorded so HCSettingsGetTraceLevel can report what the title
// asked for; the real client's trace plumbing does not exist offline.
SCARLETTX_HC_API SX_HRESULT __stdcall HCHttpCallSetTracing(uint32_t trace_level) {
    LogFirstCall("HCHttpCallSetTracing", trace_level, 0, 0, 0);
    TraceLevel().store(trace_level);
    return SX_S_OK;
}

// HRESULT HCSettingsGetTraceLevel(uint32_t* traceLevel)
SCARLETTX_HC_API SX_HRESULT __stdcall HCSettingsGetTraceLevel(
    uint32_t* trace_level) {
    LogFirstCall("HCSettingsGetTraceLevel",
                 reinterpret_cast<uint64_t>(trace_level), 0, 0, 0);
    if (!trace_level) return SX_E_INVALIDARG;
    if (WritablePointer(trace_level, sizeof(*trace_level))) {
        *trace_level = TraceLevel().load();
    }
    return SX_S_OK;
}

// void HCTraceImplMessage(const void* area, uint32_t level, const char* message)
// libHttpClient routes its own trace output through here. Error (0) and warning
// (1) levels stay quiet because the stub has nothing to report; info and above
// are forwarded to the ScarlettX log, with the guest string copied only after a
// readability check.
SCARLETTX_HC_API void __stdcall HCTraceImplMessage(const void* area,
                                                   uint32_t level,
                                                   const char* message) {
    LogFirstCall("HCTraceImplMessage", reinterpret_cast<uint64_t>(area), level,
                 reinterpret_cast<uint64_t>(message), 0);
    if (level < 2) return;
    const std::string text = ReadableCString(message, 4096);
    if (text.empty()) return;
    scarlettx::LogMessage(scarlettx::LogLevel::Trace, "libhttpclient", text);
}

// HRESULT HCTraceSetEtwEnabled(bool enabled)
// ETW is not wired up in the emulator, so the request is acknowledged and the
// flag dropped.
SCARLETTX_HC_API SX_HRESULT __stdcall HCTraceSetEtwEnabled(bool enabled) {
    LogFirstCall("HCTraceSetEtwEnabled", enabled ? 1u : 0u, 0, 0, 0);
    return SX_S_OK;
}

}  // extern "C"
