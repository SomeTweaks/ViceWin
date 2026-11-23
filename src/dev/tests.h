#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vicewindev {

// One selectable dev-bench workload.
struct TestInfo {
    std::string id;      // stable id for --run
    std::string title;   // menu line
    std::string detail;  // what it stresses, one line
};

const std::vector<TestInfo>& TestCatalog();

struct TestOptions {
    bool quick = false;        // small sizes, fewer variants (smoke)
    bool hardware = false;     // prefer a discrete GPU over WARP
    uint32_t variants = 24;    // shader soak: variants per base case
};

struct TestResult {
    std::string id;
    bool passed = false;
    double seconds = 0.0;
    std::string summary;             // one line, printed and written to the report
    std::vector<std::string> lines;  // detail rows (per shader, timings, ...)
};

TestResult RunTest(const std::string& id, const TestOptions& options);

std::string BuildMarkdownReport(const std::vector<TestResult>& results,
                                const TestOptions& options);

}  // namespace vicewindev
