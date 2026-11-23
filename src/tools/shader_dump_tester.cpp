#include "scarlettx/gpu/rdna2_lifter.h"
#include "scarlettx/gpu/shader_ir.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr size_t kScanChunkSize = 1024u * 1024u;
constexpr uint32_t kMaximumContainerSize = 256u * 1024u * 1024u;

struct SignatureOffsets {
    std::vector<uint64_t> dxbc;
    std::vector<uint64_t> xdat;
    std::vector<uint64_t> xbch;
};

struct Report {
    uint64_t files_scanned = 0;
    uint64_t scan_failures = 0;
    uint64_t marker_count = 0;
    uint64_t container_count = 0;
    uint64_t native_shader_count = 0;
    uint64_t native_compile_success_count = 0;
    uint64_t success_count = 0;
    std::map<std::string, uint64_t> unsupported_opcodes;
    std::vector<std::string> details;
};

std::string HexOffset(uint64_t offset) {
    std::ostringstream text;
    text << "0x" << std::hex << offset;
    return text.str();
}

void FindInBlock(
    const std::vector<uint8_t>& block,
    uint64_t block_offset,
    uint64_t previous_file_size,
    std::string_view signature,
    std::vector<uint64_t>& offsets) {
    auto it = block.begin();
    while (it != block.end()) {
        it = std::search(it, block.end(), signature.begin(), signature.end());
        if (it == block.end()) break;
        const uint64_t offset =
            block_offset + static_cast<uint64_t>(it - block.begin());
        if (offset + signature.size() > previous_file_size) {
            offsets.push_back(offset);
        }
        ++it;
    }
}

bool ScanFile(const std::filesystem::path& path,
              SignatureOffsets& offsets,
              std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open file";
        return false;
    }

    std::vector<uint8_t> chunk(kScanChunkSize);
    std::vector<uint8_t> carry;
    uint64_t bytes_read = 0;
    for (;;) {
        file.read(reinterpret_cast<char*>(chunk.data()),
                  static_cast<std::streamsize>(chunk.size()));
        const std::streamsize count = file.gcount();
        if (count < 0) {
            error = "negative read size";
            return false;
        }
        if (count == 0) {
            if (!file.eof()) {
                error = "file read failed";
                return false;
            }
            break;
        }

        const uint64_t previous_file_size = bytes_read;
        const uint64_t block_offset = bytes_read - carry.size();
        std::vector<uint8_t> block;
        block.reserve(carry.size() + static_cast<size_t>(count));
        block.insert(block.end(), carry.begin(), carry.end());
        block.insert(block.end(), chunk.begin(),
                     chunk.begin() + static_cast<std::ptrdiff_t>(count));

        FindInBlock(block, block_offset, previous_file_size, "DXBC",
                    offsets.dxbc);
        FindInBlock(block, block_offset, previous_file_size, "XDAT",
                    offsets.xdat);
        FindInBlock(block, block_offset, previous_file_size, "XBCH",
                    offsets.xbch);
        bytes_read += static_cast<uint64_t>(count);

        const size_t carry_size = std::min<size_t>(3, block.size());
        carry.assign(block.end() - static_cast<std::ptrdiff_t>(carry_size),
                     block.end());
    }
    return true;
}

bool ContainsOffset(const std::vector<uint64_t>& offsets,
                    uint64_t begin,
                    uint64_t end) {
    const auto it = std::lower_bound(offsets.begin(), offsets.end(), begin);
    return it != offsets.end() && *it < end;
}

bool ReadContainerAt(const std::filesystem::path& path,
                     uint64_t offset,
                     uint64_t file_size,
                     std::vector<uint8_t>& container,
                     std::string& error) {
    if (offset > file_size || file_size - offset < 32) {
        error = "truncated DXBC header";
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot reopen file";
        return false;
    }
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::array<uint8_t, 32> header{};
    file.read(reinterpret_cast<char*>(header.data()), header.size());
    if (file.gcount() != static_cast<std::streamsize>(header.size()) ||
        std::memcmp(header.data(), "DXBC", 4) != 0) {
        error = "invalid DXBC header";
        return false;
    }
    uint32_t size = 0;
    std::memcpy(&size, header.data() + 24, sizeof(size));
    if (size < header.size() || size > kMaximumContainerSize ||
        size > file_size - offset) {
        error = "invalid or oversized DXBC container";
        return false;
    }
    container.resize(size);
    std::memcpy(container.data(), header.data(), header.size());
    file.read(reinterpret_cast<char*>(container.data() + header.size()),
              static_cast<std::streamsize>(size - header.size()));
    if (file.gcount() != static_cast<std::streamsize>(size - header.size())) {
        container.clear();
        error = "truncated DXBC container";
        return false;
    }
    return true;
}

void RecordUnsupported(const std::string& message, Report& report) {
    if (message.find("Unknown RDNA2 encoding prefix") != std::string::npos) {
        ++report.unsupported_opcodes["<unknown-encoding>"];
        return;
    }
    constexpr std::string_view prefix = "RDNA2 lifting does not support ";
    const size_t start = message.find(prefix);
    if (start == std::string::npos) return;
    const size_t name_start = start + prefix.size();
    const size_t name_end = message.find(' ', name_start);
    if (name_start < message.size() && name_end > name_start) {
        const std::string opcode =
            message.substr(name_start, name_end - name_start);
        if (opcode.rfind("unknown_", 0) != 0 &&
            opcode.rfind("unsupported_", 0) != 0) {
            ++report.unsupported_opcodes[opcode];
        }
    }
}

void InspectRdna2Instructions(
    const scarlettx::XboxRdna2Program& program,
    scarlettx::ShaderStage stage,
    Report& report,
    const std::string& file_label,
    uint64_t offset) {
    std::vector<scarlettx::Rdna2Instruction> instructions;
    const scarlettx::Status decode_status =
        scarlettx::DecodeRdna2Instructions(program.instruction_bytes, instructions);
    if (!decode_status.ok()) {
        report.details.push_back(
            file_label + " @" + HexOffset(offset) +
            ": decode failed: " + decode_status.message);
        return;
    }
    for (const auto& instruction : instructions) {
        if (instruction.mnemonic.rfind("unknown_", 0) == 0 ||
            instruction.mnemonic.rfind("unsupported_", 0) == 0) {
            ++report.unsupported_opcodes[instruction.mnemonic];
        }
    }

    scarlettx::Rdna2TranslationOptions options{};
    options.stage = stage;
    options.root_cbv_count = 128;
    options.root_srv_count = 128;
    options.root_sampler_count = 128;
    options.root_uav_count = 128;
    options.thread_group_size_x = program.thread_group_size_x;
    options.thread_group_size_y = program.thread_group_size_y;
    options.thread_group_size_z = program.thread_group_size_z;
    scarlettx::Rdna2IrModule ir{};
    const scarlettx::Status lift_status =
        scarlettx::BuildRdna2Ir(instructions, options, ir);
    if (!lift_status.ok()) {
        if (lift_status.message.find("BVH traversal-step opcode") !=
            std::string::npos) {
            for (const auto& instruction : instructions) {
                if (instruction.mnemonic == "image_bvh_intersect_ray" ||
                    instruction.mnemonic == "image_bvh64_intersect_ray") {
                    ++report.unsupported_opcodes[instruction.mnemonic];
                }
            }
        }
        report.details.push_back(
            file_label + " @" + HexOffset(offset) +
            ": lift failed: " + lift_status.message);
    }
}

bool ProcessContainer(const std::filesystem::path& path,
                      uint64_t offset,
                      uint64_t file_size,
                      const SignatureOffsets& signatures,
                      Report& report) {
    std::vector<uint8_t> container;
    std::string error;
    if (!ReadContainerAt(path, offset, file_size, container, error)) {
        report.details.push_back(path.string() + " @" + HexOffset(offset) +
                                 ": " + error);
        return false;
    }
    const uint64_t end = offset + container.size();
    if (!ContainsOffset(signatures.xdat, offset, end) &&
        !ContainsOffset(signatures.xbch, offset, end)) {
        return false;
    }

    ++report.container_count;
    const std::string label = path.string();
    scarlettx::ShaderModuleInfo info{};
    scarlettx::Status status =
        scarlettx::InspectShaderContainer(container, info);
    if (!status.ok()) {
        report.details.push_back(label + " @" + HexOffset(offset) +
                                 ": inspect failed: " + status.message);
        return false;
    }

    const bool native_shader =
        info.has_xbox_native_shader || info.has_missing_dxil_bitcode;
    if (native_shader) {
        ++report.native_shader_count;
        scarlettx::XboxRdna2Program program{};
        status = scarlettx::ExtractXboxRdna2Program(container, program);
        if (status.ok()) {
            InspectRdna2Instructions(program, info.stage, report, label, offset);
        } else {
            report.details.push_back(label + " @" + HexOffset(offset) +
                                     ": FSSM extraction failed: " + status.message);
        }
    }

    scarlettx::ShaderTranslationOptions options{};
    options.stage_override = info.stage;
    options.root_cbv_count = 128;
    options.root_srv_count = 128;
    options.root_sampler_count = 128;
    options.root_uav_count = 128;
    options.bypass_shader_cache = true;
    scarlettx::ShaderTranslationResult translated{};
    status = scarlettx::TranslateXboxShaderToPc(container, options, translated);
    if (!status.ok()) {
        RecordUnsupported(status.message, report);
        report.details.push_back(label + " @" + HexOffset(offset) +
                                 ": translation failed: " + status.message);
        return false;
    }
    ++report.success_count;
    if (native_shader) ++report.native_compile_success_count;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: vicewin_dump_tester <game-directory>\n";
        return 2;
    }
    const std::filesystem::path root(argv[1]);
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec) || ec) {
        std::cerr << "Not a readable directory: " << root.string() << "\n";
        return 2;
    }

    Report report;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    if (ec) {
        std::cerr << "Cannot traverse directory: " << ec.message() << "\n";
        return 2;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            ++report.scan_failures;
            report.details.push_back("directory traversal failed: " + ec.message());
            ec.clear();
            continue;
        }
        if (!it->is_regular_file(ec) || ec) {
            if (ec) {
                ++report.scan_failures;
                report.details.push_back(
                    it->path().string() + ": cannot inspect file: " +
                    ec.message());
            }
            ec.clear();
            continue;
        }
        ++report.files_scanned;
        SignatureOffsets signatures;
        std::string scan_error;
        if (!ScanFile(it->path(), signatures, scan_error)) {
            ++report.scan_failures;
            report.details.push_back(it->path().string() + ": " + scan_error);
            continue;
        }
        report.marker_count += signatures.xdat.size() + signatures.xbch.size();

        const uint64_t file_size = std::filesystem::file_size(it->path(), ec);
        if (ec) {
            ++report.scan_failures;
            report.details.push_back(it->path().string() +
                                    ": cannot query file size: " + ec.message());
            ec.clear();
            continue;
        }
        const uint64_t containers_before = report.container_count;
        for (const uint64_t offset : signatures.dxbc) {
            ProcessContainer(it->path(), offset, file_size, signatures, report);
        }
        if (report.container_count == containers_before &&
            (!signatures.xdat.empty() || !signatures.xbch.empty())) {
            report.details.push_back(
                it->path().string() +
                ": XDAT/XBCH markers were not enclosed in a valid DXBC container");
        }
    }
    if (ec) {
        ++report.scan_failures;
        report.details.push_back("directory traversal failed: " + ec.message());
        ec.clear();
    }

    const std::filesystem::path output_directory =
        std::filesystem::current_path(ec);
    if (ec) {
        std::cerr << "Cannot resolve current directory: " << ec.message() << "\n";
        return 1;
    }
    const std::filesystem::path report_path =
        output_directory / "vicewin_dump_report.log";
    std::ofstream output(report_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "Cannot write report: " << report_path.string() << "\n";
        return 1;
    }
    output << "ViceWin shader dump validation report\n"
           << "Root: " << root.string() << "\n"
           << "Files scanned: " << report.files_scanned << "\n"
           << "Scan failures: " << report.scan_failures << "\n"
           << "XDAT/XBCH markers: " << report.marker_count << "\n"
           << "Shader containers: " << report.container_count << "\n"
           << "Native RDNA2 shaders: " << report.native_shader_count << "\n"
           << "Successful native RDNA2 compiles: "
           << report.native_compile_success_count << "\n"
           << "Successful translations: " << report.success_count << "\n"
           << "Success rate: ";
    if (report.container_count == 0) {
        output << "n/a\n";
    } else {
        const double percent =
            100.0 * static_cast<double>(report.success_count) /
            static_cast<double>(report.container_count);
        output << std::fixed << std::setprecision(2) << percent << "%\n";
    }
    output << "Native compile success rate: ";
    if (report.native_shader_count == 0) {
        output << "n/a\n";
    } else {
        const double percent =
            100.0 * static_cast<double>(report.native_compile_success_count) /
            static_cast<double>(report.native_shader_count);
        output << std::fixed << std::setprecision(2) << percent << "%\n";
    }
    output << "\nUnsupported RDNA2 opcodes (observed counts):\n";
    if (report.unsupported_opcodes.empty()) {
        output << "  none identified\n";
    } else {
        for (const auto& [opcode, count] : report.unsupported_opcodes) {
            output << "  " << opcode << ": " << count << "\n";
        }
    }
    output << "\nDetails:\n";
    if (report.details.empty()) {
        output << "  none\n";
    } else {
        for (const std::string& detail : report.details) {
            output << "  " << detail << "\n";
        }
    }
    output.close();
    if (!output) {
        std::cerr << "Failed while writing report: " << report_path.string() << "\n";
        return 1;
    }

    std::cout << "Scanned " << report.files_scanned << " files; "
              << report.success_count << "/" << report.container_count
              << " shader containers translated. Report: "
              << report_path.string() << "\n";
    return report.container_count != 0 &&
                   report.success_count == report.container_count &&
                   report.scan_failures == 0
               ? 0
               : 1;
}
