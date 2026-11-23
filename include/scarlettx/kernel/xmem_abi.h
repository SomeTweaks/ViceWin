#pragma once

#include <cstdint>

struct XMemAllocationStatistics {
    uint32_t size;
    uint32_t pad;
    uint64_t allocated_bytes;
    uint64_t cached_bytes;
    uint32_t allocation_count;
    uint32_t released_pages;
};

static_assert(sizeof(XMemAllocationStatistics) == 0x20);
