#include "sx_check.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <vector>

#include "scarlettx/core/veh_interceptor.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(_WIN32)

namespace {

using VoidFn = void (*)();
using WordFn = uint64_t (*)();

// Tiny x86-64 encoder for the stubs the tests execute. Only the handful of
// forms needed below are implemented.
struct CodeBuilder {
    std::vector<uint8_t> bytes;

    void Raw(std::initializer_list<uint8_t> values) {
        bytes.insert(bytes.end(), values.begin(), values.end());
    }
    void U8(uint8_t value) { bytes.push_back(value); }
    void U32(uint32_t value) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
    void U64(uint64_t value) {
        for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }

    void MovEax(uint32_t value) { pending_eax = value; has_pending_eax = true; }
    void MovRcx(uint64_t value) { Raw({0x48, 0xB9}); U64(value); }
    void MovRdx(uint64_t value) { Raw({0x48, 0xBA}); U64(value); }
    void MovR8(uint64_t value) { Raw({0x49, 0xB8}); U64(value); }
    void MovR9(uint64_t value) { Raw({0x49, 0xB9}); U64(value); }
    void SubRsp(uint8_t amount) { Raw({0x48, 0x83, 0xEC}); U8(amount); }
    void AddRsp(uint8_t amount) { Raw({0x48, 0x83, 0xC4}); U8(amount); }
    // mov qword ptr [rsp + disp8], imm32
    void StoreStack(uint8_t disp, uint32_t value) {
        Raw({0x48, 0xC7, 0x44, 0x24});
        U8(disp);
        U32(value);
    }
    // A console syscall stub is `mov eax, imm32; syscall`; the patcher now
    // requires that shape, so the number is emitted next to the syscall.
    void Syscall() {
        if (has_pending_eax) {
            Raw({0xB8});
            U32(pending_eax);
            has_pending_eax = false;
        }
        Raw({0x0F, 0x05});
    }
    void Ret() { Raw({0xC3}); }

    uint32_t pending_eax = 0;
    bool has_pending_eax = false;
};

// Emits the stub into a fresh RWX page, patches its syscall sites to ud2 (the
// same rewrite MappedPeImage applies to guest images) and returns the entry
// address.
void* MakeStub(const CodeBuilder& builder) {
    void* memory = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                                PAGE_EXECUTE_READWRITE);
    SX_REQUIRE(memory != nullptr);
    std::memcpy(memory, builder.bytes.data(), builder.bytes.size());
    SX_REQUIRE(scarlettx::PatchSyscallSites(static_cast<uint8_t*>(memory),
                                            builder.bytes.size()) >= 1);
    return memory;
}

// Executes `bytes` followed by `ret` without any syscall patching.
void* MakeExecutable(std::initializer_list<uint8_t> bytes) {
    void* memory = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                                PAGE_EXECUTE_READWRITE);
    SX_REQUIRE(memory != nullptr);
    auto* code = static_cast<uint8_t*>(memory);
    size_t i = 0;
    for (const uint8_t byte : bytes) {
        code[i++] = byte;
    }
    code[i] = 0xC3;
    return memory;
}

// MSVC forbids __try in functions that need object unwinding, so the SEH
// probe lives in its own function without locals that have destructors.
bool RawPrivilegedInstructionFaults() {
    void* code = MakeExecutable({0x0F, 0x09});  // wbinvd
    __try {
        reinterpret_cast<VoidFn>(code)();
        return false;
    } __except (GetExceptionCode() == EXCEPTION_PRIV_INSTRUCTION ||
                        GetExceptionCode() == EXCEPTION_ILLEGAL_INSTRUCTION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
}

// --- custom handler used by the registration test -------------------------
struct CustomCall {
    uint64_t count = 0;
    uint64_t rcx = 0;
    uint64_t rdx = 0;
    uint64_t r8 = 0;
    uint64_t r9 = 0;
};

CustomCall g_custom;

long CustomSyscallHandler(void* context) {
    auto* registers = static_cast<CONTEXT*>(context);
    ++g_custom.count;
    g_custom.rcx = registers->Rcx;
    g_custom.rdx = registers->Rdx;
    g_custom.r8 = registers->R8;
    g_custom.r9 = registers->R9;
    return 0x12345678L;
}

constexpr uint32_t kStatusNotImplemented = 0xC0000002u;
constexpr uint32_t kStatusInvalidHandle = 0xC0000008u;

uint64_t Low32(uint64_t value) { return value & 0xFFFFFFFFull; }

}  // namespace

#endif  // defined(_WIN32)

int main() {
#if defined(_WIN32)
    const uint64_t before = scarlettx::VehEmulatedInstructionCount();

    // ---- 1. Privileged console instructions ------------------------------
    {
        scarlettx::ScopedVehInterceptor veh;
        SX_REQUIRE(veh.installed());

        reinterpret_cast<VoidFn>(MakeExecutable({0x0F, 0x09}))();  // wbinvd
        reinterpret_cast<VoidFn>(MakeExecutable({0x0F, 0x08}))();  // invd
        reinterpret_cast<VoidFn>(MakeExecutable({0xFA}))();        // cli
        reinterpret_cast<VoidFn>(MakeExecutable({0xFB}))();        // sti
        reinterpret_cast<VoidFn>(MakeExecutable({0xF4}))();        // hlt

        // rdmsr: mov ecx, 0x34; rdmsr -> EAX:EDX must come back zeroed.
        SX_REQUIRE(reinterpret_cast<WordFn>(MakeExecutable(
            {0xB9, 0x34, 0x00, 0x00, 0x00, 0x0F, 0x32}))() == 0);

        // wrmsr: writes are discarded, execution continues.
        reinterpret_cast<VoidFn>(MakeExecutable(
            {0xB9, 0x34, 0x00, 0x00, 0x00, 0x0F, 0x30}))();

        // An unbound syscall number (no handler registered) reports
        // STATUS_NOT_IMPLEMENTED.
        {
            CodeBuilder stub;
            stub.MovEax(0x7E57u);
            stub.MovRcx(1);
            stub.MovRdx(2);
            stub.MovR8(3);
            stub.MovR9(4);
            stub.Syscall();
            stub.Ret();
            SX_REQUIRE(Low32(reinterpret_cast<WordFn>(MakeStub(stub))()) ==
                       kStatusNotImplemented);
        }
    }
    SX_REQUIRE(scarlettx::VehEmulatedInstructionCount() - before == 8);

    // ---- 2. Syscall dispatch ---------------------------------------------
    const uint64_t dispatch_before = scarlettx::VehEmulatedInstructionCount();
    {
        scarlettx::ScopedVehInterceptor veh;

        // 2a. Custom handler: registration, duplicate rejection, argument
        //     passing (RCX/RDX/R8/R9) and the NTSTATUS return in RAX.
        SX_REQUIRE(scarlettx::RegisterXboxSyscallHandler(
            0x4321u, "TestCustomHandler", &CustomSyscallHandler));
        SX_REQUIRE(!scarlettx::RegisterXboxSyscallHandler(
            0x4321u, "TestCustomHandlerAgain", &CustomSyscallHandler));
        {
            CodeBuilder stub;
            stub.MovEax(0x4321u);
            stub.MovRcx(0xA1u);
            stub.MovRdx(0xB2u);
            stub.MovR8(0xC3u);
            stub.MovR9(0xD4u);
            stub.Syscall();
            stub.Ret();
            SX_REQUIRE(reinterpret_cast<WordFn>(MakeStub(stub))() == 0x12345678ull);
            SX_REQUIRE(g_custom.count == 1);
            SX_REQUIRE(g_custom.rcx == 0xA1ull);
            SX_REQUIRE(g_custom.rdx == 0xB2ull);
            SX_REQUIRE(g_custom.r8 == 0xC3ull);
            SX_REQUIRE(g_custom.r9 == 0xD4ull);
        }

        // 2b. SCARLETTX_SYSCALL_MAP style binding through the parser: one
        //     valid entry, one unknown handler name, one malformed entry.
        SX_REQUIRE(scarlettx::LoadXboxSyscallMapFromString(
                       " 0x7801=NtClose , 0x7802=NoSuchHandler , garbage ") == 1);

        // NtClose on an invalid handle must report STATUS_INVALID_HANDLE.
        {
            CodeBuilder stub;
            stub.MovEax(0x7801u);
            stub.MovRcx(0x1234u);
            stub.Syscall();
            stub.Ret();
            SX_REQUIRE(Low32(reinterpret_cast<WordFn>(MakeStub(stub))()) ==
                       kStatusInvalidHandle);
        }

        // 2c. NtQueryPerformanceCounter through the table: success, and both
        //     out-parameters written.
        SX_REQUIRE(scarlettx::LoadXboxSyscallMapFromString(
                       "0x7803=NtQueryPerformanceCounter") == 1);
        {
            LARGE_INTEGER counter{};
            LARGE_INTEGER frequency{};
            CodeBuilder stub;
            stub.MovEax(0x7803u);
            stub.MovRcx(reinterpret_cast<uint64_t>(&counter));
            stub.MovRdx(reinterpret_cast<uint64_t>(&frequency));
            stub.Syscall();
            stub.Ret();
            SX_REQUIRE(reinterpret_cast<WordFn>(MakeStub(stub))() == 0);
            SX_REQUIRE(counter.QuadPart != 0);
            SX_REQUIRE(frequency.QuadPart != 0);
        }

        // 2d. NtAllocateVirtualMemory with the console MEM_64K_PAGES flag:
        //     0x80000 must be consumed (not passed to VirtualAlloc as
        //     MEM_RESET) and the region rounded to 64 KiB. Then
        //     NtFreeVirtualMemory with MEM_RELEASE.
        SX_REQUIRE(scarlettx::LoadXboxSyscallMapFromString(
                       "0x7804=NtAllocateVirtualMemory,0x7805=NtFreeVirtualMemory") == 2);
        {
            void* base = nullptr;
            SIZE_T size = 0x1000;
            CodeBuilder allocate;
            allocate.MovEax(0x7804u);
            allocate.SubRsp(0x38);
            allocate.StoreStack(0x28, 0x00082000u);  // MEM_RESERVE | MEM_64K_PAGES
            allocate.StoreStack(0x30, PAGE_READWRITE);
            allocate.MovRcx(static_cast<uint64_t>(-1));  // current process
            allocate.MovRdx(reinterpret_cast<uint64_t>(&base));
            allocate.MovR9(reinterpret_cast<uint64_t>(&size));  // RegionSize
            allocate.Syscall();
            allocate.AddRsp(0x38);
            allocate.Ret();
            SX_REQUIRE(reinterpret_cast<WordFn>(MakeStub(allocate))() == 0);
            SX_REQUIRE(base != nullptr);
            SX_REQUIRE(size == 0x10000ull);

            CodeBuilder free_memory;
            free_memory.MovEax(0x7805u);
            free_memory.MovRcx(0);
            free_memory.MovRdx(reinterpret_cast<uint64_t>(&base));
            free_memory.MovR8(reinterpret_cast<uint64_t>(&size));
            free_memory.MovR9(0x8000u);  // MEM_RELEASE
            free_memory.Syscall();
            free_memory.Ret();
            SX_REQUIRE(reinterpret_cast<WordFn>(MakeStub(free_memory))() == 0);
            SX_REQUIRE(base == nullptr);
        }

        // 2e. An unknown number still reports STATUS_NOT_IMPLEMENTED and logs
        //     its RAX/RCX/RDX/R8/R9 (the log line is asserted by the smoke
        //     boot tests; here only the status contract matters).
        {
            CodeBuilder stub;
            stub.MovEax(0xDEAD1234u);
            stub.MovRcx(0x11u);
            stub.MovRdx(0x22u);
            stub.MovR8(0x33u);
            stub.MovR9(0x44u);
            stub.Syscall();
            stub.Ret();
            SX_REQUIRE(Low32(reinterpret_cast<WordFn>(MakeStub(stub))()) ==
                       kStatusNotImplemented);
        }
    }
    SX_REQUIRE(scarlettx::VehEmulatedInstructionCount() - dispatch_before == 6);

    // ---- 3. The handler is a scoped filter -------------------------------
    // Without the interceptor the same instruction faults for real.
    SX_REQUIRE(RawPrivilegedInstructionFaults());

    std::cout << "[test_veh_interceptor] PASSED (emulated "
              << scarlettx::VehEmulatedInstructionCount() << " instructions)\n";
    return 0;
#else
    std::cout << "[test_veh_interceptor] SKIPPED (non-Windows)\n";
    return 0;
#endif
}
