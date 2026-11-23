#include "sx_check.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "core/logger.h"

namespace {

size_t CountOccurrences(const std::string& haystack, const std::string& needle) {
    size_t count = 0;
    size_t position = 0;
    while ((position = haystack.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    const std::filesystem::path log_path =
        std::filesystem::temp_directory_path() / "scarlettx_logger_spam_test.log";
    std::error_code ec;
    std::filesystem::remove(log_path, ec);

#if defined(_WIN32)
    _putenv_s("VICEWIN_LOG_PATH", log_path.string().c_str());
#else
    setenv("VICEWIN_LOG_PATH", log_path.string().c_str(), 1);
#endif

    // More than kSpamThreshold (50) identical consecutive entries.
    for (int i = 0; i < 60; ++i) {
        scarlettx::LogMessage(scarlettx::LogLevel::Warning, "spam", "same line");
    }
    // A different line closes the run and must flush the summary.
    scarlettx::LogMessage(scarlettx::LogLevel::Info, "spam", "tail line");

    const std::string text = ReadTextFile(log_path);
    SX_REQUIRE(!text.empty());

    // Exactly the threshold count of the repeated line reaches the file.
    SX_REQUIRE(CountOccurrences(text, "same line") == 50);
    // One marker at the collapse point ...
    SX_REQUIRE(text.find("collapsing further repeats of the previous line") !=
               std::string::npos);
    // ... and one summary when the run ends (60 - 50 = 10 suppressed).
    SX_REQUIRE(text.find("previous log line repeated 10 more times") !=
               std::string::npos);
    // The line after the run is written normally: the logger recovers.
    SX_REQUIRE(text.find("tail line") != std::string::npos);

    // Distinct consecutive lines are never collapsed.
    const size_t before_distinct = CountOccurrences(text, "distinct");
    for (int i = 0; i < 120; ++i) {
        scarlettx::LogMessage(scarlettx::LogLevel::Info, "spam",
                              "distinct " + std::to_string(i));
    }
    const std::string after = ReadTextFile(log_path);
    SX_REQUIRE(CountOccurrences(after, "distinct") - before_distinct == 120);

    std::filesystem::remove(log_path, ec);
    std::cout << "[test_logger_spam] PASSED\n";
    return 0;
}
