#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "log_scan.h"
#include "tests.h"

#if defined(_WIN32)
#include <io.h>
#endif

namespace {

using vicewindev::TestOptions;
using vicewindev::TestResult;

// Defined below; used by both the menu and --scan-log.
int ScanLog(const std::string& path);

void PrintBanner() {
    std::cout << "\n";
    std::cout << "  ViceWin DevBench — GTA VI class loads\n";
    std::cout << "  heavy SM 6.6 shaders + GPU physics against a CPU reference\n";
    std::cout << "  ---------------------------------------------------------------\n";
}

void PrintCatalog() {
    const auto& catalog = vicewindev::TestCatalog();
    PrintBanner();
    for (size_t i = 0; i < catalog.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << catalog[i].title << "\n";
        std::cout << "       " << catalog[i].detail << "\n";
    }
    std::cout << "  A. run every test above\n";
    std::cout << "  8. scan a vicewin.log (paste the path)\n";
    std::cout << "  0. exit\n\n";
}

void PrintUsage() {
    PrintBanner();
    std::cout << "  usage:\n"
              << "    vicewindev                       interactive test menu\n"
              << "    vicewindev --list                print the test catalog\n"
              << "    vicewindev --menu                force the interactive menu\n"
              << "    vicewindev --run <n|id> [...]    run specific tests\n"
              << "    vicewindev --all                 run every test\n"
              << "    vicewindev --scan-log <file>     triage a vicewin.log "
                 "(syscalls, imports, DXC, errors)\n\n"
              << "  flags:\n"
              << "    --quick          small sizes (smoke run, seconds)\n"
              << "    --hardware       prefer a discrete GPU for the physics dispatch\n"
              << "    --variants N     shader variants per base case (default 24)\n"
              << "    --report FILE    write a markdown report\n";
}

std::string Trim(const std::string& text) {
    size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t')) {
        ++start;
    }
    size_t end = text.size();
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    return text.substr(start, end - start);
}

bool ResolveSelection(const std::string& token, std::string& out_id) {
    const auto& catalog = vicewindev::TestCatalog();
    char* end = nullptr;
    const long number = std::strtol(token.c_str(), &end, 10);
    if (end != token.c_str() && *end == '\0') {
        if (number == 0) {
            return false;
        }
        const size_t index = static_cast<size_t>(number - 1);
        if (index >= catalog.size()) {
            return false;
        }
        out_id = catalog[index].id;
        return true;
    }
    for (const auto& entry : catalog) {
        if (entry.id == token) {
            out_id = entry.id;
            return true;
        }
    }
    return false;
}

void PrintResult(const TestResult& result) {
    std::cout << "\n  [" << (result.passed ? "PASS" : "FAIL") << "] " << result.id
              << "  (" << result.seconds << " s)\n";
    std::cout << "    " << result.summary << "\n";
    for (const std::string& line : result.lines) {
        std::cout << "      " << line << "\n";
    }
    std::cout << std::flush;
}

bool WriteReport(const std::string& path, const std::vector<TestResult>& results,
                 const TestOptions& options) {    if (path.empty()) {
        return true;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "  cannot write report: " << path << "\n";
        return false;
    }
    output << vicewindev::BuildMarkdownReport(results, options);
    std::cout << "  report written: " << path << "\n";
    return true;
}

int RunSelection(const std::vector<std::string>& ids, const TestOptions& options,
                 const std::string& report_path) {
    std::vector<TestResult> results;
    bool all_passed = true;
    for (const std::string& id : ids) {
        TestResult result = vicewindev::RunTest(id, options);
        PrintResult(result);
        all_passed = all_passed && result.passed;
        results.push_back(std::move(result));
    }
    WriteReport(report_path, results, options);
    std::cout << "\n  " << (all_passed ? "ALL SELECTED TESTS PASSED"
                                       : "SOME TESTS FAILED")
              << "\n\n";
    return all_passed ? 0 : 1;
}

int InteractiveMenu(TestOptions options, const std::string& report_path) {
    std::vector<TestResult> results;
    for (;;) {
        PrintCatalog();
        std::cout << "  select test (number or id, A for all, 0 to exit): "
                  << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) {
            std::cout << "\n";
            return 0;
        }
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        if (line == "0" || line == "q" || line == "exit") {
            WriteReport(report_path, results, options);
            return 0;
        }
        if (line == "8" || line == "log" || line == "scan") {
            std::cout << "  path to vicewin.log: " << std::flush;
            std::string path;
            if (!std::getline(std::cin, path)) {
                return 0;
            }
            path = Trim(path);
            if (!path.empty()) {
                ScanLog(path);
            }
            continue;
        }
        std::vector<std::string> ids;
        if (line == "a" || line == "A" || line == "all") {
            for (const auto& entry : vicewindev::TestCatalog()) {
                ids.push_back(entry.id);
            }
        } else {
            std::string id;
            if (!ResolveSelection(line, id)) {
                std::cout << "  unknown selection: " << line << "\n";
                continue;
            }
            ids.push_back(id);
        }
        for (const std::string& id : ids) {
            TestResult result = vicewindev::RunTest(id, options);
            PrintResult(result);
            results.push_back(std::move(result));
        }
        WriteReport(report_path, results, options);
    }
}

// Reads a vicewin.log and prints the triage report. Returns the process exit
// code: 0 on success, 1 when the file cannot be read.
int ScanLog(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "cannot open log: " << path << "\n";
        return 1;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::cout << "\n" << vicewindev::BuildLogScanReport(buffer.str(), path) << "\n";
    return 0;
}

bool StdinIsTerminal() {
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    _setmaxstdio(8192);
#endif
    std::cout.setf(std::ios::unitbuf);

    TestOptions options;
    std::string report_path;
    std::vector<std::string> selections;
    bool run_all = false;
    bool list_only = false;
    bool force_menu = false;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--quick") {
            options.quick = true;
        } else if (argument == "--hardware") {
            options.hardware = true;
        } else if (argument == "--list") {
            list_only = true;
        } else if (argument == "--menu") {
            force_menu = true;
        } else if (argument == "--all") {
            run_all = true;
        } else if (argument == "--help" || argument == "-h" || argument == "/?") {
            PrintUsage();
            return 0;
        } else if (argument == "--variants" && i + 1 < argc) {
            options.variants =
                static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            if (options.variants == 0) {
                options.variants = 1;
            }
        } else if (argument == "--report" && i + 1 < argc) {
            report_path = argv[++i];
        } else if (argument == "--scan-log" && i + 1 < argc) {
            return ScanLog(argv[++i]);
        } else if (argument == "--run" && i + 1 < argc) {
            std::string id;
            if (!ResolveSelection(Trim(argv[++i]), id)) {
                std::cerr << "unknown test: " << argv[i] << "\n";
                return 2;
            }
            selections.push_back(id);
        } else {
            std::cerr << "unknown argument: " << argument << "\n";
            PrintUsage();
            return 2;
        }
    }

    if (list_only) {
        PrintCatalog();
        for (const auto& entry : vicewindev::TestCatalog()) {
            std::cout << "  id: " << entry.id << "\n";
        }
        return 0;
    }

    if (run_all) {
        selections.clear();
        for (const auto& entry : vicewindev::TestCatalog()) {
            selections.push_back(entry.id);
        }
    }

    if (!selections.empty()) {
        return RunSelection(selections, options, report_path);
    }

    if (!force_menu && !StdinIsTerminal()) {
        PrintCatalog();
        std::cout << "  (stdin is not a terminal; pass --run <id> or --all)\n";
        return 0;
    }

    return InteractiveMenu(options, report_path);
}
