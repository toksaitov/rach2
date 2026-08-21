/**
 * Seam test for the ELF loader's hostile-image rejection (sys/proc/elf.c).
 *
 * host-deps:
 *
 * Includes elf.c directly and stubs the page/VMA seam, then drives
 * elf_load_image with crafted in-memory images through a readfn over a byte
 * buffer. Every malformed image must be rejected during validation, BEFORE any
 * page is allocated. The alloc stub records whether allocation was reached, so a
 * test tells a validation reject (alloc never reached) from an image that passed
 * validation (reached alloc, which the stub then fails). A multi-segment defect
 * (an overlapping pair) needs the first segment to load fully, which the 32-bit
 * physical-pointer model cannot fake natively, so that case is left to a guest
 * exec test. Returns the failed-check count (0 == pass).
 */

#include "proc/elf.c"

static int fails;
static int alloc_reached;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/* The kernel seam elf.c links against; only alloc_user_page is reached, and
 * only once an image has passed validation (which is exactly what it records).
 */
uintptr_t alloc_user_page(page_dir_t *dir, uintptr_t virt_addr)
{
    (void)dir;
    (void)virt_addr;
    alloc_reached = 1;
    return 0;
}

void paging_set_writable(page_dir_t *dir, uintptr_t addr, bool writable)
{
    (void)dir;
    (void)addr;
    (void)writable;
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

void *k_memset(void *ptr, char c, size_t n)
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
       only thing that rejects this image: were the cap removed, the loader would
       read past its phdr array and reach allocation, which alloc_reached catches. */
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

    return (fails > 100) ? 100 : fails;
}
