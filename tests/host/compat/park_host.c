/*
 * A call that blocks, for a host test that runs a second thread's call
 * inside a sleep of the first. `park_run` runs the call; a wait of the model
 * that finds its condition false calls `park`, which ends the call there and
 * returns from `park_run`, as the scheduler would switch back to the
 * sleeping thread. The parked call's frames are gone, so a test resumes it
 * by running it again from its start, which is the same call as long as
 * nothing it did before it blocked lasts.
 *
 * Only <setjmp.h> is included here: the kernel's headers, which the test
 * includes, define types the C library's headers would define again.
 */
#include <setjmp.h>

static jmp_buf *point;

/** Run `call`: 1 when it returned, 0 when it parked. */
int park_run(void (*call)(void))
{
    jmp_buf here;
    jmp_buf *outer = point;

    point = &here;
    if (setjmp(here) != 0) {
        point = outer;
        return 0;
    }
    call();
    point = outer;
    return 1;
}

/** Whether `park_run` is running a call that may park. */
int park_active(void)
{
    return point != 0;
}

/** End the call `park_run` is running; does not return. */
void park(void)
{
    longjmp(*point, 1);
}
