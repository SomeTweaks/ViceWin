#pragma once

#include <cstdint>
#include <string>

namespace scarlettx {

// Import binding map: which module!symbol sits behind an IAT slot, and which
// function address that slot was resolved to. The mapper fills it while it
// patches the IAT, so a crash site can be reported by name instead of by a raw
// address guessed from a byte pattern.
void RecordIatBinding(uint64_t slot_address, uint64_t target_address,
                      std::string name);

// Looks an address up as an IAT slot first, then as a resolved target.
// Returns an empty string when the address was not recorded.
std::string LookupIatBinding(uint64_t address);

}  // namespace scarlettx
