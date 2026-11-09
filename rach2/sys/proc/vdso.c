/*
 * vdso.c: the vDSO's data page and image mapped into a new image, as two
 * regions whose pages are the kernel's own frames, shared by every process
 * (../vdso.h).
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

/** The data page's frame, with a reference for the mapping, 0 on success, or
 * ENXIO past it. */
static int vdso_share_data(struct vm_file *f, uint32_t pgoff, uintptr_t *phys)
{
    (void)f;
    if (pgoff != 0) {
        return -ENXIO;
    }
    *phys = vdso_frame(VDSO_NATIVE, VDSO_DATA_PAGE);
    frame_ref(*phys);
    return 0;
}

/** The files are the kernel's and never released. */
static void vdso_release(struct vm_file *f)
{
    (void)f;
}

/*
 * The files the regions map: the data page and each image. Their reference
 * counts start at one, the kernel's, so that the last region to let one go
 * never releases it; the pages are shared, so a fork's child maps the same
 * frames and no fill is needed.
 */
static char vdso_data_name[] = "[vvar]";
static char vdso_image_name[] = "[vdso]";
static struct vm_file vdso_data_file = {1, 0, vdso_share_data, vdso_release, vdso_data_name, 0, 0};
static struct vm_file vdso_image_file = {1, 0, vdso_share_image, vdso_release, vdso_image_name,
                                         0, 0};
static struct vm_file vdso_image32_file = {1, 0, vdso_share_image32, vdso_release, vdso_image_name,
                                           0, 0};

uintptr_t vdso_span(int abi)
{
    uint32_t pages = vdso_image_pages(abi);

    return pages ? (uintptr_t)(pages + 1u) * PAGE_SIZE : 0;
}

/**
 * The regions are of the kind VM_SHARED, so that a fault maps the kernel's
 * frame through `share` and a fork shares it; VM_PROT_NOWRITE keeps
 * mprotect from making either writable, and paging_access refuses a write
 * to a read-only shared page, so neither ptrace nor /proc/PID/mem can
 * change the page every process maps.
 */
intptr_t vdso_map(struct vm_space *vm, uintptr_t at, int abi)
{
    uintptr_t image = at + PAGE_SIZE;
    struct vm_file *file = abi == VDSO_IA32 ? &vdso_image32_file : &vdso_image_file;

    if (vdso_image_pages(abi) == 0) {
        return 0;
    }
    if (vm_insert_file(vm, at, image, VM_PROT_READ | VM_PROT_NOWRITE, VM_SHARED, &vdso_data_file,
                       0) != 0) {
        return -1;
    }
    if (vm_insert_file(vm, image, at + vdso_span(abi),
                       VM_PROT_READ | VM_PROT_EXEC | VM_PROT_NOWRITE, VM_SHARED, file, 0) != 0) {
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
