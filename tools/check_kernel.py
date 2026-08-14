#!/usr/bin/env python3
"""Check a Rach2 kernel ELF against what its two loaders require.

The multiboot loaders (QEMU's -kernel and GRUB) read the program headers and
the multiboot header; nothing else in the file matters to a boot. This checks
exactly that surface, so a kernel that would fail to boot, or boot and then
overwrite itself, is refused on the host before a machine is started:

  * ELF32 executable for the Intel 80386.
  * The FIRST multiboot magic with a zero-sum checksum, on a four-byte
    boundary within the loader's search window (QEMU examines offsets below
    8192 minus the 48-byte worst-case header), must carry exactly the flags
    this kernel
    declares (page-align + meminfo, 3): the loaders obey the first valid
    header they find, so an earlier valid header with other flags, or one
    with the address-override bit, would change how the file is loaded.
  * Exactly two PT_LOAD segments (text+rodata, then data+bss), placed at or
    above 0x100000, non-overlapping, each with `p_paddr == p_vaddr` (QEMU
    loads at the physical address and would put a zero-paddr kernel in low
    memory), `p_filesz <= p_memsz`, the file range inside the file, and the
    file offset congruent to the address modulo the alignment.
  * The entry point equal to the `_start` symbol's address and inside the
    file-backed bytes of a load, not its zero-fill.
  * `_end` present and equal to the highest `p_vaddr + p_memsz`, since the
    boot allocator starts reserving physical memory at that symbol.

`--tcc` adds the assertions specific to tcc's layout, where the ELF and
program headers themselves sit inside the first load: first-load file offset
0 and address exactly 0x100000. The GNU-linked kernel keeps its first load at
file offset 0x1000 and is checked by the common profile alone.

`--selftest` builds a minimal kernel-shaped ELF in memory and asserts that
the checker accepts it and refuses each of the malformed variants a loader
would misload, so the rules above cannot rot unnoticed.

Usage: check_kernel.py [--tcc] KERNEL | check_kernel.py --selftest
"""

import struct
import sys

MULTIBOOT_MAGIC = 0x1BADB002
MULTIBOOT_FLAGS = 0x3
LOAD_FLOOR = 0x100000
SEARCH_END = 8192 - 48
PT_LOAD = 1


class CheckError(Exception):
    pass


def fail(reason):
    raise CheckError(reason)


def report_fail(text):
    print("check-kernel: fail: %s" % text, file=sys.stderr)


def check(data, tcc_profile):
    """Apply every rule to `data`; raise `CheckError` on the first violation."""
    if len(data) < 52 or data[:4] != b"\x7fELF":
        fail("not an ELF file")
    if data[4] != 1 or data[5] != 1:
        fail("not a 32-bit little-endian ELF")

    try:
        (e_type, e_machine, _ver, e_entry, e_phoff, _shoff, _flags, _ehsize,
         e_phentsize, e_phnum) = struct.unpack_from("<HHIIIIIHHH", data, 16)

        if e_type != 2:
            fail("type is %d, not ET_EXEC" % e_type)
        if e_machine != 3:
            fail("machine is %d, not EM_386" % e_machine)
        if e_phentsize != 32:
            fail("program header entry size is %d, not 32" % e_phentsize)

        loads = []
        for i in range(e_phnum):
            (p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, _pflags,
             p_align) = struct.unpack_from("<IIIIIIII", data,
                                           e_phoff + 32 * i)
            if p_type == PT_LOAD:
                loads.append((p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
                              p_align))
    except struct.error:
        fail("truncated or malformed ELF headers")

    if len(loads) != 2:
        fail("%d PT_LOAD segments, not 2" % len(loads))

    for offset, vaddr, paddr, filesz, memsz, align in loads:
        if paddr != vaddr:
            fail("load at 0x%x has p_paddr 0x%x; the loaders place segments "
                 "at the physical address" % (vaddr, paddr))
        if vaddr < LOAD_FLOOR:
            fail("load at 0x%x is below 0x%x" % (vaddr, LOAD_FLOOR))
        if filesz > memsz:
            fail("load at 0x%x has p_filesz 0x%x > p_memsz 0x%x"
                 % (vaddr, filesz, memsz))
        if offset + filesz > len(data):
            fail("load at 0x%x: file range 0x%x+0x%x runs past the end of "
                 "the file" % (vaddr, offset, filesz))
        if align > 1 and (offset % align) != (vaddr % align):
            fail("load at 0x%x: offset 0x%x not congruent to the address "
                 "modulo 0x%x" % (vaddr, offset, align))

    loads.sort(key=lambda l: l[1])
    first_end = loads[0][1] + loads[0][4]
    if loads[1][1] < first_end:
        fail("loads overlap: 0x%x < 0x%x" % (loads[1][1], first_end))

    # The multiboot header: the loaders obey the FIRST magic whose three
    # words sum to zero, so that occurrence, wherever it is, must be the one
    # header this kernel means, with exactly the declared flags.
    found = None
    for pos in range(0, min(len(data) - 11, SEARCH_END), 4):
        if struct.unpack_from("<I", data, pos)[0] == MULTIBOOT_MAGIC:
            flags, checksum = struct.unpack_from("<II", data, pos + 4)
            if (MULTIBOOT_MAGIC + flags + checksum) & 0xFFFFFFFF == 0:
                found = (pos, flags)
                break
    if found is None:
        fail("no multiboot header with a valid checksum in the loader's "
             "search window")
    if found[1] != MULTIBOOT_FLAGS:
        fail("the first valid multiboot header (file offset 0x%x) has flags "
             "0x%x, not 0x%x" % (found[0], found[1], MULTIBOOT_FLAGS))

    try:
        symbols = read_symbols(data)
    except (struct.error, ValueError):
        fail("truncated or malformed symbol table")
    if "_start" not in symbols:
        fail("no _start symbol")
    if e_entry != symbols["_start"]:
        fail("entry 0x%x is not _start 0x%x" % (e_entry, symbols["_start"]))
    entry_load = None
    for offset, vaddr, _p, filesz, memsz, _a in loads:
        if vaddr <= e_entry < vaddr + memsz:
            entry_load = (vaddr, filesz)
    if entry_load is None:
        fail("entry 0x%x is outside every load" % e_entry)
    if e_entry >= entry_load[0] + entry_load[1]:
        fail("entry 0x%x is in the zero-fill of its load, not in file-backed "
             "bytes" % e_entry)

    image_end = max(v + m for _o, v, _p, _f, m, _a in loads)
    if "_end" not in symbols:
        fail("no _end symbol")
    if symbols["_end"] != image_end:
        fail("_end 0x%x is not the image end 0x%x"
             % (symbols["_end"], image_end))

    if tcc_profile:
        offset, vaddr, paddr, _filesz, _memsz, _align = loads[0]
        if offset != 0:
            fail("tcc profile: first load file offset is 0x%x, not 0" % offset)
        if vaddr != LOAD_FLOOR:
            fail("tcc profile: first load at 0x%x, not 0x%x"
                 % (vaddr, LOAD_FLOOR))

    return ("%s: 2 loads, entry 0x%x, _end 0x%x, multiboot header at file "
            "offset 0x%x"
            % ("tcc profile" if tcc_profile else "common profile", e_entry,
               image_end, found[0]))


def read_symbols(data):
    """Return `{name: value}` for `_start` and `_end` from the symbol table."""
    (shoff,) = struct.unpack_from("<I", data, 32)
    (_ehsize, _phentsize, _phnum, e_shentsize, e_shnum,
     _shstrndx) = struct.unpack_from("<HHHHHH", data, 40)
    wanted = {}
    if shoff == 0 or e_shentsize == 0:
        return wanted
    for i in range(e_shnum):
        (_name, sh_type, _flags, _addr, sh_offset, sh_size, sh_link, _info,
         _align, sh_entsize) = struct.unpack_from(
             "<IIIIIIIIII", data, shoff + e_shentsize * i)
        if sh_type != 2:  # SHT_SYMTAB
            continue
        (_sn, _st, _sf, _sa, str_offset, _ss, _sl, _si, _sal,
         _se) = struct.unpack_from("<IIIIIIIIII", data,
                                   shoff + e_shentsize * sh_link)
        count = sh_size // sh_entsize if sh_entsize else 0
        for j in range(count):
            name_off, value = struct.unpack_from(
                "<II", data, sh_offset + sh_entsize * j)
            end = data.index(b"\x00", str_offset + name_off)
            name = data[str_offset + name_off:end].decode("ascii", "replace")
            if name in ("_start", "_end"):
                wanted[name] = value
    return wanted


def synthesize(first_off=0, mb_at=16, text_size=0x100):
    """Build a minimal valid kernel-shaped ELF: a text load at 0x100000, a
    data+bss load after it, one multiboot header inside the text at offset
    `mb_at`, and a symtab with `_start` and `_end`. `first_off=0` models the
    tcc layout, where the ELF headers sit inside the first load and the text
    follows them; a nonzero `first_off` models the GNU layout, where the
    first load maps the file from that offset and the headers stay outside
    it."""
    hdr_size = 52 + 2 * 32
    if first_off == 0:
        text_off = hdr_size
        first_file_start = 0
        first_filesz = text_off + text_size
        text_base = LOAD_FLOOR + text_off
    else:
        text_off = first_off
        first_file_start = first_off
        first_filesz = text_size
        text_base = LOAD_FLOOR

    text = bytearray(b"\x00" * text_size)
    struct.pack_into("<III", text, mb_at, MULTIBOOT_MAGIC, MULTIBOOT_FLAGS,
                     (-(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)) & 0xFFFFFFFF)
    entry_at = mb_at + 12
    text[entry_at:entry_at + 3] = b"\xf4\xeb\xfd"  # hlt; jmp back
    entry = text_base + entry_at

    data_off = ((first_file_start + first_filesz) + 0xFFF) & ~0xFFF
    data_vaddr = LOAD_FLOOR + (data_off - first_file_start)
    data_bytes = b"\x11" * 8
    bss_size = 0x20
    image_end = data_vaddr + len(data_bytes) + bss_size

    strtab = b"\x00_start\x00_end\x00"
    syms = struct.pack("<IIIBBH", 0, 0, 0, 0, 0, 0)
    syms += struct.pack("<IIIBBH", 1, entry, 0, 0x10, 0, 1)
    syms += struct.pack("<IIIBBH", 8, image_end, 0, 0x10, 0, 2)

    sym_off = data_off + len(data_bytes)
    str_off = sym_off + len(syms)
    shoff = str_off + len(strtab)

    sections = [struct.pack("<IIIIIIIIII", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
                struct.pack("<IIIIIIIIII", 0, 2, 0, 0, sym_off, len(syms),
                            2, 0, 4, 16),
                struct.pack("<IIIIIIIIII", 0, 3, 0, 0, str_off, len(strtab),
                            0, 0, 1, 0)]

    ehdr = struct.pack("<4sBBBBB7xHHIIIIIHHHHHH",
                       b"\x7fELF", 1, 1, 1, 0, 0,
                       2, 3, 1, entry, 52, shoff, 0, 52, 32, 2, 40, 3, 0)
    ph1 = struct.pack("<IIIIIIII", PT_LOAD, first_file_start, LOAD_FLOOR,
                      LOAD_FLOOR, first_filesz, first_filesz, 5, 0x1000)
    ph2 = struct.pack("<IIIIIIII", PT_LOAD, data_off, data_vaddr, data_vaddr,
                      len(data_bytes), len(data_bytes) + bss_size, 6, 0x1000)

    blob = bytearray(ehdr + ph1 + ph2)
    blob += b"\x00" * (text_off - len(blob))
    blob += text
    blob += b"\x00" * (data_off - len(blob))
    blob += data_bytes
    blob += syms
    blob += strtab
    for section in sections:
        blob += section
    return bytes(blob)


def selftest():
    """Assert the checker accepts the well-formed fixtures and refuses each
    malformed variant for the rule that variant violates, matched against the
    diagnostic text, so a mutation cannot pass by tripping a different
    rule."""
    good = synthesize()
    check(good, tcc_profile=True)
    check(good, tcc_profile=False)
    gnu_shaped = synthesize(first_off=0x1000)
    check(gnu_shaped, tcc_profile=False)

    failures = []

    def expect_fail(data, why, want, tcc_profile=False):
        try:
            check(bytes(data), tcc_profile)
        except CheckError as error:
            if want not in str(error):
                failures.append("%s: refused for the wrong rule: %s"
                                % (why, error))
            return
        failures.append("accepted %s" % why)

    def set32(data, pos, value):
        struct.pack_into("<I", data, pos, value)

    hdr = 52 + 2 * 32  # text file offset of the tcc-shaped fixture
    mb = hdr + 16      # its multiboot header's file offset

    d = bytearray(good)
    set32(d, mb + 8, 0)
    expect_fail(d, "a corrupted multiboot checksum", "no multiboot header")

    d = bytearray(good)
    set32(d, 24, 0x90000)
    expect_fail(d, "an entry that is not _start", "is not _start")

    d = bytearray(good)
    set32(d, 52 + 12, 0)
    expect_fail(d, "a zero physical address", "p_paddr")

    d = bytearray(good)
    set32(d, 52 + 32 + 4, len(good))
    expect_fail(d, "a load past the end of the file", "past the end")

    # Zero-fill entry: shrink only p_filesz, keeping p_memsz, so containment
    # still holds and the zero-fill rule itself must fire.
    d = bytearray(good)
    set32(d, 52 + 16, mb)
    expect_fail(d, "an entry in its load's zero-fill", "zero-fill")

    d = bytearray(good)
    set32(d, mb + 4, 0x10003)
    set32(d, mb + 8, (-(MULTIBOOT_MAGIC + 0x10003)) & 0xFFFFFFFF)
    expect_fail(d, "an address-override multiboot header", "flags")

    # An earlier checksum-valid header with foreign flags, written into the
    # fixture's text padding BEFORE the real header: the loaders obey the
    # first valid header, so the checker must refuse on that one's flags.
    d = bytearray(good)
    set32(d, hdr, MULTIBOOT_MAGIC)
    set32(d, hdr + 4, 0x10000)
    set32(d, hdr + 8, (-(MULTIBOOT_MAGIC + 0x10000)) & 0xFFFFFFFF)
    expect_fail(d, "an earlier valid header with foreign flags", "flags")

    # The loader's search bound, exact on both sides: a header at the last
    # offset QEMU examines (8192 - 48 - 4) is accepted, one step past it is
    # not.
    last = SEARCH_END - 4
    check(synthesize(mb_at=last - hdr, text_size=0x2000), tcc_profile=True)
    d = synthesize(mb_at=(last + 4) - hdr, text_size=0x2000)
    expect_fail(d, "a header past the loader's search bound",
                "no multiboot header")

    # The GNU-shaped fixture passes the common profile above; the tcc profile
    # must refuse it for its nonzero first-load file offset.
    expect_fail(bytearray(gnu_shaped), "the GNU layout under the tcc profile",
                "file offset", tcc_profile=True)

    if failures:
        for line in failures:
            print("check-kernel: selftest: fail: %s" % line, file=sys.stderr)
        sys.exit(1)
    print("check-kernel: selftest: ok: 4 accepted, 9 refused, each for its "
          "own rule")


def main():
    args = sys.argv[1:]
    if args == ["--selftest"]:
        selftest()
        return
    tcc_profile = False
    if args and args[0] == "--tcc":
        tcc_profile = True
        args = args[1:]
    if len(args) != 1:
        print("check-kernel: usage: check_kernel.py [--tcc] KERNEL | "
              "--selftest", file=sys.stderr)
        sys.exit(2)
    with open(args[0], "rb") as handle:
        data = handle.read()
    try:
        detail = check(data, tcc_profile)
    except CheckError as error:
        report_fail(error)
        sys.exit(1)
    print("check-kernel: ok: %s" % detail)


if __name__ == "__main__":
    main()
