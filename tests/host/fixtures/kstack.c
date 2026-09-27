/**
 * A kernel stack taken from a heap whose pages held a finished process's
 * code (entry 26 of milestone 50), with a guard page below it (entry 248).
 *
 * tests/host/kstack_mutate.sh extracts `kstack_alloc`, `kstack_free` and
 * `kstack_overrun_pid` unchanged from a copy of rach2/sys/syscall_proc.c
 * into kstack.inc, with the two sizes they use. The heap here fills every
 * page it hands out with 0xC3, an x86 `ret`, which stands for the code a
 * frame last held. Fork writes only the top of a new stack, so a byte left
 * below it stays there for the life of the process; under QEMU that kept the
 * frame on the emulator's slow store path. The stack must come back clear,
 * every byte of it.
 *
 * The heap gives pages with a guard below them (`kalloc_guarded`) and
 * without (`kalloc_pages`), and records which a stack came from and went
 * back to: a stack must be a guarded run, since the guard is what turns an
 * overrun into a fault. `kstack_overrun_pid` must name the running thread
 * for an address in the page below its stack and for no other.
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
    uint32_t kernel_stack;
} process_t;

process_t *current_proc;

/* External, so that a fault which removes the clearing leaves an unused
 * function the compiler does not refuse. */
void *kalloc_pages(uint32_t count, uintptr_t *phys);
void kfree_pages(void *pages, uint32_t count);
void *kalloc_guarded(uint32_t count);
void kfree_guarded(void *pages, uint32_t count);
void *k_memset(void *dst, int c, size_t n);
int kstack_overrun_pid(uint32_t addr);

#include "kstack.inc"

static int fails, heap_empty;
static uint32_t plain_asked, guarded_asked, guarded_freed;
static void *guarded_freed_at;

static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("kstack-proof: fail: %s\n", what);
    }
}

/* `count` pages filled with a code byte, or none once a row empties the
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
    return pages;
}

void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    if (phys) {
        *phys = 0;
    }
    plain_asked = count;
    return code_pages(count);
}

void kfree_pages(void *pages, uint32_t count)
{
    (void)count;
    free(pages);
}

void *kalloc_guarded(uint32_t count)
{
    guarded_asked = count;
    return code_pages(count);
}

void kfree_guarded(void *pages, uint32_t count)
{
    guarded_freed_at = pages;
    guarded_freed = count;
    free(pages);
}

/* The kernel's fill, for the extracted code: every byte of `n` set. */
void *k_memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;

    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

/* How many bytes of the stack at `base` are not zero. */
static size_t dirty_bytes(uintptr_t base)
{
    const unsigned char *bytes = (const unsigned char *)base;
    size_t i, dirty = 0;

    for (i = 0; i < KERNEL_STACK_SIZE; i++) {
        dirty += bytes[i] != 0;
    }
    return dirty;
}

/* A thread whose stack's top is 0xC0106000: the stack is the four pages
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
    check(kstack_overrun_pid(0xFFFFB000u) == -1, "a thread with no stack gives none");
    current_proc = NULL;
    check(kstack_overrun_pid(0xC0101000u) == -1, "no running thread gives none");
}

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
