/* The walk sequence: one count of the changes that can change what a path
 * names, which a walk compares to learn whether it raced one. */
#ifndef _WALKSEQ_H_
#define _WALKSEQ_H_

#include "../lib/kint.h"

/*
 * A change of what a path names is bracketed by `vfs_seq_begin` and
 * `vfs_seq_end`: the mounts' places, flags and views, a change of a name
 * (unlink, rmdir, rename and the name records they update), a process's
 * root or working directory, and a KRFFS relocation. The sequence moves at
 * each beginning and each end, and a count of the changes under way stays
 * above zero from the first beginning to the last end, so two changes that
 * overlap never look like none, as two that overlap could if a reader took
 * the sequence's parity for the sign of one under way. A reader takes the
 * sequence when no change is under way (`vfs_seq_read`) and judges later
 * that none has begun since (`vfs_seq_still`), as Linux's walk under RCU
 * reads `mount_lock` and `rename_lock` (fs/namei.c `path_init` and
 * `legitimize_path` in 6.12).
 *
 * The count is 64 bits, read and moved under one spinlock, which is held
 * only for that and is the last in the lock order, so that i686 never reads
 * half of it and no value comes round again in a machine's life.
 */

/** Begin a change of what a path names. Any context but an interrupt
 * handler; may nest, each with its own `vfs_seq_end`. */
void vfs_seq_begin(void);

/** End the change `vfs_seq_begin` began, on every exit path of it, and
 * wake the waiters of `vfs_seq_wait` when it was the last under way. */
void vfs_seq_end(void);

/** The sequence into `*seq`: 1 when no change is under way, 0 when one is,
 * whose reader may not take the sequence as a snapshot. */
int vfs_seq_read(uint64_t *seq);

/** Whether no change is under way and none has begun since `seq` was read
 * with `vfs_seq_read`. */
int vfs_seq_still(uint64_t seq);

/** Sleep until no change is under way and read the sequence into `*seq`,
 * for a walk that may sleep, which holds no spinlock, no namespace lock and
 * no count of a mount's operations. May sleep. */
void vfs_seq_wait(uint64_t *seq);

/* The events of the walk gate (`walk_gate`): the full walk under the big
 * lock begins, with its start and root taken; a walk without the big lock
 * has taken its start and sequence and not yet walked; and a walk without
 * the big lock gave up for the full walk. */
#define WALK_GATE_FULL     0
#define WALK_GATE_UNLOCKED 1
#define WALK_GATE_MISS     2
#define WALK_GATE_EVENTS   3

/**
 * Count `event` for the thread the walk gate is armed for, and stop it at
 * the event the gate names, the first time, until the gate is released or
 * a signal comes: a test's way to change the tree at a known point of a
 * walk, through /sys/kernel/debug/vfs/walk_gate (`walk_gate_init`), which
 * root alone may open. Anyone else, and an unarmed gate, returns at once.
 * May sleep; with the big lock held or not, and no spinlock.
 */
void walk_gate(int event);

/** Register /sys/kernel/debug/vfs/walk_gate, once debugfs is up. */
void walk_gate_init(void);

#endif /* _WALKSEQ_H_ */
