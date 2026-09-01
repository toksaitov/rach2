#ifndef _PROCESS_H_
#define _PROCESS_H_

#include "../lib/kdef.h"
#include "../lib/kint.h"
#include "../lib/ksignal.h"
#include "../fs/fd.h"
#include "../mm/vm.h"

/* Maximum number of processes. */
#define MAX_PROCS 16

/* Process states. */
typedef enum {
    PROC_UNUSED,    /* Slot is free */
    PROC_READY,     /* Ready to run */
    PROC_RUNNING,   /* Currently executing */
    PROC_BLOCKED,   /* Waiting for I/O or event */
    PROC_ZOMBIE,    /* Exited, waiting for parent to collect */
    PROC_STOPPED    /* Job-control stopped (SIGSTOP/SIGTSTP), resumes on SIGCONT */
} proc_state_t;

/*
 * Saved CPU registers for context switching.
 * Layout matches what pushal/popal expect.
 */
struct cpu_regs {
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp_dummy;  /* Ignored by popal */
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    uint32_t eip;
    uint32_t eflags;
    uint32_t esp;        /* User stack pointer */
};

typedef struct cpu_regs cpu_regs_t;

/* Maximum length of current working directory path. */
#define CWD_MAX 64

/* Supplementary group slots per process (the 4.2BSD number). */
#define NGROUPS_MAX 8

/* Process Control Block (PCB). */
struct vfs_mount;

struct process {
    uint32_t pid;           /* Process ID */
    proc_state_t state;     /* Current state */
    cpu_regs_t regs;        /* Saved CPU registers */
    uint32_t cr3;           /* Page directory physical address */
    uint32_t kernel_stack;  /* Top of kernel stack for this process */
    uint32_t parent_pid;    /* Parent process ID */
    int32_t exit_code;      /* Exit status (for zombie) */
    fd_table_t fds;         /* File descriptor table */
    char cwd[CWD_MAX];      /* Current working directory (absolute, canonical; "/" at start) */
    volatile uint32_t in_syscall; /* Non-zero while a user syscall is executing */
    /* APPEND-ONLY below this line: switch.s hard-codes field offsets, so new
     * PCB fields go at the end and the .set offsets in switch.s are re-checked
     * in the same patch (PROC_TLS_BASE, asserted against offsetof at boot). */
    uint32_t tls_base;      /* Per-process %gs TLS base (set_thread_area) */
    vm_space_t vm;          /* Address-space map (VMAs + brk + stack limit) */
    uint32_t wakeup_tick;   /* nanosleep: kernel tick to wake a timed sleeper (0 = none) */
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
    uint32_t ngroups;               /* live entries in groups[] */
    uint32_t groups[NGROUPS_MAX];   /* supplementary gids */
    /* Set once this process has been reported for running out of mappings, so
     * the diagnostic is said once and not once per failed call. It belongs to
     * the process and not to a remembered pid, which a later process reusing
     * that pid would silence. */
    uint32_t vma_reported;
    /* The working directory's hold on a mount that can be unmounted: the
     * mount and the held node, or NULL on a permanent mount (vfs_cwd_set). */
    struct vfs_mount *cwd_mnt;
    void *cwd_node;
};

typedef struct process process_t;

/* Process table. */
extern process_t proc_table[MAX_PROCS];

/* Currently running process. */
extern process_t *current_proc;

/** Reset the process table to its boot state and verify the PCB layout. */
void init_proc(void);

/** Claim the first unused process slot; returns it, or NULL if the table is
 * full. */
/** Restart pid numbering at 1, for the boot that is about to start init. */
void proc_reclaim_boot_pids(void);

process_t *alloc_process(void);

/** Mark a process slot unused. */
void free_process(process_t *proc);

/** Get process by PID; returns the slot, or NULL if not found. */
process_t *get_process(uint32_t pid);

/**
 * Reparent every child of `dead_pid` onto init (`init_pid`), waking init to
 * reap any child that is already a zombie. Called from the death paths so an
 * exiting process never orphans a child onto a slot about to be freed. No-op
 * when there is no init or when init itself is dying.
 */
void reparent_children(uint32_t dead_pid);

/** Set a process's `comm` name from the final component of `path`. */
void proc_set_comm(process_t *p, const char *path);

/** Wake timed sleepers whose deadline has reached `now`. */
void proc_wake_sleepers(uint32_t now);

/**
 * Post signal `signo` to `target`: set its pending bit and wake it if blocked.
 * Delivered on the target's next return to user mode. Defined in syscall.c.
 */
void signal_post(process_t *target, int signo);

/**
 * Non-zero if the current process has a deliverable (pending, unblocked)
 * signal. Blocking syscall loops poll it to break out so the signal is acted
 * on. Defined in syscall.c.
 */
int signal_pending(void);

/**
 * Deliver a pending signal to the current process on the way back to user mode.
 * Called from the ISR/IRQ return path with the trapframe. Defined in syscall.c.
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

void process_count_fork(void);

#endif
