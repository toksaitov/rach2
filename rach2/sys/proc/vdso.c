/*
 * vdso.c: the vDSO's data pages and image mapped into a new image, as two
 * regions whose pages are the kernel's own frames, which every process maps
 * until it writes the image (../vdso.h).
 */

#include "../vdso.h"

#include "../mm/paging.h"
#include "../mm/vm.h"
#include "../lib/kerrno.h"

/** The frame of page `pgoff` of the native image, with a reference for
 * the mapping, 0 on success, or ENXIO past its end. */
static int vdso_share_image(struct vm_file *f, uint32_t pgoff, uintptr_t *phys)
{
    (void)f;
    if (pgoff >= vdso_image_pages(VDSO_NATIVE)) {
        return -ENXIO;
    }
    *phys = vdso_frame(VDSO_NATIVE, pgoff);
    frame_ref(*phys);
    return 0;
}

/** The same of the 32-bit image an i386 program is given. */
static int vdso_share_image32(struct vm_file *f, uint32_t pgoff, uintptr_t *phys)
{
    (void)f;
    if (pgoff >= vdso_image_pages(VDSO_IA32)) {
        return -ENXIO;
    }
    *phys = vdso_frame(VDSO_IA32, pgoff);
    frame_ref(*phys);
    return 0;
}

/** The frame of page `pgoff` of [vvar], KVM's clock page at 0 and the data
 * page at 1, with a reference for the mapping, 0 on success, or ENXIO past
 * them. */
static int vdso_share_data(struct vm_file *f, uint32_t pgoff, uintptr_t *phys)
{
    (void)f;
    if (pgoff >= VDSO_VVAR_PAGES) {
        return -ENXIO;
    }
    *phys = vdso_frame(VDSO_NATIVE, pgoff == 0 ? VDSO_KVM_PAGE : VDSO_DATA_PAGE);
    frame_ref(*phys);
    return 0;
}

/** The files are the kernel's and never released. */
static void vdso_release(struct vm_file *f)
{
    (void)f;
}

/*
 * The files the regions map: [vvar] and each image. Their reference
 * counts start at one, the kernel's, so that the last region to let one go
 * never releases it. Each lends the kernel's frame (`share`), so a fork's
 * child maps the same frames and no fill is needed; the loan takes only the
 * frame's count, so a fault makes it without the big lock (`unlocked`).
 */
static char vdso_data_name[] = "[vvar]";
static char vdso_image_name[] = "[vdso]";
static struct vm_file vdso_data_file = {1, 0, vdso_share_data, vdso_release, vdso_data_name, 0,
                                        0, 1};
static struct vm_file vdso_image_file = {1, 0, vdso_share_image, vdso_release, vdso_image_name, 0,
                                         0, 1};
static struct vm_file vdso_image32_file = {
    1, 0, vdso_share_image32, vdso_release, vdso_image_name, 0, 0, 1};

uintptr_t vdso_span(int abi)
{
    uint32_t pages = vdso_image_pages(abi);

    return pages ? (uintptr_t)(pages + VDSO_VVAR_PAGES) * PAGE_SIZE : 0;
}

/**
 * [vvar], KVM's clock page and the data page, is of the kind VM_SHARED
 * with VM_PROT_NOWRITE and VM_PROT_VVAR, as Linux maps it VM_IO |
 * VM_PFNMAP and without VM_MAYWRITE or VM_MAYEXEC: mprotect can make it
 * neither writable nor executable, and ptrace, /proc/PID/mem,
 * process_vm_readv and pins can neither read nor write it, since Linux's
 * get_user_pages refuses the region. The image, [vdso], is a private
 * mapping, VM_FILE, as Linux maps it with VM_MAYWRITE so that a debugger
 * may set a breakpoint there: a fault maps the kernel's frame copy-on-write
 * through `share`, and a write after mprotect, or one of ptrace or
 * /proc/PID/mem (`paging_access`), copies the page into this address space
 * alone. The frame keeps the kernel's own reference, so even the last
 * mapping of it is copied and never written in place.
 */
intptr_t vdso_map(struct vm_space *vm, uintptr_t at, int abi)
{
    uintptr_t image = at + VDSO_VVAR_PAGES * PAGE_SIZE;
    struct vm_file *file = abi == VDSO_IA32 ? &vdso_image32_file : &vdso_image_file;

    if (vdso_image_pages(abi) == 0) {
        return 0;
    }
    if (vm_insert_file(vm, at, image, VM_PROT_READ | VM_PROT_NOWRITE | VM_PROT_VVAR, VM_SHARED,
                       &vdso_data_file, 0) != 0) {
        return -1;
    }
    if (vm_insert_file(vm, image, at + vdso_span(abi), VM_PROT_READ | VM_PROT_EXEC, VM_FILE, file,
                       0) != 0) {
        (void)vm_remove(vm, at, image);
        return -1;
    }
    return (intptr_t)image;
}

const char *vdso_region_name(const struct vm_file *f)
{
    return f == &vdso_data_file || f == &vdso_image_file || f == &vdso_image32_file ? f->path : 0;
}

int vdso_is_data(const struct vm_file *f)
{
    return f == &vdso_data_file;
}
