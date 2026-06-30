#include "syscall.h"
#include "serial.h"
#include "panic.h"
#include "kio.h"
#include "kbd.h"
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
#include "fs/krffs.h"
#include "fs/devfs.h"
#include "fs/synthfs.h"
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
#include "network/udp/udp.h"
#include "network/tcp/tcp.h"
#include "network/ip/icmp.h"
#include "network/net.h" /* NET_IP_ADDR for getsockname */

/* User stack base address (stack grows down from here) */
#define USER_STACK_BASE 0xBFFFF000

/* Validate a user-space address range (defined with the socketcall helpers). */
static int user_access_ok(uint32_t addr, uint32_t len);

/* Validate AND fault in a user buffer before the kernel hands the raw pointer to
 * a lower layer that dereferences it directly (krffs, tcp, udp, drivers). 'write'
 * is 1 if that layer writes into the buffer. Defined with user_access_ok. */
static int user_buffer_ok(uint32_t addr, uint32_t len, int write);

/* Only one process owns the framebuffer at a time; when it exits or closes fb,
 * text mode is restored. */
static volatile uint32_t fb_owner_pid = 0;

/**
 * Restore text mode if `pid` owns the framebuffer.
 *
 * Called when a process is killed (Ctrl-C) so the dead owner does not strand the
 * screen in graphics mode.
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
 * Release every backing reference a descriptor holds before its slot is freed.
 *
 * Drops framebuffer ownership, the per-fd socket reference, and an ops-backed
 * object (pipe/tty/pty). Every fd-teardown site routes through this (close, the
 * dup2 replacement of an open target, the execve CLOEXEC sweep, and the
 * process-exit fd-table release) so none can forget a backing kind as new ones
 * are added. It does NOT free the slot; the caller does that with fd_free.
 *
 * An ops-backed object rides the shared open-file description: dup/dup2/fork copy
 * the ops/priv pointers and bump the OFD refcount, so the object must be released
 * exactly once, when its LAST descriptor closes (the final OFD reference, refcount
 * about to reach 0). The socket reference, by contrast, is counted per descriptor
 * (socket_ref on dup/fork), so it is always dropped here. No descriptor is both an
 * ops object and a socket.
 */
static void fd_release_backing(fd_entry_t *e)
{
    if (!e) {
        return;
    }
    /* A synthetic directory fd's pooled cookie is freed when its last descriptor
     * closes (dup/fork share it via the OFD refcount, like an ops backing). */
    if (e->type == FD_TYPE_SYNTH_DIR && e->priv && e->ofd && e->ofd->refcount == 1) {
        ((struct synth_dir *)e->priv)->used = 0;
    }
    /* Restore text mode only if THIS process actually owns the framebuffer, so a
     * fork child that merely inherited the fb descriptor cannot clear the
     * parent's graphics mode when the child closes the fd or exits. */
    if (e->type == FD_TYPE_FRAMEBUFFER && current_proc &&
        fb_owner_pid == current_proc->pid) {
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
 * Per-process kernel stacks come from a fixed arena of MAX_PROCS slots, not from
 * the never-freed bump allocator, so a fork/exit/reap cycle returns the stack to
 * the pool instead of leaking four frames each time. The arena is reserved once
 * at init; a slot is claimed on fork and released when the zombie is reaped.
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
 * A top outside the arena (the boot stack of the init-role process) is ignored,
 * so this is safe to call on any reaped process. Only a zombie may be freed: the
 * dying process is still executing on its own stack until the parent reaps it.
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
 * is PID 1, in which case its death is fatal (panic_init_died, never returns). */
extern bool debug_shell_is_home;
extern void panic_init_died(uint32_t pid);

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
 * Release every descriptor a dying process still holds.
 *
 * Drops each socket reference and each shared open-file description (the seek
 * offset), and clears the slot. Called from both exit paths (sys_exit here and
 * terminate_current_process via an extern), so sockets, port bindings, TCBs, and
 * OFDs are freed however a process dies. Idempotent: a second call sees an empty
 * table (fd_free skips free slots).
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
 * EBX holds the exit status. Marks the process a zombie, reparents its children
 * to init, wakes the parent if it waits, and reschedules. A real init's exit is
 * fatal (panic_init_died); an init exiting under the debug shell returns to it.
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
    }

    k_printf("\nProcess exited with status %d\n", status);
    serial_printf("\nProcess exited with status %d\n", status);

    if (!current_proc) {
        __asm__ __volatile__("cli; hlt");
        return 0;
    }

    exiting_pid = current_proc->pid;
    is_init = (exiting_pid == init_pid && init_pid != 0);

    /* If the dying process is the console-owning session leader (init, or a later
     * leader that claimed the console via TIOCSCTTY, NOT only init), release the
     * controlling terminal NOW, before it zombifies and the teardown below runs
     * with interrupts on. Otherwise the timer-IRQ serial drain (tty_console_poll)
     * keeps draining COM1 against this vanishing session, dropping a ^C/^Z and
     * stealing bytes the kernel debug shell should read. shell_reset_input on the
     * is_init path releases again (idempotent). */
    if (tty_console_owned_by_leader((int)current_proc->sid, (int)current_proc->pid)) {
        tty_set_session(&console_tty, 0, 0);
    }
    /* A dying session leader also disassociates any pty slave it controlled, so a
     * later session can claim the same /dev/pts/N. */
    pty_release_leader((int)current_proc->sid, (int)current_proc->pid);

#ifdef RACH2_HARNESS
    /* Harness build: the instant the probe (run as init) exits, report its status
     * to QEMU's isa-debug-exit device and stop, BEFORE schedule() can switch to
     * a still-READY child and bypass this path, so the runner always reads a real
     * exit code. */
    if (is_init) {
        harness_exit((uint8_t)status);
    }
#endif

    /* Mark process a zombie and store the Linux wait-status encoding of a normal
     * exit (the parent's waitpid/wait4 copies exit_code straight out): the low 8
     * bits of the code go in bits 8-15, so WIFEXITED is true and WEXITSTATUS
     * returns the code. (harness_exit above still gets the raw status byte.) */
    current_proc->state = PROC_ZOMBIE;
    current_proc->exit_code = (status & 0xFF) << 8;

    /* Notify the parent: post SIGCHLD (ignored by default, but a shell installs a
     * reaper handler) and wake it if it is blocked in waitpid. */
    parent = get_process(current_proc->parent_pid);
    if (parent) {
        signal_post(parent, SIGCHLD);
        if (parent->state == PROC_BLOCKED) {
            parent->state = PROC_READY;
        }
    }

    /* Hand any surviving children to init before this slot is reaped, so an exit
     * never strands an orphan on a dead parent (the MAX_PROCS zombie leak). */
    reparent_children(exiting_pid);

    /* A real /sbin/init must never exit; if it does there is no reaper, so the
     * kernel panics. Halt BEFORE schedule(), or a still-READY process would take
     * the CPU and the system would run on with a dead PID 1. Only when the debug
     * shell is the supervisor (rescue, or the no-init test image) does an exited
     * init instead return to it, below. */
    if (is_init && !debug_shell_is_home) {
        panic_init_died(exiting_pid);   /* never returns */
    }

    schedule();

    /* No other process is READY. The init that exited ran under the debug shell
     * (a real init would have panicked above), so return to it. */
    if (is_init) {
        serial_printf("sys_exit: Init process %d exited, returning to kernel shell\n",
                      exiting_pid);
        k_puts("");
        k_puts("Init process exited. Returning to kernel debug shell.");

        /* Clean up the init process resources. Switch off its address space
         * BEFORE freeing the page directory: free_page_directory refuses to free
         * whatever CR3 currently points at, so freeing while still loaded would
         * leak the directory and its private page tables. */
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
         * interrupts below, so a keyboard IRQ during the jump cannot append to a
         * stale command buffer or see a dangling stdin waiter. shell_loop resets
         * again on entry as the catch-all. */
        shell_reset_input();

        /* Reset ESP to a fresh kernel stack and jump to shell_loop. Safe because
         * we are in kernel mode and init has been torn down. */
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

    /* Backing-ops object (pipe/tty/pty/device): dispatch through its vtable, but
     * only if the descriptor was opened for writing (a read-only fd -> -EBADF, as
     * Linux reports). The plain file and enum types below have no ops and fall
     * through to the enum dispatch. */
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
            /* tcp_send copies the user buffer into socket send buffers itself;
             * __force strips the __user tag at this boundary. */
            return tcp_send(entry->sock, (const uint8_t __force *)buf, (int)count);
        }
        /* Datagram sockets send via sendto(). */
        return -EOPNOTSUPP;
    }

    if (entry->type == FD_TYPE_CONSOLE) {
        char kbuf[256];
        uint32_t done = 0;

        if (!(entry->flags & FD_FLAG_WRITE)) {
            return -EBADF;  /* Not writable (e.g., stdin) */
        }

        /* A background process writing its controlling terminal is stopped with
         * SIGTTOU only when TOSTOP is set (otherwise background output is allowed).
         * As with SIGTTIN, a blocked/ignored SIGTTOU makes it -EIO instead. */
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

        /* Pull the user bytes in through a small kernel bounce buffer, then emit
         * from kernel memory through the tty output processor (OPOST/ONLCR, so a
         * bare '\n' becomes CR LF on both the VGA screen and the serial line),
         * looping until count bytes are done. A bad user pointer yields -EFAULT
         * only if nothing has been written yet (otherwise report the bytes
         * already emitted, matching the return-count convention). */
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
        int written;

        if (!(entry->flags & FD_FLAG_WRITE)) {
            return -EBADF;  /* Not opened for writing */
        }

        if (!entry->node) {
            return -EBADF;
        }
        if (fd_node_check(entry) != 0) {
            return -EBADF;  /* the file was unlinked or relocated out from under us */
        }

        /* krffs_write reads the user buffer directly, so range-check AND fault
         * its pages in first (as the socket path does) before stripping __user. */
        if (!user_buffer_ok((uint32_t)(__force uintptr_t)buf, count, 0)) {
            return -EFAULT;
        }
        /* O_APPEND: every write lands at the current end of file, regardless of
         * any intervening seek (the POSIX append contract). The flag lives on the
         * shared open-file description. */
        if (entry->ofd && (entry->ofd->status_flags & OFD_APPEND)) {
            entry->ofd->offset = (uint32_t)entry->node->data_size;
        }

        /* Write via the node the descriptor already resolved (a subdir file's
         * node->name is only its basename, so re-resolving by name would fail).
         * krffs_write_node updates entry->node if a grow relocates the file. */
        written = krffs_write_node(&entry->node, (const char __force *)buf, count,
                                   entry->ofd->offset);
        if (written < 0) {
            return -EIO;  /* krffs reports failure with a bare -1 */
        }
        entry->ofd->offset += (uint32_t)written;
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

        /* On any fault once bytes were already emitted, report the partial total
         * (a caller that retried would otherwise duplicate output); only report
         * the error itself when nothing has been written yet. */
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
 * Copy a user path into pathbuf as an ABSOLUTE path.
 *
 * An absolute user path is taken verbatim; a relative one is joined onto the
 * process's current working directory so the root-relative resolver (which
 * already honors '.'/'..') walks the right tree. Same return contract as
 * strncpy_from_user: the resulting length on success, or a negative errno (a join
 * that overruns the buffer is -ENAMETOOLONG). The "fb"/"mouse" device names
 * resolve correctly only from cwd "/", which holds because programs open them at
 * startup.
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
        return len;
    }
    return join_abs(pathbuf, size, current_proc->cwd, tmp, (uint32_t)len);
}

/**
 * Re-validate a descriptor's cached KRFFS node before a file operation.
 *
 * KRFFS nodes are not refcounted, so an `unlink`, an `O_TRUNC`, or a write that
 * grows and relocates a file through another descriptor (or another process, on
 * the one shared image) can free or move the node a still-open fd points at. A
 * later read/write/seek/stat through the stale `entry->node` would touch freed
 * free-list metadata. Defend like the directory-fd path (`build_dirfd_path`):
 * the node must still be a live reserved node whose reconstructed path resolves
 * back to it. A NULL node is the root directory and needs no check. Returns 0
 * when the node is live, or -EBADF when the fd has gone stale.
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
 * Absolute paths are taken verbatim; a relative path resolves against AT_FDCWD
 * (the cwd) or, for a real dirfd, against that descriptor, which must be an open
 * directory (else -EBADF/-ENOTDIR). Same return contract as copy_path_cwd.
 */
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
        return len;
    }
    if (dirfd == AT_FDCWD) {
        return join_abs(pathbuf, size, current_proc->cwd, tmp, (uint32_t)len);
    }

    /* A real dirfd must be an open directory; resolve relative to its path. */
    entry = fd_get(&current_proc->fds, dirfd);
    if (!entry) {
        return -EBADF;
    }
    /* A synthetic directory fd (/dev, /proc, /proc/<pid>[/fd]): reconstruct its
     * absolute path from the pooled kind/pid and resolve the relative name there. */
    if (entry->type == FD_TYPE_SYNTH_DIR) {
        struct synth_dir *sd = (struct synth_dir *)entry->priv;
        if (!sd) {
            return -EBADF;
        }
        synth_dir_path(sd->kind, sd->pid, dirpath, (int)sizeof(dirpath));
        return join_abs(pathbuf, size, dirpath, tmp, (uint32_t)len);
    }
    if (entry->type != FD_TYPE_DIR) {
        return -ENOTDIR;
    }
    dnode = entry->node; /* NULL marks the root directory */
    if (dnode) {
        /* KRFFS nodes are not refcounted, so a directory fd can outlive its node
         * if the directory is rmdir'd. Reject a stale fd defensively: the node
         * must still be a live reserved node, and its reconstructed path must
         * round-trip back to the same node. */
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
    return join_abs(pathbuf, size, dirpath, tmp, (uint32_t)len);
}

static int32_t synth_open_dir(const struct vpath *vp, int flags);

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
        case VNODE_DEV:
            return devfs_open(vp->name, flags);
        case VNODE_PTS:
            /* /dev/pts/N: the slave end of pty N (pty_open_slave does its own fd
             * allocation, and rejects an unallocated/locked pair). */
            if (flags & O_DIRECTORY) {
                return -ENOTDIR;
            }
            return pty_open_slave(vp->pts_index, flags);
        case VNODE_PROC_FILE:
            return procfs_open(vp, flags);
        case VNODE_DEV_DIR:
        case VNODE_PTS_DIR:
        case VNODE_PROC_DIR:
        case VNODE_PROC_PID:
        case VNODE_PROC_FD_DIR:
            return synth_open_dir(vp, flags);
        case VNODE_ENOTDIR:
            return -ENOTDIR;
        default:
            return -ENOENT; /* including VNODE_PROC_FD_LINK: read via readlink */
    }
}

/**
 * True if `name` (absolute, leading '/' stripped) names a synthetic mount.
 *
 * A mutation (mkdir/rmdir/unlink/rename/...) targeting /dev or /proc must be
 * refused here, not passed to KRFFS: otherwise mkdir("/dev") would create a real
 * KRFFS directory that shadows the synthetic one.
 */
static int synth_is_synth(const char *name)
{
    struct vpath vp;
    return synth_classify(name, &vp) != VNODE_KRFFS;
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
     * create of an existing object is -EEXIST regardless of access mode (the usual
     * O_CREAT|O_EXCL form carries write access), so that check comes first; then a
     * directory cannot be opened for writing. */
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
    int fd;
    struct vpath vp;

    /* /dev and /proc are synthetic; classified in one place, never KRFFS. */
    if (synth_classify(name, &vp) != VNODE_KRFFS) {
        return synth_open(&vp, flags);
    }

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
        if (!mouse_is_available()) {
            return -ENODEV;  /* Mouse not initialized */
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

    /* "/", ".", "..": the root directory (cwd is always "/" in KRFFS). */
    if (name[0] == '\0' ||
        (name[0] == '.' && name[1] == '\0') ||
        (name[0] == '.' && name[1] == '.' && name[2] == '\0')) {
        fd = fd_alloc(&current_proc->fds);
        if (fd < 0) {
            return -EMFILE;
        }

        entry = &current_proc->fds.entries[fd];
        entry->type = FD_TYPE_DIR;
        entry->node = NULL;  /* NULL indicates root directory */
        entry->ofd->offset = 0;
        entry->flags = FD_FLAG_READ;
        if (flags & O_CLOEXEC) {
            entry->flags |= FD_FLAG_CLOEXEC; /* opendir("/") passes O_CLOEXEC */
        }

        return fd;
    }

    /* Find the file in KRFFS. resolve_e distinguishes a missing component
     * (-ENOENT) from a non-directory in the path (-ENOTDIR) so open() reports the
     * Linux errno; only a genuine ENOENT continues to the O_CREAT path. */
    {
        int rerr = 0;
        node = krffs_resolve_e(name, &rerr);
        if (!node) {
            if (rerr == -ENOTDIR) {
                return -ENOTDIR;
            }
            if (flags & O_CREAT) {
                if (krffs_create(name) != 0) {
                    return -ENOSPC;  /* Failed to create file */
                }
                node = krffs_resolve(name);
                if (!node) {
                    return -EIO;  /* Shouldn't happen, but check anyway */
                }
                /* Stamp the caller's requested permission bits on the new file. */
                node->mode = (uint16_t)(mode & 0777);
            } else {
                return -ENOENT;  /* File not found and O_CREAT not set */
            }
        } else if (flags & O_TRUNC) {
            krffs_truncate(name, 0);
        }
    }

    fd = fd_alloc(&current_proc->fds);
    if (fd < 0) {
        return -EMFILE;  /* No free fds */
    }

    /* Set up the fd entry. A directory opens as a FD_TYPE_DIR (its node is the
     * directory whose children getdents lists); a regular file as FD_TYPE_FILE. A
     * path that resolved to the root (e.g. "x/..") is the root directory, stored
     * with a NULL node like the literal-root open above. */
    entry = &current_proc->fds.entries[fd];
    if (node == krffs_fs->node) {
        entry->type = FD_TYPE_DIR;
        entry->node = NULL;
    } else {
        entry->type = KRFFS_IS_DIR(node->mode) ? FD_TYPE_DIR : FD_TYPE_FILE;
        entry->node = node;
    }
    entry->ofd->offset = 0;

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
 * openat (syscall 295): EBX=dirfd, ECX=path, EDX=flags, ESI=mode. musl issues
 * openat (not the legacy open) for every <stdio.h> file open. An absolute path is
 * taken as-is, a relative path resolves against the cwd (AT_FDCWD) or against the
 * directory the dirfd refers to (see copy_path_at).
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

    /* Release the descriptor's backing (framebuffer/socket/ops object), then free
     * the slot. fd_free reports a bad/unallocated descriptor with -1; translate
     * to the close(2) errno. */
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
 * EBX = fd. Validates the descriptor (any open fd; the type is not checked), then
 * writes back every dirty page of the disk-backed window via `pc_sync`. The flush
 * is whole-cache rather than per-file, a safe superset of the contract. When KRFFS
 * is the RAM module the cache is empty, so the flush is a no-op and the call only
 * validates the fd. Returns 0, -EBADF for a bad descriptor, or -EIO when a disk
 * writeback fails. dropbear writes its host key with the open/write/fsync/rename
 * idiom and treats an fsync error as fatal, so the -EIO is real backpressure.
 */
static int32_t sys_fsync(isr_params_t *params)
{
    int fd = (int)params->ebx;

    if (!current_proc) {
        return -ESRCH;
    }
    if (!fd_get(&current_proc->fds, fd)) {
        return -EBADF;
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

/* lseek(2): EBX = fd, ECX = offset, EDX = whence (0=SET, 1=CUR, 2=END). Returns
 * the new offset or -errno. */
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
        /* mem devices (null/zero/full) accept a seek that always lands at 0; the
         * stream devices (random/urandom) are not seekable. */
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

    if (!entry->node) {
        return -EBADF;
    }
    if (fd_node_check(entry) != 0) {
        return -EBADF;  /* the file was unlinked or relocated out from under us */
    }
    file_size = (uint32_t)entry->node->data_size;

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
            return -EINVAL;
    }

    if (new_offset < 0) {
        return -EINVAL;
    }

    entry->ofd->offset = (uint32_t)new_offset;
    return new_offset;
}

/**
 * Read up to `count` bytes from descriptor `fd` (read(2)).
 *
 * EBX = fd, ECX = buf, EDX = count. Returns the bytes read, 0 at end, or -errno.
 * Dispatches by backing kind. A console read (stdin) blocks in the tty line
 * discipline until a cooked line or VMIN raw bytes arrive; in graphics mode it is
 * a non-blocking raw scancode read instead.
 */
static int32_t sys_read(isr_params_t *params)
{
    int fd = (int)params->ebx;
    char __user *buf = (char __user *)params->ecx;
    uint32_t count = params->edx;
    fd_entry_t *entry;

    if (!current_proc) {
        return -EFAULT;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry) {
        return -EBADF;
    }

    /* A zero-length read is a no-op that returns 0 (even with a NULL buffer, per
     * POSIX) once the fd is known valid; do not touch buf. */
    if (count == 0) {
        return 0;
    }
    if (!buf) {
        return -EFAULT;
    }

    /* Backing-ops object (pipe/tty/pty/device): dispatch through its vtable, but
     * only if the descriptor was opened for reading (a write-only fd -> -EBADF).
     * The plain file and enum types below have no ops and fall through to the
     * enum dispatch. */
    if (entry->ops) {
        if (!(entry->flags & FD_FLAG_READ)) {
            return -EBADF;
        }
        return entry->ops->read ? entry->ops->read(entry, buf, count) : -EBADF;
    }

    /* read() honors O_NONBLOCK (an OFD status flag) exactly like recv(), routing
     * to the non-blocking variant. */
    if (entry->type == FD_TYPE_SOCKET) {
        int sock_nonblock;
        if (!entry->sock || !user_buffer_ok((uint32_t)buf, count, 1)) {
            return -EFAULT;
        }
        sock_nonblock = entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK);
        /* socket_*recv write straight into the validated user buffer; __force
         * strips the __user tag at the boundary. */
        if (entry->sock->type == SOCK_DGRAM) {
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
         * Park on console_tty until a line (cooked) or VMIN bytes (raw) arrive.
         * Parking marks input_wq active, and once a session owns the console its
         * sid is nonzero; either condition tells the keyboard IRQ to cook
         * keystrokes into console_tty rather than the kernel shell, so the byte
         * wakes us (or waits as type-ahead). stdin_waiting_proc, set across the
         * read below, only suppresses the scrollback hotkeys while we are parked;
         * tty_read drains the serial port itself. The discipline echoes, edits,
         * and recognizes ISIG, so Ctrl-C on the console now arrives as a posted
         * SIGINT, and a pending signal unwinds the wait with -EINTR.
         */
        /* A SESSION LEADER reading the console with no controlling terminal claims
         * it as its SOLE ctty (its group becomes the foreground, and any pty it held
         * is dropped), so a lone program receives ^C; a non-leader does not (it would
         * strand the tty on its since-dead leader). Once a session owns the console,
         * the foreground is set by TIOCSPGRP. */
        if (console_tty.sid == 0 && (int)current_proc->sid == (int)current_proc->pid) {
            tty_make_sole_ctty(&console_tty, (int)current_proc->sid,
                               (int)current_proc->pgid);
        }
        /* A BACKGROUND process reading its controlling terminal is stopped with
         * SIGTTIN (POSIX), unless that would wedge it (SIGTTIN blocked or
         * ignored), in which case the read fails with -EIO. Decided BEFORE marking
         * ourselves the console reader, so an early return leaves no stale flag. */
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
        n = tty_read(&console_tty, kbuf, (int)want);
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
         * into the user buffer; range-check it, then __force at the boundary. */
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
        int bytes_read;

        if (!(entry->flags & FD_FLAG_READ)) {
            return -EBADF;  /* Not opened for reading */
        }

        if (!entry->node) {
            return -EBADF;
        }
        if (fd_node_check(entry) != 0) {
            return -EBADF;  /* the file was unlinked or relocated out from under us */
        }

        /* krffs_read writes straight into the user buffer, so range-check AND
         * fault its pages in (write) before stripping the __user tag. */
        if (!user_buffer_ok((uint32_t)(__force uintptr_t)buf, count, 1)) {
            return -EFAULT;
        }
        /* Read via the resolved node (see the write path); the descriptor's
         * node->name is only the basename for a file in a subdirectory. */
        bytes_read = krffs_read_node(entry->node, (char __force *)buf, count,
                                     entry->ofd->offset);
        if (bytes_read < 0) {
            return -EIO;  /* krffs reports failure with a bare -1 */
        }
        entry->ofd->offset += (uint32_t)bytes_read;
        return bytes_read;
    }

    return -EBADF;
}

/**
 * Duplicate a descriptor onto the lowest free fd (dup(2)).
 *
 * EBX = oldfd. Returns the new fd, or -errno. The duplicate SHARES oldfd's
 * open-file description (shared seek offset); a socket gains a reference; the new
 * fd's FD_CLOEXEC is clear.
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
    /* entry still points at entries[oldfd] (a fixed array; alloc did not move it). */
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

    /* Duplicate the descriptor: it SHARES the open-file description (so the seek
     * offset is shared, per POSIX), gaining a reference; a duplicated socket gains
     * another reference too; and dup2 leaves the new fd's CLOEXEC clear. */
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
 * resolved directory (a KRFFS directory or a synthetic /dev or /proc directory);
 * a regular file is -ENOTDIR, a missing path -ENOENT.
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
        return 0;
    }

    /* A synthetic directory becomes the cwd (its canonical path, self resolved to
     * a real pid, so later relative opens re-classify correctly); a synthetic
     * leaf is ENOTDIR, a missing one ENOENT. */
    {
        struct vpath vp;
        int kind = synth_classify(name, &vp);
        if (kind != VNODE_KRFFS) {
            int rpid = 0;
            if (kind == VNODE_PROC_PID || kind == VNODE_PROC_FD_DIR) {
                rpid = procfs_resolve_pid(vp.pid);
                if (rpid < 0) {
                    return -ENOENT;
                }
            }
            if (kind == VNODE_DEV_DIR || kind == VNODE_PTS_DIR ||
                kind == VNODE_PROC_DIR || kind == VNODE_PROC_PID ||
                kind == VNODE_PROC_FD_DIR) {
                synth_dir_path(kind, rpid, current_proc->cwd, sizeof(current_proc->cwd));
                return 0;
            }
            return (kind == VNODE_ENOENT) ? -ENOENT : -ENOTDIR;
        }
    }

    if (!krffs_fs) {
        return -ENOENT;
    }
    {
        int rerr = 0;
        node = krffs_resolve_e(name, &rerr);
        if (!node) {
            return (rerr == -ENOTDIR) ? -ENOTDIR : -ENOENT;
        }
    }
    /* The root node is always a directory even if its on-disk mode lacks the
     * S_IFDIR bit (it is the filesystem anchor, not a mkdir'd node). A regular
     * file as the final component is ENOTDIR. */
    if (node != krffs_fs->node && !KRFFS_IS_DIR(node->mode)) {
        return -ENOTDIR;
    }

    /* Store the canonical absolute path (krffs_node_path follows real parent
     * links, so any '.'/'..' in the request is collapsed). If it does not fit the
     * fixed cwd buffer the cwd is left unchanged. */
    if (krffs_node_path(node, current_proc->cwd, sizeof(current_proc->cwd)) != 0) {
        return -ENAMETOOLONG;
    }
    return 0;
}

/* getcwd(2): EBX = buf, ECX = size. Returns the path length including the NUL, or
 * -errno. */
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

    /* Linux getcwd(2) returns the length including the NUL on success. (Returning
     * the buffer pointer would be misread as an errno for high user addresses
     * whose top bit is set.) */
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
 * Fill a stat64 buffer from a resolved KRFFS node.
 *
 * Shared by stat64/fstat64/fstatat64. Reports S_IFDIR for directories and S_IFREG
 * for regular files; the node id is the inode number.
 */
static void krffs_fill_stat64(struct linux_stat64 *kst, krffs_node_t *node)
{
    k_memset(kst, 0, sizeof(*kst));
    if (node == krffs_fs->node) {
        /* The root is a directory even if its on-disk mode lacks S_IFDIR (it is
         * the filesystem anchor); a path can reach it via "..". */
        kst->st_mode = S_IFDIR | 0755;
    } else {
        kst->st_mode = (KRFFS_IS_DIR(node->mode) ? S_IFDIR : S_IFREG) |
                       (node->mode & 0777);
    }
    kst->st_nlink = 1; /* KRFFS has no hard links */
    kst->st_uid = node->uid;
    kst->st_gid = node->gid;
    kst->st_size = (long long)node->data_size;
    kst->st_blksize = 4096;
    kst->st_blocks = (node->data_size + 511) / 512;
    kst->st_ino = node->id;
    kst->__st_ino = (unsigned long)node->id;
}

/* Fill a stat64 from an open descriptor (factored from sys_fstat64 so the magic
 * /proc/<pid>/fd/<n> symlinks can stat their target when followed). */
static void fstat_fill(fd_entry_t *e, struct linux_stat64 *kst);

/*
 * Fill a stat64 for a synthetic (/dev or /proc) path. Devices report a character
 * device with their major/minor; /dev itself is a directory; the /proc magic
 * symlinks (/proc/self, /proc/<pid>/fd/<n>) report S_IFLNK when NOT followed and
 * their target when followed (`follow`). st_dev is the synthetic fs id, distinct
 * from KRFFS's 0, so a device's (st_dev, st_ino) identity never aliases a file's.
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
        case VNODE_PROC_PID:
        case VNODE_PROC_FD_DIR:
            if (procfs_resolve_pid(vp->pid) < 0) {
                return -ENOENT;
            }
            /* /proc/self is a symlink: an un-followed stat (lstat) sees the link,
             * a followed one resolves to the pid directory. /proc/<numeric> and
             * /proc/<pid>/fd are real directories. */
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
            /* The link exists only if descriptor procfd is actually open (the same
             * check readlink makes), so stat and readlink agree. */
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
        case VNODE_PTS:
            /* /dev/pts/N: a slave terminal (S_IFCHR), only if pty N is allocated.
             * The rdev is the Linux Unix98 pty slave major 136. */
            if (!pty_is_allocated(vp->pts_index)) {
                return -ENOENT;
            }
            kst->st_dev = DEVFS_FSID;
            kst->st_mode = 0020620; /* S_IFCHR | 0620 (crw--w----) */
            kst->st_rdev = (136u << 8) | (uint32_t)vp->pts_index;
            kst->st_ino = PTS_INO_BASE + (uint32_t)vp->pts_index;
            return 0;
        case VNODE_ENOTDIR:
            return -ENOTDIR;
        default:
            return -ENOENT;
    }
}

/**
 * readlink core (readlink(2)/readlinkat(2)).
 *
 * The only symlinks Rach2 has are /proc/self and the /proc/<pid>/fd/<n> magic
 * links (which ttyname reads). The result is the byte count and is NOT
 * NUL-terminated (the readlink contract; musl appends its own terminator and
 * treats a full buffer as truncation). `name` is the path, leading '/' stripped.
 */
static int32_t do_readlink(const char *name, char __user *ubuf, uint32_t bufsiz)
{
    struct vpath vp;
    char link[256]; /* fit a full fd-target path (the syscall path limit) */
    int n, pid, kind;

    if (!ubuf || bufsiz == 0) {
        return -EINVAL;
    }
    kind = synth_classify(name, &vp);

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
        return (kind == VNODE_ENOENT) ? -ENOENT : -EINVAL; /* not a symlink */
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
    const char *name;
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
        if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
            return -EFAULT;
        }
        return 0;
    }

    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }

    /* /dev and /proc are synthetic. */
    {
        struct vpath vp;
        if (synth_classify(name, &vp) != VNODE_KRFFS) {
            int32_t rc = synth_stat(&vp, &kst, follow);
            if (rc != 0) {
                return rc;
            }
            if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
                return -EFAULT;
            }
            return 0;
        }
    }

    if (!krffs_fs) {
        return -ENOENT;
    }

    {
        int rerr = 0;
        node = krffs_resolve_e(name, &rerr);
        if (!node) {
            return (rerr == -ENOTDIR) ? -ENOTDIR : -ENOENT; /* missing vs not-a-dir */
        }
    }

    krffs_fill_stat64(&kst, node);

    if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
        return -EFAULT;
    }

    return 0;
}

/* stat64(195) follows symlinks; lstat64(196) does not. On i386 musl maps its
 * stat()/lstat() onto these (via the statx -> *stat64 fallback), so lstat() only
 * works once 196 exists. The follow flag matters only for the /proc magic links. */
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

/**
 * Fill a stat64 from an open descriptor by type.
 *
 * Shared by sys_fstat64 and the followed /proc/<pid>/fd/<n> path (whose target is
 * just this fd).
 */
static void fstat_fill(fd_entry_t *entry, struct linux_stat64 *kst)
{
    k_memset(kst, 0, sizeof(*kst));
    if (entry->type == FD_TYPE_DIR && entry->node == NULL) {
        kst->st_mode = S_IFDIR | 0755; /* the root directory */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
    } else if (entry->node) {
        /* A stale fd (its node was unlinked or relocated) would leak freed
         * free-list bytes through the stat fields; leave the zeroed stat rather
         * than read a node that is no longer live. */
        if (fd_node_check(entry) == 0) {
            krffs_fill_stat64(kst, entry->node);
        }
    } else if (entry->type == FD_TYPE_SOCKET) {
        /* A socket fd: programs classify these with S_ISSOCK. */
        kst->st_mode = 0140666; /* S_IFSOCK | 0666 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
    } else if (entry->type == FD_TYPE_PIPE) {
        /* A pipe end: programs classify these with S_ISFIFO. */
        kst->st_mode = 010666; /* S_IFIFO | 0666 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
    } else if (entry->type == FD_TYPE_DEVICE) {
        /* A devfs character device: report its identity so fstat matches the
         * stat of its /dev path (what ttyname compares). */
        kst->st_mode = 020666; /* S_IFCHR | 0666 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_dev = DEVFS_FSID;
        kst->st_rdev = devfs_rdev(entry);
        kst->st_ino = devfs_fd_ino(entry);
    } else if (entry->type == FD_TYPE_PROC) {
        /* A /proc text snapshot: a regular file. */
        kst->st_mode = 0100444; /* S_IFREG | 0444 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_dev = PROCFS_FSID;
    } else if (entry->type == FD_TYPE_CONSOLE) {
        /* stdin/stdout/stderr and /dev/console: report the same identity as
         * stat("/dev/console") so ttyname's (st_dev, st_ino) comparison matches. */
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
         * would (so fstat and stat agree on the directory), reconstructed from the
         * fd's pooled kind/pid. */
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
        /* A pty slave: report the SAME identity as stat("/dev/pts/N") so ttyname's
         * (st_dev, st_ino) comparison matches the /proc/self/fd readlink. */
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
}

/*
 * fstat64 (syscall 197): EBX = fd, ECX = stat64 buffer. The descriptor's node is
 * stat'd; a root directory fd (node == NULL) reports the root directory.
 */
static int32_t sys_fstat64(isr_params_t *params)
{
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
    fstat_fill(entry, &kst);
    if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/*
 * fstatat64 (syscall 300): EBX = dirfd, ECX = path, EDX = stat64 buffer,
 * ESI = flags. The path resolves the same way as openat (absolute, or relative to
 * AT_FDCWD/the dirfd; see copy_path_at). AT_SYMLINK_NOFOLLOW is honored for the
 * /proc magic symlinks; KRFFS has none.
 */
static int32_t sys_fstatat64(isr_params_t *params)
{
    int dirfd = (int)params->ebx;
    const char __user *upath = (const char __user *)params->ecx;
    struct linux_stat64 __user *st = (struct linux_stat64 __user *)params->edx;
    struct linux_stat64 kst;
    char pathbuf[256];
    const char *name;
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
    } else {
        struct vpath vp;
        name = pathbuf;
        if (name[0] == '/') {
            name++;
        }
        /* /dev and /proc are synthetic; classified before the KRFFS mount check,
         * so they stat even on a kernel with no RAM disk. AT_SYMLINK_NOFOLLOW
         * (lstat) reports the magic symlinks; otherwise they are followed. */
        if (synth_classify(name, &vp) != VNODE_KRFFS) {
            int follow = !((int)params->esi & AT_SYMLINK_NOFOLLOW);
            int32_t rc = synth_stat(&vp, &kst, follow);
            if (rc != 0) {
                return rc;
            }
            if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
                return -EFAULT;
            }
            return 0;
        }
        if (!krffs_fs) {
            return -ENOENT;
        }
        {
            int rerr = 0;
            node = krffs_resolve_e(name, &rerr);
            if (!node) {
                return (rerr == -ENOTDIR) ? -ENOTDIR : -ENOENT;
            }
        }
        krffs_fill_stat64(&kst, node);
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
 * There is no real uid/credential model, so R/W/X are checked against the
 * object's mode bits (which is what makes /proc/<pid>/stat reject W_OK while
 * granting F_OK/R_OK). `name` is absolute with the leading '/' stripped.
 */
static int32_t do_access(const char *name, int mode)
{
    struct vpath vp;
    uint32_t fmode;

    if (mode & ~(R_OK | W_OK | X_OK)) {
        return -EINVAL; /* undefined access-mode bits */
    }
    if (name[0] == '\0') {
        fmode = S_IFDIR | 0755; /* the root directory; falls through to the checks */
    } else if (synth_classify(name, &vp) != VNODE_KRFFS) {
        struct linux_stat64 s;
        int32_t rc = synth_stat(&vp, &s, 1); /* access follows symlinks */
        if (rc != 0) {
            return rc; /* -ENOENT / -ENOTDIR */
        }
        fmode = s.st_mode;
    } else {
        krffs_node_t *node;
        int rerr = 0;
        if (!krffs_fs) {
            return -ENOENT;
        }
        node = krffs_resolve_e(name, &rerr);
        if (!node) {
            return (rerr == -ENOTDIR) ? -ENOTDIR : -ENOENT;
        }
        fmode = (node == krffs_fs->node) ? (uint32_t)(S_IFDIR | 0755)
                                         : (uint32_t)(S_IFREG | (node->mode & 0777));
    }
    if ((mode & R_OK) && !(fmode & 0444u)) {
        return -EACCES;
    }
    if ((mode & W_OK) && !(fmode & 0222u)) {
        return -EACCES;
    }
    if ((mode & X_OK) && !(fmode & 0111u)) {
        return -EACCES;
    }
    return 0;
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

/* An owner/group argument is acceptable iff it names root (0) or is (id_t)-1
 * ("leave unchanged"); see do_chown and the setre/setres handlers. */
static int sc_id_is_root_or_keep(uint32_t id)
{
    return id == 0 || id == (uint32_t) -1;
}

/* Does an object exist at `name` (absolute, leading '/' stripped)? 0 if so, else
 * -ENOENT/-ENOTDIR, the same synthetic-vs-KRFFS resolution do_access uses. link()
 * resolves its source through this so a missing source is reported honestly before
 * the "no hard links" -EPERM. */
static int32_t path_exists(const char *name)
{
    struct vpath vp;
    int rerr = 0;

    if (name[0] == '\0') {
        return 0; /* the root directory */
    }
    if (synth_classify(name, &vp) != VNODE_KRFFS) {
        struct linux_stat64 s;
        return synth_stat(&vp, &s, 0);
    }
    if (!krffs_fs) {
        return -ENOENT;
    }
    if (!krffs_resolve_e(name, &rerr)) {
        return (rerr == -ENOTDIR) ? -ENOTDIR : -ENOENT;
    }
    return 0;
}

/**
 * chmod core for the chmod/chown family.
 *
 * Rach2 has a single (root, id 0) identity over a RAM file system, so these keep
 * just enough state to satisfy a login server and a shell. chmod updates a KRFFS
 * file's stored permission bits; a synthetic /dev or /proc object has a fixed
 * mode, so chmod there is a no-op that only validates the path exists. chown only
 * ever (re)assigns root (no other user or group exists), so uid/gid 0 (or -1)
 * succeeds and anything else is -EPERM. dropbear chmod/chowns the session pty (a
 * /dev/pts node) to root before running the shell; busybox chmod/chown drive the
 * KRFFS path. `name` is absolute with the leading '/' stripped, matching
 * do_access.
 */
static int32_t do_chmod(const char *name, uint32_t mode)
{
    struct vpath vp;
    krffs_node_t *node;
    int rerr = 0;

    if (name[0] == '\0') {
        return 0; /* the root directory: fixed perms, no-op success */
    }
    if (synth_classify(name, &vp) != VNODE_KRFFS) {
        struct linux_stat64 s;
        int32_t rc = synth_stat(&vp, &s, 1);
        return (rc != 0) ? rc : 0; /* synthetic node: validate, then no-op */
    }
    if (!krffs_fs) {
        return -ENOENT;
    }
    node = krffs_resolve_e(name, &rerr);
    if (!node) {
        return (rerr == -ENOTDIR) ? -ENOTDIR : -ENOENT;
    }
    if (node != krffs_fs->node) {
        /* keep the type bits (e.g. the directory bit), replace the perm bits */
        node->mode = (uint16_t) ((node->mode & ~0777u) | (mode & 0777u));
    }
    return 0;
}

static int32_t do_chown(const char *name, uint32_t uid, uint32_t gid)
{
    struct vpath vp;
    int rerr = 0;

    if (!sc_id_is_root_or_keep(uid) || !sc_id_is_root_or_keep(gid)) {
        return -EPERM; /* no such user/group: only root exists */
    }
    if (name[0] == '\0') {
        return 0; /* the root directory is already root-owned */
    }
    if (synth_classify(name, &vp) != VNODE_KRFFS) {
        struct linux_stat64 s;
        return synth_stat(&vp, &s, 1); /* validate existence; ownership is fixed */
    }
    if (!krffs_fs) {
        return -ENOENT;
    }
    if (!krffs_resolve_e(name, &rerr)) {
        return (rerr == -ENOTDIR) ? -ENOTDIR : -ENOENT;
    }
    return 0; /* already uid/gid 0; nothing to store */
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
 * perms are updated; any other descriptor (device/pipe/socket/tty) is a no-op. */
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
    if (entry->type == FD_TYPE_FILE && entry->node &&
        entry->node != krffs_fs->node) {
        if (fd_node_check(entry) != 0) {
            return -EBADF;  /* never write mode through a stale (freed) node */
        }
        entry->node->mode = (uint16_t) ((entry->node->mode & ~0777u) | (mode & 0777u));
    }
    return 0;
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

    if (!current_proc) {
        return -ESRCH;
    }
    if (!fd_get(&current_proc->fds, fd)) {
        return -EBADF;
    }
    if (!sc_id_is_root_or_keep(uid) || !sc_id_is_root_or_keep(gid)) {
        return -EPERM;
    }
    return 0;
}

/**
 * link(2): always -EPERM (KRFFS has no hard links).
 *
 * KRFFS stores exactly one name per node (no link count, no shared data), so the
 * call returns -EPERM, the POSIX errno for "the filesystem does not support the
 * creation of hard links". The source is copied and resolved first, so a bad
 * pointer (-EFAULT), bad dirfd (-EBADF), or missing source (-ENOENT) is reported
 * honestly rather than masked by EPERM. dropbear depends on the EPERM: it moves a
 * freshly generated host key with link(tmp, final) and, on EPERM/EACCES, falls
 * back to writing the key directly to the final path.
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
    /* Validate BOTH paths (copy_path_cwd faults a bad pointer to -EFAULT) and that
     * the source exists, before reporting that the filesystem cannot hard-link --
     * link(valid, bad_ptr) is -EFAULT, link(missing, x) is -ENOENT, not -EPERM. */
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
    rc = path_exists(name);
    if (rc != 0) {
        return rc;
    }
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
    /* Resolve the source against olddirfd and the destination against newdirfd, so
     * a bad dirfd (-EBADF) or pointer (-EFAULT) and a missing source (-ENOENT) are
     * reported before the "no hard links" -EPERM. */
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
    rc = path_exists(name);
    if (rc != 0) {
        return rc;
    }
    return -EPERM;
}

/*
 * _llseek (syscall 140): 64-bit seek. EBX=fd, ECX=offset_high, EDX=offset_low,
 * ESI=result (long long*), EDI=whence. musl's lseek is implemented on top of this
 * on i386. KRFFS files are under 4 GB so offset_high is 0.
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
    /* The descriptor offset is 32-bit (KRFFS files are well under 4 GB); refuse a
     * seek it cannot represent rather than silently truncating to a wrong place. */
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
 * round-trips libc needs: F_DUPFD(_CLOEXEC), F_GETFD/F_SETFD (the close-on-exec
 * bit), and F_GETFL/F_SETFL (access + O_NONBLOCK/O_APPEND, stored, not enforced).
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
            /* O_APPEND and O_NONBLOCK are the settable status flags; they live on
             * the open-file description (shared by dup/fork), not the descriptor.
             * Other bits in arg (access mode, O_CLOEXEC) are accepted and ignored
             * per POSIX. */
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
             * OFD; drop it and point dst at oldfd's, taking a reference. A shared
             * socket gains a reference too; FD_CLOEXEC is per-descriptor. */
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
            /* Advisory POSIX locks. Rach2 is single-user with no competing locker,
             * so every lock request is granted (documented no-conflict semantics) --
             * dropbear/musl lock utmp/lastlog and must not see -EINVAL/-ENOSYS. */
            return 0;
        case F_GETLK:
        case F_GETLK64:
            /* Report that no lock would conflict: set l_type (the first short of
             * the struct flock the caller passed) to F_UNLCK (2). */
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
 * `follow` distinguishes stat (follow symlinks) from lstat (do not), which matters
 * only for the /proc magic symlinks, since KRFFS has none. EBX = path, ECX = stat
 * buffer.
 */
static int32_t do_old_stat(const char __user *path, struct linux_stat __user *st, int follow)
{
    struct linux_stat kst;
    char pathbuf[256];
    const char *name;
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
        if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
            return -EFAULT;
        }
        return 0;
    }

    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }

    /* /dev and /proc are synthetic. The old stat(2) ABI is a narrower struct, so
     * translate the synthetic stat64 result down into it. */
    {
        struct vpath vp;
        if (synth_classify(name, &vp) != VNODE_KRFFS) {
            struct linux_stat64 s64;
            int32_t rc = synth_stat(&vp, &s64, follow);
            if (rc != 0) {
                return rc;
            }
            kst.st_mode = (unsigned short)s64.st_mode;
            kst.st_nlink = (unsigned short)s64.st_nlink;
            kst.st_dev = (unsigned long)s64.st_dev;
            kst.st_ino = (unsigned long)s64.st_ino;
            kst.st_rdev = (unsigned long)s64.st_rdev;
            kst.st_blksize = (unsigned long)s64.st_blksize;
            if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
                return -EFAULT;
            }
            return 0;
        }
    }

    if (!krffs_fs) {
        return -ENOENT;
    }

    {
        int rerr = 0;
        node = krffs_resolve_e(name, &rerr);
        if (!node) {
            return (rerr == -ENOTDIR) ? -ENOTDIR : -ENOENT; /* missing vs not-a-dir */
        }
    }

    kst.st_mode = S_IFREG | (node->mode & 0777);
    kst.st_nlink = 1;                              /* Always 1 for KRFFS */
    kst.st_uid = (unsigned short)node->uid;
    kst.st_gid = (unsigned short)node->gid;
    kst.st_size = (unsigned long)node->data_size;
    kst.st_blksize = 4096;
    kst.st_blocks = (node->data_size + 511) / 512;
    kst.st_ino = (unsigned long)node->id; /* the node's DJB hash is the inode */

    if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
        return -EFAULT;
    }

    return 0;
}

/* stat(106) follows symlinks; lstat(107) does not. musl falls back to these from
 * statx for AT_FDCWD paths, so both must exist for stat()/lstat() to work. */
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
 * Iterates the parent's page directory and, for each present user page, maps the
 * SAME physical frame into the child (bumping the frame refcount) rather than
 * copying it; writable pages are write-protected and PTE_COW-marked in both
 * directories so the first write faults into cow_fault for a private copy. Returns
 * 0 on success, -1 on failure.
 */
static int copy_user_pages(process_t *parent, page_dir_t *child_dir)
{
    page_dir_t *parent_dir;
    uint32_t dir_idx, page_idx;
    page_table_t *parent_table;

    if (parent->cr3 == 0) {
        parent_dir = _kernel_dir;
    } else {
        /* cr3 holds &page_dir->phys_page_tables (get_page_dir_phys); recover the
         * page_dir_t* by subtracting that offset. The layout is
         * logical_page_tables[1024] then phys_page_tables[1024], so the offset is
         * 1024 * sizeof(page_table_t*) = 4096 bytes. */
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
             * Copy-on-write: share the frame instead of copying it. The child
             * maps the same physical frame, and the frame's reference count is
             * bumped so it survives until both sides drop it. A WRITABLE parent
             * page is write-protected and marked PTE_COW in BOTH directories, so
             * the first write on either side faults into cow_fault (which makes a
             * private copy and re-grants write). A READ-ONLY page (ELF text,
             * PROT_READ, or an already-COW page from an earlier fork) is shared
             * with its exact flags, no copy; a write to genuine read-only text
             * still segfaults because it carries no PTE_COW.
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

static char _exec_path_buf[64];
static char _exec_argv_storage[512];
static char *_exec_argv_bufs[16];
static char _exec_envp_storage[512];
static char *_exec_envp_bufs[16];

/**
 * Kill the current process after exec has destroyed its old address space.
 *
 * Past free_user_pages there is no image to return to, so a load/stack failure
 * cannot continue. Mark the process a zombie, wake any parent waiting on it (so
 * the scheduler has a target), switch away, and never iret back into the
 * destroyed space.
 */
static void exec_fail_destroyed(void)
{
    process_t *parent;
    page_dir_t *dir;
    int was_init = (current_proc->pid == init_pid && init_pid != 0);

    /* If the console-owning session leader is the one whose exec failed, release
     * the controlling terminal before zombifying so the timer-IRQ serial drain
     * does not target this dead session during the teardown window (mirrors
     * sys_exit; usually a no-op here because sys_execve already released before
     * free_user_pages; kept as a defensive backstop). */
    if (tty_console_owned_by_leader((int)current_proc->sid, (int)current_proc->pid)) {
        tty_set_session(&console_tty, 0, 0);
    }
    /* A dying session leader also disassociates any pty slave it controlled, so a
     * later session can claim the same /dev/pts/N. */
    pty_release_leader((int)current_proc->sid, (int)current_proc->pid);

    current_proc->state = PROC_ZOMBIE;
    /* A failed exec destroyed the image; report it as a SIGKILL death (Linux
     * wait-status: low 7 bits = the signal) so the parent's waitpid sees an
     * abnormal termination rather than a bogus exit code. */
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
     * woken above, so the scheduler always has the parent as a target: this point
     * is reached only for the init-role process itself, under the debug shell. */
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
    /* Return to the debug shell only when it is the supervisor; on a normal boot
     * (which the reasoning above makes unreachable here) halt rather than expose
     * the shell. */
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
}

/**
 * Replace the current process image with a new program (execve(2)).
 *
 * EBX = path, ECX = argv[], EDX = envp[]. Does not return on success. Copies the
 * path, argv, and envp into static buffers, validates the ELF header BEFORE
 * tearing the old image down (so a bad executable leaves the caller intact), then
 * frees the old user pages, streams the new program in, builds the SysV/i386
 * initial stack, and redirects the iret frame to the entry point. A failure after
 * the old image is gone routes to exec_fail_destroyed.
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
     * buffer. A bad user pointer, an over-long path, or an empty path is rejected
     * by the return value, so a kernel-range pointer cannot leak kernel memory.
     * The buffer is 64 bytes (NUL included). */
    if (!user_path) {
        return -EFAULT;
    }
    len = copy_path_cwd(_exec_path_buf, sizeof(_exec_path_buf), user_path);
    if (len < 0) {
        return len;
    }

    /* Copy argv from user space, range-checking every user pointer and bounding
     * the cumulative offset so _exec_argv_storage cannot overflow. An argument
     * that does not fit fails the exec rather than being silently truncated. The
     * pointer array and each string are now read through the copy helpers, which
     * fault-check each access; the bounds and validation order are unchanged. */
    if (user_argv) {
        for (argc = 0; argc < 15; argc++) {
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
            if (argv_offset >= sizeof(_exec_argv_storage)) {
                return -EFAULT;
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
        /* The 16-slot bufs array holds at most 15 args plus a NULL terminator;
         * a 16th non-NULL pointer is too many args, not a silent truncation. */
        if (argc == 15) {
            uint32_t arg_ptr = 0;
            if (copy_from_user(&arg_ptr, (const void __user *)&user_argv[15],
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
        for (envc = 0; envc < 15; envc++) {
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
                return -EFAULT;
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
        /* As with argv: a 16th non-NULL envp pointer overflows the bufs array;
         * fail rather than silently drop the tail of the environment. */
        if (envc == 15) {
            uint32_t env_ptr = 0;
            if (copy_from_user(&env_ptr, (const void __user *)&user_envp[15],
                               sizeof(env_ptr)) != 0) {
                return -EFAULT;
            }
            if (env_ptr != 0) {
                return -E2BIG;
            }
        }
    }
    _exec_envp_bufs[envc] = NULL;

    name = _exec_path_buf;
    if (name[0] == '/') {
        name++;
    }

    if (!krffs_fs) {
        return -ENOENT;
    }

    if (krffs_open(name) != 0) {
        return -ENOENT; /* the file does not exist */
    }

    if (current_proc->cr3 == 0) {
        proc_dir = _kernel_dir;
    } else {
        proc_dir = (page_dir_t *)(current_proc->cr3 - 1024 * sizeof(page_table_t *));
    }

    /* Validate the ELF header BEFORE tearing down the old image, so the common
     * "not a valid executable" failure leaves the calling process intact. */
    {
        Elf32_Ehdr ehdr;

        if (exec_krffs_read((void *)name, &ehdr, sizeof(ehdr), 0) != (int)sizeof(ehdr) ||
            !elf_valid_exec(&ehdr, sizeof(ehdr))) {
            return -ENOEXEC;
        }
    }

    /* The exec is now committed (the image is valid). Drop close-on-exec
     * descriptors before the old address space goes away. */
    close_cloexec_fds();

    /* Record the program's basename as comm for /proc/<pid>/stat and ps. */
    proc_set_comm(current_proc, name);

    /* Reset caught signal dispositions to default: their handler addresses point
     * into the OLD image that is about to be replaced. SIG_IGN dispositions and
     * the blocked mask are preserved (POSIX exec semantics). */
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
     * the destructive load below: once free_user_pages runs the old image is gone
     * and a load/stack failure routes to exec_fail_destroyed (death), so leaving
     * sid set would let the timer-IRQ serial drain target a destroyed image in
     * that window (the same class as the sys_exit teardown bug). On success it is
     * re-published with the unchanged session/group before returning to userspace,
     * preserving exec's keep-the-controlling-terminal semantics; on failure
     * exec_fail_destroyed releases again (idempotent). */
    owned_console = tty_console_owned_by_leader((int)current_proc->sid, (int)current_proc->pid);
    if (owned_console) {
        tty_set_session(&console_tty, 0, 0);
    }

    /* Free old user pages, then stream the new program in (header + program
     * headers + each PT_LOAD; no 512 KB whole-file buffer). Past this point the
     * old image is gone, so any failure terminates the process rather than
     * returning into a destroyed address space. */
    free_user_pages(proc_dir);

    if (elf_load_image(exec_krffs_read, (void *)name, proc_dir, &img) != 0) {
        exec_fail_destroyed();
        __builtin_unreachable();
    }
    entry_point = img.entry;

    stack_phys = alloc_user_page(proc_dir, USER_STACK_BASE);
    if (stack_phys == 0) {
        exec_fail_destroyed();
        __builtin_unreachable();
    }

    k_memset((void *)stack_phys, 0, PAGE_SIZE);

    /*
     * Initial user stack layout (growing down from USER_STACK_BASE + PAGE_SIZE):
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
                                             _exec_envp_bufs, &img, random16);

        if (user_esp == 0) {
            exec_fail_destroyed();
            __builtin_unreachable();
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
         * program in place, so the trapframe still holds the old program's
         * segment selectors (its %gs may be the 0x33 TLS selector) and the PCB
         * still holds its TLS base. A fresh image has no TLS until its libc
         * calls set_thread_area, so restore the plain user-data selector (0x23,
         * as enter_usermode does for a brand-new process) in all four slots and
         * clear the base in both the PCB and the live GDT descriptor. */
        params->gs = 0x23;
        params->fs = 0x23;
        params->es = 0x23;
        params->ds = 0x23;
        current_proc->tls_base = 0;
        gdt_set_tls(0);

        /* Build the address-space map (VMAs) for the new image so the page-fault
         * handler can demand-page, grow the stack, or segfault. The ELF code/data
         * pages themselves are still eagerly loaded above. */
        elf_setup_vmas(&current_proc->vm, &img, USER_STACK_BASE,
                       VM_STACK_MAX_GROWTH);
    }

    /* Reload CR3 to flush the whole TLB: after freeing the old user pages and
     * mapping new stack/code pages, the TLB may hold stale entries from the
     * replaced image. */
    __asm__ __volatile__ (
        "movl %%cr3, %%eax\n\t"
        "movl %%eax, %%cr3"
        ::: "eax", "memory"
    );

    /* Exec succeeded: re-publish the controlling terminal we released above, with
     * the unchanged session/group (exec keeps the controlling terminal), so the
     * new image owns the console exactly as the old one did. */
    if (owned_console) {
        tty_set_session(&console_tty, (int)current_proc->sid, (int)current_proc->pgid);
    }

    return 0; /* unused: iret resumes at the new entry point */
}

/**
 * Create a child process (fork(2)).
 *
 * Clones the current process: PCB, page directory (copy-on-write user pages), and
 * fd table. The child is set up to return from fork with value 0; the parent gets
 * the child's PID. Returns -errno on failure.
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

    /* alloc_process gave the child its own stdio open-file descriptions; release
     * them now (before any failure path or the fd-table copy below overwrites the
     * table), or those OFDs would be orphaned and leak the pool on every fork. */
    release_fd_table(&child->fds);

    child_dir = clone_page_directory();
    if (!child_dir) {
        free_process(child);
        return -ENOMEM;
    }

    /* Copy user pages from parent to child. On failure, release the frames
     * copy_user_pages already allocated and the cloned directory (not just the
     * PCB) or each failed fork leaks a directory and its private page tables.
     * child_dir is never the running (parent) directory, so it frees cleanly. */
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

    /* Inherit the parent's session and process group: a forked child stays in the
     * parent's job-control group until it (or the parent) calls setsid/setpgid.
     * alloc_process defaulted these to the child's own pid. */
    child->sid = parent->sid;
    child->pgid = parent->pgid;
    child->umask = parent->umask; /* the file-creation mask is inherited */

    /* Inherit the parent's TLS base: the child's copied trapframe keeps the
     * parent's %gs selector, and its address space is a copy at the same
     * virtual addresses, so the same base is correct. Without this the context
     * switch would reprogram the TLS descriptor to 0 for the child and its
     * %gs-relative reads (errno, the stack canary) would fault or read garbage. */
    child->tls_base = parent->tls_base;

    /* The child's address space is a copy of the parent's at the same virtual
     * addresses (copy_user_pages duplicated the pages), so copy the VMA map too
     * otherwise the child would have no regions and the first heap/stack
     * fault would segfault it. (k_memcpy, not struct assignment, so the
     * freestanding kernel does not need a compiler-emitted memcpy.) */
    k_memcpy(&child->vm, &parent->vm, sizeof(child->vm));

    /* The child inherits the parent's current working directory. */
    k_memcpy(child->cwd, parent->cwd, sizeof(child->cwd));

    /* Signals: the child inherits the dispositions and the blocked mask, but its
     * pending set starts empty (POSIX fork semantics). */
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

    /* The ISR frame, copied from the parent, with eax (fork's return) set to 0. */
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

/* Interrupts-off helpers (defined below with the signal code); used here for the
 * lost-wakeup-safe sleep in do_wait. */
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
 * sys_exit/terminate already stored in exit_code; `rusage_ptr` (0 for waitpid,
 * may be 0 for wait4) is zero-filled on reap. With WNOHANG, returns 0 instead of
 * blocking when a child exists but has not exited.
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
     * status means "don't report status"), so a bad or not-yet-present address is
     * rejected before any child is reaped; otherwise the copy_to_user below
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

        /* Scan ALL matching children for a reportable event (a zombie to reap, or
         * a stop/continue to report), not just the first non-zombie. */
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
                /* The zombie no longer runs on its kernel stack, so return it to
                 * the arena for the next fork; this is the single release point
                 * for a per-fork stack. */
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

            /* A stop (WUNTRACED) or continue (WCONTINUED) is REPORTED, not reaped;
             * jc_status is consumed so the same event is not delivered twice. */
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
        /* A child exists but has no reportable event. WNOHANG returns 0; otherwise
         * block until a child changes state (exit/stop/continue wakes us) or a
         * signal interrupts. Lost-wakeup-safe: the signal_pending check and the
         * PROC_BLOCKED transition are one interrupts-off step, so a signal posted
         * from an IRQ either is seen here or finds us BLOCKED and wakes us. */
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
            __asm__ __volatile__("sti; hlt; cli"); /* idle until a waker, then re-scan */
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
 * sys_wait4 - like waitpid plus an optional rusage out-pointer (zero-filled).
 * Args: EBX = pid, ECX = status, EDX = options, ESI = rusage. musl's wait/waitpid
 * are implemented on top of this.
 */
static int32_t sys_wait4(isr_params_t *params)
{
    return do_wait((int)params->ebx, (int __user *)params->ecx,
                   (int)params->edx, params->esi);
}

/*
 * creat(2): EBX = path, ECX = mode. Returns an fd open for writing, or -errno. An
 * existing file is opened (and truncated by the caller's later writes).
 */
static int32_t sys_creat(isr_params_t *params)
{
    const char __user *path = (const char __user *)params->ebx;
    uint32_t mode = params->ecx;
    fd_entry_t *entry;
    krffs_node_t *node;
    char pathbuf[256];
    const char *name;
    int len;
    int fd;

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

    if (name[0] == '\0') {
        return -EINVAL; /* empty filename */
    }

    /* A synthetic mount is read-only: never create/shadow under /dev or /proc. */
    if (synth_is_synth(name)) {
        return -EROFS;
    }

    if (!krffs_fs) {
        return -ENOENT;
    }

    if (krffs_create(name) != 0) {
        return -ENOSPC;
    }

    node = krffs_resolve(name);
    if (!node) {
        return -EIO;
    }

    node->mode = (uint16_t)(mode & 0777);

    fd = fd_alloc(&current_proc->fds);
    if (fd < 0) {
        return -EMFILE;
    }

    entry = &current_proc->fds.entries[fd];
    entry->type = FD_TYPE_FILE;
    entry->node = node;
    entry->ofd->offset = 0;
    entry->flags = FD_FLAG_READ | FD_FLAG_WRITE;

    return fd;
}

/* unlink(2): EBX = path. Returns 0 or -errno. */
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

    /* A synthetic mount is read-only. */
    if (synth_is_synth(name)) {
        return -EROFS;
    }

    if (!krffs_fs) {
        return -ENOENT;
    }

    return krffs_unlink(name);
}

/*
 * sys_mkdir - create a directory.
 * Args: EBX = path, ECX = mode (ignored; no permission model). Returns 0 or
 * -errno (-EEXIST if it exists, -ENOENT if a parent is missing).
 */
static int32_t sys_mkdir(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
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
    if (synth_is_synth(pathbuf[0] == '/' ? pathbuf + 1 : pathbuf)) {
        return -EROFS; /* cannot create under (or shadow) /dev or /proc */
    }
    if (krffs_resolve(pathbuf)) {
        return -EEXIST;
    }
    if (krffs_mkdir(pathbuf) != 0) {
        return -ENOENT; /* a parent directory does not exist */
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
    if (synth_is_synth(pathbuf[0] == '/' ? pathbuf + 1 : pathbuf)) {
        return -EROFS; /* /dev and /proc are not removable */
    }
    node = krffs_resolve(pathbuf);
    if (!node) {
        return -ENOENT;
    }
    if (!KRFFS_IS_DIR(node->mode)) {
        return -ENOTDIR;
    }
    if (krffs_rmdir(pathbuf) != 0) {
        return -ENOTEMPTY; /* still has children */
    }
    return 0;
}

/* rename(2): EBX = old path, ECX = new path. Returns 0 or -errno. */
static int32_t sys_rename(isr_params_t *params)
{
    const char __user *old_path = (const char __user *)params->ebx;
    const char __user *new_path = (const char __user *)params->ecx;
    char old_buf[256];
    char new_buf[256];
    const char *old_name;
    const char *new_name;
    int len;

    if (!current_proc || !old_path || !new_path) {
        return -EFAULT;
    }

    /* Copy both paths into kernel buffers, resolving relative names against cwd. */
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
        return -EINVAL; /* empty filename */
    }

    /* Neither end may be a synthetic mount. */
    if (synth_is_synth(old_name) || synth_is_synth(new_name)) {
        return -EROFS;
    }

    if (!krffs_fs) {
        return -ENOENT;
    }

    return krffs_rename(old_name, new_name);
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
 * get_current_time - convert kernel ticks to seconds/nanoseconds
 * Ticks are at 100 Hz, so each tick is 10ms = 10,000,000 ns
 */
static void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t ticks = kernel_ticks;
    *seconds = ticks / TIMER_FREQUENCY;
    *nanoseconds = (ticks % TIMER_FREQUENCY) * 10000000LL;  /* 10ms per tick */
}

/**
 * Update a file's access and modification timestamps (utimensat(2)).
 *
 * EBX = dirfd, ECX = path, EDX = times, ESI = flags. `times` is a 2-element
 * timespec array [atime, mtime]; a NULL `times` sets both to the current time. A
 * tv_nsec of UTIME_NOW means "now", UTIME_OMIT means "leave unchanged". Returns 0
 * or -errno.
 */
static int32_t sys_utimensat(isr_params_t *params)
{
    int dirfd = (int)params->ebx;
    const char __user *user_path = (const char __user *)params->ecx;
    struct kernel_timespec __user *user_times =
        (struct kernel_timespec __user *)params->edx;
    /* int flags = (int)params->esi; */  /* AT_SYMLINK_NOFOLLOW: inert, no symlinks */
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

    /* Copy the path (resolved against AT_FDCWD/the dirfd) and, if given, the two
     * timespecs into kernel memory through the fault-safe helpers. */
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

    /* A synthetic mount is read-only. */
    if (synth_is_synth(name)) {
        return -EROFS;
    }

    if (!krffs_fs) {
        return -ENOENT;
    }

    {
        int rerr = 0;
        node = krffs_resolve_e(name, &rerr);
        if (!node) {
            return (rerr == -ENOTDIR) ? -ENOTDIR : -ENOENT; /* missing vs not-a-dir */
        }
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
 * The n-th entry of a synthetic directory (kind/pid from the dir fd's cookie):
 * fills name/ino/dtype and returns 1, or 0 past the end. "." and ".." are entries
 * 0 and 1 of every directory.
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
        const char *dn = devfs_name(n);
        int i;
        if (!dn) {
            return 0;
        }
        for (i = 0; i < 30 && dn[i]; i++) {
            name[i] = dn[i];
        }
        name[i] = '\0';
        *ino = 16u + (uint32_t)n;
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
    if (kind == VNODE_PROC_DIR) {
        if (n == 0) {
            name[0] = 's';
            name[1] = 'e';
            name[2] = 'l';
            name[3] = 'f';
            name[4] = '\0';
            *ino = procfs_ino_dir();
            *dtype = DT_LNK;
            return 1;
        }
        {
            int p = procfs_nth_pid(n - 1);
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
 * EBX = fd, ECX = dirp buffer, EDX = count. Packs linux_dirent64 records into the
 * buffer, one per child, and advances the fd's offset cursor. Returns the bytes
 * written, 0 at end of directory, or -errno.
 */
static int32_t sys_getdents64(isr_params_t *params)
{
    int fd_num = (int)params->ebx;
    char *buf = (char *)params->ecx;
    uint32_t count = params->edx;
    fd_entry_t *entry;
    krffs_node_t *node;
    uint32_t bytes_written = 0;
    uint32_t file_index = 0;
    uint32_t target_index;
    uint32_t name_len;
    uint32_t rec_len;
    uint64_t dir_id;
    struct linux_dirent64 *dirent;

    if (!current_proc || !buf || count == 0) {
        return -EFAULT;
    }

    /* Records are packed straight into the user buffer below, so range-check AND
     * fault in the whole [buf, buf+count) span once; every record write then
     * stays inside present, validated user memory. */
    if (!user_buffer_ok((uint32_t)buf, count, 1)) {
        return -EFAULT;
    }

    entry = fd_get(&current_proc->fds, fd_num);
    if (!entry) {
        return -EBADF;
    }

    /* A synthetic directory (/dev, /proc, /proc/<pid>[/fd]) lists from the process
     * table or device registry, not KRFFS. Bare `ls /` does not merge these mount
     * names; they are reached by their absolute paths. */
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

    target_index = entry->ofd->offset; /* the file index reached so far */

    /* List only the children of THIS directory: entry->node is the directory
     * (NULL == the root), so match nodes whose parent_id is its id. */
    dir_id = entry->node ? entry->node->id : (uint64_t)KRFFS_ROOT_ID;

    node = krffs_fs->node;
    do {
        if (node->type == KRFFS_RESERVED_NODE && node->parent_id == dir_id) {
            if (file_index >= target_index) {
                /* Staging buffer for one record (a KRFFS name is at most
                 * KRFFS_FILE_NAME_BUFFER_SIZE bytes, so this always fits). */
                uint8_t recbuf[KRFFS_FILE_NAME_BUFFER_SIZE + 32];

                name_len = k_strlen((const char *)node->name);
                /* Fixed fields (19 bytes) + name + NUL, padded to 8-byte align. */
                rec_len = sizeof(uint64_t) + sizeof(int64_t) + sizeof(uint16_t) + sizeof(uint8_t) + name_len + 1;
                rec_len = (rec_len + 7) & ~7;

                if (bytes_written + rec_len > count) {
                    if (bytes_written == 0) {
                        return -EINVAL; /* buffer too small for even one entry */
                    }
                    break;
                }

                /* Build the record in kernel memory, then copy it out through the
                 * helper (no raw write into user space). */
                k_memset(recbuf, 0, rec_len);
                dirent = (struct linux_dirent64 *)recbuf;
                dirent->d_ino = node->id;                      /* the DJB hash is the inode */
                dirent->d_off = (int64_t)(file_index + 1);     /* next position */
                dirent->d_reclen = (uint16_t)rec_len;
                dirent->d_type = KRFFS_IS_DIR(node->mode) ? DT_DIR : DT_REG;
                k_memcpy(dirent->d_name, node->name, name_len + 1);

                if (copy_to_user((void __user *)(buf + bytes_written),
                                 recbuf, rec_len) != 0) {
                    return (bytes_written > 0) ?
                           (int32_t)bytes_written : -EFAULT;
                }

                bytes_written += rec_len;
                entry->ofd->offset = file_index + 1;
            }
            file_index++;
        }
    } while ((node = krffs_get_next_node(krffs_fs, node)) != krffs_fs->node);

    /* Wrote nothing after scanning every child: end of directory. */
    if (bytes_written == 0 && file_index > 0 && target_index >= file_index) {
        return 0;
    }

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
 * gettimeofday(2): EBX = tv (timeval*), ECX = tz (ignored). Reports time since
 * boot from the kernel_ticks counter (100 Hz). Returns 0 or -errno.
 */
static int32_t sys_gettimeofday(isr_params_t *params)
{
    struct kernel_timeval __user *tv = (struct kernel_timeval __user *)params->ebx;
    struct kernel_timeval ktv;

    if (!tv) {
        return -EFAULT;
    }

    /* Build the result in a kernel struct, then copy it out (this also closes a
     * previously unchecked raw write to the user pointer).
     * kernel_ticks runs at 100 Hz, so 1 tick = 10ms = 10000us */
    ktv.tv_sec = (int32_t)(kernel_ticks / 100);
    ktv.tv_usec = (int32_t)((kernel_ticks % 100) * 10000);

    if (copy_to_user(tv, &ktv, sizeof(ktv)) != 0) {
        return -EFAULT;
    }

    return 0;
}

/**
 * Read a POSIX clock (clock_gettime, syscall 265, 32-bit timespec).
 *
 * EBX = clockid, ECX = struct timespec*. CLOCK_REALTIME(0) and CLOCK_MONOTONIC(1)
 * both report time since boot at the 10 ms tick resolution (there is no wall-clock
 * RTC sync). musl falls back here from clock_gettime64 (403), which this kernel
 * does not provide.
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

    ticks = kernel_ticks;
    kts.tv_sec = (int32_t)(ticks / 100);
    kts.tv_nsec = (int32_t)((ticks % 100) * 10000000); /* 10 ms = 1e7 ns per tick */

    if (copy_to_user(ts, &kts, sizeof(kts)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * Block-and-schedule sleep (nanosleep, syscall 162, 32-bit timespec).
 *
 * EBX = const struct timespec* req, ECX = struct timespec* rem (optional). Rounds
 * up to whole 10 ms ticks, then parks the process BLOCKED on its wakeup_tick and
 * yields; the timer wakes it at the deadline while OTHER processes keep running.
 * rem is written as zero on normal completion, or the time left on -EINTR.
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
        /* A signal cuts the sleep short: stop sleeping, report the time left, and
         * return -EINTR so it is acted on when the syscall returns to user. */
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
            /* schedule() found nobody else to run: idle until the next interrupt
             * (the tick) rather than busy-spinning on the scheduler. */
            __asm__ __volatile__("sti; hlt");
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
 * Identity: Rach2 is single-user, so every process is root. getuid/geteuid/
 * getgid/getegid (the i386 *32 variants, 199-202) all report 0.
 */
static int32_t sys_getuid(isr_params_t *params)
{
    (void)params;
    return 0;
}

/* getrandom flag bits (Linux). We never block and have one source, so they only
 * gate validation here. */
#define GRND_NONBLOCK 0x0001u
#define GRND_RANDOM   0x0002u
#define GRND_INSECURE 0x0004u

/**
 * Fill a buffer with random bytes (getrandom, syscall 355).
 *
 * EBX = buf, ECX = buflen, EDX = flags. Backed by the kernel CSPRNG (lib/krandom,
 * ChaCha20). Returns the byte count, like Linux; musl's getrandom/arc4random sit
 * on top of this. We never block (GRND_NONBLOCK is a no-op) and have a single
 * source (GRND_RANDOM is equivalent); unknown flag bits are rejected.
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
    /* Fail closed: with no trusted hardware entropy (RDSEED/RDRAND), refuse rather
     * than hand out the weak boot-jitter stream for a key. A caller that explicitly
     * accepts insecure bytes passes GRND_INSECURE (the same tier as AT_RANDOM / the
     * stack canary). */
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
 * signal_post runs from IRQ context (Ctrl-C) as well as syscalls, so guard every
 * pending/blocked mutation with interrupts off to avoid a lost update. Returns
 * the prior IF so the caller can restore it. */
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
 * Post signal `signo` to `target`: set its pending bit. If the target is blocked
 * in the scheduler (sleeping/waiting), make it runnable so it reaches a
 * return-to-user delivery point. SIGKILL/SIGSTOP cannot be caught, so a pending
 * SIGKILL is honored even before delivery by leaving it pending for the default
 * path. (Interrupting a blocked syscall with -EINTR is deferred; targets here are
 * running or self.)
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

/* Non-zero if the current process has a deliverable (pending, unblocked) signal.
 * Blocking syscall loops poll this so a signal interrupts the wait (the signal is
 * then delivered/acts on the syscall's return to user). */
int signal_pending(void)
{
    uint64_t deliverable;
    uint32_t flags;
    int signo;

    if (!current_proc) {
        return 0;
    }
    /* Snapshot the 64-bit masks with IRQs off: on i386 a 64-bit read is two
     * 32-bit loads, and a signal posted from an IRQ in between could tear it. */
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

/* Post signo to a process group: pid==0 is the caller's group, pid<-1 the group
 * (-pid). Returns 0 if at least one process was signalled, -ESRCH if the group is
 * empty. signo 0 only probes for a member. */
static int32_t kill_group(int pgid, int signo)
{
    int found = 0;
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
        if (signo != 0) {
            signal_post(p, signo);
        }
    }
    return found ? 0 : -ESRCH;
}

/* kill(2): pid > 0 a single process; pid == 0 the caller's process group; pid < -1
 * the group (-pid). (pid == -1 "all processes" is not supported here.) signo 0
 * just probes existence. */
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
    if (signo == 0) {
        return 0; /* existence check only */
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

/* Block until a deliverable signal is pending. Lost-wakeup-safe: the pending check
 * and the BLOCKED transition are one interrupts-off critical section, so a signal
 * posted from an IRQ either is seen here or finds us BLOCKED and wakes us (the
 * wait_queue.c discipline). IF is re-enabled only while yielding/idling. */
static void sig_wait(void)
{
    uint32_t flags = sig_irq_save();

    while (!signal_pending()) {
        current_proc->state = PROC_BLOCKED;
        sig_irq_restore(flags);
        schedule();
        flags = sig_irq_save();
        if (current_proc->state == PROC_BLOCKED) {
            __asm__ __volatile__("sti; hlt; cli");
        }
    }
    current_proc->state = PROC_RUNNING;
    sig_irq_restore(flags);
}

/*
 * sys_pause - sleep until a signal is delivered (pause(2)). Returns -EINTR once a
 * deliverable signal is pending (the signal then acts on the return to user).
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
 * EBX = const sigset_t *mask, ECX = sigsetsize (8). Replaces the blocked mask with
 * *mask, waits for a deliverable signal, restores the old mask, then returns
 * -EINTR. The handler runs under the RESTORED mask: a simplification, since full
 * save-the-mask-through-the-handler semantics are not modelled.
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
 * ESI=sigsetsize. Stores the action per-signal; SIGKILL/SIGSTOP cannot be changed.
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
 * rt_sigreturn. It is self-consistent (the kernel owns both ends), so it does not
 * need to match musl's ucontext byte layout: a plain handler only reads `sig`,
 * and only rt_sigreturn reads `saved`. info/uc give SA_SIGINFO handlers valid
 * (zeroed) argument pointers.
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

    /* Block the handler's mask, plus this signal unless SA_NODEFER, for the run. */
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
 * Reads the frame the restorer left on the user stack, validates it, and restores
 * the saved trapframe (segment selectors and EFLAGS forced to safe user values,
 * since the frame is on the untrusted user stack) plus the pre-handler signal
 * mask. Returns the restored EAX so the dispatcher does not clobber it.
 *
 * `delta` is how far below the current user esp the frame begins, which differs by
 * trampoline: musl's __restore_rt enters with esp just past `pretcode` (delta 4),
 * while the legacy __restore pops the signal word first and so enters 4 bytes
 * higher (delta 8). Both read the same kernel-built frame.
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

    /* Restore the general registers and instruction/stack pointers, but FORCE the
     * segment selectors to ring-3 user values and sanitize EFLAGS (keep IF set,
     * mask the privileged/IOPL bits) so a tampered frame cannot escalate. */
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
     * selector (0x33) the handler ran with; coerce anything that is not a ring-3
     * selector to flat user data so a forged frame cannot smuggle a kernel
     * selector to iret (a bogus ring-3 selector at worst faults the process). */
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
 * handlers, no SA_SIGINFO). It pops the signal word before the `int $0x80`, so the
 * frame sits 8 bytes below the user esp. Reads the same kernel-built frame as the
 * rt path; the popped word is the handler's `sig` argument and is discarded.
 */
static int32_t sys_sigreturn(isr_params_t *tf)
{
    return signal_restore(tf, 8);
}

/**
 * Act on a pending signal at the return-to-user boundary.
 *
 * Called from the ISR/IRQ return path just before iret, with the trapframe about
 * to be restored. If the current process is returning to USER mode and has a
 * deliverable (pending, not blocked) signal, act on the lowest-numbered one:
 * ignore it, terminate (default), stop, or build a handler frame.
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
                /* The real init (PID 1) is unstoppable, like Unix init: stopping
                 * it would freeze the system with no reaper and no way back. Drop
                 * a job-control stop aimed at it. */
                if (current_proc->pid == init_pid && !debug_shell_is_home) {
                    continue;
                }
                /* Job-control stop: park as PROC_STOPPED until a SIGCONT (or a
                 * SIGKILL, to die) flips us back to READY. Record the stop for the
                 * parent's wait4(WUNTRACED) and nudge it with SIGCHLD. */
                parent = get_process(current_proc->parent_pid);
                current_proc->jc_status = (uint32_t)(((signo & 0xff) << 8) | 0x7f);
                current_proc->state = PROC_STOPPED;
                if (parent) {
                    signal_post(parent, SIGCHLD);
                }
                /* Yield to others; if none are runnable, idle. The scheduler keeps
                 * us STOPPED (it only flips RUNNING->READY); signal_post resumes us.
                 * `sti;hlt;cli` from IF=0 is lost-wakeup-safe. */
                while (current_proc->state == PROC_STOPPED) {
                    schedule();
                    if (current_proc->state == PROC_STOPPED) {
                        __asm__ __volatile__("sti; hlt; cli");
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

/* True if some live process is in process group `pgid` within session `sid` --
 * the TIOCSPGRP rule that a foreground group must belong to the controlling
 * session. */
static int pgrp_in_session(int pgid, int sid)
{
    int i;

    for (i = 0; i < MAX_PROCS; i++) {
        process_t *p = &proc_table[i];
        if (p->state != PROC_UNUSED && p->state != PROC_ZOMBIE &&
            (int)p->pgid == pgid && (int)p->sid == sid) {
            return 1;
        }
    }
    return 0;
}

/* The tty behind a descriptor for the shared terminal ioctls: the console for a
 * FD_TYPE_CONSOLE fd, the slave tty for a FD_TYPE_PTS (pty slave) fd, else NULL. */
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
 * The real termios layer plus the session/job-control requests, so the console
 * and a pty slave share one implementation. Sets *handled = 1 and returns the
 * result for a recognized terminal request; sets *handled = 0 (return value
 * unused) otherwise, so the caller falls through to its own handling. TCGETS/TCSETS
 * exchange the 36-byte kernel termios (n_tty.h); TCSETSF and TCFLSH flush the
 * unread input.
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
            /* Bytes a read would return right now (a complete cooked line, or the
             * queued count in raw mode); a pending EOF counts as 0. Drains the
             * input source first so freshly arrived input is counted. */
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
            /* arg is the queue selector: TCIFLUSH(0)/TCIOFLUSH(2) drop input; we
             * hold no output queue, so TCOFLUSH(1) is a no-op. Else invalid. */
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
            /* A session leader with no controlling tty claims this terminal for its
             * session; re-claiming by the same session is idempotent. */
            if ((int)current_proc->sid != (int)current_proc->pid) {
                return -EPERM; /* not a session leader */
            }
            if (t->sid != 0 && t->sid != (int)current_proc->sid) {
                return -EPERM; /* owned by another session */
            }
            /* Claim t as this session's SOLE controlling terminal: one atomic step
             * publishes t (sid + foreground group) AND drops every other terminal
             * the session held (the console and any other pty), so the timer-IRQ
             * serial drain never sees a torn or doubled ctty and /dev/tty resolves
             * to t. */
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
 * EBX = fd, ECX = request, EDX = arg. A tty fd (console or pty slave) routes to
 * the shared termios/session handler first; an ops-backed object handles its own
 * remaining requests; the framebuffer and console handle their device-specific
 * ones (FB_*, KIOCSOUND, AUDIO_*). Returns 0, the request result, or -errno
 * (-ENOTTY for a terminal request on a non-terminal fd).
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

    /* Validate the fd first, so a bad fd reports -EBADF rather than a request-
     * specific status (e.g. ioctl(-1, TCGETS) must be -EBADF, not -ENOTTY). */
    entry = fd_get(&current_proc->fds, fd_num);
    if (!entry) {
        return -EBADF;
    }

    /* tty-family fds (the console and a pty slave) share the termios/session
     * ioctls, handled here, BEFORE the generic ops dispatch, so a pty slave's
     * TCGETS/TIOCSCTTY reach the shared handler rather than its FIONBIO-only
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

    /* A backing-ops object (pipe/pty/device) owns its remaining ioctls: FIONBIO,
     * and (for the pty master) TIOCGPTN/TIOCSPTLCK/TIOCSWINSZ. */
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

    /* KIOCSOUND is handled by the console-type block below, keyed on the backing
     * object's type (not the fd number) so it cannot fire on an fd 1/2 that
     * has been closed and reopened onto a regular file (now that close(0/1/2) is
     * legal). */

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
 * Range-check a user buffer AND back its pages before the kernel passes the raw
 * pointer to a lower layer (krffs, tcp, udp, the drivers) that dereferences it
 * directly. With demand paging a numerically valid pointer may still be unmapped
 * (a never-touched heap page, or a wild address in no VMA); vm_prefault_range
 * faults the valid pages in here and rejects the bad ones, so the lower layer's
 * raw access can never fault in kernel mode and panic. 'write' is 1 when that
 * layer writes into the buffer (so the pages need write permission).
 */
static int user_buffer_ok(uint32_t addr, uint32_t len, int write)
{
    return user_access_ok(addr, len) && vm_prefault_range(addr, len, write);
}

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
/* Rach2's private ICMP-echo socketcall. RENUMBERED out of the BSD range (it used
 * to squat on 15 == Linux getsockopt, which a real server needs). Userspace
 * callers (utils/ping.c, tests/pingtest.c) use the same value. */
#define SC_PING     0x1000

/* recv flags: only MSG_DONTWAIT is honored. */
#define MSG_DONTWAIT 0x40

/* We read sin_family/sin_port/sin_addr from the first 8 sockaddr bytes. */
#define SOCKADDR_MIN 8

/**
 * Write an AF_INET source/peer sockaddr back to a caller's (addr, addrlen) pair.
 *
 * Respects the caller's declared buffer capacity: read the in/out socklen, copy at
 * most that many of the 16 sockaddr_in bytes, and report the true length (16), the
 * recvfrom(2)/accept(2) truncation semantics. Returns 0 or -EFAULT.
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

/* socket(2): only AF_INET (domain 2), SOCK_STREAM, and SOCK_DGRAM. Returns a new
 * socket fd or -errno. */
static int32_t do_socket(int domain, int type, int protocol)
{
    struct socket *sock;
    fd_entry_t    *entry;
    int            fd;

    (void)protocol;

    /* Only IPv4 (AF_INET == 2) is implemented; reject other families instead of
     * silently creating an unusable socket. */
    if (domain != 2) {
        return -EAFNOSUPPORT;
    }
    if (type != SOCK_DGRAM && type != SOCK_STREAM) {
        return -EINVAL;
    }

    sock = socket_alloc(type);
    if (!sock) {
        return -ENOMEM;
    }

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

    port = SC_BE16(sa + 2);

    if (entry->sock->type == SOCK_DGRAM) {
        /* A datagram send before bind auto-binds an ephemeral source port; drop
         * that registration before claiming the requested one, or its port-table
         * slot leaks for the socket's life. */
        if (entry->sock->local_port != 0) {
            udp_unbind(entry->sock->local_port);
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
    entry->sock->local_port = port;
    return 0;
}

/* sendto(2): a stream socket ignores the destination and behaves like send; a
 * datagram socket sends to the sockaddr, binding an ephemeral source port first. */
static int32_t do_sendto(int fd, uint32_t buf_ptr, int len,
                         int flags, uint32_t sa_ptr, int addrlen)
{
    fd_entry_t *entry;
    uint16_t    dst_port;
    uint32_t    dst_ip;
    uint8_t     sa[SOCKADDR_MIN];

    (void)flags;

    if (len < 0 || !user_buffer_ok(buf_ptr, (uint32_t)len, 0)) {
        return -EFAULT;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }

    /* On a connected stream socket, sendto with any (or no) destination behaves
     * like send; the connection's peer is the destination, so the sockaddr is
     * ignored (this is the path musl's send() takes: sendto with a NULL addr). */
    if (entry->sock->type == SOCK_STREAM) {
        return (int32_t)tcp_send(entry->sock,
                                 (const uint8_t __force *)(const void __user *)buf_ptr,
                                 len);
    }

    /* The payload must fit one unfragmented UDP packet (this stack does not
     * fragment; udp_tx caps it at 1500). Reject an oversize datagram with
     * -EMSGSIZE rather than truncate it through the uint16_t wire length or let
     * udp_tx silently drop it while this call reports a full send. */
    if (len > 1500) {
        return -EMSGSIZE;
    }

    /* Datagram path: a real destination address is required. */
    if (addrlen < SOCKADDR_MIN) {
        return -EINVAL;  /* declared sockaddr shorter than the header we read */
    }
    /* Copy the destination sockaddr into a kernel buffer for the BE helpers. */
    if (copy_from_user(sa, (const void __user *)sa_ptr, SOCKADDR_MIN) != 0) {
        return -EFAULT;
    }

    /* A datagram socket needs a source port so replies can come back. The
     * binding table is smaller than the socket table, so the ephemeral bind can
     * fail; report it rather than send from port 0 and claim success. */
    if (entry->sock->local_port == 0) {
        if (udp_bind_ephemeral(entry->sock) == 0) {
            return -EAGAIN;
        }
    }

    dst_port = SC_BE16(sa + 2);
    dst_ip   = SC_BE32(sa + 4);

    /* udp_tx reads the payload itself; __force strips __user at the boundary. */
    udp_tx(entry->sock->local_port, dst_ip, dst_port,
           (const uint8_t __force *)(const void __user *)buf_ptr, (uint16_t)len);
    return len;
}

/* recvfrom(2): a stream socket behaves like recv (no source writeback); a datagram
 * socket reports the source address, truncated to the caller's capacity. Honors
 * MSG_DONTWAIT and O_NONBLOCK. */
static int32_t do_recvfrom(int fd, uint32_t buf_ptr, int len,
                           int flags, uint32_t sa_ptr, uint32_t salen_ptr)
{
    fd_entry_t *entry;
    uint32_t    src_ip;
    uint16_t    src_port;
    int         bytes;
    int         nonblock;

    if (len < 0 || !user_buffer_ok(buf_ptr, (uint32_t)len, 1)) {
        return -EFAULT;
    }
    /* The source-address writeback is validated and truncated to the caller's
     * declared length by copy_sockaddr_to_user below. */

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }

    /* Receive without blocking when the caller asked per-call (MSG_DONTWAIT) or
     * the descriptor is O_NONBLOCK (an OFD status flag, shared by dup/fork). */
    nonblock = (flags & MSG_DONTWAIT) ||
               (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));

    /* On a connected stream socket, recvfrom behaves like recv; the peer is the
     * connection's, so the optional source-address writeback is skipped and a NULL
     * sockaddr is fine (this is the path musl's recv() takes: recvfrom with a NULL
     * addr). */
    if (entry->sock->type == SOCK_STREAM) {
        if (nonblock) {
            return (int32_t)socket_tcp_try_recv(
                entry->sock, (uint8_t __force *)(void __user *)buf_ptr, len);
        }
        return (int32_t)socket_tcp_recv(entry->sock,
                                        (uint8_t __force *)(void __user *)buf_ptr,
                                        len);
    }

    /* socket_recv/socket_try_recv write straight into the validated user buffer;
     * __force strips __user at the boundary. */
    if (nonblock) {
        bytes = socket_try_recv(entry->sock,
                                (uint8_t __force *)(void __user *)buf_ptr, len,
                                &src_ip, &src_port);
        if (bytes < 0) {
            return bytes;   /* SOCK_EAGAIN: leave the caller's sockaddr alone */
        }
    } else {
        bytes = socket_recv(entry->sock,
                            (uint8_t __force *)(void __user *)buf_ptr, len,
                            &src_ip, &src_port);
    }

    /* Report the source address, truncated to the caller's declared capacity. */
    if (copy_sockaddr_to_user(sa_ptr, salen_ptr, src_port, src_ip) != 0) {
        return -EFAULT;
    }
    return bytes;
}

static int32_t do_ping(uint32_t dst_ip)
{
    return (int32_t)icmp_ping(dst_ip);
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

/* connect(2): start the TCP handshake to the sockaddr. A blocking socket waits it
 * out (-EINTR on a signal); a non-blocking one returns 0 or -EINPROGRESS. */
static int32_t do_connect(int fd, uint32_t sa_ptr, int addrlen)
{
    fd_entry_t *entry;
    uint32_t    dst_ip;
    uint16_t    dst_port;
    uint8_t     sa[SOCKADDR_MIN];
    int         rc;

    if (addrlen < SOCKADDR_MIN) {
        return -EINVAL;  /* declared sockaddr shorter than the header we read */
    }

    /* Copy the sockaddr header into a kernel buffer for the BE helpers. */
    if (copy_from_user(sa, (const void __user *)sa_ptr, SOCKADDR_MIN) != 0) {
        return -EFAULT;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    if (entry->sock->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }

    dst_port = SC_BE16(sa + 2);
    dst_ip   = SC_BE32(sa + 4);

    if (tcp_connect(entry->sock, dst_ip, dst_port) < 0) {
        return -ECONNREFUSED;
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
    if (entry->sock->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }

    entry->sock->listening = 1;
    return tcp_listen(entry->sock, entry->sock->local_port);
}

/* accept(2): dequeue a completed connection (blocking, or -EAGAIN on a
 * non-blocking listener), install a new fd for it, and report the peer address. */
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
    listen_sock = entry->sock;

    /* A non-blocking listener returns immediately when nothing is queued. */
    if (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK) &&
        listen_sock->accept_head == listen_sock->accept_tail) {
        return -EAGAIN;
    }

    /* Block until a connection is queued, yielding to other processes; a pending
     * signal unwinds with -EINTR. */
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

    /* Report the peer address, truncated to the caller's declared capacity. On a
     * bad user pointer, tear down the descriptor we just installed (releasing its
     * socket reference) rather than leaking the accepted fd and its socket. */
    if (copy_sockaddr_to_user(sa_ptr, salen_ptr, conn_sock->remote_port,
                              conn_sock->remote_ip) != 0) {
        fd_release_backing(new_entry);
        fd_free(&current_proc->fds, new_fd);
        return -EFAULT;
    }
    return new_fd;
}

/* send(2): write to a connected stream socket (the peer is the connection's). */
static int32_t do_send(int fd, uint32_t buf_ptr, int len, int flags)
{
    fd_entry_t *entry;

    (void)flags;

    if (len < 0 || !user_buffer_ok(buf_ptr, (uint32_t)len, 0)) {
        return -EFAULT;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    if (entry->sock->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }

    /* tcp_send copies the payload itself; __force strips __user at the boundary. */
    return (int32_t)tcp_send(entry->sock,
                             (const uint8_t __force *)(const void __user *)buf_ptr,
                             len);
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
    if (entry->sock->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }

    /* Don't block when the caller asked per-call (MSG_DONTWAIT) or the descriptor
     * is O_NONBLOCK. socket_tcp_*recv write into the validated user buffer;
     * __force strips __user at the boundary. */
    nonblock = (flags & MSG_DONTWAIT) ||
               (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));
    if (nonblock) {
        return (int32_t)socket_tcp_try_recv(
            entry->sock, (uint8_t __force *)(void __user *)buf_ptr, len);
    }
    return (int32_t)socket_tcp_recv(entry->sock,
                                    (uint8_t __force *)(void __user *)buf_ptr,
                                    len);
}

/* getsockname: the socket's local address, the single NIC IP (NET_IP_ADDR) plus
 * its bound or ephemeral port. */
static int32_t do_getsockname(int fd, uint32_t sa_ptr, uint32_t salen_ptr)
{
    fd_entry_t *entry = fd_get(&current_proc->fds, fd);

    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    /* Unlike accept/recvfrom, getsockname requires the buffers (copy_sockaddr_to_
     * user treats NULL as "no address wanted"). */
    if (!sa_ptr || !salen_ptr) {
        return -EFAULT;
    }
    return copy_sockaddr_to_user(sa_ptr, salen_ptr, entry->sock->local_port,
                                 (uint32_t)NET_IP_ADDR);
}

/* getpeername: the connected peer's address (a connected stream socket only). */
static int32_t do_getpeername(int fd, uint32_t sa_ptr, uint32_t salen_ptr)
{
    fd_entry_t *entry = fd_get(&current_proc->fds, fd);

    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    /* A peer exists only once the TCP handshake completes (not a listener, not a
     * still-connecting SYN_SENT, not a closed/UDP socket). */
    if (!tcp_is_connected(entry->sock)) {
        return -ENOTCONN;
    }
    if (!sa_ptr || !salen_ptr) {
        return -EFAULT;
    }
    return copy_sockaddr_to_user(sa_ptr, salen_ptr, entry->sock->remote_port,
                                 entry->sock->remote_ip);
}

/* shutdown(how): SHUT_RD(0)/SHUT_WR(1)/SHUT_RDWR(2). Any direction that includes
 * the write side drives the TCP active close (a FIN, so the peer reads EOF);
 * SHUT_RD alone is accepted as a no-op. We have no lingering half-open state, so a
 * write-side shutdown sends the FIN exactly as close() would (documented). */
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2
static int32_t do_shutdown(int fd, int how)
{
    fd_entry_t *entry = fd_get(&current_proc->fds, fd);

    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    if (how < SHUT_RD || how > SHUT_RDWR) {
        return -EINVAL;
    }
    /* Only a connected stream socket can be shut down, NOT a listener (which
     * would free its LISTEN TCB and silently stop accepting), an unconnected, or a
     * UDP socket. */
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
#define SO_KEEPALIVE  9
#define IPPROTO_TCP   6
#define TCP_NODELAY   1

/* setsockopt: accept the boolean options a forking server sets (SO_REUSEADDR,
 * TCP_NODELAY, SO_KEEPALIVE) so its setup succeeds. The stack already reuses
 * listen ports and sends promptly, so these are honored-by-behavior or documented
 * no-ops; an unknown option is accepted rather than failing a server's startup. */
static int32_t do_setsockopt(int fd, int level, int optname, uint32_t optval_ptr,
                             int optlen)
{
    fd_entry_t *entry = fd_get(&current_proc->fds, fd);
    int val = 0;

    (void)level;
    (void)optname;
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    /* A declared int-sized option with a NULL value is a bad pointer, not a
     * silently-accepted no-op. */
    if (optlen >= (int)sizeof(int)) {
        if (!optval_ptr ||
            copy_from_user(&val, (const void __user *)optval_ptr, sizeof(val)) != 0) {
            return -EFAULT;
        }
    }
    return 0;
}

/* getsockopt: report the integer options programs read at setup: SO_ERROR (no
 * async error is tracked, so 0), SO_TYPE (the real socket type), and the boolean
 * options as 0 (off). Always reports a 4-byte int result. */
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
    } else {
        val = 0; /* SO_ERROR / SO_REUSEADDR / SO_KEEPALIVE / TCP_NODELAY: off/none */
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
    olen = (uint32_t)sizeof(val);
    if (copy_to_user((void __user *)optlen_ptr, &olen, sizeof(olen)) != 0) {
        return -EFAULT;
    }
    return 0;
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
    default:
        return -EINVAL;
    }
}

/**
 * Set up thread-local storage (set_thread_area(2), Linux i386).
 *
 * EBX points at a struct user_desc. Validates it, programs the shared TLS GDT
 * descriptor (entry GDT_TLS_ENTRY) and this process's PCB base, then writes the
 * chosen entry number back so the C library can load (entry_number << 3) | 3 into
 * %gs. The base is reapplied to the GDT on every context switch (switch.s).
 * Returns 0 or a negative errno on a bad pointer/request.
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
     * full struct internally, so the prior explicit user_access_ok is redundant). */
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
 * The caller becomes the leader of a new session AND a new process group, both
 * numbered with its pid. Fails with -EPERM if the caller already leads a process
 * group (a group leader cannot carry its group into a new session). Returns the
 * new session id (the caller's pid).
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
 * EBX = pid (0 = caller), ECX = pgid (0 = the target's own pid, i.e. make it a
 * group leader). The target must be the caller or a child of the caller, must not
 * be a session leader, and the destination group must live in the target's
 * session. Returns 0 or -errno.
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
     * same session (an existing group, regardless of whether its leader is still
     * alive, not merely "a process whose pid == pgid"). */
    if ((uint32_t)pgid != target->pid && !pgrp_in_session(pgid, (int)target->sid)) {
        return -EPERM;
    }
    target->pgid = (uint32_t)pgid;
    return 0;
}

/* Resolve a getpgid/getsid target: the caller for pid 0, else the named process. */
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
 * sys_umask - set the file-creation mask, returning the previous value (umask(2)).
 * Args: EBX = new mask (only the low 9 permission bits are kept).
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
 * Per-process CPU time is not tracked, so the four tms fields are reported as
 * zero; the return value is the monotonic tick count (already in CLK_TCK = 100 Hz
 * units). EBX = struct tms * (NULL is allowed: the caller may want only the return
 * value).
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
 * Fills the 64-byte i386 struct sysinfo: uptime in seconds, a live-process count,
 * mem_unit = 1; CPU load and memory/swap figures are reported as zero (not
 * tracked). EBX = struct sysinfo *.
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

/* Copy a C string into a fixed 65-byte utsname field (always NUL-terminated). */
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

/* setuid/setgid: Rach2 has a single (root, id 0) identity and no user database.
 * Setting the id to 0 succeeds as a no-op (a login server's post-auth privilege
 * drop to the authenticated root must not fail); setting it to any OTHER id fails
 * with -EPERM rather than silently pretending to drop to a user that does not
 * exist, so a caller is never fooled into believing it shed privilege. */
static int32_t sys_setuid(isr_params_t *params)
{
    return (params->ebx == 0) ? 0 : -EPERM;
}
static int32_t sys_setgid(isr_params_t *params)
{
    return (params->ebx == 0) ? 0 : -EPERM;
}
/* setreuid/setregid/setresuid/setresgid: the same single-root-identity policy in
 * the 2- and 3-argument forms a login server uses to drop and restore privilege
 * around an unprivileged step (dropbear reads authorized_keys under
 * setegid(pw_gid)/seteuid(pw_uid), then reverts). Each id argument is either
 * (id_t)-1 ("leave unchanged") or 0 (root, the only identity); any other value is
 * -EPERM, so a caller is never fooled into believing it became a user that does
 * not exist. uid and gid share these handlers because both are 0-only.
 * (sc_id_is_root_or_keep is defined earlier, near do_chown.) */
static int32_t sys_setreid(isr_params_t *params)
{
    /* setreuid(ruid, euid) / setregid(rgid, egid) */
    if (!sc_id_is_root_or_keep(params->ebx) ||
        !sc_id_is_root_or_keep(params->ecx)) {
        return -EPERM;
    }
    return 0;
}
static int32_t sys_setresid(isr_params_t *params)
{
    /* setresuid(ruid, euid, suid) / setresgid(rgid, egid, sgid) */
    if (!sc_id_is_root_or_keep(params->ebx) ||
        !sc_id_is_root_or_keep(params->ecx) ||
        !sc_id_is_root_or_keep(params->edx)) {
        return -EPERM;
    }
    return 0;
}
/* setgroups: the only group is root (gid 0). Clearing the list (size 0) succeeds;
 * a list is validated to be readable and to contain ONLY gid 0; any other gid is
 * "no such group" (-EPERM), so a caller is never fooled into believing it set real
 * supplementary groups. */
static int32_t sys_setgroups(isr_params_t *params)
{
    int size = (int)params->ebx;
    uint32_t list_ptr = params->ecx;
    uint32_t g;
    int i;

    if (size < 0 || size > 256) {
        return -EINVAL; /* a single-group OS never has a long supplementary list */
    }
    for (i = 0; i < size; i++) {
        if (!list_ptr ||
            copy_from_user(&g, (const void __user *)(list_ptr + (uint32_t)i * 4),
                           sizeof(g)) != 0) {
            return -EFAULT;
        }
        if (g != 0) {
            return -EPERM;
        }
    }
    return 0;
}

/* The honest resource caps: RLIMIT_NOFILE/NPROC mirror the kernel's fixed pools,
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

/* getrlimit(76)/ugetrlimit(191): 32-bit struct rlimit { u32 rlim_cur, rlim_max }. */
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
 * ESI=old (rlimit64* or NULL). 64-bit struct rlimit64 { u64 cur, max }. New limits
 * are accepted (validated readable) but not enforced; old limits report the
 * honest caps. This is the form modern musl getrlimit/setrlimit issue. */
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

/* dup3(330): like dup2 but oldfd == newfd is -EINVAL and a flag can set O_CLOEXEC
 * on the new descriptor. */
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
 * sys_brk - set the program break (heap top).
 * Arg: EBX = requested break. Returns the resulting break: the new value on
 * success, or the unchanged current break if the request is out of range. This
 * is the contract musl's __brk relies on (it compares the return to its request).
 * The heap is a single demand-paged VMA [heap_start, page_up(brk)); growing only
 * extends the region (pages fault in lazily), shrinking frees the pages above.
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
         * any other region. The heap's own [heap_start, old_top) abuts that span
         * without overlapping, so vm_overlaps tests only the *other* regions. */
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
        /* Shrink: free the pages above the new top, then trim or drop the VMA. */
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

/*
 * sys_mmap2 - map anonymous memory.
 * Args: EBX=addr (hint, ignored), ECX=length, EDX=prot, ESI=flags, EDI=fd,
 * EBP=pgoffset. Returns the mapped base address, or -errno. Only
 * MAP_ANONYMOUS|MAP_PRIVATE is supported (what musl's allocator uses); the region
 * is placed by bumping mmap_top down and is demand-paged + zero-filled on touch.
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
         * overlap/replace handling we do not have. Reject rather than silently
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
         * fault check agrees with the hardware (a read of a PROT_WRITE page does
         * not wrongly segfault). */
        vmprot |= VM_PROT_WRITE | VM_PROT_READ;
    }
    if (prot & PROT_EXEC) {
        vmprot |= VM_PROT_EXEC;
    }

    /* Bump down from mmap_top; refuse if it would underflow or hit the heap. The
     * addr hint is ignored (no MAP_FIXED support), which suits musl's malloc. */
    if (len > vm->mmap_top || vm->mmap_top - len < PAGE_UP(vm->brk)) {
        return -ENOMEM;
    }
    base = vm->mmap_top - len;

    if (vm_insert(vm, base, base + len, vmprot, VM_ANON) != 0) {
        return -ENOMEM;
    }
    vm->mmap_top = base;
    return (int32_t)base;
}

/*
 * sys_munmap - unmap a range.
 * Args: EBX=addr (page-aligned), ECX=length. Returns 0, or -errno. Drops the VMA
 * bookkeeping (trimming or splitting partial overlaps) and frees the backing
 * frames; unmapping never-faulted or never-mapped pages is a no-op, as on Linux.
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
        return -ENOMEM; /* a hole-punch split would exceed VMA_MAX */
    }
    unmap_user_range(_curr_dir, start, end);
    return 0;
}

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
     * alternate path rather than treating it as a generic -EPERM failure. */
    int32_t ret = -ENOSYS;

    /* Mark this process non-preemptible BEFORE enabling interrupts. The ISR
     * stub keeps IF=0 through entry (segments/%gs are still being saved), so
     * setting in_syscall here and only then doing sti closes the window in
     * which a timer IRQ could reschedule a process whose trapframe is still
     * half-built. The timer handler skips scheduling while in_syscall is set. */
    if (current_proc) {
        current_proc->in_syscall = 1;
    }

    /* Enable interrupts for the syscall body so the PIT/HDA keep ticking during
     * long syscalls (e.g. Doom's framebuffer writes); preemption stays disabled
     * via in_syscall above. The stub no longer does sti; iret restores IF. */
    __asm__ __volatile__("sti");

    if (syscall_num < SYSCALL_TABLE_SIZE && _syscall_table[syscall_num]) {
        ret = _syscall_table[syscall_num](params);
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
    /* Single-user: all four identity calls report root (uid/gid 0). */
    _syscall_table[SYS_GETUID32] = sys_getuid;
    _syscall_table[SYS_GETGID32] = sys_getuid;
    _syscall_table[SYS_GETEUID32] = sys_getuid;
    _syscall_table[SYS_GETEGID32] = sys_getuid;
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
    _syscall_table[SYS_FSYNC] = sys_fsync;
    _syscall_table[SYS_FDATASYNC] = sys_fsync;
    _syscall_table[SYS_LINK] = sys_link;
    _syscall_table[SYS_LINKAT] = sys_linkat;
    _syscall_table[SYS_SETREUID] = sys_setreid;
    _syscall_table[SYS_SETREGID] = sys_setreid;
    _syscall_table[SYS_SETREUID32] = sys_setreid;
    _syscall_table[SYS_SETREGID32] = sys_setreid;
    _syscall_table[SYS_SETRESUID] = sys_setresid;
    _syscall_table[SYS_SETRESGID] = sys_setresid;
    _syscall_table[SYS_SETRESUID32] = sys_setresid;
    _syscall_table[SYS_SETRESGID32] = sys_setresid;
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

    register_isr_handler(SYSCALL_VECTOR, _syscall_handler);
}
