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
 *
 * A thread's own changes are taken out of what it reads: it counts the
 * changes it has under way and the moves they made (`walk_tokens` and
 * `walk_moves` in process.h), its reads see no change under way while the
 * only ones are its own, and the sequence they give leaves out its own
 * moves. A walk of a thread inside its own change, as a creation that
 * stamps its node, is so still checked against every other change.
 */

/** Begin a change of what a path names. Any context but an interrupt
 * handler; may nest, each with its own `vfs_seq_end`. */
void vfs_seq_begin(void);

/** End the change `vfs_seq_begin` began, on every exit path of it, and
 * wake the waiters of `vfs_seq_wait` when it was the last under way. */
void vfs_seq_end(void);

/** The sequence into `*seq`, less the calling thread's own moves: 1 when
 * no change but the caller's own is under way, 0 when another is, whose
 * reader may not take the sequence as a snapshot. */
int vfs_seq_read(uint64_t *seq);

/** Whether no change but the caller's own is under way and no other has
 * begun since `seq` was read with `vfs_seq_read` by the same thread. */
int vfs_seq_still(uint64_t seq);

/** `vfs_seq_still` and, when it holds, `vfs_seq_begin`, as one step under
 * the sequence's lock, so that no other change comes between the check and
 * the beginning: 1 with the change begun, or 0 with nothing begun. */
int vfs_seq_begin_if(uint64_t seq);

/** Sleep until no change but the caller's own is under way and read the
 * sequence into `*seq`, for a walk that may sleep, which holds no spinlock,
 * no namespace lock, no file lock, no change of its own and no count of a
 * mount's operations, any of which a change under way may wait for. May
 * sleep. */
void vfs_seq_wait(uint64_t *seq);

/** Whether the calling thread has begun a change it has not ended. */
int vfs_seq_mine(void);

/*
 * A walk's stamp: the sequence a walk's answer was checked at, and whether
 * it was, since a walk with no thread, at boot, is not. Every walk leaves
 * its stamp with the calling thread (`vfs_stamp_set`); a call that makes,
 * removes, renames or changes what the walk found takes the stamp after the
 * walk (`vfs_stamp_get`), and once it holds the locks of its change, before
 * the change, checks it (`vfs_stamp_still`, `vfs_stamp_begin`): a change of
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

/* The answer of a step that consumes a walk's answer when the walk's stamp
 * no longer holds, for the system call to let go of all it took and run
 * again from its walk (`CALL_AGAIN` in syscall_fs.c): below every errno and
 * never a call's answer. */
#define VFS_AGAIN (-4095 - 2)

/**
 * Whether the stamp the calling thread consumes, which a call sets in
 * `walk_consume` (process.h) around a backend's open, still holds: 1 with
 * none set. A backend whose open selects its object again, by a name a
 * walk classified, and may sleep before its effects, as a device open
 * reserves a descriptor, checks it after that sleep and before the effects,
 * and answers VFS_AGAIN when it fails, so that no effect lands on a
 * replacement registered meanwhile (`devfs_open`).
 */
int vfs_consume_still(void);

/** `vfs_stamp_still` and the beginning of a change as one step
 * (`vfs_seq_begin_if`): 1 with the change begun, which the caller ends
 * with `vfs_seq_end`, or 0, counted as WALK_GATE_RETRY, with nothing
 * begun. The hand-off of a walk to the change it makes. */
int vfs_stamp_begin(const struct vfs_stamp *st);

/* The events of the walk gate (`walk_gate`): the full walk under the big
 * lock begins, with its start and root taken; a walk without the big lock
 * has taken its start and sequence and not yet walked; a walk without the
 * big lock gave up for the full walk; the full walk has looked a name up
 * and not yet used the answer; the full walk is about to take the namespace
 * lock of a mount it moves onto; the full walk found the sequence moved and
 * starts again; a call that changes what its walk found has its walk's
 * answer and has not yet taken the locks of its change; such a call found
 * the sequence moved once it had them and starts again; a walk sleeps until
 * no change is under way; ext2 is about to load an inode that no node in
 * memory has, with no node's lock held and its record not yet published
 * (`node_for`); the block cache is about to read a block from its device
 * (`fill_victim`), which a test only counts, since the reader may hold a
 * directory's lock; a call has let go of its walk's namespace lock and
 * still uses the answer its walk keeps (`vfs_walk_unlock`); an open that
 * creates has found its name missing and is about to look for a link there
 * to create through (`open_link_target`); a change reaches the type,
 * which a test only counts: a creation, a removal, a rename, a truncation,
 * a change of attributes, or the publication of a mount or a bind; the
 * full walk is about to take a `..`; the full walk has read a symbolic
 * link's target and not yet moved to it; the full walk has reached its end
 * and not yet made its last check (`walk_done`); and a walk without the big
 * lock has followed a symbolic link and not yet taken the next name
 * (`cached_link`). */
#define WALK_GATE_FULL     0
#define WALK_GATE_UNLOCKED 1
#define WALK_GATE_MISS     2
#define WALK_GATE_LOOKUP   3
#define WALK_GATE_NSLOCK   4
#define WALK_GATE_RESTART  5
#define WALK_GATE_HANDOFF  6
#define WALK_GATE_RETRY    7
#define WALK_GATE_WAIT     8
#define WALK_GATE_LOAD     9
#define WALK_GATE_READ     10
#define WALK_GATE_KEPT     11
#define WALK_GATE_LINK     12
#define WALK_GATE_EFFECT   13
#define WALK_GATE_DOTDOT   14
#define WALK_GATE_FOLLOW   15
#define WALK_GATE_DONE     16
#define WALK_GATE_CACHED   17
#define WALK_GATE_EVENTS   18

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

/** Whether the walk gate's `ioerr` is armed for the calling thread, which
 * this disarms: the block cache then takes its next read from the device
 * as one that failed (`fill_victim`), so that a test sees a walk whose
 * lookup's read fails. */
int walk_gate_ioerr(void);

/** Register /sys/kernel/debug/vfs/walk_gate, once debugfs is up. */
void walk_gate_init(void);

#endif /* _WALKSEQ_H_ */
