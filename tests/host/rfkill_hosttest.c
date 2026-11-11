/**
 * rfkill_hosttest.c: the rfkill core against two model owners: registration
 * and its refusals, the events of /dev/rfkill as BusyBox's and
 * wpa_supplicant's readers take them, writes by index and by type, the hard
 * block, the extended event, the attributes of /sys/class/rfkill and their
 * writes, and a full queue.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/lib/kstd.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 *
 * `rfkill.c` is included whole, so that the test sees its open list. A user
 * pointer is a host pointer, and the address 1 faults.
 */
#include "rfkill.c"

extern int printf(const char *format, ...);
extern int memcmp(const void *a, const void *b, unsigned long n);

static int failures, wakes;
static uint32_t masked; /* interrupts off, as the spinlocks leave them */
static int calls[2], last_block[2], refuse[2];
static void (*on_wait)(void);
static struct rfkill *wifi, *bt;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("rfkill_hosttest: line %d: %s\n", (int)__LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

/** Turn interrupts off, as the model has them, giving the old state. */
uint32_t kirq_save(void)
{
    uint32_t old = masked;

    masked = 1;
    return old;
}

/** Put interrupts back as `kirq_save` found them. */
void kirq_restore(uint32_t flags)
{
    masked = flags;
}

/** Count the readers' wakes; none comes with a lock held. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
    CHECK(!rfkill_lock.locked && !masked);
    wakes++;
}

/** A wait runs `on_wait`, standing for another process's change, then
 * ends as a signal would when nothing came. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    if (on_wait)
        on_wait();
    return cond(arg) ? 0 : -EINTR;
}

/** Copy to a host pointer; the address 1 faults. */
int copy_to_user(void __user *dst, const void *src, size_t len)
{
    if ((uintptr_t)dst == 1)
        return -EFAULT;
    k_memcpy((void *)dst, src, len);
    return 0;
}

/** Copy from a host pointer; the address 1 faults. */
int copy_from_user(void *dst, const void __user *src, size_t len)
{
    if ((uintptr_t)src == 1)
        return -EFAULT;
    k_memcpy(dst, (const void *)src, len);
    return 0;
}

/** The model owner `*data`: count the call, or refuse it. */
static int owner_block(void *data, int blocked)
{
    int n = *(int *)data;

    calls[n]++;
    last_block[n] = blocked;
    return refuse[n];
}

static int owner_ids[2] = {0, 1};
static const struct rfkill_ops owner_ops = {owner_block};
static const struct rfkill_ops no_ops = {0};

/** An open of /dev/rfkill with its own description. */
struct reader {
    fd_entry_t e;
    struct open_file ofd;
};

/** Open `r`, nonblocking unless `blocking`. */
static void reader_open(struct reader *r, int blocking)
{
    k_memset(r, 0, sizeof(*r));
    r->e.ofd = &r->ofd;
    r->ofd.status_flags = blocking ? 0u : OFD_NONBLOCK;
    CHECK(rfkill_open(&r->e) == 0 && r->e.priv);
}

/** Read one event of 8 bytes and check its fields. */
static void expect(struct reader *r, unsigned index, unsigned type, unsigned op, unsigned soft,
                   unsigned hard)
{
    uint8_t ev[RFKILL_EVENT_SIZE_EXT];
    uint32_t got;

    k_memset(ev, 0xEE, sizeof(ev));
    CHECK(rfkill_read(&r->e, (char __user *)ev, sizeof(ev)) == RFKILL_EVENT_SIZE_V1);
    k_memcpy(&got, ev, 4);
    CHECK(got == index && ev[4] == type && ev[5] == op && ev[6] == soft && ev[7] == hard);
    CHECK(ev[8] == 0xEE);
}

/** Expect no event: EAGAIN, and poll with only POLLOUT. */
static void expect_none(struct reader *r)
{
    uint8_t ev[8];
    struct wait_queue *wq = 0;

    CHECK(rfkill_read(&r->e, (char __user *)ev, sizeof(ev)) == -EAGAIN);
    CHECK(rfkill_poll(&r->e, POLLIN | POLLOUT, &wq) == POLLOUT && wq == &rfkill_wq);
}

/** Write the event of `index`, `type`, `op` and `soft` in `count` bytes. */
static int32_t put_event(struct reader *r, uint32_t index, unsigned type, unsigned op,
                         unsigned soft, uint32_t count)
{
    uint8_t ev[RFKILL_EVENT_SIZE_EXT];

    k_memcpy(ev, &index, 4);
    ev[4] = (uint8_t)type;
    ev[5] = (uint8_t)op;
    ev[6] = (uint8_t)soft;
    ev[7] = 1; /* the hard block a writer gives is ignored */
    ev[8] = 0;
    return rfkill_write(&r->e, (const char __user *)ev, count);
}

/** Whether attribute `attr` of `index` reads as `text`. */
static int shows(int index, int attr, const char *text)
{
    char out[160];
    int len = rfkill_show(index, attr, out, (int)sizeof(out));

    return len == (int)k_strlen(text) && memcmp(out, text, (unsigned long)len) == 0;
}

/** Registration's refusals, and the two model switches. */
static void registration(void)
{
    struct rfkill *rf = (struct rfkill *)1;
    static const char long_name[] = "abcdefghijklmnopqrstuvwxyz012345";

    CHECK(rfkill_register("phy0", RFKILL_TYPE_WLAN, 0, &owner_ops, 0, 0) == -EINVAL);
    CHECK(rfkill_register("phy0", RFKILL_TYPE_ALL, 0, &owner_ops, 0, &rf) == -EINVAL && !rf);
    CHECK(rfkill_register("phy0", RFKILL_NUM_TYPES, 0, &owner_ops, 0, &rf) == -EINVAL);
    CHECK(rfkill_register("", RFKILL_TYPE_WLAN, 0, &owner_ops, 0, &rf) == -EINVAL);
    CHECK(rfkill_register(long_name, RFKILL_TYPE_WLAN, 0, &owner_ops, 0, &rf) == -EINVAL);
    CHECK(rfkill_register("phy0", RFKILL_TYPE_WLAN, 0, 0, 0, &rf) == -EINVAL);
    CHECK(rfkill_register("phy0", RFKILL_TYPE_WLAN, 0, &no_ops, 0, &rf) == -EINVAL);
    CHECK(rfkill_nth(0) == -1 && !rfkill_present(0));
    CHECK(rfkill_register("phy0", RFKILL_TYPE_WLAN, 0, &owner_ops, &owner_ids[0], &wifi) == 0);
    CHECK(rfkill_register("hci0", RFKILL_TYPE_BLUETOOTH, -1, &owner_ops, &owner_ids[1], &bt) == 0);
    CHECK(wifi && bt && rfkill_index(wifi) == 0 && rfkill_index(bt) == 1);
    CHECK(rfkill_nth(0) == 0 && rfkill_nth(1) == 1 && rfkill_nth(2) == -1);
    CHECK(rfkill_phy(0) == 0 && rfkill_phy(1) == -1 && rfkill_phy(2) == -1);
    CHECK(!rfkill_soft_blocked(wifi) && calls[0] == 0);
}

/** The ADD events an open starts with, and the attributes. */
static void opening(struct reader *a)
{
    char small[4];

    reader_open(a, 0);
    expect(a, 0, RFKILL_TYPE_WLAN, RFKILL_OP_ADD, 0, 0);
    expect(a, 1, RFKILL_TYPE_BLUETOOTH, RFKILL_OP_ADD, 0, 0);
    expect_none(a);
    CHECK(rfkill_lookup("rfkill0", 7) == 0 && rfkill_lookup("rfkill1", 7) == 1);
    CHECK(rfkill_lookup("rfkill2", 7) == -1 && rfkill_lookup("rfkill01", 8) == -1);
    CHECK(rfkill_lookup("rfkill", 6) == -1 && rfkill_lookup("rfkillx", 7) == -1);
    CHECK(rfkill_lookup("rfkilm0", 7) == -1);
    CHECK(shows(0, RFKILL_ATTR_NAME, "phy0\n") && shows(0, RFKILL_ATTR_TYPE, "wlan\n"));
    CHECK(shows(1, RFKILL_ATTR_TYPE, "bluetooth\n") && shows(1, RFKILL_ATTR_INDEX, "1\n"));
    CHECK(shows(0, RFKILL_ATTR_SOFT, "0\n") && shows(0, RFKILL_ATTR_HARD, "0\n"));
    CHECK(shows(0, RFKILL_ATTR_STATE, "1\n") && shows(0, RFKILL_ATTR_PERSIST, "0\n"));
    CHECK(shows(0, RFKILL_ATTR_REASONS, "0x0\n"));
    CHECK(shows(0, RFKILL_ATTR_UEVENT,
                "RFKILL_NAME=phy0\nRFKILL_TYPE=wlan\nRFKILL_STATE=1\n"
                "RFKILL_HW_BLOCK_REASON=0x0\n"));
    CHECK(rfkill_show(2, RFKILL_ATTR_NAME, 0, 0) == -ENOENT);
    CHECK(rfkill_show(0, RFKILL_ATTRS, 0, 0) == -ENOENT && rfkill_show(0, -1, 0, 0) == -ENOENT);
    CHECK(rfkill_show(0, RFKILL_ATTR_NAME, small, (int)sizeof(small)) == -ENOSPC);
}

/** Writes by type and by index, their refusals, and an owner's refusal. */
static void writing(struct reader *a)
{
    int before = wakes;

    CHECK(put_event(a, 0, RFKILL_TYPE_WLAN, RFKILL_OP_CHANGE_ALL, 1, 8) == 8);
    CHECK(calls[0] == 1 && last_block[0] == 1 && calls[1] == 0 && wakes > before);
    expect(a, 0, RFKILL_TYPE_WLAN, RFKILL_OP_CHANGE, 1, 0);
    expect_none(a);
    CHECK(shows(0, RFKILL_ATTR_SOFT, "1\n") && shows(0, RFKILL_ATTR_STATE, "0\n"));
    CHECK(rfkill_soft_blocked(wifi) && !rfkill_soft_blocked(bt));
    /* An unchanged block calls nothing and sends nothing. */
    CHECK(put_event(a, 0, RFKILL_TYPE_WLAN, RFKILL_OP_CHANGE_ALL, 1, 8) == 8 && calls[0] == 1);
    expect_none(a);
    /* By index, of any type, or of the wrong one, which changes nothing. */
    CHECK(put_event(a, 1, RFKILL_TYPE_WLAN, RFKILL_OP_CHANGE, 1, 8) == 8 && calls[1] == 0);
    CHECK(put_event(a, 1, RFKILL_TYPE_ALL, RFKILL_OP_CHANGE, 1, 8) == 8 && calls[1] == 1);
    expect(a, 1, RFKILL_TYPE_BLUETOOTH, RFKILL_OP_CHANGE, 1, 0);
    CHECK(put_event(a, 7, RFKILL_TYPE_ALL, RFKILL_OP_CHANGE, 0, 8) == 8);
    expect_none(a);
    /* Seven bytes are enough; a ninth is not taken at the first size. */
    CHECK(put_event(a, 1, RFKILL_TYPE_BLUETOOTH, RFKILL_OP_CHANGE, 0, 7) == 7 && calls[1] == 2);
    expect(a, 1, RFKILL_TYPE_BLUETOOTH, RFKILL_OP_CHANGE, 0, 0);
    CHECK(put_event(a, 1, RFKILL_TYPE_BLUETOOTH, RFKILL_OP_CHANGE, 1, 9) == 8 && calls[1] == 3);
    expect(a, 1, RFKILL_TYPE_BLUETOOTH, RFKILL_OP_CHANGE, 1, 0);
    CHECK(put_event(a, 1, RFKILL_TYPE_BLUETOOTH, RFKILL_OP_CHANGE, 0, 6) == -EINVAL);
    CHECK(put_event(a, 1, RFKILL_NUM_TYPES, RFKILL_OP_CHANGE, 0, 8) == -EINVAL);
    CHECK(put_event(a, 1, RFKILL_TYPE_ALL, RFKILL_OP_ADD, 0, 8) == -EINVAL);
    CHECK(put_event(a, 1, RFKILL_TYPE_ALL, RFKILL_OP_DEL, 0, 8) == -EINVAL);
    CHECK(put_event(a, 1, RFKILL_TYPE_ALL, 7, 0, 8) == -EINVAL);
    CHECK(rfkill_write(&a->e, (const char __user *)1, 8) == -EFAULT);
    CHECK(calls[1] == 3);
    expect_none(a);
    /* A refusal leaves the block and sends nothing; the write succeeds. */
    refuse[1] = -EIO;
    CHECK(put_event(a, 0, RFKILL_TYPE_ALL, RFKILL_OP_CHANGE_ALL, 0, 8) == 8);
    CHECK(calls[0] == 2 && last_block[0] == 0 && calls[1] == 4 && last_block[1] == 0);
    CHECK(!rfkill_soft_blocked(wifi) && rfkill_soft_blocked(bt));
    expect(a, 0, RFKILL_TYPE_WLAN, RFKILL_OP_CHANGE, 0, 0);
    expect_none(a);
    refuse[1] = 0;
    CHECK(put_event(a, 1, RFKILL_TYPE_BLUETOOTH, RFKILL_OP_CHANGE, 0, 8) == 8);
    expect(a, 1, RFKILL_TYPE_BLUETOOTH, RFKILL_OP_CHANGE, 0, 0);
}

/** The hard block, its attributes and the extended event. */
static void hard(struct reader *a)
{
    uint8_t ev[RFKILL_EVENT_SIZE_EXT];
    uint32_t size;

    CHECK(rfkill_set_hw_state(wifi, 1) == 1 && calls[0] == 2);
    CHECK(rfkill_set_hw_state(wifi, 1) == 1);
    expect(a, 0, RFKILL_TYPE_WLAN, RFKILL_OP_CHANGE, 0, 1);
    expect_none(a);
    CHECK(shows(0, RFKILL_ATTR_HARD, "1\n") && shows(0, RFKILL_ATTR_STATE, "2\n"));
    CHECK(shows(0, RFKILL_ATTR_REASONS, "0x1\n"));
    CHECK(shows(0, RFKILL_ATTR_UEVENT,
                "RFKILL_NAME=phy0\nRFKILL_TYPE=wlan\nRFKILL_STATE=2\n"
                "RFKILL_HW_BLOCK_REASON=0x1\n"));
    CHECK(rfkill_set_hw_state(0, 1) == 0);
    /* The requests, then the ninth byte. */
    CHECK(rfkill_ioctl(&a->e, RFKILL_IOCTL_NOINPUT, 0) == 0);
    CHECK(rfkill_ioctl(&a->e, 0x5203u, 0) == -ENOTTY);
    CHECK(rfkill_ioctl(&a->e, RFKILL_IOCTL_MAX_SIZE, 1) == -EFAULT);
    size = 7;
    CHECK(rfkill_ioctl(&a->e, RFKILL_IOCTL_MAX_SIZE, (uintptr_t)&size) == -EINVAL);
    size = 256;
    CHECK(rfkill_ioctl(&a->e, RFKILL_IOCTL_MAX_SIZE, (uintptr_t)&size) == -EINVAL);
    size = 255;
    CHECK(rfkill_ioctl(&a->e, RFKILL_IOCTL_MAX_SIZE, (uintptr_t)&size) == 0);
    CHECK(rfkill_set_hw_state(wifi, 0) == 0);
    CHECK(rfkill_set_hw_state(wifi, 1) == 1);
    k_memset(ev, 0, sizeof(ev));
    CHECK(rfkill_read(&a->e, (char __user *)ev, sizeof(ev)) == 9 && ev[7] == 0 && ev[8] == 0);
    CHECK(rfkill_read(&a->e, (char __user *)ev, 64) == 9);
    CHECK(ev[0] == 0 && ev[5] == RFKILL_OP_CHANGE && ev[7] == 1);
    CHECK(ev[8] == RFKILL_HARD_BLOCK_SIGNAL);
    /* A short read takes the event whole; a fault loses it, as on Linux. */
    CHECK(rfkill_set_hw_state(wifi, 0) == 0);
    CHECK(rfkill_set_hw_state(wifi, 1) == 1);
    CHECK(rfkill_read(&a->e, (char __user *)ev, 4) == 4);
    CHECK(rfkill_read(&a->e, (char __user *)1, 8) == -EFAULT);
    expect_none(a);
    size = 8;
    CHECK(rfkill_ioctl(&a->e, RFKILL_IOCTL_MAX_SIZE, (uintptr_t)&size) == 0);
    CHECK(rfkill_set_hw_state(wifi, 0) == 0);
    expect(a, 0, RFKILL_TYPE_WLAN, RFKILL_OP_CHANGE, 0, 0);
}

/** A change made while a blocking reader waits. */
static void block_wifi(void)
{
    (void)rfkill_set_hw_state(wifi, 1);
}

/** A blocking read, the full queue, and a later switch. */
static void waiting(struct reader *a)
{
    struct reader b;
    struct rfkill *late = 0;
    uint8_t ev[8];
    int i;

    reader_open(&b, 1);
    expect(&b, 0, RFKILL_TYPE_WLAN, RFKILL_OP_ADD, 0, 0);
    expect(&b, 1, RFKILL_TYPE_BLUETOOTH, RFKILL_OP_ADD, 0, 0);
    CHECK(rfkill_read(&b.e, (char __user *)ev, sizeof(ev)) == -EINTR);
    on_wait = block_wifi;
    expect(&b, 0, RFKILL_TYPE_WLAN, RFKILL_OP_CHANGE, 0, 1);
    on_wait = 0;
    expect(a, 0, RFKILL_TYPE_WLAN, RFKILL_OP_CHANGE, 0, 1);
    rfkill_close(&b.e);
    CHECK(!b.e.priv && clients == (struct rfkill_client *)a->e.priv && !clients->next);
    /* Forty changes keep the last RFKILL_QUEUE. */
    for (i = 0; i < 40; ++i)
        (void)rfkill_set_hw_state(wifi, i & 1);
    for (i = 40 - RFKILL_QUEUE; i < 40; ++i)
        expect(a, 0, RFKILL_TYPE_WLAN, RFKILL_OP_CHANGE, 0, (unsigned)(i & 1));
    expect_none(a);
    /* A switch registered after a block of its type starts blocked. */
    CHECK(put_event(a, 0, RFKILL_TYPE_WLAN, RFKILL_OP_CHANGE_ALL, 1, 8) == 8 && calls[0] == 3);
    expect(a, 0, RFKILL_TYPE_WLAN, RFKILL_OP_CHANGE, 1, 1);
    CHECK(rfkill_register("phy1", RFKILL_TYPE_WLAN, 1, &owner_ops, &owner_ids[0], &late) == 0);
    CHECK(late && rfkill_soft_blocked(late) && rfkill_index(late) == 2 && calls[0] == 3);
    expect(a, 2, RFKILL_TYPE_WLAN, RFKILL_OP_ADD, 1, 0);
    rfkill_unregister(late);
    rfkill_unregister(late);
    expect(a, 2, RFKILL_TYPE_WLAN, RFKILL_OP_DEL, 1, 0);
    expect_none(a);
    CHECK(!rfkill_present(2) && rfkill_nth(2) == -1 && rfkill_lookup("rfkill2", 7) == -1);
    CHECK(rfkill_register("phy1", RFKILL_TYPE_WLAN, 1, &owner_ops, &owner_ids[0], &late) == 0);
    CHECK(late && rfkill_index(late) == 3 && rfkill_nth(2) == 3);
    expect(a, 3, RFKILL_TYPE_WLAN, RFKILL_OP_ADD, 1, 0);
    rfkill_unregister(late);
    expect(a, 3, RFKILL_TYPE_WLAN, RFKILL_OP_DEL, 1, 0);
}

/** The attributes' writes, and the table's limit. */
static void storing(struct reader *a)
{
    struct rfkill *more[RFKILL_MAX];
    struct rfkill *extra = 0;
    int i, n = 0, calls0 = calls[0];

    CHECK(rfkill_store(0, RFKILL_ATTR_SOFT, "0", 1) == 0 && calls[0] == calls0 + 1);
    CHECK(!rfkill_soft_blocked(wifi) && shows(0, RFKILL_ATTR_STATE, "2\n"));
    expect(a, 0, RFKILL_TYPE_WLAN, RFKILL_OP_CHANGE, 0, 1);
    CHECK(rfkill_store(0, RFKILL_ATTR_STATE, "0", 1) == 0 && rfkill_soft_blocked(wifi));
    expect(a, 0, RFKILL_TYPE_WLAN, RFKILL_OP_CHANGE, 1, 1);
    CHECK(rfkill_store(0, RFKILL_ATTR_STATE, "1", 1) == 0 && !rfkill_soft_blocked(wifi));
    expect(a, 0, RFKILL_TYPE_WLAN, RFKILL_OP_CHANGE, 0, 1);
    CHECK(rfkill_store(0, RFKILL_ATTR_STATE, "2", 1) == -EINVAL);
    CHECK(rfkill_store(0, RFKILL_ATTR_SOFT, "10", 2) == -EINVAL);
    CHECK(rfkill_store(0, RFKILL_ATTR_SOFT, "", 0) == -EINVAL);
    CHECK(rfkill_store(0, RFKILL_ATTR_HARD, "0", 1) == -EINVAL);
    CHECK(rfkill_store(9, RFKILL_ATTR_SOFT, "1", 1) == -ENOENT);
    refuse[0] = -EBUSY;
    CHECK(rfkill_store(0, RFKILL_ATTR_SOFT, "1", 1) == -EBUSY && !rfkill_soft_blocked(wifi));
    refuse[0] = 0;
    expect_none(a);
    for (i = 0; i < RFKILL_MAX; ++i) {
        more[i] = 0;
        if (rfkill_register("gps", RFKILL_TYPE_GPS, -1, &owner_ops, &owner_ids[1], &more[i]) == 0)
            n++;
    }
    CHECK(n == RFKILL_MAX - 2);
    CHECK(rfkill_register("gps", RFKILL_TYPE_GPS, -1, &owner_ops, &owner_ids[1], &extra) ==
              -ENOSPC &&
          !extra);
    for (i = 0; i < RFKILL_MAX; ++i)
        rfkill_unregister(more[i]);
    CHECK(rfkill_nth(2) == -1);
}

int main(void)
{
    struct reader a;

    registration();
    opening(&a);
    writing(&a);
    hard(&a);
    waiting(&a);
    storing(&a);
    rfkill_close(&a.e);
    CHECK(!clients);
    rfkill_unregister(wifi);
    rfkill_unregister(bt);
    CHECK(rfkill_nth(0) == -1);
    return failures ? 1 : 0;
}
