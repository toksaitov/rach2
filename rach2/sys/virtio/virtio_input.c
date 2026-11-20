/* virtio_input.c: keyboards, mice, tablets and touchscreens on the virtio
 * input transport. */

#include "virtio_input.h"

#include "../evdev.h"
#include "../input.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../lib/spinlock.h"
#include "../mm/kheap.h"
#include "../mouse.h"
#include "virtio.h"

/* Virtio 1.2, 5.8, the configuration selectors and the union's offset. */
#define CFG_NAME     0x01
#define CFG_SERIAL   0x02
#define CFG_IDS      0x03
#define CFG_PROPS    0x10
#define CFG_EV_BITS  0x11
#define CFG_ABS_INFO 0x12
#define CFG_SIZE     2
#define CFG_DATA     8
#define BUS_VIRTUAL  0x06

/* The event queue's little-endian record, in the host CPU's byte order. */
struct vi_event {
    uint16_t type, code;
    int32_t value;
};

struct vi_input {
    struct evdev *evdev;
    struct vi_event *events; /* one device-writable record per descriptor */
    uint64_t events_addr;
    uint32_t events_bytes;
    /* The status queue's records, one for each event under way to the
     * device, `nsts` of them, and which are under way, under `lock`, taken
     * with interrupts off. */
    struct virtqueue *status;
    struct vi_event *sts;
    uint64_t sts_addr;
    uint32_t sts_bytes;
    unsigned nsts;
    uint8_t sts_busy[VIRTQ_MAX_SIZE];
    struct spinlock lock;
    unsigned int leds; /* the LEDs the device has, a bit 1 << LED_* each */
    /* A keyboard, a device that states EV_REP, types on the console with
     * the input layer's repeat and lock LEDs, as a USB keyboard does. */
    int keyboard;
    struct input_keyboard kb;
    /* A mouse, REL_X and REL_Y without EV_REP, moves the mouse queue's
     * pointer by each packet: its buttons, a bit each from BTN_LEFT, and
     * the motion and whether the packet under way has any event. */
    int mouse;
    int touched;
    int dx, dy;
    unsigned int buttons;
};

/** Read up to `len` bytes of the item named by `select` and `subsel`. */
static unsigned vi_config(struct virtio_dev *d, uint8_t select, uint8_t subsel, void *out,
                          unsigned len)
{
    uint8_t size;

    virtio_config_write(d, 0, &select, 1);
    virtio_config_write(d, 1, &subsel, 1);
    virtio_config_read(d, CFG_SIZE, &size, 1);
    k_memset(out, 0, len);
    if (size > len) {
        size = (uint8_t)len;
    }
    if (size) {
        virtio_config_read(d, CFG_DATA, out, size);
    }
    return size;
}

/** Return whether the device's configuration bitmap has `code` set. */
static int vi_bit(const uint8_t *bits, unsigned code)
{
    return (bits[code / 8] >> (code % 8)) & 1;
}

/** Set the advertised axes, if any, and initialize the multitouch slots of
 * a device with ABS_MT_SLOT. A keyboard or a mouse has no axes, which Linux's
 * virtinput_probe takes as it is. */
static int vi_axes(struct virtio_dev *d, struct evdev *evdev)
{
    uint8_t axes[ABS_CNT / 8];
    int32_t abs[5]; /* minimum, maximum, fuzz, flat and resolution */
    int32_t slots = 0;
    unsigned i;

    vi_config(d, CFG_EV_BITS, EV_ABS, axes, sizeof(axes));
    for (i = 0; i < ABS_CNT; i++) {
        if (!vi_bit(axes, i)) {
            continue;
        }
        vi_config(d, CFG_ABS_INFO, (uint8_t)i, abs, sizeof(abs));
        if (i == ABS_MT_SLOT) {
            if (abs[1] < 0 || abs[1] >= EVDEV_MT_SLOTS) {
                return -EINVAL;
            }
            slots = abs[1] + 1;
        }
        evdev_set_abs(evdev, (uint16_t)i, abs[0], abs[1], abs[2], abs[3]);
        evdev_set_abs_res(evdev, (uint16_t)i, abs[4]);
    }
    return evdev_init_slots(evdev, slots, 0);
}

/** Offer `event` to the device. Returns the queue's error or 0. */
static int vi_queue(struct virtqueue *vq, struct vi_event *event)
{
    const struct vi_input *input = (const struct vi_input *)vq->priv;
    struct virtio_sg sg;

    sg.addr = input->events_addr + (uint32_t)(event - input->events) * sizeof(*event);
    sg.len = sizeof(*event);
    return virtqueue_add(vq, &sg, 0, 1, event);
}

/**
 * Pass the device's event `type`, `code`, `value` on: a keyboard's key to
 * the input layer, which gives it to the event device and the console and
 * repeats it, and every other event to the event device, as Linux's
 * virtinput_recv_events does; a mouse's packet, ended by SYN_REPORT, also
 * goes to the mouse queue unless an open holds the grab, as mousedev's.
 * A key's repeat from the device is dropped, since the input layer repeats.
 */
static void vi_event(struct vi_input *input, uint16_t type, uint16_t code, int32_t value)
{
    unsigned int bit;

    if (input->keyboard && type == EV_KEY) {
        if (value != 2) {
            input_keyboard_key(&input->kb, code, value != 0);
        }
        return;
    }
    evdev_report(input->evdev, type, code, value);
    if (!input->mouse) {
        return;
    }
    if (type == EV_KEY && code >= BTN_LEFT && code <= BTN_MIDDLE) {
        bit = 1u << (code - BTN_LEFT);
        input->buttons = value ? input->buttons | bit : input->buttons & ~bit;
        input->touched = 1;
    } else if (type == EV_REL && code == REL_X) {
        input->dx += value;
        input->touched = 1;
    } else if (type == EV_REL && code == REL_Y) {
        input->dy += value;
        input->touched = 1;
    } else if (type == EV_SYN && code == SYN_REPORT && input->touched) {
        /* The queue's Y is positive away from the user, REL_Y toward. */
        if (!evdev_grabbed(input->evdev)) {
            mouse_push_packet((uint8_t)(input->buttons & 0x07u), input->dx, -input->dy);
        }
        input->dx = 0;
        input->dy = 0;
        input->touched = 0;
    }
}

/** Pass a complete record on and give its buffer back to the queue. */
static void vi_done(struct virtqueue *vq, void *token, uint32_t len)
{
    struct vi_input *input = (struct vi_input *)vq->priv;
    struct vi_event *event = (struct vi_event *)token;

    if (len == sizeof(*event)) {
        vi_event(input, event->type, event->code, event->value);
    }
    (void)vi_queue(vq, event);
}

/** A status record the device has read is free again. */
static void vi_status_done(struct virtqueue *vq, void *token, uint32_t len)
{
    struct vi_input *input = (struct vi_input *)vq->priv;
    unsigned i = (unsigned)((struct vi_event *)token - input->sts);
    uint32_t flags;

    (void)len;
    flags = spin_lock_irqsave(&input->lock);
    input->sts_busy[i] = 0;
    spin_unlock_irqrestore(&input->lock, flags);
}

/**
 * Give the device the change of one of its LEDs on the status queue, as
 * Linux's virtinput_status does; the evdev output hook, called with the
 * evdev lock held and interrupts off. An event that finds every status
 * record under way, or the queue full, is dropped, as Linux's fails.
 */
static void vi_output(void *ctx, uint16_t type, uint16_t code, int32_t value)
{
    struct vi_input *input = (struct vi_input *)ctx;
    struct virtio_sg sg;
    uint32_t flags;
    unsigned i;

    if (type != EV_LED || code >= LED_CNT || !(input->leds & (1u << code))) {
        return;
    }
    flags = spin_lock_irqsave(&input->lock);
    for (i = 0; i < input->nsts && input->sts_busy[i]; i++) {
    }
    if (i < input->nsts) {
        input->sts_busy[i] = 1;
    }
    spin_unlock_irqrestore(&input->lock, flags);
    if (i == input->nsts) {
        return;
    }
    input->sts[i].type = type;
    input->sts[i].code = code;
    input->sts[i].value = value;
    sg.addr = input->sts_addr + i * sizeof(*input->sts);
    sg.len = sizeof(*input->sts);
    if (virtqueue_add(input->status, &sg, 1, 0, &input->sts[i]) != 0) {
        flags = spin_lock_irqsave(&input->lock);
        input->sts_busy[i] = 0;
        spin_unlock_irqrestore(&input->lock, flags);
        return;
    }
    virtqueue_kick(input->status);
}

/** Stop the LEDs' events, then DMA and interrupts, before taking away the
 * keyboard, the evdev node and the buffers. */
static void vi_remove(struct virtio_dev *d)
{
    struct vi_input *input = (struct vi_input *)d->priv;

    d->removed = NULL;
    evdev_set_output(input->evdev, NULL, NULL);
    virtio_teardown(d);
    d->priv = NULL;
    if (input->kb.dev) {
        input_keyboard_detach(&input->kb); /* releases its keys, unregisters */
    } else {
        evdev_unregister(input->evdev);
    }
    if (d->sys_bound) {
        evdev_sysfs_sync();
    }
    if (input->events) {
        virtio_dma_free(d, input->events, input->events_bytes);
    }
    if (input->sts) {
        virtio_dma_free(d, input->sts, input->sts_bytes);
    }
    kfree(input);
    virtio_driver_unbind(d);
    virtio_put(d);
}

/** Configure identity and capabilities while the event device is reserved:
 * a device that states EV_REP is a keyboard, and one with REL_X and REL_Y
 * otherwise a mouse, as Linux's input core and mousedev take them. */
static int vi_identify(struct virtio_dev *d, struct vi_input *input, char *name, uint16_t *ids,
                       uint8_t *keys)
{
    uint8_t props[INPUT_PROP_CNT / 8], rel[REL_CNT / 8], msc[MSC_CNT / 8], leds[LED_CNT / 8];
    char serial[EVDEV_TEXT_MAX], phys[EVDEV_TEXT_MAX];
    struct evdev *dev = input->evdev;
    unsigned i;

    vi_config(d, CFG_NAME, 0, name, EVDEV_NAME_MAX - 1);
    vi_config(d, CFG_SERIAL, 0, serial, sizeof(serial) - 1);
    serial[sizeof(serial) - 1] = '\0';
    if (vi_config(d, CFG_IDS, 0, ids, 4 * sizeof(*ids)) < 4 * sizeof(*ids)) {
        ids[0] = BUS_VIRTUAL;
        ids[1] = ids[2] = ids[3] = 0;
    }
    k_strncpy(phys, d->name ? d->name : "virtio", sizeof(phys) - sizeof("/input0"));
    phys[sizeof(phys) - sizeof("/input0")] = '\0';
    k_memcpy(phys + k_strlen(phys), "/input0", sizeof("/input0"));
    input->keyboard = vi_config(d, CFG_EV_BITS, EV_REP, rel, sizeof(rel)) > 0;
    evdev_describe(dev, phys, input->keyboard ? EVDEV_KEYBOARD : 0);
    evdev_set_sysfs_parent(dev, virtio_bus_dir(d));
    evdev_set_uniq(dev, serial);
    evdev_set_version(dev, ids[3]);

    vi_config(d, CFG_EV_BITS, EV_KEY, keys, EVDEV_KEY_BYTES);
    keys[0] &= (uint8_t)~1u; /* Linux's input_register_device clears KEY_RESERVED. */
    vi_config(d, CFG_EV_BITS, EV_REL, rel, sizeof(rel));
    for (i = 0; i < REL_CNT; i++) {
        if (vi_bit(rel, i)) {
            evdev_set_capability(dev, EV_REL, (uint16_t)i);
        }
    }
    input->mouse = !input->keyboard && vi_bit(rel, REL_X) && vi_bit(rel, REL_Y);
    vi_config(d, CFG_EV_BITS, EV_LED, leds, sizeof(leds));
    for (i = 0; i < LED_CNT; i++) {
        if (!vi_bit(leds, i)) {
            continue;
        }
        input->leds |= 1u << i;
        if (!input->keyboard || i > LED_SCROLLL) { /* a keyboard has the locks' */
            evdev_set_capability(dev, EV_LED, (uint16_t)i);
        }
    }
    vi_config(d, CFG_EV_BITS, EV_MSC, msc, sizeof(msc));
    for (i = 0; i < MSC_CNT; i++) {
        if (vi_bit(msc, i)) {
            evdev_set_capability(dev, EV_MSC, (uint16_t)i);
        }
    }
    vi_config(d, CFG_PROPS, 0, props, sizeof(props));
    for (i = 0; i < INPUT_PROP_CNT; i++) {
        if (vi_bit(props, i)) {
            evdev_set_prop(dev, i);
        }
    }
    return vi_axes(d, dev);
}

/** Probe a keyboard, mouse, tablet or touchscreen; the status queue
 * carries the LEDs' changes to the device. */
static int32_t vi_probe(struct virtio_dev *d)
{
    struct vi_input *input;
    struct virtqueue *events = 0, *status = 0;
    char name[EVDEV_NAME_MAX];
    uint8_t keys[EVDEV_KEY_BYTES];
    uint16_t ids[4];
    unsigned i;
    int rc;

    rc = virtio_driver_bind(d, "virtio_input");
    if (rc != 0) {
        virtio_put(d);
        return rc;
    }
    input = (struct vi_input *)kzalloc(sizeof(*input));
    if (!input) {
        virtio_driver_unbind(d);
        virtio_put(d);
        return -ENOMEM;
    }
    spin_lock_init(&input->lock, "virtio_input");
    d->priv = input;
    rc = virtio_init(d, 0, 2);
    if (rc != 0) {
        goto fail;
    }
    if (d->polled) {
        rc = -ENODEV;
        goto fail;
    }
    input->evdev = evdev_reserve();
    if (!input->evdev) {
        rc = -ENOSPC;
        goto fail;
    }
    name[sizeof(name) - 1] = '\0';
    rc = vi_identify(d, input, name, ids, keys);
    if (rc != 0) {
        goto fail;
    }
    rc = virtio_queue_setup(d, 0, vi_done, input, &events);
    if (rc == 0) {
        rc = virtio_queue_setup(d, 1, vi_status_done, input, &status);
    }
    if (rc != 0) {
        goto fail;
    }
    events->batch = virtqueue_kick;
    input->events_bytes = events->size * sizeof(*input->events);
    input->events = virtio_dma_alloc(d, input->events_bytes, &input->events_addr);
    if (!input->events) {
        rc = -ENOMEM;
        goto fail;
    }
    input->status = status;
    input->nsts = status->size < VIRTQ_MAX_SIZE ? status->size : VIRTQ_MAX_SIZE;
    input->sts_bytes = input->nsts * sizeof(*input->sts);
    input->sts = virtio_dma_alloc(d, input->sts_bytes, &input->sts_addr);
    if (!input->sts) {
        rc = -ENOMEM;
        goto fail;
    }
    d->removed = vi_remove;
    rc = virtio_ready(d);
    if (rc != 0) {
        goto fail;
    }
    evdev_register_reserved(input->evdev, name, ids[0], ids[1], ids[2], keys);
    /* The hook first, so that the LEDs lit at the attach reach the device. */
    evdev_set_output(input->evdev, vi_output, input);
    if (input->keyboard) {
        input_keyboard_attach_dev(&input->kb, input->evdev);
    }
    if (input->mouse) {
        input_mouse_found();
    }
    for (i = 0; i < events->size; i++) {
        rc = vi_queue(events, &input->events[i]);
        if (rc != 0) {
            goto fail;
        }
    }
    virtqueue_kick(events);
    virtio_driver_bound(d);
    klog(KLOG_INFO, "virtio_input", "bound: %s: %s", d->name, name);
    return 0;

fail:
    vi_remove(d);
    return rc;
}

void virtio_input_start(void)
{
    struct virtio_dev *d;
    int i;

    if (virtio_driver_register(VIRTIO_ID_INPUT, vi_probe) != 0) {
        klog(KLOG_WARN, "virtio_input", "driver registration failed");
    }
    for (i = 0; virtio_pci_find(VIRTIO_ID_INPUT, i, &d) == 0; i++) {
        (void)vi_probe(d);
    }
}
