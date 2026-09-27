/**
 * Check the core file's layout against an address space that changes while
 * the file is written, and the wait for the process's other threads.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * `proc/coredump.c` is included whole. The writer keeps the file in memory,
 * holes as zeros. On a chosen call it replaces the address space's array of
 * regions with a longer one and frees the old, as another process sharing
 * the address space can with an mmap while the writer sleeps: the program
 * headers must still describe the regions as they were taken, each once,
 * and nothing may be read from the freed array, which AddressSanitizer
 * judges. The wait for the other threads is a model that ends them: it must
 * come before anything is written, and the regions it leaves are the ones
 * the file records; an execve waiting for the dumping thread ends it at once.
 */
#include "proc/coredump.c"

extern void *calloc(unsigned long count, unsigned long size);
extern void *realloc(void *ptr, unsigned long size);
extern void free(void *ptr);
extern int printf(const char *format, ...);

static process_t self;
static process_t sibling;
static struct tgroup group;
static struct mm space;
process_t *current_proc = &self;
process_t *proc_list = &self;
static uint8_t *file;
static uint32_t file_len;
static int writes, grow_at, waits, writes_at_wait, grows_at_wait, failures;

#define CHECK(c, detail)                                 \
    do {                                                 \
        if (!(c)) {                                      \
            printf("coredump-test: fail: %s\n", detail); \
            failures++;                                  \
        }                                                \
    } while (0)

void *kmalloc(size_t size)
{
    return calloc(1, size);
}

void kfree(void *p)
{
    free(p);
}

void fpu_save(void *area)
{
    (void)area;
}

void fpu_to_linux(const void *area, uint8_t *out)
{
    (void)area;
    k_memset(out, 0, FPU_LINUX_SIZE);
}

/** The pages the process has: every page of a region but the second page
 * of one that is longer. */
uintptr_t paging_user_phys(struct page_dir *dir, uint32_t addr)
{
    uint32_t i;

    (void)dir;
    for (i = 0; i < space.vm.count; i++) {
        if (addr >= space.vm.vmas[i].start && addr < space.vm.vmas[i].end) {
            return addr - space.vm.vmas[i].start == PAGE_SIZE ? 0 : 0x100000u;
        }
    }
    return 0;
}

/** A page's bytes are the low byte of its page number. */
int paging_access(struct mm *mm, uint32_t addr, void *buf, uint32_t len, int write)
{
    (void)mm;
    (void)write;
    k_memset(buf, (int)((addr >> 12) & 0xFFu), len);
    return 0;
}

/** Put `count` regions of a page each, readable and writable, from
 * 0x08048000 a page apart, the third of two pages and the fourth
 * inaccessible, in the address space's array. */
static void regions_set(uint32_t count)
{
    uint32_t i;
    uint32_t at = 0x08048000u;

    free(space.vm.vmas);
    space.vm.vmas = calloc(count + 1u, sizeof(vma_t));
    space.vm.cap = count + 1u;
    space.vm.count = count;
    for (i = 0; i < count; i++) {
        space.vm.vmas[i].start = at;
        space.vm.vmas[i].end = at + (i == 2 ? 2u : 1u) * PAGE_SIZE;
        space.vm.vmas[i].prot = i == 3 ? 0u : VM_PROT_READ | VM_PROT_WRITE;
        space.vm.vmas[i].type = VM_ANON;
        at = space.vm.vmas[i].end + PAGE_SIZE;
    }
}

/** Grow the array by one region, into a new array, freeing the old one. */
static void regions_grow(void)
{
    uint32_t count = space.vm.count;
    vma_t *more = calloc(count + 2u, sizeof(vma_t));

    k_memcpy(more, space.vm.vmas, count * sizeof(vma_t));
    more[count].start = 0x70000000u;
    more[count].end = 0x70001000u;
    more[count].prot = VM_PROT_READ;
    more[count].type = VM_ANON;
    free(space.vm.vmas);
    space.vm.vmas = more;
    space.vm.cap = count + 2u;
    space.vm.count = count + 1u;
}

/** The writer: keep the bytes, holes as zeros, growing the regions on the
 * call `grow_at`. */
static int32_t put(void *ctx, const void *buf, uint32_t len)
{
    uint8_t *more;

    (void)ctx;
    if (++writes == grow_at) {
        regions_grow();
    }
    more = realloc(file, file_len + len);
    if (!more) {
        return -ENOMEM;
    }
    file = more;
    if (buf) {
        k_memcpy(file + file_len, buf, len);
    } else {
        k_memset(file + file_len, 0, len);
    }
    file_len += len;
    return 0;
}

/** The wait for the other threads: note that it came and when, and end
 * them, one of which finished an mmap first. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    CHECK(wq == &group.gone, "the dumping thread waits on its process's queue of ends");
    waits++;
    writes_at_wait = writes;
    grows_at_wait = cond(arg);
    if (!cond(arg)) {
        regions_grow();
        group.live = 1;
    }
    CHECK(cond(arg), "the wait ends when the other threads have");
    return 0;
}

static uint32_t get16(uint32_t off)
{
    uint16_t v;

    k_memcpy(&v, file + off, sizeof(v));
    return v;
}

static uint32_t get32(uint32_t off)
{
    uint32_t v;

    k_memcpy(&v, file + off, sizeof(v));
    return v;
}

/** Write a core file of the model with `live` threads and an execve
 * waiting when `exec`, the regions growing at write `grow`; its result. */
static int32_t dump(uint32_t live, int exec, int grow)
{
    static const uint8_t notes[20] = {5};

    free(file);
    file = NULL;
    file_len = 0;
    writes = waits = 0;
    writes_at_wait = -1;
    grow_at = grow;
    group.live = live;
    group.exec_task = exec ? &sibling : NULL;
    return core_write(put, NULL, notes, sizeof(notes), 0xFFFFFFFFu);
}

/** Whether program header `i` of the file is a load of the model's region
 * `r` as it was set, at file offset `off`, and the offset after it. */
static int load_is(uint32_t i, uint32_t r, uint32_t *off)
{
    uint32_t ph = 52u + 32u * i;
    uint32_t start = 0x08048000u + 2u * PAGE_SIZE * r + (r > 2 ? PAGE_SIZE : 0u);
    uint32_t mem = (r == 2 ? 2u : 1u) * PAGE_SIZE;
    uint32_t filesz = r == 3 ? 0u : mem;
    int ok = get32(ph) == PT_LOAD && get32(ph + 4) == *off && get32(ph + 8) == start &&
             get32(ph + 16) == filesz && get32(ph + 20) == mem &&
             get32(ph + 24) == (r == 3 ? 0u : (uint32_t)(PF_R | PF_W));

    *off += filesz;
    return ok;
}

/**
 * The regions are taken once: the array grows and is freed at the first
 * program header of a load, and the file still has four loads, with the
 * pages of the three readable regions, the missing page a hole, and none of
 * the inaccessible one.
 */
static void regions_taken_once(void)
{
    uint32_t off = 4096u;
    uint32_t i;

    regions_set(4);
    CHECK(dump(1, 0, 3) == 0, "the file is written while the regions grow");
    CHECK(get16(44) == 5, "the header counts the note and the four regions taken");
    for (i = 0; i < 4; i++) {
        CHECK(load_is(i + 1u, i, &off), "each load describes its region as it was taken");
    }
    CHECK(file_len == off, "the file ends with the last readable region's pages");
    CHECK(file[4096] == 0x48 && file[8192] == 0x4A && file[12288] == 0x4C,
          "a page the process has is written");
    CHECK(file[16384] == 0 && file[20479] == 0, "and a page it never touched is a hole");
}

/**
 * The file is written after the other threads end: none is written before
 * the wait, and a region one of them mapped before it ended is in the file.
 * An execve waiting for the dumping thread ends the wait at once.
 */
static void threads_end_first(void)
{
    regions_set(4);
    CHECK(dump(2, 0, 0) == 0 && waits == 1, "the dumping thread waits for the other threads");
    CHECK(writes_at_wait == 0 && !grows_at_wait, "before anything is written");
    CHECK(get16(44) == 6, "and the file has the region a thread mapped before it ended");
    regions_set(4);
    CHECK(dump(2, 1, 0) == 0 && waits == 1 && group.live == 2 && get16(44) == 5,
          "an execve waiting for the dumping thread ends the wait at once");
}

int main(void)
{
    self.pid = self.tgid = 3;
    self.tg = &group;
    self.mm = &space;
    self.state = PROC_RUNNING;
    group.leader = &self;
    sibling.tg = &group;
    regions_taken_once();
    threads_end_first();
    free(file);
    free(space.vm.vmas);
    if (failures) {
        return 1;
    }
    printf("coredump-test: pass: the regions taken once and the wait for the threads\n");
    return 0;
}
