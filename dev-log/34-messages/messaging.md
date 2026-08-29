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

## Status

Not started. Phase 5 opens this file and reads each of these in full.
