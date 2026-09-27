/**
 * Check what `proc/futex.c` reads of a thread that may end while it works,
 * and the robust futexes a thread's end releases.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * The source is included whole, to reach its lists of waiters. User memory
 * is a byte array, and a user address is an offset into it. A copy out to
 * user memory may sleep while its page is brought in, and the model's copy
 * can end the thread that `get_robust_list` names and free its record then,
 * as another process's wait may: the head that thread's list had must still
 * be the one reported, which AddressSanitizer checks by the freed record.
 *
 * The model's fault for a write, which `copy_to_user` takes first, can stand
 * for a sleep in which a waiter runs: with interrupts on, it sets
 * FUTEX_WAITERS in the word a row names, as that waiter would before it
 * sleeps. A thread that ends holding that robust futex must leave the word
 * with FUTEX_WAITERS and FUTEX_OWNER_DIED, and wake the waiter.
 */
#include "proc/futex.c"

extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);
extern int printf(const char *format, ...);

static process_t self;
process_t *current_proc = &self;
volatile uint32_t kernel_ticks;
volatile uint32_t kernel_uptime_seconds;
volatile uint32_t kernel_second_ticks;
static uint8_t user[1024];
static process_t *named;
static process_t *ends_at_copy;
static uint32_t waiters_at;
static struct futex_q waiter;
static int masked, failures, wakes;

#define CHECK(c, detail)                              \
    do {                                              \
        if (!(c)) {                                   \
            printf("futex-test: fail: %s\n", detail); \
            failures++;                               \
        }                                             \
    } while (0)

/** Whether [addr, addr + len) lies in the model's user memory. */
static int in_user(uint32_t addr, uint32_t len)
{
    return addr != 0 && addr <= sizeof(user) && len <= sizeof(user) - addr;
}

/** The word at user address `addr`. */
static uint32_t word_at(uint32_t addr)
{
    uint32_t w;

    k_memcpy(&w, user + addr, sizeof(w));
    return w;
}

/** Store `w` at user address `addr`. */
static void word_set(uint32_t addr, uint32_t w)
{
    k_memcpy(user + addr, &w, sizeof(w));
}

/** Bring a range in; a fault for a write taken with interrupts on is a
 * sleep in which the waiter on the word at `waiters_at` sets its bit. */
int vm_prefault_range(uint32_t addr, uint32_t len, int write)
{
    if (!in_user(addr, len)) {
        return 0;
    }
    if (write && !masked && waiters_at != 0 && waiters_at == addr) {
        word_set(addr, word_at(addr) | FUTEX_WAITERS);
        waiters_at = 0;
    }
    return 1;
}

int copy_from_user(void *dst, const void __user *src, uint32_t n)
{
    uint32_t at = (uint32_t)(unsigned long)src;

    if (!vm_prefault_range(at, n, 0)) {
        return -EFAULT;
    }
    k_memcpy(dst, user + at, n);
    return 0;
}

/** Copy out, ending the thread `ends_at_copy` first, once, as a sleep for
 * the page could let it end and be reaped. */
int copy_to_user(void __user *dst, const void *src, uint32_t n)
{
    uint32_t at = (uint32_t)(unsigned long)dst;

    if (ends_at_copy) {
        if (named == ends_at_copy) {
            named = NULL;
        }
        free(ends_at_copy);
        ends_at_copy = NULL;
    }
    if (!vm_prefault_range(at, n, 1)) {
        return -EFAULT;
    }
    k_memcpy(user + at, src, n);
    return 0;
}

uint32_t kirq_save(void)
{
    uint32_t prior = (uint32_t)masked;

    masked = 1;
    return prior;
}

void kirq_restore(uint32_t flags)
{
    masked = (int)flags;
}

/** The one thread besides the caller, pid 5, while it has not ended. */
process_t *get_process(uint32_t pid)
{
    return pid == 5 ? named : NULL;
}

int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    return 0;
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
    wakes++;
}

void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 100;
    *nanoseconds = 0;
}

vma_t *vm_find(vm_space_t *vm, uint32_t addr)
{
    (void)vm;
    (void)addr;
    return NULL;
}

uintptr_t paging_user_phys(struct page_dir *dir, uint32_t addr)
{
    (void)dir;
    (void)addr;
    return 0;
}

/** File the model's waiter on the word at `uaddr`, as a sleeping FUTEX_WAIT
 * files itself; the caller's address space names it. */
static void waiter_file(uint32_t uaddr)
{
    uint32_t bucket = futex_bucket(self.page_dir, uaddr);

    k_memset(&waiter, 0, sizeof(waiter));
    waiter.space = self.page_dir;
    waiter.uaddr = uaddr;
    waiter.bitset = FUTEX_MATCH_ANY;
    waiter.next = _waiters[bucket];
    _waiters[bucket] = &waiter;
    wakes = 0;
}

/** Take the model's waiter off its list when no wake did. */
static void waiter_unfile(void)
{
    futex_unlink(&waiter, futex_bucket(waiter.space, waiter.uaddr));
}

/**
 * Give the caller a robust list at user address 64: its head names the
 * entry at 256 as the list's one when `listed`, and the entry at 128 as the
 * one being taken or given back when `pending`, with `pending_bits` in its
 * low bit. A futex word is 16 bytes past its entry.
 */
static void robust_list_set(int listed, int pending, uint32_t pending_bits)
{
    word_set(64, listed ? 256u : 64u);
    word_set(68, 16u);
    word_set(72, pending ? 128u | pending_bits : 0u);
    word_set(256, 64u);
    self.robust_list = 64;
    self.robust_len = 12;
}

/**
 * A thread that ends holding a robust futex on its list, while a waiter
 * sets FUTEX_WAITERS in the sleep the fault for the write stands for: the
 * word ends with the waiters bit and FUTEX_OWNER_DIED, and the waiter is
 * woken, and the list is forgotten.
 */
static void owner_released(void)
{
    robust_list_set(1, 0, 0);
    word_set(272, 42u);
    waiters_at = 272;
    waiter_file(272);
    proc_thread_gone(&self);
    CHECK(word_at(272) == (FUTEX_WAITERS | FUTEX_OWNER_DIED),
          "the word keeps the waiters bit a waiter set during the fault");
    CHECK(waiter.woken && wakes == 1, "and the waiter is woken");
    CHECK(self.robust_list == 0, "and the thread's list is forgotten");
    if (!waiter.woken) {
        waiter_unfile();
    }
    waiters_at = 0;
}

/**
 * The futex a thread was taking or giving back when it ended, the head's
 * pending entry: with no owner in its word, a wake is owed and one waiter
 * is woken with the word left 0; not when it is a priority-inheritance
 * futex, and not when another thread owns it. One the thread owns is
 * released as a listed one is.
 */
static void pending_released(void)
{
    static const struct {
        uint32_t bits;
        uint32_t word;
        uint32_t after;
        int woken;
        const char *what;
    } rows[] = {
        {0u, 0u, 0u, 1, "an unlock the thread stored and did not wake wakes a waiter"},
        {1u, 0u, 0u, 0, "an ownerless priority-inheritance futex wakes no one"},
        {0u, 77u | FUTEX_WAITERS, 77u | FUTEX_WAITERS, 0, "another thread's futex is left"},
        {0u, 42u | FUTEX_WAITERS, FUTEX_WAITERS | FUTEX_OWNER_DIED, 1,
         "a futex the thread holds is released"},
    };
    unsigned int i;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        robust_list_set(0, 1, rows[i].bits);
        word_set(144, rows[i].word);
        waiter_file(144);
        proc_thread_gone(&self);
        CHECK(waiter.woken == rows[i].woken && wakes == rows[i].woken, rows[i].what);
        CHECK(word_at(144) == rows[i].after, "and the word is as it should be after");
        if (!waiter.woken) {
            waiter_unfile();
        }
    }
}

/**
 * get_robust_list reads the head of thread 5's list before it copies
 * anything out: the first copy ends the thread and frees its record, and the
 * head and the length must still arrive.
 */
static void robust_head_read(void)
{
    isr_params_t p;
    process_t *t = calloc(1, sizeof(*t));

    if (!t) {
        CHECK(0, "the model has a record for thread 5");
        return;
    }
    t->pid = t->tgid = 5;
    t->state = PROC_RUNNING;
    t->robust_list = 0x40;
    named = t;
    ends_at_copy = t;
    k_memset(&p, 0, sizeof(p));
    p.ebx = 5;
    p.ecx = 512;
    p.edx = 520;
    CHECK(sys_get_robust_list(&p) == 0, "get_robust_list of another thread succeeds");
    CHECK(word_at(512) == 0x40, "the head reported is the one the thread had");
    CHECK(word_at(520) == 12, "and the length is 12");
    CHECK(named == NULL, "the thread ended during the copy");
    if (ends_at_copy) {
        free(ends_at_copy);
        ends_at_copy = NULL;
    }
}

int main(void)
{
    self.pid = self.tgid = 42;
    self.state = PROC_RUNNING;
    robust_head_read();
    owner_released();
    pending_released();
    if (failures) {
        return 1;
    }
    printf("futex-test: pass: the robust head and the robust release\n");
    return 0;
}
