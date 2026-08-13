#!/usr/bin/env python3
"""
check_link.py: ask whether a program could link against what an image carries.

The guest compiles with tcc, and tcc links what the guest has: musl's `libc.a`,
tcc's runtime `libtcc1.a`, and the startup objects. Nothing appends libgcc there
the way the host's wrapper does, so a symbol musl references and neither archive
defines is not a build-time question at all: it is a student's first link
failing with an undefined symbol, which is a bad way to learn about libgcc.

So the question is asked here, on the host, of the very files that will be
staged: every undefined symbol in the libraries, minus everything they define
between them, must be empty. A weak undefined is not a failure, since a link
resolves those to zero, and neither is a symbol the C library itself defines
elsewhere in the same archive.

Two things are excluded, and both are stated rather than quietly dropped. `main`
is referenced by `crt1.o` and is the whole point of compiling a program: the
program being linked provides it. And an archive member is only pulled in when
it resolves something, so a member no ordinary link ever reaches may reference
whatever it likes; `alloca86-bt.o` is tcc's bounds-checking alloca and wants
`__bound_new_region`, which lives in the `bcheck.o` this build leaves out (musl
has no glibc malloc hooks for it). A link without `-b` never asks for it, and
`-b` is not available on this machine anyway.

A third exclusion is the linker's own symbols, `_GLOBAL_OFFSET_TABLE_` and the
section markers: those name places in the output image, not code in any input,
and tcc defines them as it lays the image out.

Usage:
    check_link.py LIB_OR_OBJECT...
Exits non-zero if anything would be left undefined.
"""
import subprocess
import sys

NM = "i686-elf-nm"

# Provided by the program being compiled, not by anything staged.
PROVIDED_BY_THE_PROGRAM = ("main",)

# Provided by the LINKER, which tcc is: these name places in the output rather
# than code in any input, and tcc defines them itself as it lays the image out
# (tccelf.c's linker symbols). No archive can be expected to hold them.
PROVIDED_BY_THE_LINKER = (
    "_GLOBAL_OFFSET_TABLE_",
    "_etext", "_edata", "_end", "__bss_start", "__ehdr_start",
    "__init_array_start", "__init_array_end",
    "__fini_array_start", "__fini_array_end",
    "__preinit_array_start", "__preinit_array_end",
)

# Members no ordinary link pulls in, so what they reference is not owed. See the
# header: this one is the bounds-checking alloca, and `-b` is not available here.
UNREACHED_MEMBERS = ("alloca86-bt.o", "alloca86_64-bt.o")


def symbols(path):
    """Return (defined, undefined) symbol name sets for an archive or object.

    Undefined symbols are attributed to the archive member that references them,
    so a member no link reaches can be left out of the question entirely.

    A definition counts only when it is EXPORTED, which `nm` writes as an
    upper-case type letter. A lower-case one is a local symbol, visible inside
    its own object and to nothing else, so it cannot resolve another object's
    reference; this archive holds exactly that case, a local `__udivmoddi4`
    beside the global one, and counting it would let the global be removed
    without the check noticing.
    """
    try:
        run = subprocess.run([NM, path], capture_output=True, text=True,
                             check=False)
    except OSError as error:
        raise SystemExit("check-link: cannot run %s: %s" % (NM, error))
    if run.returncode != 0:
        raise SystemExit("check-link: %s exited %d on %s: %s"
                         % (NM, run.returncode, path, run.stderr.strip()))
    out = run.stdout
    if path.endswith(".a") and not out.strip():
        raise SystemExit("check-link: %s reported no symbols in the archive %s, "
                         "so nothing in it was actually read" % (NM, path))

    defined = set()
    undefined = set()
    member = ""
    for line in out.splitlines():
        stripped = line.strip()
        if stripped.endswith(":") and not stripped.startswith(("0", "-")):
            member = stripped[:-1]
            continue
        parts = line.split()
        if len(parts) == 2 and parts[0] == "U":
            if member not in UNREACHED_MEMBERS and \
                    parts[1] not in PROVIDED_BY_THE_PROGRAM and \
                    parts[1] not in PROVIDED_BY_THE_LINKER:
                undefined.add(parts[1])
        elif len(parts) == 2 and parts[0] in ("w", "v"):
            pass  # a weak undefined links as zero
        elif len(parts) == 3 and parts[1] not in ("U", "u", "N") and \
                parts[1].isupper():
            defined.add(parts[2])

    return defined, undefined


def main(argv):
    if not argv:
        sys.stderr.write("usage: check_link.py LIB_OR_OBJECT...\n")
        return 2

    defined = set()
    undefined = set()
    for path in argv:
        d, u = symbols(path)
        defined |= d
        undefined |= u

    missing = sorted(undefined - defined)
    if missing:
        sys.stderr.write("check-link: FAIL\n")
        for name in missing:
            sys.stderr.write("  - %s is referenced and nothing here defines "
                             "it; a link in the guest would stop on it\n"
                             % name)
        sys.stderr.write("  the guest links libc.a, libtcc1.a and the crt "
                         "objects and nothing else: no libgcc is appended "
                         "there the way the host's wrapper appends it\n")
        return 1

    print("check-link: OK (%d symbols defined, nothing left undefined)"
          % len(defined))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
