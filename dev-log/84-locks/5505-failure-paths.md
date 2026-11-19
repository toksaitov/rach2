# KRFFS disk I/O failures

Line numbers refer to the task branch before these fixes, at a077f3d0d5. A reader means a caller after the mount lock is released. Disk includes only completed durable writes. A cache store changes what readers see immediately. After a failed device write or flush any submitted sectors might already be durable. No cleanup can infer which ones. A barrier orders earlier writes before publication. The disk root's ordinary sync point is empty. Its barrier waits for the page cache. On volumes both operations sync the buffer cache and flush the device.

Each row names the failing operation, the disk and cache state, the answer at entry to this task, and the required action. Repeated calls to a primitive inherit its row. A prefix counts caller payload below the published end of file, excluding a zero-filled hole. A moved file has one live offset in the running mount. A halt sets the read-only state before further work. A device request already submitted may complete.

## Linux 6.12 comparisons

The source keys in the tables identify the following functions and their results.

* **W**. The buffered write loop counts the bytes accepted by write_end and returns that count on a later error. Ext4 changes the in-memory inode size under the folio lock. [mm/filemap.c, generic_perform_write](https://github.com/torvalds/linux/blob/v6.12/mm/filemap.c#L3772-L3855), [fs/ext4/inode.c, ext4_da_do_write_end](https://github.com/torvalds/linux/blob/v6.12/fs/ext4/inode.c#L2793-L2845).
* **A**. Ext4 maps logical file blocks and propagates allocation and mapping errors. Extents and inode metadata have journal transactions. There is no relocation of a named inode when it grows, nor a ring, backward link or sector-split inode size. [fs/ext4/inode.c, ext4_map_blocks and ext4_mark_inode_dirty](https://github.com/torvalds/linux/blob/v6.12/fs/ext4/inode.c).
* **T**. Ext4 changes size through setattr and removes blocks through truncate. Later errors can follow a change of the in-memory inode. [fs/ext4/inode.c, ext4_setattr and ext4_truncate](https://github.com/torvalds/linux/blob/v6.12/fs/ext4/inode.c).
* **O**. Ext4 recovers numbered orphan inodes. Its legacy list walk stops on an unreadable inode. The orphan-file walk can skip that entry and continue. KRFFS must stop because the next node's address depends on the current chain. [fs/ext4/orphan.c, ext4_orphan_cleanup and ext4_process_orphan](https://github.com/torvalds/linux/blob/v6.12/fs/ext4/orphan.c#L356-L458).
* **S**. Fsync waits for file data, commits metadata and checks the writeback error. A forced shutdown returns EIO. A failed flush gives no assurance of durability. [fs/ext4/fsync.c, ext4_sync_file](https://github.com/torvalds/linux/blob/v6.12/fs/ext4/fsync.c).
* **H**. Ext4 rejects new writeback on forced shutdown. Its response to other errors depends on the mount's error policy. KRFFS has one policy for an incomplete orphan scan. [fs/ext4/inode.c, ext4_writepages](https://github.com/torvalds/linux/blob/v6.12/fs/ext4/inode.c#L2621-L2634), [fs/ext4/super.c, ext4_handle_error](https://github.com/torvalds/linux/blob/v6.12/fs/ext4/super.c#L644-L708).
* **C**. A failed folio read gives an error. Reclaim can retain a dirty folio and leave its write to writeback. Linux has no KRFFS window or fixed block-buffer reserve. [mm/filemap.c, filemap_read](https://github.com/torvalds/linux/blob/v6.12/mm/filemap.c), [mm/vmscan.c, shrink_folio_list](https://github.com/torvalds/linux/blob/v6.12/mm/vmscan.c).
* **M**. Mount validates metadata, runs recovery, and unwinds its private state on failure. Releasing the block superblock includes syncing and releasing the block device. Successful earlier recovery I/O can survive a subsequent failure. [fs/ext4/super.c, __ext4_fill_super and ext4_put_super](https://github.com/torvalds/linux/blob/v6.12/fs/ext4/super.c), [fs/super.c, kill_block_super](https://github.com/torvalds/linux/blob/v6.12/fs/super.c#L1606-L1615).
* **P**. Permission removal uses the underlying inode, including through a bind. Truncation requests the size change and privilege removal together. [fs/open.c, do_truncate](https://github.com/torvalds/linux/blob/v6.12/fs/open.c#L36-L63), [fs/ext4/file.c, ext4_buffered_write_iter](https://github.com/torvalds/linux/blob/v6.12/fs/ext4/file.c).
* **N**. Ext4 propagates a failed directory lookup. A held Linux file already has a path. Procfs propagates the link operation's error. [fs/ext4/namei.c, ext4_lookup](https://github.com/torvalds/linux/blob/v6.12/fs/ext4/namei.c), [fs/proc/base.c, proc_pid_readlink](https://github.com/torvalds/linux/blob/v6.12/fs/proc/base.c#L1710-L1749).


## Write and truncate

Locations are in `krffs.c` unless qualified.

| Row | Function, line and operation | Disk, cache and reader | Current answer | Linux | Required action |
| --- | --- | --- | --- | --- | --- |
| W01 | `krffs_write_at` 2151 header | Both retain old file. | Bad node, mapped to EIO. | W | Return an I/O error for an unreadable header. |
| W02 | `krffs_settled` 496 prior barrier | Prior metadata is cached, disk uncertain. Old file is live. | EIO | S | Complete the barrier before reusing bytes. |
| W03 | `krffs_grow_in_place` 2014 current header | Old chain and file. | EIO | A | Stop before growth. |
| W04 | `krffs_grow_in_place` 2024 successor header | Old chain and file. | EIO | A | Stop before growth. |
| W05 | `krffs_grow_into` 1930 entry barrier | Old length. A previous shrink or unlink may still be dirty. | EIO | A | Keep old allocation until its boundary is durable. |
| W06 | `krffs_grow_into` 1934 split feasibility | Old length and forward chain. Failed comparison can select a larger allocation or a move. | Fallback | A | Use the size already read from the header for the comparison. |
| W07 | `krffs_grow_into` 1944 whole-node feasibility | Old length and forward chain. Failed comparison can select a larger allocation or a move. | Fallback | A | Use the size already read from the header for the comparison. |
| W08 | `krffs_grow_into` 1937 new tail header | Old chain. The unlinked tail may be cached or durable. | EIO | A | Keep the old node live. |
| W09 | `krffs_grow_into` 1938 tail barrier | Old chain. The unlinked tail may be cached or durable. | EIO | A | Keep the old node live. |
| W10 | `krffs_grow_into` 1938 split size | Old file length. The new forward size publishes the prepared chain. | EIO, links stale | A | Publish a whole field or leave the old size. |
| W11 | `krffs_grow_into` 1947 whole-node size | Old file length. The new forward size publishes the prepared chain. | EIO, links stale | A | Publish a whole field or leave the old size. |
| W12 | `krffs_grow_into` 1941 split backward link | Forward size changed. The link is derived and can be rebuilt. | Stale links, grow continues | A | Use forward walks until repair. Keep a failed field store whole. |
| W13 | `krffs_grow_into` 1950 whole-node backward link | Forward size changed. The link is derived and can be rebuilt. | Stale links, grow continues | A | Use forward walks until repair. Keep a failed field store whole. |
| W14 | `krffs_grow_into` 1942 split barrier | Cached room is larger. Disk may still reference old free header. No new payload visible. | EIO, unsettled | A | Settle before overwriting the old header. |
| W15 | `krffs_grow_into` 1951 whole-node barrier | Cached room is larger. Disk may still reference old free header. No new payload visible. | EIO, unsettled | A | Settle before overwriting the old header. |
| W16 | `krffs_allocate_err` 1512 first-fit header | Old file live. No new reservation. | EIO or ENOSPC after a sound full walk | A | Distinguish failed read from exhausted space. |
| W17 | `krffs_take_free` 1342 reuse barrier | Old file live. Any new tail is still inside free space. | EIO | A | Stop before publishing split. |
| W18 | `krffs_take_free` 1348 tail header | Old file live. Any new tail is still inside free space. | EIO | A | Stop before publishing split. |
| W19 | `krffs_take_free` 1349 tail barrier | Old file live. Any new tail is still inside free space. | EIO | A | Stop before publishing split. |
| W20 | `krffs_take_free` 1352 split feasibility | Old file live. An unreadable old field can cause the whole extent to be taken. | Fallback | A | Compare the known free size. |
| W21 | `krffs_take_free` 1355 split size | Free space is split only after the tail is durable. Readers use old file. | EIO for size, stale links for successor | A | Keep publication whole. Derive stale links by walking forward. |
| W22 | `krffs_take_free` 1358 successor link | Free space is split only after the tail is durable. Readers use old file. | EIO for size, stale links for successor | A | Keep publication whole. Derive stale links by walking forward. |
| W23 | `krffs_copy_original` 1378 source header | Destination is free. Earlier fields can be dirty or durable. Readers still see original. | EIO | A | Never publish an incomplete copy. |
| W24 | `krffs_copy_original` 1382 name | Destination is free. Earlier fields can be dirty or durable. Readers still see original. | EIO | A | Never publish an incomplete copy. |
| W25 | `krffs_copy_original` 1383 id | Destination is free. Earlier fields can be dirty or durable. Readers still see original. | EIO | A | Never publish an incomplete copy. |
| W26 | `krffs_copy_original` 1384 parent | Destination is free. Earlier fields can be dirty or durable. Readers still see original. | EIO | A | Never publish an incomplete copy. |
| W27 | `krffs_copy_original` 1385 mode | Destination is free. Earlier fields can be dirty or durable. Readers still see original. | EIO | A | Never publish an incomplete copy. |
| W28 | `krffs_copy_original` 1386 uid | Destination is free. Earlier fields can be dirty or durable. Readers still see original. | EIO | A | Never publish an incomplete copy. |
| W29 | `krffs_copy_original` 1387 gid | Destination is free. Earlier fields can be dirty or durable. Readers still see original. | EIO | A | Never publish an incomplete copy. |
| W30 | `krffs_copy_original` 1388 atime | Destination is free. Earlier fields can be dirty or durable. Readers still see original. | EIO | A | Never publish an incomplete copy. |
| W31 | `krffs_copy_original` 1390 mtime | Destination is free. Earlier fields can be dirty or durable. Readers still see original. | EIO | A | Never publish an incomplete copy. |
| W32 | `krffs_copy_original` 1392 ctime | Destination is free. Earlier fields can be dirty or durable. Readers still see original. | EIO | A | Never publish an incomplete copy. |
| W33 | `krffs_copy_data` 1252 source bytes | Prefix copied to free space. Original live. | EIO | A | Discard unpublished copy. |
| W34 | `krffs_copy_data` 1253 destination bytes | Prefix copied to free space. Original live. | EIO | A | Discard unpublished copy. |
| W35 | `krffs_fill_copy` 1315 zero hole | Free copy may contain a prefix. Readers see original. | EIO | W | Count no bytes from this copy. |
| W36 | `krffs_fill_copy` 1319 caller data | Free copy may contain a prefix. Readers see original. | EIO | W | Count no bytes from this copy. |
| W37 | `krffs_allocate_err` 1530 empty name | Prepared data and length precede visible type. Until the type succeeds only old file is reachable. | EIO | A | Stop at the failed step. |
| W38 | `krffs_allocate_err` 1532 data barrier | Prepared data and length precede visible type. Until the type succeeds only old file is reachable. | EIO | A | Stop at the failed step. |
| W39 | `krffs_allocate_err` 1533 copy length | Prepared data and length precede visible type. Until the type succeeds only old file is reachable. | EIO | A | Stop at the failed step. |
| W40 | `krffs_allocate_err` 1534 length barrier | Prepared data and length precede visible type. Until the type succeeds only old file is reachable. | EIO | A | Stop at the failed step. |
| W41 | `krffs_allocate_err` 1535 reserved type | Prepared data and length precede visible type. Until the type succeeds only old file is reachable. | EIO | A | Stop at the failed step. |
| W42 | `krffs_allocate_err` 1541 publication barrier | Copy may be reserved on disk, original still present. | Withdraw, EIO | A | Keep original live. Withdraw or hide copy. |
| W43 | `krffs_withdraw` 1422 free type | Two copies possible on disk. Readers use original. Free type in cache only if stored. | Halt and hide copy, or mark unsettled | H | Refuse further writes on failed withdrawal, settle a successful one. |
| W44 | `krffs_withdraw` 1431 withdrawal barrier | Two copies possible on disk. Readers use original. Free type in cache only if stored. | Halt and hide copy, or mark unsettled | H | Refuse further writes on failed withdrawal, settle a successful one. |
| W45 | `krffs_move` 1987 remove old | Before free type, original live. Afterwards copy holds complete payload, including on a failed final barrier. | Error before publication, full count after (review 2.2) | W | Repoint all holders whenever the copy became live. |
| W46 | `krffs_move` 1989 final barrier | Before free type, original live. Afterwards copy holds complete payload, including on a failed final barrier. | Error before publication, full count after (review 2.2) | W | Repoint all holders whenever the copy became live. |
| W47 | `krffs_move` 1993 failure barrier | Before free type, original live. Afterwards copy holds complete payload, including on a failed final barrier. | Error before publication, full count after (review 2.2) | W | Repoint all holders whenever the copy became live. |
| W48 | `krffs_write_at` 2193 fill hole | Old EOF. Partial zeros outside EOF. | EIO | W | Keep payload invisible. |
| W49 | `krffs_data_write_long` 404 each data unit | Completed units in cache. Only bytes below published EOF are visible. Disk may lag. | Short write or EIO (review 1.1) | W | Count only complete units, up to visible EOF. |
| W50 | `krffs_write_at` 2205 data barrier | New bytes below old EOF visible even if disk failed. Beyond it hidden. | Visible prefix or EIO | W | Return the visible count. Preserve durability failure. |
| W51 | `krffs_write_at` 2208 overwrite sync | New bytes below old EOF visible even if disk failed. Beyond it hidden. | Visible prefix or EIO | W | Return the visible count. Preserve durability failure. |
| W52 | `krffs_store_length_in` 1671 unsplit length | On volume, low word may reach cache before fetching unchanged high word fails. New bytes then visible. | Can answer EIO for a visible append (review 3.1) | W | Preload and hold the whole backend store before copying. |
| W53 | `krffs_store_length_in` 1681 entry barrier | Old, intermediate or final EOF in cache. Only completed barriers guarantee disk order. Each intermediate EOF covers initialized bytes. | Visible prefix or EIO, unsettled (review 2.1) | W | Update shown after each whole piece and before its barrier. |
| W54 | `krffs_store_length_in` 1685 high store | Old, intermediate or final EOF in cache. Only completed barriers guarantee disk order. Each intermediate EOF covers initialized bytes. | Visible prefix or EIO, unsettled (review 2.1) | W | Update shown after each whole piece and before its barrier. |
| W55 | `krffs_store_length_in` 1689 high barrier | Old, intermediate or final EOF in cache. Only completed barriers guarantee disk order. Each intermediate EOF covers initialized bytes. | Visible prefix or EIO, unsettled (review 2.1) | W | Update shown after each whole piece and before its barrier. |
| W56 | `krffs_store_length_in` 1694 low store | Old, intermediate or final EOF in cache. Only completed barriers guarantee disk order. Each intermediate EOF covers initialized bytes. | Visible prefix or EIO, unsettled (review 2.1) | W | Update shown after each whole piece and before its barrier. |
| W57 | `krffs_store_length_in` 1698 low barrier | Old, intermediate or final EOF in cache. Only completed barriers guarantee disk order. Each intermediate EOF covers initialized bytes. | Visible prefix or EIO, unsettled (review 2.1) | W | Update shown after each whole piece and before its barrier. |
| W58 | `krffs_store_length_in` 1703 last high store | Old, intermediate or final EOF in cache. Only completed barriers guarantee disk order. Each intermediate EOF covers initialized bytes. | Visible prefix or EIO, unsettled (review 2.1) | W | Update shown after each whole piece and before its barrier. |
| W59 | `krffs_store_length_in` 1707 last barrier | Old, intermediate or final EOF in cache. Only completed barriers guarantee disk order. Each intermediate EOF covers initialized bytes. | Visible prefix or EIO, unsettled (review 2.1) | W | Update shown after each whole piece and before its barrier. |
| W60 | `krffs_stamp_at` 1155 mtime | Payload and EOF already visible. Timestamp may lag. | Payload count or successful truncate | W | Keep payload accounting independent of advisory timestamps. Make each field store whole. |
| W61 | `krffs_stamp_at` 1157 ctime | Payload and EOF already visible. Timestamp may lag. | Payload count or successful truncate | W | Keep payload accounting independent of advisory timestamps. Make each field store whole. |
| W62 | `krffs_truncate_node_at` 2331 header | Old file visible. No truncate store. | Bad node mapped to EIO, or EIO | T | Distinguish failed read from invalid input. |
| W63 | `krffs_truncate_node_at` 2335 prior barrier | Old file visible. No truncate store. | Bad node mapped to EIO, or EIO | T | Distinguish failed read from invalid input. |
| W64 | `krffs_resize_err` 2058 header | Old file in cache and on disk. | ENOSPC even on failed read | T | Return EIO on read failure. |
| W65 | `krffs_resize_err` 2077 successor header | Old file. No shrink yet. | EIO | T | Leave length alone. |
| W66 | `krffs_shrink_at` 1874 shorter length | Published EOF may be shorter. Old extent still allocated. | EIO, or move if no safe order | T | Do not place a header inside bytes still counted on disk. |
| W67 | `krffs_shrink_at` 1885 length barrier | Published EOF may be shorter. Old extent still allocated. | EIO, or move if no safe order | T | Do not place a header inside bytes still counted on disk. |
| W68 | `krffs_shrink_at` 1888 size comparison | Shorter length durable. Failed field read can suppress split. | Success, retains room | A | Compare cached header size directly. |
| W69 | `krffs_shrink_at` 1894 surplus header | Shorter file visible. Only successful size store exposes surplus as free. | EIO | T | Keep reachable forward chain valid. |
| W70 | `krffs_shrink_at` 1895 tail barrier | Shorter file visible. Only successful size store exposes surplus as free. | EIO | T | Keep reachable forward chain valid. |
| W71 | `krffs_shrink_at` 1895 size | Shorter file visible. Only successful size store exposes surplus as free. | EIO | T | Keep reachable forward chain valid. |
| W72 | `krffs_shrink_at` 1898 successor link | Shorter file and new forward chain visible. Backward link may lag. | Success with stale links | T | Use forward chain until repaired. |
| W73 | `krffs_truncate_node_at` 2361 move | Live offset may change. In-place extension remains hidden until EOF changes. Disk may lag an intermediate EOF. | EIO, or ENOSPC on failed search for space | T | Repoint live offset even on error. Expose initialized bytes only. |
| W74 | `krffs_truncate_node_at` 2369 zero extension | Live offset may change. In-place extension remains hidden until EOF changes. Disk may lag an intermediate EOF. | EIO, or ENOSPC on failed search for space | T | Repoint live offset even on error. Expose initialized bytes only. |
| W75 | `krffs_truncate_node_at` 2371 barrier | Live offset may change. In-place extension remains hidden until EOF changes. Disk may lag an intermediate EOF. | EIO, or ENOSPC on failed search for space | T | Repoint live offset even on error. Expose initialized bytes only. |
| W76 | `krffs_truncate_node_at` 2372 longer length | Live offset may change. In-place extension remains hidden until EOF changes. Disk may lag an intermediate EOF. | EIO, or ENOSPC on failed search for space | T | Repoint live offset even on error. Expose initialized bytes only. |

## Scan, removal and recovery

Locations are in `krffs.c` unless qualified.

| Row | Function, line and operation | Disk, cache and reader | Current answer | Linux | Required action |
| --- | --- | --- | --- | --- | --- |
| R01 | `krffs_reap_orphans_at` 3297 first header | Failed read changes no node. Earlier removals may already be durable. Unscanned orphans remain possible. | Completed count, orphan debt, halted (review 1.3) | O | End scan and keep dirty flag. |
| R02 | `krffs_reap_orphans_at` 3307 next header | Failed read changes no node. Earlier removals may already be durable. Unscanned orphans remain possible. | Completed count, orphan debt, halted (review 1.3) | O | End scan and keep dirty flag. |
| R03 | `krffs_reap_orphans_at` 3311 name | Failed read changes no node. Earlier removals may already be durable. Unscanned orphans remain possible. | Completed count, orphan debt, halted (review 1.3) | O | End scan and keep dirty flag. |
| R04 | `krffs_remove_err` 1797 victim header | No removal yet. Original live or orphan reserved. | Failure | O | Stop and halt a reaper. |
| R05 | `krffs_remove_reads` 1755 preceding node walk | Nothing from this removal stored. A forward walk substitutes for a stale link. | Failure (reviews 1.3, 2.3) | O | Finish all prerequisite reads before free type. |
| R06 | `krffs_remove_reads` 1756 preceding header | Nothing from this removal stored. A forward walk substitutes for a stale link. | Failure (reviews 1.3, 2.3) | O | Finish all prerequisite reads before free type. |
| R07 | `krffs_remove_reads` 1763 next header | Nothing from this removal stored. A forward walk substitutes for a stale link. | Failure (reviews 1.3, 2.3) | O | Finish all prerequisite reads before free type. |
| R08 | `krffs_remove_reads` 1767 link target header | Nothing from this removal stored. A forward walk substitutes for a stale link. | Failure (reviews 1.3, 2.3) | O | Finish all prerequisite reads before free type. |
| R09 | `krffs_remove_err` 1805 free type | Orphan still reserved if this byte fails. Move still has original. | Failure, typed clear | O | Leave count and original intact. |
| R10 | `krffs_remove_err` 1814 forward merge | Free type is visible. A prior merge may have changed the forward chain. Some dirty blocks can already be on disk. | Failure after possible repair or sync (review 3.2) | O | Stop on first failed merge. Mark links stale and state unsettled. |
| R11 | `krffs_remove_err` 1821 preceding merge | Free type is visible. A prior merge may have changed the forward chain. Some dirty blocks can already be on disk. | Failure after possible repair or sync (review 3.2) | O | Stop on first failed merge. Mark links stale and state unsettled. |
| R12 | `krffs_remove_err` 1829 backward repair | Free type and merged size in cache. Target may have been evicted after preflight. Old backward link remains. | Sync then failure (reviews 2.3, 3.2) | O | Return immediately. Halt reap before any further store or sync. |
| R13 | `krffs_remove_err` 1832 sync | Free type visible. Disk may have a prefix of the removal. | Failure after publication | O | Keep live copy for a move. Halt on a reap. |
| R14 | `krffs_reap_orphans_at` 3322 previous header | Removal complete. Later orphans unknown. Disk still dirty. | Count completed frees and halt (review 1.3) | O | Keep conservative orphan debt. Stop. |
| R15 | `krffs_reap_orphans_at` 3329 surviving header | Removal complete. Later orphans unknown. Disk still dirty. | Count completed frees and halt (review 1.3) | O | Keep conservative orphan debt. Stop. |
| R16 | `open_drop` krffsmount.c:457 last close | Nameless node may remain reserved or partly merged. Last reference is gone. | Removal result discarded | O | Halt on failed last-holder reap. Preserve check owed. |
| R17 | `open_drop` krffsmount.c:989 last unlink holder | Nameless node may remain reserved or partly merged. Last reference is gone. | Removal result discarded | O | Halt on failed last-holder reap. Preserve check owed. |
| R18 | `open_drop` krffsmount.c:1163 last rename holder | Nameless node may remain reserved or partly merged. Last reference is gone. | Removal result discarded | O | Halt on failed last-holder reap. Preserve check owed. |
| R19 | `krffs_remove_reserved_node` 4706 root last holder | Same state on root. File no longer named. | Void, failure discarded | O | Apply the same last-holder rule. |
| R20 | `krffs_repair_links` 3956 root header | Previously repaired links may be dirty or durable. Forward chain determines ownership. | Failure | O | Stop the repair before any more writes. |
| R21 | `krffs_repair_links` 3962 backward store | Previously repaired links may be dirty or durable. Forward chain determines ownership. | Failure | O | Stop the repair before any more writes. |
| R22 | `krffs_repair_links` 3969 next header | Previously repaired links may be dirty or durable. Forward chain determines ownership. | Failure | O | Stop the repair before any more writes. |
| R23 | `krffs_repair_links` 3974 root backward store | Previously repaired links may be dirty or durable. Forward chain determines ownership. | Failure | O | Stop the repair before any more writes. |
| R24 | `krffs_repair_parents` 3885 count headers | Earlier repair may be cached. No decision possible for unread entry. | Failure | O | Stop. Release index. |
| R25 | `krffs_repair_parents` 3896 directory index | Earlier repair may be cached. No decision possible for unread entry. | Failure | O | Stop. Release index. |
| R26 | `krffs_repair_parents` 3903 file header | Earlier repair may be cached. No decision possible for unread entry. | Failure | O | Stop. Release index. |
| R27 | `krffs_repair_parents` 3909 name presence | A failed read looks like a named file. A bad parent can be rewritten using this answer. | Continues repair | O | Return error before testing or changing parent. |
| R28 | `krffs_repair_parents` 3910 parent lookup | Earlier pieces follow the recovery order. File may temporarily lack a known parent. | Failure | O | Stop and leave recovery owed. |
| R29 | `krffs_repair_parents` 3913 recovery id lookup | Earlier pieces follow the recovery order. File may temporarily lack a known parent. | Failure | O | Stop and leave recovery owed. |
| R30 | `krffs_repair_parents` 3916 halfway parent lookup | Earlier pieces follow the recovery order. File may temporarily lack a known parent. | Failure | O | Stop and leave recovery owed. |
| R31 | `krffs_repair_parents` 3924 parent piece | Earlier pieces follow the recovery order. File may temporarily lack a known parent. | Failure | O | Stop and leave recovery owed. |
| R32 | `krffs_put_pieces` 2945 parent store | Known recoverable intermediate parent or final parent in cache. Disk may lag. | Failure | O | Do not start another piece. |
| R33 | `krffs_put_pieces` 2946 parent barrier | Known recoverable intermediate parent or final parent in cache. Disk may lag. | Failure | O | Do not start another piece. |
| R34 | `krffs_count_names` 3480 brief header | Any earlier repairs stand. Index incomplete. | Failure | O | Stop before processing names. |
| R35 | `krffs_count_names` 3483 name presence | An unreadable byte increments the count and a later pass can clear names. | Success possible | O | Propagate failed name read. |
| R36 | `krffs_select_names` 3651 full header | Selected index incomplete. Earlier groups may be durable. | Failure | O | Release index and stop. |
| R37 | `krffs_name_before` 3509 first name | Sort in memory only. Earlier groups may be durable. | Failure latched | O | Publish no result from a failed sort. |
| R38 | `krffs_name_before` 3510 second name | Sort in memory only. Earlier groups may be durable. | Failure latched | O | Publish no result from a failed sort. |
| R39 | `krffs_resolve_run` 3587 first header | Previous losers may already be unnamed. Either old or empty name at current byte. | Failure | O | Stop before reaping. |
| R40 | `krffs_resolve_run` 3588 second header | Previous losers may already be unnamed. Either old or empty name at current byte. | Failure | O | Stop before reaping. |
| R41 | `krffs_resolve_run` 3596 clear loser name | Previous losers may already be unnamed. Either old or empty name at current byte. | Failure | O | Stop before reaping. |
| R42 | `krffs_repair_at` 4007 link pass | Earlier repairs in cache or on disk. Dirty bit still set. | Failure, no halt at this return | O | Halt the repair before returning. |
| R43 | `krffs_repair_at` 4008 parent pass | Earlier repairs in cache or on disk. Dirty bit still set. | Failure, no halt at this return | O | Halt the repair before returning. |
| R44 | `krffs_repair_at` 4009 name pass | Earlier repairs in cache or on disk. Dirty bit still set. | Failure, no halt at this return | O | Halt the repair before returning. |
| R45 | `krffs_repair_at` 4028 final sync | Scan may already have halted. Earlier free types or links dirty. | Backend still called, may succeed on memory backend | O | Check halt at sync entry. A failed repair sync halts. |

## Cache, sync and shutdown

Locations are in `pagecache.c` (pc), `bcache.c` (buffer cache), `krffsmount.c` (volume) unless qualified.

| Row | Function, line and operation | Disk, cache and reader | Current answer | Linux | Required action |
| --- | --- | --- | --- | --- | --- |
| S01 | `pc_bring` 702 slot or eviction | Failed fill installs no page. Dirty victim kept on failure. Readers may use resident pages. | Miss fails | C | Pass error through explicit copies. |
| S02 | `pc_bring` 715 page read | Failed fill installs no page. Dirty victim kept on failure. Readers may use resident pages. | Miss fails | C | Pass error through explicit copies. |
| S03 | `pc_copy` 779 read page | Read streams one page at a time. Store pins both pages of an edge field before changing either. | Failure without partial field (reviews 1.1, 2.7) | C | One frame suffices for crossing reads. Keep stores atomic. |
| S04 | `pc_copy` 779 store page | Read streams one page at a time. Store pins both pages of an edge field before changing either. | Failure without partial field (reviews 1.1, 2.7) | C | One frame suffices for crossing reads. Keep stores atomic. |
| S05 | `volume_read` 104 block get | Earlier bytes in caller scratch only. Cache and disk unchanged. | Failure | C | Propagate failed read. |
| S06 | `volume_read` 108 short last block | Earlier bytes in caller scratch only. Cache and disk unchanged. | Failure | C | Propagate failed read. |
| S07 | `volume_write` 156 block get | Earlier block of one field already dirty, possibly written. Reader can see torn field. | Failure after prefix (review 3.1) | W | Acquire and validate all blocks of a field before copying. Data calls already stop at unit edges. |
| S08 | `volume_write` 160 short last block | Earlier block of one field already dirty, possibly written. Reader can see torn field. | Failure after prefix (review 3.1) | W | Acquire and validate all blocks of a field before copying. Data calls already stop at unit edges. |
| S09 | `pc_writeback` 565 backing write | Cache holds latest run. Device may have accepted a prefix. Pages redirtied, error recorded. | Failure | S | Retain dirty pages, wake waiters and report. |
| S10 | `pc_evict` 614 victim write | Victim stays resident and dirty. Other slots may serve a miss. | Failure to evict | C | Keep failed victim. Honor backing halt. |
| S11 | `pc_shrink` 853 eviction | Failed dirty page retained. Clean pages may be freed. | Count reclaimed | C | Never discard failed write as clean. |
| S12 | `pc_write_down` 922 background or balancing write | Failed pages retained. Flusher or writer may make partial progress. | Count progress | C | Use same writeback gate. |
| S13 | `pc_sync` 970 pass write | Failed pages dirty, pinned pages may belong to another writer. | Failed or busy bits | S | Wait for pins. Keep first failure across retries. |
| S14 | `krffs_disk_write` kernel.c:2798 first submission | First request may sleep. Halt may intervene before later chunks. Already submitted data may land. | Only tests halt at function entry (review 1.2) | H | Check before each device request, including continuation. |
| S15 | `krffs_disk_write` kernel.c:2807 next device request | First request may sleep. Halt may intervene before later chunks. Already submitted data may land. | Only tests halt at function entry (review 1.2) | H | Check before each device request, including continuation. |
| S16 | `write_buffer_flags` 1482 one buffer | Dirty buffer held. Device may accept part on error. Mark and record failed write. | Device error, EIO if halted (review 2.4) | H | Block every new write after halt. |
| S17 | `write_run_met` 1880 order flush | Gathered dirty buffers. Failed write retains run, releases holds. | Error, EIO at halted write (review 2.4) | H | Gate both flush and submission. |
| S18 | `write_run_met` 1898 run write | Gathered dirty buffers. Failed write retains run, releases holds. | Error, EIO at halted write (review 2.4) | H | Gate both flush and submission. |
| S19 | `flush_firsts` 1527 device flush | Earlier writes may still be in device cache. Explicit final flush can run after halt, including on unmount. | Device result | H | Reject a flush on a halted device before calling driver. |
| S20 | `sync_device` 3548 dirty passes | Failed blocks remain dirty. Other requests may already have reached medium. | First error (review 2.4) | S | Keep first error and halt gate. |
| S21 | `sync_device` 3557 final flush | Failed blocks remain dirty. Other requests may already have reached medium. | First error (review 2.4) | S | Keep first error and halt gate. |
| S22 | `bcache_sync_owned` 3592 owned passes | Other files may remain dirty. Failed file blocks kept. | Error (review 2.4) | S | Refuse halted owner sync. |
| S23 | `bcache_writeback` 3524 expired buffers | Dirty buffers retained on failed or halted device. | No progress or error (review 2.4) | H | Keep halt gate. |
| S24 | `get` 2544 dirty victim write | Victim remains dirty and unavailable after failure. Another victim can satisfy miss. | Retry then buffer or NULL | C | Exclude halted dirty victims. |
| S25 | `bcache_shrink` 3890 dirty write | Failed dirty victim retained. Others can be freed. | Count reclaimed | C | Honor write_barred and final device gate. |
| S26 | `bcache_reserve` 4005 dirty write | Still-dirty capacity cannot be promised. Other buffers remain available. | Count ready | C | Honor victim gate on reserve too. |
| S27 | `volume_sync` 189 buffer sync | Earlier stores visible. After either error disk may lag. | Failure | S | Propagate. Stop before the next ordered stage. |
| S28 | `volume_sync` 192 drive flush | Earlier stores visible. After either error disk may lag. | Failure | S | Propagate. Stop before the next ordered stage. |
| S29 | `kv_fsync` 775 whole-device sync | Readers see cached bytes. Completed writes may be durable, failed blocks retained. | First error | S | Preserve file or device error and halt gate. |
| S30 | `kv_fsync` 780 owned sync | Readers see cached bytes. Completed writes may be durable, failed blocks retained. | First error | S | Preserve file or device error and halt gate. |
| S31 | `kv_fsync` 782 header blocks | Readers see cached bytes. Completed writes may be durable, failed blocks retained. | First error | S | Preserve file or device error and halt gate. |
| S32 | `kv_fsync` 785 successor link blocks | Readers see cached bytes. Completed writes may be durable, failed blocks retained. | First error | S | Preserve file or device error and halt gate. |
| S33 | `sync_link_after` 727 file header | Earlier blocks synced. Location of link unknown. | EIO | S | Return error. |
| S34 | `sync_link_after` 734 next header | Earlier blocks synced. Location of link unknown. | EIO | S | Return error. |
| S35 | `sync_bytes` 705 each block | Earlier blocks submitted, failed one dirty. | First error | S | Stop on first failure. |
| S36 | `kv_prepare_unmount` 1264 sync | Dirty flag retained unless earlier data durable. Failed flag write may already be on disk. | Error, or keep dirty for owed check | M | Keep dirty if recovery owed. Protect halted devices at final flush. |
| S37 | `kv_prepare_unmount` 1269 flush | Dirty flag retained unless earlier data durable. Failed flag write may already be on disk. | Error, or keep dirty for owed check | M | Keep dirty if recovery owed. Protect halted devices at final flush. |
| S38 | `kv_prepare_unmount` 1293 clean flag | Dirty flag retained unless earlier data durable. Failed flag write may already be on disk. | Error, or keep dirty for owed check | M | Keep dirty if recovery owed. Protect halted devices at final flush. |
| S39 | `kv_abort_unmount` 1249 restore dirty | Mount still present after canceled unmount. Disk flag uncertain. | Read-only on error | M | Accept no new file changes if dirty bit uncertain. |
| S40 | `kv_abort_unmount` 1249 sync | Mount still present after canceled unmount. Disk flag uncertain. | Read-only on error | M | Accept no new file changes if dirty bit uncertain. |
| S41 | `kv_abort_unmount` 1250 flush | Mount still present after canceled unmount. Disk flag uncertain. | Read-only on error | M | Accept no new file changes if dirty bit uncertain. |
| S42 | `krffs_disk_mark_clean` kernel.c:2490 owed-check sync | Before clear, disk marked dirty. After last failure flag may already be clean. No more file changes permitted. | Fails shutdown clean check (review 1.2) | M | Allow clear only after successful sync and no recovery debt. Respect halt. |
| S43 | `krffs_disk_mark_clean` kernel.c:2493 mode read | Before clear, disk marked dirty. After last failure flag may already be clean. No more file changes permitted. | Fails shutdown clean check (review 1.2) | M | Allow clear only after successful sync and no recovery debt. Respect halt. |
| S44 | `krffs_disk_mark_clean` kernel.c:2495 read-error sync | Before clear, disk marked dirty. After last failure flag may already be clean. No more file changes permitted. | Fails shutdown clean check (review 1.2) | M | Allow clear only after successful sync and no recovery debt. Respect halt. |
| S45 | `krffs_disk_mark_clean` kernel.c:2499 preclear sync | Before clear, disk marked dirty. After last failure flag may already be clean. No more file changes permitted. | Fails shutdown clean check (review 1.2) | M | Allow clear only after successful sync and no recovery debt. Respect halt. |
| S46 | `krffs_disk_mark_clean` kernel.c:2504 clean store | Before clear, disk marked dirty. After last failure flag may already be clean. No more file changes permitted. | Fails shutdown clean check (review 1.2) | M | Allow clear only after successful sync and no recovery debt. Respect halt. |
| S47 | `krffs_disk_mark_clean` kernel.c:2508 clean sync | Before clear, disk marked dirty. After last failure flag may already be clean. No more file changes permitted. | Fails shutdown clean check (review 1.2) | M | Allow clear only after successful sync and no recovery debt. Respect halt. |

## Mount and halt

Locations are in `krffs.c` unless qualified unless qualified.

| Row | Function, line and operation | Disk, cache and reader | Current answer | Linux | Required action |
| --- | --- | --- | --- | --- | --- |
| M01 | `krffs_volume_mount` krffsmount.c:1466 root read | No mounted tree. Raw image untouched. | EIO | M | Release private state. |
| M02 | `krffs_chain_walk` 4065 first header | No new writes in validation. Earlier repairs may exist on final validation. Failed I/O is indistinguishable from a malformed ring. | Volume EINVAL, dirty root may retry as malformed | M | Distinguish I/O from bad format. Never begin repair following failed validation I/O. |
| M03 | `krffs_chain_walk` 4076 subsequent header | No new writes in validation. Earlier repairs may exist on final validation. Failed I/O is indistinguishable from a malformed ring. | Volume EINVAL, dirty root may retry as malformed | M | Distinguish I/O from bad format. Never begin repair following failed validation I/O. |
| M04 | `krffs_volume_mount` krffsmount.c:1499 dirty-volume repair | Constructor holds device claim. Repair buffers may remain dirty or halted. Durable completed repairs may persist. | Refusal, private object freed | M | Discard constructor buffers and device error marks before releasing claim. |
| M05 | `krffs_volume_mount` krffsmount.c:1505 final validation | Constructor holds device claim. Repair buffers may remain dirty or halted. Durable completed repairs may persist. | Refusal, private object freed | M | Discard constructor buffers and device error marks before releasing claim. |
| M06 | `krffs_volume_mount` krffsmount.c:1511 dirty mark | Constructor holds device claim. Repair buffers may remain dirty or halted. Durable completed repairs may persist. | Refusal, private object freed | M | Discard constructor buffers and device error marks before releasing claim. |
| M07 | `krffs_set_dirty` 3349 root header | Old flag before store, new flag after. Disk uncertain on sync error. | Failure | M | Propagate flag errors, respect halt. |
| M08 | `krffs_set_dirty` 3357 mode store | Old flag before store, new flag after. Disk uncertain on sync error. | Failure | M | Propagate flag errors, respect halt. |
| M09 | `krffs_set_dirty` 3360 sync | Old flag before store, new flag after. Disk uncertain on sync error. | Failure | M | Propagate flag errors, respect halt. |
| M10 | `mount_refused` vfs.c:5505 invalidate | A refused writable constructor can leave dirty buffers and a halt mark. Later sync or remount sees them. | Ignores EBUSY (review 3.3) | M | Release KRFFS repair state under the constructor's claim. Cleanup issues no I/O. |
| M11 | `krffs_mount_root` 4796 root read | No published file system. Prior dirty-root recovery may have changed cached or durable metadata. | Refuse, drop window, or retry strict failure | M | Distinguish validation I/O from recoverable link mismatch. Release window on refusal. |
| M12 | `krffs_mount_root` 4813 strict walk | No published file system. Prior dirty-root recovery may have changed cached or durable metadata. | Refuse, drop window, or retry strict failure | M | Distinguish validation I/O from recoverable link mismatch. Release window on refusal. |
| M13 | `krffs_mount_root` 4815 forward walk | No published file system. Prior dirty-root recovery may have changed cached or durable metadata. | Refuse, drop window, or retry strict failure | M | Distinguish validation I/O from recoverable link mismatch. Release window on refusal. |
| M14 | `krffs_mount_root` 4816 link repair | No published file system. Prior dirty-root recovery may have changed cached or durable metadata. | Refuse, drop window, or retry strict failure | M | Distinguish validation I/O from recoverable link mismatch. Release window on refusal. |
| M15 | `krffs_mount_root` 4827 parent repair | No published file system. Prior dirty-root recovery may have changed cached or durable metadata. | Refuse, drop window, or retry strict failure | M | Distinguish validation I/O from recoverable link mismatch. Release window on refusal. |
| M16 | `krffs_mount_root` 4828 name repair | No published file system. Prior dirty-root recovery may have changed cached or durable metadata. | Refuse, drop window, or retry strict failure | M | Distinguish validation I/O from recoverable link mismatch. Release window on refusal. |
| M17 | `krffs_root_mark_and_reap` kernel.c:3175 mode read | On success disk dirty before orphan scan. On error flag uncertain, no writable root published. | Refusal | M | Skip further stores if mount already halted with dirty flag on disk. |
| M18 | `krffs_root_mark_and_reap` kernel.c:3181 dirty store | On success disk dirty before orphan scan. On error flag uncertain, no writable root published. | Refusal | M | Skip further stores if mount already halted with dirty flag on disk. |
| M19 | `krffs_root_mark_and_reap` kernel.c:3181 dirty sync | On success disk dirty before orphan scan. On error flag uncertain, no writable root published. | Refusal | M | Skip further stores if mount already halted with dirty flag on disk. |
| M20 | `krffs_root_mark_and_reap` kernel.c:3193 final reap sync | Repairs visible, disk dirty, completion uncertain. | Warns and continues | O | Halt on failed repair writeback. |
| M21 | `krffs_halt` 1442 mark and callback | Cache may contain partial recovery. Readers may read what remains accessible. New writes must stop. | Check owed and read-only (reviews 1.2, 2.4) | H | Gate core stores, sync, device writes and flushes. |
| M22 | `krffs_backend_write` 267 halted store | Earlier dirty data retained. No subsequent change accepted. | Failure | H | Keep gate at common store. |

## Earlier review findings at callers

Locations are in `syscall_fs.c` unless qualified unless qualified.

| Row | Function, line and operation | Disk, cache and reader | Current answer | Linux | Required action |
| --- | --- | --- | --- | --- | --- |
| F01 | `cred_kill_setid` 2999 header read | Original data still visible. Clear must complete before modifying payload. | EIO or EROFS (review 1.4) | P | Use checked clear before write. |
| F02 | `cred_kill_setid` 3013 mode store | Original data still visible. Clear must complete before modifying payload. | EIO or EROFS (review 1.4) | P | Use checked clear before write. |
| F03 | `mnt_kill_setid` 3045 identify underlying inode | Bind and original root name address same mode and data. | Uses base mount (review 2.6) | P | Apply same rule through binds. |
| F04 | `mnt_kill_setid` 14182 path truncate clear | Old contents until mode cleared. | Clear error (review 2.5) | P | Preserve error before changing length. |
| F05 | `krffs_node_is_dir` 18859 rename destination header | Directory checks can evict a header. On error namespace stays as found. | EIO (review 1.5) | N | Use error-returning read. |
| F06 | `kfs_lookup` vfs.c:4064 parent | No mutation. Unreadable parent or sibling leaves lookup undecided. | EIO (review 1.6) | N | Propagate error. |
| F07 | `kfs_lookup` vfs.c:4071 child | No mutation. Unreadable parent or sibling leaves lookup undecided. | EIO (review 1.6) | N | Propagate error. |
| F08 | `fd_node_check` 2225 validation | Held descriptor still valid. Unreadable metadata gives no replacement identity. | EIO (review 1.7) | N | Preserve I/O error through caller. |
| F09 | `fd_node_check` 7352 listing seek | Held descriptor still valid. Unreadable metadata gives no replacement identity. | EIO (review 1.7) | N | Preserve I/O error through caller. |
| F10 | `fd_node_check` 19367 descriptor times | Held descriptor still valid. Unreadable metadata gives no replacement identity. | EIO (review 1.7) | N | Preserve I/O error through caller. |
| F11 | `fd_node_check` 20799 directory entries | Held descriptor still valid. Unreadable metadata gives no replacement identity. | EIO (review 1.7) | N | Preserve I/O error through caller. |
| F12 | `exe_file_path` procfs.c:1060 executable path | Reader asks for current path. Header unavailable. | EIO (review 1.8) | N | Preserve read error. |
| F13 | `exe_file_path` procfs.c:3242 descriptor path | Reader asks for current path. Header unavailable. | EIO (review 1.8) | N | Preserve read error. |
| F14 | `exe_file_path` procfs.c:1087 held name | Held name or attributes could not be resolved. Cached string is insufficient. | EIO (review 2.8) | N | Propagate the error. |
| F15 | `exe_file_path` procfs.c:1100 attributes | Held name or attributes could not be resolved. Cached string is insufficient. | EIO (review 2.8) | N | Propagate the error. |
