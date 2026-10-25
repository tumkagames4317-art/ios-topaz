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
#include "ufslog.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#include "fdt.h"
#include "loader.h"
#include "ufs.h"
#include "pmic.h"
#include "autonomy.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

/*
 * 0 = never hand control to xnu; the loader ends every boot in fastboot, which
 * is what makes unattended runs possible. 1 = jump into xnu as before, for
 * when there is a reason to watch it. Override with -DSWEET_XNU_HANDOFF=1.
 */
#ifndef SWEET_XNU_HANDOFF
#define SWEET_XNU_HANDOFF	0
#endif

/*
 * Last sector of the boot partition holds the trace record (see loader_boot).
 * boot is 64 MiB = 131072 sectors, so LBA 131071 is the final one and is never
 * part of the image we flash (which is header + kernel + ramdisk + dtb).
 */
#define BOOT_TRACE_LBA		131071u

/* Published for the panic dump; see shim.c. */
extern uint64_t g_emb, g_phys_base, g_mem_size, g_kernel_end, g_adt_pa, g_pa_of_va, g_rd_start;

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
/*
 * Clean the data cache for a range, so the bytes really reach DRAM.
 *
 * This is the bug that made xnu hang with no output. The shim runs with the
 * MMU off but the D-cache ON (ABL leaves it enabled), so every store below
 * landed in cache and never in memory. xnu starts with the MMU off and reads
 * the kernel image straight from DRAM, so it executed whatever stale bytes
 * happened to be there. Cleaning the I-cache at the end does not help: only a
 * data-cache clean writes the stores out.
 *
 * CTR_EL0.DminLine tells us the line size; use the smallest, it is always safe.
 */
static void dcache_clean(uintptr_t start, uint64_t len)
{
	uint64_t ctr, dmin, line, tmp;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	dmin = (ctr >> 16) & 0xf;		/* log2 words */
	line = 4u << dmin;
	__asm__ volatile("mrs %0, dczid_el0" : "=r"(tmp));
	if (!(tmp & 0x10)) {			/* DC ZVA available */
		uint64_t zline = 4u << (tmp & 0xf);
		if (zline > line)
			line = zline;
	}
	start &= ~(line - 1);
	for (uintptr_t a = start; a < start + len; a += line)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

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
					/*
					 * q + 36 is where the value starts, and 36 is not a
					 * multiple of 8, so this address is 4-aligned at best
					 * (dram-base lands on ...264). A 64-bit store there
					 * raises a synchronous external abort on ginkgo: with
					 * the MMU off the page is Device memory, which requires
					 * natural alignment. Write the two halves separately,
					 * each naturally aligned to 4, and let a barrier order
					 * them.
					 */
					uintptr_t vp = q + 36;
					*(volatile uint32_t *)(vp + 0) = (uint32_t)value;
					__asm__ volatile("dsb sy" ::: "memory");
					*(volatile uint32_t *)(vp + 4) = (uint32_t)(value >> 32);
					__asm__ volatile("dsb sy" ::: "memory");
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
	/*
	 * _end is a link-time constant (0x2d000) because the shim is linked at 0
	 * and the kernel may load it anywhere (Image flags 0xa). Use the
	 * PC-relative runtime address instead, or we read the payload from a
	 * bogus low address and every header field is garbage -- which is what
	 * produced the External Abort on adt_patch_chosen_u64 (FAR 0xb2568264).
	 */
	extern char *shim_end_va(void);
	uintptr_t emb = ((uintptr_t)shim_end_va() + 0xfff) & ~0xfffUL;

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

	/*
	 * ginkgo: the Android boot DTB advertises banks above 4 GiB
	 * (0x14a000000+0xaa000000) that do not exist on SM6125 -- /proc/iomem on
	 * the stock kernel ends at 0x13fffffff. Trusting the FDT there makes the
	 * shim place the kernel and ADT outside real DRAM, which is exactly the
	 * Data Abort at adt_patch_chosen_u64 (FAR 0xb2568264, stage loader).
	 * The payload's values come from mkadt.py, which we feed the verified
	 * /proc/iomem range, so prefer them and only log what the FDT claimed.
	 */
	uint64_t phys_base = p->phys_base;
	uint64_t mem_size = p->mem_size;
	uint64_t fdt_base = 0, fdt_size = 0;
	if (fdt_memory_high(dtb, &fdt_base, &fdt_size)) {
		con_hex("MEMFDT", fdt_base);
		con_hex("MEMSZ", fdt_size);
		con_str("MEMUSE", "payload (fdt ignored)");
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
		/* Push this segment out to DRAM: xnu reads it with the MMU off. */
		dcache_clean(dst, s->vmsize);
	}
	con_hex2("SEGS", "n=", p->nsegs, "end=", p->kernel_end + pa_of_va_base);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	/*
	 * Sanity-check the ADT address BEFORE touching it. An external abort here
	 * is what cost the whole night: with phys_base 0xb0000000 (topaz) the ADT
	 * landed at 0xb2560000, which is not DRAM on ginkgo, and the first store
	 * raised a synchronous external abort with no useful message. Now we say
	 * so instead of dying.
	 */
	con_hex2("ADT", "pa=", adt_pa, "sz=", p->adt_size);
	/*
	 * Publish the decisive numbers so the panic dump can repeat them: a fault
	 * here scrolls these lines off the top of the panel, which is why every
	 * panic so far showed a register dump with no values to interpret it.
	 */
	g_emb = emb;
	g_rd_start = rd_start;
	g_phys_base = phys_base;
	g_mem_size = mem_size;
	g_kernel_end = p->kernel_end;
	g_pa_of_va = pa_of_va_base;
	g_adt_pa = adt_pa;
	if (1) {
		static uint64_t trc[16] __attribute__((aligned(8)));
		const char *m = "SWEETTRC";
		for (int i = 0; i < 8; i++)
			((char *)trc)[i] = m[i];
		trc[1] = emb;
		trc[2] = rd_start;
		trc[3] = p->virt_base;
		trc[4] = p->kernel_end;
		trc[5] = p->phys_base;
		trc[6] = p->mem_size;
		trc[7] = pa_of_va_base;
		trc[8] = adt_pa;
		trc[9] = p->adt_size;
		trc[10] = (uint64_t)(uintptr_t)p;
		write_blocks(BOOT_TRACE_LBA, 1, trc);
	}
	if (adt_pa < phys_base || adt_pa + p->adt_size > phys_base + mem_size) {
		con_hex2("ADTBAD", "lo=", (uint32_t)phys_base, "hi=", (uint32_t)(phys_base + mem_size));
		return "adt outside dram";
	}
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
	/*
	 * Record the exact CPU state xnu is about to inherit. This is the single
	 * most useful line for debugging a silent xnu: if M/C/I are not all zero
	 * here, xnu starts with the wrong assumptions and says nothing.
	 */
	con_hex("SCTLR", rsr("sctlr_el1"));
	con_hex("TTBR0", rsr("ttbr0_el1"));
	con_hex("DAIF", rsr("daif"));
	con_hex("VBAR", rsr("vbar_el1"));
	con_str("XNU", "jumping; watchdog 30 s");
	ufslog_flush();

	/*
	 * The ADT and boot_args are the last things xnu reads, and both were just
	 * written through the D-cache. Clean them, then drop stale I-cache lines,
	 * before handing over.
	 */
	dcache_clean(adt_pa, p->adt_size);
	dcache_clean(ba_pa, PAGE_16K);
	dcache_clean(rd_start, 64);
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	/*
	 * Make this boot end in fastboot, with no human in the loop.
	 *
	 * Everything above is verified: the shim has found the payload, copied all
	 * ten xnu segments, patched the ADT and built boot_args. Up to here every
	 * line on the panel is a fact about this device.
	 *
	 * What follows was the problem. Jumping into xnu wedges it: no output
	 * after "XNU jumping", no panic, no reset, no return -- the panel keeps
	 * these lines until someone holds Power. The watchdog was supposed to
	 * cover exactly that case and does not (commit 7017630; this session
	 * measured 105 s with no reset). So the handoff is where autonomy has to
	 * come from instead.
	 *
	 * autonomy_to_fastboot() writes the IMEM restart reason -- which survives a
	 * warm reset and is exactly what `adb reboot bootloader` sets -- verifies
	 * the store landed, and resets through PSCI. The phone comes back up in
	 * fastboot in a few seconds, every time, because nothing here depends on a
	 * gated clock block or on a timer firing.
	 *
	 * Set SWEET_XNU_HANDOFF to 1 to restore the jump to xnu once it is worth
	 * observing; the autonomy path then becomes the timeout instead of the
	 * destination.
	 */
#if SWEET_XNU_HANDOFF
	con_str("AUTO", "handoff to xnu enabled: bounded observation");
	wdt_selftest();
	ufslog_flush();
	((xnu_entry_t)entry_pa)(ba_pa);
#endif
	con_str("XNU", "handoff disabled for autonomous runs");
	ufslog_flush();
	autonomy_to_fastboot();
	return "unreachable";

	/*
	 * xnu handed control back, which means it died early. Its framebuffer
	 * output is the only record of why, and the screen may be unreadable, so
	 * stash a downsampled copy of it in the log.
	 */
	con_str("XNU", "returned; saving framebuffer");
	{
		static char fbsnap[4096] __attribute__((aligned(4096)));
		const volatile uint32_t *fb = (const volatile uint32_t *)FB_BASE;
		/* One byte per 4x4 block: 270x600 -> too big, take every 16th row. */
		int o = 0;
		for (int y = 0; y < FB_H && o < 3900; y += 16) {
			for (int x = 0; x < FB_W && o < 3900; x += 4) {
				uint32_t px = fb[y * FB_W + x];
				/* bright pixel => something was drawn there */
				fbsnap[o++] = ((px & 0xff) > 0x40) ? '#' : '.';
			}
			fbsnap[o++] = '\n';
		}
		fbsnap[o] = 0;
		ufslog_write("---- FB snapshot ----\n");
		ufslog_write(fbsnap);
		ufslog_write("---- end snapshot ----\n");
		ufslog_flush();
	}
	return "xnu returned";
}
