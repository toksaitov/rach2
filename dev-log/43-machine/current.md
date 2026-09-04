Milestone 43, phase 7: the differences. Handoff written 2026-09-04 for
a session that may be picked up by Codex; the procedures for working
the machine from the host are in Readme.md under "Updating the
installed machine" and "Working the machine from the host"; read notes.md's Phase 1 to
Phase 5 differences and Open items for what is fixed and what is not.

The machine: the Lenovo runs kernel e9246422... (commit bb3f628's tree)
on the ext2 root as the saved default, installed and committed through
bootctl with the owner's approval; the KRFFS root is the fallback. The
hub with the keyboard, the mouse and the recovery stick is in the blue
socket. The stick carries the recovery image with the RACH2SAVE volume
holding saved copies. Freedoom is /doom1.wad on the machine and the
shareware WAD /shareware.wad; the tree stages the shareware one, so
swap them back at the end. The ordinary kernel publishes no raw disk
window without rach2.diskdiag, so partition reads and writes go through
recovery: `bootctl trial recovery && reboot` from the machine, which
boots the disk's recovery entry, a RAM root at the same address with
its own SSH host key each boot. Reach every system as
`ssh -i krffs/rach2_demo_ed25519 root@192.168.0.106` with
`-o UserKnownHostsFile=/tmp/k -o StrictHostKeyChecking=accept-new`,
since each root has a host key of its own; a stick boot takes
192.168.0.105. The owner's sudo on the mac mini reads its password from
/Users/toksaitov/Desktop/p.txt with `sudo -S -p ''`. A capture of the
machine's netconsole frames, `tcpdump -i en0 -U 'ether proto 0x88b5'`
on the Mac, was running when this was written and is not part of the
tree; start one before the next machine round so a panic keeps its
record. The BIOS clock is still local time; the owner sets it to UTC
when at the BIOS.

Fixed on the host and installed, each with a row and a mutation: the
xHCI's PCI line under nomsi (21 MB/s on the machine); the wait's torn
8254 sample behind the clock failures; the FAT32 run's entries by
block and the blank cache buffer (2.2 MB/s on the machine); the
SuperSpeed hub after a Codex review, proved with the stick behind it;
a warm reset ended by a connection change; the configuration request
given five seconds; a command past its deadline aborted from the
clock and the turn taken only on a free ring (the boot that lost every
USB device); a silent stick handed back for a port reset; the Doom
zone from free memory through brk.

Open, in order: the Doom port's host row over a two-track MIDI
fixture (the multi-track player is in and proved on the machine); the
panic at a pull of a silent stick, awaiting its record from the
netconsole capture; root's SSH key refused after a
stick boot, to reproduce with a second dropbear started with -E from a
test-account su session; the RTC set to UTC by the owner; the shareware
WAD put back as /doom1.wad on the machine (the machine's /bin/doom is
the multi-track build, ahead of the ext2 image the tree last wrote).
Then phase 8, begun: the machine built its kernel, utilities and shell
from the staged sources in seconds (hardware.md); the ext2 root must
be rewritten from the current tree (a partition 4 write, approval by
name) so the staged sources are current, then the build repeated and
the kernel it builds trial-booted through bootctl and committed; phase 9, the record and the sweep, with a Codex
review per phase and the full test set once.
File being read when written: none.
