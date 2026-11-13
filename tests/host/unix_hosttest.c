/**
 * Host test of the AF_UNIX and pipe calls, with another thread's call run
 * inside their sleeps.
 *
 * host-deps: tests/host/compat/park_host.c
 * host-deps: tests/host/compat/kirq_none.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 * host-deps: rach2/sys/tick.c
 * host-deps: rach2/sys/fs/sleeplock.c tests/host/compat/spin_none.c
 * host-deps: tests/host/compat/fd_flat.c
 *
 * `ipc/unix.c`, `ipc/pipe.c`, the mutex and the checked user copy are the
 * kernel's, included whole to reach their static state. The descriptor table
 * the threads share, the heap, the wait queues and the file system's socket
 * files are models; user memory is eight model pages. A copy that reaches a
 * page not yet present sleeps while the page comes in, and there the model
 * runs the call a row gives the other thread, the sibling, on a stack of
 * its own (`compat/park_host.c`); so do the file system's making and lookup
 * of a socket file and the release of a file's last description, which can
 * sleep too. A sibling that must wait for a lock the sleeping call holds
 * parks: its turn ends in the wait, with its frames kept, and the sleeping
 * call goes on. The row resumes the sibling in its wait only after a wake
 * of the queue it waits on, as the scheduler runs a sleeper again only once
 * a wake has made it runnable.
 *
 * The descriptors accept, socketpair, a receive of SCM_RIGHTS and pipe2
 * make are reserved while their numbers, and for accept the peer's name,
 * are copied out: a sibling's close of a reserved number is EBADF, a
 * sibling's open takes another number, and a copy that fails frees only
 * what the call reserved. A connect whose copy of the address, or lookup of
 * a path, sleeps while a sibling connects the socket fails with EISCONN,
 * and the listener has the sibling's connection alone. A second bind of a
 * socket waits while the first sleeps in the file system and then fails
 * with EINVAL; one socket file is made, and the close lets it go. A receive
 * while another's copy of a message sleeps waits for the receive lock and
 * then takes the next message, on a datagram pair and on a stream pair. A
 * send whose copy sleeps while a sibling shuts the socket down for writing
 * queues nothing and fails with EPIPE, on both. A KRFFS file with no name
 * whose other holder is a descriptor a receive has taken off its queue is
 * not freed by a sibling's close of its last descriptor, while the receive
 * installs that descriptor or closes it with the others it has no room for;
 * the release of a descriptor in flight that is its file's last holder
 * frees the file; and a descriptor the receive has installed is no longer
 * counted in flight. A descriptor in flight while its file moves is counted,
 * installed and released by the node the move gave its description. Each
 * row ends with nothing allocated. A pipe's sleeping lock is the kernel's
 * (`fs/sleeplock.c`). The sibling runs as the row's own process, which that
 * lock takes for its holder, so a sibling would take a pipe's lock that the
 * sleeping call holds without waiting; no row has it take one. Where the
 * row's own thread sleeps in a socket's wait, a row's `own_wait` moves the
 * model's clock, queues bytes or has a sibling take the signal, and the
 * deadline each wait arms is kept, so that the rows of SO_RCVTIMEO and
 * SO_SNDTIMEO judge the time limit by ticks.
 */
#include "ipc/unix.c"
#include "ipc/pipe.c"
#include "ipc/pipe_ring.c"
#include "mm/usercopy.c"
#include "proc/mutex.c"

/** The compare-and-exchange of the mutex's and the rwsem's words and of
 * `cmpxchg_user_u32`, on the host's one thread. */
unsigned int arch_atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value)
{
    unsigned int was = *p;

    if (was == old) {
        *p = value;
    }
    return was;
}

/** The exchange that frees the mutex, on the host's one thread. */
unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value)
{
    unsigned int was = *p;

    *p = value;
    return was;
}

/** The compare-and-exchange of the mutex's word, on the host's one thread. */
uintptr_t arch_atomic_cmpxchg_ptr(volatile uintptr_t *p, uintptr_t old, uintptr_t value)
{
    uintptr_t was = *p;

    if (was == old) {
        *p = value;
    }
    return was;
}

/** The exchange of the mutex's word, on the host's one thread. */
uintptr_t arch_atomic_xchg_ptr(volatile uintptr_t *p, uintptr_t value)
{
    uintptr_t was = *p;

    *p = value;
    return was;
}

/** No hypervisor preempts the host's processors here. */
int arch_cpu_preempted(int cpu)
{
    (void)cpu;
    return 0;
}

/** A spinner's pause, which no row reaches (`sched_spin_pinned`). */
void arch_relax(void)
{}
#include "ns_first.h"

extern void *malloc(unsigned long n);
extern void free(void *p);
extern void exit(int status);
extern int dprintf(int fd, const char *format, ...);

int park_run(void (*fn)(void));
int park_resume(void);
int park_active(void);
int park_waiting(void);
void park(void);

/** Print the panic's message, such as a sleeping lock given by a thread
 * that does not hold it, and exit with status 1. */
void k_panic(const char *message)
{
    dprintf(2, "unix-hosttest: fail: panic: %s\n", message);
    exit(1);
}

/** Return 0: no hold of a sleeping lock here is timed. */
uint32_t timer_now_us(void)
{
    return 0;
}

/** The cycle counter: the clock's microseconds, a cycle each. */
uint64_t arch_cycles(void)
{
    return timer_now_us();
}

/** A cycle is a microsecond here. */
uint64_t timer_cycles_us(uint64_t cycles)
{
    return cycles;
}

/** The cycles here are the clock's microseconds, the same on every
 * processor. */
int timer_cycles_agree(void)
{
    return 1;
}

#define USER_BASE 0x04000000u
#define PAGES     8
#define SLOTS     16
#define FILES     4
#define SOCKFILES 4

/* The user address `off` bytes into model page `page`. */
#define AT(page, off) (USER_BASE + (uint32_t)(page) * PAGE_SIZE + (uint32_t)(off))

/* A model page is present; missing, which the first copy to reach it brings
 * in after the sibling runs; lost, which that copy then finds unmapped; or
 * absent, which every copy finds unmapped. */
enum { PAGE_PRESENT, PAGE_MISSING, PAGE_LOST, PAGE_ABSENT };

/* A socket file bind has made, and whether a socket holds it. */
struct sockfile {
    char path[16];
    int made;
    int held;
};

static process_t self;
/* The address space of `self`, which the copies ask for and never read. */
static struct mm model_mm;
struct percpu cpu_table[NR_CPUS] = {{.current = &self}};
volatile uint64_t kernel_ticks;

/** The tick count as the kernel reads it whole: the model's `kernel_ticks`. */
uint64_t tick_count(void)
{
    return kernel_ticks;
}

/** The tick count as the clocks count it, with no tick pending here. */
uint64_t rtc_ticks_now(void)
{
    return kernel_ticks;
}

/** No row's caller is privileged: 0. */
int cred_privileged(void)
{
    return 0;
}
static struct fs_struct dirs;
static fd_table_t shared;
static fd_entry_t slots[SLOTS];
static int closing_on_get = -1; /* close begins just after this lookup */
static uint8_t memory[PAGES * PAGE_SIZE];
static int pages[PAGES];

/* KRFFS files that lost their names while open: the release of the last
 * holder of one frees it, which `freed` counts. */
static krffs_node_t nodes[FILES];
static int freed[FILES];

/* The root's namespace lock, which the release of a descriptor of one of
 * those files takes (`fd_release_lock`): how deep the holder has it. */
static struct vfs_mount rootmount;
static int root_depth;

static struct sockfile sockfiles[SOCKFILES];
static struct vfs_mount sockmount;
static int files_made;

/* The sibling's call, run once at the sleep it is armed for: a copy's page
 * fault, the file system's making of a socket file, or the release of a
 * file's last description. */
static void (*page_sibling)(void);
static void (*fs_sibling)(void);
static void (*release_sibling)(void);
/* The sibling's call that holds the root's lock when a release first asks
 * for it, which runs before the release has it. */
static void (*lock_sibling)(void);
/* How many siblings ran, and whether the last one is parked in a wait: on
 * which queue, and how many wakes of that queue came since it parked. */
static int ran, parked;
static wait_queue_t *parked_on;
static uint32_t parked_wakes;
/* Set by a row whose sleeping copy reads what a sibling that got through
 * would have freed: that copy fails, rather than read freed memory. */
static int copy_needs_park;

/* What a row's sibling acts on and what it found. */
static int reserved[2];
static int32_t sibling_rc[2];
static int seen[FILES];
static uint32_t name_len;

static int failures, live, sigpipes;
static int model_big, signal_checks, signal_without_big, ignore_signal;
static int check_signal_context;
static struct mutex *signal_unlock;

/* What the row's own thread meets while it sleeps in a flag wait, which no
 * sibling can end: run at each such sleep, it may move the clock, queue
 * bytes or have a sibling take the signal, and gives 1 for the signal flag,
 * which ends the wait with -EINTR, or 0 for a wake, after which the wait
 * tests its condition again. With none, the flag ends the wait at once. */
static int (*own_wait)(void);
/* The sleeps `own_wait` saw, and the deadline armed for each, the tick in
 * `wakeup_tick`, 0 for none. */
#define OWN_WAITS 8
static int own_waits;
static uint64_t own_deadline[OWN_WAITS];
static uint32_t inodes;

/** Report `detail` when `condition` is false, and return the condition, so
 * that a row can skip the checks a failure leaves without meaning. */
static int check(int condition, const char *detail)
{
    if (!condition) {
        dprintf(2, "unix-hosttest: fail: %s\n", detail);
        failures++;
    }
    return condition;
}

/** The host address of a model user address, or `p` for kernel memory. */
static void *host(const void *p)
{
    unsigned long a = (unsigned long)p;

    if (a >= USER_BASE && a < USER_BASE + sizeof(memory)) {
        return memory + (a - USER_BASE);
    }
    return (void *)p;
}

/** Copy `n` bytes, translating model user addresses with `host`. */
void *k_memcpy(void *dest, const void *src, size_t n)
{
    uint8_t *d = host(dest);
    const uint8_t *s = host(src);
    size_t i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

/** Fill `n` bytes with `c`, translating a model user address with
 * `host`. */
void *k_memset(void *ptr, int c, size_t n)
{
    uint8_t *d = host(ptr);
    size_t i;

    for (i = 0; i < n; i++) {
        d[i] = (uint8_t)c;
    }
    return ptr;
}

/** The kernel heap over the host's; `live` counts the blocks it has out. */
void *kmalloc(size_t size)
{
    void *p = malloc(size ? size : 1u);

    if (p) {
        live++;
    }
    return p;
}

/** `kmalloc`, zeroed. */
void *kzalloc(size_t size)
{
    void *p = kmalloc(size);

    if (p) {
        k_memset(p, 0, size);
    }
    return p;
}

/** Free a block `kmalloc` gave and count it back. */
void kfree(void *p)
{
    if (p) {
        live--;
        free(p);
    }
}

/**
 * Run the sibling call armed in `*slot`, once, on a stack of its own, as the
 * scheduler runs another thread while this one sleeps. The sibling runs
 * until it returns or parks in a wait; a parked one is resumed with
 * `wake_sibling`.
 */
static void run_sibling(void (**slot)(void))
{
    void (*call)(void) = *slot;

    *slot = NULL;
    if (!call) {
        return;
    }
    if (park_active() || park_waiting()) {
        check(0, "a sibling is armed at a sleep while another runs or sleeps");
        return;
    }
    ran++;
    parked = !park_run(call);
}

/**
 * Resume the parked sibling in its wait, as the scheduler runs a sleeper
 * again once a wake of its queue has made it runnable: 1 when the sibling
 * then returned. One that no wake has reached stays asleep, and 0 is
 * returned without running it.
 */
static int wake_sibling(void)
{
    if (!parked || parked_wakes == 0) {
        return 0;
    }
    parked = !park_resume();
    return !parked;
}

/** Park the sibling in its wait on `wq`, until a row resumes it. */
static void sleep_on(wait_queue_t *wq)
{
    parked_on = wq;
    parked_wakes = 0;
    park();
    parked_on = NULL;
}

/**
 * Bring in the pages of [addr, addr + len) for a copy: 1, or 0 when one is
 * unmapped. A missing or lost page is a sleep, in which the sibling runs; a
 * lost page is unmapped after it, and so is a missing one when the row needs
 * the sibling parked and it got through.
 */
int vm_prefault_range(uintptr_t addr, uintptr_t len, int write)
{
    uint32_t page, last;

    (void)write;
    if (len == 0) {
        return 1;
    }
    if (addr < USER_BASE || addr - USER_BASE >= sizeof(memory) ||
        len > sizeof(memory) - (addr - USER_BASE)) {
        return 0;
    }
    last = (addr - USER_BASE + len - 1u) / PAGE_SIZE;
    for (page = (addr - USER_BASE) / PAGE_SIZE; page <= last; page++) {
        if (pages[page] == PAGE_MISSING || pages[page] == PAGE_LOST) {
            int lost = pages[page] == PAGE_LOST;

            pages[page] = PAGE_PRESENT;
            run_sibling(&page_sibling);
            if (lost || (copy_needs_park && !parked)) {
                pages[page] = PAGE_ABSENT;
            }
        }
        if (pages[page] == PAGE_ABSENT) {
            return 0;
        }
    }
    return 1;
}

/** `vm_user_begin` over the model: the range brought in by
 * `vm_prefault_range`, 1 as with the regions' lock held, which the model
 * does not keep; 0 for an empty range and -1 where the prefault fails. */
int vm_user_begin(uintptr_t addr, uintptr_t len, int write)
{
    if (len == 0) {
        return 0;
    }
    return vm_prefault_range(addr, len, write) ? 1 : -1;
}

/** End what `vm_user_begin` began: nothing to give back. */
void vm_user_end(void)
{}

/**
 * `arch_user_copy` over the model: the pages of the user end are touched in
 * order, and one that is not present for the access faults and is brought
 * in as the page fault handler would bring it in (`vm_prefault_range`).
 * Returns the bytes not copied from the first page the fault cannot serve,
 * or 0.
 */
size_t arch_user_copy(void *dst, const void *src, size_t n)
{
    uintptr_t d = (uintptr_t)dst;
    int write = d >= USER_BASE && d - USER_BASE < sizeof(memory);
    uintptr_t user = write ? d : (uintptr_t)src;
    size_t done = 0;

    while (done < n) {
        size_t room = PAGE_SIZE - ((user + done) & (PAGE_SIZE - 1u));
        size_t chunk = n - done < room ? n - done : room;

        if (!vm_resident(user + done, chunk, write) &&
            !vm_prefault_range(user + done, chunk, write)) {
            return n - done;
        }
        k_memcpy((char *)dst + done, (const char *)src + done, chunk);
        done += chunk;
    }
    return 0;
}

/** `vm_prefault_held`: the model's prefault. */
int vm_prefault_held(uintptr_t addr, uintptr_t len, int write)
{
    return vm_prefault_range(addr, len, write);
}

/** The regions' lock of the string copies: the model has none. */
void mmap_read_lock(struct mm *mm)
{
    (void)mm;
}

/** Give back what `mmap_read_lock` took: nothing. */
void mmap_unlock(struct mm *mm)
{
    (void)mm;
}

/** Whether every page of [addr, addr + len) is present, with no sleep: 1,
 * or 0 for a page outside the model or not present, which the caller then
 * brings in (`vm_prefault_range`). */
int vm_resident(uintptr_t addr, uintptr_t len, int write)
{
    uint32_t page, last;

    (void)write;
    if (len == 0) {
        return 1;
    }
    if (addr < USER_BASE || addr - USER_BASE >= sizeof(memory) ||
        len > sizeof(memory) - (addr - USER_BASE)) {
        return 0;
    }
    last = (addr - USER_BASE + len - 1u) / PAGE_SIZE;
    for (page = (addr - USER_BASE) / PAGE_SIZE; page <= last; page++) {
        if (pages[page] != PAGE_PRESENT) {
            return 0;
        }
    }
    return 1;
}

/** `vm_ready_held`, which the futex word accesses of usercopy.c ask: the
 * model's `vm_resident`, with no sleep either way. */
int vm_ready_held(uintptr_t addr, uintptr_t len, int write)
{
    return vm_resident(addr, len, write);
}

/** Whether a pin may take the page of `addr`: always, since the model has
 * no region that Linux's get_user_pages refuses. */
int paging_pin_allows(struct mm *mm, uintptr_t addr, int write)
{
    (void)mm;
    (void)addr;
    (void)write;
    return 1;
}

/** Whether a buffer lies in user memory, as `user_buffer_ok` asks, without
 * bringing it in. */
int user_buffer_ok(uintptr_t addr, size_t len, int write)
{
    (void)write;
    return addr >= USER_BASE && addr - USER_BASE <= sizeof(memory) &&
           len <= sizeof(memory) - (addr - USER_BASE);
}

/*
 * The caller's structures in the kernel's own layout, as syscall_net.c and
 * syscall_fs.c read them for a native call.
 */

int iov_fetch(struct kernel_iovec *iov, uintptr_t uvec, uint32_t i)
{
    return copy_from_user(iov, (const void __user *)(uvec + (uintptr_t)i * sizeof(*iov)),
                          sizeof(*iov)) != 0
               ? -EFAULT
               : 0;
}

/** Whether the call is an i386 program's on the x86-64 kernel: never, since
 * every call of the test is native. */
int in_compat_syscall(void)
{
    return 0;
}

/* syscall_fs.c's iov_import, line for line: every length checked as the
 * caller's ssize_t, then each range, the total cut at USER_RW_MAX. */
long iov_import(struct kernel_iovec *iov, uintptr_t uvec, int count, int prefault)
{
    struct kernel_iovec one;
    uint32_t total = 0;
    int pass, i;

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < count; i++) {
            struct kernel_iovec *e = iov ? &iov[i] : &one;
            unsigned long len;

            if ((pass == 0 || !iov) && iov_fetch(e, uvec, (uint32_t)i) != 0) {
                return -EFAULT;
            }
            if ((long)e->iov_len < 0 || (in_compat_syscall() && e->iov_len > 0x7FFFFFFFu)) {
                return -EINVAL;
            }
            if (pass == 0) {
                continue;
            }
            len = count == 1 ? USER_RW_LEN(e->iov_len) : e->iov_len;
            if (!user_span_ok(e->iov_base, len)) {
                return -EFAULT;
            }
            if (len > USER_RW_MAX - total) {
                len = USER_RW_MAX - total;
            }
            if (prefault >= 0 && len > 0 && !user_buffer_ok(e->iov_base, len, prefault)) {
                return -EFAULT;
            }
            e->iov_len = len;
            total += (uint32_t)len;
        }
    }
    return (long)total;
}

uint32_t msghdr_size(void)
{
    return (uint32_t)sizeof(struct kernel_msghdr);
}

int msghdr_get(struct kernel_msghdr *h, uintptr_t u)
{
    return copy_from_user(h, (const void __user *)u, sizeof(*h)) != 0 ? -EFAULT : 0;
}

int msghdr_put(uintptr_t u, const struct kernel_msghdr *h)
{
    return copy_to_user((void __user *)u, h, sizeof(*h)) != 0 ? -EFAULT : 0;
}

uint32_t cmsg_hdr(void)
{
    return (uint32_t)CMSG_HDR;
}

uint32_t cmsg_align(uint32_t n)
{
    return (uint32_t)CMSG_ALIGN(n);
}

int cmsg_get(struct kernel_cmsghdr *h, uintptr_t at)
{
    return copy_from_user(h, (const void __user *)at, sizeof(*h)) != 0 ? -EFAULT : 0;
}

int cmsg_put(uintptr_t at, const struct kernel_cmsghdr *h)
{
    return copy_to_user((void __user *)at, h, sizeof(*h)) != 0 ? -EFAULT : 0;
}

int32_t sock_timeo_get(int name, uintptr_t val, uint32_t len, int64_t tv[2])
{
    long t[2];

    (void)name;
    if (len < sizeof(t)) {
        return -EINVAL;
    }
    if (copy_from_user(t, (const void __user *)val, sizeof(t)) != 0) {
        return -EFAULT;
    }
    tv[0] = t[0];
    tv[1] = t[1];
    return 0;
}

uint32_t sock_timeo_put(int name, const int64_t tv[2], uint8_t out[16])
{
    long t[2];

    (void)name;
    t[0] = (long)tv[0];
    t[1] = (long)tv[1];
    memcpy(out, t, sizeof(t));
    return (uint32_t)sizeof(t);
}

/** The wall clock a message is stamped with when sent: the epoch. */
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 0;
    *nanoseconds = 0;
}

/** The timestamp bits as syscall_net.c keeps them: on asks for SO_TIMESTAMP
 * alone, which these rows use, and off for none. */
uint8_t sock_tstamp_set(uint8_t bits, int optname, int val)
{
    (void)optname;
    return (uint8_t)((bits & ~0xE0u) | (val ? SOCK_RX_TSTAMP : 0u));
}

/** Whether SO_TIMESTAMP is the one asked for. */
int sock_tstamp_get(uint8_t bits, int optname)
{
    (void)optname;
    return (bits & 0xE0u) == SOCK_RX_TSTAMP;
}

/** A native SO_TIMESTAMP_OLD timeval of `sec` and `nsec`. */
uint32_t sock_tstamp_put(uint8_t bits, int64_t sec, int32_t nsec, int *type, uint8_t out[16])
{
    int64_t tv[2];

    (void)bits;
    tv[0] = sec;
    tv[1] = nsec / 1000;
    *type = SO_TIMESTAMP_OLD;
    return sock_timeo_put(SO_RCVTIMEO_OLD, tv, out);
}

/**
 * A wait: it ends when `cond` holds. A sibling whose condition does not
 * hold sleeps, and tests the condition again each time it is resumed; the
 * row's own thread is ended by a signal, since no other thread could make
 * the condition hold.
 */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    while (!cond(arg)) {
        if (!park_active()) {
            if (check_signal_context) {
                (void)signal_pending();
            }
            return -EINTR;
        }
        sleep_on(wq);
    }
    return 0;
}

/** A named wait, which the model takes as `wq_wait_event`. */
int wq_wait_event_as(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name)
{
    (void)name;
    return wq_wait_event(wq, cond, arg);
}

/** A flag wait never examines the signal queues; -EINTR stands for the
 * flag set, for its caller to judge under the big lock. */
int wq_wait_event_sigflag(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name)
{
    (void)name;
    while (!cond(arg)) {
        if (!park_active()) {
            if (!own_wait || own_waits >= OWN_WAITS) {
                return -EINTR;
            }
            own_deadline[own_waits++] = current_proc->wakeup_tick;
            if (own_wait()) {
                return -EINTR;
            }
            continue;
        }
        sleep_on(wq);
    }
    return 0;
}

/** Judge the model signal and record whether its queue was protected. An
 * ignored signal can coincide with the holder giving its mutex back. */
int signal_pending(void)
{
    if (check_signal_context) {
        signal_checks++;
        signal_without_big += !bkl_held();
    }
    if (ignore_signal) {
        ignore_signal = 0;
        if (signal_unlock) {
            mutex_unlock(signal_unlock);
            signal_unlock = NULL;
        }
        return 0;
    }
    return 1;
}

/** A killable wait, which this model never calls in a row. */
int wq_wait_event_killable(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name)
{
    return wq_wait_event_sigflag(wq, cond, arg, name);
}

/** An uninterruptible wait, which only a sibling can make and sleep in. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    while (!cond(arg)) {
        if (!park_active()) {
            dprintf(2, "unix-hosttest: fail: an uninterruptible wait that no call can end\n");
            exit(1);
        }
        sleep_on(wq);
    }
    return 0;
}

/**
 * A wake counts one for each kind of change it names, as wait_queue.c
 * does, and one in `parked_wakes` when it is of the queue the parked sibling
 * sleeps on, whatever its kinds, since it makes that sibling runnable.
 */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    int k;

    for (k = 0; k < WQ_KINDS; k++) {
        if (kinds & (1u << k)) {
            wq->wakes[k]++;
        }
    }
    if (wq == parked_on) {
        parked_wakes++;
    }
}

/** A handoff of a mutex to the parked sibling, which runs as the row's own
 * process, or to no one, counted as a wake of the sibling's queue. */
void wq_wake_first(wait_queue_t *wq, void (*fn)(struct process *first, void *arg), void *arg)
{
    if (wq == parked_on) {
        fn(current_proc, arg);
        parked_wakes++;
    } else {
        fn(NULL, arg);
    }
}

/** A wake of every kind of change, as `wq_wake` counts it. */
void wq_wake_all(wait_queue_t *wq)
{
    wq_wake(wq, WQ_ALL);
}

/** Whether a sleeper is on `wq`: the parked sibling alone can be. */
int wq_active(const wait_queue_t *wq)
{
    return wq != NULL && wq == parked_on;
}

/** Empty the queue `wq` and zero its counts. */
void wait_queue_init(wait_queue_t *wq)
{
    k_memset(wq, 0, sizeof(*wq));
}

/** Take a reference to the description `ofd`, when there is one. */
void fd_ofd_ref(struct open_file *ofd)
{
    if (ofd) {
        ofd->refcount++;
    }
}

/** Drop a reference to a description. The last of a file's closes the file
 * system's description, which can sleep, and a row's sibling can run there. */
void fd_ofd_unref(struct open_file *ofd)
{
    if (!ofd || ofd->refcount == 0 || --ofd->refcount != 0) {
        return;
    }
    if (ofd->vnode) {
        run_sibling(&release_sibling);
    }
    kfree(ofd);
}

/**
 * Reserve the lowest free slot, as `fd_alloc_from` does: it gets a new
 * description and keeps FD_TYPE_NONE until its caller gives it a type, so
 * that no lookup finds it.
 */
int fd_alloc(fd_table_t *table)
{
    int i;

    for (i = 0; i < table->cap; i++) {
        fd_entry_t *e = &table->chunks[0][i];

        if (e->type == FD_TYPE_NONE && !e->ofd) {
            e->ofd = kzalloc(sizeof(*e->ofd));
            if (!e->ofd) {
                return -ENOMEM;
            }
            e->ofd->refcount = 1;
            e->ops = NULL;
            e->priv = NULL;
            e->dev = NULL;
            return i;
        }
    }
    return -EMFILE;
}

/** Free a slot, reserved or not, and drop its description, as `fd_free`
 * does: 0, or -1 for a slot already free. */
int fd_free(fd_table_t *table, int fd)
{
    fd_entry_t *e;

    if (fd < 0 || fd >= table->cap) {
        return -1;
    }
    e = &table->chunks[0][fd];
    if (e->type == FD_TYPE_NONE && !e->ofd) {
        return -1;
    }
    fd_ofd_unref(e->ofd);
    k_memset(e, 0, sizeof(*e));
    return 0;
}

/** The entry `e` as it is now: where it was, since the model's table never
 * grows. */
fd_entry_t *fd_entry_now(fd_table_t *table, fd_entry_t *e)
{
    (void)table;
    return e;
}

/** The descriptor `fd`, or NULL for a slot that is free, reserved or
 * closing. */
fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    fd_entry_t *e;

    if (fd < 0 || fd >= table->cap) {
        return NULL;
    }
    e = &table->chunks[0][fd];
    if (e->type == FD_TYPE_NONE || (e->flags & FD_FLAG_CLOSING)) {
        return NULL;
    }
    if (closing_on_get == fd) {
        e->flags |= FD_FLAG_CLOSING;
        closing_on_get = -1;
    }
    return e;
}

/** The current node of a model KRFFS regular file descriptor. */
krffs_node_t *fd_root_node(const fd_entry_t *e)
{
    return e->type == FD_TYPE_FILE && e->node && e->ofd ? e->ofd->vnode : NULL;
}

/** The holders of the file `node` other than `except`, as `fd_node_holders`
 * counts them: the table's descriptors of it, and one for any in flight. */
static int holders(const krffs_node_t *node, const fd_entry_t *except)
{
    int n = unix_inflight_holds(node) ? 1 : 0;
    int i;

    for (i = 0; i < shared.cap; i++) {
        const fd_entry_t *e = &shared.chunks[0][i];

        if (e != except && e->type == FD_TYPE_FILE && e->node == node) {
            n++;
        }
    }
    return n;
}

/**
 * Take the root's lock for the release of `e`, as `fd_release_lock` does for
 * a descriptor of a KRFFS file, or NULL for any other. A request the caller
 * does not already hold first runs the sibling armed for it, the process
 * that holds the lock, whose call ends before the caller has it.
 */
struct vfs_mount *fd_release_lock(const fd_entry_t *e)
{
    if (e->type != FD_TYPE_FILE || !e->node) {
        return NULL;
    }
    if (root_depth == 0) {
        run_sibling(&lock_sibling);
    }
    root_depth++;
    return &rootmount;
}

/** Give back the root's lock `fd_release_lock` took. */
void vfs_ns_unlock(struct vfs_mount *mnt)
{
    check(mnt == &rootmount && root_depth > 0, "a release gives back the root's lock it took");
    root_depth--;
}

/**
 * Release what a descriptor holds, as `fd_release_objects` does for the
 * kinds the rows make: a file is freed by the release of its last holder,
 * which counts the holders and frees it under the root's lock, and a socket
 * or a pipe end is closed with its description's last descriptor.
 */
void fd_release_backing(fd_entry_t *e)
{
    struct vfs_mount *locked;

    if (!e) {
        return;
    }
    locked = fd_release_lock(e);
    if (e->type == FD_TYPE_FILE && e->node && holders(e->node, e) == 0) {
        freed[e->node - nodes]++;
        e->node = NULL;
    }
    if (locked) {
        vfs_ns_unlock(locked);
    }
    if (e->ops && e->ops->close && e->ofd && e->ofd->refcount == 1) {
        e->ops->close(e);
    }
}

/** The next socket inode number, from 1. */
uint32_t socket_inode_next(void)
{
    return ++inodes;
}

/** A failure: no row sends a network socket. */
void socket_ref(struct socket *sock)
{
    (void)sock;
    check(0, "no row sends a network socket");
}

/** Count a SIGPIPE; post nothing. */
void signal_post_thread(process_t *t, int signo, const struct k_siginfo *info)
{
    (void)t;
    (void)info;
    if (signo == SIGPIPE) {
        sigpipes++;
    }
}

/** The length of the string `s`. */
static uint32_t length(const char *s)
{
    uint32_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Whether the strings `a` and `b` are equal. */
static int same(const char *a, const char *b)
{
    uint32_t i;

    for (i = 0; a[i] && a[i] == b[i]; i++) {
    }
    return a[i] == b[i];
}

/**
 * Make the socket file `path`. The file system sleeps first, and the sibling
 * armed there runs, as another thread would while this one waits for the
 * disk. EADDRINUSE when the path has a file.
 */
int32_t fs_socket_bind(const char *path, uint32_t mode, struct vfs_ref *ref, uint32_t *ino)
{
    int i;

    (void)mode;
    run_sibling(&fs_sibling);
    for (i = 0; i < SOCKFILES; i++) {
        if (sockfiles[i].made && same(sockfiles[i].path, path)) {
            return -EADDRINUSE;
        }
    }
    for (i = 0; i < SOCKFILES && sockfiles[i].made; i++) {
    }
    if (i == SOCKFILES || length(path) >= sizeof(sockfiles[i].path)) {
        return -ENOSPC;
    }
    k_memcpy(sockfiles[i].path, path, length(path) + 1u);
    sockfiles[i].made = 1;
    sockfiles[i].held = 1;
    files_made++;
    ref->mnt = &sockmount;
    ref->node = &sockfiles[i];
    *ino = 100u + (uint32_t)i;
    return 0;
}

/** Find the socket file `path`, after a sleep in which the sibling armed
 * there runs, as in `fs_socket_bind`. ENOENT when there is none. */
int32_t fs_socket_lookup(const char *path, struct vfs_mount **mnt, uint32_t *ino)
{
    int i;

    run_sibling(&fs_sibling);
    for (i = 0; i < SOCKFILES; i++) {
        if (sockfiles[i].made && same(sockfiles[i].path, path)) {
            *mnt = &sockmount;
            *ino = 100u + (uint32_t)i;
            return 0;
        }
    }
    return -ENOENT;
}

/** Let go of a socket file, which stays until it is removed, as in Linux. */
void fs_socket_unbind(struct vfs_ref *ref)
{
    ((struct sockfile *)ref->node)->held = 0;
}

/** The host address of the model user address `addr`. */
static uint8_t *user(uint32_t addr)
{
    return memory + (addr - USER_BASE);
}

/** The 32-bit word at the model user address `addr`. */
static uint32_t word(uint32_t addr)
{
    uint32_t w;

    k_memcpy(&w, user(addr), sizeof(w));
    return w;
}

/** Store the 32-bit word `w` at the model user address `addr`. */
static void set_word(uint32_t addr, uint32_t w)
{
    k_memcpy(user(addr), &w, sizeof(w));
}

/** Store the bytes of `s`, without its NUL, at the model user address
 * `addr`. */
static void set_bytes(uint32_t addr, const char *s)
{
    k_memcpy(user(addr), s, length(s));
}

/** Whether the bytes at the model user address `addr` are those of `s`. */
static int bytes_are(uint32_t addr, const char *s)
{
    uint32_t i;

    for (i = 0; s[i]; i++) {
        if (user(addr)[i] != (uint8_t)s[i]) {
            return 0;
        }
    }
    return 1;
}

/** A sockaddr_un at `addr` with the abstract name `name`: its length. */
static uint32_t abstract_at(uint32_t addr, const char *name)
{
    uint8_t *p = user(addr);

    p[0] = AF_UNIX;
    p[1] = 0;
    p[2] = 0;
    k_memcpy(p + 3, name, length(name));
    return 3u + length(name);
}

/** A sockaddr_un at `addr` with the path `path`: its length with the NUL. */
static uint32_t path_at(uint32_t addr, const char *path)
{
    uint8_t *p = user(addr);

    p[0] = AF_UNIX;
    p[1] = 0;
    k_memcpy(p + 2, path, length(path) + 1u);
    return 3u + length(path);
}

/** Slot `fd` of the shared table, whatever its state. */
static fd_entry_t *fd_at(int fd)
{
    return &shared.chunks[0][fd];
}

/** The socket of slot `fd` of the shared table. */
static struct unix_sock *sock_at(int fd)
{
    return (struct unix_sock *)shared.chunks[0][fd].priv;
}

/** close(2) in the shared table, as `close_fd` closes a descriptor no other
 * call holds: 0, or -EBADF for a slot with no descriptor, a reserved one
 * among them. */
static int32_t do_close(int fd)
{
    fd_entry_t *e = fd_get(&shared, fd);

    if (!e) {
        return -EBADF;
    }
    e->flags |= FD_FLAG_CLOSING | FD_FLAG_RELEASING;
    fd_release_backing(e);
    fd_free(&shared, fd);
    return 0;
}

/** open(2) for reading and writing of the file `nodes[n]`, a KRFFS file
 * with no name whose description names its node: the descriptor. */
static int open_orphan(int n)
{
    int fd = fd_alloc(&shared);
    fd_entry_t *e;

    if (fd < 0) {
        return fd;
    }
    e = &shared.chunks[0][fd];
    e->node = &nodes[n];
    e->ofd->vnode = &nodes[n];
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    e->type = FD_TYPE_FILE;
    return fd;
}

/** A socketpair of `type`, its numbers stored on page 5. */
static int32_t pair(int type)
{
    return unix_socketpair(type, 0, 0, AT(5, 0));
}

/** Send the bytes of `s` over `fd` from page 5. */
static int32_t send_text(int fd, const char *s)
{
    set_bytes(AT(5, 64), s);
    return unix_sendto(fd_at(fd), AT(5, 64), length(s), 0, 0, 0);
}

/** Whether a byte sent over `from` is received on `to`. */
static int crosses(int from, int to)
{
    user(AT(5, 128))[0] = 0;
    return send_text(from, "x") == 1 &&
           unix_recvfrom(fd_at(to), AT(5, 128), 1, MSG_DONTWAIT, 0, 0) == 1 &&
           user(AT(5, 128))[0] == 'x';
}

/** Write a header at `msg` of the one-byte vector at `iov`, which names
 * `byte`, and of `room` bytes of control buffer at `ctl`. */
static void set_header(uint32_t msg, uint32_t iov, uint32_t byte, uint32_t ctl, uint32_t room)
{
    struct kernel_msghdr m;
    struct kernel_iovec v;

    k_memset(&m, 0, sizeof(m));
    m.msg_iov = iov;
    m.msg_iovlen = 1;
    m.msg_control = ctl;
    m.msg_controllen = room;
    k_memcpy(user(msg), &m, sizeof(m));
    v.iov_base = byte;
    v.iov_len = 1;
    k_memcpy(user(iov), &v, sizeof(v));
}

/** sendmsg(2) over `fd` of one byte and the descriptors `fds[0..n)` with
 * SCM_RIGHTS, from page 6. */
static int32_t send_rights(int fd, const int *fds, uint32_t n)
{
    uint32_t msg = AT(6, 0), iov = AT(6, 64), byte = AT(6, 96), ctl = AT(6, 128);
    struct kernel_cmsghdr c;
    uint32_t i;

    set_header(msg, iov, byte, ctl, CMSG_HDR + 4u * n);
    user(byte)[0] = 'F';
    c.cmsg_len = CMSG_HDR + 4u * n;
    c.cmsg_level = SOL_SOCKET;
    c.cmsg_type = SCM_RIGHTS;
    k_memcpy(user(ctl), &c, sizeof(c));
    for (i = 0; i < n; i++) {
        set_word(ctl + CMSG_HDR + 4u * i, (uint32_t)fds[i]);
    }
    return unix_sendmsg(fd_at(fd), msg, 0);
}

/** recvmsg(2) over `fd` of one byte, with `room` bytes of control buffer at
 * `ctl`, or none for 0; the header is on page 7. */
static int32_t recv_rights(int fd, uint32_t ctl, uint32_t room)
{
    uint32_t msg = AT(7, 0);

    set_header(msg, AT(7, 64), AT(7, 96), room ? ctl : 0u, room);
    return unix_recvmsg(fd_at(fd), msg, 0);
}

/** The flags recvmsg stored in the header on page 7. */
static uint32_t recv_flags(void)
{
    struct kernel_msghdr m;

    k_memcpy(&m, user(AT(7, 0)), sizeof(m));
    return m.msg_flags;
}

/** The start of a row: every page present, no sibling armed, no file freed
 * and no socket file made. */
static void setup(void)
{
    int i;

    k_memset(memory, 0, sizeof(memory));
    for (i = 0; i < PAGES; i++) {
        pages[i] = PAGE_PRESENT;
    }
    for (i = 0; i < FILES; i++) {
        freed[i] = 0;
        seen[i] = 0;
    }
    k_memset(sockfiles, 0, sizeof(sockfiles));
    files_made = 0;
    page_sibling = NULL;
    fs_sibling = NULL;
    release_sibling = NULL;
    lock_sibling = NULL;
    root_depth = 0;
    ran = 0;
    parked = 0;
    parked_on = NULL;
    copy_needs_park = 0;
    reserved[0] = reserved[1] = -1;
    sibling_rc[0] = sibling_rc[1] = 0;
    sigpipes = 0;
    closing_on_get = -1;
    signal_checks = signal_without_big = ignore_signal = check_signal_context = 0;
    signal_unlock = NULL;
    own_wait = NULL;
    own_waits = 0;
    k_memset(own_deadline, 0, sizeof(own_deadline));
    self.wakeup_tick = 0;
    self.restart_kind = RESTART_SYS;
}

/**
 * Resume a sibling still asleep, which only a fault leaves, without a wake,
 * so that its call ends and lets go of what it holds before the row closes
 * the descriptors it uses, as the kernel defers the release of a descriptor
 * until the call using it ends. One that sleeps again cannot be ended, and
 * the test stops.
 */
static void finish_sibling(const char *name)
{
    if (park_waiting() && !park_resume()) {
        dprintf(2, "unix-hosttest: fail: the %s row leaves a sibling asleep\n", name);
        exit(1);
    }
    parked = 0;
}

/** The end of the row `name`: the sibling's call is over, every descriptor
 * is closed, and nothing is left, no socket, pipe, message, receive or
 * socket file held. */
static void end_row(const char *name)
{
    int i;

    finish_sibling(name);
    for (i = 0; i < SLOTS; i++) {
        do_close(i);
        fd_free(&shared, i);
    }
    for (i = 0; i < SOCKFILES; i++) {
        if (sockfiles[i].held) {
            break;
        }
    }
    if (socks || pipes || flights || reap_head || live != 0 || i < SOCKFILES || root_depth != 0) {
        dprintf(2, "unix-hosttest: fail: the %s row leaves something allocated\n", name);
        failures++;
    }
}

/** The sibling's close of the first number the call reserved. */
static void close_reserved(void)
{
    sibling_rc[0] = do_close(reserved[0]);
}

/** The sibling's close of both numbers the call reserved. */
static void close_reserved_pair(void)
{
    sibling_rc[0] = do_close(reserved[0]);
    sibling_rc[1] = do_close(reserved[1]);
}

/** The sibling's close of the first number the call reserved, then an open
 * of a file of its own. */
static void close_and_open(void)
{
    sibling_rc[0] = do_close(reserved[0]);
    sibling_rc[1] = open_orphan(1);
}

/** The sibling's close of `reserved[0]`, noting which files had been freed
 * when it was done. */
static void close_and_look(void)
{
    int i;

    sibling_rc[0] = do_close(reserved[0]);
    for (i = 0; i < FILES; i++) {
        seen[i] = freed[i];
    }
}

/** A listener on 0 and a client on 1, named, whose connect is queued. */
static void queue_connection(void)
{
    uint32_t len;

    check(unix_socket(SOCK_STREAM, 0, 0) == 0 && unix_socket(SOCK_STREAM, 0, 0) == 1,
          "accept: two stream sockets");
    len = abstract_at(AT(1, 0), "listen");
    check(unix_bind(fd_at(0), AT(1, 0), len) == 0 && unix_listen(fd_at(0), 4) == 0,
          "accept: a listener");
    check(unix_bind(fd_at(1), AT(1, 128), abstract_at(AT(1, 128), "client")) == 0 &&
              unix_connect(fd_at(1), AT(1, 0), len) == 0,
          "accept: a named client's connect");
    set_word(AT(0, 0), 64);
}

/**
 * accept(2): the copy of the peer's name sleeps, and a sibling's close of
 * the number accept reserved must find nothing; the connection is published
 * once the name is copied. A copy that fails frees the number and ends the
 * connection, and leaves the file the sibling opened.
 */
static void accept_row(void)
{
    int32_t fd;

    setup();
    queue_connection();
    pages[0] = PAGE_MISSING;
    reserved[0] = 2;
    page_sibling = close_reserved;
    fd = unix_accept(fd_at(0), AT(1, 256), AT(0, 0), 0);
    if (check(ran == 1 && sibling_rc[0] == -EBADF,
              "a sibling's close of the number accept reserved finds nothing while the copy of "
              "the peer's name sleeps") &&
        check(fd == 2 && fd_at(2)->type == FD_TYPE_UNIX && sock_at(2)->peer == sock_at(1),
              "accept publishes the connection once the peer's name is copied")) {
        check(word(AT(0, 0)) == 9 && user(AT(1, 258))[0] == 0 && bytes_are(AT(1, 259), "client"),
              "accept reports the client's name");
        check(crosses(1, 2) && crosses(2, 1), "a byte crosses the accepted connection both ways");
    }
    end_row("accept");

    setup();
    queue_connection();
    pages[0] = PAGE_LOST;
    reserved[0] = 2;
    page_sibling = close_and_open;
    fd = unix_accept(fd_at(0), AT(1, 256), AT(0, 0), 0);
    if (check(ran == 1 && sibling_rc[0] == -EBADF && sibling_rc[1] == 3,
              "a sibling can neither close nor take the number accept reserved")) {
        check(fd == -EFAULT && !fd_at(2)->ofd && fd_at(3)->type == FD_TYPE_FILE &&
                  !sock_at(0)->acc_head && sock_at(1)->shut == SHUT_BOTH,
              "an accept whose copy fails frees the number it reserved and ends the connection, "
              "and leaves the sibling's file");
    }
    end_row("failed accept");
}

/**
 * socketpair(2): the copy of the two numbers sleeps, and a sibling's close
 * of either must find nothing; both ends are published once the numbers
 * are copied. A copy that fails frees both numbers and both sockets, and
 * leaves the file the sibling opened.
 */
static void socketpair_row(void)
{
    int32_t rc;

    setup();
    pages[0] = PAGE_MISSING;
    reserved[0] = 0;
    reserved[1] = 1;
    page_sibling = close_reserved_pair;
    rc = unix_socketpair(SOCK_STREAM, 0, 0, AT(0, 0));
    if (check(ran == 1 && sibling_rc[0] == -EBADF && sibling_rc[1] == -EBADF,
              "a sibling's close of the numbers socketpair reserved finds nothing while their copy "
              "sleeps") &&
        check(rc == 0 && word(AT(0, 0)) == 0 && word(AT(0, 4)) == 1 &&
                  fd_at(0)->type == FD_TYPE_UNIX && fd_at(1)->type == FD_TYPE_UNIX &&
                  sock_at(0)->peer == sock_at(1),
              "socketpair publishes both ends once their numbers are copied")) {
        check(crosses(0, 1) && crosses(1, 0), "a byte crosses the pair both ways");
    }
    end_row("socketpair");

    setup();
    pages[0] = PAGE_LOST;
    reserved[0] = 0;
    page_sibling = close_and_open;
    rc = unix_socketpair(SOCK_STREAM, 0, 0, AT(0, 0));
    if (check(ran == 1 && sibling_rc[0] == -EBADF && sibling_rc[1] == 2,
              "a sibling can neither close nor take a number socketpair reserved")) {
        check(rc == -EFAULT && !fd_at(0)->ofd && !fd_at(1)->ofd && fd_at(2)->type == FD_TYPE_FILE &&
                  !socks,
              "a socketpair whose copy fails frees what it reserved and its sockets, and leaves "
              "the sibling's file");
    }
    end_row("failed socketpair");
}

/** A close begun after a send pins its source descriptor must not mark
 * the received copy closing; close-on-exec also belongs to the source. */
static void rights_close_row(void)
{
    int f;
    uint32_t received;

    setup();
    check(pair(SOCK_STREAM) == 0, "rights close: a stream pair");
    f = open_orphan(0);
    fd_at(f)->flags |= FD_FLAG_CLOEXEC;
    closing_on_get = f;
    check(send_rights(0, &f, 1) == 1 && closing_on_get == -1,
          "rights close: a close begins after the sender pins its source");
    check(recv_rights(1, AT(2, 0), CMSG_HDR + 4u) == 1,
          "rights close: the receive returns a descriptor");
    received = word(AT(2, CMSG_HDR));
    check(received == 3 && fd_get(&shared, (int)received) != NULL &&
              !(fd_at((int)received)->flags &
                (FD_FLAG_CLOEXEC | FD_FLAG_CLOSING | FD_FLAG_RELEASING)),
          "rights close: the received descriptor is open without the source flags");
    fd_at(f)->flags &= ~(uint32_t)FD_FLAG_CLOSING;
    end_row("rights close");
}

/**
 * A receive of SCM_RIGHTS: the copy of the new number sleeps, and a
 * sibling's close of it must find nothing; the descriptor is installed once
 * its number is copied. A copy that fails frees the number, closes the
 * descriptor in flight and sets MSG_CTRUNC, and leaves the file the sibling
 * opened.
 */
static void rights_row(void)
{
    int32_t rc;
    int f;

    setup();
    check(pair(SOCK_STREAM) == 0, "rights: a stream pair");
    f = open_orphan(0);
    check(f == 2 && send_rights(0, &f, 1) == 1, "rights: a descriptor sent with SCM_RIGHTS");
    pages[2] = PAGE_MISSING;
    reserved[0] = 3;
    page_sibling = close_reserved;
    rc = recv_rights(1, AT(2, 0), CMSG_HDR + 4u);
    if (check(ran == 1 && sibling_rc[0] == -EBADF,
              "a sibling's close of the number a receive reserved finds nothing while the copy of "
              "the number sleeps")) {
        check(rc == 1 && word(AT(2, CMSG_HDR)) == 3 && fd_at(3)->type == FD_TYPE_FILE &&
                  fd_at(3)->node == &nodes[0] && fd_at(3)->ofd == fd_at(2)->ofd,
              "a receive installs the descriptor once its number is copied");
    }
    end_row("rights");

    setup();
    check(pair(SOCK_STREAM) == 0, "rights: a stream pair");
    f = open_orphan(0);
    check(f == 2 && send_rights(0, &f, 1) == 1, "rights: a descriptor sent with SCM_RIGHTS");
    pages[2] = PAGE_LOST;
    reserved[0] = 3;
    page_sibling = close_and_open;
    rc = recv_rights(1, AT(2, 0), CMSG_HDR + 4u);
    if (check(ran == 1 && sibling_rc[0] == -EBADF && sibling_rc[1] == 4,
              "a sibling can neither close nor take the number a receive reserved")) {
        check(rc == 1 && (recv_flags() & MSG_CTRUNC) && !fd_at(3)->ofd &&
                  fd_at(4)->type == FD_TYPE_FILE && fd_at(4)->node == &nodes[1] &&
                  fd_at(2)->ofd->refcount == 1,
              "a receive whose copy of the number fails frees the number and the descriptor in "
              "flight, and leaves the sibling's file");
    }
    end_row("failed rights");
}

/**
 * pipe2(2): the copy of the two numbers sleeps, and a sibling's close of
 * either must find nothing; both ends are given once the numbers are
 * copied. A copy that fails frees both numbers and the pipe, and leaves the
 * file the sibling opened.
 */
static void pipe_row(void)
{
    isr_params_t params;
    int32_t rc;

    setup();
    k_memset(&params, 0, sizeof(params));
    params.ebx = AT(0, 0);
    pages[0] = PAGE_MISSING;
    reserved[0] = 0;
    reserved[1] = 1;
    page_sibling = close_reserved_pair;
    rc = sys_pipe2(&params);
    if (check(ran == 1 && sibling_rc[0] == -EBADF && sibling_rc[1] == -EBADF,
              "a sibling's close of the numbers pipe2 reserved finds nothing while their copy "
              "sleeps")) {
        check(rc == 0 && word(AT(0, 0)) == 0 && word(AT(0, 4)) == 1 &&
                  fd_at(0)->type == FD_TYPE_PIPE && fd_at(1)->type == FD_TYPE_PIPE &&
                  fd_at(0)->priv == fd_at(1)->priv && (fd_at(0)->flags & FD_FLAG_READ) &&
                  (fd_at(1)->flags & FD_FLAG_WRITE),
              "pipe2 gives both descriptors their ends once the numbers are copied");
    }
    end_row("pipe2");

    setup();
    k_memset(&params, 0, sizeof(params));
    params.ebx = AT(0, 0);
    pages[0] = PAGE_LOST;
    reserved[0] = 0;
    page_sibling = close_and_open;
    rc = sys_pipe2(&params);
    if (check(ran == 1 && sibling_rc[0] == -EBADF && sibling_rc[1] == 2,
              "a sibling can neither close nor take a number pipe2 reserved")) {
        check(rc == -EFAULT && !fd_at(0)->ofd && !fd_at(1)->ofd && fd_at(2)->type == FD_TYPE_FILE &&
                  !pipes,
              "a pipe2 whose copy fails frees what it reserved and the pipe, and leaves the "
              "sibling's file");
    }
    end_row("failed pipe2");
}

/** The sibling's connect of socket 1 to the listener on 0. */
static void connect_sibling(void)
{
    sibling_rc[0] = unix_connect(fd_at(1), AT(1, 0), name_len);
}

/**
 * connect(2) to an abstract name, whose copy of the address sleeps, and to a
 * path, whose lookup of the socket file sleeps, while a sibling connects the
 * socket. The connect must fail with EISCONN and leave the listener with
 * the sibling's connection alone.
 */
static void connect_row(int by_path)
{
    int32_t rc;

    setup();
    check(unix_socket(SOCK_STREAM, 0, 0) == 0 && unix_socket(SOCK_STREAM, 0, 0) == 1,
          "connect: two stream sockets");
    name_len = by_path ? path_at(AT(1, 0), "/tmp/server") : abstract_at(AT(1, 0), "server");
    check(unix_bind(fd_at(0), AT(1, 0), name_len) == 0 && unix_listen(fd_at(0), 4) == 0,
          "connect: a listener");
    if (by_path) {
        fs_sibling = connect_sibling;
        rc = unix_connect(fd_at(1), AT(1, 0), name_len);
    } else {
        abstract_at(AT(0, 0), "server");
        pages[0] = PAGE_MISSING;
        page_sibling = connect_sibling;
        rc = unix_connect(fd_at(1), AT(0, 0), name_len);
    }
    check(ran == 1 && sibling_rc[0] == 0, "connect: the sibling connects the socket");
    if (check(rc == -EISCONN, by_path ? "a connect whose lookup of the path sleeps while a "
                                        "sibling connects the socket fails with EISCONN"
                                      : "a connect whose copy of the address sleeps while a "
                                        "sibling connects the socket fails with EISCONN") &&
        check(sock_at(0)->acc_len == 1 && sock_at(1)->peer == sock_at(0)->acc_head,
              "the listener has the sibling's connection alone")) {
        check(unix_accept(fd_at(0), 0, 0, 0) == 2 && crosses(1, 2) && crosses(2, 1),
              "a byte crosses the sibling's connection both ways");
    }
    end_row("connect");
}

/** The sibling's bind of socket 0 to the second path. */
static void bind_sibling(void)
{
    sibling_rc[0] = unix_bind(fd_at(0), AT(1, 128), name_len);
}

/**
 * bind(2) to a path: the file system's making of the socket file sleeps,
 * and a sibling's bind of the same socket must wait for the bind lock. The
 * first bind names the socket and wakes the one that waits, which goes on
 * from its wait and fails with EINVAL, so one socket file is made, and the
 * socket's close lets it go.
 */
static void bind_row(void)
{
    struct unix_sock *s;
    uint32_t len;
    int32_t rc;

    setup();
    check(unix_socket(SOCK_STREAM, 0, 0) == 0, "bind: a stream socket");
    s = sock_at(0);
    len = path_at(AT(1, 0), "/tmp/first");
    name_len = path_at(AT(1, 128), "/tmp/second");
    fs_sibling = bind_sibling;
    rc = unix_bind(fd_at(0), AT(1, 0), len);
    check(ran == 1 && parked && parked_on == &s->bindlock.wq,
          "a second bind of a socket waits for the bind lock while the first sleeps in the file "
          "system");
    check(rc == 0 && s->bound && s->bound_fs && s->fs_ref.node == &sockfiles[0] &&
              same(sockfiles[0].path, "/tmp/first"),
          "the first bind names the socket with the file it made");
    if (parked) {
        check(parked_wakes != 0, "the first bind's end wakes the bind that waits");
        check(wake_sibling() && sibling_rc[0] == -EINVAL,
              "the bind that waited goes on from its wait, finds the socket named and fails with "
              "EINVAL");
    }
    check(files_made == 1 && s->fs_ref.node == &sockfiles[0],
          "the two binds make one socket file, which the socket keeps");
    finish_sibling("bind");
    do_close(0);
    check(!sockfiles[0].held && !sockfiles[1].held,
          "the socket's close lets go of every socket file it was given");
    end_row("bind");
}

/** The sibling's nonblocking receive on socket 1. */
static void receive_sibling(void)
{
    sibling_rc[0] = unix_recvfrom(fd_at(1), AT(1, 256), 16, MSG_DONTWAIT, 0, 0);
}

/**
 * A receive whose copy of the first of two messages sleeps: a sibling's
 * receive must wait for the receive lock, the first receive must return its
 * message whole and wake the sibling as it lets the lock go, and the
 * sibling, going on from its wait, must take the second.
 */
static void receive_row(int type, const char *waits)
{
    int32_t rc;

    setup();
    check(pair(type) == 0 && send_text(0, "one") == 3 && send_text(0, "two") == 3,
          "receive: a pair with two messages queued");
    pages[0] = PAGE_MISSING;
    copy_needs_park = 1;
    page_sibling = receive_sibling;
    rc = unix_recvfrom(fd_at(1), AT(0, 0), 3, 0, 0, 0);
    check(ran == 1 && parked && parked_on == &sock_at(1)->iolock.wq, waits);
    check(rc == 3 && bytes_are(AT(0, 0), "one"),
          "the receive whose copy slept returns its message whole");
    if (parked) {
        check(parked_wakes != 0, "the end of a receive's hold of the lock wakes the receive that "
                                 "waits for it");
        check(wake_sibling() && sibling_rc[0] == 3 && bytes_are(AT(1, 256), "two"),
              "the receive that waited goes on from its wait and takes the next message");
    }
    check(!sock_at(1)->rq_head, "receive: both messages are taken");
    end_row("receive");
}

/** The sibling's shutdown of socket 0 for writing. */
static void shutdown_sibling(void)
{
    sibling_rc[0] = unix_shutdown(fd_at(0), 1);
}

/**
 * A send whose copy of the caller's bytes sleeps while a sibling shuts the
 * socket down for writing: nothing may be queued, and the send fails with
 * EPIPE, with SIGPIPE on a stream.
 */
static void send_row(int type, const char *stops)
{
    int32_t rc;

    setup();
    check(pair(type) == 0, "send: a pair");
    set_bytes(AT(0, 0), "xyz");
    pages[0] = PAGE_MISSING;
    page_sibling = shutdown_sibling;
    rc = unix_sendto(fd_at(0), AT(0, 0), 3, 0, 0, 0);
    check(ran == 1 && sibling_rc[0] == 0, "send: the sibling shuts the socket down");
    check(rc == -EPIPE && !sock_at(1)->rq_head && sigpipes == (type == SOCK_STREAM), stops);
    end_row("send");
}

/**
 * A receive's descriptors in flight, off their queue: the holder count
 * finds them until they are installed or closed. A sibling's close of a
 * file's last descriptor while the receive installs the file's descriptor,
 * or closes it with another it has no room for, must not free the file;
 * the release of the descriptor in flight that is its file's last holder
 * frees it; and a descriptor the receive has installed is no longer counted
 * in flight, so the close of it frees its file while the receive installs
 * the next.
 */
static void transit_row(void)
{
    int32_t rc;
    int f[2];

    setup();
    check(pair(SOCK_STREAM) == 0, "transit: a stream pair");
    f[0] = open_orphan(0);
    check(f[0] == 2 && send_rights(0, f, 1) == 1, "transit: a file's descriptor sent");
    pages[2] = PAGE_MISSING;
    reserved[0] = 2;
    page_sibling = close_and_look;
    rc = recv_rights(1, AT(2, 0), CMSG_HDR + 4u);
    check(ran == 1 && sibling_rc[0] == 0 && seen[0] == 0,
          "a file is not freed by the close of its last descriptor while a receive installs its "
          "descriptor in flight");
    check(rc == 1 && fd_at(3)->type == FD_TYPE_FILE && fd_at(3)->node == &nodes[0],
          "transit: the receive installs the descriptor");
    do_close(3);
    check(freed[0] == 1, "transit: the close of the installed descriptor frees the file once");
    end_row("transit while installing");

    setup();
    check(pair(SOCK_STREAM) == 0, "transit: a stream pair");
    f[0] = open_orphan(1);
    f[1] = open_orphan(2);
    check(f[0] == 2 && f[1] == 3 && send_rights(0, f, 2) == 1, "transit: two descriptors sent");
    check(do_close(2) == 0 && freed[1] == 0,
          "a file whose descriptor is queued in flight outlives the close of its last descriptor");
    reserved[0] = 3;
    release_sibling = close_and_look;
    rc = recv_rights(1, 0, 0);
    check(ran == 1 && seen[1] == 1,
          "the release of a descriptor in flight that is its file's last holder frees the file");
    check(ran == 1 && sibling_rc[0] == 0 && seen[2] == 0,
          "a file is not freed by the close of its last descriptor while a receive closes its "
          "descriptor in flight with another");
    check(rc == 1 && (recv_flags() & MSG_CTRUNC) && freed[1] == 1 && freed[2] == 1,
          "a receive with no room for descriptors closes them, and frees each file once");
    end_row("transit while closing");

    setup();
    check(pair(SOCK_STREAM) == 0, "transit: a stream pair");
    f[0] = open_orphan(3);
    f[1] = open_orphan(0);
    check(f[0] == 2 && f[1] == 3 && send_rights(0, f, 2) == 1 && do_close(2) == 0 &&
              do_close(3) == 0,
          "transit: two files whose only holders are in flight");
    pages[3] = PAGE_MISSING;
    reserved[0] = 2;
    page_sibling = close_and_look;
    rc = recv_rights(1, AT(2, PAGE_SIZE - CMSG_HDR - 4u), CMSG_HDR + 8u);
    check(ran == 1 && sibling_rc[0] == 0 && seen[3] == 1,
          "a descriptor a receive has installed is not counted in flight: its close frees the "
          "file while the receive installs the next");
    check(rc == 1 && fd_at(3)->type == FD_TYPE_FILE && fd_at(3)->node == &nodes[0],
          "transit: the receive installs the second descriptor");
    do_close(3);
    check(freed[0] == 1 && freed[3] == 1, "transit: each file is freed once");
    end_row("transit after installing");
}

/** Move the file `nodes[from]` to `nodes[to]`, as a write that grows a
 * KRFFS file can, and repoint what `vfs_repoint` repoints: the descriptors
 * in the table and their descriptions, and not the copies in flight. */
static void move_file(int from, int to)
{
    int i;

    for (i = 0; i < SLOTS; i++) {
        fd_entry_t *e = &slots[i];

        if (e->type == FD_TYPE_FILE && e->node == &nodes[from]) {
            e->node = &nodes[to];
        }
        if (e->ofd && e->ofd->vnode == &nodes[from]) {
            e->ofd->vnode = &nodes[to];
        }
    }
}

/**
 * A descriptor in flight while its file moves: the holder count must find
 * it by the node its description names, so that the close of the file's
 * last descriptor leaves the file; a receive must install it with that
 * node; and one that is never received must release that node when its
 * queue is dropped.
 */
static void moved_row(void)
{
    int f;

    setup();
    check(pair(SOCK_STREAM) == 0, "moved: a stream pair");
    f = open_orphan(0);
    check(f == 2 && send_rights(0, &f, 1) == 1, "moved: a file's descriptor sent");
    move_file(0, 1);
    check(unix_inflight_holds(&nodes[1]) && !unix_inflight_holds(&nodes[0]),
          "a descriptor in flight is counted by the node its file moved to");
    check(do_close(2) == 0 && freed[0] == 0 && freed[1] == 0,
          "moved: the file outlives the close of its last descriptor");
    check(recv_rights(1, AT(2, 0), CMSG_HDR + 4u) == 1 && fd_at(2)->type == FD_TYPE_FILE &&
              fd_at(2)->node == &nodes[1],
          "a descriptor received after its file moved is installed with the node it moved to");
    do_close(2);
    check(freed[0] == 0 && freed[1] == 1, "moved: the close of the installed descriptor frees it");
    end_row("moved and received");

    setup();
    check(pair(SOCK_STREAM) == 0, "moved: a stream pair");
    f = open_orphan(0);
    check(f == 2 && send_rights(0, &f, 1) == 1, "moved: a file's descriptor sent");
    move_file(0, 1);
    do_close(2);
    do_close(1);
    check(freed[0] == 0 && freed[1] == 1,
          "a descriptor in flight never received releases the node its file moved to");
    end_row("moved and dropped");
}

/**
 * A descriptor in flight on a queue that its socket's close drops, whose
 * file's last descriptor in a table a sibling closes while the drop's
 * request for the root's lock waits: the descriptor in flight must still be
 * where the holder count finds it then, so the sibling's close leaves the
 * file, and the drop's own close, under the lock, frees it once.
 */
static void flight_lock_row(void)
{
    int f;

    setup();
    check(pair(SOCK_STREAM) == 0, "flight lock: a stream pair");
    f = open_orphan(0);
    check(f == 2 && send_rights(0, &f, 1) == 1, "flight lock: a file's descriptor sent");
    reserved[0] = 2;
    lock_sibling = close_and_look;
    check(do_close(1) == 0, "flight lock: the close of the socket whose queue has the descriptor");
    check(ran == 1 && sibling_rc[0] == 0 && seen[0] == 0,
          "a descriptor in flight is counted until its drop has the root's lock: a close of the "
          "file's last descriptor meanwhile leaves the file");
    check(freed[0] == 1, "the drop of a descriptor in flight frees its file once");
    end_row("flight lock");
}

/** Write at `vec` two elements of 1 GiB each, whose total passes
 * USER_RW_MAX, the first at `base`, and at `msg` a header that names them.
 * Only the bytes a call moves need to be in the model's pages. */
static void set_big_vector(uint32_t msg, uint32_t vec, uint32_t base)
{
    struct kernel_msghdr m;
    struct kernel_iovec v[2];

    v[0].iov_base = base;
    v[0].iov_len = 0x40000000u;
    v[1].iov_base = USER_BASE + 0x10000000u;
    v[1].iov_len = 0x40000000u;
    k_memcpy(user(vec), v, sizeof(v));
    k_memset(&m, 0, sizeof(m));
    m.msg_iov = vec;
    m.msg_iovlen = 2;
    k_memcpy(user(msg), &m, sizeof(m));
}

/**
 * A vector whose lengths pass MAX_RW_COUNT together, USER_RW_MAX here, is
 * cut there, as Linux's import_iovec cuts it, not refused: the second
 * element keeps what the first leaves, the datagram send then fails as one
 * larger than the send buffer, and the stream receive takes what is queued.
 */
static void vector_row(void)
{
    struct kernel_iovec *iov;
    uint32_t total;

    setup();
    set_big_vector(AT(6, 0), AT(6, 64), AT(6, 256));
    check(get_iov(AT(6, 64), 2, &iov, &total) == 0 && total == USER_RW_MAX &&
              iov[0].iov_len == 0x40000000u && iov[1].iov_len == USER_RW_MAX - 0x40000000u,
          "a vector whose total passes MAX_RW_COUNT is cut there, its last element shortened");
    kfree(iov);
    check(pair(SOCK_DGRAM) == 0, "vector: a datagram pair");
    check(unix_sendmsg(fd_at(0), AT(6, 0), MSG_DONTWAIT) == -EMSGSIZE,
          "a datagram sendmsg of a vector past MAX_RW_COUNT is too large for the send buffer, "
          "not EINVAL");
    end_row("vector send");

    setup();
    check(pair(SOCK_STREAM) == 0, "vector: a stream pair");
    check(send_text(1, "abc") == 3, "vector: three bytes sent");
    set_big_vector(AT(6, 0), AT(6, 64), AT(6, 256));
    check(unix_recvmsg(fd_at(0), AT(6, 0), MSG_DONTWAIT) == 3 && user(AT(6, 256))[0] == 'a' &&
              user(AT(6, 256))[2] == 'c',
          "a stream recvmsg into a vector past MAX_RW_COUNT takes the bytes queued, not EINVAL");
    end_row("vector receive");
}

/** Queue another stream write while the tested vector's copy sleeps. */
static void vector_write_sibling(void)
{
    sibling_rc[0] = send_text(0, "XY");
}

/** A vector is one datagram, and a stream vector whose second element
 * faults commits no first element before another writer's whole message. */
static void vector_write_row(void)
{
    struct kernel_iovec iov[2];

    setup();
    check(pair(SOCK_DGRAM) == 0, "vector write: a datagram pair");
    k_memcpy(user(AT(1, 0)), "ab", 2);
    k_memcpy(user(AT(2, 0)), "cd", 2);
    iov[0].iov_base = AT(1, 0);
    iov[0].iov_len = 2;
    iov[1].iov_base = AT(2, 0);
    iov[1].iov_len = 2;
    check(unix_writev(fd_at(0), iov, 2, 4) == 4, "vector write: both elements are sent");
    check(unix_recvfrom(fd_at(1), AT(0, 0), 4, MSG_DONTWAIT, 0, 0) == 4 &&
              bytes_are(AT(0, 0), "abcd"),
          "a datagram vector arrives as one message");
    check(unix_recvfrom(fd_at(1), AT(0, 0), 4, MSG_DONTWAIT, 0, 0) == -EAGAIN,
          "a datagram vector leaves no second message");
    end_row("datagram vector write");

    setup();
    check(pair(SOCK_STREAM) == 0, "vector write: a stream pair");
    k_memcpy(user(AT(1, 0)), "ab", 2);
    k_memcpy(user(AT(2, 0)), "cd", 2);
    pages[2] = PAGE_MISSING;
    page_sibling = vector_write_sibling;
    check(unix_writev(fd_at(0), iov, 2, 4) == 4 && ran == 1 && sibling_rc[0] == 2,
          "a second writer completes while a vector's second element faults");
    check(unix_recvfrom(fd_at(1), AT(0, 0), 6, MSG_DONTWAIT, 0, 0) == 6 &&
              bytes_are(AT(0, 0), "XYabcd"),
          "no element of a stream piece is queued before its whole copy");
    end_row("stream vector write");
}

/** `m` itself: no row makes a bind mount, so a mount is its own base. */
const struct vfs_mount *vfs_mount_base(const struct vfs_mount *m)
{
    return m;
}

/** Take a spinlock; a recursive take cannot make progress in this model. */
void spin_lock(struct spinlock *lock)
{
    if (lock->locked) {
        k_panic("recursive spinlock");
    }
    lock->locked = 1;
}

/** Take the second state lock; the caller orders sockets by address. */
void spin_lock_nested(struct spinlock *lock, unsigned int subclass)
{
    (void)subclass;
    spin_lock(lock);
}

/** Give back a held spinlock. */
void spin_unlock(struct spinlock *lock)
{
    if (!lock->locked) {
        k_panic("free spinlock");
    }
    lock->locked = 0;
}

/** Take `lock` with interrupts off: one thread, so nothing waits. */
uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    spin_lock(lock);
    return 0;
}

/** Give `lock` back. */
void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    (void)flags;
    spin_unlock(lock);
}

/** Give back the model's big-lock depth. */
int bkl_drop(void)
{
    int depth = model_big;

    model_big = 0;
    return depth;
}

/** Restore the model's inherited depth. */
void bkl_take(int depth)
{
    model_big += depth;
}

/** Whether the model holds the big lock. */
int bkl_held(void)
{
    return model_big != 0;
}

/** Take the model's big lock. */
void bkl_enter(void)
{
    model_big++;
}

/** Give back one model take. */
void bkl_exit(void)
{
    model_big--;
}

/** A contended interruptible mutex checks signals only under the big
 * lock, returning EINTR for a real signal and retrying an ignored one. */
static void mutex_signal_row(void)
{
    struct mutex m;

    setup();
    mutex_init(&m, "signal row");
    mutex_lock(&m);
    check_signal_context = 1;
    check(mutex_lock_interruptible(&m) == -EINTR && signal_checks == 1 && signal_without_big == 0 &&
              model_big == 0,
          "mutex signal: a pending signal is judged under the big lock");
    mutex_unlock(&m);
    end_row("mutex signal");

    setup();
    mutex_init(&m, "ignored row");
    mutex_lock(&m);
    check_signal_context = ignore_signal = 1;
    signal_unlock = &m;
    check(mutex_lock_interruptible(&m) == 0 && signal_checks == 1 && signal_without_big == 0 &&
              model_big == 0,
          "mutex signal: an ignored signal retries and takes the free mutex");
    mutex_unlock(&m);
    end_row("ignored mutex signal");
}

/** Set the time limit `name` of `fd` to `usec` microseconds, 0 for none,
 * from page 5: 0, or `unix_setsockopt`'s error. */
static int32_t set_timeo(int fd, int name, long usec)
{
    long tv[2];

    tv[0] = usec / 1000000;
    tv[1] = usec % 1000000;
    k_memcpy(user(AT(5, 256)), tv, sizeof(tv));
    return unix_setsockopt(fd_at(fd), SOL_SOCKET, name, AT(5, 256), sizeof(tv));
}

/** Set SO_RCVTIMEO of `fd` to `usec` microseconds, 0 for none. */
static int32_t set_rcvtimeo(int fd, long usec)
{
    return set_timeo(fd, SO_RCVTIMEO_OLD, usec);
}

/** A signal ends the wait: the clock moves 10 ticks first. */
static int signal_wait(void)
{
    kernel_ticks += 10;
    return 1;
}

/** Each wait ends at its deadline, with nothing come. */
static int wait_out(void)
{
    kernel_ticks = current_proc->wakeup_tick;
    return 0;
}

/** The sibling's SO_RCVTIMEO of 400 ms on socket 0, while the receive's
 * copy sleeps. */
static void rcvtimeo_sibling(void)
{
    sibling_rc[0] = set_rcvtimeo(0, 400000);
}

/** The sibling's SO_SNDTIMEO taken off socket 1, while the send's lookup
 * of the path sleeps. */
static void sndtimeo_sibling(void)
{
    sibling_rc[0] = set_timeo(1, SO_SNDTIMEO_OLD, 0);
}

/** A wait ends at its deadline; one with none ends with a signal, since
 * nothing in the row would wake it. */
static int wait_out_or_signal(void)
{
    return current_proc->wakeup_tick == 0 ? 1 : wait_out();
}

/** A page fault that takes 250 ticks. */
static void slow_fault(void)
{
    kernel_ticks += 250;
}

/** The first wait: a byte comes on socket 0 after 50 ticks. The second
 * ends at its deadline. */
static int byte_then_out(void)
{
    if (own_waits == 1) {
        kernel_ticks += 50;
        return send_text(1, "a") != 1;
    }
    return wait_out();
}

/** The first wait: after 30 ticks a signal's wake clears the deadline,
 * and a sibling thread takes the signal before this one looks. The second
 * ends at its deadline, or with a signal when it has none. */
static int sibling_takes_signal(void)
{
    if (own_waits == 1) {
        kernel_ticks += 30;
        current_proc->wakeup_tick = 0;
        ignore_signal = 1;
        return 1;
    }
    if (current_proc->wakeup_tick == 0) {
        return 1;
    }
    return wait_out();
}

/** Each wait: a byte comes on socket 0 after 50 ticks. */
static int byte_each_50(void)
{
    kernel_ticks += 50;
    return send_text(1, "w") != 1;
}

/**
 * A signal in a socket's wait, as Linux 6.12 ends it: under SO_RCVTIMEO the
 * call is EINTR whatever SA_RESTART says, and with no limit the dispatcher
 * may restart it (sock_intr_errno, include/net/sock.h:2542-2545); a signal
 * in the wait for the bind lock is EINTR, as mutex_lock_interruptible's
 * is in Linux (net/unix/af_unix.c:1201).
 */
static void timeo_signal_row(void)
{
    struct unix_sock *s;
    uint32_t len;
    int32_t rc;

    setup();
    check(pair(SOCK_STREAM) == 0 && set_rcvtimeo(0, 100000) == 0, "timeo: a stream pair");
    own_wait = signal_wait;
    rc = unix_recvfrom(fd_at(0), AT(3, 0), 1, 0, 0, 0);
    check(rc == -EINTR && own_waits == 1 && current_proc->restart_kind == RESTART_NONE,
          "a signal in a receive's wait under SO_RCVTIMEO is EINTR, never restarted");
    current_proc->restart_kind = RESTART_SYS;
    check(set_rcvtimeo(0, 0) == 0, "timeo: the limit taken off");
    rc = unix_recvfrom(fd_at(0), AT(3, 0), 1, 0, 0, 0);
    check(rc == -EINTR && own_waits == 2 && current_proc->restart_kind == RESTART_SYS,
          "a signal in a receive's wait with no limit leaves the dispatcher to restart it");
    end_row("timeo signal");

    setup();
    check(unix_socket(SOCK_STREAM, 0, 0) == 0, "timeo: a stream socket");
    s = sock_at(0);
    mutex_lock(&s->bindlock);
    len = path_at(AT(1, 0), "/tmp/held");
    rc = unix_bind(fd_at(0), AT(1, 0), len);
    check(rc == -EINTR && current_proc->restart_kind == RESTART_NONE,
          "a signal in the wait for the bind lock is EINTR, never restarted");
    mutex_unlock(&s->bindlock);
    end_row("timeo bind lock");
}

/**
 * The time limit of SO_RCVTIMEO, as Linux 6.12 keeps it (net/unix/af_unix.c):
 * the limit is the one set when the call began, not one set while its copy
 * slept (2785); a wait after a slow copy has what the earlier waits left,
 * the copy costing nothing (2841); a wait woken by a signal that a sibling
 * thread took sleeps on to the same deadline; and a MSG_WAITALL receive
 * whose bytes come a quarter of its limit apart ends at the limit with what
 * it has. Each wait's deadline is read in `own_deadline`.
 */
static void timeo_limit_row(void)
{
    uint64_t t0;
    int32_t rc;

    setup();
    check(pair(SOCK_STREAM) == 0 && set_rcvtimeo(0, 100000) == 0 && send_text(1, "a") == 1,
          "timeo: a stream pair with a byte queued");
    pages[3] = PAGE_MISSING;
    page_sibling = rcvtimeo_sibling;
    own_wait = wait_out;
    t0 = kernel_ticks;
    rc = unix_recvfrom(fd_at(0), AT(3, 0), 2, MSG_WAITALL, 0, 0);
    check(rc == 1 && ran == 1 && sibling_rc[0] == 0 && own_waits == 1 &&
              own_deadline[0] == t0 + 101u,
          "a receive keeps the SO_RCVTIMEO it began with when another is set while its copy "
          "sleeps");
    end_row("timeo taken at the start");

    setup();
    check(pair(SOCK_STREAM) == 0 && set_rcvtimeo(0, 200000) == 0, "timeo: a stream pair");
    pages[3] = PAGE_MISSING;
    page_sibling = slow_fault;
    own_wait = byte_then_out;
    t0 = kernel_ticks;
    rc = unix_recvfrom(fd_at(0), AT(3, 0), 2, MSG_WAITALL, 0, 0);
    check(rc == 1 && ran == 1 && own_waits == 2 && own_deadline[0] == t0 + 201u &&
              own_deadline[1] == t0 + 300u + 151u,
          "a wait after a copy that sleeps 250 ticks has the 150 the first wait left");
    end_row("timeo spent asleep");

    setup();
    check(pair(SOCK_STREAM) == 0 && set_rcvtimeo(0, 100000) == 0, "timeo: a stream pair");
    own_wait = sibling_takes_signal;
    t0 = kernel_ticks;
    rc = unix_recvfrom(fd_at(0), AT(3, 0), 1, 0, 0, 0);
    check(own_waits == 2 && own_deadline[0] == t0 + 101u && own_deadline[1] == own_deadline[0],
          "a wait woken by a signal a sibling took sleeps on to the deadline it had");
    check(rc == -EAGAIN, "a receive whose waking signal a sibling took ends with EAGAIN");
    end_row("timeo sibling signal");

    setup();
    check(pair(SOCK_STREAM) == 0 && set_rcvtimeo(0, 200000) == 0, "timeo: a stream pair");
    own_wait = byte_each_50;
    t0 = kernel_ticks;
    rc = unix_recvfrom(fd_at(0), AT(3, 0), 8, MSG_WAITALL, 0, 0);
    check(own_waits == 4 && own_deadline[0] == t0 + 201u && own_deadline[1] == t0 + 201u &&
              own_deadline[2] == t0 + 201u && own_deadline[3] == t0 + 201u,
          "each wait of a MSG_WAITALL receive has what the waits before it left");
    check(rc == 4 && bytes_are(AT(3, 0), "wwww"),
          "a MSG_WAITALL receive whose bytes come 50 ticks apart ends at its 200-tick limit "
          "with four");
    end_row("timeo waitall");
}

/**
 * The time limit of SO_SNDTIMEO on a send to a full queue named by a path,
 * as Linux 6.12 takes it (net/unix/af_unix.c:2063-2075): after the copy of
 * the message and before the lookup of the path, so that a sibling's
 * clearing of the limit while the lookup sleeps leaves the send its 100 ms,
 * and the send ends with EAGAIN at that deadline instead of waiting for
 * ever.
 */
static void sndtimeo_lookup_row(void)
{
    uint64_t t0;
    uint32_t len;
    int32_t rc;
    int n;

    setup();
    check(unix_socket(SOCK_DGRAM, 0, 0) == 0 && unix_socket(SOCK_DGRAM, 0, 0) == 1,
          "timeo: two datagram sockets");
    len = path_at(AT(1, 0), "/tmp/sink");
    check(unix_bind(fd_at(0), AT(1, 0), len) == 0 && set_timeo(1, SO_SNDTIMEO_OLD, 100000) == 0,
          "timeo: a socket named by a path and a sender with SO_SNDTIMEO");
    set_bytes(AT(2, 0), "d");
    for (n = 0; n < 1024 && unix_sendto(fd_at(1), AT(2, 0), 1, MSG_DONTWAIT, AT(1, 0), len) == 1;
         n++) {
    }
    check(n > 0 && queue_full(sock_at(0)), "timeo: the named socket's queue is full");
    fs_sibling = sndtimeo_sibling;
    own_wait = wait_out_or_signal;
    t0 = kernel_ticks;
    rc = unix_sendto(fd_at(1), AT(2, 0), 1, 0, AT(1, 0), len);
    check(rc == -EAGAIN && ran == 1 && sibling_rc[0] == 0 && own_waits == 1 &&
              own_deadline[0] == t0 + 101u,
          "a named datagram send keeps the SO_SNDTIMEO it had before its lookup of the path when "
          "the limit is taken off while the lookup sleeps");
    end_row("timeo taken before the lookup");
}

/** The rows of the time limits and signals of a socket call's waits. */
static void timeo_row(void)
{
    timeo_signal_row();
    timeo_limit_row();
    sndtimeo_lookup_row();
}

/** Set up the process and its shared table, and run every row; 0 when
 * each passes, 1 otherwise. */
int main(int argc, char **argv)
{
    unix_init();
    pipe_init_pool();
    shared.chunks[0] = slots;
    shared.cap = SLOTS;
    shared.refs = 2;
    dirs.umask = 022;
    self.fs = &dirs;
    self.fds = &shared;
    self.mm = &model_mm;
    self.tgid = 100;
    self.ruid = self.euid = self.suid = 1000;
    self.rgid = self.egid = self.sgid = 1000;
    if (argc == 2 && same(argv[1], "vector-write")) {
        vector_write_row();
        dprintf(1, "unix-hosttest: result: vector-write: %d failures\n", failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && same(argv[1], "rights-close")) {
        rights_close_row();
        dprintf(1, "unix-hosttest: result: rights-close: %d failures\n", failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && same(argv[1], "timeo")) {
        timeo_row();
        dprintf(1, "unix-hosttest: result: timeo: %d failures\n", failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && same(argv[1], "mutex-signal")) {
        mutex_signal_row();
        dprintf(1, "unix-hosttest: result: mutex-signal: %d failures\n", failures);
        return failures ? 1 : 0;
    }
    accept_row();
    socketpair_row();
    rights_row();
    rights_close_row();
    pipe_row();
    connect_row(0);
    connect_row(1);
    bind_row();
    receive_row(SOCK_DGRAM, "a datagram receive while another's copy of a message sleeps waits "
                            "for the receive lock");
    receive_row(SOCK_STREAM, "a stream receive while another's copy of a message sleeps waits "
                             "for the receive lock");
    send_row(SOCK_STREAM, "a stream send whose copy sleeps while the socket is shut down for "
                          "writing queues nothing and fails with EPIPE");
    send_row(SOCK_DGRAM, "a datagram send whose copy sleeps while the socket is shut down for "
                         "writing queues nothing and fails with EPIPE");
    transit_row();
    moved_row();
    flight_lock_row();
    vector_row();
    vector_write_row();
    mutex_signal_row();
    timeo_row();
    dprintf(1, "unix-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
