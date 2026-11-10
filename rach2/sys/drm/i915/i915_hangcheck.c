/*
 * i915_hangcheck.c: the Intel graphics driver's hang detection, a
 * heartbeat on each engine while the GT is awake, after Linux 6.12's
 * gt/intel_engine_heartbeat.c with ring submission on Haswell.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../rtc.h"

/*
 * Linux sends a heartbeat request down each busy engine every interval
 * and resets the engine when the one it sent at the last beat has not
 * completed by this one; a legacy ring has no scheduler to raise its
 * priority first. A ring runs its requests in order, so that heartbeat
 * completes once every request given to the chip before it has: the beat
 * here takes the last of those as the heartbeat instead of emitting one.
 * A request whose breadcrumb and ACTHD do not move for an interval is
 * hung by this rule, and so is one that runs past it while they move, as
 * a batch that chains to itself does, which Linux resets too.
 */

/* Why an engine is reset, as Linux's heartbeat says it, by index. */
static const char *const heartbeat_msg[I915_NUM_ENGINES] = {
    "stopped heartbeat on rcs0", "stopped heartbeat on bcs0", "stopped heartbeat on vcs0",
    "stopped heartbeat on vecs0"};

void i915_hangcheck_unpark(struct drm_i915_private *i915)
{
    int i;

    i915->gt.hc_next = rtc_ticks_now() + MS_TO_TICKS(I915_HEARTBEAT_INTERVAL_MS);
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        i915->gt.engine[i].hc_armed = 0;
    }
}

/** The last request of `e` given to the chip whose breadcrumb is not yet
 * written, or NULL. Under the big kernel lock, which keeps the list. */
static struct i915_request *last_on_chip(struct i915_engine *e)
{
    struct i915_request *rq, *last = 0;

    for (rq = e->requests; rq && rq->submitted; rq = rq->next) {
        if (!i915_engine_passed(e, rq->seqno)) {
            last = rq;
        }
    }
    return last;
}

uint32_t i915_hangcheck(struct drm_i915_private *i915)
{
    struct i915_gt *gt = &i915->gt;
    struct i915_request *rq;
    uint32_t hung = 0, hws, acthd;
    uint64_t now = rtc_ticks_now();
    int i;

    if (!gt->awake || gt->wedged || i915->params.hangcheck_off || now < gt->hc_next) {
        return 0;
    }
    gt->hc_next = now + MS_TO_TICKS(I915_HEARTBEAT_INTERVAL_MS);
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        struct i915_engine *e = &gt->engine[i];

        if (!e->running) {
            continue;
        }
        hws = e->hws ? ((volatile uint32_t *)e->hws)[I915_GEM_HWS_SEQNO] : 0;
        acthd = i915_uncore_read(i915, RING_ACTHD(e->mmio_base));
        if (e->hc_armed && !i915_engine_passed(e, e->hc_seqno)) {
            klog(KLOG_ERR, "i915",
                 "hangcheck: %s: no heartbeat in %u ms: breadcrumb %u of %u, ACTHD 0x%08x, "
                 "was 0x%08x",
                 e->name, (unsigned)I915_HEARTBEAT_INTERVAL_MS, (unsigned)hws,
                 (unsigned)e->hc_seqno, (unsigned)acthd, (unsigned)e->hc_acthd);
            hung |= 1u << i;
        } else {
            e->hc_armed = 0;
            rq = last_on_chip(e);
            if (rq) {
                e->hc_armed = 1;
                e->hc_seqno = rq->seqno;
            }
        }
        e->hc_acthd = acthd;
        e->hc_hws = hws;
    }
    /* A reset of the whole GPU for one engine resumes the other too, and
     * a wedge cancels its requests; either leaves it disarmed. */
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        if ((hung & (1u << i)) && gt->engine[i].hc_armed) {
            i915_handle_error(i915, 1u << i, heartbeat_msg[i]);
        }
    }
    return hung;
}
