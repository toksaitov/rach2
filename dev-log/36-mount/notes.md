# Milestone 36 notes

## Phase 1 inventory

Base: e0279eb on master, clean, 54 ahead of origin, never pushed.

Call sites in syscall.c (grep, exact): krffs_resolve_x 20, krffs_resolve 6, krffs_read 5, krffs_stamp_new_node 4, krffs_fill_stat64 4, krffs_node_path 3, krffs_chown_node 3, krffs_chmod_node 3, then truncate/stamp/open/create 2 each and nine singles. synth_classify 14, synth_stat 10, procfs_resolve_pid 9, synth_is_synth 8, devfs_stat 4, synth_open_dir 3, the rest 1-2.

Migration order (plan's, confirmed by the counts): lookup/stat/access first because resolve_x dominates; then open/creat/openat; then descriptor I/O; then namespace changes; then rename/link; then statfs/sync/chdir/getcwd/execve.

Lexical `..` dependents to re-check at the flip commit: tests/pathcanon.c (creates /cx first, so verdicts hold under physical resolution; the shadow-prevention claim moves from spelling to resolved node), tests/dirprobe.c chdir "..", tests/dirtest.c root parent. No test found that sends `..` through a missing directory; vfsprobe adds those rows.

Suite disk confirmed by hand: krffs/mkdisk.sh 64/48, then mkmbr --lba IMG 2048:98304:7f 100352:28672:0c, then mformat -i IMG@@51380224 -F; mcopy/mmd/mdir work, long name stored with alias ALONGF~1.TXT; the volume is FAT32 with 1-sector clusters and well under 65525 clusters, which exercises the recognition rule.

Review units for the round after phase 5 (fixed now, max five at once): 1 block.h/block.c/request pool + lag device; 2 bcache.c + bcache_hosttest; 3 vfs.h/vfs.c + descriptor fields in fd.h/syscall.c; 4 fat32.c/h + fat32_hosttest + mkfatfixture; 5 mount/umount2 + claims + synthfs surface; 6 probes and suite (vfsprobe, mountprobe, lagprobe, mount.sh); 7 proofread of reader-facing docs and comments. Seven units, at most five concurrent, plus re-reviews only for contract changes.

## Open items

(none)

## Phase 2 record

The lag device delivers every completion twice, two ticks apart, and the
second copy claims an error; a kernel that believed the duplicate would fail
a healthy request, which is what makes the generation check observable from
a boot. The sleep boot logs the drops.

The deadline boot first appeared to hang the machine. It did not: the
harness drains serial only while steps run plus a short grace (3 s), and the
deadline sequence ends about 13 s after its exec, so the output never
reached the log. Suites that read a slow result must hold an expect step
open across it; mount.sh's lag boots each end with expect=LAGPROBE OK.

The fixture boot partition is 512 KiB; lagprobe's write row uses 256 KiB in.

Phase 2 mutations, proved once each through the restoring script (backup,
edit, touch, rebuild verified by the kernel mtime, one check, restore):

* complete without waking the sleeper: caught by the sleep boot
* free a timed-out slot at once: caught by the deadline boot (no late
  completion line)
* accept a stale generation: caught by the sleep boot (poisoned duplicate)
* clear the fault-safe flag on AHCI: caught by tests/run disk-ahci (root
  refused)
* publish a node for an empty MBR entry: caught by the sleep boot
  (/dev/disk0p3 open must fail)
* fifth held buffer without panic: caught by bcache_hosttest
* lower block while holding a higher one without panic: caught by
  bcache_hosttest

The script is kept with the milestone record below for reuse in later
phases; it lives outside the tree while phases run.

## Open items for phase 3

* ftruncate(93): added by the descriptor I/O family through the vfs
  truncate operation, with a vfsprobe row. truncate(92) stays out; nothing
  in the userland uses it.

## Phase 3 sequencing notes

* The lexical `..` step is gone; `..` resolves physically and the marked
  vfsprobe rows flipped in the same commit. Namespace changes now answer
  for mount points through `vfs_leaf_mount` (EEXIST for mkdir, EBUSY for
  unlink, rmdir and rename), which is what keeps a KRFFS /dev from
  shadowing the synthetic mount without a lexical guard.
* The held working-directory reference (`cwd` pinning its mount) lands in
  phase 5 with mount(2), where the reference count first has a mount that
  can be unmounted and mountprobe proves the EBUSY. The canonical cwd
  string stays for getcwd, as the plan says.
* The open-per-description mutation waits for phase 5's unmount record,
  which is the check that catches it.
* creat(2) is now open(O_WRONLY|O_CREAT|O_TRUNC) and nothing else, so
  creat on a device node succeeds as it does on Linux; the old creat refused
  every synthetic path with EROFS. ISH implements `>` with creat, so a shell
  redirect into /dev/null works now, and the drill's refusal boot observes
  the write refusal (busybox cat's "write error") instead of ISH's failed
  open.
