/*
 * The process system calls: exit, exec, fork and wait, signals, identities,
 * sessions, limits, scheduling and the address space. Each architecture's
 * systab.c puts the handlers in its table; what they share with the other
 * system call files is in syscall_internal.h.
 */
#include "arch/arch.h"
#include "arch/uabi.h"
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
#include "proc/seccomp.h"
#include "proc/ns.h"
#include "proc/wait_queue.h"
#include "proc/tgroup.h"
#include "proc/ptrace.h"
#include "proc/elf.h"
#include "proc/uimage.h"
#include "proc/sigframe.h"
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
#include "syscall_internal.h"

/**
 * Called as a process ends, by exit or by a signal, so that an owner that
 * has ended does not leave the screen in graphics mode.
 */
void fb_release_if_owner(uint32_t pid)
{
    if (fb_owner_pid == pid && fb_owner_pid != 0) {
        fb_exit();
        fb_owner_pid = 0;
    }
}

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
 * put at its path meanwhile; the dynamic linker there is read by the path of
 * its node, from the root of the mount table, which is not the path the
 * caller gave when chroot moved the caller's root. One on another mount is
 * read through a handle on the node its resolution held, opened once and
 * released after the load. It lives in the caller's own exec workspace below,
 * because a read of a mounted volume sleeps and a second exec runs while it
 * does.
 */
struct exec_file {
    char path[VFS_PATH_MAX]; /* the node's path on the KRFFS root */
    struct vfs_ref ref;      /* the mount is NULL on the KRFFS root and once closed */
    void *handle;
    struct mm *held; /* the address space whose program this file of the root is */
    int32_t err;     /* the errno of a failed read of a mount, which execve reports */
    int readable;    /* whether the caller may read the file itself */
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
        return exec_held_read(f->held->exe_file, dst, count, off);
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

/** Release the handle and the node `f` holds on another mount, if any; a
 * file of the root the new address space keeps stays with it. */
static void exec_file_close(struct exec_file *f)
{
    f->held = NULL;
    if (f->ref.mnt) {
        vfs_release(f->ref.mnt, f->handle);
        vfs_ref_release(&f->ref);
        f->ref.mnt = NULL;
        f->handle = NULL;
    }
}

/* The most of a script's first line that execve reads to parse it. */
#define EXEC_SHEBANG_MAX 128

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
    uint32_t nocaps; /* whether it has no capabilities (`exec_ids`) */
    int secure;      /* AT_SECURE */
    int unreadable;  /* an image loaded is one the caller may not read */
    uintptr_t entry; /* where the new image starts */
    elf_layout_t layout;
    elf_layout_t ilayout; /* the interpreter's */
    elf_image_t img;
    elf_image_t iimg;          /* the interpreter's */
    fd_table_t *files;         /* the caller's own table (`exec_files`) */
    struct fs_struct *fs_held; /* holds off CLONE_FS (`exec_ids`) */
    int path_end;              /* how `path` named a directory by its end */
    char path[VFS_PATH_MAX];
    char shebang_line[EXEC_SHEBANG_MAX + 1];
};

/** Give back the current process's exec workspace, if it has one: the
 * file held open, the frames of a stack no page directory took, the
 * address space of an image that did not commit, the storage of a
 * descriptor table it did not install, which holds no references, the
 * hold on CLONE_FS, and the workspace itself. */
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
    if (s->files != NULL) {
        fd_table_delete(s->files);
    }
    kfree(s);
    current_proc->exec_ws = NULL;
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
 * strings met. The caller's `path` becomes the interpreter's. The script is
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

    k_strncpy(s->path, interp, sizeof(s->path) - 1);
    s->path[sizeof(s->path) - 1] = '\0';
    rc = canonicalize_named(s->path, &s->path_end);
    return rc < 0 ? rc : 1;
}

/**
 * What a move of the effective ids undoes, as Linux's `commit_creds` undoes
 * it: when `p`'s effective uid or gid is no longer `old_euid` or `old_egid`,
 * its address space stops being dumpable, for every thread that shares it,
 * and it loses its parent-death signal, so the parent it had cannot signal
 * the identity it now has. A new effective uid also takes away the
 * capabilities it held in its user namespace, where its privilege is then
 * its uid's.
 */
static void cred_moved(process_t *p, uint32_t old_euid, uint32_t old_egid)
{
    if (p->euid != old_euid || p->egid != old_egid) {
        if (p->mm) {
            p->mm->dumpable = 0;
        }
        p->pdeath_signal = 0;
    }
    if (p->euid != old_euid) {
        p->ns_caps[0] = p->ns_caps[1] = 0;
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
 * ids its caller lacks (`secure`), its effective ids moved from `old_euid`
 * and `old_egid`, it is privileged where its caller was not
 * (`was_privileged`), or an image it loaded is one the caller may not read
 * (`unreadable`), whose bytes a core file or a tracer's attach would give
 * the caller, as Linux's `would_dump` has it. A secure exec, or one that
 * moved the effective ids or made the caller privileged, clears the
 * parent-death signal too, as Linux's `commit_creds` clears it; a secure
 * exec lowers a soft RLIMIT_STACK above 8 MiB to 8 MiB. The flag is set
 * before the address space is installed, so no other process sees the image
 * dumpable while it runs with new ids.
 */
static void exec_privileges(struct mm *mm, int secure, int unreadable, uint32_t old_euid,
                            uint32_t old_egid, int was_privileged)
{
    int moved = current_proc->euid != old_euid || current_proc->egid != old_egid ||
                (!was_privileged && proc_privileged(current_proc));

    mm->dumpable = secure || moved || unreadable ? 0u : 1u;
    if (secure || moved) {
        current_proc->pdeath_signal = 0;
    }
    if (secure && current_proc->rlim[RLIM_STACK].cur > RLIM_STACK_DEFAULT) {
        current_proc->rlim[RLIM_STACK].cur = RLIM_STACK_DEFAULT;
    }
}

/**
 * How far below its top page the stack of a new image may grow, when the
 * pages its arguments took start at `bottom`: to `limit`, the soft
 * RLIMIT_STACK in force at the exec, in whole pages, which bounds the whole
 * stack as Linux's does, but never into the range below USER_MMAP_BASE where
 * anonymous mappings are placed, and always as far as `bottom`. A limit
 * changed later applies at the next exec.
 */
static uintptr_t exec_stack_window(uintptr_t bottom, unsigned long limit)
{
    uintptr_t size = limit & ~(unsigned long)(PAGE_SIZE - 1);
    uintptr_t floor;

    if (size > USER_ADDR_MAX - USER_MMAP_BASE) {
        size = USER_ADDR_MAX - USER_MMAP_BASE;
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
        uintptr_t ptr;

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
        uintptr_t ptr;
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
 * moved. When `keep` is not NULL the file is the program the image runs,
 * and `keep`, the new address space, keeps it (`mm_exe_open`) while the walk
 * still holds the node's mount locked; one on the KRFFS root is then read
 * through that, and any other file on the root by its node's own path, which
 * `f` keeps. `f` also says whether the caller may read the file, which the
 * kernel loads for it with the execute right alone.
 *
 * A path that named a directory by its end, `end` (`copy_path_cwd`), must
 * reach one, and the walk that finds the file answers for it
 * (`resolve_named`): a file so named is -ENOTDIR. The node it checked is the
 * node opened, so a symbolic link replaced while the walk waits for a disk
 * cannot hand the open a program the check never saw.
 *
 * The walk may not arrive on the synthetic /dev and /proc mounts, which
 * hold no programs, so execve sees the mount boundary open sees and cannot
 * reach a file a hostile image placed below the synthetic prefix; a mount
 * with MS_NOEXEC holds none either. A directory is no program and is
 * -EACCES, as Linux's open for an exec answers. The caller needs the execute
 * right, root too. The set-id facts are taken while the node is at hand and
 * apply only once the exec commits. Setgid counts only with the
 * group-execute bit beside it, which is Linux's rule: without it a 02700
 * file owned 1000:0 passes the execute check on its owner bits, since the
 * check judges one identity class and the owner's first, and would hand its
 * executor egid 0. Setgid without group-execute is the mark of mandatory
 * locking in Unix, which nothing here implements. On a mount with MS_NOSUID
 * the bits are read and ignored. 0 or a negative errno.
 */
static int32_t exec_open(struct exec_file *f, const char *path, int end, struct exec_setid *id,
                         struct mm *keep)
{
    struct vfs_ref ref;
    struct vfs_stat vst;
    int nosuid;
    int32_t rc;

    f->ref.mnt = NULL;
    f->handle = NULL;
    f->held = NULL;
    f->err = 0;
    f->readable = 0;
    rc = resolve_named(path + (path[0] == '/'), VFS_WALK_LOCK, end, &ref);
    if (rc != 0) {
        return rc;
    }
    nosuid = (ref.mnt->flags & VFS_MS_NOSUID) != 0;
    if (vfs_synth_vpath(ref.mnt, ref.node) || (ref.mnt->flags & VFS_MS_NOEXEC)) {
        rc = -EACCES;
    } else if (vfs_getattr(ref.mnt, ref.node, &vst) == 0 && (vst.mode & 0170000u) == 0040000u) {
        rc = -EACCES;
    } else if (mnt_is_root(ref.mnt)) {
        const krffs_node_t *node = ref.node;

        rc = cred_access_node(node, CRED_X, 0);
        f->readable = cred_access_node(node, CRED_R, 0) == 0;
        if (rc == 0 && keep == NULL && krffs_node_path(ref.node, f->path, sizeof(f->path)) != 0) {
            rc = -ENAMETOOLONG;
        }
        vst.mode = node->mode;
        vst.uid = node->uid;
        vst.gid = node->gid;
    } else {
        rc = cred_access_ref(&ref, CRED_X, 0);
        if (rc == 0) {
            f->readable = cred_access_ref(&ref, CRED_R, 0) == 0;
            rc = vfs_getattr(ref.mnt, ref.node, &vst);
        }
        if (rc == 0) {
            rc = vfs_open_node(ref.mnt, ref.node, 0, &f->handle);
        }
        if (rc == 0) {
            f->ref = ref; /* held until exec_file_close */
        }
    }
    if (rc == 0 && keep != NULL) {
        rc = mm_exe_open(keep, &ref);
        if (rc == 0 && f->ref.mnt == NULL) {
            f->held = keep;
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
 * and the interpreter is opened in the script's place, as the program the new
 * address space keeps, which is Linux's exe for a script. A set-id script runs
 * as its invoker, as on Linux: the interpreter is what runs, it is not
 * set-id, and the script's bits would be unsafe, since the interpreter can
 * be made to read a different file than the one whose bits were checked. An
 * interpreter that is itself a script is -ENOEXEC: nothing here needs a
 * chain of them, and a chain could loop. 0, -ENOEXEC, or an error of the
 * open or the read.
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
    s->id.mode = 0;
    rc = exec_open(&s->prog, s->path, s->path_end, NULL, s->mm);
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
 * A thread that gave up its capabilities has them again after the exec, as
 * Linux gives a root image every capability, except in those three cases,
 * where Linux keeps the new permitted set to the old one; an exec that gives
 * them back holds off CLONE_FS as one that applies set-id bits does.
 */
static void exec_ids(struct exec_state *s)
{
    uint32_t mode = current_proc->no_new_privs ? 0u : s->id.mode;
    int regains = current_proc->nocaps && !current_proc->no_new_privs;

    if (!ns_ids_mapped(proc_userns(current_proc), s->id.uid, s->id.gid)) {
        mode = 0;
    }

    s->nocaps = current_proc->nocaps;
    if ((mode != 0 && !proc_privileged(current_proc)) || regains) {
        if (exec_traced() || exec_fs_shared()) {
            mode = 0;
        } else {
            s->nocaps = 0;
            s->fs_held = current_proc->fs;
            s->fs_held->in_exec++;
        }
    }

    s->euid = (mode & KRFFS_MODE_SUID) ? s->id.uid : current_proc->euid;
    s->egid = (mode & KRFFS_MODE_SGID) ? s->id.gid : current_proc->egid;
    s->secure = s->euid != current_proc->ruid || s->egid != current_proc->rgid;
}

/**
 * Load the interpreter the program's PT_INTERP names into the new address
 * space, as Linux's `load_elf_interp` loads it: a position-independent one,
 * which every dynamic linker is, at the top of the range mmap places
 * mappings in, just below USER_MMAP_BASE, and one with fixed addresses at
 * those. Its own PT_INTERP is ignored. The new image starts at its entry,
 * and `*base`, AT_BASE, is the distance it was moved by.
 *
 * The path is held to the rule execve's own is (`canonicalize_named`), in
 * the one walk of the open: one that names a directory by its end must
 * reach one, so a file so named is -ENOTDIR and a directory the open's
 * -EACCES, as Linux's open of the interpreter answers. 0, -ENOENT, -ENOTDIR,
 * -EACCES, -ELIBBAD for a file that is not an interpreter this kernel loads,
 * -ENOMEM for one that would cover the program or for memory its load could
 * not have, -EIO or a mount's error for a read that failed.
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
    rc = exec_open(&s->prog, s->layout.interp, end, NULL, NULL);
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
    if (s->ilayout.type == ET_DYN) {
        bias = USER_MMAP_BASE - (s->ilayout.hi - s->ilayout.lo) - s->ilayout.lo;
    }
    if (s->ilayout.lo + bias < s->img.image_end && s->ilayout.hi + bias > s->img.segs[0].start) {
        return -ENOMEM;
    }
    rc = elf_load_image_at(exec_file_read, &s->prog, s->mm->dir, bias, &s->iimg);
    if (rc != 0) {
        return exec_load_err(&s->prog, rc, -ELIBBAD);
    }
    exec_file_close(&s->prog);
    s->entry = s->iimg.entry;
    *base = bias;
    return 0;
}

/**
 * Build the new image in `s`'s address space of its own while the caller's
 * still runs, as Linux's `load_elf_binary` builds one: the program, at
 * ELF_ET_DYN_BASE when it is position-independent; the interpreter its
 * PT_INTERP names; the regions; and the stack, with the pointers and the
 * auxiliary vector below its strings. The stack may grow to the soft
 * RLIMIT_STACK, lowered to 8 MiB for an image that runs with ids its
 * caller lacks, and the regions may cover no more than RLIMIT_AS. The
 * caller's image is untouched, so every failure is an error execve
 * returns. 0, -ENOEXEC, -ENOMEM, -EIO, or an error of `exec_interp` or of a
 * mount's read.
 */
static int32_t exec_build(struct exec_state *s)
{
    struct uimage_aux aux;
    uintptr_t bias = 0;
    uintptr_t bottom = uimage_bottom(&s->args);
    unsigned long limit = current_proc->rlim[RLIM_STACK].cur;
    uint8_t random16[16];
    int32_t rc;

    rc = elf_read_layout(exec_file_read, &s->prog, 1, &s->layout);
    if (rc != 0) {
        return exec_load_err(&s->prog, rc, -ENOEXEC);
    }
    if (s->layout.type == ET_DYN && s->layout.lo < ELF_ET_DYN_BASE) {
        bias = ELF_ET_DYN_BASE - s->layout.lo;
    }
    rc = elf_load_image_at(exec_file_read, &s->prog, s->mm->dir, bias, &s->img);
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
    if (elf_setup_vmas(&s->mm->vm, &s->img, USER_STACK_BASE, exec_stack_window(bottom, limit)) !=
            0 ||
        (s->layout.interp[0] != '\0' && elf_add_vmas(&s->mm->vm, &s->iimg) != 0) ||
        (bottom < USER_STACK_BASE && !vm_grow_stack(&s->mm->vm, bottom, RLIM_INFINITY)) ||
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

    current_proc->pid = old->pid;
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
 * commit, so that the allocation fails while the exec can still return. A
 * shared table has all FD_TABLE_LIMIT entries (fs/fd.h) and never shrinks,
 * so the storage has room for the table as it will be then; the entries
 * copied into it now name descriptions it holds no references to, and the
 * commit writes over them. 0, or -ENOMEM.
 */
static int32_t exec_files_stage(struct exec_state *s)
{
    if (current_proc->fds->refs == 1) {
        return 0;
    }
    s->files = fd_table_dup(current_proc->fds);
    return s->files != NULL ? 0 : -ENOMEM;
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
 * shed acquired authority and take it back; resets what pointed into the old
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
    uint32_t old_euid = current_proc->euid;
    uint32_t old_egid = current_proc->egid;
    int was_privileged = proc_privileged(current_proc);
    int sig;

    proc_thread_gone(current_proc);
    if (current_proc != current_proc->tg->leader) {
        exec_take_leader();
    }
    ptimer_delete_all(current_proc->tg);
    current_proc->tg->execed = 1;
    exec_files(s);
    close_cloexec_fds();
    current_proc->euid = s->euid;
    current_proc->egid = s->egid;
    current_proc->suid = s->euid;
    current_proc->sgid = s->egid;
    current_proc->nocaps = s->nocaps;
    ns_exec(current_proc);
    exec_privileges(s->mm, s->secure, s->unreadable, old_euid, old_egid, was_privileged);
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
 * Replace the canonical path `path`, of VFS_PATH_MAX bytes, by the path of
 * the program the /proc/<pid>/exe link it names reaches, if it names one,
 * once `exec_open` has gone on from the link to that program's file: Chromium
 * runs its children by executing /proc/self/exe, and the program they run is
 * named by that path, not the link's, in its comm, and in its own link where
 * its file has no present path to give. For a process chroot put in a
 * directory, a path under /proc is not in the synthetic /proc, so the path
 * stays as the walk resolved it. 0, or the link's error.
 */
static int32_t exec_link_name(char *path)
{
    struct vpath vp;
    int pid;
    int n;

    if (current_proc->fs->root_mnt || synth_classify(path, &vp) != VNODE_PROC_EXE) {
        return 0;
    }
    pid = procfs_resolve_id(&vp);
    n = pid < 0 ? -ENOENT : procfs_exe_name(pid, path, VFS_PATH_MAX - 1);
    if (n < 0) {
        return n;
    }
    path[n] = '\0';
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
 * Replace the current process image with a new program, in the workspace
 * `s`; the body of `sys_execve`.
 *
 * The call's arguments are path, argv[] and envp[]. Makes the new image's
 * address space, resolves the path and checks the execute right, keeping the
 * program's file with the address space, copies the path, argv and envp onto
 * the new stack's pages, resolves a `#!` script to its interpreter, builds the
 * new image, with the dynamic linker it names, in that address space, sets
 * aside the storage of a descriptor table of the caller's own, ends the
 * process's other threads, and only then commits, so every refusal and every
 * failure of the load returns to the caller with its image and its descriptor
 * table intact. Returns 0 once the image is committed, with the return frame
 * at the new image's entry, or a negative errno.
 */
static int32_t exec_run(isr_params_t *params, struct exec_state *s)
{
    const char __user *user_path = (const char __user *)SC_ARG(params, 0);
    int32_t rc;

    if (!user_path) {
        return -EFAULT;
    }
    rc = copy_path_cwd(s->path, sizeof(s->path), user_path, &s->path_end);
    if (rc < 0) {
        return rc;
    }
    s->mm = mm_new();
    if (s->mm == NULL) {
        return -ENOMEM;
    }
    rc = exec_open(&s->prog, s->path, s->path_end, &s->id, s->mm);
    if (rc == 0) {
        rc = exec_link_name(s->path);
    }
    if (rc == 0) {
        rc = exec_copy_args(s, user_path, (char *const __user *)SC_ARG(params, 1),
                            (char *const __user *)SC_ARG(params, 2), &s->argc, &s->envc);
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
        rc = exec_files_stage(s);
    }
    if (rc == 0) {
        rc = exec_one_thread();
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
long sys_execve(isr_params_t *params)
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
#define CLONE_PIDFD          0x00001000u
#define CLONE_VFORK          0x00004000u
#define CLONE_PARENT         0x00008000u
#define CLONE_THREAD         0x00010000u
#define CLONE_SYSVSEM        0x00040000u
#define CLONE_SETTLS         0x00080000u
#define CLONE_PARENT_SETTID  0x00100000u
#define CLONE_CHILD_CLEARTID 0x00200000u
#define CLONE_DETACHED       0x00400000u
#define CLONE_CHILD_SETTID   0x01000000u

/* What a fork or a clone gives `do_fork`: the clone flags, the stack
 * pointer the child starts with or 0 for the parent's, the parent's address
 * CLONE_PARENT_SETTID names, the child's addresses CLONE_CHILD_SETTID and
 * CLONE_CHILD_CLEARTID name, each 0 for none, and the TLS base
 * CLONE_SETTLS gives the child. */
struct fork_req {
    uint32_t flags;
    uintptr_t newsp;
    uintptr_t ptid;
    uintptr_t settid;
    uintptr_t cleartid;
    uintptr_t tls;
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
        proc_fs_put(child);
        child->fs = parent->fs;
        parent->fs->refs++;
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
 * user, and counted after the reclaim that may sleep, so two forks cannot
 * both see the last place. 0, or -EAGAIN.
 */
static int32_t fork_nproc(const process_t *parent)
{
    const process_t *q;
    uint32_t mine = 0;

    if (parent->ruid == 0 || parent->rlim[RLIM_NPROC].cur == RLIM_INFINITY) {
        return 0;
    }
    for (q = proc_list; q; q = q->next) {
        if (q->ruid == parent->ruid && q->state != PROC_UNUSED) {
            mine++;
        }
    }
    return mine >= parent->rlim[RLIM_NPROC].cur ? -EAGAIN : 0;
}

/**
 * What a child takes of `parent` besides its memory, files and signals: the
 * session and the process group, which it keeps until it or the parent calls
 * setsid or setpgid, the nice value and the I/O priority, the OOM score
 * adjustment and its floor, the persona, the no_new_privs flag, whether it
 * gave up its capabilities, the seccomp mode and filters, the limits, the
 * credentials, and the program name, which `execve` sets and a child that
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
    child->tg->oom_score_adj = parent->tg->oom_score_adj;
    child->tg->oom_score_adj_min = parent->tg->oom_score_adj_min;
    child->persona = parent->persona;
    child->no_new_privs = parent->no_new_privs;
    child->nocaps = parent->nocaps;
    seccomp_inherit(child, parent);
    k_memcpy(child->rlim, parent->rlim, sizeof(child->rlim));
    child->ruid = parent->ruid;
    child->euid = parent->euid;
    child->suid = parent->suid;
    child->rgid = parent->rgid;
    child->egid = parent->egid;
    child->sgid = parent->sgid;
    child->ngroups = parent->ngroups;
    k_memcpy(child->groups, parent->groups, sizeof(child->groups));
    k_memcpy(child->comm, parent->comm, sizeof(child->comm));
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
        tgroup_put(child->tg);
        child->tg = parent->tg;
        child->tg->refs++;
        child->tg->live++;
        child->tgid = parent->tgid;
        child->sigacts = child->tg->action;
    } else {
        k_memcpy(child->tg->action, parent->tg->action, sizeof(child->tg->action));
    }
    child->sig_blocked = parent->sig_blocked;
    child->sig_pending = 0;
    if ((flags & (CLONE_VM | CLONE_VFORK)) != CLONE_VM) {
        child->altstack_sp = parent->altstack_sp;
        child->altstack_size = parent->altstack_size;
        child->altstack_on = parent->altstack_on;
    }
}

/** Give `child` the descriptor table of `parent`: the table itself under
 * CLONE_FILES, a copy otherwise. 0, or -ENOMEM. */
static int32_t fork_files(process_t *child, process_t *parent, uint32_t flags)
{
    if (flags & CLONE_FILES) {
        if (fd_table_share(parent->fds) != 0) {
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
 */
static void fork_frame(process_t *child, const isr_params_t *params, const struct fork_req *req)
{
    arch_fork_frame(child, params, req->newsp);
}

/**
 * Create a child process: fork(2), vfork(2), and the part of clone(2) this
 * kernel has.
 *
 * The child's record takes its parts from the parent's (`fork_ids`,
 * `fork_shares`, `fork_signals`), and its address space is a
 * copy-on-write copy, or under CLONE_VM the parent's own. The child is set
 * up to return from fork with value 0 (`fork_frame`); the parent gets the
 * child's PID, after the child has called execve or ended when `req` has
 * CLONE_VFORK. Returns -errno on failure. `req` has the stack pointer the
 * child starts with and the addresses of the ids clone stores: the parent's
 * CLONE_PARENT_SETTID word, stored before the child runs, and the child's
 * CLONE_CHILD_SETTID word and the CLONE_CHILD_CLEARTID address its thread end
 * clears. The child has the parent's TLS base, since its frame keeps the
 * parent's %gs selector and its address space has the same addresses,
 * unless CLONE_SETTLS names another, and the floating-point registers as
 * they are now, since the parent's record has only what they were at its
 * last switch away. The parent gets the child's PID in the parent's PID
 * namespace; the vfork wait names the child by the kernel's.
 *
 * The child is PROC_UNUSED while it is built, neither runnable nor seen by
 * the other calls, since the building may sleep and a switch to it before
 * its frame is laid out would run nothing; it becomes ready at the end. The
 * table `alloc_process` gave it, with stdio descriptions of its own, goes
 * first, before any failure path, since the child takes a copy of the
 * parent's table or the table itself. The result is -EINTR when a SIGKILL
 * is pending for the caller, which an exit_group or an exec by another
 * thread sends, or its process is ending or in another thread's exec, since
 * a child made then would outlive its process's end; nothing between that
 * test and the child's start sleeps. A failure gives back everything built
 * for the child so far.
 */
static int32_t do_fork(isr_params_t *params, const struct fork_req *req)
{
    process_t *parent = current_proc;
    process_t *child;
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
    child = alloc_process();
    if (!child) {
        return -ENOMEM; /* a record the heap could not give */
    }
    child->state = PROC_UNUSED;

    proc_files_put(child);

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
    rc = ns_fork(child, parent, req->flags);
    if (rc != 0) {
        goto fail_built;
    }
    child->tls_base = (req->flags & CLONE_SETTLS) ? req->tls : arch_tls_get();
    fpu_save(child->fpu);

    rc = fork_shares(child, parent, req->flags);
    if (rc != 0) {
        goto fail_built;
    }
    fork_store_ids(child, req);
    if (((parent->sig_pending | parent->tg->pending) & KSIG_BIT(SIGKILL)) || parent->tg->exiting ||
        parent->tg->exec_task) {
        rc = -EINTR;
        goto fail_built;
    }

    fork_signals(child, parent, req->flags);
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
    struct fork_req req = {SIGCHLD, 0, 0, 0, 0, 0};

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
    struct fork_req req = {CLONE_VM | CLONE_VFORK | SIGCHLD, 0, 0, 0, 0, 0};

    return do_fork(params, &req);
}

/** Make the clone `sys_clone` describes, from the arguments of `clone` or of
 * `clone3`; `params` is the caller's frame, which the child's is copied from.
 * The child's id, -EINVAL for a set of flags `sys_clone` does not take, or
 * the error of `arch_clone_tls` or of `do_fork`. */
static int32_t clone_with(isr_params_t *params, uint32_t flags, uintptr_t newsp, uintptr_t ptid,
                          unsigned long tls, uintptr_t ctid)
{
    struct fork_req req;

    if ((flags &
         ~(CSIGNAL | CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_VFORK |
           CLONE_PARENT | CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID |
           CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | CLONE_DETACHED | CLONE_NEW_ANY)) != 0 ||
        (!(flags & CLONE_THREAD) && (flags & CSIGNAL) != SIGCHLD) ||
        ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM)) ||
        ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND)) ||
        ((flags & CLONE_SIGHAND) && !(flags & CLONE_THREAD))) {
        return -EINVAL;
    }
    req.tls = 0;
    if (flags & CLONE_SETTLS) {
        uintptr_t base = 0;
        long err = arch_clone_tls(tls, &base);

        if (err != 0) {
            return (int32_t)err;
        }
        req.tls = base;
    }
    req.flags = flags;
    req.newsp = newsp;
    req.ptid = (flags & CLONE_PARENT_SETTID) ? ptid : 0;
    req.settid = (flags & CLONE_CHILD_SETTID) ? ctid : 0;
    req.cleartid = (flags & CLONE_CHILD_CLEARTID) ? ctid : 0;
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
 * Linux ignores the second. The CLONE_NEW flags give the child new
 * namespaces (`ns_fork`); a cgroup namespace and any other flag are EINVAL.
 */
long sys_clone(isr_params_t *params)
{
    return clone_with(params, (uint32_t)SC_ARG(params, 0), SC_ARG(params, 1), SC_ARG(params, 2),
                      SC_ARG(params, SC_CLONE_TLS), SC_ARG(params, SC_CLONE_CTID));
}

/* clone3's structure, Linux's struct clone_args as the eight 64-bit fields
 * of its first version, on every architecture. */
#define CA_FLAGS          0
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
 * fields are zero, else E2BIG, as Linux reads it. CLONE_PIDFD is EINVAL,
 * since no descriptor names a process here, and so is a flag past bit 31
 * or a field wider than an address. The caller's registers are left as they
 * were, since a caller may keep the structure's address in one across the
 * call.
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
    for (i = 0; i < CLONE_ARGS_FIELDS; i++) {
        if (a[i] != (uintptr_t)a[i]) {
            return -EINVAL;
        }
    }
    if ((a[CA_FLAGS] >> 31 >> 1) != 0 || (a[CA_FLAGS] & (CSIGNAL | CLONE_PIDFD)) != 0 ||
        a[CA_EXIT_SIGNAL] > 64u || (a[CA_STACK] == 0) != (a[CA_STACK_SIZE] == 0)) {
        return -EINVAL;
    }
    return clone_with(params, (uint32_t)(a[CA_FLAGS] | a[CA_EXIT_SIGNAL]),
                      a[CA_STACK] ? (uintptr_t)(a[CA_STACK] + a[CA_STACK_SIZE]) : 0,
                      (uintptr_t)a[CA_PARENT_TID], (unsigned long)a[CA_TLS],
                      (uintptr_t)a[CA_CHILD_TID]);
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

/* Linux's struct rusage, eighteen longs: the user and system CPU time as
 * two timevals of seconds and microseconds, then maxrss in kilobytes and
 * then ixrss through nivcsw, which this kernel does not count and reports
 * as zero. 72 bytes on i686. */
struct kernel_rusage {
    long utime_sec;
    long utime_usec;
    long stime_sec;
    long stime_usec;
    long maxrss;
    long uncounted[13];
};

#define RUSAGE_BYTES sizeof(struct kernel_rusage)

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
 * the CPU time of the process `p` is a thread of, every thread's, with that
 * of the children it has reaped, which its thread group keeps. A traced
 * thread that is not the leader reports its whole process's.
 */
static void rusage_of_child(struct kernel_rusage *ru, const process_t *p)
{
    uint32_t user;
    uint32_t sys;

    proc_cpu_time(p, &user, &sys);
    rusage_from_ticks(ru, user + p->tg->cutime, sys + p->tg->cstime);
    ru->maxrss =
        (long)(p->tg->maxrss_kb > p->tg->cmaxrss_kb ? p->tg->maxrss_kb : p->tg->cmaxrss_kb);
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
 * other than SIGCHLD, unless __WALL is beside it, and every child here
 * reports with SIGCHLD, so it selects none.
 */
static int wait_selects(int idtype, uint32_t id, int options, const process_t *p)
{
    if (p->state == PROC_UNUSED || p->parent_pid != current_proc->tgid || p->pid != p->tgid) {
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
 * Reap zombie `p`, the leader of a process whose threads have all ended,
 * which the wait has taken off the zombies (`wait_hold`), so that no other
 * thread's wait reaps it while the release sleeps: add the process's CPU
 * time, with its reaped children's, to the children's totals of the caller's
 * process, which its thread group keeps, and give back its address space,
 * kernel stack and record (`proc_release`). The zombie no longer runs on its
 * kernel stack, so the stack can go with the record.
 */
static void wait_reap(process_t *p)
{
    struct tgroup *me = current_proc->tg;
    uint32_t user;
    uint32_t sys;

    proc_cpu_time(p, &user, &sys);
    me->cutime += user + p->tg->cutime;
    me->cstime += sys + p->tg->cstime;
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
 * thread that is not its process's leader ends with no signal to its tracer,
 * so, as Linux's __WCLONE and __WALL have it, it is waited for with __WCLONE
 * or __WALL, and a leader without __WCLONE.
 */
static int wait_selects_tracee(int idtype, uint32_t id, int options, const process_t *p)
{
    int clone = p->pid != p->tgid;

    if (p->state == PROC_UNUSED || !(p->ptrace & PT_TRACED) || p->tracer != current_proc->tgid) {
        return 0;
    }
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
        p->tg->utime += p->utime;
        p->tg->stime += p->stime;
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
    if (rusage_ptr && !user_buffer_ok(rusage_ptr, RUSAGE_BYTES, 1)) {
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
         (!rusage_ptr || copy_to_user((void __user *)rusage_ptr, &r.ru, RUSAGE_BYTES) == 0);
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
 * positive id, and P_PGID the group with a nonnegative id, 0 being the
 * caller's. At least one of WEXITED, WSTOPPED and WCONTINUED must be given,
 * beside which WNOHANG, WNOWAIT and Linux's three thread bits are accepted;
 * `wait_find` has what each means.
 *
 * Returns 0 when a child was reported, with the siginfo's si_signo SIGCHLD,
 * si_code, si_pid, si_uid (the child's real uid) and si_status filled and
 * the child's usage in the rusage; and 0 with WNOHANG when no selected child
 * has an event, with those siginfo fields zero and the rusage untouched.
 * Only the bytes through those fields are written, as the architecture's
 * siginfo_t lays them out. -EINVAL for an unknown option or idtype,
 * no event asked for, or an id that does not suit its idtype; -EBADF for a
 * pidfd, since no descriptor is one; -ECHILD, -EINTR; -EFAULT, found before
 * the wait or after it with the child's event left in place, as `do_wait`
 * finds it.
 */
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
    uint32_t id = 0;
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
        return upid < 0 ? -EINVAL : -EBADF;
    default:
        return -EINVAL;
    }
    if (uinfo && !user_buffer_ok(uinfo, SIGINFO_CHLD_BYTES, 1)) {
        return -EFAULT;
    }
    if (rusage_ptr && !user_buffer_ok(rusage_ptr, RUSAGE_BYTES, 1)) {
        return -EFAULT;
    }
    rc = wait_find(idtype, id, options, &r);
    if (rc < 0) {
        return rc;
    }
    k_memset(info, 0, sizeof(info));
    if (rc > 0) {
        k_memset(&ki, 0, sizeof(ki));
        ki.code = r.code;
        ki.pid = pid_seen(r.pid);
        ki.uid = uid_seen(r.uid);
        ki.status = r.info_status;
        sigframe_fill_siginfo(info, SIGCHLD, &ki);
    }
    ok = (rc == 0 || !rusage_ptr ||
          copy_to_user((void __user *)rusage_ptr, &r.ru, RUSAGE_BYTES) == 0) &&
         (!uinfo || copy_to_user((void __user *)uinfo, info, SIGINFO_CHLD_BYTES) == 0);
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
 * `times` reports, as timevals. `ru_maxrss` is the larger of the present
 * address space's resident size and the most any earlier one had when it
 * ended, in kilobytes, or for RUSAGE_CHILDREN the most a reaped child
 * reported. The other counters, through context switches, are not kept and
 * are zero. -EINVAL for any other `who`, Linux's RUSAGE_BOTH among them,
 * -EFAULT for a structure that cannot be written.
 */
long sys_getrusage(isr_params_t *params)
{
    int who = (int)SC_ARG(params, 0);
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
        user = who == RUSAGE_CHILDREN ? current_proc->tg->cutime : current_proc->utime;
        sys = who == RUSAGE_CHILDREN ? current_proc->tg->cstime : current_proc->stime;
        kirq_restore(flags);
    }
    rusage_from_ticks(&ru, user, sys);
    if (who == RUSAGE_CHILDREN) {
        ru.maxrss = (long)current_proc->tg->cmaxrss_kb;
    } else {
        unsigned long now =
            current_proc->mm ? mm_resident(current_proc->mm) * (PAGE_SIZE / 1024u) : 0;

        ru.maxrss = (long)(now > current_proc->tg->maxrss_kb ? now : current_proc->tg->maxrss_kb);
    }
    if (copy_to_user((void __user *)SC_ARG(params, 1), &ru, RUSAGE_BYTES) != 0) {
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
 * (`ptrace_return_stops`). Blocking syscall loops call this on the thread's
 * stack with the big lock held. A deferred kernel debug hit is posted before
 * its mask and disposition are tested; a deferred user hit interrupts
 * whatever either says, but stays deferred until the return to user mode
 * delivers it. Posting can make this thread ready and clear its
 * `wakeup_tick`, so a caller that keeps waiting must arm its deadline again.
 * No wait queue is changed. On the x86-64 debug interrupt stack the kernel
 * calls only `debugreg_defer_user` and, when it declines, `debugreg_defer`;
 * a user debug exception moves to the thread's stack before entering C. */
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

/** kill(2): pid > 0 a single process, which the id of any of its threads
 * names; pid == 0 the caller's process group; pid < -1 the group (-pid).
 * pid == -1, every process, is -ESRCH, since it is not supported here.
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
        return -ESRCH; /* broadcast-to-all is not supported */
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

/**
 * The siginfo a caller of rt_sigqueueinfo or rt_tgsigqueueinfo gives at
 * `u`: its code and the three words after it, the pid, uid and value that
 * sigqueue fills in. A code of zero or above, or SI_TKILL, is the kernel's
 * to send, and a caller may give one only to its own process (`own`), as
 * on Linux. Returns 0, -EFAULT when `u` cannot be read, or -EPERM.
 */
static int32_t queued_info(uintptr_t u, int own, struct k_siginfo *info)
{
    uint8_t buf[SIGINFO_SIZE];

    if (copy_from_user(buf, (const void __user *)u, sizeof(buf)) != 0) {
        return -EFAULT;
    }
    sigframe_take_siginfo(buf, info);
    if ((info->code >= 0 || info->code == SI_TKILL) && !own) {
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
 * -EPERM from the siginfo (`queued_info`) or the permission.
 */
long sys_rt_sigqueueinfo(isr_params_t *params)
{
    int pid = (int)SC_ARG(params, 0);
    int signo = (int)SC_ARG(params, 1);
    process_t *target = pid > 0 ? proc_named((uint32_t)pid) : NULL;
    struct k_siginfo info;
    int32_t rc;

    if (signo < 0 || signo > KSIG_MAX) {
        return -EINVAL;
    }
    if (!target || target->state == PROC_UNUSED) {
        return -ESRCH;
    }
    rc = queued_info(SC_ARG(params, 2), target->tg == current_proc->tg, &info);
    if (rc == 0 && !cred_may_signal(target, signo)) {
        rc = -EPERM;
    }
    if (rc == 0 && signo != 0 && target->tg->live != 0) {
        signal_post_info(target, signo, &info);
    }
    return rc;
}

/** rt_tgsigqueueinfo takes tgid, tid, signal and the siginfo, posted to the
 * thread alone, as tgkill posts. */
long sys_rt_tgsigqueueinfo(isr_params_t *params)
{
    int tgid = (int)SC_ARG(params, 0);
    int tid = (int)SC_ARG(params, 1);
    int signo = (int)SC_ARG(params, 2);
    process_t *t = tid > 0 ? proc_named((uint32_t)tid) : NULL;
    struct k_siginfo info;
    int32_t rc;

    if (tgid <= 0 || tid <= 0 || signo < 0 || signo > KSIG_MAX) {
        return -EINVAL;
    }
    if (!t || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE ||
        t->tgid != pid_named((uint32_t)tgid)) {
        return -ESRCH;
    }
    rc = queued_info(SC_ARG(params, 3), t->tg == current_proc->tg, &info);
    if (rc == 0 && !cred_may_signal(t, signo)) {
        rc = -EPERM;
    }
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
 * mode.
 */
long sys_pause(isr_params_t *params)
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
 * Its arguments are const sigset_t *mask and sigsetsize (8). Replaces the
 * blocked mask with *mask, waits for a deliverable signal, and returns -EINTR.
 * The handler the signal runs runs under *mask, and the frame it gets has the
 * old mask, which its return restores; without a handler the old mask comes
 * back on the return to user mode. -EINVAL for another sigsetsize, -EFAULT
 * when the mask cannot be read.
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
    current_proc->saved_sigmask = current_proc->sig_blocked;
    current_proc->sigmask_saved = 1;
    signal_set_blocked(newmask);
    sig_wait();
    return -EINTR;
}

/**
 * Install or query a signal disposition (rt_sigaction).
 *
 * Its arguments are signo, const struct k_sigaction* act, struct k_sigaction*
 * old and sigsetsize, which must be 8. The old action is written before the
 * new one is stored. Installing SIG_IGN drops any instance of the signal
 * already pending, the process's and each thread's, as Linux does, with
 * interrupts off so that a signal posted from an interrupt cannot change the
 * pending sets in the middle. A query of SIGKILL or SIGSTOP reports SIG_DFL,
 * as in Linux. -EINVAL for a signal out of range, another sigsetsize, or a
 * new action for SIGKILL or SIGSTOP, which writes no old action; -EFAULT.
 */
long sys_rt_sigaction(isr_params_t *params)
{
    int signo = (int)SC_ARG(params, 0);
    const struct k_sigaction __user *uact = (const struct k_sigaction __user *)SC_ARG(params, 1);
    struct k_sigaction __user *uold = (struct k_sigaction __user *)SC_ARG(params, 2);
    uint32_t setsize = USER_LEN32(SC_ARG(params, 3));
    struct k_sigaction act;

    if (!current_proc) {
        return -ESRCH;
    }
    if (signo < 1 || signo > KSIG_MAX || setsize != 8) {
        return -EINVAL;
    }
    if (uact && (signo == SIGKILL || signo == SIGSTOP)) {
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
        if (act.handler == KSIG_IGN) {
            uint32_t f = sig_irq_save();
            sig_discard(current_proc, KSIG_BIT(signo));
            sig_irq_restore(f);
        }
    }
    return 0;
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
 */
static long signal_return(isr_params_t *tf, int rt)
{
    uint64_t mask;

    if (!current_proc) {
        return -ESRCH;
    }
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

/**
 * sigaltstack: set or read the alternate stack a handler with
 * SA_ONSTACK runs on. Its arguments are new stack or NULL and old stack or
 * NULL.
 *
 * The old settings are read before the change: SS_DISABLE for none, and
 * SS_ONSTACK while the process runs on the stack, when a change fails with
 * EPERM. SS_AUTODISARM is accepted and has no effect, and other flags are
 * EINVAL. A stack smaller than MINSIGSTKSZ fails with ENOMEM. The old
 * settings are zeroed first, so that the padding after x86-64's int flags
 * gives no byte of the kernel's stack. EFAULT for a copy that fails.
 */
long sys_sigaltstack(isr_params_t *params)
{
    const struct k_stack __user *uss = (const struct k_stack __user *)SC_ARG(params, 0);
    struct k_stack __user *uoss = (struct k_stack __user *)SC_ARG(params, 1);
    struct k_stack cur;
    struct k_stack ss;
    int on;
    int32_t flags;

    if (!current_proc) {
        return -ESRCH;
    }
    on = sigframe_on_altstack(FRAME_SP(params));
    k_memset(&cur, 0, sizeof(cur));
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
 * rt_sigpending: the signals pending while blocked. Its arguments are set
 * and size of the set in bytes, at most 8; -EINVAL for a larger size, -EFAULT.
 */
long sys_rt_sigpending(isr_params_t *params)
{
    uint32_t size = USER_LEN32(SC_ARG(params, 1));
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
    set = sig_pending_now() & current_proc->sig_blocked;
    sig_irq_restore(flags);
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
 * more than its number: the number back in the number's register and the
 * instruction pointer back on the instruction that made the call. */
static void restart_syscall(isr_params_t *tf, uint32_t restart_nr)
{
    SC_NR(tf) = restart_nr - 1u;
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
        p->tg->utime += p->utime;
        p->tg->stime += p->stime;
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
    current_proc->restart_nr = 0;

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
        if (restart_nr != 0 && (handler_flags & SA_RESTART)) {
            restart_syscall(tf, restart_nr);
        }
        oldmask =
            current_proc->sigmask_saved ? current_proc->saved_sigmask : current_proc->sig_blocked;
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
        restart_syscall(tf, restart_nr);
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
static int32_t sigwait_take(uint64_t set, int timed, uint32_t deadline, struct k_siginfo *info)
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

/** `sigwait_take`, with the siginfo copied to `uinfo` unless it is null;
 * -EFAULT when it cannot be. */
static int32_t sigwait_set(uint64_t set, int timed, uint32_t deadline, void __user *uinfo)
{
    struct k_siginfo info;
    uint8_t buf[128];
    int32_t signo = sigwait_take(set, timed, deadline, &info);

    if (signo > 0 && uinfo) {
        sigframe_fill_siginfo(buf, (int)signo, &info);
        if (copy_to_user(uinfo, buf, sizeof(buf)) != 0) {
            return -EFAULT;
        }
    }
    return signo;
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
 * whole 10 ms ticks and cut to 2147483646 of them, as nanosleep's is, so that
 * with the tick in progress added the deadline stays ahead of the present in
 * the signed comparison of ticks `sigwait_take` makes, which would take a
 * longer one for past; one of zero polls. SIGKILL and SIGSTOP cannot be waited
 * for. Returns the signal, -EAGAIN when the timeout passes, -EINTR for another
 * signal the thread acts on, -EINVAL for another sigsetsize or a timeout out
 * of range, -EFAULT.
 */
long sys_rt_sigtimedwait(isr_params_t *params)
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
    if (SC_ARG(params, 3) != sizeof(uint64_t)) {
        return -EINVAL;
    }
    if (copy_from_user(&set, (const void __user *)SC_ARG(params, 0), sizeof(set)) != 0) {
        return -EFAULT;
    }
    if (SC_ARG(params, 2)) {
        uint32_t ticks;

        if (copy_from_user(&ts, (const void __user *)SC_ARG(params, 2), sizeof(ts)) != 0) {
            return -EFAULT;
        }
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) {
            return -EINVAL;
        }
        ticks = (uint32_t)(ts.tv_sec > 21474836 ? 21474836 : ts.tv_sec) * 100u +
                ((uint32_t)ts.tv_nsec + 9999999u) / 10000000u;
        if (ticks > 0x7FFFFFFEu) {
            ticks = 0x7FFFFFFEu;
        }
        deadline = rtc_ticks_now() + ticks + (ticks != 0 ? 1u : 0u);
        timed = 1;
    }
    set &= ~(uint64_t)KSIG_UNCATCHABLE;
    saved = current_proc->sig_blocked;
    signal_set_blocked(saved & ~set);
    rc = sigwait_set(set, timed, deadline, (void __user *)SC_ARG(params, 1));
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

static const struct file_ops sigfd_ops = {sigfd_read, NULL, sigfd_poll, NULL, sigfd_close};

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
 * takes away. -EINVAL for -1 or an ID the namespace does not map
 * (`ids_named`), -EPERM otherwise.
 */
long sys_setuid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t uid = (uint32_t)SC_ARG(params, 0);
    uint32_t old_euid = p->euid;

    if (uid == SC_ID_KEEP || ids_named(&uid, 1, 0) != 0) {
        return -EINVAL;
    }
    if (id_privileged()) {
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

/**
 * setreuid takes a real and an effective user ID, -1 keeping either. A caller
 * that is not privileged (`id_privileged`) may set the real ID to its real or
 * effective one, and the effective ID to any of its three. The saved ID takes
 * the new effective one when the real ID is set, or when the effective ID is
 * set to other than the old real one. -EINVAL for an ID the namespace does not
 * map, -EPERM.
 */
long sys_setreuid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t v[2];
    uint32_t r, e, new_r, new_e;
    uint32_t old_euid = p->euid;

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
    if (r != SC_ID_KEEP || (e != SC_ID_KEEP && new_e != p->ruid)) {
        p->suid = new_e;
    }
    p->ruid = new_r;
    p->euid = new_e;
    cred_moved(p, old_euid, p->egid);
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
    cred_moved(p, p->euid, old_egid);
    return 0;
}

/**
 * setresuid takes a real, an effective and a saved user ID, -1 keeping any of
 * them. A caller that is not privileged (`id_privileged`) may set each only to
 * one of its current three. -EINVAL for an ID the namespace does not map,
 * -EPERM.
 */
long sys_setresuid(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t v[3];
    uint32_t r, e, s;
    uint32_t old_euid = p->euid;

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
    cred_moved(p, p->euid, old_egid);
    return 0;
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

/* The resources: six are kept in the record and enforced, and the rest
 * are unlimited. */
#define RLIMIT_FSIZE_RES 1

#define RLIMIT_STACK_RES 3

#define RLIMIT_CORE_RES 4

#define RLIMIT_NPROC_RES 6

#define RLIMIT_NOFILE_RES 7

#define RLIMIT_AS_RES 9

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
 * by anyone, and RLIMIT_NOFILE never above FD_TABLE_LIMIT. A limit that is
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
    if (slot == RLIM_NOFILE && max > FD_TABLE_LIMIT) {
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
 * the record's own width, with RLIM_INFINITY all ones in either; -EFAULT. */
long sys_getrlimit(isr_params_t *params)
{
    uint32_t resource = (uint32_t)SC_ARG(params, 0);
    uintptr_t rl = SC_ARG(params, 1);
    unsigned long out[2];

    if (!rl) {
        return -EFAULT;
    }
    rlimit_for(current_proc, resource, &out[0], &out[1]);
    if (copy_to_user((void __user *)rl, out, sizeof(out)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** setrlimit: limits in the form getrlimit writes, set by `rlimit_set`;
 * -EFAULT. */
long sys_setrlimit(isr_params_t *params)
{
    uint32_t resource = (uint32_t)SC_ARG(params, 0);
    uintptr_t rl = SC_ARG(params, 1);
    unsigned long in[2];

    if (copy_from_user(in, (const void __user *)rl, sizeof(in)) != 0) {
        return -EFAULT;
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
 * the error of `rlimit_target` or of `rlimit_set`, or -EFAULT. This is the
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

#define MAP_FIXED_NOREPLACE 0x100000

/* Round 'v' up to the next page boundary. */
#define PAGE_UP(v) (((v) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

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
 * the new top and frees its pages.
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
        if (vm_insert(vm, old_top, new_top, VM_PROT_READ | VM_PROT_WRITE, VM_HEAP) != 0) {
            return (long)vm->brk;
        }
    } else if (new_top < old_top) {
        if (vm_remove(vm, new_top, old_top) != 0) {
            return (long)vm->brk;
        }
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
         (int)current_proc->pid, current_proc->comm, VMA_LIMIT);
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

/**
 * Whether a page of a file on `mnt` can be read where a user page fault
 * reads it, with interrupts off and outside any system call. A file in
 * memory or on the root window can, and so can one on a disk the block
 * layer polls there. A loop device, which reads through the file system
 * that holds its file and sleeps its caller, and a USB stick, which answers
 * only through its interrupts, cannot: `block_request` stops the machine
 * when a fault reaches either.
 */
static int mmap_fault_readable(const struct vfs_mount *mnt)
{
    uint32_t flags;

    if (!mnt->dev) {
        return 1;
    }
    flags = block_flags(mnt->dev);
    return (flags & BLOCK_FAULT_SAFE) || ((flags & BLOCK_POLLABLE) && !(flags & BLOCK_CALLER_SYNC));
}

/** 0 when the descriptor `e` may be mapped, shared or not, with the
 * protection `prot`: ENODEV for what is not a file or is on a device a page
 * fault cannot read (`mmap_fault_readable`), since the first touch of the
 * mapping would read it there, and EACCES for a descriptor not open for
 * reading, or for a writable shared mapping, not open for writing. */
static int32_t mmap_file_check(const fd_entry_t *e, int shared, uint32_t prot)
{
    if (e->type != FD_TYPE_FILE || !e->ofd || !e->ofd->vmnt || !mmap_fault_readable(e->ofd->vmnt)) {
        return -ENODEV;
    }
    if (!(e->flags & FD_FLAG_READ) ||
        (shared && (prot & VM_PROT_WRITE) && !(e->flags & FD_FLAG_WRITE))) {
        return -EACCES;
    }
    return 0;
}

/** Whether a shared mapping through `e` maps its file's pages in memory
 * (`filemap_disk_shared`): those of a file on ext2 opened for writing, or of
 * one such a mapping maps already. */
static int mmap_disk_shares(const fd_entry_t *e)
{
    return e->ofd->vmnt->type == &vfs_ext2_type &&
           ((e->flags & FD_FLAG_WRITE) || filemap_disk_mapped(e->ofd->vmnt, e->ofd->ino));
}

/**
 * The file half of `mmap_backing`: the file the descriptor `e`, number `fd`, is
 * open on, mapped shared or not. A private mapping of it is VM_FILE, and a
 * shared mapping of a file in memory is VM_SHARED, the file's own pages; a
 * descriptor that fails `mmap_file_check` is not mapped. A shared mapping of a
 * file on ext2 is VM_SHARED too, the file's pages in memory, which its stores
 * are written back from (`filemap_disk_shared`), when the descriptor is open
 * for writing or another shared mapping has the pages in memory already:
 * ext2's inode number names one file for as long as anything holds it, which
 * is what the pages are found by. Through a descriptor open for writing, such
 * a mapping of a file that backs a loop device fails with EBUSY, as write(2)
 * to the file does. A shared mapping of a file of another disk file system,
 * whose inode numbers follow names, is served as a private one when it is
 * read-only, which reads the same bytes, and fails with ENODEV when it may be
 * written. A shared mapping served as a private one, or made through a
 * descriptor not open for writing, is given VM_PROT_NOWRITE, so that mprotect
 * cannot make it writable later. It may sleep, finding the file's path.
 */
static int32_t mmap_file(fd_entry_t *e, int fd, int shared, struct map_source *src)
{
    struct shmem *sh;
    int disk;
    int32_t rc = mmap_file_check(e, shared, src->prot);

    if (rc != 0) {
        return rc;
    }
    sh = (shared && e->ofd->vmnt->type == &vfs_tmpfs_type) ? tmpfs_shmem(e->ofd->vnode) : NULL;
    disk = shared && mmap_disk_shares(e);
    if (shared && !sh && !disk && (src->prot & VM_PROT_WRITE)) {
        return -ENODEV;
    }
    if (disk && (e->flags & FD_FLAG_WRITE) && loop_backs(e->ofd->vmnt, e->ofd->vnode)) {
        return -EBUSY;
    }
    if (shared && (!(e->flags & FD_FLAG_WRITE) || (!sh && !disk))) {
        src->prot |= VM_PROT_NOWRITE;
    }
    src->file = sh ? filemap_shared(e, sh) : disk ? filemap_disk_shared(e) : filemap_new(e);
    src->type = sh || disk ? VM_SHARED : VM_FILE;
    if (src->file) {
        src->file->path = fd_path_copy(fd);
    }
    return src->file ? 0 : -ENOMEM;
}

/**
 * A shared mapping of `len` bytes of /dev/mem from the page offset in `src`:
 * a shared region marked VM_PROT_IO, whose pages `mmap_devmem_fill` maps,
 * for a privileged caller, of the frames /dev/mem's reads reach. EPERM for
 * another caller or another frame, EINVAL for a private mapping, ENOMEM
 * with no memory for the region's object.
 */
static int32_t mmap_devmem(uint32_t flags, uintptr_t len, struct map_source *src)
{
    uint32_t i;

    if ((flags & MAP_TYPE) == MAP_PRIVATE) {
        return -EINVAL;
    }
    if (!proc_privileged(current_proc) || src->pgoff > 0x100000u - len / PAGE_SIZE) {
        return -EPERM;
    }
    for (i = 0; i < len / PAGE_SIZE; i++) {
        uint32_t frame = src->pgoff + i;

        if (frame >= 0x100u && !paging_frame_is_device((uintptr_t)frame * PAGE_SIZE)) {
            return -EPERM;
        }
    }
    src->file = filemap_anon_shared();
    src->type = VM_SHARED;
    src->prot |= VM_PROT_IO;
    return src->file ? 0 : -ENOMEM;
}

/** Map the frames of the /dev/mem region at `base`, `len` bytes from the
 * page offset `pgoff`, uncached, then give them the region's protection
 * `prot`. Each mapping takes a reference, which a frame the allocator counts,
 * one of the first megabyte, keeps while unmapping drops it; the others have
 * no count. 0, or -ENOMEM with no memory for a page table. */
static int32_t mmap_devmem_fill(uintptr_t base, uintptr_t len, uint32_t pgoff, uint32_t prot)
{
    uint32_t i;

    for (i = 0; i < len / PAGE_SIZE; i++) {
        /* present, writable, user, write-through and cache-disabled */
        if (map_user_frame(_curr_dir, base + i * PAGE_SIZE, (pgoff + i) * PAGE_SIZE,
                           PTE_P | PTE_W | PTE_U | PTE_NOCACHE) != 0) {
            return -ENOMEM;
        }
        frame_ref((pgoff + i) * PAGE_SIZE);
    }
    paging_protect_range(_curr_dir, base, base + len, prot | VM_PROT_SHARED);
    return 0;
}

/**
 * The file or shared memory a new mapping of `len` bytes maps, with the
 * region kind it makes, into `src`, whose protection and page offset the
 * caller set from its arguments: a mapping of a file (`mmap_file`), shared
 * anonymous memory (VM_SHARED), device memory (`mmap_devmem`), or NULL for
 * private anonymous memory (VM_ANON). /dev/zero maps as anonymous memory.
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
        if (devfs_rdev(e) == ((1u << 8) | 1u)) { /* /dev/mem, 1:1 */
            return mmap_devmem(flags, len, src);
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
 * Where a new mapping of `len` bytes goes, in `*base`: `addr` itself under
 * MAP_FIXED or MAP_FIXED_NOREPLACE, which fails with EEXIST for a range in
 * use; otherwise a page-aligned hint whose range is free below the
 * stack's growth window, and without one the highest free range below
 * USER_MMAP_BASE and above the heap's top, so the ranges munmap frees are used
 * again. RLIMIT_AS counts the range, less what a fixed mapping replaces.
 * 0, -ENOMEM or -EEXIST. Nothing here sleeps, so the range is still as it
 * was found when the caller inserts the region straight after.
 */
static int32_t mmap_place(vm_space_t *vm, uintptr_t addr, uintptr_t len, uint32_t flags,
                          uintptr_t *base)
{
    unsigned long limit = current_proc->rlim[RLIM_AS].cur;

    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        if (addr < USER_ADDR_MIN || addr > USER_ADDR_MAX - len) {
            return -ENOMEM;
        }
        if ((flags & MAP_FIXED_NOREPLACE) && vm_overlaps(vm, addr, addr + len)) {
            return -EEXIST;
        }
        *base = addr;
        return vm_total(vm) - vm_covered(vm, addr, addr + len) + len > limit ? -ENOMEM : 0;
    }
    *base = 0;
    if (addr != 0 && (addr & (PAGE_SIZE - 1)) == 0 && addr >= USER_ADDR_MIN &&
        len <= vm->stack_limit && addr <= vm->stack_limit - len &&
        !vm_overlaps(vm, addr, addr + len)) {
        *base = addr;
    }
    if (*base == 0) {
        *base = vm_find_free(vm, len, PAGE_UP(vm->brk), USER_MMAP_BASE);
    }
    return *base == 0 || vm_total(vm) + len > limit ? -ENOMEM : 0;
}

/**
 * Put the region `src` describes, `len` bytes, where `mmap_place` finds for
 * `addr` and `flags`, replacing what a MAP_FIXED range held, and map a
 * /dev/mem region's frames at once. The region takes its own reference to
 * the file, and the caller's is put here. Returns the address, or -errno.
 * Under MAP_FIXED the two entries that a split in the removal and the new
 * region take are reserved before the old mapping is removed, so that a
 * failure leaves the old mapping in place.
 */
static long mmap_install(uintptr_t addr, uintptr_t len, uint32_t flags, struct map_source *src)
{
    vm_space_t *vm = &current_proc->mm->vm;
    uintptr_t base = 0;
    int32_t rc = mmap_place(vm, addr, len, flags, &base);

    if (rc == 0 && (flags & MAP_FIXED)) {
        if (vm_reserve(vm, 2) == 0) {
            vm_remove(vm, base, base + len);
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
    } else if (rc == 0 && (src->prot & VM_PROT_IO) &&
               mmap_devmem_fill(base, len, src->pgoff, src->prot) != 0) {
        vm_remove(vm, base, base + len);
        unmap_user_range(_curr_dir, base, base + len);
        rc = -ENOMEM;
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
 * file, private or shared, or /dev/mem; where it goes is `mmap_place`'s. The
 * backing is found first, since finding it may sleep, and the place after, so
 * that nothing sleeps between the checks on the range and the insertion: a
 * thread sharing the address space cannot map the range meanwhile, and
 * MAP_FIXED_NOREPLACE cannot replace what it put there. MAP_FIXED replaces
 * whatever the range held. Pages are backed on first touch: zeroed, read from
 * the file, or the shared page itself.
 */
long ksys_mmap_pgoff(uintptr_t addr, uintptr_t length, uint32_t prot, uint32_t flags, int fd,
                     unsigned long pgoff)
{
    struct map_source src;
    uintptr_t len;
    int32_t rc;

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
    if (length > USER_ADDR_MAX - USER_ADDR_MIN) {
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
    mem_balance();
    src.prot = vm_prot_of(prot);
    src.pgoff = (uint32_t)pgoff;
    rc = mmap_backing(flags, fd, len, &src);
    if (rc != 0) {
        return rc;
    }
    return mmap_install(addr, len, flags, &src);
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

int32_t munmap_shm(uintptr_t addr, const struct shmem *sh, size_t size)
{
    vm_space_t *vm = &current_proc->mm->vm;
    vma_t *r = vm_find(vm, addr);
    uintptr_t end = addr;

    if (!r || r->start != addr || r->pgoff != 0 || filemap_shmem(r->file) != sh) {
        return -EINVAL;
    }
    while (r && r->start == end && filemap_shmem(r->file) == sh && end - addr < PAGE_UP(size)) {
        end = r->end;
        r = vm_find(vm, end);
    }
    if (vm_remove(vm, addr, end) != 0) {
        vma_exhausted("shmdt");
        return -ENOMEM;
    }
    unmap_user_range(_curr_dir, addr, end);
    vm_file_reap();
    return 0;
}

/** An address space is counted at the first process of `proc_list` that has
 * it. */
uint32_t shm_attaches(const struct shmem *sh)
{
    const process_t *p, *q;
    uint32_t i, n = 0;

    for (p = proc_list; p; p = p->next) {
        for (q = proc_list; p->mm && q != p && q->mm != p->mm; q = q->next) {
        }
        for (i = 0; p->mm && q == p && i < p->mm->vm.count; i++) {
            const vma_t *r = &p->mm->vm.vmas[i];

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
 * -ENOMEM when a split finds no room for its region.
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
    vm_file_reap();
    return 0;
}

/** Bring the resident pages of [start, end) to `prot`, region by region, so a
 * shared region's pages stay shared. */
static void protect_resident(uintptr_t start, uintptr_t end, uint32_t prot)
{
    while (start < end) {
        vma_t *r = vm_find(&current_proc->mm->vm, start);
        uintptr_t stop = (r && r->end < end) ? r->end : end;

        paging_protect_range(_curr_dir, start, stop,
                             prot | ((r && r->type == VM_SHARED) ? VM_PROT_SHARED : 0u));
        start = stop;
    }
}

/**
 * Change the protection of a range (mprotect(2)). Its arguments are addr
 * (page aligned), length and prot. Returns 0, or -errno: EINVAL for an
 * unaligned address or an unknown protection bit, ENOMEM when a page of the
 * range is not mapped or a split finds no room for its region, EACCES for write
 * asked of a shared mapping that may never be written (the VM_PROT_NOWRITE
 * `mmap_file` and `mmap_shm` give), with nothing changed. The regions take the
 * new protection first and the resident pages after, so a fault in between
 * reads the protection the call is setting.
 */
long sys_mprotect(isr_params_t *params)
{
    uintptr_t addr = SC_ARG(params, 0);
    uintptr_t length = SC_ARG(params, 1);
    uint32_t prot = (uint32_t)SC_ARG(params, 2);
    uintptr_t end;
    uint32_t vmprot;
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
    if (rc == -3) {
        return -EACCES;
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

/** Give [addr, addr + len), which lies in the region `r`, a region at `to`
 * of `new_len` bytes, moving the resident pages there. Returns `to`, or
 * -ENOMEM with no room for the regions or a page table. The file the region
 * maps is held across the move, which removes the old region's hold before
 * the new region takes its own. */
static long remap_move(vm_space_t *vm, vma_t *r, uintptr_t addr, uintptr_t len, uintptr_t to,
                       uintptr_t new_len)
{
    struct vm_file *file = r->file;
    uint32_t pgoff = r->pgoff + (uint32_t)((addr - r->start) / PAGE_SIZE);
    uint32_t prot = r->prot;
    vm_type_t type = r->type;
    uintptr_t moved = len < new_len ? len : new_len;

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

    if ((to & (PAGE_SIZE - 1)) != 0 || to < USER_ADDR_MIN || to > USER_ADDR_MAX - new_len ||
        (to < addr + old_len && addr < to + new_len)) {
        return -EINVAL;
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
    unmap_user_range(_curr_dir, to, to + new_len);
    return remap_move(vm, vm_find(vm, addr), addr, old_len, to, new_len);
}

/**
 * Whether mremap's arguments name ranges it can work on: a page-aligned
 * address and two lengths that are not zero, flags it knows with
 * MREMAP_FIXED only beside MREMAP_MAYMOVE, an old range that ends inside
 * user space and a new length that fits in it. The lengths are judged as
 * they are given, before they are rounded up to pages, so that neither
 * range can wrap past the end of the address space: with an old length
 * near 4 GiB the old range would seem to end below its start, and a fixed
 * move onto its own address would remove its source.
 */
static int remap_args_valid(uintptr_t addr, uintptr_t old_len, uintptr_t new_len, uint32_t flags)
{
    return (addr & (PAGE_SIZE - 1)) == 0 && old_len != 0 && new_len != 0 &&
           (flags & ~(uint32_t)(MREMAP_MAYMOVE | MREMAP_FIXED)) == 0 &&
           (!(flags & MREMAP_FIXED) || (flags & MREMAP_MAYMOVE)) && addr <= USER_ADDR_MAX &&
           old_len <= USER_ADDR_MAX - addr && new_len <= USER_ADDR_MAX - USER_ADDR_MIN;
}

/**
 * Resize a mapping, or move it (mremap(2)). Its arguments are old address,
 * old length, new length, flags and new address with MREMAP_FIXED. The old
 * range must lie in one region. With MREMAP_FIXED the mapping moves, pages
 * and all, to the new address, whatever was there going first
 * (`remap_fixed`). Otherwise a shrink cuts its end, and a growth takes the
 * range after it when that is free, or with MREMAP_MAYMOVE moves the whole
 * mapping to a free range of the new length. Returns the mapping's address,
 * or -errno: EINVAL for arguments that fail `remap_args_valid`, and for a
 * mapping of a file whose last page at the new length would not fit in 32
 * bits; EFAULT for a range not in one region, or for a growth of device
 * memory; ENOMEM when it cannot grow in place and may not move, when no range
 * is free, or past RLIMIT_AS. MREMAP_DONTUNMAP is not implemented, and is
 * EINVAL.
 */
long sys_mremap(isr_params_t *params)
{
    uintptr_t addr = SC_ARG(params, 0);
    uint32_t flags = (uint32_t)SC_ARG(params, 3);
    uintptr_t to = SC_ARG(params, 4);
    uintptr_t old_len, new_len;
    vm_space_t *vm;
    vma_t *r;
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
    if (!r || addr + old_len > r->end) {
        return -EFAULT;
    }
    if (r->file && new_len / PAGE_SIZE > 0xFFFFFFFFu - (r->pgoff + (addr - r->start) / PAGE_SIZE)) {
        return -EINVAL;
    }
    if ((r->prot & VM_PROT_IO) && new_len > old_len) {
        return -EFAULT; /* device memory does not grow, as Linux's VM_DONTEXPAND */
    }
    if (flags & MREMAP_FIXED) {
        rc = remap_fixed(vm, addr, old_len, to, new_len);
        vm_file_reap();
        return rc;
    }
    if (new_len <= old_len) {
        if (vm_remove(vm, addr + new_len, addr + old_len) != 0) {
            vma_exhausted("mremap");
            return -ENOMEM;
        }
        unmap_user_range(_curr_dir, addr + new_len, addr + old_len);
        vm_file_reap();
        return (long)addr;
    }
    if (vm_total(vm) + (new_len - old_len) > current_proc->rlim[RLIM_AS].cur) {
        return -ENOMEM;
    }
    if (addr + old_len == r->end && addr <= USER_ADDR_MAX - new_len &&
        addr + new_len <= vm->stack_limit && !vm_overlaps(vm, addr + old_len, addr + new_len)) {
        if (vm_insert_file(vm, addr + old_len, addr + new_len, r->prot, r->type, r->file,
                           r->pgoff + (uint32_t)((addr + old_len - r->start) / PAGE_SIZE)) != 0) {
            vma_exhausted("mremap");
            return -ENOMEM;
        }
        return (long)addr;
    }
    if (!(flags & MREMAP_MAYMOVE)) {
        return -ENOMEM;
    }
    to = vm_find_free(vm, new_len, PAGE_UP(vm->brk), USER_MMAP_BASE);
    if (to == 0) {
        return -ENOMEM;
    }
    rc = remap_move(vm, r, addr, old_len, to, new_len);
    vm_file_reap();
    return rc;
}

#define MADV_DONTNEED        4
#define MADV_FREE            8
#define MADV_REMOVE          9
#define MADV_DONTFORK        10
#define MADV_DONTNEED_LOCKED 24
#define MADV_COLLAPSE        25

/**
 * Whether `sys_madvise` takes `advice`: one Linux defines from MADV_NORMAL (0)
 * to MADV_COLLAPSE, except MADV_REMOVE, which would free the blocks of a file
 * under a shared mapping and is not implemented. Linux leaves 5 to 7 undefined,
 * and its advice from MADV_HWPOISON (100) up injects memory faults or installs
 * guard pages, neither of which Rach2 has.
 */
static int madvise_known(uint32_t advice)
{
    return advice <= MADV_DONTNEED || advice == MADV_FREE ||
           (advice >= MADV_DONTFORK && advice <= MADV_COLLAPSE);
}

/**
 * Advice about a range (madvise(2)). Its arguments are address, length and
 * advice. MADV_DONTNEED, MADV_DONTNEED_LOCKED and MADV_FREE let the resident
 * pages go: a private anonymous page reads as zeros again, a private page of a
 * file reads the file again, and a shared page reads what the shared memory
 * holds. Those three are EINVAL over device memory, which has no pages to read
 * again, as on Linux for VM_PFNMAP. Any other advice `madvise_known` takes
 * changes nothing, and the rest is EINVAL, as Linux refuses an advice it does
 * not define. A range with a page in no region is ENOMEM, as in Linux. A range
 * that runs past user space is EINVAL, judged by the length as given: a length
 * within a page of 4 GiB rounds up to 0, which would pass for an empty range.
 */
long sys_madvise(isr_params_t *params)
{
    uintptr_t addr = SC_ARG(params, 0);
    uint32_t advice = (uint32_t)SC_ARG(params, 2);
    uintptr_t len;

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    if ((addr & (PAGE_SIZE - 1)) != 0 || addr > USER_ADDR_MAX ||
        SC_ARG(params, 1) > USER_ADDR_MAX - addr || !madvise_known(advice)) {
        return -EINVAL;
    }
    len = PAGE_UP(SC_ARG(params, 1));
    if (len == 0) {
        return 0;
    }
    if (vm_covered(&current_proc->mm->vm, addr, addr + len) != len) {
        return -ENOMEM;
    }
    if (advice == MADV_DONTNEED || advice == MADV_DONTNEED_LOCKED || advice == MADV_FREE) {
        uintptr_t at;

        for (at = addr; at < addr + len; at = vm_find(&current_proc->mm->vm, at)->end) {
            if (vm_find(&current_proc->mm->vm, at)->prot & VM_PROT_IO) {
                return -EINVAL;
            }
        }
        unmap_user_range(_curr_dir, addr, addr + len);
    }
    return 0;
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

/**
 * Set a priority (setpriority).
 *
 * Its arguments are which, who and the nice value, clamped to -20 through 19 as
 * Linux clamps it. Each target is changed on Linux's terms: a caller whose
 * effective uid is neither the target's real nor its effective uid gets -EPERM,
 * and one that would lower the target's nice value, raising its priority, gets
 * -EACCES, since `RLIMIT_NICE` is 0; effective uid 0 may do both. The targets
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

/* The scheduling policies Linux numbers. Only SCHED_OTHER runs here; the
 * others are defined so that a call about each returns the error or the value
 * Linux returns for it. */
#define SCHED_OTHER 0

#define SCHED_FIFO 1

#define SCHED_RR 2

#define SCHED_BATCH 3

#define SCHED_IDLE 5

#define SCHED_DEADLINE 6

#define SCHED_EXT 7

/* The flag of a policy that makes a fork not pass the policy on; accepted
 * with SCHED_OTHER, for which it changes nothing here. */
#define SCHED_RESET_ON_FORK 0x40000000

/* The highest real-time priority Linux gives SCHED_FIFO and SCHED_RR. */
#define SCHED_RT_PRIO_MAX 99

/**
 * The bytes of a CPU mask as Linux sizes the one it copies: a long for
 * every bits-of-a-long processors up to the highest started.
 */
static unsigned long cpu_mask_bytes(void)
{
    unsigned long bits = sizeof(unsigned long) * 8u;
    unsigned long highest = 0;
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (cpu_table[c].online) {
            highest = (unsigned long)c;
        }
    }
    return (highest / bits + 1u) * sizeof(unsigned long);
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
    schedule();
    return 0;
}

/**
 * Change a process's policy and priority (sched_setscheduler(2)), or its
 * priority alone (sched_setparam, which passes `policy` -1).
 *
 * `uparam` points at a struct sched_param, one int. SCHED_OTHER with priority
 * 0 is the one setting a process has here, and a request for it succeeds for
 * a caller that may change the target; SCHED_RESET_ON_FORK beside it is
 * accepted and kept nowhere, since a fork has no other policy to reset. The
 * rest fails in Linux's order and with its errors: no parameter or a negative
 * pid, -EINVAL; an unreadable parameter, -EFAULT; no such process, -ESRCH; a
 * priority outside 0 to 99, or one that does not suit the policy, -EINVAL, as
 * for SCHED_DEADLINE, which takes its parameters from sched_setattr alone;
 * SCHED_FIFO and SCHED_RR, -EPERM, as Linux returns to a caller without
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

/**
 * A process's policy (sched_getscheduler): SCHED_OTHER, the
 * one every process has. -EINVAL for a negative pid, -ESRCH for no process.
 */
long sys_sched_getscheduler(isr_params_t *params)
{
    int pid = (int)SC_ARG(params, 0);

    if (pid < 0) {
        return -EINVAL;
    }
    return sched_target(pid) ? SCHED_OTHER : -ESRCH;
}

/**
 * A process's scheduling parameter (sched_getparam): the
 * priority 0 of SCHED_OTHER, written to the struct sched_param the second
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
 * two longs): one tick, 10 ms, since the timer takes the processor from every
 * process at each tick. -EINVAL for a negative pid, -ESRCH, -EFAULT.
 */
long sys_sched_rr_get_interval(isr_params_t *params)
{
    int pid = (int)SC_ARG(params, 0);
    struct kernel_timespec slice;

    if (pid < 0) {
        return -EINVAL;
    }
    if (!sched_target(pid)) {
        return -ESRCH;
    }
    slice.tv_sec = 0;
    slice.tv_nsec = 10000000;
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
 * set. -EINVAL for a length shorter than that or not a multiple of a long,
 * -ESRCH for no process, -EFAULT.
 */
long sys_sched_getaffinity(isr_params_t *params)
{
    unsigned long len = SC_ARG(params, 1);
    unsigned long bytes = cpu_mask_bytes();
    process_t *p;
    uint64_t mask;

    if (len < bytes || (len & (sizeof(unsigned long) - 1u)) != 0) {
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

/** Whether [`addr`, `addr` + `len`) is a range the current process's
 * regions cover without a gap, as mlock requires: -EINVAL for a range that
 * wraps past the end of the address space, -ENOMEM for a gap, 0. */
static int32_t lock_range(uintptr_t addr, uintptr_t len)
{
    uintptr_t start = addr & ~(uintptr_t)(PAGE_SIZE - 1);
    uintptr_t end = addr + len;

    if (end < addr) {
        return -EINVAL;
    }
    end = (end + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
    if (end < start) {
        return -EINVAL;
    }
    return vm_covered(&current_proc->mm->vm, start, end) == end - start ? 0 : -ENOMEM;
}

/**
 * mlock, munlock and mlock2: lock or unlock the pages of a
 * range, which must be mapped without a gap. There is no swap, so every page
 * already stays in memory; the calls check the range, and mlock2 its flags,
 * 0 or MLOCK_ONFAULT, and change nothing. RLIMIT_MEMLOCK is not kept.
 */
long sys_mlock(isr_params_t *params)
{
    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    return lock_range(SC_ARG(params, 0), SC_ARG(params, 1));
}

/** munlock: mlock's check of the range. */
long sys_munlock(isr_params_t *params)
{
    return sys_mlock(params);
}

/** mlock2: mlock's check of the range, after its flags; -EINVAL for a flag
 * other than MLOCK_ONFAULT (1). */
long sys_mlock2(isr_params_t *params)
{
    if (SC_ARG(params, 2) & ~1u) {
        return -EINVAL;
    }
    return sys_mlock(params);
}

/**
 * mlockall and munlockall: lock or unlock every page, which the
 * machine keeps in memory anyway. mlockall's flags are any of MCL_CURRENT (1),
 * MCL_FUTURE (2) and MCL_ONFAULT (4), but not MCL_ONFAULT alone and not none;
 * -EINVAL otherwise.
 */
long sys_mlockall(isr_params_t *params)
{
    uint32_t flags = (uint32_t)SC_ARG(params, 0);

    if (flags == 0 || (flags & ~7u) || flags == 4u) {
        return -EINVAL;
    }
    return 0;
}

/** munlockall: every page stays in memory, and the call returns 0. */
long sys_munlockall(isr_params_t *params)
{
    (void)params;
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
    uintptr_t i;
    int32_t rc;

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    if (addr & (PAGE_SIZE - 1)) {
        return -EINVAL;
    }
    rc = lock_range(addr, SC_ARG(params, 1));
    if (rc != 0) {
        return rc;
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
 * -EINVAL; the flag bits above it are kept and change nothing. This kernel
 * does not randomize addresses, so ADDR_NO_RANDOMIZE, which GDB sets, has
 * nothing to turn off.
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
 * -ESRCH, then -EPERM for a caller that may not change the target, in
 * Linux's order.
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

#define PR_SET_NO_NEW_PRIVS 38

#define PR_GET_NO_NEW_PRIVS 39

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
 * PR_SET_NO_NEW_PRIVS, with the second argument 1 and the rest 0, sets
 * no_new_privs for good: fork passes it on and exec then ignores set-id bits.
 * PR_GET_NO_NEW_PRIVS, with every argument 0, returns it.
 *
 * PR_GET_SECCOMP returns the thread's seccomp mode, and PR_SET_SECCOMP sets
 * the strict mode, or adds the filter at the third argument, as seccomp(2)
 * does.
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
    default:
        return -EINVAL;
    }
}

/* The versions of capget's and capset's header: the first has one word of
 * each set, the other two two words. */
#define CAP_VERSION_1 0x19980330u

#define CAP_VERSION_2 0x20071026u

#define CAP_VERSION_3 0x20080522u

/* The capabilities Linux names, 0 to 40; the second word of a set has the
 * nine above 31. */
#define CAP_HIGH_WORD 0x1FFu

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

/**
 * Word `i` of the capability sets `p` holds, effective and permitted alike:
 * in the first user namespace every capability for a privileged thread and
 * none for another; in another namespace the sets it was given there, or
 * every one for a thread whose effective uid is root there, and none after
 * it gave them up.
 */
static uint32_t cap_held(const process_t *p, uint32_t i)
{
    uint32_t full = i == 0 ? 0xFFFFFFFFu : CAP_HIGH_WORD;

    if (!p->userns) {
        return proc_privileged(p) ? full : 0u;
    }
    if (p->nocaps) {
        return 0u;
    }
    if (ns_caps_held(p)) {
        return p->ns_caps[i] & full;
    }
    return ns_uid_from_kernel(p->userns, p->euid) == 0 ? full : 0u;
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
 * The sets of the thread the header's pid names, the caller for 0, as
 * `cap_held` gives them; the inheritable set is always empty. With a null
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
    d[0].effective = d[0].permitted = cap_held(p, 0);
    d[1].effective = d[1].permitted = cap_held(p, 1);
    if (copy_to_user((void __user *)SC_ARG(params, 1), d, words * sizeof(d[0])) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * capset takes header and the sets' words.
 *
 * The kernel keeps every capability or none, so the calling thread may give
 * up all its capabilities, with every set empty, after which it is not
 * privileged until an exec gives them back (`exec_ids`), and may ask for all
 * it has, which changes nothing. Chromium's zygote gives them up in each
 * process it forks. In a user namespace other than the first, a thread may
 * keep any part of what it holds there, as Chromium's zygote keeps
 * CAP_SYS_ADMIN: capget reports the part, and the thread stays privileged
 * there while any is left (`ns_caps_held`). A pid other than 0 and the
 * caller's is -EPERM, as is a permitted set wider than the thread's or an
 * effective set wider than the permitted one; in the first namespace a set
 * between none and all, or any inheritable capability, is -EINVAL, where
 * Linux keeps it; an unknown version is -EINVAL, and memory that cannot be
 * read -EFAULT.
 */
long sys_capset(isr_params_t *params)
{
    struct cap_data d[2];
    uint32_t words, i, any = 0, whole = 1;
    int32_t pid;
    int32_t rc = cap_version_in(SC_ARG(params, 0), &words);

    if (rc != 0) {
        return rc;
    }
    rc = cap_pid_in(SC_ARG(params, 0), &pid);
    if (rc != 0) {
        return rc;
    }
    if (pid != 0 && pid != (int32_t)pid_seen(current_proc->pid)) {
        return -EPERM;
    }
    k_memset(d, 0, sizeof(d));
    if (!SC_ARG(params, 1) ||
        copy_from_user(d, (const void __user *)SC_ARG(params, 1), words * sizeof(d[0])) != 0) {
        return -EFAULT;
    }
    for (i = 0; i < 2; i++) {
        uint32_t full = i == 0 ? 0xFFFFFFFFu : CAP_HIGH_WORD;

        if ((d[i].permitted & ~cap_held(current_proc, i)) != 0 ||
            (d[i].effective & ~d[i].permitted) != 0) {
            return -EPERM;
        }
        any |= d[i].effective | d[i].permitted | d[i].inheritable;
        whole &= d[i].effective == full && d[i].permitted == full && d[i].inheritable == 0;
    }
    if (any == 0) {
        current_proc->nocaps = 1;
        return 0;
    }
    if (!current_proc->userns && !whole) {
        return -EINVAL;
    }
    if (current_proc->userns) {
        current_proc->ns_caps[0] = d[0].permitted;
        current_proc->ns_caps[1] = d[1].permitted;
    }
    return 0;
}
