#!/usr/bin/env python3
"""Mirror of shim/fdt_memory_high for host CI without aarch64-gcc.

Builds the same synthetic topaz /memory layout the C unit test uses and
checks 4 GiB vs 6 GiB upper-bank selection.
"""
import struct
import sys

FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_END = 1, 2, 3, 9
MEM_HIGH_MIN = 0x100000000
MEM_SIZE_MIN = 0x40000000
MEM_SIZE_MAX = 0x200000000


def be32(buf, off):
    return struct.unpack_from('>I', buf, off)[0]


def build_fdt(high_base, high_size):
    strings = b'#address-cells\0#size-cells\0reg\0'
    off_addr, off_size, off_reg = 0, 15, 27

    def emit32(v, out):
        out += struct.pack('>I', v & 0xFFFFFFFF)
        return out

    def emit_str(s, out):
        out += s.encode() + b'\0'
        while len(out) & 3:
            out += b'\0'
        return out

    st = b''
    st = emit32(FDT_BEGIN_NODE, st)
    st = emit_str('', st)
    st = emit32(FDT_PROP, st)
    st = emit32(4, st)
    st = emit32(off_addr, st)
    st = emit32(2, st)
    st = emit32(FDT_PROP, st)
    st = emit32(4, st)
    st = emit32(off_size, st)
    st = emit32(2, st)
    st = emit32(FDT_BEGIN_NODE, st)
    st = emit_str('memory', st)
    st = emit32(FDT_PROP, st)
    st = emit32(48, st)
    st = emit32(off_reg, st)
    for w in (0, 0x40000000, 0, 0x3BB00000, 0, 0x80000000, 0, 0xC0000000,
              high_base >> 32, high_base & 0xFFFFFFFF,
              high_size >> 32, high_size & 0xFFFFFFFF):
        st = emit32(w, st)
    st = emit32(FDT_END_NODE, st)
    st = emit32(FDT_END_NODE, st)
    st = emit32(FDT_END, st)

    hdr = 40
    total = hdr + len(st) + len(strings)
    buf = bytearray(total)
    struct.pack_into('>IIIIIIIIII', buf, 0,
                     FDT_MAGIC, total, hdr, hdr + len(st), hdr, 17, 16, 0,
                     len(strings), len(st))
    buf[hdr:hdr + len(st)] = st
    buf[hdr + len(st):] = strings
    return bytes(buf)


def cells(buf, off, n):
    v = 0
    for i in range(n):
        v = (v << 32) | be32(buf, off + 4 * i)
    return v


def fdt_memory_high(blob):
    if be32(blob, 0) != FDT_MAGIC:
        return None
    p = be32(blob, 8)
    strings = be32(blob, 12)
    addr_cells, size_cells = 2, 2
    depth = 0
    in_memory = False
    root_props = False
    best_base = best_size = 0

    while True:
        tok = be32(blob, p)
        p += 4
        if tok == FDT_BEGIN_NODE:
            end = blob.index(b'\0', p)
            name = blob[p:end].decode()
            p = (end + 1 + 3) & ~3
            depth += 1
            root_props = depth == 1
            in_memory = depth == 2 and (name == 'memory' or name.startswith('memory@'))
        elif tok == FDT_END_NODE:
            in_memory = False
            root_props = False
            depth -= 1
        elif tok == FDT_PROP:
            ln, nameoff = be32(blob, p), be32(blob, p + 4)
            val = p + 8
            pname = blob[strings + nameoff:].split(b'\0', 1)[0].decode()
            if root_props and depth == 1:
                if pname == '#address-cells' and ln == 4:
                    addr_cells = be32(blob, val)
                elif pname == '#size-cells' and ln == 4:
                    size_cells = be32(blob, val)
            elif in_memory and pname == 'reg':
                stride = (addr_cells + size_cells) * 4
                if stride and ln % stride == 0:
                    for off in range(0, ln, stride):
                        b = cells(blob, val + off, addr_cells)
                        s = cells(blob, val + off + 4 * addr_cells, size_cells)
                        if (b >= MEM_HIGH_MIN and MEM_SIZE_MIN <= s <= MEM_SIZE_MAX
                                and b + s >= b):
                            if s > best_size or (s == best_size and b > best_base):
                                best_base, best_size = b, s
            p = (val + ln + 3) & ~3
        elif tok == FDT_END or tok not in (FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, 4):
            return (best_base, best_size) if best_size else None


def main():
    fail = 0
    for label, base, size in (
        ('4G', 0x140000000, 0x100000000),
        ('6G', 0x140000000, 0x180000000),
    ):
        got = fdt_memory_high(build_fdt(base, size))
        if got != (base, size):
            print(f'FAIL {label}: {got}', file=sys.stderr)
            fail = 1
        else:
            print(f'ok {label} base=0x{base:x} size=0x{size:x}')
    return fail


if __name__ == '__main__':
    sys.exit(main())
