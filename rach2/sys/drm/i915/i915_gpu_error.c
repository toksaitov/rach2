/*
 * i915_gpu_error.c: the Intel graphics driver's error state, the GPU's
 * registers, rings and requests captured at a hang, after Linux 6.18's
 * i915_gpu_error.c on Haswell.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../mm/kheap.h"
#include "../../rtc.h"

/** The fault register of the engine at index `i`, Linux's RING_FAULT_REG
 * on graphics 6 and 7. */
static uint32_t fault_reg(int i)
{
    return i == I915_RCS0 ? RING_FAULT_REG_RCS : RING_FAULT_REG_BCS;
}

/**
 * Read `e`'s registers, as Linux's engine_record_registers on graphics 7,
 * its breadcrumb, the ring's dwords from its head, and its requests on the
 * chip not yet completed, as the error state keeps them.
 */
static void capture_engine(struct drm_i915_private *i915, int i, struct i915_error_engine *ee)
{
    struct i915_engine *e = &i915->gt.engine[i];
    uint32_t base = e->mmio_base, at, n;
    struct i915_request *rq;

    ee->rc_psmi = i915_uncore_read(i915, RING_PSMI_CTL(base));
    ee->fault_reg = i915_uncore_read(i915, fault_reg(i));
    ee->esr = i915_uncore_read(i915, RING_ESR(base));
    ee->faddr = i915_uncore_read(i915, RING_DMA_FADD(base));
    ee->ipeir = i915_uncore_read(i915, RING_IPEIR(base));
    ee->ipehr = i915_uncore_read(i915, RING_IPEHR(base));
    ee->instps = i915_uncore_read(i915, RING_INSTPS(base));
    ee->bbaddr = i915_uncore_read(i915, RING_BBADDR(base));
    ee->ccid = i915_uncore_read(i915, CCID(base));
    ee->bbstate = i915_uncore_read(i915, RING_BBSTATE(base));
    ee->instdone = i915_uncore_read(i915, RING_INSTDONE(base));
    ee->instpm = i915_uncore_read(i915, RING_INSTPM(base));
    ee->acthd = i915_uncore_read(i915, RING_ACTHD(base));
    ee->start = i915_uncore_read(i915, RING_START(base));
    ee->head = i915_uncore_read(i915, RING_HEAD(base));
    ee->tail = i915_uncore_read(i915, RING_TAIL(base));
    ee->ctl = i915_uncore_read(i915, RING_CTL(base));
    ee->mode = i915_uncore_read(i915, RING_MI_MODE(base));
    ee->hws_pga = i915_uncore_read(i915, e->hws_pga);
    ee->hws_seqno = e->hws ? ((volatile uint32_t *)e->hws)[I915_GEM_HWS_SEQNO] : 0;
    ee->seqno = e->seqno;
    if (e->ring) {
        at = ee->head & HEAD_ADDR & (e->ring_size - 1u);
        for (n = 0; n < I915_ERROR_RING_DW; n++) {
            ee->ring[n] = e->ring[at / 4u];
            at = (at + 4u) & (e->ring_size - 1u);
        }
    }
    for (rq = e->requests; rq && rq->submitted && ee->nrequests < I915_ERROR_REQUESTS;
         rq = rq->next) {
        struct i915_error_request *er;

        if (i915_engine_passed(e, rq->seqno)) {
            continue;
        }
        er = &ee->rq[ee->nrequests++];
        er->seqno = rq->seqno;
        er->ctx_id = rq->ce && rq->ce->ctx ? rq->ce->ctx->id : 0;
        er->pid = rq->ce && rq->ce->ctx ? rq->ce->ctx->pid : 0;
        er->head = rq->head;
        er->tail = rq->tail;
        er->batch = rq->batch;
    }
}

/** Append `s` to the `cap` bytes at `buf`, which hold `len` already, and
 * return the new length; the text is cut and stays terminated. */
static uint32_t put(char *buf, uint32_t cap, uint32_t len, const char *s)
{
    while (*s && len + 1u < cap) {
        buf[len++] = *s++;
    }
    buf[len] = '\0';
    return len;
}

/** Append `v` in base `radix`, at least `width` digits, as `put` does. */
static uint32_t put_num(char *buf, uint32_t cap, uint32_t len, uint32_t v, uint32_t radix,
                        int width)
{
    char digits[12];
    int n = 0;

    do {
        digits[n++] = "0123456789abcdef"[v % radix];
        v /= radix;
    } while (v != 0 || n < width);
    while (n > 0 && len + 1u < cap) {
        buf[len++] = digits[--n];
    }
    buf[len] = '\0';
    return len;
}

/**
 * Make the state's message, as Linux's error_msg: "GPU HANG: ecode 7:M:E",
 * M the classes of the engines that hung and E the first hung engine's
 * IPEHR exclusive-or INSTDONE (generate_ecode), then the process of its
 * first request on the chip, when a program's context made it.
 */
static void make_msg(struct drm_i915_private *i915, struct i915_gpu_error *err)
{
    struct i915_engine *e = 0;
    struct i915_request *rq;
    const struct i915_error_engine *first = 0;
    uint32_t classes = 0, len;
    int i;

    for (i = 0; i < I915_NUM_ENGINES; i++) {
        if (err->engine[i].hung) {
            classes |= 1u << i915_engine_uabi_class(&i915->gt.engine[i]);
            if (!first) {
                first = &err->engine[i];
                e = &i915->gt.engine[i];
            }
        }
    }
    len = put(err->msg, sizeof(err->msg), 0, "GPU HANG: ecode ");
    len = put_num(err->msg, sizeof(err->msg), len, i915->id->platform->graphics_ver, 10, 1);
    len = put(err->msg, sizeof(err->msg), len, ":");
    len = put_num(err->msg, sizeof(err->msg), len, classes, 16, 1);
    len = put(err->msg, sizeof(err->msg), len, ":");
    len =
        put_num(err->msg, sizeof(err->msg), len, first ? first->ipehr ^ first->instdone : 0, 16, 8);
    if (!first) {
        return;
    }
    for (rq = e->requests; rq && rq->submitted; rq = rq->next) {
        if (!i915_engine_passed(e, rq->seqno)) {
            break;
        }
    }
    if (rq && rq->submitted && rq->ce && rq->ce->ctx && rq->ce->ctx->pid) {
        len = put(err->msg, sizeof(err->msg), len, ", in ");
        len = put(err->msg, sizeof(err->msg), len, rq->ce->ctx->comm);
        len = put(err->msg, sizeof(err->msg), len, " [");
        len = put_num(err->msg, sizeof(err->msg), len, rq->ce->ctx->pid, 10, 1);
        (void)put(err->msg, sizeof(err->msg), len, "]");
    }
}

void i915_gpu_error_capture(struct drm_i915_private *i915, uint32_t engine_mask, const char *reason)
{
    struct i915_gpu_error *err = (struct i915_gpu_error *)kzalloc(sizeof(*err));
    int i;

    if (!err) {
        klog(KLOG_ERR, "i915", "hang: no memory for the error state");
        return;
    }
    err->ticks = rtc_ticks_now();
    err->engine_mask = engine_mask;
    (void)put(err->reason, sizeof(err->reason), 0, reason);
    err->eir = i915_uncore_read(i915, EIR);
    err->pgtbl_er = i915_uncore_read(i915, PGTBL_ER);
    err->error_gen6 = i915_uncore_read(i915, ERROR_GEN6);
    err->done_reg = i915_uncore_read(i915, DONE_REG);
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        if (i915->gt.engine[i].running) {
            err->engine[i].hung = (engine_mask >> i) & 1u;
            capture_engine(i915, i, &err->engine[i]);
        }
    }
    make_msg(i915, err);
    klog(KLOG_INFO, "i915", "hang: %s", err->msg);
    if (i915->gt.error) {
        kfree(err);
        return;
    }
    i915->gt.error = err;
}

void i915_gpu_error_clear(struct drm_i915_private *i915)
{
    kfree(i915->gt.error);
    i915->gt.error = 0;
}
