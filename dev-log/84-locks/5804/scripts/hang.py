#!/usr/bin/env python3
"""t5804 guest, as root, on a stall: every thread of gfx's processes with its
state, wait channel and system call, and for futex and futex_waitv the words
waited on, read through /proc/PID/mem, with their expected values and the
physical page from /proc/PID/pagemap."""
import os
import pwd
import struct
from pathlib import Path

UID = pwd.getpwnam("gfx").pw_uid
FUTEX, FUTEX_WAITV = 202, 449


def read(pid, addr, size):
    with open("/proc/%d/mem" % pid, "rb", buffering=0) as mem:
        mem.seek(addr)
        return mem.read(size)


def page(pid, addr):
    try:
        with open("/proc/%d/pagemap" % pid, "rb", buffering=0) as pm:
            pm.seek((addr >> 12) * 8)
            entry = struct.unpack("<Q", pm.read(8))[0]
    except OSError as error:
        return "pagemap: %s" % error
    if not entry >> 63:
        return "not present"
    return "pfn 0x%x" % (entry & ((1 << 55) - 1))


def main():
    print("loadavg", Path("/proc/loadavg").read_text().strip())
    for entry in sorted(Path("/proc").glob("[0-9]*"), key=lambda p: int(p.name)):
        try:
            if entry.stat().st_uid != UID:
                continue
            pid = int(entry.name)
            cmd = (entry / "cmdline").read_bytes().replace(b"\0", b" ").decode(errors="replace")
        except OSError:
            continue
        print("P %d %s" % (pid, cmd[:200]))
        for task in sorted((entry / "task").glob("[0-9]*"), key=lambda p: int(p.name)):
            try:
                stat = (task / "stat").read_text()
                comm = (task / "comm").read_text().strip()
                wchan = (task / "wchan").read_text().strip()
                sc = (task / "syscall").read_text().split()
            except OSError:
                continue
            state = stat[stat.rindex(")") + 2:].split()[0]
            print("  T %s %s state %s wchan %s syscall %s" % (task.name, comm, state, wchan,
                                                             " ".join(sc[:7])))
            if not sc or sc[0] in ("running", "-1"):
                continue
            nr = int(sc[0])
            args = [int(a, 16) for a in sc[1:7]]
            try:
                if nr == FUTEX:
                    word = struct.unpack("<I", read(pid, args[0], 4))[0]
                    print("    FX futex addr 0x%x op 0x%x expect 0x%x now 0x%x %s %s" % (
                        args[0], args[1], args[2] & 0xffffffff, word,
                        "SAME" if word == args[2] & 0xffffffff else "DIFF", page(pid, args[0])))
                elif nr == FUTEX_WAITV:
                    print("    FX waitv n %d flags 0x%x timeout 0x%x" % (args[1], args[2],
                                                                      args[3]))
                    raw = read(pid, args[0], 24 * args[1])
                    for i in range(args[1]):
                        val, uaddr, flags, _ = struct.unpack_from("<QQII", raw, 24 * i)
                        word = struct.unpack("<I", read(pid, uaddr, 4))[0]
                        print("    FX [%d] addr 0x%x expect 0x%x now 0x%x flags 0x%x %s %s" % (
                            i, uaddr, val, word, flags,
                            "SAME" if word == val & 0xffffffff else "DIFF", page(pid, uaddr)))
            except (OSError, struct.error) as error:
                print("    FX read failed: %s" % error)


if __name__ == "__main__":
    main()
