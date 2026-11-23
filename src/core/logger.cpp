#include "logger.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <filesystem>
#include <fstream>
#include <mutex>
#endif

namespace scarlettx {
namespace {

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Info: return "info";
        case LogLevel::Warning: return "warning";
        case LogLevel::Error: return "error";
    }
    return "unknown";
}

std::string FormatLine(
    LogLevel level, std::string_view component, std::string_view message) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    std::string line = "[" + std::to_string(milliseconds) + "][";
    line += LevelName(level);
    line += "][";
    line.append(component.data(), component.size());
    line += "] ";
    line.append(message.data(), message.size());
    line += "\r\n";
    return line;
}

#if defined(_WIN32)
std::wstring LogPath() {
    wchar_t configured[32768] = {};
    const DWORD length = GetEnvironmentVariableW(
        L"VICEWIN_LOG_PATH", configured, static_cast<DWORD>(std::size(configured)));
    if (length > 0 && length < std::size(configured)) {
        return std::wstring(configured, length);
    }

    wchar_t executable[32768] = {};
    const DWORD executable_length = GetModuleFileNameW(
        nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (executable_length == 0 || executable_length >= std::size(executable)) {
        return L"vicewin.log";
    }
    std::wstring path(executable, executable_length);
    const size_t separator = path.find_last_of(L"\\/");
    if (separator != std::wstring::npos) {
        path.resize(separator + 1);
    } else {
        path.clear();
    }
    path += L"vicewin.log";
    return path;
}

HANDLE LogMutex() {
    static HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\ViceWinRuntimeLog");
    return mutex;
}
#else
std::mutex& LogMutex() {
    static std::mutex mutex;
    return mutex;
}
#endif

}  // namespace

// A retail title (or a shim loop) can emit the same warning thousands of
// times per second. After kSpamThreshold identical consecutive entries the
// log collapses them: one marker line, then silence until a different line
// arrives, at which point a single "repeated N more times" summary is
// written. Every line that is written goes to disk immediately.
constexpr uint64_t kSpamThreshold = 50;

struct LogDedupeState {
    std::string last_content;
    uint64_t repeats = 0;
    bool suppressed = false;
};

LogDedupeState& Dedupe() {
    static LogDedupeState state;
    return state;
}

// Decides what to write for `line` (the full formatted line) whose
// spam-relevant content is `content` (the line without the volatile
// timestamp prefix). Fills out_lines with 0..2 lines to append and updates
// the dedupe state.
void ApplySpamCollapse(const std::string& line, const std::string& content,
                       std::vector<std::string>& out_lines) {
    LogDedupeState& dedupe = Dedupe();
    if (content == dedupe.last_content) {
        ++dedupe.repeats;
        if (dedupe.repeats < kSpamThreshold) {
            out_lines.push_back(line);
        } else if (dedupe.repeats == kSpamThreshold) {
            out_lines.push_back(line);
            out_lines.push_back(
                "[ViceWin] collapsing further repeats of the previous line\r\n");
            dedupe.suppressed = true;
        }
        // Past the threshold: write nothing until the content changes.
        return;
    }
    if (dedupe.suppressed && dedupe.repeats > kSpamThreshold) {
        out_lines.push_back(
            "[ViceWin] previous log line repeated " +
            std::to_string(dedupe.repeats - kSpamThreshold) + " more times\r\n");
    }
    dedupe.last_content = content;
    dedupe.repeats = 1;
    dedupe.suppressed = false;
    out_lines.push_back(line);
}

std::string LineContent(const std::string& line) {
    const size_t timestamp_end = line.find(']');
    return timestamp_end == std::string::npos
               ? line
               : line.substr(timestamp_end + 1);
}

void LogMessage(LogLevel level, std::string_view component, std::string_view message) {
    const std::string line = FormatLine(level, component, message);
#if defined(_WIN32)
    HANDLE mutex = LogMutex();
    const DWORD wait_result = mutex ? WaitForSingleObject(mutex, INFINITE) : WAIT_FAILED;
    if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
        OutputDebugStringA("[ViceWin] Could not acquire the log mutex\n");
        return;
    }
    std::vector<std::string> lines;
    ApplySpamCollapse(line, LineContent(line), lines);
    if (!lines.empty()) {
        HANDLE file = CreateFileW(
            LogPath().c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            for (const std::string& out : lines) {
                DWORD written = 0;
                const BOOL succeeded = WriteFile(
                    file, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
                if (!succeeded || written != out.size()) {
                    OutputDebugStringA("[ViceWin] Failed to write vicewin.log\n");
                }
            }
            CloseHandle(file);
        } else {
            OutputDebugStringA("[ViceWin] Could not open vicewin.log\n");
        }
    }
    ReleaseMutex(mutex);
#else
    std::lock_guard<std::mutex> lock(LogMutex());
    const char* configured = std::getenv("VICEWIN_LOG_PATH");
    const std::filesystem::path path =
        configured && *configured ? configured : "vicewin.log";
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (file) {
        std::vector<std::string> lines;
        ApplySpamCollapse(line, LineContent(line), lines);
        for (const std::string& out : lines) {
            file.write(out.data(), static_cast<std::streamsize>(out.size()));
        }
        file.flush();
    } else {
        std::fwrite(line.data(), 1, line.size(), stderr);
    }
#endif
}

}  // namespace scarlettx
