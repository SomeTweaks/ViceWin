#include <cstdint>

extern "C" {
__declspec(dllimport) uint64_t KernelxMissingForTest(uint64_t value);

__declspec(dllexport) uint64_t ProbeMissingKernelxImport() {
    return KernelxMissingForTest(1);
}
}
