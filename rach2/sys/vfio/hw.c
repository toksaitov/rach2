/*
 * hw.c: what VFIO takes from the rest of the kernel beyond the interfaces
 * it includes: sleeps, the caller's address space and descriptors, maps of
 * firmware memory and the boot line. A host test gives its own.
 */

#include "vfio.h"

#include "../cmdline.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../lib/kerrno.h"
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
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_ANON;
    e->ops = ops;
    e->priv = priv;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE | FD_FLAG_CLOEXEC;
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

int vfio_hw_option(const char *word)
{
    return kcmdline_has(word);
}
