/*
 * Calls that take turns on stacks of their own, for a host test that plays
 * several processors on one thread (fiber_host.c).
 */
#ifndef FIBER_HOST_H
#define FIBER_HOST_H

/* The most calls at once, numbered from 0. */
#define FIBERS 16

/** Run `fn` as call `id` on a stack of its own until it yields or returns:
 * 1 when it returned, 0 when it yielded. Only the test starts a call. */
int fiber_start(int id, void (*fn)(void));

/** Switch into call `id` where it yielded: 1 when it returned, 0 when it
 * yielded again. Only the test resumes a call. */
int fiber_resume(int id);

/** Switch from the running call back to the test, and return when the
 * test resumes it. */
void fiber_yield(void);

/** The call that runs now, or -1 when the test does. */
int fiber_running(void);

/** Whether call `id` was started and has not returned or been dropped. */
int fiber_live(int id);

/** Give up on call `id`, which yielded and will not be resumed, and free
 * its stack. */
void fiber_drop(int id);

#endif /* FIBER_HOST_H */
