/* The i915 fixtures advance a millisecond clock independently of HZ. */
#include "../../../rach2/sys/rtc.h"
#include "../../../rach2/sys/lib/spinlock.h"
#include "../../../rach2/sys/timer.h"

/** Read the fixtures' explicit millisecond count as monotonic nanoseconds. */
int64_t rtc_monotonic_ns(void)
{
    return (int64_t)kernel_ticks * 1000000u;
}

int64_t host_i915_deadline;
/* The base fixture sets the register lock whose publication it checks. */
struct spinlock *host_i915_register_lock;

/** Require the selected register lock to be released at publication and
 * keep its timestamp; no host processor receives an interrupt. */
void timer_service_wake_at(int64_t ns)
{
    if (host_i915_register_lock && host_i915_register_lock->locked) {
        __builtin_trap();
    }
    host_i915_deadline = ns;
}
