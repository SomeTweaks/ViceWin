#include "log_scan.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace vicewindev {

namespace {

struct LogLine {
    std::string level;
    std::string component;
    std::string message;
};

// "[<ms>][<level>][<component>] <message>"
bool ParseLine(const std::string& raw, LogLine& out_line) {
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    if (line.empty() || line.front() != '[') {
        return false;
    }
    const size_t stamp_end = line.find(']');
    if (stamp_end == std::string::npos) return false;
    const size_t level_open = line.find('[', stamp_end);
    if (level_open == std::string::npos) return false;
    const size_t level_close = line.find(']', level_open);
    if (level_close == std::string::npos) return false;
    const size_t comp_open = line.find('[', level_close);
    if (comp_open == std::string::npos) return false;
    const size_t comp_close = line.find(']', comp_open);
    if (comp_close == std::string::npos) return false;

    out_line.level = line.substr(level_open + 1, level_close - level_open - 1);
    out_line.component = line.substr(comp_open + 1, comp_close - comp_open - 1);
    size_t message_start = comp_close + 1;
    while (message_start < line.size() && line[message_start] == ' ') {
        ++message_start;
    }
    out_line.message = line.substr(message_start);
    return true;
}

bool ExtractHexAfter(const std::string& text, const std::string& key, uint64_t& out_value) {
    const size_t at = text.find(key);
    if (at == std::string::npos) return false;
    size_t cursor = at + key.size();
    while (cursor < text.size() && text[cursor] == ' ') ++cursor;
    size_t start = cursor;
    if (cursor + 2 <= text.size() && text[cursor] == '0' &&
        (text[cursor + 1] == 'x' || text[cursor + 1] == 'X')) {
        cursor += 2;
    }
    size_t digits = cursor;
    uint64_t value = 0;
    bool any = false;
    while (cursor < text.size()) {
        const char c = text[cursor];
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        if (digit < 0) break;
        value = (value << 4) | static_cast<uint64_t>(digit);
        ++cursor;
        any = true;
    }
    if (!any || cursor == digits) {
        // Not a number: report the raw token so the operator still sees it.
        (void)start;
        return false;
    }
    out_value = value;
    return true;
}

std::string Hex(uint64_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << value;
    return text.str();
}

struct SyscallStat {
    uint64_t count = 0;
    uint64_t first_rip = 0;
    uint64_t rcx = 0;
    uint64_t rdx = 0;
    uint64_t r8 = 0;
    uint64_t r9 = 0;
    bool have_args = false;
};

std::string GuessHandler(uint64_t number, const SyscallStat& stat) {
    // Console syscall numbers are small; a huge value means the stub's `mov
    // eax, imm32` never ran and RAX still holds data (an address, usually).
    // Binding that would send real work to the wrong handler.
    if (number > 0xFFFFull) {
        return "RAX held data, not a number — inspect rip";
    }
    // Deliberately conservative heuristics: only shapes that are hard to
    // confuse. Everything else stays "?" so nobody binds the wrong handler.
    if (stat.rcx != 0 && stat.rdx == 0 && stat.r8 == 0 && stat.r9 == 0 &&
        stat.rcx < 0x1000000ull) {
        return "NtClose?";
    }
    if (stat.rdx != 0 && stat.rcx == 0 && stat.r8 == 0 && stat.r9 == 0) {
        return "NtQueryPerformanceCounter? / NtDelayExecution?";
    }
    if (stat.rcx != 0 && stat.rdx != 0 && stat.r8 == 0 && stat.r9 == 0) {
        return "NtQueryPerformanceCounter? / NtSetEvent?";
    }
    if (stat.r8 != 0 && stat.r9 != 0) {
        return "NtAllocateVirtualMemory? / NtProtectVirtualMemory?";
    }
    return "?";
}

}  // namespace

std::string BuildLogScanReport(const std::string& log_text,
                               const std::string& source_name) {
    size_t total_lines = 0;
    size_t error_lines = 0;
    size_t warning_lines = 0;

    std::map<uint64_t, SyscallStat> unknown_syscalls;
    std::map<std::string, uint64_t> unresolved_imports;
    std::map<std::string, uint64_t> emulated_instructions;
    std::map<std::string, uint64_t> error_texts;
    std::vector<std::string> dxc_paths;
    size_t dxc_skipped = 0;
    size_t dxc_version_seen = 0;

    size_t cursor = 0;
    while (cursor <= log_text.size()) {
        const size_t newline = log_text.find('\n', cursor);
        const std::string raw = log_text.substr(
            cursor, newline == std::string::npos ? std::string::npos : newline - cursor);
        cursor = newline == std::string::npos ? log_text.size() + 1 : newline + 1;
        if (raw.empty()) {
            continue;
        }
        ++total_lines;

        LogLine line;
        if (!ParseLine(raw, line)) {
            continue;
        }
        if (line.level == "error") {
            ++error_lines;
        } else if (line.level == "warning") {
            ++warning_lines;
        }

        const std::string& message = line.message;

        if (message.rfind("unknown console syscall", 0) == 0) {
            uint64_t number = 0;
            if (ExtractHexAfter(message, "rax=", number)) {
                SyscallStat& stat = unknown_syscalls[number];
                ++stat.count;
                if (stat.first_rip == 0) {
                    ExtractHexAfter(message, "rip=", stat.first_rip);
                    stat.have_args =
                        ExtractHexAfter(message, "rcx=", stat.rcx) &&
                        ExtractHexAfter(message, "rdx=", stat.rdx) &&
                        ExtractHexAfter(message, "r8=", stat.r8) &&
                        ExtractHexAfter(message, "r9=", stat.r9);
                }
            }
            continue;
        }

        const size_t unresolved_at = message.find("Unresolved import: ");
        if (unresolved_at != std::string::npos) {
            std::string symbol = message.substr(unresolved_at + 19);
            const size_t paren = symbol.find(" (target");
            if (paren != std::string::npos) {
                symbol = symbol.substr(0, paren);
            }
            ++unresolved_imports[symbol];
            continue;
        }

        const size_t emulated_at = message.find("emulated console instruction ");
        if (emulated_at != std::string::npos) {
            std::string what = message.substr(emulated_at + 29);
            const size_t at = what.find(" at rip=");
            if (at != std::string::npos) {
                what = what.substr(0, at);
            }
            ++emulated_instructions[what];
            continue;
        }

        if (message.rfind("[dxc]", 0) == 0) {
            if (message.find("[dxc] using ") == 0) {
                if (dxc_paths.empty() ||
                    std::find(dxc_paths.begin(), dxc_paths.end(), message) == dxc_paths.end()) {
                    dxc_paths.push_back(message);
                }
            } else if (message.find("[dxc] skipped ") == 0) {
                ++dxc_skipped;
            } else if (message.find("[dxc] compiler version ") == 0) {
                ++dxc_version_seen;
                if (std::find(dxc_paths.begin(), dxc_paths.end(), message) == dxc_paths.end()) {
                    dxc_paths.push_back(message);
                }
            }
            continue;
        }

        if (line.level == "error") {
            std::string key = line.component + ": " + message.substr(0, 140);
            ++error_texts[key];
        }
    }

    std::ostringstream report;
    report << "# vicewin.log scan — " << (source_name.empty() ? "vicewin.log" : source_name)
           << "\n\n";
    report << "- lines: " << total_lines << " (errors: " << error_lines
           << ", warnings: " << warning_lines << ")\n";
    report << "- DXC: " << (dxc_paths.empty() ? "never loaded" : dxc_paths.front()) << "\n";
    if (!dxc_paths.empty()) {
        report << "- DXC candidate lines: " << dxc_paths.size() << ", skipped: "
               << dxc_skipped << "\n";
    }
    report << "\n";

    report << "## Unbound console syscalls (" << unknown_syscalls.size() << ")\n\n";
    if (unknown_syscalls.empty()) {
        report << "None. Either the title never issued a raw syscall, or every "
                  "number it used was already bound.\n\n";
    } else {
        report << "| rax | hits | first rip | rcx | rdx | r8 | r9 | likely |\n";
        report << "|---|---|---|---|---|---|---|---|\n";
        std::vector<std::pair<uint64_t, SyscallStat>> ordered(unknown_syscalls.begin(),
                                                             unknown_syscalls.end());
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& left, const auto& right) {
                      return left.second.count > right.second.count;
                  });
        for (const auto& entry : ordered) {
            const SyscallStat& stat = entry.second;
            report << "| " << Hex(entry.first) << " | " << stat.count << " | "
                   << Hex(stat.first_rip) << " | " << Hex(stat.rcx) << " | "
                   << Hex(stat.rdx) << " | " << Hex(stat.r8) << " | "
                   << Hex(stat.r9) << " | " << GuessHandler(entry.first, stat)
                   << " |\n";
        }
        report << "\nPaste-ready starting point (replace NAME with the handler that "
                  "matches the argument shape, then rerun):\n\n```\n";
        report << "set SCARLETTX_SYSCALL_MAP=";
        bool first = true;
        for (const auto& entry : ordered) {
            if (entry.first > 0xFFFFull) {
                continue;  // not a syscall number; see the table note
            }
            if (!first) report << ",";
            first = false;
            report << Hex(entry.first) << "=<NAME>";
        }
        report << "\n```\n\nHandlers available: NtAllocateVirtualMemory, "
                  "NtFreeVirtualMemory, NtProtectVirtualMemory, "
                  "NtQuerySystemInformation, NtDelayExecution, NtSetEvent, "
                  "NtWaitForSingleObject, NtQueryPerformanceCounter, NtClose, "
                  "NtCreateFile, NtReadFile.\n\n";
    }

    report << "## Unresolved guest imports (" << unresolved_imports.size() << ")\n\n";
    if (unresolved_imports.empty()) {
        report << "None.\n\n";
    } else {
        std::vector<std::pair<std::string, uint64_t>> ordered(unresolved_imports.begin(),
                                                             unresolved_imports.end());
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& left, const auto& right) {
                      return left.second > right.second;
                  });
        for (const auto& entry : ordered) {
            report << "- `" << entry.first << "` x" << entry.second << "\n";
        }
        report << "\n";
    }

    report << "## Emulated console instructions\n\n";
    if (emulated_instructions.empty()) {
        report << "None recorded.\n\n";
    } else {
        for (const auto& entry : emulated_instructions) {
            report << "- " << entry.first << ": " << entry.second << "\n";
        }
        report << "\n";
    }

    if (!error_texts.empty()) {
        report << "## Distinct errors (top 15)\n\n";
        std::vector<std::pair<std::string, uint64_t>> ordered(error_texts.begin(),
                                                             error_texts.end());
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& left, const auto& right) {
                      return left.second > right.second;
                  });
        const size_t limit = std::min<size_t>(15, ordered.size());
        for (size_t i = 0; i < limit; ++i) {
            report << "- x" << ordered[i].second << " — " << ordered[i].first << "\n";
        }
        report << "\n";
    }

    return report.str();
}

}  // namespace vicewindev
