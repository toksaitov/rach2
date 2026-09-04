Milestone 43, phase 7: the differences. Handoff written 2026-09-04 for
a session that may be picked up by Codex; the procedures for working
the machine from the host are in Readme.md under "Updating the
installed machine" and "Working the machine from the host"; read notes.md's Phase 1 to
Phase 5 differences and Open items for what is fixed and what is not.

The machine: the Lenovo runs the kernel it built itself from the
staged sources of the current tree, `8ed3c0f8...` through TinyCC, on
the ext2 root written from that tree, both committed with the owner's
approval; the KRFFS root is the fallback, and `bootctl kernel
build/rach2/kernel` would put the host's build back. The
hub with the keyboard, the mouse and the recovery stick is in the blue
socket. The stick is the recovery stick, `f530085a...`, with an empty
RACH2SAVE volume; every saved copy is on the host under
`dev-log/43-machine/saved/`. Freedoom is /doom1.wad on the machine and the
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
record. The BIOS clock holds UTC since 2026-09-04.

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

Phases 1 to 9 are closed. The machine runs its own build of the final
tree, `77b78018...`, on the ext2 root `9c20cabd...`, with the KRFFS
root as the fallback and a recovery partition that has `e2fsck`; the
acceptance script passes against it, and the stick beside it is the
recovery stick with an empty save volume. The full set, the documents'
review and every difference's row are done and recorded in notes.md.

Open, and each recorded there: the panic at a pull of a stick that had
stopped answering, which did not recur and whose record the netconsole
capture on the Mac would keep; and the frame exhaustion under the lag
device, seen once in phase 1 and not reproduced in two trials with the
record in place. Both wait on the machine showing them again. The
milestone's own Readme has the trust statement.
File being read when written: none.
