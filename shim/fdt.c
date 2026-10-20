/*
 * Just enough flattened-device-tree parsing to find what ABL put in
 * /chosen (initrd location) and /memory (DRAM banks for 4G/6G).
 * Big-endian, 4-byte aligned tokens.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

/* Prefer banks at or above 4 GiB (above Qualcomm carve-outs on topaz). */
#define MEM_HIGH_MIN	0x100000000ULL
#define MEM_SIZE_MIN	0x40000000ULL	/* 1 GiB */
#define MEM_SIZE_MAX	0x200000000ULL	/* 8 GiB sanity cap */

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static int str_is_memory(const char *name)
{
	if (streq(name, "memory"))
		return 1;
	if (name[0] == 'm' && name[1] == 'e' && name[2] == 'm' &&
	    name[3] == 'o' && name[4] == 'r' && name[5] == 'y' && name[6] == '@')
		return 1;
	return 0;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

static uint64_t cells_be(uintptr_t val, uint32_t cells)
{
	uint64_t v = 0;
	uint32_t i;

	for (i = 0; i < cells; i++)
		v = (v << 32) | be32(val + 4u * i);
	return v;
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len == 4)
					*out = be32(val);
				else if (len == 8)
					*out = (uint64_t)be32(val) << 32 | be32(val + 4);
				else
					return 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

int fdt_memory_high(uintptr_t fdt, uint64_t *base, uint64_t *size)
{
	uint32_t addr_cells = 2, size_cells = 2;	/* arm64 Linux defaults */
	uint64_t best_base = 0, best_size = 0;
	int depth = 0, in_memory = 0, root_props = 1;

	if (!fdt || !base || !size || be32(fdt) != FDT_MAGIC)
		return 0;

	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			/* Root is depth 1 with an empty name; its props set cells. */
			if (depth == 1)
				root_props = 1;
			else
				root_props = 0;
			in_memory = depth == 2 && str_is_memory(name);
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			in_memory = 0;
			root_props = 0;
			depth--;
			if (depth < 0)
				return 0;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			const char *pname = (const char *)(strings + nameoff);

			if (root_props && depth == 1) {
				if (streq(pname, "#address-cells") && len == 4)
					addr_cells = be32(val);
				else if (streq(pname, "#size-cells") && len == 4)
					size_cells = be32(val);
			} else if (in_memory && streq(pname, "reg") &&
				   addr_cells > 0 && addr_cells <= 2 &&
				   size_cells > 0 && size_cells <= 2) {
				uint32_t stride = (addr_cells + size_cells) * 4u;
				uint32_t off;

				if (stride == 0 || len % stride != 0) {
					p = align4(val + len);
					break;
				}
				for (off = 0; off + stride <= len; off += stride) {
					uint64_t b = cells_be(val + off, addr_cells);
					uint64_t s = cells_be(val + off + 4u * addr_cells,
							     size_cells);

					if (b < MEM_HIGH_MIN || s < MEM_SIZE_MIN ||
					    s > MEM_SIZE_MAX || b + s < b)
						continue;
					/* Prefer larger bank; tie-break higher base. */
					if (s > best_size ||
					    (s == best_size && b > best_base)) {
						best_base = b;
						best_size = s;
					}
				}
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:
			if (best_size) {
				*base = best_base;
				*size = best_size;
				return 1;
			}
			return 0;
		}
	}
}
