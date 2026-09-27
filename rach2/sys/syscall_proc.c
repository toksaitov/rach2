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
#include "proc/wait_queue.h"
#include "proc/tgroup.h"
#include "proc/ptrace.h"
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
#include "mm/mm.h"
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
                if (p->pid == p->tgid && p->tg->live != 0) {
                    alive++; /* a leader waits for the rest of its threads */
                    continue;
                }
                mm_drop(p);
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
 * What a process's end releases beyond its threads: the framebuffer it owns,
 * the console its session leads, and the pty slaves that session controls,
 * so a later session can claim them. The console goes before the process is
 * published as a zombie while the teardown runs with interrupts on;
 * otherwise the timer-IRQ serial drain (tty_console_poll) keeps draining
 * COM1 against the vanishing session, dropping a ^C/^Z and stealing bytes
 * the kernel debug shell should read. shell_reset_input on the init path
 * releases the console again, which changes nothing.
 */
static void exit_release_session(void)
{
    fb_release_if_owner(current_proc->tgid);
    if (tty_console_owned_by_leader((int)current_proc->sid, (int)current_proc->tgid)) {
        tty_set_session(&console_tty, 0, 0);
    }
    pty_release_leader((int)current_proc->sid, (int)current_proc->tgid);
}

/**
 * Leave the processor for good: the record of a thread that has ended is
 * never selected again, and a thread that is not its process's last is
 * freed by another's return to user mode.
 */
static void __attribute__((noreturn)) exit_schedule_away(void)
{
    for (;;) {
        __asm__ __volatile__("cli");
        schedule();
        idle_wait_irq_off();
    }
}

void proc_group_exit(int32_t code)
{
    struct tgroup *tg = current_proc->tg;
    struct k_siginfo info;
    process_t *t;

    if (tg->exiting || tg->exec_task) {
        return;
    }
    tg->exiting = 1;
    tg->exit_code = code;
    k_memset(&info, 0, sizeof(info));
    info.code = SI_KERNEL;
    for (t = proc_list; t; t = t->next) {
        if (t->tg == tg && t != current_proc && t->state != PROC_ZOMBIE) {
            signal_post_thread(t, SIGKILL, &info);
        }
    }
}

/**
 * End the calling thread with the encoded wait status `code`, and with the
 * last thread the process. Does not return.
 *
 * A thread that is not its process's last only releases what it holds and
 * leaves. The last one releases the process's session, reparents the
 * children to init, publishes the zombie and wakes its parent, then
 * reschedules. A normal child leaves its mappings, descriptor-table storage,
 * record and stack to do_wait. A real init's exit is fatal
 * (panic_init_died); an init exiting under the debug shell returns to it.
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
static int32_t exit_current(int32_t code)
{
    int status = (code >> 8) & 0xFF;
    bool is_init;
    int alone;
    uint32_t exiting_pid;

    if (!current_proc) {
        __asm__ __volatile__("cli; hlt");
        return 0;
    }
    ptrace_event(PTRACE_EVENT_EXIT, (uint32_t)code);
    exiting_pid = current_proc->tgid;
    is_init = (exiting_pid == init_pid && init_pid != 0);
    alone = current_proc->tg->live == 1;
    if (alone) {
        exit_release_session();
        if (is_init && debug_shell_is_home) {
            proc_end_others();
        }
    }
    if (!proc_exit_cleanup(code)) {
        exit_schedule_away();
    }
    if (!alone) {
        exit_release_session();
    }
    if (debug_shell_is_home) {
        klog(KLOG_INFO, "process", "exited with status %d", status);
    }

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
        exit_schedule_away();
    }

    klog(KLOG_INFO, "sys_exit", "init process %d exited with status %d, returning to kernel shell",
         exiting_pid, status);
    console_puts("Init process exited. Returning to kernel debug shell.");

    /* Clean up the init process resources; `mm_drop` switches off the
     * address space first, since free_page_directory refuses the directory
     * in CR3. */
    mm_drop(current_proc);
    switch_page_directory(_kernel_dir);
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

    /* Reset ESP to a fresh kernel stack and jump to shell_loop. Safe because
     * we are in kernel mode and init has been torn down. */
    __asm__ __volatile__("movl %0, %%esp\n\t"
                         "sti\n\t"
                         "jmp *%1\n\t"
                         :
                         : "r"((uint32_t)stack + KERNEL_STACK_SIZE_LOCAL), "r"((uint32_t)shell_loop)
                         : "memory");
    exit_schedule_away();
}

/**
 * exit (1): end the calling thread with the status in EBX; the process ends
 * with its last thread. A thread's return from its start function makes
 * this call through glibc's and musl's thread code; exit(3) and _exit(2)
 * make exit_group.
 */
int32_t sys_exit(isr_params_t *params)
{
    return exit_current((int32_t)((params->ebx & 0xFFu) << 8));
}

/**
 * exit_group (252): end every thread of the process, the process with the
 * status in EBX.
 */
int32_t sys_exit_group(isr_params_t *params)
{
    int32_t code = (int32_t)((params->ebx & 0xFFu) << 8);

    if (current_proc) {
        proc_group_exit(code);
    }
    return exit_current(code);
}

/**
 * Duplicate the parent's user address space `from`, the one in use, into
 * the child's directory, copy-on-write.
 *
 * Iterates the parent's page directory and, for each present user page, maps
 * the SAME physical frame into the child (bumping the frame refcount) rather
 * than copying it; writable pages are write-protected and PTE_COW-marked in
 * both directories so the first write faults into cow_fault for a private
 * copy. Returns 0 on success, -1 on failure.
 */
static int copy_user_pages(struct mm *from, page_dir_t *child_dir)
{
    page_dir_t *parent_dir = from->dir;
    uint32_t dir_idx, page_idx;
    page_table_t *parent_table;

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
                vma_t *r = vm_find(&from->vm, virt_addr);

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

/*
 * A file execve reads: the program, a script's interpreter, or the dynamic
 * linker a PT_INTERP names. One on the KRFFS root is read by the path of its
 * node, from the root of the mount table, which is not the path the caller
 * gave when chroot moved the caller's root; one on another mount through a
 * handle on the node its resolution held, opened once and released after
 * the load. It lives in the caller's own exec workspace below, because a
 * read of a mounted volume sleeps and a second exec runs while it does.
 */
struct exec_file {
    char path[VFS_PATH_MAX]; /* the node's path on the KRFFS root */
    struct vfs_ref ref;      /* the mount is NULL on the KRFFS root and once closed */
    void *handle;
    int32_t err; /* the last failed read's errno, reported instead of ENOEXEC */
};

/* What a program's set-id bits would do: the bits that apply, and the file
 * owner's ids they would give. */
struct exec_setid {
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
};

/** An `elf_read_fn` over an `exec_file`. */
static int exec_file_read(void *ctx, void *dst, uint32_t count, uint32_t off)
{
    struct exec_file *f = ctx;
    int32_t got;

    if (f->ref.mnt == NULL) {
        return krffs_read(f->path, (char *)dst, count, off);
    }
    got = vfs_read(f->ref.mnt, f->handle, dst, count, off);
    if (got < 0) {
        f->err = got;
        return -1;
    }
    return (int)got;
}

/** Release the handle and the node `f` holds on another mount, if any. */
static void exec_file_close(struct exec_file *f)
{
    if (f->ref.mnt) {
        vfs_release(f->ref.mnt, f->handle);
        vfs_ref_release(&f->ref);
        f->ref.mnt = NULL;
        f->handle = NULL;
    }
}

/*
 * Where execve has the program's path, the new image's arguments and the
 * new image itself until it commits.
 *
 * Each call has a workspace of its own in the heap, which the caller's record
 * points at until the call ends, so a record freed during one gives it back
 * too. It is not one for the machine: a program on a mounted volume is read
 * through the buffer cache, which sleeps, so a second execve runs while the
 * first is inside a read, and a shared workspace let the second overwrite the
 * first's arguments, its open file and the reference its resolution held.
 * `in_syscall` stops the timer from scheduling; it does not stop a call that
 * gives the processor up.
 *
 * The strings are in `args`, the pages of the new stack (`proc/uimage.h`),
 * and the image is built in `mm`, an address space of its own, so every
 * limit and every failure of the load is found while there is still a
 * caller to return an error to.
 */

/* A script's first line, read here to be parsed. */
#define EXEC_SHEBANG_MAX 128

struct exec_state {
    struct exec_file prog; /* the file being read */
    struct exec_setid id;
    struct uimage args;
    struct mm *mm; /* the new image's address space until it commits */
    uint32_t argc;
    uint32_t envc;
    uint32_t euid; /* the ids the new image runs with */
    uint32_t egid;
    int secure;     /* AT_SECURE */
    uint32_t entry; /* where the new image starts */
    elf_layout_t layout;
    elf_layout_t ilayout; /* the interpreter's */
    elf_image_t img;
    elf_image_t iimg; /* the interpreter's */
    char path[VFS_PATH_MAX];
    char shebang_line[EXEC_SHEBANG_MAX + 1];
};

/** Give back the current process's exec workspace, if it has one: the
 * file held open, the frames of a stack no page directory took, the
 * address space of an image that did not commit, and the workspace
 * itself. */
static void exec_release(void)
{
    struct exec_state *s = current_proc->exec_ws;

    if (s == NULL) {
        return;
    }
    exec_file_close(&s->prog);
    uimage_release(&s->args);
    mm_put(s->mm);
    kfree(s);
    current_proc->exec_ws = NULL;
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
 * The arguments on the new stack are rewritten as Linux's `load_script`
 * rewrites them: argv[0] is taken away, and the script's path, the argument
 * and the interpreter are put below the rest, under the limits the caller's
 * strings met. The caller's `path` becomes the interpreter's. The script is
 * read through `read` over `ctx`: the root's reader by path, or the mount's
 * over an open handle.
 *
 * @param argc the argument count, updated when the vector grows.
 * @return 1 when rewritten, 0 when the file is not a script, or a negative
 *         errno for one whose first line names nothing or whose arguments no
 *         longer fit.
 */
static int32_t exec_shebang(struct exec_state *s, uint32_t *argc,
                            int (*read)(void *, void *, uint32_t, uint32_t), void *ctx)
{
    char *interp;
    char *arg = NULL;
    char *p;
    int n;
    int i;
    int32_t rc;

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

    rc = uimage_drop_first(&s->args);
    if (rc == 0) {
        rc = uimage_push(&s->args, s->path);
    }
    if (rc == 0 && arg != NULL) {
        rc = uimage_push(&s->args, arg);
        (*argc)++;
    }
    if (rc == 0) {
        rc = uimage_push(&s->args, interp);
    }
    if (rc != 0) {
        return rc;
    }
    (*argc)++;

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
 * How far below its top page the stack of a new image may grow, when the
 * pages its arguments took start at `bottom`: to `limit`, the soft
 * RLIMIT_STACK in force at the exec, in whole pages, which bounds the whole
 * stack as Linux's does, but never into the range below VM_MMAP_BASE where
 * anonymous mappings are placed, and always as far as `bottom`. A limit
 * changed later applies at the next exec.
 */
static uint32_t exec_stack_window(uint32_t bottom, uint32_t limit)
{
    uint32_t size = limit & ~(uint32_t)(PAGE_SIZE - 1);
    uint32_t floor;

    if (size > USER_ADDR_MAX - VM_MMAP_BASE) {
        size = USER_ADDR_MAX - VM_MMAP_BASE;
    }
    floor = USER_ADDR_MAX - size;
    if (floor > bottom) {
        floor = bottom;
    }
    return USER_STACK_BASE - floor;
}

/**
 * Count the pointers of the user vector `vec` before its NULL into `*n`, as
 * Linux's `count` counts them; a NULL `vec` has none. More than `max` is
 * -E2BIG, found without reading on to the end, and a pointer that cannot be
 * read is -EFAULT.
 */
static int32_t exec_count(char *const __user *vec, uint32_t max, uint32_t *n)
{
    uint32_t i;

    *n = 0;
    if (vec == NULL) {
        return 0;
    }
    for (i = 0;; i++) {
        uint32_t ptr;

        if (copy_from_user(&ptr, (const void __user *)&vec[i], sizeof(ptr)) != 0) {
            return -EFAULT;
        }
        if (ptr == 0) {
            *n = i;
            return 0;
        }
        if (i == max) {
            return -E2BIG;
        }
        if ((i & 1023u) == 1023u) {
            sched_service_point();
        }
    }
}

/**
 * Copy the `n` strings of the user vector `vec` onto the new stack `u`, the
 * last first, as Linux's `copy_strings` copies them: each pointer is read
 * again, and a string that cannot be read is -EFAULT, one past the length
 * limit or the budget -E2BIG.
 */
static int32_t exec_copy_strings(struct uimage *u, char *const __user *vec, uint32_t n)
{
    while (n > 0) {
        uint32_t ptr;
        int32_t rc;

        n--;
        if (copy_from_user(&ptr, (const void __user *)&vec[n], sizeof(ptr)) != 0) {
            return -EFAULT;
        }
        rc = uimage_push_user(u, (const char __user *)ptr);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/**
 * Copy the program's path and the caller's vectors onto the new stack in
 * `s`, and count them into `*argc` and `*envc`, as Linux's
 * `do_execveat_common` does.
 *
 * Both vectors are counted first, and their pointers are measured against
 * the budget `uimage_start` sets from the soft RLIMIT_STACK. The strings
 * follow, the path the caller passed at the top, then the environment and
 * then the arguments, each vector last string first. An empty argv gets one
 * empty string, so a program that starts reading at argv[1] does not read
 * the environment. 0, -E2BIG, -EFAULT or -ENOMEM.
 */
static int32_t exec_copy_args(struct exec_state *s, const char __user *user_path,
                              char *const __user *user_argv, char *const __user *user_envp,
                              uint32_t *argc, uint32_t *envc)
{
    uint32_t limit = current_proc->rlim[RLIM_STACK].cur;
    uint32_t max = uimage_budget(limit) / 4u;
    int32_t rc;

    rc = exec_count(user_argv, max, argc);
    if (rc == 0) {
        rc = exec_count(user_envp, max, envc);
    }
    if (rc == 0) {
        rc = uimage_start(&s->args, USER_ADDR_MAX, limit, *argc, *envc);
    }
    if (rc == 0) {
        rc = uimage_push_user(&s->args, user_path);
    }
    if (rc == 0) {
        rc = exec_copy_strings(&s->args, user_envp, *envc);
    }
    if (rc == 0) {
        rc = exec_copy_strings(&s->args, user_argv, *argc);
    }
    if (rc == 0 && *argc == 0) {
        rc = uimage_push(&s->args, "");
        *argc = 1;
    }
    return rc;
}

/**
 * Open the file at the absolute, canonical `path` into `f` for execve to
 * read, and when `id` is not NULL, say in it what the file's set-id bits
 * would do. `path` starts at the caller's root, which chroot may have
 * moved, and a file it names on the KRFFS root is read by its node's own
 * path, which `f` keeps.
 *
 * The walk may not arrive on the synthetic /dev and /proc mounts, which
 * hold no programs, so execve sees the mount boundary open sees and cannot
 * reach a file a hostile image placed below the synthetic prefix; a mount
 * with MS_NOEXEC holds none either. The caller needs the execute right,
 * root too. The set-id facts are taken while the node is at hand and apply
 * only once the exec commits. Setgid counts only with the group-execute
 * bit beside it, which is Linux's rule: without it a 02700 file owned
 * 1000:0 passes the execute check on its owner bits, since the check judges
 * one identity class and the owner's first, and would hand its executor
 * egid 0. Setgid without group-execute is the mark of mandatory locking in
 * Unix, which nothing here implements. On a mount with MS_NOSUID the bits
 * are read and ignored. 0 or a negative errno.
 */
static int32_t exec_open(struct exec_file *f, const char *path, struct exec_setid *id)
{
    struct vfs_ref ref;
    struct vfs_stat vst;
    int nosuid;
    int32_t rc;

    f->ref.mnt = NULL;
    f->handle = NULL;
    f->err = 0;
    rc = vfs_resolve(0, path + (path[0] == '/'), VFS_WALK_LOCK, &ref, 0, 0);
    if (rc != 0) {
        return rc;
    }
    nosuid = (ref.mnt->flags & VFS_MS_NOSUID) != 0;
    if (vfs_synth_vpath(ref.mnt, ref.node) || (ref.mnt->flags & VFS_MS_NOEXEC)) {
        rc = -EACCES;
    } else if (mnt_is_root(ref.mnt)) {
        const krffs_node_t *node = ref.node;

        rc = cred_access_node(node, CRED_X, 0);
        if (rc == 0 && krffs_node_path(ref.node, f->path, sizeof(f->path)) != 0) {
            rc = -ENAMETOOLONG;
        }
        vst.mode = node->mode;
        vst.uid = node->uid;
        vst.gid = node->gid;
    } else {
        rc = cred_access_ref(&ref, CRED_X, 0);
        if (rc == 0) {
            rc = vfs_getattr(ref.mnt, ref.node, &vst);
        }
        if (rc == 0) {
            rc = vfs_open_node(ref.mnt, ref.node, 0, &f->handle);
        }
        if (rc == 0) {
            f->ref = ref; /* held until exec_file_close */
        }
    }
    vfs_walk_unlock(ref.mnt);
    if (f->ref.mnt == NULL) {
        vfs_ref_release(&ref);
    }
    if (rc == 0 && id != NULL) {
        id->mode = vst.mode & KRFFS_MODE_SUID;
        if ((vst.mode & KRFFS_MODE_SGID) && (vst.mode & 010u)) {
            id->mode |= KRFFS_MODE_SGID;
        }
        if (nosuid) {
            id->mode = 0;
        }
        id->uid = vst.uid;
        id->gid = vst.gid;
    }
    return rc;
}

/**
 * Turn an exec of a script into an exec of its interpreter, when the file
 * `s` has open is not an ELF image: `exec_shebang` rewrites the arguments,
 * and the interpreter is opened in the script's place. A set-id script runs
 * as its invoker: the interpreter is what runs, it is not set-id, and
 * honouring the script's bits is a hole Unix closed long ago, since the
 * interpreter can be made to read a different file than the one whose bits
 * were checked. One level only: an interpreter that is itself a script is
 * refused. 0, -ENOEXEC, or an error of the open or the read.
 */
static int32_t exec_script(struct exec_state *s)
{
    Elf32_Ehdr ehdr;
    int32_t rc;

    if (exec_file_read(&s->prog, &ehdr, sizeof(ehdr), 0) == (int)sizeof(ehdr) &&
        elf_valid_exec(&ehdr, sizeof(ehdr))) {
        return 0;
    }
    if (s->prog.err) {
        return s->prog.err;
    }
    rc = exec_shebang(s, &s->argc, exec_file_read, &s->prog);
    exec_file_close(&s->prog);
    if (rc <= 0) {
        return rc < 0 ? rc : -ENOEXEC;
    }
    s->id.mode = 0;
    rc = exec_open(&s->prog, s->path, NULL);
    if (rc != 0) {
        return rc;
    }
    if (exec_file_read(&s->prog, &ehdr, sizeof(ehdr), 0) != (int)sizeof(ehdr) ||
        !elf_valid_exec(&ehdr, sizeof(ehdr))) {
        return s->prog.err ? s->prog.err : -ENOEXEC;
    }
    return 0;
}

/**
 * The ids the new image runs with, into `s`: the file owner's where a
 * set-id bit applies, the way privilege is regained, and the caller's
 * effective ids otherwise. A process that set no_new_privs gains nothing:
 * its bits are read and ignored, as a nosuid mount's are. `secure`, which
 * is AT_SECURE, says whether the image runs with authority its invoker
 * lacks, the effective id differing from the real one either way, so libc
 * distrusts the environment the caller gave it.
 */
static void exec_ids(struct exec_state *s)
{
    uint32_t mode = current_proc->no_new_privs ? 0u : s->id.mode;

    s->euid = (mode & KRFFS_MODE_SUID) ? s->id.uid : current_proc->euid;
    s->egid = (mode & KRFFS_MODE_SGID) ? s->id.gid : current_proc->egid;
    s->secure = s->euid != current_proc->ruid || s->egid != current_proc->rgid;
}

/**
 * Load the interpreter the program's PT_INTERP names into the new address
 * space, as Linux's `load_elf_interp` loads it: a position-independent one,
 * which every dynamic linker is, at the top of the range mmap places
 * mappings in, just below VM_MMAP_BASE, and one with fixed addresses at
 * those. Its own PT_INTERP is ignored. The new image starts at its entry,
 * and `*base`, AT_BASE, is the distance it was moved by. 0, -ENOENT,
 * -EACCES, -ELIBBAD for a file that is not an interpreter this kernel
 * loads, -ENOMEM for one that would cover the program, or an error of the
 * read.
 */
static int32_t exec_interp(struct exec_state *s, uint32_t *base)
{
    uint32_t bias = 0;
    int32_t rc;

    if (canonicalize_abs(s->layout.interp) < 0) {
        return -ENOENT;
    }
    rc = exec_open(&s->prog, s->layout.interp, NULL);
    if (rc != 0) {
        return rc;
    }
    if (elf_read_layout(exec_file_read, &s->prog, &s->ilayout) != 0) {
        return s->prog.err ? s->prog.err : -ELIBBAD;
    }
    if (s->ilayout.type == ET_DYN) {
        bias = VM_MMAP_BASE - (s->ilayout.hi - s->ilayout.lo) - s->ilayout.lo;
    }
    if (s->ilayout.lo + bias < s->img.image_end && s->ilayout.hi + bias > s->img.segs[0].start) {
        return -ENOMEM;
    }
    if (elf_load_image_at(exec_file_read, &s->prog, s->mm->dir, bias, &s->iimg) != 0) {
        return s->prog.err ? s->prog.err : -ELIBBAD;
    }
    exec_file_close(&s->prog);
    s->entry = s->iimg.entry;
    *base = bias;
    return 0;
}

/**
 * Build the new image in an address space of its own while the caller's
 * still runs, as Linux's `load_elf_binary` builds one: the program, at
 * ELF_ET_DYN_BASE when it is position-independent; the interpreter its
 * PT_INTERP names; the regions; and the stack, with the pointers and the
 * auxiliary vector below its strings. The stack may grow to the soft
 * RLIMIT_STACK, lowered to 8 MiB for an image that runs with ids its
 * caller lacks, and the regions may cover no more than RLIMIT_AS. The
 * caller's image is untouched, so every failure is an error execve
 * returns. 0, -ENOEXEC, -ENOMEM, or an error of `exec_interp` or of a read.
 */
static int32_t exec_build(struct exec_state *s)
{
    struct uimage_aux aux;
    uint32_t bias = 0;
    uint32_t bottom = uimage_bottom(&s->args);
    uint32_t limit = current_proc->rlim[RLIM_STACK].cur;
    uint8_t random16[16];
    int32_t rc;

    if (elf_read_layout(exec_file_read, &s->prog, &s->layout) != 0) {
        return s->prog.err ? s->prog.err : -ENOEXEC;
    }
    if (s->layout.type == ET_DYN && s->layout.lo < ELF_ET_DYN_BASE) {
        bias = ELF_ET_DYN_BASE - s->layout.lo;
    }
    s->mm = mm_new();
    if (s->mm == NULL) {
        return -ENOMEM;
    }
    if (elf_load_image_at(exec_file_read, &s->prog, s->mm->dir, bias, &s->img) != 0) {
        return s->prog.err ? s->prog.err : -ENOEXEC;
    }
    exec_file_close(&s->prog);
    s->entry = s->img.entry;
    aux.base = 0;
    if (s->layout.interp[0] != '\0') {
        rc = exec_interp(s, &aux.base);
        if (rc != 0) {
            return rc;
        }
    }
    if (s->secure && limit > RLIM_STACK_DEFAULT) {
        limit = RLIM_STACK_DEFAULT;
    }
    if (elf_setup_vmas(&s->mm->vm, &s->img, USER_STACK_BASE, exec_stack_window(bottom, limit)) !=
            0 ||
        (s->layout.interp[0] != '\0' && elf_add_vmas(&s->mm->vm, &s->iimg) != 0) ||
        (bottom < USER_STACK_BASE && !vm_grow_stack(&s->mm->vm, bottom, RLIM_INFINITY32)) ||
        vm_total(&s->mm->vm) > current_proc->rlim[RLIM_AS].cur) {
        return -ENOMEM;
    }
    aux.uid = current_proc->ruid;
    aux.euid = s->euid;
    aux.gid = current_proc->rgid;
    aux.egid = s->egid;
    aux.secure = s->secure;
    krandom_bytes(random16, 16);
    if (uimage_write(&s->args, s->argc, s->envc, &s->img, random16, &aux) != 0 ||
        uimage_map(&s->args, s->mm->dir) != 0) {
        return -ENOMEM;
    }
    s->mm->arg_start = s->args.arg_start;
    s->mm->arg_end = s->args.env_start;
    s->mm->env_start = s->args.env_start;
    s->mm->env_end = s->args.env_end;
    k_memcpy(s->mm->auxv, s->args.auxv, sizeof(s->args.auxv));
    mm_set_exe(s->mm, s->path);
    return 0;
}

/** Whether the execve in the group `arg` is the group's only live
 * thread. */
static int exec_alone(void *arg)
{
    return ((const struct tgroup *)arg)->live == 1;
}

/**
 * Give the current thread the place of its process's leader, whose record,
 * that of a thread that has ended, goes: the thread takes the leader's id,
 * which is the process id, with what the process keeps in the leader's
 * record, its reaped children's time and its alarm, as Linux's `de_thread`
 * exchanges the two ids.
 */
static void exec_take_leader(void)
{
    struct tgroup *tg = current_proc->tg;
    process_t *old = tg->leader;

    current_proc->pid = old->pid;
    current_proc->cutime = old->cutime;
    current_proc->cstime = old->cstime;
    current_proc->alarm_tick = old->alarm_tick;
    current_proc->alarm_interval = old->alarm_interval;
    current_proc->jc_status = old->jc_status;
    tg->utime += old->utime;
    tg->stime += old->stime;
    tg->leader = current_proc;
    mm_drop(old);
    kstack_free(old->kernel_stack);
    free_process(old);
}

/**
 * Make the current thread its process's only one before execve replaces the
 * image, as Linux's `de_thread` does: every other thread is sent SIGKILL and
 * the caller waits, whatever it is sent, until they have ended; a caller
 * that is not the leader then takes the leader's place. While `exec_task`
 * is set, a thread the SIGKILL ends ends alone and does not make the
 * process end.
 */
static void exec_one_thread(void)
{
    struct tgroup *tg = current_proc->tg;
    struct k_siginfo info;
    process_t *t;

    if (tg->live > 1) {
        tg->exec_task = current_proc;
        k_memset(&info, 0, sizeof(info));
        info.code = SI_KERNEL;
        for (t = proc_list; t; t = t->next) {
            if (t->tg == tg && t != current_proc && t->state != PROC_ZOMBIE) {
                signal_post_thread(t, SIGKILL, &info);
            }
        }
        (void)wq_wait_event_nointr(&tg->gone, exec_alone, tg);
        tg->exec_task = NULL;
    }
    if (current_proc != tg->leader) {
        exec_take_leader();
    }
}

/**
 * Make the image `s` built the process's; past here execve cannot fail.
 *
 * Ends the process's other threads first, and releases the robust futexes
 * the thread holds and clears the word CLONE_CHILD_CLEARTID names while the
 * old image is still there to hold them. Drops the close-on-exec
 * descriptors; takes the new ids, the saved ids becoming the new effective
 * ones, which is the POSIX rule for every exec and what lets an image shed
 * acquired authority and take it back; resets what pointed into the old
 * image: caught signals' handlers (SIG_IGN, the blocked mask and the pending
 * signals are kept, as POSIX says), the alternate signal stack, the TLS base
 * and the floating-point state. Then it switches to the new address space
 * and puts the old one, which the parent of a vfork child keeps, lets that
 * parent go, and points the return frame at the entry with the stack
 * pointer at argc. A fresh image has no TLS until its libc calls
 * set_thread_area, so the segment registers get the plain user-data
 * selector, as a new process gets it, in place of the old image's TLS one.
 * The general registers are cleared, since the SysV i386 entry has argc on
 * the stack and %edx an atexit hook or zero, and %ebp 0 marks the
 * outermost frame.
 */
static void exec_commit(isr_params_t *params, struct exec_state *s)
{
    struct mm *old = current_proc->mm;
    uint32_t old_euid = current_proc->euid;
    uint32_t old_egid = current_proc->egid;
    int sig;

    exec_one_thread();
    proc_thread_gone(current_proc);
    close_cloexec_fds();
    current_proc->euid = s->euid;
    current_proc->egid = s->egid;
    current_proc->suid = s->euid;
    current_proc->sgid = s->egid;
    exec_privileges(s->secure, old_euid, old_egid);
    proc_set_comm(current_proc, s->path);
    for (sig = 1; sig <= KSIG_MAX; sig++) {
        if (current_proc->sigacts[sig].handler != KSIG_DFL &&
            current_proc->sigacts[sig].handler != KSIG_IGN) {
            k_memset(&current_proc->sigacts[sig], 0, sizeof(struct k_sigaction));
        }
    }
    current_proc->clear_child_tid = 0;
    current_proc->altstack_on = 0;
    current_proc->altstack_sp = 0;
    current_proc->altstack_size = 0;
    current_proc->vma_reported = 0;

    current_proc->mm = s->mm;
    current_proc->cr3 = s->mm->cr3;
    current_proc->page_dir = s->mm->dir;
    s->mm = NULL;
    switch_page_directory(current_proc->page_dir);
    mm_put(old);
    proc_vfork_release(current_proc);

    params->eip = s->entry;
    params->user_esp = s->args.esp;
    params->ebp = 0;
    params->edx = 0;
    params->ebx = 0;
    params->ecx = 0;
    params->esi = 0;
    params->edi = 0;
    params->gs = 0x23;
    params->fs = 0x23;
    params->es = 0x23;
    params->ds = 0x23;
    current_proc->tls_base = 0;
    gdt_set_tls(0);
    fpu_init_area(current_proc->fpu);
    fpu_restore(current_proc->fpu);
}

/**
 * Replace the current process image with a new program, in the workspace
 * `s`; the body of `sys_execve`.
 *
 * EBX = path, ECX = argv[], EDX = envp[]. Does not return on success.
 * Resolves the path and checks the execute right, copies the path, argv and
 * envp onto the new stack's pages, resolves a `#!` script to its
 * interpreter, builds the new image, with the dynamic linker it names, in
 * an address space of its own, and only then commits, so every refusal and
 * every failure of the load returns to the caller with its image intact.
 */
static int32_t exec_run(isr_params_t *params, struct exec_state *s)
{
    const char __user *user_path = (const char __user *)params->ebx;
    int32_t rc;

    if (!user_path) {
        return -EFAULT;
    }
    rc = copy_path_cwd(s->path, sizeof(s->path), user_path);
    if (rc < 0) {
        return rc;
    }
    rc = exec_open(&s->prog, s->path, &s->id);
    if (rc == 0) {
        rc = exec_copy_args(s, user_path, (char *const __user *)params->ecx,
                            (char *const __user *)params->edx, &s->argc, &s->envc);
    }
    if (rc == 0) {
        rc = exec_script(s);
    }
    if (rc == 0) {
        rc = uimage_place(&s->args, s->argc, s->envc);
    }
    if (rc == 0) {
        exec_ids(s);
        rc = exec_build(s);
    }
    if (rc == 0) {
        uint32_t tid = current_proc->pid;

        exec_commit(params, s);
        ptrace_exec(tid);
    }
    return rc;
}

/**
 * Replace the current process image with a new program (execve(2)).
 *
 * Gives the call a workspace of its own, which `exec_run` does the work in,
 * and gives it back when that returns, whether the exec failed or the new
 * image is about to be entered.
 */
int32_t sys_execve(isr_params_t *params)
{
    struct exec_state *s;
    int32_t rc;

    mem_balance();
    if (!current_proc) {
        return -ESRCH;
    }
    s = kzalloc(sizeof(*s));
    if (!s) {
        return -ENOMEM;
    }
    current_proc->exec_ws = s;
    rc = exec_run(params, s);
    exec_release();
    return rc;
}

/* clone(2) flags; the low byte is the signal the parent gets at the child's
 * exit. */
#define CSIGNAL              0x000000FFu
#define CLONE_VM             0x00000100u
#define CLONE_FS             0x00000200u
#define CLONE_FILES          0x00000400u
#define CLONE_SIGHAND        0x00000800u
#define CLONE_VFORK          0x00004000u
#define CLONE_PARENT         0x00008000u
#define CLONE_THREAD         0x00010000u
#define CLONE_SYSVSEM        0x00040000u
#define CLONE_SETTLS         0x00080000u
#define CLONE_PARENT_SETTID  0x00100000u
#define CLONE_CHILD_CLEARTID 0x00200000u
#define CLONE_DETACHED       0x00400000u
#define CLONE_CHILD_SETTID   0x01000000u

/* What a fork or a clone asks of `do_fork`: the clone flags, the stack
 * pointer the child starts with or 0 for the parent's, the parent's address
 * CLONE_PARENT_SETTID names, the child's addresses CLONE_CHILD_SETTID and
 * CLONE_CHILD_CLEARTID name, each 0 for none, and the TLS base
 * CLONE_SETTLS gives the child. */
struct fork_req {
    uint32_t flags;
    uint32_t newsp;
    uint32_t ptid;
    uint32_t settid;
    uint32_t cleartid;
    uint32_t tls;
};

/* The parents that wait in vfork until their child calls execve or ends. */
static wait_queue_t vfork_wq;

/** Whether the vfork wait for the child `*arg` is over: the child has let
 * its parent go or is gone, or the parent has SIGKILL pending. */
static int vfork_done(void *arg)
{
    const process_t *child = get_process(*(const uint32_t *)arg);

    return child == NULL || !child->vfork_waiting ||
           ((current_proc->sig_pending | current_proc->tg->pending) & KSIG_BIT(SIGKILL)) != 0;
}

/**
 * Wait, as the parent, until the child `pid` made with CLONE_VFORK lets the
 * parent go. The child runs on the parent's memory, its stack included,
 * until it calls execve or ends, so the parent must not run before. Other
 * signals wait for the end as well; only SIGKILL cuts it short, as Linux's
 * killable wait does, and the parent then dies on the way out.
 */
void proc_vfork_wait(uint32_t pid)
{
    (void)wq_wait_event_nointr(&vfork_wq, vfork_done, &pid);
}

void proc_vfork_release(process_t *p)
{
    if (p->vfork_waiting) {
        p->vfork_waiting = 0;
        wq_wake_all(&vfork_wq);
    }
}

/**
 * The address space of the child `do_fork` makes: `mm` itself, with one
 * more reference, under CLONE_VM, and otherwise a copy-on-write copy of its
 * pages and regions; an empty one for a kernel thread's child. NULL when no
 * memory was left for a copy.
 */
static struct mm *fork_mm(struct mm *mm, uint32_t flags)
{
    struct mm *copy;

    if ((flags & CLONE_VM) && mm) {
        mm_get(mm);
        return mm;
    }
    copy = mm_new();
    if (copy && mm && (copy_user_pages(mm, copy->dir) != 0 || vm_copy(&copy->vm, &mm->vm) != 0)) {
        mm_put(copy);
        return NULL;
    }
    if (copy && mm) {
        mm_copy_info(copy, mm);
    }
    return copy;
}

/**
 * Create a child process: fork(2), vfork(2), and the part of clone(2) this
 * kernel has.
 *
 * Clones the current process: PCB, address space, and fd table. The address
 * space is a copy-on-write copy, or under CLONE_VM the parent's own,
 * shared. The child is set up to return from fork with value 0; the parent
 * gets the child's PID, after the child has called execve or ended when
 * `req` has CLONE_VFORK. Returns -errno on failure. `req` has the stack
 * pointer the child starts with and the addresses of the ids clone stores:
 * the parent's CLONE_PARENT_SETTID word, stored before the child runs, and
 * the child's CLONE_CHILD_SETTID word and the CLONE_CHILD_CLEARTID address
 * its thread end clears.
 *
 * Among what the child inherits is the program name, which `execve` sets and a
 * child that never execs would otherwise never have: a daemon that forks
 * itself into the background is exactly such a child, and without this it
 * shows in `ps` and in /proc/<pid>/stat with no name for as long as it runs.
 */
static int32_t do_fork(isr_params_t *params, const struct fork_req *req)
{
    process_t *parent = current_proc;
    process_t *child;
    uint32_t pid;
    uintptr_t child_kstack_base;
    uint32_t *child_sp;
    isr_params_t *child_frame;
    int event;

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

    child->mm = fork_mm(parent->mm, req->flags);
    if (!child->mm) {
        free_process(child);
        return -ENOMEM;
    }
    child->cr3 = child->mm->cr3;
    child->page_dir = child->mm->dir;

    child_kstack_base = kstack_alloc();
    if (!child_kstack_base) {
        mm_drop(child);
        free_process(child);
        return -ENOMEM;
    }
    child->kernel_stack = child_kstack_base + KERNEL_STACK_SIZE;
    /* A thread's parent, and that of a CLONE_PARENT child, is the caller's
     * process's parent. */
    child->parent_pid =
        (req->flags & (CLONE_THREAD | CLONE_PARENT)) ? parent->parent_pid : parent->tgid;

    /* Inherit the parent's session and process group: a forked child stays in
     * the parent's job-control group until it (or the parent) calls
     * setsid/setpgid. alloc_process defaulted these to the child's own pid. */
    child->sid = parent->sid;
    child->pgid = parent->pgid;
    child->umask = parent->umask; /* the file-creation mask is inherited */
    child->nice = parent->nice;
    child->persona = parent->persona;
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
    /* The working directory's hold on its mount is the child's too, and so
     * is the root chroot gave the parent. */
    child->cwd_mnt = NULL;
    child->cwd_node = NULL;
    vfs_cwd_set(child, parent->cwd_mnt, parent->cwd_node);
    vfs_root_copy(child, parent);

    /* Inherit the parent's TLS base: the child's copied trapframe keeps the
     * parent's %gs selector, and its address space is a copy at the same
     * virtual addresses, so the same base is correct. Without this the context
     * switch would reprogram the TLS descriptor to 0 for the child and its
     * %gs-relative reads (errno, the stack canary) would fault or read garbage.
     */
    child->tls_base = (req->flags & CLONE_SETTLS) ? req->tls : parent->tls_base;
    /* The floating-point registers as they are now, since the parent's record
     * holds only what they were at its last switch away. */
    fpu_save(child->fpu);

    /* The child inherits the parent's current working directory. */
    if (proc_cwd_copy(child, parent) != 0) {
        goto fail_built;
    }

    /* Signals: the child inherits the dispositions and the blocked mask, but
     * its pending set starts empty (POSIX fork semantics). */
    k_memcpy(child->tg->action, parent->tg->action, sizeof(child->tg->action));
    child->sig_blocked = parent->sig_blocked;
    child->sig_pending = 0;
    /* A child that shares the memory and runs beside the parent does not
     * share its alternate stack, as Linux's does not. */
    if ((req->flags & (CLONE_VM | CLONE_VFORK)) != CLONE_VM) {
        child->altstack_sp = parent->altstack_sp;
        child->altstack_size = parent->altstack_size;
        child->altstack_on = parent->altstack_on;
    }

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
    if (req->newsp) {
        child_frame->user_esp = req->newsp;
    }
    /* A failed store leaves the word as the parent had it, as Linux's does. */
    if (req->settid) {
        (void)paging_store_u32(child->mm->dir, &child->mm->vm, req->settid, child->pid);
    }
    child->clear_child_tid = req->cleartid;
    pid = child->pid;
    if (req->ptid) {
        (void)copy_to_user((void __user *)req->ptid, &pid, sizeof(pid));
    }

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
    if (req->flags & CLONE_THREAD) {
        tgroup_put(child->tg);
        child->tg = parent->tg;
        child->tg->refs++;
        child->tg->live++;
        child->tgid = parent->tgid;
        child->sigacts = child->tg->action;
    }
    if (req->flags & CLONE_VFORK) {
        child->vfork_waiting = 1;
    }
    event = ptrace_fork_attach(child, req->flags);
    child->state = PROC_READY;
    if (event) {
        ptrace_event(event, pid);
    }
    if (req->flags & CLONE_VFORK) {
        proc_vfork_wait(pid);
        ptrace_event(PTRACE_EVENT_VFORK_DONE, pid);
    }
    return (int32_t)pid;

fail_built:
    /* Everything built for the child so far goes back: the working
     * directory's and the root's holds, the kernel stack and the address
     * space, then the record. */
    vfs_cwd_clear(child);
    vfs_root_clear(child);
    kstack_free(child->kernel_stack);
    mm_drop(child);
    free_process(child);
    return -ENOMEM;
}

int32_t sys_fork(isr_params_t *params)
{
    struct fork_req req = {SIGCHLD, 0, 0, 0, 0, 0};

    return do_fork(params, &req);
}

/**
 * vfork (190): a child that runs on the parent's memory while the parent
 * waits, until the child calls execve or ends. musl's vfork and glibc's call
 * it; the child must do nothing else before one of the two, since every
 * store it makes is the parent's.
 */
int32_t sys_vfork(isr_params_t *params)
{
    struct fork_req req = {CLONE_VM | CLONE_VFORK | SIGCHLD, 0, 0, 0, 0, 0};

    return do_fork(params, &req);
}

/**
 * clone (120), in the forms glibc's and musl's fork, posix_spawn and
 * pthread_create use: a new process with its own copy of the address space,
 * files and signal handlers, as fork makes one; with the caller's address
 * space itself under CLONE_VM, and with the caller waiting as vfork waits
 * under CLONE_VFORK; or a new thread of the caller's process under
 * CLONE_THREAD, which needs CLONE_SIGHAND, which in turn needs CLONE_VM.
 *
 * i386 argument order: EBX=flags, ECX=new stack or 0, EDX=parent tid address,
 * ESI=TLS descriptor, EDI=child tid address. The exit signal must be SIGCHLD
 * for a process; a thread sends none, whatever the low byte says.
 * CLONE_SETTLS gives the child the TLS base of the user_desc at ESI, which
 * set_thread_area would take. CLONE_PARENT makes the child a child of the
 * caller's parent. CLONE_PARENT_SETTID stores the child's id in the
 * parent's memory before the child runs, and a failed store is ignored, as
 * Linux ignores it. CLONE_SYSVSEM and CLONE_DETACHED are ignored, as there
 * are no System V semaphores and Linux ignores the second. Any other flag is
 * refused with EINVAL.
 */
int32_t sys_clone(isr_params_t *params)
{
    uint32_t flags = params->ebx;
    struct fork_req req;

    if ((flags & ~(CSIGNAL | CLONE_VM | CLONE_SIGHAND | CLONE_VFORK | CLONE_PARENT |
                   CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID |
                   CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | CLONE_DETACHED)) != 0 ||
        (!(flags & CLONE_THREAD) && (flags & CSIGNAL) != SIGCHLD) ||
        ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM)) ||
        ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND)) ||
        ((flags & CLONE_SIGHAND) && !(flags & CLONE_THREAD))) {
        return -EINVAL;
    }
    req.tls = 0;
    if (flags & CLONE_SETTLS) {
        user_desc_t desc;

        if (copy_from_user(&desc, (const void __user *)params->esi, sizeof(desc)) != 0) {
            return -EFAULT;
        }
        if (tls_resolve_base(&desc, &req.tls) != 0) {
            return -EINVAL;
        }
    }
    req.flags = flags;
    req.newsp = params->ecx;
    req.ptid = (flags & CLONE_PARENT_SETTID) ? params->edx : 0;
    req.settid = (flags & CLONE_CHILD_SETTID) ? params->edi : 0;
    req.cleartid = (flags & CLONE_CHILD_CLEARTID) ? params->edi : 0;
    return do_fork(params, &req);
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
 * the CPU time of the child process, every thread's, with that of the
 * children it has reaped, which its leader's record keeps.
 */
static void rusage_of_child(struct kernel_rusage *ru, const process_t *p)
{
    uint32_t user;
    uint32_t sys;

    proc_cpu_time(p, &user, &sys);
    rusage_from_ticks(ru, user + p->cutime, sys + p->cstime);
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
 * Whether `p` is a child of the caller's process that `idtype` and `id`
 * select: the leader of a process whose parent is any thread of the
 * caller's, since the children of a process are all its threads'.
 *
 * Linux's __WCLONE asks for the children that report their end with a signal
 * other than SIGCHLD, unless __WALL is beside it, and every child here
 * reports with SIGCHLD, so it selects none.
 */
static int wait_selects(int idtype, uint32_t id, int options, const process_t *p)
{
    if (p->parent_pid != current_proc->tgid || p->pid != p->tgid) {
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
 * Reap zombie `p`, the leader of a process whose threads have all ended: add
 * the process's CPU time, with its reaped children's, to the children's
 * totals of the caller's process, which its leader keeps, and give back its
 * address space, kernel stack and record. The zombie no longer runs on its
 * kernel stack, and this is the single release point for a per-fork stack.
 */
static void wait_reap(process_t *p)
{
    process_t *me = current_proc->tg->leader;
    uint32_t user;
    uint32_t sys;

    proc_cpu_time(p, &user, &sys);
    me->cutime += user + p->cutime;
    me->cstime += sys + p->cstime;
    mm_drop(p);
    kstack_free(p->kernel_stack);
    free_process(p);
}

/**
 * Whether the thread `p`, which the caller's process traces, is one `idtype`
 * and `id` select. A thread that is not its process's leader ends with no
 * signal to its tracer, so, as Linux's __WCLONE and __WALL have it, it is
 * waited for with __WCLONE or __WALL, and a leader without __WCLONE.
 */
static int wait_selects_tracee(int idtype, uint32_t id, int options, const process_t *p)
{
    int clone = p->pid != p->tgid;

    if (!(options & W_ALL) && clone != (((uint32_t)options & W_CLONE) != 0)) {
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

/** Fill `r` with the end of `p`, from the encoded status `status`. */
static void wait_fill_exit(struct wait_report *r, const process_t *p, int32_t status)
{
    if ((status & 0x7f) == 0) {
        wait_fill(r, p, status, CLD_EXITED, (status >> 8) & 0xff);
    } else {
        wait_fill(r, p, status, (status & 0x80) ? CLD_DUMPED : CLD_KILLED, status & 0x7f);
    }
}

/**
 * What a tracer's wait finds of its tracee `p`: a ptrace stop the wait has
 * not reported, reported once; a thread that ended while traced, which the
 * report frees, as no other wait reports it; or the end of a process whose
 * parent the tracer is not, reported once and left for the parent to reap.
 * With WNOWAIT nothing is marked or freed. Returns 1 with `out` filled, or 0.
 */
static int wait_tracee(process_t *p, int options, struct wait_report *out)
{
    if (p->ptrace_status != 0 && !p->ptrace_reported && p->state != PROC_ZOMBIE) {
        wait_fill(out, p, (int32_t)p->ptrace_status, CLD_TRAPPED,
                  (int32_t)((p->ptrace_status >> 8) & 0xFF));
        if (!(options & WNOWAIT)) {
            p->ptrace_reported = 1;
        }
        return 1;
    }
    if (p->state != PROC_ZOMBIE || !(options & WEXITED)) {
        return 0;
    }
    if (p->pid != p->tgid) {
        wait_fill_exit(out, p, p->exit_code);
        if (!(options & WNOWAIT)) {
            p->tg->utime += p->utime;
            p->tg->stime += p->stime;
            mm_drop(p);
            kstack_free(p->kernel_stack);
            free_process(p);
        }
        return 1;
    }
    if (p->tg->live == 0 && p->parent_pid != current_proc->tgid && !p->ptrace_exit_seen) {
        wait_fill_exit(out, p, p->exit_code);
        if (!(options & WNOWAIT)) {
            p->ptrace_exit_seen = 1;
        }
        return 1;
    }
    return 0;
}

/**
 * The shared core of waitpid, wait4 and waitid: find a child `idtype` and
 * `id` select that has an event `options` asks for, fill `out` with it and
 * return its pid. The threads the caller's process traces are waited for
 * too, as `wait_tracee` reports them.
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
            if ((p->ptrace & PT_TRACED) && p->tracer == current_proc->tgid &&
                wait_selects_tracee(idtype, id, options, p)) {
                have_child = 1;
                if (wait_tracee(p, options, out)) {
                    return (int32_t)out->pid;
                }
            }
            if (!wait_selects(idtype, id, options, p)) {
                continue;
            }
            have_child = 1;

            if (p->state == PROC_ZOMBIE && p->tg->live == 0 && (options & WEXITED)) {
                wait_fill_exit(out, p, p->exit_code);
                if (!(options & WNOWAIT)) {
                    wait_reap(p);
                }
                return (int32_t)out->pid;
            }
            if ((options & WUNTRACED) && p->tg->stop != 0 && (p->jc_status & 0xff) == 0x7f) {
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
         * otherwise block on the process's child queue until a child changes
         * state (an exit, stop or continue wakes the queue) or a signal
         * interrupts. Lost-wakeup-safe: the signal_pending check, joining the
         * queue and the PROC_BLOCKED transition are one interrupts-off step,
         * so a wake or a signal posted from an IRQ either is seen here or
         * finds us BLOCKED and wakes us. */
        if (options & WNOHANG) {
            return 0;
        }
        flags = sig_irq_save();
        if (signal_pending()) {
            sig_irq_restore(flags);
            return -EINTR;
        }
        wq_poll_add(&current_proc->tg->chld, &current_proc->waiter);
        current_proc->state = PROC_BLOCKED;
        sig_irq_restore(flags);
        schedule();
        flags = sig_irq_save();
        if (current_proc->state == PROC_BLOCKED) {
            idle_wait_irq_off(); /* idle until a waker, then re-scan */
        }
        wq_poll_remove(&current_proc->tg->chld, &current_proc->waiter);
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

/* The targets getrusage(2) takes: the caller's process, its reaped
 * children, and the calling thread. */
#define RUSAGE_SELF 0

#define RUSAGE_CHILDREN (-1)

#define RUSAGE_THREAD 1

/**
 * Resource usage (getrusage(2), syscall 77).
 *
 * EBX = who, ECX = the 72-byte i386 struct rusage. `ru_utime` and `ru_stime`
 * are the user and system time of the caller's process, every thread's, for
 * RUSAGE_SELF, the calling thread's for RUSAGE_THREAD, and the totals of the
 * children the process has reaped, each with the children those reaped, for
 * RUSAGE_CHILDREN: the ticks `times` reports, as timevals. The
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
    if (who == RUSAGE_SELF) {
        proc_cpu_time(current_proc, &user, &sys);
    } else {
        flags = kirq_save();
        user = who == RUSAGE_CHILDREN ? current_proc->tg->leader->cutime : current_proc->utime;
        sys = who == RUSAGE_CHILDREN ? current_proc->tg->leader->cstime : current_proc->stime;
        kirq_restore(flags);
    }
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

/* The stop signals, which SIGCONT cancels and which cancel SIGCONT. */
#define SIG_STOPS (KSIG_BIT(SIGSTOP) | KSIG_BIT(SIGTSTP) | KSIG_BIT(SIGTTIN) | KSIG_BIT(SIGTTOU))

/** Make `t` runnable if it sleeps, clearing its timed wake: an
 * interruptible wait then asks `signal_pending` whether to end with -EINTR,
 * and any wait that goes on arms its own deadline again. */
static void sig_wake(process_t *t)
{
    if (t && t->state == PROC_BLOCKED) {
        t->wakeup_tick = 0;
        t->state = PROC_READY;
    }
}

/** Clear the signals `bits` from the process of `target` and from each of
 * its threads. */
static void sig_discard(process_t *target, uint64_t bits)
{
    process_t *t;

    target->tg->pending &= ~bits;
    for (t = proc_list; t; t = t->next) {
        if (t->tg == target->tg) {
            t->sig_pending &= ~bits;
        }
    }
}

/**
 * What a signal to the process of `target` does before any thread takes it,
 * with interrupts off. SIGCONT cancels the pending stops and ends a stop in
 * effect: every stopped thread runs again, and the parent learns of it from
 * wait4(WCONTINUED) and a SIGCHLD. SIGKILL makes a stopped thread runnable,
 * so it can take its lethal default. A stop signal cancels a pending
 * SIGCONT. Every other signal stays pending while the process is stopped.
 */
static void sig_job_control(process_t *target, int signo)
{
    struct tgroup *tg = target->tg;
    process_t *t;

    if (signo == SIGCONT) {
        sig_discard(target, SIG_STOPS);
        if (tg->stop) {
            struct k_siginfo child;
            process_t *parent = get_process(tg->leader->parent_pid);

            tg->stop = 0;
            for (t = proc_list; t; t = t->next) {
                if (t->tg != tg) {
                    continue;
                }
                t->stop_seen = 0;
                if (t->state == PROC_STOPPED && t->ptrace_status == 0) {
                    t->state = PROC_READY;
                }
            }
            tg->leader->jc_status = 0xffff; /* WIFCONTINUED */
            if (parent) {
                k_memset(&child, 0, sizeof(child));
                child.code = CLD_CONTINUED;
                child.pid = tg->leader->pid;
                child.uid = tg->leader->ruid;
                child.status = SIGCONT;
                signal_post_info(parent, SIGCHLD, &child);
                wq_wake_all(&parent->tg->chld);
            }
        }
    } else if (signo == SIGKILL) {
        for (t = proc_list; t; t = t->next) {
            if (t->tg == tg && t->state == PROC_STOPPED) {
                t->state = PROC_READY;
            }
        }
    } else if (KSIG_BIT(signo) & SIG_STOPS) {
        sig_discard(target, KSIG_BIT(SIGCONT));
    }
}

/**
 * The thread of `target`'s process to wake for `signo`, as Linux's
 * `complete_signal` chooses it: `target` when it does not block the signal,
 * or else the first thread that does not; `target` itself when every thread
 * blocks it, and nothing when `target` has ended.
 */
static process_t *sig_taker(process_t *target, int signo)
{
    process_t *t;

    if (target->state != PROC_ZOMBIE && !(target->sig_blocked & KSIG_BIT(signo))) {
        return target;
    }
    for (t = proc_list; t; t = t->next) {
        if (t->tg == target->tg && t->state != PROC_ZOMBIE && t->state != PROC_UNUSED &&
            !(t->sig_blocked & KSIG_BIT(signo))) {
            return t;
        }
    }
    return target->state != PROC_ZOMBIE ? target : NULL;
}

/*
 * Post signal `signo` to the process `target` is a thread of, with the
 * siginfo `info`: set the process's pending bit, keeping `info` when the
 * signal was not already pending, and wake the thread `sig_taker` chooses,
 * whatever the signal's disposition and mask. Any thread that does not
 * block the signal may take it. A process whose threads have all ended
 * takes nothing. SIGKILL and SIGSTOP cannot be caught, so a pending SIGKILL
 * is honored even before delivery by leaving it pending for the default
 * path.
 */
void signal_post_info(process_t *target, int signo, const struct k_siginfo *info)
{
    struct tgroup *tg;
    uint32_t f;

    if (!target || signo < 1 || signo > KSIG_MAX || target->state == PROC_UNUSED) {
        return;
    }
    tg = target->tg;
    if (tg->live == 0) {
        return;
    }
    f = sig_irq_save();
    if (!(tg->pending & KSIG_BIT(signo))) {
        tg->info[signo] = *info;
    }
    tg->pending |= KSIG_BIT(signo);
    sig_job_control(target, signo);
    sig_wake(sig_taker(target, signo));
    sig_irq_restore(f);
}

void signal_post_thread(process_t *t, int signo, const struct k_siginfo *info)
{
    uint32_t f;

    if (!t || signo < 1 || signo > KSIG_MAX || t->state == PROC_ZOMBIE ||
        t->state == PROC_UNUSED) {
        return;
    }
    f = sig_irq_save();
    if (!(t->sig_pending & KSIG_BIT(signo))) {
        t->sig_info[signo] = *info;
    }
    t->sig_pending |= KSIG_BIT(signo);
    sig_job_control(t, signo);
    sig_wake(t);
    sig_irq_restore(f);
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
    signal_post_thread(target, signo, info);
}

/** The signals pending for the current thread: its own and its process's.
 * Read with interrupts off, since a 64-bit read is two loads. */
static uint64_t sig_pending_now(void)
{
    uint32_t flags = sig_irq_save();
    uint64_t set = current_proc->sig_pending | current_proc->tg->pending;

    sig_irq_restore(flags);
    return set;
}

/* The siginfo of a signal a process sends: `code` SI_USER for kill and
 * SI_TKILL for tkill and tgkill, with the sender's pid and real uid. */
static void sender_info(struct k_siginfo *info, int32_t code)
{
    k_memset(info, 0, sizeof(*info));
    info->code = code;
    info->pid = current_proc ? current_proc->tgid : 0;
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
    deliverable = (current_proc->sig_pending | current_proc->tg->pending) & ~current_proc->sig_blocked;
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

/* kill(2): pid > 0 a single process, which the id of any of its threads
 * names; pid == 0 the caller's process group; pid < -1 the group (-pid).
 * (pid == -1 "all processes" is not supported here.) signo 0 just probes
 * existence and permission. */
static int32_t do_kill(int pid, int signo)
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
    if (!target || target->state == PROC_UNUSED || target->tg->live == 0) {
        return -ESRCH;
    }
    if (!cred_may_signal(target, signo)) {
        return -EPERM;
    }
    if (signo == 0) {
        return 0; /* existence and permission check only */
    }
    sender_info(&info, SI_USER);
    signal_post_info(target, signo, &info);
    return 0;
}

/**
 * tkill and tgkill: send `signo` to the thread `tid` alone, which must be a
 * thread of the process `tgid` unless `tgid` is 0. Both ids are positive: a
 * thread id is never a kill(2) group selector, so a zero or negative one is
 * -EINVAL, not a group.
 */
static int32_t do_tkill(int tgid, int tid, int signo)
{
    process_t *t;
    struct k_siginfo info;

    if (tid <= 0 || tgid < 0 || signo < 0 || signo > KSIG_MAX) {
        return -EINVAL;
    }
    t = get_process((uint32_t)tid);
    if (!t || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE ||
        (tgid != 0 && t->tgid != (uint32_t)tgid)) {
        return -ESRCH;
    }
    if (!cred_may_signal(t, signo)) {
        return -EPERM;
    }
    if (signo == 0) {
        return 0;
    }
    sender_info(&info, SI_TKILL);
    signal_post_thread(t, signo, &info);
    return 0;
}

int32_t sys_kill(isr_params_t *params)
{
    return do_kill((int)params->ebx, (int)params->ecx);
}

/** tkill (238): EBX=tid, ECX=signal. */
int32_t sys_tkill(isr_params_t *params)
{
    return do_tkill(0, (int)params->ebx, (int)params->ecx);
}

/** tgkill (270): EBX=tgid, ECX=tid, EDX=signal; a tgid of 0 is -EINVAL. */
int32_t sys_tgkill(isr_params_t *params)
{
    if ((int)params->ebx <= 0) {
        return -EINVAL;
    }
    return do_tkill((int)params->ebx, (int)params->ecx, (int)params->edx);
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
        /* Installing SIG_IGN drops any already-pending instance, the
         * process's and each thread's, the way Linux does. Guard the pending
         * RMW against a concurrent IRQ signal_post. */
        if (act.handler == KSIG_IGN) {
            uint32_t f = sig_irq_save();
            sig_discard(current_proc, KSIG_BIT(signo));
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
    set = (current_proc->sig_pending | current_proc->tg->pending) & current_proc->sig_blocked;
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
 * Free the records of the threads that have ended and are not leaders,
 * which no wait reaps: their CPU time goes to their process, and their
 * address space reference, kernel stack and record go back. A record the
 * current process is not is never running, so its stack is free to take.
 */
static void reap_threads(void)
{
    process_t *p, *next;

    for (p = proc_list; p && proc_dead_threads != 0; p = next) {
        next = p->next;
        if (p != current_proc && p->state == PROC_ZOMBIE && p->pid != p->tgid) {
            p->tg->utime += p->utime;
            p->tg->stime += p->stime;
            mm_drop(p);
            kstack_free(p->kernel_stack);
            free_process(p);
            proc_dead_threads--;
        }
    }
}

/**
 * Stop the current thread, and with it its process, for the stop signal
 * `signo`: the first thread to stop records the stop for the parent's
 * wait4(WUNTRACED) and sends it SIGCHLD, and every thread parks as it next
 * returns to user mode, until a SIGCONT, or a SIGKILL to die, makes it
 * runnable. The real init (PID 1) is unstoppable, like Unix init: stopping
 * it would freeze the system with no reaper and no way back, so a
 * job-control stop aimed at it is dropped.
 */
static void sig_group_stop(int signo)
{
    struct tgroup *tg = current_proc->tg;
    process_t *parent;

    if (current_proc->tgid == init_pid && !debug_shell_is_home) {
        return;
    }
    if (tg->stop) {
        return;
    }
    tg->stop = (uint32_t)signo;
    tg->leader->jc_status = (uint32_t)(((signo & 0xff) << 8) | 0x7f);
    parent = get_process(tg->leader->parent_pid);
    if (parent) {
        struct k_siginfo child;

        k_memset(&child, 0, sizeof(child));
        child.code = CLD_STOPPED;
        child.pid = tg->leader->pid;
        child.uid = tg->leader->ruid;
        child.status = signo;
        signal_post_info(parent, SIGCHLD, &child);
        wq_wake_all(&parent->tg->chld);
    }
}

/**
 * Park the current thread while its process is stopped and no SIGKILL is
 * pending. The scheduler keeps a STOPPED thread off the processor (it only
 * flips RUNNING to READY); `sig_job_control` makes it READY again. `sti;hlt;
 * cli` from IF=0 is lost-wakeup-safe.
 */
static void sig_park(void)
{
    uint32_t flags = sig_irq_save();

    while (current_proc->tg->stop && !(sig_pending_now() & KSIG_BIT(SIGKILL))) {
        current_proc->state = PROC_STOPPED;
        schedule();
        if (current_proc->state == PROC_STOPPED) {
            idle_wait_irq_off();
        }
    }
    current_proc->state = PROC_RUNNING;
    sig_irq_restore(flags);
}

/** Take the lowest signal of `deliverable` off the current thread's pending
 * set, or else off its process's, into `*info`; the signal. */
static int sig_dequeue(uint64_t deliverable, struct k_siginfo *info)
{
    uint32_t flags;
    int signo;

    for (signo = 1; signo < KSIG_MAX; signo++) {
        if (deliverable & KSIG_BIT(signo)) {
            break;
        }
    }
    flags = sig_irq_save();
    if (current_proc->sig_pending & KSIG_BIT(signo)) {
        current_proc->sig_pending &= ~KSIG_BIT(signo);
        *info = current_proc->sig_info[signo];
    } else {
        current_proc->tg->pending &= ~KSIG_BIT(signo);
        *info = current_proc->tg->info[signo];
    }
    sig_irq_restore(flags);
    return signo;
}

/**
 * Act on a pending signal at the return-to-user boundary.
 *
 * Called from the ISR/IRQ return path just before iret, with the trapframe
 * about to be restored. If the current thread is returning to USER mode and
 * has a deliverable signal, one pending for it or for its process and not
 * blocked, act on the lowest-numbered one, the thread's own first: ignore it,
 * end the process (a default that terminates), stop the process, or build a
 * handler frame. A thread whose process is stopped parks here first. The
 * records of ended threads are freed here too, a point every running process
 * passes where none of them is in use.
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
    if (proc_dead_threads != 0) {
        reap_threads();
    }
    if (current_proc->ptrace) {
        ptrace_return_stops();
    }
    restart_nr = current_proc->restart_nr;
    current_proc->restart_nr = 0;

    for (;;) {
        uint64_t deliverable;
        struct k_sigaction *sa;
        struct k_siginfo info;
        uint64_t oldmask, newblock;
        uint32_t handler_flags;
        int signo;

        if (current_proc->tg->stop && !(sig_pending_now() & KSIG_BIT(SIGKILL)) &&
            !ptrace_group_stop((int)current_proc->tg->stop) && !current_proc->stop_seen) {
            sig_park();
        }
        deliverable = sig_pending_now() & ~current_proc->sig_blocked;
        if (deliverable == 0) {
            break;
        }
        signo = sig_dequeue(deliverable, &info);
        if (current_proc->ptrace) {
            /* The tracer sees the stop's system call, and may cancel its
             * restart through orig_eax. */
            current_proc->restart_nr = restart_nr;
            current_proc->orig_eax = restart_nr ? (int32_t)restart_nr - 1 : -1;
            signo = ptrace_signal_stop(signo, &info);
            restart_nr = current_proc->restart_nr;
            current_proc->restart_nr = 0;
            if (signo == 0) {
                continue;
            }
        }
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
                sig_group_stop(signo);
                continue; /* parks at the top, then re-checks pending signals */
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

/**
 * Wait, as rt_sigtimedwait does, for a signal of `set` to be pending for the
 * current thread or its process until the tick `deadline` when `timed`, and
 * take it: its siginfo goes to `uinfo` unless null, and its number is
 * returned. -EINTR for another signal the thread acts on, -EAGAIN at the
 * deadline, -EFAULT.
 */
static int32_t sigwait_set(uint64_t set, int timed, uint32_t deadline, void __user *uinfo)
{
    uint32_t flags = sig_irq_save();

    for (;;) {
        uint64_t hit = sig_pending_now() & set;

        if (hit) {
            struct k_siginfo info;
            uint8_t buf[128];
            int signo = sig_dequeue(hit, &info);

            current_proc->wakeup_tick = 0;
            sig_irq_restore(flags);
            if (uinfo) {
                sigframe_fill_siginfo(buf, signo, &info);
                if (copy_to_user(uinfo, buf, sizeof(buf)) != 0) {
                    return -EFAULT;
                }
            }
            return signo;
        }
        if (signal_pending() || (timed && (int32_t)(kernel_ticks - deadline) >= 0)) {
            current_proc->wakeup_tick = 0;
            sig_irq_restore(flags);
            return signal_pending() ? -EINTR : -EAGAIN;
        }
        current_proc->wakeup_tick = timed ? (deadline != 0 ? deadline : 1u) : 0;
        current_proc->state = PROC_BLOCKED;
        sig_irq_restore(flags);
        schedule();
        flags = sig_irq_save();
        if (current_proc->state == PROC_BLOCKED) {
            idle_wait_irq_off();
        }
        current_proc->state = PROC_RUNNING;
    }
}

/**
 * Wait for a signal of a set and take it (rt_sigtimedwait(2), syscall 177),
 * which sigwait, sigwaitinfo and sigtimedwait make.
 *
 * EBX = const sigset_t *set, ECX = siginfo_t *info or NULL, EDX = const
 * struct timespec *timeout or NULL, ESI = sigsetsize, which must be 8. The
 * first signal of `set` pending for the thread or its process, lowest first
 * and the thread's own before the process's, is taken off its pending set
 * without its handler running. While the call waits the signals of `set`
 * count as unblocked, as Linux counts them, so a signal sent to the process
 * wakes this thread. The timeout is rounded up to whole 10 ms ticks, and one
 * of zero polls; SIGKILL and SIGSTOP cannot be waited for.
 */
int32_t sys_rt_sigtimedwait(isr_params_t *params)
{
    struct kernel_timespec ts;
    uint64_t set;
    uint64_t saved;
    uint32_t deadline = 0;
    int timed = 0;
    int32_t rc;

    if (!current_proc) {
        return -ESRCH;
    }
    if (params->esi != sizeof(uint64_t)) {
        return -EINVAL;
    }
    if (copy_from_user(&set, (const void __user *)params->ebx, sizeof(set)) != 0) {
        return -EFAULT;
    }
    if (params->edx) {
        uint32_t ticks;

        if (copy_from_user(&ts, (const void __user *)params->edx, sizeof(ts)) != 0) {
            return -EFAULT;
        }
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) {
            return -EINVAL;
        }
        ticks = (uint32_t)(ts.tv_sec > 21474836 ? 21474836 : ts.tv_sec) * 100u +
                ((uint32_t)ts.tv_nsec + 9999999u) / 10000000u;
        deadline = kernel_ticks + ticks + (ticks != 0 ? 1u : 0u);
        timed = 1;
    }
    set &= ~(uint64_t)KSIG_UNCATCHABLE;
    saved = current_proc->sig_blocked;
    current_proc->sig_blocked &= ~set;
    rc = sigwait_set(set, timed, deadline, (void __user *)params->ecx);
    current_proc->sig_blocked = saved;
    return rc;
}

/*
 * sys_getpid - return the calling process's PID (getpid(2)), which is its
 * thread group's id.
 */
int32_t sys_getpid(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
    return (int32_t)current_proc->tgid;
}

/**
 * The calling thread's id (gettid(2)); the process id for the thread that
 * began the process.
 */
int32_t sys_gettid(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
    return (int32_t)current_proc->pid;
}

/** Give every thread of the process of `p` the session `sid` and the
 * process group `pgid`, which are the process's. */
static void group_set_ids(const process_t *p, uint32_t sid, uint32_t pgid)
{
    process_t *t;

    for (t = proc_list; t; t = t->next) {
        if (t->tg == p->tg) {
            t->sid = sid;
            t->pgid = pgid;
        }
    }
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
    if (current_proc->pgid == current_proc->tgid) {
        return -EPERM; /* already a process-group leader */
    }
    group_set_ids(current_proc, current_proc->tgid, current_proc->tgid);
    return (int32_t)current_proc->tgid;
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
    if (!target || target->state == PROC_UNUSED || target->tg->live == 0) {
        return -ESRCH;
    }
    /* Only the caller's process or one of its children may be moved. */
    if (target->tg != current_proc->tg && target->parent_pid != current_proc->tgid) {
        return -ESRCH;
    }
    /* A session leader's group is fixed (it anchors the session). */
    if (target->sid == target->tgid) {
        return -EPERM;
    }
    if (pgid == 0) {
        pgid = (int)target->tgid; /* create/lead the group named by the target */
    }
    /* The destination group must be in the target's session: either the target
     * creates its own group, or a process is ALREADY in group `pgid` within the
     * same session (an existing group, regardless of whether its leader is
     * still alive, not merely "a process whose pid == pgid"). */
    if ((uint32_t)pgid != target->tgid && !pgrp_in_session(pgid, (int)target->sid)) {
        return -EPERM;
    }
    group_set_ids(target, target->sid, (uint32_t)pgid);
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
 * Set a kept limit of `p`'s process, in the record of each of its threads:
 * the soft value at most the hard, the hard raised by root only and lowered
 * by anyone, and RLIMIT_NOFILE never above the table's limit. A limit that
 * is not kept is accepted and not changed. 0, or -EINVAL and -EPERM as POSIX
 * names them.
 */
static int rlimit_set(process_t *p, uint32_t resource, uint32_t cur, uint32_t max)
{
    int slot = rlimit_slot(resource);
    process_t *t;

    if (slot < 0) {
        return 0;
    }
    if (cur > max) {
        return -EINVAL;
    }
    if (max > p->rlim[slot].max && current_proc->euid != 0) {
        return -EPERM;
    }
    if (slot == RLIM_NOFILE && max > FD_TABLE_LIMIT) {
        return -EPERM;
    }
    for (t = proc_list; t; t = t->next) {
        if (t->tg != p->tg) {
            continue;
        }
        if (slot == RLIM_NOFILE) {
            t->fds.limit = (int)cur;
        }
        t->rlim[slot].cur = cur;
        t->rlim[slot].max = max;
    }
    return 0;
}

/**
 * The process `pid` names for a limit call: the caller for 0 or its own
 * pid; another process only to root or to a caller of the same real uid.
 */
static process_t *rlimit_target(uint32_t pid)
{
    process_t *p;

    if (pid == 0 || pid == current_proc->tgid) {
        return current_proc;
    }
    p = get_process(pid);
    if (!p || p->state == PROC_UNUSED || p->tg->live == 0) {
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

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    mem_balance();
    vm = &current_proc->mm->vm;

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
/** A copy in the heap of the path /proc/PID/fd would give the current
 * process's descriptor `fd`, for /proc/PID/maps to name the file a region
 * maps; NULL when there is no room or no path. */
static char *fd_path_copy(int fd)
{
    char *buf = kmalloc(VFS_PATH_MAX);
    char *copy = NULL;
    int n;

    if (!buf) {
        return NULL;
    }
    n = procfs_readlink_fd((int)current_proc->tgid, fd, buf, VFS_PATH_MAX - 1);
    if (n > 0) {
        copy = kmalloc((uint32_t)n + 1u);
        if (copy) {
            k_memcpy(copy, buf, (uint32_t)n);
            copy[n] = '\0';
        }
    }
    kfree(buf);
    return copy;
}

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
            if (*file) {
                (*file)->path = fd_path_copy(fd);
            }
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

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    mem_balance();
    vm = &current_proc->mm->vm;

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

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    vm = &current_proc->mm->vm;

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
        vma_t *r = vm_find(&current_proc->mm->vm, start);
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

    if (!current_proc || !current_proc->mm) {
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
    rc = vm_protect(&current_proc->mm->vm, addr, end, vmprot);
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

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    vm = &current_proc->mm->vm;
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

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    if ((addr & (PAGE_SIZE - 1)) != 0 || addr > USER_ADDR_MAX - len || advice == MADV_REMOVE ||
        advice >= MADV_HWPOISON) {
        return -EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (vm_covered(&current_proc->mm->vm, addr, addr + len) != len) {
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

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    if ((addr & (PAGE_SIZE - 1)) != 0 || (flags & ~7u) != 0 || (flags & 5u) == 5u ||
        addr > USER_ADDR_MAX - len) {
        return -EINVAL;
    }
    if (vm_covered(&current_proc->mm->vm, addr, addr + len) != len) {
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

/** Whether [`addr`, `addr` + `len`) is a range the current process's
 * regions cover without a gap, as mlock asks: -EINVAL for an address past
 * the end of the address space, -ENOMEM for a gap, 0. */
static int32_t lock_range(uint32_t addr, uint32_t len)
{
    uint32_t start = addr & ~(uint32_t)(PAGE_SIZE - 1);
    uint32_t end = addr + len;

    if (end < addr) {
        return -EINVAL;
    }
    end = (end + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
    if (end < start) {
        return -EINVAL;
    }
    return vm_covered(&current_proc->mm->vm, start, end) == end - start ? 0 : -ENOMEM;
}

/**
 * mlock (150), munlock (151) and mlock2 (376): lock or unlock the pages of a
 * range, which must be mapped without a gap. There is no swap, so every page
 * already stays in memory; the calls check the range, and mlock2 its flags,
 * 0 or MLOCK_ONFAULT, and change nothing. RLIMIT_MEMLOCK is not kept.
 */
int32_t sys_mlock(isr_params_t *params)
{
    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    return lock_range(params->ebx, params->ecx);
}

int32_t sys_munlock(isr_params_t *params)
{
    return sys_mlock(params);
}

int32_t sys_mlock2(isr_params_t *params)
{
    if (params->edx & ~1u) {
        return -EINVAL;
    }
    return sys_mlock(params);
}

/**
 * mlockall (152) and munlockall (153): lock or unlock every page, which the
 * machine keeps in memory anyway; mlockall's flags must be MCL_CURRENT,
 * MCL_FUTURE and MCL_ONFAULT, not MCL_ONFAULT alone, and not none.
 */
int32_t sys_mlockall(isr_params_t *params)
{
    uint32_t flags = params->ebx;

    if (flags == 0 || (flags & ~7u) || flags == 4u) {
        return -EINVAL;
    }
    return 0;
}

int32_t sys_munlockall(isr_params_t *params)
{
    (void)params;
    return 0;
}

/**
 * mincore (218): EBX = a page-aligned address, ECX = a length, EDX = a byte
 * for each page, which gets 1 when the page is in memory and 0 when it has
 * not been brought in yet. -EINVAL for an address not page-aligned, -ENOMEM
 * for a range with a gap, -EFAULT.
 */
int32_t sys_mincore(isr_params_t *params)
{
    uint32_t addr = params->ebx;
    uint32_t pages;
    uint32_t i;
    int32_t rc;

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    if (addr & (PAGE_SIZE - 1)) {
        return -EINVAL;
    }
    rc = lock_range(addr, params->ecx);
    if (rc != 0) {
        return rc;
    }
    pages = (params->ecx + PAGE_SIZE - 1) / PAGE_SIZE;
    for (i = 0; i < pages; i++) {
        uint8_t in = paging_user_phys(current_proc->page_dir, addr + i * PAGE_SIZE) != 0;

        if (copy_to_user((void __user *)(params->edx + i), &in, 1) != 0) {
            return -EFAULT;
        }
    }
    return 0;
}

/**
 * The CPU and NUMA node the caller runs on (getcpu(2), syscall 318): 0 and 0,
 * stored where EBX and ECX point unless either is null. EDX, the old cache
 * argument, is ignored, as Linux ignores it. -EFAULT for a word that cannot
 * be written.
 */
int32_t sys_getcpu(isr_params_t *params)
{
    uint32_t zero = 0;

    if ((params->ebx && copy_to_user((void __user *)params->ebx, &zero, sizeof(zero)) != 0) ||
        (params->ecx && copy_to_user((void __user *)params->ecx, &zero, sizeof(zero)) != 0)) {
        return -EFAULT;
    }
    return 0;
}

/**
 * The execution domain (personality(2), syscall 136): EBX is the new value,
 * or 0xFFFFFFFF to read it, and the old value is returned. The domain, the
 * low byte, must be PER_LINUX (0) or PER_LINUX32 (8), the two a Linux i386
 * program runs in, else -EINVAL; the flag bits above it are kept and change
 * nothing, since this kernel has no address randomization for
 * ADDR_NO_RANDOMIZE, which GDB sets, to turn off, and no other domain.
 */
int32_t sys_personality(isr_params_t *params)
{
    uint32_t old;

    if (!current_proc) {
        return -ESRCH;
    }
    old = current_proc->persona;
    if (params->ebx != 0xFFFFFFFFu) {
        if ((params->ebx & 0xFFu) != 0 && (params->ebx & 0xFFu) != 8u) {
            return -EINVAL;
        }
        current_proc->persona = params->ebx;
    }
    return (int32_t)old;
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
