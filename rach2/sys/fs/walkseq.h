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

/** Whether the calling thread has begun a change it has not ended, whose
 * own walks neither wait for it nor are judged by the sequence, which it
 * holds open. */
int vfs_seq_mine(void);

/*
 * A walk's stamp: the sequence a walk's answer was checked at, and whether
 * it was, since a walk by a thread inside its own change, or one that holds
 * a namespace lock and so may not wait for a change under way, is not.
 * Every walk leaves its stamp with the calling thread (`vfs_stamp_set`); a
 * call that makes, removes, renames or changes what the walk found takes the
 * stamp after the walk (`vfs_stamp_get`), and once it holds the locks of
 * its change, before the change, checks it (`vfs_stamp_still`): a change of
 * what a path names that came while it slept, in the walk's locks or its
 * own, sends the whole call round again, both walks of a call of two
 * paths, the stamp being the first walk's. Linux's open checks its walk so
 * before it makes the file (`complete_walk` and `lookup_open`, fs/namei.c
 * in 6.12).
 */
struct vfs_stamp {
    uint64_t seq;
    int on; /* 0: the walk was not checked, and neither is the stamp */
};

/** Leave `seq` with the calling thread as its last walk's stamp, checked
 * when `on`. */
void vfs_stamp_set(uint64_t seq, int on);

/** The calling thread's last walk's stamp, into `*st`. */
void vfs_stamp_get(struct vfs_stamp *st);

/** Whether nothing has changed what a path names since the stamp `st`:
 * 1 for a stamp that is not checked. The walk gate's WALK_GATE_RETRY
 * counts each 0. */
int vfs_stamp_still(const struct vfs_stamp *st);

/* The events of the walk gate (`walk_gate`): the full walk under the big
 * lock begins, with its start and root taken; a walk without the big lock
 * has taken its start and sequence and not yet walked; a walk without the
 * big lock gave up for the full walk; the full walk has looked a name up
 * and not yet used the answer; the full walk is about to take the namespace
 * lock of a mount it moves onto; the full walk found the sequence moved and
 * starts again; a call that changes what its walk found has its walk's
 * answer and has not yet taken the locks of its change; such a call found
 * the sequence moved once it had them and starts again; and a walk sleeps
 * until no change is under way. */
#define WALK_GATE_FULL     0
#define WALK_GATE_UNLOCKED 1
#define WALK_GATE_MISS     2
#define WALK_GATE_LOOKUP   3
#define WALK_GATE_NSLOCK   4
#define WALK_GATE_RESTART  5
#define WALK_GATE_HANDOFF  6
#define WALK_GATE_RETRY    7
#define WALK_GATE_WAIT     8
#define WALK_GATE_EVENTS   9

/**
 * Count `event` for the thread the walk gate is armed for, and stop it at
 * the event the gate names, the first time, until the gate is released or
 * a signal comes: a test's way to change the tree at a known point of a
 * walk, through /sys/kernel/debug/vfs/walk_gate (`walk_gate_init`), which
 * root alone may open. Anyone else, and an unarmed gate, returns at once.
 * May sleep; with the big lock held or not, and no spinlock.
 */
void walk_gate(int event);

/** Whether the walk gate's `fault` is armed for the calling thread, which
 * this disarms: the next hand-back of a type that takes the rest of a path,
 * at its mount's root, is then taken as one that used up nothing, the fault
 * of a type that `resolve_full` answers with -EIO. */
int walk_gate_fault(void);

/** Register /sys/kernel/debug/vfs/walk_gate, once debugfs is up. */
void walk_gate_init(void);

#endif /* _WALKSEQ_H_ */
