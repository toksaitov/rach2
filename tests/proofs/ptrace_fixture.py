#!/usr/bin/env python3
"""ptrace_fixture.py: put the debug-entry fixture of ptracemusl's delayed rows
into a private copy of the kernel's sources.

Usage: ptrace_fixture.py TREE ARCH

ptracemusl's delayed and delayed-poll rows need a debug exception taken at the
first instruction of the system-call entry, before x86-64 changes GS or RSP,
and so does its entry row's test of the debug interrupt stack and GS on
x86-64. The fixture injects `int $1` there, on ARCH's entry, and makes
debugreg_defer_user take the saved PC just after that two-byte instruction for
the entry's. The processor then builds the debug frame and runs the real entry
and return code; no MOV SS delay is needed. The fixture edits TREE/rach2 in
place, and each text it replaces must be there once. tests/proofs/ptrace.sh
builds the kernels of the proofs ptrace-debug-fixture-*.json with it, whose
faults take each of its two edits out in turn.
"""

import pathlib
import sys


def edit(tree, path, old, new):
    """Replace OLD, which must be once in TREE/PATH, with NEW."""
    p = pathlib.Path(tree, path)
    text = p.read_text()
    if text.count(old) != 1:
        raise SystemExit(f"ptrace-fixture: not once in {path}: {old!r}")
    p.write_text(text.replace(old, new))


def main():
    """Put both edits of the fixture into the tree the arguments name."""
    if len(sys.argv) != 3 or sys.argv[2] not in ("i686", "x86_64"):
        raise SystemExit("ptrace-fixture: usage: ptrace_fixture.py TREE i686|x86_64")
    tree, arch = sys.argv[1:]
    if arch == "x86_64":
        entry = ("rach2/sys/arch/x86_64/isr_handler.s",
                 "syscall_entry:\n    swapgs", "syscall_entry:\n    int $1\n    swapgs")
    else:
        entry = ("rach2/sys/arch/i686/isr_handler.s",
                 "isr128:\n    cli", "isr128:\n    int $1\n    cli")
    edit(tree, "rach2/sys/arch/x86/debugreg.c",
         "    if (pc != (uintptr_t)&DEBUG_SYSCALL_ENTRY) {",
         "    if (pc != (uintptr_t)&DEBUG_SYSCALL_ENTRY + 2) {")
    edit(tree, *entry)


if __name__ == "__main__":
    main()
