#include "process_launcher.h"
#include "core/logger.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

namespace {

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

bool IsViceWinVariable(const std::wstring& name) {
    return CompareStringOrdinal(name.c_str(), -1, L"VICEWIN_WIDTH", -1, TRUE) == CSTR_EQUAL ||
           CompareStringOrdinal(name.c_str(), -1, L"VICEWIN_HEIGHT", -1, TRUE) == CSTR_EQUAL ||
           CompareStringOrdinal(name.c_str(), -1, L"VICEWIN_FPS", -1, TRUE) == CSTR_EQUAL ||
           CompareStringOrdinal(name.c_str(), -1, L"VICEWIN_VSYNC", -1, TRUE) == CSTR_EQUAL ||
           CompareStringOrdinal(name.c_str(), -1, L"VICEWIN_LOG_PATH", -1, TRUE) == CSTR_EQUAL ||
           CompareStringOrdinal(name.c_str(), -1, L"SCARLETTX_DXCOMPILER_PATH", -1, TRUE) == CSTR_EQUAL;
}

bool EnvironmentNameLess(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(
               left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
}

bool BuildEnvironment(
    const ViceWinLaunchSettings& settings,
    const std::wstring& log_path,
    const std::wstring& dxcompiler_path,
    std::vector<wchar_t>& block,
    DWORD& error) {
    std::vector<std::wstring> entries;
    LPWCH environment = GetEnvironmentStringsW();
    if (!environment) {
        error = GetLastError();
        return false;
    }

    for (const wchar_t* item = environment; *item != L'\0';
         item += std::wcslen(item) + 1) {
        std::wstring entry(item);
        size_t separator = entry.find(L'=');
        if (separator == 0) {
            separator = entry.find(L'=', 1);
        }
        if (separator == std::wstring::npos ||
            !IsViceWinVariable(entry.substr(0, separator))) {
            entries.push_back(std::move(entry));
        }
    }
    FreeEnvironmentStringsW(environment);

    entries.push_back(L"VICEWIN_WIDTH=" + std::to_wstring(settings.width));
    entries.push_back(L"VICEWIN_HEIGHT=" + std::to_wstring(settings.height));
    entries.push_back(L"VICEWIN_FPS=" + std::to_wstring(settings.target_fps));
    entries.push_back(std::wstring(L"VICEWIN_VSYNC=") +
                      (settings.vsync ? L"1" : L"0"));
    entries.push_back(L"VICEWIN_LOG_PATH=" + log_path);
    if (!dxcompiler_path.empty()) {
        entries.push_back(L"SCARLETTX_DXCOMPILER_PATH=" + dxcompiler_path);
    }
    std::sort(entries.begin(), entries.end(), EnvironmentNameLess);

    size_t character_count = 1;
    for (const std::wstring& entry : entries) {
        character_count += entry.size() + 1;
    }
    block.reserve(character_count);
    for (const std::wstring& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return true;
}

bool GetLauncherDirectory(std::filesystem::path& directory, DWORD& error) {
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        error = length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        return false;
    }
    directory = std::filesystem::path(executable.data()).parent_path();
    return true;
}

bool FindPackageDirectory(
    const std::filesystem::path& executable_directory,
    std::filesystem::path& package_directory) {
    std::filesystem::path candidate = executable_directory;
    while (!candidate.empty()) {
        std::error_code file_error;
        if (std::filesystem::is_regular_file(
                candidate / L"MicrosoftGame.Config", file_error) &&
            !file_error) {
            package_directory = std::move(candidate);
            return true;
        }
        const std::filesystem::path parent = candidate.parent_path();
        if (parent == candidate) break;
        candidate = parent;
    }
    return false;
}

bool FilesAreEqual(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    bool& equal,
    std::wstring& error) {
    equal = false;
    std::error_code file_error;
    const bool destination_exists = std::filesystem::exists(destination, file_error);
    if (file_error) {
        error = L"Could not inspect a staged runtime file.";
        return false;
    }
    if (!destination_exists) {
        return true;
    }

    std::ifstream source_file(source, std::ios::binary);
    std::ifstream destination_file(destination, std::ios::binary);
    if (!source_file || !destination_file) {
        error = L"Could not read a runtime DLL while checking for updates.";
        return false;
    }

    std::array<char, 64 * 1024> source_bytes{};
    std::array<char, 64 * 1024> destination_bytes{};
    for (;;) {
        source_file.read(
            source_bytes.data(),
            static_cast<std::streamsize>(source_bytes.size()));
        destination_file.read(
            destination_bytes.data(),
            static_cast<std::streamsize>(destination_bytes.size()));
        const std::streamsize source_count = source_file.gcount();
        const std::streamsize destination_count = destination_file.gcount();
        if (source_count != destination_count ||
            (source_count > 0 &&
             std::memcmp(source_bytes.data(), destination_bytes.data(),
                         static_cast<size_t>(source_count)) != 0)) {
            return true;
        }
        if (source_count == 0) {
            if (source_file.bad() || destination_file.bad() ||
                !source_file.eof() || !destination_file.eof()) {
                error = L"Could not finish comparing staged runtime files.";
                return false;
            }
            equal = true;
            return true;
        }
    }
}

bool SyncRuntimeFiles(
    const std::filesystem::path& launcher_directory,
    const std::filesystem::path& runtime_directory,
    std::wstring& error,
    std::filesystem::path& out_dxcompiler_path) {
    static constexpr std::array<std::wstring_view, 19> required_files = {
        L"scarlettx.exe",
        L"d3d12_xs.dll",
        L"vicewin_core.dll",
        L"xgameruntime.dll",
        L"xg_xs.dll",
        L"AcpHal.dll",
        L"XboxBvhBuilder.dll",
        L"dstorage.dll",
        L"dstorage_xs.dll",
        L"xcrdapi.dll",
        L"xaudio2_9.dll",
        L"GameInput.dll",
        L"Bink2Scarlett.dll",
        L"amd_fidelityfx_upscaler_xbox.dll",
        L"amd_fidelityfx_loader_xbox.dll",
        L"libHttpClient.GDK.dll",
        L"XCurl.dll",
        L"libz.dll",
        L"chad.dll",
    };
    // The DXC pair is optional: the compiler cascade also searches the game
    // directory, the host directory, System32 and every Windows SDK
    // bin\<version>\x64, so a release without a staged toolchain still
    // compiles shaders. Blocking the launch over it was a regression.
    static constexpr std::array<std::wstring_view, 2> optional_files = {
        L"dxcompiler.dll",
        L"dxil.dll",
    };
    // The VC++ runtime is not part of our build output: when the redistributable
    // is installed it lives in System32 and the guest resolves it through the
    // normal search path. Refusing to launch over it (as this list used to do)
    // blocked machines where everything was already in place. Stage a copy into
    // the private runtime directory when one is reachable, otherwise log it.
    static constexpr std::array<std::wstring_view, 3> system_runtime_files = {
        L"MSVCP140.dll",
        L"VCRUNTIME140.dll",
        L"VCRUNTIME140_1.dll",
    };
    std::error_code file_error;
    std::filesystem::create_directories(runtime_directory, file_error);
    if (file_error) {
        error = L"Could not create ViceWin's private runtime directory.";
        return false;
    }

    for (const std::wstring_view filename : required_files) {
        const std::filesystem::path source = launcher_directory / filename;
        if (!std::filesystem::is_regular_file(source, file_error) || file_error) {
            error = L"Required runtime file is missing: " + std::wstring(filename);
            return false;
        }
        bool equal = false;
        if (!FilesAreEqual(source, runtime_directory / filename, equal, error)) {
            return false;
        }
        if (equal) {
            continue;
        }
        file_error.clear();
        std::filesystem::copy_file(
            source, runtime_directory / filename,
            std::filesystem::copy_options::overwrite_existing, file_error);
        if (file_error) {
            error = L"Could not synchronize runtime file: " + std::wstring(filename);
            return false;
        }
        const std::string copied = WideToUtf8(filename);
        scarlettx::LogMessage(
            scarlettx::LogLevel::Info, "launcher",
            "Synchronized runtime file " + copied);
    }

    for (const std::wstring_view filename : optional_files) {
        const std::filesystem::path source = launcher_directory / filename;
        if (!std::filesystem::is_regular_file(source, file_error) || file_error) {
            scarlettx::LogMessage(
                scarlettx::LogLevel::Warning, "launcher",
                "Optional runtime file not staged: " + WideToUtf8(filename) +
                    " (the DXC cascade will locate the Windows SDK itself)");
            continue;
        }
        bool equal = false;
        if (!FilesAreEqual(source, runtime_directory / filename, equal, error)) {
            return false;
        }
        if (equal) {
            continue;
        }
        file_error.clear();
        std::filesystem::copy_file(
            source, runtime_directory / filename,
            std::filesystem::copy_options::overwrite_existing, file_error);
        if (file_error) {
            error = L"Could not synchronize runtime file: " + std::wstring(filename);
            return false;
        }
        const std::string copied = WideToUtf8(filename);
        scarlettx::LogMessage(
            scarlettx::LogLevel::Info, "launcher",
            "Synchronized optional runtime file " + copied);
    }

    wchar_t system_directory[MAX_PATH] = {};
    const DWORD system_length =
        GetSystemDirectoryW(system_directory, MAX_PATH);
    const std::filesystem::path system_directory_path =
        (system_length > 0 && system_length < MAX_PATH)
            ? std::filesystem::path(std::wstring(system_directory, system_length))
            : std::filesystem::path();
    for (const std::wstring_view filename : system_runtime_files) {
        const std::filesystem::path local = launcher_directory / filename;
        const std::filesystem::path target = runtime_directory / filename;
        std::filesystem::path source;
        if (std::filesystem::is_regular_file(local, file_error) && !file_error) {
            source = local;
        } else {
            file_error.clear();
            if (!system_directory_path.empty()) {
                const std::filesystem::path from_system = system_directory_path / filename;
                if (std::filesystem::is_regular_file(from_system, file_error) &&
                    !file_error) {
                    source = from_system;
                }
            }
        }
        if (source.empty()) {
            scarlettx::LogMessage(
                scarlettx::LogLevel::Warning, "launcher",
                "VC runtime file not found next to ViceWin.exe or in System32: " +
                    WideToUtf8(filename) +
                    "; the guest will resolve it through the system search path");
            continue;
        }
        bool equal = false;
        if (!FilesAreEqual(source, target, equal, error)) {
            return false;
        }
        if (equal) {
            continue;
        }
        file_error.clear();
        std::filesystem::copy_file(
            source, target, std::filesystem::copy_options::overwrite_existing,
            file_error);
        if (file_error) {
            error = L"Could not stage VC runtime file: " + std::wstring(filename);
            return false;
        }
        scarlettx::LogMessage(
            scarlettx::LogLevel::Info, "launcher",
            "Staged VC runtime file " + WideToUtf8(filename) + " from " +
                WideToUtf8(source.wstring()));
    }

    const std::filesystem::path staged_dxc = runtime_directory / L"dxcompiler.dll";
    if (std::filesystem::is_regular_file(staged_dxc, file_error) && !file_error) {
        out_dxcompiler_path = staged_dxc;
    } else {
        scarlettx::LogMessage(
            scarlettx::LogLevel::Warning, "launcher",
            "No dxcompiler.dll staged; shader compilation relies on the Windows "
            "SDK found by the DXC cascade");
    }
    return true;
}

std::wstring QuoteWindowsArgument(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(character);
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring WindowsErrorMessage(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);
    std::wstring message = length && buffer ? buffer : L"Unknown Windows error";
    if (buffer) {
        LocalFree(buffer);
    }
    return message;
}

}  // namespace

bool LaunchPackageWithScarlettX(
    const std::wstring& executable_path,
    const ViceWinLaunchSettings& settings,
    std::wstring& error_message) {
    error_message.clear();
    std::error_code file_error;
    const std::filesystem::path executable(executable_path);
    if (!std::filesystem::is_regular_file(executable, file_error) || file_error ||
        CompareStringOrdinal(
            executable.extension().c_str(), -1, L".exe", -1, TRUE) != CSTR_EQUAL) {
        error_message = L"Select an existing executable from a ScarlettX package.";
        return false;
    }

    std::filesystem::path package_directory;
    if (!FindPackageDirectory(executable.parent_path(), package_directory)) {
        error_message = L"The selected executable is not in a package containing MicrosoftGame.Config.";
        return false;
    }

    std::filesystem::path launcher_directory;
    DWORD launcher_error = ERROR_SUCCESS;
    if (!GetLauncherDirectory(launcher_directory, launcher_error)) {
        error_message = L"Could not locate ViceWin.exe: " +
                        WindowsErrorMessage(launcher_error);
        return false;
    }
    const std::filesystem::path runtime_directory =
        launcher_directory / L"runtime";
    std::filesystem::path staged_dxcompiler;
    if (!SyncRuntimeFiles(launcher_directory, runtime_directory, error_message,
                          staged_dxcompiler)) {
        return false;
    }

    const std::filesystem::path host = runtime_directory / L"scarlettx.exe";
    const std::wstring package_path = package_directory.wstring();
    std::wstring command_line =
        QuoteWindowsArgument(host.wstring()) + L" boot " +
        QuoteWindowsArgument(package_path) +
        // A retail console dump imports console-only modules (xmem.dll,
        // chad.dll, kernelx.dll, ...). A child process cannot redirect those,
        // so the Windows loader would stop with STATUS_DLL_NOT_FOUND; the
        // in-process mapper rewrites the IAT through the redirection table and
        // the HLE registry instead.
        L" --in-process --gpu --fcap " +
        std::to_wstring(settings.target_fps);
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    const std::wstring log_path =
        (launcher_directory / L"vicewin.log").wstring();
    std::vector<wchar_t> environment;
    DWORD environment_error = ERROR_SUCCESS;
    const std::wstring dxcompiler_path = staged_dxcompiler.wstring();
    if (!BuildEnvironment(
            settings, log_path, dxcompiler_path,
            environment, environment_error)) {
        error_message = L"Could not read the current environment: " +
                        WindowsErrorMessage(environment_error);
        return false;
    }
    const std::wstring working_directory = package_path;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            host.c_str(),
            mutable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_UNICODE_ENVIRONMENT,
            environment.data(),
            working_directory.empty() ? nullptr : working_directory.c_str(),
            &startup,
            &process)) {
        error_message = L"CreateProcessW failed: " +
                        WindowsErrorMessage(GetLastError());
        return false;
    }

    scarlettx::LogMessage(
        scarlettx::LogLevel::Info, "launcher",
        "Started ScarlettX host for the selected package.");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
