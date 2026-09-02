# Milestone 43 hardware

What the Lenovo G50-70 reports in this session, beside what QEMU reported
when each check was written. The seven earlier `hardware.md` files, from
milestones 36 to 42, are the procedures; this file is their results.

## Arrival

Read over SSH from the kernel the machine booted on arrival, before any
install.

| Reading | Value |
|---------|-------|
| Address | 192.168.0.106 |
| Boot line | `rach2.root=disk rach2.slot=candidate rach2.disk=ahci rach2.netconsole rach2.ip=none` |
| `bootctl status` | candidate at sector 312, 4096 sectors, 2097152 bytes; fallback at sector 4408, 1632 sectors, 834472 bytes; saved_entry candidate; next_entry empty; booted candidate |
| `/proc/mounts` | `/dev/disk0 / krffs rw,relatime`, devfs, proc |
| `/proc/usb` | port 2 low 46d:c34b class 3 protocol 1 (keyboard); port 3 low 46d:c077 class 3 protocol 2 (mouse) |
| `/dev` | bootlog bootpart console full grubcore null ptmx random tty urandom zero |

The arrival kernel predates milestone 36: it has no partition nodes, so
the hashes of the root and recovery partitions are read after the first
install.
