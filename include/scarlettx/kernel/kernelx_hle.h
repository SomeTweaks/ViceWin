#pragma once

#include <cstddef>
#include <cstdint>

#include "scarlettx/kernel/xmem_abi.h"

namespace scarlettx {

class HleSymbolRegistry;

namespace kernelx {

void* XMemAlloc(size_t size, uint64_t attributes);
void XMemFree(void* address, uint64_t attributes);
void* XMemSet(void* destination, int value, size_t size);
void* XMemCpy(void* destination, const void* source, size_t size);
// Physical-page and mapping surface (xmem.dll). Win32 has no physically
// contiguous allocations, so "physical" pages are 64 KiB-aligned commits and
// mapping them is the identity: the guest receives the same VA it was given.
void* XMemAllocatePhysicalPages(size_t size, size_t alignment);
void XMemFreePhysicalPages(void* address, size_t size);
void* XMemMapPhysicalPages(void* physical_address_base, size_t virtual_size);
void* XMemMapPhysicalPagesScatter(void** physical_page_array, size_t number_of_pages,
                                  size_t virtual_size);
void XMemTransferMemory(void* destination, const void* source, size_t size);
void* XMemVirtualAlloc(void* address, size_t size, uint32_t allocation_type,
                       uint32_t protect, uint64_t xmem_flags);
// Measured ABI (see Leonida's xmem measurement for 1.0.2.1): ECX carries an
// opaque kind (0x20000001 at every observed call site), RDX points at the
// output; the caller reads a 64-bit working-set size at +8 and treats 0 as "no
// data". Returns TRUE/FALSE.
int32_t XMemGetWorkingSetStatistics(uint32_t kind, void* statistics);
bool XMemIsToolsMemAvailable();
int32_t XMemGetAllocationStatistics(
    uint32_t memory_type,
    int32_t alternate_pool,
    XMemAllocationStatistics* statistics);

void* VirtualAllocX(void* address,
                    size_t size,
                    uint32_t allocation_type,
                    uint64_t xmem_flags,
                    uint32_t protection);
void* MapTitleMemory(void* file_handle,
                     uint64_t offset,
                     size_t size,
                     void* base_address,
                     uint32_t protection);
bool UnmapTitleMemory(void* address);

uintptr_t MapXboxThreadMaskToHost(uint64_t xbox_mask,
                                 uintptr_t host_available_mask) noexcept;
uint32_t MapXboxProcessorIndexToHost(uint32_t xbox_index,
                                     uintptr_t host_available_mask) noexcept;

void RegisterKernelxMemorySymbols(HleSymbolRegistry& registry);
void RegisterKernelxThreadingSymbols(HleSymbolRegistry& registry);

}  // namespace kernelx
}  // namespace scarlettx
