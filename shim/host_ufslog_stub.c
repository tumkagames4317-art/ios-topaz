/*
 * Host-side stub for the UFS log, so console.c can be linked into the desktop
 * tests. The real ufslog.c talks to hardware; on the host we only need the
 * symbols to exist.
 */
#include "ufslog.h"
#include <stdio.h>

static int enabled;

void ufslog_init(void) { enabled = 1; }
int ufslog_active(void) { return enabled; }
void ufslog_write(const char *s) { if (enabled) fputs(s, stdout); }
void ufslog_str(const char *label, const char *s)
{
	if (enabled) printf("%s %s\n", label, s ? s : "(null)");
}
void ufslog_hex(const char *label, uint64_t v)
{
	if (enabled) printf("%s %016llx\n", label, (unsigned long long)v);
}
void ufslog_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	if (!enabled) return;
	printf("%s", label);
	if (a) printf(" %s=%08x", a, va);
	if (b) printf(" %s=%08x", b, vb);
	printf("\n");
}
void ufslog_flush(void) { fflush(stdout); }
void ufslog_panic(uint64_t slot, const char *stage, uint64_t esr, uint64_t elr,
		  uint64_t far, uint64_t spsr, uint64_t x0, uint64_t x1, uint64_t x30)
{
	printf("PANIC slot=%llx stage=%s esr=%llx elr=%llx far=%llx\n",
	       (unsigned long long)slot, stage, (unsigned long long)esr,
	       (unsigned long long)elr, (unsigned long long)far);
}
