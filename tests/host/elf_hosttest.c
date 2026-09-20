/**
 * Host test for the ELF loader's hostile-image rejection (sys/proc/elf.c).
 *
 * host-deps:
 *
 * Includes elf.c directly and stubs the page and VMA calls, then drives
 * elf_load_image with crafted in-memory images through a readfn over a byte
 * buffer. Every malformed image must be rejected during validation, BEFORE
 * any page is allocated. The alloc stub records whether allocation was
 * reached, so a test tells a validation reject (alloc never reached) from an
 * image that passed validation (reached alloc, which the stub then fails). A
 * separate reentry row maps synthetic frame numbers to host buffers and loads
 * a second image inside the first image's read callback. Both loads must retain
 * their own segment geometry and bytes. Returns the failed-check count.
 */

#include "proc/elf.c"

static int fails;
static int alloc_reached;
static int map_frames;
static unsigned frame_count;
static unsigned char frames[4][PAGE_SIZE];

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/** Fail allocation in rejection rows; map frames in the reentry row. */
uintptr_t alloc_user_page(page_dir_t *dir, uintptr_t virt_addr)
{
    uintptr_t phys;
    page_table_t *pt;

    alloc_reached = 1;
    if (!map_frames || frame_count == 4) {
        return 0;
    }
    pt = dir->logical_page_tables[virt_addr / PAGE_SIZE / 1024];
    CHECK(pt != NULL);
    if (!pt) {
        return 0;
    }
    phys = ++frame_count * PAGE_SIZE;
    pt->pages[virt_addr / PAGE_SIZE % 1024] = (uint32_t)phys | 7u;
    return phys;
}

void paging_set_writable(page_dir_t *dir, uintptr_t addr, bool writable)
{
    (void)dir;
    (void)addr;
    (void)writable;
}

/** Map synthetic frame numbers without truncating a host pointer to 32 bits. */
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

int vm_insert(vm_space_t *vm, uint32_t start, uint32_t end, uint32_t prot, vm_type_t type)
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

static unsigned char img_buf[8192];
static uint32_t img_size;

static int test_read(void *ctx, void *dst, uint32_t count, uint32_t off)
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

/* Lay down a valid ELF header with `nph` program headers right after it. */
static void base_image(int nph)
{
    Elf32_Ehdr *e = (Elf32_Ehdr *)img_buf;
    int i;

    for (i = 0; i < (int)sizeof(img_buf); i++) {
        img_buf[i] = 0;
    }
    e->e_ident[EI_MAG0] = ELFMAG0;
    e->e_ident[EI_MAG1] = ELFMAG1;
    e->e_ident[EI_MAG2] = ELFMAG2;
    e->e_ident[EI_MAG3] = ELFMAG3;
    e->e_ident[EI_CLASS] = ELFCLASS32;
    e->e_ident[EI_DATA] = ELFDATA2LSB;
    e->e_ident[EI_VERSION] = EV_CURRENT;
    e->e_type = ET_EXEC;
    e->e_machine = EM_386;
    e->e_version = EV_CURRENT;
    e->e_entry = 0x08048000u;
    e->e_phoff = sizeof(Elf32_Ehdr);
    e->e_phentsize = sizeof(Elf32_Phdr);
    e->e_phnum = (uint16_t)nph;
    img_size = sizeof(Elf32_Ehdr) + (uint32_t)nph * sizeof(Elf32_Phdr) + 0x100u;
}

static void set_load(int i, uint32_t off, uint32_t vaddr, uint32_t filesz, uint32_t memsz,
                     uint32_t flags)
{
    Elf32_Phdr *p = (Elf32_Phdr *)(img_buf + sizeof(Elf32_Ehdr)) + i;

    p->p_type = PT_LOAD;
    p->p_offset = off;
    p->p_vaddr = vaddr;
    p->p_filesz = filesz;
    p->p_memsz = memsz;
    p->p_flags = flags;
}

static int run(page_dir_t *dir, elf_image_t *out)
{
    alloc_reached = 0;
    return elf_load_image(test_read, 0, dir, out);
}

struct nested_image {
    unsigned char bytes[8192];
    uint32_t size;
    struct nested_image *other;
    page_dir_t dir;
    page_table_t table;
    elf_image_t loaded;
    int result;
};

/** A segment read may sleep while another process completes its own exec. */
static int nested_read(void *ctx, void *dst, uint32_t count, uint32_t off)
{
    struct nested_image *image = ctx;

    if (off == 0 && count > sizeof(Elf32_Ehdr) && image->other) {
        struct nested_image *other = image->other;

        image->other = NULL;
        other->result = elf_load_image(nested_read, other, &other->dir, &other->loaded);
    }
    if (off >= image->size) {
        return 0;
    }
    if (count > image->size - off) {
        count = image->size - off;
    }
    k_memcpy(dst, image->bytes + off, count);
    return (int)count;
}

/** Two valid images with different second segments must load independently. */
static void reentrant_load(void)
{
    static struct nested_image images[2];
    int i, j;

    k_memset(images, 0, sizeof(images));
    for (i = 0; i < 2; i++) {
        uint32_t addr = 0x08048000u + (uint32_t)i * 0x01000000u;

        base_image(2);
        set_load(0, 0, addr, 0x100u, 0x100u, PF_R | PF_X);
        set_load(1, 0x200u, addr + (uint32_t)(i + 1) * PAGE_SIZE, 0x100u, 0x100u, PF_R | PF_W);
        ((Elf32_Ehdr *)img_buf)->e_entry = addr;
        img_size = 0x300u;
        for (j = 0x200; j < 0x300; j++) {
            img_buf[j] = (unsigned char)(i + 1);
        }
        k_memcpy(images[i].bytes, img_buf, img_size);
        images[i].size = img_size;
        images[i].dir.logical_page_tables[addr / PAGE_SIZE / 1024] = &images[i].table;
    }
    images[0].other = &images[1];
    frame_count = 0;
    map_frames = 1;
    images[0].result = elf_load_image(nested_read, &images[0], &images[0].dir, &images[0].loaded);
    map_frames = 0;
    for (i = 0; i < 2; i++) {
        uint32_t addr = 0x08048000u + (uint32_t)i * 0x01000000u;
        uint32_t second = addr + (uint32_t)(i + 1) * PAGE_SIZE;
        uintptr_t phys = images[i].table.pages[second / PAGE_SIZE % 1024] & ~0xFFFu;
        unsigned char *page = kmap_temp(phys);

        CHECK(images[i].result == 0);
        CHECK(images[i].loaded.nsegs == 2);
        CHECK(images[i].loaded.phdr_vaddr == addr + sizeof(Elf32_Ehdr));
        CHECK(images[i].loaded.segs[1].start == second);
        CHECK(page != NULL);
        if (page) {
            for (j = 0; j < 0x100; j++) {
                CHECK(page[j] == (unsigned char)(i + 1));
            }
        }
    }
}

/**
 * `elf_check` bounds the program header table without wrapping.
 *
 * The debug shell's `elf` command reads the table straight after this call,
 * so an offset near 2^32 whose sum with the table length wraps below `size`
 * would send it outside the buffer it read.
 */
static void header_table_bounds(void)
{
    Elf32_Ehdr *e;

    base_image(1);
    set_load(0, 0x100u, 0x08048000u, 0x100u, 0x100u, 5u);
    e = (Elf32_Ehdr *)img_buf;
    CHECK(elf_check(img_buf, img_size));

    e->e_phoff = 0xfffffff0u;
    e->e_phnum = 1;
    CHECK(!elf_check(img_buf, img_size));

    e->e_phoff = (uint32_t)sizeof(Elf32_Ehdr);
    e->e_phnum = 0xffffu;
    CHECK(!elf_check(img_buf, img_size));

    e->e_phnum = 1;
    e->e_phentsize = 0;
    CHECK(!elf_check(img_buf, img_size));

    e->e_phentsize = (uint16_t)sizeof(Elf32_Phdr);
    CHECK(elf_check(img_buf, img_size));
}

int main(void)
{
    page_dir_t dir;
    elf_image_t out;
    int i;

    for (i = 0; i < (int)sizeof(dir); i++) {
        ((char *)&dir)[i] = 0;
    }

    /* A header read short of the ELF header is rejected before validation. */
    base_image(1);
    set_load(0, 0x100u, 0x08048000u, 0x10u, 0x10u, 5u);
    img_size = 20u;
    CHECK(run(&dir, &out) == -1 && alloc_reached == 0);

    /* A bad magic byte. */
    base_image(1);
    set_load(0, 0x100u, 0x08048000u, 0x10u, 0x10u, 5u);
    ((Elf32_Ehdr *)img_buf)->e_ident[EI_MAG0] = 0;
    CHECK(run(&dir, &out) == -1 && alloc_reached == 0);

    /* More program headers than the loader will hold. img_size covers the whole
       claimed table and phdr[0] is a valid PT_LOAD, so the e_phnum cap is the
       only thing that rejects this image: were the cap removed, the loader
       would read past its phdr array and reach allocation, which alloc_reached
       catches. */
    base_image(1);
    set_load(0, 0x100u, 0x08048000u, 0x10u, 0x10u, 5u);
    ((Elf32_Ehdr *)img_buf)->e_phnum = ELF_MAX_PHNUM + 1;
    img_size = sizeof(Elf32_Ehdr) + (uint32_t)(ELF_MAX_PHNUM + 1) * sizeof(Elf32_Phdr) + 0x100u;
    CHECK(run(&dir, &out) == -1 && alloc_reached == 0);

    /* A program-header table that runs past the image. */
    base_image(1);
    set_load(0, 0x100u, 0x08048000u, 0x10u, 0x10u, 5u);
    img_size = sizeof(Elf32_Ehdr) + 10u;
    CHECK(run(&dir, &out) == -1 && alloc_reached == 0);

    /* p_filesz larger than p_memsz. */
    base_image(1);
    set_load(0, 0x100u, 0x08048000u, 0x200u, 0x100u, 5u);
    CHECK(run(&dir, &out) == -1 && alloc_reached == 0);

    /* A segment whose end runs into the page-cache window. */
    base_image(1);
    set_load(0, 0x100u, 0xBFFFE000u, 0x2000u, 0x2000u, 5u);
    CHECK(run(&dir, &out) == -1 && alloc_reached == 0);

    /* A segment below the user image window (into the kernel identity map). */
    base_image(1);
    set_load(0, 0x100u, 0x01000000u, 0x100u, 0x100u, 5u);
    CHECK(run(&dir, &out) == -1 && alloc_reached == 0);

    /* A segment whose address range wraps past 2^32. */
    base_image(1);
    set_load(0, 0x100u, 0xBFFFE000u, 0x100u, 0xFFFFFFFFu, 5u);
    CHECK(run(&dir, &out) == -1 && alloc_reached == 0);

    /* A well-formed single-segment image passes validation and reaches the
       allocation step (which the stub then fails, so the call still returns -1
       but only after alloc_reached is set). */
    base_image(1);
    set_load(0, 0x100u, 0x08048000u, 0x100u, 0x100u, 5u);
    CHECK(run(&dir, &out) == -1 && alloc_reached == 1);

    reentrant_load();
    header_table_bounds();

    return (fails > 100) ? 100 : fails;
}
