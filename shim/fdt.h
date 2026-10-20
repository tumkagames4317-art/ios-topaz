#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/*
 * Highest DRAM bank that starts at or above 4 GiB (topaz XNU highmem window).
 * Parses every /memory or /memory@* reg using the root #address-cells /
 * #size-cells (defaults 2/1 if omitted). Returns 1 and writes *base/*size
 * on success; 0 if the FDT is missing or has no usable high bank.
 */
int fdt_memory_high(uintptr_t fdt, uint64_t *base, uint64_t *size);
