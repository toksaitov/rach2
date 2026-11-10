/* Ordered region readers for host models that do not link the VM tree. */
#include "mm/vm.h"

/** Return the first region in the model's address-ordered list. */
vma_t *vm_first(const vm_space_t *vm)
{
    return vm->first;
}

/** Return the next region in the model's address-ordered list. */
vma_t *vm_next(const vma_t *r)
{
    return r->next;
}

/** Return the first region of the model's list whose end is above `addr`. */
vma_t *vm_lower(const vm_space_t *vm, uintptr_t addr)
{
    vma_t *r;

    for (r = vm->first; r && r->end <= addr; r = r->next) {
    }
    return r;
}

/** Return the bytes the model's locked regions cover. */
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

/** Drop a reference to a policy; the model's regions and threads hold none. */
void vm_policy_put(struct vm_policy *p)
{
    (void)p;
}
