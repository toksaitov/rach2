/*
 * A second thread for a host test that runs one thread's call inside a
 * sleep of another, without threads. `park_run` runs the call on a stack of
 * its own; a wait of the model that must block calls `park`, which switches
 * back to the caller of `park_run` and keeps the call's frames, as the
 * scheduler switches back to the sleeping thread. `park_resume` switches
 * into the parked call again, and `park` returns inside it, where it waited.
 * One call runs or waits at a time.
 *
 * The switches are the C library's ucontext calls, which macOS marks
 * deprecated and still provides. Only C library headers are included here:
 * the kernel's headers, which the test includes, define types these would
 * define again.
 */
#define _XOPEN_SOURCE 700
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <stdlib.h>
#include <ucontext.h>

/* The size of a call's stack, more than the frames of any call a row makes
 * take with the sanitizers' red zones. */
#define PARK_STACK (1024 * 1024)

static ucontext_t caller, call;
static void (*entry)(void);
static char *stack;
static int running, returned;

/* The first frame on the call's own stack. When the call returns, it
 * switches to `caller`, the context of the last `park_run` or
 * `park_resume`, and is never resumed; its stack is then freed. */
static void start(void)
{
    entry();
    returned = 1;
    swapcontext(&call, &caller);
}

/* Switch into the call until it returns or parks: 1 when it returned, and
 * then its stack is freed. */
static int enter(void)
{
    running = 1;
    swapcontext(&caller, &call);
    running = 0;
    if (returned) {
        free(stack);
        stack = NULL;
    }
    return returned;
}

/** Run `fn` on a stack of its own: 1 when it returned, 0 when it parked. */
int park_run(void (*fn)(void))
{
    if (stack) {
        abort();
    }
    stack = malloc(PARK_STACK);
    if (!stack || getcontext(&call) != 0) {
        abort();
    }
    call.uc_stack.ss_sp = stack;
    call.uc_stack.ss_size = PARK_STACK;
    call.uc_link = &caller;
    entry = fn;
    returned = 0;
    makecontext(&call, start, 0);
    return enter();
}

/** Switch into the parked call where it parked: 1 when it returned, 0 when
 * it parked again. */
int park_resume(void)
{
    if (!stack) {
        abort();
    }
    return enter();
}

/** Whether a call `park_run` started is running now, not parked. */
int park_active(void)
{
    return running;
}

/** Whether a call is parked, waiting for `park_resume`. */
int park_waiting(void)
{
    return stack != NULL && !running;
}

/** Park the running call: switch back to whoever ran or resumed it, and
 * return when `park_resume` switches into it again. */
void park(void)
{
    running = 0;
    swapcontext(&call, &caller);
    running = 1;
}
