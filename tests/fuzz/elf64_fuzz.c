/**
 * Fuzz harness for the x86-64 kernel's ELF64 loader (sys/proc/elf.c): the
 * input is the image.
 *
 * fuzz-deps:
 * fuzz-cflags: -DRACH2_HOST_X86_64
 *
 * Includes elf.c compiled with x86-64's `cpu.h`, so the loader reads ELF64
 * headers and checks segments against the x86-64 user window. The input
 * goes through each path a program takes into the kernel: the debug shell's
 * `elf_check` over the bytes in memory, the boot's copy at its own
 * addresses, and execve's reads of the layout and its loads of a program
 * and of an interpreter, mapped from a file, at the biases exec gives them.
 *
 * A load that fails has rejected the image, which is a valid result. A load
 * that succeeds is checked against the input, read here apart from the
 * loader: every page it maps lies in the window from USER_ADDR_MIN to
 * USER_STACK_BASE and is mapped once, and every segment it records matches
 * a PT_LOAD whose file bytes the file has, in ascending order, with those
 * bytes and zeros after them in the pages it filled. The entry point and
 * the program header table must lie in the window and the table in a
 * segment. An image that breaks one of these rules and still loads makes
 * the harness print the rule and abort, as a sanitizer's report does.
 */
#include "proc/elf.c"
#include "../fuzz/fuzz.h"

extern void *malloc(unsigned long n);
extern void free(void *p);
extern long write(int fd, const void *buf, unsigned long n);
extern void abort(void);

#define FRAMES    16
#define IMAGE_MAX 65536
#define PAGE_MASK ((uintptr_t)(PAGE_SIZE - 1))

/* The model directory: the user page each frame of the pool maps, in the
 * order the loader took them. Frame `i` has the synthetic physical address
 * `(i + 1) * PAGE_SIZE`. */
struct page_dir {
    unsigned count;
    uintptr_t va[FRAMES];
};

/* Each frame is an allocation of its own, so a write past a frame's end is
 * a fault the sanitizer sees and not a byte in the next frame. */
static unsigned char *frames[FRAMES];
static page_dir_t dir;

static unsigned char img_buf[IMAGE_MAX];
static uint32_t img_size;

/* The file the mapped loads name; only its address is used. */
static int file_token;
#define FILE_TOKEN ((struct vm_file *)(void *)&file_token)

/** Report that the loader accepted what its rules forbid, and abort. */
static void reject(const char *rule)
{
    static const char head[] = "elf64-fuzz: loader fault: ";
    unsigned long n = 0;

    while (rule[n] != '\0') {
        n++;
    }
    (void)write(2, head, sizeof(head) - 1);
    (void)write(2, rule, n);
    (void)write(2, "\n", 1);
    abort();
}

/** Whether the page at `va` lies in the user image window. */
static int in_window(uintptr_t va)
{
    return va >= USER_ADDR_MIN && va < USER_STACK_BASE;
}

/** The index of the frame that maps the page of `addr` in `d`, or -1. */
static int frame_of(const page_dir_t *d, uintptr_t addr)
{
    unsigned i;

    for (i = 0; i < d->count; i++) {
        if (d->va[i] == (addr & ~PAGE_MASK)) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * Map a frame of the pool at `virt_addr` and return its synthetic physical
 * address, or 0 when the pool is spent or the host allocation fails. A page
 * outside the window, or one already mapped, is a loader fault.
 */
uintptr_t alloc_user_page(page_dir_t *d, uintptr_t virt_addr)
{
    if ((virt_addr & PAGE_MASK) != 0 || !in_window(virt_addr)) {
        reject("a page mapped outside the user image window");
    }
    if (frame_of(d, virt_addr) >= 0) {
        reject("a page mapped twice");
    }
    if (d->count == FRAMES) {
        return 0;
    }
    if (frames[d->count] == 0) {
        frames[d->count] = malloc(PAGE_SIZE);
        if (frames[d->count] == 0) {
            return 0;
        }
    }
    d->va[d->count++] = virt_addr;
    return (uintptr_t)d->count * PAGE_SIZE;
}

/** The frame mapped at `addr`, or 0 for an unmapped page. */
uintptr_t paging_user_phys(page_dir_t *d, uintptr_t addr)
{
    int i = frame_of(d, addr);

    return i < 0 ? 0 : (uintptr_t)(i + 1) * PAGE_SIZE;
}

/** Change nothing: the harness does not model page permissions. */
void paging_set_exec(page_dir_t *d, uintptr_t addr, bool exec)
{
    (void)d;
    (void)addr;
    (void)exec;
}

/** Change nothing: the harness does not model page permissions. */
void paging_set_writable(page_dir_t *d, uintptr_t addr, bool writable)
{
    (void)d;
    (void)addr;
    (void)writable;
}

/** The host buffer of the frame at synthetic address `phys`, or NULL for an
 * address the pool has not given out. */
void *kmap_temp(uintptr_t phys)
{
    if (phys == 0 || (phys & PAGE_MASK) != 0 || phys / PAGE_SIZE > dir.count) {
        return NULL;
    }
    return frames[phys / PAGE_SIZE - 1];
}

/** Zero the frame at `phys`; -1 when it was not given out. */
int paging_zero_frame(uintptr_t phys)
{
    void *page = kmap_temp(phys);

    if (!page) {
        return -1;
    }
    k_memset(page, 0, PAGE_SIZE);
    return 0;
}

/** Nothing to undo: `kmap_temp` maps nothing. */
void kunmap_temp(void *va)
{
    (void)va;
}

/** No address space is modelled. */
void vm_init(vm_space_t *vm)
{
    (void)vm;
}

/** Accept a region that is nonempty, whole pages and inside the window. */
int vm_insert(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot, vm_type_t type)
{
    (void)vm;
    (void)prot;
    (void)type;
    if (start >= end || ((start | end) & PAGE_MASK) != 0 || !in_window(start) ||
        end > USER_STACK_BASE) {
        reject("a region outside the user image window");
    }
    return 0;
}

/** Accept a region of the file as `vm_insert` does. */
int vm_insert_file(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot, vm_type_t type,
                   struct vm_file *file, uint32_t pgoff)
{
    (void)pgoff;
    if (file != FILE_TOKEN) {
        reject("a region of a file the load was not given");
    }
    return vm_insert(vm, start, end, prot, type);
}

/** The kernel's memset, one byte at a time. */
void *k_memset(void *ptr, int c, size_t n)
{
    unsigned char *d = ptr;

    while (n--) {
        *d++ = (unsigned char)c;
    }
    return ptr;
}

/** The kernel's memcpy, one byte at a time. */
void *k_memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

/** Copy up to `count` image bytes at `off` to `dst`; the number copied. */
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

/** Whether the `len` bytes at file offset `off` lie in the input; no bytes
 * lie in it at any offset. */
static int in_file(uint64_t off, uint64_t len)
{
    return len == 0 || (off <= img_size && len <= img_size - off);
}

/**
 * Check that the bytes of the PT_LOAD `ph`, moved by `bias`, in every page
 * the load mapped are its file bytes and then zeros.
 */
static void check_bytes(const Elf64_Phdr *ph, uintptr_t bias)
{
    uintptr_t vaddr = (uintptr_t)ph->p_vaddr + bias;
    unsigned i;

    for (i = 0; i < dir.count; i++) {
        uintptr_t a = dir.va[i] > vaddr ? dir.va[i] : vaddr;
        uintptr_t end = dir.va[i] + PAGE_SIZE;

        if (end > vaddr + ph->p_memsz) {
            end = vaddr + ph->p_memsz;
        }
        for (; a < end; a++) {
            uint64_t j = a - vaddr;
            unsigned char want = j < ph->p_filesz ? img_buf[ph->p_offset + j] : 0;

            if (frames[i][a & PAGE_MASK] != want) {
                reject("a segment's bytes differ from its file bytes and zeros");
            }
        }
    }
}

/**
 * Read the input's ELF header into `eh` and check it, and the geometry a
 * load at `bias` through `file` that succeeded recorded in `out`, against it.
 */
static void check_header(Elf64_Ehdr *eh, const elf_image_t *out, uintptr_t bias,
                         struct vm_file *file)
{
    if (img_size < sizeof(*eh)) {
        reject("a header shorter than an ELF64 header");
    }
    k_memcpy(eh, img_buf, sizeof(*eh));
    if (eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_machine != EM_X86_64 ||
        (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) || (eh->e_type == ET_EXEC && bias != 0)) {
        reject("a header of another class, machine or type");
    }
    if (eh->e_phentsize != sizeof(Elf64_Phdr) ||
        !in_file(eh->e_phoff, (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr)) ||
        out->phnum != eh->e_phnum || out->phent != sizeof(Elf64_Phdr) || out->file != file) {
        reject("a program header table that differs from the file's");
    }
    if (out->entry != eh->e_entry + bias || !in_window(out->entry)) {
        reject("an entry point outside the user image window");
    }
}

/**
 * Check the record `seg` of the PT_LOAD `ph`, loaded at `bias` through
 * `file`, after a segment that ended at `prev_end`: the pages it covers lie
 * in the window, after `prev_end`, and are either all mapped, for a copied
 * segment, or map the file's pages, for one mapped from `file`.
 */
static void check_segment(const Elf64_Phdr *ph, const elf_seg_t *seg, uintptr_t bias,
                          struct vm_file *file, uintptr_t prev_end)
{
    uintptr_t vaddr = (uintptr_t)ph->p_vaddr + bias;
    uintptr_t vend = vaddr + (uintptr_t)ph->p_memsz;
    uintptr_t a;

    if (vaddr < bias || vend < vaddr || ph->p_filesz > ph->p_memsz ||
        !in_file(ph->p_offset, ph->p_filesz)) {
        reject("a segment whose file bytes lie past the end of the file");
    }
    if (!in_window(vaddr) || vend > USER_STACK_BASE) {
        reject("a segment outside the user image window");
    }
    if (seg->start != (vaddr & ~PAGE_MASK) || seg->end != ((vend + PAGE_MASK) & ~PAGE_MASK) ||
        seg->start < prev_end || seg->flags != ph->p_flags) {
        reject("a segment record that differs from its PT_LOAD");
    }
    if (seg->file_end != seg->start) {
        if (file == NULL || ((vaddr ^ ph->p_offset) & PAGE_MASK) != 0 ||
            seg->file_end != ((vaddr + ph->p_filesz + PAGE_MASK) & ~PAGE_MASK) ||
            seg->pgoff != ph->p_offset / PAGE_SIZE) {
            reject("a mapped segment whose file pages differ from its PT_LOAD");
        }
        return;
    }
    for (a = seg->start; a < seg->end; a += PAGE_SIZE) {
        if (frame_of(&dir, a) < 0) {
            reject("a copied segment with a page left unmapped");
        }
    }
}

/**
 * Check a load of the input at `bias` that succeeded, with `file` as it was
 * given, against the headers read from the input here.
 */
static void check_image(const elf_image_t *out, uintptr_t bias, struct vm_file *file)
{
    Elf64_Ehdr eh;
    uintptr_t prev_end = 0, table_end;
    int k = 0, table_in = 0;
    unsigned n;

    check_header(&eh, out, bias, file);
    table_end = out->phdr_vaddr + (uintptr_t)eh.e_phnum * sizeof(Elf64_Phdr);
    for (n = 0; n < eh.e_phnum; n++) {
        Elf64_Phdr ph;
        const elf_seg_t *seg;

        k_memcpy(&ph, img_buf + eh.e_phoff + (uint64_t)n * sizeof(ph), sizeof(ph));
        if (ph.p_type != PT_LOAD) {
            continue;
        }
        if (k >= out->nsegs) {
            reject("a PT_LOAD without a segment record");
        }
        seg = &out->segs[k++];
        check_segment(&ph, seg, bias, file, prev_end);
        check_bytes(&ph, bias);
        if (out->phdr_vaddr >= seg->start && table_end <= seg->end) {
            table_in = 1;
        }
        prev_end = seg->end;
    }
    if (k != out->nsegs || out->image_end != prev_end) {
        reject("a segment record without a PT_LOAD");
    }
    if (!table_in) {
        reject("a program header table outside every segment");
    }
}

/** Load the input at `bias` into an empty model, through `file` when it is
 * given, and check what a load that succeeded recorded. */
static void load(uintptr_t bias, struct vm_file *file)
{
    elf_image_t out;

    k_memset(&dir, 0, sizeof(dir));
    k_memset(&out, 0, sizeof(out));
    if (elf_load_image_at(image_read, 0, &dir, bias, file, &out) != 0) {
        return;
    }
    check_image(&out, bias, file);
    if (file && elf_add_vmas(NULL, &out) != 0) {
        reject("a region the model refused");
    }
}

/** Check a layout `elf_read_layout` read, with its interpreter when
 * `interp` is set. */
static void check_layout(const elf_layout_t *l, int interp)
{
    unsigned i;

    if (l->lo > l->hi || ((l->lo | l->hi) & PAGE_MASK) != 0 || l->hi > USER_ADDR_MAX ||
        (l->type != ET_EXEC && l->type != ET_DYN)) {
        reject("a layout outside the user address range");
    }
    if (!interp || l->interp[0] == '\0') {
        return;
    }
    for (i = 0; i < ELF_INTERP_MAX && l->interp[i] != '\0'; i++) {
    }
    if (i == ELF_INTERP_MAX || l->interp[0] != '/') {
        reject("an interpreter that is not an absolute path within the limit");
    }
}

/** Run the input, cut to `IMAGE_MAX` bytes, through each path a program
 * takes into the kernel. */
int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size)
{
    elf_layout_t layout;
    unsigned char *copy;
    uintptr_t bias = 0;

    if (size > IMAGE_MAX) {
        size = IMAGE_MAX;
    }
    k_memcpy(img_buf, data, (size_t)size);
    img_size = (uint32_t)size;

    copy = malloc(size ? size : 1);
    if (copy) {
        k_memcpy(copy, data, (size_t)size);
        if (elf_check(copy, img_size)) {
            const Elf_Phdr *ph = elf_program_headers(copy);
            volatile uint32_t sum = 0;
            uint16_t i;

            for (i = 0; i < elf_program_header_count(copy); i++) {
                sum += ph[i].p_type;
            }
        }
        free(copy);
    }

    load(0, NULL);
    if (elf_read_layout(image_read, 0, 1, &layout) == 0) {
        check_layout(&layout, 1);
        if (layout.type == ET_DYN && layout.lo < ELF_ET_DYN_BASE) {
            bias = ELF_ET_DYN_BASE - layout.lo;
        }
        load(bias, FILE_TOKEN);
    }
    if (elf_read_layout(image_read, 0, 0, &layout) == 0 && layout.type == ET_DYN) {
        check_layout(&layout, 0);
        load(USER_MMAP_BASE - (layout.hi - layout.lo) - layout.lo, FILE_TOKEN);
    }
    return 0;
}
