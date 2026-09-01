# Milestone 37 notes

## Phase 1 inventory

Base: a clean tree on master at the close of milestone 36.

```text
207fb2a Add the full set's time to the milestone 36 record
64829b4 Record milestone 36
7262391 Correct the mount documents and the VFS header against the code
b1bd997 Remove the KRFFS walk predicate and resolve readlink through the mount table
43fbb0e Record the review round and the contracts it changed
1f5c1d9 Accept a debug-exit status under --ok and expect EISDIR for creat of the root
8054353 Make the probe rows prove what their names claim
70c9380 Validate long names, times and options in vfat and prove them on the host
813b0aa Retry failed writeback and check buffer ownership in the cache
889a956 Fix the review findings in the VFS and the syscall layer
cacbdd7 Own a request by generation, claim ranges on the physical disk, and stage lagged reads
ba29673 Describe the mount table, the vfat type and the block request in their Readmes
```

## Review checkpoints (fixed now; at most two units run at once)

Phase 2: unit A, the cache's write half: bcache.c/h, block.h, lag.c,
bcache_hosttest.c, the init updater. Phase 3: unit B, the writer's order of
writes and lock: fat32.c/h, vfs.c's lock change, fat32_hosttest.c's
sequences and matrix; unit C, the check and the dirty-volume repairs:
mkfatfixture.c, fatcheck_hosttest.c, the mount-time check in fat32.c. Phase
4: unit D, names and attributes: the name code in fat32.c,
fatname_hosttest.c, vfsprobe.c. Phase 5: unit E, the suite and the
rehearsal: fatwrite.sh, the probes, lagprobe.c, the recovery ISO target,
metal_accept.sh; unit F, the proofread of the reader-facing documents. Six
units, two at a time, plus re-reviews only for contract changes.

## What mtools writes (measured in phase 1)

`mformat -C -F -T 28672` (28672 sectors, one-sector clusters): 32 reserved
sectors, two FATs of 221 sectors, mirroring on, root cluster 2, FSInfo in
sector 1 with the exact free count and the hint at the next free cluster,
both dirty flags clear.

| Copied as | Stored | Case byte |
|-----------|--------|-----------|
| `hello.c` | `HELLO   C  `, no long entry | 0x18 (base and extension lower) |
| `Mixed.c` | one long entry, alias `MIXED   C  ` | 0x00 |
| `long name.txt` | one long entry, alias `LONGNA~1TXT` | 0x00 |
| `longname1.txt` | one long entry, alias `LONGNA~2TXT` | 0x00 |
| `longname2.txt` | one long entry, alias `LONGNA~3TXT` | 0x00 |
| `UPPER.TXT` | `UPPER   TXT`, no long entry | 0x00 |
| `Upper2.TXT` | one long entry, alias `UPPER2  TXT` | 0x00 |
| `Sub` (directory) | one long entry, alias `SUB        ` | 0x00 |

So mtools keeps the Windows NT rule for the short-only case (a name whose
two parts are each all upper or all lower is stored short, with the case
bits), writes a long entry otherwise, and numbers colliding aliases in
creation order (`~1`, `~2`, `~3`), not by any property of the name. This
driver's alias rule (phase 4) is recorded against this table.

## Standing rule changed by the owner

The plan's rule of five concurrent delegated units is superseded: at most
two run at once (CLAUDE.md, Agents), set during milestone 36's review round.

## Open items

(none)
