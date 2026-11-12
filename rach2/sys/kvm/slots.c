/*
 * slots.c: a VM's memory slots, ranges of the caller's memory that the
 * guest sees at guest-physical addresses, each on the address space's
 * change notice so that the guest-physical tables follow every change to
 * the pages behind it. The table is changed under the big lock, which the
 * VM's ioctls hold, and `mmu_lock`, as Linux's slots change under
 * slots_lock (virt/kvm/kvm_main.c:1870), and each change is a write of the
 * table's sequence count. A lookup from a vCPU's run takes no lock: it
 * walks the tree inside a section of the VM's readers, two counts and no
 * lock, which this file keeps for the VM (`kvm_srcu_read_lock`), and walks
 * again when the count says a change came meanwhile, as Linux's readers
 * search kvm_memslots under kvm->srcu (kvm_main.c:1599-1633). A change
 * waits for every section that may have seen a slot or a log as it was
 * before it frees either or returns, as Linux waits with
 * synchronize_srcu_expedited (kvm_main.c:1633).
 */

#include "kvm.h"

#include "../arch/arch.h"
#include "../lib/kerrno.h"
#include "../mm/kheap.h"
#include "../mm/notify.h"
#include "../proc/wait_queue.h"

/* The most pages a slot has, Linux's KVM_MEM_MAX_NR_PAGES. */
#define SLOT_MAX_PAGES 0x7FFFFFFFull

/* The flags KVM_SET_USER_MEMORY_REGION takes. */
#define SLOT_FLAGS (KVM_MEM_LOG_DIRTY_PAGES | KVM_MEM_READONLY)

/*
 * A node of one of a VM's two trees of slots, an AVL tree on `key`: the
 * slot's number in one, its first guest frame in the other. Since no two
 * slots share a number or a frame, the keys of a tree differ.
 */
struct snode {
    struct snode *link[2]; /* the subtrees of smaller and of larger keys */
    uint64_t key;
    int height; /* of the subtree this node roots: 1 for a leaf */
};

/*
 * A slot. `note` covers [hva, hva + npages pages) of the VM's address
 * space, and its `invalidate` drops what the tables map of the part that
 * changes; `note.mm` is NULL once the address space has ended.
 */
struct slot {
    struct mmn_range note;
    struct kvm_vm *vm;
    uint32_t id;
    uint32_t flags; /* KVM_MEM_* */
    uint64_t base_gfn;
    uint64_t npages;
    uint64_t hva;
    uint64_t *dirty;       /* the dirty log, a bit a page, while it logs */
    struct snode id_node;  /* in `ids`, on `id` */
    struct snode gfn_node; /* in `gfns`, on `base_gfn` */
};

/* The slot of a node of `ids` or of `gfns`. */
#define SLOT_OF_ID(n)  ((struct slot *)(void *)((char *)(n) - offsetof(struct slot, id_node)))
#define SLOT_OF_GFN(n) ((struct slot *)(void *)((char *)(n) - offsetof(struct slot, gfn_node)))

/*
 * A VM's slots, in a tree by number and one by first frame, as Linux keeps
 * them in a hash by number and a tree by frame (struct kvm_memslots,
 * include/linux/kvm_host.h); `last` is the one `kvm_slot_find_locked` last
 * found. The trees and the slots' fields are under the big lock and
 * `mmu_lock` to change, either to read. A change is also a write of `seq`,
 * odd while it is under way, so that a reader with neither lock walks the
 * tree by frame again when it moved (`kvm_slot_find_read`), as a reader of
 * Linux's seqcount_t does.
 */
struct slots {
    struct snode *ids;
    struct snode *gfns;
    struct slot *last;
    uint64_t pages; /* of every slot, which Linux bounds by an unsigned long */
    volatile uint32_t seq;
};

/** The height of the subtree `n` roots, 0 for none. */
static int tree_height(const struct snode *n)
{
    return n ? n->height : 0;
}

/** Set `n`'s height from its subtrees'. */
static void tree_fix(struct snode *n)
{
    int l = tree_height(n->link[0]), r = tree_height(n->link[1]);

    n->height = (l > r ? l : r) + 1;
}

/** Turn the subtree `n` toward the side `dir`, so that its child on the
 * other side takes its place: that child, the subtree's new root. */
static struct snode *tree_rotate(struct snode *n, int dir)
{
    struct snode *c = n->link[!dir];

    n->link[!dir] = c->link[dir];
    c->link[dir] = n;
    tree_fix(n);
    tree_fix(c);
    return c;
}

/** Restore the AVL rule at `n`, whose subtrees are AVL trees whose heights
 * differ by at most 2: the subtree's new root. */
static struct snode *tree_balance(struct snode *n)
{
    int diff, dir;

    tree_fix(n);
    diff = tree_height(n->link[1]) - tree_height(n->link[0]);
    if (diff < -1 || diff > 1) {
        dir = diff > 0;
        if (tree_height(n->link[dir]->link[!dir]) > tree_height(n->link[dir]->link[dir])) {
            n->link[dir] = tree_rotate(n->link[dir], dir);
        }
        n = tree_rotate(n, !dir);
    }
    return n;
}

/** Add `x`, whose key no node of `n` has, to the tree `n`: its new root.
 * The recursion is as deep as the tree, at most 22 for 32764 nodes. */
static struct snode *tree_insert(struct snode *n, struct snode *x)
{
    int dir;

    if (!n) {
        x->link[0] = NULL;
        x->link[1] = NULL;
        x->height = 1;
        return x;
    }
    dir = x->key > n->key;
    n->link[dir] = tree_insert(n->link[dir], x);
    return tree_balance(n);
}

/** Take the node of the least key out of the tree `n`, which is not empty,
 * into `*min`: the tree's new root. */
static struct snode *tree_remove_min(struct snode *n, struct snode **min)
{
    if (!n->link[0]) {
        *min = n;
        return n->link[1];
    }
    n->link[0] = tree_remove_min(n->link[0], min);
    return tree_balance(n);
}

/** Take the node keyed `key`, which the tree `n` has, out of it: its new
 * root. */
static struct snode *tree_remove(struct snode *n, uint64_t key)
{
    struct snode *m, *right;
    int dir;

    if (key != n->key) {
        dir = key > n->key;
        n->link[dir] = tree_remove(n->link[dir], key);
        return tree_balance(n);
    }
    if (!n->link[0] || !n->link[1]) {
        return n->link[0] ? n->link[0] : n->link[1];
    }
    right = tree_remove_min(n->link[1], &m);
    m->link[0] = n->link[0];
    m->link[1] = right;
    return tree_balance(m);
}

/** The node of the tree `n` with the greatest key not above `key`, or
 * NULL when every key is above it. */
static struct snode *tree_floor(struct snode *n, uint64_t key)
{
    struct snode *best = NULL;

    while (n) {
        if (n->key <= key) {
            best = n;
            n = n->link[1];
        } else {
            n = n->link[0];
        }
    }
    return best;
}

/** The slot of `t` numbered `id`, or NULL. */
static struct slot *slot_by_id(const struct slots *t, uint32_t id)
{
    struct snode *n = tree_floor(t->ids, id);

    return n && n->key == id ? SLOT_OF_ID(n) : NULL;
}

/** The slot of `t` with the greatest first frame not above `gfn`, the one
 * slot that may hold `gfn`, or NULL. */
static struct slot *slot_below(const struct slots *t, uint64_t gfn)
{
    struct snode *n = tree_floor(t->gfns, gfn);

    return n ? SLOT_OF_GFN(n) : NULL;
}

/** The bytes of a dirty log of `npages` pages, whole 64-bit words as
 * Linux's kvm_dirty_bitmap_bytes. */
static uint64_t log_bytes(uint64_t npages)
{
    return (npages + 63u) / 64u * 8u;
}

/** A dirty log for `s`, all clear or, with KVM_DIRTY_LOG_INITIALLY_SET,
 * its pages all set, as Linux's kvm_alloc_dirty_bitmap and its caller
 * make it: the log or NULL. */
static uint64_t *log_new(struct kvm_vm *vm, uint64_t npages)
{
    uint64_t bytes = log_bytes(npages);
    uint64_t *log;
    uint64_t i;

    if (bytes != (size_t)bytes) {
        return NULL;
    }
    log = kzalloc((size_t)bytes);
    if (log && (vm->dirty_manual & KVM_DIRTY_LOG_INITIALLY_SET)) {
        for (i = 0; i < npages / 64u; i++) {
            log[i] = ~0ull;
        }
        if (npages % 64u) {
            log[npages / 64u] = (1ull << (npages % 64u)) - 1u;
        }
    }
    return log;
}

/**
 * The change notice of a slot's range: drop what the tables map of the
 * frames behind [start, end) before the pages change, and every vCPU out of
 * the guest, so that no frame is reused while a guest reaches it. At the
 * end of the address space the whole slot goes from the tables; the slot
 * stays until it is deleted or the VM ends, and no fault can map it again,
 * since no thread of that address space is left to make one.
 */
static void slot_invalidate(struct mmn_range *r, int event, uintptr_t start, uintptr_t end)
{
    struct slot *s = (struct slot *)r->owner;

    (void)event;
    kvm_tdp_zap(s->vm, s->base_gfn + (start - s->hva) / 4096u, (end - start) / 4096u);
}

static const struct mmn_ops slot_ops = {slot_invalidate};

/** Begin a change of `t` that a reader without `mmu_lock` may see: the
 * count odd before any store of the change. Under `mmu_lock`; the
 * processor makes stores visible in the order they were made. */
static void write_begin(struct slots *t)
{
    t->seq++;
    k_barrier();
}

/** End it: the count even again, after every store of the change. */
static void write_end(struct slots *t)
{
    k_barrier();
    t->seq++;
}

int kvm_slots_init(struct kvm_vm *vm)
{
    struct slots *t = kzalloc(sizeof(struct slots));

    if (!t) {
        return -ENOMEM;
    }
    vm->slots = t;
    return 0;
}

/** Add `delta` to `*p` as one step, a full barrier: the new count. */
static uint32_t readers_add(volatile uint32_t *p, uint32_t delta)
{
    uint32_t old;

    do {
        old = *p;
    } while (arch_atomic_cmpxchg(p, old, old + delta) != old);
    return old + delta;
}

/** The section's count is joined before anything it reads is loaded, and
 * the join is a full barrier, so a replacement whose wait reads the count
 * without the join has published before the load, which finds the new. */
uint32_t kvm_srcu_read_lock(struct kvm_vm *vm)
{
    uint32_t idx = vm->srcu_gen & 1u;

    (void)readers_add(&vm->srcu_readers[idx], 1u);
    return idx;
}

/** The decrement is a full barrier before the read of `srcu_waiting`, and
 * a replacement counts itself there by an exchange before it reads the
 * count, so either the replacement sees the count at 0 or this sees it
 * waiting and wakes it. */
void kvm_srcu_read_unlock(struct kvm_vm *vm, uint32_t idx)
{
    if (readers_add(&vm->srcu_readers[idx], 0xFFFFFFFFu) == 0 && vm->srcu_waiting) {
        wq_wake_all(&vm->srcu_wq);
    }
}

/* A replacement's wait for one of the readers' counts. */
struct drain {
    struct kvm_vm *vm;
    uint32_t idx;
};

/** Whether the count `arg` names has reached 0. */
static int drained(void *arg)
{
    const struct drain *d = (const struct drain *)arg;

    return d->vm->srcu_readers[d->idx] == 0;
}

/**
 * A section that may have loaded what the caller replaced has its join
 * seen by both waits, whichever count it joined; new readers join the
 * other count during each wait, so neither waits for them. Two
 * replacements may wait at once, each letting the big lock go, and each
 * waits for both counts by number, whatever `srcu_gen` the other left.
 */
void kvm_srcu_synchronize(struct kvm_vm *vm)
{
    struct drain d;

    d.vm = vm;
    d.idx = vm->srcu_gen & 1u;
    (void)readers_add(&vm->srcu_waiting, 1u);
    (void)arch_atomic_xchg(&vm->srcu_gen, d.idx ^ 1u);
    (void)wq_wait_event_nointr(&vm->srcu_wq, drained, &d);
    (void)arch_atomic_xchg(&vm->srcu_gen, d.idx);
    d.idx ^= 1u;
    (void)wq_wait_event_nointr(&vm->srcu_wq, drained, &d);
    (void)readers_add(&vm->srcu_waiting, 0xFFFFFFFFu);
}

/** Free `s`, off the notice. */
static void slot_free(struct slot *s)
{
    kvm_count(KVM_COUNT_SLOT, -1);
    mmn_unregister(&s->note);
    kfree(s->dirty);
    kfree(s);
}

/** Free every slot of the tree by number `n`. */
static void slots_free_tree(struct snode *n)
{
    if (n) {
        slots_free_tree(n->link[0]);
        slots_free_tree(n->link[1]);
        slot_free(SLOT_OF_ID(n));
    }
}

void kvm_slots_free(struct kvm_vm *vm)
{
    struct slots *t = (struct slots *)vm->slots;

    if (!t) {
        return;
    }
    slots_free_tree(t->ids);
    kfree(t);
    vm->slots = NULL;
}

/** Whether a slot other than `id` holds any frame of [gfn, gfn + npages),
 * `npages` not 0. The other slots do not overlap each other, so of those
 * that begin before the range ends, only the last can reach into it. */
static int overlaps(const struct slots *t, uint32_t id, uint64_t gfn, uint64_t npages)
{
    const struct slot *s = slot_below(t, gfn + npages - 1u);

    if (s && s->id == id) {
        s = s->base_gfn ? slot_below(t, s->base_gfn - 1u) : NULL;
    }
    return s && gfn < s->base_gfn + s->npages;
}

/** Delete `s`, the slot `id`: off the table and its frames out of the
 * tables under `mmu_lock`, then, once no copy can be using it, the record,
 * so that the caller may unmap its memory once the call returns. */
static int32_t slot_delete(struct kvm_vm *vm, struct slots *t, struct slot *s)
{
    uint32_t flags;

    kvm_gpamap_zap(vm, s->base_gfn, s->npages);
    flags = kvm_mmu_lock(vm);
    write_begin(t);
    t->ids = tree_remove(t->ids, s->id);
    t->gfns = tree_remove(t->gfns, s->base_gfn);
    if (t->last == s) {
        t->last = NULL;
    }
    t->pages -= s->npages;
    write_end(t);
    kvm_tdp_zap_locked(vm, s->base_gfn, s->npages, &flags);
    kvm_mmu_unlock(vm, flags);
    kvm_srcu_synchronize(vm);
    slot_free(s);
    return 0;
}

/** Make the slot `id` from `r`, its range on the address space's notice. */
static int32_t slot_create(struct kvm_vm *vm, struct slots *t, uint32_t id,
                           const struct kvm_userspace_memory_region *r)
{
    struct mm *mm = kvm_vm_mm(vm);
    struct slot *s;
    uint32_t flags;

    s = kzalloc(sizeof(*s));
    if (!s) {
        return -ENOMEM;
    }
    s->vm = vm;
    s->id = id;
    s->flags = r->flags;
    s->base_gfn = r->guest_phys_addr / 4096u;
    s->npages = r->memory_size / 4096u;
    s->hva = r->userspace_addr;
    s->id_node.key = id;
    s->gfn_node.key = s->base_gfn;
    if ((s->flags & KVM_MEM_LOG_DIRTY_PAGES) && !vm->dirty_ring_size) {
        s->dirty = log_new(vm, s->npages);
        if (!s->dirty) {
            kfree(s);
            return -ENOMEM;
        }
    }
    if (mm) {
        mmn_register(mm, &s->note, (uintptr_t)s->hva, (uintptr_t)(s->hva + r->memory_size),
                     &slot_ops, s);
    }
    kvm_count(KVM_COUNT_SLOT, 1);
    flags = kvm_mmu_lock(vm);
    write_begin(t);
    t->ids = tree_insert(t->ids, &s->id_node);
    t->gfns = tree_insert(t->gfns, &s->gfn_node);
    t->pages += s->npages;
    write_end(t);
    kvm_mmu_unlock(vm, flags);
    return 0;
}

/** Move `s` to the frame `gfn`, or change its flags to `flags`: the change
 * is made under `mmu_lock`, and what the tables map of the slot's old
 * frames goes after it, so that each frame is mapped again by the new
 * rules at its next fault, also one between two batches of the zap; the
 * call returns, and frees a log it ends, only once no copy can be using
 * the slot as it was. */
static int32_t slot_change(struct kvm_vm *vm, struct slots *t, struct slot *s, uint64_t gfn,
                           uint32_t flags)
{
    uint64_t *log = s->dirty, *old = NULL, base = s->base_gfn;
    uint32_t lock;

    if ((flags & KVM_MEM_LOG_DIRTY_PAGES) && !log && !vm->dirty_ring_size) {
        log = log_new(vm, s->npages);
        if (!log) {
            return -ENOMEM;
        }
    } else if (!(flags & KVM_MEM_LOG_DIRTY_PAGES)) {
        log = NULL;
    }
    if (gfn != s->base_gfn) {
        kvm_gpamap_zap(vm, s->base_gfn, s->npages);
    }
    lock = kvm_mmu_lock(vm);
    write_begin(t);
    if (log != s->dirty) {
        old = s->dirty;
        s->dirty = log;
    }
    if (gfn != s->base_gfn) {
        t->gfns = tree_remove(t->gfns, s->base_gfn);
        s->base_gfn = gfn;
        s->gfn_node.key = gfn;
        t->gfns = tree_insert(t->gfns, &s->gfn_node);
    }
    s->flags = flags;
    write_end(t);
    kvm_tdp_zap_locked(vm, base, s->npages, &lock);
    kvm_mmu_unlock(vm, lock);
    kvm_srcu_synchronize(vm);
    kfree(old);
    return 0;
}

/** The checks of KVM_SET_USER_MEMORY_REGION that need no slot, in the
 * order of Linux's kvm_set_memory_region: 0 or -EINVAL. */
static int32_t region_ok(const struct kvm_userspace_memory_region *r)
{
    if (r->flags & ~SLOT_FLAGS) {
        return -EINVAL;
    }
    if ((r->memory_size & 4095u) || (r->guest_phys_addr & 4095u) || (r->userspace_addr & 4095u)) {
        return -EINVAL;
    }
    if (r->userspace_addr + r->memory_size < r->userspace_addr ||
        r->userspace_addr + r->memory_size > USER_ADDR_MAX) {
        return -EINVAL;
    }
    if ((r->slot >> 16) != 0 || (r->slot & 0xFFFFu) >= KVM_MEM_SLOTS) {
        return -EINVAL;
    }
    if (r->guest_phys_addr + r->memory_size < r->guest_phys_addr ||
        r->memory_size / 4096u > SLOT_MAX_PAGES) {
        return -EINVAL;
    }
    return 0;
}

/** The rest of the checks and their order are Linux's
 * kvm_set_memory_region and kvm_arch_prepare_memory_region's. */
int32_t kvm_slots_set(struct kvm_vm *vm, const struct kvm_userspace_memory_region *r)
{
    struct slots *t = (struct slots *)vm->slots;
    uint32_t id = r->slot & 0xFFFFu;
    uint64_t gfn = r->guest_phys_addr / 4096u, npages = r->memory_size / 4096u;
    int32_t err = region_ok(r);
    struct slot *old;
    int placed;

    if (err != 0) {
        return err;
    }
    old = slot_by_id(t, id);
    if (npages == 0) {
        return old ? slot_delete(vm, t, old) : -EINVAL;
    }
    if (old) {
        if (r->userspace_addr != old->hva || npages != old->npages ||
            ((r->flags ^ old->flags) & KVM_MEM_READONLY)) {
            return -EINVAL;
        }
        if (gfn == old->base_gfn && r->flags == old->flags) {
            return 0;
        }
    } else if (t->pages + npages < t->pages) {
        return -EINVAL;
    }
    placed = !old || gfn != old->base_gfn;
    if (placed && gfn + npages - 1u >= kvm_tdp_frames()) {
        return -EINVAL;
    }
    if (placed && overlaps(t, id, gfn, npages)) {
        return -EEXIST;
    }
    return old ? slot_change(vm, t, old, gfn, r->flags) : slot_create(vm, t, id, r);
}

/** Fill `out` from `s` for its frame `gfn`. */
static void view(const struct slot *s, uint64_t gfn, struct kvm_slot_view *out)
{
    out->base_gfn = s->base_gfn;
    out->npages = s->npages;
    out->flags = s->flags;
    out->id = s->id;
    out->hva = s->hva + (gfn - s->base_gfn) * 4096u;
    out->dirty = s->dirty;
}

/** The last slot found is tried first, then the tree by frame, as Linux's
 * search_memslots tries its last_used_slot. */
int kvm_slot_find_locked(struct kvm_vm *vm, uint64_t gfn, struct kvm_slot_view *out)
{
    struct slots *t = (struct slots *)vm->slots;
    struct slot *s = t ? t->last : NULL;

    if (t && (!s || gfn < s->base_gfn || gfn - s->base_gfn >= s->npages)) {
        s = slot_below(t, gfn);
        if (s && gfn - s->base_gfn >= s->npages) {
            s = NULL;
        }
        if (s) {
            t->last = s;
        }
    }
    if (!s) {
        return 0;
    }
    view(s, gfn, out);
    return 1;
}

/* The most nodes a walk of the tree by frame passes before it gives up and
 * walks again: an AVL tree of 32764 nodes is at most 22 deep, so a walk
 * longer than this met the tree in the middle of a change. */
#define WALK_MAX 48

/**
 * The walk of `tree_floor` and `view` for a reader without `mmu_lock`,
 * each link and field read once: 1 with `*out` filled, 0 for no slot, or
 * -1 when the walk ran past WALK_MAX, which only a change under way makes
 * it do, since a rotation's two stores may form a loop for a moment. What
 * it gives is the caller's to believe only when the count did not move.
 */
static int walk_read(struct snode *n, uint64_t gfn, struct kvm_slot_view *out)
{
    struct snode *best = NULL;
    const struct slot *s;
    uint32_t steps = 0;
    uint64_t key;

    while (n) {
        if (++steps > WALK_MAX) {
            return -1;
        }
        key = *(const volatile uint64_t *)&n->key;
        if (key <= gfn) {
            best = n;
            n = *(struct snode *const volatile *)&n->link[1];
        } else {
            n = *(struct snode *const volatile *)&n->link[0];
        }
    }
    if (!best) {
        return 0;
    }
    s = SLOT_OF_GFN(best);
    out->base_gfn = *(const volatile uint64_t *)&s->base_gfn;
    out->npages = *(const volatile uint64_t *)&s->npages;
    if (gfn < out->base_gfn || gfn - out->base_gfn >= out->npages) {
        return 0;
    }
    out->flags = *(const volatile uint32_t *)&s->flags;
    out->id = s->id;
    out->hva = s->hva + (gfn - out->base_gfn) * 4096u;
    out->dirty = *(uint64_t *const volatile *)&s->dirty;
    return 1;
}

/** The count is read before the walk and again after it, with the loads
 * between kept in order by the processor, as Linux's read_seqcount_begin
 * and read_seqcount_retry; an odd count is a change under way, which runs
 * under `mmu_lock` with interrupts off and ends soon. The slots the walk
 * passes stay, whatever the count, until the caller's section ends. */
int kvm_slot_find_read(struct kvm_vm *vm, uint64_t gfn, struct kvm_slot_view *out)
{
    struct slots *t = (struct slots *)vm->slots;
    uint32_t seq;
    int found;

    if (!t) {
        return 0;
    }
    for (;;) {
        seq = t->seq;
        if (!(seq & 1u)) {
            k_barrier();
            found = walk_read(*(struct snode *const volatile *)&t->gfns, gfn, out);
            k_barrier();
            if (t->seq == seq && found >= 0) {
                return found;
            }
        }
        arch_relax();
    }
}

int kvm_slot_find(struct kvm_vm *vm, uint64_t gfn, struct kvm_slot_view *out)
{
    uint32_t idx;
    int found;

    if (!vm->slots) {
        return 0;
    }
    idx = kvm_srcu_read_lock(vm);
    found = kvm_slot_find_read(vm, gfn, out);
    kvm_srcu_read_unlock(vm, idx);
    return found;
}

int kvm_slot_get(struct kvm_vm *vm, uint32_t id, struct kvm_slot_view *out)
{
    struct slots *t = (struct slots *)vm->slots;
    struct slot *s = t ? slot_by_id(t, id) : NULL;

    if (!s) {
        return 0;
    }
    view(s, s->base_gfn, out);
    return 1;
}

int32_t kvm_slots_check_extension(long cap)
{
    switch (cap) {
    case KVM_CAP_USER_MEMORY:
    case KVM_CAP_USER_MEMORY2:
    case KVM_CAP_DESTROY_MEMORY_REGION_WORKS:
    case KVM_CAP_JOIN_MEMORY_REGIONS_WORKS:
    case KVM_CAP_READONLY_MEM:
    case KVM_CAP_SYNC_MMU:
        return 1;
    case KVM_CAP_NR_MEMSLOTS:
        return KVM_MEM_SLOTS;
    case KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2:
        return (int32_t)(KVM_DIRTY_LOG_MANUAL_PROTECT_ENABLE | KVM_DIRTY_LOG_INITIALLY_SET);
    default:
        return 0;
    }
}
