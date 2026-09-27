/**
 * Check what `proc/futex.c` reads of a thread that may end while it works.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * The source is included whole. User memory is a byte array, and a user
 * address is an offset into it. A copy out to user memory may sleep while
 * its page is brought in, and the model's copy can end the thread that
 * `get_robust_list` names and free its record then, as another process's
 * wait may: the head that thread's list had must still be the one reported,
 * which AddressSanitizer checks by the freed record.
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
static int masked, failures;

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

int vm_prefault_range(uint32_t addr, uint32_t len, int write)
{
    (void)write;
    return in_user(addr, len);
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

/** The word at user address `addr`. */
static uint32_t word_at(uint32_t addr)
{
    uint32_t w;

    k_memcpy(&w, user + addr, sizeof(w));
    return w;
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
    if (failures) {
        return 1;
    }
    printf("futex-test: pass: the robust head\n");
    return 0;
}
