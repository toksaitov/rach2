# Host tests

`tests/run host` compiles project code as native C programs and runs it under AddressSanitizer and UndefinedBehaviorSanitizer. The suite tests pure kernel logic, utility libraries, parsers, and hardware models without booting Rach2. Guest probes and QEMU tests remain responsible for system calls, privilege changes, devices, and complete subsystem behavior.

## Runner

[`tests/system/suites/host.sh`](../../tests/system/suites/host.sh) discovers every `tests/host/*_hosttest.c` file. It uses `${CC:-cc}` with C99, `-g -O1`, `-fsanitize=address,undefined`, and `-fno-sanitize-recover=all`. The include path contains the kernel, utility, KRFFS, and compatibility-header directories.

Each test declares separately compiled project sources in one or more `host-deps:` comment lines. The runner rejects a dependency line that was wrapped without repeating the prefix, checks that every named file exists, and passes those files to the compiler. A test that includes a project `.c` file directly to reach its `static` functions leaves `host-deps:` empty.

The runner stores binaries and logs under `/tmp/rach2_verify`. The project `deadline` tool (built by `make`) runs each binary in the foreground with a 60-second limit and kills the whole process group of one that hangs. Exit status 0 passes; a nonzero exit, sanitizer report, compile failure, or timeout fails. Tests may print diagnostics.

The host suite first runs `check-libc`. This ensures that musl's installed library and extracted SHA-512 crypt source exist before the password tests compile.

## Adding a test

Name the file `*_hosttest.c` and put repository-root paths on repeatable `host-deps:` lines:

```c
/**
 * Host test for foo.
 *
 * host-deps: rach2/sys/lib/foo.c rach2/sys/lib/bar.c
 */

#include "foo.h"

int main(void)
{
    return check_foo() ? 0 : 1;
}
```

Do not wrap a dependency onto an unmarked continuation line. Native tests may use host libc. A test that directly includes kernel source must avoid host headers that conflict with the kernel's type definitions. Return 0 only after every assertion passes.

## Compatibility headers and fixtures

`compat/crypt.h`, `compat/shadow.h`, and `compat/sys/random.h` reproduce the musl declarations needed by `utils/pwauth.c`. The runner places this directory before the system include directories, so the tests receive the same declarations on hosts that lack those headers or declare the interfaces elsewhere. The test programs provide the corresponding symbols; guest builds never use these headers.

[`mkfatfixture.c`](mkfatfixture.c) independently writes the FAT32 volumes used by `bootctl_hosttest.c` and `fat32_hosttest.c`, which compile it (the bootctl test on first use, the vfat test on every run). It builds valid and malformed layouts, including one-sector and eight-sector clusters, so `utils/bootslot.c` and the kernel's vfat driver are checked against a separate implementation of the format. Its `--vfs` volume holds the shapes the driver must read: a fragmented root chain crossing a 4 KiB boundary, long names (one across that boundary, one with a surrogate pair, one with a bad checksum that must fall back to its lower-cased short name, one over a short name starting in 0x05), a deleted entry, a fragmented file, and a subdirectory whose `..` is the root with two damaged chains beside it; `--dir-loop` links that subdirectory to itself, `--active-second` turns mirroring off with the second FAT active and the first wrong, and `--unmasked` sets the reserved high bits of a chain's entries. The runner does not discover the fixture builder as a test.

## Tests

The suite contains 42 native test programs.

### Kernel foundations and memory

* `board_hosttest.c` checks power-controller profiles, PCI identities, PM bases, decode bits, SCI routing, sleep words, ACPI enable decisions, and the uninitialized board state.
* `frame_hosttest.c` checks the available-memory ceiling computed from crafted Multiboot memory maps, including reserved and truncated entries.
* `ahci_hosttest.c` runs the AHCI driver against a model of a controller, reached through the register interface `ahci.c` now has (`struct ahci_io`). Each row drives the bring-up step it is about rather than the whole of `ahci_init`, because a row that ran all of it would pass whenever anything at all refused. The rows: a controller that resets and one whose reset never completes, firmware that answers the handoff and firmware that never does, a port with a disk, a port with nothing on the link, a port whose device is not a disk, a controller with no implemented port, the port engines starting, a command that completes, one that never completes (where the wait must poll and then give up), one that returns a fatal task-file status, and a command the disk policy refuses.
* `bcache_hosttest.c` runs the buffer cache against a fake polled device whose every sector holds its own number: a hit and a miss, the least-recently-released victim named exactly, a failed read answered with no buffer, invalidation refused while held, a short last block, a block past the device refused, the two hold rules refused by panic (each observed from a forked child), and another process's holds not counting against the caller's.
* `fat32_hosttest.c` runs the vfat driver over the buffer cache over `mkfatfixture`'s volumes. The `--vfs` volume must read back exactly what the builder wrote, the long names and the bytes of every file, through the fragmented file, the empty file, and `..` back to the root; a valid FAT32 below 65525 clusters mounts; the volume whose active FAT is the second reads through it and not through the first FAT's wrong entries; entries with reserved high bits are masked; and each malformed volume is refused at mount with the field named or fails with `EIO` at the file it damages: a looped directory, a chain past the data region, a chain shorter than its size. An unknown mount option is `EINVAL` and a writable mount is `EROFS`.
* `xhci_hosttest.c` runs the xHCI driver against a model of a controller, reached through the register interface `xhci.c` has (`struct xhci_io`), the same shape `ahci_hosttest.c` uses. QEMU's controller resets when told, has no firmware holding it, and requires no scratchpad pages, so the rows are the cases neither machine produces: a controller parked in D3 that must be put back in D0 before its registers answer, an EHCI that must not be taken for an xHCI, apertures in I/O space, above 4 GiB, and never programmed, firmware that answers the handoff and firmware that answers slowly and firmware that never answers, a capability list whose steps leave the aperture, a reset that never finishes, a controller that never becomes ready, a scratchpad request larger than the kernel has, the command ring wrapping through its Link TRB with the cycle bit following, a command nobody answers that is given up on and whose ring is reprogrammed once the controller has stopped it, a completion for another command, the event ring read to its wrap with the dequeue pointer written back, a connect settled only after the debounce interval, a reset that keeps a connect nobody has handled yet, a port past the controller's own count, Enable Slot, Address Device, Configure Endpoint, Reset Endpoint, Set TR Dequeue and Evaluate Context each checked against the command the controller receives, the last slot the controller may give out, the Intel port routing that moves every pin the chipset allows to this controller while another maker's configuration space is left alone, and transfers on an endpoint ring with their setup, data and status blocks, a completion reported for its device and endpoint, and a short answer reported with what did not arrive.
* `tcp_hosttest.c` drives `tcp.c` segment by segment, with the IP layer replaced by a recorder and the clock a variable. It checks the control-block pool filling and refusing (which no guest probe can reach, since every block is owned by a socket and both pools hold sixteen), a SYN for a port nobody listens on answered with a reset and a reset for one not answered, a handshake, data in order delivered once, the same data again not delivered twice but acknowledged again, a segment past a hole not delivered while the acknowledgement still asks for the hole, the hole filled, a window closing and reopening, a peer's FIN, and a peer that stops answering until the retransmission gives up.
* `packet_hosttest.c` hands `ethernet.c` and `ip.c` frames no peer would send: an Ethernet header cut short, a version that is not 4, a header length below the minimum and past the end of the frame, a total length past the frame and below the header, a checksum that does not sum, a fragment, and a datagram addressed elsewhere. Each frame is copied into an allocation of exactly its length, so a read one byte past it is a read past the allocation and AddressSanitizer says so. It also checks that a whole datagram reaches the parser for its protocol and that packet sockets see it either way.
* `klog_hosttest.c` drives the kernel log against recorders in place of its four destinations. It checks the record's shape and every conversion, one newline however the format ended, the truncation mark, the level each destination takes, the order they are written in with a serial port that never drains, whole-record eviction when the ring wraps, that console output reaches the screen and the port and neither the ring nor the wire, and that a record raised from inside the delivery of another leaves both whole.
* `krandom_hosttest.c` checks deterministic reseeding, seed separation, byte filling, the all-zero seed, and a fixed monobit sanity case for the ChaCha20 stream.
* `kstd_hosttest.c` checks kernel integer formatting across bases, signed limits, buffer limits, return counts, and invalid radices.
* `kstring_hosttest.c` checks the kernel string, memory, case-conversion, reverse, and overlapping `memmove` routines.
* `modlayout_hosttest.c` checks boot-module bounds and overlap rules, accepted edge placements, and bounded diagnostic formatting.
* `rtc_hosttest.c` checks CMOS BCD, binary, and 12-hour decoding; invalid clock state; leap dates; stable samples; and time-from-anchor arithmetic.
* `vm_hosttest.c` checks VMA insertion, lookup, overlap detection, removal and splitting, and bounded stack growth.

### Processes, files, and I/O

* `block_hosttest.c` checks partition-view bounds, LBA translation, zero-length rejection, and clamping to the backing device.
* `diskcmd_hosttest.c` checks all 256 ATA command bytes under read-write and read-only policies; exactly four commands are allowed by the read-only policy.
* `diskinfo_hosttest.c` checks ATA string decoding, control-byte filtering, 48-bit sector-count formatting, raw-window bounds, and `/proc/disk` rendering.
* `elf_hosttest.c` checks that malformed ELF headers and load segments are rejected before page allocation.
* `hid_hosttest.c` hands the boot-protocol class driver reports and reads what it gave the input layer, since none of these can be produced from outside a machine: a key held down across reports, two down at once, one released while the other stays, a right-hand modifier and an arrow in their two-byte form, a rollover report whose keys are dropped while its modifiers are still read, a key still down after a rollover that must not be pressed again, a report shorter than one, a device that goes while a key is held, a second keyboard whose first report is a press rather than a hold, and a mouse's movement, buttons and inverted vertical axis.
* `mbr_hosttest.c` checks MBR parsing, FAT boot-partition selection, disk and overlap bounds, and whether partitions leave the post-MBR GRUB area clear.
* `n_tty_hosttest.c` checks canonical editing, EOF and signal characters, input and output mappings, raw `VMIN` behavior, hangup, echo, and buffer limits.
* `pagecache_hosttest.c` checks cache hits, eviction, dirty writeback, sync failure recovery, and capacity reduction when dirty pages cannot be flushed.
* `pipe_ring_hosttest.c` checks FIFO capacity, partial transfers, full-page transfers, and byte order across ring wrap.
* `poll_select_hosttest.c` checks `fd_set` conversion, `pollfd` projection, word boundaries, direction-specific readiness, and ready-count semantics.
* `readfile_hosttest.c` checks KRFFS regular-file path lookup and refusal of malformed, truncated, or overflowing node regions.
* `synthfs_hosttest.c` checks `/dev` and `/proc` path classification, slash handling, PID and descriptor bounds, and malformed synthetic paths.
* `tls_hosttest.c` checks `user_desc` validation and TLS base resolution for `set_thread_area`.
* `uimage_hosttest.c` checks the i386 initial stack, argument and environment arrays, auxiliary vectors, random bytes, `AT_SECURE`, alignment, and oversized input.
* `usb_hosttest.c` walks configuration descriptors in buffers allocated to exactly their length, so a read past the end is a failure rather than a byte of whatever follows. It checks a boot keyboard's interface found with its configuration value, class, endpoint and interval; an unknown descriptor stepped over by its length; interfaces refused for their class, their missing boot protocol, an alternate setting, a configuration numbered zero, or an endpoint that reports nothing; and descriptors that give a length of zero, run past the buffer, or stop short of their own header.
* `wait_queue_hosttest.c` checks fixed-slot enqueue, idempotent dequeue, queue capacity, and wake-all state changes.

### Network

* `arp_hosttest.c` checks ARP cache filtering, replacement and use order, counter wrap, mapping changes, request replies, and network-lock balance.
* `dhcpmsg_hosttest.c` checks DHCP discover and request construction, IPv4 and UDP validation, reply parsing, hostile options, lease validation, and renewal arithmetic.
* `netcfg_hosttest.c` checks the `/etc/network/interfaces` reader for DHCP and static stanzas, selection rules, limits, malformed files, and address validation.
* `netconf_hosttest.c` checks boot-line network parsing, address and gateway validation, live reconfiguration, provisional masks, cache invalidation, and `rach2.ip=none`.
* `netconsole_hosttest.c` checks frame fields and checksums, buffering, sequence numbers, ring overflow, complete draining, and reentrant transmit diagnostics.
* `rcvwnd_hosttest.c` checks TCP receive-window update thresholds, silly-window avoidance, 16-bit clamping, and invalid geometry.
* `rtl8168_hosttest.c` runs the RTL8168GU driver against a register model. It checks supported identity, initialization order and values, indirect register protocols, bounded handshakes, transmit and receive descriptors, ring wrap, and lost-doorbell recovery.
* `e1000_hosttest.c` runs the Intel 82540EM driver against a register model. It checks the bring-up sequence and its order, the EEPROM read protocol and the station-address registers preferred over it, refusal of an unusable address, each of the four receive checks failing on its own, ring wrap, the transmit descriptor contract, the bounded stall, the stop sequence, the link report, and that a card that is not in the machine costs no register access.
* `netdev_hosttest.c` runs the driver table against three models whose verdicts it sets. It checks the probe order, that an absent card is stepped over and a failed one stops the walk and is silenced, which reason is kept, and what the interface answers with nothing bound.

### Boot and host utilities

* `bootctl_hosttest.c` checks FAT32 discovery, kernel and fallback validation, candidate installation, environment and menu updates, cluster geometry, readback, and interrupted, dropped, or torn writes.
* `bootverb_hosttest.c` checks every `bootctl` verb, operand count, returned operand, rejection of the old spelling, and exact name matching.
* `grubslot_hosttest.c` checks the two GRUB core-image tails, blocklist validation and patching, alternating installation, slot clearing, and readback failure.
* `mkhash_hosttest.c` checks the vendored musl SHA-512 crypt against published vectors and rejects a malformed setting.
* `pwauth_hosttest.c` checks password comparison, salt-byte mapping, credential-file replacement, locking, flush failures, fork and exit cleanup, signal state, and terminal teardown.
* `pwauthatexit_hosttest.c` checks that a transaction fails and leaves no lock when the library cannot register both exit handlers.
* `sectorcopy_hosttest.c` checks decimal size parsing, 64-bit disk bounds, whole-sector streaming, short and long input, partial I/O, errors, and 64-sector write batches.
