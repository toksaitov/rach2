/* The i915 fixtures advance a millisecond clock independently of HZ. */
#include "../../../rach2/sys/rtc.h"
#include "../../../rach2/sys/timer.h"

/** Read the fixtures' explicit millisecond count as monotonic nanoseconds. */
int64_t rtc_monotonic_ns(void)
{
    return (int64_t)kernel_ticks * 1000000u;
}

int64_t host_i915_deadline;

/** Keep the last publication; no host processor receives an interrupt. */
void timer_service_wake_at(int64_t ns)
{
    host_i915_deadline = ns;
}
