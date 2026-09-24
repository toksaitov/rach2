/**
 * A kernel stack taken from a heap whose pages held a finished process's
 * code (entry 26 of milestone 50).
 *
 * tests/host/kstack_mutate.sh extracts `kstack_alloc` and `kstack_free`
 * unchanged from a copy of rach2/sys/syscall_proc.c into kstack.inc, with
 * the two sizes they use. The heap here fills every page it hands out with
 * 0xC3, an x86 `ret`, which stands for the code a frame last held. Fork
 * writes only the top of a new stack, so a byte left below it stays there
 * for the life of the process; under QEMU that kept the frame on the
 * emulator's slow store path. The stack must come back clear, every byte
 * of it.
 *
 * The fixture is hosted C: `uintptr_t` is the host's, wide enough for the
 * host pointer `kstack_alloc` returns as an integer.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* External, so that a fault which removes the clearing leaves an unused
 * function the compiler does not refuse. */
void *kalloc_pages(uint32_t count, uintptr_t *phys);
void kfree_pages(void *pages, uint32_t count);
void *k_memset(void *dst, int c, size_t n);

#include "kstack.inc"

static int fails, heap_empty;
static uint32_t pages_asked, pages_freed;
static void *freed_at;

static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("kstack-proof: fail: %s\n", what);
    }
}

/* `count` pages filled with a code byte, or none once a row empties the
 * heap. */
void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    void *pages;

    if (phys) {
        *phys = 0;
    }
    pages_asked = count;
    if (heap_empty) {
        return NULL;
    }
    pages = malloc((size_t)count * PAGE_SIZE);
    if (pages) {
        memset(pages, 0xC3, (size_t)count * PAGE_SIZE);
    }
    return pages;
}

void kfree_pages(void *pages, uint32_t count)
{
    freed_at = pages;
    pages_freed = count;
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

int main(void)
{
    uintptr_t base = kstack_alloc();

    check(base != 0 && pages_asked == KERNEL_STACK_SIZE / PAGE_SIZE,
          "a kernel stack is its four pages from the heap");
    check(base != 0 && dirty_bytes(base) == 0,
          "every byte of a new kernel stack is zero, though its pages held code");
    if (base != 0) {
        kstack_free(base + KERNEL_STACK_SIZE);
        check(freed_at == (void *)base && pages_freed == KERNEL_STACK_SIZE / PAGE_SIZE,
              "the stack's top gives the same four pages back");
    }
    heap_empty = 1;
    check(kstack_alloc() == 0, "a heap with no pages gives no stack");
    if (fails) {
        printf("kstack-proof: result: fail: %d rows\n", fails);
        return 1;
    }
    printf("kstack-proof: result: pass: a new kernel stack is cleared\n");
    return 0;
}
