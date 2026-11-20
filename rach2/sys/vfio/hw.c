/*
 * hw.c: what VFIO takes from the rest of the kernel beyond the interfaces
 * it includes: sleeps, the interrupt worker, the caller's address space
 * and descriptors, maps of firmware memory and the boot line.
 */

#include "vfio.h"

#include "../arch/arch.h"
#include "../cmdline.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../mm/paging.h"
#include "../mm/pressure.h"
#include "../proc/percpu.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/wait_queue.h"
#include "../timer.h"
#include "../udelay.h"

/** The tick the deadline at `arg` names has come. */
static int tick_reached(void *arg)
{
    return tick_count() >= *(const uint64_t *)arg;
}

/** Wait `ms` milliseconds at least: a process sleeps to the tick after,
 * giving up the big lock, as AcpiOsSleep does; the boot spins. */
void vfio_hw_msleep(unsigned int ms)
{
    if (sched_is_enabled() && !in_interrupt() && current_proc != NULL &&
        current_proc != proc_idle && current_proc != this_cpu()->idle) {
        wait_queue_t wq;
        uint64_t target = tick_count() + tick_span(ms / 1000u, ms % 1000u * 1000000u) + 1u;

        wait_queue_init(&wq);
        current_proc->wakeup_tick = target;
        (void)wq_wait_event_nointr(&wq, tick_reached, &target);
        current_proc->wakeup_tick = 0;
        return;
    }
    while (ms-- > 0) {
        k_udelay(1000);
    }
}

static wait_queue_t irq_wait;
static process_t *irq_worker;

/** Serve unmask requests after their eventfd hooks have queued them. */
static void vfio_irq_thread(void)
{
    arch_irq_enable();
    for (;;) {
        (void)wq_wait_event_idle(&irq_wait, vfio_irq_pending, NULL);
        vfio_irq_work();
        sched_service_point();
    }
}

int vfio_hw_irq_start(void)
{
    process_t *p;
    uintptr_t base;
    uint32_t flags;

    if (irq_worker != NULL) {
        return 0;
    }
    flags = kirq_save();
    base = kstack_alloc();
    p = base != 0 ? alloc_process() : NULL;
    if (p == NULL) {
        if (base != 0) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        return -ENOMEM;
    }
    wait_queue_init(&irq_wait);
    p->kernel_stack = base + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = NULL;
    p->parent_pid = 0;
    proc_set_comm(p, "kvfio");
    setup_process_stack(p, vfio_irq_thread);
    irq_worker = p;
    sched_make_ready(p);
    kirq_restore(flags);
    return 0;
}

void vfio_hw_irq_wake(void)
{
    wq_wake_all(&irq_wait);
}

struct mm *vfio_hw_mm(void)
{
    return current_proc != NULL ? current_proc->mm : NULL;
}

int32_t vfio_hw_fd_new(const struct file_ops *ops, void *priv)
{
    fd_entry_t *e;
    int fd;

    if (current_proc == NULL) {
        return -ESRCH;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        return fd;
    }
    e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_ANON;
    e->ops = ops;
    e->priv = priv;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE | FD_FLAG_CLOEXEC;
    fd_install(current_proc->fds, fd);
    return fd;
}

void *vfio_hw_fd_object(int fd, const struct file_ops *ops)
{
    fd_entry_t *e;

    if (current_proc == NULL) {
        return NULL;
    }
    e = fd_get(current_proc->fds, fd);
    return e != NULL && e->ops == ops ? e->priv : NULL;
}

uintptr_t vfio_hw_map(uint64_t phys, uint32_t size)
{
    return mmio_map(phys, size, 0);
}

void vfio_hw_unmap(uintptr_t va, uint32_t size)
{
    mmio_unmap(va, size);
}

int vfio_hw_param_bool(const char *key)
{
    return kcmdline_param_bool(key, 0);
}
