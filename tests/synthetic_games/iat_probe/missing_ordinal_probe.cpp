#include <cstdint>

extern "C" {
__declspec(dllimport) uint64_t KernelxOrdinalMissing(uint64_t value);

__declspec(dllexport) uint64_t ProbeMissingOrdinalImport() {
    return KernelxOrdinalMissing(1);
}
}
