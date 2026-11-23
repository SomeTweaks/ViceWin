#include "scarlettx/runtime/vicewin_settings.h"
#include "core/logger.h"

#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

ViceWinRuntimeSettings g_settings{};
std::once_flag g_settings_once;

#if defined(_WIN32)
uint32_t ReadEnvironmentNumber(
    const wchar_t* name, uint32_t maximum, uint32_t* configured = nullptr) {
    if (configured) {
        *configured = 0;
    }
    wchar_t value[32] = {};
    const DWORD length = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
    if (length == 0 || length >= std::size(value)) {
        return 0;
    }

    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(value, &end, 10);
    if (end == value || *end != L'\0' || parsed > maximum) {
        return 0;
    }
    if (configured) {
        *configured = 1;
    }
    return static_cast<uint32_t>(parsed);
}

uint32_t ReadVSync() {
    wchar_t value[16] = {};
    const DWORD length = GetEnvironmentVariableW(
        L"VICEWIN_VSYNC", value, static_cast<DWORD>(std::size(value)));
    if (length == 0 || length >= std::size(value)) {
        return 0;
    }
    return value[0] == L'1' || value[0] == L'y' || value[0] == L'Y' ||
                   value[0] == L't' || value[0] == L'T' ||
                   ((value[0] == L'o' || value[0] == L'O') &&
                    (value[1] == L'n' || value[1] == L'N'))
               ? 1u
               : 0u;
}
#endif

void LoadSettings() {
#if defined(_WIN32)
    g_settings.width = ReadEnvironmentNumber(L"VICEWIN_WIDTH", 16384);
    g_settings.height = ReadEnvironmentNumber(L"VICEWIN_HEIGHT", 16384);
    if (g_settings.width == 0 || g_settings.height == 0) {
        g_settings.width = 0;
        g_settings.height = 0;
    }
    g_settings.target_fps = ReadEnvironmentNumber(
        L"VICEWIN_FPS", 1000, &g_settings.target_fps_configured);
    g_settings.vsync = ReadVSync();
    scarlettx::LogMessage(
        scarlettx::LogLevel::Info, "vicewin_core",
        "Runtime settings loaded: " + std::to_string(g_settings.width) + "x" +
            std::to_string(g_settings.height) + ", fps=" +
            std::to_string(g_settings.target_fps) + ", vsync=" +
            (g_settings.vsync ? "on" : "off"));
#endif
}

}  // namespace

extern "C" int32_t ViceWinGetRuntimeSettings(ViceWinRuntimeSettings* settings) {
    if (!settings) {
        return -1;
    }
    std::call_once(g_settings_once, LoadSettings);
    *settings = g_settings;
    return 0;
}
