/*
 * virtio_input_hosttest.c: the virtio input driver,
 * rach2/sys/virtio/virtio_input.c, against a model of QEMU's virtio input
 * devices, with the virtio core, the event devices and the input layer
 * standing in as records of what the driver asked of them.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 *
 * The model is a device's configuration as QEMU's hw/input/virtio-input.c
 * answers it (virtio 1.2, 5.8.2): a write of `select` and `subsel`, then
 * `size` and the bytes of the item, 0 for an item the device lacks. Its
 * items are those of QEMU's virtio-keyboard-pci, virtio-mouse-pci and
 * virtio-tablet-pci (hw/input/virtio-input-hid.c): the keyboard's keys,
 * EV_REP and the three lock LEDs; the mouse's five buttons, REL_X, REL_Y
 * and the wheel; the tablet's buttons and ABS_X and ABS_Y of 0 to 32767.
 * The event queue's buffers are filled as the device fills them, one
 * record of eight bytes each, and the status queue's records are read and
 * kept until a row returns them, as QEMU's handle_sts reads them. The event
 * devices keep the capabilities, the reports and the output hook the
 * driver gave them; the input layer keeps the keyboard attached, its keys
 * and the mouse queue's packets.
 *
 * The rows: a keyboard, which binds as a keyboard of the input layer, gives
 * its keys there and its other events to its event device, and is told
 * each change of its LEDs on the status queue, those after every status
 * record is the device's dropped; a mouse, which binds with its buttons and
 * axes and moves the mouse queue's pointer by each packet unless an open
 * holds the grab; the tablet, as it bound before; a device with no
 * capability at all, which Linux's virtinput_probe takes as well; and each
 * removal, which leaves nothing allocated and no output hook.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

extern int printf(const char *format, ...);
extern int strcmp(const char *a, const char *b);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *p);

#include "virtio/virtio_input.c"

/* Linux's codes of the keys and LED the rows use. */
#define KEY_RESERVED 0
#define KEY_ESC      1
#define KEY_A        30
#define KEY_CAPSLOCK 58
#define LED_MISC     8

static int failures;

/** Record a failed invariant. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("virtio-input-hosttest: fail: %s\n", what);
        failures++;
    }
}

uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/* ---- the event devices ---- */

#define REPORTS_MAX 64

struct report {
    uint16_t type, code;
    int32_t value;
};

struct evdev {
    int reserved, registered, unregistered;
    unsigned int flags;
    char name[EVDEV_NAME_MAX];
    uint16_t id[3];
    uint8_t keys[EVDEV_KEY_BYTES];
    uint8_t rel[REL_CNT / 8];
    unsigned int leds; /* the LEDs given with evdev_set_capability */
    uint8_t abs[ABS_CNT / 8];
    int32_t abs_max[ABS_CNT];
    int slots;
    struct report reports[REPORTS_MAX];
    int nreports;
    evdev_output_t output;
    void *output_ctx;
    unsigned int lit; /* the LEDs the hook was last told of */
    int grabbed;
};

static struct evdev the_evdev;

struct evdev *evdev_reserve(void)
{
    k_memset(&the_evdev, 0, sizeof(the_evdev));
    the_evdev.reserved = 1;
    return &the_evdev;
}

struct evdev *evdev_register_reserved(struct evdev *dev, const char *name, uint16_t bustype,
                                      uint16_t vendor, uint16_t product, const uint8_t *keys)
{
    unsigned i;

    dev->registered = 1;
    k_strncpy(dev->name, name, sizeof(dev->name) - 1u);
    dev->id[0] = bustype;
    dev->id[1] = vendor;
    dev->id[2] = product;
    for (i = 0; i < EVDEV_KEY_BYTES; i++) {
        dev->keys[i] |= keys ? keys[i] : 0;
    }
    return dev;
}

void evdev_describe(struct evdev *dev, const char *phys, unsigned int flags)
{
    (void)phys;
    dev->flags = flags;
}

void evdev_set_sysfs_parent(struct evdev *dev, int parent)
{
    (void)dev;
    (void)parent;
}

void evdev_set_uniq(struct evdev *dev, const char *uniq)
{
    (void)dev;
    (void)uniq;
}

void evdev_set_version(struct evdev *dev, uint16_t version)
{
    (void)dev;
    (void)version;
}

void evdev_set_capability(struct evdev *dev, uint16_t type, uint16_t code)
{
    if (type == EV_REL && code < REL_CNT) {
        dev->rel[code / 8] |= (uint8_t)(1u << (code % 8));
    } else if (type == EV_LED && code < LED_CNT) {
        dev->leds |= 1u << code;
    }
}

void evdev_set_abs(struct evdev *dev, uint16_t code, int32_t minimum, int32_t maximum, int32_t fuzz,
                   int32_t flat)
{
    (void)minimum;
    (void)fuzz;
    (void)flat;
    dev->abs[code / 8] |= (uint8_t)(1u << (code % 8));
    dev->abs_max[code] = maximum;
}

void evdev_set_abs_res(struct evdev *dev, uint16_t code, int32_t resolution)
{
    (void)dev;
    (void)code;
    (void)resolution;
}

void evdev_set_prop(struct evdev *dev, unsigned int prop)
{
    (void)dev;
    (void)prop;
}

int evdev_init_slots(struct evdev *dev, int num_slots, unsigned int flags)
{
    (void)flags;
    dev->slots = num_slots;
    return 0;
}

void evdev_report(struct evdev *dev, uint16_t type, uint16_t code, int32_t value)
{
    if (dev && dev->nreports < REPORTS_MAX) {
        dev->reports[dev->nreports].type = type;
        dev->reports[dev->nreports].code = code;
        dev->reports[dev->nreports].value = value;
        dev->nreports++;
    }
}

int evdev_grabbed(struct evdev *dev)
{
    return dev->grabbed;
}

void evdev_set_output(struct evdev *dev, evdev_output_t fn, void *ctx)
{
    if (dev) {
        dev->output = fn;
        dev->output_ctx = ctx;
    }
}

void evdev_unregister(struct evdev *dev)
{
    if (dev) {
        dev->unregistered = 1;
        dev->output = NULL;
        dev->output_ctx = NULL;
    }
}

void evdev_sysfs_sync(void)
{}

/** Light the LEDs `mask` of the event device, a bit 1 << LED_* each, as
 * evdev.c does for the console's locks and a program's EV_LED: the output
 * hook is told of each LED that changes. */
static void leds_lit(unsigned int mask)
{
    unsigned i;

    for (i = 0; i < LED_CNT; i++) {
        if (((the_evdev.lit ^ mask) >> i) & 1u) {
            if (the_evdev.output) {
                the_evdev.output(the_evdev.output_ctx, EV_LED, (uint16_t)i,
                                 (int32_t)((mask >> i) & 1u));
            }
        }
    }
    the_evdev.lit = mask;
}

/* ---- the input layer ---- */

#define KEYS_MAX 16

static struct input_keyboard *attached;
static int detached;
static struct report keys_in[KEYS_MAX];
static int nkeys;
static int mouse_found;
static int packets, packet_buttons, packet_dx, packet_dy;

void input_keyboard_attach_dev(struct input_keyboard *kb, struct evdev *dev)
{
    kb->dev = dev;
    attached = kb;
    leds_lit(the_evdev.lit | (1u << LED_NUML)); /* the console's Num Lock, lit */
}

void input_keyboard_detach(struct input_keyboard *kb)
{
    detached++;
    attached = NULL;
    evdev_unregister(kb->dev);
    kb->dev = NULL;
}

void input_keyboard_key(struct input_keyboard *kb, unsigned int keycode, int down)
{
    check(kb == attached, "a key goes to the keyboard attached");
    if (nkeys < KEYS_MAX) {
        keys_in[nkeys].type = EV_KEY;
        keys_in[nkeys].code = (uint16_t)keycode;
        keys_in[nkeys].value = down;
        nkeys++;
    }
}

void input_mouse_found(void)
{
    mouse_found = 1;
}

void mouse_push_packet(uint8_t buttons, int dx, int dy)
{
    packets++;
    packet_buttons = buttons;
    packet_dx = dx;
    packet_dy = dy;
}

/* ---- the virtio core and the model device ---- */

#define QSIZE   64 /* QEMU's virtio-input queues */
#define ITEMS   16
#define STS_MAX 128

struct item {
    uint8_t select, subsel, size;
    uint8_t data[128];
};

struct pending {
    uint64_t addr;
    void *token;
};

static struct virtio_dev dev;
static struct item items[ITEMS];
static int nitems;
static uint8_t sel, subsel;
static struct virtqueue *queues[2];
static struct pending offered[2][QSIZE];
static int noffered[2];
static int kicks[2];
static struct report sts[STS_MAX]; /* the status records read */
static int nsts;
static int dma_out;
static int bound, unbound, puts, torn;

/** Add the configuration item `select`, `subsel` of `size` bytes. */
static struct item *item(uint8_t select, uint8_t sub, uint8_t size)
{
    struct item *it = &items[nitems++];

    k_memset(it, 0, sizeof(*it));
    it->select = select;
    it->subsel = sub;
    it->size = size;
    return it;
}

/** Set bit `code` of a bitmap item. */
static void bit(struct item *it, unsigned code)
{
    it->data[code / 8] |= (uint8_t)(1u << (code % 8));
}

/** The item a read of the configuration finds now, or NULL. */
static const struct item *selected(void)
{
    int i;

    for (i = 0; i < nitems; i++) {
        if (items[i].select == sel && items[i].subsel == subsel) {
            return &items[i];
        }
    }
    return NULL;
}

void virtio_config_write(struct virtio_dev *d, uint32_t offset, const void *buf, uint32_t len)
{
    (void)d;
    check(len == 1 && offset <= 1, "the driver writes only select and subsel");
    if (offset == 0) {
        sel = *(const uint8_t *)buf;
    } else {
        subsel = *(const uint8_t *)buf;
    }
}

void virtio_config_read(struct virtio_dev *d, uint32_t offset, void *buf, uint32_t len)
{
    const struct item *it = selected();

    (void)d;
    if (offset == CFG_SIZE) {
        check(len == 1, "size is one byte");
        *(uint8_t *)buf = it ? it->size : 0;
        return;
    }
    check(offset == CFG_DATA && it && len <= it->size, "a read of the data stays in the item");
    if (it) {
        k_memcpy(buf, it->data, len);
    }
}

int virtio_driver_bind(struct virtio_dev *d, const char *driver)
{
    (void)d;
    check(strcmp(driver, "virtio_input") == 0, "the driver binds by its name");
    return 0;
}

void virtio_driver_bound(struct virtio_dev *d)
{
    (void)d;
    bound++;
}

void virtio_driver_unbind(struct virtio_dev *d)
{
    (void)d;
    unbound++;
}

void virtio_put(struct virtio_dev *d)
{
    (void)d;
    puts++;
}

int virtio_init(struct virtio_dev *d, uint64_t wanted, uint16_t nqueues)
{
    (void)d;
    (void)wanted;
    check(nqueues == 2, "the driver asks for the event and status queues");
    return 0;
}

int virtio_queue_setup(struct virtio_dev *d, uint16_t index, virtqueue_done_t done, void *priv,
                       struct virtqueue **out)
{
    struct virtqueue *vq;

    if (index > 1) {
        return -ENOENT;
    }
    vq = (struct virtqueue *)calloc(1, sizeof(*vq));
    vq->dev = d;
    vq->index = index;
    vq->size = QSIZE;
    vq->done = done;
    vq->priv = priv;
    queues[index] = vq;
    d->queues[index] = vq;
    *out = vq;
    return 0;
}

int virtio_ready(struct virtio_dev *d)
{
    (void)d;
    return 0;
}

void virtio_teardown(struct virtio_dev *d)
{
    int i;

    torn++;
    for (i = 0; i < 2; i++) {
        free(queues[i]);
        queues[i] = NULL;
        d->queues[i] = NULL;
        noffered[i] = 0;
    }
}

void *virtio_dma_alloc(struct virtio_dev *d, uint32_t bytes, uint64_t *addr)
{
    void *p = calloc(1, bytes);

    (void)d;
    *addr = (uint64_t)(uintptr_t)p;
    dma_out++;
    return p;
}

void virtio_dma_free(struct virtio_dev *d, void *p, uint32_t bytes)
{
    (void)d;
    (void)bytes;
    free(p);
    dma_out--;
}

int virtio_bus_dir(const struct virtio_dev *d)
{
    (void)d;
    return 0;
}

int virtio_pci_find(uint16_t id, int nth, struct virtio_dev **out)
{
    (void)id;
    (void)nth;
    (void)out;
    return -ENODEV;
}

int virtio_driver_register(uint16_t id, virtio_probe_t probe)
{
    (void)id;
    (void)probe;
    return 0;
}

int virtqueue_add(struct virtqueue *vq, const struct virtio_sg *sg, unsigned out, unsigned in,
                  void *token)
{
    int q = vq->index;

    check(out + in == 1 && sg[0].len == sizeof(struct vi_event),
          "each buffer is one record of eight bytes");
    check(q == 0 ? in == 1 : out == 1, "the event queue's buffers are written, the status's read");
    if (noffered[q] == QSIZE) {
        return -ENOSPC;
    }
    offered[q][noffered[q]].addr = sg[0].addr;
    offered[q][noffered[q]].token = token;
    noffered[q]++;
    if (q == 1 && nsts < STS_MAX) {
        const struct vi_event *e = (const struct vi_event *)(uintptr_t)sg[0].addr;

        sts[nsts].type = e->type;
        sts[nsts].code = e->code;
        sts[nsts].value = e->value;
        nsts++;
    }
    return 0;
}

void virtqueue_kick(struct virtqueue *vq)
{
    kicks[vq->index]++;
}

/** The device fills the oldest buffer of the event queue with the event
 * `type`, `code`, `value` and returns it. */
static void send(uint16_t type, uint16_t code, int32_t value)
{
    struct vi_event *e;
    struct pending p;

    check(noffered[0] > 0, "the event queue has a buffer for the device");
    if (noffered[0] == 0) {
        return;
    }
    p = offered[0][0];
    k_memcpy(&offered[0][0], &offered[0][1], (unsigned)(noffered[0] - 1) * sizeof(p));
    noffered[0]--;
    e = (struct vi_event *)(uintptr_t)p.addr;
    e->type = type;
    e->code = code;
    e->value = value;
    queues[0]->done(queues[0], p.token, sizeof(*e));
}

/** The device returns every status record it read. */
static void status_return(void)
{
    while (noffered[1] > 0) {
        struct pending p = offered[1][--noffered[1]];

        queues[1]->done(queues[1], p.token, 0);
    }
}

/** A fresh machine with a device of no configuration. */
static void fresh(void)
{
    k_memset(&dev, 0, sizeof(dev));
    dev.name = "virtio0";
    dev.id = VIRTIO_ID_INPUT;
    k_memset(&the_evdev, 0, sizeof(the_evdev));
    nitems = 0;
    k_memset(noffered, 0, sizeof(noffered));
    k_memset(kicks, 0, sizeof(kicks));
    nsts = 0;
    attached = NULL;
    detached = nkeys = mouse_found = packets = 0;
    bound = unbound = puts = torn = 0;
}

/** The name and ids every QEMU device gives. */
static void identity(const char *name)
{
    struct item *it = item(CFG_NAME, 0, (uint8_t)k_strlen(name));

    k_memcpy(it->data, name, k_strlen(name));
    it = item(CFG_IDS, 0, 8);
    it->data[0] = BUS_VIRTUAL;
    it->data[2] = 0x27; /* QEMU's vendor, 0x0627 */
    it->data[3] = 0x06;
    it->data[4] = 1;
    it->data[6] = 1;
}

/** Remove the bound device as its transport does, and check that it left
 * nothing behind. */
static void removed(const char *what)
{
    dev.removed(&dev);
    check(torn == 1 && the_evdev.unregistered && !the_evdev.output && dma_out == 0 &&
              unbound == 1 && puts == 1 && dev.priv == NULL,
          what);
}

/* ---- the rows ---- */

/** The bound keyboard's LEDs: each change of one it has is told the
 * device on the status queue, and one that finds every status record the
 * device's is dropped until the device returns them. */
static void keyboard_leds(void)
{
    int i, before;

    leds_lit((1u << LED_NUML) | (1u << LED_CAPSL));
    check(nsts == 2 && sts[1].code == LED_CAPSL && sts[1].value == 1, "Caps Lock lit is told");
    leds_lit(1u << LED_CAPSL);
    check(nsts == 3 && sts[2].code == LED_NUML && sts[2].value == 0, "Num Lock put out is told");
    leds_lit((1u << LED_CAPSL) | (1u << LED_MISC));
    check(nsts == 3, "an LED the device lacks is not");
    status_return();
    before = nsts;
    for (i = 0; i < QSIZE + 4; i++) {
        leds_lit((i & 1) ? 0 : 1u << LED_SCROLLL);
    }
    check(nsts == before + QSIZE && noffered[1] == QSIZE,
          "a change that finds every status record the device's is dropped");
    status_return();
    leds_lit(1u << LED_CAPSL);
    check(nsts == before + QSIZE + 1, "a record the device returned is used again");
}

/** QEMU's virtio-keyboard-pci. */
static void keyboard(void)
{
    struct item *it;

    fresh();
    identity("QEMU Virtio Keyboard");
    it = item(CFG_EV_BITS, EV_KEY, 32);
    bit(it, KEY_ESC);
    bit(it, KEY_A);
    bit(it, KEY_CAPSLOCK);
    bit(it, KEY_RESERVED);
    item(CFG_EV_BITS, EV_REP, 1);
    it = item(CFG_EV_BITS, EV_LED, 1);
    bit(it, LED_NUML);
    bit(it, LED_CAPSL);
    bit(it, LED_SCROLLL);
    check(vi_probe(&dev) == 0 && bound == 1, "a keyboard with no absolute axis binds");
    check(the_evdev.registered && strcmp(the_evdev.name, "QEMU Virtio Keyboard") == 0 &&
              the_evdev.id[0] == BUS_VIRTUAL && the_evdev.id[1] == 0x0627,
          "its event device has its name and ids");
    check(the_evdev.flags == EVDEV_KEYBOARD && attached && attached->dev == &the_evdev &&
              !mouse_found,
          "a device that states EV_REP is a keyboard of the input layer");
    check((the_evdev.keys[KEY_A / 8] >> (KEY_A % 8)) & 1u && !(the_evdev.keys[0] & 1u),
          "its keys are the device's, less KEY_RESERVED");
    check(the_evdev.leds == 0, "its lock LEDs are the keyboard's, not more of its own");
    check(noffered[0] == QSIZE && kicks[0] >= 1, "every event buffer is the device's");
    check(nsts == 1 && sts[0].type == EV_LED && sts[0].code == LED_NUML && sts[0].value == 1 &&
              kicks[1] == 1,
          "the LED lit at the attach is told the device");

    send(EV_KEY, KEY_A, 1);
    send(EV_SYN, SYN_REPORT, 0);
    send(EV_KEY, KEY_A, 2);
    send(EV_KEY, KEY_A, 0);
    check(nkeys == 2 && keys_in[0].code == KEY_A && keys_in[0].value == 1 &&
              keys_in[1].code == KEY_A && keys_in[1].value == 0,
          "a key goes to the input layer, its press and release, and the device's repeat not");
    check(the_evdev.nreports == 1 && the_evdev.reports[0].type == EV_SYN,
          "the keyboard's other events go to its event device");
    check(noffered[0] == QSIZE, "each event buffer goes back to the device");

    keyboard_leds();
    removed("the keyboard's removal leaves nothing allocated and no output hook");
    check(detached == 1, "and detaches the keyboard");
}

/** QEMU's virtio-mouse-pci. */
static void mouse(void)
{
    struct item *it;

    fresh();
    identity("QEMU Virtio Mouse");
    it = item(CFG_EV_BITS, EV_KEY, 35);
    bit(it, BTN_LEFT);
    bit(it, BTN_RIGHT);
    bit(it, BTN_MIDDLE);
    bit(it, BTN_SIDE);
    bit(it, BTN_EXTRA);
    it = item(CFG_EV_BITS, EV_REL, 2);
    bit(it, REL_X);
    bit(it, REL_Y);
    bit(it, REL_WHEEL);
    check(vi_probe(&dev) == 0 && bound == 1, "a mouse with no absolute axis binds");
    check(the_evdev.flags == 0 && !attached && mouse_found, "a mouse is no keyboard, and is found");
    check(the_evdev.rel[0] == ((1u << REL_X) | (1u << REL_Y)) &&
              (the_evdev.rel[1] & (1u << (REL_WHEEL - 8))) &&
              (the_evdev.keys[BTN_LEFT / 8] >> (BTN_LEFT % 8)) & 1u,
          "its axes and buttons are the device's");

    send(EV_KEY, BTN_LEFT, 1);
    send(EV_REL, REL_X, 5);
    send(EV_REL, REL_Y, 3);
    send(EV_SYN, SYN_REPORT, 0);
    check(the_evdev.nreports == 4 && the_evdev.reports[1].type == EV_REL &&
              the_evdev.reports[1].value == 5,
          "its events go to its event device");
    check(packets == 1 && packet_buttons == 1 && packet_dx == 5 && packet_dy == -3,
          "and its packet to the mouse queue, Y turned away from the user");
    send(EV_REL, REL_WHEEL, 1);
    send(EV_SYN, SYN_REPORT, 0);
    check(packets == 1, "a packet of the wheel alone moves no pointer");
    the_evdev.grabbed = 1;
    send(EV_REL, REL_X, 1);
    send(EV_SYN, SYN_REPORT, 0);
    check(packets == 1, "nor one while an open holds the grab");
    the_evdev.grabbed = 0;
    send(EV_KEY, BTN_LEFT, 0);
    send(EV_SYN, SYN_REPORT, 0);
    check(packets == 2 && packet_buttons == 0 && packet_dx == 0, "the release is a packet");
    leds_lit(1u << LED_CAPSL);
    check(nsts == 0, "a device of no LEDs is told none");
    removed("the mouse's removal leaves nothing allocated and no output hook");
}

/** QEMU's virtio-tablet-pci, as it bound before. */
static void tablet(void)
{
    struct item *it;

    fresh();
    identity("QEMU Virtio Tablet");
    it = item(CFG_EV_BITS, EV_KEY, 35);
    bit(it, BTN_LEFT);
    it = item(CFG_EV_BITS, EV_ABS, 1);
    bit(it, ABS_X);
    bit(it, ABS_Y);
    it = item(CFG_ABS_INFO, ABS_X, 20);
    it->data[4] = 0xff;
    it->data[5] = 0x7f;
    it = item(CFG_ABS_INFO, ABS_Y, 20);
    it->data[4] = 0xff;
    it->data[5] = 0x7f;
    check(vi_probe(&dev) == 0 && the_evdev.abs_max[ABS_X] == 32767 &&
              the_evdev.abs_max[ABS_Y] == 32767 && the_evdev.slots == 0,
          "a tablet binds with its axes");
    check(!attached && !mouse_found, "and is neither keyboard nor mouse");
    removed("the tablet's removal leaves nothing allocated");
}

/** A device that states nothing, which Linux's virtinput_probe takes. */
static void bare(void)
{
    fresh();
    identity("bare");
    check(vi_probe(&dev) == 0 && the_evdev.registered, "a device of no capability binds");
    removed("its removal leaves nothing allocated");
}

int main(void)
{
    keyboard();
    mouse();
    tablet();
    bare();
    if (failures) {
        printf("virtio-input-hosttest: FAIL: %d\n", failures);
        return 1;
    }
    printf("virtio-input-hosttest: PASS\n");
    return 0;
}
