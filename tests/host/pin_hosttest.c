/**
 * Host test of pins (mm/pin.c), the lock bits of the regions (mm/vm.c) and
 * RLIMIT_MEMLOCK, and the windows of the MMIO region onto PCI BARs
 * (mm/paging.c and pci.c), with i686's `cpu.h`; pin64_hosttest.c runs the
 * same rows with x86-64's.
 *
 * host-deps: rach2/sys/mm/vm.c rach2/sys/mm/frame.c rach2/sys/mm/notify.c
 * host-deps: rach2/sys/mm/pin.c
 * host-deps: rach2/sys/pci.c rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 * host-cflags: -U__UINTPTR_TYPE__ -D__UINTPTR_TYPE__=unsigned
 * host-cflags: -U__INTPTR_TYPE__ -D__INTPTR_TYPE__=int
 * host-cflags: -U__SIZE_TYPE__ -D__SIZE_TYPE__=unsigned
 * host-cflags: -U__PTRDIFF_TYPE__ -D__PTRDIFF_TYPE__=int
 *
 * mm_model.h includes paging.c whole over a model of the page tables, with
 * frames of host memory. The rows: a pin of a user range brings its pages
 * in and pins their frames, charged to VmPin; a pin past RLIMIT_MEMLOCK,
 * of an empty, wrapping or unmapped range, or a write pin of a read-only
 * one, is refused and pins nothing, and a privileged process has no limit;
 * a pin of an object's frames refuses a frame the allocator does not count;
 * a fork copies a pinned private page for the child, and a pin of a page
 * shared copy-on-write makes it the parent's own first; an unmap of a
 * pinned page and the end of the address space tell the holder once and
 * drop the frames and the charge, while a pin charged to no one stays.
 * Device pages accepted by VFIO carry no RAM charge; mixed ranges,
 * protection, revocation and cleanup preserve that distinction.
 * mlock's regions split and merge, count in VmLck, lose their lock in a
 * fork's copy, and a locked stack grows only within RLIMIT_MEMLOCK. The
 * MMIO rows map a display's BAR in windows and free them: on i686 a 16 MiB
 * BAR at 0xFD000000 in 4 KiB pages; on x86-64 a 4 GiB BAR at 14 TiB in
 * windows of 2 MiB pages, and a window not aligned to 2 MiB in both sizes.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "mm_model.h"

#include "pci.h"

#define VA (WIN_BASE)
#define VB (WIN_BASE + SPAN)
#define VS (WIN_BASE + 3 * SPAN) /* the top of the stack */
#define P  ((uintptr_t)PAGE_SIZE)
#define RW (VM_PROT_READ | VM_PROT_WRITE)

#ifdef RACH2_HOST_X86_64
#define NAME "pin64-hosttest"
#else
#define NAME "pin-hosttest"
#endif

/* ---- a PCI function's configuration space: its command and its BARs ---- */

static uint16_t cfg_command;
static uint32_t cfg_bar[6];
/* The size each memory BAR decodes, 0 for the upper half of a 64-bit one. */
static uint64_t cfg_len[6];
/* Set when a BAR was sized while memory decoding was on. */
static int sized_decoding;

/** Make BAR `i` decode `len` bytes at `phys` with the low bits `flags`. */
static void cfg_set_bar(int i, uint64_t phys, uint64_t len, uint32_t flags)
{
    cfg_bar[i] = (uint32_t)phys | flags;
    cfg_len[i] = len;
    if ((flags & 0x6u) == 0x4u) {
        cfg_bar[i + 1] = (uint32_t)(phys >> 32);
        cfg_len[i + 1] = 0;
    }
}

/** The command register, or a BAR. */
uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    if (offset >= PCI_BAR0 && offset < PCI_BAR0 + 24) {
        return cfg_bar[(offset - PCI_BAR0) / 4];
    }
    return offset == PCI_COMMAND ? cfg_command : 0;
}

/** A BAR takes its address, or all ones for its mask: of a low half the
 * size's low bits and its flags, of an upper half the size's high bits. */
void pci_write_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint32_t value)
{
    int i = (offset - PCI_BAR0) / 4;

    (void)bus;
    (void)device;
    (void)function;
    if (offset < PCI_BAR0 || offset >= PCI_BAR0 + 24) {
        return;
    }
    if (value != 0xFFFFFFFFu) {
        cfg_bar[i] = value;
        return;
    }
    sized_decoding |= (cfg_command & PCI_CMD_MEMORY_SPACE) != 0;
    if (cfg_len[i] != 0) {
        cfg_bar[i] = ((uint32_t)~(cfg_len[i] - 1) & 0xFFFFFFF0u) | (cfg_bar[i] & 0xFu);
    } else if (i > 0) {
        cfg_bar[i] = (uint32_t)(~(cfg_len[i - 1] - 1) >> 32);
    }
}

/** The command register. */
void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint16_t value)
{
    (void)bus;
    (void)device;
    (void)function;
    if (offset == PCI_COMMAND) {
        cfg_command = value;
    }
}

/** Nothing. */
void pci_write_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)offset;
    (void)value;
}

/** Nothing: no row enables MSI. */
void pci_msi_message(uint8_t vector, uint32_t *address, uint32_t *data)
{
    (void)vector;
    *address = 0;
    *data = 0;
}

/* ---- the holder of the pins ---- */

/* The releases the holder was told of, and the pin of the last. */
static uint32_t released;
static struct pin *released_pin;

/** Count the release in the holder's counter and the test's. */
static void on_release(struct pin *pin, void *owner)
{
    (*(uint32_t *)owner)++;
    released++;
    released_pin = pin;
}

static uint32_t holder_count;
static const struct pin_holder holder = {on_release, &holder_count};

/** Make A unprivileged with RLIMIT_MEMLOCK `pages` pages, or privileged
 * with no limit when `pages` is 0xFFFFFFFF. */
static void memlock(uint32_t pages)
{
    proc_a.euid = pages == 0xFFFFFFFFu ? 0 : 1000;
    proc_a.rlim[RLIM_MEMLOCK].cur =
        pages == 0xFFFFFFFFu ? RLIM_INFINITY : (unsigned long)pages * PAGE_SIZE;
}

/* ---- the rows ---- */

static struct pin *p_range, *p_cow;

/** A pin of a user range brings its pages in and pins their frames; what is
 * refused pins nothing. */
static void pin_rows(void)
{
    struct pin *p;
    uint32_t i;
    int ok = 1;

    check(vm_insert(&mm_a.vm, VA, VA + 8 * P, RW, VM_ANON) == 0 &&
              vm_insert(&mm_a.vm, VA + 16 * P, VA + 17 * P, VM_PROT_READ, VM_ANON) == 0,
          "the regions are made");
    check(pin_user(&mm_a, VA + 8, 3 * P - 8, PIN_WRITE, &holder, &p_range) == 0 &&
              pin_count(p_range) == 3 && pin_live(p_range) && mm_a.pinned == 3,
          "a pin of three pages from inside the first is made and charged to VmPin");
    for (i = 0; i < 3; i++) {
        uintptr_t phys = pin_phys(p_range, i);
        pte_t e = pte_of(&dir_a, VA + i * P);

        ok &= phys != 0 && PTE_ADDR(e) == phys && (e & PTE_W) &&
              frame_word(phys) == 1 + FRAME_PIN_BIAS;
    }
    check(ok, "each page is in, writable, and its frame mapped once and pinned");
    check(pin_phys(p_range, 3) == 0, "a page past the pin's end has no frame");

    check(pin_user(&mm_a, VA, 0, 0, &holder, &p) == -EINVAL, "an empty range is refused");
    check(pin_user(&mm_a, (uintptr_t)0 - P, 2 * P, 0, &holder, &p) == -EINVAL,
          "a wrapping range is refused");
    check(pin_user(&mm_a, 0, P, 0, &holder, &p) == -EFAULT, "a range below user space is refused");
    check(pin_user(&mm_a, VA + 7 * P, 2 * P, 0, &holder, &p) == -EFAULT && mm_a.pinned == 3 &&
              (pte_of(&dir_a, VA + 7 * P) == 0 ||
               frame_word(PTE_ADDR(pte_of(&dir_a, VA + 7 * P))) == 1),
          "a range running into no region is refused and pins nothing");
    check(pin_user(&mm_a, VA + 16 * P, P, PIN_WRITE, &holder, &p) == -EFAULT,
          "a write pin of a read-only region is refused");
    check(pin_user(&mm_a, VA + 16 * P, P, 0, &holder, &p) == 0 && mm_a.pinned == 4,
          "a read pin of a read-only region is made");
    unpin(p);
    check(mm_a.pinned == 3, "unpin drops the charge");

    memlock(4);
    check(pin_user(&mm_a, VA + 3 * P, 2 * P, 0, &holder, &p) == -ENOMEM && mm_a.pinned == 3 &&
              pte_of(&dir_a, VA + 3 * P) == 0,
          "a pin past RLIMIT_MEMLOCK answers ENOMEM and brings nothing in");
    check(pin_user(&mm_a, VA + 3 * P, P, 0, &holder, &p) == 0 && mm_a.pinned == 4,
          "a pin up to RLIMIT_MEMLOCK is made");
    unpin(p);
    memlock(0);
    check(pin_user(&mm_a, VA + 3 * P, P, 0, &holder, &p) == -ENOMEM,
          "with RLIMIT_MEMLOCK 0 a pin answers ENOMEM");
    memlock(0xFFFFFFFFu);
    proc_a.rlim[RLIM_MEMLOCK].cur = 0;
    check(pin_user(&mm_a, VA + 3 * P, P, 0, &holder, &p) == 0,
          "a privileged process pins past RLIMIT_MEMLOCK");
    unpin(p);
    memlock(0xFFFFFFFFu);
    check(mm_a.pinned == 3 && holder_count == 0, "the refusals left the charge and told no one");
}

/** A pin of an object's frames refuses a frame the allocator does not count
 * and pins nothing then; one charged to no one has no limit. */
static void frame_rows(void)
{
    uintptr_t frames[2];
    struct pin *p;

    frames[0] = 5 * P;
    frames[1] = 6 * P + 0x10;
    check(pin_frames(NULL, frames, 2, &holder, &p) == 0 && pin_phys(p, 1) == 6 * P &&
              frame_word(5 * P) == 1 + FRAME_PIN_BIAS && mm_a.pinned == 3,
          "a pin of two frames charged to no one is made");
    unpin(p);
    check(frame_word(5 * P) == 1 && frame_word(6 * P) == 1, "unpin gives the frames back");
    frames[0] = 7 * P;
    frames[1] = (FRAMES - 1) * P;
    check(pin_frames(NULL, frames, 2, &holder, &p) == -EINVAL && frame_word(7 * P) == 1,
          "a free frame is refused, and the frame before it is not left pinned");
    frames[1] = (uintptr_t)FRAMES * P;
    check(pin_frames(NULL, frames, 2, &holder, &p) == -EINVAL,
          "a frame past the allocator's is refused");
    check(pin_frames(NULL, frames, 0, &holder, &p) == -EINVAL, "no frames are refused");
    memlock(3);
    check(pin_frames(&mm_a, frames, 1, &holder, &p) == -ENOMEM && frame_word(7 * P) == 1,
          "a charged pin past RLIMIT_MEMLOCK answers ENOMEM");
    check(pin_frames(NULL, frames, 1, &holder, &p) == 0, "one charged to no one is made");
    unpin(p);
    memlock(0xFFFFFFFFu);
}

/** A fork copies a pinned private page for the child; a pin of a page
 * shared copy-on-write makes it the parent's own first. */
static void fork_rows(void)
{
    uintptr_t pinned = pin_phys(p_range, 0);
    uintptr_t shared, own;

    *(uint32_t *)model_frame(pinned) = 0xC0FFEEu;
    check(vm_resolve_in(&mm_a, VA + 5 * P, 1, ~(uintptr_t)0) == 1, "a sixth page faults in");
    shared = PTE_ADDR(pte_of(&dir_a, VA + 5 * P));
    *(uint32_t *)model_frame(shared) = 0xBEEFu;
    check(paging_fork_copy(&mm_a, &dir_b) == 0 && vm_copy(&mm_b.vm, &mm_a.vm) == 0, "fork copies");
    check(PTE_ADDR(pte_of(&dir_b, VA)) != pinned &&
              *(uint32_t *)model_frame(PTE_ADDR(pte_of(&dir_b, VA))) == 0xC0FFEEu &&
              frame_word(pinned) == 1 + FRAME_PIN_BIAS,
          "the child has a copy of the pinned page, and the frame stays the parent's alone");
    check(PTE_ADDR(pte_of(&dir_a, VA)) == pinned && (pte_of(&dir_a, VA) & PTE_W) &&
              !(pte_of(&dir_a, VA) & PTE_COW),
          "the parent keeps the pinned page writable, not copy-on-write");
    check(PTE_ADDR(pte_of(&dir_b, VA + 5 * P)) == shared &&
              (pte_of(&dir_a, VA + 5 * P) & PTE_COW) && frame_word(shared) == 2,
          "an unpinned page is shared copy-on-write");
    check(pin_live(p_range) && holder_count == 0, "the fork leaves the pin");

    check(pin_user(&mm_a, VA + 5 * P, P, 0, &holder, &p_cow) == 0, "a pin of the shared page");
    own = pin_phys(p_cow, 0);
    check(own != shared && PTE_ADDR(pte_of(&dir_a, VA + 5 * P)) == own &&
              PTE_ADDR(pte_of(&dir_b, VA + 5 * P)) == shared && frame_word(shared) == 1 &&
              frame_word(own) == 1 + FRAME_PIN_BIAS && *(uint32_t *)model_frame(own) == 0xBEEFu,
          "pins a copy of the parent's own, and leaves the child the shared frame");
    check(holder_count == 0 && mm_a.pinned == 4, "the copy took no pin back");
}

/** An unmap of a pinned page tells the holder once and drops the frames
 * and the charge; the end of the address space does for the rest. */
static void release_rows(void)
{
    uintptr_t f0 = pin_phys(p_range, 0), f1 = pin_phys(p_range, 1);
    uintptr_t frames[1];
    struct pin *p_obj, *p_free, *p_b;

    vm_remove(&mm_a.vm, VA + P, VA + 2 * P);
    mmn_notify(&mm_a, MMN_UNMAP, VA + P, VA + 2 * P);
    unmap_user_range(&dir_a, VA + P, VA + 2 * P);
    check(released == 1 && released_pin == p_range && !pin_live(p_range) &&
              pin_phys(p_range, 0) == 0 && mm_a.pinned == 1,
          "an unmap of one pinned page tells the holder and takes the whole pin back");
    check(frame_word(f0) == 1 && frame_word(f1) == 0,
          "the still-mapped frame keeps its mapping, and the unmapped one is free");
    mmn_notify(&mm_a, MMN_UNMAP, VA, VA + 3 * P);
    check(released == 1, "a pin taken back hears of no further unmap");
    unpin(p_range);
    check(frame_word(f0) == 1, "unpin of a pin taken back drops nothing again");

    frames[0] = 7 * P;
    check(pin_frames(&mm_a, frames, 1, &holder, &p_obj) == 0 && mm_a.pinned == 2,
          "an object's frame pinned and charged to A");
    check(pin_frames(NULL, frames, 1, &holder, &p_free) == 0, "and pinned for no one");
    check(pin_user(&mm_b, VA, P, 0, &holder, &p_b) == 0 && mm_b.pinned == 1, "a pin of B's page");
    mmn_release(&mm_a);
    check(released == 3 && !pin_live(p_cow) && !pin_live(p_obj) && mm_a.pinned == 0 &&
              mm_a.notify == NULL,
          "the end of A tells each holder and drops A's charges");
    check(pin_live(p_free) && pin_live(p_b) && frame_word(7 * P) == 1 + FRAME_PIN_BIAS,
          "the end of A leaves the pins of no one and of B");
    unpin(p_cow);
    unpin(p_obj);
    unpin(p_free);
    unpin(p_b);
    check(frame_word(7 * P) == 1 && mm_b.pinned == 0 && mm_b.notify == NULL && released == 3,
          "unpin gives the rest back and tells no one");
}

/* A BAR mapped into user space; no RAM frame backs its normal address. */
static struct {
    uintptr_t phys;
    int absent;
    int puts;
} io_object;

/** The two pages of the model BAR, or a bus fault while it is revoked. */
static int io_page(void *obj, uint32_t pgoff, uintptr_t *phys)
{
    (void)obj;
    if (io_object.absent || pgoff >= 2) {
        return -1;
    }
    *phys = io_object.phys + pgoff * P;
    return 0;
}

/** Record release of the mapping's one object reference. */
static void io_put(void *obj)
{
    (void)obj;
    io_object.puts++;
}

static const struct vm_device_ops io_ops = {io_page, io_put};

/** Access refusals and each end of a device mapping release its holder. */
static void io_lifetime(uintptr_t addr, uintptr_t phys, vma_t *vma)
{
    struct pin *p, *tmp;
    uint32_t before = released;

    io_object.phys = 7 * P;
    check(pin_user(&mm_a, addr, P, PIN_IO, &holder, &tmp) == -EFAULT && mm_a.pinned == 0,
          "a device mapping cannot bypass RAM accounting");
    paging_zap_device(&io_object, 0, 2);
    io_object.phys = phys;
    vma->prot &= ~VM_PROT_WRITE;
    check(pin_user(&mm_a, addr, P, PIN_IO | PIN_WRITE, &holder, &tmp) == -EFAULT,
          "a write mapping of a read-only BAR is refused");
    check(pin_user(&mm_a, addr, P, PIN_IO, &holder, &p) == 0,
          "the same BAR permits a read mapping");
    mmn_notify(&mm_a, MMN_PROTECT, addr, addr + P);
    check(!pin_live(p) && released == before + 1, "a protection change drops device access");
    unpin(p);
    vma->prot |= VM_PROT_WRITE;
    paging_zap_device(&io_object, 0, 2);
    io_object.absent = 1;
    check(pin_user(&mm_a, addr, P, PIN_IO, &holder, &tmp) == -EFAULT && mm_a.pinned == 0,
          "a revoked BAR answers EFAULT without a charge");
    io_object.absent = 0;
    check(pin_user(&mm_a, addr, 3 * P, PIN_IO, &holder, &tmp) == -EFAULT && mm_a.pinned == 0,
          "a device range extending into a hole pins nothing");
    check(pin_user(&mm_a, addr, P, PIN_IO, &holder, &p) == 0, "a BAR pin before unmap");
    mmn_notify(&mm_a, MMN_UNMAP, addr, addr + P);
    check(!pin_live(p) && released == before + 2, "unmap drops the BAR pin once");
    unpin(p);
    check(pin_user(&mm_a, addr, P, PIN_IO, &holder, &p) == 0, "a BAR pin before exit");
    mmn_release(&mm_a);
    check(!pin_live(p) && released == before + 3 && mm_a.pinned == 0,
          "exit drops the BAR pin without subtracting a RAM charge");
    unpin(p);
    vm_remove(&mm_a.vm, addr, addr + 2 * P);
    unmap_user_range(&dir_a, addr, addr + 2 * P);
    vm_file_reap();
    check(io_object.puts == 1, "the last mapping releases the device reference");
    memlock(0xFFFFFFFFu);
}

/** Device pages carry no RAM charge; mixed ranges charge only RAM, and a
 * revoke, protection change, unmap or exit takes their DMA access back. */
static void io_rows(void)
{
    uintptr_t addr = VA + 32 * P, phys;
    struct pin *p, *mixed, *tmp;
    vma_t *vma;
    uint32_t before = released;

#ifdef RACH2_HOST_X86_64
    phys = (uintptr_t)14 << 40;
#else
    phys = 0xFD000000u;
#endif
    io_object.phys = phys;
    check(vm_insert(&mm_a.vm, addr, addr + 2 * P, RW, VM_SHARED) == 0 &&
              vm_insert(&mm_a.vm, addr - P, addr, RW, VM_ANON) == 0,
          "a device range beside one RAM page");
    vma = vm_find(&mm_a.vm, addr);
    check(vm_device_map(vma, &io_ops, &io_object, VM_PROT_UC) == 0, "the BAR is mapped");
    memlock(0);
    check(pin_user(&mm_a, addr, 2 * P, PIN_IO | PIN_WRITE, &holder, &p) == 0 && pin_count(p) == 2 &&
              pin_ram_count(p) == 0 && mm_a.pinned == 0 && pin_phys(p, 0) == phys &&
              pin_phys(p, 1) == phys + P,
          "device pages are followed without locked memory at a zero limit");
    memlock(0xFFFFFFFFu);
    check(pin_user(&mm_a, addr, P, PIN_WRITE, &holder, &tmp) == -EFAULT,
          "ordinary pins still refuse device memory");
    memlock(1);
    check(pin_user(&mm_a, addr - P, 3 * P, PIN_IO | PIN_WRITE, &holder, &mixed) == 0 &&
              pin_count(mixed) == 3 && pin_ram_count(mixed) == 1 && mm_a.pinned == 1,
          "a mixed range charges only its RAM page");
    memlock(0);
    check(pin_user(&mm_a, addr, P, PIN_IO, &holder, &tmp) == 0 && mm_a.pinned == 1,
          "device pages work when existing RAM pins exceed a reduced limit");
    unpin(tmp);
    check(pin_user(&mm_a, addr - P, 3 * P, PIN_IO, &holder, &tmp) == -ENOMEM && mm_a.pinned == 1 &&
              frame_word(pin_phys(mixed, 0)) == 1 + FRAME_PIN_BIAS,
          "a mixed range past the limit rolls back its RAM pin");
    paging_zap_device(&io_object, 0, 2);
    check(!pin_live(p) && !pin_live(mixed) && mm_a.pinned == 0 && released == before + 2 &&
              pte_of(&dir_a, addr) == 0 && pte_of(&dir_a, addr + P) == 0,
          "device revocation drops both holders before their pages go");
    unpin(p);
    unpin(mixed);
    io_lifetime(addr, phys, vma);
}

/** mlock's regions split and merge, count in VmLck, lose their lock in a
 * fork's copy; RLIMIT_MEMLOCK's room. */
static void lock_rows(void)
{
    vm_space_t *vm = &mm_a.vm;
    vm_space_t child;
    vma_t *r;

    check(vm_insert(vm, VB, VB + 8 * P, RW, VM_ANON) == 0, "a region of eight pages");
    check(vm_lock(vm, VB + P, VB + 3 * P, VM_PROT_LOCKED) == 0, "two of its pages lock");
    r = vm_find(vm, VB + P);
    check(r && r->start == VB + P && r->end == VB + 3 * P && (r->prot & VM_PROT_LOCKED) &&
              !(vm_find(vm, VB)->prot & VM_PROT_LOCKS) &&
              !(vm_find(vm, VB + 3 * P)->prot & VM_PROT_LOCKS),
          "the locked pages are a region of their own between unlocked ones");
    check(vm_locked(vm) == 2 * P && vm_locked_in(vm, VB, VB + 2 * P) == P,
          "VmLck counts the locked bytes, and of a range those inside it");
    check(vm_lock(vm, VB + 7 * P, VB + 9 * P, VM_PROT_LOCKED) == -1 && vm_locked(vm) == 2 * P,
          "a range with a gap is refused and changes nothing");
    check(vm_lock(vm, VB + 2 * P, VB + 3 * P, VM_PROT_LOCKED | VM_PROT_LOCKONFAULT) == 0 &&
              (vm_find(vm, VB + 2 * P)->prot & VM_PROT_LOCKS) ==
                  (VM_PROT_LOCKED | VM_PROT_LOCKONFAULT) &&
              vm_locked(vm) == 2 * P,
          "MLOCK_ONFAULT replaces the bits of its page and counts as locked");
    check(vm_lock(vm, VB, VB + 8 * P, 0) == 0 && vm_locked(vm) == 0 &&
              vm_find(vm, VB)->end == VB + 8 * P,
          "munlock of the whole merges the region back");

    vm_lock_all(vm, VM_PROT_LOCKED);
    vm->def_prot = VM_PROT_LOCKED;
    check(vm_locked(vm) == vm_total(vm), "mlockall locks every region");
    k_memset(&child, 0, sizeof(child));
    check(vm_copy(&child, vm) == 0 && vm_locked(&child) == 0 && child.def_prot == 0 &&
              vm_total(&child) == vm_total(vm),
          "a fork's copy has every region and no lock, now or for the future");
    vm_free(&child);
    vm_lock_all(vm, 0);
    vm->def_prot = 0;
    check(vm_locked(vm) == 0, "munlockall unlocks every region");

    memlock(4);
    check(mem_lock_room(3, 1) && !mem_lock_room(3, 2) && !mem_lock_room(5, 0),
          "RLIMIT_MEMLOCK has room up to its pages and none past them");
    proc_a.rlim[RLIM_MEMLOCK].cur = RLIM_INFINITY;
    check(mem_lock_room(0xFFFFFFFFu, 0xFFFFFFFFu), "an infinite limit has room");
    memlock(0xFFFFFFFFu);
    proc_a.rlim[RLIM_MEMLOCK].cur = 0;
    check(mem_lock_room(1, 1), "a privileged process has room past the limit");
    cpu_table[0].current = NULL;
    check(mem_lock_room(1, 1), "the kernel, with no process, has room");
    cpu_table[0].current = &proc_a;
    memlock(0xFFFFFFFFu);
}

/** A locked stack grows only within RLIMIT_MEMLOCK. */
static void stack_rows(void)
{
    vm_space_t *vm = &mm_a.vm;

    check(vm_insert(vm, VS - 2 * P, VS, RW, VM_STACK) == 0 &&
              vm_lock(vm, VS - 2 * P, VS, VM_PROT_LOCKED) == 0 && vm_locked(vm) == 2 * P,
          "a locked stack of two pages");
    memlock(3);
    check(vm_resolve_in(&mm_a, VS - 4 * P, 1, ~(uintptr_t)0) == 0 &&
              vm_find(vm, VS - P)->start == VS - 2 * P,
          "a growth of two pages past RLIMIT_MEMLOCK is refused");
    check(vm_resolve_in(&mm_a, VS - 3 * P, 1, ~(uintptr_t)0) == 1 &&
              vm_find(vm, VS - P)->start == VS - 3 * P && vm_locked(vm) == 3 * P,
          "a growth of one page within it is made and stays locked");
    memlock(0xFFFFFFFFu);
}

/** The region's use: `used` bytes taken and the largest free run. */
static int usage_is(size_t used, size_t largest)
{
    size_t u, l;

    mmio_usage(&u, &l);
    return u == used && l == largest;
}

#ifdef RACH2_HOST_X86_64
#define MIB(n)   ((size_t)(n) << 20)
#define BAR_PHYS ((uint64_t)14 << 40)
#define LARGE    ((size_t)MMU_TABLE_SPAN)

static pci_device_t dev;

/** A 4 GiB BAR at 14 TiB in windows of 2 MiB pages. */
static void mmio_rows(void)
{
    struct pci_window w1, w2, w3;
    uintptr_t va;

    k_memset(&dev, 0, sizeof(dev));
    cfg_set_bar(0, BAR_PHYS, (uint64_t)4 << 30, 0xCu);
    cfg_command = PCI_CMD_MEMORY_SPACE;
    dev.bar[0] = cfg_bar[0];
    dev.bar[1] = cfg_bar[1];
    check(pci_bar_address64(&dev, 0) == BAR_PHYS, "the BAR is at 14 TiB");
    check(pci_bar_size64(&dev, 0) == (uint64_t)4 << 30 && !sized_decoding &&
              cfg_command == PCI_CMD_MEMORY_SPACE && cfg_bar[0] == dev.bar[0] &&
              cfg_bar[1] == dev.bar[1],
          "the BAR sizes to 4 GiB with decoding off, and is restored after");

    w1.bar_size = w2.bar_size = w3.bar_size = (uint64_t)4 << 30;
    va = pci_bar_map(&dev, 0, 0, MIB(64), PTE_WC, &w1);
    check(va != 0 && va % LARGE == 0 && w1.phys == BAR_PHYS && mmio_page_size(va) == LARGE &&
              mmio_page_size(va + MIB(64) - 1) == LARGE &&
              (model_pde(va) & (PTE_PS | PTE_P | PTE_W)) == (PTE_PS | PTE_P | PTE_W) &&
              PTE_ADDR(model_pde(va)) == BAR_PHYS &&
              PTE_ADDR(model_pde(va + MIB(62))) == BAR_PHYS + MIB(62) && !large_over_entries,
          "a 64 MiB window at the BAR's start is mapped in 2 MiB pages");
    check(pci_bar_map(&dev, 0, (uint64_t)3 << 30, MIB(64), PTE_WC, &w2) != 0 &&
              usage_is(MIB(128), 0),
          "a second 64 MiB window, 3 GiB in, fills the region");
    check(pci_bar_map(&dev, 0, MIB(512), MIB(2), PTE_WC, &w3) == 0 && w3.va == 0,
          "a third window finds no room");
    check(pci_bar_map(&dev, 0, ((uint64_t)4 << 30) - MIB(1), MIB(2), PTE_WC, &w3) == 0,
          "a window past the BAR's end is refused");
    pci_bar_unmap(&w1);
    check(w1.va == 0 && model_pde(va) == MODEL_TABLE && mmio_page_size(va) == 0 &&
              usage_is(MIB(64), MIB(64)),
          "the first window's unmap puts back the tables and frees its 64 MiB");
    check(pci_bar_map(&dev, 0, MIB(512), MIB(64), PTE_WC, &w3) == va,
          "a new window takes the freed run");

    pci_bar_unmap(&w3);
    pci_bar_unmap(&w2);
    check(usage_is(0, KVA_MMIO_SIZE), "the unmaps leave the region empty");
    check(mmio_map((uint64_t)1 << 52, P, 0) == 0, "an address past the entries' reach is refused");
}

/** A window not aligned to 2 MiB, mapped in both sizes. */
static void mmio_unaligned_rows(void)
{
    struct pci_window wu;
    uintptr_t va;

    wu.bar_size = (uint64_t)4 << 30;
    va = pci_bar_map(&dev, 0, 0x1000, MIB(4), PTE_WC, &wu);
    check(va != 0 && va % LARGE == 0x1000 && mmio_page_size(va) == P &&
              PTE_ADDR(model_kpte(va)) == BAR_PHYS + 0x1000 &&
              mmio_page_size(va + LARGE - 0x1000) == LARGE &&
              PTE_ADDR(model_pde(va + LARGE - 0x1000)) == BAR_PHYS + LARGE &&
              mmio_page_size(va + MIB(4) - 1) == P &&
              PTE_ADDR(model_kpte(va + MIB(4) - 1)) == BAR_PHYS + MIB(4),
          "a window 4 KiB into the BAR has 4 KiB pages at its ends and a 2 MiB page between");
    pci_bar_unmap(&wu);
    check(model_kpte(va) == 0 && model_kpte(va + MIB(4) - 1) == 0 &&
              model_pde(va + LARGE - 0x1000) == MODEL_TABLE && usage_is(0, KVA_MMIO_SIZE),
          "its unmap leaves the region empty, every table in place");
}
#else
#define MIB(n) ((size_t)(n) << 20)

/** std-vga's 16 MiB BAR at 0xFD000000 in windows of 4 KiB pages; an
 * address past 4 GiB is refused. */
static void mmio_rows(void)
{
    pci_device_t dev;
    struct pci_window w[4], whole;
    uintptr_t va;
    uint32_t i;
    int ok = 1;

    k_memset(&dev, 0, sizeof(dev));
    cfg_set_bar(0, 0xFD000000u, MIB(16), 0x8u);
    cfg_command = PCI_CMD_MEMORY_SPACE;
    dev.bar[0] = cfg_bar[0];
    check(pci_bar_size64(&dev, 0) == MIB(16) && !sized_decoding && cfg_bar[0] == dev.bar[0],
          "the BAR sizes to 16 MiB with decoding off");
    for (i = 0; i < 4; i++) {
        w[i].bar_size = MIB(16);
        va = pci_bar_map(&dev, 0, (uint64_t)i * MIB(4), MIB(4), PTE_WC, &w[i]);
        ok &= va != 0 && mmio_page_size(va) == P && mmio_page_size(va + MIB(4) - 1) == P &&
              PTE_ADDR(model_kpte(va)) == 0xFD000000u + i * MIB(4) &&
              PTE_ADDR(model_kpte(va + MIB(4) - 1)) == 0xFD000000u + (i + 1) * MIB(4) - P;
    }
    check(ok && usage_is(MIB(16), MIB(112)), "four 4 MiB windows map the BAR in 4 KiB pages");
    for (i = 0; i < 4; i++) {
        pci_bar_unmap(&w[i]);
    }
    check(usage_is(0, KVA_MMIO_SIZE), "their unmaps free the region");
    whole.bar_size = MIB(16);
    va = pci_bar_map(&dev, 0, 0, MIB(16), PTE_WC, &whole);
    check(va == KVA_MMIO && PTE_ADDR(model_kpte(va + MIB(16) - 1)) == 0xFDFFF000u,
          "the whole BAR maps as one window");
    pci_bar_unmap(&whole);
    check(model_kpte(va) == 0 && model_kpte(va + MIB(16) - 1) == 0, "and unmaps");
    check(mmio_map(0xFFFFF000u, 2 * P, 0) == 0, "a range past 4 GiB is refused");
    check(mmio_map(0x80000000u, KVA_MMIO_SIZE + P, 0) == 0,
          "a range larger than the region is refused");
    va = mmio_map(0x80000000u, KVA_MMIO_SIZE, 0);
    check(va == KVA_MMIO && usage_is(KVA_MMIO_SIZE, 0) && mmio_map(0xFD000000u, P, 0) == 0,
          "the whole region maps as one window, after which nothing does");
    mmio_unmap(va, KVA_MMIO_SIZE);
    check(usage_is(0, KVA_MMIO_SIZE), "and frees");
}

/** i686 has no large pages in the region: nothing more. */
static void mmio_unaligned_rows(void)
{}
#endif

/** The protection keys are off in the host test, as on a processor without
 * them. */
int fpu_pkeys(void)
{
    return 0;
}

/** PKRU with the keys off. */
uint32_t fpu_pkru(void)
{
    return 0;
}

/** Run every row; 0 when each passes, 1 otherwise. */
int main(void)
{
    model_start(NAME);
    pin_rows();
    frame_rows();
    fork_rows();
    release_rows();
    io_rows();
    lock_rows();
    stack_rows();
    mmio_rows();
    mmio_unaligned_rows();
    return model_end();
}
