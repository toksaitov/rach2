/*
 * The process record, one for each thread, and the calls that make, find,
 * signal and end processes.
 */
#ifndef _PROCESS_H_
#define _PROCESS_H_

#include "../lib/kdef.h"
#include "../lib/kint.h"
#include "../lib/ksignal.h"
#include "../fs/fd.h"
#include "../mm/vm.h"
#include "../fpu.h"
#include "percpu.h"

/* Policy: the threads one real user may have, each thread of a process
 * counting, unless `prlimit64` or `/etc/rach2/limits` says otherwise, and
 * what `sysconf` reports; the bound on processes is memory. Root is not
 * limited. */
#define PROC_COUNT_POLICY 4096

/* The states of a record. */
typedef enum {
    PROC_UNUSED,  /* No process: fork building it, or a reaper freeing it */
    PROC_READY,   /* On its processor's run queue */
    PROC_RUNNING, /* Running on a processor */
    PROC_BLOCKED, /* Asleep on a wait queue or until a tick */
    PROC_ZOMBIE,  /* Ended; the record waits for its reap */
    PROC_STOPPED  /* In a job-control or ptrace stop */
} proc_state_t;

/* A limit as `prlimit64` keeps it: the soft value enforced, the hard one
 * the soft may be raised to, `RLIM_INFINITY` for none. The values are
 * longs, as Linux keeps them: 32 bits on i686, where a value past them is no
 * limit, and 64 bits on x86-64. */
struct proc_rlimit {
    unsigned long cur;
    unsigned long max;
};
/* The slots of the record's `rlim`, one for each limit the kernel keeps;
 * prlimit64's resource numbers are Linux's, which differ (`rlimit_slot` in
 * syscall_proc.c). */
#define RLIM_NPROC    0
#define RLIM_NOFILE   1
#define RLIM_AS       2
#define RLIM_STACK    3
#define RLIM_CORE     4
#define RLIM_FSIZE    5
#define RLIM_COUNT    6
#define RLIM_INFINITY (~0UL)

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
    /* The working directory's hold: its mount and node, the node held on a
     * type with `hold`, or NULL on /dev and /proc (vfs_cwd_set). */
    struct vfs_mount *cwd_mnt;
    void *cwd_node;
    /* The root directory chroot set: its mount and held node, or NULL for
     * the root of the mount table (vfs_root_set). Absolute paths start there
     * and `..` stops there. */
    struct vfs_mount *root_mnt;
    void *root_node;
    /* File-creation mask (umask(2)), low 9 bits. */
    uint32_t umask;
    /* The execs in progress, by threads that share this, that apply set-id
     * bits for a caller without privilege or give back the capabilities a
     * caller gave up: while there are any, clone with CLONE_FS fails, so no
     * process outside the thread group comes to share the record before
     * those execs have their new ids (`exec_ids` in syscall_proc.c). */
    int in_exec;
};

/* Policy: the pages one process's system calls may copy paths into at once
 * (fs/pathbuf.c); the deepest call takes four. */
#define PATH_BUFS 8

/* The deepest nesting of user and of PID namespaces (ns.h), the first
 * counted as level 0. */
#define NS_LEVELS 8

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

/*
 * The record of one thread, from the heap (`alloc_process`), on `proc_list`
 * while it lives. The threads of a process share their thread group
 * (tgroup.h) and, with CLONE_VM, CLONE_FILES and CLONE_FS, the parts those
 * flags name.
 */
struct process {
    uint32_t pid;                 /* Thread ID; the process ID is `tgid` */
    proc_state_t state;           /* Current state */
    cpu_regs_t regs;              /* What switch_context saves (cpu.h) */
    uintptr_t space_root;         /* Page directory physical address, or 0 */
    uintptr_t kernel_stack;       /* Top of kernel stack for this process */
    uint32_t parent_pid;          /* Parent process ID */
    int32_t exit_code;            /* Encoded wait status it ended with */
    fd_table_t *fds;              /* Descriptor table, shared under CLONE_FILES */
    struct fs_struct *fs;         /* Directories and umask, shared under CLONE_FS */
    volatile uint32_t in_syscall; /* Nonzero in a system call or an exit */
    /* i686's switch.s reads `regs.esp`, `kernel_stack` and `tls_base` at
     * fixed offsets, so a new field goes at the end of the record.
     * arch/i686/context.c has the same offsets and checks them against the
     * compiler's, at build time where the compiler can and at boot
     * (`arch_context_check`, from `init_proc`). */
    uintptr_t tls_base;   /* The thread pointer (arch_tls_set) */
    struct mm *mm;        /* The address space, shared by CLONE_VM (mm/mm.h) */
    uint32_t wakeup_tick; /* The tick a timed sleep ends at, 0 for none */
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
     * POSIX exec rule, set-id or not); the id calls change them by the
     * POSIX rules: a privileged caller sets any, and another only moves
     * among its own three. All zero (root) at boot until a login server
     * drops them. */
    uint32_t ruid;
    uint32_t euid;
    uint32_t suid;
    uint32_t rgid;
    uint32_t egid;
    uint32_t sgid;
    uint32_t ngroups;             /* live entries in groups[] */
    uint32_t groups[NGROUPS_MAX]; /* supplementary gids */
    /* Set once this process has been reported for running out of mappings, so
     * the diagnostic is said once and not once per failed call; an exec
     * clears it. */
    uint32_t vma_reported;
    /* The floating-point state, saved at every switch away and loaded at
     * every switch to: an area of `fpu_area_size` bytes from
     * `fpu_area_alloc`, which the record owns. */
    uint8_t *fpu;
    /* The page directory `space_root` names, or NULL for none: the record
     * of it, since a directory lives in the heap and its physical address
     * says nothing about where the kernel reads it. The switch compares it
     * with the directory the processor has loaded (`switch_space` on i686,
     * `switch_context` on x86-64). */
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
    /* Interrupt handlers entered and not left on this process's stack
     * (`irq_enter` in mm/pressure.c), counted in the record, since a
     * handler that switches away is left only when the process it
     * interrupted runs again. */
    uint32_t irq_depth;
    /* The resource limits `prlimit64` keeps, by the RLIM_ slots: threads of
     * the real user, open descriptors, and in bytes the address space, the
     * stack, a core file and a file written; soft and hard, inherited by
     * fork and exec. */
    struct proc_rlimit rlim[RLIM_COUNT];
    /* The depth of `mem_reclaim` calls the process is in: while it is above
     * 0, its own allocations, the requests a writeback needs, may take from
     * the reserve. */
    uint32_t reclaiming;
    /* An interrupt asked for a switch while this process was inside a
     * system call; the call's exit takes it. */
    volatile uint32_t need_resched;
    /* The buffer cache's list of the buffers this process holds, kept by
     * the cache so its hold rules are checked against them alone. */
    struct bbuf *bcache_held;
    /* Set by a wait-queue wake (`wq_wake`): the scheduler may prefer the
     * process once at its next selection, and clears it when it selects the
     * process or the process blocks again. */
    uint32_t woken;
    /* Whether its sleep is one no signal ends (`wq_wait_event_nointr`),
     * which /proc reports as the state D and counts as blocked. */
    uint32_t uninterruptible;
    /* The frame allocator refused this thread a user page since its last
     * page fault began: the fault is one of memory, not of the address. */
    uint32_t frame_refused;
    /* It waits, with its registers and floating-point state saved, for the
     * core another thread of its process writes (`core_park`). */
    uint32_t core_parked;
    /* The real-time interval timer, `setitimer(ITIMER_REAL)`: the tick its
     * SIGALRM is due at, 0 when it is disarmed, and the ticks between one
     * SIGALRM and the next, 0 for a single one. A fork starts with both
     * zero, since the child inherits no timer; an exec keeps them. */
    uint32_t alarm_tick;
    uint32_t alarm_interval;
    /* The user address set_tid_address and CLONE_CHILD_CLEARTID name, whose
     * word is cleared and woken when the thread ends and another thread
     * shares the address space; 0 for none. */
    uintptr_t clear_child_tid;
    /* The siginfo of each pending signal, kept with its pending bit. */
    struct k_siginfo sig_info[KSIG_MAX + 1];
    /* The alternate signal stack sigaltstack set: its base and size, and
     * whether one is set. Inherited by fork, cleared by execve. */
    uintptr_t altstack_sp;
    unsigned long altstack_size;
    uint32_t altstack_on;
    /* The mask rt_sigsuspend, pselect6 or ppoll replaced for its wait, with
     * `sigmask_saved`: the frame of the handler the wait ended for saves it,
     * so sigreturn puts it back, and a return to user mode with no handler
     * puts it back at once. */
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
    uintptr_t trap_addr;
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
    /* The tick this thread was made at, which its /proc stat file gives as
     * its start time; an exec keeps it. */
    uint32_t start_ticks;
    /* The nice value `setpriority` keeps, -20 to 19; fork passes it on and
     * exec keeps it. The scheduler does not read it. */
    int32_t nice;
    /* The I/O priority `ioprio_set` keeps, its class << 13 | its level, as
     * Linux encodes it; fork passes it on and exec keeps it. No disk queue
     * reads it. */
    uint16_t ioprio;
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
    uintptr_t robust_list;
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
    int32_t orig_nr;
    uint32_t ptrace_exit_seen;
    uintptr_t debugreg[8];
    unsigned long debug_pending; /* DR6 deferred from a kernel debug exception */
    /* The execution domain and its flags personality(2) keeps; fork passes
     * it on and exec keeps it. */
    uint32_t persona;
    /* The descriptors of its shared table the current call has looked up,
     * one bit each, which a close by another sharer leaves in place until
     * the call ends (`fd_holds_release`); `fd_holding` says whether any bit
     * is set. */
    uint32_t fd_held[FD_TABLE_LIMIT / 32];
    int fd_holding;
    /* The system call filters seccomp(2) installed, newest first, which the
     * thread shares with the threads and children that inherit them, and
     * the mode, SECCOMP_MODE_ (seccomp.h); a fork passes both on and an
     * exec keeps them. */
    struct seccomp_filter *seccomp;
    uint32_t seccomp_mode;
    /* The namespaces (ns.h): its user namespace and its PID namespace, NULL
     * for the first, its numbers in each PID namespace it is in below the
     * first, by level, and the set of its other namespaces, NULL for the
     * first set; and the capabilities it holds in its user namespace, in
     * capget's two words, every one for its creator and a process that
     * joined it, until an exec, a change of its effective uid or capset
     * changes them. A fork passes them on (`ns_fork`). */
    struct userns *userns;
    struct pidns *pidns;
    uint32_t nspid[NS_LEVELS];
    struct nsset *nsset;
    uint32_t ns_caps[2];

    /* The processor whose run queue the process is on, which is the one it
     * runs on while it runs (it is that processor's `current`) and the one
     * it last ran on after; the processors it may run on, a bit for each,
     * as sched_setaffinity(2) sets them and a fork passes them on; and its
     * depth in the big lock when it was switched away, which the switch
     * gives the processor back (sched.c). */
    int cpu;
    uint64_t affinity;
    int bkl_depth;
};

typedef struct process process_t;

/* The live processes, first to last; a walk is
 * `for (p = proc_list; p; p = p->next)`. */
extern process_t *proc_list;

/* Process 0, the record the boot and the debug shell run as; each
 * processor's idle task (smp.c) is another record, off the list. */
extern process_t *proc_idle;

/** The last process ID given out, 0 before the first. */
uint32_t proc_last_pid(void);

/* The running process, `current_proc`, is the running processor's
 * (percpu.h). */

/** Whether this processor's current process is inside a system call; no
 * for the idle process, a kernel thread and no process at all. The ATA
 * driver's wait and reclaim's writeback sleep only inside a call, and the
 * USB disk settles a stick only outside one. */
int proc_in_syscall(void);

/** Empty the process list, with no current process on this processor, and
 * panic when `arch_context_check` finds that a record the architecture's
 * assembly reads at fixed offsets, the process's on i686 and the
 * processor's on x86-64, has another layout. */
void init_proc(void);

/** Restart pid numbering at 1, for the boot that is about to start init. */
void proc_reclaim_boot_pids(void);

/** A new record from the heap, at the end of the list and READY: the next
 * pid not in use, below 2000000000, the leader of a thread group, a session
 * and a process group of its own, the default limits, a descriptor table
 * with the standard descriptions, the working directory /, the umask 022,
 * every processor in its affinity, and this processor's queue. NULL when
 * the heap has no room for it or one of its parts. Each record counts once
 * in `process_forks_since_boot`. */
process_t *alloc_process(void);

/* The bytes of a kernel stack: a process's, an idle task's, and the boot
 * stack loader.s reserves, which is as large. */
#define KERNEL_STACK_SIZE 0x4000

/** Take a record off the list and give it back to the heap with its exec
 * workspace, path pages and floating-point area, dropping its references
 * to its thread group, its directories, its seccomp filters and its
 * namespaces, and freeing its descriptor table when this was the table's
 * last reference and its descriptors were released; the caller drops the
 * address space (`mm_drop`) and frees the kernel stack first. Nothing may
 * refer to the record afterwards. A NULL `proc` is ignored. */
void free_process(process_t *proc);

/** The record whose `pid` is `pid`, a thread's id in the kernel's
 * numbering, that of the first PID namespace; NULL for none. */
process_t *get_process(uint32_t pid);

/** The user-mode registers of the thread `t`, which is not running in user
 * mode: the frame its entry to the kernel saved at the top of its kernel
 * stack. */
isr_params_t *proc_user_frame(const process_t *t);

/* Whether thread `p` has privilege, which Linux divides into capabilities
 * and this kernel keeps whole: an effective user ID of 0, with the
 * capabilities not given up (`nocaps`), in the first user namespace; a
 * process of another has privilege only over what its namespace owns
 * (`ns_privileged`). A macro, so that the host tests that compile the
 * callers without this file's functions need no stub of it. */
#define proc_privileged(p) ((p)->euid == 0 && !(p)->nocaps && !(p)->userns)

/** The CPU time in ticks of the whole process `p` is a thread of: every
 * thread's, those that have ended included, user and system. */
void proc_cpu_time(const process_t *p, uint32_t *user, uint32_t *sys);

/* The number of records of ended threads that are neither leaders nor
 * traced, which no wait reaps; the next return to user mode frees them
 * (`deliver_signals`). */
extern uint32_t proc_dead_threads;

/** Release the robust futexes the thread `p` held, then clear the word
 * CLONE_CHILD_CLEARTID names and wake a futex waiter on it, when another
 * thread shares the address space; the start of a thread's end and of its
 * execve's commit. Defined in futex.c. */
void proc_thread_gone(process_t *p);

/**
 * Give every child of the process `dead_pid` to init, or to the init of its
 * PID namespace when it is in one below the first (`ns_reaper`), and post
 * each child that has not ended the parent-death signal it asked prctl for.
 * A child that has already ended is reported to its new parent with SIGCHLD
 * and a wake of the parent's wait. No child goes to the system's init when
 * there is none (`init_pid` 0, a boot that runs the debug shell alone) or
 * init itself is the one ending. Called from the end of a process's last
 * thread, before its record can be reaped.
 */
void reparent_children(uint32_t dead_pid);

/**
 * End the current thread, whose encoded wait status is `status`: release
 * what it holds, its address space among them, and when it is the last
 * thread of its process, make the process ready for its reaper and notify
 * the parent, or the tracer first when another process traces the leader.
 * Returns nonzero when the process ended with this thread.
 *
 * From the return on, the parent or the tracer may reap the record and the
 * kernel stack, so the caller must neither block nor let an interrupt
 * switch it before its last `schedule`, and does not return to user mode.
 * A death by signal writes its core file before the call, while the address
 * space is there. Defined in exit.c.
 */
int proc_exit_cleanup(int32_t status);

/** Begin the end of the current process: send every other thread SIGKILL,
 * and have the process end with the encoded wait status `code` whichever
 * thread ends last. A process already ending keeps its status, and one in
 * another thread's execve is left to it. Defined in syscall_proc.c. */
void proc_group_exit(int32_t code);

/** End the calling thread with the wait status `code`, and with the last
 * thread the process. Does not return: an init that ends under the debug
 * shell goes back to the shell, on the boot stack. Defined in
 * syscall_proc.c. */
int32_t exit_current(int32_t code);

/** Let the parent a CLONE_VFORK clone made `p` for run again, at `p`'s
 * execve or end; nothing when no parent waits. Defined in syscall_proc.c. */
void proc_vfork_release(process_t *p);

/** Drop `p`'s reference to its descriptor table and leave it none; the last
 * reference closes every descriptor and frees the table. May sleep. Defined
 * in syscall_proc.c, as are the four calls after it. */
void proc_files_put(process_t *p);

/** Give the calling thread a descriptor table of its own when it shares one:
 * a copy whose descriptors share the table's descriptions, as close_range
 * does under CLOSE_RANGE_UNSHARE; the other sharers keep the table. execve
 * makes the same copy as it commits, in storage it took before. 0, or
 * -ENOMEM. */
int32_t proc_files_unshare(void);

/** Give the current process directories and a umask of its own, a copy of
 * those it shares with others, as unshare's CLONE_FS does. 0 or -ENOMEM. */
int32_t proc_fs_unshare(void);

/** Drop `p`'s reference to its directories and umask and leave it none;
 * the last reference lets go of the working directory's and the root's
 * holds. May sleep. */
void proc_fs_put(process_t *p);

/** Wait, as the parent, until the child `pid` made with CLONE_VFORK calls
 * execve or ends, or SIGKILL is pending for the caller; no other signal ends
 * the wait. */
void proc_vfork_wait(uint32_t pid);

/** Set `p`'s `comm` to the final component of `path`, cut to 15 bytes,
 * with null bytes to the end of `comm`. Nothing for a NULL `p` or `path`. */
void proc_set_comm(process_t *p, const char *path);

/**
 * Make ready each process BLOCKED in a timed sleep whose `wakeup_tick` has
 * come by the tick `now`, clearing it, and post SIGALRM to each process
 * whose real-time timer is due, arming it again for its interval or leaving
 * a single alarm disarmed. The boot processor's timer tick calls it.
 */
void proc_wake_sleepers(uint32_t now);

/**
 * Post `signo` to the process `target` is a thread of, with a siginfo whose
 * code is SI_KERNEL, as `signal_post_info` does. Defined in syscall_proc.c,
 * as are the three calls after it.
 */
void signal_post(process_t *target, int signo);

/**
 * Post `signo` to the process `target` is a thread of, with the siginfo
 * `info`: set the process's pending bit, keeping `info` when the signal was
 * not pending already, and wake, whatever the signal's disposition,
 * `target` when it can take the signal, else another thread that can, else
 * `target`. Job control acts at once: SIGCONT ends a stop and cancels the
 * pending stop signals, SIGKILL makes the stopped threads ready, and a stop
 * signal cancels a pending SIGCONT. A process whose threads have all ended
 * takes nothing. An interrupt handler may call it. The signal is delivered
 * at a return to user mode (`deliver_signals`).
 */
void signal_post_info(process_t *target, int signo, const struct k_siginfo *info);

/** Post signal `signo` to the thread `t` alone, as tkill and tgkill do,
 * with the siginfo `info`, and wake it. A thread that has ended takes
 * nothing. */
void signal_post_thread(process_t *t, int signo, const struct k_siginfo *info);

/** Post to the thread `target` the signal `signo` a processor exception
 * raised, with `info`. A handler that is ignored or blocked could not run,
 * so the disposition then goes back to the default and the signal is
 * unblocked: the process ends unless a handler is installed and not
 * blocked. */
void signal_force(process_t *target, int signo, const struct k_siginfo *info);

/** Make `mask`, less SIGKILL and SIGSTOP, the current thread's blocked mask,
 * and wake another thread to take a signal sent to the process that the new
 * mask blocks and the old did not. Every change of a thread's own mask is
 * made through it. Defined in syscall_proc.c. */
void signal_set_blocked(uint64_t mask);

/** The siginfo of the SIGCHLD a child's end sends: CLD_EXITED with the exit
 * code, or CLD_KILLED or CLD_DUMPED with the signal, from its wait status,
 * and the child's pid and real uid as its parent's namespaces see them. */
void proc_child_info(const process_t *child, struct k_siginfo *info);

/**
 * The tick `ticks` after `now` at which a real-time timer is due, never 0,
 * which `alarm_tick` keeps for a disarmed timer.
 */
uint32_t proc_alarm_at(uint32_t now, uint32_t ticks);

/**
 * Non-zero if the current thread, or its process, has a pending signal that
 * is neither blocked nor ignored: one that is caught, or whose default
 * action terminates or stops the process; or a stop its tracer asked for
 * with PTRACE_INTERRUPT. A deferred hardware debug hit counts as a pending
 * SIGTRAP. Blocking system call loops call it so that the signal or stop is
 * acted on. A deferred kernel debug hit is posted first, which may make
 * the thread ready and clear its `wakeup_tick`, so a caller that waits on
 * arms its deadline again. Called on the thread's stack with the big lock
 * held. Defined in syscall_proc.c.
 */
int signal_pending(void);

/**
 * Act on the current thread's pending signals on a return to user mode
 * through the frame `tf`, and on no other return: ignore them, end or stop
 * the process, or give one handler its frame; restart a restartable system
 * call a signal interrupted when no handler runs or the handler has
 * SA_RESTART; and free the records of ended threads (`proc_dead_threads`).
 * `isr_leave` calls it after every router, with interrupts off. Defined in
 * syscall_proc.c.
 */
struct isr_params;
void deliver_signals(struct isr_params *tf);

/**
 * Save the running thread's kernel registers in `old` and resume
 * `new_proc` where its last switch left it, or at the return its first
 * frame has (`setup_process_stack`, `arch_fork_frame`). It also gives the
 * processor `new_proc`'s kernel stack for the next entry from user mode
 * and its thread pointer, and loads its page directory when the processor
 * has another loaded. `schedule` calls it with interrupts off and the big
 * lock held. Defined in switch.s on i686 and in context.c, with switch.s,
 * on x86-64.
 */
extern void switch_context(process_t *old, process_t *new_proc);

/** Lay out a new thread's kernel stack for its first switch, which returns
 * into `entry_point` holding the big lock at depth 1. */
void setup_process_stack(process_t *proc, void (*entry_point)(void));

/**
 * The records `alloc_process` has given out since boot, each thread a
 * clone made and the kernel's own at boot counted: what Linux's
 * `/proc/stat` calls `processes`, a running total and not the number alive
 * now. An exec makes no record and adds nothing.
 */
uint32_t process_forks_since_boot(void);

/**
 * The nanosleeps a signal has ended before their deadline since boot, which
 * `/proc/stat` reports as `sleeps_interrupted`. A sleep the signal cut
 * short counts, and one that ran its whole length does not, so a test can
 * tell from the count, on a machine of any speed, whether a signal ended a
 * sleep where it arrived.
 */
uint32_t process_sleeps_cut_short(void);

/** Count one sleep a signal ended before its deadline; nanosleep calls it
 * (syscall.c). */
void process_count_sleep_cut_short(void);

/** Count one process handed out; `alloc_process` calls it. */
void process_count_fork(void);

/**
 * The pid of the running thread when `addr` lies in the unmapped guard
 * page below its kernel stack, which an access there says has run out,
 * or -1. Defined in syscall_proc.c.
 */
int kstack_overrun_pid(uintptr_t addr);

/** Test switch `rach2.stackoverrun`: run a thread down its kernel stack
 * until the guard below it faults. Called once, before the first program;
 * returns at once without the switch. Defined in syscall_proc.c. */
void kstack_overrun_test(void);

#endif
