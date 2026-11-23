#include "scarlettx/gpu/dxil_backend.h"
#include "scarlettx/gpu/shader_ir.h"
#include "core/logger.h"

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32) && SCARLETTX_HAS_DXC
#include <windows.h>
#include <unknwn.h>
#include <objidl.h>
#include <oaidl.h>
#include <dxcapi.h>
#endif

namespace scarlettx {

namespace {

Status ValidateContainerStructure(const std::vector<uint8_t>& container) {
    ShaderModuleInfo info{};
    Status status = InspectShaderContainer(container, info);
    if (!status.ok()) {
        return status;
    }
    if (info.kind != ShaderContainerKind::Dxil) {
        return Status::Error(StatusCode::ShaderError, "Container does not contain DXIL bitcode");
    }
    if (info.has_missing_dxil_bitcode) {
        return Status::Error(StatusCode::ShaderError, "DXIL container does not contain shader bitcode");
    }
    return Status::Ok();
}

}  // namespace

#if defined(_WIN32) && SCARLETTX_HAS_DXC
namespace {

template <typename T>
class DxcComPtr {
public:
    DxcComPtr() = default;
    DxcComPtr(const DxcComPtr&) = delete;
    DxcComPtr& operator=(const DxcComPtr&) = delete;

    ~DxcComPtr() {
        Reset();
    }

    T* Get() const noexcept {
        return value_;
    }

    T** Put() noexcept {
        Reset();
        return &value_;
    }

    T* operator->() const noexcept {
        return value_;
    }

private:
    void Reset() noexcept {
        if (value_) {
            value_->Release();
            value_ = nullptr;
        }
    }

    T* value_ = nullptr;
};

// Directory part of `path`, keeping the trailing separator.
std::wstring DirectoryOf(const std::wstring& path) {
    const size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : path.substr(0, cut + 1);
}

std::wstring ReadEnvironment(const wchar_t* name) {
    wchar_t buffer[32768] = {};
    const DWORD length = GetEnvironmentVariableW(
        name, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) {
        return std::wstring();
    }
    return std::wstring(buffer, length);
}

std::string NarrowAscii(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (const wchar_t ch : text) {
        out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    return out;
}

// Every Windows SDK `bin\<version>\x64` directory that actually holds a
// compiler, newest version first. That is where a stock Windows 11 machine
// keeps dxcompiler.dll and dxil.dll; they are not OS components and are never
// in System32. The SDK location is discovered through every channel
// available: %WindowsSdkDir% (developer prompt), any Program Files flavour
// (%ProgramFiles(x86)% is frequently absent in stripped environments), the
// system drive, and the v10.0 registry key as the authoritative fallback.
std::wstring RegistryWindowsKitsBinRoot() {
    for (const wchar_t* subkey :
         {L"SOFTWARE\\WOW6432Node\\Microsoft\\Microsoft SDKs\\Windows\\v10.0",
          L"SOFTWARE\\Microsoft\\Microsoft SDKs\\Windows\\v10.0"}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &key) !=
            ERROR_SUCCESS) {
            continue;
        }
        wchar_t buffer[MAX_PATH] = {};
        DWORD size = sizeof(buffer);
        const LSTATUS status =
            RegGetValueW(key, nullptr, L"InstallationFolder", RRF_RT_REG_SZ,
                         nullptr, buffer, &size);
        RegCloseKey(key);
        if (status == ERROR_SUCCESS && buffer[0] != L'\0') {
            std::wstring root(buffer);
            if (!root.empty() && root.back() != L'\\') {
                root += L'\\';
            }
            return root + L"bin\\";
        }
    }
    return {};
}

std::vector<std::wstring> WindowsSdkBinDirectories() {
    std::vector<std::wstring> roots;
    if (std::wstring sdk_dir = ReadEnvironment(L"WindowsSdkDir"); !sdk_dir.empty()) {
        if (!sdk_dir.empty() && sdk_dir.back() != L'\\') {
            sdk_dir += L'\\';
        }
        roots.push_back(sdk_dir + L"bin\\");
    }
    for (const wchar_t* name : {L"ProgramFiles(x86)", L"ProgramW6432", L"ProgramFiles"}) {
        if (std::wstring program_files = ReadEnvironment(name); !program_files.empty()) {
            roots.push_back(program_files + L"\\Windows Kits\\10\\bin\\");
        }
    }
    // Environments without any Program Files variable still have a system
    // drive; the 32-bit Program Files is where the SDK lives on x64 Windows.
    wchar_t system_dir[MAX_PATH] = {};
    if (GetSystemDirectoryW(system_dir, MAX_PATH) > 1 && system_dir[1] == L':') {
        const std::wstring drive(system_dir, 2);
        roots.push_back(drive + L"\\Program Files (x86)\\Windows Kits\\10\\bin\\");
        roots.push_back(drive + L"\\Program Files\\Windows Kits\\10\\bin\\");
    }
    if (std::wstring registry_root = RegistryWindowsKitsBinRoot(); !registry_root.empty()) {
        roots.push_back(registry_root);
    }

    std::vector<std::wstring> out;
    std::error_code ec;
    for (const auto& root : roots) {
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (!entry.is_directory(ec)) {
                continue;
            }
            const std::wstring candidate = entry.path().wstring() + L"\\x64\\";
            if (std::filesystem::exists(candidate + L"dxcompiler.dll", ec)) {
                if (std::find(out.begin(), out.end(), candidate) == out.end()) {
                    out.push_back(candidate);
                }
            }
        }
    }
    std::sort(out.begin(), out.end(), std::greater<>());
    return out;
}

class DxcRuntime {
public:
    DxcRuntime() {
        std::vector<std::wstring> candidates;

        // 1. An explicit override always wins.
        if (std::wstring configured = ReadEnvironment(L"SCARLETTX_DXCOMPILER_PATH");
            !configured.empty()) {
            candidates.push_back(configured);
        }
        // 2. The title's own directory. A retail dump ships a console
        //    dxcompiler.dll there that Win32 rejects with ERROR_BAD_EXE_FORMAT,
        //    so this step is expected to fail and fall through.
        if (std::wstring package = ReadEnvironment(L"SCARLETTX_PACKAGE_ROOT");
            !package.empty()) {
            candidates.push_back(package + L"\\dxcompiler.dll");
        }
        // 3. The launcher's directory (vicewin.exe / scarlettx.exe).
        wchar_t self[32768] = {};
        const DWORD self_length =
            GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
        if (self_length > 0 && self_length < std::size(self)) {
            candidates.push_back(DirectoryOf(std::wstring(self, self_length)) + L"dxcompiler.dll");
        }
        // 4. A Vulkan SDK install.
        if (std::wstring vulkan_sdk = ReadEnvironment(L"VULKAN_SDK"); !vulkan_sdk.empty()) {
            candidates.push_back(vulkan_sdk + L"\\Bin\\dxcompiler.dll");
        }
        // 5. System32.
        wchar_t system_dir[MAX_PATH] = {};
        if (GetSystemDirectoryW(system_dir, MAX_PATH) > 0) {
            candidates.push_back(std::wstring(system_dir) + L"\\dxcompiler.dll");
        }
        // 6. The Windows SDK, newest version first.
        for (const auto& directory : WindowsSdkBinDirectories()) {
            candidates.push_back(directory + L"dxcompiler.dll");
        }

        for (const auto& candidate : candidates) {
            if (TryLoad(candidate)) {
                break;
            }
        }

        if (module_) {
            create_instance_ = reinterpret_cast<DxcCreateInstanceProc>(
                GetProcAddress(module_, "DxcCreateInstance"));
            if (!create_instance_) {
                LogMessage(LogLevel::Error, "shader",
                           "[dxc] " + NarrowAscii(loaded_path_) +
                               " has no DxcCreateInstance export; trying the next candidate");
                FreeLibrary(module_);
                module_ = nullptr;
                loaded_path_.clear();
            }
        }
    }

    ~DxcRuntime() {
        if (dxil_module_) {
            FreeLibrary(dxil_module_);
        }
        if (module_) {
            FreeLibrary(module_);
        }
    }

    const std::wstring& loaded_path() const noexcept {
        return loaded_path_;
    }

    bool Available() const noexcept {
        return module_ != nullptr && create_instance_ != nullptr;
    }

    HRESULT CreateInstance(REFCLSID class_id, REFIID interface_id, void** out_object) const {
        if (!Available()) {
            return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        }
        return create_instance_(class_id, interface_id, out_object);
    }

private:
    bool TryLoad(const std::wstring& path) {
        if (path.empty()) {
            return false;
        }
        // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR needs a fully qualified name; a bare
        // file name has to go through the default search instead.
        const bool qualified =
            path.find(L':') != std::wstring::npos || path.rfind(L"\\\\", 0) == 0;
        HMODULE module = LoadLibraryExW(
            path.c_str(),
            nullptr,
            qualified ? (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) : 0);
        if (!module) {
            // ERROR_BAD_EXE_FORMAT (193) is the console-build case. Recording
            // the real code is the difference between "rejected" and "absent".
            const DWORD error = GetLastError();
            LogMessage(LogLevel::Warning, "shader",
                       "[dxc] skipped " + NarrowAscii(path) + " (Win32 error " +
                           std::to_string(error) + ")");
            return false;
        }

        module_ = module;
        loaded_path_ = path;

        // dxil.dll is loaded by the compiler from its own directory, so pull it
        // in from the same place the compiler came from; the search path cannot
        // be trusted to hold a matching build.
        const std::wstring dxil_path = DirectoryOf(path) + L"dxil.dll";
        if (!dxil_path.empty()) {
            dxil_module_ = LoadLibraryExW(
                dxil_path.c_str(),
                nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!dxil_module_) {
                LogMessage(LogLevel::Warning, "shader",
                           "[dxc] dxil.dll not loaded beside " + NarrowAscii(path) +
                               " (Win32 error " + std::to_string(GetLastError()) +
                               "); DXIL validation will be unavailable");
                dxil_module_ = nullptr;
            }
        }

        LogMessage(LogLevel::Info, "shader", "[dxc] using " + NarrowAscii(path));
        return true;
    }

    HMODULE module_ = nullptr;
    HMODULE dxil_module_ = nullptr;
    DxcCreateInstanceProc create_instance_ = nullptr;
    std::wstring loaded_path_;
};

std::string HResultMessage(const char* operation, HRESULT result) {
    std::ostringstream message;
    message << operation << " failed with HRESULT 0x" << std::hex
            << static_cast<uint32_t>(result);
    return message.str();
}

std::string OperationDiagnostics(IDxcOperationResult* operation) {
    if (!operation) {
        return {};
    }

    DxcComPtr<IDxcBlobEncoding> errors;
    if (FAILED(operation->GetErrorBuffer(errors.Put())) || !errors.Get() ||
        !errors->GetBufferPointer() || errors->GetBufferSize() == 0) {
        return {};
    }
    return std::string(
        static_cast<const char*>(errors->GetBufferPointer()),
        errors->GetBufferSize());
}

Status ValidateDxilContainer(
    const DxcRuntime& runtime,
    const std::vector<uint8_t>& container) {
    Status structure_status = ValidateContainerStructure(container);
    if (!structure_status.ok()) {
        return structure_status;
    }
    if (container.size() > std::numeric_limits<UINT32>::max()) {
        return Status::Error(StatusCode::ShaderError, "DXIL container exceeds the validator size limit");
    }

    DxcComPtr<IDxcUtils> utils;
    HRESULT hr = runtime.CreateInstance(
        CLSID_DxcUtils, __uuidof(IDxcUtils), reinterpret_cast<void**>(utils.Put()));
    if (FAILED(hr) || !utils.Get()) {
        return Status::Error(StatusCode::NotFound, HResultMessage("Creating DXC utilities", hr));
    }

    DxcComPtr<IDxcBlobEncoding> shader_blob;
    hr = utils->CreateBlobFromPinned(
        container.data(),
        static_cast<UINT32>(container.size()),
        DXC_CP_ACP,
        shader_blob.Put());
    if (FAILED(hr) || !shader_blob.Get()) {
        return Status::Error(StatusCode::ShaderError, HResultMessage("Creating DXIL blob", hr));
    }

    DxcComPtr<IDxcValidator> validator;
    hr = runtime.CreateInstance(
        CLSID_DxcValidator, __uuidof(IDxcValidator), reinterpret_cast<void**>(validator.Put()));
    if (FAILED(hr) || !validator.Get()) {
        return Status::Error(StatusCode::NotFound, HResultMessage("Creating DXIL validator", hr));
    }

    DxcComPtr<IDxcOperationResult> validation;
    hr = validator->Validate(shader_blob.Get(), DxcValidatorFlags_Default, validation.Put());
    if (FAILED(hr) || !validation.Get()) {
        return Status::Error(StatusCode::ShaderError, HResultMessage("Running DXIL validator", hr));
    }

    HRESULT validation_status = E_FAIL;
    hr = validation->GetStatus(&validation_status);
    if (FAILED(hr)) {
        return Status::Error(StatusCode::ShaderError, HResultMessage("Reading DXIL validation status", hr));
    }
    if (FAILED(validation_status)) {
        std::string message = OperationDiagnostics(validation.Get());
        if (message.empty()) {
            message = HResultMessage("DXIL validation", validation_status);
        }
        return Status::Error(StatusCode::ShaderError, std::move(message));
    }
    return Status::Ok();
}

}  // namespace
#endif

// Stable identity of the live DXC toolchain (compiler version + fixed flag
// set), used by the shader cache key so stale DXIL from a different
// dxcompiler.dll is never reused. See dxil_backend.h.
uint64_t DxcCompileIdentity() {
#if defined(_WIN32) && SCARLETTX_HAS_DXC
    static const uint64_t identity = [] {
        uint32_t version_major = 0;
        uint32_t version_minor = 0;
        DxcRuntime runtime;
        if (runtime.Available()) {
            DxcComPtr<IDxcCompiler3> compiler;
            HRESULT hr = runtime.CreateInstance(
                CLSID_DxcCompiler, __uuidof(IDxcCompiler3),
                reinterpret_cast<void**>(compiler.Put()));
            if (SUCCEEDED(hr) && compiler.Get()) {
                DxcComPtr<IDxcVersionInfo> version;
                if (SUCCEEDED(compiler->QueryInterface(
                        __uuidof(IDxcVersionInfo),
                        reinterpret_cast<void**>(version.Put()))) &&
                    version.Get() &&
                    SUCCEEDED(version->GetVersion(&version_major, &version_minor))) {
                    LogMessage(LogLevel::Info, "shader",
                               std::string("[dxc] compiler version ") +
                                   std::to_string(version_major) + "." +
                                   std::to_string(version_minor) + " from " +
                                   NarrowAscii(runtime.loaded_path()));
                }
            }
        }
        if (version_major == 0 && version_minor == 0) {
            LogMessage(LogLevel::Warning, "shader",
                       "[dxc] IDxcVersionInfo unavailable; shader cache salted "
                       "without a compiler version");
        }

        // FNV-1a over "<major>.<minor> " followed by the fixed argument list
        // CompileHlslToDxil passes to IDxcCompiler3::Compile. The per-call
        // entry point and target profile are deliberately excluded: both are
        // already covered by the shader cache key (stage, lifter version).
        // Keep in sync with the argument vector in CompileHlslToDxil.
        constexpr const wchar_t* kFixedArguments[] = {
            L"-HV 2021 ", L"-O3 ", L"-Ges ", L"-enable-16bit-types",
        };
        const std::string version_text =
            std::to_string(version_major) + "." + std::to_string(version_minor) + " ";
        auto mix = [](uint64_t hash, const void* data, size_t size) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; ++i) {
                hash ^= bytes[i];
                hash *= 1099511628211ull;
            }
            return hash;
        };
        uint64_t hash = 14695981039346656037ull;
        hash = mix(hash, version_text.data(), version_text.size());
        for (const wchar_t* argument : kFixedArguments) {
            hash = mix(hash, argument, std::wcslen(argument) * sizeof(wchar_t));
        }
        return hash;
    }();
    return identity;
#else
    return 0;
#endif
}

Status CompileHlslToDxil(
    const std::string& hlsl_source,
    const std::string& entry_point,
    const std::string& target_profile,
    std::vector<uint8_t>& out_container,
    bool enable_16bit_types) {
    out_container.clear();
    uint64_t source_hash = 14695981039346656037ull;
    for (unsigned char byte : hlsl_source) {
        source_hash ^= byte;
        source_hash *= 1099511628211ull;
    }
    std::ostringstream compile_identity;
    compile_identity << "target=" << target_profile << ", hlsl_hash=0x"
                     << std::hex << source_hash;
    auto finish = [&](Status status) {
        std::string message = compile_identity.str();
        if (status.ok()) {
            message += ", dxil_bytes=" + std::to_string(out_container.size());
            LogMessage(LogLevel::Info, "shader", message);
        } else {
            message += ", error=" + status.message;
            LogMessage(LogLevel::Error, "shader", message);
        }
        return status;
    };
    if (hlsl_source.empty() || entry_point.empty() || target_profile.empty()) {
        return finish(Status::Error(StatusCode::InvalidArgument, "HLSL source, entry point, and target are required"));
    }
    if (hlsl_source.size() > std::numeric_limits<uint32_t>::max()) {
        return finish(Status::Error(StatusCode::InvalidArgument, "HLSL source exceeds the DXC input size limit"));
    }

#if defined(_WIN32) && SCARLETTX_HAS_DXC
    DxcRuntime runtime;
    if (!runtime.Available()) {
        return finish(Status::Error(
            StatusCode::NotFound,
            "DXC runtime not found; tried SCARLETTX_DXCOMPILER_PATH, the game "
            "package directory, the vicewin.exe/ScarlettX directory, the Vulkan "
            "SDK, System32, and every installed Windows SDK bin\\<version>\\x64; "
            "install the Windows 11 SDK or set SCARLETTX_DXCOMPILER_PATH (a "
            "console dxcompiler.dll is rejected with Win32 error 193, "
            "ERROR_BAD_EXE_FORMAT)"));
    }

    DxcComPtr<IDxcUtils> utils;
    HRESULT hr = runtime.CreateInstance(
        CLSID_DxcUtils, __uuidof(IDxcUtils), reinterpret_cast<void**>(utils.Put()));
    if (FAILED(hr) || !utils.Get()) {
        return finish(Status::Error(StatusCode::NotFound, HResultMessage("Creating DXC utilities", hr)));
    }

    DxcComPtr<IDxcCompiler3> compiler;
    hr = runtime.CreateInstance(
        CLSID_DxcCompiler, __uuidof(IDxcCompiler3), reinterpret_cast<void**>(compiler.Put()));
    if (FAILED(hr) || !compiler.Get()) {
        return finish(Status::Error(StatusCode::NotFound, HResultMessage("Creating DXC compiler", hr)));
    }

    DxcComPtr<IDxcIncludeHandler> include_handler;
    hr = utils->CreateDefaultIncludeHandler(include_handler.Put());
    if (FAILED(hr) || !include_handler.Get()) {
        return finish(Status::Error(StatusCode::ShaderError, HResultMessage("Creating DXC include handler", hr)));
    }

    // The argument vector below is part of the DXC toolchain identity; if it
    // changes, DxcCompileIdentity() and the shader cache key must follow.
    const std::wstring entry_wide(entry_point.begin(), entry_point.end());
    const std::wstring target_wide(target_profile.begin(), target_profile.end());
    std::vector<LPCWSTR> arguments = {
        L"-E", entry_wide.c_str(),
        L"-T", target_wide.c_str(),
        L"-HV", L"2021",
        L"-O3",
        L"-Ges",
    };
    if (enable_16bit_types) {
        arguments.push_back(L"-enable-16bit-types");
    }
    DxcBuffer source{};
    source.Ptr = hlsl_source.data();
    source.Size = hlsl_source.size();
    source.Encoding = DXC_CP_UTF8;

    DxcComPtr<IDxcResult> compile_result;
    hr = compiler->Compile(
        &source,
        arguments.data(),
        static_cast<UINT32>(arguments.size()),
        include_handler.Get(),
        __uuidof(IDxcResult),
        reinterpret_cast<void**>(compile_result.Put()));
    if (FAILED(hr) || !compile_result.Get()) {
        return finish(Status::Error(StatusCode::ShaderError, HResultMessage("Compiling HLSL to DXIL", hr)));
    }

    HRESULT compile_status = E_FAIL;
    hr = compile_result->GetStatus(&compile_status);
    if (FAILED(hr)) {
        return finish(Status::Error(StatusCode::ShaderError, HResultMessage("Reading DXC compile status", hr)));
    }
    if (FAILED(compile_status)) {
        DxcComPtr<IDxcBlobUtf8> errors;
        std::string message;
        if (SUCCEEDED(compile_result->GetOutput(
                          DXC_OUT_ERRORS,
                          __uuidof(IDxcBlobUtf8),
                          reinterpret_cast<void**>(errors.Put()),
                          nullptr)) &&
            errors.Get() && errors->GetStringPointer()) {
            message.assign(errors->GetStringPointer(), errors->GetStringLength());
        }
        if (message.empty()) {
            message = HResultMessage("HLSL compilation", compile_status);
        }
        return finish(Status::Error(StatusCode::ShaderError, std::move(message)));
    }

    DxcComPtr<IDxcBlob> object;
    hr = compile_result->GetOutput(
        DXC_OUT_OBJECT, __uuidof(IDxcBlob), reinterpret_cast<void**>(object.Put()), nullptr);
    if (FAILED(hr) || !object.Get() || !object->GetBufferPointer() || object->GetBufferSize() == 0) {
        return finish(Status::Error(StatusCode::ShaderError, HResultMessage("Retrieving DXIL output", hr)));
    }

    const auto* begin = static_cast<const uint8_t*>(object->GetBufferPointer());
    out_container.assign(begin, begin + object->GetBufferSize());
    Status validation_status = ValidateDxilContainer(runtime, out_container);
    if (!validation_status.ok()) {
        out_container.clear();
        return finish(validation_status);
    }
    return finish(Status::Ok());
#else
    (void)hlsl_source;
    (void)entry_point;
    (void)target_profile;
    return finish(Status::Error(StatusCode::NotImplemented, "DXC support was not found at configure time"));
#endif
}

Status ValidateDxilContainer(const std::vector<uint8_t>& container) {
    Status structure_status = ValidateContainerStructure(container);
    if (!structure_status.ok()) {
        return structure_status;
    }
#if defined(_WIN32) && SCARLETTX_HAS_DXC
    DxcRuntime runtime;
    if (!runtime.Available()) {
        return Status::Error(
            StatusCode::NotFound,
            "DXC runtime not found; tried SCARLETTX_DXCOMPILER_PATH, the game "
            "package directory, the vicewin.exe/ScarlettX directory, the Vulkan "
            "SDK, System32, and every installed Windows SDK bin\\<version>\\x64");
    }
    return ValidateDxilContainer(runtime, container);
#else
    (void)container;
    return Status::Error(StatusCode::NotImplemented, "DXC support was not found at configure time");
#endif
}

}  // namespace scarlettx
