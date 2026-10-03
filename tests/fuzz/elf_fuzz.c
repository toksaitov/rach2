/**
 * Fuzz harness for the ELF loader (sys/proc/elf.c): the input is the image.
 *
 * fuzz-deps:
 *
 * Includes elf.c directly, as the host test does, with the page and VMA
 * calls stubbed over a pool of host frames, and loads the input through a
 * read callback over its bytes. A load that fails is the loader refusing an
 * image; what the sanitizers look for is a read past the image, a write
 * past a frame, or arithmetic that wraps on the way.
 */
#include "proc/elf.c"
#include "arch/i686/mmu.h"
#include "../fuzz/fuzz.h"

#define FRAMES 16

extern void *malloc(unsigned long n);

/* Each frame is an allocation of its own, so a write past a frame's end is
   a fault the sanitizer sees and not a byte in the next frame. */
static unsigned frame_count;
static unsigned char *frames[FRAMES];

uintptr_t alloc_user_page(page_dir_t *dir, uintptr_t virt_addr)
{
    uintptr_t phys;
    page_table_t *pt;

    if (frame_count == FRAMES) {
        return 0;
    }
    pt = dir->logical_page_tables[virt_addr / PAGE_SIZE / 1024];
    if (!pt) {
        return 0;
    }
    if (frames[frame_count] == 0) {
        frames[frame_count] = malloc(PAGE_SIZE);
        if (frames[frame_count] == 0) {
            return 0;
        }
    }
    phys = ++frame_count * PAGE_SIZE;
    pt->pages[virt_addr / PAGE_SIZE % 1024] = (uint32_t)phys | 7u;
    return phys;
}

/** The frame mapped at `addr`, from the tables `alloc_user_page` fills. */
uintptr_t paging_user_phys(page_dir_t *dir, uintptr_t addr)
{
    page_table_t *pt = dir->logical_page_tables[addr / PAGE_SIZE / 1024];

    return pt ? (uintptr_t)(pt->pages[addr / PAGE_SIZE % 1024] & ~0xFFFu) : 0;
}

void paging_set_exec(page_dir_t *dir, uintptr_t addr, bool exec)
{
    (void)dir;
    (void)addr;
    (void)exec;
}

void paging_set_writable(page_dir_t *dir, uintptr_t addr, bool writable)
{
    (void)dir;
    (void)addr;
    (void)writable;
}

/* Synthetic frame numbers map to host buffers; a host pointer is never cut
   to 32 bits. */
void *kmap_temp(uintptr_t phys)
{
    if (phys == 0 || phys / PAGE_SIZE > frame_count) {
        return NULL;
    }
    return frames[phys / PAGE_SIZE - 1];
}

int paging_zero_frame(uintptr_t phys)
{
    void *page = kmap_temp(phys);

    if (!page) {
        return -1;
    }
    k_memset(page, 0, PAGE_SIZE);
    return 0;
}

void kunmap_temp(void *va)
{
    (void)va;
}

void vm_init(vm_space_t *vm)
{
    (void)vm;
}

int vm_insert(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot, vm_type_t type)
{
    (void)vm;
    (void)start;
    (void)end;
    (void)prot;
    (void)type;
    return 0;
}

void *k_memset(void *ptr, int c, size_t n)
{
    unsigned char *d = ptr;

    while (n--) {
        *d++ = (unsigned char)c;
    }
    return ptr;
}

void *k_memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

#define IMAGE_MAX 65536

static unsigned char img_buf[IMAGE_MAX];
static uint32_t img_size;

static int image_read(void *ctx, void *dst, uint32_t count, uint32_t off)
{
    uint32_t avail;

    (void)ctx;
    if (off >= img_size) {
        return 0;
    }
    avail = img_size - off;
    if (avail > count) {
        avail = count;
    }
    k_memcpy(dst, img_buf + off, avail);
    return (int)avail;
}

/* The directory maps the 8 MiB at 0x08000000, where a program is linked;
   a segment elsewhere finds no table and the load fails cleanly. */
static page_dir_t dir;
static page_table_t tables[2];

int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size)
{
    elf_image_t out;
    unsigned i;

    if (size > IMAGE_MAX) {
        size = IMAGE_MAX;
    }
    k_memcpy(img_buf, data, (size_t)size);
    img_size = (uint32_t)size;
    frame_count = 0;
    k_memset(&dir, 0, sizeof(dir));
    k_memset(tables, 0, sizeof(tables));
    for (i = 0; i < 2; i++) {
        dir.logical_page_tables[0x08000000u / PAGE_SIZE / 1024 + i] = &tables[i];
    }
    k_memset(&out, 0, sizeof(out));
    (void)elf_load_image(image_read, 0, &dir, &out);
    return 0;
}
