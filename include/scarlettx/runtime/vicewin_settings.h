#pragma once

#include <cstdint>

#if defined(_WIN32)
#if defined(SCARLETTX_BUILDING_VICEWIN_CORE)
#define VICEWIN_CORE_API __declspec(dllexport)
#else
#define VICEWIN_CORE_API __declspec(dllimport)
#endif
#else
#define VICEWIN_CORE_API
#endif

struct ViceWinRuntimeSettings {
    uint32_t width;
    uint32_t height;
    uint32_t target_fps;
    uint32_t target_fps_configured;
    uint32_t vsync;
};

extern "C" VICEWIN_CORE_API int32_t ViceWinGetRuntimeSettings(
    ViceWinRuntimeSettings* settings);
