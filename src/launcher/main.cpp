#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "process_launcher.h"
#include "core/logger.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>

namespace {

constexpr wchar_t kWindowClass[] = L"ViceWinLauncherWindow";
constexpr int kPathEdit = 1001;
constexpr int kBrowseButton = 1002;
constexpr int kResolutionCombo = 1003;
constexpr int kWidthEdit = 1004;
constexpr int kHeightEdit = 1005;
constexpr int kFpsCombo = 1006;
constexpr int kVsyncCheck = 1007;
constexpr int kLaunchButton = 1008;
constexpr int kStatusLabel = 1009;

struct Resolution {
    uint32_t width;
    uint32_t height;
    const wchar_t* label;
};

constexpr std::array<Resolution, 5> kResolutions = {{
    {1280, 720, L"1280 x 720"},
    {1600, 900, L"1600 x 900"},
    {1920, 1080, L"1920 x 1080"},
    {2560, 1440, L"2560 x 1440"},
    {3840, 2160, L"3840 x 2160"},
}};
constexpr std::array<const wchar_t*, 6> kFrameRates = {
    L"30", L"45", L"60", L"90", L"120", L"Unlimited"};

HWND g_path_edit = nullptr;
HWND g_resolution_combo = nullptr;
HWND g_width_edit = nullptr;
HWND g_height_edit = nullptr;
HWND g_fps_combo = nullptr;
HWND g_vsync_check = nullptr;
HBRUSH g_dark_brush = nullptr;
std::filesystem::path g_ini_path;

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int text_size = static_cast<int>(text.size());
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), text_size,
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return "[UTF-8 conversion failed]";
    }
    std::string result(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), text_size,
            result.data(), required, nullptr, nullptr) != required) {
        return "[UTF-8 conversion failed]";
    }
    return result;
}

std::wstring GetControlText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(control, text.data(), length + 1);
    }
    text.resize(static_cast<size_t>(length));
    return text;
}

void SetControlText(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

uint32_t ParseDimension(HWND control) {
    const std::wstring text = GetControlText(control);
    wchar_t* end = nullptr;
    const unsigned long value = wcstoul(text.c_str(), &end, 10);
    if (text.empty() || end == text.c_str() || *end != L'\0' ||
        value < 320 || value > 16384) {
        return 0;
    }
    return static_cast<uint32_t>(value);
}

void UpdateCustomResolutionControls() {
    const bool custom = SendMessageW(
        g_resolution_combo, CB_GETCURSEL, 0, 0) ==
        static_cast<LRESULT>(kResolutions.size());
    EnableWindow(g_width_edit, custom);
    EnableWindow(g_height_edit, custom);
}

void LoadIniSettings() {
    wchar_t executable[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(
        nullptr, executable, static_cast<DWORD>(std::size(executable)));
    g_ini_path = length && length < std::size(executable)
        ? std::filesystem::path(executable).parent_path() / L"vicewin.ini"
        : std::filesystem::current_path() / L"vicewin.ini";

    wchar_t path[32768] = {};
    GetPrivateProfileStringW(
        L"Launch", L"Executable", L"", path,
        static_cast<DWORD>(std::size(path)), g_ini_path.c_str());
    SetControlText(g_path_edit, path);

    const uint32_t width = GetPrivateProfileIntW(
        L"Video", L"Width", 1280, g_ini_path.c_str());
    const uint32_t height = GetPrivateProfileIntW(
        L"Video", L"Height", 720, g_ini_path.c_str());
    const uint32_t fps = GetPrivateProfileIntW(
        L"Video", L"FPS", 60, g_ini_path.c_str());
    const uint32_t vsync = GetPrivateProfileIntW(
        L"Video", L"VSync", 1, g_ini_path.c_str());

    size_t resolution_index = kResolutions.size();
    for (size_t i = 0; i < kResolutions.size(); ++i) {
        if (kResolutions[i].width == width && kResolutions[i].height == height) {
            resolution_index = i;
            break;
        }
    }
    SendMessageW(g_resolution_combo, CB_SETCURSEL, resolution_index, 0);
    SetControlText(g_width_edit, std::to_wstring(width));
    SetControlText(g_height_edit, std::to_wstring(height));
    UpdateCustomResolutionControls();

    size_t fps_index = kFrameRates.size() - 1;
    for (size_t i = 0; i + 1 < kFrameRates.size(); ++i) {
        if (fps == _wtoi(kFrameRates[i])) {
            fps_index = i;
            break;
        }
    }
    SendMessageW(g_fps_combo, CB_SETCURSEL, fps_index, 0);
    SendMessageW(
        g_vsync_check, BM_SETCHECK,
        vsync ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool ReadLaunchSettings(ViceWinLaunchSettings& settings, std::wstring& error) {
    const LRESULT resolution_index = SendMessageW(
        g_resolution_combo, CB_GETCURSEL, 0, 0);
    if (resolution_index < 0 ||
        resolution_index > static_cast<LRESULT>(kResolutions.size())) {
        error = L"Choose a screen resolution.";
        return false;
    }
    if (resolution_index == static_cast<LRESULT>(kResolutions.size())) {
        settings.width = ParseDimension(g_width_edit);
        settings.height = ParseDimension(g_height_edit);
        if (settings.width == 0 || settings.height == 0) {
            error = L"Custom width and height must be between 320 and 16384.";
            return false;
        }
    } else {
        settings.width = kResolutions[static_cast<size_t>(resolution_index)].width;
        settings.height = kResolutions[static_cast<size_t>(resolution_index)].height;
    }

    const LRESULT fps_index = SendMessageW(g_fps_combo, CB_GETCURSEL, 0, 0);
    if (fps_index < 0 || fps_index >= static_cast<LRESULT>(kFrameRates.size())) {
        error = L"Choose a target frame rate.";
        return false;
    }
    settings.target_fps = fps_index == static_cast<LRESULT>(kFrameRates.size() - 1)
        ? 0u
        : static_cast<uint32_t>(_wtoi(kFrameRates[static_cast<size_t>(fps_index)]));
    settings.vsync =
        SendMessageW(g_vsync_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
    return true;
}

bool SaveIniSettings(
    const std::wstring& executable,
    const ViceWinLaunchSettings& settings) {
    const std::wstring width = std::to_wstring(settings.width);
    const std::wstring height = std::to_wstring(settings.height);
    const std::wstring fps = std::to_wstring(settings.target_fps);
    const std::wstring vsync = settings.vsync ? L"1" : L"0";
    return WritePrivateProfileStringW(
               L"Launch", L"Executable", executable.c_str(), g_ini_path.c_str()) &&
           WritePrivateProfileStringW(
               L"Video", L"Width", width.c_str(), g_ini_path.c_str()) &&
           WritePrivateProfileStringW(
               L"Video", L"Height", height.c_str(), g_ini_path.c_str()) &&
           WritePrivateProfileStringW(
               L"Video", L"FPS", fps.c_str(), g_ini_path.c_str()) &&
           WritePrivateProfileStringW(
               L"Video", L"VSync", vsync.c_str(), g_ini_path.c_str());
}

void BrowseForExecutable(HWND owner) {
    wchar_t path[32768] = {};
    const std::wstring current = GetControlText(g_path_edit);
    if (current.size() < std::size(path)) {
        std::copy(current.begin(), current.end(), path);
    }
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"Windows executables (*.exe)\0*.exe\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrDefExt = L"exe";
    if (GetOpenFileNameW(&dialog)) {
        SetControlText(g_path_edit, path);
    }
}

HWND AddControl(
    HWND parent, const wchar_t* class_name, const wchar_t* text,
    DWORD style, int x, int y, int width, int height, int id) {
    return CreateWindowExW(
        0, class_name, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
}

void CreateControls(HWND window) {
    AddControl(window, L"STATIC", L"Game executable", 0, 24, 34, 130, 24, 0);
    g_path_edit = AddControl(
        window, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
        152, 30, 365, 27, kPathEdit);
    AddControl(
        window, L"BUTTON", L"Browse...", BS_PUSHBUTTON,
        528, 30, 92, 27, kBrowseButton);

    AddControl(window, L"STATIC", L"Resolution", 0, 24, 88, 125, 24, 0);
    g_resolution_combo = AddControl(
        window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
        152, 82, 180, 160, kResolutionCombo);
    for (const Resolution& resolution : kResolutions) {
        SendMessageW(
            g_resolution_combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(resolution.label));
    }
    SendMessageW(
        g_resolution_combo, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"Custom"));
    AddControl(window, L"STATIC", L"Width", 0, 354, 88, 48, 24, 0);
    g_width_edit = AddControl(
        window, L"EDIT", L"1280", WS_BORDER | ES_NUMBER,
        401, 82, 82, 27, kWidthEdit);
    AddControl(window, L"STATIC", L"Height", 0, 493, 88, 55, 24, 0);
    g_height_edit = AddControl(
        window, L"EDIT", L"720", WS_BORDER | ES_NUMBER,
        545, 82, 75, 27, kHeightEdit);

    AddControl(window, L"STATIC", L"Target FPS", 0, 24, 143, 125, 24, 0);
    g_fps_combo = AddControl(
        window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
        152, 137, 180, 160, kFpsCombo);
    for (const wchar_t* rate : kFrameRates) {
        SendMessageW(g_fps_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(rate));
    }
    g_vsync_check = AddControl(
        window, L"BUTTON", L"V-Sync", BS_AUTOCHECKBOX,
        354, 137, 130, 27, kVsyncCheck);

    AddControl(
        window, L"STATIC", L"Settings are passed to the ScarlettX host; runtime DLLs are staged privately.",
        0, 24, 222, 590, 34, kStatusLabel);
    AddControl(
        window, L"BUTTON", L"Launch", BS_DEFPUSHBUTTON,
        500, 285, 120, 36, kLaunchButton);
    SendMessageW(g_resolution_combo, CB_SETCURSEL, 0, 0);
    SendMessageW(g_fps_combo, CB_SETCURSEL, 2, 0);
    SendMessageW(g_vsync_check, BM_SETCHECK, BST_CHECKED, 0);
    LoadIniSettings();
}

void Launch(HWND window) {
    const std::wstring executable = GetControlText(g_path_edit);
    if (executable.empty()) {
        MessageBoxW(window, L"Browse to a game executable first.", L"ViceWin", MB_ICONWARNING);
        return;
    }
    ViceWinLaunchSettings settings{};
    std::wstring error;
    if (!ReadLaunchSettings(settings, error)) {
        const std::string error_utf8 = WideToUtf8(error);
        scarlettx::LogMessage(
            scarlettx::LogLevel::Error, "launcher", error_utf8);
        MessageBoxW(window, error.c_str(), L"ViceWin", MB_ICONWARNING);
        return;
    }
    if (!SaveIniSettings(executable, settings)) {
        scarlettx::LogMessage(
            scarlettx::LogLevel::Error, "launcher",
            "Could not write vicewin.ini next to ViceWin.exe.");
        MessageBoxW(
            window, L"Could not write vicewin.ini next to ViceWin.exe.",
            L"ViceWin", MB_ICONERROR);
        return;
    }
    std::wstring settings_log = L"Launch requested: resolution=" +
        std::to_wstring(settings.width) + L"x" +
        std::to_wstring(settings.height) + L", fps=" +
        std::to_wstring(settings.target_fps) + L", vsync=" +
        (settings.vsync ? L"on" : L"off");
    const std::string settings_log_utf8 = WideToUtf8(settings_log);
    scarlettx::LogMessage(
        scarlettx::LogLevel::Info, "launcher", settings_log_utf8);
    if (!LaunchPackageWithScarlettX(executable, settings, error)) {
        const std::string error_utf8 = WideToUtf8(error);
        scarlettx::LogMessage(
            scarlettx::LogLevel::Error, "launcher", error_utf8);
        MessageBoxW(window, error.c_str(), L"ViceWin", MB_ICONERROR);
        return;
    }
    DestroyWindow(window);
}

LRESULT CALLBACK WindowProcedure(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            CreateControls(window);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case kBrowseButton:
                    BrowseForExecutable(window);
                    return 0;
                case kResolutionCombo:
                    if (HIWORD(wparam) == CBN_SELCHANGE) {
                        UpdateCustomResolutionControls();
                    }
                    return 0;
                case kLaunchButton:
                    Launch(window);
                    return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, RGB(232, 232, 232));
            SetBkColor(dc, RGB(32, 32, 32));
            return reinterpret_cast<LRESULT>(g_dark_brush);
        }
        case WM_DESTROY:
            if (g_dark_brush) {
                DeleteObject(g_dark_brush);
                g_dark_brush = nullptr;
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    g_dark_brush = CreateSolidBrush(RGB(32, 32, 32));
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = g_dark_brush;
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class)) {
        return static_cast<int>(GetLastError());
    }

    HWND window = CreateWindowExW(
        0, kWindowClass, L"ViceWin",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 660, 375,
        nullptr, nullptr, instance, nullptr);
    if (!window) {
        DeleteObject(g_dark_brush);
        g_dark_brush = nullptr;
        return static_cast<int>(GetLastError());
    }
    const BOOL use_dark_mode = TRUE;
    DwmSetWindowAttribute(
        window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
        &use_dark_mode, sizeof(use_dark_mode));
    ShowWindow(window, show_command);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
