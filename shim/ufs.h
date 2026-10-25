#pragma once
#include <stdint.h>

void test_ufs(void);

/*
 * ginkgo has no UFS; it boots from eMMC behind an SDHCI-v5 controller
 * (sdhci@4744000). emmc.c provides the same block interface as ufs.c so the
 * persistent log works on this phone. Only one of the two may be linked.
 */
void test_emmc(void);
int emmc_ok(void);

/*
 * Raw block access, exported for the persistent log (ufslog.c). ufs_ready()
 * reports whether the host took our transfer list, so the log can stay silent
 * instead of faulting when UFS never came up.
 */
int ufs_ready(void);
uint32_t write_blocks(uint32_t lba, uint32_t count, const void *buf);
uint32_t ufs_write_blocks(uint32_t lba, uint32_t count, const void *buf);
uint32_t read_blocks_pub(uint32_t lba, uint32_t count, void *buf);
