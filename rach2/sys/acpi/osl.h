/*
 * osl.h: what boot.c needs of osl.c, ACPICA's OS layer, beyond the AcpiOs*
 * functions ACPICA itself calls.
 */
#ifndef _ACPI_OSL_H_
#define _ACPI_OSL_H_

#include "../lib/kint.h"

/** Give `AcpiOsGetRootPointer` the RSDP the kernel's table reader found. */
void osl_set_root(uint64_t rsdp);

/**
 * Run the work `AcpiOsExecute` queued until none is left, in line, as the
 * boot does once ACPICA is up and before its thread is made. Not from an
 * interrupt, and not from inside a call into ACPICA, whose interpreter a
 * notify handler enters again. In the thread, kacpid, a service point
 * follows each work (`sched_service_point`), since the thread takes the
 * tick's switch only at its sleeps and service points, as Linux's worker
 * calls cond_resched between works (kernel/workqueue.c:3290-3299).
 */
void osl_drain(void);

/*
 * A work for ACPICA's thread that is queued once however often it is asked
 * before it runs, and is never refused, as Linux's work_struct on its
 * workqueue: its `function` with `context`, which the caller sets, and the
 * link and mark osl.c keeps. A driver whose request a full queue must not
 * drop uses one in place of `AcpiOsExecute`.
 */
struct osl_work {
    void (*function)(void *context);
    void *context;
    struct osl_work *next;
    int pending;
};

/**
 * Queue `work`, unless it is queued and has not begun to run; it runs after
 * the `AcpiOsExecute` work queued before it, and its mark is cleared as it
 * begins, so a request while it runs queues it again. Any context, an
 * interrupt or a spinlock held with interrupts off among them; it does not
 * sleep.
 */
void osl_queue_work(struct osl_work *work);

#endif /* _ACPI_OSL_H_ */
