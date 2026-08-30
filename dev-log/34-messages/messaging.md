# Milestone 34 migration ledger

Every kernel file that prints, and every call in it. Filled in during phase 5,
one component group at a time, written before each batch is converted.

Columns: the call's line, what it is (`record` or `console`), the level and
subsystem a record takes, whether its exact text is a test contract, and
whether it has been converted.

A file is listed here only after it has been read in full. The tool finds the
files; the reading decides what each call is.

## The files that print, and how much

Taken by counting call sites, which is how the files to read were found; the
reading itself belongs to phase 5 and none of it is done yet. 695 calls in 39
files.

    file                              k_printf k_puts k_putc s_printf s_puts s_putc
    kernel.c                                94    169     14      168      9      9
    sys/audio/audio_debug.c                  8     30      0        0      0      0
    sys/audio/dma.c                          0      0      0        1      0      0
    sys/audio/hda.c                          0      0      1        7      0      2
    sys/audio/sb16.c                         0      0      0        4      0      0
    sys/block/ahci.c                         0      0      0       20      0      0
    sys/block/ata.c                          0      0      0        2      0      0
    sys/block/block.c                        0      0      0        1      0      0
    sys/block/diskinfo.c                     0      0      0        1      0      0
    sys/board.c                              1      0      0        4      0      0
    sys/diag.c                              10      3      0       12      0      0
    sys/fs/devfs.c                           0      0      0        4      0      0
    sys/fs/krffs.c                           0      0      0       13      0      0
    sys/fs/procfs.c                          0      0      0        1      0      0
    sys/isr.c                                2      0      0        3      0      0
    sys/kio.c                                1      1      2        0      0      0
    sys/mm/heap.c                            0      0      0        1      0      0
    sys/mm/paging.c                          1      0      0        9      0      0
    sys/module.c                             0      0      0        8      0      0
    sys/mouse.c                              0      0      0        9      0      0
    sys/network/core/net_core.c              0      0      0        2      0      0
    sys/network/driver/e1000.c               0      0      0        5      0      0
    sys/network/driver/rtl8139.c             0      0      0        8      0      0
    sys/network/driver/rtl8168.c             0      0      0        4      0      0
    sys/network/ethernet/ethernet.c          0      0      0        1      0      0
    sys/network/ip/icmp.c                    0      0      0        1      0      0
    sys/network/ip/ip.c                      0      0      0        4      0      0
    sys/network/netconsole.c                 0      0      0        1      0      0
    sys/network/socket/packet.c              0      0      0        1      0      0
    sys/network/socket/raw.c                 0      0      0        1      0      0
    sys/network/socket/socket.c              0      0      0        2      0      0
    sys/network/tcp/tcp.c                    0      0      0        6      0      0
    sys/network/udp/udp.c                    0      0      0        3      0      0
    sys/panic.c                              0      2      1        0      0      0
    sys/power.c                              0      0      0       10      0      0
    sys/rtc.c                                0      0      0        3      0      0
    sys/serial.c                             0      0      0        1      1      3
    sys/syscall.c                            1      2      0        5      0      0
    sys/tty/tty.c                            0      0      1        0      0      1

Two thirds of the calls (463) are in `kernel.c`, which holds both the boot and
the debug shell. The shell's output is what the `console_*` half of the design
is for; the boot's is records. Every other file is a subsystem talking about
itself, and almost all of it is `serial_printf`, which reaches the serial port
and nothing else: no screen, no netconsole, no ring.

## Group 2: storage (read 2026-08-30)

`sys/block/ata.c`, `ahci.c`, `block.c`, `diskinfo.c`, `sys/fs/krffs.c`,
`devfs.c`, `procfs.c`. Read in full; 43 calls. No test in the tree matches any
of these texts, so none of them is a contract.

The rule the reading settles, and which the rest of the migration follows: a
call inside a function the debug shell invokes by name is console output, and
everything else is a record. The shell calls `ahci_dump`, `ahci_selftest` and
`krffs_list_files`; those three print for a person who typed a command, and
they keep their exact text.

| File | Line | What | Level |
|------|------|------|-------|
| ata.c | 78 | a command the disk policy refused | WARN |
| ata.c | 375 | the disk that answered, at boot | INFO |
| ahci.c | 323 | what is at 0:31.2 when nothing matched | DEBUG |
| ahci.c | 333 | the controller's identity and window | DEBUG |
| ahci.c | 477 | the firmware did not answer the handoff | WARN |
| ahci.c | 514 | ports enabled in PCS | DEBUG |
| ahci.c | 644 | a port whose device never became ready | WARN |
| ahci.c | 650 | a port whose signature is not a disk | DEBUG |
| ahci.c | 750 | a command the disk policy refused | WARN |
| ahci.c | 924 | the reason bring-up failed | ERR |
| ahci.c | 942 | the disk that answered, at boot | INFO |
| ahci.c | 953-987 | `ahci_dump`, a shell command | console |
| ahci.c | 998-1009 | `ahci_selftest`, a shell command | console |
| block.c | 100-104 | skipped and started, for `tests/run subset` | INFO |
| diskinfo.c | 268 | the disk `rach2.diskdiag` found | INFO |
| devfs.c | 954 | no whole-disk window to publish | DEBUG |
| devfs.c | 990 | boot partition too large | WARN |
| devfs.c | 1009 | entry 0 has the root | WARN |
| devfs.c | 1039 | no room for the loader gap | WARN |
| procfs.c | 1163 | a text past its bound, refused | ERR |
| krffs.c | 733-749 | `krffs_list_files`, a shell command | console |
| krffs.c | 1208-1404 | a file an operation could not find | DEBUG |
| krffs.c | 1664-1681 | an image that cannot be mounted | ERR |

## Group 3: memory, interrupts and syscalls (read 2026-08-30)

`sys/mm/heap.c`, `sys/mm/paging.c`, `sys/isr.c` and the diagnostics in
`sys/syscall.c` that are not the process-exit contract. 19 calls, no
contracts. A page fault that kills a process is WARN and one taken in kernel
mode is CRIT, which is the record `k_panic` follows. The two `#ifdef DEBUG`
interrupt traces became DEBUG records, which is the level they were asking
for.

Left for the cutover: `sys_exit`'s two lines, which the probe runner scores
and `tests/run exitline` matches whole. They are records with a required
substring, and the matcher has to learn the prefix before they can move.

## Group 6: network (read 2026-08-30)

The three drivers, Ethernet, IP, ICMP, UDP, TCP, the three socket families,
netconsole and the address report. 39 calls. Two are contracts and both keep
their text: `tests/run netconsole` requires `netconsole: broadcast` inside a
captured frame, and `tests/run e1000` requires `e1000: 82540EM` on the boot
line of the card-selection boot. The third selection boot wanted `RTL8139:
IRQ` and now wants `rtl8139: IRQ`, since a record's subsystem is lowercase.

The indented `    ` that made these the boot banner's detail lines is gone;
the record's own prefix is what marks them now. A frame the card refused, a
queue that filled and a checksum that failed are WARN; a link report, an
address report and a connection closing are INFO; a retransmit is DEBUG.

## Status

Group 2 is read and converted: 28 calls became records and 15 became
`console_printf`, which is every call in those seven files. `tests/run disk`,
`disk-ahci`, `persist`, `persist-ahci`, `stress`, `stress-ahci`, `diskdiag` and
`default` pass on it.

Groups 3 and 6 are read and converted, and their suites pass. Group 1
(`kernel.c`), group 4 (`board.c`, `diag.c`, `module.c`, `mouse.c`, `power.c`,
`rtc.c`, `serial.c`, `tty.c`) and group 5 (audio) are still to read.
