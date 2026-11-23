#pragma once

#include <string>

namespace vicewindev {

// Parses a vicewin.log and returns a markdown report that answers the three
// questions a first retail boot leaves behind:
//   1. which console syscall numbers were hit but not bound (and with what
//      arguments), so SCARLETTX_SYSCALL_MAP can be filled in;
//   2. which imports stayed unresolved (the guest asked for something the HLE
//      surface does not provide);
//   3. which DXC build was used and whether validation/compilation failed.
// The input is untrusted text: the scanner never throws and tolerates
// truncated or interleaved lines.
std::string BuildLogScanReport(const std::string& log_text, const std::string& source_name);

}  // namespace vicewindev
