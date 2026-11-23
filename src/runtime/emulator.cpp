#include "scarlettx/runtime/emulator.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "scarlettx/kernel/kernelx_hle.h"
#include "scarlettx/loader/pe_mapper.h"
#include "scarlettx/core/veh_interceptor.h"
#include "scarlettx/runtime/virtual_drive.h"
#include "scarlettx/core/hle_trace.h"
#include "core/logger.h"

#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace scarlettx {

namespace {

// The guest image the last-chance filter scans the stack against.
std::atomic<uint64_t> g_guest_base{0};
std::atomic<uint64_t> g_guest_end{0};

// Runs when nothing in the guest handled the exception, i.e. exactly when the
// host is about to die with a bare exit code. Writes the exception record and
// a crude stack scan (return addresses that point into the guest image) so the
// failure is diagnosable without a debugger or a WER dump.
LONG WINAPI GuestFatalExceptionFilter(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    std::ostringstream text;
    text << "guest fatal exception 0x" << std::hex << record.ExceptionCode;
    if (info->ContextRecord) {
        text << " at rip=0x" << info->ContextRecord->Rip;
    }
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        record.NumberParameters > 1) {
        text << " (" << (record.ExceptionInformation[0] == 0 ? "read"
                         : record.ExceptionInformation[0] == 1 ? "write"
                                                               : "execute")
             << " 0x" << record.ExceptionInformation[1] << ")";
    }
    LogMessage(LogLevel::Error, "loader", text.str());
    HleTraceFaultDump("the fatal exception");

    if (info->ContextRecord) {
        const CONTEXT& c = *info->ContextRecord;
        std::ostringstream regs;
        regs << std::hex << "rax=0x" << c.Rax << " rbx=0x" << c.Rbx
             << " rcx=0x" << c.Rcx << " rdx=0x" << c.Rdx
             << " rsi=0x" << c.Rsi << " rdi=0x" << c.Rdi
             << " rbp=0x" << c.Rbp << " rsp=0x" << c.Rsp
             << " r8=0x" << c.R8 << " r9=0x" << c.R9
             << " r10=0x" << c.R10 << " r11=0x" << c.R11
             << " r12=0x" << c.R12 << " r13=0x" << c.R13
             << " r14=0x" << c.R14 << " r15=0x" << c.R15;
        LogMessage(LogLevel::Error, "loader", "guest registers: " + regs.str());
    }

    if (info->ContextRecord) {
        const uint64_t base = g_guest_base.load();
        const uint64_t end = g_guest_end.load();
        const auto* stack = reinterpret_cast<const uint64_t*>(info->ContextRecord->Rsp);
        std::ostringstream frames;
        int found = 0;
        for (size_t i = 0; i < 256 && found < 12; ++i) {
            uint64_t value = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), stack + i, &value,
                                   sizeof(value), nullptr)) {
                break;
            }
            if (value >= base && value < end) {
                frames << " [" << found << "]=0x" << std::hex << value;
                ++found;
            }
        }
        LogMessage(found > 0 ? LogLevel::Error : LogLevel::Warning, "loader",
                   "guest return addresses on the stack:" +
                       (found > 0 ? frames.str()
                                  : std::string(" none (fault outside the image)")));
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

std::string FindGameConfigInDir(const std::string& package_dir) {
    const std::filesystem::path dir(package_dir);
    for (const char* candidate : {"MicrosoftGame.Config", "MicrosoftGame.config"}) {
        const auto p = dir / candidate;
        if (std::filesystem::exists(p)) {
            return p.string();
        }
    }
    return {};
}

}  // namespace

Emulator::Emulator(EmulatorConfig config) : config_(std::move(config)) {}

Status Emulator::InspectPackage(const std::string& package_dir,
                                PackageInspectionReport& out_report) const {
    out_report = PackageInspectionReport{};
    const std::string cfg_path = FindGameConfigInDir(package_dir);
    if (cfg_path.empty()) {
        return Status::Error(StatusCode::NotFound,
                             "MicrosoftGame.Config not found in package directory: " + package_dir);
    }

    Status st = LoadGameConfigFile(cfg_path, out_report.config);
    if (!st.ok()) {
        return st;
    }

    const GameExecutableEntry* exe = out_report.config.PrimaryExecutable();
    if (!exe) {
        return Status::Error(StatusCode::ParseError, "No primary executable in MicrosoftGame.Config");
    }

    const std::filesystem::path exe_path = std::filesystem::path(package_dir) / exe->name;
    if (!std::filesystem::exists(exe_path)) {
        return Status::Error(StatusCode::NotFound, "Package executable missing: " + exe_path.string());
    }
    out_report.executable_path = exe_path.string();

    return InspectPeFile(out_report.executable_path, redirects_, out_report.main_pe);
}

Status Emulator::BootPackage(const std::string& package_dir) {
#if defined(_WIN32)
    // Titles with hundreds of open .rpf/asset archives need a CRT stdio
    // ceiling far above the default; this also covers in-process boots
    // launched from tests and embedders that skip main().
    _setmaxstdio(8192);
#endif
    PackageInspectionReport report;
    Status st = InspectPackage(package_dir, report);
    if (!st.ok()) {
        return st;
    }

    if (!report.config.IsNativeScarlett()) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Package is not a native XboxGen9/Scarlett Game Core target");
    }

#if defined(_WIN32)
    // Inject HLE shim directory at the front of PATH so guest imports of
    // d3d12_xs.dll, xg_xs.dll, AcpHal.dll, xgameruntime.dll, etc. resolve to ScarlettX shims.
    std::string shim_dir = config_.shim_directory;
    if (shim_dir.empty()) {
        char exe_buf[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exe_buf, MAX_PATH) > 0) {
            shim_dir = std::filesystem::path(exe_buf).parent_path().string();
        }
    }

    auto set_env = [](const char* key, const char* value) {
        SetEnvironmentVariableA(key, value);
        _putenv_s(key, value);
    };

    if (!shim_dir.empty()) {
        std::string new_path = shim_dir;
        const char* cur_path = std::getenv("PATH");
        if (cur_path && *cur_path) {
            new_path += ";";
            new_path += cur_path;
        }
        set_env("PATH", new_path.c_str());
    }

    set_env("SCARLETTX_HEADLESS", config_.headless ? "1" : "0");
    set_env("SCARLETTX_PREFER_WARP", config_.prefer_warp ? "1" : "0");
    set_env("SCARLETTX_TRACE", config_.trace ? "1" : "0");
    set_env("SCARLETTX_STATS", config_.stats ? "1" : "0");
    set_env("SCARLETTX_FCAP", std::to_string(config_.fps_cap).c_str());
    if (!report.config.title_id.empty()) {
        set_env("SCARLETTX_TITLE_ID", report.config.title_id.c_str());
    }
    const std::string abs_pkg_dir = std::filesystem::absolute(package_dir).string();
    set_env("SCARLETTX_PACKAGE_ROOT", abs_pkg_dir.c_str());

    // Real Xbox Series X|S titles (e.g. Unity Game Core nativePlugins.txt) reference G:/.
    // The mapping is skipped when G: is already assigned on the host.
    struct VirtualDriveGuard {
        ~VirtualDriveGuard() { vfs::UnmapPackageDrive(); }
    } g_drive_guard;
    const bool drive_mapped = vfs::MapPackageDrive('G', abs_pkg_dir);
    if (drive_mapped) {
        std::cout << "[ScarlettX][vfs] Mapped virtual G:\\ -> " << abs_pkg_dir << "\n";
    }

    if (config_.max_frames > 0) {
        set_env("SCARLETTX_MAX_FRAMES", std::to_string(config_.max_frames).c_str());
    }
    if (!config_.rt_dump_path.empty()) {
        set_env("SCARLETTX_RT_DUMP",
                std::filesystem::absolute(config_.rt_dump_path).string().c_str());
    }
    if (!config_.raster_dump_path.empty()) {
        set_env("SCARLETTX_RASTER_DUMP",
                std::filesystem::absolute(config_.raster_dump_path).string().c_str());
    }

    if (config_.in_process) {
        MappedPeImage image;
        HleSymbolRegistry hle_registry;
        kernelx::RegisterKernelxMemorySymbols(hle_registry);
        kernelx::RegisterKernelxThreadingSymbols(hle_registry);
        if (config_.extra_hle_symbols) {
            hle_registry.MergeFrom(*config_.extra_hle_symbols);
        }
        PeMapOptions map_opts;
        map_opts.force_relocate = config_.force_relocate;
        map_opts.shim_directory = shim_dir;

        Status map_st = image.MapFile(report.executable_path, redirects_, hle_registry, map_opts);
        if (!map_st.ok()) {
            return map_st;
        }
        std::cout << "[ScarlettX][loader] Mapped " << report.executable_path << " at 0x"
                  << std::hex << image.MappedBase() << std::dec
                  << " (relocated=" << (image.WasRelocated() ? "yes" : "no")
                  << ", relocs=" << image.RelocationsApplied()
                  << ", imports=" << image.Resolutions().size() << ")" << std::endl;

        int exit_code = 0;
        Status call_st = Status::Ok();
        // Retail console PE64 images execute console-only instructions
        // (wbinvd, rdmsr, direct syscalls) that fault in a Win32 user-mode
        // process; the VEH interceptor emulates them for the duration of the
        // guest execution and uninstalls afterwards.
        {
            ScopedVehInterceptor veh_guard;
            if (image.GetExportAddress(config_.in_process_entry) != nullptr) {
                call_st = image.CallExportedEntry(config_.in_process_entry, exit_code);
            } else {
                set_env("SCARLETTX_REAL_TITLE", "1");
                // A title's relative paths start at its package root, as on the console.
                SetCurrentDirectoryA(drive_mapped ? "G:\\" : abs_pkg_dir.c_str());
                std::cout << "[ScarlettX][loader] Entering native PE entry point at "
                          << image.EntryPointAddress() << std::endl;
                // Also to the log: the launcher cannot see our stdout, so this
                // line is the only breadcrumb that the guest actually started.
                LogMessage(LogLevel::Info, "loader",
                           "Entering guest entry point at " +
                               [&] {
                                   std::ostringstream text;
                                   text << image.EntryPointAddress();
                                   return text.str();
                               }() +
                               " (the title runs until it returns or faults)");
                g_guest_base.store(image.MappedBase());
                g_guest_end.store(image.MappedBase() + image.SizeOfImage());
                LPTOP_LEVEL_EXCEPTION_FILTER previous_filter =
                    SetUnhandledExceptionFilter(&GuestFatalExceptionFilter);
                call_st = image.CallEntryPoint(exit_code);
                SetUnhandledExceptionFilter(previous_filter);
                LogMessage(call_st.ok() ? LogLevel::Info : LogLevel::Error, "loader",
                           "Guest entry point returned, exit code " +
                               std::to_string(exit_code));
            }
        }
        if (!call_st.ok()) {
            return call_st;
        }
        if (exit_code != 0) {
            return Status::Error(StatusCode::DeviceError,
                                 "In-process guest exited with non-zero code: " +
                                     std::to_string(exit_code));
        }
        return Status::Ok();
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + report.executable_path + "\"";
    std::string work_dir = std::filesystem::path(report.executable_path).parent_path().string();

    // The guest inherits this: if it cannot load it must end with an exit
    // code instead of waiting on a system error dialog.
    const UINT previous_error_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    // Searched before System32 and inherited by the child; PATH is not.
    if (!shim_dir.empty()) {
        SetDllDirectoryA(shim_dir.c_str());
    }
    const BOOL created = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                        work_dir.c_str(), &si, &pi);
    if (!shim_dir.empty()) {
        SetDllDirectoryA(nullptr);
    }
    SetErrorMode(previous_error_mode);
    if (!created) {
        return Status::Error(StatusCode::DeviceError,
                             "CreateProcessA failed for guest executable: " + report.executable_path);
    }

    const bool timed_out = WaitForSingleObject(pi.hProcess, 60000) == WAIT_TIMEOUT;
    if (timed_out) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (timed_out) {
        return Status::Error(StatusCode::DeviceError, "Guest process did not exit within 60 seconds and was stopped");
    }

    if (exit_code != 0) {
        char hex_buf[32] = {};
        std::snprintf(hex_buf, sizeof(hex_buf), "0x%08lX", static_cast<unsigned long>(exit_code));
        return Status::Error(StatusCode::DeviceError,
                             "Guest process exited with non-zero code: " +
                                 std::to_string(exit_code) + " (" + hex_buf + ")");
    }
    return Status::Ok();
#else
    return Status::Error(StatusCode::NotImplemented, "Guest PE boot currently requires Windows");
#endif
}

}  // namespace scarlettx
