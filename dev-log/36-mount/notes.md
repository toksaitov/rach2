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

## Phase 3 record

Phase 3 mutations, proved once each through the restoring script (the same
shape as phase 2's):

* /procx matched as /proc: caught by vfsprobe
* search permission skipped: caught by searchprobe. The plan named the
  authority suite, which never walks a denied directory; searchprobe in
  the shared boot is the check that does.
* `..` resolved lexically: caught by vfsprobe's marked rows
* a description's path re-resolved on read: caught by vfsprobe
* relocation repointing dropped: persist's growth check did not catch it,
  so vfsprobe gained a row (a second description on a file a write
  relocates must see the grown file); caught by that row
* open per descriptor instead of per description: waits for phase 5's
  unmount record, which is its check

The full set ran on the migrated tree: the regressions it found (mkdir of
the root answering EINVAL to `mkdir -p`, searchprobe pinning the lexical
`..`, the drill leaning on creat refusing a device) are fixed above; usb
failed only inside the parallel full run and passes alone, which is
investigated before the phase closes.

## Phase 4 record

The driver reads through the buffer cache one block at a time, so the
cache's hold rules are met by construction. Two decisions beyond the plan's
text: a lookup matches the short alias as well as the long name, as vfat on
Linux does, and the chain walk bounds the cluster index by the cluster
count, because a looped directory whose clusters are full never reaches a
zero entry and a per-call step count restarted from the node's hint would
never see the loop. A chain that ends before the index is ENXIO inside the
driver: a directory reads it as its end, a file as damage (EIO).

mkfatfixture gained `--vfs`, `--dir-loop`, `--active-second` and
`--unmasked`; every earlier case is byte for byte what it was, and mtools
lists the `--vfs` volume as the builder meant it (an independent reader).

Phase 4 mutations, proved once each through the restoring script (host
compile per mutant, each run under the deadline tool so a hang fails):

* refuse a valid FAT32 below 65525 clusters: caught (the small-volume row)
* skip the chain bound: caught (the looped directory hangs, and the
  deadline fails it)
* ignore the long-name checksum: caught (the bad-checksum name appears)
* read the first FAT whatever the active one: caught (the active-second
  volume's file reads wrong)
* accept a root cluster past the data region: caught (mount succeeds)
* take an unmasked FAT entry at face value: caught (the unmasked chain
  fails)
* answer `..` with the directory itself: caught (the `..` rows)

A full run started while phase 4 sources were being edited is void: the
pooled targets rebuild the kernel from the working tree, so forty suites
failed in a second each on a tree that did not compile. The full set runs
only on a quiescent tree; the run owed after phase 3 comes with phase 4's
gate.

## Phase 5 record

What the rows found, and what was decided:

* The suite's FAT volume must be formatted to its partition's size
  (`mformat -T 28672`); mtools otherwise sizes the volume to the rest of
  the image and the driver refuses total sectors past the partition.
* The boot-fixture disk's partition is a byte pattern, not a FAT volume, so
  the claim boot uses the suite's disk with its MBR entries swapped: entry
  0, the boot partition, is the FAT volume, and /dev/bootpart and
  /dev/disk0p1 name one range under two names, which is what the claims
  compare.
* vfat entries report 0777 less the umask, as vfat on Linux does; the plan's
  "0644 for files" would have made nothing on a volume executable, and the
  noexec row (a mode that would allow it) needs the Linux rule.
* The busy-buffer row found a cache defect: a buffer was keyed and held
  only after its fill, so two concurrent misses for one block both filled,
  two buffers for one block. The buffer is now claimed before the request.
* /proc/disk prints the cache counters whatever the disk's state, since a
  mount can be on any device the cache reads.
* The common layer learnt a second type: stat, access, open, chdir, chmod,
  chown, utimensat, the namespace changes, statfs and execve read a node on
  a mount that is not the root through its getattr and answer through the
  vfs wrappers; the root's nodes keep their KRFFS fast paths. execve loads
  an ELF image on another type through the mount; a script there answers
  ENOEXEC, because the shebang reader is the root's.
* The "vfsprobe through a mount" rows live in mountprobe: the read-only
  mount refuses the write-side rows, and the read-side rows (ENOENT,
  ENOTDIR, `..` out of the mount and back, openat through a directory
  descriptor, statfs) run there.
* fcntl is syscall 221 and setuid 213 on this kernel; the 16-bit numbers
  are not implemented, which mountprobe learnt the hard way.
* The mutation "open per descriptor instead of per description" is proved
  here through the unmount record (mount.sh's awk over the records).

Phase 5 mutations, proved once each through the restoring script (kernel
rebuilt per mutant, verified by its mtime, one boot each):

* umount2 with a description open in another process: caught by mountprobe
* a working directory inside an unmounted mount: caught by mountprobe
* MNT_DETACH accepted: caught by mountprobe
* rename across mounts: caught by mountprobe
* the raw writer opened over a mounted range: caught by the claim boot
* a second mount over a claimed range: caught by mountprobe
* a program run from a noexec mount: caught by mountprobe
* rmdir of a target while the constructor sleeps: caught by the lag mounted
  boot
* two processes reading one description without the lock: caught by the
  every-byte-once row of the lag mounted boot
* the busy-buffer wait dropped: caught by the counter row of the lag
  mounted boot

The phase gate: style, host, default, authority, bootpart, diskdiag,
procfiles and mount green on the tree before the commits.
