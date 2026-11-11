/*
 * osl.c: ACPICA's OS layer, the AcpiOs* functions through which ACPICA
 * reaches memory, ports, PCI configuration, locks, time, the SCI and the
 * log, each over the kernel's own; and the kernel thread that runs the work
 * ACPICA defers out of the SCI.
 */

#include "../cmdline.h"
#include "../irq.h"
#include "../klog.h"
#include "../pci.h"
#include "../rtc.h"
#include "../timer.h"
#include "../udelay.h"
#include "../arch/arch.h"
#include "../arch/x86/isr.h"
#include "../arch/x86/pciconf.h"
#include "../arch/x86/portio.h"
#include "../lib/kirq.h"
#include "../lib/spinlock.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../mm/pressure.h"
#include "../proc/percpu.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/wait_queue.h"

#include "acpi.h"

#include "acpica.h"
#include "mapcache.h"
#include "osl.h"

/* The windows of memory ACPICA may have mapped at once: its tables, the
 * FACS, and the operation regions of the firmware's methods. */
#define OSL_WINDOWS 256

/* The work AcpiOsExecute may hold before its thread runs it. */
#define OSL_QUEUE 64

/* The longest line of ACPICA's output kept whole; the rest is cut. */
#define OSL_LINE_MAX 200

static uint64_t root_pointer;

/* ---- memory ---- */

/** Map whole pages for the window cache, through the kernel's MMIO region,
 * uncached, as every table and register ACPICA reads is reached. */
static uintptr_t osl_map_pages(uint64_t phys, uint64_t len)
{
    return map_mmio((uintptr_t)phys, (size_t)len);
}

static struct acpi_window windows[OSL_WINDOWS];
static struct acpi_mapcache maps = {
    windows, OSL_WINDOWS, 0, sizeof(uintptr_t) < 8 ? (uint64_t)1 << 32 : 0, 0, osl_map_pages};
static int maps_full_said;

void *AcpiOsAllocate(ACPI_SIZE size)
{
    return kmalloc((size_t)size);
}

void AcpiOsFree(void *memory)
{
    kfree(memory);
}

/**
 * The `length` bytes at `where`, mapped whole: in a window already made
 * when one holds them, since the kernel's mappings are never taken back.
 * NULL when they cannot be mapped, with a record the first time that is
 * because every window is made.
 */
void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS where, ACPI_SIZE length)
{
    uint32_t flags = kirq_save();
    uintptr_t va = acpi_mapcache_get(&maps, (uint64_t)where, (uint64_t)length);

    if (va == 0 && maps.full && !maps_full_said) {
        maps_full_said = 1;
        klog(KLOG_ERR, "acpi", "map: all %d windows made; 0x%x%08x not mapped", OSL_WINDOWS,
             (unsigned)((uint64_t)where >> 32), (unsigned)where);
    }
    kirq_restore(flags);
    return (void *)va;
}

/** Nothing: a window stays mapped for the next request of its range. */
void AcpiOsUnmapMemory(void *address, ACPI_SIZE size)
{
    (void)address;
    (void)size;
}

ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS address, UINT64 *value, UINT32 width)
{
    void *p = AcpiOsMapMemory(address, width / 8);

    if (!p) {
        return AE_NO_MEMORY;
    }
    switch (width) {
    case 8:
        *value = *(volatile UINT8 *)p;
        break;
    case 16:
        *value = *(volatile UINT16 *)p;
        break;
    case 32:
        *value = *(volatile UINT32 *)p;
        break;
    case 64:
        *value = *(volatile UINT64 *)p;
        break;
    default:
        return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS address, UINT64 value, UINT32 width)
{
    void *p = AcpiOsMapMemory(address, width / 8);

    if (!p) {
        return AE_NO_MEMORY;
    }
    switch (width) {
    case 8:
        *(volatile UINT8 *)p = (UINT8)value;
        break;
    case 16:
        *(volatile UINT16 *)p = (UINT16)value;
        break;
    case 32:
        *(volatile UINT32 *)p = (UINT32)value;
        break;
    case 64:
        *(volatile UINT64 *)p = value;
        break;
    default:
        return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

/* ---- ports and PCI configuration ---- */

ACPI_STATUS AcpiOsReadPort(ACPI_IO_ADDRESS address, UINT32 *value, UINT32 width)
{
    if (address > 0xFFFFu) {
        return AE_BAD_PARAMETER;
    }
    switch (width) {
    case 8:
        *value = inb((uint16_t)address);
        break;
    case 16:
        *value = inw((uint16_t)address);
        break;
    case 32:
        *value = ind((uint16_t)address);
        break;
    default:
        return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS address, UINT32 value, UINT32 width)
{
    if (address > 0xFFFFu) {
        return AE_BAD_PARAMETER;
    }
    switch (width) {
    case 8:
        outb((uint16_t)address, (uint8_t)value);
        break;
    case 16:
        outw((uint16_t)address, (uint16_t)value);
        break;
    case 32:
        outd((uint16_t)address, value);
        break;
    default:
        return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

/**
 * Whether `width` bits at `reg` of the function `id` names are a register
 * this kernel reaches: segment 0, and inside the function's 4 KiB, which
 * past the first 256 bytes needs the MCFG's window (pciconf.c).
 */
static int pci_reg_ok(const ACPI_PCI_ID *id, UINT32 reg, UINT32 width)
{
    if (width != 8 && width != 16 && width != 32 && width != 64) {
        return 0;
    }
    return id->Segment == 0 && id->Bus < 256 && id->Device < 32 && id->Function < 8 &&
           reg + width / 8 <= 4096;
}

ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *id, UINT32 reg, UINT64 *value, UINT32 width)
{
    uint8_t b, d, f;
    uint32_t v;

    if (!pci_reg_ok(id, reg, width)) {
        return AE_BAD_PARAMETER;
    }
    b = (uint8_t)id->Bus;
    d = (uint8_t)id->Device;
    f = (uint8_t)id->Function;
    if (width == 64) {
        UINT64 lo, hi;

        (void)AcpiOsReadPciConfiguration(id, reg, &lo, 32);
        (void)AcpiOsReadPciConfiguration(id, reg + 4, &hi, 32);
        *value = lo | (hi << 32);
        return AE_OK;
    }
    if (reg + width / 8 <= 256) {
        *value = width == 8    ? pci_read_config8(b, d, f, (uint8_t)reg)
                 : width == 16 ? pci_read_config16(b, d, f, (uint8_t)reg)
                               : pci_read_config32(b, d, f, (uint8_t)reg);
        return AE_OK;
    }
    v = pci_read_config32_ext(b, d, f, (uint16_t)(reg & ~3u)) >> ((reg & 3u) * 8);
    *value = width == 8 ? (v & 0xFFu) : width == 16 ? (v & 0xFFFFu) : v;
    return AE_OK;
}

/** A write past the first 256 bytes reads the doubleword around a narrower
 * register and writes it back whole, as the MCFG window is written. */
ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *id, UINT32 reg, UINT64 value, UINT32 width)
{
    uint8_t b, d, f;
    uint32_t v, mask, shift;

    if (!pci_reg_ok(id, reg, width)) {
        return AE_BAD_PARAMETER;
    }
    b = (uint8_t)id->Bus;
    d = (uint8_t)id->Device;
    f = (uint8_t)id->Function;
    if (width == 64) {
        (void)AcpiOsWritePciConfiguration(id, reg, value & 0xFFFFFFFFu, 32);
        return AcpiOsWritePciConfiguration(id, reg + 4, value >> 32, 32);
    }
    if (reg + width / 8 <= 256) {
        if (width == 8) {
            pci_write_config8(b, d, f, (uint8_t)reg, (uint8_t)value);
        } else if (width == 16) {
            pci_write_config16(b, d, f, (uint8_t)reg, (uint16_t)value);
        } else {
            pci_write_config32(b, d, f, (uint8_t)reg, (uint32_t)value);
        }
        return AE_OK;
    }
    shift = (reg & 3u) * 8;
    mask = width == 8 ? 0xFFu : width == 16 ? 0xFFFFu : 0xFFFFFFFFu;
    v = width == 32 ? 0 : pci_read_config32_ext(b, d, f, (uint16_t)(reg & ~3u));
    v = (v & ~(mask << shift)) | (((uint32_t)value & mask) << shift);
    pci_write_config32_ext(b, d, f, (uint16_t)(reg & ~3u), v);
    return AE_OK;
}

/* ---- locks and semaphores ---- */

ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *out)
{
    struct spinlock *lock = (struct spinlock *)kzalloc(sizeof(*lock));

    if (!lock) {
        return AE_NO_MEMORY;
    }
    spin_lock_init(lock, "acpi_lock");
    *out = lock;
    return AE_OK;
}

void AcpiOsDeleteLock(ACPI_SPINLOCK handle)
{
    kfree(handle);
}

ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK handle)
{
    return spin_lock_irqsave((struct spinlock *)handle);
}

void AcpiOsReleaseLock(ACPI_SPINLOCK handle, ACPI_CPU_FLAGS flags)
{
    spin_unlock_irqrestore((struct spinlock *)handle, (uint32_t)flags);
}

/* A counting semaphore: the units free, at most `max`, and the processes
 * that wait for more. ACPICA's mutexes are semaphores of one unit. */
struct osl_sem {
    uint32_t units;
    uint32_t max;
    wait_queue_t waiters;
};

/* Over every semaphore's units, which an interrupt may signal. */
static struct spinlock sem_lock = SPINLOCK_INIT_NAMED("acpi_sem_lock");

/**
 * Whether the caller may block: the scheduler is on, and it is a process
 * of its own, not an interrupt and not an idle task. At boot no one may,
 * and a wait spins.
 */
static int osl_can_block(void)
{
    return sched_is_enabled() && !in_interrupt() && current_proc != NULL &&
           current_proc != proc_idle && current_proc != this_cpu()->idle;
}

/** The tick a wait of `ms` milliseconds that begins now ends at: the
 * whole ticks rounded up, and one more for the tick in progress, so it is
 * never 0, which means no deadline. */
static uint64_t osl_deadline(uint32_t ms)
{
    return tick_count() + tick_span(ms / 1000u, ms % 1000u * 1000000u) + 1u;
}

/* What a waiter on a semaphore waits for: its units, or its deadline. */
struct sem_wait {
    struct osl_sem *sem;
    uint32_t units;
    int timed;
    uint64_t deadline; /* the tick the wait ends at, when `timed` */
};

/** Whether the wait at `arg` may stop sleeping: its units are free or its
 * deadline has come. */
static int sem_ready(void *arg)
{
    const struct sem_wait *w = (const struct sem_wait *)arg;

    return w->sem->units >= w->units || (w->timed && tick_count() >= w->deadline);
}

ACPI_STATUS AcpiOsCreateSemaphore(UINT32 max, UINT32 initial, ACPI_SEMAPHORE *out)
{
    struct osl_sem *s;

    if (!out || initial > max) {
        return AE_BAD_PARAMETER;
    }
    s = (struct osl_sem *)kzalloc(sizeof(*s));
    if (!s) {
        return AE_NO_MEMORY;
    }
    s->units = initial;
    s->max = max;
    wait_queue_init(&s->waiters);
    *out = s;
    return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE handle)
{
    if (!handle) {
        return AE_BAD_PARAMETER;
    }
    kfree(handle);
    return AE_OK;
}

/**
 * Take `units` of the semaphore, waiting `timeout` milliseconds at most for
 * them, ACPI_WAIT_FOREVER for no limit. A process blocks on the
 * semaphore's queue with its deadline armed, giving up the big lock while
 * it sleeps; the boot, an interrupt and an idle task spin a millisecond at
 * a time. AE_TIME when the time ran out.
 */
ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE handle, UINT32 units, UINT16 timeout)
{
    struct osl_sem *s = (struct osl_sem *)handle;
    struct sem_wait w;
    uint32_t flags, spun = 0;

    if (!s || units == 0) {
        return AE_BAD_PARAMETER;
    }
    w.sem = s;
    w.units = units;
    w.timed = timeout != ACPI_WAIT_FOREVER;
    w.deadline = osl_deadline(timeout);
    for (;;) {
        flags = spin_lock_irqsave(&sem_lock);
        if (s->units >= units) {
            s->units -= units;
            spin_unlock_irqrestore(&sem_lock, flags);
            return AE_OK;
        }
        spin_unlock_irqrestore(&sem_lock, flags);
        if (timeout == 0) {
            return AE_TIME;
        }
        if (osl_can_block()) {
            if (w.timed && tick_count() >= w.deadline) {
                return AE_TIME;
            }
            current_proc->wakeup_tick = w.timed ? w.deadline : 0;
            (void)wq_wait_event_nointr(&s->waiters, sem_ready, &w);
            current_proc->wakeup_tick = 0;
        } else {
            if (w.timed && spun >= timeout) {
                return AE_TIME;
            }
            k_udelay(1000);
            spun++;
        }
    }
}

ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE handle, UINT32 units)
{
    struct osl_sem *s = (struct osl_sem *)handle;
    uint32_t flags;

    if (!s) {
        return AE_BAD_PARAMETER;
    }
    flags = spin_lock_irqsave(&sem_lock);
    if (units > s->max - s->units) {
        spin_unlock_irqrestore(&sem_lock, flags);
        return AE_LIMIT;
    }
    s->units += units;
    spin_unlock_irqrestore(&sem_lock, flags);
    wq_wake_all(&s->waiters);
    return AE_OK;
}

/** The running process's record, which no other thread has, or 1 before
 * there is one; ACPICA takes 0 for no thread. */
ACPI_THREAD_ID AcpiOsGetThreadId(void)
{
    return current_proc ? (ACPI_THREAD_ID)(uintptr_t)current_proc : 1;
}

/**
 * Take the firmware's Global Lock at `lock` by the ACPI specification's
 * protocol: set the owned bit, and the pending bit when it was owned
 * already, in one compare-and-exchange. 1 when the caller owns it now; 0
 * when it waits for the firmware's release, whose SCI ACPICA takes.
 */
int acpi_os_acquire_global_lock(volatile unsigned int *lock)
{
    unsigned int old, new;

    do {
        old = *lock;
        new = (old & ~3u) | 2u | ((old >> 1) & 1u);
    } while (arch_atomic_cmpxchg(lock, old, new) != old);
    return (new & 1u) == 0;
}

/** Give the Global Lock at `lock` back: clear the owned and pending bits.
 * 1 when the firmware was waiting for it and must be told. */
int acpi_os_release_global_lock(volatile unsigned int *lock)
{
    unsigned int old;

    do {
        old = *lock;
    } while (arch_atomic_cmpxchg(lock, old, old & ~3u) != old);
    return (int)(old & 1u);
}

/* ---- time ---- */

/** The tick the deadline at `arg` names has come. */
static int tick_reached(void *arg)
{
    return tick_count() >= *(const uint64_t *)arg;
}

/**
 * Wait `milliseconds`, at least: a process sleeps to the tick after,
 * giving up the big lock; the boot spins.
 */
void AcpiOsSleep(UINT64 milliseconds)
{
    uint32_t ms = milliseconds > 0xFFFFFFu ? 0xFFFFFFu : (uint32_t)milliseconds;

    if (osl_can_block()) {
        wait_queue_t wq;
        uint64_t target = osl_deadline(ms);

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

void AcpiOsStall(UINT32 microseconds)
{
    k_udelay(microseconds);
}

/** The monotonic clock in ACPICA's unit, 100 ns. */
UINT64 AcpiOsGetTimer(void)
{
    int64_t s, ns;

    rtc_monotonic(&s, &ns);
    return (UINT64)s * 10000000u + (uint32_t)ns / 100u;
}

/* ---- the SCI ---- */

static ACPI_OSD_HANDLER sci_routine;
static void *sci_context;
static int sci_line = -1;
static uint32_t sci_taken, sci_done;

/** The SCI's handler on its line's vector: ACPICA's routine, which reads
 * and clears the fixed events' and the GPEs' status. */
static void osl_sci(isr_params_t *params)
{
    ACPI_OSD_HANDLER routine = sci_routine;

    (void)params;
    if (!routine) {
        return;
    }
    sci_taken++;
    if (routine(sci_context) == ACPI_INTERRUPT_HANDLED) {
        sci_done++;
    }
}

/**
 * Take the SCI, ISA line `irq`, for `routine`: let the line go,
 * level-triggered, from the hold `board.c` put on it, and register the
 * routine on the line's vector, which routes it, with interrupts off
 * between so that no SCI comes with no handler.
 * The polarity is the MADT override's (ioapic.c). Under `rach2.nosci` the
 * line stays held and nothing is registered, and ACPICA is told it is.
 */
ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 irq, ACPI_OSD_HANDLER routine, void *context)
{
    uint32_t flags;

    if (!routine) {
        return AE_BAD_PARAMETER;
    }
    if (irq > 15) {
        klog(KLOG_ERR, "acpi", "sci: refused: line %u is not one this kernel routes", irq);
        return AE_BAD_PARAMETER;
    }
    if (sci_routine) {
        return AE_ALREADY_EXISTS;
    }
    if (kcmdline_has("rach2.nosci")) {
        pic_mask_irq((uint8_t)irq);
        klog(KLOG_INFO, "acpi", "sci: not taken: rach2.nosci; line %u stays masked", irq);
        return AE_OK;
    }
    flags = kirq_save();
    sci_context = context;
    sci_routine = routine;
    sci_line = (int)irq;
    isr_line_release((int)irq, 1);
    register_isr_handler((uint8_t)IRQ_VECTOR(irq), &osl_sci, "acpi");
    kirq_restore(flags);
    klog(KLOG_DEBUG, "acpi", "sci: line %u", irq);
    return AE_OK;
}

/** Hold the SCI's line masked again; the handler stays on the vector and
 * does nothing. */
ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32 irq, ACPI_OSD_HANDLER routine)
{
    if (!sci_routine || routine != sci_routine || (int)irq != sci_line) {
        return AE_NOT_EXIST;
    }
    pic_mask_irq((uint8_t)irq);
    sci_routine = NULL;
    sci_line = -1;
    return AE_OK;
}

uint32_t acpica_sci_count(void)
{
    return sci_taken;
}

uint32_t acpica_sci_handled(void)
{
    return sci_done;
}

/* ---- deferred work and its thread ---- */

/* The work AcpiOsExecute queued, a ring: `queue_head` is the next to run
 * and `queue_tail` the next free; both only grow. */
static struct {
    ACPI_OSD_EXEC_CALLBACK function;
    void *context;
} queue[OSL_QUEUE];
static uint32_t queue_head, queue_tail;
static int queue_running; /* an item taken off is still running */
static int queue_full_said;
static struct spinlock queue_lock = SPINLOCK_INIT_NAMED("acpi_queue_lock");
static wait_queue_t queue_work, queue_idle;
static process_t *thread;

/**
 * Queue `function` for ACPICA's thread: a notify handler, or the method of
 * a GPE, which the SCI's handler may not run, since a method may wait.
 * Every caller queues, the boot's too, for the reason `osl_drain` gives.
 * AE_NO_MEMORY when the queue is full.
 */
ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE type, ACPI_OSD_EXEC_CALLBACK function, void *context)
{
    uint32_t flags;

    (void)type;
    if (!function) {
        return AE_BAD_PARAMETER;
    }
    flags = spin_lock_irqsave(&queue_lock);
    if (queue_tail - queue_head == OSL_QUEUE) {
        int said = queue_full_said;

        queue_full_said = 1;
        spin_unlock_irqrestore(&queue_lock, flags);
        if (!said) {
            klog(KLOG_ERR, "acpi", "execute: refused: all %d places queued", OSL_QUEUE);
        }
        return AE_NO_MEMORY;
    }
    queue[queue_tail % OSL_QUEUE].function = function;
    queue[queue_tail % OSL_QUEUE].context = context;
    queue_tail++;
    spin_unlock_irqrestore(&queue_lock, flags);
    wq_wake_all(&queue_work);
    return AE_OK;
}

void osl_drain(void)
{
    ACPI_OSD_EXEC_CALLBACK function;
    void *context;
    uint32_t flags;

    for (;;) {
        flags = spin_lock_irqsave(&queue_lock);
        if (queue_head == queue_tail) {
            queue_running = 0;
            spin_unlock_irqrestore(&queue_lock, flags);
            wq_wake_all(&queue_idle);
            return;
        }
        function = queue[queue_head % OSL_QUEUE].function;
        context = queue[queue_head % OSL_QUEUE].context;
        queue_head++;
        queue_running = 1;
        spin_unlock_irqrestore(&queue_lock, flags);
        function(context);
    }
}

/** Whether the queue has work. */
static int queue_has_work(void *arg)
{
    (void)arg;
    return queue_head != queue_tail;
}

/** Whether the queue is empty and nothing taken off it still runs. */
static int queue_is_idle(void *arg)
{
    (void)arg;
    return queue_head == queue_tail && !queue_running;
}

/**
 * Wait until the work queued so far has run. Before the thread is made the
 * boot runs it in line; the thread itself, an interrupt and an idle task
 * cannot wait for it, and return at once.
 */
void AcpiOsWaitEventsComplete(void)
{
    if (!thread) {
        if (!in_interrupt()) {
            osl_drain();
        }
        return;
    }
    if (current_proc == thread || !osl_can_block()) {
        return;
    }
    (void)wq_wait_event_nointr(&queue_idle, queue_is_idle, NULL);
}

/** ACPICA's thread, kacpid: run the queue's work as it comes, holding the
 * big lock as kernel code does, and sleep when there is none in an idle
 * wait, which /proc/stat does not count as blocked; a wait inside a method
 * still counts. */
static void osl_thread(void)
{
    arch_irq_enable();
    for (;;) {
        (void)wq_wait_event_idle(&queue_work, queue_has_work, NULL);
        osl_drain();
    }
}

void acpica_thread_start(void)
{
    process_t *p;
    uintptr_t base;
    uint32_t flags;

    if (!acpica_up() || thread) {
        return;
    }
    flags = kirq_save();
    base = kstack_alloc();
    p = base ? alloc_process() : NULL;
    if (!p) {
        if (base) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        klog(KLOG_ERR, "acpi", "thread: refused: no record or stack for it");
        return;
    }
    p->kernel_stack = base + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = NULL;
    p->parent_pid = 0;
    proc_set_comm(p, "kacpid");
    setup_process_stack(p, osl_thread);
    thread = p;
    sched_make_ready(p);
    kirq_restore(flags);
    klog(KLOG_DEBUG, "acpi", "thread: kacpid is pid %d", (int)p->pid);
}

/* ---- the log ---- */

static char line[OSL_LINE_MAX];
static int line_len;
static struct spinlock line_lock = SPINLOCK_INIT_NAMED("acpi_line_lock");
static uint32_t errors, warnings;

/** Whether `s` begins with `prefix`. */
static int starts(const char *s, const char *prefix)
{
    while (*prefix != '\0') {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

/**
 * Log the line gathered so far as `acpi: acpica: TEXT`, at the level its
 * prefix gives, and count it when it is an error or a warning. An ordinary
 * message, such as a line of the table list, is `KLOG_DEBUG`, kept in the
 * log but not shown on the screen, and its prefix "ACPI: " is left out.
 */
static void flush_line(void)
{
    const char *text = line;
    int level = KLOG_DEBUG;

    line[line_len] = '\0';
    line_len = 0;
    if (starts(text, "ACPI Error") || starts(text, "ACPI Exception") ||
        starts(text, "ACPI BIOS Error")) {
        errors++;
        level = KLOG_ERR;
    } else if (starts(text, "ACPI Warning") || starts(text, "ACPI BIOS Warning")) {
        warnings++;
        level = KLOG_WARN;
    } else if (starts(text, "ACPI: ")) {
        text += 6;
    }
    if (*text != '\0') {
        klog(level, "acpi", "acpica: %s", text);
    }
}

/**
 * ACPICA's output, which comes in pieces, a message's prefix, its text and
 * its newline each by a call of its own: formatted by ACPICA's vsnprintf,
 * which knows its formats, and logged a whole line at a time.
 */
void AcpiOsVprintf(const char *format, va_list args)
{
    char piece[OSL_LINE_MAX];
    uint32_t flags;
    int i, n;

    n = vsnprintf(piece, sizeof(piece), format, args);
    if (n < 0) {
        return;
    }
    if (n >= (int)sizeof(piece)) {
        n = (int)sizeof(piece) - 1;
    }
    flags = spin_lock_irqsave(&line_lock);
    for (i = 0; i < n; i++) {
        if (piece[i] == '\n') {
            flush_line();
        } else if (line_len < OSL_LINE_MAX - 1) {
            line[line_len++] = piece[i];
        }
    }
    spin_unlock_irqrestore(&line_lock, flags);
}

void AcpiOsPrintf(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    AcpiOsVprintf(format, args);
    va_end(args);
}

uint32_t acpica_errors(void)
{
    return errors;
}

uint32_t acpica_warnings(void)
{
    return warnings;
}

/* ---- the rest ---- */

void osl_set_root(uint64_t rsdp)
{
    root_pointer = rsdp;
}

ACPI_STATUS AcpiOsInitialize(void)
{
    wait_queue_init(&queue_work);
    wait_queue_init(&queue_idle);
    return AE_OK;
}

ACPI_STATUS AcpiOsTerminate(void)
{
    return AE_OK;
}

/** The RSDP the kernel's table reader found, or ACPICA's own search of the
 * BIOS area's when it found none. */
ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer(void)
{
    ACPI_PHYSICAL_ADDRESS found = 0;

    if (root_pointer != 0) {
        return (ACPI_PHYSICAL_ADDRESS)root_pointer;
    }
    (void)AcpiFindRootPointer(&found);
    return found;
}

/** No predefined object is replaced. */
ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *init, ACPI_STRING *value)
{
    (void)init;
    *value = NULL;
    return AE_OK;
}

/** No table is replaced. */
ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *existing, ACPI_TABLE_HEADER **replacement)
{
    (void)existing;
    *replacement = NULL;
    return AE_OK;
}

/** No table is replaced by one elsewhere in memory. */
ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *existing, ACPI_PHYSICAL_ADDRESS *address,
                                        UINT32 *length)
{
    (void)existing;
    *address = 0;
    *length = 0;
    return AE_OK;
}

/** The firmware's Fatal and BreakPoint operators: logged, and the method
 * goes on. */
ACPI_STATUS AcpiOsSignal(UINT32 function, void *info)
{
    if (function == ACPI_SIGNAL_FATAL) {
        const ACPI_SIGNAL_FATAL_INFO *f = (const ACPI_SIGNAL_FATAL_INFO *)info;

        klog(KLOG_ERR, "acpi", "acpica: firmware fatal: type 0x%x, code 0x%x, argument 0x%x",
             f->Type, f->Code, f->Argument);
    }
    return AE_OK;
}
