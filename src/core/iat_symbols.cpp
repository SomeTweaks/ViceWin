#include "scarlettx/core/iat_symbols.h"

#include <mutex>
#include <unordered_map>

namespace scarlettx {

namespace {

std::mutex& BindingMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<uint64_t, std::string>& SlotNames() {
    static std::unordered_map<uint64_t, std::string> names;
    return names;
}

std::unordered_map<uint64_t, std::string>& TargetNames() {
    static std::unordered_map<uint64_t, std::string> names;
    return names;
}

}  // namespace

void RecordIatBinding(uint64_t slot_address, uint64_t target_address,
                      std::string name) {
    if (name.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(BindingMutex());
    if (slot_address != 0) {
        SlotNames()[slot_address] = name;
    }
    if (target_address != 0) {
        TargetNames()[target_address] = name;
    }
}

std::string LookupIatBinding(uint64_t address) {
    std::lock_guard<std::mutex> lock(BindingMutex());
    const auto slot = SlotNames().find(address);
    if (slot != SlotNames().end()) {
        return slot->second;
    }
    const auto target = TargetNames().find(address);
    if (target != TargetNames().end()) {
        return target->second;
    }
    return {};
}

}  // namespace scarlettx
