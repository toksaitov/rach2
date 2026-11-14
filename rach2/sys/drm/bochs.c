/*
 * bochs.c: QEMU's std-vga through its DISPI registers, Linux's bochs-drm:
 * one CRTC, primary plane and virtual connector, modes from QEMU's EDID or
 * the DMT list, and buffers in video memory that a flip scans out in place.
 */

#include "drm.h"

#include "../arch/x86/portio.h"
#include "../cmdline.h"
#include "../fs/sysfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"

/* The DISPI registers, by index: through BAR 2 at 0x500 + 2 * index, or
 * through the index and data ports. */
#define VBE_DISPI_IOPORT_INDEX           0x01ce
#define VBE_DISPI_IOPORT_DATA            0x01cf
#define VBE_DISPI_INDEX_ID               0x0
#define VBE_DISPI_INDEX_XRES             0x1
#define VBE_DISPI_INDEX_YRES             0x2
#define VBE_DISPI_INDEX_BPP              0x3
#define VBE_DISPI_INDEX_ENABLE           0x4
#define VBE_DISPI_INDEX_BANK             0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH       0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT      0x7
#define VBE_DISPI_INDEX_X_OFFSET         0x8
#define VBE_DISPI_INDEX_Y_OFFSET         0x9
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K 0xa
#define VBE_DISPI_ID0                    0xb0c0
#define VBE_DISPI_ENABLED                0x01
#define VBE_DISPI_LFB_ENABLED            0x40
#define VBE_DISPI_NOCLEARMEM             0x80

/* BAR 2's layout: the EDID from 0, the VGA ports 0x3c0 to 0x3df from
 * 0x400, the DISPI registers from 0x500, and QEMU's extension from 0x600:
 * its size, then the framebuffer's byte order. */
#define BOCHS_MMIO_EDID       0x000
#define BOCHS_MMIO_EDID_END   0x400
#define BOCHS_MMIO_VGA        0x400
#define BOCHS_MMIO_DISPI      0x500
#define BOCHS_MMIO_QEXT_SIZE  0x600
#define BOCHS_MMIO_QEXT_ORDER 0x604
#define BOCHS_ORDER_LITTLE    0x1e1e1e1eu
#define BOCHS_ORDER_BIG       0xbebebebeu

/* The VGA ports the blank uses, Linux's <video/vga.h>. */
#define VGA_ATT_W     0x3c0
#define VGA_MIS_W     0x3c2
#define VGA_IS1_RC    0x3da
#define VGA_MIS_COLOR 0x01

#define BOCHS_EDID_BLOCK 128

/* Linux's defx and defy, the mode preferred without an EDID. */
#define BOCHS_DEFX 1024
#define BOCHS_DEFY 768

#define BOCHS_MAX_SIZE 8192

/*
 * A buffer object, Linux's GEM VRAM object: in video memory at `offset`
 * while `in_vram`, else in `pages`, system memory. It is made in video
 * memory when there is room, and moved there for scanout, which pins it;
 * an unpinned object leaves video memory when a pin needs the room, the
 * least recently scanned out first. A move zaps the object's mappings, so
 * the next access faults to where it is now. `vram_next` links the
 * device's objects in video memory by offset.
 */
struct bochs_bo {
    struct drm_gem_object base;
    struct shmem pages;
    int in_vram;
    int low; /* the console's: placed from the bottom of video memory */
    uint32_t offset;
    uint32_t pin_count;
    uint32_t last_use;
    struct bochs_bo *vram_next;
};

/*
 * The device. `mmio` is BAR 2's registers, 0 for a device that has only
 * the DISPI ports; `vram` is the kernel's write-combining window on video
 * memory, `vram_size` bytes from physical `vram_phys`. `qext_size` is the
 * size of QEMU's extended registers, 0 when there are none. `xres`, `yres`
 * and `stride` are the mode in force; `stride` is 0 until one is set.
 */
struct bochs_device {
    struct drm_device drm;
    pci_device_t pci;
    uintptr_t mmio;
    uint32_t mmio_size;
    uintptr_t vram;
    uint64_t vram_phys;
    uint32_t vram_size;
    uint32_t qext_size;
    uint16_t id;
    uint32_t xres, yres, stride, yres_virtual;
    uint8_t *edid;
    uint32_t edid_len;
    struct bochs_bo *vram_bos;
    uint32_t use_clock;
    struct drm_plane plane;
    struct drm_crtc crtc;
    struct drm_encoder encoder;
    struct drm_connector connector;
};

#define to_bochs(dev) ((struct bochs_device *)(dev)->driver_priv)

/* ------------------------------------------------------------------------
 * The registers
 */

/** Write the VGA port `port`, 0x3c0 to 0x3df, through BAR 2 or the port. */
static void bochs_vga_writeb(struct bochs_device *b, uint16_t port, uint8_t val)
{
    if (b->mmio) {
        *(volatile uint8_t *)(b->mmio + BOCHS_MMIO_VGA + (port - 0x3c0u)) = val;
    } else {
        outb(port, val);
    }
}

/** Read the VGA port `port`. */
static uint8_t bochs_vga_readb(struct bochs_device *b, uint16_t port)
{
    if (b->mmio) {
        return *(volatile uint8_t *)(b->mmio + BOCHS_MMIO_VGA + (port - 0x3c0u));
    }
    return inb(port);
}

/** Read the DISPI register `reg`. */
static uint16_t bochs_dispi_read(struct bochs_device *b, uint16_t reg)
{
    if (b->mmio) {
        return *(volatile uint16_t *)(b->mmio + BOCHS_MMIO_DISPI + 2u * reg);
    }
    outw(VBE_DISPI_IOPORT_INDEX, reg);
    return inw(VBE_DISPI_IOPORT_DATA);
}

/** Write the DISPI register `reg`. */
static void bochs_dispi_write(struct bochs_device *b, uint16_t reg, uint16_t val)
{
    if (b->mmio) {
        *(volatile uint16_t *)(b->mmio + BOCHS_MMIO_DISPI + 2u * reg) = val;
    } else {
        outw(VBE_DISPI_IOPORT_INDEX, reg);
        outw(VBE_DISPI_IOPORT_DATA, val);
    }
}

/** Set the byte order QEMU reads the framebuffer in, where its extended
 * registers reach that far. */
static void bochs_set_order(struct bochs_device *b, uint32_t order)
{
    if (b->qext_size >= 8) {
        *(volatile uint32_t *)(b->mmio + BOCHS_MMIO_QEXT_ORDER) = order;
    }
}

/** Blank or show the screen through the VGA attribute controller's index,
 * as Linux's bochs_hw_blank. */
static void bochs_blank(struct bochs_device *b, int blank)
{
    bochs_vga_writeb(b, VGA_MIS_W, VGA_MIS_COLOR);
    (void)bochs_vga_readb(b, VGA_IS1_RC);
    bochs_vga_writeb(b, VGA_ATT_W, blank ? 0 : 0x20);
}

/** Set the mode `mode` at 32 bits a pixel, the whole of video memory its
 * virtual height, and show it, Linux's bochs_hw_setmode. The enable keeps
 * video memory: without VBE_DISPI_NOCLEARMEM the device zeroes the mode's
 * size from offset 0, where another buffer may lie. */
static void bochs_set_mode(struct bochs_device *b, const struct drm_mode_modeinfo *mode)
{
    if (b->drm.unplugged) {
        return;
    }
    b->xres = mode->hdisplay;
    b->yres = mode->vdisplay;
    b->stride = b->xres * 4u;
    b->yres_virtual = b->vram_size / b->stride;
    bochs_blank(b, 0);
    bochs_dispi_write(b, VBE_DISPI_INDEX_ENABLE, 0);
    bochs_dispi_write(b, VBE_DISPI_INDEX_BPP, 32);
    bochs_dispi_write(b, VBE_DISPI_INDEX_XRES, (uint16_t)b->xres);
    bochs_dispi_write(b, VBE_DISPI_INDEX_YRES, (uint16_t)b->yres);
    bochs_dispi_write(b, VBE_DISPI_INDEX_BANK, 0);
    bochs_dispi_write(b, VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)b->xres);
    bochs_dispi_write(b, VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)b->yres_virtual);
    bochs_dispi_write(b, VBE_DISPI_INDEX_X_OFFSET, 0);
    bochs_dispi_write(b, VBE_DISPI_INDEX_Y_OFFSET, 0);
    bochs_dispi_write(b, VBE_DISPI_INDEX_ENABLE,
                      VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM);
}

/**
 * Scan out from byte `addr` of video memory, the pixel at (px, py) of rows
 * `pitch` bytes long, through the virtual width and the offsets, Linux's
 * bochs_hw_setbase: a flip writes these three registers alone.
 */
static void bochs_set_base(struct bochs_device *b, uint32_t px, uint32_t py, uint32_t pitch,
                           uint32_t addr)
{
    uint32_t offset;

    if (b->drm.unplugged || pitch == 0) {
        return;
    }
    b->stride = pitch;
    offset = addr + py * pitch + px * 4u;
    bochs_dispi_write(b, VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)(pitch / 4u));
    bochs_dispi_write(b, VBE_DISPI_INDEX_X_OFFSET, (uint16_t)((offset % pitch) / 4u));
    bochs_dispi_write(b, VBE_DISPI_INDEX_Y_OFFSET, (uint16_t)(offset / pitch));
}

/* ------------------------------------------------------------------------
 * Buffer objects in video memory
 */

/**
 * Find room for `size` bytes in video memory: for an ordinary buffer the
 * highest that fits, so that the low part, where the firmware's screen and
 * the console are, is used last; for the console's (`low`) the lowest, as
 * Linux's fbdev emulation has its buffer at offset 0, so that the console's
 * buffer, pinned while it is shown, leaves the rest of video memory in one
 * piece for a master's largest mode. 0 with the offset in `*offset`, or
 * -ENOSPC.
 */
static int32_t bochs_vram_find(struct bochs_device *b, uint32_t size, int low, uint32_t *offset)
{
    struct bochs_bo *bo;
    uint32_t start = 0, end, found = 0;
    int have = 0;

    for (bo = b->vram_bos;; bo = bo->vram_next) {
        end = bo ? bo->offset : b->vram_size;
        if (end >= start && end - start >= size) {
            found = low ? start : end - size;
            have = 1;
            if (low) {
                break;
            }
        }
        if (!bo) {
            break;
        }
        start = bo->offset + (uint32_t)bo->base.size;
    }
    if (!have) {
        return -ENOSPC;
    }
    *offset = found;
    return 0;
}

/** Put `bo` in the device's list of objects in video memory at `offset`. */
static void bochs_vram_insert(struct bochs_device *b, struct bochs_bo *bo, uint32_t offset)
{
    struct bochs_bo **p = &b->vram_bos;

    while (*p && (*p)->offset < offset) {
        p = &(*p)->vram_next;
    }
    bo->offset = offset;
    bo->in_vram = 1;
    bo->vram_next = *p;
    *p = bo;
}

/** Take `bo` off the list of objects in video memory. */
static void bochs_vram_remove(struct bochs_device *b, struct bochs_bo *bo)
{
    struct bochs_bo **p;

    for (p = &b->vram_bos; *p; p = &(*p)->vram_next) {
        if (*p == bo) {
            *p = bo->vram_next;
            break;
        }
    }
    bo->in_vram = 0;
    bo->vram_next = NULL;
}

/**
 * Move `bo` from video memory to system memory: its mappings zapped, its
 * bytes copied out. 0, or -ENOMEM when system memory has no room, and it
 * stays where it was.
 */
static int32_t bochs_bo_evict(struct bochs_device *b, struct bochs_bo *bo)
{
    uint32_t size = (uint32_t)bo->base.size;

    paging_zap_device(bo, 0, size >> PAGE_SHIFT);
    bo->pages.size = bo->base.size;
    if (shmem_write(&bo->pages, (const void *)(b->vram + bo->offset), size, 0) != (int32_t)size) {
        shmem_free(&bo->pages);
        return -ENOMEM;
    }
    bo->pages.size = bo->base.size;
    bochs_vram_remove(b, bo);
    return 0;
}

/**
 * Room for `size` bytes in video memory, placed as `bochs_vram_find` places
 * a buffer of `low`, made by moving unpinned objects out, the least
 * recently scanned out first. 0 with `*offset`, or -ENOMEM when the pinned
 * objects leave no room or system memory has none.
 */
static int32_t bochs_vram_make_room(struct bochs_device *b, uint32_t size, int low,
                                    uint32_t *offset)
{
    struct bochs_bo *bo, *victim;
    int32_t err;

    while (bochs_vram_find(b, size, low, offset) != 0) {
        victim = NULL;
        for (bo = b->vram_bos; bo; bo = bo->vram_next) {
            if (bo->pin_count == 0 && (!victim || bo->last_use < victim->last_use)) {
                victim = bo;
            }
        }
        if (!victim) {
            return -ENOMEM;
        }
        err = bochs_bo_evict(b, victim);
        if (err != 0) {
            return err;
        }
    }
    return 0;
}

/**
 * Move `bo` into video memory, making room: its mappings zapped, its bytes
 * copied in, its system pages freed. 0, -ENOMEM, or -EIO when the copy
 * found no temporary mapping.
 */
static int32_t bochs_bo_to_vram(struct bochs_device *b, struct bochs_bo *bo)
{
    uint32_t size = (uint32_t)bo->base.size, offset;
    int32_t err;

    if (bo->in_vram) {
        return 0;
    }
    if (size > b->vram_size) {
        return -ENOMEM;
    }
    err = bochs_vram_make_room(b, size, bo->low, &offset);
    if (err != 0) {
        return err;
    }
    paging_zap_device(bo, 0, size >> PAGE_SHIFT);
    if (shmem_read(&bo->pages, (void *)(b->vram + offset), size, 0) != (int32_t)size) {
        return -EIO;
    }
    shmem_free(&bo->pages);
    bochs_vram_insert(b, bo, offset);
    return 0;
}

/** Keep `bo` in video memory for scanout, moving it there if need be,
 * Linux's drm_gem_vram_pin: 0 or the move's error. */
static int32_t bochs_bo_pin(struct bochs_device *b, struct bochs_bo *bo)
{
    int32_t err = bochs_bo_to_vram(b, bo);

    if (err == 0) {
        bo->pin_count++;
        bo->last_use = ++b->use_clock;
    }
    return err;
}

/** Let `bo` move again. */
static void bochs_bo_unpin(struct bochs_bo *bo)
{
    if (bo->pin_count > 0) {
        bo->pin_count--;
    }
}

/** The `page` of a mapping of a buffer: its page in video memory, or its
 * system page, given on the first touch. */
static int bochs_vm_page(void *obj, uint32_t pgoff, uintptr_t *phys)
{
    struct bochs_bo *bo = (struct bochs_bo *)obj;
    struct bochs_device *b = to_bochs(bo->base.dev);
    uintptr_t p;

    if (b->drm.unplugged || (uint64_t)pgoff >= bo->base.size >> PAGE_SHIFT) {
        return -1;
    }
    if (bo->in_vram) {
        *phys = (uintptr_t)(b->vram_phys + bo->offset + ((uint64_t)pgoff << PAGE_SHIFT));
        return 0;
    }
    p = shmem_resident(&bo->pages, pgoff);
    if (!p && shmem_allocate(&bo->pages, (uint64_t)pgoff << PAGE_SHIFT, PAGE_SIZE) == 0) {
        p = shmem_resident(&bo->pages, pgoff);
    }
    if (!p) {
        return -1;
    }
    *phys = p;
    return 0;
}

/** The `put` of a mapping: the region's reference. */
static void bochs_vm_put(void *obj)
{
    drm_gem_object_put((struct drm_gem_object *)obj);
}

static const struct vm_device_ops bochs_vm_ops = {bochs_vm_page, bochs_vm_put, NULL, 0};

/** The `mmap` of a buffer: write-combining, as TTM maps video memory. */
static int32_t bochs_bo_mmap(struct drm_gem_object *obj, vma_t *vma)
{
    return vm_device_map(vma, &bochs_vm_ops, obj, VM_PROT_WC);
}

/** The `free` of a buffer: its room in video memory or its system pages,
 * and its record. */
static void bochs_bo_free(struct drm_gem_object *obj)
{
    struct bochs_bo *bo = (struct bochs_bo *)obj;

    if (bo->in_vram) {
        bochs_vram_remove(to_bochs(obj->dev), bo);
    }
    shmem_free(&bo->pages);
    kfree(bo);
}

/** The `vmap` of a buffer: the kernel's window on it in video memory, where
 * a buffer scanned out is pinned; NULL for one in system memory. */
static volatile uint8_t *bochs_bo_vmap(struct drm_gem_object *obj)
{
    struct bochs_bo *bo = (struct bochs_bo *)obj;

    if (!bo->in_vram || obj->dev->unplugged) {
        return NULL;
    }
    return (volatile uint8_t *)(to_bochs(obj->dev)->vram + bo->offset);
}

static const struct drm_gem_object_funcs bochs_bo_funcs = {
    bochs_bo_free, bochs_bo_mmap, NULL, NULL, NULL, bochs_bo_vmap, NULL, NULL};

/**
 * MODE_CREATE_DUMB, Linux's drm_gem_vram_fill_create_dumb: rows of `width`
 * pixels with no alignment, a whole number of pages, zeroed; in video
 * memory when it has room without moving another buffer, else in system
 * memory until a scanout moves it. The console's file makes a buffer that
 * `bochs_vram_find` places from the bottom. 0, -EINVAL or -ENOMEM.
 */
static int32_t bochs_dumb_create(struct drm_file *file, struct drm_device *dev,
                                 struct drm_mode_create_dumb *args)
{
    struct bochs_device *b = to_bochs(dev);
    uint32_t pitch = args->width * ((args->bpp + 7u) / 8u), size, offset;
    struct bochs_bo *bo;
    int32_t err;

    size = (pitch * args->height + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
    if (size == 0) {
        return -EINVAL;
    }
    bo = (struct bochs_bo *)kzalloc(sizeof(*bo));
    if (!bo) {
        return -ENOMEM;
    }
    drm_gem_object_init(dev, &bo->base, size, &bochs_bo_funcs);
    bo->base.map_wc = 1;
    bo->pages.size = size;
    bo->low = file->console;
    if (bochs_vram_find(b, size, bo->low, &offset) == 0) {
        k_memset((void *)(b->vram + offset), 0, size);
        bochs_vram_insert(b, bo, offset);
    }
    err = drm_gem_handle_create(file, &bo->base, &args->handle);
    drm_gem_object_put(&bo->base);
    if (err != 0) {
        return err;
    }
    args->pitch = pitch;
    args->size = size;
    return 0;
}

/* ------------------------------------------------------------------------
 * The display
 */

static const uint32_t bochs_formats[] = {DRM_FORMAT_XRGB8888, DRM_FORMAT_BGRX8888};

/**
 * Scan out the plane's framebuffer, Linux's bochs_plane_update: its base
 * in video memory, its pitch and the first pixel its source shows, and the
 * byte order of its format. Nothing before a mode is set or without a
 * framebuffer. Linux passes the plane's CRTC position, always 0 for this
 * primary plane; the source's corner is the pixel SETCRTC's x and y name.
 */
static void bochs_plane_show(struct bochs_device *b, const struct drm_plane_state *ps)
{
    struct bochs_bo *bo;

    if (!ps || !ps->fb || !b->stride) {
        return;
    }
    bo = (struct bochs_bo *)ps->fb->obj[0];
    if (!bo->in_vram) {
        return;
    }
    bochs_set_base(b, ps->src_x >> 16, ps->src_y >> 16, ps->fb->pitches[0],
                   bo->offset + ps->fb->offsets[0]);
    bochs_set_order(b,
                    ps->fb->format == DRM_FORMAT_BGRX8888 ? BOCHS_ORDER_BIG : BOCHS_ORDER_LITTLE);
}

/** The plane's update, which a flip is. */
static void bochs_plane_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
    bochs_plane_show(to_bochs(plane->dev), drm_atomic_get_new_plane_state(state, plane));
}

/** The plane's check, Linux's simple display pipe's: the CRTC covered,
 * unscaled. */
static int32_t bochs_plane_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
    struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
    const struct drm_crtc_state *cs = NULL;

    if (ps->crtc) {
        cs = drm_atomic_get_new_crtc_state(state, ps->crtc)
                 ? drm_atomic_get_new_crtc_state(state, ps->crtc)
                 : ps->crtc->state;
    }
    return drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
                                               0, 0);
}

/** The CRTC's check: a CRTC that is on shows its primary plane. */
static int32_t bochs_crtc_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    const struct drm_crtc_state *cs = drm_atomic_get_new_crtc_state(state, crtc);

    return cs->enable && !(cs->plane_mask & drm_plane_mask(crtc->primary)) ? -EINVAL : 0;
}

/** The CRTC on: the mode set, then the plane shown, Linux's
 * bochs_pipe_enable. */
static void bochs_crtc_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    struct bochs_device *b = to_bochs(crtc->dev);

    bochs_set_mode(b, &drm_atomic_get_new_crtc_state(state, crtc)->mode);
    bochs_plane_show(b, crtc->primary->state);
}

/** The CRTC off: the screen blanked. */
static void bochs_crtc_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    struct bochs_device *b = to_bochs(crtc->dev);

    (void)state;
    if (!b->drm.unplugged) {
        bochs_blank(b, 1);
    }
}

/** A mode fits when two frames of it at 32 bits fit in video memory, so
 * that a flip can show one while the other is drawn, Linux's
 * drm_vram_helper_mode_valid. */
static int32_t bochs_crtc_mode_valid(struct drm_crtc *crtc, const struct drm_mode_modeinfo *mode)
{
    struct bochs_device *b = to_bochs(crtc->dev);
    uint64_t pages = ((uint64_t)mode->hdisplay * mode->vdisplay * 4u + PAGE_SIZE - 1) >> PAGE_SHIFT;

    return pages > (uint64_t)(b->vram_size / 2u) >> PAGE_SHIFT ? -EINVAL : 0;
}

/** The connector's modes: the EDID's, or without one the DMT modes with
 * 1024x768 preferred, Linux's bochs_connector_get_modes. */
static int bochs_get_modes(struct drm_connector *connector)
{
    struct bochs_device *b = to_bochs(connector->dev);
    struct drm_display_mode *m;
    int count = 0;

    if (b->edid) {
        count = drm_connector_update_edid(connector, b->edid, b->edid_len);
    }
    if (count == 0) {
        count = drm_add_modes_noedid(connector, BOCHS_MAX_SIZE, BOCHS_MAX_SIZE);
        for (m = connector->probed_modes; m; m = m->next) {
            if (m->info.hdisplay == BOCHS_DEFX && m->info.vdisplay == BOCHS_DEFY) {
                m->info.type |= DRM_MODE_TYPE_PREFERRED;
            }
        }
    }
    return count;
}

/**
 * The commit, with Linux's prepare_fb and cleanup_fb of the VRAM helper
 * around the core's: each framebuffer the commit shows is pinned in video
 * memory first, and each the commit replaces is unpinned after. -ENOMEM
 * when a framebuffer finds no room, with nothing changed.
 */
static int32_t bochs_atomic_commit(struct drm_device *dev, struct drm_atomic_state *state,
                                   int nonblock)
{
    struct bochs_device *b = to_bochs(dev);
    struct drm_framebuffer *old[DRM_MAX_PLANES];
    struct drm_plane_state *ps;
    int32_t err = 0;
    int i, pinned;

    for (pinned = 0; pinned < DRM_MAX_PLANES; pinned++) {
        ps = state->planes_new[pinned];
        if (ps && ps->fb && (err = bochs_bo_pin(b, (struct bochs_bo *)ps->fb->obj[0])) != 0) {
            break;
        }
    }
    if (err == 0) {
        for (i = 0; i < DRM_MAX_PLANES; i++) {
            old[i] =
                state->planes_new[i] && state->planes_old[i]->fb ? state->planes_old[i]->fb : NULL;
            drm_framebuffer_get(old[i]);
        }
        err = drm_atomic_helper_commit(dev, state, nonblock);
        for (i = 0; i < DRM_MAX_PLANES; i++) {
            if (old[i] && err == 0) {
                bochs_bo_unpin((struct bochs_bo *)old[i]->obj[0]);
            }
            drm_framebuffer_put(old[i]);
        }
        if (err == 0) {
            return 0;
        }
    }
    for (i = 0; i < pinned; i++) {
        ps = state->planes_new[i];
        if (ps && ps->fb) {
            bochs_bo_unpin((struct bochs_bo *)ps->fb->obj[0]);
        }
    }
    return err;
}

static const struct drm_plane_funcs bochs_plane_funcs = {bochs_plane_check, bochs_plane_update,
                                                         NULL};
static const struct drm_crtc_funcs bochs_crtc_funcs = {bochs_crtc_check,
                                                       bochs_crtc_enable,
                                                       bochs_crtc_disable,
                                                       NULL,
                                                       NULL,
                                                       bochs_crtc_mode_valid,
                                                       NULL,
                                                       NULL,
                                                       NULL,
                                                       NULL,
                                                       NULL,
                                                       NULL,
                                                       NULL};
static const struct drm_connector_funcs bochs_connector_funcs = {NULL, bochs_get_modes, NULL, NULL};

/**
 * Make the display, in Linux's order: the virtual connector, with the
 * EDID property when QEMU gives an EDID, then the simple display pipe's
 * plane, CRTC and encoder. 0 or -errno.
 */
static int32_t bochs_kms_init(struct bochs_device *b)
{
    struct drm_device *dev = &b->drm;
    struct drm_mode_config *mc = &dev->mode_config;
    int32_t err;

    err = drm_mode_config_init(dev);
    if (err != 0) {
        return err;
    }
    mc->max_width = BOCHS_MAX_SIZE;
    mc->max_height = BOCHS_MAX_SIZE;
    mc->preferred_depth = 24;
    mc->prefer_shadow = 0;
    mc->atomic_commit = bochs_atomic_commit;
    err =
        drm_connector_init(dev, &b->connector, &bochs_connector_funcs, DRM_MODE_CONNECTOR_VIRTUAL);
    if (err != 0) {
        return err;
    }
    if (b->edid) {
        drm_connector_attach_edid_property(&b->connector);
    }
    if ((err = drm_universal_plane_init(dev, &b->plane, 1, &bochs_plane_funcs, bochs_formats, 2,
                                        NULL, DRM_PLANE_TYPE_PRIMARY)) != 0 ||
        (err = drm_crtc_init_with_planes(dev, &b->crtc, &b->plane, NULL, &bochs_crtc_funcs)) != 0 ||
        (err = drm_encoder_init(dev, &b->encoder, DRM_MODE_ENCODER_NONE)) != 0) {
        return err;
    }
    b->encoder.possible_crtcs = drm_crtc_mask(&b->crtc);
    return drm_connector_attach_encoder(&b->connector, &b->encoder);
}

/**
 * Read QEMU's EDID from BAR 2, Linux's bochs_hw_load_edid: the base block
 * when its header is EDID's, and the extensions it names, up to the VGA
 * ports at 0x400. Nothing for a device without BAR 2 or with EDID off.
 */
static void bochs_load_edid(struct bochs_device *b)
{
    static const uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    volatile const uint8_t *edid = (volatile const uint8_t *)(b->mmio + BOCHS_MMIO_EDID);
    uint32_t i, len;

    if (!b->mmio) {
        return;
    }
    for (i = 0; i < sizeof(header); i++) {
        if (edid[i] != header[i]) {
            return;
        }
    }
    len = BOCHS_EDID_BLOCK * (1u + edid[126]);
    if (len > BOCHS_MMIO_EDID_END) {
        len = BOCHS_MMIO_EDID_END;
    }
    b->edid = (uint8_t *)kmalloc(len);
    if (!b->edid) {
        return;
    }
    for (i = 0; i < len; i++) {
        b->edid[i] = edid[i];
    }
    b->edid_len = len;
}

/** The driver's `release`, after a failed bind: the display's objects,
 * the EDID and the record. A bound device is never released. */
static void bochs_release(struct drm_device *dev)
{
    struct bochs_device *b = to_bochs(dev);

    drm_mode_config_cleanup(dev);
    kfree(b->edid);
    kfree(b);
}

/** PRIME's import of another device's buffer: -EINVAL, as Linux's VRAM
 * helper, which has no import of foreign pages; a buffer of this device
 * comes back as itself before the hook is asked. */
static struct drm_gem_object *bochs_prime_import(struct drm_device *dev, struct dma_buf *buf,
                                                 int32_t *err)
{
    (void)dev;
    (void)buf;
    *err = -EINVAL;
    return NULL;
}

static const struct drm_driver bochs_driver = {DRM_IDENT_BOCHS,    NULL, NULL, bochs_dumb_create,
                                               bochs_prime_import, NULL, NULL, 0,
                                               bochs_release};

/**
 * Bind the std-vga function `pdev` whose registers are at `mmio`, BAR 2's
 * `mmio_size` bytes or 0 for the DISPI ports, and whose video memory is at
 * `vram`, the kernel's window on `vram_size` bytes from physical
 * `vram_phys`, Linux's bochs_hw_init and bochs_kms_init: the DISPI id
 * checked, QEMU's byte order set where its extension has it, the EDID read,
 * the display made and the device registered. The function is shown bound
 * to bochs-drm in /sys before its card appears, as Linux's driver core
 * links a device to its driver before the driver's probe runs, so that a
 * reader of the card's uevent finds the parent's driver; a failure to show
 * it fails the bind, as Linux's really_probe stops when driver_sysfs_add
 * fails. Once the card is registered the function's "bind" uevent is sent,
 * as Linux's driver_bound sends it. 0, -ENODEV for a device that is not a
 * DISPI one, or -errno. The caller has removed the firmware's framebuffer
 * and mapped both.
 */
static int32_t bochs_bind(const pci_device_t *pdev, uintptr_t mmio, uint32_t mmio_size,
                          uintptr_t vram, uint64_t vram_phys, uint32_t vram_size)
{
    struct bochs_device *b = (struct bochs_device *)kzalloc(sizeof(*b));
    uint32_t mem;
    int32_t err;
    int key;

    if (!b) {
        return -ENOMEM;
    }
    b->pci = *pdev;
    b->mmio = mmio;
    b->mmio_size = mmio_size;
    b->vram = vram;
    b->vram_phys = vram_phys;
    b->id = bochs_dispi_read(b, VBE_DISPI_INDEX_ID);
    mem = (uint32_t)bochs_dispi_read(b, VBE_DISPI_INDEX_VIDEO_MEMORY_64K) * 65536u;
    if ((b->id & 0xfff0u) != VBE_DISPI_ID0) {
        klog(KLOG_ERR, "bochs", "id 0x%x is not a DISPI id", (unsigned)b->id);
        kfree(b);
        return -ENODEV;
    }
    if (vram_size != mem) {
        klog(KLOG_WARN, "bochs", "video memory: BAR 0 has %u KiB, DISPI says %u KiB",
             (unsigned)(vram_size / 1024u), (unsigned)(mem / 1024u));
        if (mem < vram_size) {
            vram_size = mem;
        }
    }
    b->vram_size = vram_size & ~(uint32_t)(PAGE_SIZE - 1);
    if (b->mmio && pdev->revision >= 2) {
        b->qext_size = *(volatile uint32_t *)(b->mmio + BOCHS_MMIO_QEXT_SIZE);
        if (b->qext_size < 4 || b->qext_size > mmio_size) {
            b->qext_size = 0;
        }
        bochs_set_order(b, BOCHS_ORDER_LITTLE);
    }
    bochs_load_edid(b);
    drm_dev_init(&b->drm, &bochs_driver);
    b->drm.driver_priv = b;
    b->drm.pci = &b->pci;
    err = bochs_kms_init(b);
    key = (int)pdev->bus << 8 | pdev->device << 3 | pdev->function;
    if (err == 0) {
        err = sysfs_pci_bind(key, "bochs-drm");
    }
    if (err == 0) {
        err = drm_dev_register(&b->drm);
        if (err != 0) {
            sysfs_pci_unbind(key);
        }
    }
    if (err == 0) {
        sysfs_pci_uevent(key, "bind");
    }
    if (err != 0) {
        klog(KLOG_ERR, "bochs", "register: error %d", (int)err);
        drm_dev_put(&b->drm);
        return err;
    }
    klog(KLOG_INFO, "bochs",
         "registered: bochs-drm, id 0x%x, %u KiB of video memory at 0x%lx, %s%s, card %d",
         (unsigned)b->id, (unsigned)(b->vram_size / 1024u), (unsigned long)vram_phys,
         mmio ? "registers in BAR 2" : "DISPI ports", b->edid ? ", EDID" : "", b->drm.index);
    return 0;
}

/* ------------------------------------------------------------------------
 * The PCI devices
 */

/* The functions Linux's bochs_pci_tbl names: QEMU's std-vga and Simics's. */
static const uint16_t bochs_ids[][2] = {{0x1234, 0x1111}, {0x4321, 0x1111}};

/**
 * Map the function's video memory write-combining, the whole of it or,
 * when the kernel's MMIO region has no room, the largest power of two
 * down to 4 MiB that it has. The size mapped, or 0.
 */
static uint32_t bochs_map_vram(const pci_device_t *pdev, uint32_t size, struct pci_window *w)
{
    w->bar_size = size;
    while (size >= 4u << 20) {
        if (pci_bar_map(pdev, 0, 0, size, PTE_WC, w) != 0) {
            return size;
        }
        size /= 2;
    }
    return 0;
}

/**
 * Probe one function, Linux's bochs_pci_probe: at least 4 MiB of video
 * memory, the firmware's framebuffer removed from every memory BAR the
 * function decodes, memory decoding on, the registers and video memory
 * mapped, then `bochs_bind`. The windows stay while the device is bound.
 */
static int32_t bochs_pci_probe(pci_device_t *pdev)
{
    uint64_t size = pci_bar_size64(pdev, 0), phys = pci_bar_address64(pdev, 0);
    struct pci_window *w;
    uint32_t mapped, mmio_size = 0;
    uintptr_t mmio = 0;
    int32_t err;
    int i;

    if (size < (4u << 20) || phys == 0) {
        klog(KLOG_ERR, "bochs", "%x:%x.%x: less than 4 MB video memory, ignoring device",
             (unsigned)pdev->bus, (unsigned)pdev->device, (unsigned)pdev->function);
        return -ENOMEM;
    }
    for (i = 0; i < 6; i++) {
        if (pci_bar_address64(pdev, i) != 0 && pci_bar_size64(pdev, i) != 0) {
            drm_aperture_remove_conflicting(pci_bar_address64(pdev, i), pci_bar_size64(pdev, i),
                                            "bochs-drm");
        }
    }
    w = (struct pci_window *)kzalloc(2 * sizeof(*w));
    if (!w) {
        return -ENOMEM;
    }
    pci_enable_memory_space(pdev);
    if (pci_bar_is_memory(pdev->bar[2]) && pci_bar_address64(pdev, 2) != 0) {
        mmio_size = (uint32_t)pci_bar_size64(pdev, 2);
        w[0].bar_size = mmio_size;
        mmio = pci_bar_map(pdev, 2, 0, mmio_size, PTE_NOCACHE, &w[0]);
        if (!mmio) {
            klog(KLOG_ERR, "bochs", "registers not mapped");
            kfree(w);
            return -ENOMEM;
        }
    }
    mapped = bochs_map_vram(pdev, size > 0x80000000u ? 0x80000000u : (uint32_t)size, &w[1]);
    err = mapped ? bochs_bind(pdev, mmio, mmio_size, w[1].va, phys, mapped) : -ENOMEM;
    if (err != 0) {
        if (!mapped) {
            klog(KLOG_ERR, "bochs", "video memory not mapped");
        }
        pci_bar_unmap(&w[1]);
        pci_bar_unmap(&w[0]);
        kfree(w);
    }
    return err;
}

/** The driver binds unless `bochs.modeset`, Linux's int module parameter
 * of -1 (drivers/gpu/drm/tiny/bochs.c, lines 53 to 57), is 0, or is -1
 * under `nomodeset`, as drm_pci_register_driver_if_modeset decides
 * (include/drm/drm_module.h, lines 61 to 70). */
int32_t bochs_probe(void)
{
    pci_device_t pdev;
    int32_t err, bound = 0;
    uint32_t i;
    int nth, modeset = kcmdline_param_int("bochs.modeset", -1);

    if (modeset == 0 || (modeset == -1 && kcmdline_setup("nomodeset"))) {
        return -ENODEV;
    }
    for (i = 0; i < sizeof(bochs_ids) / sizeof(bochs_ids[0]); i++) {
        for (nth = 0; pci_find_nth_by_id(bochs_ids[i][0], bochs_ids[i][1], nth, &pdev); nth++) {
            err = bochs_pci_probe(&pdev);
            if (err == 0) {
                bound++;
            } else if (err != -ENODEV) {
                klog(KLOG_ERR, "bochs", "%x:%x.%x: error %d", (unsigned)pdev.bus,
                     (unsigned)pdev.device, (unsigned)pdev.function, (int)err);
            }
        }
    }
    return bound > 0 ? bound : -ENODEV;
}
