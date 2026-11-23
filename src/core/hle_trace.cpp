#include "scarlettx/core/hle_trace.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "core/logger.h"

namespace scarlettx {

namespace {

constexpr size_t kRingSize = 64;

struct TraceEntry {
    const char* function = "";
    uint64_t result = 0;
    uint64_t a0 = 0;
    uint64_t a1 = 0;
};

TraceEntry& EntryAt(size_t index) {
    static TraceEntry entries[kRingSize];
    return entries[index % kRingSize];
}

std::atomic<uint64_t> g_next{0};
std::mutex& TraceMutex() {
    static std::mutex mutex;
    return mutex;
}

std::string Hex(uint64_t value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llX",
                  static_cast<unsigned long long>(value));
    return buffer;
}

// Null, STATUS_NOT_SUPPORTED and generic failure codes are what a title turns
// into an assert; everything else only enters the ring.
bool IsSuspicious(uint64_t result) {
    return result == 0 || result == 0xC00000BBull || result == 0x80004005ull ||
           result == 0x80004001ull || result == 0x80070057ull;
}

void Record(const char* function, uint64_t result, uint64_t a0, uint64_t a1) {
    const uint64_t slot = g_next.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(TraceMutex());
        TraceEntry& entry = EntryAt(static_cast<size_t>(slot));
        entry.function = function;
        entry.result = result;
        entry.a0 = a0;
        entry.a1 = a1;
    }
    if (IsSuspicious(result)) {
        LogMessage(LogLevel::Warning, "hle",
                   std::string(function) + " returned " + Hex(result) +
                       " (a0=" + Hex(a0) + " a1=" + Hex(a1) + ")");
    }
}

}  // namespace

void HleTrace(const char* function, uint64_t result) {
    Record(function, result, 0, 0);
}

void HleTrace(const char* function, uint64_t result, uint64_t a0, uint64_t a1) {
    Record(function, result, a0, a1);
}

void HleTraceFaultDump(const char* reason) {
    const uint64_t next = g_next.load(std::memory_order_relaxed);
    const size_t available =
        next < kRingSize ? static_cast<size_t>(next) : kRingSize;
    if (available == 0) {
        LogMessage(LogLevel::Warning, "hle",
                   std::string("no HLE calls recorded before: ") + reason);
        return;
    }
    std::string text = "HLE calls before ";
    text += reason;
    text += ":";
    std::lock_guard<std::mutex> lock(TraceMutex());
    const uint64_t first = next - available;
    for (uint64_t index = first; index < next; ++index) {
        const TraceEntry& entry = EntryAt(static_cast<size_t>(index));
        text += "\n    ";
        text += entry.function;
        text += " -> ";
        text += Hex(entry.result);
        text += " (a0=";
        text += Hex(entry.a0);
        text += " a1=";
        text += Hex(entry.a1);
        text += ")";
    }
    LogMessage(LogLevel::Error, "hle", text);
}

}  // namespace scarlettx
