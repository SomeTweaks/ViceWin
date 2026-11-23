#include <algorithm>
#include "sx_check.h"
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "scarlettx/loader/pe_loader.h"
#include "scarlettx/loader/pe_mapper.h"
#include "scarlettx/runtime/emulator.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

uint64_t FakeOrdinalHleHandler(uint64_t a, uint64_t b) {
    return (a * 1000ull) + b + 7ull;
}

}  // namespace

int main() {
    const std::filesystem::path probe_dir =
        std::filesystem::path(SCARLETTX_BINARY_DIR) / "packages" / "iat_probe";
    const std::filesystem::path probe_dll = probe_dir / "iat_probe.dll";
    const std::filesystem::path missing_probe_dll =
        std::filesystem::path(SCARLETTX_BINARY_DIR) / "packages" /
        "iat_missing_probe" / "iat_missing_probe.dll";
    const std::filesystem::path missing_ordinal_probe_dll =
        std::filesystem::path(SCARLETTX_BINARY_DIR) / "packages" /
        "iat_missing_ordinal_probe" / "iat_missing_ordinal_probe.dll";
    SX_REQUIRE(std::filesystem::exists(probe_dll));
    SX_REQUIRE(std::filesystem::exists(missing_probe_dll));
    SX_REQUIRE(std::filesystem::exists(missing_ordinal_probe_dll));

    // 1. Verify API-set resolution helper.
    SX_REQUIRE(scarlettx::ResolveApiSetDll("api-ms-win-crt-runtime-l1-1-0.dll") == "ucrtbase.dll");
    SX_REQUIRE(scarlettx::ResolveApiSetDll("api-ms-win-core-processthreads-l1-1-0.dll") ==
           "kernel32.dll");
    SX_REQUIRE(scarlettx::ResolveApiSetDll("api-ms-win-core-com-l1-1-0.dll") == "combase.dll");
    SX_REQUIRE(scarlettx::ResolveApiSetDll(
                   "api-ms-win-core-synch-l1-2-0.dll") == "kernelbase.dll");

    // 2. Verify static PE inspection sees both named and ordinal imports.
    scarlettx::DllRedirectionTable redirects;
    scarlettx::PeImageInfo info;
    SX_REQUIRE(scarlettx::InspectPeFile(probe_dll.string(), redirects, info).ok());
    SX_REQUIRE(info.is_pe64);

    bool saw_ord7 = false;
    bool saw_xgr = false;
    for (const auto& mod : info.imports) {
        const std::string dll = ToLower(mod.dll_name);
        if (dll == "xordinal_hle.dll") {
            for (const auto& sym : mod.symbols) {
                if (sym.by_ordinal && sym.ordinal == 7) saw_ord7 = true;
            }
        } else if (dll == "xgameruntime.dll") {
            saw_xgr = true;
        }
    }
    SX_REQUIRE(saw_ord7);
    SX_REQUIRE(saw_xgr);

    // Missing named and ordinal kernelx imports produce explicit resolver errors.
    {
        scarlettx::HleSymbolRegistry missing_registry;
        scarlettx::MappedPeImage missing;
        const auto missing_status = missing.MapFile(
            missing_probe_dll.string(), redirects, missing_registry);
        SX_REQUIRE(!missing_status.ok());
        SX_REQUIRE(missing_status.code == scarlettx::StatusCode::NotFound);
        SX_REQUIRE(missing_status.message.find("kernelx.dll!KernelxMissingForTest") !=
                   std::string::npos);
        SX_REQUIRE(!missing.IsMapped());
    }
    {
        scarlettx::HleSymbolRegistry missing_registry;
        scarlettx::MappedPeImage missing;
        const auto missing_status = missing.MapFile(
            missing_ordinal_probe_dll.string(), redirects, missing_registry);
        SX_REQUIRE(!missing_status.ok());
        SX_REQUIRE(missing_status.code == scarlettx::StatusCode::NotFound);
        SX_REQUIRE(missing_status.message.find("kernelx.dll!#42") !=
                   std::string::npos);
        SX_REQUIRE(!missing.IsMapped());
    }

    // Map in-process with forced relocation and an ordinal HLE override.
    scarlettx::HleSymbolRegistry hle_reg;
    hle_reg.RegisterOrdinal("xordinal_hle.dll", 7, reinterpret_cast<void*>(&FakeOrdinalHleHandler));

    scarlettx::PeMapOptions opts;
    opts.force_relocate = true;
    opts.shim_directory = SCARLETTX_BINARY_DIR;

    scarlettx::MappedPeImage mapped;
    auto st = mapped.MapFile(probe_dll.string(), redirects, hle_reg, opts);
    if (!st.ok()) {
        std::cerr << "[test_inproc_loader] MapFile failed: " << st.message << "\n";
        return 1;
    }

    SX_REQUIRE(mapped.IsMapped());
    SX_REQUIRE(mapped.WasRelocated());
    SX_REQUIRE(mapped.RelocationsApplied() > 0);

    // Verify resolution kinds.
    bool resolved_hle_ord = false;
    bool resolved_host_xgr = false;
    for (const auto& r : mapped.Resolutions()) {
        const std::string dll = ToLower(r.dll_name);
        if (dll == "xordinal_hle.dll" && r.by_ordinal && r.ordinal == 7) {
            SX_REQUIRE(r.kind == scarlettx::ImportResolutionKind::HleRegistry);
            resolved_hle_ord = true;
        } else if (dll == "xgameruntime.dll" && r.symbol_name == "XGameRuntimeInitialize") {
            SX_REQUIRE(r.kind == scarlettx::ImportResolutionKind::HostDll);
            resolved_host_xgr = true;
        }
    }
    SX_REQUIRE(resolved_hle_ord);
    SX_REQUIRE(resolved_host_xgr);

    // 4. Execute relocated global pointer probe.
    using ProbeRelocFn = uint64_t (*)();
    auto probe_reloc = reinterpret_cast<ProbeRelocFn>(mapped.GetExportAddress("ProbeRelocatedGlobal"));
    SX_REQUIRE(probe_reloc != nullptr);
    SX_REQUIRE(probe_reloc() == 0x5CA81E7700001234ull);

    // 4.5 The mapped image is known to the host unwinder: without this a C++
    //     exception thrown in guest code terminates the process.
    {
        // Leaf functions and linker thunks have no unwind entry, so take the
        // address of a function the image's own table describes.
        const auto* image = reinterpret_cast<const uint8_t*>(mapped.MappedBase());
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            image + reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        SX_REQUIRE(dir.VirtualAddress != 0 && dir.Size >= sizeof(RUNTIME_FUNCTION));
        const auto* first = reinterpret_cast<const RUNTIME_FUNCTION*>(image + dir.VirtualAddress);
        DWORD64 unwind_base = 0;
        const DWORD64 pc = mapped.MappedBase() + first->BeginAddress;
        SX_REQUIRE(RtlLookupFunctionEntry(pc, &unwind_base, nullptr) != nullptr);
        SX_REQUIRE(unwind_base == mapped.MappedBase());
    }

    // A section table or relocation data that overruns the file or image is rejected.
    {
        std::ifstream in(probe_dll, std::ios::binary);
        const std::vector<char> good((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
        const auto nt_of = [](std::vector<char>& pe) {
            return reinterpret_cast<IMAGE_NT_HEADERS64*>(
                pe.data() + reinterpret_cast<const IMAGE_DOS_HEADER*>(pe.data())->e_lfanew);
        };
        const std::filesystem::path bad_dll =
            std::filesystem::path(SCARLETTX_BINARY_DIR) / "iat_probe_malformed.dll";
        const auto map_bytes = [&](const std::vector<char>& pe) {
            std::ofstream(bad_dll, std::ios::binary)
                .write(pe.data(), static_cast<std::streamsize>(pe.size()));
            scarlettx::MappedPeImage bad;
            return bad.MapFile(bad_dll.string(), redirects, hle_reg, opts);
        };

        auto bad_sections = good;
        nt_of(bad_sections)->FileHeader.NumberOfSections = 0xFFFF;
        SX_REQUIRE(map_bytes(bad_sections).message == "Truncated section table");

        auto bad_block = good;
        const auto* nt = nt_of(bad_block);
        const DWORD reloc_rva =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
        size_t block_at = 0;
        const auto* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (reloc_rva >= sec->VirtualAddress &&
                reloc_rva - sec->VirtualAddress < sec->SizeOfRawData) {
                block_at = sec->PointerToRawData + (reloc_rva - sec->VirtualAddress);
            }
        }
        SX_REQUIRE(block_at != 0);
        reinterpret_cast<IMAGE_BASE_RELOCATION*>(bad_block.data() + block_at)->SizeOfBlock =
            0xFFFFFFF0;
        SX_REQUIRE(map_bytes(bad_block).message ==
                   "Relocation block exceeds relocation directory");

        auto bad_dir = good;
        nt_of(bad_dir)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress =
            nt->OptionalHeader.SizeOfImage;
        SX_REQUIRE(map_bytes(bad_dir).message == "Relocation directory exceeds PE image size");

        std::filesystem::remove(bad_dll);
    }

    // 4.6 Guest C++ exceptions and guest thread-local storage.
    {
        using ProbeThrowFn = int (*)(int);
        using ProbeTlsFn = int (*)();
        auto probe_throw = reinterpret_cast<ProbeThrowFn>(mapped.GetExportAddress("ProbeThrowCatch"));
        auto probe_tls = reinterpret_cast<ProbeTlsFn>(mapped.GetExportAddress("ProbeThreadLocalCounter"));
        SX_REQUIRE(probe_throw != nullptr && probe_tls != nullptr);
        for (int i = 0; i < 100; ++i) {
            SX_REQUIRE(probe_throw(i) == i + 1);
        }
        SX_REQUIRE(probe_tls() == 42);
        SX_REQUIRE(probe_tls() == 43);
        int on_thread_first = 0;
        int on_thread_second = 0;
        std::thread worker([&] {
            on_thread_first = probe_tls();
            on_thread_second = probe_tls();
        });
        worker.join();
        SX_REQUIRE(on_thread_first == 42 && on_thread_second == 43);
        SX_REQUIRE(probe_tls() == 44);
    }

    // 5. Execute ordinal-imported HLE handler probe.
    using ProbeOrdFn = uint64_t (*)(uint64_t, uint64_t);
    auto probe_ord = reinterpret_cast<ProbeOrdFn>(mapped.GetExportAddress("ProbeOrdinalHle"));
    SX_REQUIRE(probe_ord != nullptr);
    SX_REQUIRE(probe_ord(42, 99) == 42106ull);

    // Verify Emulator::BootPackage with in_process=true runs ScarlettGuestMain cleanly.
    scarlettx::EmulatorConfig cfg;
    cfg.in_process = true;
    cfg.force_relocate = true;
    cfg.shim_directory = SCARLETTX_BINARY_DIR;
    cfg.extra_hle_symbols = &hle_reg;
    scarlettx::Emulator emu(cfg);
    auto boot_st = emu.BootPackage(probe_dir.string());
    if (!boot_st.ok()) {
        std::cerr << "[test_inproc_loader] BootPackage(--in-process) failed: "
                  << boot_st.message << "\n";
        return 1;
    }

    // An image with stripped relocations must not load away from its preferred base.
    {
        const std::filesystem::path fixed_dll =
            std::filesystem::path(SCARLETTX_BINARY_DIR) / "test_inproc_loader_fixed.dll";
        std::filesystem::copy_file(probe_dll, fixed_dll,
                                   std::filesystem::copy_options::overwrite_existing);
        {
            std::fstream f(fixed_dll, std::ios::in | std::ios::out | std::ios::binary);
            IMAGE_DOS_HEADER dos{};
            IMAGE_NT_HEADERS64 nt{};
            f.read(reinterpret_cast<char*>(&dos), sizeof(dos));
            f.seekg(dos.e_lfanew);
            f.read(reinterpret_cast<char*>(&nt), sizeof(nt));
            nt.FileHeader.Characteristics |= IMAGE_FILE_RELOCS_STRIPPED;
            nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC] = {};
            f.seekp(dos.e_lfanew);
            f.write(reinterpret_cast<const char*>(&nt), sizeof(nt));
            SX_REQUIRE(f.good());
        }
        scarlettx::MappedPeImage fixed;
        SX_REQUIRE(!fixed.MapFile(fixed_dll.string(), redirects, hle_reg, opts).ok());
        SX_REQUIRE(!fixed.IsMapped());
        std::filesystem::remove(fixed_dll);
    }

    std::cout << "[test_inproc_loader] PASSED (relocs=" << mapped.RelocationsApplied()
              << ", imports=" << mapped.Resolutions().size() << ")\n";
    return 0;
}
