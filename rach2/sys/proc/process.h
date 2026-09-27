#ifndef _PROCESS_H_
#define _PROCESS_H_

#include "../lib/kdef.h"
#include "../lib/kint.h"
#include "../lib/ksignal.h"
#include "../fs/fd.h"
#include "../mm/vm.h"
#include "fpu.h"

/* Policy: the processes one real user may have unless `prlimit64` or
 * `/etc/rach2/limits` says otherwise, and what `sysconf` reports; the bound
 * on processes is memory. Root is not limited. */
#define PROC_COUNT_POLICY 4096

/* Process states. */
typedef enum {
    PROC_UNUSED,  /* Slot is free */
    PROC_READY,   /* Ready to run */
    PROC_RUNNING, /* Currently executing */
    PROC_BLOCKED, /* Waiting for I/O or event */
    PROC_ZOMBIE,  /* Exited, waiting for parent to collect */
    PROC_STOPPED  /* Job-control stopped (SIGSTOP/SIGTSTP), resumes on SIGCONT */
} proc_state_t;

/*
 * The record's register block. Only `esp` is used: the kernel stack pointer
 * `switch_context` saved when the process last switched away, or the one
 * built for its first switch. The other fields are read by nothing; they
 * keep `esp` at the offset switch.s reads.
 */
struct cpu_regs {
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp_dummy; /* Ignored by popal */
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    uint32_t eip;
    uint32_t eflags;
    uint32_t esp; /* Kernel stack pointer for switch_context */
};

typedef struct cpu_regs cpu_regs_t;

/* A limit as `prlimit64` keeps it: the soft value enforced, the hard one
 * the soft may be raised to, `RLIM_INFINITY32` for none. */
struct rlimit32 {
    uint32_t cur;
    uint32_t max;
};
#define RLIM_NPROC      0
#define RLIM_NOFILE     1
#define RLIM_AS         2
#define RLIM_COUNT      3
#define RLIM_INFINITY32 0xFFFFFFFFu

/* Policy: the working directory's path is kept in the record, and a
 * directory whose path is longer cannot be made current (ENAMETOOLONG). */
#define CWD_MAX 64

/* Supplementary group slots per process (the 4.2BSD number). */
#define NGROUPS_MAX 8

struct vfs_mount;

/* A sleeper's record on a wait queue: one is in every process record, for
 * the one queue a process sleeps on, and `poll` supplies one per watched
 * object from an array it took before it began, so entering a queue
 * allocates nothing. */
struct wq_waiter {
    struct process *proc;
    struct wq_waiter *next;
};

struct bbuf; /* the buffer cache's buffer, bcache.h */

/* Process Control Block (PCB). */
struct process {
    uint32_t pid;          /* Process ID */
    proc_state_t state;    /* Current state */
    cpu_regs_t regs;       /* Saved CPU registers */
    uint32_t cr3;          /* Page directory physical address */
    uint32_t kernel_stack; /* Top of kernel stack for this process */
    uint32_t parent_pid;   /* Parent process ID */
    int32_t exit_code;     /* Exit status (for zombie) */
    fd_table_t fds;        /* File descriptor table */
    char cwd[CWD_MAX];     /* Current working directory (absolute, canonical; "/" at start) */
    volatile uint32_t in_syscall; /* Non-zero while a user syscall is executing */
    /* switch.s reads `regs.esp`, `cr3`, `kernel_stack`, `tls_base` and
     * `page_dir` at fixed offsets, so a new field goes at the end of the
     * record. process.c has the same offsets and checks them against the
     * compiler's, at build time where the compiler can and at boot. */
    uint32_t tls_base;    /* Per-process %gs TLS base (set_thread_area) */
    vm_space_t vm;        /* Address-space map (VMAs + brk + stack limit) */
    uint32_t wakeup_tick; /* nanosleep: kernel tick to wake a timed sleeper (0 = none) */
    /* Signal state: per-signal disposition (sigacts[signo], index 0 unused)
     * plus 64-bit pending and blocked masks (bit signo-1). */
    struct k_sigaction sigacts[KSIG_MAX + 1];
    uint64_t sig_pending;
    uint64_t sig_blocked;
    /* Program name (basename of the execve'd path, NUL-terminated, truncated):
     * the "comm" /proc/<pid>/stat reports and ps shows. Set at execve. */
    char comm[16];
    /* Session + process-group identity. A process belongs to process group
     * `pgid` within session `sid`; fork inherits both, setsid/setpgid change
     * them. The session leader has sid==pid; a group leader has pgid==pid. */
    uint32_t sid;
    uint32_t pgid;
    /* Job-control status pending for the parent's wait4: 0 = none, a Linux
     * WIFSTOPPED value ((stopsig<<8)|0x7f) when stopped, or 0xffff when
     * continued. Consumed (cleared) by a matching wait4(WUNTRACED/WCONTINUED).
     * This single slot has one event: a second transition (a stop just after
     * an unreaped continue, or the reverse) supersedes the first, so a parent
     * that does not reap promptly can miss one stop or continue report. */
    uint32_t jc_status;
    /* File-creation mask (umask(2)); inherited across fork, low 9 bits. */
    uint32_t umask;
    /* Credentials: the V7 model with POSIX saved IDs. Real, effective, and
     * saved uid and gid plus the supplementary groups; fork copies them, exec
     * passes the real ids through, applies the file's setuid/setgid bits to
     * the effective ids, and sets the saved ids to the new effective ids (the
     * POSIX exec rule, set-id or not); the id syscalls move them under the
     * textbook rules: effective uid 0 sets anything, anyone else only swaps
     * among its own three. All zero (root) at boot until a login server
     * drops. */
    uint32_t ruid;
    uint32_t euid;
    uint32_t suid;
    uint32_t rgid;
    uint32_t egid;
    uint32_t sgid;
    uint32_t ngroups;             /* live entries in groups[] */
    uint32_t groups[NGROUPS_MAX]; /* supplementary gids */
    /* Set once this process has been reported for running out of mappings, so
     * the diagnostic is said once and not once per failed call. It belongs to
     * the process and not to a remembered pid, which a later process reusing
     * that pid would silence. */
    uint32_t vma_reported;
    /* The working directory's hold on a mount that can be unmounted: the
     * mount and the held node, or NULL on a permanent mount (vfs_cwd_set). */
    struct vfs_mount *cwd_mnt;
    void *cwd_node;
    /* The x87 and SSE state, saved at every switch away and loaded at every
     * switch to; `fxsave` needs the 16-byte alignment. */
    uint8_t fpu[FPU_STATE_SIZE] __attribute__((aligned(16)));
    /* The page directory `cr3` names, or NULL for the kernel's: the record
     * of it, since a directory lives in the heap and its physical address
     * says nothing about where the kernel reads it. `switch.s` reads this
     * offset too. */
    struct page_dir *page_dir;
    /* The list of live records, in the order they were made; the scheduler
     * walks it round-robin. */
    struct process *next;
    struct process *prev;
    /* The record this process sleeps with, see `struct wq_waiter`. */
    struct wq_waiter waiter;
    /* The `execve` workspace, allocated at the first exec and kept for the
     * record's life; `syscall.c` owns its type. */
    void *exec_ws;
    /* Interrupt handlers entered and not left on this process's stack;
     * kept in the record and not in one global, since a handler that
     * switches away leaves the count with the process it interrupted. */
    uint32_t irq_depth;
    /* The resource limits `prlimit64` keeps: processes of the real user,
     * open descriptors, and the address space in bytes; soft and hard,
     * inherited by fork and exec. */
    struct rlimit32 rlim[RLIM_COUNT];
    /* Inside `mem_reclaim`: its own allocations, the requests a writeback
     * needs, may take from the reserve. */
    uint32_t reclaiming;
    /* An interrupt asked for a switch while this process was inside a
     * system call; the call's exit takes it. */
    volatile uint32_t need_resched;
    /* The buffer cache's list of the buffers this process holds, kept by
     * the cache so its hold rules are checked against them alone. */
    struct bbuf *bcache_held;
    /** One preference after a wait-queue wake; consumed when selected. */
    uint32_t woken;
    /* The real-time interval timer, `setitimer(ITIMER_REAL)`: the tick its
     * SIGALRM is due at, 0 when it is disarmed, and the ticks between one
     * SIGALRM and the next, 0 for a single one. A fork starts with both
     * zero, since the child inherits no timer; an exec keeps them. */
    uint32_t alarm_tick;
    uint32_t alarm_interval;
    /* CPU time in ticks. The timer adds the tick it interrupts to `utime`
     * when this process was in user mode and to `stime` when it was in the
     * kernel, and a tick of an idle wait to neither. A reap adds a child's
     * two, with the children the child reaped, to `cutime` and `cstime`.
     * A fork starts all four at zero; an exec keeps them. */
    uint32_t utime;
    uint32_t stime;
    uint32_t cutime;
    uint32_t cstime;
    /* The nice value `setpriority` keeps, -20 to 19; fork passes it on and
     * exec keeps it. The scheduler does not read it. */
    int32_t nice;
};

typedef struct process process_t;

/* The live processes, first to last; a walk is
 * `for (p = proc_list; p; p = p->next)`. */
extern process_t *proc_list;

/* Process 0, the kernel's idle process, which the debug shell returns to. */
extern process_t *proc_idle;

/* Currently running process. */
extern process_t *current_proc;

/** Whether this processor is inside a user process's system call. */
int proc_in_syscall(void);

/** Reset the process table to its boot state and verify the PCB layout. */
void init_proc(void);

/** Restart pid numbering at 1, for the boot that is about to start init. */
void proc_reclaim_boot_pids(void);

/** A process record from the heap, on the list; NULL when the heap has no
 * room. */
process_t *alloc_process(void);

/** Take a record off the list and give it back to the heap with its
 * descriptor-table storage, region array and exec workspace; the caller
 * frees the user pages, the page directory and the kernel stack first.
 * Nothing may refer to the record afterwards. */
void free_process(process_t *proc);

/** Get process by PID; returns the record, or NULL if not found. */
process_t *get_process(uint32_t pid);

/**
 * Reparent every child of `dead_pid` onto init (`init_pid`), waking init to
 * reap any child that is already a zombie. Called from the death paths so an
 * exiting process never orphans a child onto a record about to be freed.
 * No-op when there is no init or when init itself is dying.
 */
void reparent_children(uint32_t dead_pid);

/** Release files, reparent the children onto init, publish the encoded exit
 * status, and wake the parent; see exit.c. */
void proc_exit_cleanup(int32_t status);

/** Set a process's `comm` name from the final component of `path`. */
void proc_set_comm(process_t *p, const char *path);

/**
 * Wake timed sleepers whose deadline has reached `now`, and post SIGALRM to
 * each process whose real-time timer is due, arming it again for its
 * interval or leaving a single alarm disarmed.
 */
void proc_wake_sleepers(uint32_t now);

/**
 * Post signal `signo` to `target`: set its pending bit and wake it if blocked.
 * Delivered on the target's next return to user mode. Defined in
 * syscall_proc.c.
 */
void signal_post(process_t *target, int signo);

/**
 * The tick `ticks` after `now` at which a real-time timer is due, never 0,
 * which `alarm_tick` keeps for a disarmed timer.
 */
uint32_t proc_alarm_at(uint32_t now, uint32_t ticks);

/**
 * Non-zero if the current process has a pending signal that is neither
 * blocked nor ignored: one that is caught, or whose default action
 * terminates or stops the process. Blocking syscall loops poll it to break
 * out so the signal is acted on. Defined in syscall_proc.c.
 */
int signal_pending(void);

/**
 * Deliver a pending signal to the current process on the way back to user mode.
 * Called from the ISR/IRQ return path with the trapframe. Defined in
 * syscall_proc.c.
 */
struct isr_params;
void deliver_signals(struct isr_params *tf);

/**
 * Context switch between two processes; saves current state to `old`, restores
 * from `new_proc`. Defined in switch.s.
 */
extern void switch_context(process_t *old, process_t *new_proc);

/** Prime a new process's kernel stack for its first context switch. */
void setup_process_stack(process_t *proc, void (*entry_point)(void));

/**
 * How many processes have been created since boot.
 *
 * What Linux's `/proc/stat` calls `processes`: a running total, not the number
 * alive now. Bumped once where a process is handed out, so a fork and an exec
 * that replaces an image are counted the way that file counts them.
 */
uint32_t process_forks_since_boot(void);

/**
 * How many sleeps a signal has ended before their deadline since boot.
 *
 * `/proc/stat` reports it as `sleeps_interrupted`. A signal that reaches a
 * sleeper is meant to end its sleep where it lands, not at the deadline, and
 * this is the count that says which happened: a sleep the signal cut short
 * raises it, and one that ran its whole length does not. It is a count and
 * not a time, so a test may hold it on any machine.
 */
uint32_t process_sleeps_cut_short(void);

/** Count one sleep a signal ended before its deadline. */
void process_count_sleep_cut_short(void);

/** Count one process handed out; `alloc_process` calls it. */
void process_count_fork(void);

#endif
