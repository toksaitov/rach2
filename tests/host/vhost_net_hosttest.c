/**
 * Test /dev/vhost-net of sys/network/driver/vhost_net.c: Linux's vhost
 * ioctls and their refusals, the memory table and its pins, the receive
 * ring filled in the sender's context and the transmit ring taken by the
 * thread's pass, a page the pin does not hold brought in by the thread
 * while the frame waits, and the descriptors the device holds, with the
 * tap, the eventfds, the pins and the guest's memory stood in for.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/kirq_none.c tests/host/compat/atomic_host.c
 *
 * The guest's memory is a buffer of the test's, whose host address is the
 * address QEMU would give for it and whose guest-physical address starts at
 * GPA. A pin's physical address of a page is the page's host address, and
 * the kernel's mapping of it is the address itself; a pin holds the pages
 * whose bits `held` has, every page when it is made, and a row clears a
 * bit to stand for a page a change took. A ring is laid out in
 * that buffer as a guest driver lays it out, and the rows read the used
 * ring, the frames and the eventfds' counts back.
 *
 * The test exits with the number of failed checks, at most 255.
 */

#include "lib/kint.h"
#include "lib/kerrno.h"
#include "fs/fd.h"
#include "fs/file.h"
#include "fs/devfs.h"
#include "proc/percpu.h"
#include "network/driver/vhost_net.c"

extern int printf(const char *fmt, ...);

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

/* ------------------------------------------------------------------ */
/* The kernel around the device.                                       */
/* ------------------------------------------------------------------ */

#define GPA     0x10000000ull
#define MEMSIZE (16u * 4096u)
#define NEVS    8
#define EV0     100 /* the descriptor of the first eventfd */
#define TAPFD   3   /* a tap with an interface */
#define TAPFD2  4   /* another tap */
#define PLAINFD 5   /* a descriptor that is no tap */

/* The guest's memory. */
static uint8_t mem[MEMSIZE] __attribute__((aligned(4096)));

/* An eventfd: references, the counter, the hooks, and what was done. */
struct eventfd {
    int refs, signals, closed;
    uint64_t count;
    struct eventfd_hook *hooks;
};

/* A pin of the test's: the range it covers, whether it is live, and the
 * pages it holds, a bit each. */
struct pin {
    uintptr_t base;
    uint32_t pages, held;
    int live;
    const struct pin_holder *holder;
};

/* The tap as the device sees it: the backend it has. */
struct tap {
    struct tap_backend *b;
};

/* What the device did to the kernel around it, which the rows read. */
static struct {
    struct devfs_node *node;
    struct eventfd ev[NEVS];
    struct tap tap[2];
    struct pin *pins[16];
    int pins_made, pins_freed, pin_error, wakes, threads;
    int refills, refill_error; /* pin_refill's calls, and its answer */
    int batches, batch_depth;  /* TCP's batches begun, and the depth now */
    int released;              /* descriptors the device let go */
    int injected, inject_len;  /* frames given to the stack, the last's length */
    uint8_t inject[2048];      /* the last frame given to the stack */
    uint8_t queue[4][64];      /* frames waiting in the tap, 60 bytes each */
    int queued;
} k;

struct percpu cpu_table[NR_CPUS];
static process_t self;
static int mm_a, mm_b;
static fd_entry_t tapfd[2], plainfd, evfd[NEVS];
static struct open_file tapofd[2], evofd[NEVS];

/** Keep the node, so that a row opens it. */
int32_t devfs_node_register(struct devfs_node *node)
{
    k.node = node;
    return 0;
}

/** The entries of the test's descriptors. */
fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    (void)table;
    if (fd == TAPFD || fd == TAPFD2) {
        return &tapfd[fd - TAPFD];
    }
    if (fd >= EV0 && fd < EV0 + NEVS) {
        return &evfd[fd - EV0];
    }
    return fd == PLAINFD ? &plainfd : 0;
}

/** A reference to a description, which the device takes to hold one. */
void fd_ofd_ref(struct open_file *ofd)
{
    ofd->refcount++;
}

/** Let go of a held copy of a descriptor: its description's reference. */
void fd_release_backing(fd_entry_t *e)
{
    e->ofd->refcount--;
    k.released++;
}

/** The name of a tap. */
const char *tap_name(const struct tap *t)
{
    (void)t;
    return "tap0";
}

/** Count a batch begun. */
void tcp_batch_begin(void)
{
    k.batches++;
    k.batch_depth++;
}

/** End a batch. */
void tcp_batch_end(void)
{
    k.batch_depth--;
}

/** The tap of a tap's entry. */
struct tap *tap_of(const struct fd_entry *e)
{
    if (e == &tapfd[0] || e == &tapfd[1]) {
        return &k.tap[e - tapfd];
    }
    return 0;
}

/** Give the tap its backend, as tap.c does. */
int tap_backend_set(struct tap *t, struct tap_backend *b)
{
    if (b && t->b && t->b != b) {
        return -EBUSY;
    }
    t->b = b;
    return 0;
}

/** Offer the frames waiting in the tap to its backend, in order. */
void tap_backend_drain(struct tap *t)
{
    while (t->b && k.queued > 0 && t->b->rx(t->b, k.queue[0], 60)) {
        k_memmove(k.queue[0], k.queue[1], sizeof(k.queue) - sizeof(k.queue[0]));
        k.queued--;
    }
}

/** Keep the frame the stack is given. */
int32_t tap_inject(struct tap *t, const uint8_t *frame, uint32_t len)
{
    (void)t;
    k.injected++;
    k.inject_len = (int)len;
    k_memcpy(k.inject, frame, len < sizeof(k.inject) ? len : sizeof(k.inject));
    return (int32_t)len;
}

/** A reference to the eventfd of `fd`, as eventfd_ctx_fdget answers. */
struct eventfd *eventfd_get(int fd, int32_t *err)
{
    if (fd >= EV0 && fd < EV0 + NEVS) {
        k.ev[fd - EV0].refs++;
        return &k.ev[fd - EV0];
    }
    *err = fd == PLAINFD ? -EINVAL : -EBADF;
    return 0;
}

/** Drop a reference. */
void eventfd_put(struct eventfd *ev)
{
    ev->refs--;
}

/** Add 1 and tell the hooks, which may take the counter. */
void eventfd_signal(struct eventfd *ev)
{
    struct eventfd_hook *h;

    ev->signals++;
    ev->count++;
    for (h = ev->hooks; h; h = h->next) {
        if (h->signal(h)) {
            ev->count = 0;
        }
    }
}

/** Put `h` on the list, or run its hangup after the last close. */
int eventfd_hook_add(struct eventfd *ev, struct eventfd_hook *h)
{
    if (ev->closed) {
        h->hangup(h);
        return -1;
    }
    h->next = ev->hooks;
    ev->hooks = h;
    return ev->count > 0;
}

/** Take `h` off the list: 1 when it was on it. */
int eventfd_hook_remove(struct eventfd *ev, struct eventfd_hook *h, uint64_t *cnt)
{
    struct eventfd_hook **pp;

    (void)cnt;
    for (pp = &ev->hooks; *pp; pp = &(*pp)->next) {
        if (*pp == h) {
            *pp = h->next;
            return 1;
        }
    }
    return 0;
}

/** Pin [addr, addr + len), a follower holding every page: its pages are
 * the test's own memory. */
int pin_user(struct mm *mm, uintptr_t addr, uintptr_t len, uint32_t flags,
             const struct pin_holder *holder, struct pin **out)
{
    struct pin *p;

    (void)mm;
    if (k.pin_error) {
        return k.pin_error;
    }
    if (flags != (PIN_WRITE | PIN_FOLLOW) || !holder->forget) {
        return -EINVAL;
    }
    if (addr < (uintptr_t)mem || addr + len > (uintptr_t)mem + MEMSIZE) {
        return -EFAULT;
    }
    p = (struct pin *)kzalloc(sizeof(*p));
    p->base = addr;
    p->pages = (uint32_t)((len + 4095u) / 4096u);
    p->held = 0xFFFFFFFFu;
    p->live = 1;
    p->holder = holder;
    k.pins[k.pins_made++ % 16] = p;
    *out = p;
    return 0;
}

/** Free a pin, and forget it. */
void unpin(struct pin *pin)
{
    int i;

    if (pin) {
        for (i = 0; i < 16; i++) {
            if (k.pins[i] == pin) {
                k.pins[i] = 0;
            }
        }
        k.pins_freed++;
        kfree(pin);
    }
}

/** Whether the pin still holds its pages. */
int pin_live(const struct pin *pin)
{
    return pin->live;
}

/** The host address of page `i`, or 0 past the end, once taken back, or
 * for a page not held. */
uintptr_t pin_phys(const struct pin *pin, uint32_t i)
{
    return pin->live && i < pin->pages && (pin->held >> i & 1u) ? pin->base + 4096u * i : 0;
}

/** Hold page `i` again, or answer `refill_error`. */
int pin_refill(struct pin *pin, uint32_t i)
{
    k.refills++;
    if (k.refill_error) {
        return k.refill_error;
    }
    if (!pin->live || i >= pin->pages) {
        return -EFAULT;
    }
    pin->held |= 1u << i;
    return 0;
}

/** Take page `off` of the guest's memory from every live pin, as a change
 * of the page would, after the holder's forget. */
static void drop_page(uint32_t off)
{
    int i;

    for (i = 0; i < 16; i++) {
        struct pin *p = k.pins[i];

        if (p && p->live && p->base == (uintptr_t)mem) {
            p->held &= ~(1u << (off / 4096u));
            p->holder->forget(p, p->holder->owner);
        }
    }
}

/** The kernel's mapping of a page is its host address. */
void *kmap_cpu(uintptr_t phys, uint32_t *flags)
{
    *flags = 0;
    return (void *)phys;
}

/** Nothing to unmap. */
void kunmap_cpu(void *va, uint32_t flags)
{
    (void)va;
    (void)flags;
}

/** Copy from a test address. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if ((uintptr_t)src < 4096) {
        return -EFAULT;
    }
    k_memcpy(dst, (const void *)src, n);
    return 0;
}

/** Copy to a test address. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if ((uintptr_t)dst < 4096) {
        return -EFAULT;
    }
    k_memcpy((void *)dst, src, n);
    return 0;
}

/** Log nothing. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Count a wake of the thread. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
    k.wakes++;
}

/** No one waits for a refill: the rows make none that sleeps. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** Not reached: the rows run the thread's passes themselves. */
int wq_wait_event_idle(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    return 0;
}

/** Not reached. */
void sched_service_point(void)
{}

/** Not reached. */
void arch_irq_enable(void)
{}

/** A stack for the thread. */
uintptr_t kstack_alloc(void)
{
    static uint8_t stack[KERNEL_STACK_SIZE];

    return (uintptr_t)stack;
}

/** Not reached: the stack is given. */
void kstack_free(uintptr_t top)
{
    (void)top;
}

/** The thread's record. */
process_t *alloc_process(void)
{
    static process_t thread;

    return &thread;
}

/** Nothing to name. */
void proc_set_comm(process_t *p, const char *name)
{
    (void)p;
    (void)name;
}

/** Nothing to set up. */
void setup_process_stack(process_t *p, void (*entry)(void))
{
    (void)p;
    (void)entry;
}

/** Count the thread made ready. */
void sched_make_ready(process_t *p)
{
    (void)p;
    k.threads++;
}

/* ------------------------------------------------------------------ */
/* The guest's rings.                                                  */
/* ------------------------------------------------------------------ */

/* Where each ring's parts are in the buffer, and the buffers' area. */
#define RX_DESC  0x0000u
#define RX_AVAIL 0x0200u
#define RX_USED  0x0400u
#define TX_DESC  0x1000u
#define TX_AVAIL 0x2000u
#define TX_USED  0x3000u
#define BUFS     0x4000u
#define RXNUM    8u
#define TXNUM    256u

/** The 16-bit word at offset `off` of the guest's memory. */
static uint16_t get16(uint32_t off)
{
    uint16_t v;

    k_memcpy(&v, mem + off, 2);
    return v;
}

/** Store the 16-bit word `v` at offset `off`. */
static void put16(uint32_t off, uint16_t v)
{
    k_memcpy(mem + off, &v, 2);
}

/** The 32-bit word at offset `off`. */
static uint32_t get32(uint32_t off)
{
    uint32_t v;

    k_memcpy(&v, mem + off, 4);
    return v;
}

/** Write descriptor `i` of the table at `table`. */
static void desc(uint32_t table, uint32_t i, uint32_t off, uint32_t len, uint16_t flags,
                 uint16_t next)
{
    uint64_t addr = GPA + off;

    k_memcpy(mem + table + 16u * i, &addr, 8);
    k_memcpy(mem + table + 16u * i + 8u, &len, 4);
    k_memcpy(mem + table + 16u * i + 12u, &flags, 2);
    k_memcpy(mem + table + 16u * i + 14u, &next, 2);
}

/** Offer the chain `head` on the ring whose available ring is at `avail`
 * and has `num` entries. */
static void offer(uint32_t avail, uint32_t num, uint16_t head)
{
    uint16_t idx = get16(avail + 2u);

    put16(avail + 4u + 2u * (idx % num), head);
    put16(avail + 2u, (uint16_t)(idx + 1u));
}

/** Whether the `n` bytes at `a` and `b` are the same. */
static int same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    while (n != 0 && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n == 0;
}

/** An ioctl of the device's descriptor `e`. */
static int32_t ioc(fd_entry_t *e, uint32_t req, void *arg)
{
    return vn_ops.ioctl(e, req, (uintptr_t)arg);
}

/** A ring request of `index` and `num`, VHOST_SET_VRING_NUM's and the
 * rest's. */
static int32_t ring2(fd_entry_t *e, uint32_t req, uint32_t index, uint32_t num)
{
    uint32_t st[2];

    st[0] = index;
    st[1] = num;
    return ioc(e, req, st);
}

/** VHOST_SET_VRING_ADDR of ring `index` at the offsets given, with `flags`. */
static int32_t ring_addr(fd_entry_t *e, uint32_t index, uint32_t flags, uint32_t d, uint32_t u,
                         uint32_t a)
{
    uint64_t addr[5];
    uint32_t st[2];

    st[0] = index;
    st[1] = flags;
    k_memcpy(&addr[0], st, 8);
    addr[1] = (uintptr_t)mem + d;
    addr[2] = (uintptr_t)mem + u;
    addr[3] = (uintptr_t)mem + a;
    addr[4] = 0;
    return ioc(e, VHOST_SET_VRING_ADDR, addr);
}

/** VHOST_SET_MEM_TABLE with `count` regions, each the whole buffer at GPA
 * shifted by its index's page, the first `count` of which may be one
 * region past the buffer. */
static int32_t mem_table(fd_entry_t *e, uint32_t count, int outside)
{
    static uint64_t table[1 + 4 * 70];
    uint32_t i;

    k_memset(table, 0, sizeof(table));
    table[0] = count;
    for (i = 0; i < count && i < 70; i++) {
        table[1 + 4 * i] = GPA + (uint64_t)MEMSIZE * i;
        table[2 + 4 * i] = MEMSIZE;
        table[3 + 4 * i] = (uintptr_t)mem + (outside && i ? (uint64_t)MEMSIZE * i : 0);
    }
    return ioc(e, VHOST_SET_MEM_TABLE, table);
}

/** Open the node into `e`, set its owner, the features of the 1.0 layout,
 * the memory and both rings with their eventfds: kick, call and error at
 * EV0 + 0, 1 and 2 for the receive ring and 3, 4 and 5 for the transmit
 * ring. Then give both rings the tap of TAPFD. */
static void ready(fd_entry_t *e)
{
    uint64_t f = VHOST_NET_F_VIRTIO_NET_HDR | VHOST_F_VERSION_1;

    k_memset(mem, 0, sizeof(mem));
    k_memset(e, 0, sizeof(*e));
    CHECK(k.node->open(k.node, e, 0) == 0, "open");
    CHECK(ioc(e, VHOST_SET_OWNER, 0) == 0, "VHOST_SET_OWNER");
    CHECK(ioc(e, VHOST_SET_FEATURES, &f) == 0, "VHOST_SET_FEATURES");
    CHECK(mem_table(e, 1, 0) == 0, "VHOST_SET_MEM_TABLE");
    CHECK(ring2(e, VHOST_SET_VRING_NUM, 0, RXNUM) == 0, "the receive ring's size");
    CHECK(ring2(e, VHOST_SET_VRING_NUM, 1, TXNUM) == 0, "the transmit ring's size");
    CHECK(ring2(e, VHOST_SET_VRING_BASE, 0, 0) == 0, "the receive ring's base");
    CHECK(ring2(e, VHOST_SET_VRING_BASE, 1, 0) == 0, "the transmit ring's base");
    CHECK(ring_addr(e, 0, 0, RX_DESC, RX_USED, RX_AVAIL) == 0, "the receive ring's address");
    CHECK(ring_addr(e, 1, 0, TX_DESC, TX_USED, TX_AVAIL) == 0, "the transmit ring's address");
    CHECK(ring2(e, VHOST_SET_VRING_KICK, 0, EV0) == 0, "the receive ring's kick");
    CHECK(ring2(e, VHOST_SET_VRING_CALL, 0, EV0 + 1) == 0, "the receive ring's call");
    CHECK(ring2(e, VHOST_SET_VRING_ERR, 0, EV0 + 2) == 0, "the receive ring's error");
    CHECK(ring2(e, VHOST_SET_VRING_KICK, 1, EV0 + 3) == 0, "the transmit ring's kick");
    CHECK(ring2(e, VHOST_SET_VRING_CALL, 1, EV0 + 4) == 0, "the transmit ring's call");
    CHECK(ring2(e, VHOST_SET_VRING_ERR, 1, EV0 + 5) == 0, "the transmit ring's error");
    CHECK(ring2(e, VHOST_NET_SET_BACKEND, 0, TAPFD) == 0, "the receive ring's backend");
    CHECK(ring2(e, VHOST_NET_SET_BACKEND, 1, TAPFD) == 0, "the transmit ring's backend");
    vn_pass();
}

/** A frame of 60 bytes whose bytes count up from `seed`. */
static void frame(uint8_t *f, uint8_t seed)
{
    uint32_t i;

    for (i = 0; i < 60; i++) {
        f[i] = (uint8_t)(seed + i);
    }
}

/* ------------------------------------------------------------------ */
/* The rows.                                                           */
/* ------------------------------------------------------------------ */

/** The requests before and around VHOST_SET_OWNER, the features and the
 * refusals of the ring requests, as Linux answers each. */
static void test_requests(void)
{
    fd_entry_t e;
    uint64_t v = 0;
    uint32_t st[2];

    printf("requests\n");
    k_memset(&e, 0, sizeof(e));
    CHECK(k.node->open(k.node, &e, 0) == 0, "open");
    CHECK(ioc(&e, VHOST_GET_FEATURES, &v) == 0 && v == VHOST_NET_FEATURES,
          "VHOST_GET_FEATURES before an owner");
    v = VHOST_NET_F_VIRTIO_NET_HDR;
    CHECK(ioc(&e, VHOST_SET_FEATURES, &v) == 0, "VHOST_SET_FEATURES before an owner");
    CHECK(((struct vnet *)e.priv)->hdr == 10, "a legacy header of 10 bytes");
    v = VHOST_F_MRG_RXBUF;
    CHECK(ioc(&e, VHOST_SET_FEATURES, &v) == -EOPNOTSUPP, "a feature not offered");
    v = 0;
    CHECK(ioc(&e, VHOST_SET_FEATURES, &v) == 0 && ((struct vnet *)e.priv)->hdr == 0,
          "no header without VHOST_NET_F_VIRTIO_NET_HDR");
    CHECK(ioc(&e, VHOST_GET_BACKEND_FEATURES, &v) == 0 && v == 0, "no backend features");
    v = 1;
    CHECK(ioc(&e, VHOST_SET_BACKEND_FEATURES, &v) == -EOPNOTSUPP, "a backend feature");
    CHECK(mem_table(&e, 1, 0) == -EPERM, "VHOST_SET_MEM_TABLE before an owner");
    CHECK(ioc(&e, VHOST_SET_OWNER, 0) == 0, "VHOST_SET_OWNER");
    CHECK(ioc(&e, VHOST_SET_OWNER, 0) == -EBUSY, "a second VHOST_SET_OWNER");
    self.mm = (struct mm *)&mm_b;
    CHECK(ring2(&e, VHOST_SET_VRING_NUM, 0, 8) == -EPERM, "a caller not the owner");
    self.mm = (struct mm *)&mm_a;
    CHECK(ring2(&e, VHOST_SET_VRING_NUM, 2, 8) == -ENOBUFS, "a third ring");
    CHECK(ring2(&e, VHOST_SET_VRING_NUM, 0, 0) == -EINVAL, "a ring of 0");
    CHECK(ring2(&e, VHOST_SET_VRING_NUM, 0, 12) == -EINVAL, "a ring of 12");
    CHECK(ring2(&e, VHOST_SET_VRING_NUM, 0, 65536) == -EINVAL, "a ring of 65536");
    CHECK(ring2(&e, VHOST_SET_VRING_NUM, 0, 32768) == 0, "a ring of 32768");
    CHECK(ring2(&e, VHOST_SET_VRING_BASE, 0, 0x10000) == -EINVAL, "a base past 0xffff");
    CHECK(ring2(&e, VHOST_SET_VRING_BASE, 0, 7) == 0, "a base of 7");
    st[0] = 0;
    st[1] = 0;
    CHECK(ioc(&e, VHOST_GET_VRING_BASE, st) == 0 && st[1] == 7, "VHOST_GET_VRING_BASE");
    CHECK(ring_addr(&e, 0, 2, RX_DESC, RX_USED, RX_AVAIL) == -EOPNOTSUPP, "an address flag");
    CHECK(ring_addr(&e, 0, 1, RX_DESC + 8, RX_USED, RX_AVAIL) == -EINVAL,
          "a table not on 16 bytes");
    CHECK(ring_addr(&e, 0, 0, RX_DESC, RX_USED + 2, RX_AVAIL) == -EINVAL,
          "a used ring not on 4 bytes");
    CHECK(ring_addr(&e, 0, 0, RX_DESC, RX_USED, RX_AVAIL + 1) == -EINVAL,
          "an available ring not on 2 bytes");
    CHECK(ring_addr(&e, 2, 0, RX_DESC, RX_USED, RX_AVAIL) == -ENOBUFS, "a third ring's address");
    CHECK(ring2(&e, VHOST_SET_VRING_KICK, 0, 99) == -EBADF, "a kick of no descriptor");
    CHECK(ring2(&e, VHOST_SET_VRING_CALL, 0, PLAINFD) == -EINVAL, "a call of no eventfd");
    CHECK(ring2(&e, VHOST_NET_SET_BACKEND, 0, PLAINFD) == -ENOTSOCK, "a backend of no tap");
    CHECK(ring2(&e, VHOST_NET_SET_BACKEND, 0, TAPFD) == -EFAULT,
          "a backend for a ring with no memory");
    CHECK(ring2(&e, VHOST_SET_VRING_BUSYLOOP_TIMEOUT, 1, 50) == 0, "a busy-poll timeout");
    st[0] = 1;
    st[1] = 0;
    CHECK(ioc(&e, VHOST_GET_VRING_BUSYLOOP_TIMEOUT, st) == 0 && st[0] == 1 && st[1] == 50,
          "the busy-poll timeout given back");
    CHECK(ring2(&e, VHOST_SET_VRING_BUSYLOOP_TIMEOUT, 2, 50) == -ENOBUFS,
          "a third ring's busy-poll timeout");
    CHECK(ioc(&e, 0xaf99u, 0) == -ENOTTY, "an unknown request");
    vn_ops.close(&e);
    CHECK(vn_devs == 0, "the close takes the device off the list");
}

/** The memory table: its bound, a region's pin kept by a table that has it
 * whole, a region the pin refuses, an empty one, a pin without memory, and
 * the reset that lets every pin go. */
static void test_memory(void)
{
    fd_entry_t e;
    int made, freed;

    printf("memory\n");
    k_memset(&e, 0, sizeof(e));
    CHECK(k.node->open(k.node, &e, 0) == 0, "open");
    CHECK(ioc(&e, VHOST_SET_OWNER, 0) == 0, "VHOST_SET_OWNER");
    CHECK(mem_table(&e, VHOST_NET_REGIONS + 1, 0) == -E2BIG, "65 regions");
    made = k.pins_made;
    freed = k.pins_freed;
    CHECK(mem_table(&e, 1, 0) == 0 && k.pins_made == made + 1, "one region pinned");
    CHECK(mem_table(&e, 1, 0) == 0 && k.pins_made == made + 1 && k.pins_freed == freed,
          "the same region keeps its pin");
    CHECK(mem_table(&e, 2, 1) == -EFAULT && k.pins_made == made + 1 && k.pins_freed == freed &&
              ((struct vnet *)e.priv)->nreg == 1,
          "a region the pin refuses fails the table, which keeps the old one");
    CHECK(mem_table(&e, 2, 0) == 0 && k.pins_made == made + 2 && ((struct vnet *)e.priv)->nreg == 2,
          "a second region of the same memory");
    k.pin_error = -ENOMEM;
    CHECK(mem_table(&e, 3, 0) == -ENOMEM, "a pin without memory");
    CHECK(((struct vnet *)e.priv)->nreg == 2 && k.pins_made == made + 2,
          "the refused table leaves the old one");
    k.pin_error = 0;
    CHECK(ioc(&e, VHOST_RESET_OWNER, 0) == 0 && k.pins_freed == freed + 2,
          "the reset lets the pins go");
    CHECK(ioc(&e, VHOST_SET_OWNER, 0) == 0, "an owner again after the reset");
    vn_ops.close(&e);
}

/** The receive ring: a frame written after the 1.0 header into a chain of
 * two buffers, its interrupt, one asked against, a buffer too short, an
 * empty ring and the kicks it asks for, the frames the tap kept drained on
 * a kick, and a bad descriptor. */
static void test_receive(void)
{
    fd_entry_t e;
    struct vnet *n;
    uint8_t f[60];
    int calls;

    printf("receive\n");
    ready(&e);
    n = (struct vnet *)e.priv;
    CHECK(k.tap[0].b == &n->tb, "the tap has the backend");
    frame(f, 1);
    CHECK(n->tb.rx(&n->tb, f, 60) == 0, "no buffer: the frame waits in the tap");
    CHECK(get16(RX_USED) == 0, "an empty ring asks for kicks");
    desc(RX_DESC, 0, BUFS, 12, DESC_WRITE | DESC_NEXT, 1);
    desc(RX_DESC, 1, BUFS + 0x100, 1500, DESC_WRITE, 0);
    offer(RX_AVAIL, RXNUM, 0);
    frame(f, 1);
    calls = k.ev[1].signals;
    CHECK(n->tb.rx(&n->tb, f, 60) == 1, "a frame taken");
    CHECK(get16(RX_USED + 2) == 1 && get32(RX_USED + 4) == 0 && get32(RX_USED + 8) == 72,
          "the chain used with 72 bytes");
    CHECK(mem[BUFS] == 0 && mem[BUFS + 10] == 1 && mem[BUFS + 11] == 0,
          "a header of no offload and num_buffers 1");
    CHECK(same(mem + BUFS + 0x100, f, 60), "the frame after the header");
    CHECK(k.ev[1].signals == calls + 1, "the call signalled");
    put16(RX_AVAIL, AVAIL_NO_INTR);
    desc(RX_DESC, 2, BUFS + 0x800, 1600, DESC_WRITE, 0);
    offer(RX_AVAIL, RXNUM, 2);
    CHECK(n->tb.rx(&n->tb, f, 60) == 1 && k.ev[1].signals == calls + 1,
          "no call while the guest asks for none");
    put16(RX_AVAIL, 0);
    desc(RX_DESC, 3, BUFS + 0x800, 40, DESC_WRITE, 0);
    offer(RX_AVAIL, RXNUM, 3);
    CHECK(n->tb.rx(&n->tb, f, 60) == 1, "a frame too long for the buffer is dropped");
    CHECK(get16(RX_USED + 2) == 2 && n->vq[VN_RX].last_avail == 2,
          "the short buffer stays the guest's");
    CHECK(n->tb.rx(&n->tb, f, 20) == 1 && get16(RX_USED + 2) == 3,
          "a frame that fits takes the short buffer");
    CHECK(n->tb.rx(&n->tb, f, 60) == 0 && get16(RX_USED) == 0,
          "the ring empty again asks for kicks");
    frame(k.queue[0], 9);
    frame(k.queue[1], 10);
    k.queued = 2;
    desc(RX_DESC, 4, BUFS + 0x1000, 1600, DESC_WRITE, 0);
    desc(RX_DESC, 5, BUFS + 0x1800, 1600, DESC_WRITE, 0);
    offer(RX_AVAIL, RXNUM, 4);
    offer(RX_AVAIL, RXNUM, 5);
    eventfd_signal(&k.ev[0]);
    CHECK(n->vq[VN_RX].kicked && vn_work, "the kick marks the ring");
    vn_pass();
    CHECK(k.queued == 0 && get16(RX_USED + 2) == 5, "the pass drains the tap");
    CHECK(mem[BUFS + 0x1000 + 12] == 9 && mem[BUFS + 0x1800 + 12] == 10,
          "the drained frames in order");
    desc(RX_DESC, 6, BUFS + 0x1000, 1600, DESC_WRITE, 0);
    desc(RX_DESC, 7, BUFS + 0x1800, 1600, DESC_WRITE, 0);
    offer(RX_AVAIL, RXNUM, 6);
    offer(RX_AVAIL, RXNUM, 7);
    eventfd_signal(&k.ev[0]);
    vn_pass();
    CHECK(get16(RX_USED) == USED_NO_NOTIFY, "no kicks while the ring has buffers");
    CHECK(n->tb.rx(&n->tb, f, 60) == 1 && n->tb.rx(&n->tb, f, 60) == 1, "both buffers used");
    CHECK(n->tb.rx(&n->tb, f, 60) == 0 && get16(RX_USED) == 0,
          "kicks again once the ring is found empty");
    offer(RX_AVAIL, RXNUM, 9);
    calls = k.ev[2].signals;
    CHECK(n->tb.rx(&n->tb, f, 60) == 0 && n->vq[VN_RX].broken, "a head past the ring breaks it");
    CHECK(k.ev[2].signals == calls + 1, "the error eventfd signalled");
    vn_ops.close(&e);
    CHECK(k.tap[0].b == 0, "the close lets the tap go");
}

/** The transmit ring: a frame gathered from two buffers and its header
 * stripped, the call, a pass's budget of 128 frames, a buffer the device
 * may write, and the error. */
static void test_transmit(void)
{
    fd_entry_t e;
    struct vnet *n;
    uint32_t i;
    int calls;

    printf("transmit\n");
    ready(&e);
    n = (struct vnet *)e.priv;
    CHECK(get16(TX_USED) == 0, "the empty ring asks for kicks");
    frame(mem + BUFS + 12, 33);
    desc(TX_DESC, 0, BUFS, 12, DESC_NEXT, 1);
    desc(TX_DESC, 1, BUFS + 12, 60, 0, 0);
    offer(TX_AVAIL, TXNUM, 0);
    calls = k.ev[4].signals;
    eventfd_signal(&k.ev[3]);
    CHECK(n->vq[VN_TX].kicked && vn_work, "the kick marks the ring");
    vn_pass();
    CHECK(k.injected == 1 && k.inject_len == 60 && k.inject[0] == 33,
          "the frame given to the stack without its header");
    CHECK(get16(TX_USED + 2) == 1 && get32(TX_USED + 8) == 0, "the chain used");
    CHECK(k.ev[4].signals == calls + 1, "the call signalled");
    desc(TX_DESC, 2, BUFS, 72, 0, 0);
    for (i = 0; i < 130; i++) {
        offer(TX_AVAIL, TXNUM, 2);
    }
    k.injected = 0;
    eventfd_signal(&k.ev[3]);
    vn_pass();
    CHECK(k.injected == 128 && vn_work && n->vq[VN_TX].kicked, "a pass takes 128 frames");
    vn_pass();
    CHECK(k.injected == 130 && !vn_work, "the next pass takes the rest");
    desc(TX_DESC, 3, BUFS, 72, DESC_WRITE, 0);
    offer(TX_AVAIL, TXNUM, 3);
    calls = k.ev[5].signals;
    eventfd_signal(&k.ev[3]);
    vn_pass();
    CHECK(n->vq[VN_TX].broken && k.ev[5].signals == calls + 1,
          "a buffer the device may write breaks the ring");
    CHECK(ring2(&e, VHOST_SET_VRING_NUM, 1, TXNUM) == -EBUSY, "a ring with the tap is busy");
    CHECK(ring2(&e, VHOST_NET_SET_BACKEND, 1, TAPFD2) == -EBUSY, "a second tap");
    CHECK(ring2(&e, VHOST_NET_SET_BACKEND, 1, -1) == 0 && k.tap[0].b == &n->tb,
          "one ring let go keeps the tap");
    CHECK(tapofd[0].refcount == 1, "the device holds the tap's descriptor");
    CHECK(ring2(&e, VHOST_NET_SET_BACKEND, 0, -1) == 0 && k.tap[0].b == 0 && n->tap == 0,
          "both let go free it");
    CHECK(tapofd[0].refcount == 0, "and its descriptor");
    vn_ops.close(&e);
}

/** What takes the rings away: the guest's memory unmapped, the tap's
 * descriptor closed, the kick's eventfd closed, and the close's references. */
static void test_teardown(void)
{
    fd_entry_t e;
    struct vnet *n;
    uint8_t f[60];
    int i;

    printf("teardown\n");
    ready(&e);
    n = (struct vnet *)e.priv;
    desc(RX_DESC, 0, BUFS, 1600, DESC_WRITE, 0);
    offer(RX_AVAIL, RXNUM, 0);
    frame(f, 1);
    n->reg[0].pin->live = 0;
    n->holder.release(n->reg[0].pin, n->holder.owner);
    CHECK(n->tb.rx(&n->tb, f, 60) == 0 && get16(RX_USED + 2) == 0,
          "no frame once the memory is unmapped");
    CHECK(mem_table(&e, 1, 0) == 0 && !n->dead, "a new table pins it again");
    CHECK(n->tb.rx(&n->tb, f, 60) == 1 && get16(RX_USED + 2) == 1, "frames again");
    CHECK(evofd[3].refcount == 1 && tapofd[0].refcount == 1,
          "the device holds the kick's descriptor and the tap's");
    CHECK(ring2(&e, VHOST_SET_VRING_KICK, 1, 99) == -EBADF && n->vq[VN_TX].kick == &k.ev[3] &&
              k.ev[3].hooks == &n->vq[VN_TX].hook && evofd[3].refcount == 1,
          "a kick of no descriptor keeps the old one");
    CHECK(ring2(&e, VHOST_SET_VRING_KICK, 1, EV0 + 6) == 0 && k.ev[3].refs == 0 &&
              k.ev[3].hooks == 0 && evofd[3].refcount == 0 && evofd[6].refcount == 1,
          "a new kick lets the old one and its descriptor go");
    n->vq[VN_TX].kicked = 0;
    eventfd_signal(&k.ev[6]);
    CHECK(n->vq[VN_TX].kicked, "the new kick marks the ring");
    n->tb.gone(&n->tb);
    k.tap[0].b = 0;
    CHECK(n->tap == 0 && n->tb.rx(&n->tb, f, 60) == 0, "the tap's close");
    vn_ops.close(&e);
    for (i = 0; i < NEVS; i++) {
        CHECK(k.ev[i].refs == 0 && evofd[i].refcount == 0, "every eventfd let go");
    }
    CHECK(tapofd[0].refcount == 0, "the tap's descriptor let go");
    CHECK(k.pins_made == k.pins_freed, "every pin let go");
}

/** A page of the guest's memory that a change took from its pin: a frame
 * for a buffer there waits in the tap while the thread brings the page in,
 * and goes after; a ring's own page does the same; a transmit chain there
 * stays the ring's until the page is in; a page that cannot be had breaks
 * the ring; and a transmit pass is one batch of TCP's. */
static void test_refill(void)
{
    fd_entry_t e;
    struct vnet *n;
    uint8_t f[60];
    int refills, calls, batches;

    printf("refill\n");
    ready(&e);
    n = (struct vnet *)e.priv;
    desc(RX_DESC, 0, BUFS, 1600, DESC_WRITE, 0);
    offer(RX_AVAIL, RXNUM, 0);
    frame(f, 5);
    drop_page(BUFS);
    refills = k.refills;
    CHECK(n->tb.rx(&n->tb, f, 60) == 0 && get16(RX_USED + 2) == 0 && n->vq[VN_RX].last_avail == 0 &&
              n->vq[VN_RX].want && !n->vq[VN_RX].broken,
          "a buffer's page not held leaves the frame in the tap and the ring as it was");
    CHECK(n->vq[VN_RX].kicked && vn_work, "and wakes the thread");
    frame(k.queue[0], 5);
    k.queued = 1;
    vn_pass();
    CHECK(k.refills == refills + 1 && !n->vq[VN_RX].want && k.queued == 1,
          "the thread brings the page in");
    vn_pass();
    CHECK(k.queued == 0 && get16(RX_USED + 2) == 1 && same(mem + BUFS + 12, f, 60) &&
              n->to_guest == 1,
          "and the frame goes on the next pass");
    desc(RX_DESC, 1, BUFS, 1600, DESC_WRITE, 0);
    offer(RX_AVAIL, RXNUM, 1);
    drop_page(RX_USED);
    CHECK(n->tb.rx(&n->tb, f, 60) == 0 && n->vq[VN_RX].want && !n->vq[VN_RX].broken,
          "a ring's own page not held leaves the frame too");
    vn_pass();
    CHECK(n->tb.rx(&n->tb, f, 60) == 1 && get16(RX_USED + 2) == 2,
          "and it goes once the page is in");
    frame(mem + BUFS + 0x100 + 12, 44);
    desc(TX_DESC, 0, BUFS + 0x100, 72, 0, 0);
    offer(TX_AVAIL, TXNUM, 0);
    drop_page(BUFS);
    k.injected = 0;
    batches = k.batches;
    eventfd_signal(&k.ev[3]);
    vn_pass();
    CHECK(k.injected == 0 && get16(TX_USED + 2) == 0 && n->vq[VN_TX].last_avail == 0 &&
              !n->vq[VN_TX].broken,
          "a transmit chain whose page is not held stays the ring's");
    CHECK(k.batches == batches + 1 && k.batch_depth == 0, "a transmit pass is one batch");
    vn_pass();
    CHECK(k.injected == 1 && k.inject[0] == 44 && get16(TX_USED + 2) == 1 && n->from_guest == 1,
          "and goes once the page is in");
    desc(RX_DESC, 2, BUFS, 1600, DESC_WRITE, 0);
    offer(RX_AVAIL, RXNUM, 2);
    drop_page(BUFS);
    k.refill_error = -EFAULT;
    calls = k.ev[2].signals;
    CHECK(n->tb.rx(&n->tb, f, 60) == 0, "a page not held again");
    vn_pass();
    CHECK(n->vq[VN_RX].broken && k.ev[2].signals == calls + 1 && !n->vq[VN_RX].want,
          "a page that cannot be had breaks the ring");
    k.refill_error = 0;
    vn_ops.close(&e);
}

/** Start the device and run the rows. */
int main(void)
{
    int i;

    for (i = 0; i < NEVS; i++) {
        evfd[i].ofd = &evofd[i];
    }
    tapfd[0].ofd = &tapofd[0];
    tapfd[1].ofd = &tapofd[1];
    cpu_table[0].current = &self;
    self.mm = (struct mm *)&mm_a;
    vhost_net_start();
    CHECK(k.node && k.threads == 1, "the node and the thread");
    test_requests();
    test_memory();
    test_receive();
    test_transmit();
    test_teardown();
    test_refill();
    printf("%d failed\n", fails);
    return fails > 255 ? 255 : fails;
}
