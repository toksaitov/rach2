/* The walk sequence of fs/walkseq.c for a single-threaded host test: the
 * same counts, with no lock, no waiters and no walk gate to arm. */
#include "fs/walkseq.h"

/* The sequence, which moves at each beginning and end of a change, and the
 * count of the changes under way. */
static uint64_t seq_now;
static uint32_t seq_active;

/** Begin a change: one more under way, and the sequence moves. */
void vfs_seq_begin(void)
{
    seq_active++;
    seq_now++;
}

/** End a change: one fewer under way, and the sequence moves. */
void vfs_seq_end(void)
{
    seq_active--;
    seq_now++;
}

/** The sequence into `*seq`; 1 when no change is under way. */
int vfs_seq_read(uint64_t *seq)
{
    *seq = seq_now;
    return seq_active == 0;
}

/** Whether no change is under way and none has begun since `seq`. */
int vfs_seq_still(uint64_t seq)
{
    return seq_active == 0 && seq_now == seq;
}

/** The sequence into `*seq`; no other thread can end a change, so a wait
 * with one under way is a fault of the test. */
void vfs_seq_wait(uint64_t *seq)
{
    if (!vfs_seq_read(seq)) {
        __builtin_trap();
    }
}

/** Whether the thread began a change it has not ended. */
int vfs_seq_mine(void)
{
    return seq_active != 0;
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
