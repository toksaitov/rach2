#ifndef _PROCESS_H_
#define _PROCESS_H_

#include "../lib/kdef.h"
#include "../lib/kint.h"
#include "../lib/ksignal.h"
#include "../fs/fd.h"
#include "../mm/vm.h"
#include "fpu.h"

/* Policy: the threads one real user may have, each thread of a process
 * counting, unless `prlimit64` or `/etc/rach2/limits` says otherwise, and
 * what `sysconf` reports; the bound on processes is memory. Root is not
 * limited. */
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
#define RLIM_STACK      3
#define RLIM_CORE       4
#define RLIM_COUNT      5
#define RLIM_INFINITY32 0xFFFFFFFFu

/* The soft RLIMIT_STACK a process starts with, 8 MiB, which is Linux's;
 * an exec that runs with ids its caller lacks lowers a larger one to it. */
#define RLIM_STACK_DEFAULT 0x800000u

/* Policy: the working directory's path is kept in the record when it fits
 * here, which nearly every path does; a longer one, up to PATH_MAX, is kept
 * in the heap (`cwd_long`). */
#define CWD_MAX 64

/*
 * The working directory, the root directory and the file-creation mask,
 * which clone with CLONE_FS shares, as Linux shares its fs_struct: every
 * thread a C library makes shares them, `refs` counting the records that do.
 * fork gives the child a copy, and exec keeps them.
 */
struct vfs_mount;

struct fs_struct {
    int refs;
    /* The working directory's path, in `cwd` when it fits and in `cwd_long`
     * from the heap when it does not; `proc_cwd` reads whichever it is. */
    char cwd[CWD_MAX];
    char *cwd_long;
    /* The working directory's hold on a mount that can be unmounted: the
     * mount and the held node, or NULL on a permanent mount (vfs_cwd_set). */
    struct vfs_mount *cwd_mnt;
    void *cwd_node;
    /* The root directory chroot set: its mount and held node, or NULL for
     * the root of the mount table (vfs_root_set). Absolute paths start there
     * and `..` stops there. */
    struct vfs_mount *root_mnt;
    void *root_node;
    /* File-creation mask (umask(2)), low 9 bits. */
    uint32_t umask;
    /* The execs in progress, by threads without root that share this, that
     * apply set-id bits: while there are any, clone with CLONE_FS fails, so
     * no process outside the thread group comes to share the record before
     * those execs have their new ids (`exec_ids` in syscall_proc.c). */
    int in_exec;
};

/* Policy: the pages one process's system calls may copy paths into at once
 * (fs/pathbuf.c); the deepest call takes four. */
#define PATH_BUFS 8

/* Supplementary group slots per process (the 4.2BSD number). */
#define NGROUPS_MAX 8

struct vfs_mount;
struct mm;
struct tgroup;

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
    uint32_t pid;                 /* Process ID */
    proc_state_t state;           /* Current state */
    cpu_regs_t regs;              /* Saved CPU registers */
    uint32_t cr3;                 /* Page directory physical address */
    uint32_t kernel_stack;        /* Top of kernel stack for this process */
    uint32_t parent_pid;          /* Parent process ID */
    int32_t exit_code;            /* Exit status (for zombie) */
    fd_table_t *fds;              /* Descriptor table, shared under CLONE_FILES */
    struct fs_struct *fs;         /* Directories and umask, shared under CLONE_FS */
    volatile uint32_t in_syscall; /* Non-zero while a user syscall is executing */
    /* switch.s reads `regs.esp`, `cr3`, `kernel_stack`, `tls_base` and
     * `page_dir` at fixed offsets, so a new field goes at the end of the
     * record. process.c has the same offsets and checks them against the
     * compiler's, at build time where the compiler can and at boot. */
    uint32_t tls_base;    /* Per-process %gs TLS base (set_thread_area) */
    struct mm *mm;        /* The address space, shared by CLONE_VM (mm/mm.h) */
    uint32_t wakeup_tick; /* nanosleep: kernel tick to wake a timed sleeper (0 = none) */
    /* Signal state: the dispositions, `sigacts[signo]` with index 0 unused,
     * which are the thread group's (`tg->action`), and this thread's 64-bit
     * pending and blocked masks (bit signo-1). */
    struct k_sigaction *sigacts;
    uint64_t sig_pending;
    uint64_t sig_blocked;
    /* Program name (basename of the execve'd path, truncated, padded with
     * NULs to the end): the "comm" /proc/<pid>/stat reports and ps shows.
     * Set at execve and by prctl's PR_SET_NAME. */
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
    /* The workspace of the `execve` in progress, NULL outside one;
     * `syscall_proc.c` owns its type and gives it back when the call ends. */
    void *exec_ws;
    /* Interrupt handlers entered and not left on this process's stack;
     * kept in the record and not in one global, since a handler that
     * switches away leaves the count with the process it interrupted. */
    uint32_t irq_depth;
    /* The resource limits `prlimit64` keeps: threads of the real user,
     * open descriptors, the address space and the stack in bytes; soft and
     * hard, inherited by fork and exec. */
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
    /* The user address set_tid_address and CLONE_CHILD_CLEARTID name, whose
     * word is cleared and woken when the thread ends and another thread
     * shares the address space; 0 for none. */
    uint32_t clear_child_tid;
    /* The siginfo of each pending signal, kept with its pending bit. */
    struct k_siginfo sig_info[KSIG_MAX + 1];
    /* The alternate signal stack sigaltstack set: its base and size, and
     * whether one is set. Inherited by fork, cleared by execve. */
    uint32_t altstack_sp;
    uint32_t altstack_size;
    uint32_t altstack_on;
    /* The mask rt_sigsuspend, pselect6 or ppoll replaced for its wait, with
     * `sigmask_saved`: the frame of the handler the wait ended for carries
     * it, so sigreturn puts it back, and a return to user mode with no
     * handler puts it back at once. */
    uint64_t saved_sigmask;
    uint32_t sigmask_saved;
    /* One more than the number of a system call that returned EINTR and
     * that a signal without a handler, or one whose handler has SA_RESTART,
     * restarts; 0 for none. Set by the dispatcher, consumed by
     * deliver_signals on the same return. */
    uint32_t restart_nr;
    /* The trap number, error code and fault address of the last processor
     * exception, for the signal frame's sigcontext. */
    uint32_t trap_nr;
    uint32_t trap_err;
    uint32_t trap_addr;
    /* The pages this process's system calls copy their paths into, and which
     * of them a call is using now, bit i for page i: taken as a call first
     * needs more at once than the record has, and kept, so that a path costs
     * no frame after the first (fs/pathbuf.c). */
    char *path_bufs[PATH_BUFS];
    uint32_t path_bufs_busy;
    /* CPU time in ticks. The timer adds the tick it interrupts to `utime`
     * when this thread was in user mode and to `stime` when it was in the
     * kernel, and a tick of an idle wait to neither. A fork starts both at
     * zero; an exec keeps them. The reaped children's time is the thread
     * group's (`tgroup.h`). */
    uint32_t utime;
    uint32_t stime;
    /* The nice value `setpriority` keeps, -20 to 19; fork passes it on and
     * exec keeps it. The scheduler does not read it. */
    int32_t nice;
    /* What prctl keeps. The signal posted to this process when its parent
     * exits, 0 for none: a fork does not pass it on, and it is cleared when
     * an exec or an identity call moves the effective ids and by an exec
     * that runs with ids its caller lacks. Whether the process and its
     * descendants may gain no privilege, which a fork passes on and nothing
     * clears, and under which exec ignores set-id bits. Whether the process
     * is dumpable is its address space's (`mm.h`). */
    uint32_t pdeath_signal;
    uint32_t no_new_privs;
    /* Set when the thread gave up its capabilities with capset. The kernel
     * keeps every capability or none, so the thread is then not privileged,
     * whatever its user IDs (`proc_privileged`). A fork passes it on, and an
     * exec clears it where Linux gives a root image its capabilities again
     * (`exec_ids`). */
    uint32_t nocaps;
    /* Set while a parent that made this process with CLONE_VFORK waits for
     * it to call execve or end; cleared, and the parent woken, at the one or
     * the other (`proc_vfork_release`). */
    uint32_t vfork_waiting;
    /* The thread group (tgroup.h) and its id, the leader's `pid`, which is
     * the process id getpid reports; `pid` is this thread's id. */
    struct tgroup *tg;
    uint32_t tgid;
    /* The robust futex list set_robust_list names, walked at the thread's
     * end, and its length; 0 for none. */
    uint32_t robust_list;
    uint32_t robust_len;
    /* Set once this thread has been through the stop in effect in its
     * process, which a tracer may then have resumed it from; cleared when
     * the process continues. */
    uint32_t stop_seen;
    /* ptrace (ptrace.h): the PT_ flags; the tracer's process id; the options
     * PTRACE_SETOPTIONS set; the wait status of the ptrace stop the thread is
     * in, 0 when it is in none; whether the tracer's wait reported that
     * stop; the signal the tracer resumes it with; the message
     * PTRACE_GETEVENTMSG reads; the siginfo of the stop; the system call a
     * stop is in, -1 for none; whether the tracer has seen the process end;
     * and the debug registers DR0 to DR7 as the tracer set them, DR6 as the
     * last debug exception left it. */
    uint32_t ptrace;
    uint32_t tracer;
    uint32_t ptrace_opts;
    uint32_t ptrace_status;
    uint32_t ptrace_reported;
    int32_t ptrace_signo;
    uint32_t ptrace_msg;
    struct k_siginfo ptrace_info;
    int32_t orig_eax;
    uint32_t ptrace_exit_seen;
    uint32_t debugreg[8];
    /* The execution domain and its flags personality(2) keeps; fork passes
     * it on and exec keeps it. */
    uint32_t persona;
    /* The descriptors of its shared table the current call has looked up,
     * one bit each, which a close by another sharer leaves in place until
     * the call ends (`fd_holds_release`); `fd_holding` says whether any bit
     * is set. */
    uint32_t fd_held[FD_TABLE_LIMIT / 32];
    int fd_holding;
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

/** Take a record off the list and give it back to the heap with its exec
 * workspace, and with its descriptor table when this was the table's last
 * reference and its descriptors were released; the caller drops the address
 * space (`mm_drop`) and frees the kernel stack first. Nothing may refer to
 * the record afterwards. */
void free_process(process_t *proc);

/** Get process by PID; returns the record, or NULL if not found. */
process_t *get_process(uint32_t pid);

/* Whether thread `p` has privilege, which Linux divides into capabilities
 * and this kernel keeps whole: an effective user ID of 0, with the
 * capabilities not given up (`nocaps`). A macro, so that the host tests that
 * compile the callers without this file's functions need no stub of it. */
#define proc_privileged(p) ((p)->euid == 0 && !(p)->nocaps)

/** The CPU time in ticks of the whole process `p` is a thread of: every
 * thread's, those that have ended included, user and system. */
void proc_cpu_time(const process_t *p, uint32_t *user, uint32_t *sys);

/* The records of ended threads that are not leaders, which no wait reaps;
 * the next return to user mode frees them (`deliver_signals`). */
extern uint32_t proc_dead_threads;

/** Clear the word CLONE_CHILD_CLEARTID names and wake a futex waiter on it,
 * when another thread shares the address space, and release the robust
 * futexes the thread held; the start of a thread's end. */
void proc_thread_gone(process_t *p);

/**
 * Reparent every child of `dead_pid` onto init (`init_pid`), waking init to
 * reap any child that is already a zombie, and post each living child the
 * parent-death signal it asked for. Called from the death paths so an exiting
 * process never orphans a child onto a record about to be freed. No child is
 * adopted when there is no init or when init itself is dying.
 */
void reparent_children(uint32_t dead_pid);

/** End the current thread: release its files and, with the process's last
 * thread, reparent the children onto init, publish the encoded exit status
 * and wake the parent; nonzero when the process ended. See exit.c. */
int proc_exit_cleanup(int32_t status);

/** Begin the end of the current process: send every other thread SIGKILL,
 * and have the process end with the encoded wait status `code` whichever
 * thread ends last. A process already ending keeps its status. */
void proc_group_exit(int32_t code);

/** Let the parent a CLONE_VFORK clone made `p` for run again, at `p`'s
 * execve or end; nothing when no parent waits. */
void proc_vfork_release(process_t *p);

/** Drop `p`'s reference to its descriptor table and leave it none; the last
 * reference closes every descriptor and frees the table. May sleep. */
void proc_files_put(process_t *p);

/** Give the calling thread a descriptor table of its own when it shares one:
 * a copy whose descriptors share the table's descriptions, as close_range
 * does under CLOSE_RANGE_UNSHARE; the other sharers keep the table. execve
 * makes the same copy as it commits, in storage it took before. 0, or
 * -ENOMEM. */
int32_t proc_files_unshare(void);

/** Drop `p`'s reference to its directories and umask and leave it none;
 * the last reference lets go of the working directory's and the root's
 * holds. May sleep. */
void proc_fs_put(process_t *p);

/** Wait, as the parent, until the child `pid` made with CLONE_VFORK calls
 * execve or ends. */
void proc_vfork_wait(uint32_t pid);

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

/* Post `signo` with the siginfo `info` records for its handler. */
void signal_post_info(process_t *target, int signo, const struct k_siginfo *info);

/** Post signal `signo` to the thread `t` alone, as tkill and tgkill do,
 * with the siginfo `info`, and wake it. */
void signal_post_thread(process_t *t, int signo, const struct k_siginfo *info);

/* Post a signal a processor exception raised: a handler that is blocked or
 * ignored cannot run, so the disposition goes back to default and the signal
 * is unblocked, and the process is killed unless a handler is installed. */
void signal_force(process_t *target, int signo, const struct k_siginfo *info);

/** Make `mask`, less SIGKILL and SIGSTOP, the current thread's blocked mask,
 * and wake another thread to take a signal sent to the process that the new
 * mask blocks and the old did not. Every change of a thread's own mask is
 * made through it. Defined in syscall_proc.c. */
void signal_set_blocked(uint64_t mask);

/* The siginfo of the SIGCHLD a child's end sends: CLD_EXITED with the exit
 * code, or CLD_KILLED or CLD_DUMPED with the signal, from its wait status. */
void proc_child_info(const process_t *child, struct k_siginfo *info);

/**
 * The tick `ticks` after `now` at which a real-time timer is due, never 0,
 * which `alarm_tick` keeps for a disarmed timer.
 */
uint32_t proc_alarm_at(uint32_t now, uint32_t ticks);

/**
 * Non-zero if the current process has a pending signal that is neither
 * blocked nor ignored: one that is caught, or whose default action
 * terminates or stops the process; or a stop its tracer asked for with
 * PTRACE_INTERRUPT. Blocking syscall loops poll it to break out so the
 * signal, or the stop, is acted on. Defined in syscall_proc.c.
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
