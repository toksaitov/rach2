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
Exits non-zero on any finding.
"""
import struct
import sys

# From rach2/sys/proc/elf.h and rach2/sys/mm/usercopy.h.
ELF_MAX_PHNUM = 32
ELF_MAX_SEGS = 8
USER_ADDR_MIN = 0x02000000
USER_ADDR_MAX = 0xC0000000
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
        if p_offset + p_filesz < p_offset or p_vaddr + p_memsz < p_vaddr:
            problems.append("%s: a segment wraps the address space" % name)
            continue
        if p_offset + p_filesz > len(data):
            problems.append("%s: a segment runs past the file" % name)
        if p_vaddr < USER_ADDR_MIN or p_vaddr + p_memsz > USER_ADDR_MAX:
            problems.append("%s: a segment lies outside the user window "
                            "[0x%08x, 0x%08x)"
                            % (name, USER_ADDR_MIN, USER_ADDR_MAX))
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
    if e_entry < USER_ADDR_MIN or e_entry >= USER_ADDR_MAX:
        problems.append("%s: the entry point is outside the user window"
                        % name)

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


def main(argv):
    if not argv:
        sys.stderr.write("usage: check_elf.py FILE...\n")
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
