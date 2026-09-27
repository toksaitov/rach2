/*
 * The process arm of the system call layer: exit, exec, fork and wait,
 * signals, identities, sessions, limits and the address space. Registered
 * by syscall.c; what it shares with the other arms is in syscall_internal.h.
 */
#include "../kernel.h"
#include "mm/kheap.h"
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
#include "proc/sigframe.h"
#include "tables/gdt.h"
#include "lib/krandom.h"
#include "lib/kerrno.h"
#include "lib/kirq.h"
#include "lib/ksignal.h"
#include "fs/fd.h"
#include "fs/file.h"
#include "fs/pathbuf.h"
#include "ipc/pipe.h"
#include "io/poll.h"
#include "tty/tty.h"
#include "tty/pty.h"
#include "rtc.h"
#include "block/loop.h"
#include "fs/krffs.h"
#include "fs/devfs.h"
#include "fs/synthfs.h"
#include "fs/vfs.h"
#include "block/usbdisk.h"
#include "fs/procfs.h"
#include "mm/paging.h"
#include "mm/pressure.h"
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
#include "cmdline.h"         /* kcmdline_has, for the ENOSYS trace flag */
#include "mm/filemap.h"
#include "fs/tmpfs.h"
#include "fs/devfs.h"
#include "syscall_internal.h"

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

/* Kernel stack size (16KB) */
#define KERNEL_STACK_SIZE 0x4000

/*
 * A process's kernel stack comes from the heap at fork, KERNEL_STACK_SIZE
 * of pages, and goes back when the zombie is reaped; the one reaping is
 * never the one whose stack it is.
 */
void init_kstacks(void)
{}

/**
 * A kernel stack, returning its base, or 0 when the heap has none.
 *
 * The caller uses `base + KERNEL_STACK_SIZE` as the stack top.
 *
 * QEMU can retain code translations in reused frames. Clear every page
 * so translations outside the used part of the stack cannot keep stack
 * stores on its slow write path. Clearing also leaves the child nothing
 * of the stack's previous owner.
 */
static uintptr_t kstack_alloc(void)
{
    void *base = kalloc_pages(KERNEL_STACK_SIZE / PAGE_SIZE, NULL);

    if (base != NULL) {
        k_memset(base, 0, KERNEL_STACK_SIZE);
    }
    return (uintptr_t)base;
}

/** Give back the stack whose top is `top`. */
static void kstack_free(uintptr_t top)
{
    if (top != 0) {
        kfree_pages((void *)(top - KERNEL_STACK_SIZE), KERNEL_STACK_SIZE / PAGE_SIZE);
    }
}

/* Fork child return point in isr_handler.s */
extern void isr_return(void);

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

/* Kernel stack from loader.s */
extern char stack[];

#define KERNEL_STACK_SIZE_LOCAL 0x4000

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
    for (fdi = 0; fdi < fd_table_cap(table); fdi++) {
        fd_entry_t *e = &table->entries[fdi];
        if (e->type == FD_TYPE_NONE) {
            continue;
        }
        fd_release_backing(e);
        fd_free(table, fdi);
    }
}

/**
 * End every other user process before init returns the machine to the shell.
 *
 * What the program left running is killed and reaped here while init is
 * still a live process the scheduler runs: a leftover asleep in the disk
 * driver with a mount's lock held finishes its call and releases the lock
 * before the signal ends it, and a leftover spinning in user mode takes the
 * signal at its next interrupt, since signals are delivered on the return
 * from an interrupt as well. The shell's resume disables the scheduler, so
 * anything left runnable or holding a lock then would run no further, and
 * the next program would wait on it forever. A zombie is reaped as `wait`
 * reaps one. Kernel threads and the idle record are not user processes
 * and stay.
 */
static void proc_end_others(void)
{
    for (;;) {
        process_t *p, *next;
        int alive = 0;

        for (p = proc_list; p; p = next) {
            next = p->next;
            if (p == current_proc || p == proc_idle) {
                continue;
            }
            if (p->state == PROC_ZOMBIE) {
                if (p->cr3 != 0) {
                    free_user_pages(p->page_dir);
                    free_page_directory(p->page_dir);
                }
                kstack_free(p->kernel_stack);
                free_process(p);
                continue;
            }
            if (p->cr3 == 0) {
                continue;
            }
            signal_post(p, SIGKILL);
            alive++;
        }
        if (!alive) {
            return;
        }
        schedule();
    }
}

/**
 * Terminate the current process (exit(2)/exit_group(2)). Does not return.
 *
 * EBX has the exit status. Releases files and cwd, reparents children to init,
 * publishes the zombie and wakes its parent, then reschedules. A normal child
 * leaves its mappings, descriptor-table storage, record and stack to do_wait.
 * A real init's exit is fatal (panic_init_died); an init exiting under the
 * debug shell returns to it.
 *
 * The plain status line goes out only where the debug shell is the supervisor.
 * There it is how a person, and the harness in its no-token mode, read a
 * transient init's outcome. On an image running `/sbin/init` and a login shell
 * every process exit would print it, a forked child's included, and the shell
 * already answers the question through `$?`. The line that hands the shell back
 * below is not gated: the probe runner scores probes from that one.
 *
 * An init exiting under the debug shell first ends every other user process
 * through `proc_end_others`, then returns without scheduling: a child left
 * runnable would take the processor, and the exited init, a zombie, would
 * never be picked again to make the return. A process that is not init
 * leaves through the scheduler as a zombie its parent reaps; when nothing is
 * ready at that moment, since its parent may be gone, it idles for the next
 * interrupt with interrupts off between checks and schedules on its own,
 * because the timer's preemption is deferred inside a call. A halt there
 * kept the machine forever.
 */
int32_t sys_exit(isr_params_t *params)
{
    int status = (int)params->ebx;
    bool is_init = false;
    uint32_t exiting_pid;

    if (current_proc && fb_owner_pid == current_proc->pid) {
        vga_mode13h_exit();
        fb_owner_pid = 0;
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

    if (is_init && debug_shell_is_home) {
        proc_end_others();
    }
    proc_exit_cleanup((status & 0xFF) << 8);

#ifdef RACH2_HARNESS
    /* Harness build: the instant the probe (run as init) exits, report its
     * status to QEMU's isa-debug-exit device and stop, BEFORE schedule() can
     * switch to a still-READY child and bypass this path, so the runner always
     * reads a real exit code. */
    if (is_init) {
        harness_exit((uint8_t)status);
    }
#endif

    /* A real /sbin/init must never exit; if it does there is no reaper, so the
     * kernel panics. Halt BEFORE schedule(), or a still-READY process would
     * take the CPU and the system would run on with a dead PID 1. Only when the
     * debug shell is the supervisor (rescue, or the no-init test image) does an
     * exited init instead return to it, below. */
    if (is_init && !debug_shell_is_home) {
        panic_init_died(exiting_pid); /* never returns */
    }

    if (!is_init) {
        schedule();
    }

    if (is_init) {
        klog(KLOG_INFO, "sys_exit",
             "init process %d exited with status %d, returning to kernel shell", exiting_pid,
             status);
        console_puts("Init process exited. Returning to kernel debug shell.");

        /* Clean up the init process resources. Switch off its address space
         * BEFORE freeing the page directory: free_page_directory refuses to
         * free whatever CR3 currently points at, so freeing while still loaded
         * would leak the directory and its private page tables. */
        if (current_proc->cr3 != 0) {
            page_dir_t *init_dir;
            init_dir = current_proc->page_dir;
            free_user_pages(init_dir);
            switch_page_directory(_kernel_dir);
            free_page_directory(init_dir);
        } else {
            switch_page_directory(_kernel_dir);
        }
        /* Freed and replaced with interrupts off: a handler between the two
         * would count its entry in the record just freed. */
        __asm__ __volatile__("cli");
        free_process(current_proc);

        init_pid = 0;

        current_proc = proc_idle;
        current_proc->state = PROC_RUNNING;

        /* Clear the shell's command line and stdin-wait state while interrupts
         * are still off, so a keyboard IRQ cannot append to a stale command
         * buffer or see a dangling stdin waiter. shell_loop resets again on
         * entry as the catch-all. */
        shell_reset_input();
        __asm__ __volatile__("sti");

        sched_disable();

        /* Reset ESP to a fresh kernel stack and jump to shell_loop. Safe
         * because we are in kernel mode and init has been torn down. */
        __asm__ __volatile__("movl %0, %%esp\n\t"
                             "sti\n\t"
                             "jmp *%1\n\t"
                             :
                             : "r"((uint32_t)stack + KERNEL_STACK_SIZE_LOCAL),
                               "r"((uint32_t)shell_loop)
                             : "memory");
    }

    for (;;) {
        __asm__ __volatile__("cli");
        schedule();
        idle_wait_irq_off();
    }
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
        /* The directory's record is kept beside cr3, since a directory
         * lives in the heap and its physical address says nothing about
         * where the kernel reads it. The layout is
         * logical_page_tables[1024] then phys_page_tables[1024], so the offset
         * is 1024 * sizeof(page_table_t*) = 4096 bytes. */
        parent_dir = parent->page_dir;
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

            if (!(pte & 0x1) || !(pte & (0x4 | PTE_NONE))) {
                continue; /* present (0x1), and user (0x4) or PROT_NONE */
            }

            virt_addr = (dir_idx << 22) | (page_idx << 12);
            parent_phys = pte & ~0xFFF;

            /* Skip the kernel's identity map below user space and any bogus
             * mapping. */
            if (virt_addr < USER_ADDR_MIN || parent_phys == 0) {
                continue;
            }

            /* A page of a shared region is the same frame in the child, with
             * the same rights: a store on either side is seen by both. */
            {
                vma_t *r = vm_find(&parent->vm, virt_addr);

                if (r && r->type == VM_SHARED) {
                    if (map_user_frame(child_dir, virt_addr, parent_phys, pte & 0xFFFu) != 0) {
                        return -1;
                    }
                    frame_ref(parent_phys);
                    continue;
                }
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
            } else if (map_user_frame(child_dir, virt_addr, parent_phys, pte & 0xFFFu) != 0) {
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

    for (fd = 0; fd < fd_table_cap(&current_proc->fds); fd++) {
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
    const struct exec_krffs_ctx *c = ctx;

    return krffs_read(c->name, (char *)dst, count, off);
}

/* A program on a mount that is not the root is read through the mount: the
 * node held from the resolution, opened once for the load and released
 * after it. It lives in the caller's own exec workspace below, because a
 * read of a mounted volume sleeps and a second exec runs while it does. */
struct exec_vfs_ctx {
    struct vfs_ref ref;
    void *handle;
    int32_t err; /* the last failed read's errno, reported instead of ENOEXEC */
};

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

static void exec_vfs_done(struct exec_vfs_ctx *c)
{
    if (c->ref.mnt) {
        vfs_release(c->ref.mnt, c->handle);
        vfs_ref_release(&c->ref);
        c->ref.mnt = NULL;
        c->handle = NULL;
    }
}

/*
 * Where execve has the new image's arguments while it destroys the old one.
 *
 * One workspace per process, and not one for the machine: a program on a
 * mounted volume is read through the buffer cache, which sleeps, so a second
 * execve runs while the first is inside a read. Sharing one set of buffers
 * meant the second overwrote the first's arguments, its open file and the
 * reference its resolution held, and the mount that reference belongs to
 * could never be unmounted afterwards. `in_syscall` stops the timer from
 * scheduling; it does not stop a call that gives the processor up.
 *
 * They cannot be on the stack (the kernel stack is 16 KB and this is about
 * seven of them) and cannot be in the caller's address space (that is what
 * is being torn down).
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

/* A script's first line, and the script's own path, which becomes an argument
 * to its interpreter. Both in the workspace, because the argv pointers handed
 * on point into them and the caller's memory is gone by the time they are
 * read. */
#define EXEC_SHEBANG_MAX 128

struct exec_state {
    struct exec_vfs_ctx vfs;
    char path[VFS_PATH_MAX];
    char argv_storage[EXEC_MAX_STRINGS];
    char *argv_bufs[EXEC_MAX_ENTRIES + 1];
    char envp_storage[EXEC_MAX_STRINGS];
    char *envp_bufs[EXEC_MAX_ENTRIES + 1];
    char shebang_line[EXEC_SHEBANG_MAX + 1];
    char script_path[VFS_PATH_MAX];
};

/* A process's workspace is allocated at its first exec and kept in its
 * record for the record's life, since a program that execs once execs
 * again; `free_process` gives it back. */
static struct exec_state *exec_state_of(process_t *proc)
{
    if (!proc->exec_ws) {
        proc->exec_ws = kzalloc(sizeof(struct exec_state));
    }
    return proc->exec_ws;
}

/**
 * Kill the current process after exec has destroyed its old address space.
 *
 * Past free_user_pages there is no image to return to, so a load/stack failure
 * cannot continue. Mark the process a zombie, wake any parent waiting on it (so
 * the scheduler has a target), switch away, and never iret back into the
 * destroyed space.
 */
static void __attribute__((noreturn)) exec_fail_destroyed(int where)
{
    /* The process cannot report this itself: its image is gone and its
     * parent sees only a death by signal. The record says which step of
     * the load failed, so a machine that dies here says why. */
    page_dir_t *dir;
    int was_init = (current_proc->pid == init_pid && init_pid != 0);

    klog(KLOG_ERR, "exec", "pid %d died: the image was destroyed and step %d failed",
         (int)current_proc->pid, where);

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

    proc_exit_cleanup(SIGKILL & 0x7F);

    /* A real /sbin/init whose exec failed is a fatal init death; halt BEFORE
     * schedule(), or a still-READY process would mask the panic and run on with
     * a dead PID 1. Only when the debug shell is the supervisor does it return
     * to the shell, below. */
    if (was_init && !debug_shell_is_home) {
        panic_init_died(current_proc->pid); /* never returns */
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
        dir = current_proc->page_dir;
        free_user_pages(dir);
        free_page_directory(dir);
    }
    /* Freed with interrupts off until the record is replaced below: a
     * handler between the two would count its entry in the freed record. */
    __asm__ __volatile__("cli");
    free_process(current_proc);
    current_proc = proc_idle;
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
                         : "r"((uint32_t)stack + KERNEL_STACK_SIZE_LOCAL), "r"((uint32_t)shell_loop)
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
 * The caller's `path` and `argv_bufs` are rewritten in place on success.
 * The script is read through `read` over `ctx`: the root's reader by path,
 * or the mount's over an open handle.
 *
 * @param argc the argument count, updated when the vector grows.
 * @return 1 when rewritten, 0 when the file is not a script, or a negative
 *         errno for one whose first line names nothing.
 */
static int32_t exec_shebang(struct exec_state *s, int *argc,
                            int (*read)(void *, void *, uint32_t, uint32_t), void *ctx)
{
    char *interp;
    char *arg = NULL;
    char *p;
    int n;
    int i;
    int extra;
    int count;
    char *rebuilt[EXEC_MAX_ENTRIES + 1];

    n = read(ctx, s->shebang_line, EXEC_SHEBANG_MAX, 0);
    if (n < 3 || s->shebang_line[0] != '#' || s->shebang_line[1] != '!') {
        return 0;
    }
    s->shebang_line[n] = '\0';
    for (i = 2; i < n; i++) {
        if (s->shebang_line[i] == '\n') {
            s->shebang_line[i] = '\0';
            break;
        }
    }
    /* A line that filled the buffer without ending is refused, not cut:
       a truncated interpreter name may name a different program that exists,
       and the kernel would then check and run that one. */
    if (i == n && n == EXEC_SHEBANG_MAX) {
        return -ENOEXEC;
    }

    p = s->shebang_line + 2;
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
    k_strncpy(s->script_path, s->path, sizeof(s->script_path) - 1);
    s->script_path[sizeof(s->script_path) - 1] = '\0';

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
    rebuilt[i++] = s->script_path;
    for (n = 1; n < *argc; n++) {
        rebuilt[i++] = s->argv_bufs[n];
    }
    rebuilt[i] = NULL;
    for (n = 0; n <= i; n++) {
        s->argv_bufs[n] = rebuilt[n];
    }
    *argc = i;

    k_strncpy(s->path, interp, sizeof(s->path) - 1);
    s->path[sizeof(s->path) - 1] = '\0';
    /* Normalized the way a path from a caller is normalized. The check that
       keeps an exec out of the synthetic mounts reads the first component and
       does not collapse `.` or `..`, so an interpreter named
       `/bin/../dev/something` would pass it while the file system resolved the
       name it really wrote. */
    if (canonicalize_abs(s->path) < 0) {
        return -ENOENT;
    }
    return 1;
}

/**
 * What a move of the effective ids undoes, as Linux's `commit_creds` undoes
 * it: when `p`'s effective uid or gid is no longer `old_euid` or `old_egid`,
 * the process stops being dumpable and loses its parent-death signal, so the
 * parent it had cannot signal the identity it now has.
 */
static void cred_moved(process_t *p, uint32_t old_euid, uint32_t old_egid)
{
    if (p->euid != old_euid || p->egid != old_egid) {
        p->dumpable = 0;
        p->pdeath_signal = 0;
    }
}

/**
 * What an exec's credentials change beside the ids, as Linux's exec does:
 * the process is dumpable again unless it runs with ids its caller lacks
 * (`secure`), such an exec clears the parent-death signal, and one that moved
 * the effective ids from `old_euid` and `old_egid` is treated as
 * `cred_moved` treats an identity call.
 */
static void exec_privileges(int secure, uint32_t old_euid, uint32_t old_egid)
{
    current_proc->dumpable = secure ? 0u : 1u;
    if (secure) {
        current_proc->pdeath_signal = 0;
        if (current_proc->rlim[RLIM_STACK].cur > RLIM_STACK_DEFAULT) {
            current_proc->rlim[RLIM_STACK].cur = RLIM_STACK_DEFAULT;
        }
    }
    cred_moved(current_proc, old_euid, old_egid);
}

/**
 * How far below its first page the stack of a new image may grow: the soft
 * RLIMIT_STACK in force at the exec, which bounds the whole stack as Linux's
 * does, in whole pages, less the page it starts with, and never into the
 * range below VM_MMAP_BASE where anonymous mappings are placed. A limit
 * changed later applies at the next exec.
 */
static uint32_t exec_stack_window(void)
{
    uint32_t limit = current_proc->rlim[RLIM_STACK].cur & ~(uint32_t)(PAGE_SIZE - 1);
    uint32_t room = USER_STACK_BASE - VM_MMAP_BASE;

    if (limit <= PAGE_SIZE) {
        return 0;
    }
    limit -= PAGE_SIZE;
    return limit < room ? limit : room;
}

/**
 * Replace the current process image with a new program (execve(2)).
 *
 * EBX = path, ECX = argv[], EDX = envp[]. Does not return on success. Copies
 * the path, argv, and envp into the process's exec workspace, allocated at
 * its first exec, resolves a `#!` script to its interpreter, and validates
 * the ELF header BEFORE tearing the old image down (so a format the loader
 * does not take leaves the caller intact), then frees the old user pages,
 * streams the new program in, builds the SysV/i386
 * initial stack, and redirects the iret frame to the entry point. A failure
 * after the old image is gone, a segment that does not load, routes to
 * exec_fail_destroyed, which ends the process.
 */
int32_t sys_execve(isr_params_t *params)
{
    const char __user *user_path = (const char __user *)params->ebx;
    char *const __user *user_argv = (char *const __user *)params->ecx;
    char *const __user *user_envp = (char *const __user *)params->edx;
    struct exec_krffs_ctx kctx;

    int argc = 0;
    int envc = 0;
    uint32_t argv_offset = 0;
    uint32_t envp_offset = 0;
    int owned_console = 0;
    uint32_t setid_mode = 0;
    uint32_t file_uid = 0;
    uint32_t file_gid = 0;
    int secure = 0;
    uint32_t old_euid;
    uint32_t old_egid;

    struct exec_state *s;
    uint32_t entry_point;

    elf_image_t img;
    uint8_t random16[16];
    page_dir_t *proc_dir;
    uintptr_t stack_phys;
    void *stack_page;
    const char *name;
    int len;

    mem_balance();
    if (!current_proc) {
        return -ESRCH;
    }
    /* This caller's workspace, by its place in the process table: what it
     * puts there is read again after a sleep, and another exec must not be
     * able to reach it. */
    s = exec_state_of(current_proc);
    if (!s) {
        return -ENOMEM;
    }
    k_memset(s, 0, sizeof(*s));

    /* Resolve the program path (relative names against the cwd) into that
     * workspace. A bad user pointer, an over-long path, or an empty path is
     * rejected by the return value, so a kernel-range pointer cannot leak
     * kernel memory. The buffer is VFS_PATH_MAX bytes (NUL included). */
    if (!user_path) {
        return -EFAULT;
    }
    len = copy_path_cwd(s->path, sizeof(s->path), user_path);
    if (len < 0) {
        return len;
    }

    /* Copy argv from user space, range-checking every user pointer and bounding
     * the cumulative offset so s->argv_storage cannot overflow. An argument
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

            if (copy_from_user(&arg_ptr, (const void __user *)&user_argv[argc], sizeof(arg_ptr)) !=
                0) {
                return -EFAULT;
            }
            if (arg_ptr == 0) {
                break; /* NULL terminator */
            }
            /* The storage is exactly full and another argument is waiting:
             * that is a size limit reached, not a bad address. */
            if (argv_offset >= sizeof(s->argv_storage)) {
                return -E2BIG;
            }
            s->argv_bufs[argc] = &s->argv_storage[argv_offset];
            r = strncpy_from_user(&s->argv_storage[argv_offset], (const char __user *)arg_ptr,
                                  sizeof(s->argv_storage) - argv_offset);
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
            if (copy_from_user(&arg_ptr, (const void __user *)&user_argv[EXEC_MAX_ENTRIES],
                               sizeof(arg_ptr)) != 0) {
                return -EFAULT;
            }
            if (arg_ptr != 0) {
                return -E2BIG;
            }
        }
    }
    s->argv_bufs[argc] = NULL;

    if (user_envp) {
        for (envc = 0; envc < EXEC_MAX_ENTRIES; envc++) {
            uint32_t env_ptr = 0;
            int r;

            if (copy_from_user(&env_ptr, (const void __user *)&user_envp[envc], sizeof(env_ptr)) !=
                0) {
                return -EFAULT;
            }
            if (env_ptr == 0) {
                break; /* NULL terminator */
            }
            if (envp_offset >= sizeof(s->envp_storage)) {
                return -E2BIG;
            }
            s->envp_bufs[envc] = &s->envp_storage[envp_offset];
            r = strncpy_from_user(&s->envp_storage[envp_offset], (const char __user *)env_ptr,
                                  sizeof(s->envp_storage) - envp_offset);
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
            if (copy_from_user(&env_ptr, (const void __user *)&user_envp[EXEC_MAX_ENTRIES],
                               sizeof(env_ptr)) != 0) {
                return -EFAULT;
            }
            if (env_ptr != 0) {
                return -E2BIG;
            }
        }
    }
    s->envp_bufs[envc] = NULL;

    /* The two vectors share the page they are laid into, so they share a
     * budget, and it is checked HERE: `build_user_stack` would catch it, but it
     * runs after `free_user_pages`, where there is no image left to return an
     * error to and the caller is killed instead. */
    if (argv_offset + envp_offset > EXEC_MAX_STRINGS) {
        return -E2BIG;
    }

    name = s->path;

    kctx.name = name;
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

        rc = vfs_resolve(0, name, VFS_WALK_LOCK, &xref, 0, 0);
        if (rc != 0) {
            return rc; /* missing, not-a-dir, or a denied ancestor */
        }
        if (vfs_synth_vpath(xref.mnt, xref.node)) {
            vfs_walk_unlock(xref.mnt);
            vfs_ref_release(&xref);
            return -EACCES;
        }
        /* MS_NOEXEC is enforced here. */
        if (xref.mnt->flags & VFS_MS_NOEXEC) {
            vfs_walk_unlock(xref.mnt);
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
                vfs_walk_unlock(xref.mnt);
                vfs_ref_release(&xref);
                return rc;
            }
            setid_mode = vst.mode & (KRFFS_MODE_SUID | KRFFS_MODE_SGID);
            if (!(vst.mode & 010u)) {
                setid_mode &=
                    ~KRFFS_MODE_SGID; /* set-gid without group execute is mandatory locking */
            }
            if (xref.mnt->flags & VFS_MS_NOSUID) {
                setid_mode = 0;
            }
            file_uid = vst.uid;
            file_gid = vst.gid;
            s->vfs.ref = xref; /* held until the image is loaded or refused */
            s->vfs.err = 0;
            rc = vfs_open_node(xref.mnt, xref.node, 0, &s->vfs.handle);
            vfs_walk_unlock(xref.mnt);
            if (rc != 0) {
                vfs_ref_release(&s->vfs.ref);
                s->vfs.ref.mnt = NULL;
                return rc;
            }
            goto checked;
        }
        prog = xref.node;
        rc = cred_access_node(prog, CRED_X, 0);
        if (rc != 0) {
            vfs_walk_unlock(xref.mnt);
            vfs_ref_release(&xref);
            return rc; /* the execute bit, for root too */
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
        setid_mode = (uint32_t)(prog->mode & KRFFS_MODE_SUID);
        if ((prog->mode & KRFFS_MODE_SGID) && (prog->mode & 0010u)) {
            setid_mode |= KRFFS_MODE_SGID;
        }
        /* MS_NOSUID: the bits are read and then ignored. */
        if (xref.mnt->flags & VFS_MS_NOSUID) {
            setid_mode = 0;
        }
        file_uid = prog->uid;
        file_gid = prog->gid;
        vfs_walk_unlock(xref.mnt);
        vfs_ref_release(&xref);
    }
checked:

    if (current_proc->cr3 == 0) {
        proc_dir = _kernel_dir;
    } else {
        proc_dir = current_proc->page_dir;
    }

    /* The ELF header is validated before the old image is torn down, so the
     * common "not a valid executable" failure leaves the calling process
     * intact. A file that is not an ELF may still be a script naming its
     * interpreter, which is checked here as the script was. */
    {
        Elf32_Ehdr ehdr;

        int script = 0;

        if (s->vfs.ref.mnt) {
            /* On another type the image is read through its mount; a
             * script there is read the same way, and its interpreter is
             * resolved below as any program is. */
            if (exec_vfs_read(&s->vfs, &ehdr, sizeof(ehdr), 0) != (int)sizeof(ehdr) ||
                !elf_valid_exec(&ehdr, sizeof(ehdr))) {
                int32_t sb =
                    s->vfs.err ? s->vfs.err : exec_shebang(s, &argc, exec_vfs_read, &s->vfs);

                exec_vfs_done(&s->vfs);
                if (sb < 0) {
                    return sb;
                }
                if (sb == 0) {
                    return -ENOEXEC;
                }
                script = 1;
            }
        } else if (exec_krffs_read(&kctx, &ehdr, sizeof(ehdr), 0) != (int)sizeof(ehdr) ||
                   !elf_valid_exec(&ehdr, sizeof(ehdr))) {
            struct exec_krffs_ctx sctx;
            int32_t sb;

            sctx.name = s->path + (s->path[0] == '/' ? 1 : 0);
            sb = exec_shebang(s, &argc, exec_krffs_read, &sctx);

            if (sb < 0) {
                return sb;
            }
            if (sb == 0) {
                return -ENOEXEC;
            }
            script = 1;
        }
        if (script) {
            /* A set-id script runs as its invoker. The interpreter is what
             * actually runs, it is not set-id, and honouring the script's bits
             * is a hole Unix closed long ago: the interpreter can be made to
             * read a different file than the one whose bits were checked. */
            setid_mode = 0;
            file_uid = 0;
            file_gid = 0;

            name = s->path;

            kctx.name = name;
            if (name[0] == '/') {
                name++;
            }
            {
                struct vfs_ref iref;
                int32_t rc;

                rc = vfs_resolve(0, name, VFS_WALK_LOCK, &iref, 0, 0);
                if (rc != 0) {
                    return rc;
                }
                if (vfs_synth_vpath(iref.mnt, iref.node) || (iref.mnt->flags & VFS_MS_NOEXEC)) {
                    vfs_walk_unlock(iref.mnt);
                    vfs_ref_release(&iref);
                    return -EACCES;
                }
                if (!mnt_is_root(iref.mnt)) {
                    /* An interpreter on another type loads through its
                     * mount, as a program there does. */
                    rc = cred_access_ref(&iref, CRED_X, 0);
                    if (rc != 0) {
                        vfs_walk_unlock(iref.mnt);
                        vfs_ref_release(&iref);
                        return rc;
                    }
                    s->vfs.ref = iref;
                    s->vfs.err = 0;
                    rc = vfs_open_node(iref.mnt, iref.node, 0, &s->vfs.handle);
                    vfs_walk_unlock(iref.mnt);
                    if (rc != 0) {
                        vfs_ref_release(&s->vfs.ref);
                        s->vfs.ref.mnt = NULL;
                        return rc;
                    }
                    if (exec_vfs_read(&s->vfs, &ehdr, sizeof(ehdr), 0) != (int)sizeof(ehdr) ||
                        !elf_valid_exec(&ehdr, sizeof(ehdr))) {
                        int32_t why = s->vfs.err ? s->vfs.err : -ENOEXEC;

                        exec_vfs_done(&s->vfs);
                        return why; /* one level only: an interpreter that is a script */
                    }
                    goto interpreter_checked;
                }
                rc = cred_access_node((krffs_node_t *)iref.node, CRED_X, 0);
                vfs_walk_unlock(iref.mnt);
                vfs_ref_release(&iref);
                if (rc != 0) {
                    return rc;
                }
            }
            if (exec_krffs_read(&kctx, &ehdr, sizeof(ehdr), 0) != (int)sizeof(ehdr) ||
                !elf_valid_exec(&ehdr, sizeof(ehdr))) {
                /* One level only: an interpreter that is itself a script. */
                return -ENOEXEC;
            }
        interpreter_checked:;
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
     * stack has no AT_UID/AT_EUID for libc to compare. A process that set
     * no_new_privs gains nothing: its bits are read and ignored, as a
     * nosuid mount's are. An exec that runs with ids its caller lacks also
     * lowers a soft RLIMIT_STACK above 8 MiB to 8 MiB, as Linux's does. */
    if (current_proc->no_new_privs) {
        setid_mode = 0;
    }
    old_euid = current_proc->euid;
    old_egid = current_proc->egid;
    if (setid_mode & KRFFS_MODE_SUID) {
        current_proc->euid = file_uid;
    }
    if (setid_mode & KRFFS_MODE_SGID) {
        current_proc->egid = file_gid;
    }
    current_proc->suid = current_proc->euid;
    current_proc->sgid = current_proc->egid;
    secure =
        (current_proc->euid != current_proc->ruid) || (current_proc->egid != current_proc->rgid);
    exec_privileges(secure, old_euid, old_egid);

    /* Record the program's basename as comm for /proc/<pid>/stat and ps. */
    proc_set_comm(current_proc, name);

    /* Reset caught signal dispositions to default: their handler addresses
     * point into the OLD image that is about to be replaced. SIG_IGN
     * dispositions, the blocked mask and the pending signals are preserved
     * (POSIX exec semantics). The new image has named no address for its
     * thread's end yet.
     */
    {
        int sig;
        for (sig = 1; sig <= KSIG_MAX; sig++) {
            if (current_proc->sigacts[sig].handler != KSIG_DFL &&
                current_proc->sigacts[sig].handler != KSIG_IGN) {
                k_memset(&current_proc->sigacts[sig], 0, sizeof(struct k_sigaction));
            }
        }
    }
    current_proc->clear_child_tid = 0;
    current_proc->altstack_on = 0;
    current_proc->altstack_sp = 0;
    current_proc->altstack_size = 0;

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

    if (elf_load_image(s->vfs.ref.mnt ? exec_vfs_read : exec_krffs_read,
                       s->vfs.ref.mnt ? (void *)&s->vfs : (void *)&kctx, proc_dir, &img) != 0) {
        exec_vfs_done(&s->vfs);
        exec_fail_destroyed(1);
    }
    exec_vfs_done(&s->vfs);
    entry_point = img.entry;

    stack_phys = alloc_user_page(proc_dir, USER_STACK_BASE);
    if (stack_phys == 0) {
        exec_fail_destroyed(2);
    }

    stack_page = kmap_temp(stack_phys);
    if (!stack_page) {
        exec_fail_destroyed(3); /* the old image is gone; nothing to return to */
    }
    k_memset(stack_page, 0, PAGE_SIZE);

    /* Build the SysV/i386 initial stack (argc, the argv and real envp pointer
     * arrays, and the auxv) in the freshly zeroed page. build_user_stack copies
     * the strings in and 16-byte-aligns the entry ESP; the layout is verified
     * host-side (tests/host/uimage_hosttest.c). */
    krandom_bytes(random16, 16);
    {
        uint32_t user_esp = build_user_stack(stack_page, USER_STACK_BASE, PAGE_SIZE, argc,
                                             s->argv_bufs, s->envp_bufs, &img, random16, secure);

        kunmap_temp(stack_page);

        if (user_esp == 0) {
            exec_fail_destroyed(4);
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
        /* A fresh image starts with the initial floating-point state, in the
         * registers now and in the record for the next switch. */
        fpu_init_area(current_proc->fpu);
        fpu_restore(current_proc->fpu);

        /* Build the address-space map (VMAs) for the new image so the
         * page-fault handler can demand-page, grow the stack, or segfault. The
         * ELF code/data pages themselves are still eagerly loaded above. */
        if (elf_setup_vmas(&current_proc->vm, &img, USER_STACK_BASE, exec_stack_window()) != 0) {
            exec_fail_destroyed(5); /* the regions could not be made */
        }
        /* RLIMIT_AS: an image whose regions cover more than the limit does
         * not run; the old image is gone by now, so the process ends. */
        if (vm_total(&current_proc->vm) > current_proc->rlim[RLIM_AS].cur) {
            klog(KLOG_WARN, "exec", "refused: the image is past RLIMIT_AS, pid %d",
                 (int)current_proc->pid);
            exec_fail_destroyed(6);
        }
        /* The new image may report a full descriptor table once of its own. */
        current_proc->vma_reported = 0;
    }

    /* Reload CR3 to flush the whole TLB: after freeing the old user pages and
     * mapping new stack/code pages, the TLB may hold stale entries from the
     * replaced image. */
    __asm__ __volatile__("movl %%cr3, %%eax\n\t"
                         "movl %%eax, %%cr3" ::
                             : "eax", "memory");

    /* Exec succeeded: re-publish the controlling terminal we released above,
     * with the unchanged session/group (exec keeps the controlling terminal),
     * so the new image owns the console exactly as the old one did. */
    if (owned_console) {
        tty_set_session(&console_tty, (int)current_proc->sid, (int)current_proc->pgid);
    }

    return 0; /* unused: iret resumes at the new entry point */
}

/**
 * Create a child process: fork(2), and the part of clone(2) this kernel has.
 *
 * Clones the current process: PCB, page directory (copy-on-write user
 * pages), and fd table. The child is set up to return from fork with value
 * 0; the parent gets the child's PID. Returns -errno on failure. A nonzero
 * `newsp` is the stack pointer the child starts with. A nonzero
 * `child_settid` is an address of the child's memory that gets the child's
 * id before it runs (CLONE_CHILD_SETTID), and `child_cleartid` the address
 * its thread end clears (CLONE_CHILD_CLEARTID).
 *
 * Among what the child inherits is the program name, which `execve` sets and a
 * child that never execs would otherwise never have: a daemon that forks
 * itself into the background is exactly such a child, and without this it
 * shows in `ps` and in /proc/<pid>/stat with no name for as long as it runs.
 */
static int32_t do_fork(isr_params_t *params, uint32_t newsp, uint32_t child_settid,
                       uint32_t child_cleartid)
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
    mem_balance();
    /* RLIMIT_NPROC: the processes of the parent's real user, this one
     * included, against the parent's limit; root is not limited. Counted by
     * the real uid at the time, so a process that took a new identity with
     * setuid counts for that user, and counted after the reclaim above,
     * which may sleep, so two forks cannot both see the last place. */
    if (parent->ruid != 0 && parent->rlim[RLIM_NPROC].cur != RLIM_INFINITY32) {
        process_t *q;
        uint32_t mine = 0;

        for (q = proc_list; q; q = q->next) {
            if (q->ruid == parent->ruid && q->state != PROC_UNUSED) {
                mine++;
            }
        }
        if (mine >= parent->rlim[RLIM_NPROC].cur) {
            return -EAGAIN;
        }
    }

    child = alloc_process();
    if (!child) {
        return -ENOMEM; /* a record the heap could not give */
    }

    /* alloc_process gave the child its own stdio open-file descriptions;
     * release them now (before any failure path or the fd-table copy below
     * overwrites the table), or those OFDs would be orphaned and leak the pool
     * on every fork. */
    release_fd_table(&child->fds);
    fd_table_free(&child->fds);

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
    child->page_dir = child_dir;
    child->parent_pid = parent->pid;

    /* Inherit the parent's session and process group: a forked child stays in
     * the parent's job-control group until it (or the parent) calls
     * setsid/setpgid. alloc_process defaulted these to the child's own pid. */
    child->sid = parent->sid;
    child->pgid = parent->pgid;
    child->umask = parent->umask; /* the file-creation mask is inherited */
    child->nice = parent->nice;
    child->dumpable = parent->dumpable;
    child->no_new_privs = parent->no_new_privs;
    k_memcpy(child->rlim, parent->rlim, sizeof(child->rlim));
    child->fds.limit = parent->fds.limit;
    child->ruid = parent->ruid; /* credentials: the child is who the parent was */
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
    /* The floating-point registers as they are now, since the parent's record
     * holds only what they were at its last switch away. */
    fpu_save(child->fpu);

    /* The child's address space is a copy of the parent's at the same virtual
     * addresses (copy_user_pages duplicated the pages), so copy the VMA map too
     * otherwise the child would have no regions and the first heap/stack
     * fault would segfault it. (k_memcpy, not struct assignment, so the
     * freestanding kernel does not need a compiler-emitted memcpy.) */
    if (vm_copy(&child->vm, &parent->vm) != 0) {
        goto fail_built;
    }

    /* The child inherits the parent's current working directory. */
    if (proc_cwd_copy(child, parent) != 0) {
        goto fail_built;
    }

    /* Signals: the child inherits the dispositions and the blocked mask, but
     * its pending set starts empty (POSIX fork semantics). */
    k_memcpy(child->sigacts, parent->sigacts, sizeof(child->sigacts));
    child->sig_blocked = parent->sig_blocked;
    child->sig_pending = 0;
    child->altstack_sp = parent->altstack_sp;
    child->altstack_size = parent->altstack_size;
    child->altstack_on = parent->altstack_on;

    /* Copy file descriptor table. The child shares the parent's open-file
     * descriptions (so a seek in one is seen by the other, per POSIX), so each
     * inherited OFD gains a reference; inherited sockets gain one too, so the
     * parent and child release independently. */
    if (fd_table_copy(&child->fds, &parent->fds) != 0) {
        goto fail_built;
    }
    {
        int fdi;
        for (fdi = 0; fdi < fd_table_cap(&child->fds); fdi++) {
            if (child->fds.entries[fdi].type == FD_TYPE_NONE) {
                continue;
            }
            fd_ofd_ref(child->fds.entries[fdi].ofd);
            if (child->fds.entries[fdi].type == FD_TYPE_SOCKET && child->fds.entries[fdi].sock) {
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
    if (newsp) {
        child_frame->user_esp = newsp;
    }
    /* A failed store leaves the word as the parent had it, as Linux's does. */
    if (child_settid) {
        (void)paging_store_u32(child_dir, &child->vm, child_settid, child->pid);
    }
    child->clear_child_tid = child_cleartid;

    /* The switch_context return address, then its saved EBP/EBX/ESI/EDI. */
    --child_sp;
    *child_sp = (uint32_t)isr_return;
    --child_sp;
    *child_sp = 0; /* EBP */
    --child_sp;
    *child_sp = 0; /* EBX */
    --child_sp;
    *child_sp = 0; /* ESI */
    --child_sp;
    *child_sp = 0; /* EDI */

    child->regs.esp = (uint32_t)child_sp;
    child->state = PROC_READY;

    return (int32_t)child->pid;

fail_built:
    /* Everything built for the child so far goes back: the working
     * directory's hold, the kernel stack, the copied pages and the
     * directory, then the record. */
    vfs_cwd_clear(child);
    kstack_free(child->kernel_stack);
    free_user_pages(child_dir);
    free_page_directory(child_dir);
    free_process(child);
    return -ENOMEM;
}

int32_t sys_fork(isr_params_t *params)
{
    return do_fork(params, 0, 0, 0);
}

/* clone(2) flags; the low byte is the signal the parent gets at the child's
 * exit. */
#define CSIGNAL              0x000000FFu
#define CLONE_PARENT_SETTID  0x00100000u
#define CLONE_CHILD_CLEARTID 0x00200000u
#define CLONE_DETACHED       0x00400000u
#define CLONE_CHILD_SETTID   0x01000000u

/**
 * clone (120), in the form glibc's fork uses: a new process with its own copy
 * of the address space, files and signal handlers, as fork makes one.
 *
 * i386 argument order: EBX=flags, ECX=new stack or 0, EDX=parent tid address,
 * ESI=TLS descriptor, EDI=child tid address. The exit signal must be SIGCHLD,
 * and the flags other than CLONE_PARENT_SETTID, CLONE_CHILD_SETTID,
 * CLONE_CHILD_CLEARTID and CLONE_DETACHED, which Linux ignores, are refused
 * with EINVAL: sharing memory, files or handlers is a later phase's work.
 * CLONE_PARENT_SETTID stores the child's id in the parent's memory, and a
 * failed store is ignored, as Linux ignores it.
 */
int32_t sys_clone(isr_params_t *params)
{
    uint32_t flags = params->ebx;
    uint32_t newsp = params->ecx;
    int32_t __user *ptid = (int32_t __user *)params->edx;
    uint32_t ctid = params->edi;
    int32_t pid;

    if ((flags & CSIGNAL) != SIGCHLD ||
        (flags & ~(CSIGNAL | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID |
                   CLONE_DETACHED)) != 0) {
        return -EINVAL;
    }
    pid = do_fork(params, newsp, (flags & CLONE_CHILD_SETTID) ? ctid : 0,
                  (flags & CLONE_CHILD_CLEARTID) ? ctid : 0);
    if (pid > 0 && (flags & CLONE_PARENT_SETTID) && ptid) {
        (void)copy_to_user(ptid, &pid, sizeof(pid));
    }
    return pid;
}

/* The options of waitpid, wait4 and waitid. */
#define WNOHANG 1

#define WUNTRACED 2 /* also report a child that has STOPPED; waitid's WSTOPPED */

#define WEXITED 4 /* report a child that has exited, which wait4 always does */

#define WCONTINUED 8 /* also report a child that has CONTINUED */

#define WNOWAIT 0x01000000 /* waitid: report the event and leave it */

#define W_NOTHREAD 0x20000000 /* Linux's __WNOTHREAD */

#define W_ALL 0x40000000 /* Linux's __WALL */

#define W_CLONE 0x80000000u /* Linux's __WCLONE */

/* The children a wait selects, as waitid's idtype names them: every child,
 * the child with an id, or the children in a process group. A pidfd names
 * no child here, since no descriptor is one. */
#define P_ALL 0

#define P_PID 1

#define P_PGID 2

#define P_PIDFD 3

/* The si_code of the SIGCHLD siginfo waitid fills. */
#define CLD_EXITED 1

#define CLD_KILLED 2

#define CLD_DUMPED 3

#define CLD_STOPPED 5

#define CLD_CONTINUED 6

/* The i386 struct rusage, 72 bytes: the user and system CPU time as two
 * timevals of 32-bit seconds and microseconds, then fourteen longs, maxrss
 * through nivcsw, which this kernel does not count and reports as zero. */
struct kernel_rusage {
    int32_t utime_sec;
    int32_t utime_usec;
    int32_t stime_sec;
    int32_t stime_usec;
    int32_t uncounted[14];
};

#define RUSAGE_BYTES 72

/** Fill `ru` with `user` and `sys` ticks of CPU time and zero the rest. */
static void rusage_from_ticks(struct kernel_rusage *ru, uint32_t user, uint32_t sys)
{
    k_memset(ru, 0, sizeof(*ru));
    ru->utime_sec = (int32_t)(user / 100u);
    ru->utime_usec = (int32_t)((user % 100u) * 10000u);
    ru->stime_sec = (int32_t)(sys / 100u);
    ru->stime_usec = (int32_t)((sys % 100u) * 10000u);
}

/**
 * What a wait reports of a child's usage, as Linux's `RUSAGE_BOTH` has it:
 * the child's own CPU time with that of the children it has reaped. Read
 * with interrupts masked, since the tick adds to a live child's.
 */
static void rusage_of_child(struct kernel_rusage *ru, const process_t *p)
{
    uint32_t flags = kirq_save();
    uint32_t user = p->utime + p->cutime;
    uint32_t sys = p->stime + p->cstime;

    kirq_restore(flags);
    rusage_from_ticks(ru, user, sys);
}

/* What a wait found: the child, its real uid, the wait status wait4 stores,
 * the si_code and si_status waitid stores, and the child's usage. */
struct wait_report {
    uint32_t pid;
    uint32_t uid;
    int32_t status;
    int32_t code;
    int32_t info_status;
    struct kernel_rusage ru;
};

/* Interrupts-off helpers (defined below with the signal code); used here for
 * the lost-wakeup-safe sleep in wait_find. */
static uint32_t sig_irq_save(void);

static void sig_irq_restore(uint32_t flags);

/**
 * Whether `p` is a child of the caller that `idtype` and `id` select.
 *
 * Linux's __WCLONE asks for the children that report their end with a signal
 * other than SIGCHLD, unless __WALL is beside it, and every child here
 * reports with SIGCHLD, so it selects none.
 */
static int wait_selects(int idtype, uint32_t id, int options, const process_t *p)
{
    if (p->parent_pid != current_proc->pid) {
        return 0;
    }
    if (((uint32_t)options & W_CLONE) && !(options & W_ALL)) {
        return 0;
    }
    switch (idtype) {
    case P_PID:
        return p->pid == id;
    case P_PGID:
        return p->pgid == id;
    default:
        return 1;
    }
}

/** Fill `r` with the report of child `p` and the `code` and `info_status`
 * a waitid gives it. */
static void wait_fill(struct wait_report *r, const process_t *p, int32_t status, int32_t code,
                      int32_t info_status)
{
    r->pid = p->pid;
    r->uid = p->ruid;
    r->status = status;
    r->code = code;
    r->info_status = info_status;
    rusage_of_child(&r->ru, p);
}

/**
 * Reap zombie `p`: add its CPU time, with its reaped children's, to the
 * caller's children's totals, and give back its pages, page directory,
 * kernel stack and record. The zombie no longer runs on its kernel stack,
 * and this is the single release point for a per-fork stack.
 */
static void wait_reap(process_t *p)
{
    current_proc->cutime += p->utime + p->cutime;
    current_proc->cstime += p->stime + p->cstime;
    if (p->cr3 != 0) {
        page_dir_t *child_dir = p->page_dir;
        free_user_pages(child_dir);
        free_page_directory(child_dir);
    }
    kstack_free(p->kernel_stack);
    free_process(p);
}

/**
 * The shared core of waitpid, wait4 and waitid: find a child `idtype` and
 * `id` select that has an event `options` asks for, fill `out` with it and
 * return its pid.
 *
 * The events are an exit (WEXITED), which reaps the zombie; a stop
 * (WUNTRACED), reported once, while the child is stopped; and a continue
 * (WCONTINUED), reported once. With WNOWAIT a zombie is left to be reaped and
 * a stop or a continue to be reported again. A zombie that is not waited for
 * as one may still report a continue it has not reported, as Linux lets it.
 * The exit's si_code is CLD_EXITED with the exit status, or CLD_KILLED or
 * CLD_DUMPED with the signal; a stop's is CLD_STOPPED with the stop signal,
 * and a continue's CLD_CONTINUED with SIGCONT.
 *
 * Returns the pid; 0 with WNOHANG when a selected child has nothing to
 * report; -ECHILD when no child is selected; or -EINTR when a signal the
 * caller acts on arrives while it waits.
 */
static int32_t wait_find(int idtype, uint32_t id, int options, struct wait_report *out)
{
    process_t *p;
    uint32_t flags;

    while (1) {
        int have_child = 0;

        /* Scan ALL selected children for a reportable event (a zombie to
         * reap, or a stop/continue to report), not just the first. */
        for (p = proc_list; p; p = p->next) {
            if (!wait_selects(idtype, id, options, p)) {
                continue;
            }
            have_child = 1;

            if (p->state == PROC_ZOMBIE && (options & WEXITED)) {
                int32_t status = p->exit_code;

                if ((status & 0x7f) == 0) {
                    wait_fill(out, p, status, CLD_EXITED, (status >> 8) & 0xff);
                } else {
                    wait_fill(out, p, status, (status & 0x80) ? CLD_DUMPED : CLD_KILLED,
                              status & 0x7f);
                }
                if (!(options & WNOWAIT)) {
                    wait_reap(p);
                }
                return (int32_t)out->pid;
            }
            if ((options & WUNTRACED) && p->state == PROC_STOPPED &&
                (p->jc_status & 0xff) == 0x7f) {
                wait_fill(out, p, (int32_t)p->jc_status, CLD_STOPPED,
                          (int32_t)((p->jc_status >> 8) & 0xff));
                if (!(options & WNOWAIT)) {
                    p->jc_status = 0;
                }
                return (int32_t)out->pid;
            }
            if ((options & WCONTINUED) && p->jc_status == 0xffff) {
                wait_fill(out, p, 0xffff, CLD_CONTINUED, SIGCONT);
                if (!(options & WNOWAIT)) {
                    p->jc_status = 0;
                }
                return (int32_t)out->pid;
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

/**
 * Shared core of waitpid(7) and wait4(114).
 *
 * `pid` selects as Linux's does: a pid, -1 for every child, 0 for the
 * caller's process group, and less than -1 for the group -pid; the most
 * negative int names no group. The options are WNOHANG, WUNTRACED and
 * WCONTINUED, with Linux's three thread bits, and an exit is always waited
 * for; the other bits are ignored. `status` (may be NULL) receives the
 * Linux-encoded wait status that sys_exit/terminate already stored in
 * exit_code; `rusage_ptr` (0 for waitpid, may be 0 for wait4) receives the
 * child's CPU time with its reaped children's, for a stop or a continue as
 * for an exit. Both are faulted in for writing before any child is reaped,
 * so a bad address answers -EFAULT instead of losing the zombie.
 */
static int32_t do_wait(int pid, int __user *status, int options, uint32_t rusage_ptr)
{
    struct wait_report r;
    int idtype = P_PGID;
    uint32_t id;
    int32_t rc;

    if (!current_proc) {
        return -ESRCH;
    }
    if (status && !user_buffer_ok((uint32_t)(__force uintptr_t)status, sizeof(int), 1)) {
        return -EFAULT;
    }
    if (rusage_ptr && !user_buffer_ok(rusage_ptr, RUSAGE_BYTES, 1)) {
        return -EFAULT;
    }
    if (pid > 0) {
        idtype = P_PID;
        id = (uint32_t)pid;
    } else if (pid == -1) {
        idtype = P_ALL;
        id = 0;
    } else if (pid == 0) {
        id = current_proc->pgid;
    } else {
        id = 0u - (uint32_t)pid;
    }
    options &= (int)(WNOHANG | WUNTRACED | WCONTINUED | W_NOTHREAD | W_ALL | W_CLONE);
    rc = wait_find(idtype, id, options | WEXITED, &r);
    if (rc <= 0) {
        return rc;
    }
    if (status) {
        copy_to_user((void __user *)status, &r.status, sizeof(r.status));
    }
    if (rusage_ptr) {
        copy_to_user((void __user *)rusage_ptr, &r.ru, RUSAGE_BYTES);
    }
    return rc;
}

/*
 * sys_waitpid - wait for a child to change state.
 * Args: EBX = pid (-1 = any child), ECX = status pointer, EDX = options.
 * Returns the PID of the child reaped, or reported stopped or continued,
 * 0 (WNOHANG, none ready), or -errno.
 */
int32_t sys_waitpid(isr_params_t *params)
{
    return do_wait((int)params->ebx, (int __user *)params->ecx, (int)params->edx, 0);
}

/*
 * sys_wait4 - like waitpid plus an optional rusage out-pointer, which
 * receives the child's CPU time. Args: EBX = pid, ECX = status, EDX =
 * options, ESI = rusage. musl's wait/waitpid are implemented on top of this.
 */
int32_t sys_wait4(isr_params_t *params)
{
    return do_wait((int)params->ebx, (int __user *)params->ecx, (int)params->edx, params->esi);
}

/* The six fields of the SIGCHLD siginfo_t that waitid writes, at the start
 * of the 128-byte i386 structure. */
struct kernel_chld_info {
    int32_t signo;
    int32_t err;
    int32_t code;
    int32_t pid;
    int32_t uid;
    int32_t status;
};

/**
 * Wait for a child to change state (waitid(2), syscall 284).
 *
 * EBX = idtype, ECX = id, EDX = siginfo_t *, ESI = options, EDI = struct
 * rusage *, Linux's fifth argument. P_ALL selects every child, P_PID the
 * child with a positive id, and P_PGID the group with a nonnegative id, 0
 * being the caller's. At least one of WEXITED, WSTOPPED and WCONTINUED must
 * be given, beside which WNOHANG, WNOWAIT and Linux's three thread bits are
 * accepted; `wait_find` has what each means.
 *
 * Returns 0 when a child was reported, with the siginfo's si_signo SIGCHLD,
 * si_code, si_pid, si_uid (the child's real uid) and si_status filled and
 * the child's usage in the rusage; and 0 with WNOHANG when no selected child
 * has an event, with those siginfo fields zero and the rusage untouched.
 * Only those six fields are written. -EINVAL for an unknown option or idtype,
 * no event asked for, or an id that does not suit its idtype; -EBADF for a
 * pidfd, since no descriptor is one; -ECHILD, -EINTR; -EFAULT, found before
 * any child is reaped, as `do_wait` finds it.
 */
int32_t sys_waitid(isr_params_t *params)
{
    int idtype = (int)params->ebx;
    int32_t upid = (int32_t)params->ecx;
    uint32_t uinfo = params->edx;
    int options = (int)params->esi;
    uint32_t rusage_ptr = params->edi;
    struct kernel_chld_info info;
    struct wait_report r;
    uint32_t id = 0;
    int32_t rc;

    if ((uint32_t)options & ~(WNOHANG | WNOWAIT | WEXITED | WUNTRACED | WCONTINUED | W_NOTHREAD |
                              W_ALL | W_CLONE)) {
        return -EINVAL;
    }
    if (!(options & (WEXITED | WUNTRACED | WCONTINUED))) {
        return -EINVAL;
    }
    switch (idtype) {
    case P_ALL:
        break;
    case P_PID:
        if (upid <= 0) {
            return -EINVAL;
        }
        id = (uint32_t)upid;
        break;
    case P_PGID:
        if (upid < 0) {
            return -EINVAL;
        }
        id = upid != 0 ? (uint32_t)upid : current_proc->pgid;
        break;
    case P_PIDFD:
        return upid < 0 ? -EINVAL : -EBADF;
    default:
        return -EINVAL;
    }
    if (uinfo && !user_buffer_ok(uinfo, sizeof(info), 1)) {
        return -EFAULT;
    }
    if (rusage_ptr && !user_buffer_ok(rusage_ptr, RUSAGE_BYTES, 1)) {
        return -EFAULT;
    }
    rc = wait_find(idtype, id, options, &r);
    if (rc < 0) {
        return rc;
    }
    k_memset(&info, 0, sizeof(info));
    if (rc > 0) {
        info.signo = SIGCHLD;
        info.code = r.code;
        info.pid = (int32_t)r.pid;
        info.uid = (int32_t)r.uid;
        info.status = r.info_status;
        if (rusage_ptr) {
            copy_to_user((void __user *)rusage_ptr, &r.ru, RUSAGE_BYTES);
        }
    }
    if (uinfo) {
        copy_to_user((void __user *)uinfo, &info, sizeof(info));
    }
    return 0;
}

/* The targets getrusage(2) takes: the caller, its reaped children, and the
 * calling thread, which is the caller while each process is one thread. */
#define RUSAGE_SELF 0

#define RUSAGE_CHILDREN (-1)

#define RUSAGE_THREAD 1

/**
 * Resource usage (getrusage(2), syscall 77).
 *
 * EBX = who, ECX = the 72-byte i386 struct rusage. `ru_utime` and `ru_stime`
 * are the caller's user and system time for RUSAGE_SELF and RUSAGE_THREAD,
 * and the totals of the children it has reaped, each with the children those
 * reaped, for RUSAGE_CHILDREN: the ticks `times` reports, as timevals. The
 * fourteen counters after them, maximum resident size through context
 * switches, are not kept and are zero. -EINVAL for any other `who`, Linux's
 * RUSAGE_BOTH among them, -EFAULT for a structure that cannot be written.
 */
int32_t sys_getrusage(isr_params_t *params)
{
    int who = (int)params->ebx;
    struct kernel_rusage ru;
    uint32_t flags;
    uint32_t user;
    uint32_t sys;

    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN && who != RUSAGE_THREAD) {
        return -EINVAL;
    }
    flags = kirq_save();
    user = who == RUSAGE_CHILDREN ? current_proc->cutime : current_proc->utime;
    sys = who == RUSAGE_CHILDREN ? current_proc->cstime : current_proc->stime;
    kirq_restore(flags);
    rusage_from_ticks(&ru, user, sys);
    if (copy_to_user((void __user *)params->ecx, &ru, RUSAGE_BYTES) != 0) {
        return -EFAULT;
    }
    return 0;
}

/*
 * Identity (the i386 *32 variants, 199-202): the four getters read the PCB
 * credentials, all zero (root) until a login server drops them. The setters
 * and the group list live with the rest of the id family near do_chown.
 */
int32_t sys_getuid(isr_params_t *params)
{
    (void)params;
    return (int32_t)current_proc->ruid;
}

int32_t sys_geteuid(isr_params_t *params)
{
    (void)params;
    return (int32_t)current_proc->euid;
}

int32_t sys_getgid(isr_params_t *params)
{
    (void)params;
    return (int32_t)current_proc->rgid;
}

int32_t sys_getegid(isr_params_t *params)
{
    (void)params;
    return (int32_t)current_proc->egid;
}

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

/* The signal masks are 64-bit, so a read-modify-write is two stores on i386;
 * signal_post runs from IRQ context (Ctrl-C) as well as from a syscall, so a
 * mutation an interrupt can meet half done is made with interrupts off.
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
 * Post signal `signo` to `target` with the siginfo `info`: set its pending
 * bit, and keep `info` when the signal was not already pending. If the target
 * is blocked in the scheduler (sleeping/waiting), make it runnable and clear
 * its timed wake, whatever the signal's disposition and mask: an
 * interruptible wait then asks `signal_pending` whether to end with -EINTR,
 * and any wait that goes on arms its own deadline again. SIGKILL/SIGSTOP
 * cannot be caught, so a pending SIGKILL is honored even before delivery by
 * leaving it pending for the default path.
 */
void signal_post_info(process_t *target, int signo, const struct k_siginfo *info)
{
    if (!target || signo < 1 || signo > KSIG_MAX) {
        return;
    }
    if (target->state == PROC_ZOMBIE || target->state == PROC_UNUSED) {
        return;
    }
    {
        uint32_t f = sig_irq_save();
        if (!(target->sig_pending & KSIG_BIT(signo))) {
            target->sig_info[signo] = *info;
        }
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
            target->sig_pending &=
                ~(KSIG_BIT(SIGSTOP) | KSIG_BIT(SIGTSTP) | KSIG_BIT(SIGTTIN) | KSIG_BIT(SIGTTOU));
            if (target->state == PROC_STOPPED) {
                process_t *parent = get_process(target->parent_pid);
                target->state = PROC_READY;
                target->jc_status = 0xffff; /* WIFCONTINUED */
                if (parent) {
                    if (!(parent->sig_pending & KSIG_BIT(SIGCHLD))) {
                        parent->sig_info[SIGCHLD].code = CLD_CONTINUED;
                        parent->sig_info[SIGCHLD].pid = target->pid;
                        parent->sig_info[SIGCHLD].uid = target->ruid;
                        parent->sig_info[SIGCHLD].status = SIGCONT;
                        parent->sig_info[SIGCHLD].addr = 0;
                    }
                    parent->sig_pending |= KSIG_BIT(SIGCHLD);
                    if (parent->state == PROC_BLOCKED) {
                        parent->wakeup_tick = 0;
                        parent->state = PROC_READY;
                    }
                }
            }
        } else if (signo == SIGKILL && target->state == PROC_STOPPED) {
            target->state = PROC_READY;
        } else if ((signo == SIGSTOP || signo == SIGTSTP || signo == SIGTTIN || signo == SIGTTOU)) {
            target->sig_pending &= ~KSIG_BIT(SIGCONT); /* a stop cancels pending cont */
        }
        sig_irq_restore(f);
    }
}

void signal_post(process_t *target, int signo)
{
    struct k_siginfo info;

    k_memset(&info, 0, sizeof(info));
    info.code = SI_KERNEL;
    signal_post_info(target, signo, &info);
}

void signal_force(process_t *target, int signo, const struct k_siginfo *info)
{
    struct k_sigaction *sa = &target->sigacts[signo];
    uint32_t f = sig_irq_save();

    if (sa->handler == KSIG_IGN || (target->sig_blocked & KSIG_BIT(signo))) {
        sa->handler = KSIG_DFL;
        target->sig_blocked &= ~KSIG_BIT(signo);
    }
    sig_irq_restore(f);
    signal_post_info(target, signo, info);
}

/* The siginfo of a signal a process sends: `code` SI_USER for kill and
 * SI_TKILL for tkill and tgkill, with the sender's pid and real uid. */
static void sender_info(struct k_siginfo *info, int32_t code)
{
    k_memset(info, 0, sizeof(*info));
    info->code = code;
    info->pid = current_proc ? current_proc->pid : 0;
    info->uid = current_proc ? current_proc->ruid : 0;
}

/* Non-zero if the current process has a pending signal that is neither
 * blocked nor ignored: one that is caught, or whose default action terminates
 * or stops the process. Blocking syscall loops poll this so a signal
 * interrupts the wait (the signal is then delivered/acts on the syscall's
 * return to user). */
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
    /* Only a signal that would ACT interrupts a wait: a caught handler, a
     * default terminate or a default stop. A pending signal that is ignored
     * (SIG_IGN or a default-ignore like SIGCHLD) must NOT cause -EINTR. */
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
    return current_proc->ruid == target->ruid || current_proc->ruid == target->suid ||
           current_proc->euid == target->ruid || current_proc->euid == target->suid;
}

/* Post signo to a process group: pid==0 is the caller's group, pid<-1 the group
 * (-pid). Signals every member the caller may signal; the POSIX outcome: 0 if
 * any member took it, -EPERM if members exist but none may be signalled,
 * -ESRCH if the group is empty. signo 0 only probes. */
static int32_t kill_group(int pgid, int signo)
{
    int found = 0;
    int allowed = 0;
    process_t *p;

    for (p = proc_list; p; p = p->next) {
        if (p->state == PROC_ZOMBIE) {
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
            struct k_siginfo info;

            sender_info(&info, SI_USER);
            signal_post_info(p, signo, &info);
        }
    }
    if (!found) {
        return -ESRCH;
    }
    return allowed ? 0 : -EPERM;
}

/* kill(2): pid > 0 a single process; pid == 0 the caller's process group; pid <
 * -1 the group (-pid). (pid == -1 "all processes" is not supported here.) signo
 * 0 just probes existence and permission. `code` is the siginfo's si_code: a
 * single target gets it, a group SI_USER. */
static int32_t do_kill(int pid, int signo, int32_t code)
{
    process_t *target;
    struct k_siginfo info;

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
    sender_info(&info, code);
    signal_post_info(target, signo, &info);
    return 0;
}

int32_t sys_kill(isr_params_t *params)
{
    return do_kill((int)params->ebx, (int)params->ecx, SI_USER);
}

int32_t sys_tkill(isr_params_t *params)
{
    /* tkill(tid, sig): tid == pid here. A tid must be a positive thread id;
     * unlike kill(2), a zero or negative value is an error, not a group
     * selector, so reject it before do_kill would read it as one. */
    if ((int)params->ebx <= 0) {
        return -EINVAL;
    }
    return do_kill((int)params->ebx, (int)params->ecx, SI_TKILL);
}

int32_t sys_tgkill(isr_params_t *params)
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
    return do_kill(tid, sig, SI_TKILL);
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
int32_t sys_pause(isr_params_t *params)
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
 * with *mask, waits for a deliverable signal, and returns -EINTR. The handler
 * the signal runs runs under *mask, and the frame it gets carries the old
 * mask, which its return restores; without a handler the old mask comes back
 * on the return to user mode.
 */
int32_t sys_rt_sigsuspend(isr_params_t *params)
{
    const void __user *umask = (const void __user *)params->ebx;
    uint32_t sigsetsize = params->ecx;
    uint64_t newmask = 0;

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
    current_proc->saved_sigmask = current_proc->sig_blocked;
    current_proc->sigmask_saved = 1;
    current_proc->sig_blocked = newmask;
    sig_wait();
    return -EINTR;
}

/**
 * Install or query a signal disposition (rt_sigaction, syscall 174).
 *
 * EBX=signo, ECX=const struct k_sigaction* act, EDX=struct k_sigaction* old,
 * ESI=sigsetsize. Stores the action per-signal; SIGKILL/SIGSTOP cannot be
 * changed.
 */
int32_t sys_rt_sigaction(isr_params_t *params)
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
        if (copy_to_user(uold, &current_proc->sigacts[signo], sizeof(struct k_sigaction)) != 0) {
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
int32_t sys_rt_sigprocmask(isr_params_t *params)
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

/**
 * The shared body of sigreturn and rt_sigreturn.
 *
 * The frame the handler returned from gives back the registers, the
 * floating-point state and the mask; a frame that cannot be read or that
 * would load a faulting state kills the process with SIGSEGV. Returns the
 * restored EAX, so the dispatcher's store of the result leaves it as it was.
 */
static int32_t signal_return(isr_params_t *tf, int rt)
{
    uint64_t mask;

    if (!current_proc) {
        return -ESRCH;
    }
    if (sigframe_restore(tf, rt, &mask) != 0) {
        terminate_current_process(SIGSEGV);
        return 0;
    }
    current_proc->sig_blocked = mask & ~KSIG_UNCATCHABLE;
    return (int32_t)tf->eax;
}

/*
 * rt_sigreturn (syscall 173): the return of an SA_SIGINFO handler, through a
 * restorer that issues the call with the stack pointer 4 bytes past the
 * frame's start.
 */
int32_t sys_rt_sigreturn(isr_params_t *tf)
{
    return signal_return(tf, 1);
}

/*
 * sigreturn (syscall 119): the return of a plain handler, through a restorer
 * that pops the signal number first, 8 bytes past the frame's start.
 */
int32_t sys_sigreturn(isr_params_t *tf)
{
    return signal_return(tf, 0);
}

/* The i386 stack_t of sigaltstack. */
struct k_stack {
    uint32_t ss_sp;
    int32_t ss_flags;
    uint32_t ss_size;
};

#define SS_ONSTACK    1
#define SS_DISABLE    2
#define SS_AUTODISARM (1 << 31)
#define MINSIGSTKSZ   2048u

/**
 * sigaltstack (186): set or read the alternate stack a handler with
 * SA_ONSTACK runs on. EBX=new stack or NULL, ECX=old stack or NULL.
 *
 * The old settings are read before the change: SS_DISABLE for none, and
 * SS_ONSTACK while the process runs on the stack, when a change is refused
 * with EPERM. SS_AUTODISARM is accepted and has no effect. A stack smaller
 * than MINSIGSTKSZ is refused with ENOMEM.
 */
int32_t sys_sigaltstack(isr_params_t *params)
{
    const struct k_stack __user *uss = (const struct k_stack __user *)params->ebx;
    struct k_stack __user *uoss = (struct k_stack __user *)params->ecx;
    struct k_stack cur;
    struct k_stack ss;
    int on;
    int32_t flags;

    if (!current_proc) {
        return -ESRCH;
    }
    on = sigframe_on_altstack(params->user_esp);
    cur.ss_sp = current_proc->altstack_sp;
    cur.ss_size = current_proc->altstack_size;
    cur.ss_flags = !current_proc->altstack_on ? SS_DISABLE : (on ? SS_ONSTACK : 0);
    if (uss) {
        if (copy_from_user(&ss, uss, sizeof(ss)) != 0) {
            return -EFAULT;
        }
        if (on) {
            return -EPERM;
        }
        flags = ss.ss_flags & ~SS_AUTODISARM;
        if (flags != 0 && flags != SS_DISABLE && flags != SS_ONSTACK) {
            return -EINVAL;
        }
        if (flags == SS_DISABLE) {
            current_proc->altstack_on = 0;
            current_proc->altstack_sp = 0;
            current_proc->altstack_size = 0;
        } else {
            if (ss.ss_size < MINSIGSTKSZ) {
                return -ENOMEM;
            }
            current_proc->altstack_sp = ss.ss_sp;
            current_proc->altstack_size = ss.ss_size;
            current_proc->altstack_on = 1;
        }
    }
    if (uoss && copy_to_user(uoss, &cur, sizeof(cur)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * rt_sigpending (176): the signals pending while blocked. EBX=set, ECX=size
 * of the set in bytes, at most 8.
 */
int32_t sys_rt_sigpending(isr_params_t *params)
{
    uint32_t size = params->ecx;
    uint32_t words[2];
    uint64_t set;
    uint32_t flags;

    if (!current_proc) {
        return -ESRCH;
    }
    if (size > sizeof(words)) {
        return -EINVAL;
    }
    flags = sig_irq_save();
    set = current_proc->sig_pending & current_proc->sig_blocked;
    sig_irq_restore(flags);
    words[0] = (uint32_t)set;
    words[1] = (uint32_t)(set >> 32);
    if (copy_to_user((void __user *)params->ebx, words, size) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * sigprocmask (126): the old form of rt_sigprocmask, with signals 1 to 32
 * in one word. EBX=how, ECX=new set or NULL, EDX=old set or NULL. SIG_SETMASK
 * replaces signals 1 to 32 and leaves the rest.
 */
int32_t sys_sigprocmask(isr_params_t *params)
{
    int how = (int)params->ebx;
    const uint32_t __user *uset = (const uint32_t __user *)params->ecx;
    uint32_t __user *uold = (uint32_t __user *)params->edx;
    uint32_t old;
    uint32_t set;
    uint64_t mask;

    if (!current_proc) {
        return -ESRCH;
    }
    old = (uint32_t)current_proc->sig_blocked;
    if (uset) {
        if (copy_from_user(&set, uset, sizeof(set)) != 0) {
            return -EFAULT;
        }
        mask = current_proc->sig_blocked;
        switch (how) {
        case 0: /* SIG_BLOCK */
            mask |= set;
            break;
        case 1: /* SIG_UNBLOCK */
            mask &= ~(uint64_t)set;
            break;
        case 2: /* SIG_SETMASK */
            mask = (mask & 0xFFFFFFFF00000000ULL) | set;
            break;
        default:
            return -EINVAL;
        }
        current_proc->sig_blocked = mask & ~KSIG_UNCATCHABLE;
    }
    if (uold && copy_to_user(uold, &old, sizeof(old)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/* Restart the system call the dispatcher recorded: its number back in EAX and
 * the instruction pointer back on its two-byte `int $0x80`. */
static void restart_syscall(isr_params_t *tf, uint32_t restart_nr)
{
    tf->eax = restart_nr - 1u;
    tf->eip -= 2u;
}

/**
 * Act on a pending signal at the return-to-user boundary.
 *
 * Called from the ISR/IRQ return path just before iret, with the trapframe
 * about to be restored. If the current process is returning to USER mode and
 * has a deliverable (pending, not blocked) signal, act on the
 * lowest-numbered one: ignore it, terminate (default), stop, or build a
 * handler frame.
 *
 * A system call the dispatcher found interrupted and restartable is
 * restarted when no handler runs, and when the handler that runs has
 * SA_RESTART; otherwise it keeps its EINTR. A mask rt_sigsuspend, pselect6 or
 * ppoll replaced goes into the handler's frame, or back in place at once when
 * no handler runs.
 */
void deliver_signals(isr_params_t *tf)
{
    uint32_t restart_nr;

    if (!current_proc) {
        return;
    }
    if ((tf->cs & 3) != 3) {
        return; /* not returning to user mode */
    }
    restart_nr = current_proc->restart_nr;
    current_proc->restart_nr = 0;

    for (;;) {
        uint64_t deliverable = current_proc->sig_pending & ~current_proc->sig_blocked;
        struct k_sigaction *sa;
        struct k_siginfo info;
        uint64_t oldmask, newblock;
        uint32_t handler_flags;
        int signo;

        if (deliverable == 0) {
            break;
        }
        for (signo = 1; signo <= KSIG_MAX; signo++) {
            if (deliverable & KSIG_BIT(signo)) {
                break;
            }
        }
        if (signo > KSIG_MAX) {
            break; /* unreachable (deliverable != 0); keeps the shift bounded */
        }
        current_proc->sig_pending &= ~KSIG_BIT(signo);
        info = current_proc->sig_info[signo];
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
                    struct k_siginfo child;

                    k_memset(&child, 0, sizeof(child));
                    child.code = CLD_STOPPED;
                    child.pid = current_proc->pid;
                    child.uid = current_proc->ruid;
                    child.status = signo;
                    signal_post_info(parent, SIGCHLD, &child);
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
        handler_flags = sa->flags;
        if (restart_nr != 0 && (handler_flags & SA_RESTART)) {
            restart_syscall(tf, restart_nr);
        }
        oldmask = current_proc->sigmask_saved ? current_proc->saved_sigmask
                                              : current_proc->sig_blocked;
        if (sigframe_setup(tf, signo, sa, &info, oldmask) != 0) {
            terminate_current_process(SIGSEGV);
            return;
        }
        current_proc->sigmask_saved = 0;
        /* Block the handler's mask, plus this signal unless SA_NODEFER, for
         * the run. */
        newblock = current_proc->sig_blocked | sigact_mask(sa);
        if (!(handler_flags & SA_NODEFER)) {
            newblock |= KSIG_BIT(signo);
        }
        current_proc->sig_blocked = newblock & ~KSIG_UNCATCHABLE;
        /* SA_RESETHAND is for the next signal; this one's handler is in the
         * frame already. */
        if (handler_flags & SA_RESETHAND) {
            sa->handler = KSIG_DFL;
        }
        return;
    }

    if (restart_nr != 0) {
        restart_syscall(tf, restart_nr);
    }
    if (current_proc->sigmask_saved) {
        current_proc->sig_blocked = current_proc->saved_sigmask;
        current_proc->sigmask_saved = 0;
    }
}

/*
 * sys_getpid - return the calling process's PID (getpid(2)).
 */
int32_t sys_getpid(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
    return (int32_t)current_proc->pid;
}

/**
 * The calling thread's id (gettid(2)): the process id, since each process
 * is one thread, as Linux's is for a process that has made none.
 */
int32_t sys_gettid(isr_params_t *params)
{
    return sys_getpid(params);
}

/**
 * Create a new session (setsid(2)).
 *
 * The caller becomes the leader of a new session AND a new process group,
 * both numbered with its pid. Fails with -EPERM if the caller already leads
 * a process group (a group leader cannot carry its group into a new
 * session). Returns the new session id (the caller's pid).
 */
int32_t sys_setsid(isr_params_t *params)
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
int32_t sys_setpgid(isr_params_t *params)
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
int32_t sys_getpgid(isr_params_t *params)
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
int32_t sys_getpgrp(isr_params_t *params)
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
int32_t sys_getsid(isr_params_t *params)
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
int32_t sys_getppid(isr_params_t *params)
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
int32_t sys_umask(isr_params_t *params)
{
    uint32_t old;

    if (!current_proc) {
        return -ESRCH;
    }
    old = current_proc->umask;
    current_proc->umask = params->ebx & 0777;
    return (int32_t)old;
}

int32_t sys_setuid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t uid = params->ebx;
    uint32_t old_euid = p->euid;

    if (cred_privileged()) {
        p->ruid = uid;
        p->euid = uid;
        p->suid = uid;
        cred_moved(p, old_euid, p->egid);
        return 0;
    }
    if (uid == p->ruid || uid == p->suid) {
        p->euid = uid;
        cred_moved(p, old_euid, p->egid);
        return 0;
    }
    return -EPERM;
}

int32_t sys_setgid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t gid = params->ebx;
    uint32_t old_egid = p->egid;

    if (cred_privileged()) {
        p->rgid = gid;
        p->egid = gid;
        p->sgid = gid;
        cred_moved(p, p->euid, old_egid);
        return 0;
    }
    if (gid == p->rgid || gid == p->sgid) {
        p->egid = gid;
        cred_moved(p, p->euid, old_egid);
        return 0;
    }
    return -EPERM;
}

int32_t sys_setreuid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t r = params->ebx;
    uint32_t e = params->ecx;
    uint32_t new_r = (r == SC_ID_KEEP) ? p->ruid : r;
    uint32_t new_e = (e == SC_ID_KEEP) ? p->euid : e;
    uint32_t old_euid = p->euid;

    if (!cred_privileged()) {
        if (r != SC_ID_KEEP && new_r != p->ruid && new_r != p->euid) {
            return -EPERM;
        }
        if (e != SC_ID_KEEP && new_e != p->ruid && new_e != p->euid && new_e != p->suid) {
            return -EPERM;
        }
    }
    if (r != SC_ID_KEEP || (e != SC_ID_KEEP && new_e != p->ruid)) {
        p->suid = new_e;
    }
    p->ruid = new_r;
    p->euid = new_e;
    cred_moved(p, old_euid, p->egid);
    return 0;
}

int32_t sys_setregid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t r = params->ebx;
    uint32_t e = params->ecx;
    uint32_t new_r = (r == SC_ID_KEEP) ? p->rgid : r;
    uint32_t new_e = (e == SC_ID_KEEP) ? p->egid : e;
    uint32_t old_egid = p->egid;

    if (!cred_privileged()) {
        if (r != SC_ID_KEEP && new_r != p->rgid && new_r != p->egid) {
            return -EPERM;
        }
        if (e != SC_ID_KEEP && new_e != p->rgid && new_e != p->egid && new_e != p->sgid) {
            return -EPERM;
        }
    }
    if (r != SC_ID_KEEP || (e != SC_ID_KEEP && new_e != p->rgid)) {
        p->sgid = new_e;
    }
    p->rgid = new_r;
    p->egid = new_e;
    cred_moved(p, p->euid, old_egid);
    return 0;
}

int32_t sys_setresuid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t r = params->ebx;
    uint32_t e = params->ecx;
    uint32_t s = params->edx;
    uint32_t old_euid = p->euid;

    if (!cred_privileged()) {
        uint32_t v[3];
        int i;
        v[0] = r;
        v[1] = e;
        v[2] = s;
        for (i = 0; i < 3; i++) {
            if (v[i] != SC_ID_KEEP && v[i] != p->ruid && v[i] != p->euid && v[i] != p->suid) {
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
    cred_moved(p, old_euid, p->egid);
    return 0;
}

int32_t sys_setresgid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t r = params->ebx;
    uint32_t e = params->ecx;
    uint32_t s = params->edx;
    uint32_t old_egid = p->egid;

    if (!cred_privileged()) {
        uint32_t v[3];
        int i;
        v[0] = r;
        v[1] = e;
        v[2] = s;
        for (i = 0; i < 3; i++) {
            if (v[i] != SC_ID_KEEP && v[i] != p->rgid && v[i] != p->egid && v[i] != p->sgid) {
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
    cred_moved(p, p->euid, old_egid);
    return 0;
}

/* getresuid/getresgid (the *32 forms): report all three ids through the three
 * user pointers, the introspection half of the saved-id model. */
int32_t sys_getresuid(isr_params_t *params)
{
    process_t *p = current_proc;

    if (copy_to_user((void __user *)params->ebx, &p->ruid, sizeof(p->ruid)) != 0 ||
        copy_to_user((void __user *)params->ecx, &p->euid, sizeof(p->euid)) != 0 ||
        copy_to_user((void __user *)params->edx, &p->suid, sizeof(p->suid)) != 0) {
        return -EFAULT;
    }
    return 0;
}

int32_t sys_getresgid(isr_params_t *params)
{
    process_t *p = current_proc;

    if (copy_to_user((void __user *)params->ebx, &p->rgid, sizeof(p->rgid)) != 0 ||
        copy_to_user((void __user *)params->ecx, &p->egid, sizeof(p->egid)) != 0 ||
        copy_to_user((void __user *)params->edx, &p->sgid, sizeof(p->sgid)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/* setgroups/getgroups (the *32 forms): the supplementary list, privileged to
 * set (effective uid 0), NGROUPS_MAX entries. getgroups with size 0 reports the
 * count, the POSIX sizing idiom. */
int32_t sys_setgroups(isr_params_t *params)
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
        if (copy_from_user(&tmp[i], (const void __user *)(list_ptr + i * 4), sizeof(tmp[i])) != 0) {
            return -EFAULT;
        }
    }
    for (i = 0; i < size; i++) {
        p->groups[i] = tmp[i];
    }
    p->ngroups = size;
    return 0;
}

int32_t sys_getgroups(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t size = params->ebx;
    uint32_t list_ptr = params->ecx;
    uint32_t i;

    if ((int32_t)size < 0) {
        return -EINVAL; /* the ABI size is signed; a negative is not a count */
    }
    if (size == 0) {
        return (int32_t)p->ngroups;
    }
    if (size < p->ngroups) {
        return -EINVAL;
    }
    for (i = 0; i < p->ngroups; i++) {
        if (copy_to_user((void __user *)(list_ptr + i * 4), &p->groups[i], sizeof(p->groups[i])) !=
            0) {
            return -EFAULT;
        }
    }
    return (int32_t)p->ngroups;
}

/* The resources: four are kept in the record and enforced, and the rest
 * are unlimited. */
#define RLIMIT_STACK_RES 3

#define RLIMIT_NPROC_RES 6

#define RLIMIT_NOFILE_RES 7

#define RLIMIT_AS_RES 9

#define RLIM_INF32 RLIM_INFINITY32

/** The record's slot for `resource`, or -1 for one that is not kept. */
static int rlimit_slot(uint32_t resource)
{
    switch (resource) {
    case RLIMIT_NPROC_RES:
        return RLIM_NPROC;
    case RLIMIT_NOFILE_RES:
        return RLIM_NOFILE;
    case RLIMIT_AS_RES:
        return RLIM_AS;
    case RLIMIT_STACK_RES:
        return RLIM_STACK;
    default:
        return -1;
    }
}

static void rlimit_for(const process_t *p, uint32_t resource, uint32_t *cur, uint32_t *max)
{
    int slot = rlimit_slot(resource);

    if (slot >= 0) {
        *cur = p->rlim[slot].cur;
        *max = p->rlim[slot].max;
        return;
    }
    *cur = *max = RLIM_INF32;
}

/**
 * Set a kept limit of `p`: the soft value at most the hard, the hard raised
 * by root only and lowered by anyone, and RLIMIT_NOFILE never above the
 * table's limit. A limit that is not kept is accepted and not changed.
 * 0, or -EINVAL and -EPERM as POSIX names them.
 */
static int rlimit_set(process_t *p, uint32_t resource, uint32_t cur, uint32_t max)
{
    int slot = rlimit_slot(resource);

    if (slot < 0) {
        return 0;
    }
    if (cur > max) {
        return -EINVAL;
    }
    if (max > p->rlim[slot].max && current_proc->euid != 0) {
        return -EPERM;
    }
    if (slot == RLIM_NOFILE) {
        if (max > FD_TABLE_LIMIT) {
            return -EPERM;
        }
        p->fds.limit = (int)cur;
    }
    p->rlim[slot].cur = cur;
    p->rlim[slot].max = max;
    return 0;
}

/**
 * The process `pid` names for a limit call: the caller for 0 or its own
 * pid; another process only to root or to a caller of the same real uid.
 */
static process_t *rlimit_target(uint32_t pid)
{
    process_t *p;

    if (pid == 0 || pid == current_proc->pid) {
        return current_proc;
    }
    p = get_process(pid);
    if (!p || p->state == PROC_UNUSED || p->state == PROC_ZOMBIE) {
        return NULL;
    }
    if (current_proc->euid != 0 && p->ruid != current_proc->ruid) {
        return NULL;
    }
    return p;
}

/* getrlimit(76)/ugetrlimit(191): 32-bit struct rlimit { u32 rlim_cur,
 * rlim_max }. */
int32_t sys_getrlimit(isr_params_t *params)
{
    uint32_t resource = params->ebx;
    uint32_t rl = params->ecx;
    uint32_t out[2];

    if (!rl) {
        return -EFAULT;
    }
    rlimit_for(current_proc, resource, &out[0], &out[1]);
    if (copy_to_user((void __user *)rl, out, sizeof(out)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/* setrlimit(75): the 32-bit form of a set. */
int32_t sys_setrlimit(isr_params_t *params)
{
    uint32_t resource = params->ebx;
    uint32_t rl = params->ecx;
    uint32_t in[2];

    if (copy_from_user(in, (const void __user *)rl, sizeof(in)) != 0) {
        return -EFAULT;
    }
    return rlimit_set(current_proc, resource, in[0], in[1]);
}

/* A 64-bit limit as the record holds it: the high word set is no limit, and
 * a value past 32 bits is kept as no limit too, since nothing here reaches
 * it. */
static uint32_t rlimit_from64(uint32_t lo, uint32_t hi)
{
    return hi ? RLIM_INF32 : lo;
}

/* prlimit64(340): EBX=pid (0/self), ECX=resource, EDX=new (rlimit64* or NULL),
 * ESI=old (rlimit64* or NULL). 64-bit struct rlimit64 { u64 cur, max }. The
 * old limits are read before the new are set, as POSIX's `setrlimit` and
 * `getrlimit` see them, and written to the caller after: a copy that fails
 * leaves the new limits in place, as it does on Linux. This is the form musl
 * issues for both. */
int32_t sys_prlimit64(isr_params_t *params)
{
    uint32_t pid = params->ebx;
    uint32_t resource = params->ecx;
    uint32_t newp = params->edx;
    uint32_t oldp = params->esi;
    process_t *p;
    uint32_t cur32, max32;
    uint32_t in[4];
    uint32_t out[4]; /* cur_lo, cur_hi, max_lo, max_hi */
    int rc = 0;

    /* The user copies may sleep, and another process may end while they
     * do, so the record is looked up between them and not held across one:
     * the new limits are read first, the old are written last. */
    if (newp && copy_from_user(in, (const void __user *)newp, sizeof(in)) != 0) {
        return -EFAULT;
    }
    p = rlimit_target(pid);
    if (!p) {
        return get_process(pid) ? -EPERM : -ESRCH;
    }
    rlimit_for(p, resource, &cur32, &max32);
    out[0] = cur32;
    out[1] = (cur32 == RLIM_INF32) ? 0xFFFFFFFFu : 0u; /* 64-bit RLIM_INFINITY */
    out[2] = max32;
    out[3] = (max32 == RLIM_INF32) ? 0xFFFFFFFFu : 0u;
    if (newp) {
        rc = rlimit_set(p, resource, rlimit_from64(in[0], in[1]), rlimit_from64(in[2], in[3]));
    }
    if (oldp && copy_to_user((void __user *)oldp, out, sizeof(out)) != 0) {
        return -EFAULT;
    }
    return rc;
}

/*
 * mmap2 and mprotect protection bits and flags (Linux i386 ABI). The PROT_*
 * bits become VMA permissions; only anonymous mappings exist, since a mapping
 * of a file needs pages the file's cache holds.
 */
#define PROT_READ 0x1

#define PROT_WRITE 0x2

#define PROT_EXEC 0x4

#define MAP_SHARED 0x01

#define MAP_PRIVATE 0x02

#define MAP_SHARED_VALIDATE 0x03

#define MAP_TYPE 0x0F

#define MAP_FIXED 0x10

#define MAP_ANONYMOUS 0x20

#define MAP_FIXED_NOREPLACE 0x100000

/* Round 'v' up to the next page boundary. */
#define PAGE_UP(v) (((v) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

/*
 * sys_brk - set the program break (heap top). Arg: EBX = requested break.
 * Returns the resulting break: the new value on success, or the unchanged
 * current break if the request is out of range. This is the contract musl's
 * allocator relies on when it issues SYS_brk (it compares the return to its
 * request; musl's public brk() never issues the call). The heap is the
 * demand-paged range [heap_start, page_up(brk)), which mprotect may have cut
 * into regions of different protection: growing adds a read-write region at
 * the top, which merges with a read-write top piece, and shrinking removes
 * the range above the new top and frees its pages.
 */
int32_t sys_brk(isr_params_t *params)
{
    uint32_t req = params->ebx;
    vm_space_t *vm;
    uint32_t old_top, new_top;

    if (!current_proc) {
        return -ESRCH;
    }
    mem_balance();
    vm = &current_proc->vm;

    /* brk(0) queries the break; below the heap base or into the stack window is
     * refused with the break left unchanged. */
    if (req < vm->heap_start || req > vm->stack_limit) {
        return (int32_t)vm->brk;
    }

    old_top = PAGE_UP(vm->brk);
    new_top = PAGE_UP(req);

    if (new_top > old_top) {
        /* Grow: the new pages [old_top, new_top) must not collide with mmap or
         * any other region. */
        if (vm_overlaps(vm, old_top, new_top)) {
            return (int32_t)vm->brk;
        }
        if (vm_total(vm) + (new_top - old_top) > current_proc->rlim[RLIM_AS].cur) {
            return (int32_t)vm->brk; /* RLIMIT_AS: brk reports the break unchanged */
        }
        if (vm_insert(vm, old_top, new_top, VM_PROT_READ | VM_PROT_WRITE, VM_HEAP) != 0) {
            return (int32_t)vm->brk;
        }
    } else if (new_top < old_top) {
        if (vm_remove(vm, new_top, old_top) != 0) {
            return (int32_t)vm->brk;
        }
        unmap_user_range(_curr_dir, new_top, old_top);
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
         (int)current_proc->pid, current_proc->comm, VMA_LIMIT);
}

/* The VMA protection of an mmap or mprotect `prot`. Write and execute each
 * grant read, which this i386 paging cannot withhold from them. */
static uint32_t vm_prot_of(uint32_t prot)
{
    uint32_t vmprot = 0;

    if (prot & PROT_READ) {
        vmprot |= VM_PROT_READ;
    }
    if (prot & PROT_WRITE) {
        vmprot |= VM_PROT_WRITE | VM_PROT_READ;
    }
    if (prot & PROT_EXEC) {
        vmprot |= VM_PROT_EXEC | VM_PROT_READ;
    }
    return vmprot;
}

/**
 * The file or shared memory a new mapping maps, with the region kind it
 * makes: a private mapping of a file (VM_FILE), a shared mapping of a file
 * in memory or of shared anonymous memory (VM_SHARED), or NULL for private
 * anonymous memory (VM_ANON). /dev/zero maps as anonymous memory. A shared
 * mapping of a file on a disk file system is served as a private one when
 * it is read-only, which reads the same bytes, and refused with ENODEV when
 * it may be written, since its stores would not reach the file.
 */
static int32_t mmap_backing(uint32_t flags, uint32_t prot, int fd, struct vm_file **file,
                            vm_type_t *type)
{
    int shared = (flags & MAP_TYPE) != MAP_PRIVATE;
    fd_entry_t *e;
    struct shmem *sh;

    *file = NULL;
    *type = VM_ANON;
    if (!(flags & MAP_ANONYMOUS)) {
        e = fd_get(&current_proc->fds, fd);
        if (!e) {
            return -EBADF;
        }
        if (devfs_rdev(e) == ((1u << 8) | 5u)) { /* /dev/zero, 1:5 */
            flags |= MAP_ANONYMOUS;
        } else {
            if (e->type != FD_TYPE_FILE || !e->ofd || !e->ofd->vmnt) {
                return -ENODEV;
            }
            if (!(e->flags & FD_FLAG_READ) ||
                (shared && (prot & PROT_WRITE) && !(e->flags & FD_FLAG_WRITE))) {
                return -EACCES;
            }
            sh = (shared && e->ofd->vmnt->type == &vfs_tmpfs_type) ? tmpfs_shmem(e->ofd->vnode)
                                                                   : NULL;
            if (shared && !sh && (prot & PROT_WRITE)) {
                return -ENODEV;
            }
            *file = sh ? filemap_shared(e, sh) : filemap_new(e);
            *type = sh ? VM_SHARED : VM_FILE;
            return *file ? 0 : -ENOMEM;
        }
    }
    if (shared) {
        *file = filemap_anon_shared();
        *type = VM_SHARED;
        return *file ? 0 : -ENOMEM;
    }
    return 0;
}

/*
 * sys_mmap2 - map memory. Args: EBX=addr, ECX=length, EDX=prot, ESI=flags,
 * EDI=fd, EBP=pgoffset, the offset in the file in pages. Returns the mapped
 * base address, or -errno. What is mapped is `mmap_backing`'s: anonymous
 * memory, private or shared, or a file, private or shared. MAP_FIXED
 * replaces whatever the range held, and MAP_FIXED_NOREPLACE refuses a range
 * in use with EEXIST. Otherwise a page-aligned hint that is free below the
 * stack's growth window is taken as it is, and without one the highest free
 * range below VM_MMAP_BASE and above the heap's top is used, so the ranges
 * munmap frees are used again. Pages are backed on first touch: zeroed, read
 * from the file, or the shared page itself.
 */
int32_t sys_mmap2(isr_params_t *params)
{
    uint32_t addr = params->ebx;
    uint32_t length = params->ecx;
    uint32_t prot = params->edx;
    uint32_t flags = params->esi;
    vm_space_t *vm;
    uint32_t len, base, limit;
    struct vm_file *file;
    vm_type_t type;
    int32_t rc;

    if (!current_proc) {
        return -ESRCH;
    }
    mem_balance();
    vm = &current_proc->vm;

    if (length == 0 || (prot & ~(uint32_t)(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) {
        return -EINVAL;
    }
    if ((flags & MAP_TYPE) != MAP_SHARED && (flags & MAP_TYPE) != MAP_PRIVATE &&
        (flags & MAP_TYPE) != MAP_SHARED_VALIDATE) {
        return -EINVAL;
    }
    if (length > USER_ADDR_MAX - USER_ADDR_MIN) {
        return -ENOMEM;
    }
    len = PAGE_UP(length);
    limit = current_proc->rlim[RLIM_AS].cur;

    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        if ((addr & (PAGE_SIZE - 1)) != 0) {
            return -EINVAL;
        }
        if (addr < USER_ADDR_MIN || addr > USER_ADDR_MAX - len) {
            return -ENOMEM;
        }
        if ((flags & MAP_FIXED_NOREPLACE) && vm_overlaps(vm, addr, addr + len)) {
            return -EEXIST;
        }
        if (vm_total(vm) - vm_covered(vm, addr, addr + len) + len > limit) {
            return -ENOMEM; /* RLIMIT_AS, the replaced bytes not counted twice */
        }
        base = addr;
    } else {
        base = 0;
        if (addr != 0 && (addr & (PAGE_SIZE - 1)) == 0 && addr >= USER_ADDR_MIN &&
            len <= vm->stack_limit && addr <= vm->stack_limit - len &&
            !vm_overlaps(vm, addr, addr + len)) {
            base = addr;
        }
        if (base == 0) {
            base = vm_find_free(vm, len, PAGE_UP(vm->brk), VM_MMAP_BASE);
        }
        if (base == 0 || vm_total(vm) + len > limit) {
            return -ENOMEM;
        }
    }
    /* What is mapped is found before anything MAP_FIXED replaces goes, so a
       refusal leaves the old mapping. */
    rc = mmap_backing(flags, prot, (int)params->edi, &file, &type);
    if (rc != 0) {
        return rc;
    }
    if (flags & MAP_FIXED) {
        /* A split in the removal and the new region take two entries, found
           before the old mapping goes. */
        if (vm_reserve(vm, 2) != 0) {
            vm_file_put(file);
            vma_exhausted("mmap");
            return -ENOMEM;
        }
        vm_remove(vm, base, base + len);
        unmap_user_range(_curr_dir, base, base + len);
    }
    if (vm_insert_file(vm, base, base + len, vm_prot_of(prot), type, file, params->ebp) != 0) {
        vm_file_put(file);
        vma_exhausted("mmap");
        return -ENOMEM;
    }
    vm_file_put(file);
    return (int32_t)base;
}

/*
 * sys_munmap - unmap a range. Args: EBX=addr (page-aligned), ECX=length.
 * Returns 0, or -errno. Drops the VMA bookkeeping (trimming or splitting
 * partial overlaps) and frees the backing frames; unmapping never-faulted or
 * never-mapped pages is a no-op, as on Linux.
 */
int32_t sys_munmap(isr_params_t *params)
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

/* Bring the resident pages of [start, end) to `prot`, region by region, so a
 * shared region's pages stay shared. */
static void protect_resident(uint32_t start, uint32_t end, uint32_t prot)
{
    while (start < end) {
        vma_t *r = vm_find(&current_proc->vm, start);
        uint32_t stop = (r && r->end < end) ? r->end : end;

        paging_protect_range(_curr_dir, start, stop,
                             prot | ((r && r->type == VM_SHARED) ? VM_PROT_SHARED : 0u));
        start = stop;
    }
}

/*
 * sys_mprotect - change the protection of a range. Args: EBX=addr (page
 * aligned), ECX=length, EDX=prot. Returns 0, or -errno: EINVAL for an
 * unaligned address or an unknown protection bit, ENOMEM when a page of the
 * range is not mapped or a split finds no room for its region. The regions
 * take the new protection first and the resident pages after, so a fault in
 * between reads the protection the call is setting.
 */
int32_t sys_mprotect(isr_params_t *params)
{
    uint32_t addr = params->ebx;
    uint32_t length = params->ecx;
    uint32_t prot = params->edx;
    uint32_t end, vmprot;
    int rc;

    if (!current_proc) {
        return -ESRCH;
    }
    if ((addr & (PAGE_SIZE - 1)) != 0 ||
        (prot & ~(uint32_t)(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) {
        return -EINVAL;
    }
    if (length == 0) {
        return 0;
    }
    if (addr < USER_ADDR_MIN || addr >= USER_ADDR_MAX || length > USER_ADDR_MAX - addr) {
        return -ENOMEM;
    }
    end = PAGE_UP(addr + length);
    vmprot = vm_prot_of(prot);
    rc = vm_protect(&current_proc->vm, addr, end, vmprot);
    if (rc == -1) {
        return -ENOMEM;
    }
    if (rc != 0) {
        vma_exhausted("mprotect");
        return -ENOMEM;
    }
    protect_resident(addr, end, vmprot);
    return 0;
}

#define MREMAP_MAYMOVE   1
#define MREMAP_FIXED     2
#define MREMAP_DONTUNMAP 4

/* Give [addr, addr + len), the pages of one region from its page `pgoff`,
 * the region at `to`, moving the pages there and making the region `new_len`
 * long. The file the region maps is held across the move, which removes
 * the old region's hold before the new region takes its own. */
static int32_t remap_move(vm_space_t *vm, vma_t *r, uint32_t addr, uint32_t len, uint32_t to,
                          uint32_t new_len)
{
    struct vm_file *file = r->file;
    uint32_t pgoff = r->pgoff + (addr - r->start) / PAGE_SIZE;
    uint32_t prot = r->prot;
    vm_type_t type = r->type;
    uint32_t moved = len < new_len ? len : new_len;

    if (vm_reserve(vm, 3) != 0) {
        vma_exhausted("mremap");
        return -ENOMEM;
    }
    if (paging_move_range(_curr_dir, addr, to, moved) != 0) {
        return -ENOMEM;
    }
    vm_file_get(file);
    vm_remove(vm, addr, addr + len);
    unmap_user_range(_curr_dir, addr, addr + len);
    if (vm_insert_file(vm, to, to + new_len, prot, type, file, pgoff) != 0) {
        vm_file_put(file);
        unmap_user_range(_curr_dir, to, to + moved);
        return -ENOMEM;
    }
    vm_file_put(file);
    return (int32_t)to;
}

/*
 * sys_mremap - resize a mapping, or move it. Args: EBX=old address, ECX=old
 * length, EDX=new length, ESI=flags, EDI=new address with MREMAP_FIXED.
 * The old range must lie in one region. A shrink cuts its end; a growth
 * takes the range after it when that is free, or with MREMAP_MAYMOVE moves
 * the whole mapping, pages and all, to a free range of the new length, or
 * to the new address with MREMAP_FIXED, whatever was there going first.
 * Returns the mapping's address, or -errno: EINVAL, EFAULT for a range not
 * in one region, ENOMEM when it can grow in place and may not move, or when
 * no range is free. MREMAP_DONTUNMAP is not implemented.
 */
int32_t sys_mremap(isr_params_t *params)
{
    uint32_t addr = params->ebx;
    uint32_t old_len = PAGE_UP(params->ecx);
    uint32_t new_len = PAGE_UP(params->edx);
    uint32_t flags = params->esi;
    uint32_t to = params->edi;
    vm_space_t *vm;
    vma_t *r;

    if (!current_proc) {
        return -ESRCH;
    }
    vm = &current_proc->vm;
    if ((addr & (PAGE_SIZE - 1)) != 0 || new_len == 0 || old_len == 0 ||
        (flags & ~(uint32_t)(MREMAP_MAYMOVE | MREMAP_FIXED)) != 0 ||
        ((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE))) {
        return -EINVAL;
    }
    if (params->edx > USER_ADDR_MAX - USER_ADDR_MIN || addr > USER_ADDR_MAX - old_len) {
        return -EINVAL;
    }
    r = vm_find(vm, addr);
    if (!r || addr + old_len > r->end) {
        return -EFAULT;
    }
    if (flags & MREMAP_FIXED) {
        if ((to & (PAGE_SIZE - 1)) != 0 || to < USER_ADDR_MIN || to > USER_ADDR_MAX - new_len ||
            (to < addr + old_len && addr < to + new_len)) {
            return -EINVAL;
        }
        if (vm_total(vm) - vm_covered(vm, to, to + new_len) + (new_len > old_len ? new_len - old_len : 0) >
            current_proc->rlim[RLIM_AS].cur) {
            return -ENOMEM;
        }
        if (vm_reserve(vm, 4) != 0) {
            vma_exhausted("mremap");
            return -ENOMEM;
        }
        vm_remove(vm, to, to + new_len);
        unmap_user_range(_curr_dir, to, to + new_len);
        r = vm_find(vm, addr);
        return remap_move(vm, r, addr, old_len, to, new_len);
    }
    if (new_len <= old_len) {
        if (vm_remove(vm, addr + new_len, addr + old_len) != 0) {
            vma_exhausted("mremap");
            return -ENOMEM;
        }
        unmap_user_range(_curr_dir, addr + new_len, addr + old_len);
        return (int32_t)addr;
    }
    if (vm_total(vm) + (new_len - old_len) > current_proc->rlim[RLIM_AS].cur) {
        return -ENOMEM;
    }
    if (addr + old_len == r->end && addr <= USER_ADDR_MAX - new_len &&
        addr + new_len <= vm->stack_limit && !vm_overlaps(vm, addr + old_len, addr + new_len)) {
        if (vm_insert_file(vm, addr + old_len, addr + new_len, r->prot, r->type, r->file,
                           r->pgoff + (addr + old_len - r->start) / PAGE_SIZE) != 0) {
            vma_exhausted("mremap");
            return -ENOMEM;
        }
        return (int32_t)addr;
    }
    if (!(flags & MREMAP_MAYMOVE)) {
        return -ENOMEM;
    }
    to = vm_find_free(vm, new_len, PAGE_UP(vm->brk), VM_MMAP_BASE);
    if (to == 0) {
        return -ENOMEM;
    }
    return remap_move(vm, r, addr, old_len, to, new_len);
}

#define MADV_DONTNEED 4
#define MADV_FREE     8
#define MADV_REMOVE   9
#define MADV_HWPOISON 100

/*
 * sys_madvise - advice about a range. Args: EBX=address, ECX=length,
 * EDX=advice. MADV_DONTNEED and MADV_FREE let the resident pages go: a
 * private anonymous page reads as zeros again, a private page of a file
 * reads the file again, and a shared page reads what the shared memory
 * holds. The other advice Linux has is accepted and changes nothing, but
 * MADV_REMOVE and MADV_HWPOISON, which are refused with EINVAL. A range with
 * a page in no region is ENOMEM, as in Linux.
 */
int32_t sys_madvise(isr_params_t *params)
{
    uint32_t addr = params->ebx;
    uint32_t len = PAGE_UP(params->ecx);
    uint32_t advice = params->edx;

    if (!current_proc) {
        return -ESRCH;
    }
    if ((addr & (PAGE_SIZE - 1)) != 0 || addr > USER_ADDR_MAX - len || advice == MADV_REMOVE ||
        advice >= MADV_HWPOISON) {
        return -EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (vm_covered(&current_proc->vm, addr, addr + len) != len) {
        return -ENOMEM;
    }
    if (advice == MADV_DONTNEED || advice == MADV_FREE) {
        unmap_user_range(_curr_dir, addr, addr + len);
    }
    return 0;
}

/*
 * sys_msync - write a mapping back. Args: EBX=address, ECX=length,
 * EDX=flags. A shared mapping here maps the file in memory itself, and a
 * mapping of a disk file is private or read-only, so there is nothing to
 * write: the call checks its arguments and returns 0, or ENOMEM for a range
 * with a page in no region, or EINVAL.
 */
int32_t sys_msync(isr_params_t *params)
{
    uint32_t addr = params->ebx;
    uint32_t len = PAGE_UP(params->ecx);
    uint32_t flags = params->edx;

    if (!current_proc) {
        return -ESRCH;
    }
    if ((addr & (PAGE_SIZE - 1)) != 0 || (flags & ~7u) != 0 || (flags & 5u) == 5u ||
        addr > USER_ADDR_MAX - len) {
        return -EINVAL;
    }
    if (vm_covered(&current_proc->vm, addr, addr + len) != len) {
        return -ENOMEM;
    }
    return 0;
}

/* The three kinds of target setpriority(2) and getpriority(2) take. */
#define PRIO_PROCESS 0

#define PRIO_PGRP 1

#define PRIO_USER 2

/* The range of a nice value. */
#define NICE_MIN (-20)

#define NICE_MAX 19

/**
 * Whether `p` is a target of a priority call: the process `who` names for
 * PRIO_PROCESS, a member of group `who` for PRIO_PGRP, and a process of real
 * user `who` for PRIO_USER, where `who` 0 is the caller, its group or its
 * real uid. Zombies count, as Linux counts them until they are reaped, and
 * the kernel's own record, pid 0, does not.
 */
static int prio_target(const process_t *p, int which, uint32_t who)
{
    if (p->pid == 0) {
        return 0;
    }
    switch (which) {
    case PRIO_PROCESS:
        return p->pid == (who != 0 ? who : current_proc->pid);
    case PRIO_PGRP:
        return p->pgid == (who != 0 ? who : current_proc->pgid);
    default:
        return p->ruid == (who != 0 ? who : current_proc->ruid);
    }
}

/**
 * Read a priority (getpriority(2), syscall 96).
 *
 * EBX = which, ECX = who. Returns the highest priority among the targets,
 * as Linux's raw call returns it, 20 less the nice value, so 1 for nice 19
 * and 40 for nice -20; the C library turns it back into the nice value.
 * -EINVAL for an unknown `which`, -ESRCH when nothing matches.
 */
int32_t sys_getpriority(isr_params_t *params)
{
    int which = (int)params->ebx;
    process_t *p;
    int32_t best = -ESRCH;

    if (which < PRIO_PROCESS || which > PRIO_USER) {
        return -EINVAL;
    }
    for (p = proc_list; p; p = p->next) {
        if (prio_target(p, which, params->ecx) && 20 - p->nice > best) {
            best = 20 - p->nice;
        }
    }
    return best;
}

/**
 * Set a priority (setpriority(2), syscall 97).
 *
 * EBX = which, ECX = who, EDX = the nice value, clamped to -20 through 19
 * as Linux clamps it. Each target is changed on Linux's terms: a caller
 * whose effective uid is neither the target's real nor its effective uid
 * gets -EPERM, and one that would lower the target's nice value, raising its
 * priority, gets -EACCES, since `RLIMIT_NICE` is 0; effective uid 0 may do
 * both. The targets that pass are changed whatever the others answer, and the
 * call returns the last refusal, 0 when there was none, or -ESRCH when
 * nothing matched. The value is kept and inherited, and the scheduler does
 * not read it: every ready process takes its turn.
 */
int32_t sys_setpriority(isr_params_t *params)
{
    int which = (int)params->ebx;
    int32_t nice = (int32_t)params->edx;
    process_t *p;
    int32_t rc = -ESRCH;

    if (which < PRIO_PROCESS || which > PRIO_USER) {
        return -EINVAL;
    }
    if (nice < NICE_MIN) {
        nice = NICE_MIN;
    }
    if (nice > NICE_MAX) {
        nice = NICE_MAX;
    }
    for (p = proc_list; p; p = p->next) {
        if (!prio_target(p, which, params->ecx)) {
            continue;
        }
        if (!cred_privileged() && current_proc->euid != p->ruid &&
            current_proc->euid != p->euid) {
            rc = -EPERM;
            continue;
        }
        if (nice < p->nice && !cred_privileged()) {
            rc = -EACCES;
            continue;
        }
        if (rc == -ESRCH) {
            rc = 0;
        }
        p->nice = nice;
    }
    return rc;
}

/* The scheduling policies Linux numbers. Only SCHED_OTHER is run here; the
 * others are known so that each is refused as Linux refuses it, or answered
 * as Linux answers for it. */
#define SCHED_OTHER 0

#define SCHED_FIFO 1

#define SCHED_RR 2

#define SCHED_BATCH 3

#define SCHED_IDLE 5

#define SCHED_DEADLINE 6

#define SCHED_EXT 7

/* The flag a policy may carry that asks a fork not to pass the policy on;
 * accepted with SCHED_OTHER, which it changes nothing for here. */
#define SCHED_RESET_ON_FORK 0x40000000

/* The highest real-time priority Linux gives SCHED_FIFO and SCHED_RR. */
#define SCHED_RT_PRIO_MAX 99

/* The bytes of a CPU mask: one 32-bit word, of which CPU 0 is the only
 * bit a machine of one processor sets. */
#define CPU_MASK_BYTES 4u

/**
 * The process a scheduling call names: the caller for pid 0, otherwise the
 * process with that pid, zombies included as Linux includes them, or NULL.
 */
static process_t *sched_target(int pid)
{
    if (pid == 0) {
        return current_proc;
    }
    return get_process((uint32_t)pid);
}

/**
 * Whether the caller may change `p`'s scheduling: effective uid 0, or an
 * effective uid equal to the target's effective or real uid, Linux's rule for
 * a caller without CAP_SYS_NICE.
 */
static int sched_may_change(const process_t *p)
{
    return cred_privileged() || current_proc->euid == p->euid || current_proc->euid == p->ruid;
}

/**
 * Give way to the other ready processes (sched_yield(2), syscall 158).
 *
 * The caller is put behind them in the round-robin order and runs again when
 * its turn comes, at once when no other process is ready. Returns 0.
 */
int32_t sys_sched_yield(isr_params_t *params)
{
    (void)params;
    schedule();
    return 0;
}

/**
 * Change a process's policy and priority (sched_setscheduler(2), syscall
 * 156), or its priority alone (sched_setparam(2), syscall 154, which passes
 * `policy` -1).
 *
 * `uparam` points at a struct sched_param, one int. SCHED_OTHER with priority
 * 0 is the one setting a process has here, and asking for it succeeds for a
 * caller that may change the target; SCHED_RESET_ON_FORK beside it is
 * accepted and kept nowhere, since a fork has no other policy to reset. The
 * rest is refused in Linux's order and with its errors: no parameter or a
 * negative pid, -EINVAL; an unreadable parameter, -EFAULT; no such process,
 * -ESRCH; a priority past 99, or one that does not suit the policy, -EINVAL,
 * as for SCHED_DEADLINE, which takes its parameters from sched_setattr
 * alone; SCHED_FIFO and SCHED_RR, -EPERM, as Linux answers a caller without
 * CAP_SYS_NICE when RLIMIT_RTPRIO is 0; and a caller that may not change the
 * target, -EPERM. SCHED_BATCH and SCHED_IDLE, which Linux grants anyone, and
 * every other number, -EINVAL: this kernel has one policy.
 */
static int32_t sched_set(int pid, int policy, const void __user *uparam)
{
    int32_t priority;
    process_t *p;

    if (!uparam || pid < 0) {
        return -EINVAL;
    }
    if (copy_from_user(&priority, uparam, sizeof(priority)) != 0) {
        return -EFAULT;
    }
    p = sched_target(pid);
    if (!p) {
        return -ESRCH;
    }
    if (policy >= 0) {
        policy &= ~SCHED_RESET_ON_FORK;
    } else {
        policy = SCHED_OTHER;
    }
    if (priority < 0 || priority > SCHED_RT_PRIO_MAX) {
        return -EINVAL;
    }
    switch (policy) {
    case SCHED_OTHER:
        if (priority != 0) {
            return -EINVAL;
        }
        return sched_may_change(p) ? 0 : -EPERM;
    case SCHED_FIFO:
    case SCHED_RR:
        return priority == 0 ? -EINVAL : -EPERM;
    default:
        return -EINVAL;
    }
}

/** sched_setscheduler(2), syscall 156: EBX = pid, ECX = policy, EDX = the
 * struct sched_param; see `sched_set`. A negative policy is -EINVAL first. */
int32_t sys_sched_setscheduler(isr_params_t *params)
{
    if ((int)params->ecx < 0) {
        return -EINVAL;
    }
    return sched_set((int)params->ebx, (int)params->ecx, (const void __user *)params->edx);
}

/** sched_setparam(2), syscall 154: EBX = pid, ECX = the struct
 * sched_param; see `sched_set`. */
int32_t sys_sched_setparam(isr_params_t *params)
{
    return sched_set((int)params->ebx, -1, (const void __user *)params->ecx);
}

/**
 * A process's policy (sched_getscheduler(2), syscall 157): SCHED_OTHER, the
 * one every process has. -EINVAL for a negative pid, -ESRCH for no process.
 */
int32_t sys_sched_getscheduler(isr_params_t *params)
{
    int pid = (int)params->ebx;

    if (pid < 0) {
        return -EINVAL;
    }
    return sched_target(pid) ? SCHED_OTHER : -ESRCH;
}

/**
 * A process's scheduling parameter (sched_getparam(2), syscall 155): the
 * priority 0 of SCHED_OTHER, written to the struct sched_param at ECX.
 * -EINVAL for no parameter or a negative pid, -ESRCH, -EFAULT.
 */
int32_t sys_sched_getparam(isr_params_t *params)
{
    int pid = (int)params->ebx;
    void __user *uparam = (void __user *)params->ecx;
    int32_t priority = 0;

    if (!uparam || pid < 0) {
        return -EINVAL;
    }
    if (!sched_target(pid)) {
        return -ESRCH;
    }
    return copy_to_user(uparam, &priority, sizeof(priority)) != 0 ? -EFAULT : 0;
}

/**
 * The priority range of a policy, as Linux gives it whether the policy runs
 * here or not: 1 to 99 for SCHED_FIFO and SCHED_RR, 0 to 0 for the other
 * policies Linux has, and -EINVAL for a number that is none. `max` picks the
 * top of the range.
 */
static int32_t sched_priority_bound(int policy, int max)
{
    switch (policy) {
    case SCHED_FIFO:
    case SCHED_RR:
        return max ? SCHED_RT_PRIO_MAX : 1;
    case SCHED_OTHER:
    case SCHED_BATCH:
    case SCHED_IDLE:
    case SCHED_DEADLINE:
    case SCHED_EXT:
        return 0;
    default:
        return -EINVAL;
    }
}

/** sched_get_priority_max(2), syscall 159: EBX = policy. */
int32_t sys_sched_get_priority_max(isr_params_t *params)
{
    return sched_priority_bound((int)params->ebx, 1);
}

/** sched_get_priority_min(2), syscall 160: EBX = policy. */
int32_t sys_sched_get_priority_min(isr_params_t *params)
{
    return sched_priority_bound((int)params->ebx, 0);
}

/**
 * A process's round-robin time slice (sched_rr_get_interval(2), syscall 161,
 * 32-bit timespec): one tick, 10 ms, since the timer takes the processor
 * from every process at each tick. -EINVAL for a negative pid, -ESRCH,
 * -EFAULT.
 */
int32_t sys_sched_rr_get_interval(isr_params_t *params)
{
    int pid = (int)params->ebx;
    struct kernel_timespec slice;

    if (pid < 0) {
        return -EINVAL;
    }
    if (!sched_target(pid)) {
        return -ESRCH;
    }
    slice.tv_sec = 0;
    slice.tv_nsec = 10000000;
    if (copy_to_user((void __user *)params->ecx, &slice, sizeof(slice)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * The CPUs a process may run on (sched_getaffinity(2), syscall 242).
 *
 * EBX = pid, ECX = the bytes of the mask at EDX. The mask has CPU 0 alone,
 * the one processor, and the call writes its four bytes and returns 4, as
 * Linux returns the bytes it wrote; the C library clears the rest of a larger
 * set. -EINVAL for a length that is zero or not a multiple of four bytes,
 * -ESRCH for no process, -EFAULT.
 */
int32_t sys_sched_getaffinity(isr_params_t *params)
{
    uint32_t len = params->ecx;
    uint32_t mask = 1;

    if (len == 0 || (len & 3u) != 0) {
        return -EINVAL;
    }
    if (!sched_target((int)params->ebx)) {
        return -ESRCH;
    }
    if (copy_to_user((void __user *)params->edx, &mask, CPU_MASK_BYTES) != 0) {
        return -EFAULT;
    }
    return (int32_t)CPU_MASK_BYTES;
}

/**
 * Set the CPUs a process may run on (sched_setaffinity(2), syscall 241).
 *
 * EBX = pid, ECX = the bytes of the mask at EDX. At most four bytes are read,
 * as Linux reads no more than its own mask's size, and a shorter mask is
 * zero past its end. A mask with CPU 0 is accepted, since that is where every
 * process runs; one without it names no CPU of this machine and is refused
 * with -EINVAL. -EFAULT, then -ESRCH, then -EPERM for a caller that may not
 * change the target, in Linux's order.
 */
int32_t sys_sched_setaffinity(isr_params_t *params)
{
    uint32_t len = params->ecx;
    uint32_t mask = 0;
    process_t *p;

    if (len > CPU_MASK_BYTES) {
        len = CPU_MASK_BYTES;
    }
    if (copy_from_user(&mask, (const void __user *)params->edx, len) != 0) {
        return -EFAULT;
    }
    p = sched_target((int)params->ebx);
    if (!p) {
        return -ESRCH;
    }
    if (!sched_may_change(p)) {
        return -EPERM;
    }
    return (mask & 1u) ? 0 : -EINVAL;
}

/* The prctl options this kernel has; every other is EINVAL, as Linux answers
 * an option it does not know. */
#define PR_SET_PDEATHSIG 1

#define PR_GET_PDEATHSIG 2

#define PR_GET_DUMPABLE 3

#define PR_SET_DUMPABLE 4

#define PR_SET_NAME 15

#define PR_GET_NAME 16

#define PR_SET_NO_NEW_PRIVS 38

#define PR_GET_NO_NEW_PRIVS 39

/**
 * Operations on the calling process (prctl(2), syscall 172).
 *
 * EBX = option, ECX to EDI = its four arguments, each as Linux takes it:
 *
 * PR_SET_PDEATHSIG sets the signal, 1 to 64 or 0 for none, that this
 * process receives when its parent exits (`reparent_children` posts it), and
 * PR_GET_PDEATHSIG stores it as an int at the second argument.
 *
 * PR_GET_DUMPABLE returns whether the process is dumpable, and
 * PR_SET_DUMPABLE sets it to 0 or 1. Nothing reads it here, since the kernel
 * writes no core files and has no tracing.
 *
 * PR_SET_NAME sets the program name `/proc/<pid>/stat` and ps show from the
 * string at the second argument, cut to 15 bytes, and PR_GET_NAME stores the
 * name's 16 bytes, NUL-padded, at the second argument.
 *
 * PR_SET_NO_NEW_PRIVS, with the second argument 1 and the rest 0, sets
 * no_new_privs for good: fork passes it on and exec then ignores set-id bits.
 * PR_GET_NO_NEW_PRIVS, with every argument 0, returns it.
 *
 * Returns 0 or the value asked for; -EINVAL for any other option, a signal
 * past 64, a dumpable value other than 0 and 1, or arguments not as above;
 * -EFAULT for a pointer that cannot be read or written.
 */
int32_t sys_prctl(isr_params_t *params)
{
    uint32_t arg2 = params->ecx;
    uint32_t rest = params->edx | params->esi | params->edi;
    int32_t value;
    char name[sizeof(current_proc->comm)];

    switch (params->ebx) {
    case PR_SET_PDEATHSIG:
        if (arg2 > KSIG_MAX) {
            return -EINVAL;
        }
        current_proc->pdeath_signal = arg2;
        return 0;
    case PR_GET_PDEATHSIG:
        value = (int32_t)current_proc->pdeath_signal;
        return copy_to_user((void __user *)arg2, &value, sizeof(value)) != 0 ? -EFAULT : 0;
    case PR_GET_DUMPABLE:
        return (int32_t)current_proc->dumpable;
    case PR_SET_DUMPABLE:
        if (arg2 > 1) {
            return -EINVAL;
        }
        current_proc->dumpable = arg2;
        return 0;
    case PR_SET_NAME:
        k_memset(name, 0, sizeof(name));
        if (strncpy_from_user(name, (const char __user *)arg2, sizeof(name) - 1) == -EFAULT) {
            return -EFAULT;
        }
        name[sizeof(name) - 1] = '\0';
        k_memcpy(current_proc->comm, name, sizeof(name));
        return 0;
    case PR_GET_NAME:
        k_memcpy(name, current_proc->comm, sizeof(name));
        return copy_to_user((void __user *)arg2, name, sizeof(name)) != 0 ? -EFAULT : 0;
    case PR_SET_NO_NEW_PRIVS:
        if (arg2 != 1 || rest != 0) {
            return -EINVAL;
        }
        current_proc->no_new_privs = 1;
        return 0;
    case PR_GET_NO_NEW_PRIVS:
        if (arg2 != 0 || rest != 0) {
            return -EINVAL;
        }
        return current_proc->no_new_privs ? 1 : 0;
    default:
        return -EINVAL;
    }
}
