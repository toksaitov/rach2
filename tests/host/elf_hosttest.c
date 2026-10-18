/**
 * elf_hosttest.c: the ELF loader's checks of a malformed image
 * (`proc/elf.c`).
 *
 * host-deps:
 *
 * Includes elf.c directly and stubs the page and VMA calls, then drives
 * elf_load_image with crafted in-memory images through a readfn over a byte
 * buffer. Every malformed image must be rejected with ENOEXEC during
 * validation, before any page is allocated. The alloc stub records whether
 * allocation was reached, so a test tells a validation reject (alloc never
 * reached) from an image that passed validation (reached alloc, which the
 * stub then fails, and which must answer ENOMEM). A reader that fails must
 * make the load answer EIO. A separate reentry row maps synthetic frame
 * numbers to host buffers and loads a second image inside the first image's
 * read callback. Both loads must retain their own segment geometry and
 * bytes. A position-independent image is read for its layout and
 * interpreter and loaded at a base, and read as an interpreter is, with its
 * own PT_INTERP not read. Loaded with a file to map, a segment whose offset
 * and address agree modulo the page takes no frame but the one where its
 * file bytes end, which holds those bytes and zeros after them, and gets a
 * region of the file and one of anonymous memory; one whose offset and
 * address disagree is copied, and one that names bytes past the file's end
 * fails before any frame is taken. Each failed check prints a line on
 * stderr, and the test exits with their count, at most 100.
 */

#include "proc/elf.c"
#include "arch/i686/mmu.h"

extern int dprintf(int fd, const char *format, ...);

static int fails;
/* Whether `alloc_user_page` was called since the flag was cleared. */
static int alloc_reached;
/* Whether `alloc_user_page` maps frames, or fails every allocation. */
static int map_frames;
/* The frames taken from `frames`; frame N is at physical address N pages. */
static unsigned frame_count;
static unsigned char frames[4][PAGE_SIZE];

/** Count and print a failure when `cond` is false. */
#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            dprintf(2, "elf-hosttest: failed: line %d: %s\n", __LINE__, #cond); \
            fails++;                                                            \
        }                                                                       \
    } while (0)

/** Note that allocation was reached and fail it in the rejection rows; with
 * `map_frames` set, map the next of `frames` at `virt_addr` in `dir` and
 * return its physical address, or 0 when the four are taken. */
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

/** The frame mapped at `addr`, from the tables `alloc_user_page` fills. */
uintptr_t paging_user_phys(page_dir_t *dir, uintptr_t addr)
{
    page_table_t *pt = dir->logical_page_tables[addr / PAGE_SIZE / 1024];

    return pt ? (uintptr_t)(pt->pages[addr / PAGE_SIZE % 1024] & ~0xFFFu) : 0;
}

/** The model has no page rights to change. */
void paging_set_writable(page_dir_t *dir, uintptr_t addr, bool writable)
{
    (void)dir;
    (void)addr;
    (void)writable;
}

/** The model has no page rights to change. */
void paging_set_exec(page_dir_t *dir, uintptr_t addr, bool exec)
{
    (void)dir;
    (void)addr;
    (void)exec;
}

/** The host buffer of the synthetic frame at `phys`, or NULL; the frames
 * are numbered so that a host pointer is never truncated to 32 bits. */
void *kmap_temp(uintptr_t phys)
{
    if (phys == 0 || phys / PAGE_SIZE > frame_count) {
        return NULL;
    }
    return frames[phys / PAGE_SIZE - 1];
}

/** Zero the synthetic frame at `phys`; 0, or -1 when there is none. */
int paging_zero_frame(uintptr_t phys)
{
    void *page = kmap_temp(phys);

    if (!page) {
        return -1;
    }
    k_memset(page, 0, PAGE_SIZE);
    return 0;
}

/** A frame of `kmap_temp` needs no unmapping. */
void kunmap_temp(void *va)
{
    (void)va;
}

/** The model keeps no regions. */
void vm_init(vm_space_t *vm)
{
    (void)vm;
}

/* The regions `vm_insert` and `vm_insert_file` were given, in order. */
static struct {
    uintptr_t start, end;
    struct vm_file *file;
    uint32_t pgoff;
    uint32_t prot;
} regions[8];
static int region_count;

/** Record the region, a file's or anonymous memory's; 0. */
int vm_insert_file(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot, vm_type_t type,
                   struct vm_file *file, uint32_t pgoff)
{
    (void)vm;
    (void)type;
    if (region_count < 8) {
        regions[region_count].start = start;
        regions[region_count].end = end;
        regions[region_count].file = file;
        regions[region_count].pgoff = pgoff;
        regions[region_count].prot = prot;
        region_count++;
    }
    return 0;
}

/** Record the region of anonymous memory; 0. */
int vm_insert(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot, vm_type_t type)
{
    return vm_insert_file(vm, start, end, prot, type, NULL, 0);
}

/** memset, for elf.c. */
void *k_memset(void *ptr, int c, size_t n)
{
    unsigned char *d = ptr;
    while (n--) {
        *d++ = (unsigned char)c;
    }
    return ptr;
}

/** memcpy, for elf.c. */
void *k_memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

/* The image the rows load, its first `img_size` bytes readable. */
static unsigned char img_buf[8192];
static uint32_t img_size;

/* The file offset from which `test_read` fails, as a device error would;
 * past any image for none. */
static uint32_t read_fails_at = 0xFFFFFFFFu;

/**
 * The reader of the rows: up to `count` bytes at `off` of `img_buf`, 0 past
 * `img_size`, and -1 from `read_fails_at` on.
 */
static int test_read(void *ctx, void *dst, uint32_t count, uint32_t off)
{
    uint32_t avail;
    (void)ctx;
    if (off >= read_fails_at) {
        return -1;
    }
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

/** Lay down a valid ELF header with `nph` program headers, all zero, right
 * after it, and set `img_size` to cover them and 0x100 bytes more. */
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

/** Make program header `i` a PT_LOAD of the given fields. */
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

/**
 * Load the image in `img_buf` into `dir`, clearing `alloc_reached` first; the
 * load's result.
 */
static int run(page_dir_t *dir, elf_image_t *out)
{
    alloc_reached = 0;
    return elf_load_image(test_read, 0, dir, out);
}

/** An image of the reentry row, with the page directory it loads into. */
struct nested_image {
    unsigned char bytes[8192];
    uint32_t size;
    struct nested_image *other; /* loaded inside this one's read, or NULL */
    page_dir_t dir;
    page_table_t table;
    elf_image_t loaded;
    int result;
};

/** The reader of the reentry row: up to `count` bytes at `off` of the image
 * `ctx`. The read of the first segment, at offset 0, loads the image's
 * `other` first, once, as a read that sleeps lets another process's exec
 * run. Returns the bytes read. */
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

/** Two valid images with different second segments load independently, one
 * inside the other's read. */
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

/** Count and print a failure, naming the row `row`, unless `elf_check`
 * accepts the image exactly when `accept` is nonzero. */
static void check_bound(const char *row, int accept)
{
    if (!elf_check(img_buf, img_size) != !accept) {
        dprintf(2, "elf-hosttest: failed: elf_check: %s\n", row);
        fails++;
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
    check_bound("one header", 1);

    e->e_phoff = 0xfffffff0u;
    e->e_phnum = 1;
    check_bound("offset near 2^32", 0);

    e->e_phoff = (uint32_t)sizeof(Elf32_Ehdr);
    e->e_phnum = 0xffffu;
    check_bound("65535 headers", 0);

    e->e_phnum = 1;
    e->e_phentsize = 0;
    check_bound("zero e_phentsize", 0);

    e->e_phentsize = (uint16_t)sizeof(Elf32_Phdr);
    check_bound("one header again", 1);
}

/** Make program header `i` a PT_INTERP naming `path`, whose bytes, NUL
 * included, are put at `off`. */
static void set_interp(int i, uint32_t off, const char *path)
{
    Elf32_Phdr *p = (Elf32_Phdr *)(img_buf + sizeof(Elf32_Ehdr)) + i;
    uint32_t n = 0;

    do {
        img_buf[off + n] = (unsigned char)path[n];
    } while (path[n++] != '\0');
    p->p_type = PT_INTERP;
    p->p_offset = off;
    p->p_filesz = n;
    p->p_memsz = n;
}

/** Whether the strings `a` and `b` are equal. */
static int same_text(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/**
 * A position-independent program: its layout names its interpreter, and a
 * load at a base moves its segment, entry and program headers by the base.
 * Without a base it lies below the user window and is refused before a page
 * is taken, as are an unaligned base, a base for an ET_EXEC image, and an
 * interpreter path that is relative or has no NUL. Read as an interpreter
 * is read, without its PT_INTERP, the same image with either path has its
 * layout and names no interpreter.
 */
static void dynamic_images(void)
{
    static page_dir_t dir;
    static page_table_t table;
    elf_layout_t lay;
    elf_image_t out;

    base_image(2);
    ((Elf32_Ehdr *)img_buf)->e_type = ET_DYN;
    ((Elf32_Ehdr *)img_buf)->e_entry = 0x120u;
    set_interp(0, 0x100u, "/lib/ld-linux.so.2");
    set_load(1, 0, 0, 0x200u, 0x1800u, PF_R | PF_X);
    img_size = 0x200u;
    CHECK(elf_read_layout(test_read, 0, 1, &lay) == 0);
    CHECK(lay.type == ET_DYN && lay.lo == 0 && lay.hi == 0x2000u);
    CHECK(same_text(lay.interp, "/lib/ld-linux.so.2"));

    k_memset(&dir, 0, sizeof(dir));
    dir.logical_page_tables[ELF_ET_DYN_BASE / PAGE_SIZE / 1024] = &table;
    frame_count = 0;
    map_frames = 1;
    CHECK(elf_load_image_at(test_read, 0, &dir, ELF_ET_DYN_BASE, NULL, &out) == 0);
    map_frames = 0;
    CHECK(out.entry == ELF_ET_DYN_BASE + 0x120u);
    CHECK(out.phdr_vaddr == ELF_ET_DYN_BASE + sizeof(Elf32_Ehdr));
    CHECK(out.nsegs == 1 && out.segs[0].start == ELF_ET_DYN_BASE &&
          out.segs[0].end == ELF_ET_DYN_BASE + 0x2000u);

    alloc_reached = 0;
    CHECK(elf_load_image_at(test_read, 0, &dir, 0, NULL, &out) == -ENOEXEC && alloc_reached == 0);
    alloc_reached = 0;
    CHECK(elf_load_image_at(test_read, 0, &dir, ELF_ET_DYN_BASE + 0x10u, NULL, &out) == -ENOEXEC &&
          alloc_reached == 0);

    img_buf[0x100 + 18] = 'x';
    CHECK(elf_read_layout(test_read, 0, 1, &lay) == -ENOEXEC);
    CHECK(elf_read_layout(test_read, 0, 0, &lay) == 0 && lay.interp[0] == '\0' &&
          lay.type == ET_DYN && lay.hi == 0x2000u);
    set_interp(0, 0x100u, "ld.so");
    CHECK(elf_read_layout(test_read, 0, 1, &lay) == -ENOEXEC);
    CHECK(elf_read_layout(test_read, 0, 0, &lay) == 0 && lay.interp[0] == '\0');

    base_image(1);
    set_load(0, 0x100u, 0x08048000u, 0x100u, 0x100u, 5u);
    alloc_reached = 0;
    CHECK(elf_load_image_at(test_read, 0, &dir, ELF_ET_DYN_BASE, NULL, &out) == -ENOEXEC &&
          alloc_reached == 0);
}

/**
 * The regions `elf_add_vmas` makes of the image `mapped_segments` loaded,
 * whose segments map `file`: the file's two pages from page 0, then an
 * anonymous page for each segment.
 */
static void mapped_regions(const elf_image_t *out, struct vm_file *file)
{
    vm_space_t vm;

    region_count = 0;
    CHECK(elf_add_vmas(&vm, out) == 0 && region_count == 3);
    CHECK(regions[0].start == 0x08048000u && regions[0].end == 0x0804A000u &&
          regions[0].file == file && regions[0].pgoff == 0);
    CHECK(regions[1].start == 0x0804A000u && regions[1].end == 0x0804B000u &&
          regions[1].file == NULL);
    CHECK(regions[2].start == 0x0804B000u && regions[2].end == 0x0804C000u &&
          regions[2].file == NULL);
}

/**
 * The protection `elf_add_vmas` gives the regions of segments with each
 * kind of `p_flags`: write and execute each grant read, as the processor
 * reads a page it may write or run, and a segment with no flags has none.
 */
static void region_protection(void)
{
    static const uint32_t flags[5] = {PF_X, PF_W, PF_R, PF_R | PF_X, 0};
    static const uint32_t prot[5] = {VM_PROT_READ | VM_PROT_EXEC, VM_PROT_READ | VM_PROT_WRITE,
                                     VM_PROT_READ, VM_PROT_READ | VM_PROT_EXEC, 0};
    vm_space_t vm;
    elf_image_t out;
    int i;

    k_memset(&out, 0, sizeof(out));
    out.nsegs = 5;
    for (i = 0; i < 5; i++) {
        out.segs[i].start = 0x08048000u + (uintptr_t)i * PAGE_SIZE;
        out.segs[i].file_end = out.segs[i].start;
        out.segs[i].end = out.segs[i].start + PAGE_SIZE;
        out.segs[i].flags = flags[i];
    }
    region_count = 0;
    CHECK(elf_add_vmas(&vm, &out) == 0 && region_count == 5);
    for (i = 0; i < 5; i++) {
        CHECK(regions[i].prot == prot[i]);
    }
}

/**
 * Segments loaded with a file to map. The first, at offset 0, has 0x1100
 * file bytes in three pages: it takes one frame, at its second page, with
 * the file's bytes from 0x1000 and zeros after them, and its regions are
 * the file's two pages from page 0 and an anonymous third. The second, at
 * file offset 0x1100 and an address 0x200 into its page, cannot be mapped
 * and is copied into a frame of its own. A mapped segment whose last file
 * byte lies past the file's end fails the load before any frame is taken.
 */
static void mapped_segments(void)
{
    static page_dir_t dir;
    static page_table_t table;
    struct vm_file *file = (struct vm_file *)&table;
    elf_image_t out;
    unsigned char *page;
    int i;

    base_image(2);
    set_load(0, 0, 0x08048000u, 0x1100u, 0x3000u, PF_R | PF_W);
    set_load(1, 0x1100u, 0x0804B200u, 0x10u, 0x10u, PF_R);
    img_size = 0x1110u;
    for (i = 0x1000; i < 0x1110; i++) {
        img_buf[i] = (unsigned char)(i < 0x1100 ? 'f' : 'g');
    }
    k_memset(&dir, 0, sizeof(dir));
    k_memset(&table, 0, sizeof(table));
    dir.logical_page_tables[0x08048000u / PAGE_SIZE / 1024] = &table;
    frame_count = 0;
    map_frames = 1;
    CHECK(elf_load_image_at(test_read, 0, &dir, 0, file, &out) == 0);
    map_frames = 0;
    CHECK(out.file == file && out.nsegs == 2 && frame_count == 2);
    CHECK(out.segs[0].start == 0x08048000u && out.segs[0].file_end == 0x0804A000u &&
          out.segs[0].end == 0x0804B000u && out.segs[0].pgoff == 0);
    CHECK(out.segs[1].start == 0x0804B000u && out.segs[1].file_end == 0x0804B000u);
    CHECK(paging_user_phys(&dir, 0x08048000u) == 0 && paging_user_phys(&dir, 0x0804A000u) == 0);
    page = kmap_temp(paging_user_phys(&dir, 0x08049000u));
    CHECK(page != NULL && page[0] == 'f' && page[0xFF] == 'f' && page[0x100] == 0 &&
          page[PAGE_SIZE - 1] == 0);
    page = kmap_temp(paging_user_phys(&dir, 0x0804B000u));
    CHECK(page != NULL && page[0x1FF] == 0 && page[0x200] == 'g' && page[0x20F] == 'g' &&
          page[0x210] == 0);
    mapped_regions(&out, file);

    base_image(1);
    set_load(0, 0, 0x08048000u, 0x2000u, 0x2000u, PF_R | PF_X);
    img_size = 0x1FFFu;
    map_frames = 1;
    alloc_reached = 0;
    CHECK(elf_load_image_at(test_read, 0, &dir, 0, file, &out) == -ENOEXEC && alloc_reached == 0);
    map_frames = 0;
}

/**
 * A mapped segment whose memory ends in the page its 0x1100 file bytes end
 * in, before the file does: that page is loaded at once, with zeros after
 * the segment's bytes where the file has more, and the region of the file
 * covers it.
 */
static void mapped_bss_page(void)
{
    static page_dir_t dir;
    static page_table_t table;
    struct vm_file *file = (struct vm_file *)&table;
    elf_image_t out;
    unsigned char *page;
    int i;

    base_image(1);
    set_load(0, 0, 0x08048000u, 0x1100u, 0x1800u, PF_R | PF_W);
    img_size = 0x1200u;
    for (i = 0x1000; i < 0x1200; i++) {
        img_buf[i] = (unsigned char)(i < 0x1100 ? 'f' : 'g');
    }
    k_memset(&dir, 0, sizeof(dir));
    k_memset(&table, 0, sizeof(table));
    dir.logical_page_tables[0x08048000u / PAGE_SIZE / 1024] = &table;
    frame_count = 0;
    map_frames = 1;
    CHECK(elf_load_image_at(test_read, 0, &dir, 0, file, &out) == 0);
    map_frames = 0;
    CHECK(out.nsegs == 1 && frame_count == 1 && out.segs[0].file_end == 0x0804A000u &&
          out.segs[0].end == 0x0804A000u);
    page = kmap_temp(paging_user_phys(&dir, 0x08049000u));
    CHECK(page != NULL && page[0xFF] == 'f' && page[0x100] == 0 && page[0x1FF] == 0);
}

/**
 * A read that fails is an I/O error, not a malformed image: a reader that
 * fails on a segment's bytes makes the load answer EIO once its page was
 * taken, and one that fails on the program headers makes the layout read
 * answer EIO, where a file that merely ends before them is ENOEXEC.
 */
static void read_error(void)
{
    static page_dir_t dir;
    static page_table_t table;
    elf_layout_t lay;
    elf_image_t out;

    base_image(1);
    set_load(0, 0x100u, 0x08048000u, 0x100u, 0x100u, 5u);
    k_memset(&dir, 0, sizeof(dir));
    dir.logical_page_tables[0x08048000u / PAGE_SIZE / 1024] = &table;
    frame_count = 0;
    map_frames = 1;
    read_fails_at = 0x100u;
    alloc_reached = 0;
    CHECK(elf_load_image(test_read, 0, &dir, &out) == -EIO && alloc_reached == 1);
    map_frames = 0;
    read_fails_at = sizeof(Elf32_Ehdr);
    CHECK(elf_read_layout(test_read, 0, 1, &lay) == -EIO);
    read_fails_at = 0xFFFFFFFFu;
    img_size = sizeof(Elf32_Ehdr) + 10u;
    CHECK(elf_read_layout(test_read, 0, 1, &lay) == -ENOEXEC);
}

/**
 * Run the rejection rows on an image each, then the read error, reentry, header
 * table, position-independent and mapped rows.
 *
 * The row of too many program headers covers the whole claimed table with
 * `img_size` and makes phdr[0] a valid PT_LOAD, so the cap on e_phnum is the
 * only check that rejects it: without the cap the loader would read past its
 * array of program headers and reach allocation. A well-formed image passes
 * validation and reaches the allocation, which the stub fails, so the load
 * answers ENOMEM and not the ENOEXEC of a malformed image.
 */
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
    CHECK(run(&dir, &out) == -ENOEXEC && alloc_reached == 0);

    /* A bad magic byte. */
    base_image(1);
    set_load(0, 0x100u, 0x08048000u, 0x10u, 0x10u, 5u);
    ((Elf32_Ehdr *)img_buf)->e_ident[EI_MAG0] = 0;
    CHECK(run(&dir, &out) == -ENOEXEC && alloc_reached == 0);

    /* More program headers than the loader will hold. */
    base_image(1);
    set_load(0, 0x100u, 0x08048000u, 0x10u, 0x10u, 5u);
    ((Elf32_Ehdr *)img_buf)->e_phnum = ELF_MAX_PHNUM + 1;
    img_size = sizeof(Elf32_Ehdr) + (uint32_t)(ELF_MAX_PHNUM + 1) * sizeof(Elf32_Phdr) + 0x100u;
    CHECK(run(&dir, &out) == -ENOEXEC && alloc_reached == 0);

    /* A program-header table that runs past the image. */
    base_image(1);
    set_load(0, 0x100u, 0x08048000u, 0x10u, 0x10u, 5u);
    img_size = sizeof(Elf32_Ehdr) + 10u;
    CHECK(run(&dir, &out) == -ENOEXEC && alloc_reached == 0);

    /* p_filesz larger than p_memsz. */
    base_image(1);
    set_load(0, 0x100u, 0x08048000u, 0x200u, 0x100u, 5u);
    CHECK(run(&dir, &out) == -ENOEXEC && alloc_reached == 0);

    /* A segment whose end runs into the stack's top page, USER_STACK_BASE. */
    base_image(1);
    set_load(0, 0x100u, 0xBFFFE000u, 0x2000u, 0x2000u, 5u);
    CHECK(run(&dir, &out) == -ENOEXEC && alloc_reached == 0);

    /* A segment below the user image window (into the kernel identity map). */
    base_image(1);
    set_load(0, 0x100u, 0x01000000u, 0x100u, 0x100u, 5u);
    CHECK(run(&dir, &out) == -ENOEXEC && alloc_reached == 0);

    /* A segment whose address range wraps past 2^32. */
    base_image(1);
    set_load(0, 0x100u, 0xBFFFE000u, 0x100u, 0xFFFFFFFFu, 5u);
    CHECK(run(&dir, &out) == -ENOEXEC && alloc_reached == 0);

    /* A well-formed image passes validation and the failed allocation. */
    base_image(1);
    set_load(0, 0x100u, 0x08048000u, 0x100u, 0x100u, 5u);
    CHECK(run(&dir, &out) == -ENOMEM && alloc_reached == 1);

    read_error();

    reentrant_load();
    header_table_bounds();
    dynamic_images();
    region_protection();
    mapped_segments();
    mapped_bss_page();

    dprintf(1, "elf-hosttest: result: %s: %d failures\n", fails ? "fail" : "pass", fails);
    return (fails > 100) ? 100 : fails;
}
