#pragma once

#include <cstddef>
#include <cstdint>

namespace scarlettx {

// Installs a process-wide vectored exception handler (first in the chain)
// that emulates the console-only x86-64 instructions a retail Xbox Series X
// PE64 image executes natively in-process, which a Win32 user-mode process
// cannot run:
//
//   - wbinvd / invd  (0F 08/09): cache maintenance — skipped
//   - rdmsr / wrmsr  (0F 30/32): model-specific registers — reads report 0
//   - cli / sti / hlt (FA/FB/F4): interrupt control — skipped
//   - syscall        (0F 05): dispatched through the console syscall table
//     (see RegisterXboxSyscallHandler / LoadXboxSyscallMapFromString); an
//     unbound number logs RAX and RCX/RDX/R8/R9 and returns
//     STATUS_NOT_IMPLEMENTED so the title's user-mode fallback path runs
//
// `syscall` needs help: on Windows it executes instead of faulting (it is
// the user->kernel transition of the host itself), so a guest syscall would
// reach the Windows kernel with an Xbox syscall number. PatchSyscallSites()
// rewrites `0F 05` to `0F 0B` (ud2) in mapped guest code and records the
// site; the interceptor then dispatches the syscall when the ud2 faults.
// MappedPeImage patches executable sections automatically.
//
// Console TEB/GS: GameOS is a Windows-derived kernel and its x64 TEB keeps
// the desktop Windows layout, so `gs:[...]` reads execute natively and need
// no interception (there is nothing to trap: they do not fault). What differs
// are the syscall numbers and a few kernel structures, which is why the
// dispatch table exists at all.
//
// cpuid is deliberately NOT intercepted: it executes natively on the host
// and console-specific leafs simply return host/hypervisor data.
//
// Any other faulting instruction is passed on unchanged (the handler is a
// filter, not a blanket swallow), so genuine guest bugs still surface as
// crashes. Scope the interceptor around guest code execution; the destructor
// removes the handler.
class ScopedVehInterceptor {
public:
    ScopedVehInterceptor();
    ~ScopedVehInterceptor();

    ScopedVehInterceptor(const ScopedVehInterceptor&) = delete;
    ScopedVehInterceptor& operator=(const ScopedVehInterceptor&) = delete;

    bool installed() const noexcept { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;
};

// Rewrites every `syscall` (0F 05) in [code, code+size) to `ud2` (0F 0B) and
// registers the site with the interceptor. The pages must be writable at
// call time (MappedPeImage patches before applying section protections).
// Returns the number of sites patched.
size_t PatchSyscallSites(uint8_t* code, size_t size);

// Forgets patched syscall sites in [base, base+size); call when the guest
// image that owns them is unmapped.
void UnregisterPatchedSyscalls(const uint8_t* base, size_t size);

// --- Direct syscall dispatch --------------------------------------------
//
// A patched syscall site executes with the console's register state: the
// syscall number in RAX (set by the guest stub's `mov eax, imm32`) and the
// Windows-x64 argument registers RCX, RDX, R8, R9 (+ stack at [Rsp+0x28).
// A handler receives a pointer to the Win32 CONTEXT that raised the fault
// (passed as void* so this header stays independent of <windows.h>) and
// returns the NTSTATUS to place in RAX before execution resumes after the
// ud2.
using XboxSyscallHandler = long (*)(void* context);

// Binds a console syscall number to a handler. Name is used for logging.
// Returns false if the number is already bound.
bool RegisterXboxSyscallHandler(uint32_t number, const char* name,
                                 XboxSyscallHandler handler);

// Binds a comma-separated "number=Name" list, e.g.
//   SCARLETTX_SYSCALL_MAP=0x1a=NtAllocateVirtualMemory,0x2b=NtClose
// to the built-in handler implementations (NtAllocateVirtualMemory,
// NtFreeVirtualMemory, NtProtectVirtualMemory, NtQuerySystemInformation,
// NtDelayExecution, NtSetEvent, NtWaitForSingleObject,
// NtQueryPerformanceCounter, NtClose, NtCreateFile, NtReadFile).
// Console syscall numbers are not public, so the practical workflow is:
// boot once, read the exact numbers the log records for unknown calls, then
// bind them. Returns the number of entries bound.
size_t LoadXboxSyscallMapFromString(const char* mapping);

// Total number of instructions emulated by any ScopedVehInterceptor in this
// process. Intended for tests and diagnostics.
uint64_t VehEmulatedInstructionCount();

}  // namespace scarlettx
