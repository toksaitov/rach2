/* Record a bounded display and rendering timeline for a profiling run. */

#include "drm_frame_trace.h"

#ifdef RACH2_DRM_FRAME_TRACE
#include "../fs/debugfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../lib/spinlock.h"
#include "../mm/usercopy.h"
#include "../rtc.h"

#define FRAME_TRACE_ROWS 32768u

struct frame_trace_header {
    uint32_t version, count, lost, reserved;
    int64_t start_ns, stop_ns;
};

struct frame_trace_row {
    int64_t ns;
    uint64_t a, b, c;
    uint32_t event, object;
};

volatile int drm_frame_trace_enabled;
static struct spinlock frame_trace_lock = SPINLOCK_INIT_NAMED("drm_frame_trace_lock");
static struct frame_trace_header capture;
static struct frame_trace_row records[FRAME_TRACE_ROWS];

void drm_frame_trace(enum drm_frame_event event, uint32_t object, uint64_t a, uint64_t b,
                     uint64_t c)
{
    struct frame_trace_row *row;
    uint32_t flags;

    if (!drm_frame_trace_enabled) {
        return;
    }
    flags = spin_lock_irqsave(&frame_trace_lock);
    if (drm_frame_trace_enabled && capture.count < FRAME_TRACE_ROWS) {
        row = &records[capture.count++];
        row->ns = rtc_monotonic_ns();
        row->a = a;
        row->b = b;
        row->c = c;
        row->event = (uint32_t)event;
        row->object = object;
    } else if (drm_frame_trace_enabled) {
        capture.lost++;
    }
    spin_unlock_irqrestore(&frame_trace_lock, flags);
}

/** Give a stopped capture's size and optionally copy it. The big lock
 * serializes reads and control writes. Once stopped, no producer writes. */
static int32_t frame_trace_show(void *data, char *buf, uint32_t cap)
{
    uint32_t bytes, len;

    (void)data;
    if (drm_frame_trace_enabled) {
        return -EBUSY;
    }
    bytes = capture.count * sizeof(records[0]);
    len = sizeof(capture) + bytes;
    if (buf && cap >= len) {
        k_memcpy(buf, &capture, sizeof(capture));
        k_memcpy(buf + sizeof(capture), records, bytes);
    }
    return (int32_t)len;
}

/** Start a fresh capture or stop the current one. The big lock is held. */
static int32_t frame_trace_store(void *data, const char __user *buf, uint32_t len)
{
    char word[6];
    uint32_t flags;
    int start;

    (void)data;
    if (len != 6u && len != 5u) {
        return -EINVAL;
    }
    if (copy_from_user(word, buf, len) != 0) {
        return -EFAULT;
    }
    start = len == 6u && word[0] == 's' && word[1] == 't' && word[2] == 'a' && word[3] == 'r' &&
            word[4] == 't' && word[5] == '\n';
    if (!start && !(len == 5u && word[0] == 's' && word[1] == 't' && word[2] == 'o' &&
                    word[3] == 'p' && word[4] == '\n')) {
        return -EINVAL;
    }
    flags = spin_lock_irqsave(&frame_trace_lock);
    if (start) {
        if (drm_frame_trace_enabled) {
            spin_unlock_irqrestore(&frame_trace_lock, flags);
            return -EBUSY;
        }
        capture.version = 1;
        capture.count = capture.lost = 0;
        capture.start_ns = rtc_monotonic_ns();
        capture.stop_ns = 0;
        drm_frame_trace_enabled = 1;
    } else if (drm_frame_trace_enabled) {
        capture.stop_ns = rtc_monotonic_ns();
        drm_frame_trace_enabled = 0;
    }
    spin_unlock_irqrestore(&frame_trace_lock, flags);
    return (int32_t)len;
}

static const struct debugfs_text_ops frame_trace_ops = {frame_trace_show, frame_trace_store};

void drm_frame_trace_init(void)
{
    if (debugfs_create_text("drm_frame_trace", 0600u, &frame_trace_ops, NULL) != 0) {
        klog(KLOG_WARN, "drm", "debugfs: frame trace not registered");
    }
}
#endif
