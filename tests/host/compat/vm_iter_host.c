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
