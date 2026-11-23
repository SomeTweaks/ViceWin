#pragma once

#include <string_view>

namespace scarlettx {

enum class LogLevel {
    Trace,
    Info,
    Warning,
    Error,
};

void LogMessage(LogLevel level, std::string_view component, std::string_view message);

}  // namespace scarlettx
