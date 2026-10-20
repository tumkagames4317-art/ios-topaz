/*
 * Host unit test for fdt_memory_high (4G vs 6G upper banks).
 *   gcc -O1 -Wall -o out/fdt_mem_test fdt.c fdt_mem_test.c && ./out/fdt_mem_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_END		9

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void align_pad(uint8_t *buf, size_t *len)
{
	while (*len & 3)
		buf[(*len)++] = 0;
}

/* Minimal FDT: root #address-cells=2 #size-cells=2, /memory with three banks. */
static size_t build_fdt(uint8_t *buf, size_t cap, uint64_t high_base, uint64_t high_size)
{
	uint8_t structb[512], strings[64];
	size_t so = 0, st = 0;
	uint32_t off_addr, off_size, off_reg;
	const size_t hdr = 40;

	off_addr = (uint32_t)st;
	memcpy(strings + st, "#address-cells", 15);
	st += 15;
	off_size = (uint32_t)st;
	memcpy(strings + st, "#size-cells", 12);
	st += 12;
	off_reg = (uint32_t)st;
	memcpy(strings + st, "reg", 4);
	st += 4;

#define EMIT32(v) do { put_be32(structb + so, (uint32_t)(v)); so += 4; } while (0)
#define EMITSTR(s) do { \
		size_t n = strlen(s) + 1; \
		memcpy(structb + so, s, n); \
		so += n; \
		align_pad(structb, &so); \
	} while (0)

	EMIT32(FDT_BEGIN_NODE);
	EMITSTR("");
	EMIT32(FDT_PROP); EMIT32(4); EMIT32(off_addr); EMIT32(2);
	EMIT32(FDT_PROP); EMIT32(4); EMIT32(off_size); EMIT32(2);
	EMIT32(FDT_BEGIN_NODE);
	EMITSTR("memory");
	EMIT32(FDT_PROP);
	EMIT32(48);
	EMIT32(off_reg);
	EMIT32(0); EMIT32(0x40000000); EMIT32(0); EMIT32(0x3bb00000);
	EMIT32(0); EMIT32(0x80000000); EMIT32(0); EMIT32(0xc0000000);
	EMIT32((uint32_t)(high_base >> 32)); EMIT32((uint32_t)high_base);
	EMIT32((uint32_t)(high_size >> 32)); EMIT32((uint32_t)high_size);
	EMIT32(FDT_END_NODE);
	EMIT32(FDT_END_NODE);
	EMIT32(FDT_END);
#undef EMIT32
#undef EMITSTR

	if (hdr + so + st > cap)
		return 0;
	memset(buf, 0, hdr + so + st);
	put_be32(buf + 0, FDT_MAGIC);
	put_be32(buf + 4, (uint32_t)(hdr + so + st));
	put_be32(buf + 8, (uint32_t)hdr);			/* off_dt_struct */
	put_be32(buf + 12, (uint32_t)(hdr + so));		/* off_dt_strings */
	put_be32(buf + 16, (uint32_t)hdr);			/* unused rsv */
	put_be32(buf + 20, 17);
	put_be32(buf + 24, 16);
	put_be32(buf + 32, (uint32_t)st);
	put_be32(buf + 36, (uint32_t)so);
	memcpy(buf + hdr, structb, so);
	memcpy(buf + hdr + so, strings, st);
	return hdr + so + st;
}

static int expect(const char *name, uint64_t base, uint64_t size)
{
	uint8_t buf[1024];
	uint64_t b = 0, s = 0;

	if (!build_fdt(buf, sizeof(buf), base, size)) {
		fprintf(stderr, "%s: build failed\n", name);
		return 1;
	}
	if (!fdt_memory_high((uintptr_t)buf, &b, &s)) {
		fprintf(stderr, "%s: fdt_memory_high failed\n", name);
		return 1;
	}
	if (b != base || s != size) {
		fprintf(stderr, "%s: got base=0x%llx size=0x%llx want 0x%llx 0x%llx\n",
			name, (unsigned long long)b, (unsigned long long)s,
			(unsigned long long)base, (unsigned long long)size);
		return 1;
	}
	printf("ok %s base=0x%llx size=0x%llx\n", name,
	       (unsigned long long)b, (unsigned long long)s);
	return 0;
}

int main(void)
{
	uint64_t b = 0, s = 0;
	int fail = 0;

	fail |= expect("4G", 0x140000000ULL, 0x100000000ULL);
	fail |= expect("6G", 0x140000000ULL, 0x180000000ULL);
	if (fdt_memory_high(0, &b, &s)) {
		fprintf(stderr, "null fdt should fail\n");
		fail = 1;
	}
	return fail ? 1 : 0;
}
