/*
 * gpamap.c: a guest's pages mapped into a program of the host. A guest's
 * driver reports a buffer of its own as runs of guest-physical pages; the
 * map resolves them through the VM's slots into the frames of its caller's
 * memory, holds each by a reference, and takes them back when that memory,
 * a slot over it or the VM changes. The transport over memory shared with
 * a VM (vgpu-shm(4)) maps a GPU buffer of the VM's into a program this way.
 */

#include "kvm.h"

#include "../lib/kerrno.h"
#include "../mm/kheap.h"
#include "../mm/mm.h"
#include "../mm/notify.h"
#include "../mm/paging.h"

/* A part of a map that one slot holds: `pages` guest frames from `gfn`, at
 * `hva` in the caller's memory. */
struct gpamap_run {
    uint64_t gfn;
    uint64_t pages;
    uint64_t hva;
};

/*
 * A map. `note` covers the caller's addresses from the lowest of its runs
 * to the end of the highest, and its `invalidate` takes the map back when a
 * change there meets a run. `vm` is NULL, and `frames` empty, once the map
 * is taken back; the record stays its owner's until `kvm_gpamap_free`.
 */
struct kvm_gpamap {
    struct mmn_range note; /* first, so that the range is the record */
    struct kvm_vm *vm;
    struct kvm_gpamap *next; /* the VM's maps, from vm->gpamap */
    void (*revoke)(void *owner);
    void *owner;
    uint64_t pages;
    uintptr_t *frames; /* `pages` of them, each with a reference, while live */
    uint32_t nruns;
    struct gpamap_run *runs;
};

/** Take `m` off its VM's list. */
static void unlink_map(struct kvm_gpamap *m)
{
    struct kvm_gpamap **p;

    for (p = (struct kvm_gpamap **)&m->vm->gpamap; *p; p = &(*p)->next) {
        if (*p == m) {
            *p = m->next;
            break;
        }
    }
    m->next = NULL;
}

/** Let go of what `m` holds, once: its place on the VM's list and the
 * notice, then, after its owner's `revoke` when `tell` is set, its frames. */
static void take_back(struct kvm_gpamap *m, int tell)
{
    uint64_t i;

    if (!m->vm) {
        return;
    }
    unlink_map(m);
    m->vm = NULL;
    mmn_unregister(&m->note);
    if (tell && m->revoke) {
        m->revoke(m->owner);
    }
    for (i = 0; i < m->pages; i++) {
        frame_unref(m->frames[i]);
        m->frames[i] = 0;
    }
    kvm_count(KVM_COUNT_GPA_MAP_PAGE, -(int)m->pages);
    kvm_count(KVM_COUNT_GPA_MAP, -1);
}

/** Whether a run of `m` lies in the caller's [start, end). */
static int meets_hva(const struct kvm_gpamap *m, uintptr_t start, uintptr_t end)
{
    uint32_t i;

    for (i = 0; i < m->nruns; i++) {
        const struct gpamap_run *r = &m->runs[i];

        if (r->hva < end && start < r->hva + r->pages * 4096u) {
            return 1;
        }
    }
    return 0;
}

/** The change notice of a map's range: any change to a page a run holds,
 * an unmap, a protection change, a copy-on-write break, a drop or a
 * truncation, or the end of the address space, takes the map back. */
static void map_invalidate(struct mmn_range *r, int event, uintptr_t start, uintptr_t end)
{
    struct kvm_gpamap *m = (struct kvm_gpamap *)r;

    if (event == MMN_RELEASE || meets_hva(m, start, end)) {
        take_back(m, 1);
    }
}

static const struct mmn_ops map_ops = {map_invalidate};

/* The most pages one map holds. */
#define GPAMAP_MAX_PAGES 0x7FFFFFFFu

/** Check the runs and count their pages and the parts the slots cut them
 * into: 0 with the counts in `*pages` and `*parts`, -EINVAL or -EFAULT as
 * `kvm_gpamap_new` says. */
static int count_parts(struct kvm_vm *vm, const struct kvm_gpa_run *runs, uint32_t n,
                       uint64_t *pages, uint32_t *parts)
{
    struct kvm_slot_view v;
    uint64_t total = 0, gfn, left, take;
    uint32_t i;

    *parts = 0;
    for (i = 0; i < n; i++) {
        if (runs[i].pages == 0 || (runs[i].gpa & 4095u) ||
            runs[i].pages > GPAMAP_MAX_PAGES - total ||
            runs[i].gpa / 4096u + runs[i].pages > kvm_tdp_frames()) {
            return -EINVAL;
        }
        total += runs[i].pages;
        for (gfn = runs[i].gpa / 4096u, left = runs[i].pages; left > 0; gfn += take, left -= take) {
            if (!kvm_slot_find(vm, gfn, &v) || (v.flags & KVM_MEM_READONLY)) {
                return -EFAULT;
            }
            take = v.base_gfn + v.npages - gfn;
            take = take < left ? take : left;
            if (*parts == 0xFFFFFFFFu) {
                return -ENOMEM;
            }
            (*parts)++;
        }
    }
    *pages = total;
    return 0;
}

/** Fill `m`'s parts and frames from the runs `count_parts` passed, taking
 * nothing yet: 0, or -EFAULT for a page that is not ordinary RAM. */
static int fill_parts(struct kvm_gpamap *m, struct kvm_vm *vm, const struct kvm_gpa_run *runs,
                      uint32_t n)
{
    struct mm *mm = kvm_vm_mm(vm);
    struct kvm_slot_view v;
    uint64_t gfn, left, take, j, page = 0;
    uint32_t i, k = 0;

    for (i = 0; i < n; i++) {
        for (gfn = runs[i].gpa / 4096u, left = runs[i].pages; left > 0; gfn += take, left -= take) {
            (void)kvm_slot_find(vm, gfn, &v);
            take = v.base_gfn + v.npages - gfn;
            take = take < left ? take : left;
            m->runs[k].gfn = gfn;
            m->runs[k].pages = take;
            m->runs[k].hva = v.hva;
            k++;
            for (j = 0; j < take; j++, page++) {
                m->frames[page] = paging_user_ram_frame(mm, (uintptr_t)(v.hva + j * 4096u));
                if (!m->frames[page]) {
                    return -EFAULT;
                }
            }
        }
    }
    return 0;
}

/** The lowest and the end of the highest of `m`'s parts in the caller. */
static void hva_bounds(const struct kvm_gpamap *m, uintptr_t *lo, uintptr_t *hi)
{
    uint32_t i;

    *lo = (uintptr_t)m->runs[0].hva;
    *hi = *lo;
    for (i = 0; i < m->nruns; i++) {
        uintptr_t a = (uintptr_t)m->runs[i].hva, b = a + (uintptr_t)m->runs[i].pages * 4096u;

        *lo = a < *lo ? a : *lo;
        *hi = b > *hi ? b : *hi;
    }
}

int kvm_gpamap_new(struct kvm_vm *vm, const struct kvm_gpa_run *runs, uint32_t n,
                   void (*revoke)(void *owner), void *owner, struct kvm_gpamap **out)
{
    struct mm *mm = kvm_vm_mm(vm);
    struct kvm_gpamap *m;
    uintptr_t lo, hi;
    uint64_t i, pages;
    uint32_t parts;
    int rc;

    *out = NULL;
    if (n == 0) {
        return -EINVAL;
    }
    if (!mm) {
        return -EFAULT;
    }
    rc = count_parts(vm, runs, n, &pages, &parts);
    if (rc != 0) {
        return rc;
    }
    m = kzalloc(sizeof(*m));
    if (!m) {
        return -ENOMEM;
    }
    m->frames = kzalloc((size_t)pages * sizeof(*m->frames));
    m->runs = kzalloc((size_t)parts * sizeof(*m->runs));
    if (!m->frames || !m->runs) {
        kfree(m->frames);
        kfree(m->runs);
        kfree(m);
        return -ENOMEM;
    }
    m->nruns = parts;
    m->pages = pages;
    rc = fill_parts(m, vm, runs, n);
    if (rc != 0) {
        kfree(m->frames);
        kfree(m->runs);
        kfree(m);
        return rc;
    }
    for (i = 0; i < pages; i++) {
        frame_ref(m->frames[i]);
    }
    hva_bounds(m, &lo, &hi);
    mmn_register(mm, &m->note, lo, hi, &map_ops, m);
    m->vm = vm;
    m->revoke = revoke;
    m->owner = owner;
    m->next = (struct kvm_gpamap *)vm->gpamap;
    vm->gpamap = m;
    kvm_count(KVM_COUNT_GPA_MAP, 1);
    kvm_count(KVM_COUNT_GPA_MAP_PAGE, (int)pages);
    *out = m;
    return 0;
}

uintptr_t kvm_gpamap_frame(const struct kvm_gpamap *m, uint64_t i)
{
    return m && m->vm && i < m->pages ? m->frames[i] : 0;
}

void kvm_gpamap_free(struct kvm_gpamap *m)
{
    if (!m) {
        return;
    }
    take_back(m, 0);
    kfree(m->frames);
    kfree(m->runs);
    kfree(m);
}

void kvm_gpamap_zap(struct kvm_vm *vm, uint64_t gfn, uint64_t npages)
{
    struct kvm_gpamap *m = (struct kvm_gpamap *)vm->gpamap, *next;
    uint32_t i;

    for (; m; m = next) {
        next = m->next;
        for (i = 0; i < m->nruns; i++) {
            if (m->runs[i].gfn < gfn + npages && gfn < m->runs[i].gfn + m->runs[i].pages) {
                take_back(m, 1);
                break;
            }
        }
    }
}

void kvm_gpamap_vm_end(struct kvm_vm *vm)
{
    while (vm->gpamap) {
        take_back((struct kvm_gpamap *)vm->gpamap, 1);
    }
}
