/* The walk sequence of fs/walkseq.c for a single-threaded host test: the
 * same counts, with no lock, no waiters and no walk gate to arm, and a
 * switch that lets the test make changes as another thread. */
#include "fs/walkseq.h"

/* The sequence, which moves at each beginning and end of a change, and the
 * count of the changes under way; and of those, as fs/walkseq.c keeps them
 * for each thread, the changes under way the test's thread began and the
 * moves they made. `seq_other` set makes the changes that begin and end
 * another thread's (`walkseq_host_other`). */
static uint64_t seq_now;
static uint32_t seq_active;
static uint32_t own_tokens;
static uint64_t own_moves;
static int seq_other;

/** Make the changes that begin and end from now another thread's when `on`
 * is set, and the test thread's own, as at the start, when it is clear. */
void walkseq_host_other(int on)
{
    seq_other = on;
}

/** Begin a change: one more under way, and the sequence moves. */
void vfs_seq_begin(void)
{
    seq_active++;
    seq_now++;
    if (!seq_other) {
        own_tokens++;
        own_moves++;
    }
}

/** End a change: one fewer under way, and the sequence moves. */
void vfs_seq_end(void)
{
    if (!seq_other && own_tokens > 0) {
        own_tokens--;
        own_moves++;
    }
    seq_active--;
    seq_now++;
}

/** The sequence into `*seq`, less the moves of the thread's own changes,
 * as fs/walkseq.c takes them out; 1 when no change is under way but the
 * thread's own. */
int vfs_seq_read(uint64_t *seq)
{
    *seq = seq_now - own_moves;
    return seq_active == own_tokens;
}

/** Whether no other thread's change is under way or has come since
 * `seq`. */
int vfs_seq_still(uint64_t seq)
{
    return seq_active == own_tokens && seq_now - own_moves == seq;
}

/** `vfs_seq_still` and `vfs_seq_begin` as one step. */
int vfs_seq_begin_if(uint64_t seq)
{
    if (!vfs_seq_still(seq)) {
        return 0;
    }
    vfs_seq_begin();
    return 1;
}

/** The sequence into `*seq`, with no other thread's change to wait for. */
void vfs_seq_wait(uint64_t *seq)
{
    (void)vfs_seq_read(seq);
}

/** Whether the thread began a change it has not ended. */
int vfs_seq_mine(void)
{
    return own_tokens != 0;
}

/* The last walk's stamp, the single thread's. */
static struct vfs_stamp stamp_last;

/** Leave `seq` as the last walk's stamp, checked when `on`. */
void vfs_stamp_set(uint64_t seq, int on)
{
    stamp_last.seq = seq;
    stamp_last.on = on;
}

/** The last walk's stamp, into `*st`. */
void vfs_stamp_get(struct vfs_stamp *st)
{
    *st = stamp_last;
}

/** Whether nothing has changed what a path names since `st`. */
int vfs_stamp_still(const struct vfs_stamp *st)
{
    return !st->on || vfs_seq_still(st->seq);
}

/** Whether the stamp a call consumes still holds: always, with no other
 * thread. */
int vfs_consume_still(void)
{
    return 1;
}

/** `vfs_stamp_still` and the beginning of a change as one step. */
int vfs_stamp_begin(const struct vfs_stamp *st)
{
    if (!vfs_stamp_still(st)) {
        return 0;
    }
    vfs_seq_begin();
    return 1;
}

/** The walk gate, which no host test arms. */
void walk_gate(int event)
{
    (void)event;
}

/** The walk gate's fault, which no host test arms. */
int walk_gate_fault(void)
{
    return 0;
}
