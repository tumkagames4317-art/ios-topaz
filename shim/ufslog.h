#pragma once
#include <stdint.h>

/*
 * Persistent console log on UFS. See ufslog.c for why this exists: it makes
 * shim debugging fully autonomous, because a later recovery boot can read the
 * text with dd instead of the user photographing a half-second panic screen.
 */
void ufslog_init(void);
int ufslog_active(void);
void ufslog_write(const char *s);
void ufslog_str(const char *label, const char *s);
void ufslog_hex(const char *label, uint64_t v);
void ufslog_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
void ufslog_flush(void);
void ufslog_panic(uint64_t slot, const char *stage, uint64_t esr, uint64_t elr,
		  uint64_t far, uint64_t spsr, uint64_t x0, uint64_t x1, uint64_t x30);
