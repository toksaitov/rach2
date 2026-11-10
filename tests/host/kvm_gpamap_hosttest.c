/**
 * Check KVM's maps of a guest's pages into a program of the host, gpamap.c,
 * over a model of the VM's slots and of its maker's memory.
 *
 * host-cflags: -DRACH2_HOST_X86_64 -DRACH2_HOST_CPU
 *
 * gpamap.c and the change notice's mm/notify.c are the kernel's, included
 * whole. The slots are a table here, each a range of guest frames at an
 * address of the maker's memory, read-only when its flag says so; the
 * maker's memory is a table of frames by page, 0 for a page that cannot be
 * shared, and the frames' references are counted.
 *
 * The rows: a map of two runs, one cut by a slot's end, holding a reference
 * to each frame in the runs' order and counting its maps and pages; the
 * refusals, an empty, unaligned, too wide or too large run, a page in no
 * slot, in a read-only slot or not shareable, and every page of a maker
 * whose address space has ended, each with nothing held and no range on
 * the notice; a change of the maker's memory beside the runs leaving the
 * map, and one that meets a run taking it back through the owner's revoke
 * once, with every reference let go and its frames answering 0; the end of
 * the maker's memory, a slot's change over a run and the VM's end each taking
 * maps back; and a free without a revoke, after which nothing is held.
 */
#include "kvm/gpamap.c"
#include "mm/notify.c"

extern int printf(const char *format, ...);
extern void *malloc(unsigned long n);
extern void *calloc(unsigned long n, unsigned long size);
extern void free(void *p);

static int failures;

/** Count and print a failure when `condition` is false. */
#define CHECK(condition, detail)                                 \
    do {                                                         \
        if (!(condition)) {                                      \
            printf("kvm-gpamap-hosttest: failed: %s\n", detail); \
            failures++;                                          \
        }                                                        \
    } while (0)

/* ---- the heap, counted ---- */

static int live;

void *kmalloc(size_t size)
{
    live++;
    return malloc(size ? size : 1);
}

void *kzalloc(size_t size)
{
    live++;
    return calloc(1, size ? size : 1);
}

void kfree(void *p)
{
    if (p) {
        live--;
        free(p);
    }
}

/* ---- the maker's memory: a page's frame, or 0 for one not shareable ---- */

#define MEM_BASE  0x400000u
#define MEM_PAGES 64u
#define FRAME0    0x10000000u

static uintptr_t mem_frame[MEM_PAGES];
static int refs[MEM_PAGES];
static struct mm maker;
static char maker_dir; /* stands for the maker's page directory */

/** The page of the model's memory that frame `phys` is, or -1. */
static int frame_page(uintptr_t phys)
{
    uint32_t i;

    for (i = 0; i < MEM_PAGES; i++) {
        if (mem_frame[i] == phys) {
            return (int)i;
        }
    }
    return -1;
}

uintptr_t paging_user_ram_frame(struct mm *mm, uintptr_t va)
{
    if (mm != &maker || va < MEM_BASE || va >= MEM_BASE + MEM_PAGES * 4096u) {
        return 0;
    }
    return mem_frame[(va - MEM_BASE) / 4096u];
}

void frame_ref(uintptr_t phys)
{
    int i = frame_page(phys);

    CHECK(i >= 0, "a reference to a frame of the model's memory");
    if (i >= 0) {
        refs[i]++;
    }
}

uint32_t frame_unref(uintptr_t phys)
{
    int i = frame_page(phys);

    CHECK(i >= 0 && refs[i] > 0, "a reference let go that was taken");
    if (i >= 0 && refs[i] > 0) {
        refs[i]--;
    }
    return i >= 0 ? (uint32_t)refs[i] : 0;
}

/** References held over the whole of the model's memory. */
static int held(void)
{
    int n = 0;
    uint32_t i;

    for (i = 0; i < MEM_PAGES; i++) {
        n += refs[i];
    }
    return n;
}

/* ---- the VM: its slots, its maker and KVM's counts ---- */

struct model_slot {
    uint64_t base_gfn, npages, hva;
    uint32_t flags;
};

static struct model_slot slots[4];
static uint32_t nslots;
static long counts[KVM_COUNT_TOTAL];

int kvm_slot_find(struct kvm_vm *vm, uint64_t gfn, struct kvm_slot_view *out)
{
    uint32_t i;

    (void)vm;
    for (i = 0; i < nslots; i++) {
        if (gfn >= slots[i].base_gfn && gfn < slots[i].base_gfn + slots[i].npages) {
            out->base_gfn = slots[i].base_gfn;
            out->npages = slots[i].npages;
            out->flags = slots[i].flags;
            out->id = i;
            out->hva = slots[i].hva + (gfn - slots[i].base_gfn) * 4096u;
            out->dirty = NULL;
            return 1;
        }
    }
    return 0;
}

uint64_t kvm_tdp_frames(void)
{
    return (uint64_t)1 << 36;
}

struct mm *kvm_vm_mm(struct kvm_vm *vm)
{
    (void)vm;
    return &maker;
}

void kvm_count(enum kvm_resource resource, int change)
{
    counts[resource] += change;
}

/* ---- the owner's revoke ---- */

static int revokes;
static const char *seen; /* the owner the last revoke was for */
static int seen_held;    /* the references held while it ran */

/** Count a revoke, its owner, and the references still held during it. */
static void revoke(void *owner)
{
    revokes++;
    seen = (const char *)owner;
    seen_held = held();
}

/** The model: two slots of RAM side by side in the guest, at two places of
 * the maker's memory, then a read-only one, each page its own frame. */
static void reset(struct kvm_vm *vm)
{
    uint32_t i;

    for (i = 0; i < MEM_PAGES; i++) {
        mem_frame[i] = FRAME0 + i * 4096u;
        refs[i] = 0;
    }
    slots[0].base_gfn = 0x100;
    slots[0].npages = 8;
    slots[0].hva = MEM_BASE + 16 * 4096u;
    slots[0].flags = 0;
    slots[1].base_gfn = 0x108;
    slots[1].npages = 8;
    slots[1].hva = MEM_BASE;
    slots[1].flags = 0;
    slots[2].base_gfn = 0x200;
    slots[2].npages = 4;
    slots[2].hva = MEM_BASE + 32 * 4096u;
    slots[2].flags = KVM_MEM_READONLY;
    nslots = 3;
    revokes = 0;
    seen = NULL;
    vm->gpamap = NULL;
    maker.dir = (struct page_dir *)(void *)&maker_dir;
}

/** Make a map of `n` runs for the owner `who`, or say why not. */
static int make(struct kvm_vm *vm, const struct kvm_gpa_run *runs, uint32_t n, const char *who,
                struct kvm_gpamap **m)
{
    return kvm_gpamap_new(vm, runs, n, revoke, (void *)(uintptr_t)who, m);
}

/** A map of two runs, the first cut by its slot's end, in order. */
static void map_rows(struct kvm_vm *vm)
{
    static const struct kvm_gpa_run runs[] = {{0x106000, 4}, {0x101000, 1}};
    struct kvm_gpamap *m;

    reset(vm);
    CHECK(make(vm, runs, 2, "a", &m) == 0 && m != NULL, "a map of two runs over two slots");
    if (!m) {
        return;
    }
    /* Frames 0x106 and 0x107 are pages 22 and 23 of slot 0's memory, 0x108
     * and 0x109 pages 0 and 1 of slot 1's, and 0x101 page 17. */
    CHECK(kvm_gpamap_frame(m, 0) == mem_frame[22] && kvm_gpamap_frame(m, 1) == mem_frame[23] &&
              kvm_gpamap_frame(m, 2) == mem_frame[0] && kvm_gpamap_frame(m, 3) == mem_frame[1] &&
              kvm_gpamap_frame(m, 4) == mem_frame[17],
          "each page's frame through its slot, in the runs' order");
    CHECK(kvm_gpamap_frame(m, 5) == 0, "no frame past the map's end");
    CHECK(m->nruns == 3, "the first run cut where its slot ends");
    CHECK(held() == 5 && refs[22] == 1 && refs[17] == 1, "a reference to each frame");
    CHECK(counts[KVM_COUNT_GPA_MAP] == 1 && counts[KVM_COUNT_GPA_MAP_PAGE] == 5,
          "the map and its pages counted");
    CHECK(maker.notify == &m->note, "the map's range on the maker's change notice");
    kvm_gpamap_free(m);
    CHECK(revokes == 0, "a free calls no revoke");
    CHECK(held() == 0 && maker.notify == NULL, "a free lets every frame and the notice go");
    CHECK(counts[KVM_COUNT_GPA_MAP] == 0 && counts[KVM_COUNT_GPA_MAP_PAGE] == 0,
          "the counts back after the free");
}

/** Each refusal, with nothing held after it. */
static void refusal_rows(struct kvm_vm *vm)
{
    static const struct {
        struct kvm_gpa_run run;
        int want;
        const char *what;
    } rows[] = {
        {{0x100000, 0}, -EINVAL, "an empty run"},
        {{0x100800, 1}, -EINVAL, "a run not page-aligned"},
        {{(uint64_t)1 << 48, 1}, -EINVAL, "a run past the guest-physical width"},
        {{0x100000, 0x80000000ull}, -EINVAL, "more pages than a map holds"},
        {{0x300000, 1}, -EFAULT, "a page in no slot"},
        {{0x10e000, 4}, -EFAULT, "a run that leaves the slots"},
        {{0x200000, 1}, -EFAULT, "a page in a read-only slot"},
        {{0x103000, 1}, -EFAULT, "a page that cannot be shared"},
    };
    static const struct kvm_gpa_run live_run = {0x101000, 2};
    struct kvm_gpamap *m;
    uint32_t i;
    int r;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        reset(vm);
        mem_frame[19] = 0; /* gfn 0x103: page 19 of slot 0's memory */
        r = make(vm, &rows[i].run, 1, "a", &m);
        CHECK(r == rows[i].want && m == NULL, rows[i].what);
        CHECK(held() == 0 && maker.notify == NULL && live == 0 && vm->gpamap == NULL,
              "nothing held after a refusal");
    }
    reset(vm);
    CHECK(make(vm, &rows[1].run, 0, "a", &m) == -EINVAL, "no runs");
    reset(vm);
    CHECK(make(vm, &live_run, 1, "a", &m) == 0, "a map of the run the next row refuses");
    kvm_gpamap_free(m);
    maker.dir = NULL; /* mm_put's mark of an address space that has ended */
    r = make(vm, &live_run, 1, "a", &m);
    CHECK(r == -EFAULT && m == NULL, "a page of a maker whose address space has ended");
    CHECK(held() == 0 && maker.notify == NULL && live == 0 && vm->gpamap == NULL,
          "nothing held and no range on the notice after it");
    reset(vm);
}

/** Changes of the maker's memory: beside the runs, then over one. */
static void notice_rows(struct kvm_vm *vm)
{
    static const struct kvm_gpa_run run = {0x101000, 2};
    struct kvm_gpamap *m;

    reset(vm);
    CHECK(make(vm, &run, 1, "a", &m) == 0, "a map for the notice's rows");
    if (!m) {
        return;
    }
    mmn_notify(&maker, MMN_UNMAP, MEM_BASE + 19 * 4096u, MEM_BASE + 20 * 4096u);
    CHECK(revokes == 0 && kvm_gpamap_frame(m, 0) == mem_frame[17],
          "a change beside the runs leaves the map");
    mmn_notify(&maker, MMN_COW, MEM_BASE + 18 * 4096u, MEM_BASE + 19 * 4096u);
    CHECK(revokes == 1 && seen_held == 2,
          "a copy-on-write break over a run calls the revoke while the frames are held");
    CHECK(kvm_gpamap_frame(m, 0) == 0 && held() == 0 && maker.notify == NULL && vm->gpamap == NULL,
          "the taken-back map holds nothing and answers 0");
    CHECK(counts[KVM_COUNT_GPA_MAP] == 0 && counts[KVM_COUNT_GPA_MAP_PAGE] == 0,
          "the counts back after the taking back");
    kvm_gpamap_free(m);
    CHECK(revokes == 1 && live == 0, "a free after the taking back lets the record go alone");

    reset(vm);
    CHECK(make(vm, &run, 1, "a", &m) == 0, "a map for the release row");
    mmn_release(&maker);
    CHECK(revokes == 1 && held() == 0, "the end of the maker's memory takes the map back");
    kvm_gpamap_free(m);
}

/** A slot's change over a run, and the VM's end. */
static void vm_rows(struct kvm_vm *vm)
{
    static const struct kvm_gpa_run a = {0x101000, 1}, b = {0x109000, 1};
    struct kvm_gpamap *ma, *mb;

    reset(vm);
    CHECK(make(vm, &a, 1, "a", &ma) == 0 && make(vm, &b, 1, "b", &mb) == 0, "two maps of one VM");
    kvm_gpamap_zap(vm, 0x108, 8);
    CHECK(revokes == 1 && seen[0] == 'b' && kvm_gpamap_frame(mb, 0) == 0 &&
              kvm_gpamap_frame(ma, 0) == mem_frame[17],
          "a slot's change takes back the map over it alone");
    kvm_gpamap_vm_end(vm);
    CHECK(revokes == 2 && seen[0] == 'a' && held() == 0 && vm->gpamap == NULL &&
              maker.notify == NULL,
          "the VM's end takes back every map");
    kvm_gpamap_free(ma);
    kvm_gpamap_free(mb);
    kvm_gpamap_free(NULL);
    CHECK(live == 0 && revokes == 2, "every record freed, with no revoke more");
}

int main(void)
{
    struct kvm_vm *vm = (struct kvm_vm *)calloc(1, sizeof(*vm));

    if (!vm) {
        return 1;
    }
    map_rows(vm);
    refusal_rows(vm);
    notice_rows(vm);
    vm_rows(vm);
    free(vm);
    if (failures) {
        printf("kvm-gpamap-hosttest: %d failure(s)\n", failures);
        return 1;
    }
    printf("kvm-gpamap-hosttest: ok\n");
    return 0;
}
