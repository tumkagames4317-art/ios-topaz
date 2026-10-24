#pragma once
#include <stdint.h>

void test_ufs(void);

/*
 * Raw block access, exported for the persistent log (ufslog.c). ufs_ready()
 * reports whether the host took our transfer list, so the log can stay silent
 * instead of faulting when UFS never came up.
 */
int ufs_ready(void);
uint32_t write_blocks(uint32_t lba, uint32_t count, const void *buf);
uint32_t ufs_write_blocks(uint32_t lba, uint32_t count, const void *buf);
uint32_t read_blocks_pub(uint32_t lba, uint32_t count, void *buf);
