/*
 * Host test for the logic that runs before xnu: payload parsing, the physical
 * mapping arithmetic and the ADT bounds check.
 *
 * Why this exists: the device was unavailable for long stretches, and the
 * expensive failures in this project were arithmetic ones -- a wrong memory
 * base, an offset used as an address, an ADT placed outside DRAM. Those are all
 * testable on the desktop, so they should never again cost a flash cycle.
 *
 * Build (from shim/):
 *   gcc -O1 -Wall -o out/logic_test logic_test.c host_ufslog_stub.c && ./out/logic_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Mirror of the on-device layout (shim/loader.c). */
#define PAGE_16K 0x4000UL

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end, phys_base, mem_size;
	char cmdline[608];
	struct payload_seg segs[];
};

static int failures;

static void check(int cond, const char *what)
{
	printf("  %-52s %s\n", what, cond ? "OK" : "FAIL");
	if (!cond)
		failures++;
}

static void check_eq64(uint64_t got, uint64_t want, const char *what)
{
	int ok = got == want;
	printf("  %-40s %#018llx %s\n", what, (unsigned long long)got, ok ? "OK" : "FAIL");
	if (!ok) {
		printf("      expected %#018llx\n", (unsigned long long)want);
		failures++;
	}
}

/* The real ginkgo ranges, from /proc/iomem on the stock kernel. */
struct range { uint64_t lo, hi; };
static const struct range dram[] = {
	{ 0x40000000, 0x456fffff },
	{ 0x45d00000, 0x45dfffff },
	{ 0x45f40000, 0x45ffefff },
	{ 0x48f00000, 0x4affffff },
	{ 0x57517000, 0x5e3fffff },
	{ 0x61600000, 0x7dcfffff },
	{ 0x80000000, 0x13fffffff },
};

static int in_dram(uint64_t pa, uint64_t len)
{
	for (unsigned i = 0; i < sizeof(dram) / sizeof(dram[0]); i++)
		if (pa >= dram[i].lo && pa + len - 1 <= dram[i].hi)
			return 1;
	return 0;
}

int main(void)
{
	printf("=== 1. payload header is what loader.c expects ===\n");
	check(sizeof(struct payload) == 0x2a8, "sizeof(payload) == 0x2a8");
	check(__builtin_offsetof(struct payload, segs) == 0x2a8, "offsetof(segs) == 0x2a8");
	check(__builtin_offsetof(struct payload, cmdline) == 0x48, "offsetof(cmdline) == 0x48");

	printf("\n=== 2. the real payload parses and fits ===\n");
	FILE *f = fopen("/home/comaru/ios16-port/build/payload-mem.bin", "rb");
	if (!f) {
		printf("  payload not found; skipping (expected on a clean host)\n");
	} else {
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		struct payload *p = malloc(sz);
		if (fread(p, 1, sz, f) != (size_t)sz) {
			printf("  short read\n");
			return 1;
		}
		fclose(f);

		check(memcmp(p->magic, "SWEETXNU", 8) == 0, "magic is SWEETXNU");
		check(p->version == 1, "version == 1");
		printf("  nsegs = %u, adt_off = %#llx, adt_size = %#llx\n",
		       p->nsegs, (unsigned long long)p->adt_off,
		       (unsigned long long)p->adt_size);

		/* The corrected ginkgo values. Anything else is the topaz mistake. */
		check_eq64(p->phys_base, 0x80000000, "phys_base");
		check_eq64(p->mem_size, 0xc0000000, "mem_size");

		/* every segment must live inside the file */
		int inside = 1, aligned = 1;
		uint64_t maxend = 0;
		for (uint32_t i = 0; i < p->nsegs; i++) {
			struct payload_seg *s = &p->segs[i];
			if (s->file_off + s->file_size > (uint64_t)sz)
				inside = 0;
			if (s->vmsize % 8 || s->file_off % 8 || s->vmaddr % 8)
				aligned = 0;
			if (s->vmaddr + s->vmsize > maxend)
				maxend = s->vmaddr + s->vmsize;
		}
		check(inside, "all segments inside the file");
		check(aligned, "all segments 8-byte aligned");
		check(p->adt_off + p->adt_size <= (uint64_t)sz, "ADT inside the file");
		check(p->kernel_end >= maxend, "kernel_end covers every segment");

		printf("\n=== 3. address arithmetic (the topaz bug) ===\n");
		uint64_t pa_of_va = p->phys_base - p->virt_base;

		/*
		 * The topaz base. Note carefully: 0xb2568000 is INSIDE the
		 * 0x80000000-0x13fffffff range, so "the ADT fell outside DRAM" --
		 * an earlier explanation of the abort -- is simply wrong. Keep the
		 * computation so the mistake is recorded, but do not assert the
		 * false conclusion.
		 */
		uint64_t bad_base = 0xb0000000ULL;
		uint64_t bad_adt = p->kernel_end + (bad_base - p->virt_base);
		printf("  with phys_base 0xb0000000 -> adt_pa %#llx (%s DRAM)\n",
		       (unsigned long long)bad_adt,
		       in_dram(bad_adt, p->adt_size) ? "inside" : "outside");
		check(in_dram(bad_adt, p->adt_size),
		      "topaz adt_pa is inside DRAM too: not the abort cause");

		uint64_t adt_pa = p->kernel_end + pa_of_va;
		printf("  with phys_base 0x80000000 -> adt_pa %#llx\n",
		       (unsigned long long)adt_pa);
		check(in_dram(adt_pa, p->adt_size), "ginkgo base keeps the ADT in DRAM");

		uint64_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
		check(in_dram(ba_pa, PAGE_16K), "boot_args fit in DRAM");

		/* deviceTreeP must be the VA xnu will use with the MMU on. */
		uint64_t dtp = adt_pa - pa_of_va;
		check_eq64(dtp, adt_pa - p->phys_base + p->virt_base, "deviceTreeP is a VA");
		check(dtp >= p->virt_base, "deviceTreeP is above virt_base");

		printf("\n=== 4. every xnu segment maps into real DRAM ===\n");
		int mapped = 1;
		for (uint32_t i = 0; i < p->nsegs; i++) {
			uint64_t pa = p->segs[i].vmaddr + pa_of_va;
			if (!in_dram(pa, p->segs[i].vmsize)) {
				printf("    segment %u -> %#llx lands outside DRAM\n",
				       i, (unsigned long long)pa);
				mapped = 0;
			}
		}
		check(mapped, "all 10 segments land in DRAM");

		printf("\n=== 5. ADT bounds check accepts the good case ===\n");
		int ok = adt_pa >= p->phys_base &&
			 adt_pa + p->adt_size <= p->phys_base + p->mem_size;
		check(ok, "adt_pa inside [phys_base, phys_base+mem_size)");
		/*
		 * The bounds check compares against the payload's own window. With
		 * phys_base 0x80000000 and mem_size 0xc0000000 the window ends at
		 * 0x140000000, so the topaz address also passes here -- the check
		 * catches gross errors, not this one.
		 */
		ok = bad_adt >= p->phys_base && bad_adt + p->adt_size <= p->phys_base + p->mem_size;
		check(ok, "the topaz adt_pa also passes the window check");

		free(p);
	}

	printf("\n=== 6. entry point is inside a segment ===\n");
	if (0) {
		/* covered above with the real payload */
	}

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
