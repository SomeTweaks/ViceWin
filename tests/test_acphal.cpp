#include <cstdint>
#include <cstring>
#include <iostream>

#include "scarlettx/audio/acphal_abi.h"

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << "[test_acphal] FAILED: " #condition                   \
                      << " at line " << __LINE__ << "\n";                      \
            return 1;                                                           \
        }                                                                       \
    } while (0)

using ConnectFn = int32_t(SCARLETTX_ACP_CALL*)(void*, uint32_t, uint32_t);
using DisconnectFn = int32_t(SCARLETTX_ACP_CALL*)(void*);
using SubmitFn = int32_t(SCARLETTX_ACP_CALL*)(
    void*, uint32_t, uint32_t, uint32_t, const void*, uint32_t);
using PopMessageFn = uint8_t(SCARLETTX_ACP_CALL*)(void*, void*);
using ReleaseFn = uint32_t(SCARLETTX_ACP_CALL*)(void*);

int main() {
    void* acp = nullptr;
    CHECK(AcpHalCreate(&acp) == 0);
    CHECK(acp != nullptr);
    void** vtable = *reinterpret_cast<void***>(acp);

    const auto connect = reinterpret_cast<ConnectFn>(vtable[0]);
    const auto disconnect = reinterpret_cast<DisconnectFn>(vtable[1]);
    const auto submit = reinterpret_cast<SubmitFn>(vtable[2]);
    const auto pop_message = reinterpret_cast<PopMessageFn>(vtable[3]);
    const auto release = reinterpret_cast<ReleaseFn>(vtable[5]);

    const int32_t connect_result = connect(acp, 16, 16);
#if defined(_WIN32)
    CHECK(connect_result == 0 || connect_result < 0);
#else
    CHECK(connect_result == static_cast<int32_t>(0x80004001u));
#endif
    const int32_t submit_result = submit(acp, 1, 7, 0, nullptr, 0);
    CHECK(submit_result ==
          (connect_result >= 0 ? static_cast<int32_t>(0x80004001u)
                               : static_cast<int32_t>(0x80070006u)));

    CHECK(disconnect(acp) == 0);
    uint8_t message[24];
    std::memset(message, 0xCC, sizeof(message));
    CHECK(pop_message(acp, message) == 1);
    uint32_t message_type = 0;
    std::memcpy(&message_type, message, sizeof(message_type));
    CHECK(message_type == 0x80);
    CHECK(pop_message(acp, message) == 0);
    CHECK(release(acp) == 0);

    void* virtual_address = nullptr;
    uint32_t physical_address = 0;
    CHECK(ApuAlloc(&virtual_address, &physical_address, 128, 64, 0) == 0);
    CHECK(virtual_address != nullptr && physical_address != 0);
    CHECK(ApuIsVirtualAddressValid(virtual_address, 128) == 1);
    CHECK(ApuMapVirtualAddress(virtual_address) == physical_address);
    CHECK(ApuMapApuAddress(physical_address) == virtual_address);
    CHECK(ApuFree(virtual_address) == 0);
    CHECK(ApuIsVirtualAddressValid(virtual_address, 1) == 0);

    std::cout << "[test_acphal] ACP/XAudio2 bridge and APU memory checks passed.\n";
    return 0;
}
