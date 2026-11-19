/* virtio_input.c: tablets and touchscreens on the virtio input transport. */

#include "virtio_input.h"

#include "../evdev.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
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

/** Set the advertised axes and initialize the multitouch slots, if any. */
static int vi_axes(struct virtio_dev *d, struct evdev *evdev)
{
    uint8_t axes[ABS_CNT / 8];
    int32_t abs[5]; /* minimum, maximum, fuzz, flat and resolution */
    int32_t slots = 0;
    unsigned i;

    vi_config(d, CFG_EV_BITS, EV_ABS, axes, sizeof(axes));
    if (!(vi_bit(axes, ABS_X) && vi_bit(axes, ABS_Y)) &&
        !(vi_bit(axes, ABS_MT_SLOT) && vi_bit(axes, ABS_MT_POSITION_X) &&
          vi_bit(axes, ABS_MT_POSITION_Y))) {
        return -ENODEV;
    }
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

/** Pass a complete record to evdev and give its buffer back to the queue. */
static void vi_done(struct virtqueue *vq, void *token, uint32_t len)
{
    struct vi_input *input = (struct vi_input *)vq->priv;
    struct vi_event *event = (struct vi_event *)token;

    if (len == sizeof(*event)) {
        evdev_report(input->evdev, event->type, event->code, event->value);
    }
    (void)vi_queue(vq, event);
}

/** Stop DMA and interrupts before taking away the evdev node and buffers. */
static void vi_remove(struct virtio_dev *d)
{
    struct vi_input *input = (struct vi_input *)d->priv;

    d->removed = NULL;
    virtio_teardown(d);
    d->priv = NULL;
    evdev_unregister(input->evdev);
    if (input->events) {
        virtio_dma_free(d, input->events, input->events_bytes);
    }
    kfree(input);
    virtio_driver_unbind(d);
    virtio_put(d);
}

/** Configure identity and capabilities while the event device is reserved. */
static int vi_identify(struct virtio_dev *d, struct evdev *dev, char *name, uint16_t *ids,
                       uint8_t *keys)
{
    uint8_t props[INPUT_PROP_CNT / 8], rel[REL_CNT / 8];
    char serial[EVDEV_TEXT_MAX], phys[EVDEV_TEXT_MAX];
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
    evdev_describe(dev, phys, 0);
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
    vi_config(d, CFG_PROPS, 0, props, sizeof(props));
    for (i = 0; i < INPUT_PROP_CNT; i++) {
        if (vi_bit(props, i)) {
            evdev_set_prop(dev, i);
        }
    }
    return vi_axes(d, dev);
}

/** Probe a tablet and leave its status queue empty. */
static int32_t vi_probe(struct virtio_dev *d)
{
    struct vi_input *input;
    struct virtqueue *events, *status;
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
    rc = vi_identify(d, input->evdev, name, ids, keys);
    if (rc != 0) {
        goto fail;
    }
    rc = virtio_queue_setup(d, 0, vi_done, input, &events);
    if (rc == 0) {
        rc = virtio_queue_setup(d, 1, NULL, NULL, &status);
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
    d->removed = vi_remove;
    rc = virtio_ready(d);
    if (rc != 0) {
        goto fail;
    }
    evdev_register_reserved(input->evdev, name, ids[0], ids[1], ids[2], keys);
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
