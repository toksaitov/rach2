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
#define RLIM_MEMLOCK  6
#define RLIM_COUNT    7
#define RLIM_INFINITY (~0UL)

/* RLIMIT_MEMLOCK a process starts with, soft and hard, 8 MiB, which is
 * Linux's MLOCK_LIMIT: the bytes a process without privilege may lock with
 * the mlock calls, and apart from them pin for devices. */
#define RLIM_MEMLOCK_DEFAULT 0x800000u

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
    uint64_t wakeup_tick; /* The tick a timed sleep ends at, 0 for none */
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
     * stack, a core file, a file written and memory locked; soft and hard,
     * inherited by fork and exec. */
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
    /* The switches away from it while it was blocked or stopped, and while
     * it could still run, counted by `schedule`: /proc's
     * voluntary_ctxt_switches and nonvoluntary_ctxt_switches, and getrusage. */
    unsigned long nvcsw;
    unsigned long nivcsw;
    /* Whether its sleep is one no signal ends (`wq_wait_event_nointr`),
     * which /proc reports as the state D and counts as blocked; a kernel
     * thread's idle wait (`wq_wait_event_idle`) is not marked. */
    uint32_t uninterruptible;
    /* The frame allocator refused this thread a user page since its last
     * page fault began: the fault is one of memory, not of the address. */
    uint32_t frame_refused;
    /* A device object had no page for this thread's access since its last
     * page fault began (`device_fault`): the fault raises SIGBUS. */
    uint32_t bus_fault;
    /* A device object could not give the page yet since its last page fault
     * began (`device_fault`): a fault from user mode returns, so that the
     * thread's signals are handled, and the access runs again. */
    uint32_t fault_retry;
    /* It waits, with its registers and floating-point state saved, for the
     * core another thread of its process writes (`core_park`). */
    uint32_t core_parked;
    /* The real-time interval timer, `setitimer(ITIMER_REAL)`: the tick its
     * SIGALRM is due at, 0 when it is disarmed, and the ticks between one
     * SIGALRM and the next, 0 for a single one. A fork starts with both
     * zero, since the child inherits no timer; an exec keeps them. */
    uint64_t alarm_tick;
    uint64_t alarm_interval;
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
    uint8_t restart_always; /* Linux ERESTARTNOINTR, even without SA_RESTART */
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
    /* A kernel buffer the user-copy checks take as user memory while a
     * descriptor-to-descriptor copy runs it through the read and write
     * paths (`user_window_open`); `uwin_len` 0 when none is open. */
    uintptr_t uwin_base;
    uint32_t uwin_len;
    /* CPU time in ticks. The timer adds the tick it interrupts to `utime`
     * when this thread was in user mode and to `stime` when it was in the
     * kernel, and a tick of an idle wait to neither. A fork starts both at
     * zero; an exec keeps them. The reaped children's time is the thread
     * group's (`tgroup.h`). They are 64 bits wide, so they do not wrap; on
     * the i686 a reader on another processor than the one counting could
     * see the two halves of one count apart, once in 2^32 of its ticks. */
    uint64_t utime;
    uint64_t stime;
    /* The page faults this thread took, getrusage's ru_minflt and ru_majflt:
     * each page a fault, or the kernel's own touch of user memory, backed or
     * copied for it, and of those the pages of a file mapping the file cache
     * did not hold, which were read through the file system. A fork starts
     * both at zero; an exec keeps them. */
    unsigned long min_flt;
    unsigned long maj_flt;
    /* The tick this thread was made at, which its /proc stat file gives as
     * its start time; an exec keeps it. */
    uint64_t start_ticks;
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
    /* The capabilities, in capget's two words, numbered 0 to 40 as Linux
     * numbers them, each stored as the bits lost so that a record of zeros
     * has them all. `cap_lost` are those the thread does not hold even when
     * its ids give it privilege, which capset takes away and an exec gives
     * back up to the bounding set; `bset_lost` are those dropped from the
     * bounding set with PR_CAPBSET_DROP, which no exec gives back. A fork
     * passes both on (`fork_ids`). `nocaps` is set while CAP_SYS_ADMIN is
     * in `cap_lost`: the kernel's checks of privilege are whole, and a
     * thread without that capability is not privileged, whatever its ids
     * (`proc_privileged`). `cap_store` keeps the three in step.
     * `cap_inh` and `cap_amb` are the inheritable and ambient sets, stored
     * as the bits held, empty in a record of zeros. An exec keeps the
     * inheritable set and adds it to the bounding set an image of root
     * regains. The ambient set stays within the inheritable set and the
     * capabilities held: capset narrows it with them, and an exec keeps it
     * for an image without set-id bits. A fork passes both on, and making or
     * joining a user namespace empties both. `amb_granted` is set while the
     * thread holds capabilities its ids do not give it, those of the
     * ambient set an exec of an image without privilege gave it, as Linux
     * gives them, with `cap_lost` the rest (`exec_ambient`); the next exec,
     * ids that leave root and making or joining a user namespace clear it,
     * and a fork passes it on. */
    uint32_t cap_lost[2];
    uint32_t bset_lost[2];
    uint32_t nocaps;
    uint32_t cap_inh[2];
    uint32_t cap_amb[2];
    uint32_t amb_granted;
    /* Set while a parent that made this process with CLONE_VFORK waits for
     * it to call execve or end; cleared, and the parent woken, at the one or
     * the other (`proc_vfork_release`). */
    uint32_t vfork_waiting;
    /* The thread group (tgroup.h) and its id, the leader's `pid`, which is
     * the process id getpid reports; `pid` is this thread's id. */
    struct tgroup *tg;
    uint32_t tgid;
    /* The robust futex list set_robust_list names, walked at the thread's
     * end, and its length; 0 for none. `compat_robust_list` is the list an
     * i386 call names on x86-64, of 32-bit entries, which Linux keeps beside
     * the native one, so that a thread running both kinds of code has
     * both; 0 for none, and on i686 always. */
    uintptr_t robust_list;
    uint32_t robust_len;
    uintptr_t compat_robust_list;
    /* rseq(2) (rseq.c): the address of the area the thread registered, 0
     * for none, its length and its signature; the events that abort a
     * critical section, RSEQ_EVENT_ bits, set since a return to user mode
     * last took them; and whether the next return to user mode must look
     * at the area. A fork passes the registration on, a thread and an exec
     * start without one. */
    uintptr_t rseq;
    uint32_t rseq_len;
    uint32_t rseq_sig;
    uint32_t rseq_event;
    uint32_t rseq_notify;
    /* Base policy and real-time priority, 1 to 99 for FIFO/RR, otherwise 0.
     * Inherited priority is separate so queries and fork see the base. */
    uint8_t sched_policy;
    uint8_t sched_reset;
    uint8_t rt_priority;
    uint8_t pi_priority;
    uint8_t ready_rt;            /* the queued entry counts in its queue's rt_count */
    uint8_t sched_yielded;       /* allow an equal FIFO peer at the next selection */
    uint8_t futex_exit_state;    /* 0 live, 1 releasing futexes, 2 released */
    struct futex_pi *pi_blocked; /* PI lock this thread waits for, or NULL */
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
    /* The descriptors of its shared table the current call has looked up
     * or reserved, one bit each, which a close by another sharer leaves in
     * place until the call ends (`fd_holds_release`): `fd_held_words`
     * words from the heap, NULL until the thread first shares a table, and
     * as wide as the table, which grows every sharer's map before it grows
     * (fs/fd.c). `fd_holding` says whether any bit is set. */
    uint32_t *fd_held;
    int fd_held_words;
    int fd_holding;
    /* While the thread sleeps in `poll_wait` (io/poll.c), the queues it has
     * records on and the records, `poll_n` of each, which a close by
     * another thread may take off their queues (`close_fd`); NULL and 0 at
     * any other time. */
    struct wait_queue **poll_wqs;
    struct wq_waiter *poll_recs;
    uint32_t poll_n;
    /* Set while the thread waits in epoll_wait on the set at descriptor
     * `epoll_wait_fd`. */
    int epoll_waiting;
    int epoll_wait_fd;
    /* The system call filters seccomp(2) installed, newest first, which the
     * thread shares with the threads and children that inherit them, and
     * the mode, SECCOMP_MODE_ (seccomp.h); a fork passes both on and an
     * exec keeps them. */
    struct seccomp_filter *seccomp;
    uint32_t seccomp_mode;
    /* The namespaces (ns.h): its user namespace and its PID namespace, NULL
     * for the first, its numbers in each PID namespace it is in below the
     * first, by level, and the set of its other namespaces, NULL for the
     * first set; and whether it holds capabilities in its user namespace
     * whatever its ids there, as its creator and a process that joined it
     * do until an exec or a change of its effective uid. A fork passes them
     * on (`ns_fork`). */
    struct userns *userns;
    struct pidns *pidns;
    uint32_t nspid[NS_LEVELS];
    struct nsset *nsset;
    uint32_t ns_granted;

    /* The processor whose run queue the process is on, which is the one it
     * runs on while it runs (it is that processor's `current`) and the one
     * it last ran on after; the processors it may run on, a bit for each,
     * as sched_setaffinity(2) sets them and a fork passes them on; and its
     * depth in the big lock when it was switched away, which the switch
     * gives the processor back (sched.c). */
    int cpu;
    uint64_t affinity;
    int bkl_depth;
    /* Ready-list links, owned by sched.c; current and idle tasks are absent. */
    struct process *ready_prev, *ready_next;
    int ready_linked;
    /* Index links, owned by process.c under the big kernel lock. */
    struct process *pid_next;
    struct proc_user_count *user_count;
    /* Active timed waits and real alarms only; timer_linked marks
     * membership. */
    struct process *timer_prev, *timer_next;
    int timer_linked;
    /* The waits for the disk this thread has finished in the block layer,
     * modulo 2^32, which a sleeping lock compares across a hold to tell
     * whether the hold crossed one (fs/sleeplock.h); and the file locks it
     * holds, a node's and a mount's allocator lock, under which the buffer
     * cache and the name cache must not enter a file system (fs/vfs.h's
     * `vfs_ns_held_by_caller`); and the bytes it has written to ext2 files
     * since it last had to write the cache back (`em_balance`). */
    uint32_t disk_waits;
    int file_locks;
    uint32_t fs_written;
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
 * every processor in its affinity, and this processor as its last CPU.
 * The caller builds its context, then calls sched_make_ready to enqueue it
 * or makes it current directly. NULL when the heap has no room for it or
 * one of its parts. Each record counts once
 * in `process_forks_since_boot`. */
process_t *alloc_process(void);

/** Allocate a child being built, with no descriptor table or stdio.
 * parent supplies the real uid, its group when thread is nonzero, and its
 * directories when share_fs is nonzero. A shared group gains a reference
 * but no live thread until fork_signals commits the child. The caller
 * installs shared or copied files and completes its context before making
 * it ready; free_process unwinds an unfinished record. NULL on allocation
 * failure. The caller holds the big kernel lock. */
process_t *alloc_process_fork(const process_t *parent, int thread, int share_fs);

/* The bytes of a kernel stack: a process's, an idle task's, and the boot
 * stack loader.s reserves, which is as large. */
#define KERNEL_STACK_SIZE 0x4000

/** A zeroed kernel stack of KERNEL_STACK_SIZE bytes with an unmapped guard
 * page below it, returning its base, whose stack top is `base +
 * KERNEL_STACK_SIZE`; 0 when the heap has none. Defined in syscall_proc.c. */
uintptr_t kstack_alloc(void);

/** Give back the stack of `kstack_alloc` whose top is `top`, with its guard
 * page. A `top` of 0 and the boot stack's are ignored. Defined in
 * syscall_proc.c. */
void kstack_free(uintptr_t top);

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

/** Exchange two indexed thread ids at exec, under the big kernel lock. */
void proc_exchange_pids(process_t *a, process_t *b);

/** Change the real uid and its record count, under the big kernel lock.
 * Return 0, or -1 without a change if a new user's count cannot be allocated.
 * Every allocated record counts until free_process, including a fork in build.
 */
int proc_set_ruid(process_t *p, uint32_t uid);

/** Records charged to p's real uid, under the big kernel lock. */
uint32_t proc_user_records(const process_t *p);

/** Refresh membership after changing a deadline, alarm or state.
 * Hold the big kernel lock; this masks local interrupts while changing links.
 */
void proc_timer_update(process_t *p);

/** Work since boot: pid bucket entries, uid bucket entries, timer entries
 * and timer passes. The index is 0, 1, 2 or 3; counters wrap modulo 2^32.
 * Hold the big kernel lock.
 */
uint32_t proc_work_count(unsigned int index);

/** The user-mode registers of the thread `t`, which is not running in user
 * mode: the frame its entry to the kernel saved at the top of its kernel
 * stack. */
isr_params_t *proc_user_frame(const process_t *t);

/* Whether thread `p` has privilege, which Linux divides into capabilities
 * and this kernel keeps whole: an effective user ID of 0, with
 * CAP_SYS_ADMIN not given up (`nocaps`), in the first user namespace; a
 * process of another has privilege only over what its namespace owns
 * (`ns_privileged`). A macro, so that the host tests that compile the
 * callers without this file's functions need no stub of it. */
#define proc_privileged(p) ((p)->euid == 0 && !(p)->nocaps && !(p)->userns)

/* Whether [addr, addr + len) lies in the kernel buffer the thread `p` has
 * opened to the user-copy checks (`user_window_open`). A macro, so that
 * paging.c and usercopy.c, which host tests compile apart, share it. */
#define proc_window_covers(p, addr, len)                                 \
    ((p) && (p)->uwin_len != 0 && (uintptr_t)(addr) >= (p)->uwin_base && \
     (uintptr_t)(len) <= (p)->uwin_len &&                                \
     (uintptr_t)(addr) - (p)->uwin_base <= (p)->uwin_len - (uintptr_t)(len))

/** The CPU time in ticks of the whole process `p` is a thread of: every
 * thread's, those that have ended included, user and system. */
void proc_cpu_time(const process_t *p, uint64_t *user, uint64_t *sys);

/** The minor and major page faults of the whole process `p` is a thread of,
 * every thread's, those that have ended included. */
void proc_faults(const process_t *p, unsigned long *minor, unsigned long *major);

/** The voluntary and involuntary switches of the whole process `p` is a
 * thread of, including threads whose records have gone. Under the big lock;
 * both counters wrap as native unsigned longs. */
void proc_switches(const process_t *p, unsigned long *voluntary, unsigned long *involuntary);

/** Add the CPU time, page faults and context switches of `t`, a thread that
 * has ended and whose record goes, to its group's totals. */
void proc_thread_fold(const process_t *t);

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
 * Give every child of the process `dead_pid` to the nearest ancestor of that
 * process that is a child subreaper (prctl's PR_SET_CHILD_SUBREAPER) and has
 * not ended, short of an init; with none, to the init of the ending
 * process's PID namespace (`ns_reaper`). Post each child that has not ended
 * the parent-death signal it asked prctl for.
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
void proc_wake_sleepers(uint64_t now);

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

/** The siginfo of the SIGCHLD a child's end sends to `receiver`, its parent
 * or its tracer: CLD_EXITED with the exit code, or CLD_KILLED or CLD_DUMPED
 * with the signal, from its wait status, and the child's pid and real uid as
 * the receiver's namespaces see them. */
void proc_child_info(const process_t *child, const process_t *receiver, struct k_siginfo *info);
/** Advance the group's successful-exec generation, preserving old children. */
void proc_exec_generation(process_t *parent);
/** Whether the child's end sends SIGCHLD; account for a new parent image. */
int proc_child_signal(process_t *child, const process_t *parent);

/**
 * The tick `ticks` after `now` at which a real-time timer is due, never 0,
 * which `alarm_tick` keeps for a disarmed timer.
 */
uint64_t proc_alarm_at(uint64_t now, uint64_t ticks);

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
