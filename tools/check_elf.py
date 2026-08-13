#!/usr/bin/env python3
"""
check_elf.py: judge an ELF against what this kernel will actually load.

`file(1)` says "ELF 32-bit LSB executable, Intel 80386, statically linked" for
images this kernel refuses, and says nothing about the two rules that matter
most here. So this reads the header itself, and it reads it in two halves that
are deliberately kept apart, because they are two different claims.

The LOADER half is `elf_load_image`'s contract (rach2/sys/proc/elf.c), copied
here rule for rule: the identification bytes, the type and machine, the program
header geometry, the segment bounds, and the entry point. An image that fails
any of these is one exec refuses.

The POLICY half is this project's, not the kernel's: no `PT_INTERP` and no
`PT_DYNAMIC`. The loader does not refuse those, it IGNORES them, which is worse
than refusing: a dynamic image loads, jumps, and runs with nothing relocated.
The check exists here precisely because the kernel will not make it.

Usage:
    check_elf.py FILE...        judge each file, report every finding
    check_elf.py --selftest     run the boundary cases over crafted headers
Exits non-zero on any finding.
"""
import struct
import sys

# From rach2/sys/proc/elf.h and elf.c. The window is the LOADER's own
# (ELF_USER_MIN/ELF_USER_MAX at elf.c:16-17), which is one page below the
# usercopy window that mm/usercopy.h defines: exec leaves that page out, and a
# checker using the wider bound would approve an image exec refuses.
ELF_MAX_PHNUM = 32
ELF_MAX_SEGS = 8
ELF_USER_MIN = 0x02000000
ELF_USER_MAX = 0xBFFFF000
UINT32_MAX = 0xFFFFFFFF
PAGE_SIZE = 0x1000

ET_EXEC = 2
EM_386 = 3
PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3

EHDR = "<16sHHIIIIIHHHHHH"
EHDR_SIZE = struct.calcsize(EHDR)
PHDR = "<IIIIIIII"
PHDR_SIZE = struct.calcsize(PHDR)


def loader_findings(name, data):
    """What `elf_load_image` would refuse. Returns a list of problems."""
    problems = []
    if len(data) < EHDR_SIZE:
        return ["%s: shorter than an ELF header" % name]

    (ident, e_type, e_machine, _ver, e_entry, e_phoff, _shoff, _flags,
     _ehsize, e_phentsize, e_phnum, _shentsize, _shnum,
     _shstrndx) = struct.unpack(EHDR, data[:EHDR_SIZE])

    if ident[:4] != b"\x7fELF":
        return ["%s: no ELF signature" % name]
    if ident[4] != 1:
        problems.append("%s: not ELFCLASS32" % name)
    if ident[5] != 1:
        problems.append("%s: not ELFDATA2LSB" % name)
    if ident[6] != 1:
        problems.append("%s: not EV_CURRENT" % name)
    if e_type != ET_EXEC:
        problems.append("%s: e_type is %d, not ET_EXEC; this kernel loads no "
                        "other kind" % (name, e_type))
    if e_machine != EM_386:
        problems.append("%s: e_machine is %d, not EM_386" % (name, e_machine))
    if e_phoff == 0 or e_phnum == 0:
        problems.append("%s: no program header table" % name)
        return problems
    if e_phentsize != PHDR_SIZE:
        problems.append("%s: e_phentsize is %d, not %d"
                        % (name, e_phentsize, PHDR_SIZE))
        return problems
    if e_phnum > ELF_MAX_PHNUM:
        problems.append("%s: %d program headers, more than the %d this kernel "
                        "reads" % (name, e_phnum, ELF_MAX_PHNUM))
    if e_phoff + e_phnum * PHDR_SIZE > len(data):
        problems.append("%s: the program header table runs past the file"
                        % name)
        return problems

    phdrs = []
    for i in range(e_phnum):
        off = e_phoff + i * PHDR_SIZE
        phdrs.append(struct.unpack(PHDR, data[off:off + PHDR_SIZE]))

    loads = [p for p in phdrs if p[0] == PT_LOAD]
    if not loads:
        problems.append("%s: no PT_LOAD segment" % name)
        return problems
    if len(loads) > ELF_MAX_SEGS:
        problems.append("%s: %d PT_LOAD segments, more than the %d this kernel "
                        "has slots for" % (name, len(loads), ELF_MAX_SEGS))

    phdr_covered = False
    end = 0
    for (_type, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, _flags,
         _align) in loads:
        if p_filesz > p_memsz:
            problems.append("%s: a segment's file size exceeds its memory size"
                            % name)
        # The loader's wrap tests are 32-bit; Python's integers do not wrap, so
        # the same question is asked as an explicit bound.
        if p_offset + p_filesz > UINT32_MAX or p_vaddr + p_memsz > UINT32_MAX:
            problems.append("%s: a segment wraps the 32-bit address space"
                            % name)
            continue
        if p_offset + p_filesz > len(data):
            problems.append("%s: a segment runs past the file" % name)
        if p_vaddr < ELF_USER_MIN or p_vaddr + p_memsz > ELF_USER_MAX:
            problems.append("%s: a segment lies outside the window exec loads "
                            "into, [0x%08x, 0x%08x)"
                            % (name, ELF_USER_MIN, ELF_USER_MAX))
        page_start = p_vaddr & ~(PAGE_SIZE - 1)
        if page_start < end:
            problems.append("%s: the PT_LOAD segments are not ascending and "
                            "page-disjoint" % name)
        end = (p_vaddr + p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)
        # AT_PHDR is derived from a header table that lies inside a load.
        if p_offset <= e_phoff and \
                e_phoff + e_phnum * PHDR_SIZE <= p_offset + p_filesz:
            phdr_covered = True

    if not phdr_covered:
        problems.append("%s: the program header table lies outside every "
                        "PT_LOAD, so AT_PHDR would name nothing" % name)
    if e_entry < ELF_USER_MIN or e_entry >= ELF_USER_MAX:
        problems.append("%s: the entry point is outside the window exec loads "
                        "into" % name)

    return problems


def policy_findings(name, data):
    """What THIS PROJECT refuses, which the loader would accept."""
    problems = []
    if len(data) < EHDR_SIZE:
        return problems
    (_ident, _type, _machine, _ver, _entry, e_phoff, _shoff, _flags, _ehsize,
     e_phentsize, e_phnum, _shentsize, _shnum,
     _shstrndx) = struct.unpack(EHDR, data[:EHDR_SIZE])
    if e_phoff == 0 or e_phentsize != PHDR_SIZE:
        return problems
    if e_phoff + e_phnum * PHDR_SIZE > len(data):
        return problems

    for i in range(e_phnum):
        off = e_phoff + i * PHDR_SIZE
        p_type = struct.unpack(PHDR, data[off:off + PHDR_SIZE])[0]
        if p_type == PT_INTERP:
            problems.append("%s: has a PT_INTERP, so it wants a dynamic "
                            "linker; this system has none and the loader does "
                            "not refuse it, it ignores it" % name)
        if p_type == PT_DYNAMIC:
            problems.append("%s: has a PT_DYNAMIC, so it expects relocation "
                            "nobody here performs" % name)

    return problems


def check(path):
    """Judge one file. Returns a list of problems."""
    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError as error:
        return ["%s: cannot read: %s" % (path, error)]
    if not data:
        return ["%s: empty" % path]

    return loader_findings(path, data) + policy_findings(path, data)


# The cases that decide where the loader's window ends, crafted rather than
# found: no binary in this tree lives near the ceiling, and the checker was
# wrong about it once, using the usercopy window (0xC0000000) where the loader
# uses one page less.
#
# The addresses are LITERAL and not written in terms of the constants above,
# which is the whole point: a case built out of ELF_USER_MAX moves when
# ELF_USER_MAX is wrong, passes, and proves nothing. These are the numbers
# rach2/sys/proc/elf.c:16-17 actually holds, written out.
#
# Each is (name, e_entry, p_vaddr, p_memsz, want_ok).
BOUNDARY_CASES = [
    ("a segment ending exactly at the ceiling", 0x08048000,
     0xBFFFE000, 0x1000, True),
    ("a segment ending one byte past the ceiling", 0x08048000,
     0xBFFFE000, 0x1001, False),
    ("a segment inside the page exec leaves out", 0x08048000,
     0xBFFFF000, 0x1000, False),
    ("an entry point at the ceiling", 0xBFFFF000, 0x08048000, 0x1000, False),
    ("an entry point one byte below it", 0xBFFFEFFF, 0x08048000, 0x1000, True),
    ("a segment starting below the floor", 0x08048000,
     0x01FFF000, 0x1000, False),
]


def crafted(entry, vaddr, memsz):
    """One PT_LOAD image with the header table inside it, for the cases above."""
    phoff = EHDR_SIZE
    filesz = EHDR_SIZE + PHDR_SIZE
    ehdr = struct.pack(EHDR, b"\x7fELF\x01\x01\x01" + b"\0" * 9, ET_EXEC,
                       EM_386, 1, entry, phoff, 0, 0, EHDR_SIZE, PHDR_SIZE, 1,
                       0, 0, 0)
    phdr = struct.pack(PHDR, PT_LOAD, 0, vaddr, vaddr, filesz,
                       max(memsz, filesz), 5, PAGE_SIZE)

    return ehdr + phdr


def selftest():
    """Run the boundary cases. Returns a process status."""
    bad = 0
    for name, entry, vaddr, memsz, want_ok in BOUNDARY_CASES:
        got = loader_findings("case", crafted(entry, vaddr, memsz))
        if want_ok and got:
            print("  FAIL: %s: expected a clean run, got: %s" % (name, got[0]))
            bad += 1
        elif not want_ok and not got:
            print("  FAIL: %s: expected a finding, the run was clean" % name)
            bad += 1
    if bad:
        print("check-elf: selftest: %d of %d cases failed"
              % (bad, len(BOUNDARY_CASES)))

        return 1

    print("check-elf: selftest: %d cases PASS" % len(BOUNDARY_CASES))

    return 0


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest()
    if not argv:
        sys.stderr.write("usage: check_elf.py FILE... | --selftest\n")
        return 2

    problems = []
    for path in argv:
        problems.extend(check(path))
    if problems:
        sys.stderr.write("check-elf: FAIL\n")
        for problem in problems:
            sys.stderr.write("  - " + problem + "\n")
        return 1

    print("check-elf: OK (%s: loadable by this kernel, and static)"
          % ", ".join(argv))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
