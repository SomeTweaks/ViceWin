#pragma once

#include <cstdint>
#include <string>

struct ViceWinLaunchSettings {
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t target_fps = 60;
    bool vsync = true;
};

bool LaunchPackageWithScarlettX(
    const std::wstring& executable_path,
    const ViceWinLaunchSettings& settings,
    std::wstring& error_message);
