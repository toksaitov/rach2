/**
 * A kernel stack taken from a heap whose pages held a finished process's
 * code, with a guard page below it.
 *
 * tests/host/kstack_mutate.sh extracts `kstack_alloc`, `kstack_free` and
 * `kstack_overrun_pid` unchanged from a copy of rach2/sys/syscall_proc.c
 * into kstack.inc, with the two sizes they use. The heap here fills every
 * page it hands out with 0xC3, an x86 `ret`, which stands for the code a
 * frame last held. Fork writes only the top of a new stack, so a byte left
 * below it stays there for the life of the process; under QEMU a frame that
 * holds code bytes takes the emulator's slow store path. The stack must come
 * back clear, every byte of it.
 *
 * The heap gives pages with a guard below them (`kalloc_guarded`) and
 * without (`kalloc_pages`), and records which a stack came from and went
 * back to: a stack must be a guarded run, since the guard is what turns an
 * overrun into a fault. `kstack_overrun_pid` must name the running thread
 * for an address in the page below its stack and for no other. The boot
 * stack is the kernel image's, and `kstack_free` must give nothing back for
 * its top, which a record reaches when a thread of a program on the boot
 * stack calls execve and the leader's record goes; the heap here frees only
 * what it gave.
 *
 * The fixture is hosted C: `uintptr_t` is the host's, wide enough for the
 * host pointer `kstack_alloc` returns as an integer. The addresses the
 * overrun rows use are the kernel's, as numbers; nothing is read there.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The two fields of the kernel's process record the extracted code reads. */
typedef struct process {
    uint32_t pid;
    uintptr_t kernel_stack;
} process_t;

process_t *current_proc;

/* The boot stack, as loader.s reserves it; defined below, once the size is
 * known. */
extern char stack[];

/* External, so that a fault which removes the clearing leaves no unused
 * static function for the compiler to warn about. */
void *kalloc_pages(uint32_t count, uintptr_t *phys);
void kfree_pages(void *pages, uint32_t count);
void *kalloc_guarded(uint32_t count);
void kfree_guarded(void *pages, uint32_t count);
void *k_memset(void *dst, int c, size_t n);
int kstack_overrun_pid(uintptr_t addr);

#include "kstack.inc"

char stack[KERNEL_STACK_SIZE];

static int fails, heap_empty;
/* The page counts of the last plain and guarded allocations and guarded
 * free, the pages that free named, and the pages the heap gave last. */
static uint32_t plain_asked, guarded_asked, guarded_freed;
static void *guarded_freed_at, *last_given;

/** Count and print a failed row named `what`. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("kstack-proof: fail: %s\n", what);
    }
}

/** `count` pages filled with a code byte, or NULL once a row empties the
 * heap. */
static void *code_pages(uint32_t count)
{
    void *pages;

    if (heap_empty) {
        return NULL;
    }
    pages = malloc((size_t)count * PAGE_SIZE);
    if (pages) {
        memset(pages, 0xC3, (size_t)count * PAGE_SIZE);
    }
    last_given = pages;
    return pages;
}

/** The heap's pages without a guard: code pages, the count recorded. */
void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    if (phys) {
        *phys = 0;
    }
    plain_asked = count;
    return code_pages(count);
}

/** Free pages `kalloc_pages` gave. */
void kfree_pages(void *pages, uint32_t count)
{
    (void)count;
    free(pages);
}

/** The heap's pages with a guard below them: code pages, the count
 * recorded. */
void *kalloc_guarded(uint32_t count)
{
    guarded_asked = count;
    return code_pages(count);
}

/** Record a free whatever it names, and free only the pages the heap gave
 * last, as the kernel's logs and ignores pages it did not give. */
void kfree_guarded(void *pages, uint32_t count)
{
    guarded_freed_at = pages;
    guarded_freed = count;
    if (pages == last_given) {
        free(pages);
        last_given = NULL;
    }
}

/** The kernel's fill, for the extracted code: every byte of `n` set. */
void *k_memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;

    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

/** How many bytes of the stack at `base` are not zero. */
static size_t dirty_bytes(uintptr_t base)
{
    const unsigned char *bytes = (const unsigned char *)base;
    size_t i, dirty = 0;

    for (i = 0; i < KERNEL_STACK_SIZE; i++) {
        dirty += bytes[i] != 0;
    }
    return dirty;
}

/** A thread whose stack's top is 0xC0106000: the stack is the four pages
 * from 0xC0102000 and its guard the page from 0xC0101000. */
static void overrun_rows(void)
{
    process_t thread = {9, 0xC0106000u};
    process_t unstarted = {10, 0};

    current_proc = &thread;
    check(kstack_overrun_pid(0xC0101000u) == 9 && kstack_overrun_pid(0xC0101FFCu) == 9,
          "an address in the guard page below the running thread's stack gives its pid");
    check(kstack_overrun_pid(0xC0102000u) == -1 && kstack_overrun_pid(0xC0105FFCu) == -1,
          "an address in the stack itself gives none");
    check(kstack_overrun_pid(0xC0100FFFu) == -1 && kstack_overrun_pid(0xC0106000u) == -1,
          "an address below the guard or above the stack gives none");
    current_proc = &unstarted;
    check(kstack_overrun_pid((uintptr_t)0 - KERNEL_STACK_SIZE - PAGE_SIZE) == -1,
          "a thread with no stack gives none, even at the guard its zero top would wrap to");
    current_proc = NULL;
    check(kstack_overrun_pid(0xC0101000u) == -1, "no running thread gives none");
}

/** Run the rows: 0 when every row passes, 1 otherwise. */
int main(void)
{
    uintptr_t base = kstack_alloc();

    check(base != 0 && guarded_asked == KERNEL_STACK_SIZE / PAGE_SIZE && plain_asked == 0,
          "a kernel stack is four pages from the heap with a guard page below them");
    check(base != 0 && dirty_bytes(base) == 0,
          "every byte of a new kernel stack is zero, though its pages held code");
    if (base != 0) {
        kstack_free(base + KERNEL_STACK_SIZE);
        check(guarded_freed_at == (void *)base && guarded_freed == KERNEL_STACK_SIZE / PAGE_SIZE,
              "the stack's top gives the same four pages and their guard back");
    }
    guarded_freed_at = NULL;
    guarded_freed = 0;
    kstack_free((uintptr_t)stack + KERNEL_STACK_SIZE);
    check(guarded_freed_at == NULL && guarded_freed == 0,
          "the boot stack's top gives nothing back");
    heap_empty = 1;
    check(kstack_alloc() == 0, "a heap with no pages gives no stack");
    overrun_rows();
    if (fails) {
        printf("kstack-proof: result: fail: %d rows\n", fails);
        return 1;
    }
    printf("kstack-proof: result: pass: a new kernel stack is cleared and guarded\n");
    return 0;
}
