/*
 * Milestone 4: act as iBoot for xnu.
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT, so this only has to: find the payload in the ABL ramdisk, copy the
 * segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump to the entry point with the MMU
 * and caches off, x0 = boot_args (physical).
 *
 * phys_base/mem_size come from the payload (mkadt --highmem) but are
 * overridden from ABL's /memory when a high (>=4 GiB) bank is present, so
 * one image covers the 4 GiB and 6 GiB topaz SKUs.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

static int name_eq32(const volatile char *p, const char *want)
{
	for (int i = 0; i < 32; i++) {
		char c = p[i];
		if (c != want[i])
			return 0;
		if (!c)
			return 1;
	}
	return 0;
}

/* Advance past one Apple ADT node; return 0 on overflow. */
static uintptr_t adt_skip_node(uintptr_t p, uintptr_t end)
{
	uint32_t i, nprops, nchildren;

	if (p + 8 > end)
		return 0;
	nprops = *(volatile uint32_t *)p;
	nchildren = *(volatile uint32_t *)(p + 4);
	p += 8;
	for (i = 0; i < nprops; i++) {
		uint32_t sz;
		if (p + 36 > end)
			return 0;
		sz = *(volatile uint32_t *)(p + 32) & 0x7fffffffu;
		p += 36 + ((sz + 3) & ~3u);
	}
	for (i = 0; i < nchildren; i++) {
		p = adt_skip_node(p, end);
		if (!p)
			return 0;
	}
	return p;
}

/*
 * Patch an 8-byte little-endian property on /chosen in a copied Apple ADT.
 * Returns 1 if the property was found and overwritten in place.
 */
static int adt_patch_chosen_u64(uintptr_t adt, uint32_t adt_size,
				const char *prop, uint64_t value)
{
	uintptr_t end = adt + adt_size;
	uintptr_t p;
	uint32_t i, nprops, nchildren;

	if (adt_size < 8)
		return 0;
	nprops = *(volatile uint32_t *)adt;
	nchildren = *(volatile uint32_t *)(adt + 4);
	p = adt + 8;
	for (i = 0; i < nprops; i++) {
		uint32_t sz;
		if (p + 36 > end)
			return 0;
		sz = *(volatile uint32_t *)(p + 32) & 0x7fffffffu;
		p += 36 + ((sz + 3) & ~3u);
	}
	for (i = 0; i < nchildren; i++) {
		uintptr_t child = p;
		uint32_t cp, cc, j;
		uintptr_t q;
		int is_chosen = 0;

		if (child + 8 > end)
			return 0;
		cp = *(volatile uint32_t *)child;
		cc = *(volatile uint32_t *)(child + 4);
		q = child + 8;
		for (j = 0; j < cp; j++) {
			uint32_t sz;
			if (q + 36 > end)
				return 0;
			sz = *(volatile uint32_t *)(q + 32) & 0x7fffffffu;
			if (name_eq32((volatile char *)q, "name") &&
			    name_eq32((volatile char *)(q + 36), "chosen"))
				is_chosen = 1;
			q += 36 + ((sz + 3) & ~3u);
		}
		if (is_chosen) {
			q = child + 8;
			for (j = 0; j < cp; j++) {
				uint32_t sz;
				if (q + 36 > end)
					return 0;
				sz = *(volatile uint32_t *)(q + 32) & 0x7fffffffu;
				if (name_eq32((volatile char *)q, prop) && sz == 8) {
					*(volatile uint64_t *)(q + 36) = value;
					return 1;
				}
				q += 36 + ((sz + 3) & ~3u);
			}
			return 0;
		}
		(void)cc;
		p = adt_skip_node(child, end);
		if (!p)
			return 0;
	}
	return 0;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

/* build.sh appends the payload to the Image at the first 4K boundary after _end */
extern char _end[];

const char *loader_boot(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;
	uintptr_t emb = ((uintptr_t)_end + 0xfff) & ~0xfffUL;

	/*
	 * topaz: the ABL only passes the vendor_boot ramdisk for a v4 boot
	 * image, so the payload rides behind the shim in the kernel Image.
	 * Its exact size is unknown here; the kernel memory check below only
	 * needs an upper bound, and the Image is far below phys_base.
	 */
	if (magic_ok((const char *)emb)) {
		rd_start = emb;
		rd_end = emb + 0x8000000;
		con_hex("EMBED", emb);
	} else {
		if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
		    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
			return "no initrd in /chosen";
		con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);
	}

	/*
	 * Boot image v4 (topaz ABL ignores v1/v2): ABL concatenates the
	 * vendor_boot ramdisk(s) and ours, so the payload is somewhere inside
	 * the initrd and not necessarily 8-byte aligned. Find it and move it
	 * down to an aligned address; the vendor ramdisk below it is unused.
	 */
	uintptr_t hit = 0;
	for (uintptr_t a = rd_start; a + 8 <= rd_end; a++)
		if (magic_ok((const char *)a)) {
			hit = a;
			break;
		}
	if (!hit)
		return "no SWEETXNU payload";
	if (hit != rd_start) {
		uintptr_t dst = (rd_start + 7) & ~7UL;
		volatile uint8_t *d = (volatile uint8_t *)dst;
		volatile const uint8_t *src = (volatile const uint8_t *)hit;

		con_hex2("PAYLOAD", "at=", hit, "to=", dst);
		for (uint64_t i = 0; i < rd_end - hit; i++)
			d[i] = src[i];
		rd_end = dst + (rd_end - hit);
		rd_start = dst;
	}
	const struct payload *p = (const struct payload *)rd_start;
	if (p->version != 1 || (p->adt_off & 7) || (rd_start & 7))
		return "bad payload header";

	uint64_t phys_base = p->phys_base;
	uint64_t mem_size = p->mem_size;
	uint64_t fdt_base = 0, fdt_size = 0;
	if (fdt_memory_high(dtb, &fdt_base, &fdt_size)) {
		phys_base = fdt_base;
		mem_size = fdt_size;
		con_hex("MEMFDT", fdt_base);
		con_hex("MEMSZ", fdt_size);
	} else {
		con_str("MEMFDT", "using payload");
	}

	uint64_t pa_of_va_base = phys_base - p->virt_base;	/* pa = va + this */
	con_hex2("PHYS", "base=", (uint32_t)phys_base, "size=", (uint32_t)mem_size);
	con_hex("VIRT", p->virt_base);
	con_hex("ENTRY", p->entry_va);

	/* The payload must not overlap where the kernel goes. */
	if (rd_end > phys_base && rd_start < phys_base + mem_size)
		return "ramdisk overlaps kernel memory";

	/* Kernel image + ADT + boot_args must fit in the managed window. */
	if (p->kernel_end - p->virt_base + 2 * PAGE_16K + p->adt_size > mem_size)
		return "kernel does not fit in DRAM bank";

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", p->nsegs, "end=", p->kernel_end + pa_of_va_base);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);
	if (!adt_patch_chosen_u64(adt_pa, (uint32_t)p->adt_size, "dram-base", phys_base) ||
	    !adt_patch_chosen_u64(adt_pa, (uint32_t)p->adt_size, "dram-size", mem_size))
		con_str("ADT", "dram patch failed");
	else
		con_str("ADT", "dram patched");

	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = phys_base;
	ba->memSize = mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = FB_BASE;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = mem_size;
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping (fbcon clears screen)");

	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
