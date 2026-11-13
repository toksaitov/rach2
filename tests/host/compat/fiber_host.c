/*
 * Calls that take turns on stacks of their own, for a host test that plays
 * several processors on one thread: each processor's call runs until it
 * yields, as a wait's turn does, and the test resumes the calls in an order
 * a row chooses, so an interleaving is the same on every run.
 *
 * `fiber_start` runs a call on a stack of its own until it yields or
 * returns; `fiber_yield`, inside the call, switches back to the test, and
 * `fiber_resume` switches into the call again where it yielded. A call the
 * test gives up on is dropped with `fiber_drop`, which frees its stack.
 * The switches are the C library's ucontext calls, which macOS marks
 * deprecated and still provides. Only C library headers are included here:
 * the kernel's headers, which the test includes, define types these would
 * define again.
 */
#define _XOPEN_SOURCE 700
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <stdlib.h>
#include <ucontext.h>

#include "fiber_host.h"

/* The size of a call's stack, more than the frames of any call a row makes
 * take with the sanitizers' red zones. */
#define FIBER_STACK (1024 * 1024)

/* A call: its context, its stack (NULL when none is started) and whether
 * it returned. */
struct fiber {
    ucontext_t ctx;
    char *stack;
    void (*fn)(void);
    int returned;
};

static struct fiber fibers[FIBERS];
static ucontext_t tester;
static int running = -1;

/** The first frame on a call's own stack: run the call, mark it returned
 * and switch back to the test for good. */
static void fiber_entry(void)
{
    struct fiber *f = &fibers[running];

    f->fn();
    f->returned = 1;
    swapcontext(&f->ctx, &tester);
}

/** Switch into call `id` until it yields or returns: 1 when it returned,
 * and then its stack is freed. */
static int fiber_enter(int id)
{
    struct fiber *f = &fibers[id];

    running = id;
    swapcontext(&tester, &f->ctx);
    running = -1;
    if (f->returned) {
        free(f->stack);
        f->stack = NULL;
    }
    return f->returned;
}

int fiber_start(int id, void (*fn)(void))
{
    struct fiber *f;

    if (id < 0 || id >= FIBERS || fibers[id].stack || running >= 0) {
        abort();
    }
    f = &fibers[id];
    f->stack = malloc(FIBER_STACK);
    if (!f->stack || getcontext(&f->ctx) != 0) {
        abort();
    }
    f->ctx.uc_stack.ss_sp = f->stack;
    f->ctx.uc_stack.ss_size = FIBER_STACK;
    f->ctx.uc_link = &tester;
    f->fn = fn;
    f->returned = 0;
    makecontext(&f->ctx, fiber_entry, 0);
    return fiber_enter(id);
}

int fiber_resume(int id)
{
    if (id < 0 || id >= FIBERS || !fibers[id].stack || running >= 0) {
        abort();
    }
    return fiber_enter(id);
}

void fiber_yield(void)
{
    if (running < 0) {
        abort();
    }
    swapcontext(&fibers[running].ctx, &tester);
}

int fiber_running(void)
{
    return running;
}

int fiber_live(int id)
{
    return id >= 0 && id < FIBERS && fibers[id].stack != NULL;
}

void fiber_drop(int id)
{
    if (id < 0 || id >= FIBERS || running >= 0) {
        abort();
    }
    free(fibers[id].stack);
    fibers[id].stack = NULL;
}
