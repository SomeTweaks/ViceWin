#pragma once

#include <cstdint>

namespace scarlettx {

// Ring buffer of the most recent HLE entry points and what they returned.
//
// A retail title that asserts on a null pointer gives no hint which call
// produced it, and wrapping every shim export would be a lot of noise for one
// answer. Instead every interesting entry point reports here: suspicious
// results (null, 0xC00000BB STATUS_NOT_SUPPORTED, failed HRESULT) are logged
// immediately, and the ring is dumped when the guest dies, so the last calls
// before the trap are always visible.
void HleTrace(const char* function, uint64_t result);
void HleTrace(const char* function, uint64_t result, uint64_t a0, uint64_t a1);

// Writes the ring (newest last) into the log. Called from the fatal-exception
// filter and can be called manually when diagnosing.
void HleTraceFaultDump(const char* reason);

}  // namespace scarlettx
