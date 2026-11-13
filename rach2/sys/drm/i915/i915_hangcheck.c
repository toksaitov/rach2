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

void i915_hangcheck_unpark(struct drm_i915_private *i915, uint64_t now)
{
    int i;

    i915->gt.hc_next = now + MS_TO_TICKS(I915_HEARTBEAT_INTERVAL_MS);
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        i915->gt.engine[i].hc_armed = 0;
        i915->gt.engine[i].hc_stopped = 0;
    }
}

int i915_hangcheck_unpark_engine(struct drm_i915_private *i915, struct i915_engine *e, uint64_t now)
{
    e->hc_armed = 0;
    e->hc_stopped = 0;
    e->hc_first = now + MS_TO_TICKS(I915_HEARTBEAT_INTERVAL_MS);
    if (i915->gt.hc_next != 0) {
        return 0;
    }
    i915->gt.hc_next = e->hc_first;
    return 1;
}

void i915_hangcheck_enable(struct drm_i915_private *i915, int on)
{
    int i, taken = i915_gem_lock(i915);

    if (on && i915->params.hangcheck_off) {
        for (i = 0; i < I915_NUM_ENGINES; i++) {
            i915->gt.engine[i].hc_armed = 0;
        }
    }
    i915->params.hangcheck_off = (uint8_t)!on;
    i915_gem_unlock(i915, taken);
}

/** The last request of `e` given to the chip whose breadcrumb is not yet
 * written, or NULL. Under the GEM lock, without which no request leaves
 * the list. */
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
    int i, taken, off;

    if (!gt->awake || gt->wedged || gt->hc_next == 0 || now < gt->hc_next) {
        return 0;
    }
    /* One beat at a time, under the GEM lock that its reset needs, since
     * the worker and every waiter call it; the test again under it. */
    taken = i915_gem_lock(i915);
    if (!gt->awake || gt->wedged || gt->hc_next == 0 || now < gt->hc_next) {
        i915_gem_unlock(i915, taken);
        return 0;
    }
    /* With i915.enable_hangcheck off the beat still judges the heartbeat
     * of the last and resets an engine that missed it, but sends no new
     * one, and then drops the engine's heartbeat until the engine next
     * unparks (gt/intel_engine_heartbeat.c:167-209 and :234-238 in
     * 6.12.115). */
    off = i915->params.hangcheck_off;
    gt->hc_next = off ? 0 : now + MS_TO_TICKS(I915_HEARTBEAT_INTERVAL_MS);
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        struct i915_engine *e = &gt->engine[i];

        if (!e->running || e->hc_stopped) {
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
            /* An engine is armed no sooner than its own first beat, an
             * interval after it unparked, as Linux's next_heartbeat queues
             * it (gt/intel_engine_heartbeat.c:59-62 in 6.12.115). */
            e->hc_armed = 0;
            rq = off || now < e->hc_first ? 0 : last_on_chip(e);
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
    if (off) {
        for (i = 0; i < I915_NUM_ENGINES; i++) {
            gt->engine[i].hc_armed = 0;
            gt->engine[i].hc_stopped = 1;
        }
    }
    i915_gem_unlock(i915, taken);
    return hung;
}
