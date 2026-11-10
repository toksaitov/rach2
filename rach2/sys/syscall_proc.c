/*
 * The process system calls: exit, exec, fork and wait, signals, identities,
 * sessions, limits, scheduling and the address space. Each architecture's
 * systab.c puts the handlers in its table; what they share with the other
 * system call files is in syscall_internal.h.
 */
#include "arch/arch.h"
#include "arch/uabi.h"
#include "fpu.h"
#include "../kernel.h"
#include "mm/filecache.h"
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
#include "proc/futex.h"
#include "proc/seccomp.h"
#include "proc/ns.h"
#include "proc/wait_queue.h"
#include "proc/tgroup.h"
#include "proc/ptrace.h"
#include "proc/elf.h"
#include "proc/uimage.h"
#include "proc/sigframe.h"
#include "proc/pidfd.h"
#include "proc/rseq.h"
#include "proc/smp.h"
#include "lib/krandom.h"
#include "lib/kerrno.h"
#include "lib/kirq.h"
#include "lib/ksignal.h"
#include "fs/fd.h"
#include "fs/file.h"
#include "fs/pathbuf.h"
#include "ipc/pipe.h"
#include "io/poll.h"
#include "io/anon.h"
#include "tty/tty.h"
#include "tty/pty.h"
#include "rtc.h"
#include "block/loop.h"
#include "fs/ext2.h"
#include "fs/krffs.h"
#include "fs/devfs.h"
#include "io/timerfd.h"
#include "fs/synthfs.h"
#include "fs/vfs.h"
#include "fs/name.h"
#include "block/usbdisk.h"
#include "fs/procfs.h"
#include "mm/paging.h"
#include "mm/pressure.h"
#include "mm/pagecache.h"
#include "mm/heap.h"
#include "mm/notify.h"
#include "mm/usercopy.h"
#include "mm/vm.h"
#include "lib/kuser.h"
#include "lib/kstring.h"
#include "display/fb.h"
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
#include "cmdline.h" /* kcmdline_has, for the rach2.stackoverrun test switch */
#include "mm/filemap.h"
#include "mm/mm.h"
#include "fs/tmpfs.h"
#include "fs/devfs.h"
#include "timer.h"
#include "vdso.h"
#include "syscall_internal.h"

/* The boot stack, from loader.s, which process 0, the first program and the
 * programs the debug shell starts run on. */
extern char stack[];

/**
 * The page below the base is the heap's guard (`kalloc_guarded`), taken and
 * never mapped, so a thread that runs past the base faults there at once
 * instead of writing over the heap page below; `isr_double_fault` reports it.
 *
 * QEMU can retain code translations in reused frames. Clear every page
 * so translations outside the used part of the stack cannot keep stack
 * stores on its slow write path. Clearing also leaves the child nothing
 * of the stack's previous owner.
 */
uintptr_t kstack_alloc(void)
{
    void *base = kalloc_guarded(KERNEL_STACK_SIZE / PAGE_SIZE);

    if (base != NULL) {
        k_memset(base, 0, KERNEL_STACK_SIZE);
    }
    return (uintptr_t)base;
}

/**
 * A stack goes back when its zombie is reaped, when a fork fails after taking
 * it, when exec takes the leader's place, and when a test of the debug
 * shell's ends; the caller never runs on it.
 *
 * The boot stack is part of the kernel image and not the heap's, and is left
 * as it is. A record whose stack it is can still come here: a thread of a
 * program on the boot stack that calls execve takes its leader's place, and
 * the leader's record goes (`exec_take_leader`).
 */
void kstack_free(uintptr_t top)
{
    if (top != 0 && top != (uintptr_t)stack + KERNEL_STACK_SIZE) {
        kfree_guarded((void *)(top - KERNEL_STACK_SIZE), KERNEL_STACK_SIZE / PAGE_SIZE);
    }
}

/**
 * The page below the stack is the guard `kstack_alloc` leaves below a stack
 * it gives, or the one loader.s leaves below the boot stack, which process
 * 0, the first program and the programs the debug shell starts run on. The
 * double fault's handler and the page fault's call this with the address
 * the processor left in CR2.
 */
int kstack_overrun_pid(uintptr_t addr)
{
    uintptr_t guard;

    if (!current_proc || current_proc->kernel_stack < KERNEL_STACK_SIZE + PAGE_SIZE) {
        return -1;
    }
    guard = current_proc->kernel_stack - KERNEL_STACK_SIZE - PAGE_SIZE;
    return addr - guard < PAGE_SIZE ? (int)current_proc->pid : -1;
}

/* The depth at which the overrun test's descent would turn back; a stack of
 * KERNEL_STACK_SIZE holds fewer than sixteen of its frames. */
#define KSTACK_DESCENT_MAX 4096u

/** One frame of the overrun test's descent: a quarter page of locals, the
 * lowest byte written, so every page the descent reaches is touched. */
static uint32_t kstack_descend(uint32_t depth)
{
    volatile uint8_t frame[1024];

    frame[0] = (uint8_t)depth;
    if (depth >= KSTACK_DESCENT_MAX) {
        return frame[0];
    }
    return kstack_descend(depth + 1) + frame[0];
}

/** The overrun test's descent, on the stack of the running thread; it does
 * not return, and the kernel panics if the descent comes back. */
static void kstack_overrun_run(void)
{
    klog(KLOG_INFO, "kstack", "overrun: pid %d descends its stack at 0x%x", (int)current_proc->pid,
         current_proc->kernel_stack - KERNEL_STACK_SIZE);
    kstack_descend(0);
    k_panic("kstack: overrun: the descent came back");
}

/**
 * `rach2.stackoverrun=fork` on the boot line runs a thread on a stack
 * `kstack_alloc` gives, as a fork's child has one, and
 * `rach2.stackoverrun=boot` descends the boot stack as process 0. The fault
 * is a double fault, since the page fault's own frame cannot be pushed, and
 * its handler reports the thread and stops the machine. The thread's record
 * is taken after its stack, so that it lies above the stack, where a descent
 * that finds no guard cannot write over it. Called once, before the first
 * program; without the option it returns at once.
 */
void kstack_overrun_test(void)
{
    process_t *from = current_proc;
    process_t *thread;
    uintptr_t base;

    if (!from) {
        return;
    }
    if (kcmdline_has("rach2.stackoverrun=boot")) {
        kstack_overrun_run();
    }
    if (!kcmdline_has("rach2.stackoverrun=fork")) {
        return;
    }
    base = kstack_alloc();
    thread = alloc_process();
    if (!thread || !base) {
        k_panic("kstack: overrun: no record or stack for the thread");
        return;
    }
    thread->kernel_stack = base + KERNEL_STACK_SIZE;
    thread->space_root = 0;
    thread->state = PROC_RUNNING;
    setup_process_stack(thread, kstack_overrun_run);
    current_proc = thread;
    switch_context(from, thread);
}

/**
 * Give back the record `p` of a thread or a process that has ended: its
 * kernel stack and the record, and an address space it still has, which one
 * that ended through `proc_exit_cleanup` gave up then. A release of an
 * address space may sleep, on a file's close or its writeback, and another
 * reaper may run then, so the caller has first taken `p` off the zombies by
 * marking it `PROC_UNUSED`, which no reaper and no wait takes.
 */
static void proc_release(process_t *p)
{
    mm_drop(p);
    kstack_free(p->kernel_stack);
    free_process(p);
}

/*
 * Init process tracking from kernel.c. A real init's death is fatal
 * (panic_init_died); an init exiting under the debug shell returns to it.
 */
extern volatile uint32_t init_pid;

/* The debug shell's loop, in kernel.c, reached when an init exits while the
 * debug shell is the supervisor (rescue mode or the no-init test image). */
extern void shell_loop(void);

extern void shell_reset_input(void);

/* Boot-home policy from kernel.c. debug_shell_is_home is true when the debug
 * shell supervises (an exited init returns to it); false when a real /sbin/init
 * is PID 1, in which case its death is fatal (panic_init_died, never returns).
 */
extern bool debug_shell_is_home;

extern void panic_init_died(uint32_t pid);

/**
 * Each descriptor drops its socket reference and its open file description
 * (the seek offset), and its slot is cleared. `proc_files_put` calls it when
 * the last reference to the table goes, which both ends of a process reach
 * (`proc_exit_cleanup`), so sockets, port bindings, TCBs and descriptions
 * are freed however a process ends. A second call finds every slot free and
 * does nothing.
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

void proc_files_put(process_t *p)
{
    fd_table_t *table = p->fds;

    if (!table) {
        return;
    }
    if (table->refs == 1) {
        release_fd_table(table);
        fd_table_delete(table);
    } else {
        table->refs--;
    }
    p->fds = NULL;
}

void proc_fs_put(process_t *p)
{
    struct fs_struct *fs = p->fs;

    if (!fs) {
        return;
    }
    if (fs->refs == 1) {
        vfs_cwd_clear(p);
        vfs_root_clear(p);
        kfree(fs->cwd_long);
        kfree(fs);
    } else {
        fs->refs--;
    }
    p->fs = NULL;
}

/**
 * Make `table`, whose entries were copied from another table, a table of its
 * own: each descriptor names the same open file description, which gains a
 * reference, so a seek in one process is seen by the other, as POSIX says,
 * and a socket gains one too, so that the two release it independently. A
 * descriptor a sharer closed, which a call of another still holds, is
 * dropped from the copy, and so is a slot a sharer's call has reserved and
 * not yet given a type, whose description the copy has no reference to, as
 * Linux's `dup_fd` frees a descriptor not yet installed.
 */
static void files_take(fd_table_t *table)
{
    int fdi;

    for (fdi = 0; fdi < table->cap; fdi++) {
        fd_entry_t *e = &table->entries[fdi];

        if ((e->flags & FD_FLAG_CLOSING) || (e->type == FD_TYPE_NONE && e->ofd != NULL)) {
            k_memset(e, 0, sizeof(*e));
        }
        if (e->type == FD_TYPE_NONE) {
            continue;
        }
        fd_ofd_ref(e->ofd);
        if (e->type == FD_TYPE_SOCKET && e->sock) {
            socket_ref(e->sock);
        }
    }
}

/** A copy of the descriptor table `src` for a child that does not share it,
 * as `files_take` makes one; NULL when the heap has no room. */
static fd_table_t *files_copy(const fd_table_t *src)
{
    fd_table_t *table = fd_table_dup(src);

    if (table) {
        files_take(table);
    }
    return table;
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
 * reaps one, taken off the zombies before its release, which may sleep
 * (`proc_release`), and the walk starts again after each, since the list
 * may have changed while it slept. Kernel threads and the idle record are
 * not user processes and stay: a kernel thread has no address space and is
 * in no system call. A user process that has given up its address space in
 * the call that ends it (`proc_exit_cleanup`) may be asleep there, in a
 * file's release, and is waited for until it is a zombie, and reaped; it
 * is sent no signal, since it does not return to user mode. A record a
 * reaper has taken off the zombies is that reaper's to give back.
 *
 * Each turn waits for an interrupt after the switch, with the big lock let
 * go: a leftover running on another processor takes its signal in that
 * processor's interrupt, which needs the lock, and `schedule` returns at
 * once, the lock held, when this processor has nothing else to run.
 */
static void proc_end_others(void)
{
    for (;;) {
        process_t *p;
        int alive = 0;

        for (p = proc_list; p; p = p->next) {
            if (p->tg == current_proc->tg || p == proc_idle) {
                continue;
            }
            if (p->state == PROC_ZOMBIE) {
                if (p->pid == p->tgid && p->tg->live != 0) {
                    alive++; /* a leader waits for the rest of its threads */
                    continue;
                }
                break;
            }
            if (p->space_root == 0) {
                alive += p->in_syscall != 0 && p->state != PROC_UNUSED;
                continue;
            }
            signal_post(p, SIGKILL);
            alive++;
        }
        if (p) {
            p->state = PROC_UNUSED;
            proc_release(p);
            continue;
        }
        if (!alive) {
            return;
        }
        schedule();
        idle_wait_irq_on();
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
        arch_irq_disable();
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
 * A thread that is not its process's last only releases what it holds and
 * leaves. The last one releases the process's session, reparents the
 * children to init, publishes the zombie and wakes its parent, then
 * reschedules. Every thread gives up its address space and its descriptors
 * at its end (`proc_exit_cleanup`); its record and kernel stack go when it
 * is reaped. A real init's exit is fatal (panic_init_died); an init exiting
 * under the debug shell returns to it.
 *
 * The plain status line goes out only where the debug shell is the supervisor.
 * There it is how a person, and the harness in its no-token mode, read a
 * transient init's outcome. On an image running `/sbin/init` and a login shell
 * every process exit would print it, a forked child's included, and the shell
 * already answers the question through `$?`. The line that hands the shell
 * back is printed whenever an init returns to the shell: the probe runner
 * scores probes from that one.
 *
 * In the harness build an init's exit reports its status to QEMU's
 * isa-debug-exit device at once, before the scheduler can switch to a child
 * that is ready, so the runner reads the init's own exit code. A real
 * /sbin/init that exits leaves no reaper, and the kernel panics before the
 * scheduler can give the processor to a ready process and run on with no
 * PID 1.
 *
 * An init exiting under the debug shell first ends every other user process
 * through `proc_end_others`, then returns without scheduling: a child left
 * runnable would take the processor, and the exited init, a zombie, would
 * never be picked again to make the return. Its record is freed and replaced
 * by the idle record with interrupts off, since an interrupt handler between
 * the two would count its entry in the freed record, and the shell's command
 * line and stdin-wait state are cleared in the same window, so a keyboard
 * interrupt cannot append to a stale command buffer or find a stale stdin
 * waiter; `shell_loop` clears them again on entry. The shell then runs from
 * the top of the boot stack.
 *
 * A process that is not init leaves through the scheduler as a zombie its
 * parent reaps; when nothing is ready at that moment, since its parent may be
 * gone, it idles for the next interrupt with interrupts off between checks
 * and schedules on its own, because the timer's preemption is deferred
 * inside a call.
 */
int32_t exit_current(int32_t code)
{
    int status = (code >> 8) & 0xFF;
    bool is_init;
    int alone;
    uint32_t exiting_pid;

    if (!current_proc) {
        arch_stop();
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
    if (is_init) {
        harness_exit((uint8_t)status);
    }
#endif

    if (is_init && !debug_shell_is_home) {
        panic_init_died(exiting_pid); /* never returns */
    }

    if (!is_init) {
        exit_schedule_away();
    }

    klog(KLOG_INFO, "sys_exit", "init process %d exited with status %d, returning to kernel shell",
         exiting_pid, status);
    console_puts("Init process exited. Returning to kernel debug shell.");

    arch_irq_disable();
    free_process(current_proc);

    init_pid = 0;

    current_proc = proc_idle;
    current_proc->state = PROC_RUNNING;

    shell_reset_input();
    arch_irq_enable();

    sched_disable();

    arch_run_on_stack((uintptr_t)stack + KERNEL_STACK_SIZE, shell_loop);
}

/**
 * exit: end the calling thread with the status its argument has; the
 * process ends with its last thread. A thread's return from its start function
 * makes this call through glibc's and musl's thread code; exit(3) and _exit(2)
 * make exit_group.
 */
long sys_exit(isr_params_t *params)
{
    return exit_current((int32_t)((SC_ARG(params, 0) & 0xFFu) << 8));
}

/**
 * exit_group: end every thread of the process, the process with the status
 * its argument has.
 */
long sys_exit_group(isr_params_t *params)
{
    int32_t code = (int32_t)((SC_ARG(params, 0) & 0xFFu) << 8);

    if (current_proc) {
        proc_group_exit(code);
    }
    return exit_current(code);
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

    for (fd = 0; fd < fd_table_cap(current_proc->fds); fd++) {
        fd_entry_t *entry = fd_get(current_proc->fds, fd);
        if (!entry || !(entry->flags & FD_FLAG_CLOEXEC)) {
            continue;
        }
        fd_release_backing(entry);
        fd_free(current_proc->fds, fd);
    }
}

/*
 * A file execve reads: the program, a script's interpreter, or the dynamic
 * linker a PT_INTERP names. The program, or the interpreter a script names,
 * on the KRFFS root is read through the description of it the new address
 * space keeps (`mm_exe_open`), at the node the file is at now, so the load
 * reads the file whose rights and set-id bits the exec checked, whatever is
 * put at its path meanwhile. The ELF interpreter has a description of its
 * own with the same lifetime. One on another mount is
 * read through a handle on the node its resolution held, opened once and
 * released after the load. It lives in the caller's own exec workspace below,
 * because a read of a mounted volume sleeps and a second exec runs while it
 * does.
 */
struct exec_file {
    char path[VFS_PATH_MAX]; /* the node's path on the KRFFS root */
    struct vfs_ref ref;      /* the mount is NULL on the KRFFS root and once closed */
    void *handle;
    struct open_file *held; /* the file of the root the new image keeps */
    struct vm_file *map;    /* a private mapping of the file for its segments, or NULL */
    int32_t err;            /* the errno of a failed read of a mount, which execve reports */
    int readable;           /* whether the caller may read the file itself */
};

/* What a program's set-id bits would do: the bits that apply, and the file
 * owner's ids they would give. */
struct exec_setid {
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
};

/**
 * Read the program's file of the KRFFS root through `ofd`, the description
 * its new address space keeps: at the node the file is at now, under the
 * root's lock, which a relocation takes, and not a directory, which
 * `krffs_read` does not read either. The count read, or a negative value.
 */
static int exec_held_read(const struct open_file *ofd, void *dst, uint32_t count, uint32_t off)
{
    krffs_node_t *node;
    int got = -1;

    vfs_ns_lock(ofd->vmnt);
    node = ofd->vnode;
    if (node != krffs_fs->node && !KRFFS_IS_DIR(node->mode)) {
        got = krffs_read_node(node, (char *)dst, count, off);
    }
    vfs_ns_unlock(ofd->vmnt);
    return got;
}

/**
 * An `elf_read_fn` over an `exec_file`. A whole page of a mounted file at a
 * page's offset, which is what the loader reads of a segment, comes from the
 * file page cache when it has it, and is kept there when it has not, so a
 * program started again reads no page of its text from the disk. The count
 * read, or a negative value; a failed read of a mount is -1 and leaves its
 * error in `f->err`.
 */
static int exec_file_read(void *ctx, void *dst, uint32_t count, uint32_t off)
{
    struct exec_file *f = ctx;
    int page = count == PAGE_SIZE && off % PAGE_SIZE == 0;
    uint32_t epoch = filecache_epoch();
    int32_t got;

    if (f->held != NULL) {
        return exec_held_read(f->held, dst, count, off);
    }
    if (f->ref.mnt == NULL) {
        return krffs_read(f->path, (char *)dst, count, off);
    }
    if (page && filecache_read_node(f->ref.mnt, f->ref.node, off / PAGE_SIZE, dst)) {
        return (int)count;
    }
    got = vfs_read(f->ref.mnt, f->handle, dst, count, off);
    if (got < 0) {
        f->err = got;
        return -1;
    }
    if (page && got == PAGE_SIZE) {
        filecache_store_node(f->ref.mnt, f->ref.node, off / PAGE_SIZE, dst, epoch);
    }
    return (int)got;
}

/**
 * The error execve answers for a load of the file `f` the ELF loader failed
 * with `rc`: the error of a read of a mount, which the loader can only call
 * -EIO, else `rc`, with a malformed image, -ENOEXEC, answered as
 * `malformed`.
 */
static int32_t exec_load_err(const struct exec_file *f, int rc, int32_t malformed)
{
    if (f->err != 0) {
        return f->err;
    }
    return rc == -ENOEXEC ? malformed : rc;
}

/** Release the handle and the node `f` holds on another mount, if any, and
 * the mapping of the file the loader did not take; a file of the root the
 * new address space keeps stays with it. */
static void exec_file_close(struct exec_file *f)
{
    f->held = NULL;
    if (f->map) {
        vm_file_put(f->map);
        f->map = NULL;
        vm_file_reap();
    }
    if (f->ref.mnt) {
        vfs_release(f->ref.mnt, f->handle);
        vfs_ref_release(&f->ref);
        f->ref.mnt = NULL;
        f->handle = NULL;
    }
}

/* The most of a script's first line that execve reads to parse it. */
#define EXEC_SHEBANG_MAX 128

/* execveat(2)'s flags, as <fcntl.h> numbers them. */
#define EXEC_AT_FDCWD            (-100)
#define EXEC_AT_SYMLINK_NOFOLLOW 0x100u
#define EXEC_AT_EMPTY_PATH       0x1000u

/* How execve or execveat named the program: the directory a relative path
 * is taken from, a descriptor or EXEC_AT_FDCWD; the path; the vectors; and
 * execveat's flags, 0 for execve. */
struct exec_call {
    int dirfd;
    const char __user *path;
    char *const __user *argv;
    char *const __user *envp;
    uint32_t flags;
};

/*
 * Where execve has the program's path, the new image's arguments and the
 * new image itself until it commits.
 *
 * Each call has a workspace of its own in the heap, which the caller's record
 * points at until the call ends, so a record freed during one gives it back
 * too. A program on a mounted volume is read through the buffer cache, which
 * sleeps, so a second execve runs while the first is inside a read, and a
 * workspace shared by the two would let the second overwrite the first's
 * arguments, its open file and the reference its resolution holds.
 * `in_syscall` stops the timer from scheduling; it does not stop a call that
 * gives the processor up.
 *
 * The strings are in `args`, the pages of the new stack (`proc/uimage.h`),
 * and the image is built in `mm`, an address space of its own, made before
 * the program is opened so that it keeps the program's file from the open
 * on, so every limit and every failure of the load is found while there is
 * still a caller to return an error to.
 */
struct exec_state {
    struct exec_file prog; /* the file being read */
    struct exec_setid id;
    struct uimage args;
    struct mm *mm; /* the new image's address space until it commits */
    uint32_t argc;
    uint32_t envc;
    uint32_t euid; /* the ids the new image runs with */
    uint32_t egid;
    uint32_t cap_lost[2]; /* the capabilities it lacks (`exec_ids`) */
    int regains;          /* whether it holds any its caller lost */
    int secure;           /* AT_SECURE */
    int unreadable;       /* an image loaded is one the caller may not read */
    int compat;           /* the program is an i386 one (`exec_classify`) */
    int path_gone;        /* its path is a close-on-exec descriptor's (`exec_name`) */
    uintptr_t entry;      /* where the new image starts */
    /* The new image's layout (`exec_layout`): the address just past its
     * stack, its mmap base, a position-independent program's base, and how
     * far above the program's end its heap starts; and the persona it runs
     * with. */
    uintptr_t stack_top;
    uintptr_t mmap_base;
    uintptr_t dyn_base;
    uintptr_t heap_shift;
    uint32_t persona;
    elf_layout_t layout;
    elf_layout_t ilayout; /* the interpreter's */
    elf_image_t img;
    elf_image_t iimg;          /* the interpreter's */
    fd_table_t *files;         /* the caller's own table (`exec_files`) */
    struct fs_struct *fs_held; /* holds off CLONE_FS (`exec_ids`) */
    int path_end;              /* how `path` named a directory by its end */
    char path[VFS_PATH_MAX];
    /* The base name of a `#!` script, which names the new image's comm in
     * place of its interpreter's, as Linux names it by bprm->filename; empty
     * for a program run directly. */
    char script[16];
    char shebang_line[EXEC_SHEBANG_MAX + 1];
};

/** Give back the current process's exec workspace, if it has one: the
 * file held open, the frames of a stack no page directory took, the
 * address space of an image that did not commit, the references to the
 * files the images map, which their regions hold on their own, the
 * storage of a descriptor table it did not install, which holds no
 * references, the hold on CLONE_FS, and the workspace itself. */
static void exec_release(void)
{
    struct exec_state *s = current_proc->exec_ws;

    if (s == NULL) {
        return;
    }
    if (s->fs_held != NULL) {
        s->fs_held->in_exec--;
    }
    exec_file_close(&s->prog);
    uimage_release(&s->args);
    mm_put(s->mm);
    vm_file_put(s->img.file);
    vm_file_put(s->iimg.file);
    vm_file_reap();
    if (s->files != NULL) {
        fd_table_delete(s->files);
    }
    kfree(s);
    current_proc->exec_ws = NULL;
}

/** Keep the base name of the script at `s->path` in `s->script`, which
 * names the new image's comm in place of its interpreter's. */
static void exec_keep_script(struct exec_state *s)
{
    const char *base = s->path;
    const char *p;

    for (p = s->path; *p; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    k_strncpy(s->script, base, sizeof(s->script) - 1);
}

/**
 * Rewrite an exec of a script into an exec of its interpreter.
 *
 * A file whose first two bytes are `#!` names the program that runs it. As
 * on Linux, the rest of that line is the interpreter, named here by an
 * absolute path, and at most one argument, which is all that follows the
 * name less the blanks around it; the program then runs as
 * `interpreter [argument] script original-arguments...`, so that a program
 * such as a DHCP client can exec a script directly. A first line that fills
 * the buffer without ending is -ENOEXEC and is not cut short, since a
 * truncated interpreter name may name a different program that exists.
 * `exec_script` follows one level only.
 *
 * The arguments on the new stack are rewritten as Linux's `load_script`
 * rewrites them: argv[0] is taken away, and the script's path, the argument
 * and the interpreter are put below the rest, under the limits the caller's
 * strings met. The caller's `path` becomes the interpreter's, and its base
 * name is kept in `script` for the new image's comm. The script is
 * read through `read` over `ctx`, which `exec_script` makes `exec_file_read`
 * over the program it opened.
 *
 * The interpreter's path is made canonical, as a path from a caller is, with
 * `s->path_end` saying how it named a directory by its end
 * (`canonicalize_named`), which the open of the interpreter answers for.
 *
 * @param argc the argument count, updated when the vector grows.
 * @return 1 when rewritten, 0 when the file is not a script, or a negative
 *         errno for one whose first line is too long, names nothing or names
 *         a relative path, or whose arguments no longer fit.
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

    exec_keep_script(s);
    k_strncpy(s->path, interp, sizeof(s->path) - 1);
    s->path[sizeof(s->path) - 1] = '\0';
    rc = canonicalize_named(s->path, &s->path_end);
    return rc < 0 ? rc : 1;
}

/** What a move of `p`'s effective or file-system ids undoes: its address
 * space stops being dumpable, for every thread that shares it, and it loses
 * its parent-death signal, so the parent it had cannot signal the identity
 * it now has. */
static void cred_undump(process_t *p)
{
    if (p->mm) {
        p->mm->dumpable = 0;
    }
    p->pdeath_signal = 0;
}

/**
 * What a move of the effective ids undoes, as Linux's `commit_creds` undoes
 * it: when `p`'s effective uid or gid is no longer `old_euid` or `old_egid`,
 * what `cred_undump` takes away. A new effective uid also ends the
 * capabilities it was granted in its user namespace (`ns_granted`), where
 * its privilege is then its uid's.
 */
static void cred_moved(process_t *p, uint32_t old_euid, uint32_t old_egid)
{
    if (p->euid != old_euid || p->egid != old_egid) {
        cred_undump(p);
    }
    if (p->euid != old_euid) {
        p->ns_granted = 0;
    }
}

/**
 * Make `p`'s file-system user id its effective one again, as Linux does in
 * every call that sets user ids, whether or not the call moved the effective
 * id. An id setfsuid had set apart moves by that, which undoes what a move
 * of the effective id does (`cred_undump`).
 */
static void fsuid_follow(process_t *p)
{
    if ((p->fs_own & PROC_FSUID_OWN) && p->fsuid != p->euid) {
        cred_undump(p);
    }
    p->fs_own &= ~PROC_FSUID_OWN;
}

/** `fsuid_follow` for the file-system group id, in every call that sets
 * group ids. */
static void fsgid_follow(process_t *p)
{
    if ((p->fs_own & PROC_FSGID_OWN) && p->fsgid != p->egid) {
        cred_undump(p);
    }
    p->fs_own &= ~PROC_FSGID_OWN;
}

/** Whether any of the caller's real, effective and saved user ids is root
 * in its user namespace. */
static int uids_root(const process_t *p)
{
    return uid_seen(p->ruid) == 0 || uid_seen(p->euid) == 0 || uid_seen(p->suid) == 0;
}

/**
 * Empty `p`'s ambient set when its user ids leave root: `had_root`, from
 * `uids_root` before the change, said one of them was root, and none is
 * now. Linux's `cap_emulate_setxuid` does so, since a program written
 * before the ambient set expects a change to ids other than root and an
 * exec to leave no capability; it empties the permitted set too, and so
 * the capabilities an exec gave from the ambient set go (`amb_granted`).
 */
static void ambient_uid_moved(process_t *p, int had_root)
{
    if (had_root && !uids_root(p)) {
        p->cap_amb[0] = p->cap_amb[1] = 0;
        p->amb_granted = 0;
    }
}

/** Whether the caller may set any id its user namespace maps: privileged
 * there, which in the first namespace is root's. */
static int id_privileged(void)
{
    return ns_privileged(current_proc, proc_userns(current_proc));
}

/**
 * Take the ids a setter names, `n` of them at `v`, in the caller's user
 * namespace (`uid_named`, or `gid_named` for groups), as the kernel's,
 * SC_ID_KEEP left as it is. 0, or -EINVAL for one the namespace does not
 * map.
 */
static int32_t ids_named(uint32_t *v, int n, int gid)
{
    int i;

    for (i = 0; i < n; i++) {
        int64_t k;

        if (v[i] == SC_ID_KEEP) {
            continue;
        }
        k = gid ? gid_named(v[i]) : uid_named(v[i]);
        if (k < 0) {
            return -EINVAL;
        }
        v[i] = (uint32_t)k;
    }
    return 0;
}

/**
 * What an exec's credentials change beside the ids, as Linux's exec does:
 * the new image's address space `mm` is dumpable unless the image runs with
 * ids its caller lacks (`secure`), its effective or file-system ids moved
 * (`ids_moved`), it is privileged where its caller was not
 * (`was_privileged`), or an image it loaded is one the caller may not read
 * (`unreadable`), whose bytes a core file or a tracer's attach would give
 * the caller, as Linux's `would_dump` has it. A secure exec, or one that
 * moved those ids or made the caller privileged, clears the
 * parent-death signal too, as Linux's `commit_creds` clears it; a secure
 * exec lowers a soft RLIMIT_STACK above 8 MiB to 8 MiB. The flag is set
 * before the address space is installed, so no other process sees the image
 * dumpable while it runs with new ids.
 */
static void exec_privileges(struct mm *mm, int secure, int unreadable, int ids_moved,
                            int was_privileged)
{
    int moved = ids_moved || (!was_privileged && proc_privileged(current_proc));

    mm->dumpable = secure || moved || unreadable ? 0u : 1u;
    if (secure || moved) {
        current_proc->pdeath_signal = 0;
    }
    if (secure && current_proc->rlim[RLIM_STACK].cur > RLIM_STACK_DEFAULT) {
        current_proc->rlim[RLIM_STACK].cur = RLIM_STACK_DEFAULT;
    }
}

/**
 * How far below its top page the stack of the new image in `s` may grow,
 * when the pages its arguments took start at `bottom`: to `limit`, the soft
 * RLIMIT_STACK in force at the exec, in whole pages, which bounds the whole
 * stack as Linux's does, but never into the range below the image's mmap
 * base where anonymous mappings are placed, and always as far as `bottom`.
 * A limit changed later applies at the next exec.
 */
static uintptr_t exec_stack_window(const struct exec_state *s, uintptr_t bottom,
                                   unsigned long limit)
{
    uintptr_t size = limit & ~(unsigned long)(PAGE_SIZE - 1);
    uintptr_t floor;

    if (size > s->stack_top - s->mmap_base) {
        size = s->stack_top - s->mmap_base;
    }
    floor = s->stack_top - size;
    if (floor > bottom) {
        floor = bottom;
    }
    return s->stack_top - PAGE_SIZE - floor;
}

/**
 * Read pointer `i` of the user vector `vec` into `*ptr`: a pointer of the
 * caller's ABI, 4 bytes for an i386 call, as Linux's `get_user_arg_ptr`
 * reads a compat execve's. 0, or -EFAULT.
 */
static int32_t exec_vec_get(char *const __user *vec, uint32_t i, uintptr_t *ptr)
{
    if (sizeof(void *) > 4u && in_compat_syscall()) {
        uint32_t p32;

        if (copy_from_user(&p32, (const void __user *)((uintptr_t)vec + (uintptr_t)i * 4u), 4u) !=
            0) {
            return -EFAULT;
        }
        *ptr = p32;
        return 0;
    }
    return copy_from_user(ptr, (const void __user *)&vec[i], sizeof(*ptr)) != 0 ? -EFAULT : 0;
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
        uintptr_t ptr;

        if (exec_vec_get(vec, i, &ptr) != 0) {
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
        uintptr_t ptr;
        int32_t rc;

        n--;
        if (exec_vec_get(vec, n, &ptr) != 0) {
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
 * Copy the program's path and the vectors of the call `c` onto the new
 * stack in `s`, and count them into `s->argc` and `s->envc`, as Linux's
 * `do_execveat_common` does.
 *
 * Both vectors are counted first, and their pointers are measured against
 * the budget `uimage_start` sets from the soft RLIMIT_STACK. The strings
 * follow, the path the caller passed at the top, or `name` in its place
 * when it is not NULL, then the environment and then the arguments, each
 * vector last string first. An empty argv gets one empty string, so a
 * program that starts reading at argv[1] does not read the environment. 0,
 * -E2BIG, -EFAULT or -ENOMEM.
 */
static int32_t exec_copy_args(struct exec_state *s, const struct exec_call *c, const char *name)
{
    const char __user *user_path = c->path;
    char *const __user *user_argv = c->argv;
    char *const __user *user_envp = c->envp;
    uint32_t *argc = &s->argc;
    uint32_t *envc = &s->envc;
    unsigned long limit = current_proc->rlim[RLIM_STACK].cur;
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
        rc = name != NULL ? uimage_push(&s->args, name) : uimage_push_user(&s->args, user_path);
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
 * Go on from the node `ref` a locked walk ended on to the file it leads to,
 * when it is a /proc/PID/fd/N link (`proc_fd_file`), with `*held` the
 * description the caller puts once the mount is unlocked; any other node
 * stays as it is. On an error the walk's node is released and nothing is
 * held.
 */
static int32_t exec_follow_fd(struct vfs_ref *ref, struct open_file **held)
{
    const struct vpath *vp = vfs_synth_vpath(ref->mnt, ref->node);
    struct vpath link;

    if (vp == NULL || vp->kind != VNODE_PROC_FD_LINK) {
        return 0;
    }
    link = *vp;
    vfs_walk_unlock(ref->mnt);
    vfs_ref_release(ref);
    return proc_fd_file(&link, ref, held);
}

/**
 * Have the new address space `keep` keep the file `ref` a locked walk ended
 * on, its program or its ELF interpreter (`mm_exe_open`), and read a file of
 * the KRFFS root through that, as `f` says. The program names the address
 * space, as /proc/PID/exe shows it, by `real`, the path the walk took to it,
 * or by `path`, the path the exec was given, when `real` is empty; when the
 * walk went on to the program from the descriptor's link whose description
 * is `link`, by the path that descriptor was opened by, if it has one. 0,
 * -ETXTBSY or -ENOMEM.
 */
static int32_t exec_keep(struct exec_file *f, const struct vfs_ref *ref, struct mm *keep,
                         const struct open_file *link, const char *real, const char *path)
{
    const char *name = real[0] != '\0' ? real : path;
    int32_t rc = mm_exe_open(keep, ref);

    if (rc == 0 && f->ref.mnt == NULL) {
        f->held = keep->interp_file ? keep->interp_file : keep->exe_file;
    }
    if (rc == 0 && keep->interp_file == NULL) {
        mm_set_exe(keep, link != NULL && link->dirpath != NULL ? link->dirpath : name);
    }
    return rc;
}

/**
 * Make `f->map` a private mapping of the file at `ref`, whose attributes on
 * a mount other than the KRFFS root are `vst`, for the loader to map the
 * file's segments by (`elf_load_image_at`), as Linux's `elf_map` maps them
 * from the open file. The mapping keeps a description of the file open for
 * reading, in a copy of a descriptor no table has (`filemap_new`), so the
 * image keeps its file through a close, a rename or an unlink, as a mapping
 * by mmap does. A file that is not regular gets none, and the loader copies
 * its segments. 0, or -ENOMEM or the open's error.
 */
static int32_t exec_map(struct exec_file *f, const struct vfs_ref *ref, const struct vfs_stat *vst)
{
    const krffs_node_t *node = ref->node;
    int root = mnt_is_root(ref->mnt);
    uint32_t type = root ? krffs_mode_type(node->mode) : vst->mode & 0170000u;
    struct open_file *ofd;
    fd_entry_t e;
    int32_t rc;

    if (type != 0100000u) {
        return 0;
    }
    ofd = fd_ofd_new();
    if (!ofd) {
        return -ENOMEM;
    }
    rc = vfs_open_node(ref->mnt, ref->node, 0, &ofd->fhandle);
    if (rc != 0) {
        fd_ofd_unref(ofd);
        return rc;
    }
    vfs_ref_hold(ref);
    ofd->vmnt = ref->mnt;
    ofd->vnode = ref->node;
    ofd->ino = root ? krffs_node_ino(node) : vst->ino;
    vfs_desc_open(ref->mnt);
    k_memset(&e, 0, sizeof(e));
    e.type = FD_TYPE_FILE;
    e.flags = FD_FLAG_READ;
    e.ofd = ofd;
    e.node = root ? ref->node : NULL;
    f->map = filemap_new(&e);
    fd_ofd_unref(ofd);
    return f->map ? 0 : -ENOMEM;
}

/** Transfer the resolved entry, or share the descriptor execution selected. */
static void exec_keep_entry(struct mm *keep, const struct open_file *link, struct vfs_name **entry)
{
    struct open_file *file = keep->interp_file ? keep->interp_file : keep->exe_file;

    file->name = *entry ? *entry : vfs_name_ref(link ? link->name : NULL);
    *entry = NULL;
}

/* How `exec_open` finds the file: by a walk of its path that follows every
 * symbolic link, or one whose last component may not be a link, as
 * execveat's AT_SYMLINK_NOFOLLOW has it; a `how` of 0 or more is instead a
 * descriptor, whose file is opened. */
#define EXEC_WALK          (-1)
#define EXEC_WALK_NOFOLLOW (-2)

/**
 * The file the caller's descriptor `fd` is open on, for execveat's
 * AT_EMPTY_PATH, into `ref` as `proc_fd_file` leaves it, with `*held` the
 * description: the file /proc/self/fd/N reaches, without the walk to it,
 * so a process whose root has no /proc runs it too, as on Linux. 0,
 * -EBADF for a descriptor that is not open, or `proc_fd_file`'s error.
 */
static int32_t exec_fd_file(int fd, struct vfs_ref *ref, struct open_file **held)
{
    struct vpath vp;

    if (fd_get(current_proc->fds, fd) == NULL) {
        return -EBADF;
    }
    k_memset(&vp, 0, sizeof(vp));
    vp.kind = VNODE_PROC_FD_LINK;
    vp.pid = SYNTH_PID_SELF;
    vp.procfd = fd;
    return proc_fd_file(&vp, ref, held);
}

/** Whether the node `ref` a walk that did not follow its last component
 * ended on is a symbolic link: a link of a mounted volume or of the KRFFS
 * root, or a /proc/PID/fd/N link, which execveat with AT_SYMLINK_NOFOLLOW
 * refuses as Linux does. */
static int exec_is_link(const struct vfs_ref *ref)
{
    const struct vpath *vp = vfs_synth_vpath(ref->mnt, ref->node);
    struct vfs_stat vst;

    if (vp != NULL) {
        return vp->kind == VNODE_PROC_FD_LINK;
    }
    if (mnt_is_root(ref->mnt)) {
        return krffs_mode_type(((const krffs_node_t *)ref->node)->mode) == 0120000u;
    }
    return vfs_getattr(ref->mnt, ref->node, &vst) == 0 && (vst.mode & 0170000u) == 0120000u;
}

/**
 * Check that the node `ref` an exec's walk ended on is a program the caller
 * may run, as `exec_open` says, and open it into `f`: its mode and owner go
 * in `*vst`, and a file off the KRFFS root is opened and its reference kept
 * in `f->ref`, held until `exec_file_close`. `keep` is exec_open's. 0 or a
 * negative errno.
 */
static int32_t exec_check(struct exec_file *f, const struct vfs_ref *ref, const struct mm *keep,
                          struct vfs_stat *vst)
{
    int32_t rc;

    if (vfs_synth_vpath(ref->mnt, ref->node) || (ref->mnt->flags & VFS_MS_NOEXEC)) {
        return -EACCES;
    }
    if (vfs_getattr(ref->mnt, ref->node, vst) == 0 && (vst->mode & 0170000u) == 0040000u) {
        return -EACCES;
    }
    if (mnt_is_root(ref->mnt)) {
        const krffs_node_t *node = ref->node;

        rc = cred_access_node(node, CRED_X, 0);
        f->readable = cred_access_node(node, CRED_R, 0) == 0;
        if (rc == 0 && keep == NULL && krffs_node_path(ref->node, f->path, sizeof(f->path)) != 0) {
            rc = -ENAMETOOLONG;
        }
        vst->mode = node->mode;
        vst->uid = node->uid;
        vst->gid = node->gid;
        return rc;
    }
    rc = cred_access_ref(ref, CRED_X, 0);
    if (rc == 0) {
        f->readable = cred_access_ref(ref, CRED_R, 0) == 0;
        rc = vfs_getattr(ref->mnt, ref->node, vst);
    }
    if (rc == 0) {
        rc = vfs_open_node(ref->mnt, ref->node, 0, &f->handle);
    }
    if (rc == 0) {
        f->ref = *ref;
    }
    return rc;
}

/**
 * Open the file at the absolute, canonical `path` into `f` for execve to
 * read, and when `id` is not NULL, say in it what the file's set-id bits
 * would do. `path` starts at the caller's root, which chroot may have
 * moved. `keep`, the new address space, keeps the program and its ELF
 * interpreter (`mm_exe_open`), excluding writers while the walk
 * still holds the node's mount locked; one on the KRFFS root is then read
 * through that, and any other file on the root by its node's own path, which
 * `f` keeps. `f` also says whether the caller may read the file, which the
 * kernel loads for it with the execute right alone, and has the mapping of
 * the file the loader maps its segments by (`exec_map`). The program names
 * the new address space by the path the walk took to its file, its symbolic
 * links resolved (`resolve_named_real`), or by `path` when that does not fit:
 * a second walk of `path` could meet a link replaced in between and name
 * another file.
 *
 * A path that named a directory by its end, `end` (`copy_path_cwd`), must
 * reach one, and the walk that finds the file answers for it
 * (`resolve_named`): a file so named is -ENOTDIR. The node it checked is the
 * node opened, so a symbolic link replaced while the walk waits for a disk
 * cannot hand the open a program the check never saw. `how` is EXEC_WALK;
 * EXEC_WALK_NOFOLLOW, for which a last component that is a symbolic link is
 * -ELOOP; or a descriptor of the caller's, whose file is opened in place of
 * a walk (`exec_fd_file`), `path` naming it.
 *
 * A walk that ends on a /proc/PID/fd/N link goes on to the file the
 * descriptor is open on (`proc_fd_file`), as Linux's does, which is how
 * the C library's fexecve runs a descriptor without execveat; every check
 * below is made of that file, as of one its own path named. The program's
 * name in its address space, which /proc/PID/exe and maps give, is then the
 * path the descriptor was opened by, when it has one. Any other end on the
 * synthetic /dev and /proc mounts, which hold no programs, is refused, so
 * execve sees the mount boundary open sees and cannot reach a file a
 * hostile image placed below the synthetic prefix; a mount with MS_NOEXEC
 * holds none either. A directory is no program and is -EACCES, as Linux's
 * open for an exec answers. The caller needs the execute right, root too.
 * The set-id facts are taken while the node is at hand and apply only once
 * the exec commits. Setgid counts only with the group-execute bit beside
 * it, which is Linux's rule: without it a 02700 file owned 1000:0 passes the
 * execute check on its owner bits, since the check judges one identity
 * class and the owner's first, and would hand its executor egid 0. Setgid
 * without group-execute is the mark of mandatory locking in Unix, which
 * nothing here implements. On a mount with MS_NOSUID the bits are read and
 * ignored. 0 or a negative errno.
 */
static int32_t exec_open(struct exec_file *f, const char *path, int end, int how,
                         struct exec_setid *id, struct mm *keep)
{
    struct open_file *link = NULL;
    struct vfs_name *entry = NULL;
    struct vfs_ref ref;
    struct vfs_stat vst;
    char *real = path_buf_get();
    int nosuid;
    int32_t rc;

    f->ref.mnt = NULL;
    f->handle = NULL;
    f->held = NULL;
    f->map = NULL;
    f->err = 0;
    f->readable = 0;
    if (real == NULL) {
        return -ENOMEM;
    }
    real[0] = '\0';
    if (how >= 0) {
        rc = exec_fd_file(how, &ref, &link);
    } else {
        rc =
            resolve_named_entry(path + (path[0] == '/'),
                                VFS_WALK_LOCK | (how == EXEC_WALK_NOFOLLOW ? VFS_WALK_NOFOLLOW : 0),
                                end, &ref, real, &entry);
        if (rc == 0 && how == EXEC_WALK_NOFOLLOW && exec_is_link(&ref)) {
            vfs_walk_unlock(ref.mnt);
            vfs_ref_release(&ref);
            rc = -ELOOP;
        } else if (rc == 0) {
            rc = exec_follow_fd(&ref, &link);
        }
    }
    if (rc != 0) {
        vfs_name_put(entry);
        path_buf_put(real);
        return rc;
    }
    nosuid = (ref.mnt->flags & VFS_MS_NOSUID) != 0;
    rc = exec_check(f, &ref, keep, &vst);
    if (rc == 0) {
        rc = exec_map(f, &ref, &vst);
    }
    if (rc == 0 && keep != NULL) {
        rc = exec_keep(f, &ref, keep, link, real, path);
        if (rc == 0) {
            exec_keep_entry(keep, link, &entry);
        }
    }
    vfs_name_put(entry);
    path_buf_put(real);
    vfs_walk_unlock(ref.mnt);
    if (f->ref.mnt == NULL) {
        vfs_ref_release(&ref);
    }
    fd_ofd_unref(link);
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
 * and the interpreter is opened in the script's place, as the program the
 * new address space keeps and is named by, which is Linux's exe for a
 * script: its open (`exec_keep`) replaces the script's name, the one its
 * descriptor gave among them. A set-id script runs as its invoker, as on
 * Linux: the interpreter is what runs, it is not set-id, and the script's
 * bits would be unsafe, since the interpreter can be made to read a
 * different file than the one whose bits were checked. An interpreter that
 * is itself a script is -ENOEXEC: nothing here needs a chain of them, and a
 * chain could loop. A script execveat reached through a close-on-exec
 * descriptor is -ENOENT, since its interpreter could not open it, as Linux's
 * BINPRM_FLAGS_PATH_INACCESSIBLE has it. 0, -ENOEXEC, -ENOENT, or an error
 * of the open or the read.
 */
static int32_t exec_script(struct exec_state *s)
{
    Elf_Ehdr ehdr;
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
    mm_exe_close(s->mm);
    if (rc <= 0) {
        return rc < 0 ? rc : -ENOEXEC;
    }
    if (s->path_gone) {
        return -ENOENT;
    }
    s->id.mode = 0;
    rc = exec_open(&s->prog, s->path, s->path_end, EXEC_WALK, NULL, s->mm);
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
 * Whether the calling thread has a tracer that is not privileged
 * (`ptrace_tracer_privileged`), which could write the new image's memory and
 * registers at the exec's stop and so act with any ids or capabilities the
 * image would be given. Linux's `ptracer_capable` makes the same test of the
 * credentials the tracer had when it attached.
 */
static int exec_traced(void)
{
    return (current_proc->ptrace & PT_TRACED) && !ptrace_tracer_privileged(current_proc);
}

/**
 * Whether the calling thread's working directory, root and umask are shared
 * with a process outside its thread group, which could change them under
 * the new image once it had the ids its set-id bits give, as Linux's
 * `check_unsafe_exec` tests: more records share them than threads of the
 * caller's process do.
 */
static int exec_fs_shared(void)
{
    const process_t *t;
    int sharers = 0;

    for (t = proc_list; t; t = t->next) {
        if (t->tg == current_proc->tg && t->fs == current_proc->fs) {
            sharers++;
        }
    }
    return current_proc->fs->refs > sharers;
}

/**
 * Whether the image `s` describes may run with an effective uid of root in
 * the caller's user namespace, with the caller's or, where `mode` has the
 * set-user-ID bit, the file owner's: either, since `exec_ids` may yet ignore
 * the bit. A thread whose capabilities its last exec gave from the ambient
 * set (`amb_granted`) regains others only in such an image.
 */
static int exec_root_image(const struct exec_state *s, uint32_t mode)
{
    return uid_seen(current_proc->euid) == 0 ||
           ((mode & KRFFS_MODE_SUID) && uid_seen(s->id.uid) == 0);
}

/**
 * The ids the new image runs with, into `s`: the file owner's where a
 * set-id bit applies, the way privilege is regained, and the caller's
 * effective ids otherwise. `secure`, which is AT_SECURE, says whether the
 * image runs with authority its invoker lacks, the effective id differing
 * from the real one either way, so libc distrusts the environment the
 * caller gave it.
 *
 * A process that set no_new_privs gains nothing: its bits are read and
 * ignored, as a nosuid mount's are, and neither does one in a user
 * namespace whose maps do not cover the file's owner and group, as on Linux,
 * so a namespace's root cannot run a program as root. Neither does a caller
 * that is not privileged whose thread has a tracer that is not
 * (`exec_traced`), or whose directories and umask another process shares
 * (`exec_fs_shared`), since either could change the image once it had the
 * ids; Linux's `cap_bprm_creds_from_file` ignores the bits in the same cases
 * for a caller without CAP_SETUID. No tracer that is not root can attach while
 * the exec runs (`ptrace_attach`), and an exec that applies the bits for
 * such a caller holds off CLONE_FS in the threads that share its directories
 * (`fork_fs`) until its workspace goes, so the choice made here holds until
 * the image is the process's.
 *
 * A thread that gave up capabilities has those of its bounding set and its
 * inheritable set again after the exec, as Linux gives a root image
 * pI | pB, except in those three cases, where Linux keeps the new permitted
 * set to the old one; an exec that gives any back holds off CLONE_FS as one
 * that applies set-id bits does. A capability dropped from the bounding set
 * and not inheritable stays lost. The ambient set is within the inheritable
 * set and the capabilities held, so every case keeps it; `exec_ambient`
 * then gives an image whose ids give none those of the ambient set. A
 * thread that holds those its last exec gave it so (`amb_granted`) holds no
 * others, and an image of root regains the rest as above, in the same three
 * cases keeping to those it held, as Linux keeps it; an image whose ids give
 * none, the next exec of a sandbox such as bubblewrap's, regains nothing and
 * holds its ambient set again. An exec never gives the image a capability
 * its caller's permitted set lacked under no_new_privs, a tracer that is not
 * privileged or shared directories.
 */
static void exec_ids(struct exec_state *s)
{
    uint32_t mode = current_proc->no_new_privs ? 0u : s->id.mode;
    uint32_t root_lost[2], i;
    int gains = 0;

    if (!ns_ids_mapped(proc_userns(current_proc), s->id.uid, s->id.gid)) {
        mode = 0;
    }

    for (i = 0; i < 2; i++) {
        uint32_t lost = current_proc->cap_lost[i];

        root_lost[i] = current_proc->bset_lost[i] & ~current_proc->cap_inh[i];
        s->cap_lost[i] = lost | root_lost[i];
        if (lost & ~root_lost[i]) {
            gains = 1;
        }
    }
    if (current_proc->amb_granted && !exec_root_image(s, mode)) {
        gains = 0;
    }
    s->regains = gains && !current_proc->no_new_privs;
    if ((mode != 0 && !proc_privileged(current_proc)) || s->regains) {
        if (exec_traced() || exec_fs_shared()) {
            mode = 0;
            s->regains = 0;
        } else if (s->regains) {
            s->cap_lost[0] = root_lost[0];
            s->cap_lost[1] = root_lost[1];
        }
        if (mode != 0 || s->regains) {
            s->fs_held = current_proc->fs;
            s->fs_held->in_exec++;
        }
    }

    s->euid = (mode & KRFFS_MODE_SUID) ? s->id.uid : current_proc->euid;
    s->egid = (mode & KRFFS_MODE_SGID) ? s->id.gid : current_proc->egid;
    s->secure = s->euid != current_proc->ruid || s->egid != current_proc->rgid;
}

/**
 * Apply `p`'s ambient set at an exec, once the image's ids and
 * capabilities are its own, as Linux's `cap_bprm_creds_from_file` does for
 * an image without file capabilities. Linux empties the set for an image
 * that runs with ids its caller lacks (`secure`, its is_setid), and
 * otherwise keeps it and gives the image those capabilities, effective and
 * permitted, whatever its uid. An image of root holds them already, since
 * the ambient set is within those its caller held and the inheritable set
 * (`exec_ids`). An image whose ids give no capability (`cap_ids`) holds
 * those of the ambient set and loses the rest (`amb_granted`), so that a
 * sandbox an ordinary user starts, such as bubblewrap's --cap-add in a user
 * namespace whose uid is not 0, keeps what it was asked to keep.
 */
static void exec_ambient(process_t *p, int secure)
{
    uint32_t i;

    p->amb_granted = 0;
    if (secure) {
        p->cap_amb[0] = p->cap_amb[1] = 0;
    }
    if (!cap_ids(p) && (p->cap_amb[0] | p->cap_amb[1]) != 0) {
        p->amb_granted = 1;
        cap_store(p, ~p->cap_amb[0], ~p->cap_amb[1]);
    }
    for (i = 0; i < 2; i++) {
        p->cap_amb[i] &= cap_word(p, i);
    }
}

/* personality(2)'s flag that turns randomization off, which GDB and
 * `setarch -R` set, and the flags Linux's PER_CLEAR_ON_SETID clears for an
 * image that runs with authority its caller lacks: READ_IMPLIES_EXEC,
 * ADDR_NO_RANDOMIZE, ADDR_COMPAT_LAYOUT and MMAP_PAGE_ZERO. */
#define ADDR_NO_RANDOMIZE  0x0040000u
#define PER_CLEAR_ON_SETID 0x0740000u

/** A random whole number of pages below `pages`, a power of two of at most
 * 2^32, as an offset in bytes; 0 when `pages` is 0. */
static uintptr_t aslr_pages(uintptr_t pages)
{
    if (pages == 0) {
        return 0;
    }
    return ((uintptr_t)krandom_u32() & (pages - 1u)) * PAGE_SIZE;
}

/**
 * Choose the new image's layout and its persona into `s`, as Linux's
 * `load_elf_binary` chooses them, and move its stack, whose strings are
 * copied, to the top chosen. Called once `exec_ids` has set the ids.
 *
 * The persona is the caller's, less ADDR_NO_RANDOMIZE and the other flags
 * of PER_CLEAR_ON_SETID when the image runs with ids its caller lacks or
 * has capabilities given back, so that no caller fixes the addresses of a
 * set-user-ID program. When kernel.randomize_va_space is 0 or the persona
 * has ADDR_NO_RANDOMIZE, the image is at the fixed addresses of cpu.h: its
 * stack ends at USER_ADDR_MAX, its mmap base is USER_MMAP_BASE, a
 * position-independent program goes at ELF_ET_DYN_BASE and the heap starts
 * at the program's end. Otherwise each offset is drawn on its own, under
 * the architecture's ASLR_ bounds: the stack's top is lowered by a random
 * number of pages and the entry stack pointer by a random multiple of 16
 * bytes, the mmap base is lowered and the program's base raised by random
 * numbers of pages, and at 2 the heap's start is raised by one.
 *
 * An i386 program on the x86-64 kernel (`s->compat`) has the layout Linux
 * gives a compat task, the IA32_ addresses and bounds of cpu.h, and its
 * stack's words are 4 bytes, with AT_PLATFORM "i686", Linux's
 * COMPAT_ELF_PLATFORM.
 */
static void exec_layout(struct exec_state *s)
{
    uintptr_t stack_pages = ASLR_STACK_PAGES;
    uintptr_t mmap_pages = ASLR_MMAP_PAGES;
    uint32_t pad = 0;

    s->persona = current_proc->persona;
    if (s->secure || s->regains) {
        s->persona &= ~PER_CLEAR_ON_SETID;
    }
    s->stack_top = USER_ADDR_MAX;
    s->mmap_base = USER_MMAP_BASE;
    s->dyn_base = ELF_ET_DYN_BASE;
#if ELF_IA32
    if (s->compat) {
        s->stack_top = IA32_TASK_SIZE;
        s->mmap_base = IA32_MMAP_BASE;
        s->dyn_base = IA32_ET_DYN_BASE;
        stack_pages = IA32_ASLR_STACK_PAGES;
        mmap_pages = IA32_ASLR_MMAP_PAGES;
        s->args.word = 4u;
        s->args.platform = "i686";
    }
#endif
    s->heap_shift = 0;
    if (randomize_va_space != 0 && !(s->persona & ADDR_NO_RANDOMIZE)) {
        s->stack_top -= aslr_pages(stack_pages);
        if (ASLR_SP_BYTES != 0) {
            pad = krandom_u32() & (uint32_t)(ASLR_SP_BYTES - 1u) & ~15u;
        }
        s->mmap_base -= aslr_pages(mmap_pages);
        s->dyn_base += aslr_pages(mmap_pages);
        if (randomize_va_space >= 2) {
            s->heap_shift = aslr_pages(ASLR_BRK_PAGES);
        }
    }
    uimage_move(&s->args, s->stack_top, pad);
}

/**
 * Load the interpreter the program's PT_INTERP names into the new address
 * space, as Linux's `load_elf_interp` loads it: a position-independent one,
 * which every dynamic linker is, at the top of the range mmap places mappings
 * in, just below the image's mmap base, and one with fixed addresses at those.
 * Its own PT_INTERP is ignored. The new image starts at its entry, and `*base`,
 * AT_BASE, is the distance it was moved by.
 *
 * The path is held to the rule execve's own is (`canonicalize_named`), in
 * the one walk of the open: one that names a directory by its end must
 * reach one, so a file so named is -ENOTDIR and a directory the open's
 * -EACCES, as Linux's open of the interpreter answers. 0, -ENOENT, -ENOTDIR,
 * -EACCES, -ELIBBAD for a file that is not an interpreter this kernel loads
 * or one of another class than the program's, an i386 interpreter of an
 * x86-64 program or the reverse, -ENOMEM for one that would cover the
 * program or for memory its load could not have, -EIO or a mount's error
 * for a read that failed.
 */
static int32_t exec_interp(struct exec_state *s, uintptr_t *base)
{
    uintptr_t bias = 0;
    int end;
    int32_t rc;

    rc = canonicalize_named(s->layout.interp, &end);
    if (rc < 0) {
        return rc;
    }
    rc = exec_open(&s->prog, s->layout.interp, end, EXEC_WALK, NULL, s->mm);
    if (rc != 0) {
        return rc;
    }
    if (!s->prog.readable) {
        s->unreadable = 1;
    }
    rc = elf_read_layout(exec_file_read, &s->prog, 0, &s->ilayout);
    if (rc != 0) {
        return exec_load_err(&s->prog, rc, -ELIBBAD);
    }
    if (s->ilayout.cls != s->layout.cls) {
        return -ELIBBAD;
    }
    if (s->ilayout.type == ET_DYN) {
        bias = s->mmap_base - (s->ilayout.hi - s->ilayout.lo) - s->ilayout.lo;
    }
    if (s->ilayout.lo + bias < s->img.image_end && s->ilayout.hi + bias > s->img.segs[0].start) {
        return -ENOMEM;
    }
    rc = elf_load_image_at(exec_file_read, &s->prog, s->mm->dir, bias, s->prog.map, &s->iimg);
    s->prog.map = NULL;
    if (rc != 0) {
        return exec_load_err(&s->prog, rc, -ELIBBAD);
    }
    exec_file_close(&s->prog);
    s->entry = s->iimg.entry;
    *base = bias;
    return 0;
}

/* The auxiliary vector an image is given fits the address space's copy of
 * it, which exec_build and the boot's first program store whole. */
typedef char
    mm_auxv_holds_uimage[sizeof(((struct mm *)0)->auxv) == sizeof(((struct uimage *)0)->auxv) ? 1
                                                                                              : -1];

/**
 * Build the new image in `s`'s address space of its own while the caller's
 * still runs, as Linux's `load_elf_binary` builds one, in the layout
 * `exec_layout` chose for the program whose layout `exec_classify` read: the
 * program, at its base when it is position-independent and linked below
 * ELF_ET_DYN_BASE, or IA32_ET_DYN_BASE for an i386 one; the interpreter its
 * PT_INTERP names; the vDSO of the program's ABI, one free page above the
 * range mmap places mappings in, so that no mapping there ends against it
 * (`vdso_map`); the regions; and the stack, with the pointers and the
 * auxiliary vector below its strings, an i386 program's with AT_HWCAP and
 * AT_SYSINFO beside the rest. The address space is marked an i386 one for
 * such a program, and keeps the vDSO's address. Each of the program
 * and the interpreter maps its segments from its file where it can, and the
 * reference to the file's mapping goes from the open file to the image, which
 * keeps it until `exec_release`. The stack may grow to the soft RLIMIT_STACK,
 * lowered to 8 MiB for an image that runs with ids its caller lacks, and the
 * regions may cover no more than RLIMIT_AS. The caller's image is untouched, so
 * every failure is an error execve returns. 0, -ENOEXEC, -ENOMEM, -EIO, or an
 * error of `exec_interp` or of a mount's read.
 */
static int32_t exec_build(struct exec_state *s)
{
    struct uimage_aux aux;
    uintptr_t bias = 0;
    uintptr_t bottom = uimage_bottom(&s->args);
    uintptr_t vdso_at = s->mmap_base + PAGE_SIZE;
    unsigned long limit = current_proc->rlim[RLIM_STACK].cur;
    uintptr_t dyn_floor = ELF_ET_DYN_BASE;
    int abi = s->compat ? VDSO_IA32 : VDSO_NATIVE;
    uint8_t random16[16];
    intptr_t ehdr = 0;
    int32_t rc;

    k_memset(&aux, 0, sizeof(aux));
#if ELF_IA32
    if (s->compat) {
        dyn_floor = IA32_ET_DYN_BASE;
    }
#endif
    if (s->layout.type == ET_DYN && s->layout.lo < dyn_floor) {
        bias = s->dyn_base - s->layout.lo;
    }
    rc = elf_load_image_at(exec_file_read, &s->prog, s->mm->dir, bias, s->prog.map, &s->img);
    s->prog.map = NULL;
    if (rc != 0) {
        return exec_load_err(&s->prog, rc, -ENOEXEC);
    }
    s->unreadable = !s->prog.readable;
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
    if (elf_setup_vmas(&s->mm->vm, &s->img, s->img.image_end + s->heap_shift,
                       s->stack_top - PAGE_SIZE, exec_stack_window(s, bottom, limit),
                       s->mmap_base) != 0 ||
        (s->layout.interp[0] != '\0' && elf_add_vmas(&s->mm->vm, &s->iimg) != 0) ||
        (ehdr = vdso_map(&s->mm->vm, vdso_at, abi)) < 0 ||
        (bottom < s->stack_top - PAGE_SIZE && !vm_grow_stack(&s->mm->vm, bottom, RLIM_INFINITY)) ||
        vm_total(&s->mm->vm) > current_proc->rlim[RLIM_AS].cur) {
        return -ENOMEM;
    }
    aux.sysinfo_ehdr = (uintptr_t)ehdr;
#if ELF_IA32
    if (s->compat) {
        aux.hwcap = ia32_hwcap();
        aux.sysinfo = ehdr != 0 ? vdso32_vsyscall_at((uintptr_t)ehdr) : 0;
    }
#endif
    s->mm->compat = (uint32_t)s->compat;
    s->mm->vdso = (uintptr_t)ehdr;
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
    k_memcpy(s->mm->auxv, s->args.auxv, sizeof(s->mm->auxv));
    if (s->layout.interp[0] != '\0') {
        mm_set_interp(s->mm, s->layout.interp);
    }
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
 * record, its alarm and its stop or continue report, as Linux's `de_thread`
 * exchanges the two ids.
 */
static void exec_take_leader(void)
{
    struct tgroup *tg = current_proc->tg;
    process_t *old = tg->leader;

    proc_exchange_pids(current_proc, old);
    current_proc->alarm_tick = old->alarm_tick;
    current_proc->alarm_interval = old->alarm_interval;
    current_proc->jc_status = old->jc_status;
    proc_thread_fold(old);
    tg->leader = current_proc;
    mm_drop(old);
    kstack_free(old->kernel_stack);
    free_process(old);
    proc_timer_update(current_proc);
}

/**
 * Make the current thread its process's only one before execve replaces the
 * image, as Linux's `de_thread` does: every other thread is sent SIGKILL and
 * the caller waits, whatever it is sent, until they have ended. While
 * `exec_task` is set, a thread the SIGKILL ends ends alone and does not make
 * the process end.
 *
 * A process that is ending, or one another of whose threads is in this wait
 * already, has sent the caller SIGKILL too. The caller then sends nothing
 * and leaves `exec_task` to its owner: the exec returns -EAGAIN, and the
 * thread ends on its way back to user mode, as Linux's `de_thread` has its
 * exec give up. Two execs that both waited would each wait for the other,
 * since neither wait acts on a signal. 0 or -EAGAIN.
 */
static int32_t exec_one_thread(void)
{
    struct tgroup *tg = current_proc->tg;
    struct k_siginfo info;
    process_t *t;

    if (tg->exiting || tg->exec_task != NULL) {
        return -EAGAIN;
    }
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
    return 0;
}

/**
 * Set aside for a caller that shares its descriptor table the storage of
 * the table of its own the new image gets, which `exec_files` fills at the
 * commit, so that the allocation fails while the exec can still return.
 * The entries copied into it now name descriptions it holds no references
 * to, and the commit writes over them. A table never shrinks, but another
 * sharer may grow it while the exec sleeps, so `exec_files_fit` widens the
 * storage again after the last sleep. 0, or -ENOMEM.
 */
static int32_t exec_files_stage(struct exec_state *s)
{
    if (current_proc->fds->refs == 1) {
        return 0;
    }
    s->files = fd_table_dup(current_proc->fds);
    return s->files != NULL ? 0 : -ENOMEM;
}

/** Widen the storage `exec_files_stage` set aside to the shared table as it
 * is now, after `exec_one_thread`, the exec's last sleep; nothing between
 * here and the commit sleeps, so no sharer grows the table again. 0, or
 * -ENOMEM. */
static int32_t exec_files_fit(struct exec_state *s)
{
    if (s->files == NULL || current_proc->fds->refs == 1) {
        return 0;
    }
    return fd_table_fit(s->files, current_proc->fds->cap);
}

/**
 * Give the caller, as the exec commits, a descriptor table of its own in the
 * storage `exec_files_stage` set aside, when the table it shares still has
 * another sharer: a copy of the table as it is now, made as `files_copy`
 * makes one, so the close-on-exec descriptors are closed for the new image
 * alone and the other sharers keep the table. A caller whose other sharers
 * have all gone keeps the table, and the storage goes back with the
 * workspace.
 */
static void exec_files(struct exec_state *s)
{
    fd_table_t *shared = current_proc->fds;
    fd_table_t *own = s->files;

    if (own == NULL || shared->refs == 1) {
        return;
    }
    k_memcpy(own->entries, shared->entries, sizeof(fd_entry_t) * (size_t)shared->cap);
    own->next_free = 0;
    files_take(own);
    s->files = NULL;
    proc_files_put(current_proc);
    current_proc->fds = own;
}

/**
 * Make the image `s` built the process's, once `exec_one_thread` has ended
 * the process's other threads; past here execve cannot fail.
 *
 * The thread first releases the robust futexes it holds and clears the word
 * CLONE_CHILD_CLEARTID names, while the old image is still there to hold them,
 * and under its own id, which the words it locked hold; a thread that is not
 * the leader then takes the leader's place and id. Gives a caller that shares
 * its descriptor table one of its own (`exec_files`) and drops the
 * close-on-exec descriptors; takes the new ids, the saved ids becoming the new
 * effective ones, which is the POSIX rule for every exec and what lets an image
 * shed acquired authority and take it back, and the file-system ids the
 * effective ones again; resets what pointed into the old
 * image: caught signals' handlers (SIG_IGN, the blocked mask and the pending
 * signals are kept, as POSIX says), the alternate signal stack, the TLS base
 * and the floating-point state, and deletes the POSIX timers, as POSIX says.
 * Then it switches to the new address space and puts the old one, which the
 * parent of a vfork child keeps, lets that parent go, and points the return
 * frame at the entry with the stack pointer at argc. A fresh image has no TLS
 * until its libc calls set_thread_area, so the segment registers get the plain
 * user-data selector, as a new process gets it, in place of the old image's TLS
 * one. The general registers are cleared, since the System V entry has argc on
 * the stack, a register that holds an atexit hook or zero, and a frame pointer
 * of 0 marking the outermost frame (`arch_exec_frame`).
 */
static void exec_commit(isr_params_t *params, struct exec_state *s)
{
    struct mm *old = current_proc->mm;
    int ids_moved = current_proc->euid != s->euid || current_proc->egid != s->egid ||
                    proc_fsuid(current_proc) != s->euid || proc_fsgid(current_proc) != s->egid;
    int was_privileged = proc_privileged(current_proc);
    int sig;

    proc_thread_gone(current_proc);
    current_proc->futex_exit_state = 0;
    if (current_proc != current_proc->tg->leader) {
        exec_take_leader();
    }
    ptimer_delete_all(current_proc->tg);
    current_proc->tg->execed = 1;
    current_proc->tg->no_child_signal = 0;
    proc_exec_generation(current_proc);
    exec_files(s);
    close_cloexec_fds();
    current_proc->euid = s->euid;
    current_proc->egid = s->egid;
    current_proc->suid = s->euid;
    current_proc->sgid = s->egid;
    current_proc->fs_own = 0;
    cap_store(current_proc, s->cap_lost[0], s->cap_lost[1]);
    current_proc->persona = s->persona;
    ns_exec(current_proc);
    exec_ambient(current_proc, s->secure);
    exec_privileges(s->mm, s->secure, s->unreadable, ids_moved, was_privileged);
    proc_set_comm(current_proc, s->script[0] ? s->script : s->path);
    /* Linux's flush_signal_handlers: a caught signal reverts to its
     * default, an ignored one stays ignored, and each loses its flags,
     * restorer and mask, which rt_sigaction reports afterwards. */
    for (sig = 1; sig <= KSIG_MAX; sig++) {
        uintptr_t handler = current_proc->sigacts[sig].handler == KSIG_IGN ? KSIG_IGN : KSIG_DFL;

        k_memset(&current_proc->sigacts[sig], 0, sizeof(struct k_sigaction));
        current_proc->sigacts[sig].handler = handler;
    }
    current_proc->clear_child_tid = 0;
    rseq_exec(current_proc);
    current_proc->altstack_on = 0;
    current_proc->altstack_sp = 0;
    current_proc->altstack_size = 0;
    current_proc->vma_reported = 0;

    current_proc->mm = s->mm;
    current_proc->space_root = s->mm->space_root;
    current_proc->page_dir = s->mm->dir;
    s->mm = NULL;
    switch_page_directory(current_proc->page_dir);
    mm_put(old);
    proc_vfork_release(current_proc);

    arch_exec_frame(params, s->entry, s->args.sp);
    current_proc->tls_base = 0;
    arch_tls_set(0);
    fpu_init_area(current_proc->fpu);
    fpu_restore(current_proc->fpu);
}

/**
 * Whether /proc in the calling process's root is the synthetic /proc: at
 * the root of the mount table, and in a root chroot gave the process that
 * has it by a bind, but not in one whose /proc is an ordinary directory.
 */
static int proc_is_synthetic(void)
{
    struct vfs_ref ref;
    const struct vpath *vp;
    int yes;

    if (!current_proc->fs->root_mnt) {
        return 1;
    }
    if (vfs_resolve(NULL, "proc", 0, &ref, NULL, 0) != 0) {
        return 0;
    }
    vp = vfs_synth_vpath(ref.mnt, ref.node);
    yes = vp && vp->kind == VNODE_PROC_DIR;
    vfs_ref_release(&ref);
    return yes;
}

/**
 * Replace the canonical path `s->path` by the path of the program the
 * /proc/<pid>/exe link it names reaches, if it names one, once `exec_open`
 * has gone on from the link to that program's file, and name the new address
 * space by it: Chromium runs its children by executing /proc/self/exe, and
 * the program they run is named by that path, not the link's, in its comm,
 * and in its own link where its file has no present path to give. For a
 * process whose root has no synthetic /proc (`proc_is_synthetic`), a path
 * under /proc is an ordinary one, so the path stays as the walk resolved it.
 * 0, or the link's error.
 */
static int32_t exec_link_name(struct exec_state *s)
{
    char *path = s->path;
    struct vpath vp;
    int pid;
    int n;

    if (synth_classify(path, &vp) != VNODE_PROC_EXE || !proc_is_synthetic()) {
        return 0;
    }
    pid = procfs_resolve_id(&vp);
    n = pid < 0 ? -ENOENT : procfs_exe_name(pid, path, VFS_PATH_MAX - 1);
    if (n < 0) {
        return n;
    }
    path[n] = '\0';
    mm_set_exe(s->mm, path);
    return 0;
}

int32_t proc_files_unshare(void)
{
    fd_table_t *own;

    if (current_proc->fds->refs == 1) {
        return 0;
    }
    own = files_copy(current_proc->fds);
    if (!own) {
        return -ENOMEM;
    }
    proc_files_put(current_proc);
    current_proc->fds = own;
    return 0;
}

int32_t proc_fs_unshare(void)
{
    struct fs_struct *was = current_proc->fs, *own;

    if (was->refs == 1) {
        return 0;
    }
    own = kzalloc(sizeof(*own));
    if (!own) {
        return -ENOMEM;
    }
    if (was->cwd_long) {
        own->cwd_long = kmalloc(k_strlen(was->cwd_long) + 1);
        if (!own->cwd_long) {
            kfree(own);
            return -ENOMEM;
        }
        k_memcpy(own->cwd_long, was->cwd_long, k_strlen(was->cwd_long) + 1);
    }
    k_memcpy(own->cwd, was->cwd, sizeof(own->cwd));
    own->refs = 1;
    own->umask = was->umask;
    current_proc->fs = own;
    vfs_cwd_set(current_proc, was->cwd_mnt, was->cwd_node);
    vfs_root_set(current_proc, was->root_mnt, was->root_node);
    was->refs--;
    return 0;
}

/**
 * Read the layout of the program `s` has open into `s->layout`, and from its
 * class whether it is an i386 program, which the x86-64 kernel runs beside
 * its own (`s->compat`), as Linux's binfmt handlers each recognize their
 * class, before the layout is chosen. 0, -ENOEXEC for a file that is not a
 * program this kernel loads, or a read's error (`exec_load_err`).
 */
static int32_t exec_classify(struct exec_state *s)
{
    int32_t rc = elf_read_layout(exec_file_read, &s->prog, 1, &s->layout);

    if (rc != 0) {
        return exec_load_err(&s->prog, rc, -ENOEXEC);
    }
    s->compat = s->layout.cls != ELF_CLASS;
    return 0;
}

/**
 * Find the program the call `c` names into `s->path`, and how `exec_open`
 * reaches it into `*how`, as Linux's `do_execveat_common` names it: a path
 * copied and made canonical, from the working directory or from the
 * directory `c->dirfd` is open on, walked to its end, or under
 * AT_SYMLINK_NOFOLLOW with a last component that may not be a link; or,
 * for an empty path with AT_EMPTY_PATH, the file `c->dirfd` is open on,
 * named "/dev/fd/N". AT_EXECFN, the stack's first string, is the caller's
 * path when it is absolute or taken from the working directory, and
 * otherwise "/dev/fd/N/path", or "/dev/fd/N" for the descriptor itself, as
 * Linux's `bprm->filename`; such a name goes into the page `*name`, which
 * the caller gives back, and is NULL for the caller's own. Such a program
 * reached through a close-on-exec descriptor has no path the new image
 * could open, which `s->path_gone` records. An empty path with
 * AT_EMPTY_PATH and the working directory names a directory, -EACCES. 0,
 * -EFAULT, -EINVAL for an unknown flag, -EBADF for a descriptor that is not
 * open, -ENOMEM, or the path's error.
 */
static int32_t exec_name(struct exec_state *s, const struct exec_call *c, int *how, char **name)
{
    fd_entry_t *e;
    char first = 0;
    int n;
    int32_t rc;

    *how = EXEC_WALK;
    *name = NULL;
    if (c->path == NULL || copy_from_user(&first, (const void __user *)c->path, 1) != 0) {
        return -EFAULT;
    }
    if ((c->flags & ~(EXEC_AT_SYMLINK_NOFOLLOW | EXEC_AT_EMPTY_PATH)) != 0) {
        return -EINVAL;
    }
    if (first == '\0' && (c->flags & EXEC_AT_EMPTY_PATH)) {
        if (c->dirfd == EXEC_AT_FDCWD) {
            return -EACCES;
        }
        if (c->dirfd < 0 || fd_get(current_proc->fds, c->dirfd) == NULL) {
            return -EBADF;
        }
        *how = c->dirfd;
        s->path_end = 0;
        k_memcpy(s->path, "/dev/fd/", 9);
        k_itoa(c->dirfd, s->path + 8, sizeof(s->path) - 8u, 10, false);
    } else {
        rc = copy_path_exec_at(s->path, sizeof(s->path), c->dirfd, c->path, &s->path_end);
        if (rc < 0) {
            return rc;
        }
        if (c->flags & EXEC_AT_SYMLINK_NOFOLLOW) {
            *how = EXEC_WALK_NOFOLLOW;
        }
        if (c->dirfd == EXEC_AT_FDCWD || first == '/') {
            return 0;
        }
    }
    e = fd_get(current_proc->fds, c->dirfd);
    s->path_gone = e != NULL && (e->flags & FD_FLAG_CLOEXEC) != 0;
    *name = path_buf_get();
    if (*name == NULL) {
        return -ENOMEM;
    }
    k_memcpy(*name, "/dev/fd/", 9);
    k_itoa(c->dirfd, *name + 8, VFS_PATH_MAX - 8u, 10, false);
    if (*how >= 0) {
        return 0;
    }
    n = (int)k_strlen(*name);
    (*name)[n++] = '/';
    rc = strncpy_from_user(*name + n, c->path, VFS_PATH_MAX - (uint32_t)n);
    if (rc < 0) {
        return rc;
    }
    return rc == (int32_t)(VFS_PATH_MAX - (uint32_t)n) ? -ENAMETOOLONG : 0;
}

/**
 * Replace the current process image with the program the call `c` names,
 * in the workspace `s`; the body of `sys_execve` and `sys_execveat`.
 *
 * Makes the new image's address space, resolves the path (`exec_name`) and
 * checks the execute right, keeping the program's file with the address
 * space, copies the path, argv and envp onto the new stack's pages, resolves
 * a `#!` script to its interpreter, builds the new image, with the dynamic
 * linker it names, in that address space, sets aside the storage of a
 * descriptor table of the caller's own, ends the process's other threads,
 * and only then commits, so every refusal and every failure of the load
 * returns to the caller with its image and its descriptor table intact. The
 * vectors are of the caller's ABI, 4-byte pointers for an i386 call
 * (`exec_vec_get`). Returns 0 once the image is committed, with the return
 * frame at the new image's entry, or a negative errno.
 */
static int32_t exec_run(isr_params_t *params, struct exec_state *s, const struct exec_call *c)
{
    char *name = NULL;
    int how;
    int32_t rc;

    rc = exec_name(s, c, &how, &name);
    if (rc == 0) {
        s->mm = mm_new();
        rc = s->mm == NULL ? -ENOMEM : 0;
    }
    if (rc == 0) {
        rc = exec_open(&s->prog, s->path, s->path_end, how, &s->id, s->mm);
    }
    if (rc == 0) {
        rc = exec_link_name(s);
    }
    if (rc == 0) {
        rc = exec_copy_args(s, c, name);
    }
    path_buf_put(name);
    if (rc == 0) {
        rc = exec_script(s);
    }
    if (rc == 0) {
        rc = exec_classify(s);
    }
    if (rc == 0) {
        exec_ids(s);
        exec_layout(s);
        rc = uimage_place(&s->args, s->argc, s->envc);
    }
    if (rc == 0) {
        rc = exec_build(s);
    }
    if (rc == 0) {
        rc = exec_files_stage(s);
    }
    if (rc == 0) {
        rc = exec_one_thread();
    }
    if (rc == 0) {
        rc = exec_files_fit(s);
    }
    if (rc == 0) {
        uint32_t tid = current_proc->pid;

        exec_commit(params, s);
        ptrace_exec(tid);
    }
    return rc;
}

/**
 * Run the program the call `c` names in place of the caller's image: give
 * the call a workspace of its own, which `exec_run` does the work in, and
 * give it back when that returns, whether the exec failed or the new image
 * is about to be entered.
 */
static long exec_call_run(isr_params_t *params, const struct exec_call *c)
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
    rc = exec_run(params, s, c);
    exec_release();
    return rc;
}

/** Replace the current process image with a new program (execve(2)): the
 * path, argv[] and envp[], the path taken from the working directory. */
long sys_execve(isr_params_t *params)
{
    struct exec_call c;

    c.dirfd = EXEC_AT_FDCWD;
    c.path = (const char __user *)SC_ARG(params, 0);
    c.argv = (char *const __user *)SC_ARG(params, 1);
    c.envp = (char *const __user *)SC_ARG(params, 2);
    c.flags = 0;
    return exec_call_run(params, &c);
}

/**
 * execveat(2): execve with a directory descriptor, from which a relative
 * path is taken, and flags: AT_SYMLINK_NOFOLLOW, under which a path whose
 * last component is a symbolic link is ELOOP, and AT_EMPTY_PATH, under
 * which an empty path runs the file the descriptor is open on, as glibc's
 * fexecve does through it (`exec_name`). Its arguments are the descriptor,
 * the path, argv[], envp[] and the flags.
 */
long sys_execveat(isr_params_t *params)
{
    struct exec_call c;

    c.dirfd = (int)SC_ARG(params, 0);
    c.path = (const char __user *)SC_ARG(params, 1);
    c.argv = (char *const __user *)SC_ARG(params, 2);
    c.envp = (char *const __user *)SC_ARG(params, 3);
    c.flags = (uint32_t)SC_ARG(params, 4);
    return exec_call_run(params, &c);
}

/* clone(2) flags; the low byte is the signal the parent gets at the child's
 * exit. */
#define CSIGNAL              0x000000FFu
#define CLONE_VM             0x00000100u
#define CLONE_FS             0x00000200u
#define CLONE_FILES          0x00000400u
#define CLONE_SIGHAND        0x00000800u
#define CLONE_PIDFD          0x00001000u
#define CLONE_VFORK          0x00004000u
#define CLONE_PARENT         0x00008000u
#define CLONE_THREAD         0x00010000u
#define CLONE_SYSVSEM        0x00040000u
#define CLONE_SETTLS         0x00080000u
#define CLONE_PARENT_SETTID  0x00100000u
#define CLONE_CHILD_CLEARTID 0x00200000u
#define CLONE_DETACHED       0x00400000u
#define CLONE_UNTRACED       0x00800000u
#define CLONE_CHILD_SETTID   0x01000000u

/* clone3's flags past the 32 bits of clone's. */
#define CLONE_CLEAR_SIGHAND 0x100000000ull

/* What a fork or a clone gives `do_fork`: the clone flags, the stack
 * pointer the child starts with or 0 for the parent's, the parent's address
 * CLONE_PARENT_SETTID names, the child's addresses CLONE_CHILD_SETTID and
 * CLONE_CHILD_CLEARTID name, each 0 for none, the TLS base CLONE_SETTLS
 * gives the child, the parent's address CLONE_PIDFD stores the child's pidfd
 * at, and whether the child's handlers go back to their defaults, clone3's
 * CLONE_CLEAR_SIGHAND. An i386 call's CLONE_SETTLS gives the child a TLS
 * descriptor in place of a base (`ia32_clone_tls`): `tls_desc_set`, with
 * the entry and the descriptor. */
struct fork_req {
    uint32_t flags;
    uintptr_t newsp;
    uintptr_t ptid;
    uintptr_t settid;
    uintptr_t cleartid;
    uintptr_t tls;
    uintptr_t pidfd;
    uint32_t clear_sighand;
    uint32_t tls_desc_set;
    uint32_t tls_entry;
    uint64_t tls_desc;
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
 * The child runs on the parent's memory, its stack included, until it calls
 * execve or ends, so the parent must not run before. A signal other than
 * SIGKILL does not end the wait; SIGKILL does, as in Linux's killable wait,
 * and the parent then dies on its way out.
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
    if (copy && mm && (paging_fork_copy(mm, copy->dir) != 0 || vm_copy(&copy->vm, &mm->vm) != 0)) {
        mm_put(copy);
        return NULL;
    }
    if (copy && mm) {
        mm_copy_info(copy, mm);
        if (arch_mm_copy(copy, mm) != 0) {
            mm_put(copy);
            return NULL;
        }
    }
    return copy;
}

/**
 * Give `child` the directories and umask of `parent`: the same record under
 * CLONE_FS, and otherwise a copy, with holds of its own on the working
 * directory's mount and on the root. 0; -EAGAIN for CLONE_FS while a
 * thread without root that shares the record is in an exec that applies
 * set-id bits (`exec_ids`), as Linux's `copy_fs` fails during such an exec;
 * or -ENOMEM.
 */
static int fork_fs(process_t *child, process_t *parent, uint32_t flags)
{
    if (flags & CLONE_FS) {
        if (parent->fs->in_exec != 0) {
            return -EAGAIN;
        }
        return 0;
    }
    child->fs->umask = parent->fs->umask;
    vfs_cwd_set(child, parent->fs->cwd_mnt, parent->fs->cwd_node);
    vfs_root_copy(child, parent);
    return proc_cwd_copy(child, parent) != 0 ? -ENOMEM : 0;
}

/**
 * RLIMIT_NPROC for a fork or a clone by `parent`: the threads of its real
 * user, this one included, against its limit; root is not limited. Every
 * record counts, as Linux counts every task: each thread of a process, and
 * one that has ended until it is reaped. Counted by the real uid at the
 * time, so a thread that took a new identity with setuid counts for that
 * user. The check follows reclaim, which may sleep, and the new record
 * reserves its count before another building step can sleep. A
 * SCHED_DEADLINE parent without SCHED_RESET_ON_FORK may not fork at all, as
 * Linux's sched_fork refuses it. 0, or -EAGAIN.
 */
static int32_t fork_nproc(const process_t *parent)
{
    if (parent->sched_policy == SCHED_DEADLINE && !parent->sched_reset) {
        return -EAGAIN; /* Linux's sched_fork refuses a deadline thread's child */
    }
    if (parent->ruid == 0 || parent->rlim[RLIM_NPROC].cur == RLIM_INFINITY) {
        return 0;
    }
    return proc_user_records(parent) >= parent->rlim[RLIM_NPROC].cur ? -EAGAIN : 0;
}

/**
 * What a child takes of `parent` besides its memory, files and signals: the
 * session and the process group, which it keeps until it or the parent calls
 * setsid or setpgid, the nice value, the scheduling policy with its
 * parameters, the I/O priority, the OOM score adjustment and
 * its floor, the persona, the no_new_privs flag, whether it
 * gave up its capabilities, the seccomp mode and filters, the limits, the
 * credentials, the memory policy, with a reference that `mm_drop` gives
 * back, and the program name, which `execve` sets and a child that
 * never execs would otherwise never have: a daemon that forks itself into
 * the background is such a child, and without it shows in `ps` and in
 * /proc/<pid>/stat with no name. The dumpable flag comes with the address
 * space (`fork_mm`). A thread shares its process's adjustment, since the
 * group it joins keeps it.
 */
static void fork_ids(process_t *child, const process_t *parent)
{
    child->sid = parent->sid;
    child->pgid = parent->pgid;
    child->nice = parent->nice;
    child->ioprio = parent->ioprio;
    child->sched_policy = parent->sched_policy;
    child->rt_priority = parent->rt_priority;
    child->sched_slice = parent->sched_slice;
    child->dl_runtime = parent->dl_runtime;
    child->dl_deadline = parent->dl_deadline;
    child->dl_period = parent->dl_period;
    child->dl_flags = parent->dl_flags;
    if (parent->sched_reset) {
        /* SCHED_RESET_ON_FORK, as Linux's sched_fork: a real-time or
         * deadline policy becomes SCHED_OTHER at nice 0, a negative nice
         * becomes 0, the slice the default, and the flag is not passed on. */
        if (parent->rt_priority || parent->sched_policy == SCHED_DEADLINE) {
            child->sched_policy = SCHED_OTHER;
            child->nice = 0;
        } else if (child->nice < 0) {
            child->nice = 0;
        }
        child->rt_priority = 0;
        child->sched_slice = 0;
    }
    if (child->tg != parent->tg) {
        child->tg->oom_score_adj = parent->tg->oom_score_adj;
        child->tg->oom_score_adj_min = parent->tg->oom_score_adj_min;
    }
    child->persona = parent->persona;
    child->no_new_privs = parent->no_new_privs;
    child->bset_lost[0] = parent->bset_lost[0];
    child->bset_lost[1] = parent->bset_lost[1];
    k_memcpy(child->cap_inh, parent->cap_inh, sizeof(child->cap_inh));
    k_memcpy(child->cap_amb, parent->cap_amb, sizeof(child->cap_amb));
    child->amb_granted = parent->amb_granted;
    cap_store(child, parent->cap_lost[0], parent->cap_lost[1]);
    seccomp_inherit(child, parent);
    k_memcpy(child->rlim, parent->rlim, sizeof(child->rlim));
    child->euid = parent->euid;
    child->suid = parent->suid;
    child->rgid = parent->rgid;
    child->egid = parent->egid;
    child->sgid = parent->sgid;
    child->fsuid = parent->fsuid;
    child->fsgid = parent->fsgid;
    child->fs_own = parent->fs_own;
    child->ngroups = parent->ngroups;
    k_memcpy(child->groups, parent->groups, sizeof(child->groups));
    k_memcpy(child->comm, parent->comm, sizeof(child->comm));
    child->mempolicy = vm_policy_get(parent->mempolicy);
}

/**
 * The signals of a child of `parent`: under CLONE_THREAD it joins the
 * parent's process, whose dispositions and signals it shares; otherwise its
 * process takes a copy of the dispositions. Either way it has the parent's
 * blocked mask and no pending signal, as POSIX says of fork, and the
 * alternate stack unless it shares the memory and runs beside the parent, as
 * Linux's child does.
 */
static void fork_signals(process_t *child, const process_t *parent, uint32_t flags)
{
    if (flags & CLONE_THREAD) {
        child->tg->live++;
        child->tgid = parent->tgid;
        child->sigacts = child->tg->action;
    } else {
        k_memcpy(child->tg->action, parent->tg->action, sizeof(child->tg->action));
        child->tg->no_child_signal =
            (flags & CLONE_PARENT) ? parent->tg->no_child_signal : (flags & CSIGNAL) == 0;
        child->tg->parent_exec_generation = (flags & CLONE_PARENT)
                                                ? parent->tg->parent_exec_generation
                                                : parent->tg->exec_generation;
    }
    child->sig_blocked = parent->sig_blocked;
    child->sig_pending = 0;
    if ((flags & (CLONE_VM | CLONE_VFORK)) != CLONE_VM) {
        child->altstack_sp = parent->altstack_sp;
        child->altstack_size = parent->altstack_size;
        child->altstack_on = parent->altstack_on;
    }
}

/** CLONE_CLEAR_SIGHAND: every handler of `child`'s process goes back to
 * the default action, and an ignored signal stays ignored, each with no
 * flags and an empty mask, as Linux's `flush_signal_handlers` leaves them. */
static void fork_clear_handlers(process_t *child)
{
    struct k_sigaction *act = child->tg->action;
    int sig;
    int ignored;

    for (sig = 1; sig <= KSIG_MAX; sig++) {
        ignored = act[sig].handler == KSIG_IGN;
        k_memset(&act[sig], 0, sizeof(act[sig]));
        if (ignored) {
            act[sig].handler = KSIG_IGN;
        }
    }
}

/**
 * CLONE_PIDFD: a pidfd of `child` in a descriptor of the caller's, reserved
 * here, with its number stored as an int at `at` in the caller's memory, as
 * Linux stores it before the child runs. The child, made with a copy of the
 * table before, has no such descriptor. Returns the descriptor, with the
 * pidfd in `*pf` for `pidfd_install`; -ENOMEM, -EMFILE, or -EFAULT for a
 * store that failed, with nothing kept.
 */
static int32_t fork_pidfd(process_t *child, uintptr_t at, struct pidfd **pf)
{
    int32_t fd;

    *pf = pidfd_new(child);
    if (!*pf) {
        return -ENOMEM;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd >= 0 && copy_to_user((void __user *)at, &fd, sizeof(fd)) != 0) {
        fd_free(current_proc->fds, fd);
        fd = -EFAULT;
    }
    if (fd < 0) {
        pidfd_free(*pf);
        *pf = NULL;
    }
    return fd;
}

/** Give `child` the descriptor table of `parent`: the table itself under
 * CLONE_FILES, a copy otherwise. 0, or -ENOMEM. */
static int32_t fork_files(process_t *child, process_t *parent, uint32_t flags)
{
    if (flags & CLONE_FILES) {
        if (fd_table_share(parent->fds, child) != 0) {
            return -ENOMEM;
        }
        child->fds = parent->fds;
        return 0;
    }
    child->fds = files_copy(parent->fds);
    return child->fds ? 0 : -ENOMEM;
}

/** Give `child` the directories and the descriptor table of `parent`, each
 * shared or copied as `flags` says (`fork_fs`, `fork_files`); 0, or the
 * error of the first that failed. */
static int32_t fork_shares(process_t *child, process_t *parent, uint32_t flags)
{
    int32_t rc = fork_fs(child, parent, flags);

    return rc != 0 ? rc : fork_files(child, parent, flags);
}

/**
 * Store the child's id where CLONE_CHILD_SETTID and CLONE_PARENT_SETTID say:
 * in the child's address space through its regions, in a writable one only,
 * a private page taking a copy of its own and a shared one written in place,
 * and in the parent's. A failed store leaves the word as it was, as Linux's
 * does. Either store may sleep, bringing a page in.
 */
static void fork_store_ids(process_t *child, const struct fork_req *req)
{
    uint32_t pid = pid_seen_by(child, child->pid);
    const vma_t *r;

    if (req->settid) {
        r = vm_find(&child->mm->vm, req->settid);
        if (r && (r->prot & VM_PROT_WRITE)) {
            (void)paging_access(child->mm, req->settid, &pid, sizeof(pid), 1);
        }
    }
    child->clear_child_tid = req->cleartid;
    if (req->ptid) {
        pid = pid_seen(child->pid);
        (void)copy_to_user((void __user *)req->ptid, &pid, sizeof(pid));
    }
}

/**
 * Lay out `child`'s kernel stack for its first switch, from the parent's
 * trap frame `params`: the architecture copies the frame with 0 for fork's
 * result and the new stack pointer a clone gives, and above it what its
 * `switch_context` pops on the way back to user mode (`arch_fork_frame`).
 * The TLS descriptor an i386 clone's CLONE_SETTLS gave goes in the child's
 * thread-area slots, which its first switch loads.
 */
static void fork_frame(process_t *child, const isr_params_t *params, const struct fork_req *req)
{
    arch_fork_frame(child, params, req->newsp);
#if ELF_IA32
    if (req->tls_desc_set) {
        ia32_clone_tls_set(child, req->tls_entry, req->tls_desc);
    }
#endif
}

/**
 * Create a child process: fork(2), vfork(2), and the part of clone(2) this
 * kernel has.
 *
 * The child's record takes its parts from the parent's (`fork_ids`,
 * `fork_shares`, `fork_signals`), and its address space is a copy-on-write
 * copy, or under CLONE_VM the parent's own. The child is set up to return from
 * fork with value 0 (`fork_frame`); the parent gets the child's PID, after the
 * child has called execve or ended when `req` has CLONE_VFORK. Returns -errno
 * on failure. `req` has the stack pointer the child starts with and the
 * addresses of the ids clone stores: the parent's CLONE_PARENT_SETTID word,
 * stored before the child runs, and the child's CLONE_CHILD_SETTID word and the
 * CLONE_CHILD_CLEARTID address its thread end clears; and the address
 * CLONE_PIDFD stores a pidfd of the child at, made before the child runs
 * (`fork_pidfd`). The child has the parent's TLS base, since its frame keeps
 * the parent's %gs selector and its address space has the same addresses,
 * unless CLONE_SETTLS names another, and the floating-point registers as they
 * are now, since the parent's record has only what they were at its last switch
 * away. The parent gets the child's PID in the parent's PID namespace; the
 * vfork wait names the child by the kernel's.
 *
 * The child is PROC_UNUSED while it is built, neither runnable nor seen by
 * the other calls, since the building may sleep and a switch to it before
 * its frame is laid out would run nothing; it becomes ready at the end. The
 * record starts without a descriptor table, since the child takes a copy
 * of the parent's table or the table itself. The result is -EINTR when a
 * SIGKILL is pending for the caller, which an exit_group or an exec by another
 * thread sends, or its process is ending or in another thread's exec, since
 * a child made then would outlive its process's end; nothing between that
 * test and the child's start sleeps. A failure gives back everything built
 * for the child so far.
 */
static int32_t do_fork(isr_params_t *params, const struct fork_req *req)
{
    process_t *parent = current_proc;
    process_t *child;
    struct pidfd *pf = NULL;
    int32_t pidfd = -1;
    uint32_t pid, kid;
    uintptr_t child_kstack_base;
    int32_t rc;
    int event;

    if (!parent) {
        return -ESRCH;
    }
    mem_balance();
    rc = fork_nproc(parent);
    if (rc != 0) {
        return rc;
    }
    child =
        alloc_process_fork(parent, (req->flags & CLONE_THREAD) != 0, (req->flags & CLONE_FS) != 0);
    if (!child) {
        return -ENOMEM;
    }

    child->mm = fork_mm(parent->mm, req->flags);
    if (!child->mm) {
        free_process(child);
        return -ENOMEM;
    }
    child->space_root = child->mm->space_root;
    child->page_dir = child->mm->dir;

    child_kstack_base = kstack_alloc();
    if (!child_kstack_base) {
        mm_drop(child);
        free_process(child);
        return -ENOMEM;
    }
    child->kernel_stack = child_kstack_base + KERNEL_STACK_SIZE;
    /* A thread's or a CLONE_PARENT child's parent is the caller's parent. */
    child->parent_pid =
        (req->flags & (CLONE_THREAD | CLONE_PARENT)) ? parent->parent_pid : parent->tgid;
    fork_ids(child, parent);
    rseq_fork(child, parent, (req->flags & CLONE_VM) != 0);
    rc = ns_fork(child, parent, req->flags);
    if (rc != 0) {
        goto fail_built;
    }
    child->tls_base = (req->flags & CLONE_SETTLS) && !req->tls_desc_set ? req->tls : arch_tls_get();
    fpu_save(child->fpu);

    rc = fork_shares(child, parent, req->flags);
    if (rc != 0) {
        goto fail_built;
    }
    fork_store_ids(child, req);
    if (req->flags & CLONE_PIDFD) {
        pidfd = fork_pidfd(child, req->pidfd, &pf);
        if (pidfd < 0) {
            rc = pidfd;
            goto fail_built;
        }
    }
    if (((parent->sig_pending | parent->tg->pending) & KSIG_BIT(SIGKILL)) || parent->tg->exiting ||
        parent->tg->exec_task) {
        rc = -EINTR;
        goto fail_pidfd;
    }
    if (pf) {
        pidfd_install(pf, pidfd, 0);
    }

    fork_signals(child, parent, req->flags);
    if (req->clear_sighand) {
        fork_clear_handlers(child);
    }
    fork_frame(child, params, req);
    kid = child->pid;
    pid = pid_seen(kid);
    if (req->flags & CLONE_VFORK) {
        child->vfork_waiting = 1;
    }
    event = ptrace_fork_attach(child, req->flags);
    child->cpu = parent->cpu;
    child->affinity = parent->affinity;
    child->bkl_depth = 1;
    sched_make_ready(child);
    if (event) {
        ptrace_event(event, pid);
    }
    if (req->flags & CLONE_VFORK) {
        proc_vfork_wait(kid);
        ptrace_event(PTRACE_EVENT_VFORK_DONE, pid);
    }
    return (int32_t)pid;

fail_pidfd:
    if (pf) {
        fd_free(parent->fds, pidfd);
        pidfd_free(pf);
    }
fail_built:
    proc_files_put(child);
    proc_fs_put(child);
    kstack_free(child->kernel_stack);
    mm_drop(child);
    free_process(child);
    return rc;
}

/** fork: make a child with a copy-on-write copy of the caller's memory,
 * whose end sends the caller SIGCHLD (`do_fork`). */
long sys_fork(isr_params_t *params)
{
    struct fork_req req = {SIGCHLD, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    return do_fork(params, &req);
}

/**
 * vfork: a child that runs on the parent's memory while the parent
 * waits, until the child calls execve or ends. musl's vfork and glibc's call
 * it; the child must do nothing else before one of the two, since every
 * store it makes is the parent's.
 */
long sys_vfork(isr_params_t *params)
{
    struct fork_req req = {CLONE_VM | CLONE_VFORK | SIGCHLD, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    return do_fork(params, &req);
}

/**
 * Put the thread pointer that CLONE_SETTLS in `flags` gives, from the TLS
 * argument `tls`, in `req`: a base (`arch_clone_tls`), or in an i386 call on
 * x86-64 the descriptor of the `struct user_desc` at `tls`
 * (`ia32_clone_tls`), as Linux's `set_new_tls` has it. Without the flag
 * `req` gets none. 0 or the error of either.
 */
static long clone_tls(uint32_t flags, unsigned long tls, struct fork_req *req)
{
    uintptr_t base = 0;
    long err = 0;

    req->tls = 0;
    req->tls_desc_set = 0;
    req->tls_entry = 0;
    req->tls_desc = 0;
    if (!(flags & CLONE_SETTLS)) {
        return 0;
    }
#if ELF_IA32
    if (in_compat_syscall()) {
        err = ia32_clone_tls(tls, &req->tls_entry, &req->tls_desc);
        req->tls_desc_set = err == 0;
        return err;
    }
#endif
    err = arch_clone_tls(tls, &base);
    req->tls = base;
    return err;
}

/** Make the clone `sys_clone` describes, from the arguments of `clone` or of
 * `clone3`; `params` is the caller's frame, which the child's is copied from,
 * `pidfd` the address CLONE_PIDFD stores at, and `clear_sighand` clone3's
 * CLONE_CLEAR_SIGHAND. In an i386 call on x86-64 the TLS argument of
 * CLONE_SETTLS is a `struct user_desc`, whose descriptor the child gets in
 * place of a thread pointer, as Linux's `set_new_tls` has it. The child's
 * id, -EINVAL for a set of flags `sys_clone` does not take, or the error of
 * `arch_clone_tls`, `ia32_clone_tls` or `do_fork`. */
static int32_t clone_with(isr_params_t *params, uint32_t flags, uintptr_t newsp, uintptr_t ptid,
                          unsigned long tls, uintptr_t ctid, uintptr_t pidfd, int clear_sighand)
{
    struct fork_req req;
    long err;

    if ((flags & ~(CSIGNAL | CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_PIDFD |
                   CLONE_VFORK | CLONE_PARENT | CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |
                   CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID |
                   CLONE_DETACHED | CLONE_UNTRACED | CLONE_NEW_ANY)) != 0 ||
        (!(flags & CLONE_THREAD) && (flags & CSIGNAL) != 0 && (flags & CSIGNAL) != SIGCHLD) ||
        ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM)) ||
        ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND)) ||
        ((flags & CLONE_SIGHAND) && !(flags & CLONE_THREAD)) ||
        ((flags & CLONE_PIDFD) && (flags & (CLONE_THREAD | CLONE_DETACHED))) ||
        ((flags & CLONE_PIDFD) && (flags & CLONE_PARENT_SETTID) && pidfd == ptid) ||
        (clear_sighand && (flags & CLONE_SIGHAND))) {
        return -EINVAL;
    }
    err = clone_tls(flags, tls, &req);
    if (err != 0) {
        return (int32_t)err;
    }
    req.flags = flags;
    req.newsp = newsp;
    req.ptid = (flags & CLONE_PARENT_SETTID) ? ptid : 0;
    req.settid = (flags & CLONE_CHILD_SETTID) ? ctid : 0;
    req.cleartid = (flags & CLONE_CHILD_CLEARTID) ? ctid : 0;
    req.pidfd = pidfd;
    req.clear_sighand = clear_sighand != 0;
    return do_fork(params, &req);
}

/**
 * clone, in the forms glibc's and musl's fork, posix_spawn and
 * pthread_create use: a new process with its own copy of the address space,
 * files and signal handlers, as fork makes one; with the caller's address
 * space itself under CLONE_VM, and with the caller waiting as vfork waits
 * under CLONE_VFORK; or a new thread of the caller's process under
 * CLONE_THREAD, which needs CLONE_SIGHAND, which in turn needs CLONE_VM.
 * CLONE_FILES shares the caller's descriptor table in place of a copy, and
 * CLONE_FS its working directory, root directory and umask.
 *
 * The arguments are the flags, the new stack or 0 and the parent tid
 * address, then the TLS argument and the child tid address in the order of
 * the architecture (SC_CLONE_TLS and SC_CLONE_CTID): i386 has the TLS
 * argument first, x86-64 the child tid. The exit signal must be SIGCHLD for
 * a process; a thread sends none, whatever the low byte says. CLONE_SETTLS
 * gives the child the thread pointer its TLS argument gives
 * (`arch_clone_tls`). CLONE_PARENT makes the child a child of the
 * caller's parent. CLONE_PARENT_SETTID stores the child's id in the
 * parent's memory before the child runs, and a failed store is ignored, as
 * Linux ignores it. CLONE_SYSVSEM and CLONE_DETACHED are ignored: a
 * process's semaphore adjustments are its own, which threads share, and
 * Linux ignores the second. CLONE_PIDFD stores a pidfd of the child, as
 * an int, at the parent tid address, so it takes neither CLONE_PARENT_SETTID
 * nor CLONE_THREAD nor CLONE_DETACHED. The CLONE_NEW flags give the child
 * new namespaces (`ns_fork`); a cgroup namespace and any other flag are
 * EINVAL.
 */
long sys_clone(isr_params_t *params)
{
    return clone_with(params, (uint32_t)SC_ARG(params, 0), SC_ARG(params, 1), SC_ARG(params, 2),
                      SC_ARG(params, SC_CLONE_TLS), SC_ARG(params, SC_CLONE_CTID),
                      SC_ARG(params, 2), 0);
}

/** clone(2) from arguments its caller has read in its own order, the i386
 * call's (`compat_sys_ia32_clone`): the flags, the new stack, the parent
 * tid address, the TLS argument and the child tid address. */
long ksys_clone(isr_params_t *params, uint32_t flags, uintptr_t newsp, uintptr_t ptid,
                unsigned long tls, uintptr_t ctid)
{
    return clone_with(params, flags, newsp, ptid, tls, ctid, ptid, 0);
}

/* clone3's structure, Linux's struct clone_args as the eight 64-bit fields
 * of its first version, on every architecture. */
#define CA_FLAGS          0
#define CA_PIDFD          1
#define CA_CHILD_TID      2
#define CA_PARENT_TID     3
#define CA_EXIT_SIGNAL    4
#define CA_STACK          5
#define CA_STACK_SIZE     6
#define CA_TLS            7
#define CLONE_ARGS_FIELDS 8

/* How much of a larger structure Linux defines, through set_tid, set_tid_size
 * and cgroup; past it the bytes must be zero, and at most a page is read. */
#define CLONE_ARGS_KNOWN 88

/**
 * clone3: clone with its arguments in a structure, which glibc tries
 * before clone for pthread_create, fork and posix_spawn. The flags are
 * clone's, with the exit signal in a field of its own, and the stack is the
 * lowest address of the region with its size, so the child starts at their
 * sum. A structure shorter than the 64 bytes of Linux's first version is
 * EINVAL; a longer one is read when the fields this kernel does not take,
 * set_tid, set_tid_size and cgroup, are zero, and its bytes past those
 * fields are zero, else E2BIG, as Linux reads it. CLONE_PIDFD stores the
 * child's pidfd at the address of its own field, and CLONE_CLEAR_SIGHAND,
 * which glibc's posix_spawn passes with CLONE_VM and CLONE_VFORK, gives the
 * child's process the default handlers in place of the caller's
 * (`fork_clear_handlers`); with CLONE_SIGHAND it is EINVAL. Any other flag
 * past bit 31, CLONE_INTO_CGROUP among them, a signal in the flags, which
 * has a field of its own, CLONE_DETACHED and a field other than the flags
 * wider than an address are EINVAL. The caller's registers are left as
 * they were, since a caller may keep the structure's address in one across
 * the call.
 */
long sys_clone3(isr_params_t *params)
{
    uint64_t a[CLONE_ARGS_FIELDS];
    uint32_t extra[(CLONE_ARGS_KNOWN - 64) / 4];
    uintptr_t uargs = SC_ARG(params, 0);
    uint32_t size = USER_LEN32(SC_ARG(params, 1)), at, byte, i;

    if (size < 64u) {
        return -EINVAL;
    }
    if (size > 4096u) {
        return -E2BIG;
    }
    if (copy_from_user(a, (const void __user *)uargs, sizeof(a)) != 0) {
        return -EFAULT;
    }
    for (at = 64u; at < size; at += 4u) {
        uint32_t n = size - at < 4u ? size - at : 4u;

        byte = 0;
        if (copy_from_user(&byte, (const void __user *)(uargs + at), n) != 0) {
            return -EFAULT;
        }
        if (at < CLONE_ARGS_KNOWN) {
            extra[(at - 64u) / 4u] = byte;
        } else if (byte != 0) {
            return -E2BIG;
        }
    }
    for (i = 0; i < (CLONE_ARGS_KNOWN - 64u) / 4u && 64u + 4u * i < size; i++) {
        if (extra[i] != 0) {
            return -EINVAL;
        }
    }
    for (i = CA_FLAGS + 1; i < CLONE_ARGS_FIELDS; i++) {
        if (a[i] != (uintptr_t)a[i]) {
            return -EINVAL;
        }
    }
    if ((a[CA_FLAGS] & ~(0xFFFFFFFFull | CLONE_CLEAR_SIGHAND)) != 0 ||
        (a[CA_FLAGS] & (CSIGNAL | CLONE_DETACHED)) != 0 || a[CA_EXIT_SIGNAL] > 64u ||
        (a[CA_STACK] == 0) != (a[CA_STACK_SIZE] == 0)) {
        return -EINVAL;
    }
    return clone_with(params, (uint32_t)(a[CA_FLAGS] | a[CA_EXIT_SIGNAL]),
                      a[CA_STACK] ? (uintptr_t)(a[CA_STACK] + a[CA_STACK_SIZE]) : 0,
                      (uintptr_t)a[CA_PARENT_TID], (unsigned long)a[CA_TLS],
                      (uintptr_t)a[CA_CHILD_TID], (uintptr_t)a[CA_PIDFD],
                      (a[CA_FLAGS] & CLONE_CLEAR_SIGHAND) != 0);
}

/* The options of waitpid, wait4 and waitid. */
#define WNOHANG 1

#define WUNTRACED 2 /* also report a child that has stopped; waitid's WSTOPPED */

#define WEXITED 4 /* report a child that has exited, which wait4 always does */

#define WCONTINUED 8 /* also report a child that has continued */

#define WNOWAIT 0x01000000 /* waitid: report the event and leave it */

#define W_NOTHREAD 0x20000000 /* Linux's __WNOTHREAD */

#define W_ALL 0x40000000 /* Linux's __WALL */

#define W_CLONE 0x80000000u /* Linux's __WCLONE */

/* The children a wait selects, as waitid's idtype names them: every child,
 * the child with an id, the children in a process group, or the child a
 * pidfd names. */
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

/* Linux's struct rusage, eighteen longs: the user and system CPU time as
 * two timevals of seconds and microseconds, maxrss in kilobytes, ixrss,
 * idrss and isrss, the minor and major page faults, and then nswap through
 * nivcsw. The fields this kernel does not count are zero. 72 bytes on
 * i686. */
struct kernel_rusage {
    long utime_sec;
    long utime_usec;
    long stime_sec;
    long stime_usec;
    long maxrss;
    long sizes[3];
    long minflt;
    long majflt;
    long uncounted[6];
    long nvcsw;
    long nivcsw;
};

/** The bytes of the caller's struct rusage: 72, eighteen 32-bit longs, for
 * an i386 caller on x86-64, as Linux's compat_rusage, else the native
 * structure's. */
static uint32_t rusage_bytes(void)
{
#if defined(IA32_SYSCALL_TABLE_SIZE)
    if (in_compat_syscall()) {
        return 18u * 4u;
    }
#endif
    return (uint32_t)sizeof(struct kernel_rusage);
}

/** Copy `ru` to the caller's struct rusage at `u`, each long truncated to
 * 32 bits for an i386 caller on x86-64, as Linux's put_compat_rusage does;
 * 0, or -EFAULT. */
static int rusage_put(uintptr_t u, const struct kernel_rusage *ru)
{
#if defined(IA32_SYSCALL_TABLE_SIZE)
    if (in_compat_syscall()) {
        const long *w = (const long *)ru;
        int32_t ru32[18];
        unsigned i;

        for (i = 0; i < 18u; i++) {
            ru32[i] = (int32_t)w[i];
        }
        return copy_to_user((void __user *)u, ru32, sizeof(ru32));
    }
#endif
    return copy_to_user((void __user *)u, ru, sizeof(*ru));
}

/** Fill `ru` with `user` and `sys` ticks of CPU time and zero the rest. */
static void rusage_from_ticks(struct kernel_rusage *ru, uint64_t user, uint64_t sys)
{
    uint32_t rest;

    k_memset(ru, 0, sizeof(*ru));
    ru->utime_sec = (long)tick_div(user, TICK_HZ, &rest);
    ru->utime_usec = (long)(rest * TICK_US);
    ru->stime_sec = (long)tick_div(sys, TICK_HZ, &rest);
    ru->stime_usec = (long)(rest * TICK_US);
}

/**
 * What a wait reports of a child's usage, as Linux's `RUSAGE_BOTH` has it:
 * the CPU time, page faults and switches of the process `p` is a thread of:
 * every thread's, with those of its reaped children, which its thread group
 * keeps. A traced thread that is not the leader reports its whole
 * process's.
 */
static void rusage_of_child(struct kernel_rusage *ru, const process_t *p)
{
    uint64_t user;
    uint64_t sys;
    unsigned long minor;
    unsigned long major;
    unsigned long voluntary;
    unsigned long involuntary;

    proc_cpu_time(p, &user, &sys);
    rusage_from_ticks(ru, user + p->tg->cutime, sys + p->tg->cstime);
    ru->maxrss =
        (long)(p->tg->maxrss_kb > p->tg->cmaxrss_kb ? p->tg->maxrss_kb : p->tg->cmaxrss_kb);
    proc_faults(p, &minor, &major);
    ru->minflt = (long)(minor + p->tg->cmin_flt);
    ru->majflt = (long)(major + p->tg->cmaj_flt);
    proc_switches(p, &voluntary, &involuntary);
    ru->nvcsw = (long)(voluntary + p->tg->cnvcsw);
    ru->nivcsw = (long)(involuntary + p->tg->cnivcsw);
}

/* What a wait found: the child, its real uid, the wait status wait4 stores,
 * the si_code and si_status waitid stores, and the child's usage; and the
 * event the wait holds for the caller while the caller copies these out, the
 * record and what taking it does (`wait_hold`), NULL and WAIT_KEEP for
 * none. */
struct wait_report {
    uint32_t pid;
    uint32_t uid;
    int32_t status;
    int32_t code;
    int32_t info_status;
    struct kernel_rusage ru;
    process_t *p;
    int take;
};

/* What taking a held event does (`wait_done`): nothing, since none is held;
 * reap the zombie process; free the traced thread that ended; take the end
 * of the process a tracer traces; take the stop or continue report; take the
 * ptrace stop report. */
#define WAIT_KEEP 0

#define WAIT_REAP 1

#define WAIT_FREE 2

#define WAIT_SEEN 3

#define WAIT_JOB 4

#define WAIT_TRAP 5

/* Interrupts-off helpers (defined below with the signal code); used here for
 * the lost-wakeup-safe sleep in wait_find. */
static uint32_t sig_irq_save(void);

static void sig_irq_restore(uint32_t flags);

/**
 * Whether `p` is a child of the caller's process that `idtype` and `id`
 * select: the leader of a process whose parent is any thread of the
 * caller's, since the children of a process are all its threads'. A record
 * another reaper has taken, or that fork is still building, is no child
 * (`PROC_UNUSED`).
 *
 * Linux's __WCLONE selects the children that report their end with a signal
 * other than SIGCHLD, including zero, unless __WALL is beside it.
 */
static int wait_selects(int idtype, uint32_t id, int options, const process_t *p)
{
    if (p->state == PROC_UNUSED || p->parent_pid != current_proc->tgid || p->pid != p->tgid) {
        return 0;
    }
    if (!(options & W_ALL) && p->tg->no_child_signal != (((uint32_t)options & W_CLONE) != 0)) {
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
 * Reap zombie `p`, the leader of a process whose threads have all ended,
 * which the wait has taken off the zombies (`wait_hold`), so that no other
 * thread's wait reaps it while the release sleeps: add the process's CPU
 * time, page faults and switches, with its reaped children's, to the caller's
 * children's totals, which its thread group keeps, and give back its
 * address space, kernel stack and record (`proc_release`). The zombie no
 * longer runs on its kernel stack, so the stack can go with the record.
 */
static void wait_reap(process_t *p)
{
    struct tgroup *me = current_proc->tg;
    uint64_t user;
    uint64_t sys;
    unsigned long minor;
    unsigned long major;
    unsigned long voluntary;
    unsigned long involuntary;

    proc_cpu_time(p, &user, &sys);
    me->cutime += user + p->tg->cutime;
    me->cstime += sys + p->tg->cstime;
    proc_faults(p, &minor, &major);
    me->cmin_flt += minor + p->tg->cmin_flt;
    me->cmaj_flt += major + p->tg->cmaj_flt;
    proc_switches(p, &voluntary, &involuntary);
    me->cnvcsw += voluntary + p->tg->cnvcsw;
    me->cnivcsw += involuntary + p->tg->cnivcsw;
    if (p->tg->maxrss_kb > me->cmaxrss_kb) {
        me->cmaxrss_kb = p->tg->maxrss_kb;
    }
    if (p->tg->cmaxrss_kb > me->cmaxrss_kb) {
        me->cmaxrss_kb = p->tg->cmaxrss_kb;
    }
    proc_release(p);
}

/**
 * Whether `p` is a thread the caller's process traces that `idtype` and `id`
 * select; a record another reaper has taken is none (`PROC_UNUSED`). A
 * tracer's wait includes every tracee as if __WALL were set, independently
 * of its exit notification signal and whether it is a group leader.
 */
static int wait_selects_tracee(int idtype, uint32_t id, int options, const process_t *p)
{
    (void)options;

    if (p->state == PROC_UNUSED || !(p->ptrace & PT_TRACED) || p->tracer != current_proc->tgid) {
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
 * Hold the event of `p` that `r` reports for the caller while it copies the
 * report out, so that no other thread's wait takes the event meanwhile: take
 * the record off the zombies for a reap or a free, or mark the tracer's view
 * of the process's end, the stop or continue report, or the ptrace stop
 * report taken, as `take` says. `wait_done` then finishes the event or gives
 * it back. WNOWAIT holds nothing, since it leaves every event where it is.
 */
static void wait_hold(struct wait_report *r, process_t *p, int take, int options)
{
    r->p = NULL;
    r->take = WAIT_KEEP;
    if (options & WNOWAIT) {
        return;
    }
    switch (take) {
    case WAIT_REAP:
    case WAIT_FREE:
        p->state = PROC_UNUSED;
        break;
    case WAIT_SEEN:
        p->ptrace_exit_seen = 1;
        break;
    case WAIT_JOB:
        p->jc_status = 0;
        break;
    default:
        p->ptrace_reported = 1;
        break;
    }
    r->p = p;
    r->take = take;
}

/**
 * Take the event the wait holds in `r`, whose report the caller has copied
 * out: reap the zombie process or free the traced thread that ended, which
 * no other wait could take meanwhile, or let go the process whose end its
 * tracer has now seen, which its parent may then reap (`ptrace_exit_release`),
 * when no other part of the kernel freed it meanwhile. The other events were
 * taken when they were held.
 */
static void wait_take(const struct wait_report *r)
{
    process_t *p = r->p;

    if (r->take == WAIT_REAP) {
        wait_reap(p);
    } else if (r->take == WAIT_FREE) {
        proc_thread_fold(p);
        proc_release(p);
    } else if (r->take == WAIT_SEEN && get_process(r->pid) == p) {
        ptrace_exit_release(p);
    }
}

/**
 * Finish the event the wait holds in `r` (`wait_hold`): take it when the
 * caller copied the report out, `ok` (`wait_take`), and otherwise give it
 * back.
 *
 * A copy fails when another thread unmapped or protected the caller's buffer
 * while the wait slept. The event is then given back as if it had not been
 * reported, and the caller's other waiting threads, which passed it over,
 * are woken. A report of a record that may have ended meanwhile, a stop, a
 * continue, a ptrace stop or the end a tracer sees, is given back only when
 * the record is still there and has no newer report.
 */
static void wait_done(struct wait_report *r, int ok)
{
    process_t *p = r->p;
    int there;

    if (p == NULL || ok) {
        if (p != NULL) {
            wait_take(r);
        }
        return;
    }
    there = get_process(r->pid) == p;
    if (r->take == WAIT_REAP || r->take == WAIT_FREE) {
        p->state = PROC_ZOMBIE;
    } else if (r->take == WAIT_SEEN && there) {
        p->ptrace_exit_seen = 0;
    } else if (r->take == WAIT_JOB && there && p->jc_status == 0) {
        p->jc_status = (uint32_t)r->status;
    } else if (r->take == WAIT_TRAP && there && p->ptrace_status == (uint32_t)r->status) {
        p->ptrace_reported = 0;
    }
    wq_wake_all(&current_proc->tg->chld);
}

/**
 * What a tracer's wait finds of its tracee `p`: a ptrace stop the wait has
 * not reported, reported once; a thread that ended while traced, which the
 * report frees, as no other wait reports it; or the end of a process whose
 * parent the tracer is not, which the parent's wait passes over until this
 * report lets the process go to the parent (`wait_take`). A stop's si_status
 * is its wait status shifted right by 8 bits, the signal with the event
 * above it, as Linux's `wait_task_stopped` gives it, so waitid tells an
 * event stop from a plain SIGTRAP. Returns 1 with `out` filled and the event
 * held (`wait_hold`), or 0.
 */
static int wait_tracee(process_t *p, int options, struct wait_report *out)
{
    if (p->ptrace_status != 0 && !p->ptrace_reported && p->state != PROC_ZOMBIE) {
        wait_fill(out, p, (int32_t)p->ptrace_status, CLD_TRAPPED, (int32_t)(p->ptrace_status >> 8));
        wait_hold(out, p, WAIT_TRAP, options);
        return 1;
    }
    if (p->state != PROC_ZOMBIE || !(options & WEXITED)) {
        return 0;
    }
    if (p->pid != p->tgid) {
        wait_fill_exit(out, p, p->exit_code);
        wait_hold(out, p, WAIT_FREE, options);
        return 1;
    }
    if (p->tg->live == 0 && p->parent_pid != current_proc->tgid && !p->ptrace_exit_seen) {
        wait_fill_exit(out, p, p->exit_code);
        wait_hold(out, p, WAIT_SEEN, options);
        return 1;
    }
    return 0;
}

/**
 * What a wait finds of its child `p`: its end (WEXITED), once every thread
 * has ended, which the report reaps; a stop (WUNTRACED), reported once while
 * the child is stopped and has not ended or begun to end, since the stop a
 * SIGKILL ended stays recorded; or a continue (WCONTINUED), reported once.
 * A process another process traces is reaped only after its tracer's wait
 * has reported its end and let it go (`wait_tracee`), as Linux has it, so
 * that the tracer learns of the end first. Returns 1 with `out` filled and
 * the event held (`wait_hold`), or 0.
 */
static int wait_child(process_t *p, int options, struct wait_report *out)
{
    int traced = (p->ptrace & PT_TRACED) && p->tracer != current_proc->tgid;
    int stopped = p->tg->stop != 0 && p->tg->live != 0 && !p->tg->exiting;

    if (p->state == PROC_ZOMBIE && p->tg->live == 0 && (options & WEXITED) && !traced) {
        wait_fill_exit(out, p, p->exit_code);
        wait_hold(out, p, WAIT_REAP, options);
        return 1;
    }
    if ((options & WUNTRACED) && stopped && (p->jc_status & 0xff) == 0x7f) {
        wait_fill(out, p, (int32_t)p->jc_status, CLD_STOPPED,
                  (int32_t)((p->jc_status >> 8) & 0xff));
    } else if ((options & WCONTINUED) && p->jc_status == 0xffff) {
        wait_fill(out, p, 0xffff, CLD_CONTINUED, SIGCONT);
    } else {
        return 0;
    }
    wait_hold(out, p, WAIT_JOB, options);
    return 1;
}

/**
 * The shared core of waitpid, wait4 and waitid: find a child `idtype` and
 * `id` select that has one of the events `options` lists, fill `out` with it
 * and return its pid, holding the event for the caller until it has copied
 * the report out and calls `wait_done`. The threads the caller's process
 * traces are waited for too, as `wait_tracee` reports them.
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
 * Every selected child is looked at until one has an event. Without WNOHANG
 * the caller then sleeps on its process's child queue, which an exit, a stop
 * or a continue wakes, until a child has an event or a signal interrupts. The
 * test of `signal_pending`, the joining of the queue and the change to
 * PROC_BLOCKED are one step with interrupts off, so a wake or a signal posted
 * from an interrupt is either seen by the test or finds the caller blocked
 * and wakes it.
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

        for (p = proc_list; p; p = p->next) {
            if (wait_selects_tracee(idtype, id, options, p)) {
                have_child = 1;
                if (wait_tracee(p, options, out)) {
                    return (int32_t)out->pid;
                }
            }
            if (wait_selects(idtype, id, options, p)) {
                have_child = 1;
                if (wait_child(p, options, out)) {
                    return (int32_t)out->pid;
                }
            }
        }

        if (!have_child) {
            return -ECHILD;
        }
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
 * Shared core of waitpid and wait4.
 *
 * `pid` selects as Linux's does: a pid, -1 for every child, 0 for the
 * caller's process group, and less than -1 for the group -pid; the most
 * negative int names no group. The options are WNOHANG, WUNTRACED and
 * WCONTINUED, with Linux's three thread bits, and an exit is always waited
 * for; the other bits are ignored. `status` (may be NULL) receives the
 * Linux-encoded wait status that sys_exit/terminate already stored in
 * exit_code; `rusage_ptr` (0 for waitpid, may be 0 for wait4) receives the
 * child's CPU time with its reaped children's, for a stop or a continue as
 * for an exit. Both are faulted in for writing before the wait, so a bad
 * address answers -EFAULT at once. A copy that fails after the wait, since
 * another thread unmapped or protected a buffer while the wait slept,
 * answers -EFAULT too, and leaves the child's event for a later wait
 * (`wait_done`): no zombie is lost. The pid returned is the child's in the
 * caller's PID namespace, found before `wait_done` frees a reaped child.
 */
static int32_t do_wait(int pid, int __user *status, int options, uintptr_t rusage_ptr)
{
    struct wait_report r;
    int idtype = P_PGID;
    uint32_t id, seen;
    int32_t rc;
    int ok;

    if (!current_proc) {
        return -ESRCH;
    }
    if (status && !user_buffer_ok((__force uintptr_t)status, sizeof(int), 1)) {
        return -EFAULT;
    }
    if (rusage_ptr && !user_buffer_ok(rusage_ptr, rusage_bytes(), 1)) {
        return -EFAULT;
    }
    if (pid > 0) {
        idtype = P_PID;
        id = pid_named((uint32_t)pid);
    } else if (pid == -1) {
        idtype = P_ALL;
        id = 0;
    } else if (pid == 0) {
        id = current_proc->pgid;
    } else {
        id = pid_named(0u - (uint32_t)pid);
    }
    options &= (int)(WNOHANG | WUNTRACED | WCONTINUED | W_NOTHREAD | W_ALL | W_CLONE);
    rc = wait_find(idtype, id, options | WEXITED, &r);
    if (rc <= 0) {
        return rc;
    }
    seen = pid_seen((uint32_t)rc);
    ok = (!status || copy_to_user((void __user *)status, &r.status, sizeof(r.status)) == 0) &&
         (!rusage_ptr || rusage_put(rusage_ptr, &r.ru) == 0);
    wait_done(&r, ok);
    return ok ? (int32_t)seen : -EFAULT;
}

/**
 * waitpid: wait for a child to change state (`do_wait`). Its arguments are
 * pid (-1 for any child), the status pointer and the options. Returns the
 * PID of the child reaped, or reported stopped or continued, 0 under WNOHANG
 * when none is ready, or -errno.
 */
long sys_waitpid(isr_params_t *params)
{
    return do_wait((int)SC_ARG(params, 0), (int __user *)SC_ARG(params, 1), (int)SC_ARG(params, 2),
                   0);
}

/**
 * wait4: waitpid with an optional rusage pointer, which receives the
 * child's CPU time. Its arguments are pid, status, options and rusage.
 * musl's wait and waitpid make this call.
 */
long sys_wait4(isr_params_t *params)
{
    return do_wait((int)SC_ARG(params, 0), (int __user *)SC_ARG(params, 1), (int)SC_ARG(params, 2),
                   SC_ARG(params, 3));
}

/**
 * Wait for a child to change state (waitid).
 *
 * Its arguments are idtype, id, siginfo_t *, options and struct rusage *,
 * Linux's fifth argument. P_ALL selects every child, P_PID the child with a
 * positive id, P_PGID the group with a nonnegative id, 0 being the
 * caller's, and P_PIDFD the child the pidfd `id` names, which a non-blocking
 * pidfd waits for as WNOHANG does, with -EAGAIN for no event, as on Linux.
 * At least one of WEXITED, WSTOPPED and WCONTINUED must be given,
 * beside which WNOHANG, WNOWAIT and Linux's three thread bits are accepted;
 * `wait_find` has what each means.
 *
 * Returns 0 when a child was reported, with the siginfo's si_signo SIGCHLD,
 * si_code, si_pid, si_uid (the child's real uid) and si_status filled and
 * the child's usage in the rusage; and 0 with WNOHANG when no selected child
 * has an event, with those siginfo fields zero and the rusage untouched.
 * Only the bytes through those fields are written, as the caller's
 * siginfo_t lays them out (`siginfo_to_caller`). -EINVAL for an unknown
 * option or idtype, no event asked for, or an id that does not suit its
 * idtype; -EBADF for a
 * descriptor that is not a pidfd; -ECHILD, which a pidfd of a process
 * already reaped gives, -EINTR, -EAGAIN; -EFAULT, found before
 * the wait or after it with the child's event left in place, as `do_wait`
 * finds it.
 */
/** The bytes of a siginfo_t through si_status, which waitid writes: 24 for
 * an i386 caller on x86-64, as Linux's compat waitid writes, and
 * SIGINFO_CHLD_BYTES for a native one. */
static uint32_t siginfo_chld_bytes(void)
{
#if defined(IA32_SYSCALL_TABLE_SIZE)
    if (in_compat_syscall()) {
        return SIGINFO32_CHLD_BYTES;
    }
#endif
    return SIGINFO_CHLD_BYTES;
}

long sys_waitid(isr_params_t *params)
{
    int idtype = (int)SC_ARG(params, 0);
    int32_t upid = (int32_t)SC_ARG(params, 1);
    uintptr_t uinfo = SC_ARG(params, 2);
    int options = (int)SC_ARG(params, 3);
    uintptr_t rusage_ptr = SC_ARG(params, 4);
    uint8_t info[SIGINFO_SIZE];
    struct k_siginfo ki;
    struct wait_report r;
    process_t *named;
    uint32_t id = 0;
    int nonblock = 0;
    int32_t rc;
    int ok;

    if ((uint32_t)options &
        ~(WNOHANG | WNOWAIT | WEXITED | WUNTRACED | WCONTINUED | W_NOTHREAD | W_ALL | W_CLONE)) {
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
        id = pid_named((uint32_t)upid);
        break;
    case P_PGID:
        if (upid < 0) {
            return -EINVAL;
        }
        id = upid != 0 ? pid_named((uint32_t)upid) : current_proc->pgid;
        break;
    case P_PIDFD:
        if (upid < 0) {
            return -EINVAL;
        }
        rc = pidfd_lookup((int)upid, &named, &nonblock);
        if (rc != 0) {
            return rc;
        }
        if (!named) {
            return -ECHILD;
        }
        idtype = P_PID;
        id = named->pid;
        if (nonblock) {
            options |= WNOHANG;
        }
        break;
    default:
        return -EINVAL;
    }
    if (uinfo && !user_buffer_ok(uinfo, siginfo_chld_bytes(), 1)) {
        return -EFAULT;
    }
    if (rusage_ptr && !user_buffer_ok(rusage_ptr, rusage_bytes(), 1)) {
        return -EFAULT;
    }
    rc = wait_find(idtype, id, options, &r);
    if (rc < 0) {
        return rc;
    }
    if (rc == 0 && nonblock) {
        return -EAGAIN;
    }
    k_memset(info, 0, sizeof(info));
    if (rc > 0) {
        k_memset(&ki, 0, sizeof(ki));
        ki.code = r.code;
        ki.pid = pid_seen(r.pid);
        ki.uid = uid_seen(r.uid);
        ki.status = r.info_status;
        siginfo_to_caller(info, SIGCHLD, &ki);
    }
    ok = (rc == 0 || !rusage_ptr || rusage_put(rusage_ptr, &r.ru) == 0) &&
         (!uinfo || copy_to_user((void __user *)uinfo, info, siginfo_chld_bytes()) == 0);
    if (rc > 0) {
        wait_done(&r, ok);
    }
    return ok ? 0 : -EFAULT;
}

/* The targets getrusage(2) takes: the caller's process, its reaped
 * children, and the calling thread. */
#define RUSAGE_SELF 0

#define RUSAGE_CHILDREN (-1)

#define RUSAGE_THREAD 1

/**
 * Resource usage (getrusage).
 *
 * The first argument is who, the second the struct rusage, of longs.
 * `ru_utime` and `ru_stime` are the user and system time of the caller's
 * process, every thread's, for RUSAGE_SELF, the calling thread's for
 * RUSAGE_THREAD, and the totals of the children the process has reaped,
 * each with the children those reaped, for RUSAGE_CHILDREN: the ticks
 * `times` reports, as timevals. `ru_maxrss` is the most the present
 * address space or any earlier one has had resident (`mm_resident_peak`),
 * in kilobytes, or for RUSAGE_CHILDREN the most a reaped child reported;
 * no call walks the page tables for it. `ru_minflt` and `ru_majflt` are
 * the page faults of the same threads or children (`proc_faults`).
 * `ru_nvcsw` and `ru_nivcsw` count voluntary and involuntary context
 * switches of the same threads or children. The other counters are zero.
 * -EINVAL for any other `who`, Linux's RUSAGE_BOTH among them, -EFAULT for
 * a structure that cannot be written.
 */
long sys_getrusage(isr_params_t *params)
{
    int who = (int)SC_ARG(params, 0);
    struct kernel_rusage ru;
    uint32_t flags;
    uint64_t user;
    uint64_t sys;
    unsigned long minor;
    unsigned long major;

    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN && who != RUSAGE_THREAD) {
        return -EINVAL;
    }
    if (who == RUSAGE_SELF) {
        proc_cpu_time(current_proc, &user, &sys);
    } else {
        flags = kirq_save();
        user = who == RUSAGE_CHILDREN ? current_proc->tg->cutime : current_proc->utime;
        sys = who == RUSAGE_CHILDREN ? current_proc->tg->cstime : current_proc->stime;
        kirq_restore(flags);
    }
    rusage_from_ticks(&ru, user, sys);
    if (who == RUSAGE_SELF) {
        proc_faults(current_proc, &minor, &major);
        ru.minflt = (long)minor;
        ru.majflt = (long)major;
        proc_switches(current_proc, &minor, &major);
        ru.nvcsw = (long)minor;
        ru.nivcsw = (long)major;
    } else if (who == RUSAGE_THREAD) {
        ru.minflt = (long)current_proc->min_flt;
        ru.majflt = (long)current_proc->maj_flt;
        ru.nvcsw = (long)current_proc->nvcsw;
        ru.nivcsw = (long)current_proc->nivcsw;
    } else {
        ru.minflt = (long)current_proc->tg->cmin_flt;
        ru.majflt = (long)current_proc->tg->cmaj_flt;
        ru.nvcsw = (long)current_proc->tg->cnvcsw;
        ru.nivcsw = (long)current_proc->tg->cnivcsw;
    }
    if (who == RUSAGE_CHILDREN) {
        ru.maxrss = (long)current_proc->tg->cmaxrss_kb;
    } else {
        unsigned long now =
            current_proc->mm ? mm_resident_peak(current_proc->mm) * (PAGE_SIZE / 1024u) : 0;

        ru.maxrss = (long)(now > current_proc->tg->maxrss_kb ? now : current_proc->tg->maxrss_kb);
    }
    if (rusage_put(SC_ARG(params, 1), &ru) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * getuid: the caller's real uid. The four id getters, on i386 the 32-bit
 * calls 199 to 202, read the ids of the caller's record as its user
 * namespace maps them (`uid_seen`, `gid_seen`), and return them unsigned, so
 * that an id past 2^31 is not an error on x86-64. The setters and the group
 * list are later in this file.
 */
long sys_getuid(isr_params_t *params)
{
    (void)params;
    return (long)uid_seen(current_proc->ruid);
}

/** geteuid: the caller's effective uid, as `sys_getuid` gives an id. */
long sys_geteuid(isr_params_t *params)
{
    (void)params;
    return (long)uid_seen(current_proc->euid);
}

/** getgid: the caller's real gid, as `sys_getuid` gives an id. */
long sys_getgid(isr_params_t *params)
{
    (void)params;
    return (long)gid_seen(current_proc->rgid);
}

/** getegid: the caller's effective gid, as `sys_getuid` gives an id. */
long sys_getegid(isr_params_t *params)
{
    (void)params;
    return (long)gid_seen(current_proc->egid);
}

/** The default action for a signal whose disposition is SIG_DFL. */
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
    case SIGQUIT:
    case SIGILL:
    case SIGTRAP:
    case SIGABRT:
    case SIGBUS:
    case SIGFPE:
    case SIGSEGV:
    case SIGXCPU:
    case SIGXFSZ:
    case SIGSYS:
        return KSIG_DEFAULT_CORE;
    default:
        return KSIG_DEFAULT_TERM;
    }
}

/** The 64-bit sa_mask the user installed (two 32-bit words, signals 1..64). */
static uint64_t sigact_mask(const struct k_sigaction *sa)
{
    return (uint64_t)sa->mask[0] | ((uint64_t)sa->mask[1] << 32);
}

/** Turn interrupts off for a change of the signal sets. The signal masks are
 * 64-bit, so a read-modify-write is two stores on i386; signal_post runs from
 * IRQ context (Ctrl-C) as well as from a syscall, so a mutation an interrupt
 * can meet half done is made with interrupts off. Returns the prior IF so
 * the caller can restore it. */
static uint32_t sig_irq_save(void)
{
    return kirq_save();
}

/** Put back the interrupt flag `sig_irq_save` returned. */
static void sig_irq_restore(uint32_t flags)
{
    kirq_restore(flags);
}

/* The stop signals, which SIGCONT cancels and which cancel SIGCONT. */
#define SIG_STOPS (KSIG_BIT(SIGSTOP) | KSIG_BIT(SIGTSTP) | KSIG_BIT(SIGTTIN) | KSIG_BIT(SIGTTOU))

/** Make `t` runnable if it sleeps, clearing its timed wake: an
 * interruptible wait then calls `signal_pending` to learn whether to end
 * with -EINTR, and any wait that goes on arms its own deadline again. */
static void sig_wake(process_t *t)
{
    if (t && t->state == PROC_BLOCKED) {
        t->wakeup_tick = 0;
        sched_make_ready(t);
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
                    sched_make_ready(t);
                }
            }
            ptrace_group_change(tg);
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
                sched_make_ready(t);
            }
        }
    } else if (KSIG_BIT(signo) & SIG_STOPS) {
        sig_discard(target, KSIG_BIT(SIGCONT));
    }
}

/**
 * Whether the thread `t` can take one of the signals `bits` now: it has not
 * ended, it is not stopped, and it does not block them all. A stopped thread
 * is passed over, as Linux's `wants_signal` passes it over, since `sig_wake`
 * cannot wake it and a sibling that could take the signal would sleep on.
 */
static int sig_wants(const process_t *t, uint64_t bits)
{
    return t->state != PROC_ZOMBIE && t->state != PROC_UNUSED && t->state != PROC_STOPPED &&
           (bits & ~t->sig_blocked) != 0;
}

/**
 * The thread of `target`'s process to wake for `signo`, as Linux's
 * `complete_signal` chooses it: `target` when it can take the signal
 * (`sig_wants`), or else the first thread that can; `target` itself when no
 * thread can, and nothing when `target` has ended.
 */
static process_t *sig_taker(process_t *target, int signo)
{
    process_t *t;

    if (sig_wants(target, KSIG_BIT(signo))) {
        return target;
    }
    for (t = proc_list; t; t = t->next) {
        if (t->tg == target->tg && sig_wants(t, KSIG_BIT(signo))) {
            return t;
        }
    }
    return target->state != PROC_ZOMBIE ? target : NULL;
}

/**
 * A signal sent to the process wakes one thread (`sig_taker`), which may be
 * this one. When the new mask blocks a signal still pending for the process
 * that the old mask did not, this thread will not take it, so another thread
 * that can is woken to take it, as Linux's `retarget_shared_pending` wakes
 * one; otherwise the signal could wait while a thread that would take it
 * sleeps. A handler's mask and its return, the masks a wait installs and puts
 * back, and sigprocmask all set the mask here.
 */
void signal_set_blocked(uint64_t mask)
{
    process_t *self = current_proc;
    uint32_t flags = sig_irq_save();
    uint64_t moved;
    process_t *t;

    mask &= ~(uint64_t)KSIG_UNCATCHABLE;
    moved = self->tg->pending & mask & ~self->sig_blocked;
    self->sig_blocked = mask;
    for (t = proc_list; t && moved != 0; t = t->next) {
        if (t != self && t->tg == self->tg && sig_wants(t, moved)) {
            moved &= t->sig_blocked;
            sig_wake(t);
        }
    }
    sig_irq_restore(flags);
}

/**
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

    if (!t || signo < 1 || signo > KSIG_MAX || t->state == PROC_ZOMBIE || t->state == PROC_UNUSED) {
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

/** The signals pending for the current thread and its process, read after
 * the deferred kernel debug hits are posted, so a signal wait can dequeue
 * them with their siginfo. A deferred user hit stays out of these sets until
 * the return to user mode delivers it. Called on the thread's stack with the
 * big lock held. Read with interrupts off, since a 64-bit read is two loads
 * on i686. */
static uint64_t sig_pending_now(void)
{
    uint32_t flags = sig_irq_save();
    uint64_t set;

    arch_debug_deliver();
    set = current_proc->sig_pending | current_proc->tg->pending;
    sig_irq_restore(flags);
    return set;
}

/** The siginfo of a signal a process sends to `target`: `code` SI_USER for
 * kill and SI_TKILL for tkill and tgkill, with the sender's pid and real uid
 * as the target's namespaces see them. */
static void sender_info(struct k_siginfo *info, int32_t code, const process_t *target)
{
    k_memset(info, 0, sizeof(*info));
    info->code = code;
    info->pid = current_proc ? pid_seen_by(target, current_proc->tgid) : 0;
    info->uid = current_proc ? ns_uid_from_kernel(proc_userns(target), current_proc->ruid) : 0;
}

/** A stop the tracer asked for with PTRACE_INTERRUPT ends a wait as a stop
 * signal would, so the stop is taken on the way back to user mode
 * (`ptrace_return_stops`). A traced thread counts an ignored signal too, as
 * Linux's sig_ignored does, since its tracer sees each signal at a
 * signal-delivery stop, where a wait it cut short is restarted. Blocking
 * syscall loops call this on the thread's stack with the big lock held. A
 * deferred kernel debug hit is posted before its mask and disposition are
 * tested; a deferred user hit interrupts whatever either says, but stays
 * deferred until the return to user mode delivers it. Posting can make this
 * thread ready and clear its `wakeup_tick`, so a caller that keeps waiting
 * must arm its deadline again. No wait queue is changed. On the x86-64 debug
 * interrupt stack the kernel calls only `debugreg_defer_user` and, when it
 * declines, `debugreg_defer`; a user debug exception moves to the thread's
 * stack before entering C. */
int signal_pending(void)
{
    uint64_t deliverable;
    uint32_t flags;
    int debug_pending;
    int signo;

    if (!current_proc) {
        return 0;
    }
    if (current_proc->ptrace & PT_INTERRUPT) {
        return 1;
    }
    flags = sig_irq_save();
    deliverable = sig_pending_now() & ~current_proc->sig_blocked;
    debug_pending = arch_debug_pending();
    sig_irq_restore(flags);
    for (signo = 1; signo <= KSIG_MAX; signo++) {
        struct k_sigaction *sa;

        if (!(deliverable & KSIG_BIT(signo))) {
            continue;
        }
        sa = &current_proc->sigacts[signo];
        if (current_proc->ptrace & PT_TRACED) {
            return 1;
        }
        if (sa->handler == KSIG_IGN) {
            continue;
        }
        if (sa->handler == KSIG_DFL && ksig_default_action(signo) == KSIG_DEFAULT_IGNORE) {
            continue;
        }
        return 1;
    }
    return debug_pending;
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
 * move into a session to reach something. It does expose one narrow thing: a
 * member of a session may make a root process in that same session run its
 * `SIGCONT` handler at a time of the member's choosing.
 *
 * The signo 0 existence probe keeps the strict rule. The exception exists for
 * resuming a stopped job, not for discovering what is running, and POSIX
 * requires permission for the probe too. `tty_signal_group` stays unchecked:
 * those signals come from the terminal, not from a process.
 */
static int cred_may_signal(const process_t *target, int signo)
{
    if (proc_privileged(current_proc)) {
        return 1;
    }
    if (signo == SIGCONT && current_proc->sid == target->sid) {
        return 1;
    }
    return current_proc->ruid == target->ruid || current_proc->ruid == target->suid ||
           current_proc->euid == target->ruid || current_proc->euid == target->suid;
}

/** Post `signo` to every member of the process group `pgid` the caller may
 * signal, which kill(2) selects with a pid of 0 or below -1. The POSIX
 * outcome: 0 if any member may be signalled, -EPERM if members exist but none
 * may be, -ESRCH if the group is empty. signo 0 only probes. A member that
 * has ended and is not yet reaped counts and discards the signal, as on
 * Linux. */
static int32_t kill_group(int pgid, int signo)
{
    int found = 0;
    int allowed = 0;
    process_t *p;

    for (p = proc_list; p; p = p->next) {
        if (p->state == PROC_UNUSED || (int)p->pgid != pgid) {
            continue;
        }
        found = 1;
        if (!cred_may_signal(p, signo)) {
            continue;
        }
        allowed = 1;
        if (signo != 0 && p->state != PROC_ZOMBIE) {
            struct k_siginfo info;

            sender_info(&info, SI_USER, p);
            signal_post_info(p, signo, &info);
        }
    }
    if (!found) {
        return -ESRCH;
    }
    return allowed ? 0 : -EPERM;
}

/** Post `signo` to every process the caller sees and may signal, but its
 * own, the init of its PID namespace and the kernel's threads, which
 * kill(2) selects with a pid of -1, as Linux does; an init ends the others
 * so at a shutdown. Each thread of a process is posted to, and the posts
 * merge in the one pending set. The outcome is `kill_group`'s: 0, -EPERM
 * or -ESRCH. */
static int32_t kill_all(int signo)
{
    int found = 0;
    int allowed = 0;
    process_t *p;

    for (p = proc_list; p; p = p->next) {
        if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE || p == proc_idle ||
            p->space_root == 0 || p->tg == current_proc->tg || pid_seen(p->tgid) <= 1) {
            continue;
        }
        found = 1;
        if (!cred_may_signal(p, signo)) {
            continue;
        }
        allowed = 1;
        if (signo != 0) {
            struct k_siginfo info;

            sender_info(&info, SI_USER, p);
            signal_post_info(p, signo, &info);
        }
    }
    if (!found) {
        return -ESRCH;
    }
    return allowed ? 0 : -EPERM;
}

/** kill(2): pid > 0 a single process, which the id of any of its threads
 * names; pid == 0 the caller's process group; pid < -1 the group (-pid);
 * pid == -1 every process the caller may signal but itself and its PID
 * namespace's init (`kill_all`).
 * signo 0 just probes existence and permission. A process whose threads
 * have all ended exists until its parent reaps it, and takes a signal and
 * discards it, as on Linux, where `kill -0` of a zombie succeeds. 0,
 * -EINVAL, -ESRCH or -EPERM. */
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
        return pid_named((uint32_t)-pid) ? kill_group((int)pid_named((uint32_t)-pid), signo)
                                         : -ESRCH;
    }
    if (pid == -1) {
        return current_proc ? kill_all(signo) : -ESRCH;
    }
    target = proc_named((uint32_t)pid);
    if (!target || target->state == PROC_UNUSED) {
        return -ESRCH;
    }
    if (!cred_may_signal(target, signo)) {
        return -EPERM;
    }
    if (signo == 0 || target->tg->live == 0) {
        return 0;
    }
    sender_info(&info, SI_USER, target);
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
    t = proc_named((uint32_t)tid);
    if (!t || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE ||
        (tgid != 0 && t->tgid != pid_named((uint32_t)tgid))) {
        return -ESRCH;
    }
    if (!cred_may_signal(t, signo)) {
        return -EPERM;
    }
    if (signo == 0) {
        return 0;
    }
    sender_info(&info, SI_TKILL, t);
    signal_post_thread(t, signo, &info);
    return 0;
}

/** kill takes pid and signal (`do_kill`). */
long sys_kill(isr_params_t *params)
{
    return do_kill((int)SC_ARG(params, 0), (int)SC_ARG(params, 1));
}

/** tkill takes tid and signal. */
long sys_tkill(isr_params_t *params)
{
    return do_tkill(0, (int)SC_ARG(params, 0), (int)SC_ARG(params, 1));
}

/** tgkill takes tgid, tid and signal; a tgid that is not positive is
 * -EINVAL. */
long sys_tgkill(isr_params_t *params)
{
    if ((int)SC_ARG(params, 0) <= 0) {
        return -EINVAL;
    }
    return do_tkill((int)SC_ARG(params, 0), (int)SC_ARG(params, 1), (int)SC_ARG(params, 2));
}

void siginfo_to_caller(uint8_t *out, int signo, const struct k_siginfo *info)
{
#if defined(IA32_SYSCALL_TABLE_SIZE)
    if (in_compat_syscall()) {
        ia32_fill_siginfo(out, signo, info);
        return;
    }
#endif
    sigframe_fill_siginfo(out, signo, info);
}

void siginfo_from_caller(const uint8_t *in, struct k_siginfo *info)
{
#if defined(IA32_SYSCALL_TABLE_SIZE)
    if (in_compat_syscall()) {
        ia32_take_siginfo(in, info);
        return;
    }
#endif
    sigframe_take_siginfo(in, info);
}

/**
 * Copy and decode a queued signal's siginfo, in the caller's layout
 * (`siginfo_from_caller`), before resolving its target.
 * Returns 0 or -EFAULT. When nonnull, `signo` gets the signal number from
 * the same copy. This may sleep on a user page fault.
 */
static int32_t queued_info(uintptr_t u, struct k_siginfo *info, int32_t *signo)
{
    uint8_t buf[SIGINFO_SIZE];

    if (copy_from_user(buf, (const void __user *)u, sizeof(buf)) != 0) {
        return -EFAULT;
    }
    siginfo_from_caller(buf, info);
    if (signo) {
        k_memcpy(signo, buf, sizeof(*signo));
    }
    return 0;
}

/**
 * Check a copied siginfo's code and the caller's permission to signal `t`.
 * Kernel codes and SI_TKILL may be sent only within the caller's process.
 * Returns 0 or -EPERM; the big lock is held and this does not sleep.
 */
static int32_t queued_permit(process_t *t, int signo, const struct k_siginfo *info)
{
    if (((info->code >= 0 || info->code == SI_TKILL) && t->tg != current_proc->tg) ||
        !cred_may_signal(t, signo)) {
        return -EPERM;
    }
    return 0;
}

/**
 * rt_sigqueueinfo, which sigqueue calls, takes pid, signal and the siginfo,
 * posted to the process with that siginfo, under kill's permission. The kernel
 * keeps one pending instance of each signal, so a real-time signal sent again
 * while pending is merged, where Linux queues it. Signal 0, and a signal to a
 * process whose threads have all ended, are checked and not posted. -EINVAL
 * for a signal out of range, -ESRCH for no such process, and -EFAULT or
 * -EPERM from the siginfo or the permission.
 */
long sys_rt_sigqueueinfo(isr_params_t *params)
{
    int pid = (int)SC_ARG(params, 0);
    int signo = (int)SC_ARG(params, 1);
    process_t *target;
    struct k_siginfo info;
    int32_t rc;

    if (signo < 0 || signo > KSIG_MAX) {
        return -EINVAL;
    }
    rc = queued_info(SC_ARG(params, 2), &info, NULL);
    if (rc != 0) {
        return rc;
    }
    target = pid > 0 ? proc_named((uint32_t)pid) : NULL;
    if (!target || target->state == PROC_UNUSED) {
        return -ESRCH;
    }
    rc = queued_permit(target, signo, &info);
    if (rc == 0 && signo != 0 && target->tg->live != 0) {
        signal_post_info(target, signo, &info);
    }
    return rc;
}

/**
 * pidfd_send_signal takes a pidfd, a signal, a siginfo or 0, and flags,
 * which must be 0: the signal posted to the pidfd's process under kill's
 * permission, with the siginfo given, whose si_signo must be the signal and
 * whose code `queued_permit` checks, or else kill's. A process outside the
 * caller's PID namespace and those below it is EINVAL. Signal 0, and a signal
 * to a process whose threads have all ended, are checked and not posted.
 * -EINVAL, -EBADF for a descriptor that is not a pidfd, -ESRCH for a
 * process reaped, -EFAULT or -EPERM.
 */
long sys_pidfd_send_signal(isr_params_t *params)
{
    int signo = (int)SC_ARG(params, 1);
    uintptr_t uinfo = SC_ARG(params, 2);
    process_t *target;
    struct k_siginfo info;
    int32_t given;
    int32_t rc;

    if (SC_ARG(params, 3) != 0 || signo < 0 || signo > KSIG_MAX) {
        return -EINVAL;
    }
    if (uinfo) {
        rc = queued_info(uinfo, &info, &given);
        if (rc != 0) {
            return rc;
        }
        if (given != signo) {
            return -EINVAL;
        }
    }
    rc = pidfd_lookup((int)SC_ARG(params, 0), &target, NULL);
    if (rc != 0) {
        return rc;
    }
    if (!target) {
        return -ESRCH;
    }
    if (pid_seen(target->tgid) == 0) {
        return -EINVAL;
    }
    if (uinfo) {
        rc = queued_permit(target, signo, &info);
        if (rc != 0) {
            return rc;
        }
    } else {
        if (!cred_may_signal(target, signo)) {
            return -EPERM;
        }
        sender_info(&info, SI_USER, target);
    }
    if (signo != 0 && target->tg->live != 0) {
        signal_post_info(target, signo, &info);
    }
    return 0;
}

/** rt_tgsigqueueinfo takes tgid, tid, signal and the siginfo, posted to the
 * thread alone, as tgkill posts. */
long sys_rt_tgsigqueueinfo(isr_params_t *params)
{
    int tgid = (int)SC_ARG(params, 0);
    int tid = (int)SC_ARG(params, 1);
    int signo = (int)SC_ARG(params, 2);
    process_t *t;
    struct k_siginfo info;
    int32_t rc;

    if (tgid <= 0 || tid <= 0 || signo < 0 || signo > KSIG_MAX) {
        return -EINVAL;
    }
    rc = queued_info(SC_ARG(params, 3), &info, NULL);
    if (rc != 0) {
        return rc;
    }
    t = proc_named((uint32_t)tid);
    if (!t || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE ||
        t->tgid != pid_named((uint32_t)tgid)) {
        return -ESRCH;
    }
    rc = queued_permit(t, signo, &info);
    if (rc == 0 && signo != 0) {
        signal_post_thread(t, signo, &info);
    }
    return rc;
}

/**
 * Block the current thread until a deliverable signal is pending.
 *
 * The test of the pending signals and the move to PROC_BLOCKED are one
 * section with interrupts off, so a signal posted from an interrupt is
 * either seen by the test or finds the thread blocked and wakes it, as in
 * `wait_queue.c`. Interrupts are on only while the thread yields or idles.
 */
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

/**
 * Sleep until a signal is delivered (pause(2)). Returns -EINTR once a
 * deliverable signal is pending; the signal then acts on the return to user
 * mode. The call stands for Linux's ERESTARTNOHAND: a signal that runs no
 * handler, as a stop and a continue, restarts it.
 */
long sys_pause(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
    sig_wait();
    current_proc->restart_kind = RESTART_NOHAND;
    return -EINTR;
}

/**
 * Atomically install a signal mask and wait (rt_sigsuspend(2)).
 *
 * Its arguments are const sigset_t *mask and sigsetsize (8). Replaces the
 * blocked mask with *mask, waits for a deliverable signal, and returns -EINTR.
 * The handler the signal runs runs under *mask, and the frame it gets has the
 * old mask, which its return restores; without a handler the old mask comes
 * back on the return to user mode, and the call restarts (`signal_suspend`).
 * -EINVAL for another sigsetsize, -EFAULT when the mask cannot be read.
 */
long sys_rt_sigsuspend(isr_params_t *params)
{
    const void __user *umask = (const void __user *)SC_ARG(params, 0);
    uint32_t sigsetsize = USER_LEN32(SC_ARG(params, 1));
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
    return signal_suspend(newmask);
}

long signal_suspend(uint64_t mask)
{
    current_proc->saved_sigmask = current_proc->sig_blocked;
    current_proc->sigmask_saved = 1;
    signal_set_blocked(mask);
    sig_wait();
    current_proc->restart_kind = RESTART_NOHAND;
    return -EINTR;
}

long signal_action(int signo, const struct k_sigaction *act, struct k_sigaction *old)
{
    if (signo < 1 || signo > KSIG_MAX) {
        return -EINVAL;
    }
    if (act && (signo == SIGKILL || signo == SIGSTOP)) {
        return -EINVAL; /* cannot be caught or ignored */
    }
    if (old) {
        *old = current_proc->sigacts[signo];
        old->flags &= ~(unsigned long)SA_IA32_ABI;
    }
    if (act) {
        current_proc->sigacts[signo] = *act;
#if defined(IA32_SYSCALL_TABLE_SIZE)
        current_proc->sigacts[signo].flags &= ~(unsigned long)SA_IA32_ABI;
        if (in_compat_syscall()) {
            current_proc->sigacts[signo].flags |= SA_IA32_ABI;
        }
#endif
        if (act->handler == KSIG_IGN) {
            uint32_t f = sig_irq_save();
            sig_discard(current_proc, KSIG_BIT(signo));
            sig_irq_restore(f);
        }
    }
    return 0;
}

/**
 * Install or query a signal disposition (rt_sigaction).
 *
 * Its arguments are signo, const struct k_sigaction* act, struct k_sigaction*
 * old and sigsetsize, which must be 8. The new action is read first and the
 * old one written after the new one is stored, as Linux does
 * (`signal_action`). -EINVAL for another sigsetsize, a signal out of range
 * or a new action for SIGKILL or SIGSTOP, which writes no old action;
 * -EFAULT.
 */
long sys_rt_sigaction(isr_params_t *params)
{
    int signo = (int)SC_ARG(params, 0);
    const struct k_sigaction __user *uact = (const struct k_sigaction __user *)SC_ARG(params, 1);
    struct k_sigaction __user *uold = (struct k_sigaction __user *)SC_ARG(params, 2);
    uint32_t setsize = USER_LEN32(SC_ARG(params, 3));
    struct k_sigaction act, old;
    long rc;

    if (!current_proc) {
        return -ESRCH;
    }
    if (setsize != 8) {
        return -EINVAL;
    }
    if (uact && copy_from_user(&act, uact, sizeof(act)) != 0) {
        return -EFAULT;
    }
    rc = signal_action(signo, uact ? &act : NULL, uold ? &old : NULL);
    if (rc == 0 && uold && copy_to_user(uold, &old, sizeof(old)) != 0) {
        return -EFAULT;
    }
    return rc;
}

/**
 * Examine or change the blocked-signal mask (rt_sigprocmask).
 *
 * Its arguments are how (0 BLOCK/1 UNBLOCK/2 SETMASK), const set*, old set* and
 * sigsetsize. SIGKILL/SIGSTOP can never be blocked, and a signal of the process
 * that the new mask blocks is left to another thread (`signal_set_blocked`).
 * The old mask is written before the new one is read. -EINVAL for another
 * sigsetsize or an unknown `how`, -EFAULT.
 */
long sys_rt_sigprocmask(isr_params_t *params)
{
    int how = (int)SC_ARG(params, 0);
    const void __user *uset = (const void __user *)SC_ARG(params, 1);
    void __user *uold = (void __user *)SC_ARG(params, 2);
    uint32_t setsize = USER_LEN32(SC_ARG(params, 3));
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
            set |= current_proc->sig_blocked;
            break;
        case 1: /* SIG_UNBLOCK */
            set = current_proc->sig_blocked & ~set;
            break;
        case 2: /* SIG_SETMASK */
            break;
        default:
            return -EINVAL;
        }
        signal_set_blocked(set);
    }
    return 0;
}

/**
 * The shared body of sigreturn and rt_sigreturn.
 *
 * The frame the handler returned from gives back the registers, the
 * floating-point state and the mask; a frame that cannot be read or that would
 * load a faulting state kills the process with SIGSEGV. Returns the restored
 * result register, so the dispatcher's store of the result leaves it as it was.
 * A call the handler's signal cut short is not resumed by restart_syscall
 * after it, as Linux clears the restart block here.
 */
static long signal_return(isr_params_t *tf, int rt)
{
    uint64_t mask;

    if (!current_proc) {
        return -ESRCH;
    }
    current_proc->restart.fn = NULL;
    if (sigframe_restore(tf, rt, &mask) != 0) {
        terminate_current_process(SIGSEGV | WCOREFLAG);
        return 0;
    }
    signal_set_blocked(mask);
    return (long)SC_RET(tf);
}

/**
 * rt_sigreturn: the return of an SA_SIGINFO handler, and of every handler on
 * x86-64, through a restorer that issues the call with the stack pointer one
 * word past the frame's start, 4 bytes on i686 and 8 on x86-64.
 */
long sys_rt_sigreturn(isr_params_t *tf)
{
    return signal_return(tf, 1);
}

/**
 * sigreturn, on i686 only: the return of a plain handler, through a restorer
 * that pops the signal number first, 8 bytes past the frame's start.
 */
long sys_sigreturn(isr_params_t *tf)
{
    return signal_return(tf, 0);
}

/* Linux's stack_t of sigaltstack. */
struct k_stack {
    uintptr_t ss_sp;
    int32_t ss_flags;
    unsigned long ss_size;
};

#define SS_ONSTACK    1
#define SS_DISABLE    2
#define SS_AUTODISARM 0x80000000u
#define MINSIGSTKSZ   2048u

long signal_altstack(const struct k_altstack *ss, struct k_altstack *old, uintptr_t sp)
{
    int on = sigframe_on_altstack(sp);
    int32_t flags;

    if (old) {
        old->sp = current_proc->altstack_sp;
        old->size = current_proc->altstack_size;
        old->flags = !current_proc->altstack_on ? SS_DISABLE : (on ? SS_ONSTACK : 0);
    }
    if (!ss) {
        return 0;
    }
    if (on) {
        return -EPERM;
    }
    flags = ss->flags & ~SS_AUTODISARM;
    if (flags != 0 && flags != SS_DISABLE && flags != SS_ONSTACK) {
        return -EINVAL;
    }
    if (flags == SS_DISABLE) {
        current_proc->altstack_on = 0;
        current_proc->altstack_sp = 0;
        current_proc->altstack_size = 0;
    } else {
        if (ss->size < MINSIGSTKSZ) {
            return -ENOMEM;
        }
        current_proc->altstack_sp = ss->sp;
        current_proc->altstack_size = ss->size;
        current_proc->altstack_on = 1;
    }
    return 0;
}

/**
 * sigaltstack: set or read the alternate stack a handler with
 * SA_ONSTACK runs on (`signal_altstack`). Its arguments are new stack or
 * NULL and old stack or NULL. The old settings are zeroed first, so that
 * the padding after x86-64's int flags gives no byte of the kernel's
 * stack, and written only when the call succeeds. EFAULT for a copy that
 * fails.
 */
long sys_sigaltstack(isr_params_t *params)
{
    const struct k_stack __user *uss = (const struct k_stack __user *)SC_ARG(params, 0);
    struct k_stack __user *uoss = (struct k_stack __user *)SC_ARG(params, 1);
    struct k_stack cur;
    struct k_stack ss;
    struct k_altstack in, old;
    long rc;

    if (!current_proc) {
        return -ESRCH;
    }
    if (uss) {
        if (copy_from_user(&ss, uss, sizeof(ss)) != 0) {
            return -EFAULT;
        }
        in.sp = ss.ss_sp;
        in.flags = ss.ss_flags;
        in.size = ss.ss_size;
    }
    rc = signal_altstack(uss ? &in : NULL, &old, FRAME_SP(params));
    if (rc != 0) {
        return rc;
    }
    k_memset(&cur, 0, sizeof(cur));
    cur.ss_sp = old.sp;
    cur.ss_flags = old.flags;
    cur.ss_size = old.size;
    if (uoss && copy_to_user(uoss, &cur, sizeof(cur)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * rt_sigpending: the signals pending while blocked. Its arguments are set
 * and size of the set in bytes, at most 8; -EINVAL for a larger size, -EFAULT.
 */
uint64_t signal_pending_blocked(void)
{
    uint32_t flags = sig_irq_save();
    uint64_t set = sig_pending_now() & current_proc->sig_blocked;

    sig_irq_restore(flags);
    return set;
}

long sys_rt_sigpending(isr_params_t *params)
{
    uint32_t size = USER_LEN32(SC_ARG(params, 1));
    uint32_t words[2];
    uint64_t set;

    if (!current_proc) {
        return -ESRCH;
    }
    if (size > sizeof(words)) {
        return -EINVAL;
    }
    set = signal_pending_blocked();
    words[0] = (uint32_t)set;
    words[1] = (uint32_t)(set >> 32);
    if (copy_to_user((void __user *)SC_ARG(params, 0), words, size) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * sigprocmask: the old form of rt_sigprocmask, with signals 1 to 32
 * in one word. Its arguments are how, new set or NULL and old set or NULL.
 * SIG_SETMASK replaces signals 1 to 32 and leaves the rest. -EINVAL for an
 * unknown `how`, -EFAULT.
 */
long sys_sigprocmask(isr_params_t *params)
{
    int how = (int)SC_ARG(params, 0);
    const uint32_t __user *uset = (const uint32_t __user *)SC_ARG(params, 1);
    uint32_t __user *uold = (uint32_t __user *)SC_ARG(params, 2);
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
        signal_set_blocked(mask);
    }
    if (uold && copy_to_user(uold, &old, sizeof(old)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** Restart the system call the dispatcher recorded, `restart_nr` being one
 * more than its number: the number back in the number's register, or
 * restart_syscall's for a call of the RESTART_BLOCK kind, which resumes the
 * thread's restart block, as Linux's restart does, and the instruction
 * pointer back on the instruction that made the call. */
static void restart_syscall(isr_params_t *tf, uint32_t restart_nr, int kind)
{
    SC_NR(tf) = kind == RESTART_BLOCK ? SC_RESTART_NR(tf) : restart_nr - 1u;
    FRAME_PC(tf) -= SYSCALL_INSN_LEN;
}

/**
 * Free the records of the threads that have ended and are not leaders,
 * which no wait reaps, a traced one aside, which its tracer's wait reaps:
 * their CPU time goes to their process, and their address space reference,
 * kernel stack and record go back (`proc_release`). A zombie other than the
 * current thread is on no processor while the caller holds the big lock,
 * since a thread ends and switches away with the lock held, so its stack is
 * free to take. The release of the address space may sleep, and another
 * reaper may run then, so a record is taken off the zombies first, and the
 * walk starts again after each one, since the list may have changed under
 * it.
 */
static void reap_threads(void)
{
    process_t *p;

    while (proc_dead_threads != 0) {
        for (p = proc_list; p; p = p->next) {
            if (p != current_proc && p->state == PROC_ZOMBIE && p->pid != p->tgid &&
                !(p->ptrace & PT_TRACED)) {
                break;
            }
        }
        if (!p) {
            return;
        }
        p->state = PROC_UNUSED;
        proc_dead_threads--;
        proc_thread_fold(p);
        proc_release(p);
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
    ptrace_group_change(tg);
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
 * Park the current thread while its process is stopped, no SIGKILL is
 * pending and no tracer has attached to it. The scheduler keeps a STOPPED
 * thread off the processor (it only flips RUNNING to READY);
 * `sig_job_control` makes it READY again, and so does an attach
 * (`ptrace_unpark`), after which the caller makes the stop the tracer's.
 * `sti;hlt; cli` from IF=0 is lost-wakeup-safe.
 */
static void sig_park(void)
{
    uint32_t flags = sig_irq_save();

    while (current_proc->tg->stop && !(sig_pending_now() & KSIG_BIT(SIGKILL)) &&
           !(current_proc->ptrace & PT_TRACED)) {
        current_proc->state = PROC_STOPPED;
        schedule();
        if (current_proc->state == PROC_STOPPED) {
            idle_wait_irq_off();
        }
    }
    current_proc->state = PROC_RUNNING;
    sig_irq_restore(flags);
}

/**
 * Take a signal of `wanted`, one of which is pending, off the current
 * thread's pending signals into `*info`, as Linux's `dequeue_signal` takes
 * it: the lowest of the thread's own, or, when the thread has none of
 * `wanted`, the lowest of its process's. Returns the signal.
 */
static int sig_dequeue(uint64_t wanted, struct k_siginfo *info)
{
    uint32_t flags = sig_irq_save();
    uint64_t own = current_proc->sig_pending & wanted;
    uint64_t from = own != 0 ? own : current_proc->tg->pending & wanted;
    int signo;

    for (signo = 1; signo < KSIG_MAX; signo++) {
        if (from & KSIG_BIT(signo)) {
            break;
        }
    }
    if (own != 0) {
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
 * Take the stop of the current thread's process, when one is in effect, no
 * SIGKILL is pending and the thread has not reported the stop: a traced
 * thread reports it to its tracer (`ptrace_group_stop`), and another parks
 * (`sig_park`). Returns 1 when it did either, so the caller looks at the
 * stop again: a thread its tracer detached meanwhile then parks. 0 when
 * there was nothing to take.
 */
static int group_stop_taken(void)
{
    if (!current_proc->tg->stop || (sig_pending_now() & KSIG_BIT(SIGKILL)) ||
        current_proc->stop_seen) {
        return 0;
    }
    if (!ptrace_group_stop((int)current_proc->tg->stop)) {
        sig_park();
    }
    return 1;
}

/** Whether a handler with the flags `flags` lets a call of the restart
 * kind `kind` restart, as Linux's handle_signal judges it: ERESTARTSYS
 * under SA_RESTART, ERESTARTNOINTR always, and the others never. */
static int signal_restarts(uint32_t flags, int kind)
{
    return kind == RESTART_NOINTR || (kind == RESTART_SYS && (flags & SA_RESTART) != 0);
}

/**
 * Only a frame that returns to user mode is acted on, and one handler at most
 * is given a frame on each return. A deliverable signal, one pending for the
 * thread or for its process and not blocked, is taken (`sig_dequeue`), the
 * lowest-numbered of the thread's own or, when it has none, of its process's,
 * and is ignored, ends the process (a default that terminates), stops the
 * process, or gets a handler frame. The handler runs with its sa_mask blocked
 * as well, and its own signal unless SA_NODEFER; the signals still pending
 * are acted on when its sigreturn comes back through here. SA_RESETHAND
 * resets the action after the frame has the handler. A thread whose process
 * is stopped parks here first; a traced one reports the stop to its tracer
 * instead and looks at the stop again when it resumes, so one its tracer
 * detached meanwhile parks, as Linux's detach leaves a tracee to its group's
 * stop. The records of ended threads are freed here too, a point every
 * running process passes where none of them is in use.
 *
 * A system call the dispatcher found interrupted and restartable is
 * restarted when no handler runs, as after a stop and a continue, whatever
 * its kind (`restart_kind`), and when the handler that runs lets its kind
 * restart (`signal_restarts`); otherwise it keeps its EINTR. A mask
 * rt_sigsuspend, pselect6 or ppoll replaced goes into the handler's frame,
 * or back in place at once when no handler runs.
 */
void deliver_signals(isr_params_t *tf)
{
    uint32_t restart_nr;
    int restart_kind;

    if (!current_proc) {
        return;
    }
    if (!FRAME_USER(tf)) {
        return;
    }
    if (proc_dead_threads != 0) {
        reap_threads();
    }
    if (current_proc->ptrace) {
        ptrace_return_stops();
    }
    restart_nr = current_proc->restart_nr;
    restart_kind = current_proc->restart_kind;
    current_proc->restart_nr = 0;
    current_proc->restart_kind = RESTART_SYS;

    for (;;) {
        uint64_t deliverable;
        struct k_sigaction *sa;
        struct k_siginfo info;
        uint64_t oldmask, newblock;
        uint32_t handler_flags;
        int signo;

        if (group_stop_taken()) {
            continue;
        }
        deliverable = sig_pending_now() & ~current_proc->sig_blocked;
        if (deliverable == 0) {
            break;
        }
        signo = sig_dequeue(deliverable, &info);
        if (current_proc->ptrace) {
            /* The tracer may cancel the call's restart through orig_nr. */
            current_proc->restart_nr = restart_nr;
            current_proc->orig_nr = restart_nr ? (int32_t)restart_nr - 1 : -1;
            signo = ptrace_signal_stop(signo, &info);
            restart_nr = current_proc->restart_nr;
            current_proc->restart_nr = 0;
            if (signo == 0) {
                continue;
            }
        }
        sa = &current_proc->sigacts[signo];

        if (sa->handler == KSIG_IGN) {
            continue;
        }
        if (sa->handler == KSIG_DFL) {
            enum ksig_default def = ksig_default_action(signo);
            if (def == KSIG_DEFAULT_TERM || def == KSIG_DEFAULT_CORE) {
                terminate_current_process(def == KSIG_DEFAULT_CORE ? signo | WCOREFLAG : signo);
                return; /* not reached */
            }
            if (def == KSIG_DEFAULT_STOP) {
                sig_group_stop(signo);
                continue;
            }
            continue; /* default ignore (SIGCHLD/SIGCONT/SIGURG/SIGWINCH) */
        }

        handler_flags = (uint32_t)sa->flags;
        if (restart_nr != 0 && signal_restarts(handler_flags, restart_kind)) {
            restart_syscall(tf, restart_nr, restart_kind);
        }
        oldmask =
            current_proc->sigmask_saved ? current_proc->saved_sigmask : current_proc->sig_blocked;
        rseq_signal(tf, signo);
        if (sigframe_setup(tf, signo, sa, &info, oldmask) != 0) {
            terminate_current_process(SIGSEGV | WCOREFLAG);
            return;
        }
        current_proc->sigmask_saved = 0;
        newblock = current_proc->sig_blocked | sigact_mask(sa);
        if (!(handler_flags & SA_NODEFER)) {
            newblock |= KSIG_BIT(signo);
        }
        signal_set_blocked(newblock);
        if (handler_flags & SA_RESETHAND) {
            sa->handler = KSIG_DFL;
        }
        return;
    }

    if (restart_nr != 0) {
        restart_syscall(tf, restart_nr, restart_kind);
    }
    if (current_proc->sigmask_saved) {
        current_proc->sigmask_saved = 0;
        signal_set_blocked(current_proc->saved_sigmask);
    }
}

/**
 * Wait, as rt_sigtimedwait does, for a signal of `set` to be pending for the
 * current thread or its process until the tick `deadline` when `timed`, and
 * take it, its siginfo into `info`: its number, or -EINTR for another
 * signal the thread acts on, or -EAGAIN at the deadline.
 */
static int32_t sigwait_take(uint64_t set, int timed, uint64_t deadline, struct k_siginfo *info)
{
    uint32_t flags = sig_irq_save();

    for (;;) {
        uint64_t hit = sig_pending_now() & set;

        if (hit) {
            int signo = sig_dequeue(hit, info);

            current_proc->wakeup_tick = 0;
            sig_irq_restore(flags);
            return signo;
        }
        if (signal_pending() || (timed && tick_count() >= deadline)) {
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

/** `sigwait_take`, with the siginfo copied to `uinfo` unless it is null, in
 * the caller's layout; -EFAULT when it cannot be. */
static int32_t sigwait_set(uint64_t set, int timed, uint64_t deadline, void __user *uinfo)
{
    struct k_siginfo info;
    uint8_t buf[128];
    int32_t signo = sigwait_take(set, timed, deadline, &info);

    if (signo > 0 && uinfo) {
        siginfo_to_caller(buf, (int)signo, &info);
        if (copy_to_user(uinfo, buf, sizeof(buf)) != 0) {
            return -EFAULT;
        }
    }
    return signo;
}

/** Read the caller's `struct timespec` at `u` into `ts`: 0 or -EFAULT. */
static int32_t timespec_read(uintptr_t u, struct kernel_timespec *ts)
{
    return copy_from_user(ts, (const void __user *)u, sizeof(*ts)) == 0 ? 0 : -EFAULT;
}

/**
 * Wait for a signal of a set and take it (rt_sigtimedwait),
 * which sigwait, sigwaitinfo and sigtimedwait make.
 *
 * Its arguments are const sigset_t *set, siginfo_t *info or NULL, const struct
 * timespec *timeout or NULL and sigsetsize, which must be 8. A signal of `set`
 * pending for the thread or its process, the lowest of the thread's own or,
 * when it has none of them, the lowest of the process's, is taken off its
 * pending set without its handler running. While the call waits the signals of
 * `set` count as unblocked, as Linux counts them, so a signal sent to the
 * process wakes this thread; when it returns the old mask comes back, and a
 * signal of `set` that it blocks and that is still pending for the process is
 * left to another thread (`signal_set_blocked`). The timeout is rounded up to
 * whole ticks (`tick_span`), and the tick in progress added; one of zero
 * polls. SIGKILL and SIGSTOP cannot be waited
 * for. Returns the signal, -EAGAIN when the timeout passes, -EINTR for another
 * signal the thread acts on, -EINVAL for another sigsetsize or a timeout out
 * of range, -EFAULT.
 */
long sys_rt_sigtimedwait(isr_params_t *params)
{
    if (!current_proc) {
        return -ESRCH;
    }
    return signal_timedwait(SC_ARG(params, 3), SC_ARG(params, 0), SC_ARG(params, 1),
                            SC_ARG(params, 2), timespec_read);
}

long signal_timedwait(unsigned long setsize, uintptr_t uset, uintptr_t uinfo, uintptr_t uts,
                      int32_t (*read_ts)(uintptr_t u, struct kernel_timespec *ts))
{
    struct kernel_timespec ts;
    uint64_t set;
    uint64_t saved;
    uint64_t deadline = 0;
    int timed = 0;
    int32_t rc;

    if (setsize != sizeof(uint64_t)) {
        return -EINVAL;
    }
    if (copy_from_user(&set, (const void __user *)uset, sizeof(set)) != 0) {
        return -EFAULT;
    }
    if (uts) {
        uint64_t ticks;

        rc = read_ts(uts, &ts);
        if (rc != 0) {
            return rc;
        }
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) {
            return -EINVAL;
        }
        ticks = tick_span(ts.tv_sec, (uint32_t)ts.tv_nsec);
        deadline = rtc_ticks_now() + ticks + (ticks != 0 ? 1u : 0u);
        timed = 1;
    }
    set &= ~(uint64_t)KSIG_UNCATCHABLE;
    saved = current_proc->sig_blocked;
    signal_set_blocked(saved & ~set);
    rc = sigwait_set(set, timed, deadline, (void __user *)uinfo);
    signal_set_blocked(saved);
    return rc;
}

#define SFD_NONBLOCK 04000
#define SFD_CLOEXEC  02000000

/* A signalfd: the signals its reads take. */
struct sigfd {
    struct anon_obj anon;
    uint64_t mask;
};

/** Store the 32-bit `v` at `at`, which need not be aligned. */
static void put_word(uint8_t *at, uint32_t v)
{
    k_memcpy(at, &v, sizeof(v));
}

/** Store the 64-bit `v` at `at`, which need not be aligned. */
static void put_u64(uint8_t *at, uint64_t v)
{
    k_memcpy(at, &v, sizeof(v));
}

/**
 * The 128-byte struct signalfd_siginfo of `signo` and `info`, in Linux's
 * fields: a timer's id, overrun and value, a child's pid, uid and status, a
 * fault's address, or a sender's pid, uid and value.
 */
static void sigfd_fill(uint8_t *out, int signo, const struct k_siginfo *info)
{
    int fault = (signo == SIGSEGV || signo == SIGBUS || signo == SIGILL || signo == SIGFPE ||
                 signo == SIGTRAP) &&
                info->code > 0;

    k_memset(out, 0, 128);
    put_word(out, (uint32_t)signo);
    put_word(out + 8, (uint32_t)info->code);
    if (fault) {
        put_u64(out + 72, info->addr); /* ssi_addr */
    } else if (info->code == SI_TIMER) {
        put_word(out + 24, info->pid);             /* ssi_tid */
        put_word(out + 32, info->uid);             /* ssi_overrun */
        put_word(out + 44, (uint32_t)info->value); /* ssi_int */
        put_u64(out + 48, info->value);            /* ssi_ptr */
    } else {
        put_word(out + 12, info->pid);
        put_word(out + 16, info->uid);
        put_word(out + 40, (uint32_t)info->status); /* ssi_status */
        put_word(out + 44, (uint32_t)info->status); /* ssi_int */
        put_u64(out + 48, info->code == SI_QUEUE || info->code == SI_MESGQ
                              ? info->value
                              : (uint32_t)info->status);
    }
}

/**
 * read(2) of a signalfd: the pending signals of its mask, a 128-byte record
 * each, as many as fit, taken off the thread's or the process's pending set
 * as sigtimedwait takes them. With none pending it waits for one, the mask
 * counting as unblocked meanwhile, as sigtimedwait's set does, or fails with
 * EAGAIN when non-blocking, and with EINTR when another signal ends the wait;
 * EINVAL for a buffer shorter than a record, EFAULT when no record could be
 * copied.
 */
static int32_t sigfd_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct sigfd *sf = (struct sigfd *)e->priv;
    uint8_t one[128];
    uint32_t done = 0;

    if (count < sizeof(one)) {
        return -EINVAL;
    }
    while (done + sizeof(one) <= count) {
        struct k_siginfo info;
        uint64_t hit = sig_pending_now() & sf->mask;
        int32_t signo;

        if (hit) {
            signo = sig_dequeue(hit, &info);
        } else if (done > 0) {
            break;
        } else if (e->ofd && (e->ofd->status_flags & OFD_NONBLOCK)) {
            return -EAGAIN;
        } else {
            uint64_t saved = current_proc->sig_blocked;

            signal_set_blocked(saved & ~sf->mask);
            signo = sigwait_take(sf->mask, 0, 0, &info);
            signal_set_blocked(saved);
            if (signo < 0) {
                return signo;
            }
        }
        sigfd_fill(one, (int)signo, &info);
        if (copy_to_user((void __user *)(buf + done), one, sizeof(one)) != 0) {
            return done > 0 ? (int32_t)done : -EFAULT;
        }
        done += sizeof(one);
    }
    return (int32_t)done;
}

/** Readable while a signal of the mask is pending for the poller. A signal
 * sent while it sleeps wakes it as the thread the signal would go to. */
static short sigfd_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct sigfd *sf = (struct sigfd *)e->priv;

    if (wq) {
        *wq = NULL;
    }
    return (short)((sig_pending_now() & sf->mask) ? (events & POLLIN) : 0);
}

/** Free the signalfd's record when its last descriptor closes. */
static void sigfd_close(fd_entry_t *e)
{
    kfree(e->priv);
    e->priv = NULL;
}

static const struct file_ops sigfd_ops = {sigfd_read,  NULL, sigfd_poll, NULL,
                                          sigfd_close, NULL, NULL};

/**
 * signalfd and signalfd4 take -1 for a new signalfd or the descriptor of one to
 * change, the mask and its size, which must be 8, and for signalfd4 the flags
 * SFD_NONBLOCK and SFD_CLOEXEC. SIGKILL and SIGSTOP are left out of the mask,
 * as Linux leaves them. Returns the descriptor, or -EINVAL for another size,
 * an unknown flag or an open descriptor that is not a signalfd, -EBADF for a
 * descriptor that is not open, -EFAULT, -ENOMEM, or `fd_alloc`'s error.
 */
static int32_t signalfd_common(isr_params_t *params, uint32_t flags)
{
    int fd = (int)SC_ARG(params, 0);
    uint64_t mask;
    struct sigfd *sf;
    fd_entry_t *e;

    if (SC_ARG(params, 2) != sizeof(mask) || (flags & ~(uint32_t)(SFD_NONBLOCK | SFD_CLOEXEC))) {
        return -EINVAL;
    }
    if (copy_from_user(&mask, (const void __user *)SC_ARG(params, 1), sizeof(mask)) != 0) {
        return -EFAULT;
    }
    mask &= ~(uint64_t)KSIG_UNCATCHABLE;
    if (fd != -1) {
        e = fd_get(current_proc->fds, fd);
        if (!e) {
            return -EBADF;
        }
        if (e->ops != &sigfd_ops) {
            return -EINVAL;
        }
        ((struct sigfd *)e->priv)->mask = mask;
        return fd;
    }
    sf = kzalloc(sizeof(*sf));
    if (!sf) {
        return -ENOMEM;
    }
    sf->anon.name = "[signalfd]";
    sf->mask = mask;
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(sf);
        return fd;
    }
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_ANON;
    e->ops = &sigfd_ops;
    e->priv = sf;
    e->flags = FD_FLAG_READ | ((flags & SFD_CLOEXEC) ? FD_FLAG_CLOEXEC : 0u);
    if (flags & SFD_NONBLOCK) {
        e->ofd->status_flags |= OFD_NONBLOCK;
    }
    return fd;
}

/** signalfd: `signalfd_common` with no flags. */
long sys_signalfd(isr_params_t *params)
{
    return signalfd_common(params, 0);
}

/** signalfd4: `signalfd_common` with the flags of the fourth argument. */
long sys_signalfd4(isr_params_t *params)
{
    return signalfd_common(params, (uint32_t)SC_ARG(params, 3));
}

/**
 * The calling process's PID (getpid(2)), which is its thread group's id, in
 * its PID namespace (`pid_seen`), as every id these calls give and take is.
 */
long sys_getpid(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
    return (int32_t)pid_seen(current_proc->tgid);
}

/**
 * The calling thread's id (gettid(2)); the process id for the thread that
 * began the process.
 */
long sys_gettid(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
    return (int32_t)pid_seen(current_proc->pid);
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
 * The caller becomes the leader of a new session and of a new process group,
 * both numbered with its pid. Fails with -EPERM if the caller already leads
 * a process group (a group leader cannot take its group into a new
 * session). Returns the new session id (the caller's pid).
 */
long sys_setsid(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
    if (current_proc->pgid == current_proc->tgid) {
        return -EPERM;
    }
    group_set_ids(current_proc, current_proc->tgid, current_proc->tgid);
    return (int32_t)pid_seen(current_proc->tgid);
}

/**
 * Set a process's process group (setpgid(2)).
 *
 * Its arguments are pid (0 = caller) and pgid (0 = the target's own pid, i.e.
 * make it a group leader). The target must be the caller or a child of the
 * caller, else -ESRCH. A child must be in the caller's session, else -EPERM.
 * The target must not be a session leader, whose group is fixed with its
 * session, else -EPERM. A group other than the target's own must already
 * exist in the target's session, else -EPERM: some process must be in it,
 * whether or not its leader is still alive (`pgrp_in_session`). Returns 0;
 * -EINVAL for a negative pgid, -EPERM for a pgid the caller cannot see, and
 * -ESRCH for no such process. A child that has successfully executed a new
 * image cannot be moved by its parent; the call returns -EACCES.
 */
long sys_setpgid(isr_params_t *params)
{
    int pid = (int)SC_ARG(params, 0);
    int pgid = (int)SC_ARG(params, 1);
    process_t *target;

    if (!current_proc) {
        return -ESRCH;
    }
    if (pgid < 0) {
        return -EINVAL;
    }
    if (pgid != 0) {
        pgid = (int)pid_named((uint32_t)pgid);
        if (pgid == 0) {
            return -EPERM;
        }
    }
    target = (pid == 0) ? current_proc : proc_named((uint32_t)pid);
    if (!target || target->state == PROC_UNUSED || target->tg->live == 0) {
        return -ESRCH;
    }
    if (target->tg != current_proc->tg && target->parent_pid != current_proc->tgid) {
        return -ESRCH;
    }
    if (target->tg != current_proc->tg && target->sid != current_proc->sid) {
        return -EPERM;
    }
    if (target->tg != current_proc->tg && target->tg->execed) {
        return -EACCES;
    }
    if (target->sid == target->tgid) {
        return -EPERM;
    }
    if (pgid == 0) {
        pgid = (int)target->tgid;
    }
    if ((uint32_t)pgid != target->tgid && !pgrp_in_session(pgid, (int)target->sid)) {
        return -EPERM;
    }
    group_set_ids(target, target->sid, (uint32_t)pgid);
    return 0;
}

/** Resolve a getpgid/getsid target: the caller for pid 0, else the named
 * process, or NULL for none. */
static process_t *pgrp_target(int pid)
{
    if (pid == 0) {
        return current_proc;
    }
    return proc_named((uint32_t)pid);
}

/**
 * A process's process-group id (getpgid(2)). Its argument is pid (0 =
 * caller); -ESRCH for no such process.
 */
long sys_getpgid(isr_params_t *params)
{
    process_t *target;

    if (!current_proc) {
        return -ESRCH;
    }
    target = pgrp_target((int)SC_ARG(params, 0));
    if (!target) {
        return -ESRCH;
    }
    return (int32_t)pid_seen(target->pgid);
}

/** The caller's process-group id (getpgrp(2), the no-arg form). */
long sys_getpgrp(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
    return (int32_t)pid_seen(current_proc->pgid);
}

/**
 * A process's session id (getsid(2)). Its argument is pid (0 = caller);
 * -ESRCH for no such process.
 */
long sys_getsid(isr_params_t *params)
{
    process_t *target;

    if (!current_proc) {
        return -ESRCH;
    }
    target = pgrp_target((int)SC_ARG(params, 0));
    if (!target) {
        return -ESRCH;
    }
    return (int32_t)pid_seen(target->sid);
}

/** The parent's PID (getppid(2)). */
long sys_getppid(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
    return (int32_t)pid_seen(current_proc->parent_pid);
}

/**
 * Set the file-creation mask, returning the previous value (umask(2)). Its
 * argument is the new mask, of which only the low 9 permission bits are kept.
 */
long sys_umask(isr_params_t *params)
{
    uint32_t old;

    if (!current_proc) {
        return -ESRCH;
    }
    old = current_proc->fs->umask;
    current_proc->fs->umask = SC_ARG(params, 0) & 0777;
    return (int32_t)old;
}

/**
 * setuid takes a user ID. A caller privileged in its user namespace
 * (`id_privileged`) gets it as its real, effective and saved IDs; another may
 * make its real or saved ID its effective one. The change is the calling
 * thread's alone, as on Linux, and a new effective ID loses what `cred_moved`
 * takes away; ids that leave root empty the ambient set
 * (`ambient_uid_moved`). -EINVAL for -1 or an ID the namespace does not map
 * (`ids_named`), -EPERM otherwise.
 */
long sys_setuid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t uid = (uint32_t)SC_ARG(params, 0);
    uint32_t old_euid = p->euid;
    int had_root = uids_root(p);

    if (uid == SC_ID_KEEP || ids_named(&uid, 1, 0) != 0) {
        return -EINVAL;
    }
    if (id_privileged()) {
        if (proc_set_ruid(p, uid) != 0) {
            return -ENOMEM;
        }
        p->euid = uid;
        p->suid = uid;
        fsuid_follow(p);
        cred_moved(p, old_euid, p->egid);
        ambient_uid_moved(p, had_root);
        return 0;
    }
    if (uid == p->ruid || uid == p->suid) {
        p->euid = uid;
        fsuid_follow(p);
        cred_moved(p, old_euid, p->egid);
        ambient_uid_moved(p, had_root);
        return 0;
    }
    return -EPERM;
}

/** setgid: setuid's rule over the group IDs. */
long sys_setgid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t gid = (uint32_t)SC_ARG(params, 0);
    uint32_t old_egid = p->egid;

    if (gid == SC_ID_KEEP || ids_named(&gid, 1, 1) != 0) {
        return -EINVAL;
    }
    if (id_privileged()) {
        p->rgid = gid;
        p->egid = gid;
        p->sgid = gid;
        fsgid_follow(p);
        cred_moved(p, p->euid, old_egid);
        return 0;
    }
    if (gid == p->rgid || gid == p->sgid) {
        p->egid = gid;
        fsgid_follow(p);
        cred_moved(p, p->euid, old_egid);
        return 0;
    }
    return -EPERM;
}

/**
 * setreuid takes a real and an effective user ID, -1 keeping either. A caller
 * that is not privileged (`id_privileged`) may set the real ID to its real or
 * effective one, and the effective ID to any of its three. The saved ID takes
 * the new effective one when the real ID is set, or when the effective ID is
 * set to other than the old real one. Ids that leave root empty the ambient
 * set (`ambient_uid_moved`). -EINVAL for an ID the namespace does not map,
 * -EPERM.
 */
long sys_setreuid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t v[2];
    uint32_t r, e, new_r, new_e;
    uint32_t old_euid = p->euid;
    int had_root = uids_root(p);

    v[0] = (uint32_t)SC_ARG(params, 0);
    v[1] = (uint32_t)SC_ARG(params, 1);
    if (ids_named(v, 2, 0) != 0) {
        return -EINVAL;
    }
    r = v[0];
    e = v[1];
    new_r = (r == SC_ID_KEEP) ? p->ruid : r;
    new_e = (e == SC_ID_KEEP) ? p->euid : e;
    if (!id_privileged()) {
        if (r != SC_ID_KEEP && new_r != p->ruid && new_r != p->euid) {
            return -EPERM;
        }
        if (e != SC_ID_KEEP && new_e != p->ruid && new_e != p->euid && new_e != p->suid) {
            return -EPERM;
        }
    }
    r = p->ruid;
    if (proc_set_ruid(p, new_r) != 0) {
        return -ENOMEM;
    }
    if (v[0] != SC_ID_KEEP || (e != SC_ID_KEEP && new_e != r)) {
        p->suid = new_e;
    }
    p->euid = new_e;
    fsuid_follow(p);
    cred_moved(p, old_euid, p->egid);
    ambient_uid_moved(p, had_root);
    return 0;
}

/** setregid: setreuid's rule over the group IDs. */
long sys_setregid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t v[2];
    uint32_t r, e, new_r, new_e;
    uint32_t old_egid = p->egid;

    v[0] = (uint32_t)SC_ARG(params, 0);
    v[1] = (uint32_t)SC_ARG(params, 1);
    if (ids_named(v, 2, 1) != 0) {
        return -EINVAL;
    }
    r = v[0];
    e = v[1];
    new_r = (r == SC_ID_KEEP) ? p->rgid : r;
    new_e = (e == SC_ID_KEEP) ? p->egid : e;
    if (!id_privileged()) {
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
    fsgid_follow(p);
    cred_moved(p, p->euid, old_egid);
    return 0;
}

/**
 * setresuid takes a real, an effective and a saved user ID, -1 keeping any of
 * them. A caller that is not privileged (`id_privileged`) may set each only to
 * one of its current three. Ids that leave root empty the ambient set
 * (`ambient_uid_moved`). -EINVAL for an ID the namespace does not map,
 * -EPERM.
 */
long sys_setresuid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t v[3];
    uint32_t r, e, s;
    uint32_t old_euid = p->euid;
    int had_root = uids_root(p);

    v[0] = (uint32_t)SC_ARG(params, 0);
    v[1] = (uint32_t)SC_ARG(params, 1);
    v[2] = (uint32_t)SC_ARG(params, 2);
    if (ids_named(v, 3, 0) != 0) {
        return -EINVAL;
    }
    r = v[0];
    e = v[1];
    s = v[2];
    if (!id_privileged()) {
        int i;

        for (i = 0; i < 3; i++) {
            if (v[i] != SC_ID_KEEP && v[i] != p->ruid && v[i] != p->euid && v[i] != p->suid) {
                return -EPERM;
            }
        }
    }
    if (r != SC_ID_KEEP && proc_set_ruid(p, r) != 0) {
        return -ENOMEM;
    }
    if (e != SC_ID_KEEP) {
        p->euid = e;
    }
    if (s != SC_ID_KEEP) {
        p->suid = s;
    }
    fsuid_follow(p);
    cred_moved(p, old_euid, p->egid);
    ambient_uid_moved(p, had_root);
    return 0;
}

/** setresgid: setresuid's rule over the group IDs. */
long sys_setresgid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t v[3];
    uint32_t r, e, s;
    uint32_t old_egid = p->egid;

    v[0] = (uint32_t)SC_ARG(params, 0);
    v[1] = (uint32_t)SC_ARG(params, 1);
    v[2] = (uint32_t)SC_ARG(params, 2);
    if (ids_named(v, 3, 1) != 0) {
        return -EINVAL;
    }
    r = v[0];
    e = v[1];
    s = v[2];
    if (!id_privileged()) {
        int i;

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
    fsgid_follow(p);
    cred_moved(p, p->euid, old_egid);
    return 0;
}

/**
 * setfsuid's work on the user ID `uid`, as the caller's user namespace
 * names it: it becomes the caller's file-system user ID when it is the
 * caller's real, effective, saved or file-system ID, or the caller is
 * privileged in its user namespace (`id_privileged`). The call has no error,
 * as on Linux: an ID that is refused, or that the namespace does not map, -1
 * among them, changes nothing, and the previous file-system user ID comes
 * back either way, as the namespace sees it, which is how a program reads
 * it. The ID is the calling thread's alone and stays apart from the
 * effective one until the next call that sets user IDs or an exec
 * (`fsuid_follow`); a change undoes what `cred_undump` takes away.
 */
static long fsuid_set(uint32_t uid)
{
    process_t *p = current_proc;
    uint32_t old = proc_fsuid(p);
    int64_t k = uid == SC_ID_KEEP ? -1 : uid_named(uid);

    uid = (uint32_t)k;
    if (k >= 0 && uid != old &&
        (uid == p->ruid || uid == p->euid || uid == p->suid || id_privileged())) {
        p->fsuid = uid;
        p->fs_own |= PROC_FSUID_OWN;
        cred_undump(p);
    }
    return (long)uid_seen(old);
}

/** `fsuid_set` for the file-system group ID, which also decides the group
 * class of the file access checks. */
static long fsgid_set(uint32_t gid)
{
    process_t *p = current_proc;
    uint32_t old = proc_fsgid(p);
    int64_t k = gid == SC_ID_KEEP ? -1 : gid_named(gid);

    gid = (uint32_t)k;
    if (k >= 0 && gid != old &&
        (gid == p->rgid || gid == p->egid || gid == p->sgid || id_privileged())) {
        p->fsgid = gid;
        p->fs_own |= PROC_FSGID_OWN;
        cred_undump(p);
    }
    return (long)gid_seen(old);
}

/** The 32-bit ID of a 16-bit ID argument, as Linux's low2highuid: its low
 * 16 bits, the 16-bit -1 becoming the 32-bit one. */
static uint32_t id16_wide(unsigned long v)
{
    return (uint16_t)v == 0xFFFFu ? SC_ID_KEEP : (uint32_t)(uint16_t)v;
}

/** setfsuid (setfsuid32 on i386) takes a user ID and returns the previous
 * file-system user ID (`fsuid_set`). */
long sys_setfsuid(isr_params_t *params)
{
    return fsuid_set((uint32_t)SC_ARG(params, 0));
}

/** setfsgid (setfsgid32 on i386) takes a group ID and returns the previous
 * file-system group ID (`fsgid_set`). */
long sys_setfsgid(isr_params_t *params)
{
    return fsgid_set((uint32_t)SC_ARG(params, 0));
}

/** i386's setfsuid, call 138, with a 16-bit ID (`id16_wide`). The previous
 * ID comes back whole, not cut to 16 bits, as Linux's setfsuid16 returns
 * it. */
long sys_setfsuid16(isr_params_t *params)
{
    return fsuid_set(id16_wide(SC_ARG(params, 0)));
}

/** i386's setfsgid, call 139, as `sys_setfsuid16`. */
long sys_setfsgid16(isr_params_t *params)
{
    return fsgid_set(id16_wide(SC_ARG(params, 0)));
}

/** getresuid (getresuid32 on i686): the real, effective and saved user IDs, as
 * the caller's user namespace sees them, through the three user pointers;
 * -EFAULT. */
long sys_getresuid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t r = uid_seen(p->ruid), e = uid_seen(p->euid), s = uid_seen(p->suid);

    if (copy_to_user((void __user *)SC_ARG(params, 0), &r, sizeof(r)) != 0 ||
        copy_to_user((void __user *)SC_ARG(params, 1), &e, sizeof(e)) != 0 ||
        copy_to_user((void __user *)SC_ARG(params, 2), &s, sizeof(s)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** getresgid: getresuid's report of the group IDs. */
long sys_getresgid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t r = gid_seen(p->rgid), e = gid_seen(p->egid), s = gid_seen(p->sgid);

    if (copy_to_user((void __user *)SC_ARG(params, 0), &r, sizeof(r)) != 0 ||
        copy_to_user((void __user *)SC_ARG(params, 1), &e, sizeof(e)) != 0 ||
        copy_to_user((void __user *)SC_ARG(params, 2), &s, sizeof(s)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * setgroups (setgroups32 on i686) takes a count and a list of group IDs, at
 * most NGROUPS_MAX, as the calling thread's supplementary groups. -EINVAL for
 * a longer list or an ID the user namespace does not map, -EPERM for a caller
 * not privileged there (`id_privileged`) or one whose namespace denies
 * setgroups (`ns_setgroups_denied`), -EFAULT.
 */
long sys_setgroups(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t size = USER_LEN32(SC_ARG(params, 0));
    uintptr_t list_ptr = SC_ARG(params, 1);
    uint32_t tmp[NGROUPS_MAX];
    uint32_t i;

    if (size > NGROUPS_MAX) {
        return -EINVAL; /* sized before privilege, the order Linux reports */
    }
    if (!id_privileged() || ns_setgroups_denied(p)) {
        return -EPERM;
    }
    for (i = 0; i < size; i++) {
        if (copy_from_user(&tmp[i], (const void __user *)(list_ptr + i * 4), sizeof(tmp[i])) != 0) {
            return -EFAULT;
        }
    }
    if (ids_named(tmp, (int)size, 1) != 0) {
        return -EINVAL;
    }
    for (i = 0; i < size; i++) {
        p->groups[i] = tmp[i];
    }
    p->ngroups = size;
    return 0;
}

/**
 * getgroups (getgroups32 on i686) takes a count and a list to fill with the
 * supplementary groups, as the user namespace sees them, and returns their
 * number; a count of 0 returns the number alone, the POSIX sizing idiom.
 * -EINVAL for a negative count or one smaller than the number, -EFAULT.
 */
long sys_getgroups(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t size = USER_LEN32(SC_ARG(params, 0));
    uintptr_t list_ptr = SC_ARG(params, 1);
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
        uint32_t g = gid_seen(p->groups[i]);

        if (copy_to_user((void __user *)(list_ptr + i * 4), &g, sizeof(g)) != 0) {
            return -EFAULT;
        }
    }
    return (int32_t)p->ngroups;
}

/* The resources: nine are kept in the record and enforced, and the rest
 * are unlimited. */
#define RLIMIT_FSIZE_RES 1

#define RLIMIT_STACK_RES 3

#define RLIMIT_CORE_RES 4

#define RLIMIT_NPROC_RES 6

#define RLIMIT_NOFILE_RES 7

#define RLIMIT_MEMLOCK_RES 8

#define RLIMIT_AS_RES 9

#define RLIMIT_NICE_RES 13

#define RLIMIT_RTPRIO_RES 14

/* Linux's count of resources; a resource number at or past it is EINVAL. */
#define RLIM_NLIMITS 16u

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
    case RLIMIT_CORE_RES:
        return RLIM_CORE;
    case RLIMIT_FSIZE_RES:
        return RLIM_FSIZE;
    case RLIMIT_MEMLOCK_RES:
        return RLIM_MEMLOCK;
    case RLIMIT_NICE_RES:
        return RLIM_NICE;
    case RLIMIT_RTPRIO_RES:
        return RLIM_RTPRIO;
    default:
        return -1;
    }
}

/** The soft and hard limits of `resource` for `p`, RLIM_INFINITY for both of
 * one that is not kept. */
static void rlimit_for(const process_t *p, uint32_t resource, unsigned long *cur,
                       unsigned long *max)
{
    int slot = rlimit_slot(resource);

    if (slot >= 0) {
        *cur = p->rlim[slot].cur;
        *max = p->rlim[slot].max;
        return;
    }
    *cur = *max = RLIM_INFINITY;
}

/**
 * Set a kept limit of `p`'s process, in the record of each of its threads:
 * the soft value at most the hard, the hard raised by root only and lowered
 * by anyone, and RLIMIT_NOFILE never above `fd_nr_open`, fs.nr_open, as
 * Linux refuses it with EPERM even to root. A limit that is
 * not kept is accepted and not changed. RLIMIT_NOFILE is the process's own
 * even when it shares its descriptor table with another under CLONE_FILES,
 * since each allocation reads the caller's (`fd_limit`). 0, or -EINVAL and
 * -EPERM as POSIX names them.
 */
static int rlimit_set(process_t *p, uint32_t resource, unsigned long cur, unsigned long max)
{
    int slot = rlimit_slot(resource);
    process_t *t;

    if (slot < 0) {
        return 0;
    }
    if (cur > max) {
        return -EINVAL;
    }
    if (max > p->rlim[slot].max && !proc_privileged(current_proc)) {
        return -EPERM;
    }
    if (slot == RLIM_NOFILE && max > (unsigned long)fd_nr_open) {
        return -EPERM;
    }
    for (t = proc_list; t; t = t->next) {
        if (t->tg == p->tg) {
            t->rlim[slot].cur = cur;
            t->rlim[slot].max = max;
        }
    }
    return 0;
}

/**
 * Find the process `pid` names for a limit call and store it in `*out`: the
 * caller for 0 or its own pid; another process only to root or to a caller
 * of the same real uid. A zombie counts until it is reaped, since its record
 * keeps its limits and its thread group until then, as a Linux zombie keeps
 * its `signal_struct`. Returns 0, -ESRCH for a pid the caller's PID
 * namespace does not show or no record has, or -EPERM.
 */
static int rlimit_target(uint32_t pid, process_t **out)
{
    process_t *p;

    *out = current_proc;
    if (pid == 0) {
        return 0;
    }
    pid = pid_named(pid);
    if (pid == current_proc->tgid) {
        return 0;
    }
    p = pid != 0 ? get_process(pid) : NULL;
    if (!p || p->state == PROC_UNUSED) {
        return -ESRCH;
    }
    if (!proc_privileged(current_proc) && p->ruid != current_proc->ruid) {
        return -EPERM;
    }
    *out = p;
    return 0;
}

/** getrlimit/ugetrlimit: struct rlimit { unsigned long rlim_cur, rlim_max },
 * the record's own width, with RLIM_INFINITY all ones in either; -EINVAL
 * for a resource of RLIM_NLIMITS or more, then -EFAULT. */
long sys_getrlimit(isr_params_t *params)
{
    uint32_t resource = (uint32_t)SC_ARG(params, 0);
    uintptr_t rl = SC_ARG(params, 1);
    unsigned long out[2];

    if (resource >= RLIM_NLIMITS) {
        return -EINVAL;
    }
    if (!rl) {
        return -EFAULT;
    }
    rlimit_for(current_proc, resource, &out[0], &out[1]);
    if (copy_to_user((void __user *)rl, out, sizeof(out)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/* The old getrlimit's largest value, which an unlimited one reads as. */
#define OLD_RLIM_MAX 0x7FFFFFFFul

/**
 * The old getrlimit of i386, number 76: two 32-bit limits, each at most
 * 0x7FFFFFFF, unlimited among them, as Linux's old_getrlimit gives them, on
 * the i686 kernel and for an i386 caller on x86-64. -EINVAL for a resource
 * of RLIM_NLIMITS or more, -EFAULT.
 */
long sys_old_getrlimit(isr_params_t *params)
{
    uint32_t resource = (uint32_t)SC_ARG(params, 0);
    unsigned long cur, max;
    uint32_t out[2];

    if (resource >= RLIM_NLIMITS) {
        return -EINVAL;
    }
    rlimit_for(current_proc, resource, &cur, &max);
    out[0] = (uint32_t)(cur > OLD_RLIM_MAX ? OLD_RLIM_MAX : cur);
    out[1] = (uint32_t)(max > OLD_RLIM_MAX ? OLD_RLIM_MAX : max);
    if (copy_to_user((void __user *)SC_ARG(params, 1), out, sizeof(out)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** setrlimit: limits in the form getrlimit writes, set by `rlimit_set`;
 * -EFAULT, then -EINVAL for a resource of RLIM_NLIMITS or more. */
long sys_setrlimit(isr_params_t *params)
{
    uint32_t resource = (uint32_t)SC_ARG(params, 0);
    uintptr_t rl = SC_ARG(params, 1);
    unsigned long in[2];

    if (copy_from_user(in, (const void __user *)rl, sizeof(in)) != 0) {
        return -EFAULT;
    }
    if (resource >= RLIM_NLIMITS) {
        return -EINVAL;
    }
    return rlimit_set(current_proc, resource, in[0], in[1]);
}

/** A 64-bit limit as the record holds it: a value as large as the record's
 * RLIM_INFINITY or larger is no limit, as Linux's `rlim64_is_infinity`
 * reads it, so on i686 a value past 32 bits is kept as no limit. */
static unsigned long rlimit_from64(uint64_t v)
{
    return v >= (uint64_t)RLIM_INFINITY ? RLIM_INFINITY : (unsigned long)v;
}

/** The record's limit as prlimit64 gives it: RLIM_INFINITY as the 64-bit
 * one, all ones. */
static uint64_t rlimit_to64(unsigned long v)
{
    return v == RLIM_INFINITY ? ~(uint64_t)0 : (uint64_t)v;
}

/**
 * prlimit64 takes pid (0 for the caller), resource, new (rlimit64* or NULL)
 * and old (rlimit64* or NULL); a struct rlimit64 is two 64-bit words, cur and
 * max. The old limits are read before the new are set, as POSIX's `setrlimit`
 * and `getrlimit` see them, and written to the caller after: a copy that fails
 * leaves the new limits in place, as it does on Linux. The copies may sleep,
 * and the target may end while they do, so the new limits are read before the
 * target is looked up, and the old are written after its last use. Returns 0,
 * the error of `rlimit_target`, -EINVAL for a resource of RLIM_NLIMITS or
 * more, the error of `rlimit_set`, or -EFAULT. This is the
 * form musl issues for both.
 */
long sys_prlimit64(isr_params_t *params)
{
    uint32_t pid = (uint32_t)SC_ARG(params, 0);
    uint32_t resource = (uint32_t)SC_ARG(params, 1);
    uintptr_t newp = SC_ARG(params, 2);
    uintptr_t oldp = SC_ARG(params, 3);
    process_t *p;
    unsigned long cur, max;
    uint64_t in[2];
    uint64_t out[2]; /* cur, max */
    int rc = 0;

    if (newp && copy_from_user(in, (const void __user *)newp, sizeof(in)) != 0) {
        return -EFAULT;
    }
    rc = rlimit_target(pid, &p);
    if (rc != 0) {
        return rc;
    }
    if (resource >= RLIM_NLIMITS) {
        return -EINVAL;
    }
    rlimit_for(p, resource, &cur, &max);
    out[0] = rlimit_to64(cur);
    out[1] = rlimit_to64(max);
    if (newp) {
        rc = rlimit_set(p, resource, rlimit_from64(in[0]), rlimit_from64(in[1]));
    }
    if (oldp && copy_to_user((void __user *)oldp, out, sizeof(out)) != 0) {
        return -EFAULT;
    }
    return rc;
}

/*
 * The protection bits and flags of mmap, mmap2 and mprotect, Linux's values
 * on i386 and x86-64 alike. The PROT_* bits become VMA permissions
 * (`vm_prot_of`).
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

#define MAP_32BIT 0x40 /* x86-64: below 2 GiB; i386 has every address so */

/* Where MAP_32BIT looks for room, Linux's 1 GiB to 2 GiB, which leaves the
 * first gigabyte to a program's own image and heap. */
#define MAP_32BIT_LO 0x40000000UL
#define MAP_32BIT_HI 0x80000000UL

#define MAP_LOCKED 0x2000

#define MAP_POPULATE 0x8000

#define MAP_NONBLOCK 0x10000 /* turns MAP_POPULATE off, as on Linux */

#define MAP_FIXED_NOREPLACE 0x100000

/* Round 'v' up to the next page boundary. */
#define PAGE_UP(v) (((v) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

/** Whether the caller may lock memory at all, Linux's can_do_mlock: its
 * soft RLIMIT_MEMLOCK is above 0, or it is privileged, which has
 * CAP_IPC_LOCK. */
static int mlock_allowed(void)
{
    return current_proc->rlim[RLIM_MEMLOCK].cur != 0 || proc_privileged(current_proc);
}

/**
 * Set the program break, the heap's top (brk(2)). Its argument is the
 * requested break. Returns the resulting break: the new value on success, or
 * the unchanged current break if the request is below the heap's start or into
 * the stack's growth window, if growing would meet another region or pass
 * RLIMIT_AS, or if there is no room for a region. brk(0) is below the heap's
 * start, so it reads the break. This is the contract musl's allocator relies
 * on when it issues SYS_brk (it compares the return to its request; musl's
 * public brk() never issues the call). The heap is the demand-paged range
 * [heap_start, page_up(brk)), which mprotect may have cut into regions of
 * different protection: growing adds a read-write region at the top, which
 * merges with a read-write top piece, and shrinking removes the range above
 * the new top and frees its pages. Under mlockall(MCL_FUTURE) the growth is
 * locked and counts against RLIMIT_MEMLOCK, past which the break stays, and
 * its pages are brought in unless MCL_ONFAULT was given, as on Linux.
 */
long sys_brk(isr_params_t *params)
{
    uintptr_t req = SC_ARG(params, 0);
    vm_space_t *vm;
    uintptr_t old_top, new_top;

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    mem_balance();
    vm = &current_proc->mm->vm;

    if (req < vm->heap_start || req > vm->stack_limit) {
        return (long)vm->brk;
    }

    old_top = PAGE_UP(vm->brk);
    new_top = PAGE_UP(req);

    if (new_top > old_top) {
        if (vm_overlaps(vm, old_top, new_top)) {
            return (long)vm->brk;
        }
        if (vm_total(vm) + (new_top - old_top) > current_proc->rlim[RLIM_AS].cur) {
            return (long)vm->brk; /* RLIMIT_AS: brk reports the break unchanged */
        }
        if ((vm->def_prot & VM_PROT_LOCKED) &&
            !mem_lock_room(vm_locked(vm) / PAGE_SIZE, (new_top - old_top) / PAGE_SIZE)) {
            return (long)vm->brk;
        }
        if (vm_insert(vm, old_top, new_top, VM_PROT_READ | VM_PROT_WRITE | vm->def_prot, VM_HEAP) !=
            0) {
            return (long)vm->brk;
        }
        if ((vm->def_prot & VM_PROT_LOCKS) == VM_PROT_LOCKED) {
            vm->brk = req;
            (void)vm_populate(old_top, new_top);
            return (long)req;
        }
    } else if (new_top < old_top) {
        if (vm_sealed_at(vm, new_top, old_top) < old_top || vm_remove(vm, new_top, old_top) != 0) {
            return (long)vm->brk;
        }
        mmn_notify(current_proc->mm, MMN_UNMAP, new_top, old_top);
        unmap_user_range(_curr_dir, new_top, old_top);
    }

    vm->brk = req;
    vm_file_reap();
    return (long)req;
}

/**
 * Log that the current process ran out of mappings in the call `what`, once
 * per process.
 *
 * `vm.c` uses none of the kernel's globals, so it cannot say which process
 * ran out; here the caller is known. Without the line, a program whose
 * library ran out of mappings looks like a program whose library failed for
 * its own reasons. It is logged once per process, because a program that
 * meets the limit meets it in a loop and a flood of lines would bury the
 * first one. The mark is in the process record, since a pid is reused, and
 * the next process to have that number would go unreported.
 */
static void vma_exhausted(const char *what)
{
    if (!current_proc || current_proc->vma_reported) {
        return;
    }
    current_proc->vma_reported = 1;
    klog(KLOG_WARN, "vm", "%s: pid %d (%s) has used all %d of its mappings", what,
         (int)current_proc->pid, current_proc->comm, vm_max_map_count);
}

/* pkey_alloc's access rights, PKEY_DISABLE_ACCESS and PKEY_DISABLE_WRITE. */
#define PKEY_DISABLE_ACCESS 1u
#define PKEY_DISABLE_WRITE  2u
#define PKEY_ACCESS_MASK    3u

/** Give the key `key` the rights `rights`, of PKEY_ACCESS_MASK, in the
 * calling thread's PKRU, keeping every other key's, as Linux's
 * arch_set_user_pkey_access does. */
static void pkey_rights(int key, uint32_t rights)
{
    uint32_t pkru = fpu_pkru() & ~(PKRU_AD(key) | PKRU_WD(key));

    if (rights & PKEY_DISABLE_ACCESS) {
        pkru |= PKRU_AD(key);
    }
    if (rights & PKEY_DISABLE_WRITE) {
        pkru |= PKRU_WD(key);
    }
    fpu_pkru_set(pkru);
}

/** Allocate the lowest free protection key of `mm`, as Linux's
 * mm_pkey_alloc: the key, or -1 when the keys are off or all sixteen are
 * taken. */
static int pkey_take(struct mm *mm)
{
    int key;

    if (!fpu_pkeys()) {
        return -1;
    }
    for (key = 0; key < FPU_PKEYS; key++) {
        if (!(mm->pkeys & (1u << key))) {
            mm->pkeys |= 1u << key;
            return key;
        }
    }
    return -1;
}

/** Whether a program may name the key `key` of `mm`, as Linux's
 * mm_pkey_is_allocated: one it allocated, or the default key 0 until it
 * frees it, but never the execute-only key; none with the keys off. */
static int pkey_named(const struct mm *mm, int key)
{
    return fpu_pkeys() && key >= 0 && key < FPU_PKEYS && key != mm->pkey_exec &&
           (mm->pkeys & (1u << key)) != 0;
}

/**
 * The key of `mm` for a mapping made execute-only, by mmap or mprotect with
 * PROT_EXEC alone, as Linux's __execute_only_pkey: allocated the first
 * time, and then denied every data access in the calling thread's PKRU,
 * as it is again whenever the thread allows it reads. The pages can then
 * be run and not read. -1 with the keys off or no key free; the mapping
 * then keeps the key it would have had.
 */
static int pkey_exec_only(struct mm *mm)
{
    int key = mm->pkey_exec;

    if (key < 0) {
        key = pkey_take(mm);
        if (key < 0) {
            return -1;
        }
        mm->pkey_exec = key;
    } else if (fpu_pkru() & PKRU_AD(key)) {
        return key;
    }
    pkey_rights(key, PKEY_DISABLE_ACCESS);
    return key;
}

/** The protection key bits of a new mapping of the mmap `prot`: the
 * execute-only key for PROT_EXEC alone where there is one, as Linux's
 * do_mmap takes it, and key 0 otherwise. */
static uint32_t mmap_key(uint32_t prot)
{
    int key = prot == PROT_EXEC ? pkey_exec_only(current_proc->mm) : -1;

    return key > 0 ? (uint32_t)key << VM_PROT_PKEY_SHIFT : 0u;
}

/** The VMA protection of an mmap or mprotect `prot`. Write and execute each
 * grant read, which x86 paging cannot withhold from them. */
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

/** A copy in the heap of the path /proc/PID/fd would give the current
 * process's descriptor `fd`, for /proc/PID/maps to name the file a region
 * maps; NULL when there is no room or no path. Rebuilding the path may
 * sleep in the disk. */
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

/* What a new mapping maps, which `mmap_backing` finds: the file or shared
 * memory, NULL for private anonymous memory; the region kind; the file's
 * page at the region's start; and the region's protection. */
struct map_source {
    struct vm_file *file;
    vm_type_t type;
    uint32_t pgoff;
    uint32_t prot;
};

/** 0 when the descriptor `e` may be mapped, shared or not, with the
 * protection `prot`: ENODEV for what is neither a file nor an object whose
 * operations map it (`file_ops.mmap`), and EACCES for a descriptor not open
 * for reading, or for a writable shared mapping, not open for writing. */
static int32_t mmap_file_check(const fd_entry_t *e, int shared, uint32_t prot)
{
    if (!(e->ops && e->ops->mmap) && (e->type != FD_TYPE_FILE || !e->ofd || !e->ofd->vmnt)) {
        return -ENODEV;
    }
    if (!(e->flags & FD_FLAG_READ) ||
        (shared && (prot & VM_PROT_WRITE) && !(e->flags & FD_FLAG_WRITE))) {
        return -EACCES;
    }
    return 0;
}

/**
 * The rules of a mapping through `e` of a file whose memory is `sh`, when
 * it is a file of memfd_secret: shared alone, else -EINVAL, and never
 * executable, else -EPERM, as Linux's noexec mount of the files answers
 * first; the region gets VM_PROT_SECRET in `src`. 0 or the error; 0 for
 * any other file.
 */
static int32_t mmap_secret(const fd_entry_t *e, struct shmem *sh, int shared,
                           struct map_source *src)
{
    if (!sh || !tmpfs_secret(e->ofd->vnode)) {
        return 0;
    }
    if (src->prot & VM_PROT_EXEC) {
        return -EPERM;
    }
    if (!shared) {
        return -EINVAL;
    }
    src->prot |= VM_PROT_SECRET;
    return 0;
}

/**
 * The file half of `mmap_backing`: the file the descriptor `e`, number `fd`, is
 * open on, mapped shared or not. A private mapping of it is VM_FILE, and a
 * shared mapping of a file in memory is VM_SHARED, the file's own pages; a
 * descriptor that fails `mmap_file_check` is not mapped. A shared mapping of a
 * file on ext2 is VM_SHARED too, the file's pages in memory, which its stores
 * are written back from (`filemap_disk_shared`):
 * ext2's inode number names one file for as long as anything holds it, which
 * is what the pages are found by. Private tmpfs and ext2 mappings borrow
 * those same pages until their first store makes a copy. A shared mapping
 * through a descriptor open for writing of a file that backs a loop device
 * fails with EBUSY, as write(2)
 * to the file does. A shared mapping of a file of another disk file system,
 * whose inode numbers follow names, is served as a private one when it is
 * read-only, which reads the same bytes, and fails with ENODEV when it may be
 * written. A shared mapping served as a private one, or made through a
 * descriptor not open for writing, is given VM_PROT_NOWRITE, so that mprotect
 * cannot make it writable later. A file of memfd_secret maps as
 * `mmap_secret` says. It may sleep, finding the file's path.
 */
static int32_t mmap_file(fd_entry_t *e, int fd, int shared, struct map_source *src)
{
    struct shmem *sh;
    int disk;
    int32_t rc = mmap_file_check(e, shared, src->prot);

    if (rc != 0) {
        return rc;
    }
    sh = (e->ofd->vmnt->type == &vfs_tmpfs_type) ? tmpfs_shmem(e->ofd->vnode) : NULL;
    rc = mmap_secret(e, sh, shared, src);
    if (rc != 0) {
        return rc;
    }
    disk = e->ofd->vmnt->type == &vfs_ext2_type;
    if (shared && !sh && !disk && (src->prot & VM_PROT_WRITE)) {
        return -ENODEV;
    }
    if (shared && disk && (e->flags & FD_FLAG_WRITE) && loop_backs(e->ofd->vmnt, e->ofd->vnode)) {
        return -EBUSY;
    }
    if (shared && (!(e->flags & FD_FLAG_WRITE) || (!sh && !disk))) {
        src->prot |= VM_PROT_NOWRITE;
    }
    src->file = !shared ? filemap_private(e, sh, disk)
                : sh    ? filemap_shared(e, sh)
                : disk  ? filemap_disk_shared(e)
                        : filemap_new(e);
    src->type = shared && (sh || disk) ? VM_SHARED : VM_FILE;
    if (src->file) {
        src->file->path = fd_path_copy(fd);
    }
    return src->file ? 0 : -ENOMEM;
}

/**
 * The half of `mmap_backing` for a descriptor `e`, number `fd`, whose object
 * maps itself (`file_ops.mmap`): a region of `len` bytes with the protection
 * and page offset in `src`, VM_SHARED for a shared mapping and VM_FILE for a
 * private one, is given to the operation, which sets its file, kind and
 * protection, and those go into `src`. A shared mapping through a
 * descriptor not open for writing is given VM_PROT_NOWRITE, as `mmap_file`
 * gives it. The region the call builds is not in the address space, so
 * its start is 0 and its end the length. The operation's error, or
 * `mmap_file_check`'s; ENOMEM with no memory for the path.
 */
static int32_t mmap_object(fd_entry_t *e, int fd, int shared, uintptr_t len, struct map_source *src)
{
    vma_t vma;
    int32_t rc = mmap_file_check(e, shared, src->prot);

    if (rc != 0) {
        return rc;
    }
    k_memset(&vma, 0, sizeof(vma));
    vma.end = len;
    vma.prot = src->prot | ((shared && !(e->flags & FD_FLAG_WRITE)) ? VM_PROT_NOWRITE : 0u);
    vma.type = shared ? VM_SHARED : VM_FILE;
    vma.pgoff = src->pgoff;
    rc = e->ops->mmap(e, &vma);
    if (rc != 0) {
        return rc;
    }
    src->file = vma.file;
    src->type = vma.type;
    src->prot = vma.prot;
    src->pgoff = vma.pgoff;
    if (src->file && !src->file->path) {
        src->file->path = fd_path_copy(fd);
    }
    return 0;
}

/**
 * The file or shared memory a new mapping of `len` bytes maps, with the
 * region kind it makes, into `src`, whose protection and page offset the
 * caller set from its arguments: an object that maps itself, a device's or
 * a driver's (`mmap_object`), a mapping of a file (`mmap_file`), shared
 * anonymous memory (VM_SHARED), or NULL for private anonymous memory
 * (VM_ANON). /dev/zero maps as anonymous memory.
 * Anonymous memory has its pages from its own first on, so its offset is
 * set to 0, as Linux ignores it. It may sleep, finding a file's path.
 */
static int32_t mmap_backing(uint32_t flags, int fd, uintptr_t len, struct map_source *src)
{
    int shared = (flags & MAP_TYPE) != MAP_PRIVATE;
    fd_entry_t *e;

    src->file = NULL;
    src->type = VM_ANON;
    if (!(flags & MAP_ANONYMOUS)) {
        e = fd_get(current_proc->fds, fd);
        if (!e) {
            return -EBADF;
        }
        if (e->ops && e->ops->mmap) {
            return mmap_object(e, fd, shared, len, src);
        }
        if (devfs_rdev(e) != ((1u << 8) | 5u)) { /* /dev/zero, 1:5, maps as anonymous memory */
            return mmap_file(e, fd, shared, src);
        }
    }
    src->pgoff = 0;
    if (shared) {
        src->file = filemap_anon_shared();
        src->type = VM_SHARED;
        return src->file ? 0 : -ENOMEM;
    }
    return 0;
}

/**
 * The end of the calling process's address space, Linux's TASK_SIZE:
 * IA32_TASK_SIZE for an i386 program on x86-64, whose every mapping lies
 * below 4 GiB, and USER_ADDR_MAX for any other.
 */
static uintptr_t task_size(void)
{
#if ELF_IA32
    if (current_proc->mm->compat) {
        return IA32_TASK_SIZE;
    }
#endif
    return USER_ADDR_MAX;
}

/**
 * The highest free range of `vm` that holds `len` bytes, for a mapping
 * without a usable hint, or 0: below the mmap base, down to the bottom of
 * user space whatever the heap's place, as Linux's top-down search goes, and
 * when that has no room, as Linux then falls back to its bottom-up search,
 * over the whole range below the stack's growth window and its guard gap.
 * In an i386 call the mmap base is at most IA32_MMAP_BASE, Linux's
 * mmap_compat_base, and the
 * range ends by IA32_TASK_SIZE, so that the call's 32-bit result holds the
 * address. The search goes below the heap's top: an i386 Wine reserves
 * nearly all that lies above its loader's heap until its program starts.
 */
static uintptr_t mmap_free(vm_space_t *vm, uintptr_t len)
{
    uintptr_t hi = vm->mmap_base;
    uintptr_t end = vm->stack_limit > VM_STACK_GUARD_GAP ? vm->stack_limit - VM_STACK_GUARD_GAP : 0;

#if ELF_IA32
    if (in_compat_syscall()) {
        hi = hi < IA32_MMAP_BASE ? hi : IA32_MMAP_BASE;
        end = end < IA32_TASK_SIZE ? end : IA32_TASK_SIZE;
    }
#endif
    return vm_find_mmap(vm, len, USER_ADDR_MIN, hi, end);
}

/**
 * Where a new mapping of `len` bytes goes, in `*base`: `addr` itself under
 * MAP_FIXED or MAP_FIXED_NOREPLACE, which fails with EEXIST for a range in
 * use and with ENOMEM for one past the address space's end (`task_size`);
 * otherwise the hint rounded down to its page when that range is free
 * below the stack's growth window and guard gap, and without one the range
 * `mmap_free` finds, so the ranges munmap frees are used again. MAP_32BIT in
 * a 64-bit call, as on Linux, rounds the hint up, takes it only when its
 * range ends by 2 GiB, and otherwise takes the highest free range from
 * 1 GiB to 2 GiB; a 32-bit call ignores it, its every address being below
 * 4 GiB. RLIMIT_AS counts the range, less what a fixed mapping replaces. 0,
 * -ENOMEM or -EEXIST. Nothing here sleeps, so the range is still as it was
 * found when the caller inserts the region straight after.
 */
static int32_t mmap_place(vm_space_t *vm, uintptr_t addr, uintptr_t len, uint32_t flags,
                          uintptr_t *base)
{
    unsigned long limit = current_proc->rlim[RLIM_AS].cur;
    uintptr_t top = vm->stack_limit > VM_STACK_GUARD_GAP ? vm->stack_limit - VM_STACK_GUARD_GAP : 0;
    int low = 0;

    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        if (addr < USER_ADDR_MIN || len > task_size() || addr > task_size() - len) {
            return -ENOMEM;
        }
        if ((flags & MAP_FIXED_NOREPLACE) && vm_overlaps(vm, addr, addr + len)) {
            return -EEXIST;
        }
        *base = addr;
        return vm_total(vm) - vm_covered(vm, addr, addr + len) + len > limit ? -ENOMEM : 0;
    }
    if ((flags & MAP_32BIT) && sizeof(void *) > 4u && !in_compat_syscall()) {
        low = 1;
        top = top < MAP_32BIT_HI ? top : MAP_32BIT_HI;
        addr = PAGE_UP(addr);
    }
    *base = 0;
    addr &= ~(uintptr_t)(PAGE_SIZE - 1);
    if (addr != 0 && addr >= USER_ADDR_MIN && len <= top && addr <= top - len &&
        !vm_overlaps(vm, addr, addr + len)) {
        *base = addr;
    }
    if (*base == 0) {
        *base = low ? vm_find_free(vm, len, MAP_32BIT_LO, MAP_32BIT_HI) : mmap_free(vm, len);
    }
    return *base == 0 || vm_total(vm) + len > limit ? -ENOMEM : 0;
}

/**
 * Put the region `src` describes, `len` bytes, where `mmap_place` finds for
 * `addr` and `flags`, replacing what a MAP_FIXED range held, or failing with
 * EPERM when a region of that range is sealed (mseal). The region takes its
 * own reference to the file, and the caller's is put here. Returns the
 * address, or -errno.
 * Under MAP_FIXED the two entries that a split in the removal and the new
 * region take are reserved before the old mapping is removed, so that a
 * failure leaves the old mapping in place.
 */
static long mmap_install(uintptr_t addr, uintptr_t len, uint32_t flags, struct map_source *src)
{
    vm_space_t *vm = &current_proc->mm->vm;
    uintptr_t base = 0;
    int32_t rc = mmap_place(vm, addr, len, flags, &base);

    if (rc == 0 && (flags & MAP_FIXED) && vm_sealed_at(vm, base, base + len) < base + len) {
        rc = -EPERM;
    }
    if (rc == 0 && (flags & MAP_FIXED)) {
        if (vm_reserve(vm, 2) == 0) {
            vm_remove(vm, base, base + len);
            mmn_notify(current_proc->mm, MMN_UNMAP, base, base + len);
            unmap_user_range(_curr_dir, base, base + len);
        } else {
            rc = -ENOMEM;
            vma_exhausted("mmap");
        }
    }
    if (rc == 0 &&
        vm_insert_file(vm, base, base + len, src->prot, src->type, src->file, src->pgoff) != 0) {
        rc = -ENOMEM;
        vma_exhausted("mmap");
    }
    vm_file_put(src->file);
    vm_file_reap();
    return rc == 0 ? (long)base : rc;
}

/**
 * The core of mmap(2) and mmap2(2): map memory. Its arguments are addr,
 * length, prot, flags, fd and pgoff, the offset in the file in pages, which
 * anonymous memory ignores. The offset and the length in pages must sum to a
 * value an unsigned long holds, as Linux has it, whatever is mapped, and for
 * a file or shared memory, whose pages the kernel numbers in 32 bits, to less
 * than 2^32, which on i686 is the same bound; else EOVERFLOW. Private
 * anonymous memory on x86-64 has only the first bound. Returns the mapped
 * base address, or -errno.
 *
 * What is mapped is `mmap_backing`'s: anonymous memory, private or shared, a
 * file, private or shared, or an object that maps itself, such as /dev/mem;
 * where it goes is `mmap_place`'s. The backing is found first, since
 * finding it may sleep, and the place after, so
 * that nothing sleeps between the checks on the range and the insertion: a
 * thread sharing the address space cannot map the range meanwhile, and
 * MAP_FIXED_NOREPLACE cannot replace what it put there. MAP_FIXED replaces
 * whatever the range held. Pages are backed on first touch: zeroed, read from
 * the file, or the shared page itself; with MAP_POPULATE, unless
 * MAP_NONBLOCK is given too, or with MAP_LOCKED, they are backed before the
 * call returns, as far as memory allows (`vm_populate`); a device's pages
 * are always backed on first touch.
 *
 * A mapping with PROT_EXEC alone takes the execute-only protection key, which
 * the calling thread may run and not read, where the keys are on
 * (`pkey_exec_only`); any other has key 0.
 *
 * MAP_LOCKED locks the mapping, as mlock does, and so does mlockall's
 * MCL_FUTURE, with its MCL_ONFAULT; a device's mapping is never locked.
 * MAP_LOCKED is EPERM for a process without privilege whose RLIMIT_MEMLOCK
 * is 0, and a locked mapping that would take the locked pages past the
 * limit is EAGAIN, as on Linux, which counts the new length whatever a
 * MAP_FIXED range replaces.
 */
long ksys_mmap_pgoff(uintptr_t addr, uintptr_t length, uint32_t prot, uint32_t flags, int fd,
                     unsigned long pgoff)
{
    struct map_source src;
    uintptr_t len;
    uint32_t locks;
    int32_t rc;
    long base;

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    if (length == 0 || (prot & ~(uint32_t)(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) {
        return -EINVAL;
    }
    if ((flags & MAP_TYPE) != MAP_SHARED && (flags & MAP_TYPE) != MAP_PRIVATE &&
        (flags & MAP_TYPE) != MAP_SHARED_VALIDATE) {
        return -EINVAL;
    }
    if (length > task_size() - USER_ADDR_MIN) {
        return -ENOMEM;
    }
    len = PAGE_UP(length);
    if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && (addr & (PAGE_SIZE - 1)) != 0) {
        return -EINVAL;
    }
    if (pgoff + len / PAGE_SIZE < pgoff ||
        (((flags & MAP_TYPE) != MAP_PRIVATE || !(flags & MAP_ANONYMOUS)) &&
         pgoff + len / PAGE_SIZE > 0xFFFFFFFFul)) {
        return -EOVERFLOW;
    }
    locks = current_proc->mm->vm.def_prot;
    if (flags & MAP_LOCKED) {
        if (!mlock_allowed()) {
            return -EPERM;
        }
        locks = VM_PROT_LOCKED;
    }
    if ((locks & VM_PROT_LOCKED) &&
        !mem_lock_room(vm_locked(&current_proc->mm->vm) / PAGE_SIZE, len / PAGE_SIZE)) {
        return -EAGAIN;
    }
    mem_balance();
    src.prot = vm_prot_of(prot) | mmap_key(prot);
    src.pgoff = (uint32_t)pgoff;
    rc = mmap_backing(flags, fd, len, &src);
    if (rc != 0) {
        return rc;
    }
    if (!(src.prot & VM_PROT_IO)) {
        src.prot |= locks;
    }
    base = mmap_install(addr, len, flags, &src);
    if ((unsigned long)base < (unsigned long)-4095L &&
        ((locks & VM_PROT_LOCKS) == VM_PROT_LOCKED ||
         (flags & (MAP_POPULATE | MAP_NONBLOCK)) == MAP_POPULATE)) {
        (void)vm_populate((uintptr_t)base, (uintptr_t)base + len);
    }
    return base;
}

/** mmap2(2), on i686: addr, length, prot, flags, fd and the offset in pages. */
long sys_mmap2(isr_params_t *params)
{
    return ksys_mmap_pgoff(SC_ARG(params, 0), SC_ARG(params, 1), (uint32_t)SC_ARG(params, 2),
                           (uint32_t)SC_ARG(params, 3), (int)SC_ARG(params, 4), SC_ARG(params, 5));
}

/** mmap(2), on x86-64: mmap2's arguments with the offset in bytes, a multiple
 * of the page, or EINVAL. */
long sys_mmap(isr_params_t *params)
{
    if (SC_ARG(params, 5) & (PAGE_SIZE - 1u)) {
        return -EINVAL;
    }
    return ksys_mmap_pgoff(SC_ARG(params, 0), SC_ARG(params, 1), (uint32_t)SC_ARG(params, 2),
                           (uint32_t)SC_ARG(params, 3), (int)SC_ARG(params, 4),
                           SC_ARG(params, 5) >> PAGE_SHIFT);
}

/** The segment is mapped as a shared mapping of its tmpfs file, through a
 * descriptor made here and open for reading and writing; `access` limits the
 * region instead. */
long mmap_shm(struct open_file *ofd, uintptr_t addr, uintptr_t len, uint32_t access, int place,
              char *path)
{
    static const uint32_t places[] = {0, MAP_FIXED_NOREPLACE, MAP_FIXED};
    uint32_t prot = PROT_READ | ((access & SHM_MAP_WRITE) ? PROT_WRITE : 0u) |
                    ((access & SHM_MAP_EXEC) ? PROT_EXEC : 0u);
    struct map_source src;
    fd_entry_t e;

    if (!current_proc || !current_proc->mm) {
        kfree(path);
        return -ESRCH;
    }
    k_memset(&e, 0, sizeof(e));
    e.type = FD_TYPE_FILE;
    e.ofd = ofd;
    e.flags = FD_FLAG_READ | FD_FLAG_WRITE;
    src.prot = vm_prot_of(prot) | ((prot & PROT_WRITE) ? 0u : VM_PROT_NOWRITE);
    src.pgoff = 0;
    src.type = VM_SHARED;
    src.file = filemap_shared(&e, tmpfs_shmem(ofd->vnode));
    if (!src.file) {
        kfree(path);
        return -ENOMEM;
    }
    src.file->path = path;
    mem_balance();
    return mmap_install(addr, PAGE_UP(len), places[place], &src);
}

long mmap_device(uintptr_t len, uint32_t prot, int32_t (*map)(vma_t *vma, void *arg), void *arg)
{
    struct map_source src;
    vma_t vma;
    int32_t rc;

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    if (len == 0 || len > task_size()) {
        return len == 0 ? -EINVAL : -ENOMEM;
    }
    k_memset(&vma, 0, sizeof(vma));
    vma.end = PAGE_UP(len);
    vma.prot = prot;
    vma.type = VM_SHARED;
    rc = map(&vma, arg);
    if (rc != 0) {
        return rc;
    }
    src.file = vma.file;
    src.type = vma.type;
    src.prot = vma.prot;
    src.pgoff = vma.pgoff;
    mem_balance();
    return mmap_install(0, PAGE_UP(len), MAP_SHARED, &src);
}

int32_t munmap_shm(uintptr_t addr, const struct shmem *sh, size_t size)
{
    vm_space_t *vm = &current_proc->mm->vm;
    vma_t *r = vm_find(vm, addr);
    uintptr_t end = addr;
    int32_t rc = -EPERM;

    if (!r || r->start != addr || r->pgoff != 0 || filemap_shmem(r->file) != sh) {
        return -EINVAL;
    }
    while (r && r->start == end && filemap_shmem(r->file) == sh && end - addr < PAGE_UP(size)) {
        end = r->end;
        r = vm_find(vm, end);
    }
    while (addr < end) {
        uintptr_t next = vm_find(vm, addr)->end;

        if (vm_find(vm, addr)->prot & VM_PROT_SEALED) {
            addr = next; /* kept, as Linux's shmdt ignores the unmap's EPERM */
            continue;
        }
        if (vm_remove(vm, addr, next) != 0) {
            vma_exhausted("shmdt");
            rc = rc == 0 ? 0 : -ENOMEM; /* 0 once a region went */
            break;
        }
        mmn_notify(current_proc->mm, MMN_UNMAP, addr, next);
        unmap_user_range(_curr_dir, addr, next);
        rc = 0;
        addr = next;
    }
    vm_file_reap();
    return rc;
}

/** An address space is counted at the first process of `proc_list` that has
 * it. */
uint32_t shm_attaches(const struct shmem *sh)
{
    const process_t *p, *q;
    uint32_t n = 0;
    const vma_t *r;

    for (p = proc_list; p; p = p->next) {
        for (q = proc_list; p->mm && q != p && q->mm != p->mm; q = q->next) {
        }
        for (r = p->mm && q == p ? vm_first(&p->mm->vm) : NULL; r; r = vm_next(r)) {
            n += r->pgoff == 0 && filemap_shmem(r->file) == sh;
        }
    }
    return n;
}

/**
 * Unmap a range (munmap(2)). Its arguments are addr (page-aligned) and
 * length. Drops the VMA bookkeeping (trimming or splitting partial overlaps)
 * and frees the backing frames; unmapping never-faulted or never-mapped pages
 * is a no-op, as on Linux. Returns 0; -EINVAL for a zero length, an unaligned
 * address, or a range that leaves user space or wraps, which is judged before
 * the end is rounded up so that the rounding cannot wrap to a smaller end;
 * -EPERM, with nothing unmapped, when a region of the range is sealed
 * (mseal); -ENOMEM when a split finds no room for its region.
 */
long sys_munmap(isr_params_t *params)
{
    uintptr_t addr = SC_ARG(params, 0);
    uintptr_t length = SC_ARG(params, 1);
    vm_space_t *vm;
    uintptr_t start, end;

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    vm = &current_proc->mm->vm;

    if (length == 0 || (addr & (PAGE_SIZE - 1)) != 0) {
        return -EINVAL;
    }
    if (addr >= task_size() || length > task_size() - addr) {
        return -EINVAL;
    }

    start = addr;
    end = PAGE_UP(addr + length);

    if (vm_sealed_at(vm, start, end) < end) {
        return -EPERM;
    }
    if (vm_remove(vm, start, end) != 0) {
        vma_exhausted("munmap");
        return -ENOMEM;
    }
    mmn_notify(current_proc->mm, MMN_UNMAP, start, end);
    unmap_user_range(_curr_dir, start, end);
    vm_file_reap();
    return 0;
}

/** Bring the resident pages of [start, end) to their regions' protection,
 * `prot` where no region holds them, region by region, so that a shared
 * region's pages stay shared and each page takes its region's key. */
static void protect_resident(uintptr_t start, uintptr_t end, uint32_t prot)
{
    while (start < end) {
        vma_t *r = vm_find(&current_proc->mm->vm, start);
        uintptr_t stop = (r && r->end < end) ? r->end : end;

        paging_protect_range(_curr_dir, start, stop,
                             r ? r->prot | (r->type == VM_SHARED ? VM_PROT_SHARED : 0u) : prot);
        start = stop;
    }
}

/** The checks of `mprotect_key` before it looks at the regions, in Linux's
 * order: 1 to go on, 0 for an empty range, or -errno. */
static long mprotect_refused(const struct mm *mm, uintptr_t addr, uintptr_t length, uint32_t prot,
                             int key)
{
    if ((addr & (PAGE_SIZE - 1)) != 0) {
        return -EINVAL;
    }
    if (length == 0) {
        return 0;
    }
    if (PAGE_UP(length) == 0 || addr + PAGE_UP(length) <= addr) {
        return -ENOMEM;
    }
    if ((prot & ~(uint32_t)(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0 ||
        (key != -1 && !pkey_named(mm, key))) {
        return -EINVAL;
    }
    if (addr < USER_ADDR_MIN || addr >= task_size() || length > task_size() - addr) {
        return -ENOMEM;
    }
    return 1;
}

/**
 * Change the protection of a range under the protection key `key`, or -1
 * for mprotect's: the core of mprotect(2) and pkey_mprotect(2). Its
 * arguments are addr (page aligned), length and prot. The checks run in
 * Linux's order: EINVAL for an unaligned address, 0 for an empty range,
 * ENOMEM for one that wraps, EINVAL for an unknown protection bit or a
 * key the caller may not name (`pkey_named`), and ENOMEM when a page of
 * the range is not mapped or a split finds no room for its region. EACCES
 * for write asked of a shared mapping that may never be written (the
 * VM_PROT_NOWRITE `mmap_file` and `mmap_shm` give) or for execute asked of
 * a mapping of memfd_secret, with nothing changed. A sealed region
 * (mseal) is EPERM, and the regions below it in the range take the new
 * protection first, as Linux changes region by region and stops at the
 * sealed one. The regions take the new protection first and the resident
 * pages after, so a fault in between reads the protection the call is
 * setting.
 *
 * The regions take `key`; with -1, PROT_EXEC alone takes the execute-only
 * key where the keys are on (`pkey_exec_only`), any other protection moves
 * a region of that key back to key 0, and every other region keeps its
 * own, as Linux's arch_override_mprotect_pkey chooses.
 */
static long mprotect_key(isr_params_t *params, int key)
{
    uintptr_t addr = SC_ARG(params, 0);
    uintptr_t length = SC_ARG(params, 1);
    uint32_t prot = (uint32_t)SC_ARG(params, 2);
    uintptr_t end, sealed;
    uint32_t vmprot;
    int exec_key = -1;
    struct mm *mm;
    long rc;

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    mm = current_proc->mm;
    rc = mprotect_refused(mm, addr, length, prot, key);
    if (rc <= 0) {
        return rc;
    }
    end = PAGE_UP(addr + length);
    vmprot = vm_prot_of(prot);
    if (key == -1 && prot == PROT_EXEC) {
        int xo = pkey_exec_only(mm);

        key = xo > 0 ? xo : -1;
    } else if (key == -1) {
        exec_key = mm->pkey_exec;
    }
    sealed = vm_sealed_at(&mm->vm, addr, end);
    rc = vm_protect_key(&mm->vm, addr, sealed, vmprot, key, exec_key);
    if (rc == 0 && sealed < end) {
        const vma_t *r = vm_find(&mm->vm, sealed);

        rc = ((vmprot & VM_PROT_WRITE) && (r->prot & VM_PROT_NOWRITE)) ||
                     ((vmprot & VM_PROT_EXEC) && (r->prot & VM_PROT_SECRET))
                 ? -3
                 : -4;
    }
    if (rc == -1) {
        return -ENOMEM;
    }
    if (rc == -3) {
        return -EACCES;
    }
    if (rc != 0 && rc != -4) {
        vma_exhausted("mprotect");
        return -ENOMEM;
    }
    mmn_notify(mm, MMN_PROTECT, addr, sealed);
    protect_resident(addr, sealed, vmprot);
    return rc == -4 ? -EPERM : 0;
}

/** Change the protection of a range (mprotect(2)): `mprotect_key` with no
 * key named. */
long sys_mprotect(isr_params_t *params)
{
    return mprotect_key(params, -1);
}

#define MREMAP_MAYMOVE   1
#define MREMAP_FIXED     2
#define MREMAP_DONTUNMAP 4

/** Give [addr, addr + len), which lies in the region `r`, a region at `to`
 * of `new_len` bytes, moving the resident pages there. Returns `to`, or
 * -ENOMEM with no room for the regions or a page table. The file the region
 * maps and its policy, which the new region keeps as Linux's copy_vma keeps
 * it, are held across the move, which removes the old region's holds before
 * the new region takes its own. */
static long remap_move(vm_space_t *vm, vma_t *r, uintptr_t addr, uintptr_t len, uintptr_t to,
                       uintptr_t new_len)
{
    vma_t like = *r;
    uint32_t pgoff = r->pgoff + (uint32_t)((addr - r->start) / PAGE_SIZE);
    uintptr_t moved = len < new_len ? len : new_len;
    int rc;

    if (vm_reserve(vm, 3) != 0) {
        vma_exhausted("mremap");
        return -ENOMEM;
    }
    if (paging_move_range(_curr_dir, addr, to, moved) != 0) {
        return -ENOMEM;
    }
    mmn_notify(current_proc->mm, MMN_UNMAP, addr, addr + len);
    vm_file_get(like.file);
    (void)vm_policy_get(like.policy);
    vm_remove(vm, addr, addr + len);
    unmap_user_range(_curr_dir, addr, addr + len);
    rc = vm_insert_like(vm, to, to + new_len, &like, pgoff);
    vm_file_put(like.file);
    vm_policy_put(like.policy);
    if (rc != 0) {
        unmap_user_range(_curr_dir, to, to + moved);
        return -ENOMEM;
    }
    return (long)to;
}

/**
 * mremap with MREMAP_FIXED: give [addr, addr + old_len), which lies in one
 * region, the range [to, to + new_len), whatever was mapped there going
 * first. EINVAL for a new address not page-aligned, outside user space or
 * overlapping the old range; ENOMEM with no room for the regions, or past
 * RLIMIT_AS, which counts what the move leaves: every region but the old
 * range and what the destination held, and the new range. The two ranges
 * do not overlap and the old one is mapped whole, so the subtraction cannot
 * wrap. The destination's removal does not sleep, so the old range is still
 * in its region when the move starts.
 */
static long remap_fixed(vm_space_t *vm, uintptr_t addr, uintptr_t old_len, uintptr_t to,
                        uintptr_t new_len)
{
    unsigned long limit = current_proc->rlim[RLIM_AS].cur;
    uintptr_t rest;

    if ((to & (PAGE_SIZE - 1)) != 0 || to < USER_ADDR_MIN || to > task_size() - new_len ||
        (to < addr + old_len && addr < to + new_len)) {
        return -EINVAL;
    }
    if (vm_sealed_at(vm, to, to + new_len) < to + new_len) {
        return -EPERM;
    }
    rest = vm_total(vm) - vm_covered(vm, to, to + new_len) - old_len;
    if (new_len > limit || rest > limit - new_len) {
        return -ENOMEM;
    }
    if (vm_reserve(vm, 4) != 0) {
        vma_exhausted("mremap");
        return -ENOMEM;
    }
    vm_remove(vm, to, to + new_len);
    mmn_notify(current_proc->mm, MMN_UNMAP, to, to + new_len);
    unmap_user_range(_curr_dir, to, to + new_len);
    return remap_move(vm, vm_find(vm, addr), addr, old_len, to, new_len);
}

/**
 * Whether mremap's arguments name ranges it can work on: a page-aligned
 * address and two lengths that are not zero, flags it knows with
 * MREMAP_FIXED only beside MREMAP_MAYMOVE, an old range that ends inside
 * the caller's address space (`task_size`) and a new length that fits in
 * it. The lengths are judged as
 * they are given, before they are rounded up to pages, so that neither
 * range can wrap past the end of the address space: with an old length
 * near 4 GiB the old range would seem to end below its start, and a fixed
 * move onto its own address would remove its source.
 */
static int remap_args_valid(uintptr_t addr, uintptr_t old_len, uintptr_t new_len, uint32_t flags)
{
    return (addr & (PAGE_SIZE - 1)) == 0 && old_len != 0 && new_len != 0 &&
           (flags & ~(uint32_t)(MREMAP_MAYMOVE | MREMAP_FIXED)) == 0 &&
           (!(flags & MREMAP_FIXED) || (flags & MREMAP_MAYMOVE)) && addr <= task_size() &&
           old_len <= task_size() - addr && new_len <= task_size() - USER_ADDR_MIN;
}

/**
 * The change `sys_mremap` makes once its arguments are checked: give
 * [addr, addr + old_len), which lies in the region `r`, `new_len` bytes, at
 * `to` under MREMAP_FIXED, in place when it shrinks or the range after it
 * is free, and elsewhere under MREMAP_MAYMOVE. Returns the mapping's
 * address, or -ENOMEM. The caller reaps the files the change let go.
 */
static long remap_resize(vma_t *r, uintptr_t addr, uintptr_t old_len, uintptr_t new_len,
                         uint32_t flags, uintptr_t to)
{
    vm_space_t *vm = &current_proc->mm->vm;

    if (flags & MREMAP_FIXED) {
        return remap_fixed(vm, addr, old_len, to, new_len);
    }
    if (new_len <= old_len) {
        if (vm_remove(vm, addr + new_len, addr + old_len) != 0) {
            vma_exhausted("mremap");
            return -ENOMEM;
        }
        mmn_notify(current_proc->mm, MMN_UNMAP, addr + new_len, addr + old_len);
        unmap_user_range(_curr_dir, addr + new_len, addr + old_len);
        return (long)addr;
    }
    if (vm_total(vm) + (new_len - old_len) > current_proc->rlim[RLIM_AS].cur) {
        return -ENOMEM;
    }
    if (addr + old_len == r->end && addr <= task_size() - new_len &&
        addr + new_len <= vm->stack_limit && !vm_overlaps(vm, addr + old_len, addr + new_len)) {
        if (vm_insert_like(vm, addr + old_len, addr + new_len, r,
                           r->pgoff + (uint32_t)((addr + old_len - r->start) / PAGE_SIZE)) != 0) {
            vma_exhausted("mremap");
            return -ENOMEM;
        }
        return (long)addr;
    }
    if (!(flags & MREMAP_MAYMOVE)) {
        return -ENOMEM;
    }
    to = mmap_free(vm, new_len);
    if (to == 0) {
        return -ENOMEM;
    }
    return remap_move(vm, r, addr, old_len, to, new_len);
}

/**
 * Resize a mapping, or move it (mremap(2)). Its arguments are old address, old
 * length, new length, flags and new address with MREMAP_FIXED. The old range
 * must lie in one region. With MREMAP_FIXED the mapping moves, pages and all,
 * to the new address, whatever was there going first (`remap_fixed`). Otherwise
 * a shrink cuts its end, and a growth takes the range after it when that is
 * free, or with MREMAP_MAYMOVE moves the whole mapping to a free range of the
 * new length. Returns the mapping's address, or -errno: EINVAL for arguments
 * that fail `remap_args_valid`, and for a mapping of a file whose last page at
 * the new length would not fit in 32 bits; EFAULT for a range not in one
 * region, or for a growth of device memory; EPERM when the region at the old
 * address is sealed (mseal), or under MREMAP_FIXED a region of the new range
 * is; ENOMEM when it cannot grow in place and may not move, when no range is
 * free, or past RLIMIT_AS; EAGAIN when the growth of a locked mapping would
 * take the locked pages past RLIMIT_MEMLOCK. The growth of a locked mapping is
 * locked and brought in, unless it was locked on fault, as on Linux.
 * MREMAP_DONTUNMAP is not implemented, and is EINVAL.
 */
long sys_mremap(isr_params_t *params)
{
    uintptr_t addr = SC_ARG(params, 0);
    uint32_t flags = (uint32_t)SC_ARG(params, 3);
    uintptr_t to = SC_ARG(params, 4);
    uintptr_t old_len, new_len;
    vm_space_t *vm;
    vma_t *r;
    int locked;
    long rc;

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    vm = &current_proc->mm->vm;
    if (!remap_args_valid(addr, SC_ARG(params, 1), SC_ARG(params, 2), flags)) {
        return -EINVAL;
    }
    old_len = PAGE_UP(SC_ARG(params, 1));
    new_len = PAGE_UP(SC_ARG(params, 2));
    r = vm_find(vm, addr);
    if (!r) {
        return -EFAULT;
    }
    if (r->prot & VM_PROT_SEALED) {
        return -EPERM;
    }
    if (addr + old_len > r->end) {
        return -EFAULT;
    }
    if (r->file && new_len / PAGE_SIZE > 0xFFFFFFFFu - (r->pgoff + (addr - r->start) / PAGE_SIZE)) {
        return -EINVAL;
    }
    if ((r->prot & VM_PROT_IO) && new_len > old_len) {
        return -EFAULT; /* device memory does not grow, as Linux's VM_DONTEXPAND */
    }
    locked = (r->prot & VM_PROT_LOCKS) == VM_PROT_LOCKED;
    if ((r->prot & VM_PROT_LOCKED) && new_len > old_len &&
        !mem_lock_room(vm_locked(vm) / PAGE_SIZE, (new_len - old_len) / PAGE_SIZE)) {
        return -EAGAIN;
    }
    rc = remap_resize(r, addr, old_len, new_len, flags, to);
    vm_file_reap();
    if (locked && new_len > old_len && (unsigned long)rc < (unsigned long)-4095L) {
        (void)vm_populate((uintptr_t)rc + old_len, (uintptr_t)rc + new_len);
    }
    return rc;
}

#define MADV_DONTNEED        4
#define MADV_FREE            8
#define MADV_REMOVE          9
#define MADV_DONTFORK        10
#define MADV_DOFORK          11
#define MADV_WIPEONFORK      18
#define MADV_KEEPONFORK      19
#define MADV_COLD            20
#define MADV_PAGEOUT         21
#define MADV_DONTNEED_LOCKED 24
#define MADV_COLLAPSE        25
#define MADV_GUARD_INSTALL   102
#define MADV_GUARD_REMOVE    103

/**
 * Whether `sys_madvise` takes `advice`: one Linux defines from MADV_NORMAL (0)
 * to MADV_COLLAPSE, except MADV_REMOVE, which would free the blocks of a file
 * under a shared mapping and is not implemented, and the two of guard regions,
 * which Linux 6.13 added. Linux leaves 5 to 7 undefined, and its MADV_HWPOISON
 * (100) and MADV_SOFT_OFFLINE (101) inject memory faults, which Rach2 does
 * not.
 */
static int madvise_known(uint32_t advice)
{
    return advice <= MADV_DONTNEED || advice == MADV_FREE ||
           (advice >= MADV_DONTFORK && advice <= MADV_COLLAPSE) || advice == MADV_GUARD_INSTALL ||
           advice == MADV_GUARD_REMOVE;
}

/**
 * Whether the region `r` may hold a guard, as Linux 6.13's
 * is_valid_guard_vma has it, or be wiped at a fork, as Linux's
 * MADV_WIPEONFORK takes it: anonymous and private, the memory of mmap's
 * MAP_ANONYMOUS, the heap or the stack, and not device memory; and for a
 * guard's `install` not locked, whose pages Linux will not drop.
 */
static int madvise_guard_allowed(const vma_t *r, int install)
{
    return r->file == NULL && (r->type == VM_ANON || r->type == VM_HEAP || r->type == VM_STACK) &&
           !(r->prot & VM_PROT_IO) && !(install && (r->prot & VM_PROT_LOCKED));
}

int madvise_refused(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t advice)
{
    int drop = advice == MADV_DONTNEED || advice == MADV_FREE || advice == MADV_COLD ||
               advice == MADV_PAGEOUT;
    int anon =
        advice == MADV_GUARD_INSTALL || advice == MADV_GUARD_REMOVE || advice == MADV_WIPEONFORK;
    uintptr_t at;

    if (!drop && !anon && advice != MADV_DONTNEED_LOCKED && advice != MADV_DOFORK) {
        return 0;
    }
    for (at = start; at < end; at = vm_find(vm, at)->end) {
        const vma_t *r = vm_find(vm, at);

        if (anon ? !madvise_guard_allowed(r, advice == MADV_GUARD_INSTALL)
                 : (r->prot & VM_PROT_IO) || (drop && (r->prot & VM_PROT_LOCKED))) {
            return 1;
        }
    }
    return 0;
}

/**
 * Where `advice` over [start, end), which regions of `vm` cover, must stop
 * with EPERM, as Linux's can_modify_vma_madv has it: at the first sealed
 * region (mseal) of anonymous, private memory without write, for the advice
 * that would discard its pages, MADV_DONTNEED, MADV_DONTNEED_LOCKED,
 * MADV_FREE, MADV_REMOVE, MADV_DONTFORK, MADV_WIPEONFORK and, as Linux 6.13
 * adds, MADV_GUARD_INSTALL. The start of
 * that region, or `start` when it begins below, or `end` when the advice may
 * go over the whole range. A region of a program's segment counts as a
 * file's.
 */
static uintptr_t madvise_sealed_at(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t advice)
{
    const vma_t *r;

    if (advice != MADV_DONTNEED && advice != MADV_DONTNEED_LOCKED && advice != MADV_FREE &&
        advice != MADV_REMOVE && advice != MADV_DONTFORK && advice != MADV_WIPEONFORK &&
        advice != MADV_GUARD_INSTALL) {
        return end;
    }
    for (r = vm_lower(vm, start); r && r->start < end; r = vm_next(r)) {
        if ((r->prot & VM_PROT_SEALED) && !(r->prot & VM_PROT_WRITE) && r->file == NULL &&
            r->type != VM_SHARED && r->type != VM_CODE && r->type != VM_DATA) {
            return r->start > start ? r->start : start;
        }
    }
    return end;
}

/** The mark of a region (vm.h) that `advice` sets or clears: VM_PROT_DONTFORK
 * for MADV_DONTFORK and MADV_DOFORK, VM_PROT_WIPEONFORK for MADV_WIPEONFORK
 * and MADV_KEEPONFORK, VM_PROT_GUARD for the two of guards, or 0. */
static uint32_t madvise_mark_bit(uint32_t advice)
{
    switch (advice) {
    case MADV_DONTFORK:
    case MADV_DOFORK:
        return VM_PROT_DONTFORK;
    case MADV_WIPEONFORK:
    case MADV_KEEPONFORK:
        return VM_PROT_WIPEONFORK;
    case MADV_GUARD_INSTALL:
    case MADV_GUARD_REMOVE:
        return VM_PROT_GUARD;
    default:
        return 0;
    }
}

/**
 * Advice about a range (madvise(2)). Its arguments are address, length and
 * advice. MADV_DONTNEED, MADV_DONTNEED_LOCKED and MADV_FREE let the resident
 * pages go: a private anonymous page reads as zeros again, a private page of a
 * file reads the file again, and a shared page reads what the shared memory
 * holds. Those three are EINVAL over device memory, which has no pages to read
 * again, as on Linux for VM_PFNMAP, and MADV_DONTNEED and MADV_FREE are EINVAL
 * over a locked region, whose pages MADV_DONTNEED_LOCKED alone drops, as on
 * Linux for VM_LOCKED; MADV_COLD and MADV_PAGEOUT, which change nothing here,
 * are EINVAL over either, as Linux's can_madv_lru_vma refuses them. Over a
 * sealed region of anonymous memory without write (mseal) the advice that
 * discards pages stops with EPERM, the regions below it in the range advised
 * first, as Linux advises region by region. MADV_DONTFORK marks the regions so
 * that a fork's child does not get them, and MADV_DOFORK clears the mark;
 * DOFORK is EINVAL over device memory, as on Linux for VM_IO, and either is
 * ENOMEM when a region cannot be split. MADV_WIPEONFORK marks the regions so
 * that a fork's child gets them without their pages, reading zeros, and
 * MADV_KEEPONFORK clears the mark (VM_PROT_WIPEONFORK, vm.h); WIPEONFORK is
 * EINVAL over a region that is not private anonymous memory, as on Linux,
 * and either is ENOMEM when a region cannot be split. MADV_GUARD_INSTALL
 * makes the range a guard (VM_PROT_GUARD, vm.h), whose pages go and whose every
 * access is SIGSEGV with SEGV_MAPERR, and MADV_GUARD_REMOVE ends the guard,
 * leaving pages that read as zeros; both are EINVAL over a region that may not
 * hold a guard (`madvise_guard_allowed`) and ENOMEM when a region cannot be
 * split. Linux marks a guard's pages in its page tables and splits no region,
 * so a guard shows in /proc/PID/maps here as a region of its own, with the
 * protection of the region it was cut from. Any other advice `madvise_known`
 * takes changes nothing, and the rest is EINVAL, as Linux refuses an advice it
 * does not define. A range with a page in no region is ENOMEM, as in Linux. A
 * range that runs past user space is EINVAL, judged by the length as given: a
 * length within a page of 4 GiB rounds up to 0, which would pass for an empty
 * range.
 */
long sys_madvise(isr_params_t *params)
{
    uintptr_t addr = SC_ARG(params, 0);
    uint32_t advice = (uint32_t)SC_ARG(params, 2);
    vm_space_t *vm;
    uintptr_t len, end;

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    vm = &current_proc->mm->vm;
    if ((addr & (PAGE_SIZE - 1)) != 0 || addr > USER_ADDR_MAX ||
        SC_ARG(params, 1) > USER_ADDR_MAX - addr || !madvise_known(advice)) {
        return -EINVAL;
    }
    len = PAGE_UP(SC_ARG(params, 1));
    if (len == 0) {
        return 0;
    }
    if (vm_covered(vm, addr, addr + len) != len) {
        return -ENOMEM;
    }
    end = madvise_sealed_at(vm, addr, addr + len, advice);
    if (madvise_refused(vm, addr, end, advice)) {
        return -EINVAL;
    }
    if (madvise_mark_bit(advice) != 0) {
        uint32_t bit = madvise_mark_bit(advice);
        int set =
            advice == MADV_DONTFORK || advice == MADV_GUARD_INSTALL || advice == MADV_WIPEONFORK;

        if (vm_advise(vm, addr, end, set ? bit : 0u, set ? 0u : bit) != 0) {
            vma_exhausted("madvise");
            return -ENOMEM;
        }
        if (advice == MADV_GUARD_INSTALL) {
            mmn_notify(current_proc->mm, MMN_DROP, addr, end);
            unmap_user_range(_curr_dir, addr, end);
        }
    } else if (advice == MADV_DONTNEED || advice == MADV_DONTNEED_LOCKED || advice == MADV_FREE) {
        mmn_notify(current_proc->mm, MMN_DROP, addr, end);
        unmap_user_range(_curr_dir, addr, end);
    }
    return end < addr + len ? -EPERM : 0;
}

/**
 * Seal a range (mseal(2)), as Linux 6.12's mm/mseal.c does. Its arguments are
 * address, length and flags. Each region of the range takes VM_PROT_SEALED
 * (vm.h), split at the range's ends; sealing a sealed region again is no
 * error, and no call unseals. Returns 0, or -errno: EINVAL for flags that are
 * not 0, an unaligned address, or a range that wraps; ENOMEM, with nothing
 * sealed, for a range with a page in no region, or when a split finds no
 * room. An empty range is 0. Linux has seals on 64-bit kernels alone: the
 * i686 kernel answers EPERM to every call, as Linux's i386 kernel does, and
 * the x86-64 kernel seals for an i386 program too.
 */
long sys_mseal(isr_params_t *params)
{
    uintptr_t start = SC_ARG(params, 0);
    uintptr_t len_in = SC_ARG(params, 1);
    vm_space_t *vm;
    uintptr_t len;

    if (sizeof(uintptr_t) < 8u) {
        return -EPERM;
    }
    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    vm = &current_proc->mm->vm;
    if (SC_ARG(params, 2) != 0 || (start & (PAGE_SIZE - 1)) != 0) {
        return -EINVAL;
    }
    len = PAGE_UP(len_in);
    if ((len_in != 0 && len == 0) || start + len < start) {
        return -EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (vm_covered(vm, start, start + len) != len) {
        return -ENOMEM;
    }
    if (vm_advise(vm, start, start + len, VM_PROT_SEALED, 0u) != 0) {
        vma_exhausted("mseal");
        return -ENOMEM;
    }
    return 0;
}

/**
 * Change the protection of a range under a protection key (pkey_mprotect(2)).
 * Its arguments are mprotect's and the key, an int: -1 is mprotect itself,
 * and any other must be one the caller may name (`pkey_named`), a key it
 * allocated or the default key 0, which the range then takes, or EINVAL
 * after mprotect's checks of the address (EINVAL), an empty range (0) and
 * one that wraps (ENOMEM), as `mprotect_key` orders them. With the keys
 * off no key may be named, as on Linux where the processor has none. Linux
 * builds the keys' calls on x86-64 alone, so the i686 kernel has no row for
 * this one and its two companions, and answers ENOSYS.
 */
long sys_pkey_mprotect(isr_params_t *params)
{
    return mprotect_key(params, (int32_t)SC_ARG(params, 3));
}

/**
 * Allocate a protection key (pkey_alloc(2)). Its arguments are flags, which
 * must be 0, and the key's first access rights, of PKEY_ACCESS_MASK; either
 * otherwise is EINVAL. The answer is the lowest key, 1 to 15 or 0 once the
 * program freed it, that the address space has not allocated, with its
 * rights written into the calling thread's PKRU, as Linux's; ENOSPC when
 * every key is taken, the execute-only key among them, and always with the
 * keys off.
 */
long sys_pkey_alloc(isr_params_t *params)
{
    uintptr_t rights = SC_ARG(params, 1);
    int key;

    if (SC_ARG(params, 0) != 0 || (rights & ~(uintptr_t)PKEY_ACCESS_MASK) != 0) {
        return -EINVAL;
    }
    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    key = pkey_take(current_proc->mm);
    if (key < 0) {
        return -ENOSPC;
    }
    pkey_rights(key, (uint32_t)rights);
    return key;
}

/**
 * Free a protection key (pkey_free(2)): its argument, an int, must be a key
 * the caller may name (`pkey_named`), or the answer is EINVAL. The key may
 * be allocated again; the regions that have it keep it, and PKRU keeps its
 * rights, as on Linux. With the keys off every key is EINVAL.
 */
long sys_pkey_free(isr_params_t *params)
{
    int key = (int32_t)SC_ARG(params, 0);

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    if (!pkey_named(current_proc->mm, key)) {
        return -EINVAL;
    }
    current_proc->mm->pkeys &= ~(1u << key);
    return 0;
}

/**
 * Map a shadow stack (map_shadow_stack(2)). Linux builds the call on x86-64
 * with CONFIG_X86_USER_SHADOW_STACK, as the distributions do, and answers
 * EOPNOTSUPP before any check of its arguments where the processor's shadow
 * stacks for user mode are not enabled, which Rach2 never enables: to 64-bit
 * and i386 programs alike. The i686 kernel has no row, as Linux cannot build
 * the call there, and answers ENOSYS.
 */
long sys_map_shadow_stack(isr_params_t *params)
{
    (void)params;
    return -EOPNOTSUPP;
}

/**
 * Write back the shared mappings of disk files among the regions of
 * [addr, end), which the caller found covered: each region's file whole
 * (`filemap_msync`), and with `flush` the file system's fsync of it after.
 * The writes sleep, so each region is found again by address and its file
 * held across its write. 0, or -EIO when a write failed.
 */
static int32_t msync_range(uintptr_t addr, uintptr_t end, int flush)
{
    int32_t rc = 0;

    while (addr < end) {
        const vma_t *r = vm_find(&current_proc->mm->vm, addr);
        struct vm_file *f;

        if (!r) {
            break;
        }
        addr = r->end;
        f = r->type == VM_SHARED ? r->file : NULL;
        if (f) {
            vm_file_get(f);
            if (filemap_msync(f, flush) != 0) {
                rc = -EIO;
            }
            vm_file_put(f);
        }
    }
    vm_file_reap();
    return rc;
}

/**
 * Write a mapping back (msync(2)). Its arguments are address, length and
 * flags. A shared mapping of a disk file has its file's pages written back
 * (`msync_range`), at once with MS_ASYNC too, and with MS_SYNC the file
 * system's fsync after; a shared mapping of a file in memory maps the file's
 * pages themselves, and a private one has nothing to write. Returns 0, EIO for
 * a failed write, ENOMEM for a range with a page in no region, or EINVAL for
 * an unaligned address, an unknown flag, MS_ASYNC with MS_SYNC, or a range
 * that runs past user space, judged by the length as given, as `sys_madvise`
 * judges it.
 */
long sys_msync(isr_params_t *params)
{
    uintptr_t addr = SC_ARG(params, 0);
    uint32_t flags = (uint32_t)SC_ARG(params, 2);
    uintptr_t len;

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    if ((addr & (PAGE_SIZE - 1)) != 0 || (flags & ~7u) != 0 || (flags & 5u) == 5u ||
        addr > USER_ADDR_MAX || SC_ARG(params, 1) > USER_ADDR_MAX - addr) {
        return -EINVAL;
    }
    len = PAGE_UP(SC_ARG(params, 1));
    if (vm_covered(&current_proc->mm->vm, addr, addr + len) != len) {
        return -ENOMEM;
    }
    return msync_range(addr, addr + len, (flags & 4u) != 0); /* MS_SYNC */
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
 * The kernel's id of the target `who` that a priority call of kind `which`
 * names in the caller's namespaces: 0 stays the caller's own, and an id the
 * caller cannot see matches nothing.
 */
static uint32_t prio_who(int which, uint32_t who)
{
    int64_t uid;

    if (who == 0) {
        return 0;
    }
    if (which != PRIO_USER) {
        who = pid_named(who);
        return who != 0 ? who : 0xFFFFFFFFu;
    }
    uid = uid_named(who);
    return uid >= 0 ? (uint32_t)uid : 0xFFFFFFFFu;
}

/**
 * Read a priority (getpriority).
 *
 * Its arguments are which and who. Returns the highest priority among the
 * targets, as Linux's raw call returns it, 20 less the nice value, so 1 for
 * nice 19 and 40 for nice -20; the C library turns it back into the nice value.
 * -EINVAL for an unknown `which`, -ESRCH when nothing matches.
 */
long sys_getpriority(isr_params_t *params)
{
    int which = (int)SC_ARG(params, 0);
    uint32_t who = prio_who(which, (uint32_t)SC_ARG(params, 1));
    process_t *p;
    int32_t best = -ESRCH;

    if (which < PRIO_PROCESS || which > PRIO_USER) {
        return -EINVAL;
    }
    for (p = proc_list; p; p = p->next) {
        if (prio_target(p, which, who) && 20 - p->nice > best) {
            best = 20 - p->nice;
        }
    }
    return best;
}

int proc_nice_allowed(const process_t *p, int32_t nice)
{
    return cred_privileged() || (unsigned long)(20 - nice) <= p->rlim[RLIM_NICE].cur;
}

/**
 * Set a priority (setpriority).
 *
 * Its arguments are which, who and the nice value, clamped to -20 through 19 as
 * Linux clamps it. Each target is changed on Linux's terms: a caller whose
 * effective uid is neither the target's real nor its effective uid gets -EPERM,
 * and one that would lower the target's nice value, raising its priority,
 * past what the target's `RLIMIT_NICE` allows gets -EACCES
 * (`proc_nice_allowed`); effective uid 0 may do both. The targets
 * that pass are changed even when others fail, and the call returns the error
 * of the last that failed, 0 when none did, or -ESRCH when nothing matched;
 * -EINVAL for an unknown `which`. The value is kept and inherited, and the
 * scheduler does not read it: every ready process takes its turn.
 */
long sys_setpriority(isr_params_t *params)
{
    int which = (int)SC_ARG(params, 0);
    uint32_t who = prio_who(which, (uint32_t)SC_ARG(params, 1));
    int32_t nice = (int32_t)SC_ARG(params, 2);
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
        if (!prio_target(p, which, who)) {
            continue;
        }
        if (!cred_privileged() && current_proc->euid != p->ruid && current_proc->euid != p->euid) {
            rc = -EPERM;
            continue;
        }
        if (nice < p->nice && !proc_nice_allowed(p, nice)) {
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

/* The kinds of target ioprio_set(2) and ioprio_get(2) take, one more than
 * the priority calls' numbers for the same kinds. */
#define IOPRIO_WHO_PROCESS 1

#define IOPRIO_WHO_USER 3

/* The I/O classes, in the top three bits of a priority, and the level of
 * the best-effort class a process without a class is compared at. */
#define IOPRIO_CLASS_NONE 0

#define IOPRIO_CLASS_RT 1

#define IOPRIO_CLASS_BE 2

#define IOPRIO_CLASS_IDLE 3

#define IOPRIO_BE_NORM (IOPRIO_CLASS_BE << 13 | 4)

/** Whether `ioprio` is a priority Linux takes from this caller: 0 when it
 * is, else the error. The real-time class needs a privileged caller. */
static int32_t ioprio_check(uint32_t ioprio)
{
    uint32_t level = ioprio & 7u;

    switch (ioprio >> 13) {
    case IOPRIO_CLASS_RT:
        if (!cred_privileged()) {
            return -EPERM;
        }
        return (ioprio & 0x1fffu) > 7u ? -EINVAL : 0;
    case IOPRIO_CLASS_BE:
        return (ioprio & 0x1fffu) > 7u ? -EINVAL : 0;
    case IOPRIO_CLASS_IDLE:
        return 0;
    case IOPRIO_CLASS_NONE:
        return level != 0 ? -EINVAL : 0;
    default:
        return -EINVAL;
    }
}

/**
 * Set an I/O priority (ioprio_set).
 *
 * Its arguments are which, who and the priority. The targets are those of
 * `setpriority`: a thread, a process group or a real user, `who` 0 being the
 * caller's own. A caller may change a target whose real uid is its own real or
 * effective uid, and a privileged one any. The targets are changed in turn
 * until one the caller may not change, which ends the call with -EPERM, as
 * Linux stops; 0 when every target took it, -ESRCH when nothing matched, and
 * the error of `ioprio_check` for a priority it does not pass, or -EINVAL for
 * an unknown `which`. The value is kept and inherited, and no disk queue reads
 * it.
 */
long sys_ioprio_set(isr_params_t *params)
{
    int which = (int)SC_ARG(params, 0);
    uint32_t who = prio_who(which - 1, (uint32_t)SC_ARG(params, 1));
    process_t *p;
    int32_t rc;

    if (which < IOPRIO_WHO_PROCESS || which > IOPRIO_WHO_USER) {
        return -EINVAL;
    }
    rc = ioprio_check((uint32_t)SC_ARG(params, 2));
    if (rc != 0) {
        return rc;
    }
    rc = -ESRCH;
    for (p = proc_list; p; p = p->next) {
        if (!prio_target(p, which - 1, who)) {
            continue;
        }
        if (!cred_privileged() && p->ruid != current_proc->euid && p->ruid != current_proc->ruid) {
            return -EPERM;
        }
        p->ioprio = (uint16_t)SC_ARG(params, 2);
        rc = 0;
    }
    return rc;
}

/**
 * Read an I/O priority (ioprio_get).
 *
 * Its arguments are which and who. Returns the priority of the one target, or
 * the best of a group's or a user's: the lowest value, with a target of no
 * class compared as the normal best-effort level, as Linux's `ioprio_best`
 * does. A process never given one has class none, level 0. -EINVAL for an
 * unknown `which`, -ESRCH when nothing matches.
 */
long sys_ioprio_get(isr_params_t *params)
{
    int which = (int)SC_ARG(params, 0);
    uint32_t who = prio_who(which - 1, (uint32_t)SC_ARG(params, 1));
    process_t *p;
    int32_t best = -ESRCH;

    if (which < IOPRIO_WHO_PROCESS || which > IOPRIO_WHO_USER) {
        return -EINVAL;
    }
    for (p = proc_list; p; p = p->next) {
        int32_t prio, a, b;

        if (!prio_target(p, which - 1, who)) {
            continue;
        }
        prio = p->ioprio;
        if (best == -ESRCH) {
            best = prio;
            continue;
        }
        a = best >> 13 == IOPRIO_CLASS_NONE ? IOPRIO_BE_NORM : best;
        b = prio >> 13 == IOPRIO_CLASS_NONE ? IOPRIO_BE_NORM : prio;
        best = a < b ? a : b;
    }
    return best;
}

/* The flag of a policy that clears itself in a fork's child, which keeps
 * the policy, as Linux keeps SCHED_BATCH and SCHED_IDLE. */
#define SCHED_RESET_ON_FORK 0x40000000

/* The highest real-time priority Linux gives SCHED_FIFO and SCHED_RR. */
#define SCHED_RT_PRIO_MAX 99

/** The bytes of a long of the caller's CPU mask: 4 for an i386 caller on
 * x86-64, whose mask is of 32-bit longs, as Linux's compat calls read it. */
static unsigned long cpu_mask_unit(void)
{
#if defined(IA32_SYSCALL_TABLE_SIZE)
    if (in_compat_syscall()) {
        return 4u;
    }
#endif
    return sizeof(unsigned long);
}

/**
 * The bytes of a CPU mask as Linux sizes the one it copies: a long of the
 * caller's (`cpu_mask_unit`) for every bits-of-that-long processors up to
 * the highest started. The mask's bytes are the same little-endian bits in
 * either size of long.
 */
static unsigned long cpu_mask_bytes(void)
{
    unsigned long unit = cpu_mask_unit();
    unsigned long bits = unit * 8u;
    unsigned long highest = 0;
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (cpu_table[c].online) {
            highest = (unsigned long)c;
        }
    }
    return (highest / bits + 1u) * unit;
}

/** The processors started, a bit for each. */
static uint64_t cpu_online_mask(void)
{
    uint64_t mask = 0;
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (cpu_table[c].online) {
            mask |= (uint64_t)1 << c;
        }
    }
    return mask;
}

/**
 * The process a scheduling call names: the caller for pid 0, otherwise the
 * process with that pid, zombies included as Linux includes them, or NULL.
 */
static process_t *sched_target(int pid)
{
    if (pid == 0) {
        return current_proc;
    }
    return proc_named((uint32_t)pid);
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
 * Give way to the other ready processes (sched_yield).
 *
 * The caller is put behind them in the round-robin order and runs again when
 * its turn comes, at once when no other process is ready. Returns 0.
 */
long sys_sched_yield(isr_params_t *params)
{
    (void)params;
    current_proc->sched_yielded = 1;
    schedule();
    return 0;
}

/* struct sched_attr of sched_setattr(2) and sched_getattr(2), Linux's
 * layout, the same for an i386 caller: SCHED_ATTR_SIZE_VER0 bytes up to
 * the period, SCHED_ATTR_SIZE_VER1 with the utilization clamps. */
struct sched_attr {
    uint32_t size;           /* the bytes the caller's structure has */
    uint32_t sched_policy;   /* SCHED_ */
    uint64_t sched_flags;    /* SCHED_FLAG_ */
    int32_t sched_nice;      /* the ordinary policies' nice value */
    uint32_t sched_priority; /* FIFO's and RR's priority, else 0 */
    uint64_t sched_runtime;  /* nanoseconds: DEADLINE's, or the slice asked */
    uint64_t sched_deadline; /* nanoseconds, DEADLINE's relative deadline */
    uint64_t sched_period;   /* nanoseconds, DEADLINE's, 0 for the deadline */
    uint32_t sched_util_min; /* the clamps, which this kernel does not have */
    uint32_t sched_util_max;
};

#define SCHED_ATTR_SIZE_VER0 48u
#define SCHED_ATTR_SIZE_VER1 56u

/* The flags of struct sched_attr, Linux's values. SCHED_FLAG_SUGOV is the
 * kernel's own, which the flags' check lets by and a later one refuses, as
 * Linux's __sched_setscheduler does. */
#define SCHED_FLAG_RESET_ON_FORK 0x01u
#define SCHED_FLAG_RECLAIM       0x02u
#define SCHED_FLAG_DL_OVERRUN    0x04u
#define SCHED_FLAG_KEEP_POLICY   0x08u
#define SCHED_FLAG_KEEP_PARAMS   0x10u
#define SCHED_FLAG_UTIL_CLAMP    0x60u
#define SCHED_FLAG_ALL           0x7Fu
#define SCHED_FLAG_SUGOV         0x10000000u
#define SCHED_DL_FLAGS           (SCHED_FLAG_RECLAIM | SCHED_FLAG_DL_OVERRUN)

/* SCHED_DEADLINE's bounds, Linux's defaults: a period of 100 us to 2^22 us
 * (sched_deadline_period_min_us and _max_us), a runtime of 1024 ns at
 * least, and the bandwidth of the admitted threads, runtime over period in
 * fixed point of SCHED_BW_SHIFT bits, at most 95% of each processor
 * (sched_rt_runtime_us over sched_rt_period_us). */
#define SCHED_DL_PERIOD_MIN  ((uint64_t)100000u)
#define SCHED_DL_PERIOD_MAX  (((uint64_t)1 << 22) * 1000u)
#define SCHED_DL_RUNTIME_MIN 1024u
#define SCHED_BW_SHIFT       20
#define SCHED_DL_BW_CPU      996147u

/* The slice an ordinary thread may ask for, Linux's clamp of 0.1 to 100 ms,
 * and the default one before its scaling by the processors. */
#define SCHED_SLICE_MIN  100000u
#define SCHED_SLICE_MAX  100000000u
#define SCHED_SLICE_BASE 750000u

/* What a call asks of a thread's scheduling, as Linux's struct sched_attr
 * reaches __sched_setscheduler. */
struct sched_req {
    int policy;        /* -1 keeps the thread's, with its reset flag */
    uint32_t flags;    /* SCHED_FLAG_, the high word refused before */
    int32_t nice;      /* -20 to 19 */
    uint32_t priority; /* as the caller gave it */
    uint64_t runtime;
    uint64_t deadline;
    uint64_t period;
};

/** Whether `policy` is one Linux's valid_policy takes; SCHED_EXT is not, as
 * for a kernel without sched_ext. */
static int sched_policy_valid(int policy)
{
    return policy == SCHED_OTHER || policy == SCHED_BATCH || policy == SCHED_IDLE ||
           policy == SCHED_FIFO || policy == SCHED_RR || policy == SCHED_DEADLINE;
}

/** Whether `policy` is one of the ordinary ones, Linux's fair_policy. */
static int sched_policy_fair(int policy)
{
    return policy == SCHED_OTHER || policy == SCHED_BATCH || policy == SCHED_IDLE;
}

/** Whether `policy` is SCHED_FIFO or SCHED_RR. */
static int sched_policy_rt(int policy)
{
    return policy == SCHED_FIFO || policy == SCHED_RR;
}

/**
 * `runtime` over `period` as a fraction of SCHED_BW_SHIFT bits, Linux's
 * to_ratio; 0 for a period of 0. Long division, since the freestanding
 * kernel has no 64-bit divide; runtime is under 2^43 ns where it is
 * called, so the shift does not overflow.
 */
static uint64_t sched_bw(uint64_t period, uint64_t runtime)
{
    uint64_t n = runtime << SCHED_BW_SHIFT, q = 0, r = 0;
    int i;

    if (period == 0) {
        return 0;
    }
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= period) {
            r -= period;
            q |= (uint64_t)1 << i;
        }
    }
    return q;
}

/** Whether `a` has deadline parameters Linux's __checkparam_dl takes. */
static int sched_dl_valid(const struct sched_req *a)
{
    uint64_t period = a->period ? a->period : a->deadline;

    if (a->flags & SCHED_FLAG_SUGOV) {
        return 1;
    }
    if (a->deadline == 0 || a->runtime < SCHED_DL_RUNTIME_MIN || (a->deadline >> 63) != 0 ||
        (a->period >> 63) != 0) {
        return 0;
    }
    return period >= a->deadline && a->deadline >= a->runtime && period >= SCHED_DL_PERIOD_MIN &&
           period <= SCHED_DL_PERIOD_MAX;
}

/**
 * Whether admitting `p` with `policy` and the parameters of `a` would pass
 * the deadline bandwidth, as Linux's sched_dl_overflow: the bandwidth of
 * every other live SCHED_DEADLINE thread and the new one together over 95%
 * of each processor started. A thread leaving the policy, or keeping its
 * bandwidth, always passes.
 */
static int sched_dl_overflow(const process_t *p, int policy, const struct sched_req *a)
{
    uint64_t total, cap, bw;
    const process_t *q;

    if (policy != SCHED_DEADLINE || (a->flags & SCHED_FLAG_SUGOV)) {
        return 0;
    }
    bw = sched_bw(a->period ? a->period : a->deadline, a->runtime);
    if (p->sched_policy == SCHED_DEADLINE && bw == sched_bw(p->dl_period, p->dl_runtime)) {
        return 0;
    }
    total = bw;
    for (q = proc_list; q; q = q->next) {
        if (q != p && q->sched_policy == SCHED_DEADLINE && q->state != PROC_ZOMBIE &&
            q->state != PROC_UNUSED) {
            total += sched_bw(q->dl_period, q->dl_runtime);
        }
    }
    cap = (uint64_t)SCHED_DL_BW_CPU * (uint64_t)smp_cpu_count();
    return total > cap;
}

/**
 * Linux's checks of a caller without CAP_SYS_NICE
 * (user_check_sched_setscheduler), by the target's RLIMIT_NICE and
 * RLIMIT_RTPRIO: no nice value lower than the first allows; no entry to a
 * real-time policy while the second is 0, and no priority past both the
 * present one and the second; no SCHED_DEADLINE; no leaving SCHED_IDLE,
 * unless the first allows the present nice value; no target of another
 * user; and no clearing of SCHED_RESET_ON_FORK. 0, or -EPERM.
 */
static int32_t sched_user_check(const process_t *p, const struct sched_req *a, int policy,
                                int reset)
{
    unsigned long rtprio = p->rlim[RLIM_RTPRIO].cur;

    if (sched_policy_fair(policy) && a->nice < p->nice && !proc_nice_allowed(p, a->nice)) {
        return -EPERM;
    }
    if (sched_policy_rt(policy) && ((policy != p->sched_policy && rtprio == 0) ||
                                    (a->priority > p->rt_priority && a->priority > rtprio))) {
        return -EPERM;
    }
    if (policy == SCHED_DEADLINE ||
        (p->sched_policy == SCHED_IDLE && policy != SCHED_IDLE && !proc_nice_allowed(p, p->nice)) ||
        !sched_may_change(p) || (p->sched_reset && !reset)) {
        return -EPERM;
    }
    return 0;
}

/**
 * The checks of `a` that come before any look at the processors, in Linux's
 * order (__sched_setscheduler), for `policy`, the one `p` is to have: a
 * flag Linux does not know, a priority past 99 or one that does not suit
 * the policy, or deadline parameters out of their bounds, -EINVAL; what the
 * caller may not do without privilege (`sched_user_check`), -EPERM; the
 * kernel's SCHED_FLAG_SUGOV, -EINVAL; the utilization clamps, -EOPNOTSUPP,
 * as from a kernel without them. 0 when they pass.
 */
static int32_t sched_check(const process_t *p, const struct sched_req *a, int policy, int reset)
{
    int32_t rc;

    if ((a->flags & ~(SCHED_FLAG_ALL | SCHED_FLAG_SUGOV)) != 0 || a->priority > SCHED_RT_PRIO_MAX ||
        (policy == SCHED_DEADLINE && !sched_dl_valid(a)) ||
        sched_policy_rt(policy) != (a->priority != 0)) {
        return -EINVAL;
    }
    if (!cred_privileged()) {
        rc = sched_user_check(p, a, policy, reset);
        if (rc != 0) {
            return rc;
        }
    }
    if (a->flags & SCHED_FLAG_SUGOV) {
        return -EINVAL;
    }
    return (a->flags & SCHED_FLAG_UTIL_CLAMP) ? -EOPNOTSUPP : 0;
}

/** Store in `p` the parameters `a` gives `policy`: a deadline thread's
 * runtime, deadline, period and flags, or an ordinary thread's nice value
 * and slice, clamped to 0.1 to 100 ms; and the policy and priority. */
static void sched_store(process_t *p, const struct sched_req *a, int policy)
{
    if (policy == SCHED_DEADLINE) {
        p->dl_runtime = a->runtime;
        p->dl_deadline = a->deadline;
        p->dl_period = a->period ? a->period : a->deadline;
        p->dl_flags = a->flags & SCHED_DL_FLAGS;
    } else if (sched_policy_fair(policy)) {
        p->nice = a->nice;
        p->sched_slice = a->runtime == 0                ? 0
                         : a->runtime < SCHED_SLICE_MIN ? SCHED_SLICE_MIN
                         : a->runtime > SCHED_SLICE_MAX ? SCHED_SLICE_MAX
                                                        : (uint32_t)a->runtime;
    }
    p->sched_policy = (uint8_t)policy;
    p->rt_priority = (uint8_t)a->priority;
}

/**
 * Give `p` the scheduling `a` asks for, in Linux's order and with its
 * errors (__sched_setscheduler): a policy Linux does not take, -EINVAL;
 * `sched_check`'s errors; SCHED_DEADLINE for a thread whose affinity leaves
 * out a processor started, -EPERM; and bandwidth the deadline threads do
 * not have (`sched_dl_overflow`), -EBUSY. A request that leaves a deadline
 * thread's parameters as they are keeps only its reset flag, and so does
 * one with SCHED_FLAG_KEEP_PARAMS.
 *
 * A SCHED_DEADLINE thread takes its turn among the ordinary threads: the
 * scheduler has no deadline class, and a real-time priority without
 * Linux's throttling at the runtime would let one that overran its
 * runtime starve the rest.
 */
static int32_t sched_apply(process_t *p, const struct sched_req *a)
{
    int policy = a->policy;
    uint64_t online = cpu_online_mask();
    int32_t rc;
    int reset;

    if (policy < 0) {
        reset = p->sched_reset;
        policy = p->sched_policy;
    } else {
        reset = (a->flags & SCHED_FLAG_RESET_ON_FORK) != 0;
        if (!sched_policy_valid(policy)) {
            return -EINVAL;
        }
    }
    rc = sched_check(p, a, policy, reset);
    if (rc != 0) {
        return rc;
    }
    if (policy == p->sched_policy && policy == SCHED_DEADLINE && a->runtime == p->dl_runtime &&
        a->deadline == p->dl_deadline && a->period == p->dl_period &&
        (a->flags & SCHED_DL_FLAGS) == p->dl_flags) {
        p->sched_reset = (uint8_t)reset;
        return 0;
    }
    if (policy == SCHED_DEADLINE && (p->affinity & online) != online) {
        return -EPERM;
    }
    if (sched_dl_overflow(p, policy, a)) {
        return -EBUSY;
    }
    p->sched_reset = (uint8_t)reset;
    if (!(a->flags & SCHED_FLAG_KEEP_PARAMS)) {
        sched_store(p, a, policy);
        futex_pi_recalculate();
        sched_priority_changed(p);
    }
    return 0;
}

/**
 * Change a thread's policy and priority (sched_setscheduler(2)), or its
 * priority alone (sched_setparam, which passes `policy` -1 and keeps the
 * policy and its SCHED_RESET_ON_FORK).
 *
 * `uparam` points at a struct sched_param, one int. No parameter or a
 * negative pid, -EINVAL; an unreadable parameter, -EFAULT; no such thread,
 * -ESRCH; then `sched_apply`, with the thread's nice value and slice kept
 * as Linux's _sched_setscheduler keeps them, and SCHED_RESET_ON_FORK in
 * `policy` taken as the flag. SCHED_DEADLINE takes its parameters from
 * sched_setattr alone, and is -EINVAL here.
 */
static int32_t sched_set(int pid, int policy, const void __user *uparam)
{
    struct sched_req a;
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
    k_memset(&a, 0, sizeof(a));
    a.policy = policy;
    if (policy >= 0 && (policy & SCHED_RESET_ON_FORK)) {
        a.flags = SCHED_FLAG_RESET_ON_FORK;
        a.policy = policy & ~SCHED_RESET_ON_FORK;
    }
    a.priority = (uint32_t)priority;
    a.nice = p->nice;
    a.runtime = p->sched_slice;
    return sched_apply(p, &a);
}

/** sched_setscheduler takes pid, policy and the struct sched_param; see
 * `sched_set`. A negative policy is -EINVAL first. */
long sys_sched_setscheduler(isr_params_t *params)
{
    if ((int)SC_ARG(params, 1) < 0) {
        return -EINVAL;
    }
    return sched_set((int)SC_ARG(params, 0), (int)SC_ARG(params, 1),
                     (const void __user *)SC_ARG(params, 2));
}

/** sched_setparam takes pid and the struct sched_param; see `sched_set`. */
long sys_sched_setparam(isr_params_t *params)
{
    return sched_set((int)SC_ARG(params, 0), -1, (const void __user *)SC_ARG(params, 1));
}

/** The slice an ordinary thread has, in nanoseconds: the one it asked for,
 * or Linux's default, 0.75 ms times one more than the log of the
 * processors started, eight at most (sysctl_sched_base_slice). */
static uint32_t sched_slice_of(const process_t *p)
{
    int cpus = smp_cpu_count();
    uint32_t factor = 1;

    if (p->sched_slice != 0) {
        return p->sched_slice;
    }
    while (cpus > 1 && factor < 4) {
        cpus >>= 1;
        factor++;
    }
    return SCHED_SLICE_BASE * factor;
}

/** Fill in `k` the parameters of `p`'s present policy, as Linux's
 * get_params: a deadline thread's parameters and flags, a real-time
 * thread's priority, or an ordinary thread's nice value and slice. */
static void sched_get_params(const process_t *p, struct sched_attr *k)
{
    if (p->sched_policy == SCHED_DEADLINE) {
        k->sched_priority = p->rt_priority;
        k->sched_runtime = p->dl_runtime;
        k->sched_deadline = p->dl_deadline;
        k->sched_period = p->dl_period;
        k->sched_flags = (k->sched_flags & ~(uint64_t)SCHED_DL_FLAGS) | p->dl_flags;
    } else if (sched_policy_rt(p->sched_policy)) {
        k->sched_priority = p->rt_priority;
    } else {
        k->sched_nice = p->nice;
        k->sched_runtime = sched_slice_of(p);
    }
}

/**
 * Copy the caller's struct sched_attr into `k`, as Linux's sched_copy_attr:
 * a size of 0 is SCHED_ATTR_SIZE_VER0's, one below that or past a page is
 * -E2BIG, and so is a larger structure than this kernel's whose further
 * bytes are not all 0, either with this kernel's size written back into
 * the caller's; the utilization clamps in a structure without them are
 * -EINVAL; and the nice value is clamped to -20 to 19. -EFAULT for bytes
 * that cannot be read.
 */
static int32_t sched_copy_attr(struct sched_attr __user *uattr, struct sched_attr *k)
{
    uint32_t size, at, n, ksize = SCHED_ATTR_SIZE_VER1;
    unsigned char buf[64];

    k_memset(k, 0, sizeof(*k));
    if (copy_from_user(&size, uattr, sizeof(size)) != 0) {
        return -EFAULT;
    }
    if (size == 0) {
        size = SCHED_ATTR_SIZE_VER0;
    }
    if (size < SCHED_ATTR_SIZE_VER0 || size > PAGE_SIZE) {
        goto too_big;
    }
    for (at = ksize; at < size; at += n) {
        uint32_t i;

        n = size - at < sizeof(buf) ? size - at : (uint32_t)sizeof(buf);
        if (copy_from_user(buf, (const char __user *)uattr + at, n) != 0) {
            return -EFAULT;
        }
        for (i = 0; i < n; i++) {
            if (buf[i] != 0) {
                goto too_big;
            }
        }
    }
    if (copy_from_user(k, uattr, size < ksize ? size : ksize) != 0) {
        return -EFAULT;
    }
    if ((k->sched_flags & SCHED_FLAG_UTIL_CLAMP) && size < SCHED_ATTR_SIZE_VER1) {
        return -EINVAL;
    }
    k->sched_nice = k->sched_nice < -20 ? -20 : (k->sched_nice > 19 ? 19 : k->sched_nice);
    return 0;

too_big:
    (void)copy_to_user(uattr, &ksize, sizeof(ksize));
    return -E2BIG;
}

/**
 * sched_setattr(pid, attr, flags): set a thread's policy and its
 * parameters from a struct sched_attr, as Linux's. No structure, a negative
 * pid or flags, -EINVAL; then `sched_copy_attr`'s errors; a policy with its
 * top bit set, -EINVAL; no such thread, -ESRCH; then `sched_apply`'s.
 * SCHED_FLAG_KEEP_POLICY keeps the thread's policy and its reset flag, and
 * SCHED_FLAG_KEEP_PARAMS checks the thread's present parameters in place of
 * the structure's and changes nothing but the reset flag.
 */
long sys_sched_setattr(isr_params_t *params)
{
    int pid = (int)SC_ARG(params, 0);
    struct sched_attr __user *uattr = (struct sched_attr __user *)SC_ARG(params, 1);
    struct sched_attr k;
    struct sched_req a;
    process_t *p;
    int32_t rc;

    if (!uattr || pid < 0 || (uint32_t)SC_ARG(params, 2) != 0) {
        return -EINVAL;
    }
    rc = sched_copy_attr(uattr, &k);
    if (rc != 0) {
        return rc;
    }
    if ((int32_t)k.sched_policy < 0) {
        return -EINVAL;
    }
    p = sched_target(pid);
    if (!p) {
        return -ESRCH;
    }
    if (k.sched_flags & SCHED_FLAG_KEEP_PARAMS) {
        sched_get_params(p, &k);
    }
    if ((k.sched_flags >> 32) != 0) {
        return -EINVAL; /* no flag Linux knows is there */
    }
    a.policy = (k.sched_flags & SCHED_FLAG_KEEP_POLICY) ? -1 : (int)k.sched_policy;
    a.flags = (uint32_t)k.sched_flags;
    a.nice = k.sched_nice;
    a.priority = k.sched_priority;
    a.runtime = k.sched_runtime;
    a.deadline = k.sched_deadline;
    a.period = k.sched_period;
    return sched_apply(p, &a);
}

/**
 * sched_getattr(pid, attr, size, flags): a thread's policy and parameters
 * as a struct sched_attr, as Linux's: its policy, SCHED_FLAG_RESET_ON_FORK
 * when it has it, the parameters of its policy (`sched_get_params`), and
 * no utilization clamps. As much of the structure as `size` holds is
 * written, and its size field gives that, at most this kernel's. No
 * structure, a negative pid, a size below SCHED_ATTR_SIZE_VER0 or past a
 * page, or flags, -EINVAL; no such thread, -ESRCH; -EFAULT.
 */
long sys_sched_getattr(isr_params_t *params)
{
    int pid = (int)SC_ARG(params, 0);
    uintptr_t uattr = SC_ARG(params, 1);
    uint32_t usize = (uint32_t)SC_ARG(params, 2);
    struct sched_attr k;
    process_t *p;

    if (!uattr || pid < 0 || usize > PAGE_SIZE || usize < SCHED_ATTR_SIZE_VER0 ||
        (uint32_t)SC_ARG(params, 3) != 0) {
        return -EINVAL;
    }
    p = sched_target(pid);
    if (!p) {
        return -ESRCH;
    }
    k_memset(&k, 0, sizeof(k));
    k.sched_policy = p->sched_policy;
    if (p->sched_reset) {
        k.sched_flags |= SCHED_FLAG_RESET_ON_FORK;
    }
    sched_get_params(p, &k);
    k.sched_flags &= SCHED_FLAG_ALL;
    k.size = usize < SCHED_ATTR_SIZE_VER1 ? usize : SCHED_ATTR_SIZE_VER1;
    if (!user_span_ok(uattr, usize) || copy_to_user((void __user *)uattr, &k, k.size) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * A thread's policy (sched_getscheduler), with SCHED_RESET_ON_FORK when it
 * was set with it. -EINVAL for a negative pid, -ESRCH for no process.
 */
long sys_sched_getscheduler(isr_params_t *params)
{
    int pid = (int)SC_ARG(params, 0);
    process_t *p;

    if (pid < 0) {
        return -EINVAL;
    }
    p = sched_target(pid);
    if (!p) {
        return -ESRCH;
    }
    return (long)p->sched_policy | (p->sched_reset ? SCHED_RESET_ON_FORK : 0);
}

/**
 * A process's base scheduling priority, without PI donations, written to
 * the struct sched_param the second
 * argument points at. -EINVAL for no parameter or a negative pid, -ESRCH,
 * -EFAULT.
 */
long sys_sched_getparam(isr_params_t *params)
{
    int pid = (int)SC_ARG(params, 0);
    void __user *uparam = (void __user *)SC_ARG(params, 1);
    int32_t priority = 0;

    if (!uparam || pid < 0) {
        return -EINVAL;
    }
    {
        process_t *p = sched_target(pid);

        if (!p) {
            return -ESRCH;
        }
        priority = p->rt_priority;
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

/** sched_get_priority_max takes policy. */
long sys_sched_get_priority_max(isr_params_t *params)
{
    return sched_priority_bound((int)SC_ARG(params, 0), 1);
}

/** sched_get_priority_min takes policy. */
long sys_sched_get_priority_min(isr_params_t *params)
{
    return sched_priority_bound((int)SC_ARG(params, 0), 0);
}

/**
 * A process's round-robin time slice (sched_rr_get_interval, a timespec of
 * two longs): one tick, since the timer takes the processor from every
 * process at each tick, and 0 for SCHED_FIFO and SCHED_DEADLINE, which
 * Linux gives no slice. -EINVAL for a negative pid, -ESRCH, -EFAULT.
 */
long sys_sched_rr_get_interval(isr_params_t *params)
{
    int pid = (int)SC_ARG(params, 0);
    struct kernel_timespec slice;
    process_t *p;

    if (pid < 0) {
        return -EINVAL;
    }
    p = sched_target(pid);
    if (!p) {
        return -ESRCH;
    }
    slice.tv_sec = 0;
    slice.tv_nsec =
        p->sched_policy == SCHED_FIFO || p->sched_policy == SCHED_DEADLINE ? 0 : (long)TICK_NS;
    if (copy_to_user((void __user *)SC_ARG(params, 1), &slice, sizeof(slice)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * The CPUs a process may run on (sched_getaffinity).
 *
 * Its arguments are the pid, the size of the mask and the mask. The mask has
 * a bit for each processor the process may run on of those started, and the
 * call writes `cpu_mask_bytes()` of it and returns that count, as Linux
 * returns the bytes it wrote; the C library clears the rest of a larger
 * set. -EINVAL for a length shorter than that or not a multiple of the
 * caller's long,
 * -ESRCH for no process, -EFAULT.
 */
long sys_sched_getaffinity(isr_params_t *params)
{
    unsigned long len = SC_ARG(params, 1);
    unsigned long bytes = cpu_mask_bytes();
    process_t *p;
    uint64_t mask;

    if (len < bytes || (len & (cpu_mask_unit() - 1u)) != 0) {
        return -EINVAL;
    }
    p = sched_target((int)SC_ARG(params, 0));
    if (!p) {
        return -ESRCH;
    }
    mask = p->affinity & cpu_online_mask();
    if (copy_to_user((void __user *)SC_ARG(params, 2), &mask, bytes) != 0) {
        return -EFAULT;
    }
    return (long)bytes;
}

/**
 * The page-aligned range of [`addr`, `addr` + `len`) in `*start` and `*end`,
 * as the mlock calls take it: the address rounded down and the length,
 * with what that adds, rounded up. 0, or -EINVAL for a range that wraps
 * past the end of the address space.
 */
static int32_t lock_range(uintptr_t addr, uintptr_t len, uintptr_t *start, uintptr_t *end)
{
    *start = addr & ~(uintptr_t)(PAGE_SIZE - 1);
    *end = addr + len;
    if (*end < addr) {
        return -EINVAL;
    }
    *end = (*end + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
    return *end < *start ? -EINVAL : 0;
}

/**
 * Bring in the pages of the regions of [start, end) that are locked and not
 * on fault, region by region, so that a lock over a large reservation walks
 * only what is mapped. 0, -1 when a page could not be had, or -2 at a
 * region of memfd_secret, whose pages Linux's populate cannot take, the
 * regions before it brought in.
 */
static int lock_populate(uintptr_t start, uintptr_t end)
{
    uintptr_t at = start;

    while (at < end) {
        const vma_t *r = vm_lower(&current_proc->mm->vm, at);
        uintptr_t stop;

        if (!r || r->start >= end) {
            break;
        }
        if (r->start > at) {
            at = r->start;
        }
        stop = r->end < end ? r->end : end;
        if ((r->prot & VM_PROT_LOCKS) == VM_PROT_LOCKED && (r->prot & VM_PROT_SECRET)) {
            return -2;
        }
        if ((r->prot & VM_PROT_LOCKS) == VM_PROT_LOCKED && vm_populate(at, stop) != 0) {
            return -1;
        }
        at = stop;
    }
    return 0;
}

/**
 * Lock [addr, addr + len) with the lock bits `bits`, VM_PROT_LOCKED with or
 * without VM_PROT_LOCKONFAULT, or unlock it with 0, as Linux's do_mlock and
 * munlock: the range must be mapped without a gap, and a device's regions
 * in it are left as they are. A lock is EPERM for a process without
 * privilege whose RLIMIT_MEMLOCK is 0, and ENOMEM when the pages the range
 * would add to those already locked pass it; the pages are then brought in
 * unless locked on fault, and EAGAIN tells that memory ran out first, with
 * the range locked. -ENOMEM for a gap, when a split has no room, or, with
 * the range locked, for a region of memfd_secret, as Linux's mlock fails
 * on one.
 */
static long do_mlock(uintptr_t addr, uintptr_t len, uint32_t bits)
{
    vm_space_t *vm;
    uintptr_t start, end, have;
    int32_t rc;

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    vm = &current_proc->mm->vm;
    if (bits && !mlock_allowed()) {
        return -EPERM;
    }
    rc = lock_range(addr, len, &start, &end);
    if (rc != 0 || start == end) {
        return rc;
    }
    if (bits) {
        have = (vm_locked(vm) - vm_locked_in(vm, start, end)) / PAGE_SIZE;
        if (!mem_lock_room(have, (end - start) / PAGE_SIZE)) {
            return -ENOMEM;
        }
    }
    rc = vm_lock(vm, start, end, bits);
    if (rc == -2) {
        vma_exhausted("mlock");
    }
    if (rc != 0) {
        return -ENOMEM;
    }
    if ((bits & VM_PROT_LOCKS) == VM_PROT_LOCKED) {
        rc = lock_populate(start, end);
        if (rc != 0) {
            return rc == -2 ? -ENOMEM : -EAGAIN;
        }
    }
    return 0;
}

/**
 * mlock(addr, len): lock the pages of a range in memory, as `do_mlock`
 * says. There is no swap, so a page stays in memory whether or not it is
 * locked; the lock brings the pages in, counts them in VmLck, holds them to
 * RLIMIT_MEMLOCK and keeps madvise from dropping them.
 */
long sys_mlock(isr_params_t *params)
{
    return do_mlock(SC_ARG(params, 0), SC_ARG(params, 1), VM_PROT_LOCKED);
}

/** munlock(addr, len): unlock the pages of a range, which must be mapped
 * without a gap; -ENOMEM otherwise. */
long sys_munlock(isr_params_t *params)
{
    return do_mlock(SC_ARG(params, 0), SC_ARG(params, 1), 0);
}

/** mlock2(addr, len, flags): mlock, or with MLOCK_ONFAULT (1) a lock of
 * the pages as they are touched; -EINVAL for another flag. */
long sys_mlock2(isr_params_t *params)
{
    uint32_t flags = (uint32_t)SC_ARG(params, 2);

    if (flags & ~1u) {
        return -EINVAL;
    }
    return do_mlock(SC_ARG(params, 0), SC_ARG(params, 1),
                    VM_PROT_LOCKED | (flags ? VM_PROT_LOCKONFAULT : 0u));
}

/**
 * mlockall(flags): lock every region now with MCL_CURRENT (1), and every
 * region mmap and brk make from now on with MCL_FUTURE (2), each on fault
 * with MCL_ONFAULT (4); a device's regions are never locked. -EINVAL for
 * no flag, MCL_ONFAULT alone or an unknown one; -EPERM for a process
 * without privilege whose RLIMIT_MEMLOCK is 0; -ENOMEM under MCL_CURRENT
 * when the whole address space is larger than the limit, as Linux compares
 * total_vm. A flag set replaces the last: MCL_FUTURE given and then left
 * out stops locking new regions. MCL_CURRENT brings the pages in unless
 * MCL_ONFAULT is given.
 */
long sys_mlockall(isr_params_t *params)
{
    uint32_t flags = (uint32_t)SC_ARG(params, 0);
    uint32_t bits = VM_PROT_LOCKED | ((flags & 4u) ? VM_PROT_LOCKONFAULT : 0u);
    vm_space_t *vm;

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    if (flags == 0 || (flags & ~7u) || flags == 4u) {
        return -EINVAL;
    }
    if (!mlock_allowed()) {
        return -EPERM;
    }
    vm = &current_proc->mm->vm;
    if ((flags & 1u) && !mem_lock_room(0, vm_total(vm) / PAGE_SIZE)) {
        return -ENOMEM;
    }
    vm->def_prot = (flags & 2u) ? bits : 0u;
    if (flags & 1u) {
        vm_lock_all(vm, bits);
        if (!(flags & 4u)) {
            (void)lock_populate(USER_ADDR_MIN, USER_ADDR_MAX);
        }
    }
    return 0;
}

/** munlockall(): unlock every region and stop locking new ones; 0. */
long sys_munlockall(isr_params_t *params)
{
    (void)params;
    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    current_proc->mm->vm.def_prot = 0;
    vm_lock_all(&current_proc->mm->vm, 0);
    return 0;
}

/**
 * mincore takes a page-aligned address, a length and a byte for each page,
 * which gets 1 when the page is in memory and 0 when it has not been brought in
 * yet. -EINVAL for an address not page-aligned, -ENOMEM for a range with a gap,
 * -EFAULT.
 */
long sys_mincore(isr_params_t *params)
{
    uintptr_t addr = SC_ARG(params, 0);
    uintptr_t pages;
    uintptr_t i, start, end;
    int32_t rc;

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    if (addr & (PAGE_SIZE - 1)) {
        return -EINVAL;
    }
    rc = lock_range(addr, SC_ARG(params, 1), &start, &end);
    if (rc != 0) {
        return rc;
    }
    if (vm_covered(&current_proc->mm->vm, start, end) != end - start) {
        return -ENOMEM;
    }
    pages = (SC_ARG(params, 1) + PAGE_SIZE - 1) / PAGE_SIZE;
    for (i = 0; i < pages; i++) {
        uint8_t in = paging_user_phys(current_proc->page_dir, addr + i * PAGE_SIZE) != 0;

        if (copy_to_user((void __user *)(SC_ARG(params, 2) + i), &in, 1) != 0) {
            return -EFAULT;
        }
    }
    return 0;
}

/**
 * The CPU and NUMA node the caller runs on (getcpu): the processor's number
 * and node 0, the one node, stored where the first two arguments point
 * unless either is null. The processor may change as soon as the call
 * returns, as on Linux. The third, the old cache argument, is ignored, as
 * Linux ignores it. -EFAULT for a word that cannot be written.
 */
long sys_getcpu(isr_params_t *params)
{
    uint32_t cpu = arch_cpu_id();
    uint32_t zero = 0;

    if ((SC_ARG(params, 0) &&
         copy_to_user((void __user *)SC_ARG(params, 0), &cpu, sizeof(cpu)) != 0) ||
        (SC_ARG(params, 1) &&
         copy_to_user((void __user *)SC_ARG(params, 1), &zero, sizeof(zero)) != 0)) {
        return -EFAULT;
    }
    return 0;
}

/**
 * personality, the execution domain, takes the new value, or 0xFFFFFFFF to read
 * it, and the old value is returned. The domain, the low byte, must be
 * PER_LINUX (0) or PER_LINUX32 (8), the domains of Linux programs on x86, else
 * -EINVAL. The flag bits above it are kept, and the next exec reads one:
 * ADDR_NO_RANDOMIZE, which GDB and `setarch -R` set, places the new image
 * at fixed addresses (`exec_layout`). The others change nothing.
 */
long sys_personality(isr_params_t *params)
{
    uint32_t old;

    if (!current_proc) {
        return -ESRCH;
    }
    old = current_proc->persona;
    if (SC_ARG(params, 0) != 0xFFFFFFFFu) {
        if ((SC_ARG(params, 0) & 0xFFu) != 0 && (SC_ARG(params, 0) & 0xFFu) != 8u) {
            return -EINVAL;
        }
        current_proc->persona = (uint32_t)SC_ARG(params, 0);
    }
    return (int32_t)old;
}

/**
 * Set the CPUs a process may run on (sched_setaffinity).
 *
 * Its arguments are the pid, the size of the mask and the mask. At most
 * `cpu_mask_bytes()` are read, as Linux reads no more than its own mask's
 * size, and a shorter mask is zero past its end. The process is bound to the
 * started processors the mask has (`sched_set_affinity`), and moves off one
 * it no longer has; a mask with none fails with -EINVAL. -EFAULT, then
 * -ESRCH, then -EPERM for a caller that may not change the target, then
 * -EBUSY for a SCHED_DEADLINE target and a mask without every processor
 * started, whose bandwidth was admitted against them all, in Linux's order.
 */
long sys_sched_setaffinity(isr_params_t *params)
{
    unsigned long len = SC_ARG(params, 1);
    uint64_t mask = 0;
    process_t *p;

    if (len > cpu_mask_bytes()) {
        len = cpu_mask_bytes();
    }
    if (copy_from_user(&mask, (const void __user *)SC_ARG(params, 2), len) != 0) {
        return -EFAULT;
    }
    p = sched_target((int)SC_ARG(params, 0));
    if (!p) {
        return -ESRCH;
    }
    if (!sched_may_change(p)) {
        return -EPERM;
    }
    mask &= cpu_online_mask();
    if (p->sched_policy == SCHED_DEADLINE && mask != cpu_online_mask()) {
        return -EBUSY; /* Linux's dl_task_check_affinity */
    }
    if (mask == 0) {
        return -EINVAL;
    }
    sched_set_affinity(p, mask);
    return 0;
}

/* The prctl options this kernel has; every other is EINVAL, as Linux returns
 * for an option it does not have. */
#define PR_SET_PDEATHSIG 1

#define PR_GET_PDEATHSIG 2

#define PR_GET_DUMPABLE 3

#define PR_SET_DUMPABLE 4

#define PR_SET_NAME 15

#define PR_GET_SECCOMP 21

#define PR_SET_SECCOMP 22

#define PR_GET_NAME 16

#define PR_SET_CHILD_SUBREAPER 36

#define PR_GET_CHILD_SUBREAPER 37

#define PR_SET_NO_NEW_PRIVS 38

#define PR_GET_NO_NEW_PRIVS 39

#define PR_CAPBSET_READ 23

#define PR_CAPBSET_DROP 24

#define PR_CAP_AMBIENT 47

/* PR_CAP_AMBIENT's operations, its second argument. */
#define PR_CAP_AMBIENT_IS_SET    1
#define PR_CAP_AMBIENT_RAISE     2
#define PR_CAP_AMBIENT_LOWER     3
#define PR_CAP_AMBIENT_CLEAR_ALL 4

/** Whether `cap` is in the two words of the set `set`. */
static int cap_in(const uint32_t *set, unsigned long cap)
{
    return (set[cap >> 5] & (1u << (cap & 31u))) != 0;
}

/**
 * prctl's PR_CAP_AMBIENT, the operation `op` with the capability `cap` and
 * the last two arguments `rest`, which must be 0, as Linux's `cap_task_prctl`
 * takes it. PR_CAP_AMBIENT_IS_SET answers whether `cap` is in the ambient
 * set, 1 or 0; PR_CAP_AMBIENT_RAISE adds it, for a thread that holds it
 * (`cap_word`) and has it in its inheritable set, else -EPERM;
 * PR_CAP_AMBIENT_LOWER takes it out, and PR_CAP_AMBIENT_CLEAR_ALL, whose
 * `cap` must be 0 too, empties the set. -EINVAL for another operation or a
 * capability past CAP_LAST_CAP.
 */
static long prctl_ambient(unsigned long op, unsigned long cap, unsigned long rest)
{
    process_t *p = current_proc;

    if (op == PR_CAP_AMBIENT_CLEAR_ALL) {
        if (cap != 0 || rest != 0) {
            return -EINVAL;
        }
        p->cap_amb[0] = p->cap_amb[1] = 0;
        return 0;
    }
    if (cap > CAP_LAST_CAP || rest != 0) {
        return -EINVAL;
    }
    switch (op) {
    case PR_CAP_AMBIENT_IS_SET:
        return cap_in(p->cap_amb, cap);
    case PR_CAP_AMBIENT_RAISE:
        if (!cap_holds(p, (uint32_t)cap) || !cap_in(p->cap_inh, cap)) {
            return -EPERM;
        }
        p->cap_amb[cap >> 5] |= 1u << (cap & 31u);
        return 0;
    case PR_CAP_AMBIENT_LOWER:
        p->cap_amb[cap >> 5] &= ~(1u << (cap & 31u));
        return 0;
    default:
        return -EINVAL;
    }
}

/**
 * prctl's PR_CAPBSET_READ and PR_CAPBSET_DROP, for the option `option` with
 * the capability `cap`. PR_CAPBSET_READ answers whether `cap` is in the
 * bounding set, 1 or 0, and PR_CAPBSET_DROP takes it out for good, for a
 * thread that holds CAP_SETPCAP in its user namespace, else -EPERM; an exec
 * then gives the capability to no image that has it neither inheritable
 * nor ambient (`exec_ids`). A capability past CAP_LAST_CAP is -EINVAL.
 */
static long prctl_bset(unsigned long option, unsigned long cap)
{
    if (cap > CAP_LAST_CAP) {
        return -EINVAL;
    }
    if (option == PR_CAPBSET_READ) {
        return cap_in(current_proc->bset_lost, cap) ? 0 : 1;
    }
    if (!cap_holds(current_proc, CAP_SETPCAP)) {
        return -EPERM;
    }
    current_proc->bset_lost[cap >> 5] |= 1u << (cap & 31u);
    return 0;
}

/**
 * Operations on the calling process (prctl).
 *
 * Its arguments are the option and four more, each as Linux takes it:
 *
 * PR_SET_PDEATHSIG sets the signal, 1 to 64 or 0 for none, that this
 * process receives when its parent exits (`reparent_children` posts it), and
 * PR_GET_PDEATHSIG stores it as an int at the second argument.
 *
 * PR_GET_DUMPABLE returns whether the process is dumpable, and
 * PR_SET_DUMPABLE sets it to 0 or 1. The flag is the address space's, so
 * every thread of the process has the one flag; it decides whether the
 * process may leave a core file (`terminate_core`), and whether a caller
 * without effective uid 0 may trace it or open its /proc/PID/mem.
 *
 * PR_SET_NAME sets the program name `/proc/<pid>/stat` and ps show from the
 * string at the second argument, cut to 15 bytes, and PR_GET_NAME stores the
 * name's 16 bytes, NUL-padded, at the second argument.
 *
 * PR_SET_CHILD_SUBREAPER marks the process a child subreaper when the
 * second argument is not 0 and clears the mark when it is, and
 * PR_GET_CHILD_SUBREAPER stores the mark, 0 or 1, as an int at the second
 * argument. The mark is the process's, for all its threads: when a
 * descendant's parent ends, the orphan goes to the nearest ancestor so
 * marked rather than to init (`reparent_children`).
 *
 * PR_SET_NO_NEW_PRIVS, with the second argument 1 and the rest 0, sets
 * no_new_privs for good: fork passes it on and exec then ignores set-id bits.
 * PR_GET_NO_NEW_PRIVS, with every argument 0, returns it.
 *
 * PR_GET_SECCOMP returns the thread's seccomp mode, and PR_SET_SECCOMP sets
 * the strict mode, or adds the filter at the third argument, as seccomp(2)
 * does.
 *
 * PR_CAPBSET_READ and PR_CAPBSET_DROP read and drop a capability of the
 * bounding set (`prctl_bset`), and PR_CAP_AMBIENT reads and changes the
 * ambient set (`prctl_ambient`).
 *
 * Returns 0 or the value asked for; -EINVAL for any other option, a signal
 * past 64, a dumpable value other than 0 and 1, or arguments not as above;
 * -EFAULT for a pointer that cannot be read or written.
 */
long sys_prctl(isr_params_t *params)
{
    uintptr_t arg2 = SC_ARG(params, 1);
    unsigned long rest = SC_ARG(params, 2) | SC_ARG(params, 3) | SC_ARG(params, 4);
    int32_t value;
    char name[sizeof(current_proc->comm)];

    switch (SC_ARG(params, 0)) {
    case PR_SET_PDEATHSIG:
        if (arg2 > KSIG_MAX) {
            return -EINVAL;
        }
        current_proc->pdeath_signal = (uint32_t)arg2;
        return 0;
    case PR_GET_PDEATHSIG:
        value = (int32_t)current_proc->pdeath_signal;
        return copy_to_user((void __user *)arg2, &value, sizeof(value)) != 0 ? -EFAULT : 0;
    case PR_GET_DUMPABLE:
        return (int32_t)current_proc->mm->dumpable;
    case PR_SET_DUMPABLE:
        if (arg2 > 1) {
            return -EINVAL;
        }
        current_proc->mm->dumpable = (uint32_t)arg2;
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
    case PR_SET_CHILD_SUBREAPER:
        current_proc->tg->child_subreaper = arg2 != 0;
        return 0;
    case PR_GET_CHILD_SUBREAPER:
        value = current_proc->tg->child_subreaper ? 1 : 0;
        return copy_to_user((void __user *)arg2, &value, sizeof(value)) != 0 ? -EFAULT : 0;
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
    case PR_GET_SECCOMP:
        return (int32_t)current_proc->seccomp_mode;
    case PR_SET_SECCOMP:
        return seccomp_prctl_set((uint32_t)arg2, SC_ARG(params, 2));
    case PR_CAPBSET_READ:
    case PR_CAPBSET_DROP:
        return prctl_bset(SC_ARG(params, 0), arg2);
    case PR_CAP_AMBIENT:
        return prctl_ambient(arg2, SC_ARG(params, 2), SC_ARG(params, 3) | SC_ARG(params, 4));
    default:
        return -EINVAL;
    }
}

/* The versions of capget's and capset's header: the first has one word of
 * each set, the other two two words. */
#define CAP_VERSION_1 0x19980330u

#define CAP_VERSION_2 0x20071026u

#define CAP_VERSION_3 0x20080522u

struct cap_data {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};

/**
 * Read the version at the start of the header at `uhdr` and the number of
 * words it gives each set into `*words`. An unknown version is -EINVAL,
 * with the version this kernel prefers written back into the header, which
 * is how Linux tells a program the version to use. The pid after it is read
 * only by a call that needs it (`cap_pid_in`), so a query of the version
 * through a header whose version word ends a page works as on Linux. -EFAULT
 * for a null header or one that cannot be read or written.
 */
static int32_t cap_version_in(uintptr_t uhdr, uint32_t *words)
{
    uint32_t version, preferred = CAP_VERSION_3;

    if (!uhdr || copy_from_user(&version, (const void __user *)uhdr, sizeof(version)) != 0) {
        return -EFAULT;
    }
    if (version == CAP_VERSION_1) {
        *words = 1;
        return 0;
    }
    if (version == CAP_VERSION_2 || version == CAP_VERSION_3) {
        *words = 2;
        return 0;
    }
    if (copy_to_user((void __user *)uhdr, &preferred, sizeof(preferred)) != 0) {
        return -EFAULT;
    }
    return -EINVAL;
}

/** The pid in the header at `uhdr`, after its version, into `*pid`. */
static int32_t cap_pid_in(uintptr_t uhdr, int32_t *pid)
{
    if (copy_from_user(pid, (const void __user *)(uhdr + 4u), sizeof(*pid)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * capget takes header and the sets' words.
 *
 * The sets of the thread the header's pid names, the caller for 0: the
 * effective set as `cap_eff_word` gives it, which has the capabilities of
 * file access by the file-system user id, the permitted set as `cap_word`
 * gives it with the effective set's capabilities, and the inheritable set
 * capset stored (`cap_inh`). With a null
 * second argument the call is a query of the version alone, and returns 0 even
 * for an unknown version, over which the preferred one is written
 * (`cap_version_in`). -ESRCH for a pid that names no thread, -EINVAL for a
 * negative one or an unknown version, -EFAULT for memory that cannot be read
 * or written.
 */
long sys_capget(isr_params_t *params)
{
    struct cap_data d[2];
    const process_t *p = current_proc;
    uint32_t words;
    int32_t pid;
    int32_t rc = cap_version_in(SC_ARG(params, 0), &words);

    if (rc != 0 || SC_ARG(params, 1) == 0) {
        return (SC_ARG(params, 1) == 0 && rc == -EINVAL) ? 0 : rc;
    }
    rc = cap_pid_in(SC_ARG(params, 0), &pid);
    if (rc != 0) {
        return rc;
    }
    if (pid < 0) {
        return -EINVAL;
    }
    if (pid != 0) {
        p = proc_named((uint32_t)pid);
        if (!p || p->state == PROC_UNUSED) {
            return -ESRCH;
        }
    }
    k_memset(d, 0, sizeof(d));
    d[0].effective = cap_eff_word(p, 0);
    d[1].effective = cap_eff_word(p, 1);
    d[0].permitted = cap_word(p, 0) | d[0].effective;
    d[1].permitted = cap_word(p, 1) | d[1].effective;
    d[0].inheritable = p->cap_inh[0];
    d[1].inheritable = p->cap_inh[1];
    if (copy_to_user((void __user *)SC_ARG(params, 1), d, words * sizeof(d[0])) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * capset takes header and the sets' words.
 *
 * The calling thread keeps the capabilities of the permitted set it gives
 * and loses the rest (`cap_store`), until an exec gives back those of its
 * bounding and inheritable sets (`exec_ids`). The kernel keeps one set for
 * the effective and permitted ones, so the effective set a thread holds is
 * its permitted one, and it is privileged while that has CAP_SYS_ADMIN
 * (`nocaps`): Chromium's zygote gives up every capability in each process
 * it forks, and keeps CAP_SYS_ADMIN alone in a user namespace; bubblewrap
 * keeps those its caller asks for, in the inheritable set too, and raises
 * them as ambient so that the command it runs keeps them.
 *
 * The rules are Linux's `cap_capset`: bits past CAP_LAST_CAP are ignored; a
 * pid other than 0 and the caller's is -EPERM, as is a permitted set wider
 * than the thread holds, an effective set wider than the permitted one, or
 * an inheritable set with a capability not already inheritable that is
 * outside the bounding set or, for a thread without CAP_SETPCAP, outside
 * the permitted set it holds. The ambient set keeps only what is in both
 * the new permitted and inheritable sets. An unknown version is -EINVAL,
 * and memory that cannot be read -EFAULT.
 */
long sys_capset(isr_params_t *params)
{
    struct cap_data d[2];
    process_t *p = current_proc;
    uint32_t words, i;
    int32_t pid;
    int32_t rc = cap_version_in(SC_ARG(params, 0), &words);
    int setpcap = cap_holds(p, CAP_SETPCAP);

    if (rc != 0) {
        return rc;
    }
    rc = cap_pid_in(SC_ARG(params, 0), &pid);
    if (rc != 0) {
        return rc;
    }
    if (pid != 0 && pid != (int32_t)pid_seen(p->pid)) {
        return -EPERM;
    }
    k_memset(d, 0, sizeof(d));
    if (!SC_ARG(params, 1) ||
        copy_from_user(d, (const void __user *)SC_ARG(params, 1), words * sizeof(d[0])) != 0) {
        return -EFAULT;
    }
    for (i = 0; i < 2; i++) {
        uint32_t valid = i == 0 ? 0xFFFFFFFFu : CAP_HIGH_WORD;
        uint32_t held = cap_word(p, i) | cap_eff_word(p, i);
        uint32_t inh_room = valid & ~p->bset_lost[i];

        d[i].effective &= valid;
        d[i].permitted &= valid;
        d[i].inheritable &= valid;
        if (!setpcap) {
            inh_room &= held;
        }
        if ((d[i].permitted & ~held) != 0 || (d[i].effective & ~d[i].permitted) != 0 ||
            (d[i].inheritable & ~(p->cap_inh[i] | inh_room)) != 0) {
            return -EPERM;
        }
    }
    cap_store(p, ~d[0].permitted, ~d[1].permitted);
    for (i = 0; i < 2; i++) {
        p->cap_inh[i] = d[i].inheritable;
        p->cap_amb[i] &= d[i].permitted & d[i].inheritable;
    }
    return 0;
}
