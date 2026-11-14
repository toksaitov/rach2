/* vm.c: address regions in an AVL tree with stable nodes and ordered links. */

#include "vm.h"

#include "../arch/arch.h"
#include "../lib/kdef.h"
#include "../lib/kerrno.h"
#include "../lib/spinlock.h"
#include "kheap.h"

#define VM_PAGE_MASK  0xFFFu
#define VM_PAGE_SHIFT 12

/* Node allocations stay with the address space until vm_free. */
struct vm_chunk {
    struct vm_chunk *next;
    vma_t nodes[VMA_FIRST];
};

int randomize_va_space = ASLR_MMAP_PAGES != 0 ? 2 : 0;
int vm_max_map_count = VMA_LIMIT;
static struct vm_file *vm_dead;
/* Covers `vm_dead`, which a fault's last reference reaches without the
 * regions' write lock, and each file's count falling to 0 (`vm_file_put`). */
static struct spinlock vm_dead_lock = SPINLOCK_INIT_NAMED("vm_dead_lock");
/* Gap candidates examined since boot, which searches in different address
 * spaces add to at once (`gap_count`). */
static volatile uint32_t gap_steps;

/** Read the number of gap candidates examined since boot. */
uint32_t vm_gap_steps(void)
{
    return gap_steps;
}

/** Add `delta` to the count at `p` in one step, as a fault holding the
 * regions' lock to read and another processor's change may both move it;
 * the count after. */
static uint32_t ref_add(volatile uint32_t *p, int delta)
{
    unsigned int old;

    do {
        old = *p;
    } while (arch_atomic_cmpxchg((volatile unsigned int *)p, old, old + (unsigned int)delta) !=
             old);
    return (uint32_t)(old + (unsigned int)delta);
}

/** Add the `n` gap candidates one search examined to `gap_steps`, in one
 * step, since searches in other address spaces run at once. */
static void gap_count(uint32_t n)
{
    (void)ref_add(&gap_steps, (int)n);
}

void vm_file_get(struct vm_file *f)
{
    if (f) {
        (void)ref_add(&f->refs, 1);
    }
}

int vm_file_tryget(struct vm_file *f)
{
    unsigned int old;

    do {
        old = f->refs;
        if (old == 0) {
            return 0;
        }
    } while (arch_atomic_cmpxchg((volatile unsigned int *)&f->refs, old, old + 1u) != old);
    return 1;
}

void vm_file_put(struct vm_file *f)
{
    unsigned int old;
    uint32_t flags;

    if (!f) {
        return;
    }
    for (;;) {
        old = f->refs;
        if (old <= 1u) {
            break;
        }
        if (arch_atomic_cmpxchg((volatile unsigned int *)&f->refs, old, old - 1u) == old) {
            return;
        }
    }
    flags = spin_lock_irqsave(&vm_dead_lock);
    if (ref_add(&f->refs, -1) == 0) {
        f->dead = vm_dead;
        vm_dead = f;
    }
    spin_unlock_irqrestore(&vm_dead_lock, flags);
}

int vm_file_dead(void)
{
    return vm_dead != NULL;
}

/* The file of a device region: the object it maps and what the object
 * does, which the record's reference to the object keeps alive. */
struct vm_device {
    struct vm_file vf; /* first, so that the file is the record */
    const struct vm_device_ops *ops;
    void *obj;
};

/** The `device` operation of a device region's file: the object's page. */
static int vm_device_page(struct vm_file *f, uint32_t pgoff, uintptr_t *phys)
{
    struct vm_device *d = (struct vm_device *)f;

    return d->ops->page(d->obj, pgoff, phys);
}

/** Release a device region's file: drop its reference to the object. */
static void vm_device_release(struct vm_file *f)
{
    struct vm_device *d = (struct vm_device *)f;

    d->ops->put(d->obj);
    kfree(d->vf.path);
    kfree(d);
}

int vm_device_map(vma_t *vma, const struct vm_device_ops *ops, void *obj, uint32_t cache)
{
    struct vm_device *d;

    if (vma->type != VM_SHARED) {
        return -EINVAL;
    }
    d = kmalloc(sizeof(*d));
    if (!d) {
        return -ENOMEM;
    }
    d->vf.refs = 1;
    d->vf.fill = NULL;
    d->vf.share = NULL;
    d->vf.release = vm_device_release;
    d->vf.path = NULL;
    d->vf.dead = NULL;
    d->vf.device = vm_device_page;
    d->vf.unlocked = 0;
    d->vf.around = NULL;
    d->vf.mkwrite = NULL;
    d->vf.same = NULL;
    d->ops = ops;
    d->obj = obj;
    vma->file = &d->vf;
    vma->prot |= VM_PROT_IO | (cache & (VM_PROT_WC | VM_PROT_UC));
    return 0;
}

void *vm_device_object(const struct vm_file *f)
{
    return f && f->device == vm_device_page ? ((const struct vm_device *)f)->obj : NULL;
}

int vm_device_whole(const struct vm_file *f)
{
    return f && f->device == vm_device_page && ((const struct vm_device *)f)->ops->whole;
}

int vm_device_access(struct vm_file *f, uint64_t offset, void *buf, uint32_t len, int write)
{
    struct vm_device *d = (struct vm_device *)f;

    if (!f || f->device != vm_device_page || !d->ops->access) {
        return -EIO;
    }
    return d->ops->access(d->obj, offset, buf, len, write);
}

struct vm_policy *vm_policy_get(struct vm_policy *p)
{
    if (p) {
        (void)ref_add(&p->refs, 1);
    }
    return p;
}

void vm_policy_put(struct vm_policy *p)
{
    if (p && ref_add(&p->refs, -1) == 0) {
        kfree(p);
    }
}

int vm_policy_equal(const struct vm_policy *a, const struct vm_policy *b)
{
    uint32_t i;

    if (a == b) {
        return 1;
    }
    if (!a || !b || a->mode != b->mode || a->flags != b->flags || a->home != b->home ||
        a->nodes != b->nodes) {
        return 0;
    }
    for (i = 0; a->flags && i < VM_NODES_WORDS; i++) {
        if (a->user[i] != b->user[i]) {
            return 0;
        }
    }
    return 1;
}

void vm_file_reap(void)
{
    struct vm_file *f;
    uint32_t flags;

    for (;;) {
        flags = spin_lock_irqsave(&vm_dead_lock);
        f = vm_dead;
        if (f) {
            vm_dead = f->dead;
            f->dead = NULL;
        }
        spin_unlock_irqrestore(&vm_dead_lock, flags);
        if (!f) {
            return;
        }
        f->release(f);
    }
}

vma_t *vm_first(const vm_space_t *vm)
{
    return vm->first;
}

vma_t *vm_next(const vma_t *r)
{
    return r->next;
}

vma_t *vm_lower(const vm_space_t *vm, uintptr_t addr)
{
    vma_t *r = vm->root;
    vma_t *found = NULL;

    while (r) {
        if (r->end <= addr) {
            r = r->right;
        } else {
            found = r;
            r = r->left;
        }
    }
    return found;
}

/** Read a subtree's height, with zero for an empty subtree. */
static unsigned int vm_height(const vma_t *r)
{
    return r ? r->height : 0;
}

/** Recompute the height of a node whose children's heights are current. */
static void vm_height_update(vma_t *r)
{
    unsigned int l = vm_height(r->left), h = vm_height(r->right);

    r->height = 1 + (l > h ? l : h);
}

/** Replace old in its parent's link, or in the tree root, with r. */
static void vm_replace(vm_space_t *vm, vma_t *old, vma_t *r)
{
    if (!old->parent) {
        vm->root = r;
    } else if (old->parent->left == old) {
        old->parent->left = r;
    } else {
        old->parent->right = r;
    }
    if (r) {
        r->parent = old->parent;
    }
}

/** Rotate a right child up and return the new subtree root. */
static vma_t *vm_rotate_left(vm_space_t *vm, vma_t *r)
{
    vma_t *s = r->right;

    vm_replace(vm, r, s);
    r->right = s->left;
    if (r->right) {
        r->right->parent = r;
    }
    s->left = r;
    r->parent = s;
    vm_height_update(r);
    vm_height_update(s);
    return s;
}

/** Rotate a left child up and return the new subtree root. */
static vma_t *vm_rotate_right(vm_space_t *vm, vma_t *r)
{
    vma_t *s = r->left;

    vm_replace(vm, r, s);
    r->left = s->right;
    if (r->left) {
        r->left->parent = r;
    }
    s->right = r;
    r->parent = s;
    vm_height_update(r);
    vm_height_update(s);
    return s;
}

/** Restore the AVL height bound from a changed subtree up to the root. */
static void vm_balance(vm_space_t *vm, vma_t *r)
{
    while (r) {
        vm_height_update(r);
        if (vm_height(r->left) > vm_height(r->right) + 1) {
            if (vm_height(r->left->right) > vm_height(r->left->left)) {
                (void)vm_rotate_left(vm, r->left);
            }
            r = vm_rotate_right(vm, r);
        } else if (vm_height(r->right) > vm_height(r->left) + 1) {
            if (vm_height(r->right->left) > vm_height(r->right->right)) {
                (void)vm_rotate_right(vm, r->right);
            }
            r = vm_rotate_left(vm, r);
        }
        r = r->parent;
    }
}

/** Allocate n spare nodes without changing the live-region limit. */
static int vm_nodes_reserve(vm_space_t *vm, uint32_t n)
{
    struct vm_chunk *chunk;
    uint32_t i;

    while (vm->cap - vm->count - vm->held < n) {
        chunk = kmalloc(sizeof(*chunk));
        if (!chunk) {
            return -1;
        }
        chunk->next = vm->chunks;
        vm->chunks = chunk;
        for (i = 0; i < VMA_FIRST; i++) {
            chunk->nodes[i].next = vm->spare;
            vm->spare = &chunk->nodes[i];
        }
        vm->cap += VMA_FIRST;
    }
    return 0;
}

int vm_reserve(vm_space_t *vm, uint32_t n)
{
    uint32_t limit = vm_max_map_count > 0 ? (uint32_t)vm_max_map_count : 0;

    if (n && (vm->count > limit || n > limit - vm->count)) {
        return -1;
    }
    return vm_nodes_reserve(vm, n);
}

/** Insert a region from a reserved node, taking its file reference; its
 * policy is the default. */
static vma_t *vm_link(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot, vm_type_t type,
                      struct vm_file *file, uint32_t pgoff)
{
    vma_t *r = vm->spare;
    vma_t *parent = NULL, *prev = NULL, *next = NULL;
    vma_t **link = &vm->root;

    vm->spare = r->next;
    while (*link) {
        parent = *link;
        if (start < parent->start) {
            next = parent;
            link = &parent->left;
        } else {
            prev = parent;
            link = &parent->right;
        }
    }
    r->start = start;
    r->end = end;
    r->prot = prot;
    r->type = type;
    r->file = file;
    r->pgoff = pgoff;
    r->policy = NULL;
    r->anon = 0;
    r->parent = parent;
    r->left = r->right = NULL;
    r->height = 1;
    r->prev = prev;
    r->next = next;
    if (prev) {
        prev->next = r;
    } else {
        vm->first = r;
    }
    if (next) {
        next->prev = r;
    } else {
        vm->last = r;
    }
    *link = r;
    vm->count++;
    vm->total += end - start;
    vm_file_get(file);
    vm_balance(vm, parent);
    return r;
}

/** Remove r without moving any surviving region, and return its node: to
 * the spare nodes, dropping its file, or, when `hold`, to the detached
 * list with its file still held (`vm_detach`). */
static void vm_delete(vm_space_t *vm, vma_t *r, int hold)
{
    vma_t *parent = r->parent;
    vma_t *s;

    if (!r->left || !r->right) {
        vm_replace(vm, r, r->left ? r->left : r->right);
    } else {
        s = r->next;
        if (s->parent != r) {
            parent = s->parent;
            vm_replace(vm, s, s->right);
            s->right = r->right;
            s->right->parent = s;
        } else {
            parent = s;
        }
        vm_replace(vm, r, s);
        s->left = r->left;
        s->left->parent = s;
        s->height = r->height;
    }
    if (r->prev) {
        r->prev->next = r->next;
    } else {
        vm->first = r->next;
    }
    if (r->next) {
        r->next->prev = r->prev;
    } else {
        vm->last = r->prev;
    }
    vm->count--;
    vm->total -= r->end - r->start;
    vm_policy_put(r->policy);
    r->policy = NULL;
    if (hold) {
        r->next = vm->detached;
        vm->detached = r;
        vm->held++;
    } else {
        vm_file_put(r->file);
        r->next = vm->spare;
        vm->spare = r;
    }
    vm_balance(vm, parent);
}

/** Split r at an interior address, using a reserved node for the upper
 * part, which takes a reference to r's policy. */
static vma_t *vm_split(vm_space_t *vm, vma_t *r, uintptr_t at)
{
    uintptr_t end = r->end;
    uint32_t pgoff = r->pgoff;
    vma_t *upper;

    if (r->file) {
        pgoff += (uint32_t)((at - r->start) >> VM_PAGE_SHIFT);
    }
    r->end = at;
    vm->total -= end - at;
    upper = vm_link(vm, at, end, r->prot, r->type, r->file, pgoff);
    upper->policy = vm_policy_get(r->policy);
    upper->anon = r->anon;
    return upper;
}

/** Whether regions of the files `a` and `b` may merge: one file, or two
 * mappings of one open file that map the same pages (`same`). */
static int vm_file_same(const struct vm_file *a, const struct vm_file *b)
{
    return a == b || (a && b && a->same && a->same == b->same && a->same(a, b));
}

/** Merge a compatible right neighbour into r; return 1 if one was removed.
 * When the two map one open file through two records, r keeps its own and
 * the neighbour's is put, which `vm_file_reap` releases after the change.
 * Two private regions of separate records merge only while at most one of
 * them has pages of its own (`anon`), as Linux's is_mergeable_anon_vma
 * refuses two regions with different anon_vma records. */
static int vm_merge(vm_space_t *vm, vma_t *r)
{
    vma_t *s = r->next;
    uintptr_t end;

    if (!s || r->end != s->start || r->prot != s->prot || r->type != s->type ||
        !vm_file_same(r->file, s->file) || !vm_policy_equal(r->policy, s->policy) ||
        (r->file && r->pgoff + ((r->end - r->start) >> VM_PAGE_SHIFT) != s->pgoff) ||
        (r->file != s->file && r->type != VM_SHARED && r->anon && s->anon)) {
        return 0;
    }
    end = s->end;
    r->anon |= s->anon;
    vm_delete(vm, s, 0); /* r holds its file still */
    vm->total += end - r->end;
    r->end = end;
    return 1;
}

void vm_release_detached(vm_space_t *vm)
{
    vma_t *r;

    while ((r = vm->detached) != NULL) {
        vm->detached = r->next;
        vm->held--;
        vm_file_put(r->file);
        r->file = NULL;
        r->next = vm->spare;
        vm->spare = r;
    }
}

void vm_init(vm_space_t *vm)
{
    vma_t *r;
    vma_t *next;

    vm_release_detached(vm);
    r = vm->first;
    while (r) {
        next = r->next;
        vm_file_put(r->file);
        vm_policy_put(r->policy);
        r->policy = NULL;
        r->next = vm->spare;
        vm->spare = r;
        r = next;
    }
    vm->root = vm->first = vm->last = NULL;
    vm->count = 0;
    vm->total = 0;
    vm->gap_len = 0;
    vm->heap_start = vm->brk = vm->stack_limit = vm->mmap_base = 0;
    vm->def_prot = 0;
}

void vm_free(vm_space_t *vm)
{
    struct vm_chunk *chunk, *next;

    vm_init(vm);
    for (chunk = vm->chunks; chunk; chunk = next) {
        next = chunk->next;
        kfree(chunk);
    }
    vm->chunks = NULL;
    vm->spare = NULL;
    vm->cap = 0;
}

int vm_copy(vm_space_t *dst, const vm_space_t *src)
{
    const vma_t *r;
    vma_t *copy;

    *dst = *src;
    dst->root = dst->first = dst->last = dst->spare = dst->detached = NULL;
    dst->chunks = NULL;
    dst->count = dst->cap = dst->held = 0;
    dst->total = 0;
    dst->gap_len = 0;
    dst->def_prot = 0;
    if (vm_nodes_reserve(dst, src->count) != 0) {
        vm_free(dst);
        return -1;
    }
    for (r = vm_first(src); r; r = vm_next(r)) {
        if (!(r->prot & VM_PROT_DONTFORK)) {
            copy = vm_link(dst, r->start, r->end, r->prot & ~(uint32_t)VM_PROT_LOCKS, r->type,
                           r->file, r->pgoff);
            copy->policy = vm_policy_get(r->policy);
            copy->anon = (r->prot & VM_PROT_WIPEONFORK) ? 0 : r->anon;
        }
    }
    return 0;
}

vma_t *vm_find(vm_space_t *vm, uintptr_t addr)
{
    vma_t *r = vm_lower(vm, addr);

    return r && r->start <= addr ? r : NULL;
}

int vm_overlaps(const vm_space_t *vm, uintptr_t start, uintptr_t end)
{
    const vma_t *r = vm_lower(vm, start);

    return start < end && r && r->start < end;
}

int vm_insert(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot, vm_type_t type)
{
    return vm_insert_file(vm, start, end, prot, type, NULL, 0);
}

int vm_insert_file(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot, vm_type_t type,
                   struct vm_file *file, uint32_t pgoff)
{
    vma_t like;

    like.prot = prot;
    like.type = type;
    like.file = file;
    like.policy = NULL;
    like.anon = 0;
    return vm_insert_like(vm, start, end, &like, pgoff);
}

int vm_insert_like(vm_space_t *vm, uintptr_t start, uintptr_t end, const vma_t *like,
                   uint32_t pgoff)
{
    vma_t *r;

    if (start >= end || ((start | end) & VM_PAGE_MASK) || vm_overlaps(vm, start, end) ||
        vm_reserve(vm, 1) != 0) {
        return -1;
    }
    r = vm_link(vm, start, end, like->prot, like->type, like->file, pgoff);
    r->policy = vm_policy_get(like->policy);
    r->anon = like->anon;
    (void)vm_merge(vm, r);
    if (r->prev) {
        (void)vm_merge(vm, r->prev);
    }
    return 0;
}

int vm_set_policy(vm_space_t *vm, uintptr_t start, uintptr_t end, struct vm_policy *pol)
{
    vma_t *r = vm_find(vm, start);

    if (!r || start >= end || end > r->end) {
        return 0;
    }
    if (vm_reserve(vm, (uint32_t)(r->start < start) + (uint32_t)(r->end > end)) != 0) {
        return -1;
    }
    if (r->start < start) {
        r = vm_split(vm, r, start);
    }
    if (r->end > end) {
        (void)vm_split(vm, r, end);
    }
    (void)vm_policy_get(pol);
    vm_policy_put(r->policy);
    r->policy = pol;
    (void)vm_merge(vm, r);
    if (r->prev) {
        (void)vm_merge(vm, r->prev);
    }
    return 0;
}

int vm_detach(vm_space_t *vm, uintptr_t start, uintptr_t end)
{
    vma_t *r, *next;

    vm->gap_len = 0;
    if (start >= end) {
        return 0;
    }
    r = vm_lower(vm, start);
    if (r && r->start < start && r->end > end) {
        if (vm_reserve(vm, 1) != 0) {
            return -1;
        }
        (void)vm_split(vm, r, end);
        vm->total -= r->end - start;
        r->end = start;
        return 0;
    }
    while (r && r->start < end) {
        next = vm_next(r);
        if (start <= r->start && end >= r->end) {
            vm_delete(vm, r, 1);
        } else if (start <= r->start) {
            vm->total -= end - r->start;
            if (r->file) {
                r->pgoff += (uint32_t)((end - r->start) >> VM_PAGE_SHIFT);
            }
            r->start = end;
            break;
        } else {
            vm->total -= r->end - start;
            r->end = start;
        }
        r = next;
    }
    return 0;
}

int vm_remove(vm_space_t *vm, uintptr_t start, uintptr_t end)
{
    int rc = vm_detach(vm, start, end);

    vm_release_detached(vm);
    return rc;
}

/* Options of `vm_change`, above every protection bit: a region with
 * VM_PROT_NOWRITE, or with VM_PROT_SECRET, stops the change; a private
 * region made writable is charged; and, with the key k in bits 24 to 27, a
 * region left with key k, the execute-only key, takes key 0. */
#define VM_CHANGE_NOWRITE   0x80000000u
#define VM_CHANGE_NOEXEC    0x40000000u
#define VM_CHANGE_ACCOUNT   0x20000000u
#define VM_CHANGE_XO        0x10000000u
#define VM_CHANGE_XO_KEY(k) (VM_CHANGE_XO | ((uint32_t)(k) << 24))
#define VM_CHANGE_OPTS      0xFF000000u

/**
 * The protection `vm_change` gives the region `r`: `(r->prot & ~mask) | bits`,
 * with VM_PROT_ACCOUNT added under VM_CHANGE_ACCOUNT to a private region that
 * was not writable and is not VM_PROT_NORESERVE, as Linux's mprotect_fixup
 * charges one, and dropped by a change that leaves write off from anonymous
 * memory none of whose pages was written, as mprotect_fixup drops it from an
 * anonymous region with no anon_vma; and with VM_CHANGE_XO_KEY a key left at
 * the execute-only key dropped to 0. A change that leaves the protection as
 * it was changes nothing, as mprotect_fixup returns first when the flags are
 * the same.
 */
static uint32_t vm_change_prot(const vma_t *r, uint32_t mask, uint32_t bits, uint32_t opts)
{
    uint32_t prot = (r->prot & ~mask) | bits;

    if (prot == r->prot) {
        return prot;
    }
    if ((opts & VM_CHANGE_ACCOUNT) && r->type != VM_SHARED &&
        !(r->prot & (VM_PROT_WRITE | VM_PROT_NORESERVE))) {
        prot |= VM_PROT_ACCOUNT;
    } else if ((mask & VM_PROT_WRITE) && !(prot & VM_PROT_WRITE) && !r->anon && !r->file &&
               r->type != VM_SHARED && !(r->prot & VM_PROT_IO)) {
        prot &= ~(uint32_t)VM_PROT_ACCOUNT;
    }
    if ((opts & VM_CHANGE_XO) && VM_PROT_KEY(prot) == ((opts >> 24) & 15u)) {
        prot &= ~(uint32_t)VM_PROT_PKEY;
    }
    return prot;
}

/**
 * Give each region of [start, end) the protection `(prot & ~mask) | bits`, for
 * `vm_protect`, `vm_advise` and `vm_lock`, splitting the regions the range cuts
 * at its ends and merging the pieces with neighbours that then match. `opts`
 * holds protection bits, a region with any of which keeps its protection, and
 * VM_CHANGE_NOWRITE and VM_CHANGE_NOEXEC, with which a region with
 * VM_PROT_NOWRITE, or VM_PROT_SECRET or VM_PROT_VVAR, stops the change
 * with -3, as Linux's
 * mprotect answers EACCES without VM_MAYWRITE or VM_MAYEXEC, and
 * VM_CHANGE_ACCOUNT and VM_CHANGE_XO_KEY (`vm_change_prot`). An empty range
 * changes nothing. 0,
 * -1 for a gap, -2 when a split has no room, or -3; of -1 and -3, the one
 * met first in address order, and every error leaves the regions as they
 * were.
 */
static int vm_change(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t mask, uint32_t bits,
                     uint32_t opts)
{
    uint32_t skip = opts & ~(uint32_t)VM_CHANGE_OPTS;
    uintptr_t at = start;
    vma_t *r, *first, *last = NULL;
    uint32_t splits;

    if (start >= end) {
        return 0;
    }
    first = vm_lower(vm, start);
    for (r = first; r && at < end; r = vm_next(r)) {
        if (r->start > at) {
            break;
        }
        if (((opts & VM_CHANGE_NOWRITE) && (r->prot & VM_PROT_NOWRITE)) ||
            ((opts & VM_CHANGE_NOEXEC) && (r->prot & (VM_PROT_SECRET | VM_PROT_VVAR)))) {
            return -3;
        }
        at = r->end;
        last = r;
    }
    if (at < end) {
        return -1;
    }
    splits = (first->start < start) + (last->end > end);
    if (vm_reserve(vm, splits) != 0) {
        return -2;
    }
    if (first->start < start) {
        first = vm_split(vm, first, start);
    }
    for (r = first; r && r->start < end; r = vm_next(r)) {
        if (r->end > end) {
            (void)vm_split(vm, r, end);
        }
        if (!(r->prot & skip)) {
            r->prot = vm_change_prot(r, mask, bits, opts);
        }
    }
    r = first->prev ? first->prev : first;
    while (r && r->next && r->next->start <= end) {
        if (!vm_merge(vm, r)) {
            r = vm_next(r);
        }
    }
    return 0;
}

int vm_protect(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot)
{
    return vm_protect_key(vm, start, end, prot, -1, -1);
}

int vm_protect_key(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot, int key,
                   int exec_key)
{
    uint32_t mask = ~(uint32_t)VM_PROT_KEPT;
    uint32_t bits = prot & ~(uint32_t)VM_PROT_KEPT;
    uint32_t opts = ((prot & VM_PROT_WRITE) ? VM_CHANGE_NOWRITE | VM_CHANGE_ACCOUNT : 0u) |
                    ((prot & VM_PROT_EXEC) ? VM_CHANGE_NOEXEC : 0u);

    if (key >= 0) {
        mask |= VM_PROT_PKEY;
        bits |= ((uint32_t)key << VM_PROT_PKEY_SHIFT) & VM_PROT_PKEY;
    } else if (exec_key > 0) {
        opts |= VM_CHANGE_XO_KEY(exec_key);
    }
    return vm_change(vm, start, end, mask, bits, opts);
}

int vm_advise(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t set, uint32_t clear)
{
    uint32_t mask =
        (set | clear) & (VM_PROT_DONTFORK | VM_PROT_GUARD | VM_PROT_SEALED | VM_PROT_WIPEONFORK);

    return vm_change(vm, start, end, mask, set & mask, 0);
}

uintptr_t vm_protect_stop(const vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot,
                          int *why)
{
    uintptr_t at = start;
    const vma_t *r;

    *why = 0;
    for (r = vm_lower(vm, start); r && at < end; r = vm_next(r)) {
        if (r->start > at) {
            break;
        }
        if (((prot & VM_PROT_WRITE) && (r->prot & VM_PROT_NOWRITE)) ||
            ((prot & VM_PROT_EXEC) && (r->prot & (VM_PROT_SECRET | VM_PROT_VVAR)))) {
            *why = -3;
            return at;
        }
        if (r->prot & VM_PROT_SEALED) {
            *why = -4;
            return at;
        }
        at = r->end;
    }
    if (at < end) {
        *why = -1;
        return at;
    }
    return end;
}

uintptr_t vm_sealed_at(const vm_space_t *vm, uintptr_t start, uintptr_t end)
{
    const vma_t *r;

    for (r = vm_lower(vm, start); r && r->start < end; r = vm_next(r)) {
        if (r->prot & VM_PROT_SEALED) {
            return r->start > start ? r->start : start;
        }
    }
    return end;
}

int vm_lock(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t bits)
{
    return vm_change(vm, start, end, VM_PROT_LOCKS, bits & VM_PROT_LOCKS,
                     VM_PROT_IO | VM_PROT_SECRET);
}

void vm_lock_all(vm_space_t *vm, uint32_t bits)
{
    vma_t *r;

    for (r = vm->first; r; r = r->next) {
        if (!(r->prot & (VM_PROT_IO | VM_PROT_SECRET))) {
            r->prot = (r->prot & ~(uint32_t)VM_PROT_LOCKS) | (bits & VM_PROT_LOCKS);
        }
    }
    r = vm->first;
    while (r && r->next) {
        if (!vm_merge(vm, r)) {
            r = r->next;
        }
    }
}

uintptr_t vm_locked_in(const vm_space_t *vm, uintptr_t start, uintptr_t end)
{
    uintptr_t total = 0;
    const vma_t *r;

    if (start >= end) {
        return 0;
    }
    for (r = vm_lower(vm, start); r && r->start < end; r = vm_next(r)) {
        if (r->prot & VM_PROT_LOCKED) {
            uintptr_t s = r->start > start ? r->start : start;
            uintptr_t e = r->end < end ? r->end : end;

            total += e - s;
        }
    }
    return total;
}

uintptr_t vm_locked(const vm_space_t *vm)
{
    uintptr_t total = 0;
    const vma_t *r;

    for (r = vm->first; r; r = r->next) {
        if (r->prot & VM_PROT_LOCKED) {
            total += r->end - r->start;
        }
    }
    return total;
}

uintptr_t vm_find_free(vm_space_t *vm, uintptr_t len, uintptr_t lo, uintptr_t hi)
{
    vma_t *r;
    uintptr_t top, bottom;
    uint32_t steps = 0;

    if (len == 0 || hi <= lo || hi - lo < len) {
        return 0;
    }
    top = hi;
    if (vm->gap_len && len >= vm->gap_len && lo == vm->gap_lo && hi == vm->gap_hi) {
        top = vm->gap_top;
    }
    vm->gap_lo = lo;
    vm->gap_hi = hi;
    vm->gap_len = len;
    r = vm_lower(vm, top);
    if (r && r->start < top) {
        top = r->start;
    }
    r = r ? r->prev : vm->last;
    for (;;) {
        steps++;
        bottom = r && r->end > lo ? r->end : lo;
        if (top > bottom && top - bottom >= len) {
            vm->gap_top = top;
            gap_count(steps);
            return top - len;
        }
        if (!r || r->start <= lo) {
            vm->gap_top = lo;
            gap_count(steps);
            return 0;
        }
        top = r->start;
        r = r->prev;
    }
}

/** Find a top-down mapping, falling back to the whole range bottom-up. */
uintptr_t vm_find_mmap(vm_space_t *vm, uintptr_t len, uintptr_t lo, uintptr_t base, uintptr_t hi)
{
    uintptr_t at = vm_find_free(vm, len, lo, base < hi ? base : hi);
    const vma_t *r;
    uint32_t steps = 0;

    if (at || !len || lo >= hi || len > hi - lo) {
        return at;
    }
    at = lo;
    for (r = vm_lower(vm, lo); r && r->start < hi; r = vm_next(r)) {
        steps++;
        if (r->start > at && len <= r->start - at) {
            gap_count(steps);
            return at;
        }
        if (r->end > at) {
            at = r->end;
        }
    }
    gap_count(steps);
    return at <= hi - len ? at : 0;
}

uintptr_t vm_covered(const vm_space_t *vm, uintptr_t start, uintptr_t end)
{
    uintptr_t total = 0;
    const vma_t *r;

    if (start >= end) {
        return 0;
    }
    for (r = vm_lower(vm, start); r && r->start < end; r = vm_next(r)) {
        uintptr_t s = r->start > start ? r->start : start;
        uintptr_t e = r->end < end ? r->end : end;

        total += e - s;
    }
    return total;
}

uintptr_t vm_total(const vm_space_t *vm)
{
    return vm->total;
}

int vm_grow_stack(vm_space_t *vm, uintptr_t page, uintptr_t as_limit)
{
    vma_t *r = vm_lower(vm, page);
    uintptr_t growth;

    if (!r || r->type != VM_STACK || page >= r->start || page < vm->stack_limit) {
        return 0;
    }
    if (r->prev && r->prev->type != VM_STACK &&
        (r->prev->prot & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC)) &&
        (page < r->prev->end || page - r->prev->end < VM_STACK_GUARD_GAP)) {
        return 0;
    }
    growth = r->start - page;
    if (vm->total > as_limit || growth > as_limit - vm->total) {
        return 0;
    }
    if (r->prot & VM_PROT_GUARD) {
        /* Linux's guard marks pages, not the region: the growth is stack. */
        vma_t like = *r;

        like.prot &= ~(uint32_t)VM_PROT_GUARD;
        return vm_insert_like(vm, page, r->start, &like, 0) == 0;
    }
    vm->total += growth;
    r->start = page;
    return 1;
}
