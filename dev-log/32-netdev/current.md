# Milestone 32: resume state

Phase: 2, the driver. Complete.

Done: `rach2/sys/network/driver/e1000.h` and `e1000.c` written against the
interface as it stands, added to `rach2/sources.mk`, and placed second in the
`netdev.c` table. Evidence: `make` clean with `-Werror`; a boot with `-device
e1000,netdev=net0` binds the card, reads MAC 52:54:00:12:34:56, and reports
link 1000 Mb/s full duplex; `/pingtest` reports `PINGTEST OK` over it, so
transmit, receive, ARP, and ICMP all work; a boot with no `--device` still
binds the RTL8139; `tests/run style`, `tests/run host`, and `tests/run
default` (63/63) all pass. Kernel grew text +4062 and bss +115968 from the
phase 1 baseline, the second matching the rings and buffers the plan sized.

Next: phase 3, the host model and the suite. Write
`tests/host/e1000_hosttest.c`, then give the `dhcp` target and `ssh.sh` a card
parameter, then add the `e1000` suite.
