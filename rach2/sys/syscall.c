#include "klog.h"
#include "syscall.h"
#include "power.h"
#include "serial.h"
#include "panic.h"
#include "kio.h"
#include "kbd.h"
#include "input.h"
#include "mouse.h"
#include "proc/process.h"
#include "proc/sched.h"
#include "proc/elf.h"
#include "proc/uimage.h"
#include "proc/tls.h"
#include "tables/gdt.h"
#include "lib/krandom.h"
#include "lib/kerrno.h"
#include "lib/ksignal.h"
#include "fs/fd.h"
#include "fs/file.h"
#include "ipc/pipe.h"
#include "io/poll.h"
#include "tty/tty.h"
#include "tty/pty.h"
#include "rtc.h"
#include "fs/krffs.h"
#include "fs/devfs.h"
#include "fs/synthfs.h"
#include "fs/vfs.h"
#include "fs/procfs.h"
#include "mm/paging.h"
#include "mm/pagecache.h"
#include "mm/heap.h"
#include "mm/usercopy.h"
#include "mm/vm.h"
#include "lib/kuser.h"
#include "lib/kstring.h"
#include "display/vga_gfx.h"
#include "speaker.h"
#include "audio/audio.h"
#include "network/socket/socket.h"
#include "network/socket/packet.h"
#include "network/socket/raw.h"
#include "network/ethernet/ethernet.h"
#include "network/netif.h"
#include "network/driver/netdev.h"
#include "network/udp/udp.h"
#include "network/tcp/tcp.h"
#include "network/ip/ip.h"
#include "network/ip/icmp.h"
#include "network/net.h"
#include "network/netconf.h" /* net_ip_addr for getsockname */
#include "cmdline.h"    /* kcmdline_has, for the ENOSYS trace flag */

/* User stack base address (stack grows down from here) */
#define USER_STACK_BASE 0xBFFFF000

/* Validate a user-space address range (defined with the socketcall helpers). */
static int user_access_ok(uint32_t addr, uint32_t len);

/* Validate AND fault in a user buffer before the kernel hands the raw pointer
 * to a lower layer that dereferences it directly (krffs, tcp, udp, drivers).
 * 'write' is 1 if that layer writes into the buffer. Defined with
 * user_access_ok. */
static int user_buffer_ok(uint32_t addr, uint32_t len, int write);

/* Send one datagram from `sock`, judging the destination against the socket's
 * permission to broadcast. Defined with the socketcall helpers. */
static int32_t dgram_send(struct socket *sock, uint32_t dst_ip, uint16_t dst_port,
                          const uint8_t *buf, uint16_t len);

/* Only one process owns the framebuffer at a time; when it exits or closes fb,
 * text mode is restored. */
static volatile uint32_t fb_owner_pid = 0;

/**
 * Restore text mode if `pid` owns the framebuffer.
 *
 * Called when a process is killed (Ctrl-C) so the dead owner does not strand
 * the screen in graphics mode.
 */
void fb_release_if_owner(uint32_t pid)
{
    if (fb_owner_pid == pid && fb_owner_pid != 0) {
        vga_mode13h_exit();
        fb_owner_pid = 0;
    }
}

/* The pool backing FD_TYPE_SYNTH_DIR descriptors. A 32-bit pid does not fit
 * packed in the fd's priv pointer alongside the kind without truncation, so the
 * (kind, pid) live here (struct synth_dir is in synthfs.h); priv points at the
 * slot, freed when the last descriptor referencing it closes (see
 * fd_release_backing). */
static struct synth_dir synth_dir_pool[32];

static struct synth_dir *synth_dir_alloc(int kind, int pid)
{
    int i;
    for (i = 0; i < (int)(sizeof(synth_dir_pool) / sizeof(synth_dir_pool[0])); i++) {
        if (!synth_dir_pool[i].used) {
            synth_dir_pool[i].kind = kind;
            synth_dir_pool[i].pid = pid;
            synth_dir_pool[i].used = 1;
            return &synth_dir_pool[i];
        }
    }
    return NULL;
}

/**
 * How many descriptors other than `except` hold `node` as their backing file.
 *
 * A scan of every process's descriptor table, and deliberately not a counter on
 * the node. A counter would have to be incremented in `fork`'s table copy, in
 * `dup`, in `dup2`, and in every future path that copies an `fd_entry_t`, and
 * one missed increment is a use-after-free that no test would show. The tables
 * are the count, so there is nothing to keep in step.
 *
 * MAX_PROCS and MAX_FDS are both 16, so this is at most 256 pointer compares
 * of kernel memory; it never touches the page-cache window.
 */
static int fd_node_holders(const krffs_node_t *node, const fd_entry_t *except)
{
    int held = 0;
    int p;
    int i;

    if (!node) {
        return 0;
    }
    for (p = 0; p < MAX_PROCS; p++) {
        if (proc_table[p].state == PROC_UNUSED) {
            continue;
        }
        for (i = 0; i < MAX_FDS; i++) {
            fd_entry_t *e = &proc_table[p].fds.entries[i];

            if (e != except && e->type == FD_TYPE_FILE && e->node == node) {
                held++;
            }
        }
    }
    return held;
}

/**
 * Whether any descriptor anywhere has `node`. For callers outside this file.
 *
 * The debug shell removes and renames through the file system directly, below
 * the syscall layer that owns the holder rule, so it asks here rather than
 * carrying its own copy of the answer.
 */
int fs_node_is_held(const krffs_node_t *node)
{
    return fd_node_holders(node, NULL) > 0;
}


/**
 * Repoint every holder from one node to another, for callers outside this
 * file. The debug shell writes through the file system directly, and a write
 * that grows a file past its extent relocates it; the sweep over
 * descriptors, descriptions, working directories and mounts belongs to the
 * VFS, which is the one place that knows every holder kind.
 */
void fs_node_moved(const krffs_node_t *from, krffs_node_t *to)
{
    if (!from || from == to) {
        return;
    }
    vfs_repoint(vfs_root_mount(), (void *)(krffs_node_t *)from, to);
}

/**
 * Release every backing reference a descriptor holds before its slot is freed.
 *
 * Drops framebuffer ownership, the per-fd socket reference, and an ops-backed
 * object (pipe/tty/pty). Every fd-teardown site routes through this (close, the
 * dup2 replacement of an open target, the execve CLOEXEC sweep, and the
 * process-exit fd-table release) so none can forget a backing kind as new ones
 * are added. It does NOT free the slot; the caller does that with fd_free.
 *
 * An ops-backed object rides the shared open-file description: dup/dup2/fork
 * copy the ops/priv pointers and bump the OFD refcount, so the object must
 * be released exactly once, when its LAST descriptor closes (the final OFD
 * reference, refcount about to reach 0). The socket reference, by contrast,
 * is counted per descriptor (socket_ref on dup/fork), so it is always
 * dropped here. No descriptor is both an ops object and a socket.
 */
static void fd_release_backing(fd_entry_t *e)
{
    if (!e) {
        return;
    }
    /* A KRFFS node is a backing kind like any other here. An orphan (a reserved
     * node no path names) is freed by its LAST holder, and this runs while the
     * closing slot is still populated, which is what `except` is for: the
     * closing descriptor is skipped, not cleared first, so "last holder"
     * needs no ordering rule between two functions. */
    if (e->type == FD_TYPE_FILE && e->node && krffs_fs &&
        e->node != krffs_fs->node && e->node->name[0] == '\0' &&
        fd_node_holders(e->node, e) == 0) {
        krffs_remove_reserved_node(krffs_fs, e->node);
        e->node = NULL;
    }
    /* A synthetic directory fd's pooled cookie is freed when its last
     * descriptor closes (dup/fork share it via the OFD refcount, like an ops
     * backing). */
    if (e->type == FD_TYPE_SYNTH_DIR && e->priv && e->ofd && e->ofd->refcount == 1) {
        ((struct synth_dir *)e->priv)->used = 0;
    }
    /* Restore text mode only if THIS process actually owns the framebuffer, so
     * a fork child that merely inherited the fb descriptor cannot clear the
     * parent's graphics mode when the child closes the fd or exits. */
    if (e->type == FD_TYPE_FRAMEBUFFER && current_proc &&
        fb_owner_pid == current_proc->pid && e->ofd && e->ofd->refcount == 1) {
        vga_mode13h_exit();
        fb_owner_pid = 0;
    }
    if (e->type == FD_TYPE_SOCKET && e->sock) {
        socket_unref(e->sock);
    }
    if (e->ops && e->ops->close && e->ofd && e->ofd->refcount == 1) {
        e->ops->close(e);
    }
}

/*
 * ioctl request codes (Linux compatible subset)
 */
#define KIOCSOUND      0x4B2F  /* Set PC speaker frequency (0 = off) */
#define TCGETS         0x5401  /* Get terminal attributes (termios) */
#define TCSETS         0x5402  /* Set terminal attributes now */
#define TCSETSW        0x5403  /* Set after the output drains */
#define TCSETSF        0x5404  /* Set after output drains, flush input */
#define TCFLSH         0x540B  /* Flush input and/or output queues */
#define FIONREAD       0x541B  /* Bytes available to read */
#define TIOCGWINSZ     0x5413  /* Get terminal window size */
#define TIOCSWINSZ     0x5414  /* Set terminal window size */
#define TIOCSCTTY      0x540E  /* Make this tty the controlling terminal */
#define TIOCGPGRP      0x540F  /* Get the foreground process group */
#define TIOCSPGRP      0x5410  /* Set the foreground process group */
#define TIOCNOTTY      0x5422  /* Detach from the controlling terminal */
#define FB_MODE_13H    0x4601  /* Switch to VGA Mode 13h */
#define FB_MODE_TEXT   0x4602  /* Switch back to text mode */
#define FB_SETPALETTE  0x4603  /* Set 256-color palette (768 bytes) */

/*
 * Audio ioctl request codes
 */
#define AUDIO_GET_INFO    0x4101  /* Get audio driver info */
#define AUDIO_START       0x4102  /* Start audio playback */
#define AUDIO_STOP        0x4103  /* Stop audio playback */
#define AUDIO_WRITE       0x4104  /* Write audio samples */
#define AUDIO_SET_VOLUME  0x4105  /* Set volume (0-100) */

/*
 * Audio info structure returned by AUDIO_GET_INFO
 */
struct audio_info {
    uint32_t driver_type;   /* AUDIO_DRIVER_NONE, SPEAKER, SB16, HDA */
    uint32_t has_real_audio; /* 1 if real audio (not PC speaker) */
};

/*
 * Audio start parameters for AUDIO_START
 */
struct audio_start_params {
    uint32_t sample_rate;   /* e.g., 11025, 22050, 44100 */
    uint32_t format;        /* 0 = U8, 1 = S16LE */
    uint32_t channels;      /* 1 = mono, 2 = stereo */
};

/*
 * Audio write parameters for AUDIO_WRITE
 */
struct audio_write_params {
    const uint8_t *buffer;
    uint32_t size;
};

/* Kernel stack size (16KB) */
#define KERNEL_STACK_SIZE 0x4000

/*
 * Per-process kernel stacks come from a fixed arena of MAX_PROCS slots, not
 * from the never-freed bump allocator, so a fork/exit/reap cycle returns the
 * stack to the pool instead of leaking four frames each time. The arena is
 * reserved once at init; a slot is claimed on fork and released when the
 * zombie is reaped.
 */
static uintptr_t kstack_arena;
static uint8_t kstack_in_use[MAX_PROCS];

/**
 * Reserve the kernel-stack arena. Called once at init, before the first fork.
 */
static void init_kstacks(void)
{
    kstack_arena = mem_reserve((size_t)MAX_PROCS * KERNEL_STACK_SIZE, true);
    if (kstack_arena == 0) {
        k_panic("kstack: cannot reserve the kernel-stack arena");
    }
}

/**
 * Claim a kernel stack, returning its base, or 0 if the arena is full.
 *
 * The caller uses `base + KERNEL_STACK_SIZE` as the stack top.
 */
static uintptr_t kstack_alloc(void)
{
    uint32_t i;

    if (kstack_arena == 0) {
        return 0;
    }
    for (i = 0; i < MAX_PROCS; i++) {
        if (!kstack_in_use[i]) {
            kstack_in_use[i] = 1;
            return kstack_arena + (uintptr_t)i * KERNEL_STACK_SIZE;
        }
    }
    return 0;
}

/**
 * Release the kernel stack whose top is `top`.
 *
 * A top outside the arena (the boot stack of the init-role process) is
 * ignored, so this is safe to call on any reaped process. Only a zombie may
 * be freed: the dying process is still executing on its own stack until the
 * parent reaps it.
 */
static void kstack_free(uintptr_t top)
{
    uintptr_t base = top - KERNEL_STACK_SIZE;

    if (kstack_arena != 0 && base >= kstack_arena &&
        base < kstack_arena + (uintptr_t)MAX_PROCS * KERNEL_STACK_SIZE) {
        kstack_in_use[(base - kstack_arena) / KERNEL_STACK_SIZE] = 0;
    }
}

/* Fork child return point in isr_handler.s */
extern void isr_return(void);

/*
 * Stdin buffer from kernel.c - used for userspace stdin reads.
 * The keyboard handler fills this buffer when a process is waiting.
 */
#define STDIN_BUFFER_SIZE 256
extern char stdin_buffer[STDIN_BUFFER_SIZE];
extern volatile size_t stdin_pos;
extern volatile bool stdin_ready;
extern volatile process_t *stdin_waiting_proc;

/*
 * Init process tracking from kernel.c. A real init's death is fatal
 * (panic_init_died); an init exiting under the debug shell returns to it.
 */
extern volatile uint32_t init_pid;

/* Shell loop from kernel.c - the debug shell, reached when an init exits while
 * the debug shell is the supervisor (rescue mode or the no-init test image). */
extern void shell_loop(void);
extern void shell_reset_input(void);

/* Boot-home policy from kernel.c. debug_shell_is_home is true when the debug
 * shell supervises (an exited init returns to it); false when a real /sbin/init
 * is PID 1, in which case its death is fatal (panic_init_died, never returns).
 */
extern bool debug_shell_is_home;
extern void panic_init_died(uint32_t pid);
extern void kernel_shutdown(int how);

/* Kernel stack from loader.s */
extern char stack[];
#define KERNEL_STACK_SIZE_LOCAL 0x4000

/*
 * Syscall function type - takes pointer to params, returns int32_t
 */
typedef int32_t (*syscall_fn_t)(isr_params_t *);

/*
 * Syscall dispatch table
 */
static syscall_fn_t _syscall_table[SYSCALL_TABLE_SIZE];

/**
 * Release every descriptor a dying process still has.
 *
 * Drops each socket reference and each shared open-file description (the
 * seek offset), and clears the slot. Called from both exit paths (sys_exit
 * here and terminate_current_process via an extern), so sockets, port
 * bindings, TCBs, and OFDs are freed however a process dies. Idempotent: a
 * second call sees an empty table (fd_free skips free slots).
 */
void release_fd_table(fd_table_t *table)
{
    int fdi;

    if (!table) {
        return;
    }
    for (fdi = 0; fdi < MAX_FDS; fdi++) {
        fd_entry_t *e = &table->entries[fdi];
        if (e->type == FD_TYPE_NONE) {
            continue;
        }
        fd_release_backing(e);
        fd_free(table, fdi);
    }
}

/**
 * Terminate the current process (exit(2)/exit_group(2)). Does not return.
 *
 * EBX has the exit status. Marks the process a zombie, reparents its children
 * to init, wakes the parent if it waits, and reschedules. A real init's exit is
 * fatal (panic_init_died); an init exiting under the debug shell returns to it.
 *
 * The plain status line goes out only where the debug shell is the supervisor.
 * There it is how a person, and the harness in its no-token mode, read a
 * transient init's outcome. On an image running `/sbin/init` and a login shell
 * every process exit would print it, a forked child's included, and the shell
 * already answers the question through `$?`. The line that hands the shell back
 * below is not gated: the probe runner scores probes from that one.
 */
static int32_t sys_exit(isr_params_t *params)
{
    int status = (int)params->ebx;
    process_t *parent;
    bool is_init = false;
    uint32_t exiting_pid;

    if (current_proc && fb_owner_pid == current_proc->pid) {
        vga_mode13h_exit();
        fb_owner_pid = 0;
    }

    if (current_proc) {
        release_fd_table(&current_proc->fds);
        vfs_cwd_clear(current_proc);
    }

    if (debug_shell_is_home) {
        klog(KLOG_INFO, "process", "exited with status %d", status);
    }

    if (!current_proc) {
        __asm__ __volatile__("cli; hlt");
        return 0;
    }

    exiting_pid = current_proc->pid;
    is_init = (exiting_pid == init_pid && init_pid != 0);

    /* If the dying process is the console-owning session leader (init, or a
     * later leader that claimed the console via TIOCSCTTY, NOT only init),
     * release the controlling terminal NOW, before it zombifies and the
     * teardown below runs with interrupts on. Otherwise the timer-IRQ serial
     * drain (tty_console_poll) keeps draining COM1 against this vanishing
     * session, dropping a ^C/^Z and stealing bytes the kernel debug shell
     * should read. shell_reset_input on the is_init path releases again
     * (idempotent). */
    if (tty_console_owned_by_leader((int)current_proc->sid, (int)current_proc->pid)) {
        tty_set_session(&console_tty, 0, 0);
    }
    /* A dying session leader also disassociates any pty slave it controlled, so
     * a later session can claim the same /dev/pts/N. */
    pty_release_leader((int)current_proc->sid, (int)current_proc->pid);

#ifdef RACH2_HARNESS
    /* Harness build: the instant the probe (run as init) exits, report its
     * status to QEMU's isa-debug-exit device and stop, BEFORE schedule() can
     * switch to a still-READY child and bypass this path, so the runner always
     * reads a real exit code. */
    if (is_init) {
        harness_exit((uint8_t)status);
    }
#endif

    /* Mark process a zombie and store the Linux wait-status encoding of a
     * normal exit (the parent's waitpid/wait4 copies exit_code straight out):
     * the low 8 bits of the code go in bits 8-15, so WIFEXITED is true and
     * WEXITSTATUS returns the code. (harness_exit above still gets the raw
     * status byte.) */
    current_proc->state = PROC_ZOMBIE;
    current_proc->exit_code = (status & 0xFF) << 8;

    /* Notify the parent: post SIGCHLD (ignored by default, but a shell installs
     * a reaper handler) and wake it if it is blocked in waitpid. */
    parent = get_process(current_proc->parent_pid);
    if (parent) {
        signal_post(parent, SIGCHLD);
        if (parent->state == PROC_BLOCKED) {
            parent->state = PROC_READY;
        }
    }

    /* Hand any surviving children to init before this slot is reaped, so an
     * exit never strands an orphan on a dead parent (the MAX_PROCS zombie
     * leak). */
    reparent_children(exiting_pid);

    /* A real /sbin/init must never exit; if it does there is no reaper, so the
     * kernel panics. Halt BEFORE schedule(), or a still-READY process would
     * take the CPU and the system would run on with a dead PID 1. Only when the
     * debug shell is the supervisor (rescue, or the no-init test image) does an
     * exited init instead return to it, below. */
    if (is_init && !debug_shell_is_home) {
        panic_init_died(exiting_pid);   /* never returns */
    }

    schedule();

    /* No other process is READY. The init that exited ran under the debug shell
     * (a real init would have panicked above), so return to it. */
    if (is_init) {
        klog(KLOG_INFO, "sys_exit",
             "init process %d exited with status %d, returning to kernel shell",
                      exiting_pid, status);
        console_puts("Init process exited. Returning to kernel debug shell.");

        /* Clean up the init process resources. Switch off its address space
         * BEFORE freeing the page directory: free_page_directory refuses to
         * free whatever CR3 currently points at, so freeing while still loaded
         * would leak the directory and its private page tables. */
        if (current_proc->cr3 != 0) {
            page_dir_t *init_dir;
            init_dir = (page_dir_t *)(current_proc->cr3 - 1024 * sizeof(page_table_t *));
            free_user_pages(init_dir);
            switch_page_directory(_kernel_dir);
            free_page_directory(init_dir);
        } else {
            switch_page_directory(_kernel_dir);
        }
        free_process(current_proc);

        init_pid = 0;

        current_proc = &proc_table[0];
        current_proc->state = PROC_RUNNING;

        sched_disable();

        /* Clear the shell's command line + stdin-wait state BEFORE re-enabling
         * interrupts below, so a keyboard IRQ during the jump cannot append to
         * a stale command buffer or see a dangling stdin waiter. shell_loop
         * resets again on entry as the catch-all. */
        shell_reset_input();

        /* Reset ESP to a fresh kernel stack and jump to shell_loop. Safe
         * because we are in kernel mode and init has been torn down. */
        __asm__ __volatile__(
            "movl %0, %%esp\n\t"
            "sti\n\t"
            "jmp *%1\n\t"
            :
            : "r"((uint32_t)stack + KERNEL_STACK_SIZE_LOCAL),
              "r"((uint32_t)shell_loop)
            : "memory"
        );
    }

    __asm__ __volatile__("cli; hlt");
    return 0;
}

/* Re-validate a descriptor's cached KRFFS node before a file op (defined below
 * near the other path helpers); 0 if live, -EBADF if the fd has gone stale. */
static int fd_node_check(fd_entry_t *entry);

/* The credential helpers the write path needs, defined with the rest of the
 * credential model below. */
static int cred_privileged(void);
static void cred_kill_setid(krffs_node_t *node);

/**
 * Write `count` bytes of user buffer `buf` to descriptor `fd`.
 *
 * The shared body of write(2) and writev(2). Dispatches by backing kind: an
 * ops-backed object (pipe/tty/pty/device) through its vtable, a socket to the
 * network, the console through the tty output processor, a KRFFS file, or the
 * framebuffer. Returns the bytes written or a negative errno.
 */
static int32_t write_fd(int fd, const char __user *buf, uint32_t count)
{
    fd_entry_t *entry;

    if (!current_proc) {
        return -ESRCH;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry) {
        return -EBADF;
    }

    /* Backing-ops object (pipe/tty/pty/device): dispatch through its vtable,
     * but only if the descriptor was opened for writing (a read-only fd ->
     * -EBADF, as Linux reports). The plain file and enum types below have no
     * ops and fall through to the enum dispatch. */
    if (entry->ops) {
        if (!(entry->flags & FD_FLAG_WRITE)) {
            return -EBADF;
        }
        return entry->ops->write ? entry->ops->write(entry, buf, count) : -EBADF;
    }

    if (entry->type == FD_TYPE_SOCKET) {
        if (!entry->sock) {
            return -EBADF;
        }
        if (entry->sock->type == SOCK_STREAM) {
            if (!user_buffer_ok((uint32_t)buf, count, 0)) {
                return -EFAULT;
            }
            /* socket_tcp_send copies the user buffer into the socket send ring
             * itself, blocking on send-ring space unless the fd is
             * non-blocking; __force strips the __user tag at this boundary. */
            return socket_tcp_send(entry->sock, (const uint8_t __force *)buf,
                                   (int)count,
                                   entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));
        }
        /* A datagram socket writes to the peer it connected to, and has
         * nowhere to send without one. */
        if (entry->sock->type == SOCK_DGRAM) {
            if (!entry->sock->connected) {
                return -EDESTADDRREQ;
            }
            if (!user_buffer_ok((uint32_t)buf, count, 0)) {
                return -EFAULT;
            }
            if (count > 1500) {
                return -EMSGSIZE;
            }
            return dgram_send(entry->sock, entry->sock->remote_ip,
                              entry->sock->remote_port,
                              (const uint8_t __force *)buf, (uint16_t)count);
        }
        return -EOPNOTSUPP;
    }

    if (entry->type == FD_TYPE_CONSOLE) {
        char kbuf[256];
        uint32_t done = 0;

        if (!(entry->flags & FD_FLAG_WRITE)) {
            return -EBADF;  /* Not writable (e.g., stdin) */
        }

        /* A background process writing its controlling terminal is stopped with
         * SIGTTOU only when TOSTOP is set (otherwise background output is
         * allowed). As with SIGTTIN, a blocked/ignored SIGTTOU makes it -EIO
         * instead. */
        if ((console_tty.ldisc.tio.c_lflag & TOSTOP) &&
            tty_is_background(&console_tty, (int)current_proc->sid,
                              (int)current_proc->pgid)) {
            if ((current_proc->sig_blocked & KSIG_BIT(SIGTTOU)) ||
                current_proc->sigacts[SIGTTOU].handler == KSIG_IGN) {
                return -EIO;
            }
            tty_signal_group((int)current_proc->pgid, SIGTTOU);
            return -EINTR;
        }

        /* Pull the user bytes in through a small kernel bounce buffer, then
         * emit from kernel memory through the tty output processor
         * (OPOST/ONLCR, so a bare '\n' becomes CR LF on both the VGA screen and
         * the serial line), looping until count bytes are done. A bad user
         * pointer yields -EFAULT only if nothing has been written yet
         * (otherwise report the bytes already emitted, matching the
         * return-count convention). */
        while (done < count) {
            uint32_t chunk = count - done;

            if (chunk > sizeof(kbuf)) {
                chunk = sizeof(kbuf);
            }
            if (copy_from_user(kbuf, (const void __user *)(buf + done), chunk) != 0) {
                return (done > 0) ? (int32_t)done : -EFAULT;
            }
            tty_write(&console_tty, (const unsigned char *)kbuf, (int)chunk);
            done += chunk;
        }
        return (int32_t)count;
    }

    if (entry->type == FD_TYPE_FILE) {
        struct open_file *ofd = entry->ofd;
        int32_t written;

        if (!(entry->flags & FD_FLAG_WRITE)) {
            return -EBADF;  /* Not opened for writing */
        }
        if (!ofd || !ofd->vmnt) {
            return -EBADF;
        }
        /* The file system reads the user buffer directly, so range-check AND
         * fault its pages in first (as the socket path does) before stripping
         * __user. */
        if (!user_buffer_ok((uint32_t)(__force uintptr_t)buf, count, 0)) {
            return -EFAULT;
        }
        fd_ofd_lock(ofd);
        /* O_APPEND: every write lands at the current end of file, regardless of
         * any intervening seek (the POSIX append contract). The flag lives on
         * the shared open-file description. */
        if (ofd->status_flags & OFD_APPEND) {
            struct vfs_stat vst;

            if (vfs_getattr(ofd->vmnt, ofd->vnode, &vst) == 0) {
                ofd->offset = (uint32_t)vst.size;
            }
        }
        written = vfs_write(ofd->vmnt, ofd->fhandle, (const char __force *)buf,
                            count, ofd->offset);
        if (written > 0) {
            /* Through the UPDATED node pointer: a grow relocates the file and
             * the write repointed every holder, this descriptor included.
             * Only a write that moved bytes disarms the set-id bits, and a
             * privileged writer is exempt: that is the CAP_FSETID analogue,
             * and without it any root process silently disarms /bin/su by
             * rewriting it. */
            if (!cred_privileged()) {
                cred_kill_setid(entry->node);
            }
            ofd->offset += (uint32_t)written;
        }
        fd_ofd_unlock(ofd);
        return written;
    }

    if (entry->type == FD_TYPE_FRAMEBUFFER) {
        uint32_t to_copy;

        if (!vga_graphics_mode) {
            return -EBADF;  /* Not in graphics mode */
        }

        to_copy = count;
        if (to_copy > VGA_MODE13H_SIZE) {
            to_copy = VGA_MODE13H_SIZE;
        }

        if (copy_from_user((void *)VGA_FRAMEBUFFER, (const void __user *)buf, to_copy) != 0) {
            return -EFAULT;
        }

        return (int32_t)to_copy;
    }

    return -EBADF;
}

/**
 * Truncate an open file to `length` (ftruncate(2)).
 *
 * EBX = fd, ECX = length. The descriptor must be a writable file on a
 * mounted file system. A successful shrink or extend clears the set-id bits
 * for an unprivileged caller, like a write.
 */
static int32_t sys_ftruncate(isr_params_t *params)
{
    int fd = (int)params->ebx;
    uint32_t length = params->ecx;
    fd_entry_t *entry;
    int rc;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_FILE) {
        return -EBADF;
    }
    if (!(entry->flags & FD_FLAG_WRITE)) {
        return -EINVAL; /* ftruncate wants a writable descriptor */
    }
    if (!entry->ofd || !entry->ofd->vmnt) {
        return -EBADF;
    }
    fd_ofd_lock(entry->ofd);
    rc = vfs_truncate(entry->ofd->vmnt, entry->ofd->fhandle, length);
    if (rc == 0 && !cred_privileged()) {
        cred_kill_setid(entry->node);
    }
    fd_ofd_unlock(entry->ofd);
    return rc;
}

/* write(2): EBX = fd, ECX = buf, EDX = count. */
static int32_t sys_write(isr_params_t *params)
{
    return write_fd((int)params->ebx, (const char __user *)params->ecx, params->edx);
}

/*
 * Gather write (writev(2)). musl's buffered stdio (printf) emits its output
 * through this call. EBX = fd, ECX = struct iovec[], EDX = iovcnt. Returns the
 * total bytes written across the iovecs, or a negative errno.
 */
struct kernel_iovec {
    uint32_t iov_base;
    uint32_t iov_len;
};

#define IOV_MAX_LOCAL 1024

static int32_t sys_writev(isr_params_t *params)
{
    int fd = (int)params->ebx;
    uint32_t iov_user = params->ecx;
    int iovcnt = (int)params->edx;
    struct kernel_iovec iov;
    int32_t total = 0;
    int i;

    if (iovcnt < 0 || iovcnt > IOV_MAX_LOCAL) {
        return -EINVAL;
    }

    for (i = 0; i < iovcnt; i++) {
        int32_t r;

        /* On any fault once bytes were already emitted, report the partial
         * total (a caller that retried would otherwise duplicate output); only
         * report the error itself when nothing has been written yet. */
        if (copy_from_user(&iov,
                           (const void __user *)(iov_user +
                               (uint32_t)i * sizeof(struct kernel_iovec)),
                           sizeof(iov)) != 0) {
            return (total > 0) ? total : -EFAULT;
        }
        if (iov.iov_len == 0) {
            continue;
        }
        /* An individual length past the signed range, or a running total that
         * would overflow it, is rejected as Linux does (-EINVAL). */
        if (iov.iov_len > 0x7FFFFFFFu ||
            (uint32_t)total > 0x7FFFFFFFu - iov.iov_len) {
            return (total > 0) ? total : -EINVAL;
        }
        if (!user_access_ok(iov.iov_base, iov.iov_len)) {
            return (total > 0) ? total : -EFAULT;
        }

        r = write_fd(fd, (const char __user *)iov.iov_base, iov.iov_len);
        if (r < 0) {
            return (total > 0) ? total : r;
        }
        total += r;
        if ((uint32_t)r < iov.iov_len) {
            break;  /* short write; stop, as writev does */
        }
    }

    return total;
}

/* Linux open flags */
#define O_APPEND   02000    /* every write lands at end-of-file */
#define O_CREAT    0100
#define O_EXCL     0200
#define O_TRUNC    01000
#define O_NONBLOCK 04000    /* a would-block operation returns -EAGAIN */
#define O_DIRECTORY 0200000 /* the target must be a directory (else -ENOTDIR) */
#define O_CLOEXEC  02000000 /* close-on-exec: the fd is dropped across execve */

/* Special dir fd for the openat family: resolve relative to the current dir. */
#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_SYMLINK_FOLLOW 0x400 /* linkat: follow a symlink source */
#define AT_EMPTY_PATH 0x1000    /* *at: operate on the dirfd itself */

/**
 * Join base + '/' + rel into pathbuf as an absolute path.
 *
 * `base` is "/" or "/a/b" with no trailing slash; `rellen` is k_strlen(rel).
 * Returns the length, or -ENAMETOOLONG if it would not fit.
 */
static int join_abs(char *pathbuf, uint32_t size, const char *base,
                    const char *rel, uint32_t rellen)
{
    uint32_t blen = (uint32_t)k_strlen(base);

    if (blen <= 1) {
        /* base is "/" (or empty): the result is just "/" + rel. */
        if (1 + rellen + 1 > size) {
            return -ENAMETOOLONG;
        }
        pathbuf[0] = '/';
        k_memcpy(pathbuf + 1, rel, rellen + 1);
        return (int)(1 + rellen);
    }
    if (blen + 1 + rellen + 1 > size) {
        return -ENAMETOOLONG;
    }
    k_memcpy(pathbuf, base, blen);
    pathbuf[blen] = '/';
    k_memcpy(pathbuf + blen + 1, rel, rellen + 1);
    return (int)(blen + 1 + rellen);
}

/**
 * Collapse `.` and `..` in an absolute path that a physical walk has just
 * resolved, for the working directory's string: after the walk succeeded,
 * every `..` names the parent the walk climbed to. `path` must start with
 * '/'; the output never grows, so the rewrite is in place.
 */
static void collapse_dots(char *path)
{
    int out = 1;
    const char *p = path + 1;

    while (*p) {
        const char *comp;
        int clen, i;

        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        comp = p;
        while (*p && *p != '/') {
            p++;
        }
        clen = (int)(p - comp);
        if (clen == 1 && comp[0] == '.') {
            continue;
        }
        if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
            while (out > 1 && path[out - 1] != '/') {
                out--;
            }
            if (out > 1) {
                out--;
            }
            continue;
        }
        if (out > 1) {
            path[out++] = '/';
        }
        for (i = 0; i < clen; i++) {
            path[out++] = comp[i];
        }
    }
    path[out] = '\0';
}

static int canonicalize_abs(char *path)
{
    int out = 1; /* path[0] is '/'; out is the current length */
    const char *p = path + 1;

    while (*p) {
        const char *comp;
        int clen, i;

        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        comp = p;
        while (*p && *p != '/') {
            p++;
        }
        clen = (int)(p - comp);

        if (clen == 1 && comp[0] == '.') {
            continue;
        }
        /* `..` stays in the path: it is resolved physically by the walk,
         * where a missing or unsearchable directory before it fails, so a
         * spelling can no longer reach past what the caller may see. */
        if (out > 1) {
            path[out++] = '/';
        }
        for (i = 0; i < clen; i++) {
            path[out++] = comp[i];
        }
    }
    path[out] = '\0';
    return out;
}

/**
 * Copy a user path into pathbuf as an ABSOLUTE path.
 *
 * An absolute user path is taken verbatim; a relative one is joined onto the
 * process's current working directory so the root-relative resolver (which
 * already honors '.'/'..') walks the right tree. Same return contract as
 * strncpy_from_user: the resulting length on success, or a negative errno (a
 * join that overruns the buffer is -ENAMETOOLONG). The "fb"/"mouse" device
 * names resolve correctly only from cwd "/", which is the case because programs
 * open them at startup.
 */
static int copy_path_cwd(char *pathbuf, uint32_t size, const char __user *upath)
{
    char tmp[256];
    int len;

    len = strncpy_from_user(tmp, upath, sizeof(tmp));
    if (len < 0) {
        return len;
    }
    if (len == 0) {
        return -ENOENT; /* the empty path is never a valid file (Linux ENOENT) */
    }
    if (tmp[0] == '/') {
        if ((uint32_t)len + 1 > size) {
            return -ENAMETOOLONG;
        }
        k_memcpy(pathbuf, tmp, (uint32_t)len + 1);
    } else {
        int r = join_abs(pathbuf, size, current_proc->cwd, tmp, (uint32_t)len);
        if (r < 0) {
            return r;
        }
    }
    return canonicalize_abs(pathbuf);
}

/**
 * Re-validate a descriptor's cached KRFFS node before a file operation.
 *
 * A held node is never freed now, so this stops being an identity test and
 * becomes what it always was in fact, a resolution. Two shapes are live: an
 * orphan, which no path names and which nothing will free while this descriptor
 * holds it; and a named node, which must still resolve back to itself. Anything
 * else is a descriptor whose file went away by a route that did not consult the
 * holders, and stays -EBADF.
 *
 * The old round-trip could not tell a freed node from a different file the
 * allocator later placed at the same address. That hole is closed for every
 * regular-file descriptor by the lifetime, not by this check: no address
 * a descriptor has is handed out while it has it.
 *
 * A NULL node is the root directory and needs no check. Directories are
 * deliberately out of scope: `rmdir` still frees the node under an open
 * directory descriptor, so a relative path through one still answers -EBADF.
 */
static int fd_node_check(fd_entry_t *entry)
{
    char path[256];
    krffs_node_t *node = entry->node;

    if (node == NULL) {
        return 0;
    }
    if (node->type != KRFFS_RESERVED_NODE) {
        return -EBADF;
    }
    if (node->name[0] == '\0') {
        return 0; /* an orphan: unreachable by name, live for its holders */
    }
    if (krffs_node_path(node, path, sizeof(path)) != 0) {
        return -EBADF;
    }
    if (krffs_resolve(path) != node) {
        return -EBADF;
    }
    return 0;
}

/**
 * Copy a user path for the *at family into pathbuf as an ABSOLUTE path.
 *
 * Absolute paths are taken verbatim; a relative path resolves against
 * AT_FDCWD (the cwd) or, for a real dirfd, against that descriptor, which
 * must be an open directory (else -EBADF/-ENOTDIR). Same return contract as
 * copy_path_cwd.
 */
static int mnt_is_root(const struct vfs_mount *mnt);
static int copy_path_at(char *pathbuf, uint32_t size, int dirfd,
                        const char __user *upath)
{
    char tmp[256];
    char dirpath[256];
    fd_entry_t *entry;
    krffs_node_t *dnode;
    int len;

    len = strncpy_from_user(tmp, upath, sizeof(tmp));
    if (len < 0) {
        return len;
    }
    if (len == 0) {
        /* Empty path: Linux needs AT_EMPTY_PATH (unsupported) to mean the dirfd
         * itself, so this is ENOENT for both open and stat. */
        return -ENOENT;
    }
    if (tmp[0] == '/') {
        if ((uint32_t)len + 1 > size) {
            return -ENAMETOOLONG;
        }
        k_memcpy(pathbuf, tmp, (uint32_t)len + 1);
        return canonicalize_abs(pathbuf);
    }
    if (dirfd == AT_FDCWD) {
        int r = join_abs(pathbuf, size, current_proc->cwd, tmp, (uint32_t)len);
        return r < 0 ? r : canonicalize_abs(pathbuf);
    }

    /* A real dirfd must be an open directory; resolve relative to its path. */
    entry = fd_get(&current_proc->fds, dirfd);
    if (!entry) {
        return -EBADF;
    }
    /* A synthetic directory fd (/dev, /proc, /proc/<pid>[/fd]): reconstruct its
     * absolute path from the pooled kind/pid and resolve the relative name
     * there. */
    if (entry->type == FD_TYPE_SYNTH_DIR) {
        struct synth_dir *sd = (struct synth_dir *)entry->priv;
        if (!sd) {
            return -EBADF;
        }
        synth_dir_path(sd->kind, sd->pid, dirpath, (int)sizeof(dirpath));
        {
            int r = join_abs(pathbuf, size, dirpath, tmp, (uint32_t)len);
            return r < 0 ? r : canonicalize_abs(pathbuf);
        }
    }
    if (entry->type != FD_TYPE_DIR) {
        return -ENOTDIR;
    }
    dnode = entry->node; /* NULL marks the root directory */
    if (!dnode && entry->ofd && entry->ofd->vmnt && !mnt_is_root(entry->ofd->vmnt)) {
        if (entry->ofd->dirpath[0] == '\0') {
            return -ENAMETOOLONG;
        }
        k_memcpy(dirpath, entry->ofd->dirpath, k_strlen(entry->ofd->dirpath) + 1);
    } else if (dnode) {
        /* KRFFS nodes are not refcounted, so a directory fd can outlive its
         * node if the directory is rmdir'd. Reject a stale fd defensively: the
         * node must still be a live reserved node, and its reconstructed path
         * must round-trip back to the same node. */
        if (dnode->type != KRFFS_RESERVED_NODE) {
            return -EBADF;
        }
        if (krffs_node_path(dnode, dirpath, sizeof(dirpath)) != 0) {
            return -ENAMETOOLONG;
        }
        if (krffs_resolve(dirpath) != dnode) {
            return -EBADF;
        }
    } else {
        dirpath[0] = '/';
        dirpath[1] = '\0';
    }
    {
        int r = join_abs(pathbuf, size, dirpath, tmp, (uint32_t)len);
        return r < 0 ? r : canonicalize_abs(pathbuf);
    }
}

static int32_t synth_open_dir(const struct vpath *vp, int flags);

/** Non-zero when the caller has appropriate privilege (effective uid 0). */
static int cred_privileged(void)
{
    return current_proc->euid == 0;
}

/**
 * Drop the set-id bits a modification must not leave standing on `node`.
 *
 * Setuid always goes: a file whose contents or owner just changed no longer
 * grants what its owner meant it to grant. Setgid goes only when the
 * group-execute bit is set with it, matching the grant rule in `execve`:
 * without that bit the file cannot hand out a gid, and the pair means the
 * mandatory-lock mark elsewhere in Unix.
 *
 * The root node returns untouched, and by node identity, not by
 * `KRFFS_IS_DIR`: `mkfs` stamps the root a bare 0755 with no type bits, so
 * the directory test reads false on it, and `KRFFS_MODE_DIRTY` shares the
 * setuid position. A clear that reached the root would report an unclean
 * image as cleanly unmounted, which is file-system corruption, not a
 * permission bug, and `chown(path, -1, -1)` is permitted to every caller.
 */
static void cred_kill_setid(krffs_node_t *node)
{
    if (!node || node == krffs_fs->node || KRFFS_IS_DIR(node->mode)) {
        return;
    }
    node->mode &= (uint16_t) ~KRFFS_MODE_SUID;
    if (node->mode & 0010u) {
        node->mode &= (uint16_t) ~KRFFS_MODE_SGID;
    }
}

/** Non-zero when `gid` is in the caller's supplementary list. */
static int cred_in_suppl(uint32_t gid)
{
    uint32_t i;

    for (i = 0; i < current_proc->ngroups; i++) {
        if (current_proc->groups[i] == gid) {
            return 1;
        }
    }
    return 0;
}

/** Non-zero when `gid` is the caller's effective gid or in its supplementary
 * list: the set a file owner may chgrp to, and the group the access check
 * matches. */
static int cred_in_group(uint32_t gid)
{
    return gid == current_proc->egid || cred_in_suppl(gid);
}

/* Access-check `want` bits, matching an octal permission digit. */
#define CRED_R 4
#define CRED_W 2
#define CRED_X 1

/**
 * The nine-bit permission check over explicit metadata: one identity class is
 * chosen and judged alone, the classic rule. The owner is judged by the owner
 * bits, else a group match (primary or supplementary) by the group bits, else
 * the other bits; an effective uid of 0 passes everything. `use_real` judges
 * by the real ids instead, for access(2), the one call that asks who the
 * process really is, not who it is acting as.
 */
static int32_t cred_access_bits(uint32_t owner, uint32_t group, uint32_t mode,
                                int want, int use_real)
{
    process_t *p = current_proc;
    uint32_t uid = use_real ? p->ruid : p->euid;
    uint32_t bits;

    if (uid == 0) {
        return 0;
    }
    if (uid == owner) {
        bits = mode >> 6;
    } else if ((use_real ? p->rgid : p->egid) == group ||
               cred_in_suppl(group)) {
        bits = mode >> 3;
    } else {
        bits = mode;
    }
    return ((uint32_t) want & ~bits & 7u) ? -EACCES : 0;
}

/* Whether `mnt` is the root: its nodes are KRFFS's, read directly; a node on
 * any other mount is read through the type's getattr. */
static int mnt_is_root(const struct vfs_mount *mnt)
{
    return mnt == vfs_root_mount();
}

/**
 * The permission check over a node of any type, from the attributes its
 * file system reports: the same nine-bit rule as cred_access_node, plus
 * the execute rule for a regular file.
 */
static int32_t cred_access_ref(const struct vfs_ref *r, int want, int use_real)
{
    struct vfs_stat st;
    int rc = vfs_getattr(r->mnt, r->node, &st);

    if (rc != 0) {
        return rc;
    }
    if ((want & CRED_X) && (st.mode & 0170000u) != 0040000u && !(st.mode & 0111u)) {
        return -EACCES;
    }
    return cred_access_bits(st.uid, st.gid, st.mode & 0777u, want, use_real);
}

/**
 * The permission check over a KRFFS node. The root directory reads as 0755,
 * the fixed identity stat reports (its stored mode has the dirty-mount
 * flag). Executing a regular file demands an execute bit somewhere even for
 * root: root runs programs, not data.
 */
static int32_t cred_access_node(const krffs_node_t *node, int want,
                                int use_real)
{
    uint32_t mode = (node == krffs_fs->node)
                        ? 0755u
                        : (uint32_t) (node->mode & 0777u);

    if ((want & CRED_X) && !KRFFS_IS_DIR(node->mode) &&
        node != krffs_fs->node && !(mode & 0111u)) {
        return -EACCES;
    }
    return cred_access_bits(node->uid, node->gid, mode, want, use_real);
}




/**
 * The sticky rule on removing or renaming `victim` out of directory `dir`.
 *
 * With the directory's sticky bit set, write permission on it is no longer
 * enough: only the entry's owner, the directory's owner, or a privileged caller
 * may take an entry out. That is what makes a shared 1777 directory usable, and
 * `-EPERM` is the errno POSIX names for it, not `EACCES`.
 *
 * The SVR4 relaxation that also permits it when the caller can write the file
 * is not POSIX and is not adopted here: it would let a mode a victim chose for
 * their own file decide who may delete it.
 *
 * The root is non-sticky by node identity, not by reading its mode, the
 * same fixed-mode rule `cred_access_node` uses, so `KRFFS_MODE_DIRTY` never
 * enters a permission answer.
 */
static int32_t cred_sticky_ok(const krffs_node_t *dir, const krffs_node_t *victim)
{
    if (!dir || dir == krffs_fs->node || !(dir->mode & KRFFS_MODE_STICKY)) {
        return 0;
    }
    if (cred_privileged() || current_proc->euid == dir->uid ||
        (victim && current_proc->euid == victim->uid)) {
        return 0;
    }
    return -EPERM;
}

/**
 * Open a path that classified as synthetic (/dev or /proc).
 *
 * Devices bind their file_ops; directories get a synthetic dir fd; /proc files
 * snapshot their text. A synthetic mount is never created or fallen through to
 * KRFFS.
 */
static int32_t synth_open(const struct vpath *vp, int flags)
{
    switch (vp->kind) {
        case VNODE_DEV: {
            /* The nine-bit check devices always deserved and only bootpart
             * needs: every entry is owned root:root, so for the 0666 devices
             * this grants what it always granted, and the raw boot-partition
             * window (0600) opens for root alone. */
            uint32_t dmode;
            int want;

            if (devfs_stat(vp->name, &dmode, NULL, NULL) != 0) {
                return -ENOENT;
            }
            switch (flags & 3) {
                case 1:
                    want = CRED_W;
                    break;
                case 2:
                    want = CRED_R | CRED_W;
                    break;
                default:
                    want = CRED_R;
                    break;
            }
            {
                int32_t rc = cred_access_bits(0, 0, dmode & 0777u, want, 0);
                if (rc != 0) {
                    return rc;
                }
            }
            return devfs_open(vp->name, flags);
        }
        case VNODE_PTS:
            /* /dev/pts/N: the slave end of pty N (pty_open_slave does its own
             * fd allocation, and rejects an unallocated/locked pair). The open
             * is permission-checked against the pty's ownership, so one user
             * cannot open another's terminal. */
            if (flags & O_DIRECTORY) {
                return -ENOTDIR;
            }
            {
                struct pty *pt = pty_get(vp->pts_index);
                if (pt) {
                    unsigned acc = (unsigned) flags & 3u;
                    int want = (acc == 0) ? CRED_R
                                          : (acc == 1) ? CRED_W
                                                       : (CRED_R | CRED_W);
                    int32_t rc = cred_access_bits(pt->uid, pt->gid,
                                                  (uint32_t) (pt->mode & 0777u),
                                                  want, 0);
                    if (rc != 0) {
                        return rc;
                    }
                }
            }
            return pty_open_slave(vp->pts_index, flags);
        case VNODE_PROC_FILE:
        case VNODE_PROC_CMDLINE:
        case VNODE_PROC_NET_DEV:
        case VNODE_PROC_UPTIME:
        case VNODE_PROC_STAT_SYS:
        case VNODE_PROC_MEMINFO:
        case VNODE_PROC_MOUNTS:
        case VNODE_PROC_USB:
        case VNODE_PROC_FILESYSTEMS:
        case VNODE_PROC_NET_ROUTE:
        case VNODE_PROC_NET_ARP:
        case VNODE_PROC_NET_TCP:
            return procfs_open(vp, flags);
        case VNODE_PROC_DISK:
            /* Root only. The file has the drive's serial number, and an
             * image that has this file also has an unprivileged account. */
            {
                int32_t rc = cred_access_bits(0, 0, 0400u, CRED_R, 0);
                if (rc != 0) {
                    return rc;
                }
            }
            return procfs_open(vp, flags);
        case VNODE_DEV_DIR:
        case VNODE_PTS_DIR:
        case VNODE_PROC_DIR:
        case VNODE_PROC_NET_DIR:
        case VNODE_PROC_PID:
        case VNODE_PROC_FD_DIR:
            return synth_open_dir(vp, flags);
        case VNODE_ENOTDIR:
            return -ENOTDIR;
        default:
            return -ENOENT; /* including VNODE_PROC_FD_LINK: read via readlink */
    }
}


/* Open a synthetic directory (/dev, /proc, /proc/<pid>, /proc/<pid>/fd). The
 * SYNTH_DIR_* priv encode/decode macros live in synthfs.h. */
static int32_t synth_open_dir(const struct vpath *vp, int flags)
{
    fd_entry_t *e;
    int fd;
    int pid = 0;

    struct synth_dir *sd;

    if (vp->kind == VNODE_PROC_PID || vp->kind == VNODE_PROC_FD_DIR) {
        pid = procfs_resolve_pid(vp->pid);
        if (pid < 0) {
            return -ENOENT;
        }
    }
    /* The directory exists, so honour flags that contradict that. An exclusive
     * create of an existing object is -EEXIST regardless of access mode (the
     * usual O_CREAT|O_EXCL form implies write access), so that check comes
     * first; then a directory cannot be opened for writing. */
    if ((flags & O_CREAT) && (flags & O_EXCL)) {
        return -EEXIST;
    }
    if ((flags & 3) != 0) {
        return -EISDIR;
    }
    sd = synth_dir_alloc(vp->kind, pid);
    if (!sd) {
        return -ENFILE;
    }
    fd = fd_alloc(&current_proc->fds);
    if (fd < 0) {
        sd->used = 0;
        return -EMFILE;
    }
    e = &current_proc->fds.entries[fd];
    e->type = FD_TYPE_SYNTH_DIR;
    e->node = NULL;
    e->sock = NULL;
    e->ops = NULL;
    e->priv = sd;
    e->ofd->offset = 0;
    e->flags = FD_FLAG_READ;
    if (flags & O_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    return fd;
}

/**
 * Stamps a newly created KRFFS node with the creating process's identity:
 * owner and group from the effective ids, permission bits from `mode` with
 * the file-creation mask applied, the type bits kept. The V7 creation rule:
 * who you effectively are is whose the file is.
 */
static void krffs_stamp_new_node(krffs_node_t *node, uint32_t mode)
{
    node->uid = current_proc->euid;
    node->gid = current_proc->egid;
    node->mode = (uint16_t) ((node->mode & ~07777u) |
                             (mode & 07777u & ~current_proc->umask));
}

/**
 * Open core shared by open(5) and openat(295).
 *
 * `name` is a kernel-space path with any leading '/' already stripped; `flags`
 * are the Linux open flags and `mode` the permission bits to stamp on a file
 * created by O_CREAT (ignored otherwise). Resolves a device ("fb"/"mouse") or a
 * KRFFS path and installs a descriptor. Returns the fd or -errno. Opening "/"
 * opens the root directory for getdents64.
 */
static int32_t do_open(const char *name, int flags, uint32_t mode)
{
    fd_entry_t *entry;
    krffs_node_t *node;
    struct vfs_ref ref;
    int fd;
    int rc;

    if (name[0] == 'f' && name[1] == 'b' && name[2] == '\0') {
        if (fb_owner_pid != 0 && fb_owner_pid != current_proc->pid) {
            return -EBUSY;  /* Already in use */
        }

        fd = fd_alloc(&current_proc->fds);
        if (fd < 0) {
            return -EMFILE;
        }

        entry = &current_proc->fds.entries[fd];
        entry->type = FD_TYPE_FRAMEBUFFER;
        entry->node = NULL;
        entry->ofd->offset = 0;
        entry->flags = FD_FLAG_READ | FD_FLAG_WRITE;

        vga_mode13h_enter();
        fb_owner_pid = current_proc->pid;

        kbd_clear_scancodes(); /* drop stale scancodes left by the shell */

        return fd;
    }

    if (name[0] == 'm' && name[1] == 'o' && name[2] == 'u' &&
        name[3] == 's' && name[4] == 'e' && name[5] == '\0') {
        if (!mouse_is_available() && !input_mouse_present()) {
            return -ENODEV;  /* no mouse of any kind was found */
        }

        fd = fd_alloc(&current_proc->fds);
        if (fd < 0) {
            return -EMFILE;
        }

        entry = &current_proc->fds.entries[fd];
        entry->type = FD_TYPE_MOUSE;
        entry->node = NULL;
        entry->ofd->offset = 0;
        entry->flags = FD_FLAG_READ;

        mouse_clear_packets();

        return fd;
    }

    if (!fd_can_alloc(&current_proc->fds)) {
        return -EMFILE; /* known before anything is created or truncated */
    }
    rc = vfs_resolve(0, name, 0, &ref, 0, 0);
    if (rc == -ENOENT && (flags & O_CREAT)) {
        /* The missing-file half of O_CREAT: hold the parent, check its
         * write bit, create under the namespace lock, and stamp the new
         * node with the caller's identity and masked mode. */
        struct vfs_ref parent;
        char leafname[VFS_NAME_MAX + 1];
        void *made;

        rc = vfs_resolve(0, name, VFS_WALK_PARENT, &parent, leafname,
                         sizeof(leafname));
        if (rc != 0) {
            return rc;
        }
        if (vfs_synth_vpath(parent.mnt, parent.node)) {
            vfs_ref_release(&parent);
            return -EROFS; /* nothing is created under /dev or /proc */
        }
        if (!mnt_is_root(parent.mnt)) {
            vfs_ref_release(&parent);
            return -EROFS; /* no other type creates in this milestone */
        }
        rc = (int)cred_access_node((krffs_node_t *)parent.node, CRED_W, 0);
        if (rc != 0) {
            vfs_ref_release(&parent);
            return rc;
        }
        vfs_ns_lock(parent.mnt);
        rc = vfs_create(parent.mnt, parent.node, leafname, mode, &made);
        vfs_ns_unlock(parent.mnt);
        if (rc != 0) {
            vfs_ref_release(&parent);
            return rc;
        }
        krffs_stamp_new_node((krffs_node_t *)made, mode);
        ref.mnt = parent.mnt;
        ref.node = made; /* held by creation; the parent's hold goes back */
        if (parent.mnt->type->ops->put) {
            parent.mnt->type->ops->put(parent.mnt, parent.node);
        }
        node = made;
    } else if (rc != 0) {
        return rc;
    } else {
        const struct vpath *vp = vfs_synth_vpath(ref.mnt, ref.node);

        if (vp) {
            struct vpath vpc = *vp;

            vfs_ref_release(&ref);
            return synth_open(&vpc, flags);
        }
        node = ref.node;
        if (!mnt_is_root(ref.mnt)) {
            /* A node of another type: the checks from its attributes in
             * the order open(2) keeps (EEXIST, EISDIR, ENOTDIR), then the
             * mount's EROFS for a write-side request, then permission. */
            struct vfs_stat vst;
            unsigned acc = (unsigned)flags & 3u;
            int want = acc == 0 ? CRED_R : (acc == 1 ? CRED_W : (CRED_R | CRED_W));

            rc = vfs_getattr(ref.mnt, node, &vst);
            if (rc != 0) {
                vfs_ref_release(&ref);
                return rc;
            }
            if ((flags & O_CREAT) && (flags & O_EXCL)) {
                vfs_ref_release(&ref);
                return -EEXIST;
            }
            if ((vst.mode & 0170000u) == 0040000u && (acc != 0 || (flags & O_TRUNC))) {
                vfs_ref_release(&ref);
                return -EISDIR;
            }
            if ((vst.mode & 0170000u) != 0040000u && (flags & O_DIRECTORY)) {
                vfs_ref_release(&ref);
                return -ENOTDIR;
            }
            if ((acc != 0 || (flags & O_TRUNC)) && (ref.mnt->flags & VFS_MS_RDONLY)) {
                vfs_ref_release(&ref);
                return -EROFS;
            }
            if (flags & O_TRUNC) {
                want |= CRED_W;
            }
            rc = (int)cred_access_ref(&ref, want, 0);
            if (rc != 0) {
                vfs_ref_release(&ref);
                return rc;
            }
            if (flags & O_TRUNC) {
                rc = vfs_truncate(ref.mnt, node, 0);
                if (rc != 0) {
                    vfs_ref_release(&ref);
                    return rc;
                }
            }
            goto setup;
        }
        {
            int is_dir = (node == krffs_fs->node) || KRFFS_IS_DIR(node->mode);

            /* O_CREAT|O_EXCL requires the file not already exist. */
            if ((flags & O_CREAT) && (flags & O_EXCL)) {
                vfs_ref_release(&ref);
                return -EEXIST;
            }
            /* A directory cannot be opened for writing or truncated into a
             * file (the access mode is the low two bits); O_DIRECTORY
             * requires one. */
            if (is_dir && (((unsigned)flags & 3u) != 0 || (flags & O_TRUNC))) {
                vfs_ref_release(&ref);
                return -EISDIR;
            }
            if (!is_dir && (flags & O_DIRECTORY)) {
                vfs_ref_release(&ref);
                return -ENOTDIR;
            }
        }
        /* The open itself is permission-checked: the access mode asks for
         * read, write, or both, and truncation is a write. */
        {
            int want;
            unsigned acc = (unsigned) flags & 3u;
            int32_t crc;

            if (acc == 0) {
                want = CRED_R;
            } else if (acc == 1) {
                want = CRED_W;
            } else {
                want = CRED_R | CRED_W;
            }
            if (flags & O_TRUNC) {
                want |= CRED_W;
            }
            crc = cred_access_node(node, want, 0);
            if (crc != 0) {
                vfs_ref_release(&ref);
                return crc;
            }
        }
        if (flags & O_TRUNC) {
            /* A truncate to zero only ever shrinks, and a shrink keeps the
             * node where it is. This branch is already past the -EISDIR for
             * the root and for any directory, so only a regular file
             * reaches it. */
            rc = vfs_truncate(ref.mnt, node, 0);
            if (rc != 0) {
                vfs_ref_release(&ref);
                return rc;
            }
            if (!cred_privileged()) {
                cred_kill_setid(node);
            }
        }
    }

setup:
    fd = fd_alloc(&current_proc->fds);
    if (fd < 0) {
        vfs_ref_release(&ref);
        return -EMFILE;  /* No free fds */
    }

    /* Set up the fd entry. A directory opens as a FD_TYPE_DIR (its node is the
     * directory whose children getdents lists); a regular file as FD_TYPE_FILE.
     * A path that resolved to the root (e.g. "x/..") is the root directory,
     * stored with a NULL node like a literal "/" open. A node of another
     * type keeps `node` NULL: the description's own fields name it, and
     * the KRFFS-only paths (the holder rule, the set-id clear) have nothing
     * to do with it. */
    entry = &current_proc->fds.entries[fd];
    if (!mnt_is_root(ref.mnt)) {
        struct vfs_stat vst;

        rc = vfs_getattr(ref.mnt, node, &vst);
        if (rc != 0) {
            fd_free(&current_proc->fds, fd);
            vfs_ref_release(&ref);
            return rc;
        }
        entry->type = (vst.mode & 0170000u) == 0040000u ? FD_TYPE_DIR : FD_TYPE_FILE;
        entry->node = NULL;
        entry->ofd->dirpath[0] = '\0';
        if (entry->type == FD_TYPE_DIR && k_strlen(name) + 1 < sizeof(entry->ofd->dirpath)) {
            entry->ofd->dirpath[0] = '/';
            k_memcpy(entry->ofd->dirpath + 1, name, k_strlen(name) + 1);
        }
    } else if (node == krffs_fs->node) {
        entry->type = FD_TYPE_DIR;
        entry->node = NULL;
    } else {
        entry->type = KRFFS_IS_DIR(node->mode) ? FD_TYPE_DIR : FD_TYPE_FILE;
        entry->node = node;
    }
    entry->ofd->offset = 0;

    /* The description takes the resolution's hold and one reference on the
     * mount; its last close gives all of it back through vfs_desc_close. */
    rc = vfs_open_node(ref.mnt, node, flags, &entry->ofd->fhandle);
    if (rc != 0) {
        fd_free(&current_proc->fds, fd);
        vfs_ref_release(&ref);
        return rc;
    }
    entry->ofd->vmnt = ref.mnt;
    entry->ofd->vnode = node;
    vfs_desc_open(ref.mnt);
    vfs_leave(ref.mnt);

    /* Access mode is the low two bits (O_RDONLY=0/O_WRONLY=1/O_RDWR=2); create,
     * trunc and cloexec live higher up, so mask before testing; the legacy
     * `flags == 0` form misreads O_RDONLY|O_CREAT as not-readable. */
    if ((flags & 3) == 1) {
        entry->flags = FD_FLAG_WRITE;
    } else if ((flags & 3) == 2) {
        entry->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    } else {
        entry->flags = FD_FLAG_READ;
    }
    if (entry->ofd) {
        /* O_APPEND/O_NONBLOCK are OFD status flags, shared by dup/fork. */
        if (flags & O_APPEND) {
            entry->ofd->status_flags |= OFD_APPEND;
        }
        if (flags & O_NONBLOCK) {
            entry->ofd->status_flags |= OFD_NONBLOCK;
        }
    }
    if (flags & O_CLOEXEC) {
        entry->flags |= FD_FLAG_CLOEXEC; /* opendir() and fopen(.,"e") rely on this */
    }

    return fd;
}

/**
 * mount(2): EBX = source, ECX = target, EDX = type, ESI = flags, EDI = data.
 *
 * Effective uid 0 is required. The strings are copied here; the order of
 * work and every other refusal are the VFS's (vfs_do_mount). `data` may be
 * NULL, which is an empty option string.
 */
static int32_t sys_mount(isr_params_t *params)
{
    char tpath[256];
    char source[64];
    char target[256];
    char type[16];
    char data[128];
    int n;

    if (!current_proc) {
        return -ESRCH;
    }
    if (!cred_privileged()) {
        return -EPERM;
    }
    if (!params->ebx || !params->ecx || !params->edx) {
        return -EFAULT;
    }
    n = strncpy_from_user(source, (const char __user *)params->ebx, sizeof(source));
    if (n < 0) {
        return n;
    }
    n = strncpy_from_user(target, (const char __user *)params->ecx, sizeof(target));
    if (n < 0) {
        return n;
    }
    /* The target as an absolute canonical path: a relative one is joined
     * to the working directory, as every other path-taking call does. */
    if (target[0] == '/') {
        k_memcpy(tpath, target, k_strlen(target) + 1);
        n = canonicalize_abs(tpath);
    } else {
        n = join_abs(tpath, sizeof(tpath), current_proc->cwd, target, k_strlen(target));
        if (n >= 0) {
            n = canonicalize_abs(tpath);
        }
    }
    if (n < 0) {
        return n;
    }
    n = strncpy_from_user(type, (const char __user *)params->edx, sizeof(type));
    if (n < 0) {
        return n;
    }
    data[0] = '\0';
    if (params->edi) {
        n = strncpy_from_user(data, (const char __user *)params->edi, sizeof(data));
        if (n < 0) {
            return n;
        }
    }
    if (target[0] != '/') {
        /* A relative target is joined to the working directory the way
         * every other path is. */
        char joined[256];
        int r = join_abs(joined, sizeof(joined), current_proc->cwd, target,
                         (uint32_t)k_strlen(target));

        if (r < 0) {
            return r;
        }
        k_memcpy(target, joined, sizeof(target));
    }
    return vfs_do_mount(source, tpath, type, params->esi, data);
}

/**
 * umount2(2): EBX = target, ECX = flags. Effective uid 0 is required, and
 * the flags must be zero: MNT_FORCE, MNT_DETACH and UMOUNT_NOFOLLOW are
 * promises this kernel cannot keep and answer EINVAL.
 */
static int32_t sys_umount2(isr_params_t *params)
{
    char tpath[256];
    char target[256];
    int n;

    if (!current_proc) {
        return -ESRCH;
    }
    if (!cred_privileged()) {
        return -EPERM;
    }
    if (params->ecx != 0) {
        return -EINVAL;
    }
    if (!params->ebx) {
        return -EFAULT;
    }
    n = strncpy_from_user(target, (const char __user *)params->ebx, sizeof(target));
    if (n < 0) {
        return n;
    }
    /* The target as an absolute canonical path: a relative one is joined
     * to the working directory, as every other path-taking call does. */
    if (target[0] == '/') {
        k_memcpy(tpath, target, k_strlen(target) + 1);
        n = canonicalize_abs(tpath);
    } else {
        n = join_abs(tpath, sizeof(tpath), current_proc->cwd, target, k_strlen(target));
        if (n >= 0) {
            n = canonicalize_abs(tpath);
        }
    }
    if (n < 0) {
        return n;
    }
    if (target[0] != '/') {
        char joined[256];
        int r = join_abs(joined, sizeof(joined), current_proc->cwd, target,
                         (uint32_t)k_strlen(target));

        if (r < 0) {
            return r;
        }
        k_memcpy(target, joined, sizeof(target));
    }
    return vfs_do_umount(tpath);
}

/**
 * The removal rules for a directory on a type other than the root: write
 * permission on the directory, and in a sticky directory the entry's owner,
 * the directory's owner, or root. The root type keeps its own copy of the
 * rule beside its node fields.
 */
static int cred_remove_ref(const struct vfs_ref *parent, const char *leaf)
{
    struct vfs_stat pst, est;
    void *node;
    int rc = cred_access_ref(parent, CRED_W, 0);

    if (rc != 0 || cred_privileged()) {
        return rc;
    }
    rc = vfs_getattr(parent->mnt, parent->node, &pst);
    if (rc != 0 || !(pst.mode & 01000u) || pst.uid == current_proc->euid) {
        return rc;
    }
    rc = parent->mnt->type->ops->lookup(parent->mnt, parent->node, leaf, &node);
    if (rc != 0) {
        return rc == -ENOENT ? 0 : rc; /* a name not there yet is not held by anyone */
    }
    rc = vfs_getattr(parent->mnt, node, &est);
    if (parent->mnt->type->ops->put) {
        parent->mnt->type->ops->put(parent->mnt, node);
    }
    if (rc != 0) {
        return rc;
    }
    return est.uid == current_proc->euid ? 0 : -EPERM;
}

static int32_t sys_open(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
    int flags = (int)params->ecx;
    uint32_t mode = params->edx; /* open(2): the O_CREAT mode is the third arg */
    char pathbuf[256];
    const char *name;
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    len = copy_path_cwd(pathbuf, sizeof(pathbuf), upath);
    if (len < 0) {
        return len;
    }
    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }
    return do_open(name, flags, mode);
}

/*
 * openat (syscall 295): EBX=dirfd, ECX=path, EDX=flags, ESI=mode. musl
 * issues openat (not the legacy open) for every <stdio.h> file open. An
 * absolute path is taken as-is, a relative path resolves against the cwd
 * (AT_FDCWD) or against the directory the dirfd refers to (see
 * copy_path_at).
 */
static int32_t sys_openat(isr_params_t *params)
{
    int dirfd = (int)params->ebx;
    const char __user *upath = (const char __user *)params->ecx;
    int flags = (int)params->edx;
    uint32_t mode = params->esi; /* openat(2): the O_CREAT mode is the fourth arg */
    char pathbuf[256];
    const char *name;
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    len = copy_path_at(pathbuf, sizeof(pathbuf), dirfd, upath);
    if (len < 0) {
        return len;
    }
    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }
    return do_open(name, flags, mode);
}

/* close(2): EBX = fd. Returns 0 or -EBADF. */
static int32_t sys_close(isr_params_t *params)
{
    int fd = (int)params->ebx;
    fd_entry_t *entry;

    if (!current_proc) {
        return -ESRCH;
    }

    /* Closing stdin/stdout/stderr is legal; a shell or daemon closes fd 0/1/2
     * and reopens onto the freed (lowest) number to redirect them. */

    /* Release the descriptor's backing (framebuffer/socket/ops object), then
     * free the slot. fd_free reports a bad/unallocated descriptor with -1;
     * translate to the close(2) errno. */
    entry = fd_get(&current_proc->fds, fd);
    fd_release_backing(entry);
    if (fd_free(&current_proc->fds, fd) != 0) {
        return -EBADF;
    }
    return 0;
}

/**
 * Flush a file's data to stable storage (fsync(2); also serves fdatasync).
 *
 * EBX = fd. Validates the descriptor (any open fd; the type is not checked),
 * then writes back every dirty page of the disk-backed window via `pc_sync`.
 * The flush is whole-cache, not per-file, a safe superset of the
 * contract. When KRFFS is the RAM module the cache is empty, so the flush is
 * a no-op and the call only validates the fd. Returns 0, -EBADF for a bad
 * descriptor, or -EIO when a disk writeback fails. dropbear writes its host
 * key with the open/write/fsync/rename idiom and treats an fsync error as
 * fatal, so the -EIO is real backpressure.
 */
static int32_t sys_fsync(isr_params_t *params)
{
    int fd = (int)params->ebx;

    if (!current_proc) {
        return -ESRCH;
    }
    {
        fd_entry_t *e = fd_get(&current_proc->fds, fd);

        if (!e) {
            return -EBADF;
        }
        if (e->ofd && e->ofd->vmnt) {
            return vfs_fsync(e->ofd->vmnt, e->ofd->fhandle);
        }
    }
    if (pc_sync() != 0) {
        return -EIO;
    }
    return 0;
}

/**
 * Flush all buffered file-system writes to stable storage (sync(2)).
 *
 * Takes no argument. Writes back every dirty page of the disk-backed window via
 * `pc_sync` (a no-op when KRFFS is the RAM module). Like Linux's sync(2), it
 * returns 0 even if a writeback failed; the dirty pages stay for a later retry.
 */
static int32_t sys_sync(isr_params_t *params)
{
    (void) params;
    pc_sync();
    return 0;
}


/* The magics Linux's reboot(2) demands, which musl passes for the caller: a
 * first word, one of four accepted second words, and then the command. */
#define REBOOT_MAGIC1     0xfee1deadu
#define REBOOT_MAGIC2     672274793u  /* 0x28121969 */
#define REBOOT_MAGIC2A    85072278u   /* 0x05121996 */
#define REBOOT_MAGIC2B    369367448u  /* 0x16041998 */
#define REBOOT_MAGIC2C    537993216u  /* 0x20112000 */

/* The commands. Two of them are negative read as a signed int, which is why
 * everything here compares raw 32-bit values. */
#define REBOOT_CMD_RESTART  0x01234567u
#define REBOOT_CMD_HALT     0xcdef0123u
#define REBOOT_CMD_POWER_OFF 0x4321fedcu
#define REBOOT_CMD_CAD_ON   0x89abcdefu
#define REBOOT_CMD_CAD_OFF  0x00000000u

/**
 * reboot(2): stop the machine. EBX = magic1, ECX = magic2, EDX = command.
 *
 * Does not return for the three commands that stop the machine. Returns
 * `-EPERM` for an unprivileged caller, `-EINVAL` for a wrong magic or an
 * unknown command, and 0 for `RB_DISABLE_CAD`.
 *
 * The privilege check comes before the magics, which inverts the order the rest
 * of this file uses, and Linux inverts it the same way. A magic-first handler
 * would let any uid learn the ABI of the one call that stops the machine and
 * erases the evidence of what it was doing; nothing is learned from `-EPERM`.
 *
 * `RB_DISABLE_CAD` returning 0 is what makes the rest testable. Every command
 * that works stops the machine, so without one that succeeds quietly a caller
 * cannot tell an accepted magic from a rejected one: both would answer
 * `-EINVAL`. It asks for a state this kernel is permanently in, since there is
 * no keyboard reboot to disable, so 0 is a true answer. `RB_ENABLE_CAD` asks
 * for a feature that does not exist and answers `-EINVAL`, because a 0 there
 * would be the one reply a caller could not tell from a working one.
 */
static int32_t sys_reboot(isr_params_t *params)
{
    uint32_t magic1 = (uint32_t) params->ebx;
    uint32_t magic2 = (uint32_t) params->ecx;
    uint32_t cmd = (uint32_t) params->edx;

    if (!cred_privileged()) {
        return -EPERM;
    }
    if (magic1 != REBOOT_MAGIC1) {
        return -EINVAL;
    }
    if (magic2 != REBOOT_MAGIC2 && magic2 != REBOOT_MAGIC2A &&
        magic2 != REBOOT_MAGIC2B && magic2 != REBOOT_MAGIC2C) {
        return -EINVAL;
    }

    switch (cmd) {
    case REBOOT_CMD_RESTART:
        kernel_shutdown(POWER_RESTART);
        break;
    case REBOOT_CMD_HALT:
        kernel_shutdown(POWER_HALT);
        break;
    case REBOOT_CMD_POWER_OFF:
        kernel_shutdown(POWER_OFF);
        break;
    case REBOOT_CMD_CAD_OFF:
        return 0;
    default:
        break;
    }

    return -EINVAL;
}

/**
 * Resolve a seek on a raw disk window, in unsigned arithmetic throughout.
 *
 * The obvious form adds the delta to the base as a signed 64-bit value and
 * then checks the result for negativity, which is undefined the moment the sum
 * leaves the signed range: a descriptor parked at the top of that range and
 * seeked forward by one has no defined result to test. So the base is unsigned,
 * the delta's magnitude is taken without ever negating it (negating the most
 * negative value is itself undefined), and both directions are bounds-checked
 * before the arithmetic, not after.
 *
 * @param cur    the window's current offset.
 * @param size   the window's byte size, for SEEK_END.
 * @param delta  the requested displacement, which may be negative.
 * @param whence 0 SEEK_SET, 1 SEEK_CUR, 2 SEEK_END.
 * @param out    the resolved offset, untouched on failure.
 * @return 0, `-EINVAL` for a bad whence or a result before the start, or
 *         `-EOVERFLOW` for one past what an offset can hold.
 */
static int32_t raw_seek_resolve(uint64_t cur, uint64_t size, long long delta, int whence,
                                uint64_t *out)
{
    uint64_t base;
    uint64_t mag;

    switch (whence) {
        case 0:
            base = 0;
            break;
        case 1:
            base = cur;
            break;
        case 2:
            base = size;
            break;
        default:
            return -EINVAL;
    }

    if (delta >= 0) {
        if ((uint64_t)delta > (uint64_t)0xFFFFFFFFFFFFFFFFull - base) {
            return -EOVERFLOW;
        }
        *out = base + (uint64_t)delta;
        return 0;
    }

    /* -(delta) is undefined when delta is the most negative value, so the
       magnitude is built from delta + 1, which is always representable. */
    mag = (uint64_t)(-(delta + 1)) + 1u;
    if (mag > base) {
        return -EINVAL;
    }
    *out = base - mag;
    return 0;
}

/**
 * lseek(2): EBX = fd, ECX = offset, EDX = whence (0=SET, 1=CUR, 2=END).
 *
 * A raw disk window seeks like a file whose size is the window's: SEEK_END
 * resolves against it, and an offset past the end is allowed, since a read
 * there returns 0 and a write `-ENOSPC`. Its offset is the open-file
 * description's 64-bit `raw_offset`, which this call can address only part of;
 * a real offset it cannot return is refused with `-EOVERFLOW` rather than
 * truncated, and `_llseek` is the way to reach the far end of a large drive.
 *
 * @return the new offset, or -errno.
 */
static int32_t sys_lseek(isr_params_t *params)
{
    int fd = (int)params->ebx;
    int32_t offset = (int32_t)params->ecx;
    int whence = (int)params->edx;
    fd_entry_t *entry;
    uint32_t file_size;
    int32_t new_offset;

    if (!current_proc) {
        return -ESRCH;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry) {
        return -EBADF;
    }
    if (entry->type == FD_TYPE_PIPE) {
        return -ESPIPE; /* a pipe is not seekable (POSIX) */
    }
    if (entry->type == FD_TYPE_DEVICE) {
        uint64_t raw_size;
        uint32_t log_size;

        if (devfs_fd_is_raw(entry)) {
            uint64_t resolved;
            int32_t rc;

            if (!devfs_fd_raw_size(entry, &raw_size)) {
                return -ENXIO; /* the window was withdrawn under an open fd */
            }
            rc = raw_seek_resolve(entry->ofd->raw_offset, raw_size, (long long)offset, whence,
                                  &resolved);
            if (rc != 0) {
                return rc;
            }
            if (resolved > (uint64_t)0x7FFFFFFF) {
                return -EOVERFLOW; /* a real offset this call cannot return */
            }
            entry->ofd->raw_offset = resolved;
            return (int32_t)resolved;
        }
        if (devfs_fd_log_size(entry, &log_size)) {
            switch (whence) {
                case 0: /* SEEK_SET */
                    new_offset = offset;
                    break;
                case 1: /* SEEK_CUR */
                    new_offset = (int32_t)entry->ofd->offset + offset;
                    break;
                case 2: /* SEEK_END */
                    new_offset = (int32_t)log_size + offset;
                    break;
                default:
                    return -EINVAL;
            }
            if (new_offset < 0) {
                return -EINVAL;
            }
            entry->ofd->offset = (uint32_t)new_offset;
            return new_offset;
        }
        /* mem devices (null/zero/full) accept a seek that always lands at 0;
         * the stream devices (random/urandom) are not seekable. */
        if (devfs_lseek_zero(entry)) {
            entry->ofd->offset = 0;
            return 0;
        }
        return -ESPIPE;
    }
    if (entry->type == FD_TYPE_CONSOLE || entry->type == FD_TYPE_MOUSE) {
        return -ESPIPE; /* a terminal/mouse is a valid fd, just not seekable */
    }
    if (entry->type != FD_TYPE_FILE) {
        return -EBADF;
    }

    if (!entry->ofd || !entry->ofd->vmnt) {
        return -EBADF;
    }
    fd_ofd_lock(entry->ofd);
    {
        struct vfs_stat vst;
        int rc = vfs_getattr(entry->ofd->vmnt, entry->ofd->vnode, &vst);

        if (rc != 0) {
            fd_ofd_unlock(entry->ofd);
            return rc;
        }
        file_size = (uint32_t)vst.size;
    }

    switch (whence) {
        case 0:  /* SEEK_SET */
            new_offset = offset;
            break;
        case 1:  /* SEEK_CUR */
            new_offset = (int32_t)entry->ofd->offset + offset;
            break;
        case 2:  /* SEEK_END */
            new_offset = (int32_t)file_size + offset;
            break;
        default:
            new_offset = -1;
            break;
    }

    if (new_offset < 0) {
        fd_ofd_unlock(entry->ofd);
        return -EINVAL;
    }

    entry->ofd->offset = (uint32_t)new_offset;
    fd_ofd_unlock(entry->ofd);
    return new_offset;
}

/**
 * Whether this is the query descriptor rather than a socket that carries data.
 *
 * A DHCP client opens AF_INET/SOCK_RAW/IPPROTO_RAW to ask the interface
 * ioctls about the machine and does nothing else with it. It has no protocol,
 * so it can send and receive nothing; a raw socket with a protocol is a real
 * one, and the paths below tell them apart here rather than by socket type.
 */
static int sock_is_query(const struct socket *sock)
{
    return sock->type == SOCK_RAW && sock->protocol == 0;
}

/**
 * Read up to `count` bytes from descriptor `fd` (read(2)).
 *
 * EBX = fd, ECX = buf, EDX = count. Returns the bytes read, 0 at end, or
 * -errno. Dispatches by backing kind. A console read (stdin) blocks in the
 * tty line discipline until a cooked line or VMIN raw bytes arrive; in
 * graphics mode it is a non-blocking raw scancode read instead.
 */
static int32_t read_fd(int fd, char __user *buf, uint32_t count)
{
    fd_entry_t *entry;

    if (!current_proc) {
        return -EFAULT;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry) {
        return -EBADF;
    }

    /* A zero-length read is a no-op that returns 0 (even with a NULL buffer,
     * per POSIX) once the fd is known valid; do not touch buf. */
    if (count == 0) {
        return 0;
    }
    if (!buf) {
        return -EFAULT;
    }

    /* Backing-ops object (pipe/tty/pty/device): dispatch through its vtable,
     * but only if the descriptor was opened for reading (a write-only fd ->
     * -EBADF). The plain file and enum types below have no ops and fall through
     * to the enum dispatch. */
    if (entry->ops) {
        if (!(entry->flags & FD_FLAG_READ)) {
            return -EBADF;
        }
        return entry->ops->read ? entry->ops->read(entry, buf, count) : -EBADF;
    }

    /* read() honors O_NONBLOCK (an OFD status flag) exactly like recv(),
     * routing to the non-blocking variant. */
    if (entry->type == FD_TYPE_SOCKET) {
        int sock_nonblock;
        if (!entry->sock || !user_buffer_ok((uint32_t)buf, count, 1)) {
            return -EFAULT;
        }
        if (sock_is_query(entry->sock)) {
            return -EOPNOTSUPP;
        }
        sock_nonblock = entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK);
        /* socket_*recv write straight into the validated user buffer; __force
         * strips the __user tag at the boundary. */
        if (entry->sock->type == SOCK_DGRAM || entry->sock->type == SOCK_RAW) {
            if (sock_nonblock) {
                return socket_try_recv(entry->sock, (uint8_t __force *)buf,
                                       (int)count, NULL, NULL);
            }
            return socket_recv(entry->sock, (uint8_t __force *)buf, (int)count,
                               NULL, NULL);
        }
        if (sock_nonblock) {
            return socket_tcp_try_recv(entry->sock, (uint8_t __force *)buf,
                                       (int)count);
        }
        return socket_tcp_recv(entry->sock, (uint8_t __force *)buf, (int)count);
    }

    if (entry->type == FD_TYPE_CONSOLE) {
        unsigned char kbuf[N_TTY_READ_MAX];
        uint32_t want;
        int n;

        if (!(entry->flags & FD_FLAG_READ)) {
            return -EBADF;  /* Not readable (e.g., stdout/stderr) */
        }

        /*
         * If the process owns the framebuffer (graphics mode), use non-blocking
         * raw scancode input for game controls, bypassing the line discipline.
         */
        if (fb_owner_pid == current_proc->pid) {
            /* Non-blocking raw scancode read for games. kbd_read_scancodes
             * writes into the user buffer; range-check it first, then __force
             * strips the __user tag at the boundary. */
            if (!user_buffer_ok((uint32_t)buf, count, 1)) {
                return -EFAULT;
            }
            return kbd_read_scancodes((uint8_t __force *)buf, (int)count);
        }

        want = count;
        if (want > sizeof(kbuf)) {
            want = sizeof(kbuf);  /* one line/burst per call; caller loops */
        }

        /*
         * Park on console_tty until a line (cooked) or VMIN bytes (raw)
         * arrive. Parking marks input_wq active, and once a session owns the
         * console its sid is nonzero; either condition tells the keyboard
         * IRQ to cook keystrokes into console_tty, not the kernel
         * shell, so the byte wakes us (or waits as type-ahead).
         * stdin_waiting_proc, set across the read below, only suppresses the
         * scrollback hotkeys while we are parked; tty_read drains the serial
         * port itself. The discipline echoes, edits, and recognizes ISIG, so
         * Ctrl-C on the console now arrives as a posted SIGINT, and a
         * pending signal unwinds the wait with -EINTR.
         */
        /* A SESSION LEADER reading the console with no controlling terminal
         * claims it as its SOLE ctty (its group becomes the foreground, and any
         * pty it held is dropped), so a lone program receives ^C; a non-leader
         * does not (it would strand the tty on its since-dead leader). Once a
         * session owns the console, the foreground is set by TIOCSPGRP. */
        if (console_tty.sid == 0 && (int)current_proc->sid == (int)current_proc->pid) {
            tty_make_sole_ctty(&console_tty, (int)current_proc->sid,
                               (int)current_proc->pgid);
        }
        /* A BACKGROUND process reading its controlling terminal is stopped with
         * SIGTTIN (POSIX), unless that would wedge it (SIGTTIN blocked or
         * ignored), in which case the read fails with -EIO. Decided BEFORE
         * marking ourselves the console reader, so an early return leaves no
         * stale flag. */
        if (tty_is_background(&console_tty, (int)current_proc->sid,
                              (int)current_proc->pgid)) {
            if ((current_proc->sig_blocked & KSIG_BIT(SIGTTIN)) ||
                current_proc->sigacts[SIGTTIN].handler == KSIG_IGN) {
                return -EIO;
            }
            tty_signal_group((int)current_proc->pgid, SIGTTIN);
            return -EINTR; /* the SIGTTIN stops us; the read is retried when fg */
        }
        stdin_waiting_proc = current_proc;
        n = tty_read(&console_tty, kbuf, (int)want,
                     entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));
        stdin_waiting_proc = NULL;
        if (n < 0) {
            return n;  /* -EINTR: a signal cut the wait short */
        }
        if (copy_to_user(buf, kbuf, (uint32_t)n) != 0) {
            return -EFAULT;
        }
        return n;
    }

    if (entry->type == FD_TYPE_MOUSE) {
        int packets_requested;
        int packets_read;

        packets_requested = (int)(count / sizeof(struct mouse_packet));
        if (packets_requested <= 0) {
            return 0;  /* Buffer too small for even one packet */
        }

        /* Read packets from the mouse ring buffer. mouse_read_packets writes
         * into the user buffer; range-check it, then __force at the boundary.
         */
        if (!user_buffer_ok((uint32_t)buf,
                            (uint32_t)packets_requested * sizeof(struct mouse_packet),
                            1)) {
            return -EFAULT;
        }
        packets_read = mouse_read_packets((struct mouse_packet __force *)buf,
                                          packets_requested);

        return packets_read * (int32_t)sizeof(struct mouse_packet);
    }

    if (entry->type == FD_TYPE_FILE) {
        struct open_file *ofd = entry->ofd;
        int32_t bytes_read;

        if (!(entry->flags & FD_FLAG_READ)) {
            return -EBADF;  /* Not opened for reading */
        }
        if (!ofd || !ofd->vmnt) {
            return -EBADF;
        }
        /* The file system writes straight into the user buffer, so
         * range-check AND fault its pages in (write) before stripping the
         * __user tag. The description is held across the operation: with
         * dup or fork sharing this offset, the second reader sleeps here
         * rather than reading the same bytes while the first is in a
         * request. */
        if (!user_buffer_ok((uint32_t)(__force uintptr_t)buf, count, 1)) {
            return -EFAULT;
        }
        fd_ofd_lock(ofd);
        bytes_read = vfs_read(ofd->vmnt, ofd->fhandle, (char __force *)buf,
                              count, ofd->offset);
        if (bytes_read > 0) {
            ofd->offset += (uint32_t)bytes_read;
        }
        fd_ofd_unlock(ofd);
        return bytes_read;
    }

    return -EBADF;
}

/* read(2): EBX = fd, ECX = buf, EDX = count. */
static int32_t sys_read(isr_params_t *params)
{
    return read_fd((int)params->ebx, (char __user *)params->ecx, params->edx);
}

/* Defined with the terminal ioctls below: the tty behind a descriptor, or NULL
 * for one that is not a terminal. */
static struct tty *fd_tty(fd_entry_t *e);

/**
 * Read into `buf` only if that can be done without waiting. Returns the bytes
 * read, 0 when there are none, or a negative errno.
 *
 * This is what fills the second and later buffers of a `readv` on anything
 * whose bytes arrive, not sit there. Readiness is asked of the backing
 * object itself: the ops vtable answers `poll`, and a stream socket has a
 * non-blocking read of its own. Nothing can consume the bytes between the
 * question and the read, since a system call here runs with interrupts off from
 * end to end.
 *
 * A terminal answers 0 whatever it has. Its read returns one line, and a
 * second line into the next buffer would answer a question the reader did not
 * ask.
 */
static int32_t read_ready(fd_entry_t *entry, int fd, char __user *buf,
                          uint32_t count)
{
    if (!entry) {
        return 0;
    }
    if (fd_tty(entry)) {
        return 0;
    }
    if (entry->ops) {
        struct wait_queue *wq = 0;

        if (!entry->ops->poll || !entry->ops->read) {
            return 0;
        }
        if (!(entry->ops->poll(entry, POLLIN, &wq) & POLLIN)) {
            return 0;
        }

        return entry->ops->read(entry, buf, count);
    }
    if (entry->type == FD_TYPE_SOCKET && entry->sock) {
        int32_t r;

        if (sock_is_query(entry->sock)) {
            return -EOPNOTSUPP;
        }
        if (!user_buffer_ok((uint32_t)buf, count, 1)) {
            return -EFAULT;
        }
        if (entry->sock->type == SOCK_RAW) {
            return socket_try_recv(entry->sock, (uint8_t __force *)buf,
                                   (int)count, NULL, NULL);
        }
        r = socket_tcp_try_recv(entry->sock, (uint8_t __force *)buf,
                                (int)count);

        return (r == SOCK_EAGAIN) ? 0 : r;
    }

    return read_fd(fd, buf, count);
}

/**
 * Fill the buffers of a `readv` from one waiting datagram.
 *
 * A datagram is a message, so it is taken once and copied out in pieces: the
 * buffers are filled in order until the datagram runs out, and what is left
 * over when the buffers run out first is dropped, which is what a `read` too
 * small for a datagram already does. Exactly one datagram is consumed either
 * way, and a datagram of no bytes returns 0 without being end of input.
 *
 * The message is taken out of the socket whole and under one hold, and only
 * then spread across the buffers. Both halves of that are deliberate.
 *
 * Taking it under one hold is not optional: a syscall body runs with interrupts
 * ENABLED (only preemption is off, see net.h), so the receive interrupt would
 * otherwise deliver the next datagram over this one between two buffers, and
 * the caller would get a prefix of one message followed by bytes of another.
 *
 * Spreading it afterwards, with interrupts on and through `copy_to_user`, is
 * what keeps a caller's own iovec array from being a weapon. The buffers are
 * read from user memory as the copy goes, so a caller can point one buffer AT
 * its own vector list and have the datagram rewrite the destinations behind it.
 * Those addresses were never checked by the pass that ran before the read.
 * `copy_to_user` refuses an address it cannot fault in, which makes such a
 * caller a process scribbling on itself, not a raw kernel write to
 * whatever it named.
 *
 * The message buffer is one datagram wide on a 16 KB kernel stack. That is the
 * whole of what "a kernel buffer to read into" ever needed to be.
 *
 * `src_ip` and `src_port` report the datagram's sender for a caller that has
 * somewhere to put it, and are NULL for one that has not; the sender is read
 * under the same hold as the copy, since consuming is what publishes it.
 *
 * Returns the bytes copied, or a negative errno.
 */
static int32_t readv_dgram(fd_entry_t *entry, uint32_t iov_user, int iovcnt,
                           uint32_t *src_ip, uint16_t *src_port)
{
    uint8_t message[SOCK_RECV_BUF_SIZE];
    struct kernel_iovec iov;
    uint32_t offset = 0;
    int32_t total = 0;
    uint32_t held;
    int nonblock;
    int taken;
    int i;

    nonblock = entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK);
    if (nonblock) {
        if (socket_dgram_pending(entry->sock) < 0) {
            return -EAGAIN;
        }
    } else {
        int rc = socket_dgram_wait(entry->sock);

        if (rc < 0) {
            return rc;
        }
    }

    held = net_lock();
    taken = socket_dgram_peek(entry->sock, 0, message, (int)sizeof(message));
    if (taken < 0) {
        taken = 0;  /* it went between the wait and the hold */
    }
    socket_dgram_consume(entry->sock, src_ip, src_port);
    net_unlock(held);

    for (i = 0; i < iovcnt && (int)offset < taken; i++) {
        uint32_t chunk;

        if (copy_from_user(&iov,
                           (const void __user *)(iov_user +
                               (uint32_t)i * sizeof(struct kernel_iovec)),
                           sizeof(iov)) != 0) {
            break;
        }
        if (iov.iov_len == 0) {
            continue;
        }
        chunk = (uint32_t)taken - offset;
        if (chunk > iov.iov_len) {
            chunk = iov.iov_len;
        }
        if (copy_to_user((void __user *)iov.iov_base, message + offset,
                         chunk) != 0) {
            break;  /* the caller moved its own buffer out from under itself */
        }
        offset += chunk;
        total += (int32_t)chunk;
        if (chunk < iov.iov_len) {
            break;
        }
    }

    return total;
}

/*
 * Scatter read (readv(2)). The twin of `writev` above, and it exists for the
 * same reason that one does: musl's buffered stdio issues this call rather than
 * `read` whenever it is filling more than a single byte, so without it every
 * program that reads through `fgets`, `fread` or `scanf` fails on this system.
 * That is why several programs in this tree read a byte at a time through
 * `read` instead, and why busybox's `chpasswd`, which reads its input with
 * `xmalloc_fgetline`, exited without a message before this existed.
 *
 * EBX = fd, ECX = struct iovec[], EDX = iovcnt. Returns the total bytes read
 * across the iovecs, or a negative errno.
 *
 * The whole vector list is checked before a byte is taken, because a read
 * cannot be undone: a caller that named a bad destination in its last buffer
 * gets -EFAULT with its input still queued, not a partial total over
 * data that is now gone. The check is the one `write` makes of a buffer it is
 * about to hand a driver, `user_buffer_ok` with write access, so an address
 * that is merely inside the user window but unmapped or read-only is refused
 * here and not discovered half way through. A list that asks for nothing still
 * checks the descriptor, so a closed fd is -EBADF and not a quiet zero.
 *
 * The buffers are then filled in order, and what "in order" costs depends on
 * what is behind the descriptor, because a second read is not always the same
 * thing as reading on.
 *
 * A regular file is read again for each buffer. Its read returns at once and
 * its bytes carry no message boundary, so a walk is the single scatter read.
 *
 * A datagram socket is NOT read again: one datagram is one message, and a
 * second read would take the next one while the first one's tail was dropped.
 * The waiting datagram is copied out piece by piece instead, through
 * `socket_dgram_peek`, and consumed once at the end, so the whole of it fills
 * as many buffers as it needs and the remainder past the last one is discarded
 * the way a plain `read` discards it.
 *
 * A pipe, a stream socket, a device or a `/proc` file is read again only while
 * it says it has more, since a read of an empty one blocks for input nobody has
 * written. Asking and then reading is safe for two reasons together: no other
 * process runs in between, because a syscall is not preemptible here, and the
 * interrupts that do run can only ADD to what is readable, never take it away.
 * Nothing else empties a pipe or a receive ring.
 *
 * A terminal is read exactly once, and that is not a limitation but its
 * contract: the line discipline hands back one line, and filling a second
 * buffer with the line after it would return two lines where a reader asked
 * for one.
 *
 * Returning less than was asked for is what a reader must already handle, and
 * musl's stdio, which is what issues this call, loops.
 */
static int32_t sys_readv(isr_params_t *params)
{
    int fd = (int)params->ebx;
    uint32_t iov_user = params->ecx;
    int iovcnt = (int)params->edx;
    struct kernel_iovec iov;
    fd_entry_t *entry;
    uint32_t wanted = 0;
    int32_t total = 0;
    int walk;
    int i;

    if (iovcnt < 0 || iovcnt > IOV_MAX_LOCAL) {
        return -EINVAL;
    }

    for (i = 0; i < iovcnt; i++) {
        if (copy_from_user(&iov,
                           (const void __user *)(iov_user +
                               (uint32_t)i * sizeof(struct kernel_iovec)),
                           sizeof(iov)) != 0) {
            return -EFAULT;
        }
        if (iov.iov_len == 0) {
            continue;
        }
        if (iov.iov_len > 0x7FFFFFFFu || wanted > 0x7FFFFFFFu - iov.iov_len) {
            return -EINVAL;
        }
        if (!user_buffer_ok(iov.iov_base, iov.iov_len, 1)) {
            return -EFAULT;
        }
        wanted += iov.iov_len;
    }

    if (wanted == 0) {
        return read_fd(fd, (char __user *)0, 0);
    }

    entry = current_proc ? fd_get(&current_proc->fds, fd) : (fd_entry_t *)0;
    if (entry && entry->type == FD_TYPE_SOCKET && entry->sock &&
        entry->sock->type == SOCK_DGRAM) {
        return readv_dgram(entry, iov_user, iovcnt, 0, 0);
    }
    walk = entry && !entry->ops && entry->type == FD_TYPE_FILE;

    for (i = 0; i < iovcnt; i++) {
        int32_t r;

        if (copy_from_user(&iov,
                           (const void __user *)(iov_user +
                               (uint32_t)i * sizeof(struct kernel_iovec)),
                           sizeof(iov)) != 0) {
            return (total > 0) ? total : -EFAULT;
        }
        if (iov.iov_len == 0) {
            continue;
        }
        if (!user_access_ok(iov.iov_base, iov.iov_len)) {
            return (total > 0) ? total : -EFAULT;
        }

        /* The first buffer is filled by the ordinary read, which waits if it
         * must; every buffer after it only by one that will not wait. */
        if (total > 0 && !walk) {
            r = read_ready(entry, fd, (char __user *)iov.iov_base,
                           iov.iov_len);
            if (r <= 0) {
                break;
            }
        } else {
            r = read_fd(fd, (char __user *)iov.iov_base, iov.iov_len);
            if (r < 0) {
                return (total > 0) ? total : r;
            }
        }
        total += r;
        if ((uint32_t)r < iov.iov_len) {
            break;
        }
    }

    return total;
}


/**
 * Duplicate a descriptor onto the lowest free fd (dup(2)).
 *
 * EBX = oldfd. Returns the new fd, or -errno. The duplicate SHARES oldfd's
 * open-file description (shared seek offset); a socket gains a reference;
 * the new fd's FD_CLOEXEC is clear.
 */
static int32_t sys_dup(isr_params_t *params)
{
    int oldfd = (int)params->ebx;
    fd_entry_t *entry;
    fd_entry_t *dst;
    int newfd;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(&current_proc->fds, oldfd);
    if (!entry) {
        return -EBADF;
    }

    newfd = fd_alloc_from(&current_proc->fds, 0);
    if (newfd < 0) {
        return -EMFILE;
    }
    /* entry still points at entries[oldfd] (a fixed array; alloc did not
     * move it). */
    dst = &current_proc->fds.entries[newfd];
    fd_ofd_unref(dst->ofd); /* drop the fresh OFD; share oldfd's instead */
    dst->type  = entry->type;
    dst->node  = entry->node;
    dst->sock  = entry->sock;
    dst->ops   = entry->ops;  /* a dup shares the same backing object + vtable */
    dst->priv  = entry->priv;
    dst->ofd   = entry->ofd;
    fd_ofd_ref(dst->ofd);
    dst->flags = entry->flags & ~(uint32_t)FD_FLAG_CLOEXEC;
    if (entry->type == FD_TYPE_SOCKET && entry->sock) {
        socket_ref(entry->sock);
    }
    return newfd;
}

/* dup2(2): EBX = oldfd, ECX = newfd. Returns newfd or -errno. */
static int32_t sys_dup2(isr_params_t *params)
{
    int oldfd = (int)params->ebx;
    int newfd = (int)params->ecx;
    fd_entry_t *old_entry;
    fd_entry_t *new_entry;

    if (!current_proc) {
        return -ESRCH;
    }

    old_entry = fd_get(&current_proc->fds, oldfd);
    if (!old_entry) {
        return -EBADF;
    }

    if (newfd < 0 || newfd >= MAX_FDS) {
        return -EBADF;
    }

    if (oldfd == newfd) {
        return newfd;
    }

    /* Close newfd if it's open (ignore errors), releasing every backing it held
     * (socket reference, ops object, framebuffer ownership). */
    if (fd_is_valid(&current_proc->fds, newfd)) {
        fd_release_backing(fd_get(&current_proc->fds, newfd));
        fd_free(&current_proc->fds, newfd);
    }

    /* Duplicate the descriptor: it SHARES the open-file description (so the
     * seek offset is shared, per POSIX), gaining a reference; a duplicated
     * socket gains another reference too; and dup2 leaves the new fd's CLOEXEC
     * clear. */
    new_entry = &current_proc->fds.entries[newfd];
    new_entry->type  = old_entry->type;
    new_entry->node  = old_entry->node;
    new_entry->sock  = old_entry->sock;
    new_entry->ops   = old_entry->ops;  /* share the backing object + vtable */
    new_entry->priv  = old_entry->priv;
    new_entry->ofd   = old_entry->ofd;
    fd_ofd_ref(new_entry->ofd);
    new_entry->flags = old_entry->flags & ~(uint32_t)FD_FLAG_CLOEXEC;

    if (new_entry->type == FD_TYPE_SOCKET && new_entry->sock) {
        socket_ref(new_entry->sock);
    }

    return newfd;
}

/**
 * Change the current working directory (chdir(2)).
 *
 * EBX = path. Returns 0 or -errno. Stores the canonical absolute path of the
 * resolved directory (a KRFFS directory or a synthetic /dev or /proc
 * directory); a regular file is -ENOTDIR, a missing path -ENOENT.
 */
static int32_t sys_chdir(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
    char pathbuf[256];
    const char *name;
    krffs_node_t *node;
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }

    /* Resolve the target relative to the current cwd, into an absolute path. */
    len = copy_path_cwd(pathbuf, sizeof(pathbuf), upath);
    if (len < 0) {
        return len;
    }

    /* "/" (or an empty path that absolutized to "/") is the root. */
    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }
    if (name[0] == '\0') {
        current_proc->cwd[0] = '/';
        current_proc->cwd[1] = '\0';
        vfs_cwd_clear(current_proc);
        return 0;
    }

    /* A synthetic directory becomes the cwd (its canonical path, self resolved
     * to a real pid, so later relative opens re-classify correctly); a
     * synthetic leaf is ENOTDIR, a missing one ENOENT. */
    {
        struct vfs_ref ref;
        const struct vpath *svp;
        int32_t rc = vfs_resolve(0, pathbuf, 0, &ref, 0, 0);

        if (rc != 0) {
            return rc;
        }
        svp = vfs_synth_vpath(ref.mnt, ref.node);
        if (svp) {
            struct vpath vp = *svp;
            int rpid = 0;

            vfs_ref_release(&ref);
            if (vp.kind == VNODE_PROC_PID || vp.kind == VNODE_PROC_FD_DIR) {
                rpid = procfs_resolve_pid(vp.pid);
                if (rpid < 0) {
                    return -ENOENT;
                }
            }
            if (vp.kind == VNODE_DEV_DIR || vp.kind == VNODE_PTS_DIR ||
                vp.kind == VNODE_PROC_DIR || vp.kind == VNODE_PROC_NET_DIR ||
                vp.kind == VNODE_PROC_PID ||
                vp.kind == VNODE_PROC_FD_DIR) {
                synth_dir_path(vp.kind, rpid, current_proc->cwd,
                               sizeof(current_proc->cwd));
                vfs_cwd_clear(current_proc);
                return 0;
            }
            return -ENOTDIR; /* a synthetic leaf */
        }
        node = ref.node;
        if (!mnt_is_root(ref.mnt)) {
            /* A directory on another type: its attributes decide, and the
             * canonical string is the request with `.` and `..` collapsed,
             * which the physical walk that just succeeded makes exact. */
            struct vfs_stat vst;

            rc = vfs_getattr(ref.mnt, node, &vst);
            if (rc == 0 && (vst.mode & 0170000u) != 0040000u) {
                rc = -ENOTDIR;
            }
            if (rc == 0) {
                rc = (int)cred_access_ref(&ref, CRED_X, 0);
            }
            if (rc != 0) {
                vfs_ref_release(&ref);
                return rc;
            }
            if (k_strlen(pathbuf) + 1 > sizeof(current_proc->cwd)) {
                vfs_ref_release(&ref);
                return -ENAMETOOLONG;
            }
            k_strncpy(current_proc->cwd, pathbuf, sizeof(current_proc->cwd));
            collapse_dots(current_proc->cwd);
            vfs_cwd_set(current_proc, ref.mnt, ref.node);
            vfs_ref_release(&ref);
            return 0;
        }
        /* The root node is always a directory even if its on-disk mode lacks
         * the S_IFDIR bit (it is the filesystem anchor, not a mkdir'd node). A
         * regular file as the final component is ENOTDIR. */
        if (node != krffs_fs->node && !KRFFS_IS_DIR(node->mode)) {
            vfs_ref_release(&ref);
            return -ENOTDIR;
        }
        rc = cred_access_node(node, CRED_X, 0);
        if (rc != 0) {
            vfs_ref_release(&ref);
            return rc; /* entering a directory is the search (x) right */
        }
        /* Store the canonical absolute path (krffs_node_path follows real
         * parent links, so any '.'/'..' in the request is collapsed). If it
         * does not fit the fixed cwd buffer the cwd is left unchanged. */
        if (krffs_node_path(node, current_proc->cwd,
                            sizeof(current_proc->cwd)) != 0) {
            vfs_ref_release(&ref);
            return -ENAMETOOLONG;
        }
        vfs_cwd_set(current_proc, ref.mnt, ref.node);
        vfs_ref_release(&ref);
    }
    return 0;
}

/* getcwd(2): EBX = buf, ECX = size. Returns the path length including the
 * NUL, or -errno. */
static int32_t sys_getcwd(isr_params_t *params)
{
    char __user *buf = (char __user *)params->ebx;
    uint32_t size = params->ecx;
    uint32_t cwd_len;

    if (!current_proc) {
        return -ESRCH;
    }
    if (!buf) {
        return -EFAULT;
    }
    if (size == 0) {
        return -EINVAL;
    }

    cwd_len = k_strlen(current_proc->cwd);
    if (cwd_len + 1 > size) {
        return -ERANGE;  /* buffer too small for the path + NUL */
    }

    if (copy_to_user(buf, current_proc->cwd, cwd_len + 1) != 0) {
        return -EFAULT;
    }

    /* Linux getcwd(2) returns the length including the NUL on success.
     * (Returning the buffer pointer would be misread as an errno for high user
     * addresses whose top bit is set.) */
    return (int32_t)(cwd_len + 1);
}

/*
 * Linux 32-bit stat structure (used by syscall 106)
 * This is the older structure used by ISH and some programs.
 */
struct linux_stat {
    unsigned long  st_dev;
    unsigned long  st_ino;
    unsigned short st_mode;
    unsigned short st_nlink;
    unsigned short st_uid;
    unsigned short st_gid;
    unsigned long  st_rdev;
    unsigned long  st_size;
    unsigned long  st_blksize;
    unsigned long  st_blocks;
    unsigned long  st_atime;
    unsigned long  st_atime_nsec;
    unsigned long  st_mtime;
    unsigned long  st_mtime_nsec;
    unsigned long  st_ctime;
    unsigned long  st_ctime_nsec;
    unsigned long  __unused4;
    unsigned long  __unused5;
};

/*
 * Linux stat64 structure for 32-bit systems
 */
struct linux_stat64 {
    unsigned long long st_dev;
    unsigned char      __pad0[4];
    unsigned long      __st_ino;
    unsigned int       st_mode;
    unsigned int       st_nlink;
    unsigned long      st_uid;
    unsigned long      st_gid;
    unsigned long long st_rdev;
    unsigned char      __pad3[4];
    long long          st_size;
    unsigned long      st_blksize;
    unsigned long long st_blocks;
    unsigned long      st_atime;
    unsigned long      st_atime_nsec;
    unsigned long      st_mtime;
    unsigned long      st_mtime_nsec;
    unsigned long      st_ctime;
    unsigned long      st_ctime_nsec;
    unsigned long long st_ino;
};

/* File type bits for st_mode */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000

/**
 * Report a node's three times into a stat64.
 *
 * KRFFS stores them; before this kernel had a clock nothing read them out, so
 * every file reported the epoch and no build tool could tell a source from the
 * object built from it. Access time is REPORTED but never advanced by a read
 * (see fs/krffs.c): the file system is a write-back page cache, and stamping an
 * atime on every read would dirty a page on every read.
 */
static void stat64_node_times(struct linux_stat64 *kst, const krffs_node_t *node)
{
    kst->st_atime = (unsigned long)node->access_time.seconds;
    kst->st_atime_nsec = (unsigned long)node->access_time.nanoseconds;
    kst->st_mtime = (unsigned long)node->modification_time.seconds;
    kst->st_mtime_nsec = (unsigned long)node->modification_time.nanoseconds;
    kst->st_ctime = (unsigned long)node->attribute_change_time.seconds;
    kst->st_ctime_nsec = (unsigned long)node->attribute_change_time.nanoseconds;
}

/**
 * Report the boot time into a stat64, for a node with no history of its own: a
 * device, a /proc file, a pipe, a socket.
 */
static void stat64_boot_times(struct linux_stat64 *kst)
{
    unsigned long boot = (unsigned long)rtc_boot_time();

    kst->st_atime = boot;
    kst->st_mtime = boot;
    kst->st_ctime = boot;
    kst->st_atime_nsec = 0;
    kst->st_mtime_nsec = 0;
    kst->st_ctime_nsec = 0;
}

/**
 * The root directory's own three times, into a stat64.
 *
 * The root is a real node and creating an entry in it stamps its times, so it
 * reports them like any other directory; a machine with no file system mounted
 * has nothing to report but the boot time.
 */
static void stat64_root_times(struct linux_stat64 *kst)
{
    if (krffs_fs && krffs_fs->node) {
        stat64_node_times(kst, krffs_fs->node);
    } else {
        stat64_boot_times(kst);
    }
}

/**
 * Fill a stat64 buffer from a resolved KRFFS node.
 *
 * Shared by stat64/fstat64/fstatat64. Reports S_IFDIR for directories and
 * S_IFREG for regular files; the node id is the inode number.
 */
/* A stat64 for a node on a mount that is not the root, from its getattr:
 * the mount's index is the device, so a program telling files apart by
 * (st_dev, st_ino) sees two mounts as two devices. */
static int32_t vfs_fill_stat64(struct linux_stat64 *kst, const struct vfs_ref *r)
{
    struct vfs_stat st;
    int rc = vfs_getattr(r->mnt, r->node, &st);
    int i;

    if (rc != 0) {
        return rc;
    }
    k_memset(kst, 0, sizeof(*kst));
    for (i = 0; i < VFS_MOUNTS; i++) {
        if (vfs_mount_at(i) == r->mnt) {
            kst->st_dev = (unsigned long long)(0x10u + (unsigned)i);
        }
    }
    kst->st_mode = st.mode;
    kst->st_nlink = st.nlink;
    kst->st_uid = st.uid;
    kst->st_gid = st.gid;
    kst->st_rdev = st.rdev;
    kst->st_size = (long long)st.size;
    kst->st_blksize = 4096;
    kst->st_blocks = (long long)((st.size + 511) / 512);
    kst->st_ino = st.ino;
    kst->__st_ino = (unsigned long)st.ino;
    kst->st_atime = (long)st.atime_sec;
    kst->st_atime_nsec = (unsigned long)st.atime_nsec;
    kst->st_mtime = (long)st.mtime_sec;
    kst->st_mtime_nsec = (unsigned long)st.mtime_nsec;
    kst->st_ctime = (long)st.ctime_sec;
    kst->st_ctime_nsec = (unsigned long)st.ctime_nsec;
    return 0;
}

static void krffs_fill_stat64(struct linux_stat64 *kst, krffs_node_t *node)
{
    k_memset(kst, 0, sizeof(*kst));
    if (node == krffs_fs->node) {
        /* The root is a directory even if its on-disk mode lacks S_IFDIR (it is
         * the filesystem anchor); a path can reach it via "..". */
        kst->st_mode = S_IFDIR | 0755;
    } else {
        kst->st_mode = (KRFFS_IS_DIR(node->mode) ? S_IFDIR : S_IFREG) |
                       (node->mode & 07777);
    }
    kst->st_nlink = 1; /* KRFFS has no hard links */
    kst->st_uid = node->uid;
    kst->st_gid = node->gid;
    kst->st_size = (long long)node->data_size;
    kst->st_blksize = 4096;
    kst->st_blocks = (node->data_size + 511) / 512;
    kst->st_ino = node->id;
    kst->__st_ino = (unsigned long)node->id;
    stat64_node_times(kst, node);
}

/* Fill a stat64 from an open descriptor (factored from sys_fstat64 so the magic
 * /proc/<pid>/fd/<n> symlinks can stat their target when followed). */
static int32_t fstat_fill(fd_entry_t *e, struct linux_stat64 *kst);

/*
 * Fill a stat64 for a synthetic (/dev or /proc) path. Devices report a
 * character device with their major/minor; /dev itself is a directory; the
 * /proc magic symlinks (/proc/self, /proc/<pid>/fd/<n>) report S_IFLNK when
 * NOT followed and their target when followed (`follow`). st_dev is the
 * synthetic fs id, distinct from KRFFS's 0, so a device's (st_dev, st_ino)
 * identity never aliases a file's.
 */
/* Stable /dev/pts inodes (distinct from devfs's): the directory, then one per
 * slave (so a slave's (st_dev, st_ino) identity is consistent for ttyname). */
#define PTS_INO_DIR  3u
#define PTS_INO_BASE 0x1000u

static int32_t synth_stat(const struct vpath *vp, struct linux_stat64 *kst, int follow)
{
    uint32_t mode = 0, rdev = 0, ino = 0;

    k_memset(kst, 0, sizeof(*kst));
    kst->st_nlink = 1;
    kst->st_blksize = 4096;
    stat64_boot_times(kst);

    switch (vp->kind) {
        case VNODE_DEV_DIR:
            (void)devfs_stat("", &mode, &rdev, &ino);
            kst->st_dev = DEVFS_FSID;
            kst->st_mode = mode;
            kst->st_ino = ino;
            return 0;
        case VNODE_DEV:
            if (devfs_stat(vp->name, &mode, &rdev, &ino) != 0) {
                return -ENOENT;
            }
            kst->st_dev = DEVFS_FSID;
            kst->st_mode = mode;
            kst->st_rdev = rdev;
            kst->st_ino = ino;
            return 0;
        case VNODE_PROC_DIR:
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = S_IFDIR | 0555;
            kst->st_ino = procfs_ino_dir();
            return 0;
        case VNODE_PROC_CMDLINE:
            /* /proc/cmdline: the boot command line, a read-only regular
             * file. */
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = S_IFREG | 0444;
            kst->st_ino = procfs_ino_cmdline();
            return 0;
        case VNODE_PROC_NET_DIR:
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = S_IFDIR | 0555;
            kst->st_ino = procfs_ino_netdir();
            return 0;
        case VNODE_PROC_NET_DEV:
            /* /proc/net/dev: what the interface has carried, anyone's to
             * read. */
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = S_IFREG | 0444;
            kst->st_ino = procfs_ino_netdev();
            return 0;
        case VNODE_PROC_UPTIME:
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = S_IFREG | 0444;
            kst->st_ino = procfs_ino_uptime();
            return 0;
        case VNODE_PROC_STAT_SYS:
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = S_IFREG | 0444;
            kst->st_ino = procfs_ino_statsys();
            return 0;
        case VNODE_PROC_MEMINFO:
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = S_IFREG | 0444;
            kst->st_ino = procfs_ino_meminfo();
            return 0;
        case VNODE_PROC_MOUNTS:
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = S_IFREG | 0444;
            kst->st_ino = procfs_ino_mounts();
            return 0;
        case VNODE_PROC_USB:
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = S_IFREG | 0444;
            kst->st_ino = procfs_ino_usb();
            return 0;
        case VNODE_PROC_FILESYSTEMS:
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = S_IFREG | 0444;
            kst->st_ino = procfs_ino_filesystems();
            return 0;
        case VNODE_PROC_NET_ROUTE:
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = S_IFREG | 0444;
            kst->st_ino = procfs_ino_netroute();
            return 0;
        case VNODE_PROC_NET_ARP:
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = S_IFREG | 0444;
            kst->st_ino = procfs_ino_netarp();
            return 0;
        case VNODE_PROC_NET_TCP:
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = S_IFREG | 0444;
            kst->st_ino = procfs_ino_nettcp();
            return 0;
        case VNODE_PROC_DISK:
            /* /proc/disk: what the disk reported, root-readable only. */
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = S_IFREG | 0400;
            kst->st_ino = procfs_ino_disk();
            return 0;
        case VNODE_PROC_PID:
        case VNODE_PROC_FD_DIR:
            if (procfs_resolve_pid(vp->pid) < 0) {
                return -ENOENT;
            }
            /* /proc/self is a symlink: an un-followed stat (lstat) sees the
             * link, a followed one resolves to the pid directory.
             * /proc/<numeric> and /proc/<pid>/fd are real directories. */
            if (vp->kind == VNODE_PROC_PID && vp->pid == SYNTH_PID_SELF && !follow) {
                kst->st_dev = PROCFS_FSID;
                kst->st_mode = 0120777; /* S_IFLNK | 0777 */
                kst->st_ino = procfs_ino_dir();
                return 0;
            }
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = S_IFDIR | 0555;
            kst->st_ino = procfs_ino_pid(procfs_resolve_pid(vp->pid));
            return 0;
        case VNODE_PROC_FILE:
            if (procfs_resolve_pid(vp->pid) < 0) {
                return -ENOENT;
            }
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = S_IFREG | 0444;
            kst->st_ino = procfs_ino_pid(procfs_resolve_pid(vp->pid));
            return 0;
        case VNODE_PROC_FD_LINK: {
            char scratch[16];
            int rpid = procfs_resolve_pid(vp->pid);
            if (rpid < 0) {
                return -ENOENT;
            }
            /* The link exists only if descriptor procfd is actually open (the
             * same check readlink makes), so stat and readlink agree. */
            if (procfs_readlink_fd(rpid, vp->procfd, scratch, (int)sizeof(scratch)) < 0) {
                return -ENOENT;
            }
            if (follow) {
                /* A followed stat reports the target descriptor's identity. */
                process_t *p = get_process((uint32_t)rpid);
                if (p && vp->procfd >= 0 && vp->procfd < MAX_FDS &&
                    p->fds.entries[vp->procfd].type != FD_TYPE_NONE) {
                    fstat_fill(&p->fds.entries[vp->procfd], kst);
                    return 0;
                }
                return -ENOENT;
            }
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = 0120777; /* S_IFLNK | 0777 */
            kst->st_ino = procfs_ino_pid(rpid);
            return 0;
        }
        case VNODE_PTS_DIR:
            /* /dev/pts: a directory. */
            kst->st_dev = DEVFS_FSID;
            kst->st_mode = S_IFDIR | 0755;
            kst->st_ino = PTS_INO_DIR;
            return 0;
        case VNODE_PTS: {
            /* /dev/pts/N: a slave terminal (S_IFCHR), only if pty N is
             * allocated. The rdev is the Linux Unix98 pty slave major 136.
             * Ownership lives on the pty, where the login server's chown/chmod
             * put it. */
            struct pty *pt = pty_get(vp->pts_index);
            if (!pt) {
                return -ENOENT;
            }
            kst->st_dev = DEVFS_FSID;
            kst->st_mode = 0020000u | (uint32_t)(pt->mode & 0777u);
            kst->st_uid = pt->uid;
            kst->st_gid = pt->gid;
            kst->st_rdev = (136u << 8) | (uint32_t)vp->pts_index;
            kst->st_ino = PTS_INO_BASE + (uint32_t)vp->pts_index;
            return 0;
        }
        case VNODE_ENOTDIR:
            return -ENOTDIR;
        default:
            return -ENOENT;
    }
}

/**
 * readlink core (readlink(2)/readlinkat(2)).
 *
 * The only symlinks Rach2 has are /proc/self and the /proc/<pid>/fd/<n>
 * magic links (which ttyname reads). The result is the byte count and is NOT
 * NUL-terminated (the readlink contract; musl appends its own terminator and
 * treats a full buffer as truncation). `name` is the path, leading '/'
 * stripped.
 */
static int32_t do_readlink(const char *name, char __user *ubuf, uint32_t bufsiz)
{
    struct vpath vp;
    struct vfs_ref ref;
    const struct vpath *svp;
    char link[256]; /* fit a full fd-target path (the syscall path limit) */
    int n, pid, kind;

    if (!ubuf || bufsiz == 0) {
        return -EINVAL;
    }
    kind = (int)vfs_resolve(0, name, 0, &ref, 0, 0);
    if (kind != 0) {
        return kind;
    }
    svp = vfs_synth_vpath(ref.mnt, ref.node);
    if (!svp) {
        vfs_ref_release(&ref);
        return -EINVAL; /* nothing outside /proc is a link */
    }
    vp = *svp;
    vfs_ref_release(&ref);
    kind = vp.kind;

    if (kind == VNODE_PROC_PID && vp.pid == SYNTH_PID_SELF) {
        /* /proc/self is a symlink to the caller's numeric pid. */
        uint32_t uv;
        char t[12];
        int tn = 0;
        pid = procfs_resolve_pid(vp.pid);
        if (pid < 0) {
            return -ENOENT;
        }
        n = 0;
        uv = (uint32_t)pid;
        if (uv == 0) {
            link[n++] = '0';
        } else {
            while (uv) {
                t[tn++] = (char)('0' + uv % 10u);
                uv /= 10u;
            }
            while (tn) {
                link[n++] = t[--tn];
            }
        }
    } else if (kind == VNODE_PROC_FD_LINK) {
        pid = procfs_resolve_pid(vp.pid);
        if (pid < 0) {
            return -ENOENT;
        }
        n = procfs_readlink_fd(pid, vp.procfd, link, (int)sizeof(link));
        if (n < 0) {
            return n;
        }
    } else {
        return -EINVAL; /* not a symlink */
    }

    if ((uint32_t)n > bufsiz) {
        n = (int)bufsiz; /* truncate to the caller's buffer */
    }
    if (copy_to_user(ubuf, link, (uint32_t)n) != 0) {
        return -EFAULT;
    }
    return n;
}

static int32_t sys_readlink(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
    char __user *ubuf = (char __user *)params->ecx;
    uint32_t bufsiz = params->edx;
    char pathbuf[256];
    const char *name;
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    len = copy_path_cwd(pathbuf, sizeof(pathbuf), upath);
    if (len < 0) {
        return len;
    }
    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }
    return do_readlink(name, ubuf, bufsiz);
}

static int32_t sys_readlinkat(isr_params_t *params)
{
    int dirfd = (int)params->ebx;
    const char __user *upath = (const char __user *)params->ecx;
    char __user *ubuf = (char __user *)params->edx;
    uint32_t bufsiz = params->esi;
    char pathbuf[256];
    const char *name;
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    len = copy_path_at(pathbuf, sizeof(pathbuf), dirfd, upath);
    if (len < 0) {
        return len;
    }
    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }
    return do_readlink(name, ubuf, bufsiz);
}

static int32_t do_stat64(const char __user *path, struct linux_stat64 __user *st, int follow)
{
    struct linux_stat64 kst;
    char pathbuf[256];
    int len;

    if (!current_proc || !path || !st) {
        return -EFAULT;
    }

    len = copy_path_cwd(pathbuf, sizeof(pathbuf), path);
    if (len < 0) {
        return len;
    }

    /* Build the result in a kernel struct, then copy it out in one shot. */
    k_memset(&kst, 0, sizeof(kst));

    {
        struct vfs_ref ref;
        const struct vpath *vp;
        int rc = vfs_resolve(0, pathbuf, 0, &ref, 0, 0);

        if (rc != 0) {
            return rc; /* missing, not-a-dir, or a denied ancestor */
        }
        vp = vfs_synth_vpath(ref.mnt, ref.node);
        if (vp) {
            int32_t src = synth_stat(vp, &kst, follow);

            vfs_ref_release(&ref);
            if (src != 0) {
                return src;
            }
        } else if (!mnt_is_root(ref.mnt)) {
            rc = vfs_fill_stat64(&kst, &ref);
            vfs_ref_release(&ref);
            if (rc != 0) {
                return rc;
            }
        } else if ((krffs_node_t *)ref.node == krffs_fs->node) {
            /* The root's identity is fixed: ino 0, 0755, and real times: an
             * entry created in it stamps them, which is how a program
             * watching a directory sees it. */
            kst.st_mode = S_IFDIR | 0755;
            kst.st_nlink = 1;
            kst.st_ino = 0;
            kst.st_blksize = 4096;
            stat64_root_times(&kst);
            vfs_ref_release(&ref);
        } else {
            krffs_fill_stat64(&kst, (krffs_node_t *)ref.node);
            vfs_ref_release(&ref);
        }
    }

    if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
        return -EFAULT;
    }

    return 0;
}

/* stat64(195) follows symlinks; lstat64(196) does not. On i386 musl maps its
 * stat()/lstat() onto these (via the statx -> *stat64 fallback), so lstat()
 * only works once 196 exists. The follow flag matters only for the /proc magic
 * links. */
static int32_t sys_stat64(isr_params_t *params)
{
    return do_stat64((const char __user *)params->ebx,
                     (struct linux_stat64 __user *)params->ecx, 1);
}

static int32_t sys_lstat64(isr_params_t *params)
{
    return do_stat64((const char __user *)params->ebx,
                     (struct linux_stat64 __user *)params->ecx, 0);
}

/*
 * The i386 struct statfs64, as Linux writes it and musl reads it.
 *
 * `f_bsize` is the unit `f_blocks` and `f_bfree` are counted in. KRFFS has no
 * block structure at all, so the unit is chosen rather than found: one byte
 * would overflow the 64-bit counts on no disk this machine can hold, and 1024
 * is what `df` prints in by default, so the numbers a person reads are the
 * numbers the kernel holds divided by nothing.
 */
struct linux_statfs64 {
    uint32_t f_type;
    uint32_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    uint32_t f_fsid[2];
    uint32_t f_namelen;
    uint32_t f_frsize;
    uint32_t f_flags;
    uint32_t f_spare[4];
};

#define STATFS_BSIZE 1024u

/* The three mounts /proc/mounts names, and the magic each reports. The values
 * are Linux's for the two synthetic ones, so a program that recognises them
 * finds what it expects; KRFFS has no assigned magic and uses its own name. */
#define STATFS_MAGIC_KRFFS 0x4B524646u /* "KRFF" */
#define STATFS_MAGIC_PROC  0x9FA0u
#define STATFS_MAGIC_DEVFS 0x1373u

/**
 * Fill a statfs64 for one of the three file systems this machine mounts.
 *
 * Only the root has blocks: it is KRFFS, and the region's size and its free
 * nodes' usable bytes are both real, reported in 1024-byte units because that
 * is what `df` prints in by default. The two synthetic mounts report their own
 * magic and no blocks at all, which is what they have and what Linux reports
 * for them; answering with the root's numbers would tell `df` that `/proc` is
 * a 16 MB file system 77 percent full. `f_files` is zero throughout, which says
 * "not counted" rather than claiming an inode budget this format does not keep.
 */
static int32_t statfs_fill(struct linux_statfs64 __user *out, uint32_t magic)
{
    struct linux_statfs64 st;
    uint32_t total = krffs_total_bytes();
    uint32_t free_bytes = krffs_free_bytes();
    int i;

    k_memset(&st, 0, sizeof(st));
    st.f_type = magic;
    st.f_bsize = STATFS_BSIZE;
    st.f_frsize = STATFS_BSIZE;
    if (magic == STATFS_MAGIC_KRFFS) {
        st.f_blocks = total / STATFS_BSIZE;
        st.f_bfree = free_bytes / STATFS_BSIZE;
        st.f_bavail = st.f_bfree;
    }
    st.f_files = 0;
    st.f_ffree = 0;
    st.f_namelen = KRFFS_FILE_NAME_BUFFER_SIZE - 1;
    for (i = 0; i < 4; i++) {
        st.f_spare[i] = 0;
    }
    if (copy_to_user(out, &st, sizeof(st)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** A statfs64 from a mount's own statfs: the numbers vfat reports. */
static int32_t statfs_fill_mount(struct linux_statfs64 __user *out, struct vfs_mount *m)
{
    struct linux_statfs64 st;
    struct vfs_statfs vs;
    int rc = vfs_statfs(m, &vs);

    if (rc != 0) {
        return rc;
    }
    k_memset(&st, 0, sizeof(st));
    st.f_type = vs.magic;
    st.f_bsize = vs.bsize;
    st.f_frsize = vs.bsize;
    st.f_blocks = vs.blocks;
    st.f_bfree = vs.bfree;
    st.f_bavail = vs.bfree;
    st.f_namelen = vs.namelen;
    if (copy_to_user(out, &st, sizeof(st)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** Which of the three mounts a classified path belongs to. */
static uint32_t statfs_magic_for_kind(int kind)
{
    if (kind == VNODE_DEV_DIR || kind == VNODE_DEV || kind == VNODE_PTS_DIR ||
        kind == VNODE_PTS) {
        return STATFS_MAGIC_DEVFS;
    }
    if (kind == VNODE_KRFFS) {
        return STATFS_MAGIC_KRFFS;
    }
    return STATFS_MAGIC_PROC;
}

/**
 * statfs64(2): the file system a path is on.
 *
 * EBX is the path, ECX the caller's sizeof(struct statfs64), EDX the buffer.
 * The size is checked rather than ignored: a caller built against a different
 * layout would otherwise be handed bytes it would read as different fields.
 */
static int32_t sys_statfs64(isr_params_t *params)
{
    char pathbuf[256];
    int len;

    if (!current_proc || !params->ebx || !params->edx) {
        return -EFAULT;
    }
    if (params->ecx != sizeof(struct linux_statfs64)) {
        return -EINVAL;
    }
    len = copy_path_cwd(pathbuf, sizeof(pathbuf), (const char __user *)params->ebx);
    if (len < 0) {
        return len;
    }
    /* The path has to name something. Answering for a path that does not exist
     * would report a file system where there is no file, which is the one thing
     * a caller uses this call to find out. */
    {
        struct vfs_ref ref;
        const struct vpath *svp;
        uint32_t magic = STATFS_MAGIC_KRFFS;
        int rc = vfs_resolve(0, pathbuf, 0, &ref, 0, 0);

        if (rc != 0) {
            return rc;
        }
        svp = vfs_synth_vpath(ref.mnt, ref.node);
        if (svp) {
            magic = statfs_magic_for_kind(svp->kind);
        } else if (!mnt_is_root(ref.mnt)) {
            struct vfs_mount *m = ref.mnt;

            vfs_ref_release(&ref);
            return statfs_fill_mount((struct linux_statfs64 __user *)params->edx, m);
        }
        vfs_ref_release(&ref);
        return statfs_fill((struct linux_statfs64 __user *)params->edx,
                           magic);
    }
}

/**
 * fstatfs64(2): the same numbers, named by an open descriptor.
 *
 * EBX is the descriptor, ECX the caller's size, EDX the buffer.
 */
static int32_t sys_fstatfs64(isr_params_t *params)
{
    fd_entry_t *e;

    if (params->ecx != sizeof(struct linux_statfs64)) {
        return -EINVAL;
    }
    if (!current_proc || !params->edx) {
        return -EFAULT;
    }
    e = fd_get(&current_proc->fds, (int)params->ebx);
    if (!e) {
        return -EBADF;
    }
    /* The descriptor's own kind decides which mount answers. A socket or a pipe
     * is on no file system at all, and Linux answers `-ENOSYS` for those rather
     * than handing back the root's numbers. */
    if (e->type == FD_TYPE_PROC) {
        return statfs_fill((struct linux_statfs64 __user *)params->edx, STATFS_MAGIC_PROC);
    }
    if (e->type == FD_TYPE_DEVICE || e->type == FD_TYPE_CONSOLE ||
        e->type == FD_TYPE_PTM || e->type == FD_TYPE_SYNTH_DIR) {
        return statfs_fill((struct linux_statfs64 __user *)params->edx, STATFS_MAGIC_DEVFS);
    }
    if (e->type != FD_TYPE_FILE && e->type != FD_TYPE_DIR) {
        return -ENOSYS;
    }
    if (e->ofd && e->ofd->vmnt && !mnt_is_root(e->ofd->vmnt)) {
        return statfs_fill_mount((struct linux_statfs64 __user *)params->edx, e->ofd->vmnt);
    }
    return statfs_fill((struct linux_statfs64 __user *)params->edx, STATFS_MAGIC_KRFFS);
}

/**
 * Fill a stat64 from an open descriptor by type.
 *
 * Shared by sys_fstat64 and the followed /proc/<pid>/fd/<n> path (whose
 * target is just this fd). Where the object has an identity of its own (a
 * socket, a pipe, a device, a /proc snapshot, a KRFFS node), the inode
 * reported here is that identity and is the same number the descriptor's
 * `/proc/<pid>/fd` link carries, so a program reading one can match the other.
 */
static int32_t fstat_fill(fd_entry_t *entry, struct linux_stat64 *kst)
{
    k_memset(kst, 0, sizeof(*kst));
    stat64_boot_times(kst); /* a node with times of its own overwrites these */
    if ((entry->type == FD_TYPE_FILE || entry->type == FD_TYPE_DIR) && entry->ofd &&
        entry->ofd->vmnt && !mnt_is_root(entry->ofd->vmnt)) {
        struct vfs_ref r;

        r.mnt = entry->ofd->vmnt;
        r.node = entry->ofd->vnode;
        return vfs_fill_stat64(kst, &r);
    }
    if (entry->type == FD_TYPE_DIR && entry->node == NULL) {
        kst->st_mode = S_IFDIR | 0755; /* the root directory */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        /* The root has times of its own, so a descriptor on it reports what a
         * path to it reports; the boot-time default above is for the nodes that
         * have no history, and the root is not one of them. */
        stat64_root_times(kst);
    } else if (entry->node) {
        /* A stale fd (its node was unlinked or relocated) would leak freed
         * free-list bytes through the stat fields; leave the zeroed stat rather
         * than read a node that is no longer live. */
        if (fd_node_check(entry) == 0) {
            krffs_fill_stat64(kst, entry->node);
        }
    } else if (entry->type == FD_TYPE_SOCKET) {
        /* A socket fd: programs classify these with S_ISSOCK, and the inode is
         * the socket's own, the same number /proc/net/tcp and the fd symlink
         * report, so a program can match a row to a descriptor. */
        kst->st_mode = 0140666; /* S_IFSOCK | 0666 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_ino = entry->sock ? entry->sock->inode : 0u;
    } else if (entry->type == FD_TYPE_PIPE) {
        /* A pipe end: S_ISFIFO, and the pipe's own inode (see fd.h). */
        pipe_t *pp = (pipe_t *)entry->priv;
        kst->st_mode = 010666; /* S_IFIFO | 0666 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_ino = pp ? pp->inode : 0u;
    } else if (entry->type == FD_TYPE_DEVICE) {
        /* A devfs character device: report its identity so fstat matches the
         * stat of its /dev path (what ttyname compares), the mode included. */
        kst->st_mode = devfs_fd_mode(entry);
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_dev = DEVFS_FSID;
        kst->st_rdev = devfs_rdev(entry);
        kst->st_ino = devfs_fd_ino(entry);
    } else if (entry->type == FD_TYPE_PROC) {
        /* A /proc text snapshot: a regular file whose inode is its origin's,
         * so fstat matches the stat of the path it was opened by. */
        kst->st_mode = 0100444; /* S_IFREG | 0444 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_dev = PROCFS_FSID;
        kst->st_ino = procfs_fd_ino(entry);
    } else if (entry->type == FD_TYPE_CONSOLE) {
        /* stdin/stdout/stderr and /dev/console: report the same identity as
         * stat("/dev/console") so ttyname's (st_dev, st_ino) comparison
         * matches. */
        uint32_t cmode = 0, crdev = 0, cino = 0;
        (void)devfs_stat("console", &cmode, &crdev, &cino);
        kst->st_mode = 020666; /* S_IFCHR | 0666 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_dev = DEVFS_FSID;
        kst->st_rdev = crdev;
        kst->st_ino = cino;
    } else if (entry->type == FD_TYPE_SYNTH_DIR) {
        /* A /dev or /proc directory fd: report the SAME identity its path stat
         * would (so fstat and stat agree on the directory), reconstructed from
         * the fd's pooled kind/pid. */
        struct vpath vp;
        struct synth_dir *sd = (struct synth_dir *)entry->priv;
        k_memset(&vp, 0, sizeof(vp));
        vp.kind = sd ? sd->kind : VNODE_ENOENT;
        vp.pid = sd ? sd->pid : 0;
        if (synth_stat(&vp, kst, 1) != 0) {
            kst->st_mode = S_IFDIR | 0555; /* the process exited: a bare directory */
            kst->st_nlink = 1;
            kst->st_blksize = 4096;
            kst->st_dev = PROCFS_FSID;
        }
    } else if (entry->type == FD_TYPE_PTS) {
        /* A pty slave: report the SAME identity as stat("/dev/pts/N") so
         * ttyname's (st_dev, st_ino) comparison matches the /proc/self/fd
         * readlink. */
        struct vpath vp;
        k_memset(&vp, 0, sizeof(vp));
        vp.kind = VNODE_PTS;
        vp.pts_index = pty_slave_index(entry);
        if (synth_stat(&vp, kst, 1) != 0) {
            kst->st_mode = 020620; /* S_IFCHR | 0620 (master hung up) */
            kst->st_nlink = 1;
            kst->st_blksize = 4096;
            kst->st_dev = DEVFS_FSID;
        }
    } else if (entry->type == FD_TYPE_PTM) {
        /* A pty master (/dev/ptmx): a character device. */
        kst->st_mode = 020666; /* S_IFCHR | 0666 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
    } else {
        /* A framebuffer/mouse fd: report a character device, size 0. */
        kst->st_mode = 020666; /* S_IFCHR | 0666 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
    }
    return 0;
}

/*
 * fstat64 (syscall 197): EBX = fd, ECX = stat64 buffer. The descriptor's
 * node is stat'd; a root directory fd (node == NULL) reports the root
 * directory.
 */
static int32_t sys_fstat64(isr_params_t *params)
{
    int32_t rc;
    int fd = (int)params->ebx;
    struct linux_stat64 __user *st = (struct linux_stat64 __user *)params->ecx;
    struct linux_stat64 kst;
    fd_entry_t *entry;

    if (!current_proc || !st) {
        return -EFAULT;
    }
    entry = fd_get(&current_proc->fds, fd);
    if (!entry) {
        return -EBADF;
    }
    rc = fstat_fill(entry, &kst);
    if (rc != 0) {
        return rc;
    }
    if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/*
 * fstatat64 (syscall 300): EBX = dirfd, ECX = path, EDX = stat64 buffer, ESI
 * = flags. The path resolves the same way as openat (absolute, or relative
 * to AT_FDCWD/the dirfd; see copy_path_at). AT_SYMLINK_NOFOLLOW is honored
 * for the /proc magic symlinks; KRFFS has none.
 */
static int32_t sys_fstatat64(isr_params_t *params)
{
    int dirfd = (int)params->ebx;
    const char __user *upath = (const char __user *)params->ecx;
    struct linux_stat64 __user *st = (struct linux_stat64 __user *)params->edx;
    struct linux_stat64 kst;
    char pathbuf[256];
    krffs_node_t *node;
    int len;

    if (!current_proc || !upath || !st) {
        return -EFAULT;
    }
    len = copy_path_at(pathbuf, sizeof(pathbuf), dirfd, upath);
    if (len < 0) {
        return len;
    }

    k_memset(&kst, 0, sizeof(kst));
    if (pathbuf[0] == '/' && pathbuf[1] == '\0') {
        kst.st_mode = S_IFDIR | 0755; /* the root directory */
        kst.st_nlink = 1;
        kst.st_blksize = 4096;
        stat64_root_times(&kst);
    } else {
        struct vfs_ref ref;
        const struct vpath *svp;
        int32_t rc = vfs_resolve(0, pathbuf, 0, &ref, 0, 0);

        if (rc != 0) {
            return rc;
        }
        svp = vfs_synth_vpath(ref.mnt, ref.node);
        if (svp) {
            /* AT_SYMLINK_NOFOLLOW (lstat) reports the magic symlinks;
             * otherwise they are followed. */
            int follow = !((int)params->esi & AT_SYMLINK_NOFOLLOW);

            rc = synth_stat(svp, &kst, follow);
            vfs_ref_release(&ref);
            if (rc != 0) {
                return rc;
            }
        } else if (!mnt_is_root(ref.mnt)) {
            rc = vfs_fill_stat64(&kst, &ref);
            vfs_ref_release(&ref);
            if (rc != 0) {
                return rc;
            }
        } else {
            node = ref.node;
            krffs_fill_stat64(&kst, node);
            vfs_ref_release(&ref);
        }
    }

    if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/* access(2) mode bits. */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

/**
 * access core: does the path exist and grant the requested mode?
 *
 * R/W/X are checked against the object's mode bits by the caller's REAL ids
 * (the credential model access(2) uniquely uses), which is what makes
 * /proc/<pid>/stat reject W_OK while granting F_OK/R_OK. `name` is absolute
 * with the leading '/' stripped.
 */
static int32_t do_access(const char *name, int mode)
{
    struct vfs_ref ref;
    const struct vpath *vp;
    int32_t rc;
    int werr;

    if (mode & ~(R_OK | W_OK | X_OK)) {
        return -EINVAL; /* undefined access-mode bits */
    }
    /* access(2) is the one caller judged by the REAL ids: it exists so a
     * set-id program can ask what its invoker could do. The walk charges
     * search the same way, so a set-id program cannot report reachability
     * its caller does not have. */
    werr = vfs_resolve(0, name, VFS_WALK_REAL_IDS, &ref, 0, 0);
    if (werr != 0) {
        return werr;
    }
    vp = vfs_synth_vpath(ref.mnt, ref.node);
    if (vp) {
        struct linux_stat64 s;

        rc = synth_stat(vp, &s, 1); /* access follows symlinks */
        vfs_ref_release(&ref);
        if (rc != 0) {
            return rc; /* -ENOENT / -ENOTDIR */
        }
        /* Synthetic metadata is fixed and root-owned; judge it by the real
         * ids like any other access(2) answer. */
        return cred_access_bits(s.st_uid, s.st_gid, s.st_mode & 0777u,
                                mode, 1);
    }
    /* R_OK/W_OK/X_OK are the same octal digits the checker takes. */
    if (!mnt_is_root(ref.mnt)) {
        rc = cred_access_ref(&ref, mode, 1);
        if (rc == 0 && (mode & W_OK) && (ref.mnt->flags & VFS_MS_RDONLY)) {
            rc = -EROFS; /* access(2) reports the mount's refusal too */
        }
    } else {
        rc = cred_access_node((krffs_node_t *)ref.node, mode, 1);
    }
    vfs_ref_release(&ref);
    return rc;
}

static int32_t sys_access(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
    int mode = (int)params->ecx;
    char pathbuf[256];
    const char *name;
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    len = copy_path_cwd(pathbuf, sizeof(pathbuf), upath);
    if (len < 0) {
        return len;
    }
    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }
    return do_access(name, mode);
}

static int32_t sys_faccessat(isr_params_t *params)
{
    int dirfd = (int)params->ebx;
    const char __user *upath = (const char __user *)params->ecx;
    int mode = (int)params->edx;
    char pathbuf[256];
    const char *name;
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    len = copy_path_at(pathbuf, sizeof(pathbuf), dirfd, upath);
    if (len < 0) {
        return len;
    }
    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }
    return do_access(name, mode);
}

/**
 * The chmod rule over a resolved KRFFS node: the owner or root replaces the
 * permission bits; anyone else is refused. The root node's mode is never
 * touched (its spare bits carry the dirty-mount flag), but the rule still
 * answers, so a non-owner probing `/` gets the correct -EPERM.
 */
static int32_t krffs_chmod_node(krffs_node_t *node, uint32_t mode)
{
    if (!cred_privileged() && current_proc->euid != node->uid) {
        return -EPERM;
    }
    if (node != krffs_fs->node) {
        uint32_t want = mode & 07777u;

        /* chmod(2)'s own rule: an unprivileged caller storing a mode on a file
         * whose group is not one of theirs does not get to keep setgid. This is
         * the third place POSIX names a set-id clear, after write and chown,
         * and it is the one no write-clear can stand in for: an owner may mint
         * 02710 on a gid-0 file they own by chmod alone, with no write ever
         * running. Deliberately NOT conditioned on group-execute the way
         * cred_kill_setid is, because the caller is choosing that bit in this
         * same call, so reading it back as a reason to spare the mode would
         * defeat the rule. Setuid needs no rule here: only the owner may chmod,
         * and the setuid grant is that owner's own identity. */
        if (!cred_privileged() && !KRFFS_IS_DIR(node->mode) &&
            !cred_in_group(node->gid)) {
            want &= ~(uint32_t) KRFFS_MODE_SGID;
        }
        node->mode = (uint16_t) ((node->mode & ~07777u) | want);
        krffs_stamp(node, KRFFS_STAMP_ATTR);
    }
    return 0;
}

/**
 * The chown rule over a resolved KRFFS node, the POSIX split: changing the
 * owner takes root; changing the group takes root, or being the owner and
 * naming one of your own groups. `(id_t)-1` keeps a field, and a no-change
 * value needs no right at all, so `chown(path, -1, -1)` always succeeds on an
 * existing file.
 */
static int32_t krffs_chown_node(krffs_node_t *node, uint32_t uid, uint32_t gid)
{
    int uid_changes = (uid != (uint32_t) -1 && uid != node->uid);
    int gid_changes = (gid != (uint32_t) -1 && gid != node->gid);

    if (uid_changes && !cred_privileged()) {
        return -EPERM;
    }
    if (gid_changes && !cred_privileged() &&
        (current_proc->euid != node->uid || !cred_in_group(gid))) {
        return -EPERM;
    }
    if (uid != (uint32_t) -1) {
        node->uid = uid;
    }
    if (gid != (uint32_t) -1) {
        node->gid = gid;
    }
    /* Stamped once, after both fields, and on the same gate the set-id clear
     * below uses: a chown that changed an owner changed the node's attributes,
     * whichever of the two it was, and chown(path, -1, -1) changed nothing and
     * must leave the times alone. Stamping inside the uid branch, which is
     * where this started, left `chown(path, -1, gid)` unstamped. */
    /* The set-id clear is not gated on privilege: POSIX drops the bits whoever
     * asks, which is why cp and install chown before they chmod. Both are gated
     * on a field ACTUALLY having changed, and that gate has real weight:
     * chown(path, -1, -1) succeeds for every caller by design, since a value
     * that changes nothing needs no right, so a clear that ran on it would let
     * any uid disarm /bin/su with one call. */
    if (uid_changes || gid_changes) {
        krffs_stamp(node, KRFFS_STAMP_ATTR);
        cred_kill_setid(node);
    }
    return 0;
}


/**
 * chmod core for the chmod/chown family.
 *
 * A KRFFS path stores through `krffs_chmod_node` under the owner-or-root rule.
 * A pty slave (`/dev/pts/N`) stores on the pty, the state dropbear moves
 * before handing the terminal to the user. Every other synthetic /dev or
 * /proc object has fixed root-owned metadata: a no-change request validates
 * and succeeds, a change is refused. `name` is absolute with the leading '/'
 * stripped, matching do_access.
 */
static int32_t do_chmod(const char *name, uint32_t mode)
{
    struct vpath vp;
    const struct vpath *svp;
    struct vfs_ref ref;
    krffs_node_t *node;
    int32_t rc;

    rc = vfs_resolve(0, name, 0, &ref, 0, 0);
    if (rc != 0) {
        return rc;
    }
    svp = vfs_synth_vpath(ref.mnt, ref.node);
    if (svp) {
        vp = *svp;
        vfs_ref_release(&ref);
        if (vp.kind == VNODE_PTS) {
            struct pty *pt = pty_get(vp.pts_index);
            if (!pt) {
                return -ENOENT;
            }
            if (!cred_privileged() && current_proc->euid != pt->uid) {
                return -EPERM;
            }
            pt->mode = (uint16_t) (mode & 0777u);
            return 0;
        }
        return cred_privileged() ? 0 : -EPERM; /* fixed root-owned metadata */
    }
    if (!mnt_is_root(ref.mnt)) {
        struct vfs_stat vst;

        rc = vfs_getattr(ref.mnt, ref.node, &vst);
        if (rc == 0 && !cred_privileged() && current_proc->euid != vst.uid) {
            rc = -EPERM;
        }
        if (rc == 0) {
            if (!cred_privileged() && !cred_in_group(vst.gid)) {
                mode &= ~02000u; /* set-gid on a group not one's own is dropped */
            }
            vst.mode = mode;
            rc = vfs_setattr(ref.mnt, ref.node, VFS_SET_MODE, &vst);
        }
        vfs_ref_release(&ref);
        return rc;
    }
    node = ref.node;
    rc = krffs_chmod_node(node, mode);
    vfs_ref_release(&ref);
    return rc;
}

static int32_t do_chown(const char *name, uint32_t uid, uint32_t gid)
{
    struct vpath vp;
    const struct vpath *svp;
    struct vfs_ref ref;
    krffs_node_t *node;
    int32_t rc;

    rc = vfs_resolve(0, name, 0, &ref, 0, 0);
    if (rc != 0) {
        return rc;
    }
    svp = vfs_synth_vpath(ref.mnt, ref.node);
    if (svp) {
        vp = *svp;
        vfs_ref_release(&ref);
        if (vp.kind == VNODE_PTS) {
            struct pty *pt = pty_get(vp.pts_index);
            int uid_changes;
            int gid_changes;
            if (!pt) {
                return -ENOENT;
            }
            uid_changes = (uid != (uint32_t) -1 && uid != pt->uid);
            gid_changes = (gid != (uint32_t) -1 && gid != pt->gid);
            if ((uid_changes || gid_changes) && !cred_privileged()) {
                return -EPERM; /* the login server that chowns a pty is root */
            }
            if (uid != (uint32_t) -1) {
                pt->uid = uid;
            }
            if (gid != (uint32_t) -1) {
                pt->gid = gid;
            }
            return 0;
        }
        /* Fixed root-owned metadata: a no-change request succeeds. */
        if ((uid != (uint32_t) -1 && uid != 0) ||
            (gid != (uint32_t) -1 && gid != 0)) {
            return -EPERM;
        }
        return 0;
    }
    if (!mnt_is_root(ref.mnt)) {
        struct vfs_stat vst;
        int what = 0;

        rc = vfs_getattr(ref.mnt, ref.node, &vst);
        if (rc == 0 && !cred_privileged()) {
            if (current_proc->euid != vst.uid ||
                (uid != (uint32_t)-1 && uid != vst.uid) ||
                (gid != (uint32_t)-1 && gid != vst.gid && !cred_in_group(gid))) {
                rc = -EPERM;
            }
        }
        if (rc == 0) {
            if (uid != (uint32_t)-1 && uid != vst.uid) {
                vst.uid = uid;
                what |= VFS_SET_UID;
            }
            if (gid != (uint32_t)-1 && gid != vst.gid) {
                vst.gid = gid;
                what |= VFS_SET_GID;
            }
            if (what != 0 && (vst.mode & 06000u)) {
                vst.mode &= ~06000u; /* an owner change clears the set-id bits, for root too */
                what |= VFS_SET_MODE;
            }
            rc = what ? vfs_setattr(ref.mnt, ref.node, what, &vst) : 0;
        }
        vfs_ref_release(&ref);
        return rc;
    }
    node = ref.node;
    rc = krffs_chown_node(node, uid, gid);
    vfs_ref_release(&ref);
    return rc;
}

/* chmod(path, mode) and fchmodat(dirfd, path, mode[, flags]) share do_chmod. */
static int32_t sys_chmod(isr_params_t *params)
{
    const char __user *upath = (const char __user *) params->ebx;
    uint32_t mode = params->ecx;
    char pathbuf[256];
    const char *name;
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    len = copy_path_cwd(pathbuf, sizeof(pathbuf), upath);
    if (len < 0) {
        return len;
    }
    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }
    return do_chmod(name, mode);
}

static int32_t sys_fchmodat(isr_params_t *params)
{
    int dirfd = (int) params->ebx;
    const char __user *upath = (const char __user *) params->ecx;
    uint32_t mode = params->edx;
    char pathbuf[256];
    const char *name;
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    len = copy_path_at(pathbuf, sizeof(pathbuf), dirfd, upath);
    if (len < 0) {
        return len;
    }
    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }
    return do_chmod(name, mode);
}

/* fchmod(fd, mode): the descriptor form. A KRFFS-backed regular file's stored
 * perms are updated; any other descriptor (device/pipe/socket/tty) is a no-op.
 */
static int32_t sys_fchmod(isr_params_t *params)
{
    int fd = (int) params->ebx;
    uint32_t mode = params->ecx;
    fd_entry_t *entry;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(&current_proc->fds, fd);
    if (!entry) {
        return -EBADF;
    }
    if ((entry->type == FD_TYPE_FILE || entry->type == FD_TYPE_DIR) &&
        entry->node) {
        return krffs_chmod_node(entry->node, mode);
    }
    if (entry->type == FD_TYPE_PTS) {
        struct pty *pt = pty_get(pty_slave_index(entry));
        if (!pt) {
            return -EBADF;
        }
        if (!cred_privileged() && current_proc->euid != pt->uid) {
            return -EPERM;
        }
        pt->mode = (uint16_t) (mode & 0777u);
        return 0;
    }
    /* Every other descriptor has fixed root-owned metadata, like its path. */
    return cred_privileged() ? 0 : -EPERM;
}

/* chown/lchown/fchownat share do_chown; both the 16-bit and *32 numbers route
 * here since musl issues the 16-bit chown on i386. */
static int32_t sys_chown(isr_params_t *params)
{
    const char __user *upath = (const char __user *) params->ebx;
    uint32_t uid = params->ecx;
    uint32_t gid = params->edx;
    char pathbuf[256];
    const char *name;
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    len = copy_path_cwd(pathbuf, sizeof(pathbuf), upath);
    if (len < 0) {
        return len;
    }
    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }
    return do_chown(name, uid, gid);
}

static int32_t sys_fchownat(isr_params_t *params)
{
    int dirfd = (int) params->ebx;
    const char __user *upath = (const char __user *) params->ecx;
    uint32_t uid = params->edx;
    uint32_t gid = params->esi;
    char pathbuf[256];
    const char *name;
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    len = copy_path_at(pathbuf, sizeof(pathbuf), dirfd, upath);
    if (len < 0) {
        return len;
    }
    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }
    return do_chown(name, uid, gid);
}

/* fchown(fd, uid, gid): the descriptor form. Only the root identity exists, so
 * the id policy is enforced and the call is otherwise a no-op. */
static int32_t sys_fchown(isr_params_t *params)
{
    int fd = (int) params->ebx;
    uint32_t uid = params->ecx;
    uint32_t gid = params->edx;
    fd_entry_t *entry;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(&current_proc->fds, fd);
    if (!entry) {
        return -EBADF;
    }
    if ((entry->type == FD_TYPE_FILE || entry->type == FD_TYPE_DIR) &&
        entry->node) {
        return krffs_chown_node(entry->node, uid, gid);
    }
    if (entry->type == FD_TYPE_PTS) {
        struct pty *pt = pty_get(pty_slave_index(entry));
        int uid_changes;
        int gid_changes;
        if (!pt) {
            return -EBADF;
        }
        uid_changes = (uid != (uint32_t) -1 && uid != pt->uid);
        gid_changes = (gid != (uint32_t) -1 && gid != pt->gid);
        if ((uid_changes || gid_changes) && !cred_privileged()) {
            return -EPERM;
        }
        if (uid != (uint32_t) -1) {
            pt->uid = uid;
        }
        if (gid != (uint32_t) -1) {
            pt->gid = gid;
        }
        return 0;
    }
    /* Every other descriptor has fixed root-owned metadata, like its path:
     * a no-change request succeeds, a change is refused. */
    if ((uid != (uint32_t) -1 && uid != 0) ||
        (gid != (uint32_t) -1 && gid != 0)) {
        return -EPERM;
    }
    return 0;
}

/**
 * link(2): always -EPERM (KRFFS has no hard links).
 *
 * KRFFS stores exactly one name per node (no link count, no shared data), so
 * the call returns -EPERM, the POSIX errno for "the filesystem does not
 * support the creation of hard links". The source is copied and resolved
 * first, so a bad pointer (-EFAULT), bad dirfd (-EBADF), or missing source
 * (-ENOENT) is reported as itself, not masked by EPERM. dropbear
 * depends on the EPERM: it moves a freshly generated host key with link(tmp,
 * final) and, on EPERM/EACCES, falls back to writing the key directly to the
 * final path.
 */
static int32_t sys_link(isr_params_t *params)
{
    const char __user *uold = (const char __user *) params->ebx;
    const char __user *unew = (const char __user *) params->ecx;
    char oldbuf[256];
    char newbuf[256];
    const char *name;
    int len;
    int32_t rc;

    if (!current_proc || !uold || !unew) {
        return -EFAULT;
    }
    /* Validate BOTH paths (copy_path_cwd faults a bad pointer to -EFAULT) and
     * that the source exists, before reporting that the filesystem cannot
     * hard-link: link(valid, bad_ptr) is -EFAULT, link(missing, x) is
     * -ENOENT, not -EPERM. */
    len = copy_path_cwd(oldbuf, sizeof(oldbuf), uold);
    if (len < 0) {
        return len;
    }
    len = copy_path_cwd(newbuf, sizeof(newbuf), unew);
    if (len < 0) {
        return len;
    }
    name = oldbuf;
    if (name[0] == '/') {
        name++;
    }
    {
        struct vfs_ref ref;

        rc = vfs_resolve(0, name, 0, &ref, 0, 0);
        if (rc != 0) {
            return rc;
        }
        vfs_ref_release(&ref);
    }
    /* The source exists; no mounted file system here has hard links, so the
     * answer is the type's: EPERM. The day a type implements `link`, this
     * gains a parent walk and goes through vfs_link. */
    return -EPERM;
}

static int32_t sys_linkat(isr_params_t *params)
{
    int olddirfd = (int) params->ebx;
    const char __user *uold = (const char __user *) params->ecx;
    int newdirfd = (int) params->edx;
    const char __user *unew = (const char __user *) params->esi;
    int flags = (int) params->edi;
    char oldbuf[256];
    char newbuf[256];
    const char *name;
    int len;
    int32_t rc;

    if (!current_proc || !uold || !unew) {
        return -EFAULT;
    }
    /* Only AT_SYMLINK_FOLLOW and AT_EMPTY_PATH are defined for linkat. */
    if (flags & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) {
        return -EINVAL;
    }
    /* Resolve the source against olddirfd and the destination against newdirfd,
     * so a bad dirfd (-EBADF) or pointer (-EFAULT) and a missing source
     * (-ENOENT) are reported before the "no hard links" -EPERM. */
    len = copy_path_at(oldbuf, sizeof(oldbuf), olddirfd, uold);
    if (len < 0) {
        return len;
    }
    len = copy_path_at(newbuf, sizeof(newbuf), newdirfd, unew);
    if (len < 0) {
        return len;
    }
    name = oldbuf;
    if (name[0] == '/') {
        name++;
    }
    {
        struct vfs_ref ref;

        rc = vfs_resolve(0, name, 0, &ref, 0, 0);
        if (rc != 0) {
            return rc;
        }
        vfs_ref_release(&ref);
    }
    /* The source exists; no mounted file system here has hard links, so the
     * answer is the type's: EPERM. The day a type implements `link`, this
     * gains a parent walk and goes through vfs_link. */
    return -EPERM;
}

/*
 * _llseek (syscall 140): 64-bit seek. EBX=fd, ECX=offset_high, EDX=offset_low,
 * ESI=result (long long*), EDI=whence. musl's lseek is implemented on top of
 * this on i386. KRFFS files are under 4 GB so offset_high is 0 for them; the
 * raw disk windows are the reason this call has a full 64-bit offset, since
 * a whole drive's far end cannot be named in 32 bits.
 */
static int32_t sys__llseek(isr_params_t *params)
{
    int fd = (int)params->ebx;
    uint32_t offset_high = params->ecx;
    uint32_t offset_low = params->edx;
    long long __user *result = (long long __user *)params->esi;
    int whence = (int)params->edi;
    fd_entry_t *entry;
    long long off, new_off;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(&current_proc->fds, fd);
    if (entry && entry->type == FD_TYPE_PIPE) {
        return -ESPIPE; /* a pipe is not seekable (POSIX) */
    }
    if (entry && entry->type == FD_TYPE_DEVICE) {
        uint64_t raw_size;
        uint32_t log_size;

        if (devfs_fd_is_raw(entry)) {
            uint64_t resolved;
            int32_t rc;

            if (!devfs_fd_raw_size(entry, &raw_size)) {
                return -ENXIO; /* the window was withdrawn under an open fd */
            }
            off = (long long)(((uint64_t)offset_high << 32) |
                              (uint64_t)offset_low);
            rc = raw_seek_resolve(entry->ofd->raw_offset, raw_size, off, whence, &resolved);
            if (rc != 0) {
                return rc;
            }
            if (resolved > (uint64_t)0x7FFFFFFFFFFFFFFFull) {
                return -EOVERFLOW;
            }
            if (!result) {
                return -EFAULT;
            }
            new_off = (long long)resolved;
            entry->ofd->raw_offset = resolved;
            if (copy_to_user(result, &new_off, sizeof(new_off)) != 0) {
                return -EFAULT;
            }
            return 0;
        }
        if (devfs_fd_log_size(entry, &log_size)) {
            off = (long long)(((uint64_t)offset_high << 32) |
                              (uint64_t)offset_low);
            switch (whence) {
                case 0: /* SEEK_SET */
                    new_off = off;
                    break;
                case 1: /* SEEK_CUR */
                    new_off = (long long)entry->ofd->offset + off;
                    break;
                case 2: /* SEEK_END */
                    new_off = (long long)log_size + off;
                    break;
                default:
                    return -EINVAL;
            }
            if (new_off < 0 || new_off > (long long)0xFFFFFFFFu) {
                return -EINVAL;
            }
            if (!result) {
                return -EFAULT;
            }
            entry->ofd->offset = (uint32_t)new_off;
            if (copy_to_user(result, &new_off, sizeof(new_off)) != 0) {
                return -EFAULT;
            }
            return 0;
        }
        if (devfs_lseek_zero(entry)) {
            long long zero = 0;
            entry->ofd->offset = 0;
            if (result && copy_to_user(result, &zero, sizeof(zero)) != 0) {
                return -EFAULT;
            }
            return 0;
        }
        return -ESPIPE;
    }
    if (entry && (entry->type == FD_TYPE_CONSOLE || entry->type == FD_TYPE_MOUSE)) {
        return -ESPIPE;
    }
    if (!entry || entry->type != FD_TYPE_FILE || !entry->node) {
        return -EBADF;
    }
    if (fd_node_check(entry) != 0) {
        return -EBADF;  /* the file was unlinked or relocated out from under us */
    }

    off = (long long)(((uint64_t)offset_high << 32) | (uint64_t)offset_low);
    switch (whence) {
        case 0: /* SEEK_SET */
            new_off = off;
            break;
        case 1: /* SEEK_CUR */
            new_off = (long long)entry->ofd->offset + off;
            break;
        case 2: /* SEEK_END */
            new_off = (long long)entry->node->data_size + off;
            break;
        default:
            return -EINVAL;
    }
    if (new_off < 0) {
        return -EINVAL;
    }
    /* The descriptor offset is 32-bit (KRFFS files are well under 4 GB); refuse
     * a seek it cannot represent, not silently truncating to a wrong
     * place. */
    if (new_off > (long long)0xFFFFFFFFu) {
        return -EINVAL;
    }
    /* The result pointer is mandatory: a NULL one is a caller bug, not a silent
     * success (musl always passes a valid loff_t*). */
    if (!result) {
        return -EFAULT;
    }

    entry->ofd->offset = (uint32_t)new_off;
    if (copy_to_user(result, &new_off, sizeof(new_off)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * Descriptor control (fcntl64, syscall 221).
 *
 * EBX=fd, ECX=cmd, EDX=arg. Supports the descriptor-flag and status-flag
 * round-trips libc needs: F_DUPFD(_CLOEXEC), F_GETFD/F_SETFD (the
 * close-on-exec bit), and F_GETFL/F_SETFL (access + O_NONBLOCK/O_APPEND,
 * stored, not enforced).
 */
static int32_t sys_fcntl64(isr_params_t *params)
{
    int fd = (int)params->ebx;
    int cmd = (int)params->ecx;
    uint32_t arg = params->edx;
    fd_entry_t *entry;

    enum {
        F_DUPFD = 0,
        F_GETFD = 1,
        F_SETFD = 2,
        F_GETFL = 3,
        F_SETFL = 4,
        F_GETLK = 5,
        F_SETLK = 6,
        F_SETLKW = 7,
        F_GETLK64 = 12,
        F_SETLK64 = 13,
        F_SETLKW64 = 14,
        F_DUPFD_CLOEXEC = 1030
    };

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(&current_proc->fds, fd);
    if (!entry) {
        return -EBADF;
    }

    switch (cmd) {
        case F_GETFD:
            return (entry->flags & FD_FLAG_CLOEXEC) ? 1 : 0; /* 1 == FD_CLOEXEC */
        case F_SETFD:
            if (arg & 1) {
                entry->flags |= FD_FLAG_CLOEXEC;
            } else {
                entry->flags &= ~(uint32_t)FD_FLAG_CLOEXEC;
            }
            return 0;
        case F_GETFL: {
            /* Report the access mode (O_RDONLY=0/O_WRONLY=1/O_RDWR=2) plus the
             * OFD status flags we track (O_APPEND, O_NONBLOCK). */
            int rw = entry->flags & (FD_FLAG_READ | FD_FLAG_WRITE);
            int fl;
            if (rw == (FD_FLAG_READ | FD_FLAG_WRITE)) {
                fl = 2;
            } else if (rw == FD_FLAG_WRITE) {
                fl = 1;
            } else {
                fl = 0;
            }
            if (entry->ofd) {
                fl |= (int)(entry->ofd->status_flags & (OFD_APPEND | OFD_NONBLOCK));
            }
            return fl;
        }
        case F_SETFL:
            /* O_APPEND and O_NONBLOCK are the settable status flags; they live
             * on the open-file description (shared by dup/fork), not the
             * descriptor. Other bits in arg (access mode, O_CLOEXEC) are
             * accepted and ignored per POSIX. */
            if (entry->ofd) {
                entry->ofd->status_flags = arg & (OFD_APPEND | OFD_NONBLOCK);
            }
            return 0;
        case F_DUPFD:
        case F_DUPFD_CLOEXEC: {
            int newfd;
            fd_entry_t *dst;
            /* F_DUPFD duplicates onto the LOWEST free fd >= arg (the POSIX
             * contract). An arg past the table is -EINVAL; no slot at or above
             * it is -EMFILE. */
            if (arg >= (uint32_t)MAX_FDS) {
                return -EINVAL;
            }
            newfd = fd_alloc_from(&current_proc->fds, (int)arg);
            if (newfd < 0) {
                return -EMFILE;
            }
            dst = &current_proc->fds.entries[newfd];
            /* The duplicate SHARES oldfd's open-file description (so the seek
             * offset is shared, per POSIX). fd_alloc_from just gave dst a fresh
             * OFD; drop it and point dst at oldfd's, taking a reference. A
             * shared socket gains a reference too; FD_CLOEXEC is
             * per-descriptor. */
            fd_ofd_unref(dst->ofd);
            dst->type  = entry->type;
            dst->node  = entry->node;
            dst->sock  = entry->sock;
            dst->ops   = entry->ops;  /* share the backing object + vtable */
            dst->priv  = entry->priv;
            dst->ofd   = entry->ofd;
            fd_ofd_ref(dst->ofd);
            dst->flags = entry->flags;
            if (entry->type == FD_TYPE_SOCKET && entry->sock) {
                socket_ref(entry->sock);
            }
            if (cmd == F_DUPFD_CLOEXEC) {
                dst->flags |= FD_FLAG_CLOEXEC;
            } else {
                dst->flags &= ~(uint32_t)FD_FLAG_CLOEXEC;
            }
            return newfd;
        }
        case F_SETLK:
        case F_SETLKW:
        case F_SETLK64:
        case F_SETLKW64:
            /* Advisory POSIX locks. Rach2 is single-user with no competing
             * locker, so every lock request is granted (documented no-conflict
             * semantics), because dropbear and musl lock utmp and lastlog
             * and must not see -EINVAL or -ENOSYS. */
            return 0;
        case F_GETLK:
        case F_GETLK64:
            /* Report that no lock would conflict: set l_type (the first short
             * of the struct flock the caller passed) to F_UNLCK (2). */
            if (!arg) {
                return -EFAULT;
            }
            {
                short unlck = 2; /* F_UNLCK */
                if (copy_to_user((void __user *)arg, &unlck, sizeof(unlck)) != 0) {
                    return -EFAULT;
                }
            }
            return 0;
        default:
            return -EINVAL;
    }
}

/**
 * stat core for the old 32-bit stat struct (stat(106)/lstat(107)).
 *
 * Used by ISH and some programs; fills struct linux_stat, not linux_stat64.
 * `follow` distinguishes stat (follow symlinks) from lstat (do not), which
 * matters only for the /proc magic symlinks, since KRFFS has none. EBX =
 * path, ECX = stat buffer.
 */
/**
 * Copy the three times out of a stat64 into the narrower old stat.
 *
 * The old ABI is a different struct, so every path that fills it fills it
 * separately; before this kernel had a clock they were all zero and the
 * difference did not show.
 */
static void stat_times_from_stat64(struct linux_stat *kst, const struct linux_stat64 *s64)
{
    kst->st_atime = s64->st_atime;
    kst->st_atime_nsec = s64->st_atime_nsec;
    kst->st_mtime = s64->st_mtime;
    kst->st_mtime_nsec = s64->st_mtime_nsec;
    kst->st_ctime = s64->st_ctime;
    kst->st_ctime_nsec = s64->st_ctime_nsec;
}

/** The three times of a KRFFS node, into the old stat. */
static void stat_node_times(struct linux_stat *kst, const krffs_node_t *node)
{
    struct linux_stat64 s64;

    stat64_node_times(&s64, node);
    stat_times_from_stat64(kst, &s64);
}

/** The root directory's three times, into the old stat. */
static void stat_root_times(struct linux_stat *kst)
{
    struct linux_stat64 s64;

    stat64_root_times(&s64);
    stat_times_from_stat64(kst, &s64);
}

static int32_t do_old_stat(const char __user *path, struct linux_stat __user *st, int follow)
{
    struct linux_stat kst;
    char pathbuf[256];
    krffs_node_t *node;
    int len;

    if (!current_proc || !path || !st) {
        return -EFAULT;
    }

    len = copy_path_cwd(pathbuf, sizeof(pathbuf), path);
    if (len < 0) {
        return len;
    }

    /* Build the result in a kernel struct, then copy it out in one shot. */
    k_memset(&kst, 0, sizeof(kst));

    if ((pathbuf[0] == '/' && pathbuf[1] == '\0') || pathbuf[0] == '\0') {
        kst.st_mode = S_IFDIR | 0755;  /* drwxr-xr-x */
        kst.st_nlink = 1;
        kst.st_ino = 0;  /* Root inode is 0 */
        kst.st_blksize = 4096;
        /* The root is a real directory with real times: an entry created in it
         * stamps them, which is how a program watching a directory sees it. */
        stat_root_times(&kst);
        if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
            return -EFAULT;
        }
        return 0;
    }

    /* /dev and /proc are synthetic. The old stat(2) ABI is a narrower struct,
     * so translate the synthetic stat64 result down into it. */
    {
        struct vfs_ref ref;
        const struct vpath *svp;
        int32_t rc = vfs_resolve(0, pathbuf, 0, &ref, 0, 0);

        if (rc != 0) {
            return rc; /* missing, not-a-dir, or a denied ancestor */
        }
        svp = vfs_synth_vpath(ref.mnt, ref.node);
        if (svp) {
            struct linux_stat64 s64;

            rc = synth_stat(svp, &s64, follow);
            vfs_ref_release(&ref);
            if (rc != 0) {
                return rc;
            }
            kst.st_mode = (unsigned short)s64.st_mode;
            kst.st_nlink = (unsigned short)s64.st_nlink;
            kst.st_dev = (unsigned long)s64.st_dev;
            kst.st_ino = (unsigned long)s64.st_ino;
            kst.st_rdev = (unsigned long)s64.st_rdev;
            kst.st_blksize = (unsigned long)s64.st_blksize;
            stat_times_from_stat64(&kst, &s64);
            if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
                return -EFAULT;
            }
            return 0;
        }
        if (!mnt_is_root(ref.mnt)) {
            struct linux_stat64 s64;

            rc = vfs_fill_stat64(&s64, &ref);
            vfs_ref_release(&ref);
            if (rc != 0) {
                return rc;
            }
            kst.st_mode = (unsigned short)s64.st_mode;
            kst.st_nlink = (unsigned short)s64.st_nlink;
            kst.st_dev = (unsigned long)s64.st_dev;
            kst.st_ino = (unsigned long)s64.st_ino;
            kst.st_uid = (unsigned short)s64.st_uid;
            kst.st_gid = (unsigned short)s64.st_gid;
            kst.st_size = (unsigned long)s64.st_size;
            kst.st_blksize = (unsigned long)s64.st_blksize;
            kst.st_blocks = (unsigned long)s64.st_blocks;
            stat_times_from_stat64(&kst, &s64);
            if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
                return -EFAULT;
            }
            return 0;
        }
        node = ref.node;
        vfs_ref_release(&ref);
    }

    /* The type comes from the node, as it does in the stat64 path. This read
     * S_IFREG for everything, so a directory stated through the old ABI came
     * back a regular file; musl reaches this only when statx and fstatat64 are
     * both unavailable, which is why it went unnoticed. */
    kst.st_mode = (unsigned short)((KRFFS_IS_DIR(node->mode) ? S_IFDIR : S_IFREG) |
                                   (node->mode & 07777));
    kst.st_nlink = 1;                              /* Always 1 for KRFFS */
    kst.st_uid = (unsigned short)node->uid;
    kst.st_gid = (unsigned short)node->gid;
    kst.st_size = (unsigned long)node->data_size;
    kst.st_blksize = 4096;
    kst.st_blocks = (node->data_size + 511) / 512;
    kst.st_ino = (unsigned long)node->id; /* the node's DJB hash is the inode */
    stat_node_times(&kst, node);

    if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
        return -EFAULT;
    }

    return 0;
}

/* stat(106) follows symlinks; lstat(107) does not. musl falls back to these
 * from statx for AT_FDCWD paths, so both must exist for stat()/lstat() to work.
 */
static int32_t sys_stat(isr_params_t *params)
{
    return do_old_stat((const char __user *)params->ebx,
                       (struct linux_stat __user *)params->ecx, 1);
}

static int32_t sys_lstat(isr_params_t *params)
{
    return do_old_stat((const char __user *)params->ebx,
                       (struct linux_stat __user *)params->ecx, 0);
}

/**
 * Duplicate the parent's user address space into the child, copy-on-write.
 *
 * Iterates the parent's page directory and, for each present user page, maps
 * the SAME physical frame into the child (bumping the frame refcount) rather
 * than copying it; writable pages are write-protected and PTE_COW-marked in
 * both directories so the first write faults into cow_fault for a private
 * copy. Returns 0 on success, -1 on failure.
 */
static int copy_user_pages(process_t *parent, page_dir_t *child_dir)
{
    page_dir_t *parent_dir;
    uint32_t dir_idx, page_idx;
    page_table_t *parent_table;

    if (parent->cr3 == 0) {
        parent_dir = _kernel_dir;
    } else {
        /* cr3 has &page_dir->phys_page_tables (get_page_dir_phys); recover
         * the page_dir_t* by subtracting that offset. The layout is
         * logical_page_tables[1024] then phys_page_tables[1024], so the offset
         * is 1024 * sizeof(page_table_t*) = 4096 bytes. */
        parent_dir = (page_dir_t *)(parent->cr3 - 1024 * sizeof(page_table_t *));
    }

    for (dir_idx = 0; dir_idx < 1024; dir_idx++) {
        parent_table = parent_dir->logical_page_tables[dir_idx];
        if (!parent_table) {
            continue;
        }

        for (page_idx = 0; page_idx < 1024; page_idx++) {
            uint32_t pte = parent_table->pages[page_idx];
            uint32_t virt_addr;
            uint32_t parent_phys;

            if (!(pte & 0x1) || !(pte & 0x4)) {
                continue;  /* present (0x1) + user (0x4) only */
            }

            virt_addr = (dir_idx << 22) | (page_idx << 12);
            parent_phys = pte & ~0xFFF;

            /* Skip the identity-mapped kernel region (first 32MB) and any bogus
             * mapping. */
            if (virt_addr < HARD_MEMORY_LIMIT || parent_phys == 0) {
                continue;
            }

            /*
             * Copy-on-write: share the frame instead of copying it. The
             * child maps the same physical frame, and the frame's reference
             * count is bumped so it survives until both sides drop it. A
             * WRITABLE parent page is write-protected and marked PTE_COW in
             * BOTH directories, so the first write on either side faults
             * into cow_fault (which makes a private copy and re-grants
             * write). A READ-ONLY page (ELF text, PROT_READ, or an
             * already-COW page from an earlier fork) is shared with its
             * exact flags, no copy; a write to genuine read-only text still
             * segfaults because it has no PTE_COW.
             */
            if (pte & 0x2) {
                uint32_t cow = 0x1u | 0x4u | PTE_COW; /* present + user + COW (RO) */
                if (map_user_frame(child_dir, virt_addr, parent_phys, cow) != 0) {
                    return -1;
                }
                parent_table->pages[page_idx] = (pte & ~0x2u) | PTE_COW;
                paging_flush_tlb(virt_addr); /* the parent is the current dir */
            } else if (map_user_frame(child_dir, virt_addr, parent_phys,
                                      pte & 0xFFFu) != 0) {
                return -1;
            }
            frame_ref(parent_phys);
        }
    }

    return 0;
}

/**
 * Drop every descriptor marked close-on-exec.
 *
 * Called by execve once it has committed to the new image so FD_FLAG_CLOEXEC
 * descriptors (set via O_CLOEXEC or fcntl F_SETFD) do not leak into it. Mirrors
 * sys_close's per-type teardown. Scans from fd 0: stdio is not close-on-exec by
 * default, but a program may set FD_CLOEXEC on it, and Linux honors that here.
 */
static void close_cloexec_fds(void)
{
    int fd;

    for (fd = 0; fd < MAX_FDS; fd++) {
        fd_entry_t *entry = fd_get(&current_proc->fds, fd);
        if (!entry || !(entry->flags & FD_FLAG_CLOEXEC)) {
            continue;
        }
        fd_release_backing(entry);
        fd_free(&current_proc->fds, fd);
    }
}

/* elf_read_fn over a KRFFS file (ctx is the path): lets the segment loader pull
 * the ELF header, program headers, and each PT_LOAD straight from the file. */
static int exec_krffs_read(void *ctx, void *dst, uint32_t count, uint32_t off)
{
    return krffs_read((const char *)ctx, (char *)dst, count, off);
}

/* A program on a mount that is not the root is read through the mount: the
 * node held from the resolution, opened once for the load and released
 * after it. One exec runs at a time (see the argument buffers below), so
 * the context is static. */
struct exec_vfs_ctx {
    struct vfs_ref ref;
    void *handle;
    int32_t err; /* the last failed read's errno, reported instead of ENOEXEC */
};
static struct exec_vfs_ctx exec_ctx;

static int exec_vfs_read(void *ctx, void *dst, uint32_t count, uint32_t off)
{
    struct exec_vfs_ctx *c = ctx;
    int32_t got = vfs_read(c->ref.mnt, c->handle, dst, count, off);

    if (got < 0) {
        c->err = got;
        return -1;
    }
    return (int)got;
}

static void exec_vfs_done(void)
{
    if (exec_ctx.ref.mnt) {
        vfs_release(exec_ctx.ref.mnt, exec_ctx.handle);
        vfs_ref_release(&exec_ctx.ref);
        exec_ctx.ref.mnt = NULL;
        exec_ctx.handle = NULL;
    }
}

/*
 * Where execve has the new image's arguments while it destroys the old one.
 *
 * Static, and safe to be static: one process at a time is between filling these
 * and using them, because `in_syscall` stops the timer from scheduling and no
 * path inside execve blocks. They cannot be on the stack (the kernel stack is
 * 16 KB) and cannot be in the caller's address space (that is what is being
 * torn down).
 *
 * The sizes come from the page these end up in, not from a round number.
 * `build_user_stack` lays the strings, both pointer arrays, the argc word, a
 * nine-entry auxv and 16 bytes of AT_RANDOM into ONE 4096-byte page, and it
 * runs AFTER the old image is freed, so a vector that fits here and not there
 * kills the caller instead of failing it. For 63 entries in each vector the
 * fixed part is 147 words of info block, 16 bytes of AT_RANDOM and up to 15
 * bytes of alignment, about 640 bytes; the remaining space is shared by the
 * strings of BOTH vectors, which is why the budget below is one number for the
 * two of them and not one each. 3072 leaves room for an auxv entry somebody
 * adds later.
 *
 * The old limits were 15 entries and 512 bytes each, which a link line
 * passes without trying: a compiler, `-static`, `-o`, an output name and a
 * dozen objects is over both.
 */
#define EXEC_MAX_ENTRIES 63
#define EXEC_MAX_STRINGS 3072

static char _exec_path_buf[256];
static char _exec_argv_storage[EXEC_MAX_STRINGS];
static char *_exec_argv_bufs[EXEC_MAX_ENTRIES + 1];
static char _exec_envp_storage[EXEC_MAX_STRINGS];
static char *_exec_envp_bufs[EXEC_MAX_ENTRIES + 1];

/* A script's first line, and the script's own path, which becomes an argument
 * to its interpreter. Both static, because the argv pointers handed on point
 * into them and the caller's memory is gone by the time they are read. */
#define EXEC_SHEBANG_MAX 128
static char _exec_shebang_line[EXEC_SHEBANG_MAX + 1];
static char _exec_script_path[sizeof(_exec_path_buf)];

/**
 * Kill the current process after exec has destroyed its old address space.
 *
 * Past free_user_pages there is no image to return to, so a load/stack failure
 * cannot continue. Mark the process a zombie, wake any parent waiting on it (so
 * the scheduler has a target), switch away, and never iret back into the
 * destroyed space.
 */
static void __attribute__((noreturn)) exec_fail_destroyed(void)
{
    process_t *parent;
    page_dir_t *dir;
    int was_init = (current_proc->pid == init_pid && init_pid != 0);

    /* If the console-owning session leader is the one whose exec failed,
     * release the controlling terminal before zombifying so the timer-IRQ
     * serial drain does not target this dead session during the teardown window
     * (mirrors sys_exit; usually a no-op here because sys_execve already
     * released before free_user_pages; kept as a defensive backstop). */
    if (tty_console_owned_by_leader((int)current_proc->sid, (int)current_proc->pid)) {
        tty_set_session(&console_tty, 0, 0);
    }
    /* A dying session leader also disassociates any pty slave it controlled, so
     * a later session can claim the same /dev/pts/N. */
    pty_release_leader((int)current_proc->sid, (int)current_proc->pid);

    current_proc->state = PROC_ZOMBIE;
    /* A failed exec destroyed the image; report it as a SIGKILL death (Linux
     * wait-status: low 7 bits = the signal) so the parent's waitpid sees an
     * abnormal termination, not a bogus exit code. */
    current_proc->exit_code = SIGKILL & 0x7F;

    /* Wake a parent blocked in wait() so the scheduler has a target. */
    parent = get_process(current_proc->parent_pid);
    if (parent && parent->state == PROC_BLOCKED) {
        parent->state = PROC_READY;
    }

    /* Hand any surviving children to init, as the other death paths do. */
    reparent_children(current_proc->pid);

    /* Release the descriptor table (socket refs + open-file descriptions), the
     * same teardown sys_exit and terminate do, so a failed-exec death leaks no
     * sockets or OFDs. */
    release_fd_table(&current_proc->fds);
    vfs_cwd_clear(current_proc);

    /* A real /sbin/init whose exec failed is a fatal init death; halt BEFORE
     * schedule(), or a still-READY process would mask the panic and run on with
     * a dead PID 1. Only when the debug shell is the supervisor does it return
     * to the shell, below. */
    if (was_init && !debug_shell_is_home) {
        panic_init_died(current_proc->pid);   /* never returns */
    }

    schedule();

    /* schedule() returned -> nothing else is runnable. Reparenting guarantees
     * every process has a live parent (init at worst), and a blocked parent was
     * woken above, so the scheduler always has the parent as a target: this
     * point is reached only for the init-role process itself, under the debug
     * shell. */
    if (current_proc->pid == init_pid) {
        init_pid = 0;
    }
    /* Switch to the kernel directory BEFORE freeing the dead one, otherwise
     * free_page_directory() refuses the still-active directory and leaks it. */
    sched_disable();
    switch_page_directory(_kernel_dir);
    if (current_proc->cr3 != 0) {
        dir = (page_dir_t *)(current_proc->cr3 - 1024 * sizeof(page_table_t *));
        free_user_pages(dir);
        free_page_directory(dir);
    }
    free_process(current_proc);
    current_proc = &proc_table[0];
    current_proc->state = PROC_RUNNING;
    /* Fresh command line + drop stdin-wait state before re-enabling IRQs. */
    shell_reset_input();
    /* Return to the debug shell only when it is the supervisor; on a normal
     * boot (which the reasoning above makes unreachable here) halt rather than
     * expose the shell. */
    if (!debug_shell_is_home) {
        for (;;) {
            __asm__ __volatile__("cli; hlt");
        }
    }
    __asm__ __volatile__("movl %0, %%esp\n\t"
                         "sti\n\t"
                         "jmp *%1\n\t"
                         :
                         : "r"((uint32_t)stack + KERNEL_STACK_SIZE_LOCAL),
                           "r"((uint32_t)shell_loop)
                         : "memory");
    /* The jmp above never falls through; this loop makes the noreturn
     * contract visible to the compiler, which cannot know that. */
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

/**
 * Rewrite an exec of a script into an exec of its interpreter.
 *
 * A file whose first two bytes are `#!` names the program that should run it,
 * and Linux's rule is what everything expects: the rest of that line is the
 * interpreter and at most one argument, and the program then runs as
 * `interpreter [argument] script original-arguments...`. Without this, an exec
 * of a script fails with `ENOEXEC` and only a shell, which reads the file
 * itself, can run one. A DHCP client execs its lease script directly, so the
 * machine needs it.
 *
 * One level only. A script whose interpreter is itself a script is refused,
 * not followed, because a chain of them is a loop nobody can see the
 * end of, and nothing here needs one.
 *
 * `_exec_path_buf` and `_exec_argv_bufs` are rewritten in place on success.
 *
 * @param argc the argument count, updated when the vector grows.
 * @return 1 when rewritten, 0 when the file is not a script, or a negative
 *         errno for one whose first line names nothing.
 */
static int32_t exec_shebang(int *argc)
{
    char *interp;
    char *arg = NULL;
    char *p;
    int n;
    int i;
    int extra;
    int count;
    char *rebuilt[EXEC_MAX_ENTRIES + 1];

    n = exec_krffs_read((void *)(_exec_path_buf + (_exec_path_buf[0] == '/' ? 1 : 0)),
                        _exec_shebang_line, EXEC_SHEBANG_MAX, 0);
    if (n < 3 || _exec_shebang_line[0] != '#' || _exec_shebang_line[1] != '!') {
        return 0;
    }
    _exec_shebang_line[n] = '\0';
    for (i = 2; i < n; i++) {
        if (_exec_shebang_line[i] == '\n') {
            _exec_shebang_line[i] = '\0';
            break;
        }
    }
    /* A line that filled the buffer without ending is refused, not cut:
       a truncated interpreter name may name a different program that exists,
       and the kernel would then check and run that one. */
    if (i == n && n == EXEC_SHEBANG_MAX) {
        return -ENOEXEC;
    }

    p = _exec_shebang_line + 2;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        return -ENOEXEC; /* `#!` and nothing after it names no interpreter */
    }
    interp = p;
    while (*p && *p != ' ' && *p != '\t') {
        p++;
    }
    if (*p) {
        *p++ = '\0';
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p) {
            /* The remainder is ONE argument, trailing blanks removed, which is
               what Linux passes and what every `#!/bin/sh -e` line means. */
            char *end = p;

            while (*end) {
                end++;
            }
            while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
                end--;
            }
            *end = '\0';
            arg = p;
        }
    }
    if (interp[0] != '/') {
        return -ENOEXEC; /* an interpreter is named by an absolute path */
    }

    /* The script's own path becomes an argument, so it must outlive the buffer
       the interpreter's path is about to be written into. The two buffers are
       the same size, so this is a copy and never a truncation: handing the
       interpreter a shortened name would run a different file from the one
       whose permissions were just checked. */
    k_strncpy(_exec_script_path, _exec_path_buf, sizeof(_exec_script_path) - 1);
    _exec_script_path[sizeof(_exec_script_path) - 1] = '\0';

    extra = arg ? 2 : 1;
    count = extra + (*argc > 0 ? *argc - 1 : 0);
    if (count >= EXEC_MAX_ENTRIES) {
        return -E2BIG;
    }
    rebuilt[0] = interp;
    i = 1;
    if (arg) {
        rebuilt[i++] = arg;
    }
    rebuilt[i++] = _exec_script_path;
    for (n = 1; n < *argc; n++) {
        rebuilt[i++] = _exec_argv_bufs[n];
    }
    rebuilt[i] = NULL;
    for (n = 0; n <= i; n++) {
        _exec_argv_bufs[n] = rebuilt[n];
    }
    *argc = i;

    k_strncpy(_exec_path_buf, interp, sizeof(_exec_path_buf) - 1);
    _exec_path_buf[sizeof(_exec_path_buf) - 1] = '\0';
    /* Normalized the way a path from a caller is normalized. The check that
       keeps an exec out of the synthetic mounts reads the first component and
       does not collapse `.` or `..`, so an interpreter named
       `/bin/../dev/something` would pass it while the file system resolved the
       name it really wrote. */
    if (canonicalize_abs(_exec_path_buf) < 0) {
        return -ENOENT;
    }
    return 1;
}

/**
 * Replace the current process image with a new program (execve(2)).
 *
 * EBX = path, ECX = argv[], EDX = envp[]. Does not return on success. Copies
 * the path, argv, and envp into static buffers, validates the ELF header
 * BEFORE tearing the old image down (so a bad executable leaves the caller
 * intact), then frees the old user pages, streams the new program in, builds
 * the SysV/i386 initial stack, and redirects the iret frame to the entry
 * point. A failure after the old image is gone routes to
 * exec_fail_destroyed.
 */
static int32_t sys_execve(isr_params_t *params)
{
    const char __user *user_path = (const char __user *)params->ebx;
    char *const __user *user_argv = (char *const __user *)params->ecx;
    char *const __user *user_envp = (char *const __user *)params->edx;

    int argc = 0;
    int envc = 0;
    uint32_t argv_offset = 0;
    uint32_t envp_offset = 0;
    int owned_console = 0;
    uint32_t setid_mode = 0;
    uint32_t file_uid = 0;
    uint32_t file_gid = 0;
    int secure = 0;

    uint32_t entry_point;
    elf_image_t img;
    uint8_t random16[16];
    page_dir_t *proc_dir;
    uintptr_t stack_phys;
    const char *name;
    int len;

    if (!current_proc) {
        return -ESRCH;
    }

    /* Resolve the program path (relative names against the cwd) into the static
     * buffer. A bad user pointer, an over-long path, or an empty path is
     * rejected by the return value, so a kernel-range pointer cannot leak
     * kernel memory. The buffer is 256 bytes (NUL included). */
    if (!user_path) {
        return -EFAULT;
    }
    len = copy_path_cwd(_exec_path_buf, sizeof(_exec_path_buf), user_path);
    if (len < 0) {
        return len;
    }

    /* Copy argv from user space, range-checking every user pointer and bounding
     * the cumulative offset so _exec_argv_storage cannot overflow. An argument
     * that does not fit fails the exec instead of being silently truncated.
     * The pointer array and each string are read through the copy helpers,
     * which fault-check each access.
     *
     * The two vectors share one string budget, which is checked after both are
     * in: the page they are laid into is one page whichever of them fills it,
     * and the check that would otherwise catch it runs after the old image is
     * gone, where the only way to fail is to kill the caller. */
    if (user_argv) {
        for (argc = 0; argc < EXEC_MAX_ENTRIES; argc++) {
            uint32_t arg_ptr = 0;
            int r;

            /* Read the argv[argc] pointer out of the user array. */
            if (copy_from_user(&arg_ptr, (const void __user *)&user_argv[argc],
                               sizeof(arg_ptr)) != 0) {
                return -EFAULT;
            }
            if (arg_ptr == 0) {
                break;  /* NULL terminator */
            }
            /* The storage is exactly full and another argument is waiting:
             * that is a size limit reached, not a bad address. */
            if (argv_offset >= sizeof(_exec_argv_storage)) {
                return -E2BIG;
            }
            _exec_argv_bufs[argc] = &_exec_argv_storage[argv_offset];
            r = strncpy_from_user(&_exec_argv_storage[argv_offset],
                                  (const char __user *)arg_ptr,
                                  sizeof(_exec_argv_storage) - argv_offset);
            if (r == -EFAULT) {
                return -EFAULT;
            }
            if (r < 0) {
                return -E2BIG; /* did not fit */
            }
            argv_offset += (uint32_t)r + 1;
        }
        /* The bufs array has EXEC_MAX_ENTRIES args plus a NULL terminator;
         * one more non-NULL pointer is too many args, not a silent truncation.
         */
        if (argc == EXEC_MAX_ENTRIES) {
            uint32_t arg_ptr = 0;
            if (copy_from_user(&arg_ptr,
                               (const void __user *)&user_argv[EXEC_MAX_ENTRIES],
                               sizeof(arg_ptr)) != 0) {
                return -EFAULT;
            }
            if (arg_ptr != 0) {
                return -E2BIG;
            }
        }
    }
    _exec_argv_bufs[argc] = NULL;

    /* Copy the real envp the same way. */
    if (user_envp) {
        for (envc = 0; envc < EXEC_MAX_ENTRIES; envc++) {
            uint32_t env_ptr = 0;
            int r;

            if (copy_from_user(&env_ptr, (const void __user *)&user_envp[envc],
                               sizeof(env_ptr)) != 0) {
                return -EFAULT;
            }
            if (env_ptr == 0) {
                break;  /* NULL terminator */
            }
            if (envp_offset >= sizeof(_exec_envp_storage)) {
                return -E2BIG;
            }
            _exec_envp_bufs[envc] = &_exec_envp_storage[envp_offset];
            r = strncpy_from_user(&_exec_envp_storage[envp_offset],
                                  (const char __user *)env_ptr,
                                  sizeof(_exec_envp_storage) - envp_offset);
            if (r == -EFAULT) {
                return -EFAULT;
            }
            if (r < 0) {
                return -E2BIG; /* did not fit */
            }
            envp_offset += (uint32_t)r + 1;
        }
        /* As with argv: one more non-NULL envp pointer overflows the bufs
         * array; fail, not silently drop the tail of the environment. */
        if (envc == EXEC_MAX_ENTRIES) {
            uint32_t env_ptr = 0;
            if (copy_from_user(&env_ptr,
                               (const void __user *)&user_envp[EXEC_MAX_ENTRIES],
                               sizeof(env_ptr)) != 0) {
                return -EFAULT;
            }
            if (env_ptr != 0) {
                return -E2BIG;
            }
        }
    }
    _exec_envp_bufs[envc] = NULL;

    /* The two vectors share the page they are laid into, so they share a
     * budget, and it is checked HERE: `build_user_stack` would catch it, but it
     * runs after `free_user_pages`, where there is no image left to return an
     * error to and the caller is killed instead. */
    if (argv_offset + envp_offset > EXEC_MAX_STRINGS) {
        return -E2BIG;
    }

    name = _exec_path_buf;
    if (name[0] == '/') {
        name++;
    }

    /* The synthetic /dev and /proc mounts hold no executables: the walk
     * arriving on one is refused, so execve sees the same mount boundary as
     * open and cannot reach a file a hostile image placed below the
     * synthetic prefix. */
    {
        struct vfs_ref xref;
        krffs_node_t *prog;
        int32_t rc;

        rc = vfs_resolve(0, name, 0, &xref, 0, 0);
        if (rc != 0) {
            return rc; /* missing, not-a-dir, or a denied ancestor */
        }
        if (vfs_synth_vpath(xref.mnt, xref.node)) {
            vfs_ref_release(&xref);
            return -EACCES;
        }
        /* MS_NOEXEC is enforced here, the one place exec decides. */
        if (xref.mnt->flags & VFS_MS_NOEXEC) {
            vfs_ref_release(&xref);
            return -EACCES;
        }
        if (!mnt_is_root(xref.mnt)) {
            /* A program on another type: its attributes decide the execute
             * right and the set-id facts; the image is read through the
             * mount below. */
            struct vfs_stat vst;

            rc = (int)cred_access_ref(&xref, CRED_X, 0);
            if (rc == 0) {
                rc = vfs_getattr(xref.mnt, xref.node, &vst);
            }
            if (rc != 0) {
                vfs_ref_release(&xref);
                return rc;
            }
            setid_mode = vst.mode & (KRFFS_MODE_SUID | KRFFS_MODE_SGID);
            if (!(vst.mode & 010u)) {
                setid_mode &= ~KRFFS_MODE_SGID; /* set-gid without group execute is mandatory locking */
            }
            if (xref.mnt->flags & VFS_MS_NOSUID) {
                setid_mode = 0;
            }
            file_uid = vst.uid;
            file_gid = vst.gid;
            exec_ctx.ref = xref; /* held until the image is loaded or refused */
            exec_ctx.err = 0;
            rc = vfs_open_node(xref.mnt, xref.node, 0, &exec_ctx.handle);
            if (rc != 0) {
                vfs_ref_release(&exec_ctx.ref);
                exec_ctx.ref.mnt = NULL;
                return rc;
            }
            goto checked;
        }
        prog = xref.node;
        rc = cred_access_node(prog, CRED_X, 0);
        if (rc != 0) {
            vfs_ref_release(&xref);
            return rc; /* exec is the execute bit, for root too */
        }
        /* Capture the set-id facts while the node is at hand; they apply only
         * once the exec commits, so a rejected image changes nothing. The
         * root node cannot reach here (a directory fails the execute check),
         * so the dirty flag can never read as a set-id bit.
         *
         * Setgid is honored only when the group-execute bit is set with it,
         * which is Linux's rule. Without that condition a 02700 file owned
         * 1000:0 passes the execute check on its OWNER bits, since the access
         * check judges one identity class and the owner's first, and then hands
         * its executor egid 0. The mode needs no write to appear either: an
         * owner may chmod any 07777 onto their own file. Setgid without
         * group-execute means something else in Unix (the mandatory-locking
         * mark), and nothing here implements that: the lock family is advisory
         * and always granted. */
        setid_mode = (uint32_t) (prog->mode & KRFFS_MODE_SUID);
        if ((prog->mode & KRFFS_MODE_SGID) && (prog->mode & 0010u)) {
            setid_mode |= KRFFS_MODE_SGID;
        }
        /* MS_NOSUID: the bits are read and then ignored, which is the
         * flag's whole meaning. */
        if (xref.mnt->flags & VFS_MS_NOSUID) {
            setid_mode = 0;
        }
        file_uid   = prog->uid;
        file_gid   = prog->gid;
        vfs_ref_release(&xref);
    }
checked:

    if (current_proc->cr3 == 0) {
        proc_dir = _kernel_dir;
    } else {
        proc_dir = (page_dir_t *)(current_proc->cr3 - 1024 * sizeof(page_table_t *));
    }

    /* Validate the ELF header BEFORE tearing down the old image, so the common
     * "not a valid executable" failure leaves the calling process intact. A
     * file that is not an ELF may still be a script naming its interpreter, and
     * then what runs is the interpreter, checked here exactly as the script
     * was. */
    {
        Elf32_Ehdr ehdr;

        if (exec_ctx.ref.mnt) {
            /* On another type only an ELF image runs: a script names an
             * interpreter by path, and the shebang reader is the root's. */
            if (exec_vfs_read(&exec_ctx, &ehdr, sizeof(ehdr), 0) != (int)sizeof(ehdr) ||
                !elf_valid_exec(&ehdr, sizeof(ehdr))) {
                int32_t why = exec_ctx.err ? exec_ctx.err : -ENOEXEC;

                exec_vfs_done();
                return why;
            }
        } else if (exec_krffs_read((void *)name, &ehdr, sizeof(ehdr), 0) != (int)sizeof(ehdr) ||
            !elf_valid_exec(&ehdr, sizeof(ehdr))) {
            int32_t sb = exec_shebang(&argc);

            if (sb < 0) {
                return sb;
            }
            if (sb == 0) {
                return -ENOEXEC;
            }
            /* A set-id script runs as its invoker. The interpreter is what
             * actually runs, it is not set-id, and honouring the script's bits
             * is a hole Unix closed long ago: the interpreter can be made to
             * read a different file than the one whose bits were checked. */
            setid_mode = 0;
            file_uid = 0;
            file_gid = 0;

            name = _exec_path_buf;
            if (name[0] == '/') {
                name++;
            }
            {
                struct vfs_ref iref;
                int32_t rc;

                rc = vfs_resolve(0, name, 0, &iref, 0, 0);
                if (rc != 0) {
                    return rc;
                }
                if (vfs_synth_vpath(iref.mnt, iref.node) ||
                    (iref.mnt->flags & VFS_MS_NOEXEC)) {
                    vfs_ref_release(&iref);
                    return -EACCES;
                }
                if (!mnt_is_root(iref.mnt)) {
                    /* An interpreter on another type loads through its
                     * mount, as a program there does. */
                    rc = cred_access_ref(&iref, CRED_X, 0);
                    if (rc != 0) {
                        vfs_ref_release(&iref);
                        return rc;
                    }
                    exec_ctx.ref = iref;
                    exec_ctx.err = 0;
                    rc = vfs_open_node(iref.mnt, iref.node, 0, &exec_ctx.handle);
                    if (rc != 0) {
                        vfs_ref_release(&exec_ctx.ref);
                        exec_ctx.ref.mnt = NULL;
                        return rc;
                    }
                    if (exec_vfs_read(&exec_ctx, &ehdr, sizeof(ehdr), 0) != (int)sizeof(ehdr) ||
                        !elf_valid_exec(&ehdr, sizeof(ehdr))) {
                        int32_t why = exec_ctx.err ? exec_ctx.err : -ENOEXEC;

                        exec_vfs_done();
                        return why; /* one level only: an interpreter that is a script */
                    }
                    goto interpreter_checked;
                }
                rc = cred_access_node((krffs_node_t *)iref.node, CRED_X, 0);
                vfs_ref_release(&iref);
                if (rc != 0) {
                    return rc;
                }
            }
            if (exec_krffs_read((void *)name, &ehdr, sizeof(ehdr), 0) != (int)sizeof(ehdr) ||
                !elf_valid_exec(&ehdr, sizeof(ehdr))) {
                /* One level only: an interpreter that is itself a script. */
                return -ENOEXEC;
            }
interpreter_checked:
            ;
        }
    }

    /* The exec is now committed (the image is valid). Drop close-on-exec
     * descriptors before the old address space goes away. */
    close_cloexec_fds();

    /* The set-id bits, the way privilege is regained: the new image runs with
     * the file owner's effective ids while the real ids still say who is
     * asking. Then the POSIX exec rule for every exec, set-id or not: the
     * saved ids take the new effective ids, which is what lets the image shed
     * the acquired authority and take it back. AT_SECURE then reports whether
     * the image runs with authority its invoker lacks, the effective id
     * differing from the real one either way, so libc distrusts the
     * caller-supplied environment; it is the only such signal, since the
     * stack has no AT_UID/AT_EUID for libc to compare. */
    if (setid_mode & KRFFS_MODE_SUID) {
        current_proc->euid = file_uid;
    }
    if (setid_mode & KRFFS_MODE_SGID) {
        current_proc->egid = file_gid;
    }
    current_proc->suid = current_proc->euid;
    current_proc->sgid = current_proc->egid;
    secure = (current_proc->euid != current_proc->ruid) ||
             (current_proc->egid != current_proc->rgid);

    /* Record the program's basename as comm for /proc/<pid>/stat and ps. */
    proc_set_comm(current_proc, name);

    /* Reset caught signal dispositions to default: their handler addresses
     * point into the OLD image that is about to be replaced. SIG_IGN
     * dispositions and the blocked mask are preserved (POSIX exec semantics).
     */
    {
        int s;
        for (s = 1; s <= KSIG_MAX; s++) {
            if (current_proc->sigacts[s].handler != KSIG_DFL &&
                current_proc->sigacts[s].handler != KSIG_IGN) {
                k_memset(&current_proc->sigacts[s], 0, sizeof(struct k_sigaction));
            }
        }
        current_proc->sig_pending = 0;
    }

    /* If this process owns the console, release the controlling terminal across
     * the destructive load below: once free_user_pages runs the old image is
     * gone and a load/stack failure routes to exec_fail_destroyed (death), so
     * leaving sid set would let the timer-IRQ serial drain target a destroyed
     * image in that window (the same class as the sys_exit teardown bug). On
     * success it is re-published with the unchanged session/group before
     * returning to userspace, preserving exec's keep-the-controlling-terminal
     * semantics; on failure exec_fail_destroyed releases again (idempotent). */
    owned_console = tty_console_owned_by_leader((int)current_proc->sid, (int)current_proc->pid);
    if (owned_console) {
        tty_set_session(&console_tty, 0, 0);
    }

    /* Free old user pages, then stream the new program in (header + program
     * headers + each PT_LOAD; no 512 KB whole-file buffer). Past this point the
     * old image is gone, so any failure terminates the process rather than
     * returning into a destroyed address space. */
    free_user_pages(proc_dir);

    if (elf_load_image(exec_ctx.ref.mnt ? exec_vfs_read : exec_krffs_read,
                       exec_ctx.ref.mnt ? (void *)&exec_ctx : (void *)name, proc_dir,
                       &img) != 0) {
        exec_vfs_done();
        exec_fail_destroyed();
    }
    exec_vfs_done();
    entry_point = img.entry;

    stack_phys = alloc_user_page(proc_dir, USER_STACK_BASE);
    if (stack_phys == 0) {
        exec_fail_destroyed();
    }

    k_memset((void *)stack_phys, 0, PAGE_SIZE);

    /*
     * Initial user stack layout (growing down from USER_STACK_BASE +
     * PAGE_SIZE):
     *
     *   [arg strings]   - null-terminated strings
     *   [NULL]          - envp terminator
     *   [NULL]          - argv terminator
     *   [argv[argc-1]]  - pointers to arg strings
     *   ...
     *   [argv[0]]
     *   [argc]          <- ESP points here
     */

    /* Build the SysV/i386 initial stack (argc, the argv and real envp pointer
     * arrays, and the auxv) in the freshly zeroed page. build_user_stack copies
     * the strings in and 16-byte-aligns the entry ESP; the layout is verified
     * host-side (tests/host/uimage_hosttest.c). */
    krandom_bytes(random16, 16);
    {
        uint32_t user_esp = build_user_stack((void *)stack_phys, USER_STACK_BASE,
                                             PAGE_SIZE, argc, _exec_argv_bufs,
                                             _exec_envp_bufs, &img, random16,
                                             secure);

        if (user_esp == 0) {
            exec_fail_destroyed();
        }

        /* Modify the ISR return frame so iret resumes at the new program's
         * entry point on its fresh stack. */
        params->eip = entry_point;
        params->user_esp = user_esp;

        /* SysV i386 process entry gives argc/argv on the stack and %edx as an
         * atexit hook or zero. %ebp marks the outermost frame. Clear the GP
         * registers inherited across exec so startup code cannot read stale
         * state from the old image. */
        params->ebp = 0;
        params->edx = 0;
        params->ebx = 0;
        params->ecx = 0;
        params->esi = 0;
        params->edi = 0;

        /* Reset thread-local storage for the new image. exec replaces the
         * program in place, so the trapframe still has the old program's
         * segment selectors (its %gs may be the 0x33 TLS selector) and the PCB
         * still has its TLS base. A fresh image has no TLS until its libc
         * calls set_thread_area, so restore the plain user-data selector (0x23,
         * as enter_usermode does for a brand-new process) in all four slots and
         * clear the base in both the PCB and the live GDT descriptor. */
        params->gs = 0x23;
        params->fs = 0x23;
        params->es = 0x23;
        params->ds = 0x23;
        current_proc->tls_base = 0;
        gdt_set_tls(0);

        /* Build the address-space map (VMAs) for the new image so the
         * page-fault handler can demand-page, grow the stack, or segfault. The
         * ELF code/data pages themselves are still eagerly loaded above. */
        elf_setup_vmas(&current_proc->vm, &img, USER_STACK_BASE,
                       VM_STACK_MAX_GROWTH);
        /* A new program in an old process: what the last one ran out of is not
         * this one's, so it may say so once too. */
        current_proc->vma_reported = 0;
    }

    /* Reload CR3 to flush the whole TLB: after freeing the old user pages and
     * mapping new stack/code pages, the TLB may hold stale entries from the
     * replaced image. */
    __asm__ __volatile__ (
        "movl %%cr3, %%eax\n\t"
        "movl %%eax, %%cr3"
        ::: "eax", "memory"
    );

    /* Exec succeeded: re-publish the controlling terminal we released above,
     * with the unchanged session/group (exec keeps the controlling terminal),
     * so the new image owns the console exactly as the old one did. */
    if (owned_console) {
        tty_set_session(&console_tty, (int)current_proc->sid, (int)current_proc->pgid);
    }

    return 0; /* unused: iret resumes at the new entry point */
}

/**
 * Create a child process (fork(2)).
 *
 * Clones the current process: PCB, page directory (copy-on-write user
 * pages), and fd table. The child is set up to return from fork with value
 * 0; the parent gets the child's PID. Returns -errno on failure.
 *
 * Among what the child inherits is the program name, which `execve` sets and a
 * child that never execs would otherwise never have: a daemon that forks
 * itself into the background is exactly such a child, and without this it
 * shows in `ps` and in /proc/<pid>/stat with no name for as long as it runs.
 */
static int32_t sys_fork(isr_params_t *params)
{
    process_t *parent = current_proc;
    process_t *child;
    page_dir_t *child_dir;
    uintptr_t child_kstack_base;
    uint32_t *child_sp;
    isr_params_t *child_frame;

    if (!parent) {
        return -ESRCH;
    }

    child = alloc_process();
    if (!child) {
        return -EAGAIN;
    }

    /* alloc_process gave the child its own stdio open-file descriptions;
     * release them now (before any failure path or the fd-table copy below
     * overwrites the table), or those OFDs would be orphaned and leak the pool
     * on every fork. */
    release_fd_table(&child->fds);

    child_dir = clone_page_directory();
    if (!child_dir) {
        free_process(child);
        return -ENOMEM;
    }

    /* Copy user pages from parent to child. On failure, release the frames
     * copy_user_pages already allocated and the cloned directory (not just the
     * PCB) or each failed fork leaks a directory and its private page tables.
     * child_dir is never the running (parent) directory, so it frees cleanly.
     */
    if (copy_user_pages(parent, child_dir) != 0) {
        free_user_pages(child_dir);
        free_page_directory(child_dir);
        free_process(child);
        return -ENOMEM;
    }

    child_kstack_base = kstack_alloc();
    if (!child_kstack_base) {
        free_user_pages(child_dir);
        free_page_directory(child_dir);
        free_process(child);
        return -ENOMEM;
    }
    child->kernel_stack = child_kstack_base + KERNEL_STACK_SIZE;

    child->cr3 = get_page_dir_phys(child_dir);
    child->parent_pid = parent->pid;

    /* Inherit the parent's session and process group: a forked child stays in
     * the parent's job-control group until it (or the parent) calls
     * setsid/setpgid. alloc_process defaulted these to the child's own pid. */
    child->sid = parent->sid;
    child->pgid = parent->pgid;
    child->umask = parent->umask; /* the file-creation mask is inherited */
    child->ruid = parent->ruid;   /* credentials: the child is who the parent was */
    child->euid = parent->euid;
    child->suid = parent->suid;
    child->rgid = parent->rgid;
    child->egid = parent->egid;
    child->sgid = parent->sgid;
    child->ngroups = parent->ngroups;
    k_memcpy(child->groups, parent->groups, sizeof(child->groups));
    k_memcpy(child->comm, parent->comm, sizeof(child->comm));
    /* The working directory's hold on its mount is the child's too. */
    child->cwd_mnt = NULL;
    child->cwd_node = NULL;
    vfs_cwd_set(child, parent->cwd_mnt, parent->cwd_node);

    /* Inherit the parent's TLS base: the child's copied trapframe keeps the
     * parent's %gs selector, and its address space is a copy at the same
     * virtual addresses, so the same base is correct. Without this the context
     * switch would reprogram the TLS descriptor to 0 for the child and its
     * %gs-relative reads (errno, the stack canary) would fault or read garbage.
     */
    child->tls_base = parent->tls_base;

    /* The child's address space is a copy of the parent's at the same virtual
     * addresses (copy_user_pages duplicated the pages), so copy the VMA map too
     * otherwise the child would have no regions and the first heap/stack
     * fault would segfault it. (k_memcpy, not struct assignment, so the
     * freestanding kernel does not need a compiler-emitted memcpy.) */
    k_memcpy(&child->vm, &parent->vm, sizeof(child->vm));

    /* The child inherits the parent's current working directory. */
    k_memcpy(child->cwd, parent->cwd, sizeof(child->cwd));

    /* Signals: the child inherits the dispositions and the blocked mask, but
     * its pending set starts empty (POSIX fork semantics). */
    k_memcpy(child->sigacts, parent->sigacts, sizeof(child->sigacts));
    child->sig_blocked = parent->sig_blocked;
    child->sig_pending = 0;

    /* Copy file descriptor table. The child shares the parent's open-file
     * descriptions (so a seek in one is seen by the other, per POSIX), so each
     * inherited OFD gains a reference; inherited sockets gain one too, so the
     * parent and child release independently. */
    k_memcpy(&child->fds, &parent->fds, sizeof(fd_table_t));
    {
        int fdi;
        for (fdi = 0; fdi < MAX_FDS; fdi++) {
            if (child->fds.entries[fdi].type == FD_TYPE_NONE) {
                continue;
            }
            fd_ofd_ref(child->fds.entries[fdi].ofd);
            if (child->fds.entries[fdi].type == FD_TYPE_SOCKET &&
                child->fds.entries[fdi].sock) {
                socket_ref(child->fds.entries[fdi].sock);
            }
        }
    }

    /*
     * Set up child's kernel stack for returning from fork.
     *
     * Stack layout (growing down from kernel_stack):
     *   [ISR frame copied from parent, with eax=0]
     *   [return address = isr_return]
     *   [saved registers for switch_context: ebp, ebx, esi, edi]
     *
     * When child is scheduled via switch_context:
     * 1. Pops edi, esi, ebx, ebp
     * 2. ret jumps to isr_return
     * 3. isr_return restores segments, popal (eax=0), iret to userspace
     */
    child_sp = (uint32_t *)child->kernel_stack;

    /* The ISR frame, copied from the parent, with eax (fork's return) set
     * to 0. */
    child_sp -= sizeof(isr_params_t) / sizeof(uint32_t);
    child_frame = (isr_params_t *)child_sp;
    k_memcpy(child_frame, params, sizeof(isr_params_t));
    child_frame->eax = 0;

    /* The switch_context return address, then its saved EBP/EBX/ESI/EDI. */
    --child_sp;
    *child_sp = (uint32_t)isr_return;
    --child_sp;
    *child_sp = 0;  /* EBP */
    --child_sp;
    *child_sp = 0;  /* EBX */
    --child_sp;
    *child_sp = 0;  /* ESI */
    --child_sp;
    *child_sp = 0;  /* EDI */

    child->regs.esp = (uint32_t)child_sp;
    child->state = PROC_READY;

    return (int32_t)child->pid;
}


/* waitpid/wait4 options. */
#define WNOHANG    1
#define WUNTRACED  2  /* also report a child that has STOPPED */
#define WCONTINUED 8  /* also report a child that has CONTINUED */

/* Bytes of struct rusage to zero-fill for wait4 (i386: two 8-byte timevals plus
 * 14 longs). We do not track per-process resource usage yet, so it is reported
 * as all zero. */
#define RUSAGE_BYTES 72

/* Interrupts-off helpers (defined below with the signal code); used here for
 * the lost-wakeup-safe sleep in do_wait. */
static uint32_t sig_irq_save(void);
static void sig_irq_restore(uint32_t flags);

/* Does process p match a wait pid selector? pid > 0 = that exact pid; -1 = any
 * child; 0 = any child in the caller's process group; < -1 = any child in group
 * (-pid). (The caller has already confirmed p is its child.) */
static int wait_pid_match(int pid, const process_t *p)
{
    if (pid > 0) {
        return (int)p->pid == pid;
    }
    if (pid == -1) {
        return 1;
    }
    if (pid == 0) {
        return (int)p->pgid == (int)current_proc->pgid;
    }
    if (pid == -2147483647 - 1) {
        return 0; /* INT_MIN: -pid would overflow; no such group */
    }
    return (int)p->pgid == -pid;
}

/**
 * Shared core of waitpid(7) and wait4(114).
 *
 * `status` (may be NULL) receives the Linux-encoded wait status that
 * sys_exit/terminate already stored in exit_code; `rusage_ptr` (0 for
 * waitpid, may be 0 for wait4) is zero-filled on reap. With WNOHANG, returns
 * 0 instead of blocking when a child exists but has not exited.
 */
static int32_t do_wait(int pid, int __user *status, int options, uint32_t rusage_ptr)
{
    uint32_t child_pid;
    int exit_code;
    uint32_t i;
    uint32_t flags;

    if (!current_proc) {
        return -ESRCH;
    }

    /* Validate AND fault in the status (and rusage) pointers up front (a NULL
     * status means "don't report status"), so a bad or not-yet-present address
     * is rejected before any child is reaped; otherwise the copy_to_user below
     * could fail after free_process() has destroyed the zombie, losing it.
     * Prefault for write, since that is how the results are delivered. */
    if (status &&
        !user_buffer_ok((uint32_t)(__force uintptr_t)status, sizeof(int), 1)) {
        return -EFAULT;
    }
    if (rusage_ptr && !user_buffer_ok(rusage_ptr, RUSAGE_BYTES, 1)) {
        return -EFAULT;
    }

    while (1) {
        int have_child = 0;

        /* Scan ALL matching children for a reportable event (a zombie to reap,
         * or a stop/continue to report), not just the first non-zombie. */
        for (i = 0; i < MAX_PROCS; i++) {
            process_t *p = &proc_table[i];

            if (p->state == PROC_UNUSED || p->parent_pid != current_proc->pid) {
                continue;
            }
            if (!wait_pid_match(pid, p)) {
                continue;
            }
            have_child = 1;

            /* A zombie is reaped: collect status, free pages + slot. */
            if (p->state == PROC_ZOMBIE) {
                child_pid = p->pid;
                exit_code = p->exit_code;
                if (p->cr3 != 0) {
                    page_dir_t *child_dir =
                        (page_dir_t *)(p->cr3 - 1024 * sizeof(page_table_t *));
                    free_user_pages(child_dir);
                    free_page_directory(child_dir);
                }
                /* The zombie no longer runs on its kernel stack, so return it
                 * to the arena for the next fork; this is the single release
                 * point for a per-fork stack. */
                kstack_free(p->kernel_stack);
                free_process(p);
                if (status) {
                    copy_to_user((void __user *)status, &exit_code, sizeof(exit_code));
                }
                if (rusage_ptr) {
                    uint8_t zero[RUSAGE_BYTES];
                    k_memset(zero, 0, sizeof(zero));
                    copy_to_user((void __user *)rusage_ptr, zero, RUSAGE_BYTES);
                }
                return (int32_t)child_pid;
            }

            /* A stop (WUNTRACED) or continue (WCONTINUED) is REPORTED, not
             * reaped; jc_status is consumed so the same event is not delivered
             * twice. */
            if ((options & WUNTRACED) && p->state == PROC_STOPPED &&
                (p->jc_status & 0xff) == 0x7f) {
                exit_code = (int)p->jc_status;
                p->jc_status = 0;
                if (status) {
                    copy_to_user((void __user *)status, &exit_code, sizeof(exit_code));
                }
                return (int32_t)p->pid;
            }
            if ((options & WCONTINUED) && p->jc_status == 0xffff) {
                exit_code = (int)p->jc_status;
                p->jc_status = 0;
                if (status) {
                    copy_to_user((void __user *)status, &exit_code, sizeof(exit_code));
                }
                return (int32_t)p->pid;
            }
        }

        if (!have_child) {
            return -ECHILD;
        }
        /* A child exists but has no reportable event. WNOHANG returns 0;
         * otherwise block until a child changes state (exit/stop/continue wakes
         * us) or a signal interrupts. Lost-wakeup-safe: the signal_pending
         * check and the PROC_BLOCKED transition are one interrupts-off step, so
         * a signal posted from an IRQ either is seen here or finds us BLOCKED
         * and wakes us. */
        if (options & WNOHANG) {
            return 0;
        }
        flags = sig_irq_save();
        if (signal_pending()) {
            sig_irq_restore(flags);
            return -EINTR;
        }
        current_proc->state = PROC_BLOCKED;
        sig_irq_restore(flags);
        schedule();
        flags = sig_irq_save();
        if (current_proc->state == PROC_BLOCKED) {
            idle_wait_irq_off(); /* idle until a waker, then re-scan */
        }
        current_proc->state = PROC_RUNNING;
        sig_irq_restore(flags);
    }
}

/*
 * sys_waitpid - wait for a child to change state.
 * Args: EBX = pid (-1 = any child), ECX = status pointer, EDX = options.
 * Returns the reaped child's PID, 0 (WNOHANG, none ready), or -errno.
 */
static int32_t sys_waitpid(isr_params_t *params)
{
    return do_wait((int)params->ebx, (int __user *)params->ecx,
                   (int)params->edx, 0);
}

/*
 * sys_wait4 - like waitpid plus an optional rusage out-pointer
 * (zero-filled). Args: EBX = pid, ECX = status, EDX = options, ESI = rusage.
 * musl's wait/waitpid are implemented on top of this.
 */
static int32_t sys_wait4(isr_params_t *params)
{
    return do_wait((int)params->ebx, (int __user *)params->ecx,
                   (int)params->edx, params->esi);
}

/*
 * creat(2): EBX = path, ECX = mode. Returns an fd open for writing, or
 * -errno. An existing regular file is truncated to zero by the call; a
 * directory, the root included, is EISDIR.
 */
static int32_t sys_creat(isr_params_t *params)
{
    const char __user *path = (const char __user *)params->ebx;
    uint32_t mode = params->ecx;
    char pathbuf[256];
    const char *name;
    int len;

    if (!current_proc || !path) {
        return -EFAULT;
    }

    len = copy_path_cwd(pathbuf, sizeof(pathbuf), path);
    if (len < 0) {
        return len;
    }

    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }
    /* creat(2) is open(O_WRONLY|O_CREAT|O_TRUNC), and it is exactly that
     * call, so the two cannot drift: the EISDIR for a directory, the write
     * check, the truncate with its set-id clear, and the stamp of a new
     * node all happen where open does them. */
    return do_open(name, 1 /* O_WRONLY */ | O_CREAT | O_TRUNC, mode);
}

/**
 * unlink(2): EBX = path. Returns 0 or -errno.
 *
 * Every refusal is decided here, not below, because `krffs_unlink`
 * answers all of them with a bare -1 that userspace decodes as `EPERM`
 * whatever went wrong. A missing component gives `-ENOENT` and a plain file
 * in the middle of the path `-ENOTDIR`, both from the reporting resolver;
 * the root and any directory give `-EISDIR`, which is what tells a caller
 * to use `rmdir`. The root is tested by node identity because it has
 * 0755 with no type bits and would otherwise read as a plain file.
 */
static int32_t sys_unlink(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
    char pathbuf[256];
    const char *path;
    const char *name;
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }

    len = copy_path_cwd(pathbuf, sizeof(pathbuf), upath);
    if (len < 0) {
        return len;
    }
    path = pathbuf;

    if (path[0] == '/' && path[1] == '\0') {
        return -EISDIR; /* the root directory cannot be unlinked */
    }

    name = path;
    if (name[0] == '/') {
        name++;
    }

    if (name[0] == '\0') {
        return -EINVAL; /* empty filename */
    }

    {
        struct vfs_ref parent;
        char leaf[VFS_NAME_MAX + 1];
        krffs_node_t *node;
        void *found;
        int32_t rc;

        rc = vfs_resolve(0, pathbuf, VFS_WALK_PARENT, &parent, leaf,
                         sizeof(leaf));
        if (rc != 0) {
            return rc;
        }
        if (vfs_synth_vpath(parent.mnt, parent.node)) {
            vfs_ref_release(&parent);
            return -EROFS; /* the synthetic mounts are read-only */
        }
        if (vfs_leaf_mount(parent.mnt, parent.node, leaf)) {
            vfs_ref_release(&parent);
            return -EBUSY; /* a mount point cannot be unlinked */
        }
        if (!mnt_is_root(parent.mnt)) {
            rc = cred_remove_ref(&parent, leaf);
            if (rc == 0) {
                rc = vfs_unlink_name(parent.mnt, parent.node, leaf);
            }
            vfs_ref_release(&parent);
            return rc;
        }
        /* The directory's write bit is checked before the target is even
         * looked up: a caller with no write permission there must see
         * -EACCES, not learn whether the name exists. */
        rc = cred_access_node((krffs_node_t *)parent.node, CRED_W, 0);
        if (rc != 0) {
            vfs_ref_release(&parent);
            return rc;
        }
        rc = parent.mnt->type->ops->lookup(parent.mnt, parent.node, leaf,
                                           &found);
        if (rc != 0) {
            vfs_ref_release(&parent);
            return rc;
        }
        node = found;
        if (node == krffs_fs->node || KRFFS_IS_DIR(node->mode)) {
            vfs_ref_release(&parent);
            return -EISDIR;
        }
        rc = cred_sticky_ok((krffs_node_t *)parent.node, node);
        if (rc != 0) {
            vfs_ref_release(&parent);
            return rc;
        }
        /* The last decision, never the first: orphaning bypasses the refusals
         * the file system would have made, so every one of them is decided
         * above. With a holder the node stays live and nameless until the
         * last close; with none it is freed now, because the release lives
         * at that close and an unconditional orphan would leak an extent on
         * every removal made while nothing happened to be reading. */
        if (fd_node_holders(node, NULL) > 0) {
            krffs_unlink_held(node);
            vfs_ref_release(&parent);
            return 0;
        }
        vfs_ns_lock(parent.mnt);
        rc = vfs_unlink_name(parent.mnt, parent.node, leaf);
        vfs_ns_unlock(parent.mnt);
        vfs_ref_release(&parent);
        return rc;
    }
}

/*
 * sys_mkdir - create a directory.
 * Args: EBX = path, ECX = mode (masked by umask, like any creation). Returns 0
 * or -errno (-EEXIST if it exists, -ENOENT if a parent is missing).
 */
static int32_t sys_mkdir(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
    krffs_node_t *node;
    char pathbuf[256];
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    if (!krffs_fs) {
        return -ENOENT;
    }
    len = copy_path_cwd(pathbuf, sizeof(pathbuf), upath);
    if (len < 0) {
        return len;
    }
    if (pathbuf[0] == '/' && pathbuf[1] == '\0') {
        return -EEXIST; /* the root exists; mkdir -p walks through here */
    }
    {
        struct vfs_ref parent;
        char leaf[VFS_NAME_MAX + 1];
        void *found;
        int32_t rc;

        rc = vfs_resolve(0, pathbuf, VFS_WALK_PARENT, &parent, leaf,
                         sizeof(leaf));
        if (rc != 0) {
            return rc; /* a missing parent, or a denied ancestor */
        }
        if (vfs_synth_vpath(parent.mnt, parent.node)) {
            vfs_ref_release(&parent);
            return -EROFS; /* nothing is created under /dev or /proc */
        }
        /* A name a mount stands at exists, whatever the type's own lookup
         * says: a KRFFS /dev would shadow the synthetic mount. */
        if (vfs_leaf_mount(parent.mnt, parent.node, leaf)) {
            vfs_ref_release(&parent);
            return -EEXIST;
        }
        if (parent.mnt->type->ops->lookup(parent.mnt, parent.node, leaf,
                                          &found) == 0) {
            if (parent.mnt->type->ops->put) {
                parent.mnt->type->ops->put(parent.mnt, found);
            }
            vfs_ref_release(&parent);
            return -EEXIST;
        }
        if (!mnt_is_root(parent.mnt)) {
            rc = cred_access_ref(&parent, CRED_W, 0);
            if (rc == 0) {
                rc = vfs_mkdir(parent.mnt, parent.node, leaf, params->ecx);
            }
            vfs_ref_release(&parent);
            return rc;
        }
        rc = cred_access_node((krffs_node_t *)parent.node, CRED_W, 0);
        if (rc != 0) {
            vfs_ref_release(&parent);
            return rc;
        }
        vfs_ns_lock(parent.mnt);
        rc = vfs_mkdir(parent.mnt, parent.node, leaf, params->ecx);
        vfs_ns_unlock(parent.mnt);
        if (rc != 0) {
            vfs_ref_release(&parent);
            return rc;
        }
        if (parent.mnt->type->ops->lookup(parent.mnt, parent.node, leaf,
                                          &found) == 0) {
            node = found;
            krffs_stamp_new_node(node, params->ecx);
        }
        vfs_ref_release(&parent);
    }
    return 0;
}

/*
 * sys_rmdir - remove an empty directory.
 * Args: EBX = path. Returns 0 or -errno (-ENOENT, -ENOTDIR, -ENOTEMPTY).
 */
static int32_t sys_rmdir(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
    krffs_node_t *node;
    char pathbuf[256];
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    if (!krffs_fs) {
        return -ENOENT;
    }
    len = copy_path_cwd(pathbuf, sizeof(pathbuf), upath);
    if (len < 0) {
        return len;
    }
    {
        struct vfs_ref parent;
        char leaf[VFS_NAME_MAX + 1];
        void *found;
        int32_t rc;

        rc = vfs_resolve(0, pathbuf, VFS_WALK_PARENT, &parent, leaf,
                         sizeof(leaf));
        if (rc != 0) {
            return rc;
        }
        if (vfs_synth_vpath(parent.mnt, parent.node)) {
            vfs_ref_release(&parent);
            return -EROFS; /* the synthetic mounts are read-only */
        }
        if (vfs_leaf_mount(parent.mnt, parent.node, leaf)) {
            vfs_ref_release(&parent);
            return -EBUSY; /* a mount point cannot be removed */
        }
        if (!mnt_is_root(parent.mnt)) {
            rc = cred_remove_ref(&parent, leaf);
            if (rc == 0) {
                rc = vfs_rmdir(parent.mnt, parent.node, leaf);
            }
            vfs_ref_release(&parent);
            return rc;
        }
        rc = parent.mnt->type->ops->lookup(parent.mnt, parent.node, leaf,
                                           &found);
        if (rc != 0) {
            vfs_ref_release(&parent);
            return rc;
        }
        node = found;
        if (!KRFFS_IS_DIR(node->mode)) {
            vfs_ref_release(&parent);
            return -ENOTDIR;
        }
        rc = cred_access_node((krffs_node_t *)parent.node, CRED_W, 0);
        if (rc != 0) {
            vfs_ref_release(&parent);
            return rc; /* removing an entry writes its directory */
        }
        rc = cred_sticky_ok((krffs_node_t *)parent.node, node);
        if (rc != 0) {
            vfs_ref_release(&parent);
            return rc;
        }
        vfs_ns_lock(parent.mnt);
        rc = vfs_rmdir(parent.mnt, parent.node, leaf);
        vfs_ns_unlock(parent.mnt);
        vfs_ref_release(&parent);
        return rc;
    }
}

/**
 * rename(2): EBX = old path, ECX = new path. Returns 0 or -errno.
 *
 * As in `unlink`, every refusal is decided here, because `krffs_rename`
 * answers all of them with a bare -1 that userspace reads as `EPERM`. The
 * two directory cases are the ones POSIX separates and this file system
 * does not: a directory SOURCE is `-EPERM`, since a directory's children
 * reference its id and a move would orphan them, so the rename cannot be
 * performed at all; a directory DESTINATION under a plain source is
 * `-EISDIR`, the case that errno is reserved for. The rest follow the
 * reporting resolver: `-ENOENT` for a missing source or destination parent,
 * `-ENOTDIR` for a plain file used as a path component, `-EINVAL` for a
 * destination naming no entry.
 *
 * Either path naming the root is `-EINVAL`, from the empty-name check above:
 * `copy_path_cwd` canonicalizes it to "/", which loses its only component.
 * `-ENOENT` would be a lie, since the root exists, and POSIX names `EINVAL`
 * for a pathname that is the root. The identity tests below are therefore
 * unreachable today and are kept as defence: freeing the root as a replaced
 * destination would take the file system with it, and that guarantee should
 * not rest on a canonicalizer in another function.
 */
static int32_t sys_rename(isr_params_t *params)
{
    const char __user *old_path = (const char __user *)params->ebx;
    const char __user *new_path = (const char __user *)params->ecx;
    char old_buf[256];
    char new_buf[256];
    char oleaf[VFS_NAME_MAX + 1];
    char nleaf[VFS_NAME_MAX + 1];
    const char *old_name;
    const char *new_name;
    struct vfs_ref opar;
    struct vfs_ref npar;
    krffs_node_t *node;
    krffs_node_t *existing;
    void *found;
    int len;
    int32_t rc;

    if (!current_proc || !old_path || !new_path) {
        return -EFAULT;
    }

    /* Copy both paths into kernel buffers, resolving relative names against
     * cwd. */
    len = copy_path_cwd(old_buf, sizeof(old_buf), old_path);
    if (len < 0) {
        return len;
    }
    len = copy_path_cwd(new_buf, sizeof(new_buf), new_path);
    if (len < 0) {
        return len;
    }

    old_name = old_buf;
    if (old_name[0] == '/') {
        old_name++;
    }
    new_name = new_buf;
    if (new_name[0] == '/') {
        new_name++;
    }

    if (old_name[0] == '\0' || new_name[0] == '\0') {
        return -EINVAL; /* empty filename, and either path naming the root */
    }

    /* A rename writes both directories: the entry leaves one and lands in
     * the other. Both write bits are checked before the source is even
     * looked up, as unlink orders it. */
    rc = vfs_resolve(0, old_buf, VFS_WALK_PARENT, &opar, oleaf,
                     sizeof(oleaf));
    if (rc != 0) {
        return rc;
    }
    rc = mnt_is_root(opar.mnt) ? cred_access_node((krffs_node_t *)opar.node, CRED_W, 0)
                               : cred_access_ref(&opar, CRED_W, 0);
    if (rc != 0) {
        vfs_ref_release(&opar);
        return rc;
    }
    rc = vfs_resolve(0, new_buf, VFS_WALK_PARENT, &npar, nleaf,
                     sizeof(nleaf));
    if (rc != 0) {
        vfs_ref_release(&opar);
        return rc;
    }
    rc = mnt_is_root(npar.mnt) ? cred_access_node((krffs_node_t *)npar.node, CRED_W, 0)
                               : cred_access_ref(&npar, CRED_W, 0);
    if (rc != 0) {
        goto out;
    }
    if (vfs_synth_vpath(opar.mnt, opar.node) ||
        vfs_synth_vpath(npar.mnt, npar.node)) {
        rc = -EROFS;
        goto out;
    }
    /* rename is within one mount; across two it is the common layer's
     * EXDEV, and a program falls back to copy and unlink. */
    if (opar.mnt != npar.mnt) {
        rc = -EXDEV;
        goto out;
    }
    if (vfs_leaf_mount(opar.mnt, opar.node, oleaf) ||
        vfs_leaf_mount(npar.mnt, npar.node, nleaf)) {
        rc = -EBUSY; /* a mount point moves for no one */
        goto out;
    }
    if (!mnt_is_root(opar.mnt)) {
        rc = cred_remove_ref(&opar, oleaf);
        if (rc == 0) {
            rc = cred_remove_ref(&npar, nleaf);
        }
        if (rc == 0) {
            rc = vfs_rename(opar.mnt, opar.node, oleaf, npar.node, nleaf);
        }
        goto out;
    }

    rc = opar.mnt->type->ops->lookup(opar.mnt, opar.node, oleaf, &found);
    if (rc != 0) {
        goto out;
    }
    node = found;
    /* The sticky rule on the SOURCE: taking the entry out of its directory
     * is a removal, whatever it is called. */
    rc = cred_sticky_ok((krffs_node_t *)opar.node, node);
    if (rc != 0) {
        goto out;
    }
    if (node == krffs_fs->node || KRFFS_IS_DIR(node->mode)) {
        rc = -EPERM; /* a directory's children reference its id */
        goto out;
    }

    existing = NULL;
    if (npar.mnt->type->ops->lookup(npar.mnt, npar.node, nleaf, &found) ==
        0) {
        existing = found;
    }
    if (existing && existing != node) {
        /* The sticky rule on the DESTINATION, which is the half whose
         * omission leaves the attack open while a probe that only tries
         * unlink goes green: renaming over an entry removes it just as
         * surely. */
        rc = cred_sticky_ok((krffs_node_t *)npar.node, existing);
        if (rc != 0) {
            goto out;
        }
        if (existing == krffs_fs->node || KRFFS_IS_DIR(existing->mode)) {
            rc = -EISDIR;
            goto out;
        }
        /* Orphan the destination the rename is about to replace, so a
         * reader holding it keeps reading the file it opened whole. Last,
         * after every refusal above: otherwise a rename that then fails
         * would have destroyed the very file it refused to touch. With no
         * holder, the file system frees it exactly as before. */
        if (fd_node_holders(existing, NULL) > 0) {
            krffs_orphan_node(existing);
        }
    }
    vfs_ns_lock(opar.mnt);
    rc = vfs_rename(opar.mnt, opar.node, oleaf, npar.node, nleaf);
    vfs_ns_unlock(opar.mnt);
out:
    vfs_ref_release(&npar);
    vfs_ref_release(&opar);
    return rc;
}

/*
 * Timespec structure for utimensat
 */
struct kernel_timespec {
    int32_t tv_sec;
    int32_t tv_nsec;
};

/* Special nsec values */
#define UTIME_NOW  ((1L << 30) - 1L)
#define UTIME_OMIT ((1L << 30) - 2L)

/* Timer tick frequency (100 Hz) */
#define TIMER_FREQUENCY 100

/* Global tick counter from kernel.c - incremented at 100 Hz */
extern volatile uint32_t kernel_ticks;

/*
 * get_current_time - the current real time, in seconds and nanoseconds
 *
 * Answered by the clock (sys/rtc.c): the second the CMOS reported at boot plus
 * the ticks since that read, or time since boot on a machine whose clock could
 * not be read. Resolution is the 10 ms tick either way.
 */
static void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    rtc_realtime(seconds, nanoseconds);
}

/**
 * Update a file's access and modification timestamps (utimensat(2)).
 *
 * EBX = dirfd, ECX = path, EDX = times, ESI = flags. `times` is a 2-element
 * timespec array [atime, mtime]; a NULL `times` sets both to the current
 * time. A tv_nsec of UTIME_NOW means "now", UTIME_OMIT means "leave
 * unchanged". Returns 0 or -errno.
 */
static int32_t sys_utimensat(isr_params_t *params)
{
    int dirfd = (int)params->ebx;
    const char __user *user_path = (const char __user *)params->ecx;
    struct kernel_timespec __user *user_times =
        (struct kernel_timespec __user *)params->edx;
    /* int flags = (int)params->esi; */
    char pathbuf[256];
    struct kernel_timespec ktimes[2];
    const struct kernel_timespec *times = NULL;
    const char *name;
    krffs_node_t *node;
    int64_t now_sec, now_nsec;
    int len;

    if (!current_proc || !user_path) {
        return -EFAULT;
    }

    /* Copy the path (resolved against AT_FDCWD/the dirfd) and, if given, the
     * two timespecs into kernel memory through the fault-safe helpers. */
    len = copy_path_at(pathbuf, sizeof(pathbuf), dirfd, user_path);
    if (len < 0) {
        return len;
    }
    if (user_times) {
        if (copy_from_user(ktimes, user_times, sizeof(ktimes)) != 0) {
            return -EFAULT;
        }
        times = ktimes;
    }

    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }

    if (name[0] == '\0') {
        return 0; /* root timestamps are not modifiable; report success */
    }

    {
        struct vfs_ref ref;
        int rc = vfs_resolve(0, name, 0, &ref, 0, 0);

        if (rc != 0) {
            return rc; /* missing, not-a-dir, or a denied ancestor */
        }
        if (vfs_synth_vpath(ref.mnt, ref.node)) {
            vfs_ref_release(&ref);
            return -EROFS; /* the synthetic mounts are read-only */
        }
        if (!mnt_is_root(ref.mnt)) {
            struct vfs_stat vst;
            int what = 0;
            int now_both = times == NULL || (times[0].tv_nsec == UTIME_NOW &&
                                             times[1].tv_nsec == UTIME_NOW);

            rc = vfs_getattr(ref.mnt, ref.node, &vst);
            if (rc == 0 && times != NULL && times[0].tv_nsec == UTIME_OMIT &&
                times[1].tv_nsec == UTIME_OMIT) {
                vfs_ref_release(&ref);
                return 0;
            }
            if (rc == 0 && !cred_privileged() && current_proc->euid != vst.uid) {
                rc = now_both ? cred_access_ref(&ref, CRED_W, 0) : -EPERM;
            }
            if (rc == 0) {
                get_current_time(&now_sec, &now_nsec);
                if (times == NULL || times[0].tv_nsec != UTIME_OMIT) {
                    int now = times == NULL || times[0].tv_nsec == UTIME_NOW;

                    vst.atime_sec = now ? now_sec : times[0].tv_sec;
                    vst.atime_nsec = now ? now_nsec : times[0].tv_nsec;
                    what |= VFS_SET_ATIME;
                }
                if (times == NULL || times[1].tv_nsec != UTIME_OMIT) {
                    int now = times == NULL || times[1].tv_nsec == UTIME_NOW;

                    vst.mtime_sec = now ? now_sec : times[1].tv_sec;
                    vst.mtime_nsec = now ? now_nsec : times[1].tv_nsec;
                    what |= VFS_SET_MTIME;
                }
                rc = vfs_setattr(ref.mnt, ref.node, what, &vst);
            }
            vfs_ref_release(&ref);
            return rc;
        }
        node = ref.node;
        /* The hold is a no-op for the root type and the times are stored
         * before anything else can run, so the ref is returned here. */
        vfs_ref_release(&ref);
        if (node == krffs_fs->node) {
            return 0; /* root timestamps are not modifiable; report success */
        }
    }

    /* The POSIX permission split. Both UTIME_OMIT changes nothing, so it
     * needs no right and touches no timestamp (not even ctime). Setting both
     * to "now" (times NULL, or both UTIME_NOW) is the owner's, or anyone with
     * write, or root. Any explicit time is the owner's or root's alone. */
    if (times != NULL &&
        times[0].tv_nsec == UTIME_OMIT && times[1].tv_nsec == UTIME_OMIT) {
        return 0;
    }
    if (times == NULL ||
        (times[0].tv_nsec == UTIME_NOW && times[1].tv_nsec == UTIME_NOW)) {
        if (!cred_privileged() && current_proc->euid != node->uid &&
            cred_access_node(node, CRED_W, 0) != 0) {
            return -EACCES;
        }
    } else if (!cred_privileged() && current_proc->euid != node->uid) {
        return -EPERM;
    }

    get_current_time(&now_sec, &now_nsec);

    if (times == NULL) {
        node->access_time.seconds = now_sec;
        node->access_time.nanoseconds = now_nsec;
        node->modification_time.seconds = now_sec;
        node->modification_time.nanoseconds = now_nsec;
    } else {
        if (times[0].tv_nsec == UTIME_OMIT) {
            /* leave atime unchanged */
        } else if (times[0].tv_nsec == UTIME_NOW) {
            node->access_time.seconds = now_sec;
            node->access_time.nanoseconds = now_nsec;
        } else {
            node->access_time.seconds = times[0].tv_sec;
            node->access_time.nanoseconds = times[0].tv_nsec;
        }

        if (times[1].tv_nsec == UTIME_OMIT) {
            /* leave mtime unchanged */
        } else if (times[1].tv_nsec == UTIME_NOW) {
            node->modification_time.seconds = now_sec;
            node->modification_time.nanoseconds = now_nsec;
        } else {
            node->modification_time.seconds = times[1].tv_sec;
            node->modification_time.nanoseconds = times[1].tv_nsec;
        }
    }

    /* ctime always advances on a metadata change. */
    node->attribute_change_time.seconds = now_sec;
    node->attribute_change_time.nanoseconds = now_nsec;

    return 0;
}

/*
 * Linux dirent64 structure for getdents64 syscall
 */
struct linux_dirent64 {
    uint64_t       d_ino;     /* 64-bit inode number */
    int64_t        d_off;     /* offset to next entry */
    uint16_t       d_reclen;  /* Size of this dirent */
    uint8_t        d_type;    /* File type */
    char           d_name[1]; /* Filename (variable length, null-terminated) */
};

/* Directory entry types */
#define DT_REG  8   /* Regular file */
#define DT_DIR  4   /* Directory */
#ifndef DT_CHR
#define DT_CHR 2
#endif
#ifndef DT_LNK
#define DT_LNK 10
#endif

/* Format an unsigned decimal into out (NUL-terminated). */
static void synth_u32_str(char *out, uint32_t v)
{
    char tmp[12];
    int n = 0;
    int i = 0;

    if (v == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    while (v) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n) {
        out[i++] = tmp[--n];
    }
    out[i] = '\0';
}

/*
 * The n-th entry of a synthetic directory (kind/pid from the dir fd's
 * cookie): fills name/ino/dtype and returns 1, or 0 past the end. "." and
 * ".." are entries 0 and 1 of every directory.
 */
static int synth_dirent_at(int kind, int pid, int n, char *name, uint64_t *ino, uint8_t *dtype)
{
    if (n == 0) {
        name[0] = '.';
        name[1] = '\0';
        *ino = 1;
        *dtype = DT_DIR;
        return 1;
    }
    if (n == 1) {
        name[0] = '.';
        name[1] = '.';
        name[2] = '\0';
        *ino = 1;
        *dtype = DT_DIR;
        return 1;
    }
    n -= 2;

    if (kind == VNODE_DEV_DIR) {
        const char *dn = NULL;
        uint32_t dino = 0;
        int i;

        /* Through devfs_dirent, which skips absent entries and reports the
         * registry inode: two devices are dynamically present now, so a walk
         * that stopped at the first absent one would hide the second. */
        if (!devfs_dirent(n, &dn, &dino) || !dn) {
            return 0;
        }
        for (i = 0; i < 30 && dn[i]; i++) {
            name[i] = dn[i];
        }
        name[i] = '\0';
        *ino = dino;
        *dtype = DT_CHR;
        return 1;
    }
    if (kind == VNODE_PTS_DIR) {
        /* /dev/pts: one entry per allocated pty slave, named by its index. */
        int i, c = 0;
        for (i = 0; i < PTY_MAX; i++) {
            if (pty_is_allocated(i)) {
                if (c == n) {
                    synth_u32_str(name, (uint32_t)i);
                    *ino = PTS_INO_BASE + (uint32_t)i;
                    *dtype = DT_CHR;
                    return 1;
                }
                c++;
            }
        }
        return 0;
    }
    /* /proc/net has one file. */
    if (kind == VNODE_PROC_NET_DIR) {
        /* No `tcp6`: there is no IPv6 here, and a file that existed and was
         * empty would tell netstat the family is present with nothing on it. */
        static const char *const netfiles[] = {"dev", "route", "arp", "tcp"};
        int count = (int)(sizeof(netfiles) / sizeof(netfiles[0]));

        if (n < count) {
            int i;

            for (i = 0; netfiles[n][i] != '\0'; i++) {
                name[i] = netfiles[n][i];
            }
            name[i] = '\0';
            *dtype = DT_REG;
            if (n == 0) {
                *ino = procfs_ino_netdev();
            } else if (n == 1) {
                *ino = procfs_ino_netroute();
            } else if (n == 2) {
                *ino = procfs_ino_netarp();
            } else {
                *ino = procfs_ino_nettcp();
            }
            return 1;
        }
        return 0;
    }
    if (kind == VNODE_PROC_DIR) {
        /* The machine's own files come first, in a table so that adding one is
         * a line here and not another index in a chain; the pid directories
         * follow, numbered from where the table ends. */
        static const char *const globals[] = {"cmdline", "disk",    "uptime", "stat",
                                              "meminfo", "mounts",  "net",    "self",
                                              "usb"};
        int count = (int)(sizeof(globals) / sizeof(globals[0]));

        if (n < count) {
            int i;

            for (i = 0; globals[n][i] != '\0'; i++) {
                name[i] = globals[n][i];
            }
            name[i] = '\0';
            *dtype = DT_REG;
            if (n == 0) {
                *ino = procfs_ino_cmdline();
            } else if (n == 1) {
                *ino = procfs_ino_disk();
            } else if (n == 2) {
                *ino = procfs_ino_uptime();
            } else if (n == 3) {
                *ino = procfs_ino_statsys();
            } else if (n == 4) {
                *ino = procfs_ino_meminfo();
            } else if (n == 5) {
                *ino = procfs_ino_mounts();
            } else if (n == 6) {
                *ino = procfs_ino_netdir();
                *dtype = DT_DIR;
            } else {
                *ino = procfs_ino_dir();
                *dtype = DT_LNK;
            }
            return 1;
        }
        {
            int p = procfs_nth_pid(n - count);
            if (p < 0) {
                return 0;
            }
            synth_u32_str(name, (uint32_t)p);
            *ino = procfs_ino_pid(p);
            *dtype = DT_DIR;
            return 1;
        }
    }
    if (kind == VNODE_PROC_PID) {
        static const char *files[3] = {"stat", "cmdline", "fd"};
        process_t *pp = get_process((uint32_t)pid);
        int i;
        if (!pp || pp->state == PROC_UNUSED) {
            return 0; /* the process exited: an empty directory (just . and ..) */
        }
        if (n >= 3) {
            return 0;
        }
        for (i = 0; files[n][i]; i++) {
            name[i] = files[n][i];
        }
        name[i] = '\0';
        *ino = procfs_ino_pid(pid);
        *dtype = (n == 2) ? (uint8_t)DT_DIR : (uint8_t)DT_REG;
        return 1;
    }
    if (kind == VNODE_PROC_FD_DIR) {
        process_t *pp = get_process((uint32_t)pid);
        int i, c = 0;
        if (!pp) {
            return 0;
        }
        for (i = 0; i < MAX_FDS; i++) {
            if (pp->fds.entries[i].type != FD_TYPE_NONE) {
                if (c == n) {
                    synth_u32_str(name, (uint32_t)i);
                    *ino = procfs_ino_pid(pid);
                    *dtype = DT_LNK;
                    return 1;
                }
                c++;
            }
        }
        return 0;
    }
    return 0;
}

/* getdents for a synthetic directory fd: pack records from synth_dirent_at,
 * advancing the fd's offset cursor (same record layout as the KRFFS path). */
static int32_t synth_getdents(fd_entry_t *e, char *buf, uint32_t count)
{
    struct synth_dir *sd = (struct synth_dir *)e->priv;
    int kind = sd ? sd->kind : VNODE_ENOENT;
    int pid = sd ? sd->pid : 0;
    uint32_t written = 0;
    uint32_t idx = e->ofd->offset;
    char name[32];
    uint64_t ino;
    uint8_t dtype;

    while (synth_dirent_at(kind, pid, (int)idx, name, &ino, &dtype)) {
        uint8_t recbuf[64];
        uint32_t name_len = k_strlen(name);
        uint32_t rec_len = sizeof(uint64_t) + sizeof(int64_t) + sizeof(uint16_t) +
                           sizeof(uint8_t) + name_len + 1;
        struct linux_dirent64 *d;

        rec_len = (rec_len + 7) & ~7u;
        if (written + rec_len > count) {
            if (written == 0) {
                return -EINVAL; /* buffer too small for even one entry */
            }
            break;
        }
        k_memset(recbuf, 0, rec_len);
        d = (struct linux_dirent64 *)recbuf;
        d->d_ino = ino;
        d->d_off = (int64_t)(idx + 1);
        d->d_reclen = (uint16_t)rec_len;
        d->d_type = dtype;
        k_memcpy(d->d_name, name, name_len + 1);
        if (copy_to_user((void __user *)(buf + written), recbuf, rec_len) != 0) {
            return written > 0 ? (int32_t)written : -EFAULT;
        }
        written += rec_len;
        idx++;
        e->ofd->offset = idx;
    }
    return (int32_t)written;
}

/**
 * Read directory entries (getdents64(2)).
 *
 * EBX = fd, ECX = dirp buffer, EDX = count. Packs linux_dirent64 records
 * into the buffer, one per child, and advances the fd's offset cursor.
 * Returns the bytes written, 0 at end of directory, or -errno.
 */
static int32_t sys_getdents64(isr_params_t *params)
{
    int fd_num = (int)params->ebx;
    char *buf = (char *)params->ecx;
    uint32_t count = params->edx;
    fd_entry_t *entry;
    uint32_t bytes_written = 0;
    uint32_t name_len;
    uint32_t rec_len;
    struct linux_dirent64 *dirent;

    if (!current_proc || !buf || count == 0) {
        return -EFAULT;
    }

    /* Records are packed straight into the user buffer below, so range-check
     * AND fault in the whole [buf, buf+count) span once; every record write
     * then stays inside present, validated user memory. */
    if (!user_buffer_ok((uint32_t)buf, count, 1)) {
        return -EFAULT;
    }

    entry = fd_get(&current_proc->fds, fd_num);
    if (!entry) {
        return -EBADF;
    }

    /* A synthetic directory (/dev, /proc, /proc/<pid>[/fd]) lists from the
     * process table or device registry, not KRFFS. Bare `ls /` does not merge
     * these mount names; they are reached by their absolute paths. */
    if (entry->type == FD_TYPE_SYNTH_DIR) {
        return synth_getdents(entry, buf, count);
    }

    if (entry->type != FD_TYPE_DIR) {
        return -ENOTDIR;
    }

    if (!krffs_fs) {
        return -EIO;
    }
    if (fd_node_check(entry) != 0) {
        return -EBADF;  /* the directory was removed out from under this fd */
    }

    /*
     * The cursor is the byte offset of the last node returned, plus one, and
     * NOT a count of the children seen so far. That distinction is the whole of
     * a bug this had: `rm -rf` reads a batch of names, unlinks them, and reads
     * again, and a cursor that counted children renumbered every surviving
     * child when one was removed, so the second read skipped as many entries as
     * the first read had deleted. The files left behind then made the directory
     * refuse to be removed, with `rm` reporting only that it was not empty
     * (tests/unlinkdirprobe.c).
     *
     * What an offset guarantees, exactly. Deleting an entry does not move any
     * other node: `krffs_remove_reserved_node` only widens free extents, and
     * allocation splits a free node instead of shifting a reserved one. So a
     * listing interleaved with deletions, which is the case that matters and
     * the one `rm` performs, sees every surviving entry once.
     *
     * What it does not guarantee. Growing a file CAN move its node: when the
     * following extent is not free and large enough,
     * `krffs_resize_reserved_node` allocates elsewhere, copies, and frees
     * the old node. A child that grows that way while another descriptor is
     * mid-listing lands at a new offset, and is missed if that offset is
     * below the cursor or repeated if above. The same is true of a file
     * created during a listing in space a removed one freed. Both are
     * concurrent modifications of a directory being read, which POSIX leaves
     * unspecified, and neither is reachable from the loop this exists for;
     * the previous count-based cursor failed the case that was reachable.
     */
    {
        struct open_file *ofd = entry->ofd;
        uint32_t cursor;

        if (!ofd || !ofd->vmnt) {
            return -EBADF;
        }
        fd_ofd_lock(ofd);
        cursor = ofd->offset;
        for (;;) {
            struct vfs_dirent de;
            uint8_t recbuf[VFS_NAME_MAX + 1 + 32];
            int got = vfs_readdir(ofd->vmnt, ofd->fhandle, &cursor, &de);

            if (got == 0) {
                break;
            }
            if (got < 0) {
                fd_ofd_unlock(ofd);
                return (bytes_written > 0) ? (int32_t)bytes_written : got;
            }
            name_len = k_strlen(de.name);
            /* Fixed fields (19 bytes) + name + NUL, padded to 8-byte
             * align. */
            rec_len = sizeof(uint64_t) + sizeof(int64_t) + sizeof(uint16_t) +
                      sizeof(uint8_t) + name_len + 1;
            rec_len = (rec_len + 7) & ~7;
            if (bytes_written + rec_len > count) {
                if (bytes_written == 0) {
                    return -EINVAL; /* buffer too small for even one entry */
                }
                break; /* the uncommitted cursor re-reads this entry next */
            }
            /* Build the record in kernel memory, then copy it out through
             * the helper (no raw write into user space). */
            k_memset(recbuf, 0, rec_len);
            dirent = (struct linux_dirent64 *)recbuf;
            dirent->d_ino = de.ino;
            dirent->d_off = (int64_t)cursor; /* next position */
            dirent->d_reclen = (uint16_t)rec_len;
            dirent->d_type = de.is_dir ? DT_DIR : DT_REG;
            k_memcpy(dirent->d_name, de.name, name_len + 1);
            if (copy_to_user((void __user *)(buf + bytes_written), recbuf,
                             rec_len) != 0) {
                fd_ofd_unlock(ofd);
                return (bytes_written > 0) ? (int32_t)bytes_written : -EFAULT;
            }
            bytes_written += rec_len;
            ofd->offset = cursor; /* committed with the record, not before */
        }
        fd_ofd_unlock(ofd);
    }

    /* Nothing written after scanning every node is the end of the directory. A
     * buffer too small for even one record returned -EINVAL above, so zero
     * here has only the one meaning. */
    return (int32_t)bytes_written;
}

/*
 * timeval structure for gettimeofday
 */
struct kernel_timeval {
    int32_t tv_sec;   /* seconds */
    int32_t tv_usec;  /* microseconds */
};

/*
 * gettimeofday(2): EBX = tv (timeval*), ECX = tz (ignored). Reports real time
 * from the clock anchored at boot (sys/rtc.c), or time since boot when there
 * was no clock to read. Returns 0 or -errno.
 */
static int32_t sys_gettimeofday(isr_params_t *params)
{
    struct kernel_timeval __user *tv = (struct kernel_timeval __user *)params->ebx;
    struct kernel_timeval ktv;

    if (!tv) {
        return -EFAULT;
    }

    /* Build the result in a kernel struct, then copy it out (this also closes a
     * previously unchecked raw write to the user pointer). */
    {
        int64_t seconds;
        int64_t nanoseconds;

        get_current_time(&seconds, &nanoseconds);
        ktv.tv_sec = (int32_t)seconds;
        /* Narrowed before the divide: a 64-bit division would call __divdi3,
         * and this kernel links no libgcc. A nanosecond field is under 1e9. */
        ktv.tv_usec = (int32_t)nanoseconds / 1000;
    }

    if (copy_to_user(tv, &ktv, sizeof(ktv)) != 0) {
        return -EFAULT;
    }

    return 0;
}

/**
 * Read a POSIX clock (clock_gettime, syscall 265, 32-bit timespec).
 *
 * EBX = clockid, ECX = struct timespec*. CLOCK_REALTIME(0) reports real time
 * from the clock read at boot (sys/rtc.c); CLOCK_MONOTONIC(1) reports time
 * since boot, which is what the scheduler, the sleep path and the audio timing
 * loops mean by a clock. Both run at the 10 ms tick resolution, and neither is
 * disciplined by anything: the machine reads the CMOS once and then keeps its
 * own time. musl falls back here from clock_gettime64 (403), which this kernel
 * does not provide, so times are 32-bit here and this ABI ends in 2038.
 */
static int32_t sys_clock_gettime(isr_params_t *params)
{
    int clk = (int)params->ebx;
    struct kernel_timespec __user *ts = (struct kernel_timespec __user *)params->ecx;
    struct kernel_timespec kts;
    uint32_t ticks;

    if (!ts) {
        return -EFAULT;
    }
    if (clk != 0 && clk != 1) { /* CLOCK_REALTIME / CLOCK_MONOTONIC only */
        return -EINVAL;
    }

    if (clk == 0) {
        int64_t seconds;
        int64_t nanoseconds;

        get_current_time(&seconds, &nanoseconds);
        kts.tv_sec = (int32_t)seconds;
        kts.tv_nsec = (int32_t)nanoseconds;
    } else {
        ticks = kernel_ticks;
        kts.tv_sec = (int32_t)(ticks / 100);
        kts.tv_nsec = (int32_t)((ticks % 100) * 10000000); /* 1e7 ns per tick */
    }

    if (copy_to_user(ts, &kts, sizeof(kts)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * Block-and-schedule sleep (nanosleep, syscall 162, 32-bit timespec).
 *
 * EBX = const struct timespec* req, ECX = struct timespec* rem (optional).
 * Rounds up to whole 10 ms ticks, then parks the process BLOCKED on its
 * wakeup_tick and yields; the timer wakes it at the deadline while OTHER
 * processes keep running. rem is written as zero on normal completion, or
 * the time left on -EINTR.
 */
static int32_t sys_nanosleep(isr_params_t *params)
{
    const struct kernel_timespec __user *ureq =
        (const struct kernel_timespec __user *)params->ebx;
    struct kernel_timespec __user *urem =
        (struct kernel_timespec __user *)params->ecx;
    struct kernel_timespec req;
    uint32_t ticks, target;

    if (!current_proc || !ureq) {
        return -EFAULT;
    }
    if (copy_from_user(&req, ureq, sizeof(req)) != 0) {
        return -EFAULT;
    }
    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000) {
        return -EINVAL;
    }

    /* Convert to whole 10 ms ticks without 64-bit division (the freestanding
     * kernel has no libgcc __udivdi3): 1 s == 100 ticks, plus the ceil of the
     * sub-second nanoseconds. Absurd durations are clamped so sec*100 cannot
     * overflow and the signed tick comparison below stays valid. */
    {
        uint32_t sec = (uint32_t)req.tv_sec;
        uint32_t nsec_ticks = ((uint32_t)req.tv_nsec + 9999999u) / 10000000u;
        if (sec > 21474836u) {
            sec = 21474836u; /* keeps the intermediate sec*100 below 2^31 */
        }
        ticks = sec * 100u + nsec_ticks;
        if (ticks > 0x7FFFFFFFu) {
            ticks = 0x7FFFFFFFu; /* keep the signed (target-now) delta valid */
        }
    }
    if (ticks == 0 && (req.tv_sec != 0 || req.tv_nsec != 0)) {
        ticks = 1; /* any nonzero request waits at least one tick */
    }

    target = kernel_ticks + ticks;
    if (target == 0) {
        target = 1; /* 0 is the "not sleeping" sentinel; never store it as a deadline */
    }
    while ((int32_t)(target - kernel_ticks) > 0) {
        current_proc->wakeup_tick = target;
        current_proc->state = PROC_BLOCKED;
        schedule(); /* yield to any other READY process; the timer wakes us */
        /* A signal cuts the sleep short: stop sleeping, report the time left,
         * and return -EINTR so it is acted on when the syscall returns to user.
         */
        if (signal_pending()) {
            uint32_t left = ((int32_t)(target - kernel_ticks) > 0)
                                ? (target - kernel_ticks) : 0u;
            current_proc->wakeup_tick = 0;
            current_proc->state = PROC_RUNNING;
            if (urem) {
                struct kernel_timespec rem;
                rem.tv_sec = (int32_t)(left / 100u);
                rem.tv_nsec = (int32_t)((left % 100u) * 10000000u);
                (void)copy_to_user(urem, &rem, sizeof(rem));
            }
            return -EINTR;
        }
        if ((int32_t)(target - kernel_ticks) > 0) {
            /* schedule() found nobody else to run: idle until the next
             * interrupt (the tick), not busy-spinning on the scheduler.
             */
            idle_wait_irq_on();
        }
    }
    current_proc->wakeup_tick = 0;
    current_proc->state = PROC_RUNNING;

    if (urem) {
        struct kernel_timespec zero;
        zero.tv_sec = 0;
        zero.tv_nsec = 0;
        if (copy_to_user(urem, &zero, sizeof(zero)) != 0) {
            return -EFAULT;
        }
    }
    return 0;
}

/*
 * Identity (the i386 *32 variants, 199-202): the four getters read the PCB
 * credentials, all zero (root) until a login server drops them. The setters
 * and the group list live with the rest of the id family near do_chown.
 */
static int32_t sys_getuid(isr_params_t *params)
{
    (void)params;
    return (int32_t) current_proc->ruid;
}
static int32_t sys_geteuid(isr_params_t *params)
{
    (void)params;
    return (int32_t) current_proc->euid;
}
static int32_t sys_getgid(isr_params_t *params)
{
    (void)params;
    return (int32_t) current_proc->rgid;
}
static int32_t sys_getegid(isr_params_t *params)
{
    (void)params;
    return (int32_t) current_proc->egid;
}

/* getrandom flag bits (Linux). We never block and have one source, so they only
 * gate validation here. */
#define GRND_NONBLOCK 0x0001u
#define GRND_RANDOM   0x0002u
#define GRND_INSECURE 0x0004u

/**
 * Fill a buffer with random bytes (getrandom, syscall 355).
 *
 * EBX = buf, ECX = buflen, EDX = flags. Backed by the kernel CSPRNG
 * (lib/krandom, ChaCha20). Returns the byte count, like Linux; musl's
 * getrandom/arc4random sit on top of this. We never block (GRND_NONBLOCK is
 * a no-op) and have a single source (GRND_RANDOM is equivalent); unknown
 * flag bits are rejected.
 */
static int32_t sys_getrandom(isr_params_t *params)
{
    void __user *buf = (void __user *)params->ebx;
    uint32_t len = params->ecx;
    uint32_t flags = params->edx;
    uint8_t kbuf[256];
    uint32_t done = 0;

    if (flags & ~(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE)) {
        return -EINVAL; /* unknown flag bits, as Linux rejects */
    }
    if (len > 0x7FFFFFFFu) {
        return -EINVAL; /* keep the byte count representable as a positive int32 */
    }
    if (len == 0) {
        return 0;
    }
    if (!buf) {
        return -EFAULT;
    }
    /* Fail closed: with no trusted hardware entropy (RDSEED/RDRAND), refuse,
     * not hand out the weak boot-jitter stream for a key. A caller that
     * explicitly accepts insecure bytes passes GRND_INSECURE (the same tier as
     * AT_RANDOM / the stack canary). */
    if (!krandom_is_trusted() && !(flags & GRND_INSECURE)) {
        return -EIO;
    }
    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > sizeof(kbuf)) {
            chunk = sizeof(kbuf);
        }
        krandom_bytes(kbuf, chunk);
        if (copy_to_user((uint8_t __user *)buf + done, kbuf, chunk) != 0) {
            return (done > 0) ? (int32_t)done : -EFAULT;
        }
        done += chunk;
    }
    return (int32_t)done;
}

/* ===========================================================================
 * Signals: per-process disposition + pending/blocked masks, delivery on
 * return-to-user via a frame on the user stack, and rt_sigreturn.
 * ======================================================================== */

extern void terminate_current_process(int32_t signo);

/* The default action for a signal whose disposition is SIG_DFL. */
static enum ksig_default ksig_default_action(int signo)
{
    switch (signo) {
        case SIGCHLD:
        case SIGURG:
        case SIGWINCH:
        case SIGCONT:
            return KSIG_DEFAULT_IGNORE;
        case SIGSTOP:
        case SIGTSTP:
        case SIGTTIN:
        case SIGTTOU:
            return KSIG_DEFAULT_STOP;
        default:
            return KSIG_DEFAULT_TERM;
    }
}

/* The 64-bit sa_mask the user installed (two 32-bit words, signals 1..64). */
static uint64_t sigact_mask(const struct k_sigaction *sa)
{
    return (uint64_t)sa->mask[0] | ((uint64_t)sa->mask[1] << 32);
}

/* Coerce a restored data/TLS selector: keep it only if it is a ring-3 (RPL 3)
 * selector (so %gs == 0x33 TLS survives), else force flat user data 0x23. */
static uint32_t safe_user_seg(uint32_t sel)
{
    return ((sel & 3u) == 3u) ? (sel & 0xFFFFu) : 0x23u;
}

/* The signal masks are 64-bit, so a read-modify-write is two stores on i386;
 * signal_post runs from IRQ context (Ctrl-C) as well as syscalls, so guard
 * every pending/blocked mutation with interrupts off to avoid a lost update.
 * Returns the prior IF so the caller can restore it. */
static uint32_t sig_irq_save(void)
{
    uint32_t flags;
    __asm__ __volatile__("pushf\n\t"
                         "cli\n\t"
                         "pop %0"
                         : "=r"(flags)::"memory");
    return flags;
}

static void sig_irq_restore(uint32_t flags)
{
    if (flags & 0x200u) {
        __asm__ __volatile__("sti" ::: "memory");
    }
}

/*
 * Post signal `signo` to `target`: set its pending bit. If the target is
 * blocked in the scheduler (sleeping/waiting), make it runnable so it
 * reaches a return-to-user delivery point. SIGKILL/SIGSTOP cannot be caught,
 * so a pending SIGKILL is honored even before delivery by leaving it pending
 * for the default path. (Interrupting a blocked syscall with -EINTR is
 * deferred; targets here are running or self.)
 */
void signal_post(process_t *target, int signo)
{
    if (!target || signo < 1 || signo > KSIG_MAX) {
        return;
    }
    if (target->state == PROC_ZOMBIE || target->state == PROC_UNUSED) {
        return;
    }
    {
        uint32_t f = sig_irq_save();
        target->sig_pending |= KSIG_BIT(signo);
        if (target->state == PROC_BLOCKED) {
            target->wakeup_tick = 0;
            target->state = PROC_READY;
        }
        /* Job control: SIGCONT cancels any pending stop and resumes a stopped
         * process (its parent learns via wait4(WCONTINUED)); SIGKILL wakes a
         * stopped process so it can take its lethal default. Every other signal
         * stays pending on a stopped process until it is continued. */
        if (signo == SIGCONT) {
            target->sig_pending &= ~(KSIG_BIT(SIGSTOP) | KSIG_BIT(SIGTSTP) |
                                     KSIG_BIT(SIGTTIN) | KSIG_BIT(SIGTTOU));
            if (target->state == PROC_STOPPED) {
                process_t *parent = get_process(target->parent_pid);
                target->state = PROC_READY;
                target->jc_status = 0xffff; /* WIFCONTINUED */
                if (parent) {
                    parent->sig_pending |= KSIG_BIT(SIGCHLD);
                    if (parent->state == PROC_BLOCKED) {
                        parent->wakeup_tick = 0;
                        parent->state = PROC_READY;
                    }
                }
            }
        } else if (signo == SIGKILL && target->state == PROC_STOPPED) {
            target->state = PROC_READY;
        } else if ((signo == SIGSTOP || signo == SIGTSTP || signo == SIGTTIN ||
                    signo == SIGTTOU)) {
            target->sig_pending &= ~KSIG_BIT(SIGCONT); /* a stop cancels pending cont */
        }
        sig_irq_restore(f);
    }
}

/* Non-zero if the current process has a deliverable (pending, unblocked)
 * signal. Blocking syscall loops poll this so a signal interrupts the wait (the
 * signal is then delivered/acts on the syscall's return to user). */
int signal_pending(void)
{
    uint64_t deliverable;
    uint32_t flags;
    int signo;

    if (!current_proc) {
        return 0;
    }
    /* Snapshot the 64-bit masks with IRQs off: on i386 a 64-bit read is two
     * 32-bit loads, and a signal posted from an IRQ in between could tear it.
     */
    flags = sig_irq_save();
    deliverable = current_proc->sig_pending & ~current_proc->sig_blocked;
    sig_irq_restore(flags);
    /* Only a signal that would ACT interrupts a wait: a caught handler or a
     * default-terminate. A pending signal that is ignored (SIG_IGN or a
     * default-ignore like SIGCHLD) must NOT cause -EINTR. */
    for (signo = 1; signo <= KSIG_MAX; signo++) {
        struct k_sigaction *sa;

        if (!(deliverable & KSIG_BIT(signo))) {
            continue;
        }
        sa = &current_proc->sigacts[signo];
        if (sa->handler == KSIG_IGN) {
            continue;
        }
        if (sa->handler == KSIG_DFL && ksig_default_action(signo) == KSIG_DEFAULT_IGNORE) {
            continue; /* default ignore (SIGCHLD/SIGCONT/...): does not interrupt */
        }
        return 1; /* caught, or default terminate/stop: it acts, so it interrupts */
    }
    return 0;
}

/**
 * The signal-permission rule (kill(2)), judged per target and per signal.
 *
 * A privileged sender passes. `SIGCONT` passes within one session, whoever owns
 * the target. Otherwise either of the sender's ids, real or effective, must
 * match either of the target's, real or saved.
 *
 * The saved-uid arm is what POSIX requires and what makes a set-id program
 * signallable by the identity that launched it. The session exception is what
 * makes job control work at all: `/bin/su` is 04755 and calls `setuid`, so the
 * shell it execs runs with real uid 0, and ash implements `fg` as
 * `killpg(pgid, SIGCONT)` and ignores the result. Without the exception a
 * uid-1000 shell cannot resume a job it started through `su`: the `killpg`
 * fails silently and the shell waits forever on a process that will never run.
 *
 * The exception cannot extend a sender's reach beyond its own session, because
 * `setsid` only ever creates a session and never joins one, so a process cannot
 * move into a session to reach something. What it does expose is narrow and
 * worth naming: a member of a session may make a root process in that same
 * session run its `SIGCONT` handler at a time of the member's choosing.
 *
 * The signo 0 existence probe keeps the strict rule. The exception exists for
 * resuming a stopped job, not for discovering what is running, and POSIX asks
 * for permission on the probe too. `tty_signal_group` stays unchecked: those
 * signals come from the terminal, not from a process.
 */
static int cred_may_signal(const process_t *target, int signo)
{
    if (current_proc->euid == 0) {
        return 1;
    }
    if (signo == SIGCONT && current_proc->sid == target->sid) {
        return 1;
    }
    return current_proc->ruid == target->ruid ||
           current_proc->ruid == target->suid ||
           current_proc->euid == target->ruid ||
           current_proc->euid == target->suid;
}

/* Post signo to a process group: pid==0 is the caller's group, pid<-1 the group
 * (-pid). Signals every member the caller may signal; the POSIX outcome: 0 if
 * any member took it, -EPERM if members exist but none may be signalled,
 * -ESRCH if the group is empty. signo 0 only probes. */
static int32_t kill_group(int pgid, int signo)
{
    int found = 0;
    int allowed = 0;
    int i;

    for (i = 0; i < MAX_PROCS; i++) {
        process_t *p = &proc_table[i];
        if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE) {
            continue;
        }
        if ((int)p->pgid != pgid) {
            continue;
        }
        found = 1;
        if (!cred_may_signal(p, signo)) {
            continue;
        }
        allowed = 1;
        if (signo != 0) {
            signal_post(p, signo);
        }
    }
    if (!found) {
        return -ESRCH;
    }
    return allowed ? 0 : -EPERM;
}

/* kill(2): pid > 0 a single process; pid == 0 the caller's process group; pid <
 * -1 the group (-pid). (pid == -1 "all processes" is not supported here.) signo
 * 0 just probes existence and permission. */
static int32_t do_kill(int pid, int signo)
{
    process_t *target;

    if (signo < 0 || signo > KSIG_MAX) {
        return -EINVAL;
    }
    if (pid == 0) {
        return current_proc ? kill_group((int)current_proc->pgid, signo) : -ESRCH;
    }
    if (pid < -1) {
        if (pid == -2147483647 - 1) {
            return -ESRCH; /* INT_MIN: -pid would overflow */
        }
        return kill_group(-pid, signo);
    }
    if (pid == -1) {
        return -ESRCH; /* broadcast-to-all is not supported */
    }
    target = get_process((uint32_t)pid);
    if (!target || target->state == PROC_UNUSED || target->state == PROC_ZOMBIE) {
        return -ESRCH;
    }
    if (!cred_may_signal(target, signo)) {
        return -EPERM;
    }
    if (signo == 0) {
        return 0; /* existence and permission check only */
    }
    signal_post(target, signo);
    return 0;
}

static int32_t sys_kill(isr_params_t *params)
{
    return do_kill((int)params->ebx, (int)params->ecx);
}

static int32_t sys_tkill(isr_params_t *params)
{
    /* tkill(tid, sig): tid == pid here. A tid must be a positive thread id;
     * unlike kill(2), a zero or negative value is an error, not a group
     * selector, so reject it before do_kill would read it as one. */
    if ((int)params->ebx <= 0) {
        return -EINVAL;
    }
    return do_kill((int)params->ebx, (int)params->ecx);
}

static int32_t sys_tgkill(isr_params_t *params)
{
    int tgid = (int)params->ebx;
    int tid = (int)params->ecx;
    int sig = (int)params->edx;
    process_t *target;

    /* tgkill(tgid, tid, sig): both ids must be positive (a tid is never a
     * kill(2) group selector, so a zero or negative one is an error, not a
     * group), and the tgid must name the same single process as the tid, one
     * thread per process here. */
    if (tgid <= 0 || tid <= 0) {
        return -EINVAL;
    }
    target = get_process((uint32_t)tid);
    if (!target || target->state == PROC_UNUSED || target->state == PROC_ZOMBIE ||
        (uint32_t)tgid != target->pid) {
        return -ESRCH;
    }
    return do_kill(tid, sig);
}

/* Block until a deliverable signal is pending. Lost-wakeup-safe: the pending
 * check and the BLOCKED transition are one interrupts-off critical section, so
 * a signal posted from an IRQ either is seen here or finds us BLOCKED and wakes
 * us (the wait_queue.c discipline). IF is re-enabled only while
 * yielding/idling. */
static void sig_wait(void)
{
    uint32_t flags = sig_irq_save();

    while (!signal_pending()) {
        current_proc->state = PROC_BLOCKED;
        sig_irq_restore(flags);
        schedule();
        flags = sig_irq_save();
        if (current_proc->state == PROC_BLOCKED) {
            idle_wait_irq_off();
        }
    }
    current_proc->state = PROC_RUNNING;
    sig_irq_restore(flags);
}

/*
 * sys_pause - sleep until a signal is delivered (pause(2)). Returns -EINTR
 * once a deliverable signal is pending (the signal then acts on the return
 * to user).
 */
static int32_t sys_pause(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
    sig_wait();
    return -EINTR;
}

/**
 * Atomically install a signal mask and wait (rt_sigsuspend(2)).
 *
 * EBX = const sigset_t *mask, ECX = sigsetsize (8). Replaces the blocked mask
 * with *mask, waits for a deliverable signal, restores the old mask, then
 * returns -EINTR. The handler runs under the RESTORED mask: a simplification,
 * since full save-the-mask-through-the-handler semantics are not modelled.
 */
static int32_t sys_rt_sigsuspend(isr_params_t *params)
{
    const void __user *umask = (const void __user *)params->ebx;
    uint32_t sigsetsize = params->ecx;
    uint64_t newmask = 0;
    uint64_t oldmask;

    if (!current_proc) {
        return -ESRCH;
    }
    if (sigsetsize != sizeof(uint64_t)) {
        return -EINVAL;
    }
    if (copy_from_user(&newmask, umask, sizeof(newmask)) != 0) {
        return -EFAULT;
    }
    newmask &= ~(uint64_t)KSIG_UNCATCHABLE; /* SIGKILL/SIGSTOP can never be blocked */
    oldmask = current_proc->sig_blocked;
    current_proc->sig_blocked = newmask;
    sig_wait();
    current_proc->sig_blocked = oldmask;
    return -EINTR;
}

/**
 * Install or query a signal disposition (rt_sigaction, syscall 174).
 *
 * EBX=signo, ECX=const struct k_sigaction* act, EDX=struct k_sigaction* old,
 * ESI=sigsetsize. Stores the action per-signal; SIGKILL/SIGSTOP cannot be
 * changed.
 */
static int32_t sys_rt_sigaction(isr_params_t *params)
{
    int signo = (int)params->ebx;
    const struct k_sigaction __user *uact = (const struct k_sigaction __user *)params->ecx;
    struct k_sigaction __user *uold = (struct k_sigaction __user *)params->edx;
    uint32_t setsize = params->esi;
    struct k_sigaction act;

    if (!current_proc) {
        return -ESRCH;
    }
    if (signo < 1 || signo > KSIG_MAX || setsize != 8) {
        return -EINVAL;
    }
    if (signo == SIGKILL || signo == SIGSTOP) {
        return -EINVAL; /* cannot be caught or ignored */
    }
    if (uold) {
        if (copy_to_user(uold, &current_proc->sigacts[signo],
                         sizeof(struct k_sigaction)) != 0) {
            return -EFAULT;
        }
    }
    if (uact) {
        if (copy_from_user(&act, uact, sizeof(act)) != 0) {
            return -EFAULT;
        }
        current_proc->sigacts[signo] = act;
        /* Installing SIG_IGN drops any already-pending instance the way Linux
         * does. Guard the pending RMW against a concurrent IRQ signal_post. */
        if (act.handler == KSIG_IGN) {
            uint32_t f = sig_irq_save();
            current_proc->sig_pending &= ~KSIG_BIT(signo);
            sig_irq_restore(f);
        }
    }
    return 0;
}

/**
 * Examine or change the blocked-signal mask (rt_sigprocmask, syscall 175).
 *
 * EBX=how (0 BLOCK/1 UNBLOCK/2 SETMASK), ECX=const set*, EDX=old set*,
 * ESI=sigsetsize. SIGKILL/SIGSTOP can never be blocked.
 */
static int32_t sys_rt_sigprocmask(isr_params_t *params)
{
    int how = (int)params->ebx;
    const void __user *uset = (const void __user *)params->ecx;
    void __user *uold = (void __user *)params->edx;
    uint32_t setsize = params->esi;
    uint32_t words[2];
    uint64_t set;

    if (!current_proc) {
        return -ESRCH;
    }
    if (setsize != 8) {
        return -EINVAL;
    }
    if (uold) {
        words[0] = (uint32_t)current_proc->sig_blocked;
        words[1] = (uint32_t)(current_proc->sig_blocked >> 32);
        if (copy_to_user(uold, words, sizeof(words)) != 0) {
            return -EFAULT;
        }
    }
    if (uset) {
        if (copy_from_user(words, uset, sizeof(words)) != 0) {
            return -EFAULT;
        }
        set = (uint64_t)words[0] | ((uint64_t)words[1] << 32);
        switch (how) {
            case 0: /* SIG_BLOCK */
                current_proc->sig_blocked |= set;
                break;
            case 1: /* SIG_UNBLOCK */
                current_proc->sig_blocked &= ~set;
                break;
            case 2: /* SIG_SETMASK */
                current_proc->sig_blocked = set;
                break;
            default:
                return -EINVAL;
        }
        current_proc->sig_blocked &= ~KSIG_UNCATCHABLE; /* never block KILL/STOP */
    }
    return 0;
}

/*
 * The frame pushed on the user stack to run a handler, then read back by
 * rt_sigreturn. It is self-consistent (the kernel owns both ends), so it
 * does not need to match musl's ucontext byte layout: a plain handler only
 * reads `sig`, and only rt_sigreturn reads `saved`. info/uc give SA_SIGINFO
 * handlers valid (zeroed) argument pointers.
 */
struct sig_frame {
    uint32_t pretcode;     /* return address: the restorer (musl __restore_rt) */
    uint32_t sig;          /* handler arg 1 */
    uint32_t pinfo;        /* handler arg 2 (&info) */
    uint32_t puc;          /* handler arg 3 (&uc) */
    isr_params_t saved;    /* the interrupted trapframe, restored by sigreturn */
    uint64_t saved_mask;   /* the blocked mask to restore */
    uint32_t magic;        /* sanity tag checked by rt_sigreturn */
    uint8_t info[128];     /* siginfo_t storage (zeroed; si_signo set) */
    uint8_t uc[128];       /* ucontext storage (zeroed) */
};

#define SIG_FRAME_MAGIC 0x5347464du /* "SGFM" */

/**
 * Build a signal frame on the user stack and redirect the trapframe to the
 * handler. Returns 0, or -1 if the user stack could not be written.
 */
static int signal_setup_frame(isr_params_t *tf, int signo, struct k_sigaction *sa)
{
    struct sig_frame frame;
    uint32_t sp = tf->user_esp;
    uint32_t frame_addr;
    uint64_t newblock;

    /* Place the frame below the current user esp, aligned so the handler entry
     * esp+4 is 16-byte aligned (the i386 call ABI). */
    sp -= sizeof(struct sig_frame);
    sp &= ~0xFu;
    sp -= 4;
    frame_addr = sp;

    k_memset(&frame, 0, sizeof(frame));
    frame.pretcode = sa->restorer;
    frame.sig = (uint32_t)signo;
    frame.pinfo = frame_addr + (uint32_t)((char *)frame.info - (char *)&frame);
    frame.puc = frame_addr + (uint32_t)((char *)frame.uc - (char *)&frame);
    frame.saved = *tf;
    frame.saved_mask = current_proc->sig_blocked;
    frame.magic = SIG_FRAME_MAGIC;
    frame.info[0] = (uint8_t)signo; /* siginfo_t.si_signo (first int) low byte */

    if (copy_to_user((void __user *)frame_addr, &frame, sizeof(frame)) != 0) {
        return -1;
    }

    /* Block the handler's mask, plus this signal unless SA_NODEFER, for the
     * run. */
    newblock = current_proc->sig_blocked | sigact_mask(sa);
    if (!(sa->flags & SA_NODEFER)) {
        newblock |= KSIG_BIT(signo);
    }
    newblock &= ~KSIG_UNCATCHABLE;
    current_proc->sig_blocked = newblock;

    if (sa->flags & SA_RESETHAND) {
        sa->handler = KSIG_DFL; /* one-shot */
    }

    /* Enter the handler: eip = handler, esp = frame (args just above it). */
    tf->eip = sa->handler;
    tf->user_esp = frame_addr;
    return 0;
}

/**
 * The shared body of sigreturn.
 *
 * Reads the frame the restorer left on the user stack, validates it, and
 * restores the saved trapframe (segment selectors and EFLAGS forced to safe
 * user values, since the frame is on the untrusted user stack) plus the
 * pre-handler signal mask. Returns the restored EAX so the dispatcher does not
 * clobber it.
 *
 * `delta` is how far below the current user esp the frame begins, which
 * differs by trampoline: musl's __restore_rt enters with esp just past
 * `pretcode` (delta 4), while the legacy __restore pops the signal word
 * first and so enters 4 bytes higher (delta 8). Both read the same
 * kernel-built frame.
 */
static int32_t signal_restore(isr_params_t *tf, uint32_t delta)
{
    struct sig_frame frame;
    uint32_t frame_addr;

    if (!current_proc) {
        return -ESRCH;
    }
    frame_addr = tf->user_esp - delta;
    if (copy_from_user(&frame, (const void __user *)frame_addr, sizeof(frame)) != 0) {
        terminate_current_process(SIGSEGV);
        return 0;
    }
    if (frame.magic != SIG_FRAME_MAGIC) {
        terminate_current_process(SIGSEGV); /* forged/clobbered frame */
        return 0;
    }

    /* Restore the general registers and instruction/stack pointers, but FORCE
     * the segment selectors to ring-3 user values and sanitize EFLAGS (keep IF
     * set, mask the privileged/IOPL bits) so a tampered frame cannot escalate.
     */
    tf->edi = frame.saved.edi;
    tf->esi = frame.saved.esi;
    tf->ebp = frame.saved.ebp;
    tf->ebx = frame.saved.ebx;
    tf->edx = frame.saved.edx;
    tf->ecx = frame.saved.ecx;
    tf->eax = frame.saved.eax;
    tf->eip = frame.saved.eip;
    tf->user_esp = frame.saved.user_esp;
    tf->cs = 0x1b;      /* privilege-critical: always the user code selector */
    tf->user_ss = 0x23; /* privilege-critical: always the user data selector */
    /* Restore the saved data/TLS selectors so %gs keeps the per-process TLS
     * selector (0x33) the handler ran with; coerce anything that is not a
     * ring-3 selector to flat user data so a forged frame cannot smuggle a
     * kernel selector to iret (a bogus ring-3 selector at worst faults the
     * process). */
    tf->ds = safe_user_seg(frame.saved.ds);
    tf->es = safe_user_seg(frame.saved.es);
    tf->fs = safe_user_seg(frame.saved.fs);
    tf->gs = safe_user_seg(frame.saved.gs);
    tf->eflags = (frame.saved.eflags & 0x00000cd5u) | 0x202u; /* safe flags + IF */

    current_proc->sig_blocked = frame.saved_mask & ~KSIG_UNCATCHABLE;
    return (int32_t)tf->eax;
}

/*
 * rt_sigreturn (syscall 173): return through musl's __restore_rt (SA_SIGINFO
 * handlers), a bare `int $0x80`, so the frame sits 4 bytes below the user esp.
 */
static int32_t sys_rt_sigreturn(isr_params_t *tf)
{
    return signal_restore(tf, 4);
}

/*
 * sigreturn (syscall 119): return through musl's legacy __restore (plain
 * handlers, no SA_SIGINFO). It pops the signal word before the `int $0x80`,
 * so the frame sits 8 bytes below the user esp. Reads the same kernel-built
 * frame as the rt path; the popped word is the handler's `sig` argument and
 * is discarded.
 */
static int32_t sys_sigreturn(isr_params_t *tf)
{
    return signal_restore(tf, 8);
}

/**
 * Act on a pending signal at the return-to-user boundary.
 *
 * Called from the ISR/IRQ return path just before iret, with the trapframe
 * about to be restored. If the current process is returning to USER mode and
 * has a deliverable (pending, not blocked) signal, act on the
 * lowest-numbered one: ignore it, terminate (default), stop, or build a
 * handler frame.
 */
void deliver_signals(isr_params_t *tf)
{
    if (!current_proc) {
        return;
    }
    if ((tf->cs & 3) != 3) {
        return; /* not returning to user mode */
    }

    for (;;) {
        uint64_t deliverable = current_proc->sig_pending & ~current_proc->sig_blocked;
        struct k_sigaction *sa;
        int signo;

        if (deliverable == 0) {
            return;
        }
        for (signo = 1; signo <= KSIG_MAX; signo++) {
            if (deliverable & KSIG_BIT(signo)) {
                break;
            }
        }
        if (signo > KSIG_MAX) {
            return; /* unreachable (deliverable != 0); keeps the shift bounded */
        }
        current_proc->sig_pending &= ~KSIG_BIT(signo);
        sa = &current_proc->sigacts[signo];

        if (sa->handler == KSIG_IGN) {
            continue; /* explicitly ignored */
        }
        if (sa->handler == KSIG_DFL) {
            enum ksig_default def = ksig_default_action(signo);
            if (def == KSIG_DEFAULT_TERM) {
                terminate_current_process(signo); /* does not return */
                return;
            }
            if (def == KSIG_DEFAULT_STOP) {
                process_t *parent;
                /* The real init (PID 1) is unstoppable, like Unix init:
                 * stopping it would freeze the system with no reaper and no way
                 * back. Drop a job-control stop aimed at it. */
                if (current_proc->pid == init_pid && !debug_shell_is_home) {
                    continue;
                }
                /* Job-control stop: park as PROC_STOPPED until a SIGCONT (or a
                 * SIGKILL, to die) flips us back to READY. Record the stop for
                 * the parent's wait4(WUNTRACED) and nudge it with SIGCHLD. */
                parent = get_process(current_proc->parent_pid);
                current_proc->jc_status = (uint32_t)(((signo & 0xff) << 8) | 0x7f);
                current_proc->state = PROC_STOPPED;
                if (parent) {
                    signal_post(parent, SIGCHLD);
                }
                /* Yield to others; if none are runnable, idle. The scheduler
                 * keeps us STOPPED (it only flips RUNNING->READY); signal_post
                 * resumes us. `sti;hlt;cli` from IF=0 is lost-wakeup-safe. */
                while (current_proc->state == PROC_STOPPED) {
                    schedule();
                    if (current_proc->state == PROC_STOPPED) {
                        idle_wait_irq_off();
                    }
                }
                continue; /* resumed: re-check pending signals */
            }
            continue; /* default ignore (SIGCHLD/SIGCONT/SIGURG/SIGWINCH) */
        }
        /* A user handler: build the frame and run it. The remaining pending
         * signals deliver after sigreturn returns through this path again. */
        if (signal_setup_frame(tf, signo, sa) != 0) {
            terminate_current_process(SIGSEGV);
        }
        return;
    }
}

/* True if some process is in process group `pgid` within session `sid`. This is
 * the TIOCSPGRP rule that a foreground group must belong to the controlling
 * session.
 *
 * A zombie counts. A process leaves its group when it is reaped and not when it
 * exits, and a shell depends on that: putting a pipeline in the foreground
 * means setting the terminal's group to the first member's pid, and that member
 * can be a short command that has already exited by the time the shell gets
 * there. Excluding zombies refused the claim in exactly that race and left the
 * shell reporting that it could not set the terminal's process group. */
static int pgrp_in_session(int pgid, int sid)
{
    int i;

    for (i = 0; i < MAX_PROCS; i++) {
        process_t *p = &proc_table[i];
        if (p->state != PROC_UNUSED &&
            (int)p->pgid == pgid && (int)p->sid == sid) {
            return 1;
        }
    }
    return 0;
}

/* The tty behind a descriptor for the shared terminal ioctls: the console for a
 * FD_TYPE_CONSOLE fd, the slave tty for a FD_TYPE_PTS (pty slave) fd, else
 * NULL. */
static struct tty *fd_tty(fd_entry_t *e)
{
    if (e->type == FD_TYPE_CONSOLE) {
        return &console_tty;
    }
    if (e->type == FD_TYPE_PTS) {
        return pty_slave_tty(e);
    }
    return NULL;
}

/**
 * The shared terminal ioctls operating on a GIVEN tty.
 *
 * The real termios layer plus the session/job-control requests, so the
 * console and a pty slave share one implementation. Sets *handled = 1 and
 * returns the result for a recognized terminal request; sets *handled = 0
 * (return value unused) otherwise, so the caller falls through to its own
 * handling. TCGETS/TCSETS exchange the 36-byte kernel termios (n_tty.h);
 * TCSETSF and TCFLSH flush the unread input.
 */
static int32_t tty_common_ioctl(struct tty *t, uint32_t request, uint32_t arg, int *handled)
{
    *handled = 1;
    switch (request) {
        case TCGETS:
            if (!arg) {
                return -EFAULT;
            }
            if (copy_to_user((void __user *)arg, &t->ldisc.tio,
                             sizeof(struct ktermios)) != 0) {
                return -EFAULT;
            }
            return 0;

        case FIONREAD: {
            /* Bytes a read would return right now (a complete cooked line, or
             * the queued count in raw mode); a pending EOF counts as 0. Drains
             * the input source first so freshly arrived input is counted. */
            int navail;
            if (!arg) {
                return -EFAULT;
            }
            navail = tty_inq(t);
            if (copy_to_user((void __user *)arg, &navail, sizeof(navail)) != 0) {
                return -EFAULT;
            }
            return 0;
        }

        case TCSETS:
        case TCSETSW:
        case TCSETSF: {
            struct ktermios kt;
            if (!arg) {
                return -EFAULT;
            }
            if (copy_from_user(&kt, (const void __user *)arg,
                               sizeof(struct ktermios)) != 0) {
                return -EFAULT;
            }
            /* Install with interrupts off (vs IRQ-side input); TCSETSF also
             * discards pending input. */
            tty_set_termios(t, &kt, request == TCSETSF);
            return 0;
        }

        case TCFLSH:
            /* arg is the queue selector: TCIFLUSH(0)/TCIOFLUSH(2) drop input;
             * we hold no output queue, so TCOFLUSH(1) is a no-op. Else invalid.
             */
            if (arg > 2) {
                return -EINVAL;
            }
            if (arg == 0 || arg == 2) {
                tty_flush_input(t);
            }
            return 0;

        case TIOCGWINSZ:
            if (!arg) {
                return -EFAULT;
            }
            if (copy_to_user((void __user *)arg, &t->winsize,
                             sizeof(t->winsize)) != 0) {
                return -EFAULT;
            }
            return 0;

        case TIOCSWINSZ:
            if (!arg) {
                return -EFAULT;
            }
            if (copy_from_user(&t->winsize, (const void __user *)arg,
                               sizeof(t->winsize)) != 0) {
                return -EFAULT;
            }
            return 0;

        case TIOCGPGRP: {
            int pgrp = t->fg_pgid;
            if (!arg) {
                return -EFAULT;
            }
            if (copy_to_user((void __user *)arg, &pgrp, sizeof(pgrp)) != 0) {
                return -EFAULT;
            }
            return 0;
        }

        case TIOCSPGRP: {
            int pgrp;
            if (!arg) {
                return -EFAULT;
            }
            if (copy_from_user(&pgrp, (const void __user *)arg, sizeof(pgrp)) != 0) {
                return -EFAULT;
            }
            /* The caller must hold this terminal as its controlling terminal
             * (checked before the argument, as Linux does)... */
            if (t->sid == 0 || (int)current_proc->sid != t->sid) {
                return -ENOTTY;
            }
            if (pgrp <= 0) {
                return -EINVAL;
            }
            /* ...and the new foreground group must live in that session. */
            if (!pgrp_in_session(pgrp, t->sid)) {
                return -EPERM;
            }
            t->fg_pgid = pgrp;
            return 0;
        }

        case TIOCSCTTY:
            /* A session leader with no controlling tty claims this terminal for
             * its session; re-claiming by the same session is idempotent. */
            if ((int)current_proc->sid != (int)current_proc->pid) {
                return -EPERM; /* not a session leader */
            }
            if (t->sid != 0 && t->sid != (int)current_proc->sid) {
                return -EPERM; /* owned by another session */
            }
            /* Claim t as this session's SOLE controlling terminal: one atomic
             * step publishes t (sid + foreground group) AND drops every other
             * terminal the session held (the console and any other pty), so the
             * timer-IRQ serial drain never sees a torn or doubled ctty and
             * /dev/tty resolves to t. */
            tty_make_sole_ctty(t, (int)current_proc->sid, (int)current_proc->pgid);
            return 0;

        case TIOCNOTTY:
            /* Give up the controlling terminal if it belongs to our session. */
            if (t->sid == (int)current_proc->sid) {
                tty_set_session(t, 0, 0);
            }
            return 0;

        default:
            *handled = 0;
            return 0;
    }
}

/**
 * Device control (ioctl(2)).
 *
 * EBX = fd, ECX = request, EDX = arg. A tty fd (console or pty slave) routes
 * to the shared termios/session handler first; an ops-backed object handles
 * its own remaining requests; the framebuffer and console handle their
 * device-specific ones (FB_*, KIOCSOUND, AUDIO_*). Returns 0, the request
 * result, or -errno (-ENOTTY for a terminal request on a non-terminal fd).
 */
static int32_t sys_ioctl(isr_params_t *params)
{
    int fd_num = (int)params->ebx;
    uint32_t request = params->ecx;
    uint32_t arg = params->edx;
    fd_entry_t *entry;

    if (!current_proc) {
        return -ESRCH;
    }

    /* Validate the fd first, so a bad fd reports -EBADF, not a request-
     * specific status (e.g. ioctl(-1, TCGETS) must be -EBADF, not -ENOTTY). */
    entry = fd_get(&current_proc->fds, fd_num);
    if (!entry) {
        return -EBADF;
    }

    /* A socket accepts the interface ioctls: this is where a program sets the
     * machine's address, which is what a DHCP client is for. They are checked
     * before the tty block because a socket is not a terminal and would
     * otherwise fall through to -ENOTTY. */
    if (entry->type == FD_TYPE_SOCKET && entry->sock) {
        int handled;
        int32_t r = netif_ioctl(request, arg, &handled);

        if (handled) {
            return r;
        }
    }

    /* tty-family fds (the console and a pty slave) share the termios/session
     * ioctls, handled here, BEFORE the generic ops dispatch, so a pty slave's
     * TCGETS/TIOCSCTTY reach the shared handler, not its FIONBIO-only
     * ops->ioctl. On a non-terminal fd the terminal-only requests stay -ENOTTY
     * (the fallthrough below), as Linux reports. */
    {
        struct tty *t = fd_tty(entry);
        if (t) {
            int handled;
            int32_t r = tty_common_ioctl(t, request, arg, &handled);
            if (handled) {
                return r;
            }
            /* not a terminal ioctl: fall through (a pts slave still has its
             * ops->ioctl for FIONBIO; the console falls to the audio block). */
        }
    }

    /* A backing-ops object (pipe/pty/device) owns its remaining ioctls:
     * FIONBIO, and (for the pty master) TIOCGPTN/TIOCSPTLCK/TIOCSWINSZ. */
    if (entry->ops) {
        return entry->ops->ioctl ? entry->ops->ioctl(entry, request, arg)
                                 : -ENOTTY;
    }
    if (request == TCGETS || request == TCSETS || request == TCSETSW ||
        request == TCSETSF || request == TCFLSH || request == TIOCGWINSZ ||
        request == TIOCSWINSZ || request == TIOCGPGRP || request == TIOCSPGRP ||
        request == TIOCSCTTY || request == TIOCNOTTY) {
        return -ENOTTY; /* terminal-only requests on a non-terminal fd */
    }

    /* KIOCSOUND is handled by the console-type block below, keyed on the
     * backing object's type (not the fd number) so it cannot fire on an fd 1/2
     * that has been closed and reopened onto a regular file (now that
     * close(0/1/2) is legal). */

    if (entry->type == FD_TYPE_FRAMEBUFFER) {
        switch (request) {
            case FB_SETPALETTE:
                if (arg != 0) {
                    /* 256 RGB triples = 768 bytes read straight from user. */
                    if (!user_buffer_ok(arg, 768, 0)) {
                        return -EFAULT;
                    }
                    vga_mode13h_set_palette((const uint8_t *)arg);
                    return 0;
                }
                return -EFAULT;

            case FB_MODE_13H:
                return 0; /* already in mode 13h while fb is open */

            case FB_MODE_TEXT:
                vga_mode13h_exit();
                fb_owner_pid = 0;
                return 0;

            default:
                return -ENOTTY;
        }
    }

    if (entry->type == FD_TYPE_CONSOLE) {
        if (request == KIOCSOUND) {
            if (arg == 0) {
                speaker_off();
            } else {
                speaker_on(arg);
            }
            return 0;
        }

        switch (request) {
            case AUDIO_GET_INFO: {
                struct audio_info kinfo;
                if (arg) {
                    k_memset(&kinfo, 0, sizeof(kinfo));
                    kinfo.driver_type = audio ? audio->type : AUDIO_DRIVER_NONE;
                    kinfo.has_real_audio = audio_has_real_output() ? 1 : 0;
                    if (copy_to_user((void __user *)arg, &kinfo,
                                     sizeof(kinfo)) != 0) {
                        return -EFAULT;
                    }
                }
                return 0;
            }

            case AUDIO_START: {
                struct audio_start_params kp;
                if (!audio || !arg) {
                    return -EFAULT;
                }
                if (copy_from_user(&kp, (const void __user *)arg,
                                   sizeof(kp)) != 0) {
                    return -EFAULT;
                }
                return audio->start(kp.sample_rate, (audio_format_t)kp.format,
                                    (int)kp.channels);
            }

            case AUDIO_STOP:
                if (audio) {
                    audio->stop();
                    return 0;
                }
                return -ENODEV;

            case AUDIO_WRITE: {
                struct audio_write_params kp;
                if (!audio || !arg) {
                    return -EFAULT;
                }
                /* Copy the param struct in through the helper, then range-check
                 * the PCM buffer it points at before the driver reads it (the
                 * bulk copy is the driver's, as with the framebuffer/socket
                 * data paths). */
                if (copy_from_user(&kp, (const void __user *)arg,
                                   sizeof(kp)) != 0) {
                    return -EFAULT;
                }
                if (!kp.buffer ||
                    !user_buffer_ok((uint32_t)(uintptr_t)kp.buffer, kp.size, 0)) {
                    return -EFAULT;
                }
                return audio->write(kp.buffer, kp.size);
            }

            case AUDIO_SET_VOLUME:
                if (audio) {
                    audio->set_volume((int)arg);
                    return 0;
                }
                return -ENODEV;
        }
    }

    return -ENOTTY;
}

/* Network socketcall (SYS_SOCKETCALL = 102).
 *
 * Linux i386 convention: EBX is the sub-call number, ECX points to an array of
 * 32-bit arguments in user memory. Pointer arguments index into user memory,
 * so every user address is range-checked before it is dereferenced.
 */

/*
 * User address window. The kernel is identity-mapped in the low 32 MB and user
 * space lives above it, below 0xC0000000. A user pointer and the whole length
 * it spans must fall inside this window; anything else is rejected so a syscall
 * argument cannot read or write kernel memory. The window and the check now
 * live in mm/usercopy (USER_ADDR_MIN/MAX, user_range_ok); user_access_ok is a
 * thin alias kept for the existing call sites.
 */
static int user_access_ok(uint32_t addr, uint32_t len)
{
    return user_range_ok(addr, len);
}

/*
 * Range-check a user buffer AND back its pages before the kernel passes the
 * raw pointer to a lower layer (krffs, tcp, udp, the drivers) that
 * dereferences it directly. With demand paging a numerically valid pointer
 * may still be unmapped (a never-touched heap page, or a wild address in no
 * VMA); vm_prefault_range faults the valid pages in here and rejects the bad
 * ones, so the lower layer's raw access can never fault in kernel mode and
 * panic. 'write' is 1 when that layer writes into the buffer (so the pages
 * need write permission).
 */
static int user_buffer_ok(uint32_t addr, uint32_t len, int write)
{
    return user_access_ok(addr, len) && vm_prefault_range(addr, len, write);
}

/* Read and write the little-endian words of a caller's own structures (a
 * msghdr's pointers and lengths), which are host order and not wire order. */
#define SC_LE32(p) \
    (((uint32_t)((uint8_t *)(p))[0]) | \
     ((uint32_t)((uint8_t *)(p))[1] <<  8) | \
     ((uint32_t)((uint8_t *)(p))[2] << 16) | \
     ((uint32_t)((uint8_t *)(p))[3] << 24))
#define SC_WLE32(p, v) do { \
    ((uint8_t *)(p))[0] = (uint8_t)(v); \
    ((uint8_t *)(p))[1] = (uint8_t)((v) >>  8); \
    ((uint8_t *)(p))[2] = (uint8_t)((v) >> 16); \
    ((uint8_t *)(p))[3] = (uint8_t)((v) >> 24); \
} while (0)

/* Read and write big-endian sockaddr fields (sin_port, sin_addr). */
#define SC_BE16(p) \
    ((uint16_t)(((uint8_t *)(p))[0] << 8 | ((uint8_t *)(p))[1]))
#define SC_BE32(p) \
    (((uint32_t)((uint8_t *)(p))[0] << 24) | \
     ((uint32_t)((uint8_t *)(p))[1] << 16) | \
     ((uint32_t)((uint8_t *)(p))[2] <<  8) | \
     ((uint32_t)((uint8_t *)(p))[3]))
#define SC_WBE16(p, v) do { \
    ((uint8_t *)(p))[0] = (uint8_t)((v) >> 8); \
    ((uint8_t *)(p))[1] = (uint8_t)(v); \
} while (0)
#define SC_WBE32(p, v) do { \
    ((uint8_t *)(p))[0] = (uint8_t)((v) >> 24); \
    ((uint8_t *)(p))[1] = (uint8_t)((v) >> 16); \
    ((uint8_t *)(p))[2] = (uint8_t)((v) >>  8); \
    ((uint8_t *)(p))[3] = (uint8_t)(v); \
} while (0)

/* socketcall sub-function numbers (Linux standard). */
#define SC_SOCKET       1
#define SC_BIND         2
#define SC_CONNECT      3
#define SC_LISTEN       4
#define SC_ACCEPT       5
#define SC_GETSOCKNAME  6
#define SC_GETPEERNAME  7
#define SC_SEND         9
#define SC_RECV        10
#define SC_SENDTO      11
#define SC_RECVFROM    12
#define SC_SHUTDOWN    13
#define SC_SETSOCKOPT  14
#define SC_GETSOCKOPT  15
#define SC_RECVMSG     17
/* Rach2's private ICMP-echo socketcall. RENUMBERED out of the BSD range (it
 * used to squat on 15 == Linux getsockopt, which a real server needs).
 * Userspace callers (utils/ping.c, tests/pingtest.c) use the same value. */
#define SC_PING     0x1000

/**
 * Canonicalize a destination address at the socket boundary.
 *
 * The stack below the socket layer knows exactly one local address
 * (`net_ip_addr`): every TCB stamps it, checksums verify against it, and
 * `getsockname` reports it. 127/8 is therefore an alias for that one address,
 * rewritten here before any connection state exists, so no lower layer ever
 * sees a second local address and `getpeername` stays truthful.
 */
static uint32_t canonical_dst_ip(uint32_t ip)
{
    return (ip & 0xFF000000UL) == 0x7F000000UL ? net_ip_addr : ip;
}

/**
 * Send one datagram, judging the destination first.
 *
 * A datagram to a broadcast address reaches every machine on the wire, so a
 * socket must have asked for that with `SO_BROADCAST` before it can send one.
 * A program that broadcasts by accident wakes the whole network, and the
 * option exists so that it has to mean it.
 */
static int32_t dgram_send(struct socket *sock, uint32_t dst_ip, uint16_t dst_port,
                          const uint8_t *buf, uint16_t len)
{
    int r;

    if (!sock->broadcast && netconf_is_broadcast(dst_ip)) {
        return -EACCES;
    }
    /* One IPv4 datagram over Ethernet: 1500 bytes of frame, less the twenty of
       the IP header and the eight of UDP's. A larger one is refused here rather
       than dropped further down, where this call would have reported it sent. */
    if (len > 1472) {
        return -EMSGSIZE;
    }
    if (sock->local_port == 0 && udp_bind_ephemeral(sock) == 0) {
        return -EAGAIN;
    }
    /* The hop limit is the socket's, which IP_TTL sets and traceroute counts
       up. What the send layer refuses is reported here rather than counted as
       bytes sent. */
    r = udp_tx(sock->local_port, dst_ip, dst_port, buf, len, sock->ip_ttl);
    if (r < 0) {
        return r;
    }
    return (int32_t)len;
}

/* The raw send buffer: one MTU, filled from user space in syscall context.
   Only syscall context touches it, and this kernel runs one process there at a
   time, so it needs no lock of its own; what needs the lock is the send, which
   goes through the transmit buffers an interrupt also builds in. */
static uint8_t raw_send_buf[1500];

/* recv flags: only MSG_DONTWAIT is honored. */
#define MSG_DONTWAIT 0x40

/* The query-only raw descriptor a DHCP client asks the interface through. */
#define SOCK_RAW     3
#define IPPROTO_RAW  255
#define IPPROTO_ICMP   1

/* setsockopt(2)/getsockopt(2) at the IP level: the hop limit a socket's
 * datagrams leave with, and the option that would hand the caller the header,
 * which this stack does not implement. */
#define SOL_IP        0
#define IP_TTL        2
#define IP_HDRINCL    3

/* setsockopt levels and the one packet option that is answered. */
#define SOL_PACKET      263
#define PACKET_AUXDATA  8

/* We read sin_family/sin_port/sin_addr from the first 8 sockaddr bytes. */
#define SOCKADDR_MIN 8

/* The whole of a struct sockaddr_in: family (2), port (2), address (4), and
 * eight bytes of padding. `SOCKADDR_MIN` above is only the part that is read.
 */
#define SOCKADDR_IN_LEN 16

/* struct sockaddr_ll, the address a packet socket binds and sends to, with the
 * offset of each field beside it: family 0 (2 bytes), protocol 2 (2), ifindex
 * 4 (4), hatype 8 (2), pkttype 10 (1), halen 11 (1), addr 12 (8), for 20 in
 * all. A caller declaring fewer bytes is refused, not read past. */
#define SOCKADDR_LL_LEN      20
#define SOCKADDR_LL_PROTOCOL 2
#define SOCKADDR_LL_IFINDEX  4
#define SOCKADDR_LL_HATYPE   8
#define ARPHRD_ETHER         1  /* sll_hatype for an Ethernet interface */
#define SOCKADDR_LL_HALEN    11
#define SOCKADDR_LL_ADDR     12

/**
 * Write an AF_INET source/peer sockaddr back to a caller's (addr, addrlen)
 * pair.
 *
 * Respects the caller's declared buffer capacity: read the in/out socklen,
 * copy at most that many of the 16 sockaddr_in bytes, and report the true
 * length (16), the recvfrom(2)/accept(2) truncation semantics. Returns 0 or
 * -EFAULT.
 */
static int copy_sockaddr_to_user(uint32_t sa_ptr, uint32_t salen_ptr,
                                 uint16_t port, uint32_t ip)
{
    uint8_t  sa[16];
    uint32_t caller_len;
    uint32_t copy_len;

    if (!sa_ptr || !salen_ptr) {
        return 0;  /* caller does not want the address back */
    }

    if (copy_from_user(&caller_len, (const void __user *)salen_ptr,
                       sizeof(caller_len)) != 0) {
        return -EFAULT;
    }

    k_memset(sa, 0, sizeof(sa));
    /* sin_family is in HOST byte order (little-endian on i386), NOT network
     * order: a big-endian AF_INET would read back as 0x0200 and a peer-address
     * consumer (dropbear's connection logging) rejects it as an unknown family.
     * Only sin_port and sin_addr are network (big-endian). */
    sa[0] = 2; /* AF_INET, low byte */
    sa[1] = 0;
    SC_WBE16(sa + 2, port);
    SC_WBE32(sa + 4, ip);

    copy_len = (caller_len < sizeof(sa)) ? caller_len : (uint32_t)sizeof(sa);
    if (copy_len > 0 &&
        copy_to_user((void __user *)sa_ptr, sa, copy_len) != 0) {
        return -EFAULT;
    }

    /* Report the real sockaddr_in length even when the caller's buffer was
     * shorter (and thus the address was truncated). */
    caller_len = (uint32_t)sizeof(sa);
    if (copy_to_user((void __user *)salen_ptr, &caller_len,
                     sizeof(caller_len)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/* socket(2): only AF_INET (domain 2), SOCK_STREAM, and SOCK_DGRAM. Returns a
 * new socket fd or -errno. */
static int32_t do_socket(int domain, int type, int protocol)
{
    struct socket *sock;
    fd_entry_t    *entry;
    int            fd;

    /* IPv4, and the packet family the DHCP client needs to see replies that
     * arrive before this machine owns an address. Any other family is refused,
     * not made into a socket that cannot work. */
    if (domain != AF_INET && domain != AF_PACKET) {
        return -EAFNOSUPPORT;
    }
    /* The query descriptor. A DHCP client opens AF_INET/SOCK_RAW/IPPROTO_RAW
     * and does nothing with it but ask the interface ioctls about the machine
     * (busybox's `udhcp_read_interface`), so that is what it is here: a
     * descriptor that accepts ioctls and closes, and refuses to send or
     * receive, because this stack has no raw IP path to offer. Root's, like
     * every raw socket. */
    if (domain == AF_INET && type == SOCK_RAW) {
        /* Two raw sockets, and the protocol tells them apart. IPPROTO_RAW is
         * the query descriptor a DHCP client opens for the interface ioctls
         * and nothing else; IPPROTO_ICMP is the one `ping` and `traceroute`
         * send and receive on. Both are root's. */
        if (protocol != IPPROTO_RAW && protocol != IPPROTO_ICMP) {
            return -EPROTONOSUPPORT;
        }
        if (!current_proc || current_proc->euid != 0) {
            return -EPERM;
        }
    } else if (type != SOCK_DGRAM && type != SOCK_STREAM) {
        return -EINVAL;
    }
    if (domain == AF_PACKET) {
        /* It reads every frame of its protocol the machine receives and sends
         * frames the IP layer never saw, so it is root's. The protocol is in
         * network byte order, as Linux takes it, and the two this stack has
         * frames for are IPv4 and ARP. */
        if (!current_proc || current_proc->euid != 0) {
            return -EPERM;
        }
        if (type != SOCK_DGRAM) {
            return -ESOCKTNOSUPPORT;
        }
        /* Zero is legal and means the socket has no protocol yet: `arping`
         * opens one that way and names ARP in its bind. */
        if ((uint16_t)protocol != 0 && (uint16_t)protocol != PACKET_PROTO_IP &&
            (uint16_t)protocol != PACKET_PROTO_ARP) {
            return -EINVAL;
        }
    }

    sock = socket_alloc(type);
    if (!sock) {
        return -ENOMEM;
    }
    sock->family = (uint16_t)domain;
    if (domain == AF_INET && type == SOCK_RAW && protocol == IPPROTO_ICMP) {
        int reg;

        sock->protocol = (uint16_t)protocol;
        reg = raw_register(sock, (uint8_t)protocol);
        if (reg != 0) {
            socket_unref(sock);
            return reg;
        }
    }
    if (domain == AF_PACKET) {
        /* The argument is in network order, as Linux takes it; the socket
         * holds it in host order, which is what the wire comparison uses. */
        sock->protocol = (uint16_t)(((uint32_t)protocol & 0xffu) << 8 |
                                    (((uint32_t)protocol >> 8) & 0xffu));
    }
    sock->uid = current_proc->euid;

    fd = fd_alloc(&current_proc->fds);
    if (fd < 0) {
        socket_unref(sock);
        return -EMFILE;
    }

    entry         = &current_proc->fds.entries[fd];
    entry->type   = FD_TYPE_SOCKET;
    entry->sock   = sock;
    entry->node   = NULL;
    entry->ofd->offset = 0;
    entry->flags  = FD_FLAG_READ | FD_FLAG_WRITE;
    return fd;
}

/* bind(2): record the local port from the sockaddr; a datagram socket also
 * registers it with UDP (-EADDRINUSE if taken). */
static int32_t do_bind(int fd, uint32_t sa_ptr, int addrlen)
{
    fd_entry_t *entry;
    uint16_t    port;
    uint8_t     sa[SOCKADDR_MIN];

    /* The caller's declared sockaddr must be at least the header we read; a
     * short or negative addrlen must not make us read past it. */
    if (addrlen < SOCKADDR_MIN) {
        return -EINVAL;
    }

    /* Copy the sockaddr header into a kernel buffer; the BE helpers below then
     * operate on the kernel copy. */
    if (copy_from_user(sa, (const void __user *)sa_ptr, SOCKADDR_MIN) != 0) {
        return -EFAULT;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }

    /* A packet socket binds to an interface, not to a port: the address is a
     * sockaddr_ll and binding is what puts it in the delivery table. */
    if (entry->sock->family == AF_PACKET) {
        uint8_t sll[SOCKADDR_LL_LEN];
        int32_t ifindex;
        uint16_t proto;
        int32_t r;

        if (addrlen < SOCKADDR_LL_LEN) {
            return -EINVAL;
        }
        if (copy_from_user(sll, (const void __user *)sa_ptr, SOCKADDR_LL_LEN) != 0) {
            return -EFAULT;
        }
        ifindex = (int32_t)((uint32_t)sll[SOCKADDR_LL_IFINDEX] |
                            ((uint32_t)sll[SOCKADDR_LL_IFINDEX + 1] << 8) |
                            ((uint32_t)sll[SOCKADDR_LL_IFINDEX + 2] << 16) |
                            ((uint32_t)sll[SOCKADDR_LL_IFINDEX + 3] << 24));
        /* A protocol named here replaces the one `socket` was given, which is
         * Linux's rule and is how a client that opened with 0 says what it
         * wants. Naming none leaves the socket's own. The change is made only
         * after the bind is accepted: a bind that fails must leave the socket
         * receiving exactly what it received before. */
        proto = SC_BE16(sll + SOCKADDR_LL_PROTOCOL);
        if (proto != 0 && proto != ETH_TYPE_IP && proto != ETH_TYPE_ARP) {
            return -EINVAL;
        }
        r = packet_bind(entry->sock, (int)ifindex);
        if (r != 0) {
            return r;
        }
        if (proto != 0 && proto != entry->sock->protocol) {
            entry->sock->protocol = proto;
            /* What is queued arrived under the old protocol. A reader would
               be given it labelled with the new one, so it goes. */
            packet_flush(entry->sock);
        }
        return 0;
    }

    /* A raw socket binds to an address, not to a port: it says which of the
     * machine's addresses it wants datagrams for. `traceroute` binds its
     * receive socket to the address its send socket ended up with, and dies if
     * it cannot. This machine has one address, so the bind is a statement
     * about which address it is, and an address the machine does not have is
     * refused rather than accepted and quietly ignored. */
    if (entry->sock->type == SOCK_RAW) {
        uint32_t want;

        if (sock_is_query(entry->sock)) {
            return -EOPNOTSUPP;
        }
        want = SC_BE32(sa + 4);
        if (want != 0 && want != (uint32_t)net_ip_addr &&
            (want & 0xFF000000UL) != 0x7F000000UL) {
            return -EADDRNOTAVAIL;
        }
        return 0;
    }

    port = SC_BE16(sa + 2);

    /* A port below 1024 is a service's, and a service is root's. Without this
     * any user could take the port a DHCP client or an SSH server answers on
     * and be the one machines on the wire talk to. */
    if (port != 0 && port < 1024 && (!current_proc || current_proc->euid != 0)) {
        return -EACCES;
    }

    if (entry->sock->type == SOCK_DGRAM) {
        /* A datagram send before bind auto-binds an ephemeral source port; drop
         * that registration before claiming the requested one, or its
         * port-table slot leaks for the socket's life. */
        if (entry->sock->local_port != 0) {
            udp_unbind(entry->sock);
            entry->sock->local_port = 0;
        }
        if (port == 0) {
            /* Port 0 means "any port": let the stack pick a free ephemeral and
             * record it, the same path a send before bind takes. */
            if (udp_bind_ephemeral(entry->sock) == 0) {
                return -EADDRINUSE;
            }
            return 0;
        }
        if (udp_bind(port, entry->sock) < 0) {
            return -EADDRINUSE;
        }
    }
    if (entry->sock->type == SOCK_STREAM && port != 0 && tcp_port_listening(port)) {
        /* Advisory, and where a server reports it: the invariant belongs to
         * `tcp_listen`, since `bind` and `listen` are separate syscalls and any
         * check made only here is bypassed by binding both sockets first. */
        return -EADDRINUSE;
    }
    entry->sock->local_port = port;
    return 0;
}

/* sendto(2): a stream socket ignores the destination and behaves like send; a
 * datagram socket sends to the sockaddr, binding an ephemeral source port
 * first. */
static int32_t do_sendto(int fd, uint32_t buf_ptr, int len,
                         int flags, uint32_t sa_ptr, int addrlen)
{
    fd_entry_t *entry;
    uint16_t    dst_port;
    uint32_t    dst_ip;
    uint8_t     sa[SOCKADDR_MIN];

    if (len < 0 || !user_buffer_ok(buf_ptr, (uint32_t)len, 0)) {
        return -EFAULT;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }

    /* The query descriptor accepts ioctls and nothing else: it has no protocol
     * to send under, and a caller that got silence here would be waiting for a
     * datagram that cannot arrive. */
    if (sock_is_query(entry->sock)) {
        return -EOPNOTSUPP;
    }

    /* A raw socket sends the ICMP message the caller built, checksum included:
     * this layer supplies the IPv4 header and rewrites nothing, since a
     * program sending a wrong checksum on purpose is testing something. */
    if (entry->sock->type == SOCK_RAW) {
        uint8_t raw_sa[SOCKADDR_IN_LEN];
        uint32_t raw_dst;
        int r;

        if (!sa_ptr || addrlen < SOCKADDR_MIN) {
            return -EDESTADDRREQ;
        }
        k_memset(raw_sa, 0, sizeof(raw_sa));
        if (copy_from_user(raw_sa, (const void __user *)sa_ptr, SOCKADDR_MIN) != 0) {
            return -EFAULT;
        }
        if ((uint16_t)((uint16_t)raw_sa[0] | ((uint16_t)raw_sa[1] << 8)) != AF_INET) {
            return -EAFNOSUPPORT;
        }
        if (len > (int)sizeof(raw_send_buf)) {
            return -EMSGSIZE;
        }
        raw_dst = canonical_dst_ip(SC_BE32(raw_sa + 4));
        /* The copy runs with interrupts on, since it may fault; the send is
           then held against a receive interrupt that would build a datagram
           through the same transmit buffers, which is the rule in net.h and
           what udp_tx does for the same reason. */
        if (copy_from_user(raw_send_buf, (const void __user *)buf_ptr,
                           (uint32_t)len) != 0) {
            return -EFAULT;
        }
        {
            uint32_t netflags = net_lock();
            r = ip_tx(raw_dst, (uint8_t)entry->sock->protocol, raw_send_buf,
                      (uint16_t)len, entry->sock->ip_ttl);
            net_unlock(netflags);
        }
        if (r < 0) {
            return r;
        }
        return (int32_t)len;
    }

    /* On a connected stream socket, sendto with any (or no) destination behaves
     * like send; the connection's peer is the destination, so the sockaddr is
     * ignored (this is the path musl's send() takes: sendto with a NULL addr).
     */
    if (entry->sock->type == SOCK_STREAM) {
        int nonblock = (flags & MSG_DONTWAIT) ||
                       (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));
        return (int32_t)socket_tcp_send(
            entry->sock, (const uint8_t __force *)(const void __user *)buf_ptr, len,
            nonblock);
    }

    /* A packet socket sends an IP packet the caller built, to a MAC address it
     * names: this layer adds the Ethernet header and nothing else. */
    if (entry->sock->family == AF_PACKET) {
        uint8_t sll[SOCKADDR_LL_LEN];
        uint16_t proto;

        if (len > 1500) {
            return -EMSGSIZE;
        }
        if (addrlen < SOCKADDR_LL_LEN) {
            return -EINVAL;
        }
        if (copy_from_user(sll, (const void __user *)sa_ptr, SOCKADDR_LL_LEN) != 0) {
            return -EFAULT;
        }
        if (sll[SOCKADDR_LL_HALEN] != 6) {
            return -EINVAL;
        }
        proto = SC_BE16(sll + SOCKADDR_LL_PROTOCOL);
        if (proto == 0) {
            proto = entry->sock->protocol;
        }
        /* The payload is read where it lies, as the UDP path reads it: the
           range above was validated, and __force strips __user at the call. */
        return (int32_t)packet_send(entry->sock, sll + SOCKADDR_LL_ADDR, proto,
                                    (const uint8_t __force *)(const void __user *)buf_ptr,
                                    (uint16_t)len);
    }

    /* The payload must fit one unfragmented UDP packet (this stack does not
     * fragment; udp_tx caps it at 1500). Reject an oversize datagram with
     * -EMSGSIZE, not truncate it through the uint16_t wire length or let
     * udp_tx silently drop it while this call reports a full send. */
    if (len > 1500) {
        return -EMSGSIZE;
    }

    /* A connected datagram socket needs no destination: `send` is `sendto`
     * with none, which is what musl's send() issues. */
    if (sa_ptr == 0 || addrlen == 0) {
        if (!entry->sock->connected) {
            return -EDESTADDRREQ;
        }
        return dgram_send(entry->sock, entry->sock->remote_ip, entry->sock->remote_port,
                          (const uint8_t __force *)(const void __user *)buf_ptr,
                          (uint16_t)len);
    }

    /* Datagram path: a real destination address is required. */
    if (addrlen < SOCKADDR_MIN) {
        return -EINVAL;  /* declared sockaddr shorter than the header we read */
    }
    /* Copy the destination sockaddr into a kernel buffer for the BE helpers. */
    if (copy_from_user(sa, (const void __user *)sa_ptr, SOCKADDR_MIN) != 0) {
        return -EFAULT;
    }

    dst_port = SC_BE16(sa + 2);
    dst_ip   = canonical_dst_ip(SC_BE32(sa + 4));

    /* One send path, so the broadcast permission and the ephemeral bind are
     * the same here as they are for a write to a connected socket. udp_tx
     * reads the payload itself; __force strips __user at the boundary. */
    return dgram_send(entry->sock, dst_ip, dst_port,
                      (const uint8_t __force *)(const void __user *)buf_ptr,
                      (uint16_t)len);
}

/* recvfrom(2): a stream socket behaves like recv (no source writeback); a
 * datagram socket reports the source address, truncated to the caller's
 * capacity. Honors MSG_DONTWAIT and O_NONBLOCK. */
static int32_t do_recvfrom(int fd, uint32_t buf_ptr, int len,
                           int flags, uint32_t sa_ptr, uint32_t salen_ptr)
{
    fd_entry_t *entry;
    uint32_t    src_ip = 0;
    uint16_t    src_port = 0;
    uint32_t    addr_len = 0;
    uint32_t    addr_give = 0;
    int         want_addr;
    int         bytes;
    int         nonblock;

    if (len < 0 || !user_buffer_ok(buf_ptr, (uint32_t)len, 1)) {
        return -EFAULT;
    }
    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    /* Where the source address will land is settled here, before a datagram is
     * taken: taking one destroys it, and a fault found afterwards would report
     * an error for a message that no longer exists. The caller's capacity is
     * read once and kept, so that a caller whose length word overlaps its own
     * receive buffer cannot change it under the writeback. What is checked is
     * what will actually be written: twenty bytes for a packet socket's
     * sockaddr_ll, sixteen for an AF_INET sender. */
    want_addr = (sa_ptr != 0 && salen_ptr != 0);
    if (want_addr) {
        uint32_t cap = 0;

        if (!user_buffer_ok(salen_ptr, sizeof(cap), 1) ||
            copy_from_user(&cap, (const void __user *)salen_ptr, sizeof(cap)) != 0) {
            return -EFAULT;
        }
        addr_len = (entry->sock->family == AF_PACKET) ? (uint32_t)SOCKADDR_LL_LEN
                                                      : (uint32_t)SOCKADDR_IN_LEN;
        addr_give = cap < addr_len ? cap : addr_len;
        if (addr_give > 0 && !user_buffer_ok(sa_ptr, addr_give, 1)) {
            return -EFAULT;
        }
    }

    /* The query descriptor accepts ioctls and nothing else: nothing is
     * delivered to a descriptor with no protocol, and a caller that got
     * silence here would be waiting for a datagram that cannot arrive. */
    if (sock_is_query(entry->sock)) {
        return -EOPNOTSUPP;
    }

    /* Receive without blocking when the caller asked per-call (MSG_DONTWAIT) or
     * the descriptor is O_NONBLOCK (an OFD status flag, shared by dup/fork). */
    nonblock = (flags & MSG_DONTWAIT) ||
               (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));

    /* On a connected stream socket, recvfrom behaves like recv; the peer is the
     * connection's, so the optional source-address writeback is skipped and a
     * NULL sockaddr is fine (this is the path musl's recv() takes: recvfrom
     * with a NULL addr). */
    if (entry->sock->type == SOCK_STREAM) {
        if (nonblock) {
            return (int32_t)socket_tcp_try_recv(
                entry->sock, (uint8_t __force *)(void __user *)buf_ptr, len);
        }
        return (int32_t)socket_tcp_recv(entry->sock,
                                        (uint8_t __force *)(void __user *)buf_ptr,
                                        len);
    }

    /* socket_recv/socket_try_recv write straight into the validated user
     * buffer; __force strips __user at the boundary. */
    if (nonblock) {
        bytes = socket_try_recv(entry->sock,
                                (uint8_t __force *)(void __user *)buf_ptr, len,
                                &src_ip, &src_port);
    } else {
        bytes = socket_recv(entry->sock,
                            (uint8_t __force *)(void __user *)buf_ptr, len,
                            &src_ip, &src_port);
    }

    /* A receive that failed leaves the caller's address arguments alone: there
     * is no sender to report, and the values that would be written are the ones
     * this frame never set. */
    if (bytes < 0) {
        return bytes;
    }

    /* Report the source address, truncated to the capacity read before the
     * datagram was taken. A packet socket reports the link address the frame
     * came from, a sockaddr_ll; an AF_INET one reports the sender. */
    if (want_addr) {
        uint8_t sa[SOCKADDR_LL_LEN];

        k_memset(sa, 0, sizeof(sa));
        if (entry->sock->family == AF_PACKET) {
            if (packet_last_addr(entry->sock, sa) == 0) {
                addr_len = 0;
                addr_give = 0;
            }
        } else {
            sa[0] = (uint8_t)(AF_INET & 0xff);
            SC_WBE16(sa + 2, src_port);
            SC_WBE32(sa + 4, src_ip);
        }
        if (addr_give > 0 && copy_to_user((void __user *)sa_ptr, sa, addr_give) != 0) {
            return -EFAULT;
        }
        if (copy_to_user((void __user *)salen_ptr, &addr_len, sizeof(addr_len)) != 0) {
            return -EFAULT;
        }
    }
    return bytes;
}

static int32_t do_ping(uint32_t dst_ip)
{
    return (int32_t)icmp_ping(canonical_dst_ip(dst_ip));
}

/* Wait predicate: a stream connect has finished its handshake one way or the
 * other (established, or the TCB went away / reset). */
static int connect_done(void *arg)
{
    struct socket *sock = (struct socket *)arg;
    return sock->tcb == NULL ||
           sock->tcb->state == TCP_ESTABLISHED ||
           sock->tcb->state == TCP_CLOSED;
}

/* Wait predicate: a listening socket has a connection queued for accept. */
static int accept_ready(void *arg)
{
    struct socket *sock = (struct socket *)arg;
    return sock->accept_head != sock->accept_tail;
}

/* connect(2): start the TCP handshake to the sockaddr. A blocking socket waits
 * it out (-EINTR on a signal); a non-blocking one returns 0 or -EINPROGRESS. */
static int32_t do_connect(int fd, uint32_t sa_ptr, int addrlen)
{
    fd_entry_t *entry;
    uint32_t    dst_ip;
    uint16_t    dst_port;
    uint16_t    family;
    uint8_t     sa[SOCKADDR_MIN];
    int         rc;

    /* The family is read first and on its own, because a disconnect is a
     * family and nothing else: `connect(fd, &unspec, sizeof(sa_family_t))` is
     * how a program lets go of a peer, and requiring a whole address for it
     * would leave that program connected. */
    /* The descriptor is resolved before its argument is read, so that a call on
     * something that is not a socket says so instead of reporting whatever was
     * wrong with an address it was never going to use. */
    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    if (entry->sock->type == SOCK_RAW) {
        return -EOPNOTSUPP;
    }
    /* A packet socket has no peer to connect to: it is bound to an interface
     * and sends to a hardware address each time. Reaching the datagram arm
     * below would put it in the UDP port table, which its teardown never
     * unbinds from. */
    if (entry->sock->family == AF_PACKET) {
        return -EOPNOTSUPP;
    }

    k_memset(sa, 0, sizeof(sa));
    if (addrlen < 2) {
        return -EINVAL;
    }
    if (copy_from_user(sa, (const void __user *)sa_ptr, 2) != 0) {
        return -EFAULT;
    }
    family = (uint16_t)((uint16_t)sa[0] | ((uint16_t)sa[1] << 8));
    /* Two bytes are enough for a disconnect and only for a disconnect, and only
     * a datagram socket has one to do: a stream's connection ends by closing
     * it. Anything else must give a whole address, or the bytes past the family
     * would be this function's stack. */
    if (family == 0) {
        if (entry->sock->type != SOCK_DGRAM) {
            return -EAFNOSUPPORT;
        }
    } else {
        if (addrlen < SOCKADDR_MIN) {
            return -EINVAL; /* declared sockaddr shorter than the header we read */
        }
        if (copy_from_user(sa, (const void __user *)sa_ptr, SOCKADDR_MIN) != 0) {
            return -EFAULT;
        }
    }
    /* A datagram socket's connect records where it will send and who it will
     * hear from; there is no handshake and nothing to wait for. AF_UNSPEC
     * undoes it, which is how a program lets go of a peer. */
    if (entry->sock->type == SOCK_DGRAM) {
        uint32_t held;

        if (family == 0) {
            held = net_lock();
            entry->sock->connected = 0;
            entry->sock->remote_ip = 0;
            entry->sock->remote_port = 0;
            net_unlock(held);
            return 0;
        }
        if (family != AF_INET) {
            return -EAFNOSUPPORT;
        }
        /* Something must answer, so the socket needs a source port: the same
         * ephemeral bind a send before bind would have taken. The bind and the
         * peer are published together, under one hold. A bind that landed first
         * would put the socket in the port table with no peer recorded, and a
         * datagram arriving in that interval would be admitted from anyone; a
         * reconnect that published a new port beside an old address would admit
         * a sender that is neither the old peer nor the new one. */
        held = net_lock();
        if (entry->sock->local_port == 0 && udp_bind_ephemeral(entry->sock) == 0) {
            net_unlock(held);
            return -EAGAIN;
        }
        entry->sock->remote_port = SC_BE16(sa + 2);
        entry->sock->remote_ip = canonical_dst_ip(SC_BE32(sa + 4));
        entry->sock->connected = 1;
        net_unlock(held);
        return 0;
    }
    if (entry->sock->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }
    if (entry->sock->listening || entry->sock->tcb) {
        /* A socket that is already listening, connecting, or connected keeps
         * its TCB; a second tcp_connect would allocate another and leak the
         * first, whose stale delivery could reach a since-reused socket. */
        return (entry->sock->tcb && entry->sock->tcb->state == TCP_SYN_SENT)
                   ? -EALREADY
                   : -EISCONN;
    }

    dst_port = SC_BE16(sa + 2);
    dst_ip   = canonical_dst_ip(SC_BE32(sa + 4));

    {
        /* Verbatim: the refusal a closed port earns comes from the handshake,
         * not from here, so collapsing a full pool or a duplicate pairing into
         * ECONNREFUSED would report a local failure as the peer's answer. */
        int opened = tcp_connect(entry->sock, dst_ip, dst_port);

        if (opened < 0) {
            return opened;
        }
    }

    /* A non-blocking socket does not wait out the handshake: report it as
     * in-progress (or already done) and let the caller poll for completion. */
    if (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK)) {
        if (entry->sock->tcb && entry->sock->tcb->state == TCP_ESTABLISHED) {
            return 0;
        }
        return -EINPROGRESS;
    }

    /* Block until the handshake completes, fails, or times out, yielding to
     * other processes; a pending signal unwinds with -EINTR. */
    rc = wq_wait_event(&entry->sock->waiters, connect_done, entry->sock);
    if (rc < 0) {
        return rc;
    }

    if (!entry->sock->tcb || entry->sock->tcb->state != TCP_ESTABLISHED) {
        return -ECONNREFUSED;
    }
    return 0;
}

/* listen(2): mark a stream socket passive and open its LISTEN TCB. */
static int32_t do_listen(int fd, int backlog)
{
    fd_entry_t *entry;

    (void)backlog;   /* a fixed-size accept queue is used */

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    if (entry->sock->type == SOCK_RAW) {
        return -EOPNOTSUPP;
    }
    if (entry->sock->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }
    if (entry->sock->tcb || entry->sock->listening) {
        return -EINVAL; /* already listening or connected: a fresh TCB would leak the old one */
    }

    {
        int rc = tcp_listen(entry->sock, entry->sock->local_port);
        if (rc == 0) {
            entry->sock->listening = 1;
        }
        return rc;
    }
}

/* accept(2): dequeue a completed connection (blocking, or -EAGAIN on a
 * non-blocking listener), install a new fd for it, and report the peer address.
 */
static int32_t do_accept(int fd, uint32_t sa_ptr, uint32_t salen_ptr)
{
    fd_entry_t    *entry;
    fd_entry_t    *new_entry;
    struct socket *listen_sock;
    struct socket *conn_sock;
    int            new_fd;

    /* The peer-address writeback is validated and truncated to the caller's
     * declared length by copy_sockaddr_to_user below. */

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    if (entry->sock->type == SOCK_RAW) {
        return -EOPNOTSUPP;
    }
    listen_sock = entry->sock;

    /* A non-blocking listener returns immediately when nothing is queued. */
    if (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK) &&
        listen_sock->accept_head == listen_sock->accept_tail) {
        return -EAGAIN;
    }

    /* Block until a connection is queued, yielding to other processes; a
     * pending signal unwinds with -EINTR. */
    {
        int rc = wq_wait_event(&listen_sock->waiters, accept_ready, listen_sock);
        if (rc < 0) {
            return rc;
        }
    }

    conn_sock = listen_sock->accept_queue[listen_sock->accept_head];
    listen_sock->accept_head =
        (listen_sock->accept_head + 1) & (SOCK_ACCEPT_BACKLOG - 1);

    /* The queued socket already holds the reference created when tcp_rx
     * accepted it; the new descriptor takes ownership of that reference. */
    new_fd = fd_alloc(&current_proc->fds);
    if (new_fd < 0) {
        socket_unref(conn_sock);
        return -EMFILE;
    }
    new_entry         = &current_proc->fds.entries[new_fd];
    new_entry->type   = FD_TYPE_SOCKET;
    new_entry->sock   = conn_sock;
    new_entry->node   = NULL;
    new_entry->ofd->offset = 0;
    new_entry->flags  = FD_FLAG_READ | FD_FLAG_WRITE;

    /* Report the peer address, truncated to the caller's declared capacity. On
     * a bad user pointer, tear down the descriptor we just installed (releasing
     * its socket reference) instead of leaking the accepted fd and its socket.
     */
    if (copy_sockaddr_to_user(sa_ptr, salen_ptr, conn_sock->remote_port,
                              conn_sock->remote_ip) != 0) {
        fd_release_backing(new_entry);
        fd_free(&current_proc->fds, new_fd);
        return -EFAULT;
    }
    return new_fd;
}

/* send(2): write to a connected stream socket (the peer is the
 * connection's). */
static int32_t do_send(int fd, uint32_t buf_ptr, int len, int flags)
{
    fd_entry_t *entry;
    int         nonblock;

    if (len < 0 || !user_buffer_ok(buf_ptr, (uint32_t)len, 0)) {
        return -EFAULT;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    if (entry->sock->type == SOCK_RAW) {
        return -EOPNOTSUPP;
    }
    if (entry->sock->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }

    /* socket_tcp_send copies the payload itself, blocking on send-ring space
     * unless asked not to; __force strips __user at the boundary. */
    nonblock = (flags & MSG_DONTWAIT) ||
               (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));
    return (int32_t)socket_tcp_send(
        entry->sock, (const uint8_t __force *)(const void __user *)buf_ptr, len,
        nonblock);
}

/* recv(2): read from a connected stream socket. Honors MSG_DONTWAIT and
 * O_NONBLOCK. */
static int32_t do_recv(int fd, uint32_t buf_ptr, int len, int flags)
{
    fd_entry_t *entry;
    int         nonblock;

    if (len < 0 || !user_buffer_ok(buf_ptr, (uint32_t)len, 1)) {
        return -EFAULT;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    if (entry->sock->type == SOCK_RAW) {
        return -EOPNOTSUPP;
    }
    /* Don't block when the caller asked per-call (MSG_DONTWAIT) or the
     * descriptor is O_NONBLOCK. socket_tcp_*recv write into the validated user
     * buffer; __force strips __user at the boundary. */
    nonblock = (flags & MSG_DONTWAIT) ||
               (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));

    /* A datagram socket reads one message with no sender reported, which is
     * what `recv` is: `recvfrom` without the address. Refusing it here would
     * leave one of the four ways of reading a socket disagreeing with the
     * other three about whether the same queued datagram exists. */
    if (entry->sock->type == SOCK_DGRAM) {
        if (nonblock) {
            return (int32_t)socket_try_recv(
                entry->sock, (uint8_t __force *)(void __user *)buf_ptr, len, 0, 0);
        }
        return (int32_t)socket_recv(entry->sock,
                                    (uint8_t __force *)(void __user *)buf_ptr, len, 0, 0);
    }
    if (entry->sock->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }
    if (nonblock) {
        return (int32_t)socket_tcp_try_recv(
            entry->sock, (uint8_t __force *)(void __user *)buf_ptr, len);
    }
    return (int32_t)socket_tcp_recv(entry->sock,
                                    (uint8_t __force *)(void __user *)buf_ptr,
                                    len);
}

/* getsockname: the socket's local address, the single NIC IP (net_ip_addr) plus
 * its bound or ephemeral port. */
static int32_t do_getsockname(int fd, uint32_t sa_ptr, uint32_t salen_ptr)
{
    fd_entry_t *entry = fd_get(&current_proc->fds, fd);

    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    if (entry->sock->type == SOCK_RAW) {
        return -EOPNOTSUPP;
    }
    /* Unlike accept/recvfrom, getsockname requires the buffers
     * (copy_sockaddr_to_user treats NULL as "no address wanted"). */
    if (!sa_ptr || !salen_ptr) {
        return -EFAULT;
    }
    /* A packet socket's own address is a sockaddr_ll: the interface it is
     * bound to and the hardware address it sends from. `arping` reads the MAC
     * back from here and refuses to run without one, so the fields are the
     * machine's real ones and not a shape with zeroes in it. */
    if (entry->sock->family == AF_PACKET) {
        uint8_t   sll[SOCKADDR_LL_LEN];
        uint32_t  caller_len, copy_len;
        int       i;

        if (copy_from_user(&caller_len, (const void __user *)salen_ptr,
                           sizeof(caller_len)) != 0) {
            return -EFAULT;
        }
        k_memset(sll, 0, sizeof(sll));
        sll[0] = (uint8_t)(AF_PACKET & 0xff);
        sll[1] = (uint8_t)((AF_PACKET >> 8) & 0xff);
        sll[SOCKADDR_LL_PROTOCOL] = (uint8_t)((entry->sock->protocol >> 8) & 0xff);
        sll[SOCKADDR_LL_PROTOCOL + 1] = (uint8_t)(entry->sock->protocol & 0xff);
        sll[SOCKADDR_LL_IFINDEX] = (uint8_t)NETDEV_IFINDEX;
        sll[SOCKADDR_LL_HATYPE] = (uint8_t)ARPHRD_ETHER; /* the low byte of 1 */
        sll[SOCKADDR_LL_HALEN] = 6;
        for (i = 0; i < 6; i++) {
            sll[SOCKADDR_LL_ADDR + i] = net_mac[i];
        }
        copy_len = caller_len < (uint32_t)SOCKADDR_LL_LEN ? caller_len
                                                         : (uint32_t)SOCKADDR_LL_LEN;
        if (copy_len > 0 &&
            copy_to_user((void __user *)sa_ptr, sll, copy_len) != 0) {
            return -EFAULT;
        }
        caller_len = (uint32_t)SOCKADDR_LL_LEN; /* the true length, as Linux reports */
        if (copy_to_user((void __user *)salen_ptr, &caller_len,
                         sizeof(caller_len)) != 0) {
            return -EFAULT;
        }
        return 0;
    }
    return copy_sockaddr_to_user(sa_ptr, salen_ptr, entry->sock->local_port,
                                 (uint32_t)net_ip_addr);
}

/* getpeername: the connected peer's address (a connected stream socket
 * only). */
static int32_t do_getpeername(int fd, uint32_t sa_ptr, uint32_t salen_ptr)
{
    fd_entry_t *entry = fd_get(&current_proc->fds, fd);

    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    if (entry->sock->type == SOCK_RAW) {
        return -EOPNOTSUPP;
    }
    /* A peer exists once the TCP handshake completes (not a listener, not a
     * still-connecting SYN_SENT, not a closed socket), or once a datagram
     * socket has been connected to one, which is a peer with no handshake. */
    if (entry->sock->type == SOCK_DGRAM) {
        if (!entry->sock->connected) {
            return -ENOTCONN;
        }
    } else if (!tcp_is_connected(entry->sock)) {
        return -ENOTCONN;
    }
    if (!sa_ptr || !salen_ptr) {
        return -EFAULT;
    }
    return copy_sockaddr_to_user(sa_ptr, salen_ptr, entry->sock->remote_port,
                                 entry->sock->remote_ip);
}

/* shutdown(how): SHUT_RD(0)/SHUT_WR(1)/SHUT_RDWR(2). Any direction that
 * includes the write side drives the TCP active close (a FIN, so the peer reads
 * EOF); SHUT_RD alone is accepted as a no-op. We have no lingering half-open
 * state, so a write-side shutdown sends the FIN exactly as close() would
 * (documented). */
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2
static int32_t do_shutdown(int fd, int how)
{
    fd_entry_t *entry = fd_get(&current_proc->fds, fd);

    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    if (entry->sock->type == SOCK_RAW) {
        return -EOPNOTSUPP;
    }
    if (how < SHUT_RD || how > SHUT_RDWR) {
        return -EINVAL;
    }
    /* Only a connected stream socket can be shut down, NOT a listener (which
     * would free its LISTEN TCB and silently stop accepting), an unconnected,
     * or a UDP socket. */
    if (!tcp_is_connected(entry->sock)) {
        return -ENOTCONN;
    }
    if (how == SHUT_WR || how == SHUT_RDWR) {
        tcp_close(entry->sock); /* send FIN; idempotent vs the later close() */
    }
    return 0;
}

/* Socket-option levels/names recognized by get/setsockopt. */
#define SOL_SOCKET    1
#define SO_REUSEADDR  2
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_BROADCAST  6
#define SO_BINDTODEVICE 25
#define IFNAMSIZ     16
#define SO_KEEPALIVE  9
#define IPPROTO_TCP   6
#define TCP_NODELAY   1

/* setsockopt: accept the boolean options a forking server sets (SO_REUSEADDR,
 * TCP_NODELAY, SO_KEEPALIVE) so its setup succeeds. The stack already reuses
 * listen ports and sends promptly, so these are honored-by-behavior or
 * documented no-ops; an unknown option is accepted instead of failing a
 * server's startup. */
static int32_t do_setsockopt(int fd, int level, int optname, uint32_t optval_ptr,
                             int optlen)
{
    fd_entry_t *entry = fd_get(&current_proc->fds, fd);
    int val = 0;

    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    /* The one option this stack answers with a refusal, not a blanket
     * success. A DHCP client asks a packet socket for PACKET_AUXDATA so that it
     * can be told the kernel has not checked a datagram's UDP checksum; there
     * is no such offload here and no ancillary data at all, and a success would
     * promise data that never arrives. Refused, the client verifies the
     * checksum itself, which is the outcome that is true. */
    if (level == SOL_PACKET && optname == PACKET_AUXDATA) {
        return -ENOPROTOOPT;
    }
    (void)optname;
    /* SO_BINDTODEVICE's value is a name, not a number, and it is judged
     * against the one interface this machine has. */
    if (level == SOL_SOCKET && optname == SO_BINDTODEVICE) {
        char name[IFNAMSIZ];
        uint32_t take = (optlen < 0) ? 0u : (uint32_t)optlen;

        /* The name is at the start of whatever the caller passed, and busybox
         * passes a whole `struct ifreq` for it, 32 bytes on this machine. So
         * only the name's worth is read and a larger buffer is not a refusal.
         */
        if (take == 0) {
            return -EINVAL;
        }
        if (take > IFNAMSIZ) {
            take = IFNAMSIZ;
        }
        k_memset(name, 0, sizeof(name));
        if (!optval_ptr ||
            copy_from_user(name, (const void __user *)optval_ptr, take) != 0) {
            return -EFAULT;
        }
        name[IFNAMSIZ - 1] = '\0';
        if (name[0] == '\0') {
            entry->sock->bound_device = 0; /* an empty name unbinds */
            return 0;
        }
        if (!netif_name_is_ours(name)) {
            return -ENODEV;
        }
        entry->sock->bound_device = 1;
        return 0;
    }

    /* A declared int-sized option with a NULL value is a bad pointer, not a
     * silently-accepted no-op. */
    if (optlen >= (int)sizeof(int)) {
        if (!optval_ptr ||
            copy_from_user(&val, (const void __user *)optval_ptr, sizeof(val)) != 0) {
            return -EFAULT;
        }
    }

    /* The hop limit is real, because traceroute is nothing but this option
     * counted up: a stack that accepted it and sent every probe with the same
     * limit would answer at the first hop every time and look like a machine
     * one hop from everywhere. */
    if (level == SOL_IP && optname == IP_TTL) {
        if (optlen < (int)sizeof(int)) {
            return -EINVAL;
        }
        if (val < 1 || val > 255) {
            return -EINVAL;
        }
        entry->sock->ip_ttl = (uint8_t)val;
        return 0;
    }
    /* And the option that would hand the caller the IP header to fill in is
     * refused rather than accepted and ignored, which would send a datagram
     * whose header the caller believes it wrote. */
    if (level == SOL_IP && optname == IP_HDRINCL) {
        return -ENOPROTOOPT;
    }

    /* The two flags this stack really keeps. Everything else is accepted and
     * has no effect, which is what it has always done: refusing them would
     * stop programs that set options a small stack has no use for. */
    if (level == SOL_SOCKET && (optname == SO_BROADCAST || optname == SO_REUSEADDR)) {
        /* A flag whose value was too short to read is not a flag set to zero:
           the caller passed something this could not understand. */
        if (optlen < (int)sizeof(int)) {
            return -EINVAL;
        }
        if (optname == SO_BROADCAST) {
            entry->sock->broadcast = val ? 1 : 0;
        } else {
            entry->sock->reuseaddr = val ? 1 : 0;
        }
    }
    return 0;
}

/* getsockopt: report the integer options programs read at setup: SO_ERROR (no
 * async error is tracked, so 0), SO_TYPE (the real socket type), and the
 * boolean options as 0 (off). Always reports a 4-byte int result. */
static int32_t do_getsockopt(int fd, int level, int optname, uint32_t optval_ptr,
                             uint32_t optlen_ptr)
{
    fd_entry_t *entry = fd_get(&current_proc->fds, fd);
    int val = 0;
    uint32_t olen;

    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    if (level == SOL_SOCKET && optname == SO_TYPE) {
        val = entry->sock->type;
    } else if (level == SOL_SOCKET && optname == SO_ERROR) {
        /* Report and clear the connection's pending async error (the standard
         * non-blocking-connect completion check). Read-and-clear under net_lock
         * so the IRQ that sets it cannot race the clear. */
        uint32_t flags = net_lock();
        if (entry->sock->tcb) {
            val = (int)entry->sock->tcb->so_error;
            entry->sock->tcb->so_error = 0;
        }
        net_unlock(flags);
    } else if (level == SOL_SOCKET && optname == SO_BINDTODEVICE) {
        /* This one answers with a name, not a number. */
        char name[IFNAMSIZ];
        uint32_t give;

        k_memset(name, 0, sizeof(name));
        if (entry->sock->bound_device) {
            k_strncpy(name, NETDEV_IFNAME, sizeof(name) - 1);
        }
        if (!optval_ptr || !optlen_ptr) {
            return -EFAULT;
        }
        if (copy_from_user(&olen, (const void __user *)optlen_ptr, sizeof(olen)) != 0) {
            return -EFAULT;
        }
        give = k_strlen(name) + 1u;
        if (give > olen) {
            give = olen;
        }
        if (give > 0 && copy_to_user((void __user *)optval_ptr, name, give) != 0) {
            return -EFAULT;
        }
        if (copy_to_user((void __user *)optlen_ptr, &give, sizeof(give)) != 0) {
            return -EFAULT;
        }
        return 0;
    } else if (level == SOL_IP && optname == IP_TTL) {
        val = (int)entry->sock->ip_ttl;
    } else if (level == SOL_IP && optname == IP_HDRINCL) {
        return -ENOPROTOOPT;
    } else if (level == SOL_SOCKET && optname == SO_BROADCAST) {
        val = entry->sock->broadcast;
    } else if (level == SOL_SOCKET && optname == SO_REUSEADDR) {
        val = entry->sock->reuseaddr;
    } else {
        val = 0; /* SO_KEEPALIVE / TCP_NODELAY and the rest: off/none */
    }
    if (!optval_ptr || !optlen_ptr) {
        return -EFAULT;
    }
    if (copy_from_user(&olen, (const void __user *)optlen_ptr, sizeof(olen)) != 0) {
        return -EFAULT;
    }
    if (olen > sizeof(val)) {
        olen = (uint32_t)sizeof(val);
    }
    if (olen > 0 && copy_to_user((void __user *)optval_ptr, &val, olen) != 0) {
        return -EFAULT;
    }
    /* The count actually written, which for a caller that offered less than an
       int is less than an int: telling it four would be describing bytes that
       were never put there. */
    if (copy_to_user((void __user *)optlen_ptr, &olen, sizeof(olen)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/*
 * struct msghdr on i386: name (4), namelen (4), iov (4), iovlen (4), control
 * (4), controllen (4), flags (4). The offsets are the ABI and are written out,
 * not modelled, because a struct here would have to match a layout
 * this kernel does not otherwise depend on.
 */
#define MSGHDR_LEN         28
#define MSGHDR_NAME        0
#define MSGHDR_NAMELEN     4
#define MSGHDR_IOV         8
#define MSGHDR_IOVLEN      12
#define MSGHDR_CONTROLLEN  20
#define MSGHDR_FLAGS       24

/**
 * Receive one datagram into a scatter list (recvmsg, socketcall 17).
 *
 * The DHCP client is what needs this: it reads its raw socket through
 * `recvmsg`, not `read` so that it can look for `PACKET_AUXDATA` in the
 * control data, and finding none it verifies the UDP checksum itself. So the
 * scatter half is the whole of the work here. The control buffer is answered
 * with a length of zero, which is the true answer: this stack attaches no
 * ancillary data to anything.
 *
 * The sender is reported through `msg_name`: a `sockaddr_in` for an AF_INET
 * socket, and for a packet socket the `sockaddr_ll` the frame came from, with
 * the sender's hardware address and who the frame was addressed to.
 */
static int32_t do_recvmsg(int fd, uint32_t msg_ptr, int flags)
{
    uint8_t hdr[MSGHDR_LEN];
    fd_entry_t *entry;
    uint32_t iov_user, name_ptr, namelen;
    uint32_t addr_len, addr_give;
    uint32_t iovlen;
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    uint32_t i;
    int32_t taken;

    if (copy_from_user(hdr, (const void __user *)msg_ptr, MSGHDR_LEN) != 0) {
        return -EFAULT;
    }
    entry = current_proc ? fd_get(&current_proc->fds, fd) : (fd_entry_t *)0;
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    if (entry->sock->type != SOCK_DGRAM) {
        return -EOPNOTSUPP;
    }

    name_ptr = SC_LE32(hdr + MSGHDR_NAME);
    namelen  = SC_LE32(hdr + MSGHDR_NAMELEN);
    iov_user = SC_LE32(hdr + MSGHDR_IOV);
    iovlen   = SC_LE32(hdr + MSGHDR_IOVLEN);
    if (iovlen > (uint32_t)IOV_MAX_LOCAL) {
        return -EINVAL;
    }

    /* Every destination is checked before the datagram is taken, because
       taking it is what destroys it: a fault discovered half way through the
       copy would report an error for a message the machine no longer has.
       This is the pass `sys_readv` makes for the same reason. */
    if (!user_buffer_ok(msg_ptr, MSGHDR_LEN, 1)) {
        return -EFAULT;
    }
    /* Only what will be written is checked, and it is settled here rather than
       after the datagram is taken: a packet socket reports twenty bytes and an
       AF_INET one sixteen, however much the caller declared. Validating the
       whole declared capacity would refuse a call whose address fits
       perfectly. */
    addr_len = (entry->sock->family == AF_PACKET) ? (uint32_t)SOCKADDR_LL_LEN
                                                  : (uint32_t)SOCKADDR_IN_LEN;
    addr_give = namelen < addr_len ? namelen : addr_len;
    if (name_ptr && addr_give > 0 && !user_buffer_ok(name_ptr, addr_give, 1)) {
        return -EFAULT;
    }
    for (i = 0; i < iovlen; i++) {
        struct kernel_iovec iov;

        if (copy_from_user(&iov,
                           (const void __user *)(iov_user +
                                                 i * (uint32_t)sizeof(struct kernel_iovec)),
                           sizeof(iov)) != 0) {
            return -EFAULT;
        }
        if (iov.iov_len == 0) {
            continue;
        }
        if (iov.iov_len > 0x7FFFFFFFu || !user_buffer_ok(iov.iov_base, iov.iov_len, 1)) {
            return -EFAULT;
        }
    }

    if ((flags & MSG_DONTWAIT) && socket_dgram_pending(entry->sock) < 0) {
        return -EAGAIN;
    }

    taken = readv_dgram(entry, iov_user, (int)iovlen, &src_ip, &src_port);
    if (taken < 0) {
        return taken;
    }

    /* A struct sockaddr_in is 16 bytes (family, port, address, and eight bytes
       of padding a caller may compare against sizeof) and a sockaddr_ll is 20.
       Either is written whole where it fits, truncated where the caller's
       buffer is smaller, and the true length is reported either way, which is
       what Linux does. */
    if (name_ptr && addr_len > 0) {
        uint8_t sa[SOCKADDR_LL_LEN];

        k_memset(sa, 0, sizeof(sa));
        if (entry->sock->family == AF_PACKET) {
            if (packet_last_addr(entry->sock, sa) == 0) {
                addr_len = 0;
                addr_give = 0;
            }
        } else {
            sa[0] = (uint8_t)(AF_INET & 0xff);
            SC_WBE16(sa + 2, src_port);
            SC_WBE32(sa + 4, src_ip);
        }
        if (addr_give > 0 && copy_to_user((void __user *)name_ptr, sa, addr_give) != 0) {
            return -EFAULT;
        }
    }
    /* The true length, whatever the caller's capacity was: that is how a caller
       learns its buffer was too small. Zero for a packet socket, which has no
       sender to report. */
    SC_WLE32(hdr + MSGHDR_NAMELEN, name_ptr ? addr_len : 0u);
    SC_WLE32(hdr + MSGHDR_CONTROLLEN, 0);
    SC_WLE32(hdr + MSGHDR_FLAGS, 0);
    if (copy_to_user((void __user *)msg_ptr, hdr, MSGHDR_LEN) != 0) {
        return -EFAULT;
    }
    return taken;
}

/**
 * Multiplexed socket syscall (socketcall, syscall 102).
 *
 * EBX is the SC_* sub-call number, ECX a user pointer to its argument array.
 * Copies the right number of 32-bit args in one fault-checked read, then
 * dispatches to the matching do_* helper. Returns its result, or -EINVAL for an
 * unknown sub-call.
 */
static int32_t sys_socketcall(isr_params_t *params)
{
    int      call = (int)params->ebx;
    uint32_t argp = params->ecx;
    uint32_t args[6];
    int      nargs;

    switch (call) {
    case SC_PING:                       nargs = 1; break;
    case SC_LISTEN: case SC_SHUTDOWN:   nargs = 2; break;
    case SC_SOCKET: case SC_BIND:
    case SC_CONNECT: case SC_ACCEPT:
    case SC_GETSOCKNAME:
    case SC_GETPEERNAME:                nargs = 3; break;
    case SC_RECVMSG:                    nargs = 3; break;
    case SC_SEND: case SC_RECV:         nargs = 4; break;
    case SC_SETSOCKOPT:
    case SC_GETSOCKOPT:                 nargs = 5; break;
    case SC_SENDTO: case SC_RECVFROM:   nargs = 6; break;
    default:
        return -EINVAL;
    }

    /* Pull the argument array out of user memory in one fault-checked copy. */
    if (copy_from_user(args, (const void __user *)argp,
                       (uint32_t)nargs * 4) != 0) {
        return -EFAULT;
    }

    switch (call) {
    case SC_SOCKET:
        return do_socket((int)args[0], (int)args[1], (int)args[2]);
    case SC_BIND:
        return do_bind((int)args[0], args[1], (int)args[2]);
    case SC_SENDTO:
        return do_sendto((int)args[0], args[1], (int)args[2],
                         (int)args[3], args[4], (int)args[5]);
    case SC_RECVFROM:
        return do_recvfrom((int)args[0], args[1], (int)args[2],
                           (int)args[3], args[4], args[5]);
    case SC_PING:
        return do_ping(args[0]);
    case SC_CONNECT:
        return do_connect((int)args[0], args[1], (int)args[2]);
    case SC_LISTEN:
        return do_listen((int)args[0], (int)args[1]);
    case SC_ACCEPT:
        return do_accept((int)args[0], args[1], args[2]);
    case SC_GETSOCKNAME:
        return do_getsockname((int)args[0], args[1], args[2]);
    case SC_GETPEERNAME:
        return do_getpeername((int)args[0], args[1], args[2]);
    case SC_SHUTDOWN:
        return do_shutdown((int)args[0], (int)args[1]);
    case SC_SETSOCKOPT:
        return do_setsockopt((int)args[0], (int)args[1], (int)args[2], args[3],
                             (int)args[4]);
    case SC_GETSOCKOPT:
        return do_getsockopt((int)args[0], (int)args[1], (int)args[2], args[3],
                             args[4]);
    case SC_SEND:
        return do_send((int)args[0], args[1], (int)args[2], (int)args[3]);
    case SC_RECV:
        return do_recv((int)args[0], args[1], (int)args[2], (int)args[3]);
    case SC_RECVMSG:
        return do_recvmsg((int)args[0], args[1], (int)args[2]);
    default:
        return -EINVAL;
    }
}

/**
 * Set up thread-local storage (set_thread_area(2), Linux i386).
 *
 * EBX points at a struct user_desc. Validates it, programs the shared TLS
 * GDT descriptor (entry GDT_TLS_ENTRY) and this process's PCB base, then
 * writes the chosen entry number back so the C library can load
 * (entry_number << 3) | 3 into %gs. The base is reapplied to the GDT on
 * every context switch (switch.s). Returns 0 or a negative errno on a bad
 * pointer/request.
 */
static int32_t sys_set_thread_area(isr_params_t *params)
{
    uint32_t uptr = params->ebx;
    user_desc_t desc;
    uint32_t base = 0;

    if (!current_proc) {
        return -ESRCH;  /* TLS is only meaningful for a user process */
    }

    /* Read the descriptor through the fault-checked helper (it range-checks the
     * full struct internally, so the prior explicit user_access_ok is
     * redundant). */
    if (copy_from_user(&desc, (const void __user *)uptr, sizeof(user_desc_t)) != 0) {
        return -EFAULT;
    }

    if (tls_resolve_base(&desc, &base) != 0) {
        return -EINVAL;
    }

    /* Program the descriptor now and remember the base so the context switch
     * can reprogram entry GDT_TLS_ENTRY for this process when it is resumed. */
    gdt_set_tls(base);
    current_proc->tls_base = base;

    /* Report the allocated slot back (musl reads user_desc.entry_number). */
    desc.entry_number = GDT_TLS_ENTRY;
    if (copy_to_user((void __user *)uptr, &desc.entry_number,
                     sizeof(desc.entry_number)) != 0) {
        return -EFAULT;
    }

    return 0;
}

/*
 * sys_getpid - return the calling process's PID (getpid(2)).
 */
static int32_t sys_getpid(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
    return (int32_t)current_proc->pid;
}

/**
 * Create a new session (setsid(2)).
 *
 * The caller becomes the leader of a new session AND a new process group,
 * both numbered with its pid. Fails with -EPERM if the caller already leads
 * a process group (a group leader cannot carry its group into a new
 * session). Returns the new session id (the caller's pid).
 */
static int32_t sys_setsid(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
    if (current_proc->pgid == current_proc->pid) {
        return -EPERM; /* already a process-group leader */
    }
    current_proc->sid = current_proc->pid;
    current_proc->pgid = current_proc->pid;
    return (int32_t)current_proc->pid;
}

/**
 * Set a process's process group (setpgid(2)).
 *
 * EBX = pid (0 = caller), ECX = pgid (0 = the target's own pid, i.e. make it
 * a group leader). The target must be the caller or a child of the caller,
 * must not be a session leader, and the destination group must live in the
 * target's session. Returns 0 or -errno.
 */
static int32_t sys_setpgid(isr_params_t *params)
{
    int pid = (int)params->ebx;
    int pgid = (int)params->ecx;
    process_t *target;

    if (!current_proc) {
        return -ESRCH;
    }
    if (pgid < 0) {
        return -EINVAL;
    }
    target = (pid == 0) ? current_proc : get_process((uint32_t)pid);
    if (!target || target->state == PROC_UNUSED || target->state == PROC_ZOMBIE) {
        return -ESRCH;
    }
    /* Only the caller or one of its children may be moved. */
    if (target != current_proc && target->parent_pid != current_proc->pid) {
        return -ESRCH;
    }
    /* A session leader's group is fixed (it anchors the session). */
    if (target->sid == target->pid) {
        return -EPERM;
    }
    if (pgid == 0) {
        pgid = (int)target->pid; /* create/lead the group named by the target */
    }
    /* The destination group must be in the target's session: either the target
     * creates its own group, or a process is ALREADY in group `pgid` within the
     * same session (an existing group, regardless of whether its leader is
     * still alive, not merely "a process whose pid == pgid"). */
    if ((uint32_t)pgid != target->pid && !pgrp_in_session(pgid, (int)target->sid)) {
        return -EPERM;
    }
    target->pgid = (uint32_t)pgid;
    return 0;
}

/* Resolve a getpgid/getsid target: the caller for pid 0, else the named
 * process. */
static process_t *pgrp_target(int pid)
{
    if (pid == 0) {
        return current_proc;
    }
    return get_process((uint32_t)pid);
}

/*
 * sys_getpgid - return a process's process-group id (getpgid(2)).
 * Args: EBX = pid (0 = caller).
 */
static int32_t sys_getpgid(isr_params_t *params)
{
    process_t *target;

    if (!current_proc) {
        return -ESRCH;
    }
    target = pgrp_target((int)params->ebx);
    if (!target) {
        return -ESRCH;
    }
    return (int32_t)target->pgid;
}

/*
 * sys_getpgrp - the caller's process-group id (getpgrp(2), the no-arg form).
 */
static int32_t sys_getpgrp(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
    return (int32_t)current_proc->pgid;
}

/*
 * sys_getsid - return a process's session id (getsid(2)).
 * Args: EBX = pid (0 = caller).
 */
static int32_t sys_getsid(isr_params_t *params)
{
    process_t *target;

    if (!current_proc) {
        return -ESRCH;
    }
    target = pgrp_target((int)params->ebx);
    if (!target) {
        return -ESRCH;
    }
    return (int32_t)target->sid;
}

/*
 * sys_getppid - return the parent's PID (getppid(2)).
 */
static int32_t sys_getppid(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
    return (int32_t)current_proc->parent_pid;
}

/*
 * sys_umask - set the file-creation mask, returning the previous value
 * (umask(2)). Args: EBX = new mask (only the low 9 permission bits are
 * kept).
 */
static int32_t sys_umask(isr_params_t *params)
{
    uint32_t old;

    if (!current_proc) {
        return -ESRCH;
    }
    old = current_proc->umask;
    current_proc->umask = params->ebx & 0777;
    return (int32_t)old;
}

/**
 * Process times (times(2)).
 *
 * Per-process CPU time is not tracked, so the four tms fields are reported
 * as zero; the return value is the monotonic tick count (already in CLK_TCK
 * = 100 Hz units). EBX = struct tms * (NULL is allowed: the caller may want
 * only the return value).
 */
static int32_t sys_times(isr_params_t *params)
{
    struct kernel_tms {
        int32_t tms_utime;
        int32_t tms_stime;
        int32_t tms_cutime;
        int32_t tms_cstime;
    } t;

    if (params->ebx) {
        t.tms_utime = 0;
        t.tms_stime = 0;
        t.tms_cutime = 0;
        t.tms_cstime = 0;
        if (copy_to_user((void __user *)params->ebx, &t, sizeof(t)) != 0) {
            return -EFAULT;
        }
    }
    return (int32_t)kernel_ticks;
}

/**
 * System statistics (sysinfo(2)).
 *
 * Fills the 64-byte i386 struct sysinfo: uptime in seconds, a live-process
 * count, mem_unit = 1; CPU load and memory/swap figures are reported as zero
 * (not tracked). EBX = struct sysinfo *.
 */
static int32_t sys_sysinfo(isr_params_t *params)
{
    struct kernel_sysinfo {
        int32_t uptime;
        uint32_t loads[3];
        uint32_t totalram;
        uint32_t freeram;
        uint32_t sharedram;
        uint32_t bufferram;
        uint32_t totalswap;
        uint32_t freeswap;
        uint16_t procs;
        uint16_t pad;
        uint32_t totalhigh;
        uint32_t freehigh;
        uint32_t mem_unit;
        char _f[8];
    } si;
    uint32_t i;
    int n = 0;

    if (!params->ebx) {
        return -EFAULT;
    }
    for (i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state != PROC_UNUSED) {
            n++;
        }
    }
    /* Fill every field of the ABI struct explicitly: uptime, the process count,
     * mem_unit, and physical RAM (from the frame bitmap) are real; load average
     * and swap are not tracked, so they are reported as zero. */
    si.uptime = (int32_t)(kernel_ticks / 100);
    si.loads[0] = 0;
    si.loads[1] = 0;
    si.loads[2] = 0;
    si.totalram = frame_total_count() * PAGE_SIZE;
    si.freeram = frame_free_count() * PAGE_SIZE;
    si.sharedram = 0;
    si.bufferram = 0;
    si.totalswap = 0;
    si.freeswap = 0;
    si.procs = (uint16_t)n;
    si.pad = 0;
    si.totalhigh = 0;
    si.freehigh = 0;
    si.mem_unit = 1;
    for (i = 0; i < sizeof(si._f); i++) {
        si._f[i] = 0;
    }
    if (copy_to_user((void __user *)params->ebx, &si, sizeof(si)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * Record the clear-child-tid pointer and return the tid (set_tid_address(2)).
 *
 * Rach2 has no threads, so this only returns the caller's PID as its thread id;
 * the pointer (EBX) is accepted and ignored.
 */
static int32_t sys_set_tid_address(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
    return (int32_t)current_proc->pid;
}

/*
 * uname(2): fill the six 65-byte fields of struct utsname (new-style, as musl
 * expects) in user memory. EBX = struct utsname *.
 */
struct kernel_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

/* The mutable host name (uname's nodename, what gethostname reads, sethostname
 * sets). Single global; Rach2 is one host. */
static char k_hostname[65] = "rach2";

/* Copy a C string into a fixed 65-byte utsname field (always
 * NUL-terminated). */
static void uname_field(char *dst, const char *src)
{
    uint32_t n = k_strlen(src);

    if (n > 64) {
        n = 64;
    }
    k_memcpy(dst, src, n);
    dst[n] = '\0';
}

static int32_t sys_uname(isr_params_t *params)
{
    struct kernel_utsname __user *user_uts =
        (struct kernel_utsname __user *)params->ebx;
    struct kernel_utsname uts;

    k_memset(&uts, 0, sizeof(uts));
    uname_field(uts.sysname, "Rach2");
    uname_field(uts.nodename, k_hostname);
    uname_field(uts.release, "1.0.0");
    uname_field(uts.version, "Rach2 musl port");
    uname_field(uts.machine, "i686");
    uname_field(uts.domainname, "(none)");

    if (copy_to_user(user_uts, &uts, sizeof(uts)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/* sethostname(74): set the single mutable host name (uname's nodename, what
 * gethostname reads back). */
static int32_t sys_sethostname(isr_params_t *params)
{
    const char __user *name = (const char __user *)params->ebx;
    int len = (int)params->ecx;
    char buf[65];

    if (len < 0 || len > 64) {
        return -EINVAL;
    }
    if (len > 0 && copy_from_user(buf, name, (uint32_t)len) != 0) {
        return -EFAULT;
    }
    buf[len] = '\0';
    k_memcpy(k_hostname, buf, (uint32_t)len + 1);
    return 0;
}

/* The id setters: the V7 rule with POSIX saved IDs, over the PCB credentials.
 * Privilege is effective uid 0 for the uid and gid families alike. A privileged
 * caller sets anything; an unprivileged one only swaps among its own real,
 * effective, and saved ids, so a shed identity cannot be regained. The saved id
 * follows POSIX: a 2-argument call that sets the real id, or sets the effective
 * id away from the pre-call real id, also saves the new effective id. `KEEP`
 * ((id_t)-1) leaves a field unchanged. */
#define SC_ID_KEEP 0xFFFFFFFFu

static int32_t sys_setuid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t uid = params->ebx;

    if (cred_privileged()) {
        p->ruid = uid;
        p->euid = uid;
        p->suid = uid;
        return 0;
    }
    if (uid == p->ruid || uid == p->suid) {
        p->euid = uid;
        return 0;
    }
    return -EPERM;
}
static int32_t sys_setgid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t gid = params->ebx;

    if (cred_privileged()) {
        p->rgid = gid;
        p->egid = gid;
        p->sgid = gid;
        return 0;
    }
    if (gid == p->rgid || gid == p->sgid) {
        p->egid = gid;
        return 0;
    }
    return -EPERM;
}
static int32_t sys_setreuid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t r = params->ebx;
    uint32_t e = params->ecx;
    uint32_t new_r = (r == SC_ID_KEEP) ? p->ruid : r;
    uint32_t new_e = (e == SC_ID_KEEP) ? p->euid : e;

    if (!cred_privileged()) {
        if (r != SC_ID_KEEP && new_r != p->ruid && new_r != p->euid) {
            return -EPERM;
        }
        if (e != SC_ID_KEEP && new_e != p->ruid && new_e != p->euid &&
            new_e != p->suid) {
            return -EPERM;
        }
    }
    if (r != SC_ID_KEEP || (e != SC_ID_KEEP && new_e != p->ruid)) {
        p->suid = new_e;
    }
    p->ruid = new_r;
    p->euid = new_e;
    return 0;
}
static int32_t sys_setregid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t r = params->ebx;
    uint32_t e = params->ecx;
    uint32_t new_r = (r == SC_ID_KEEP) ? p->rgid : r;
    uint32_t new_e = (e == SC_ID_KEEP) ? p->egid : e;

    if (!cred_privileged()) {
        if (r != SC_ID_KEEP && new_r != p->rgid && new_r != p->egid) {
            return -EPERM;
        }
        if (e != SC_ID_KEEP && new_e != p->rgid && new_e != p->egid &&
            new_e != p->sgid) {
            return -EPERM;
        }
    }
    if (r != SC_ID_KEEP || (e != SC_ID_KEEP && new_e != p->rgid)) {
        p->sgid = new_e;
    }
    p->rgid = new_r;
    p->egid = new_e;
    return 0;
}
static int32_t sys_setresuid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t r = params->ebx;
    uint32_t e = params->ecx;
    uint32_t s = params->edx;

    if (!cred_privileged()) {
        uint32_t v[3];
        int i;
        v[0] = r;
        v[1] = e;
        v[2] = s;
        for (i = 0; i < 3; i++) {
            if (v[i] != SC_ID_KEEP && v[i] != p->ruid && v[i] != p->euid &&
                v[i] != p->suid) {
                return -EPERM;
            }
        }
    }
    if (r != SC_ID_KEEP) {
        p->ruid = r;
    }
    if (e != SC_ID_KEEP) {
        p->euid = e;
    }
    if (s != SC_ID_KEEP) {
        p->suid = s;
    }
    return 0;
}
static int32_t sys_setresgid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t r = params->ebx;
    uint32_t e = params->ecx;
    uint32_t s = params->edx;

    if (!cred_privileged()) {
        uint32_t v[3];
        int i;
        v[0] = r;
        v[1] = e;
        v[2] = s;
        for (i = 0; i < 3; i++) {
            if (v[i] != SC_ID_KEEP && v[i] != p->rgid && v[i] != p->egid &&
                v[i] != p->sgid) {
                return -EPERM;
            }
        }
    }
    if (r != SC_ID_KEEP) {
        p->rgid = r;
    }
    if (e != SC_ID_KEEP) {
        p->egid = e;
    }
    if (s != SC_ID_KEEP) {
        p->sgid = s;
    }
    return 0;
}
/* getresuid/getresgid (the *32 forms): report all three ids through the three
 * user pointers, the introspection half of the saved-id model. */
static int32_t sys_getresuid(isr_params_t *params)
{
    process_t *p = current_proc;

    if (copy_to_user((void __user *) params->ebx, &p->ruid,
                     sizeof(p->ruid)) != 0 ||
        copy_to_user((void __user *) params->ecx, &p->euid,
                     sizeof(p->euid)) != 0 ||
        copy_to_user((void __user *) params->edx, &p->suid,
                     sizeof(p->suid)) != 0) {
        return -EFAULT;
    }
    return 0;
}
static int32_t sys_getresgid(isr_params_t *params)
{
    process_t *p = current_proc;

    if (copy_to_user((void __user *) params->ebx, &p->rgid,
                     sizeof(p->rgid)) != 0 ||
        copy_to_user((void __user *) params->ecx, &p->egid,
                     sizeof(p->egid)) != 0 ||
        copy_to_user((void __user *) params->edx, &p->sgid,
                     sizeof(p->sgid)) != 0) {
        return -EFAULT;
    }
    return 0;
}
/* setgroups/getgroups (the *32 forms): the supplementary list, privileged to
 * set (effective uid 0), NGROUPS_MAX entries. getgroups with size 0 reports the
 * count, the POSIX sizing idiom. */
static int32_t sys_setgroups(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t size = params->ebx;
    uint32_t list_ptr = params->ecx;
    uint32_t tmp[NGROUPS_MAX];
    uint32_t i;

    if (size > NGROUPS_MAX) {
        return -EINVAL; /* sized before privilege, the order Linux reports */
    }
    if (!cred_privileged()) {
        return -EPERM;
    }
    for (i = 0; i < size; i++) {
        if (copy_from_user(&tmp[i],
                           (const void __user *)(list_ptr + i * 4),
                           sizeof(tmp[i])) != 0) {
            return -EFAULT;
        }
    }
    for (i = 0; i < size; i++) {
        p->groups[i] = tmp[i];
    }
    p->ngroups = size;
    return 0;
}
static int32_t sys_getgroups(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t size = params->ebx;
    uint32_t list_ptr = params->ecx;
    uint32_t i;

    if ((int32_t) size < 0) {
        return -EINVAL; /* the ABI size is signed; a negative is not a count */
    }
    if (size == 0) {
        return (int32_t) p->ngroups;
    }
    if (size < p->ngroups) {
        return -EINVAL;
    }
    for (i = 0; i < p->ngroups; i++) {
        if (copy_to_user((void __user *)(list_ptr + i * 4), &p->groups[i],
                         sizeof(p->groups[i])) != 0) {
            return -EFAULT;
        }
    }
    return (int32_t) p->ngroups;
}

/* The real resource caps: RLIMIT_NOFILE/NPROC mirror the kernel's fixed pools,
 * RLIMIT_STACK the user stack growth cap; the rest are effectively unlimited.
 * cur == max. */
#define RLIMIT_STACK_RES  3
#define RLIMIT_NPROC_RES  6
#define RLIMIT_NOFILE_RES 7
#define RLIM_INF32 0xFFFFFFFFu
static void rlimit_for(uint32_t resource, uint32_t *cur, uint32_t *max)
{
    uint32_t v;

    switch (resource) {
    case RLIMIT_NOFILE_RES:
        v = (uint32_t)MAX_FDS;
        break;
    case RLIMIT_NPROC_RES:
        v = (uint32_t)MAX_PROCS;
        break;
    case RLIMIT_STACK_RES:
        v = 0x800000u; /* 8 MB user-stack growth cap (documented) */
        break;
    default:
        v = RLIM_INF32;
        break;
    }
    *cur = v;
    *max = v;
}

/* getrlimit(76)/ugetrlimit(191): 32-bit struct rlimit { u32 rlim_cur,
 * rlim_max }. */
static int32_t sys_getrlimit(isr_params_t *params)
{
    uint32_t resource = params->ebx;
    uint32_t rl = params->ecx;
    uint32_t out[2];

    if (!rl) {
        return -EFAULT;
    }
    rlimit_for(resource, &out[0], &out[1]);
    if (copy_to_user((void __user *)rl, out, sizeof(out)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/* prlimit64(340): EBX=pid (0/self), ECX=resource, EDX=new (rlimit64* or NULL),
 * ESI=old (rlimit64* or NULL). 64-bit struct rlimit64 { u64 cur, max }. New
 * limits are accepted (validated readable) but not enforced; old limits report
 * the real caps. This is the form modern musl getrlimit/setrlimit issue. */
static int32_t sys_prlimit64(isr_params_t *params)
{
    uint32_t resource = params->ecx;
    uint32_t newp = params->edx;
    uint32_t oldp = params->esi;
    uint32_t cur32, max32;

    if (oldp) {
        uint32_t out[4]; /* cur_lo, cur_hi, max_lo, max_hi */
        rlimit_for(resource, &cur32, &max32);
        out[0] = cur32;
        out[1] = (cur32 == RLIM_INF32) ? 0xFFFFFFFFu : 0u; /* 64-bit RLIM_INFINITY */
        out[2] = max32;
        out[3] = (max32 == RLIM_INF32) ? 0xFFFFFFFFu : 0u;
        if (copy_to_user((void __user *)oldp, out, sizeof(out)) != 0) {
            return -EFAULT;
        }
    }
    if (newp) {
        uint32_t in[4];
        if (copy_from_user(in, (const void __user *)newp, sizeof(in)) != 0) {
            return -EFAULT;
        }
    }
    return 0;
}

/* dup3(330): like dup2 but oldfd == newfd is -EINVAL and a flag can set
 * O_CLOEXEC on the new descriptor. */
static int32_t sys_dup3(isr_params_t *params)
{
    int oldfd = (int)params->ebx;
    int newfd = (int)params->ecx;
    uint32_t flags = params->edx;
    int32_t r;

    if (oldfd == newfd) {
        return -EINVAL;
    }
    if (flags & ~(uint32_t)O_CLOEXEC) {
        return -EINVAL;
    }
    r = sys_dup2(params); /* shares EBX=oldfd, ECX=newfd */
    if (r >= 0 && (flags & O_CLOEXEC)) {
        fd_entry_t *e = fd_get(&current_proc->fds, newfd);
        if (e) {
            e->flags |= FD_FLAG_CLOEXEC;
        }
    }
    return r;
}

/*
 * mmap2 protection bits and flags (Linux i386 ABI). We support PROT_* as VMA
 * permissions and only anonymous private mappings; file-backed mmap would need
 * a page cache.
 */
#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define PROT_EXEC     0x4
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* Round 'v' up to the next page boundary. */
#define PAGE_UP(v) (((v) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

/*
 * sys_brk - set the program break (heap top). Arg: EBX = requested break.
 * Returns the resulting break: the new value on success, or the unchanged
 * current break if the request is out of range. This is the contract musl's
 * __brk relies on (it compares the return to its request). The heap is a
 * single demand-paged VMA [heap_start, page_up(brk)); growing only extends
 * the region (pages fault in lazily), shrinking frees the pages above.
 */
static int32_t sys_brk(isr_params_t *params)
{
    uint32_t req = params->ebx;
    vm_space_t *vm;
    vma_t *heap;
    uint32_t i, old_top, new_top;

    if (!current_proc) {
        return -ESRCH;
    }
    vm = &current_proc->vm;

    /* brk(0) queries the break; below the heap base or into the stack window is
     * refused with the break left unchanged. */
    if (req < vm->heap_start || req > vm->stack_limit) {
        return (int32_t)vm->brk;
    }

    old_top = PAGE_UP(vm->brk);
    new_top = PAGE_UP(req);

    /* Find the heap region, if the heap has ever grown above its base. */
    heap = NULL;
    for (i = 0; i < vm->count; i++) {
        if (vm->vmas[i].type == VM_HEAP) {
            heap = &vm->vmas[i];
            break;
        }
    }

    if (new_top > old_top) {
        /* Grow: the new pages [old_top, new_top) must not collide with mmap or
         * any other region. The heap's own [heap_start, old_top) abuts that
         * span without overlapping, so vm_overlaps tests only the *other*
         * regions. */
        if (vm_overlaps(vm, old_top, new_top)) {
            return (int32_t)vm->brk;
        }
        /* Extend the region, or create it on the first non-empty break. */
        if (heap) {
            heap->end = new_top;
        } else if (vm_insert(vm, vm->heap_start, new_top,
                             VM_PROT_READ | VM_PROT_WRITE, VM_HEAP) != 0) {
            return (int32_t)vm->brk;
        }
    } else if (new_top < old_top) {
        /* Shrink: free the pages above the new top, then trim or drop the
         * VMA. */
        unmap_user_range(_curr_dir, new_top, old_top);
        if (heap) {
            if (new_top <= vm->heap_start) {
                vm_remove(vm, heap->start, heap->end);
            } else {
                heap->end = new_top;
            }
        }
    }

    vm->brk = req;
    return (int32_t)req;
}

/**
 * Say on the serial log that a process ran out of mappings, once per process.
 *
 * `vm.c` is deliberately independent of the kernel's globals, so it cannot
 * name the process it refused; here the caller is known. Silence is what this
 * replaces, and it cost hours: a program whose library ran out of mappings
 * looks exactly like a program whose library failed for its own reasons. Said
 * once per process, because a program that meets the ceiling meets it in a
 * loop and a flood of lines would bury the first one. The mark is in the
 * process, not a remembered pid: a pid is reused, and the next process to hold
 * that number would be silenced by a report it never made.
 */
static void vma_exhausted(const char *what)
{
    if (!current_proc || current_proc->vma_reported) {
        return;
    }
    current_proc->vma_reported = 1;
    klog(KLOG_WARN, "vm", "%s: pid %d (%s) has used all %d of its mappings", what,
                  (int)current_proc->pid, current_proc->comm, VMA_MAX);
}

/*
 * sys_mmap2 - map anonymous memory. Args: EBX=addr (hint, ignored),
 * ECX=length, EDX=prot, ESI=flags, EDI=fd, EBP=pgoffset. Returns the mapped
 * base address, or -errno. Only MAP_ANONYMOUS|MAP_PRIVATE is supported (what
 * musl's allocator uses); the region is placed by bumping mmap_top down and
 * is demand-paged + zero-filled on touch.
 */
static int32_t sys_mmap2(isr_params_t *params)
{
    uint32_t length = params->ecx;
    uint32_t prot = params->edx;
    uint32_t flags = params->esi;
    vm_space_t *vm;
    uint32_t len, base, vmprot;

    if (!current_proc) {
        return -ESRCH;
    }
    vm = &current_proc->vm;

    if (length == 0) {
        return -EINVAL;
    }
    if (!(flags & MAP_ANONYMOUS)) {
        return -ENOSYS; /* file-backed mmap is not implemented */
    }
    if (flags & MAP_FIXED) {
        /* We choose the address ourselves; honoring a fixed hint would need
         * overlap/replace handling we do not have. Reject, not silently
         * return a different address the caller would then fault on. */
        return -ENOSYS;
    }

    len = PAGE_UP(length);

    vmprot = 0;
    if (prot & PROT_READ) {
        vmprot |= VM_PROT_READ;
    }
    if (prot & PROT_WRITE) {
        /* On i386 a present page is always readable (there is no write-only
         * PTE) so writable implies readable. Record that in the VMA prot so the
         * fault check agrees with the hardware (a read of a PROT_WRITE page
         * does not wrongly segfault). */
        vmprot |= VM_PROT_WRITE | VM_PROT_READ;
    }
    if (prot & PROT_EXEC) {
        vmprot |= VM_PROT_EXEC;
    }

    /* Bump down from mmap_top; refuse if it would underflow or hit the heap.
     * The addr hint is ignored (no MAP_FIXED support), which suits musl's
     * malloc. */
    if (len > vm->mmap_top || vm->mmap_top - len < PAGE_UP(vm->brk)) {
        return -ENOMEM;
    }
    base = vm->mmap_top - len;

    if (vm_insert(vm, base, base + len, vmprot, VM_ANON) != 0) {
        vma_exhausted("mmap");
        return -ENOMEM;
    }
    vm->mmap_top = base;
    return (int32_t)base;
}

/*
 * sys_munmap - unmap a range. Args: EBX=addr (page-aligned), ECX=length.
 * Returns 0, or -errno. Drops the VMA bookkeeping (trimming or splitting
 * partial overlaps) and frees the backing frames; unmapping never-faulted or
 * never-mapped pages is a no-op, as on Linux.
 */
static int32_t sys_munmap(isr_params_t *params)
{
    uint32_t addr = params->ebx;
    uint32_t length = params->ecx;
    vm_space_t *vm;
    uint32_t start, end;

    if (!current_proc) {
        return -ESRCH;
    }
    vm = &current_proc->vm;

    if (length == 0 || (addr & (PAGE_SIZE - 1)) != 0) {
        return -EINVAL;
    }
    /* Reject a range that leaves user space or wraps, so PAGE_UP(addr + length)
     * below cannot overflow to a smaller end and silently no-op. */
    if (addr >= USER_ADDR_MAX || length > USER_ADDR_MAX - addr) {
        return -EINVAL;
    }

    start = addr;
    end = PAGE_UP(addr + length);

    if (vm_remove(vm, start, end) != 0) {
        vma_exhausted("munmap");
        return -ENOMEM;
    }
    unmap_user_range(_curr_dir, start, end);
    return 0;
}

/* Name each missing syscall behind `rach2.trace=enosys`, once per number PER
 * PROCESS, not once per boot. Per boot was the first shape and it hides
 * exactly what such a trace is for: `login` asks for setitimer early, so a
 * later program asking for the same number would say nothing, and a reader
 * comparing before and after would conclude the later program asked for
 * nothing. Recording which process last reported a number costs one word per
 * number and makes each program's own gaps visible.
 *
 * A number the table cannot hold is printed every time, since there is nowhere
 * to remember it; nothing should ever ask for one, and a boot that fills the
 * console with one is telling the truth about a program in a retry loop. Off
 * unless the flag is on the command line, so no other boot sees a line it did
 * not before. */
static int _trace_enosys = 0;
static uint32_t _enosys_last_pid[SYSCALL_TABLE_SIZE];

/**
 * The syscall trap handler: dispatch on the number in EAX.
 *
 * Looks up _syscall_table[EAX] and calls it, storing the result back in
 * params->eax for popal/iret to return to userspace. Marks the process
 * non-preemptible (in_syscall) around the dispatch and runs the body with
 * interrupts on so the PIT/HDA keep ticking during long syscalls.
 */
static void _syscall_handler(isr_params_t *params)
{
    uint32_t syscall_num = params->eax;
    /* Default to -ENOSYS so an unimplemented or out-of-range number reports the
     * real "no such syscall" errno; musl relies on -ENOSYS to fall back to an
     * alternate path instead of treating it as a generic -EPERM failure. */
    int32_t ret = -ENOSYS;

    /* Mark this process non-preemptible BEFORE enabling interrupts. The ISR
     * stub keeps IF=0 through entry (segments/%gs are still being saved), so
     * setting in_syscall here and only then doing sti closes the window in
     * which a timer IRQ could reschedule a process whose trapframe is still
     * half-built. The timer handler skips scheduling while in_syscall is set.
     */
    if (current_proc) {
        current_proc->in_syscall = 1;
    }

    /* Enable interrupts for the syscall body so the PIT/HDA keep ticking during
     * long syscalls (e.g. Doom's framebuffer writes); preemption stays disabled
     * via in_syscall above. The stub no longer does sti; iret restores IF. */
    __asm__ __volatile__("sti");

    if (syscall_num < SYSCALL_TABLE_SIZE && _syscall_table[syscall_num]) {
        ret = _syscall_table[syscall_num](params);
    } else if (_trace_enosys) {
        /* A missing syscall is otherwise invisible: -ENOSYS goes back in a
         * register and nothing is printed, so a program that fails for want of
         * one fails for no stated reason. */
        uint32_t pid = current_proc ? current_proc->pid : 0;
        const char *who = current_proc ? current_proc->comm : "?";

        if (syscall_num >= SYSCALL_TABLE_SIZE) {
            klog(KLOG_WARN, "syscall",
                 "%d: not implemented, and past the table (asked for by %s)",
                 (int)syscall_num, who);
        } else if (_enosys_last_pid[syscall_num] != pid) {
            _enosys_last_pid[syscall_num] = pid;
            klog(KLOG_WARN, "syscall",
                 "%d: not implemented (asked for by %s)", (int)syscall_num, who);
        }
    }

    /* Disable interrupts again so the epilogue restores the trapframe (segments
     * and %gs) atomically; iret then restores the caller's IF. */
    __asm__ __volatile__("cli");

    if (current_proc) {
        current_proc->in_syscall = 0;
    }

    /* Store return value in EAX - will be restored by popal */
    params->eax = (uint32_t)ret;
}

void init_syscall(void)
{
    uint32_t i;

    init_kstacks();

    for (i = 0; i < SYSCALL_TABLE_SIZE; i++) {
        _syscall_table[i] = NULL;
    }

    _syscall_table[SYS_EXIT] = sys_exit;
    _syscall_table[SYS_FORK] = sys_fork;
    _syscall_table[SYS_READ] = sys_read;
    _syscall_table[SYS_WRITE] = sys_write;
    _syscall_table[SYS_OPEN] = sys_open;
    _syscall_table[SYS_CLOSE] = sys_close;
    _syscall_table[SYS_WAITPID] = sys_waitpid;
    _syscall_table[SYS_WAIT4] = sys_wait4;
    _syscall_table[SYS_CREAT] = sys_creat;
    _syscall_table[SYS_UNLINK] = sys_unlink;
    _syscall_table[SYS_MKDIR] = sys_mkdir;
    _syscall_table[SYS_RMDIR] = sys_rmdir;
    _syscall_table[SYS_EXECVE] = sys_execve;
    _syscall_table[SYS_CHDIR] = sys_chdir;
    _syscall_table[SYS_BRK] = sys_brk;
    _syscall_table[SYS_MMAP2] = sys_mmap2;
    _syscall_table[SYS_MUNMAP] = sys_munmap;
    _syscall_table[SYS_LSEEK] = sys_lseek;
    _syscall_table[SYS_RENAME] = sys_rename;
    _syscall_table[SYS_IOCTL] = sys_ioctl;
    _syscall_table[SYS_DUP] = sys_dup;
    _syscall_table[SYS_DUP2] = sys_dup2;
    _syscall_table[SYS_PIPE] = sys_pipe;
    _syscall_table[SYS_PIPE2] = sys_pipe2;
    _syscall_table[SYS_POLL] = sys_poll;
    _syscall_table[SYS_PPOLL] = sys_ppoll;
    _syscall_table[SYS_SELECT] = sys_select;
    _syscall_table[SYS_NEWSELECT] = sys__newselect;
    _syscall_table[SYS_PSELECT6] = sys_pselect6;
    _syscall_table[SYS_GETTIMEOFDAY] = sys_gettimeofday;
    _syscall_table[SYS_STAT] = sys_stat;
    _syscall_table[SYS_LSTAT] = sys_lstat;
    _syscall_table[SYS_GETCWD] = sys_getcwd;
    _syscall_table[SYS_STAT64] = sys_stat64;
    _syscall_table[SYS_LSTAT64] = sys_lstat64;
    _syscall_table[SYS_FSTAT64] = sys_fstat64;
    _syscall_table[SYS_STATFS64] = sys_statfs64;
    _syscall_table[SYS_FSTATFS64] = sys_fstatfs64;
    _syscall_table[SYS_FSTATAT64] = sys_fstatat64;
    _syscall_table[SYS_OPENAT] = sys_openat;
    _syscall_table[SYS_READLINK] = sys_readlink;
    _syscall_table[SYS_READLINKAT] = sys_readlinkat;
    _syscall_table[SYS_ACCESS] = sys_access;
    _syscall_table[SYS_FACCESSAT] = sys_faccessat;
    _syscall_table[SYS_LLSEEK] = sys__llseek;
    _syscall_table[SYS_FCNTL64] = sys_fcntl64;
    _syscall_table[SYS_GETDENTS64] = sys_getdents64;
    _syscall_table[SYS_UTIMENSAT] = sys_utimensat;
    _syscall_table[SYS_CLOCK_GETTIME] = sys_clock_gettime;
    _syscall_table[SYS_NANOSLEEP] = sys_nanosleep;
    _syscall_table[SYS_GETRANDOM] = sys_getrandom;
    _syscall_table[SYS_KILL] = sys_kill;
    _syscall_table[SYS_TKILL] = sys_tkill;
    _syscall_table[SYS_TGKILL] = sys_tgkill;
    _syscall_table[SYS_RT_SIGACTION] = sys_rt_sigaction;
    _syscall_table[SYS_RT_SIGPROCMASK] = sys_rt_sigprocmask;
    _syscall_table[SYS_RT_SIGRETURN] = sys_rt_sigreturn;
    _syscall_table[SYS_SIGRETURN] = sys_sigreturn;
    /* Identity: the four getters read the PCB credentials. */
    _syscall_table[SYS_GETUID32] = sys_getuid;
    _syscall_table[SYS_GETGID32] = sys_getgid;
    _syscall_table[SYS_GETEUID32] = sys_geteuid;
    _syscall_table[SYS_GETEGID32] = sys_getegid;
    _syscall_table[SYS_GETRESUID32] = sys_getresuid;
    _syscall_table[SYS_GETRESGID32] = sys_getresgid;
    _syscall_table[SYS_GETGROUPS32] = sys_getgroups;
    _syscall_table[SYS_SOCKETCALL] = sys_socketcall;
    _syscall_table[SYS_SET_THREAD_AREA] = sys_set_thread_area;
    _syscall_table[SYS_GETPID] = sys_getpid;
    _syscall_table[SYS_SETSID] = sys_setsid;
    _syscall_table[SYS_SETPGID] = sys_setpgid;
    _syscall_table[SYS_GETPGID] = sys_getpgid;
    _syscall_table[SYS_GETPGRP] = sys_getpgrp;
    _syscall_table[SYS_GETSID] = sys_getsid;
    _syscall_table[SYS_PAUSE] = sys_pause;
    _syscall_table[SYS_RT_SIGSUSPEND] = sys_rt_sigsuspend;
    _syscall_table[SYS_GETPPID] = sys_getppid;
    _syscall_table[SYS_UMASK] = sys_umask;
    _syscall_table[SYS_TIMES] = sys_times;
    _syscall_table[SYS_SYSINFO] = sys_sysinfo;
    _syscall_table[SYS_VFORK] = sys_fork; /* conservative alias */
    _syscall_table[SYS_READV] = sys_readv;
    _syscall_table[SYS_WRITEV] = sys_writev;
    _syscall_table[SYS_UNAME] = sys_uname;
    _syscall_table[SYS_SETHOSTNAME] = sys_sethostname;
    _syscall_table[SYS_SETUID32] = sys_setuid;
    _syscall_table[SYS_SETGID32] = sys_setgid;
    _syscall_table[SYS_SETGROUPS32] = sys_setgroups;
    _syscall_table[SYS_GETRLIMIT] = sys_getrlimit;
    _syscall_table[SYS_UGETRLIMIT] = sys_getrlimit;
    _syscall_table[SYS_PRLIMIT64] = sys_prlimit64;
    _syscall_table[SYS_DUP3] = sys_dup3;
    _syscall_table[SYS_SYNC] = sys_sync;
    _syscall_table[SYS_REBOOT] = sys_reboot;
    _syscall_table[SYS_FSYNC] = sys_fsync;
    _syscall_table[SYS_FTRUNCATE] = sys_ftruncate;
    _syscall_table[SYS_MOUNT] = sys_mount;
    _syscall_table[SYS_UMOUNT2] = sys_umount2;
    _syscall_table[SYS_FDATASYNC] = sys_fsync;
    _syscall_table[SYS_LINK] = sys_link;
    _syscall_table[SYS_LINKAT] = sys_linkat;
    /* Only the *32 id forms are wired: musl remaps the whole legacy 16-bit
     * family to them on i386, and the 16-bit ABI's narrower ids and 0xFFFF
     * keep-sentinel differ, so serving those numbers with 32-bit semantics
     * would lie to a raw caller. They fall to ENOSYS instead. */
    _syscall_table[SYS_SETREUID32] = sys_setreuid;
    _syscall_table[SYS_SETREGID32] = sys_setregid;
    _syscall_table[SYS_SETRESUID32] = sys_setresuid;
    _syscall_table[SYS_SETRESGID32] = sys_setresgid;
    _syscall_table[SYS_CHMOD] = sys_chmod;
    _syscall_table[SYS_FCHMODAT] = sys_fchmodat;
    _syscall_table[SYS_FCHMOD] = sys_fchmod;
    _syscall_table[SYS_CHOWN] = sys_chown;
    _syscall_table[SYS_CHOWN32] = sys_chown;
    _syscall_table[SYS_LCHOWN] = sys_chown;
    _syscall_table[SYS_LCHOWN32] = sys_chown;
    _syscall_table[SYS_FCHOWNAT] = sys_fchownat;
    _syscall_table[SYS_FCHOWN] = sys_fchown;
    _syscall_table[SYS_FCHOWN32] = sys_fchown;
    _syscall_table[SYS_SET_TID_ADDRESS] = sys_set_tid_address;
    /* musl's exit() issues exit_group(); for a single-threaded process it is
     * exactly _exit(). */
    _syscall_table[SYS_EXIT_GROUP] = sys_exit;

    _trace_enosys = kcmdline_has("rach2.trace=enosys");

    register_isr_handler(SYSCALL_VECTOR, _syscall_handler);
}
