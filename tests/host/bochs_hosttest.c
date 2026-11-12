/*
 * bochs_hosttest.c: the std-vga driver, rach2/sys/drm/bochs.c, against a
 * register model: BAR 2's EDID, VGA ports, DISPI registers and QEMU's
 * extension as memory the driver reads and writes, and video memory as a
 * host buffer; bochs64_hosttest.c runs the same rows with x86-64's
 * structures.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/lib/kstd.c rach2/sys/cmdline.c
 * host-deps: tests/host/compat/resv_host.c
 * host-deps: tests/host/compat/mutex_none.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * The rows: a function whose DISPI id is not one, refused; a bind with
 * QEMU's EDID, which is Linux's bochs-drm, sets the little-endian byte
 * order through QEMU's extension and lists Linux's 15 modes, those of the
 * EDID whose two frames fit in 16 MiB, with the EDID property; a bind
 * without an EDID, which lists the DMT modes that fit, 1024x768 first and
 * preferred; SETCRTC, which programs the mode as Linux's bochs_hw_setmode
 * and shows the buffer through the virtual width and the offsets; a page
 * flip, which writes the offsets alone; BGRX8888, which sets the big-endian
 * order; a mode too large for video memory refused; the CRTC off, which
 * blanks the screen; buffers made top down in video memory and in system
 * memory once it is full; a scanout that moves the least recently shown
 * buffer out and the new one in with their bytes and zaps the mappings of
 * both; a buffer that cannot fit beside the one shown refused with ENOMEM
 * and the screen kept; a move out that finds no system memory, which
 * leaves the buffer where it was; the pages the mappings get; PRIME's
 * refusal of a foreign buffer; the PCI probe, which binds nothing under
 * `nomodeset` or with less than 4 MiB, removes the firmware's framebuffer
 * from each memory BAR and maps both; the probe under boot lines of
 * `nomodeset` and `bochs.modeset`, which bind it or not as Linux's
 * drm_pci_register_driver_if_modeset decides; and a failed registration, which
 * releases the device whole.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#define KMS_MODEL_DEVICES
#include "drm_kms_model.c"
#include "drm/bochs.c"

extern void *malloc(unsigned long size);
extern void *memset(void *s, int c, unsigned long n);
extern void *memcpy(void *d, const void *s, unsigned long n);

/* QEMU 10.2.1's EDID for std-vga at its default 1280x800, the base block
 * and a CEA extension, as tests/host/drm_edid_hosttest.c has it. */
static const uint8_t qemu_edid[256] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x49, 0x14, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00,
    0x2a, 0x18, 0x01, 0x04, 0xa5, 0x20, 0x14, 0x78, 0x06, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26,
    0x0f, 0x50, 0x54, 0x21, 0x08, 0x00, 0xe1, 0xc0, 0xd1, 0xc0, 0xd1, 0x00, 0xa9, 0x40, 0xb3, 0x00,
    0x95, 0x00, 0x81, 0x80, 0x81, 0x40, 0xea, 0x29, 0x00, 0xc0, 0x51, 0x20, 0x1c, 0x30, 0x40, 0x26,
    0x44, 0x40, 0x45, 0xcb, 0x10, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0xf7, 0x00, 0x0a, 0x00, 0x40,
    0x82, 0x00, 0x28, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x32,
    0x7d, 0x1e, 0xa0, 0xff, 0x01, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc,
    0x00, 0x51, 0x45, 0x4d, 0x55, 0x20, 0x4d, 0x6f, 0x6e, 0x69, 0x74, 0x6f, 0x72, 0x0a, 0x01, 0x3a,
    0x02, 0x03, 0x0b, 0x00, 0x46, 0x7d, 0x65, 0x60, 0x59, 0x1f, 0x61, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2f,
};

/* The modes Linux 6.12 lists for that EDID on bochs-drm, preferred first:
 * those of virtio-gpu's 26 whose two frames fit in 16 MiB. */
static const uint16_t std_vga_modes[15][2] = {
    {1280, 800},  {1920, 1080}, {1920, 1080}, {1600, 1200}, {1680, 1050},
    {1400, 1050}, {1280, 1024}, {1440, 900},  {1280, 960},  {1360, 768},
    {1280, 768},  {1024, 768},  {800, 600},   {640, 480},   {640, 480},
};

/* ---- the device: BAR 2 and 16 MiB of video memory ---- */

#define VRAM_SIZE (16u << 20)
#define VRAM_PHYS 0x80000000u
#define MMIO_SIZE 0x1000u

static uint16_t mmio_words[MMIO_SIZE / 2];
static uint8_t *const mmio = (uint8_t *)mmio_words;
static uint8_t *vram;

/** DISPI register `reg` as the driver left it. */
static uint16_t dispi(int reg)
{
    return mmio_words[(BOCHS_MMIO_DISPI + 2 * reg) / 2];
}

/** Set DISPI register `reg` as the device holds it. */
static void dispi_set(int reg, uint16_t v)
{
    mmio_words[(BOCHS_MMIO_DISPI + 2 * reg) / 2] = v;
}

/** QEMU's byte-order register. */
static uint32_t qext_order(void)
{
    uint32_t v;

    memcpy(&v, mmio + BOCHS_MMIO_QEXT_ORDER, 4);
    return v;
}

/** Reset the model: QEMU's std-vga, revision 2, its extension 8 bytes,
 * 16 MiB, and the EDID when `edid` is set. */
static void model_reset(int edid)
{
    uint32_t qext = 8;

    memset(mmio, 0, MMIO_SIZE);
    dispi_set(VBE_DISPI_INDEX_ID, 0xb0c5);
    dispi_set(VBE_DISPI_INDEX_VIDEO_MEMORY_64K, VRAM_SIZE / 65536u);
    memcpy(mmio + BOCHS_MMIO_QEXT_SIZE, &qext, 4);
    if (edid) {
        memcpy(mmio, qemu_edid, sizeof(qemu_edid));
    }
    memset(vram, 0xee, VRAM_SIZE);
}

/* ---- the kernel around the driver ---- */

/** Refused: the ports are never used with BAR 2 present. */
void outb(uint16_t port, uint8_t val)
{
    (void)port;
    (void)val;
    CHECK(0, "no VGA port is written with BAR 2 present");
}

/** Refused, as outb. */
uint8_t inb(uint16_t port)
{
    (void)port;
    CHECK(0, "no VGA port is read with BAR 2 present");
    return 0;
}

/** Refused, as outb. */
void outw(uint16_t port, uint16_t val)
{
    (void)port;
    (void)val;
    CHECK(0, "no DISPI port is written with BAR 2 present");
}

/** Refused, as outb. */
uint16_t inw(uint16_t port)
{
    (void)port;
    CHECK(0, "no DISPI port is read with BAR 2 present");
    return 0;
}

/* The device's record as the core keeps it. */
static int registered, register_rc, released;
static struct drm_device *last_dev;

/** The core's init, as drm.c's. */
void drm_dev_init(struct drm_device *d, const struct drm_driver *driver)
{
    k_memset(d, 0, sizeof(*d));
    d->driver = driver;
    d->features = driver->ident.features;
    d->index = -1;
    d->refs = 1;
    mutex_init(&d->mode_lock, "drm_mode_lock");
}

/** Registration: card 0, or the row's error. */
int32_t drm_dev_register(struct drm_device *d)
{
    if (register_rc != 0) {
        return register_rc;
    }
    d->registered = 1;
    d->index = 0;
    last_dev = d;
    registered++;
    return 0;
}

/** Take a reference. */
void drm_dev_get(struct drm_device *d)
{
    d->refs++;
}

/** Drop a reference; the last releases the record through the driver. */
void drm_dev_put(struct drm_device *d)
{
    if (d && --d->refs == 0) {
        released++;
        d->driver->release(d);
    }
}

/* The objects the driver made, for the teardown. */
#define MAX_BOS 16
static struct drm_gem_object *bos[MAX_BOS];
static int nbos;

/** The core's object init, which the model's GEM lacks. */
void drm_gem_object_init(struct drm_device *d, struct drm_gem_object *obj, uint64_t size,
                         const struct drm_gem_object_funcs *funcs)
{
    k_memset(obj, 0, sizeof(*obj));
    obj->refs = 1;
    obj->dev = d;
    obj->size = size;
    obj->funcs = funcs;
    obj->resv = &obj->resv_own;
    if (nbos < MAX_BOS) {
        bos[nbos++] = obj;
    }
}

/* The firmware's ranges the probe removed. */
static uint64_t removed_base[8], removed_size[8];
static int nremoved;

/** Record a removal. */
int32_t drm_aperture_remove_conflicting(uint64_t base, uint64_t size, const char *name)
{
    CHECK(strcmp(name, "bochs-drm") == 0, "the removal names bochs-drm");
    if (nremoved < 8) {
        removed_base[nremoved] = base;
        removed_size[nremoved] = size;
        nremoved++;
    }
    return 0;
}

/* ---- system memory: each shmem's bytes in a host buffer ---- */

#define MAX_SHMEM 16
static struct {
    struct shmem *sh;
    uint8_t *bytes;
} shm[MAX_SHMEM];
static int shmem_fail;

/** The bytes of `sh`, made zeroed of its size when `make` is set. */
static uint8_t *shm_bytes(struct shmem *sh, int make)
{
    int i, free_slot = -1;

    for (i = 0; i < MAX_SHMEM; i++) {
        if (shm[i].sh == sh) {
            return shm[i].bytes;
        }
        if (!shm[i].sh && free_slot < 0) {
            free_slot = i;
        }
    }
    if (!make || free_slot < 0) {
        return NULL;
    }
    shm[free_slot].sh = sh;
    shm[free_slot].bytes = (uint8_t *)calloc(1, (unsigned long)sh->size);
    return shm[free_slot].bytes;
}

/** Read the buffer's bytes, zeros for none made. */
int32_t shmem_read(struct shmem *sh, void *buf, uint32_t count, uint64_t off)
{
    uint8_t *b = shm_bytes(sh, 0);

    if (b) {
        memcpy(buf, b + off, count);
    } else {
        memset(buf, 0, count);
    }
    return (int32_t)count;
}

/** Write the buffer's bytes, -ENOSPC while `shmem_fail` is set. */
int32_t shmem_write(struct shmem *sh, const void *buf, uint32_t count, uint64_t off)
{
    uint8_t *b;

    if (shmem_fail) {
        return -ENOSPC;
    }
    b = shm_bytes(sh, 1);
    memcpy(b + off, buf, count);
    sh->held = (uint32_t)(sh->size >> PAGE_SHIFT);
    return (int32_t)count;
}

/** The frame of a page: an address inside the host buffer, as a number. */
uintptr_t shmem_resident(const struct shmem *sh, uint32_t pgoff)
{
    uint8_t *b = shm_bytes((struct shmem *)(uintptr_t)sh, 0);

    return b ? (uintptr_t)(b + ((uintptr_t)pgoff << PAGE_SHIFT)) : 0;
}

/** Make the buffer's bytes. */
int shmem_allocate(struct shmem *sh, uint64_t off, uint64_t len)
{
    (void)off;
    (void)len;
    return shm_bytes(sh, 1) ? 0 : -ENOSPC;
}

/** Free the buffer's bytes. */
void shmem_free(struct shmem *sh)
{
    int i;

    for (i = 0; i < MAX_SHMEM; i++) {
        if (shm[i].sh == sh) {
            free(shm[i].bytes);
            shm[i].sh = NULL;
            shm[i].bytes = NULL;
        }
    }
    sh->size = 0;
    sh->held = 0;
}

/* ---- mappings ---- */

static int zaps;
static const void *zapped[8];

/** Record a zap of `obj`'s mappings. */
void paging_zap_device(const void *obj, uint32_t pgoff, uint32_t count)
{
    (void)pgoff;
    (void)count;
    if (zaps < 8) {
        zapped[zaps] = obj;
    }
    zaps++;
}

static const struct vm_device_ops *mapped_ops;
static uint32_t mapped_cache;

/** Record a device mapping. */
int vm_device_map(vma_t *vma, const struct vm_device_ops *ops, void *obj, uint32_t cache)
{
    (void)vma;
    (void)obj;
    mapped_ops = ops;
    mapped_cache = cache;
    return 0;
}

/* ---- the PCI function ---- */

static pci_device_t vga_fn;
static int fn_present, mapped_bars[6];
static uint64_t vram_bar_size = VRAM_SIZE;
static struct pci_window *windows; /* the probe's array, BAR 2's first */

/** The model's function, the first of QEMU's ids while it is present. */
bool pci_find_nth_by_id(uint16_t vendor, uint16_t device, int nth, pci_device_t *out)
{
    if (!fn_present || nth != 0 || vendor != 0x1234 || device != 0x1111) {
        return false;
    }
    *out = vga_fn;
    return true;
}

/** BAR 0 at VRAM_PHYS and BAR 2 above it. */
uint64_t pci_bar_address64(const pci_device_t *d, int i)
{
    (void)d;
    return i == 0 ? VRAM_PHYS : i == 2 ? 0x81000000u : 0;
}

/** BAR 0 `vram_bar_size` bytes and BAR 2 the registers' page. */
uint64_t pci_bar_size64(const pci_device_t *d, int i)
{
    (void)d;
    return i == 0 ? vram_bar_size : i == 2 ? MMIO_SIZE : 0;
}

/** A memory BAR has bit 0 clear. */
bool pci_bar_is_memory(uint32_t bar)
{
    return (bar & 1u) == 0;
}

/** Nothing to enable. */
void pci_enable_memory_space(pci_device_t *d)
{
    (void)d;
}

/** Map BAR 0 to the host's video memory and BAR 2 to the registers. */
uintptr_t pci_bar_map(const pci_device_t *d, int i, uint64_t offset, size_t size, pte_t cache,
                      struct pci_window *w)
{
    (void)d;
    (void)offset;
    CHECK((i == 0 && cache == PTE_WC && size == VRAM_SIZE) ||
              (i == 2 && cache == PTE_NOCACHE && size == MMIO_SIZE),
          "video memory is mapped write-combining whole and the registers uncached");
    mapped_bars[i]++;
    if (i == 2) {
        windows = w;
    }
    w->va = i == 0 ? (uintptr_t)vram : (uintptr_t)mmio;
    w->size = size;
    return w->va;
}

/** Unmap a window. */
void pci_bar_unmap(struct pci_window *w)
{
    w->va = 0;
}

/* ---- the rows ---- */

static struct bochs_device *b;
static struct drm_file fa;
static uint32_t fbs[16];
static int nfbs;

/** Bind the model's function as the probe would, with BAR 2 and the whole
 * of video memory; the result. */
static int32_t bind(void)
{
    vga_fn.vendor_id = 0x1234;
    vga_fn.device_id = 0x1111;
    vga_fn.revision = 2;
    vga_fn.bar[0] = VRAM_PHYS | 8u;
    vga_fn.bar[2] = 0x81000000u;
    return bochs_bind(&vga_fn, (uintptr_t)mmio, MMIO_SIZE, (uintptr_t)vram, VRAM_PHYS, VRAM_SIZE);
}

/** Take the device the last bind registered and open the master's file on
 * it. */
static void take_device(void)
{
    b = to_bochs(last_dev);
    file_open(&fa, &b->drm);
    b->drm.master = &fa;
    fa.is_master = 1;
}

/** Remove every framebuffer, free every buffer and drop the device, whose
 * last reference releases it. */
static void unbind(void)
{
    int i;

    for (i = 0; i < nfbs; i++) {
        CHECK(drm_mode_rmfb_ioctl(&b->drm, &fbs[i], &fa) == 0, "RMFB");
    }
    nfbs = 0;
    for (i = 0; i < MAX_HANDLES; i++) {
        if (handles[i].file == &fa) {
            handles[i].file = NULL;
        }
    }
    for (i = 0; i < nbos; i++) {
        bos[i]->funcs->free(bos[i]);
    }
    nbos = 0;
    drm_dev_put(&b->drm);
    b = NULL;
}

static struct drm_mode_modeinfo modes[64];
static uint32_t props[32];

/** GETCONNECTOR as the master, which probes, then again for the list; the
 * number of modes, with the modes in `modes` and the property IDs in
 * `props`, and the count of properties in `*nprops`. */
static uint32_t get_modes(uint32_t *nprops)
{
    struct drm_mode_get_connector r;
    uint64_t values[32];

    k_memset(&r, 0, sizeof(r));
    r.connector_id = b->connector.base.id;
    CHECK(drm_mode_getconnector(&b->drm, &r, &fa) == 0, "GETCONNECTOR probes");
    CHECK(r.count_modes <= 64 && r.count_props <= 32, "the lists fit the row's arrays");
    r.modes_ptr = UPTR(modes);
    r.props_ptr = UPTR(props);
    r.prop_values_ptr = UPTR(values);
    r.count_encoders = 0;
    CHECK(drm_mode_getconnector(&b->drm, &r, &fa) == 0, "GETCONNECTOR lists");
    CHECK(r.connector_type == DRM_MODE_CONNECTOR_VIRTUAL && r.connection == 1,
          "the connector is a virtual one, connected");
    *nprops = r.count_props;
    return r.count_modes;
}

/** The probed mode of `w` by `h`. */
static const struct drm_mode_modeinfo *mode_of(uint32_t w, uint32_t h)
{
    const struct drm_display_mode *m;

    for (m = b->connector.modes; m; m = m->next) {
        if (m->info.hdisplay == w && m->info.vdisplay == h) {
            return &m->info;
        }
    }
    return NULL;
}

/** MODE_CREATE_DUMB of `w` by `h` at 32 bits; its object, with its handle
 * in `*handle`. */
static struct bochs_bo *dumb(uint32_t w, uint32_t h, uint32_t *handle)
{
    struct drm_mode_create_dumb d;

    k_memset(&d, 0, sizeof(d));
    d.width = w;
    d.height = h;
    d.bpp = 32;
    CHECK(bochs_dumb_create(&fa, &b->drm, &d) == 0, "MODE_CREATE_DUMB");
    CHECK(d.pitch == w * 4u &&
              d.size == ((uint64_t)w * h * 4u + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE,
          "a dumb buffer's rows are unpadded and its size whole pages");
    *handle = d.handle;
    return (struct bochs_bo *)bos[nbos - 1];
}

/** ADDFB2 of `handle`, `w` by `h` in `format`; the ID, 0 on a refusal. */
static uint32_t addfb(uint32_t handle, uint32_t w, uint32_t h, uint32_t format)
{
    struct drm_mode_fb_cmd2 r;

    k_memset(&r, 0, sizeof(r));
    r.width = w;
    r.height = h;
    r.pixel_format = format;
    r.handles[0] = handle;
    r.pitches[0] = w * 4u;
    if (drm_mode_addfb2_ioctl(&b->drm, &r, &fa) != 0) {
        return 0;
    }
    fbs[nfbs++] = r.fb_id;
    return r.fb_id;
}

/** SETCRTC of `fb_id` in `mode` at (x, y), or off for `mode` NULL. */
static int32_t setcrtc(uint32_t fb_id, const struct drm_mode_modeinfo *mode, uint32_t x, uint32_t y)
{
    struct drm_mode_crtc s;
    uint32_t conn = b->connector.base.id;

    k_memset(&s, 0, sizeof(s));
    s.crtc_id = b->crtc.base.id;
    if (mode) {
        s.fb_id = fb_id;
        s.x = x;
        s.y = y;
        s.mode_valid = 1;
        s.mode = *mode;
        s.set_connectors_ptr = UPTR(&conn);
        s.count_connectors = 1;
    }
    return drm_mode_setcrtc(&b->drm, &s, &fa);
}

/** PAGE_FLIP to `fb_id`. */
static int32_t flip(uint32_t fb_id)
{
    struct drm_mode_crtc_page_flip f;

    k_memset(&f, 0, sizeof(f));
    f.crtc_id = b->crtc.base.id;
    f.fb_id = fb_id;
    return drm_mode_page_flip_ioctl(&b->drm, &f, &fa);
}

/** Whether the DISPI offsets show byte `addr` of video memory as the first
 * pixel with rows `pitch` bytes long. */
static int shows(uint32_t addr, uint32_t pitch)
{
    return dispi(VBE_DISPI_INDEX_VIRT_WIDTH) == pitch / 4u &&
           dispi(VBE_DISPI_INDEX_Y_OFFSET) == addr / pitch &&
           dispi(VBE_DISPI_INDEX_X_OFFSET) == (addr % pitch) / 4u;
}

/** Whether `n` bytes at `p` are all `v`. */
static int all(const uint8_t *p, uint32_t n, uint8_t v)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (p[i] != v) {
            return 0;
        }
    }
    return 1;
}

/** A function whose DISPI id is not one is refused, and nothing kept. */
static void row_refusal(void)
{
    int base = allocations;

    model_reset(1);
    dispi_set(VBE_DISPI_INDEX_ID, 0xffff);
    CHECK(bind() == -ENODEV && registered == 0 && allocations == base,
          "a function whose DISPI id is 0xffff is not bound");
}

/** The bind with QEMU's EDID: the identity, the byte order and Linux's 15
 * modes, 1280x800 preferred, with the EDID property. */
static void row_edid(void)
{
    uint32_t i, n, nprops = 0;
    int32_t err = 0;
    int edid_prop = 0;

    model_reset(1);
    CHECK(bind() == 0 && registered == 1, "std-vga binds");
    take_device();
    CHECK(strcmp(b->drm.driver->ident.name, "bochs-drm") == 0 && b->id == 0xb0c5 &&
              b->vram_size == VRAM_SIZE && b->qext_size == 8,
          "the device is bochs-drm, DISPI 0xb0c5, with 16 MiB and QEMU's extension");
    CHECK(qext_order() == BOCHS_ORDER_LITTLE, "the bind sets the little-endian byte order");
    CHECK(b->edid && b->edid_len == 256, "the bind reads the EDID and its extension");
    n = get_modes(&nprops);
    CHECK(n == 15, "the EDID gives Linux's 15 modes that fit two frames in 16 MiB");
    for (i = 0; i < n && i < 15; i++) {
        CHECK(modes[i].hdisplay == std_vga_modes[i][0] && modes[i].vdisplay == std_vga_modes[i][1],
              "the modes come in Linux's order");
    }
    CHECK(modes[0].type & DRM_MODE_TYPE_PREFERRED, "1280x800 is preferred");
    for (i = 0; i < nprops; i++) {
        edid_prop |= props[i] == b->drm.mode_config.edid_property->base.id;
    }
    CHECK(edid_prop, "the connector has the EDID property");
    CHECK(b->drm.driver->gem_prime_import(&b->drm, NULL, &err) == NULL && err == -EINVAL,
          "PRIME refuses a foreign buffer with EINVAL");
    unbind();
}

/** Without an EDID, the DMT modes that fit, 1024x768 preferred, and no
 * EDID property. */
static void row_no_edid(void)
{
    uint32_t i, n, nprops = 0;
    int fit = 1, edid_prop = 0;

    model_reset(0);
    CHECK(bind() == 0, "std-vga binds without an EDID");
    take_device();
    CHECK(!b->edid, "no EDID is read");
    n = get_modes(&nprops);
    CHECK(n > 0 && modes[0].hdisplay == 1024 && modes[0].vdisplay == 768 &&
              (modes[0].type & DRM_MODE_TYPE_PREFERRED),
          "the DMT list has 1024x768 first, preferred");
    for (i = 0; i < n; i++) {
        fit &= (uint32_t)modes[i].hdisplay * modes[i].vdisplay * 4u <= VRAM_SIZE / 2;
    }
    CHECK(fit && mode_of(1920, 1080) && !mode_of(2560, 1600),
          "each DMT mode has two frames in 16 MiB, and 2560x1600 does not");
    for (i = 0; i < nprops; i++) {
        edid_prop |= props[i] == b->drm.mode_config.edid_property->base.id;
    }
    CHECK(!edid_prop, "no EDID property without an EDID");
    unbind();
}

/* The buffers of a row and their framebuffers. */
static struct bochs_bo *bo_a, *bo_b, *bo_c, *bo_d, *bo_e;
static uint32_t id_a, id_b, id_c, id_d, id_e, id_bgr, id_big;

/** Two buffers of 1280x800 and one wider, and SETCRTC of the first. */
static void setcrtc_on(void)
{
    uint32_t ha, hc, he, nprops;

    model_reset(1);
    CHECK(bind() == 0, "std-vga binds");
    take_device();
    get_modes(&nprops);
    bo_a = dumb(1280, 800, &ha);
    bo_c = dumb(1280, 800, &hc);
    CHECK(bo_a->in_vram && bo_a->offset == VRAM_SIZE - 4096000u && bo_c->in_vram &&
              bo_c->offset == bo_a->offset - 4096000u,
          "dumb buffers are made top down in video memory");
    CHECK(all(vram + bo_c->offset, 4096000u, 0), "a dumb buffer is zeroed");
    bo_e = dumb(1296, 810, &he);
    id_a = addfb(ha, 1280, 800, DRM_FORMAT_XRGB8888);
    id_c = addfb(hc, 1280, 800, DRM_FORMAT_XRGB8888);
    id_e = addfb(he, 1296, 810, DRM_FORMAT_XRGB8888);
    id_bgr = addfb(ha, 1280, 800, DRM_FORMAT_BGRX8888);
    CHECK(id_a && id_c && id_e && id_bgr, "ADDFB2 in XRGB8888 and BGRX8888");
    CHECK(setcrtc(id_a, mode_of(1280, 800), 0, 0) == 0, "SETCRTC");
    CHECK(dispi(VBE_DISPI_INDEX_ENABLE) == 0xc1 && dispi(VBE_DISPI_INDEX_BPP) == 32 &&
              dispi(VBE_DISPI_INDEX_XRES) == 1280 && dispi(VBE_DISPI_INDEX_YRES) == 800 &&
              dispi(VBE_DISPI_INDEX_BANK) == 0 &&
              dispi(VBE_DISPI_INDEX_VIRT_HEIGHT) == VRAM_SIZE / 5120u,
          "SETCRTC programs 1280x800 at 32 bits, the virtual height all of video memory, kept");
    CHECK(shows(bo_a->offset, 5120), "the offsets show the buffer");
    CHECK(mmio[BOCHS_MMIO_VGA] == 0x20 && qext_order() == BOCHS_ORDER_LITTLE,
          "the screen is shown, little-endian");
    CHECK(bo_a->pin_count == 1, "the buffer shown is pinned");
    CHECK(setcrtc(id_a, mode_of(1280, 800), 0, 0) == 0 && bo_a->pin_count == 1,
          "a SETCRTC of the same buffer keeps one pin");
    CHECK(setcrtc(id_e, mode_of(1280, 800), 8, 2) == 0 && shows(bo_e->offset + 2 * 5184 + 32, 5184),
          "SETCRTC's x and y and a wider buffer's pitch move the first pixel");
    CHECK(bo_e->pin_count == 1 && bo_a->pin_count == 0, "SETCRTC moves the pin to the new buffer");
}

/** A flip, and the byte order of each format. */
static void setcrtc_flip(void)
{
    CHECK(setcrtc(id_a, mode_of(1280, 800), 0, 0) == 0 && flip(id_c) == 0, "PAGE_FLIP");
    CHECK(shows(bo_c->offset, 5120) && dispi(VBE_DISPI_INDEX_ENABLE) == 0xc1 &&
              dispi(VBE_DISPI_INDEX_XRES) == 1280,
          "the flip shows the other buffer");
    dispi_set(VBE_DISPI_INDEX_XRES, 0x1234);
    CHECK(flip(id_a) == 0 && shows(bo_a->offset, 5120) && dispi(VBE_DISPI_INDEX_XRES) == 0x1234,
          "a flip writes the offsets alone");
    CHECK(bo_a->pin_count == 1 && bo_c->pin_count == 0,
          "a flip pins the new buffer and unpins the old");
    CHECK(setcrtc(id_bgr, mode_of(1280, 800), 0, 0) == 0 && qext_order() == BOCHS_ORDER_BIG,
          "BGRX8888 sets the big-endian order");
    CHECK(setcrtc(id_a, mode_of(1280, 800), 0, 0) == 0 && qext_order() == BOCHS_ORDER_LITTLE,
          "XRGB8888 sets the little-endian order back");
}

/** A mode too large, a buffer too large and the CRTC off. */
static void setcrtc_refusals(void)
{
    struct drm_mode_modeinfo big;
    uint32_t hbig;

    big = *mode_of(1280, 800);
    big.hdisplay = 2560;
    big.hsync_start = 2600;
    big.hsync_end = 2700;
    big.htotal = 2800;
    big.vdisplay = 1600;
    big.vsync_start = 1603;
    big.vsync_end = 1609;
    big.vtotal = 1650;
    (void)dumb(2560, 1600, &hbig);
    id_big = addfb(hbig, 2560, 1600, DRM_FORMAT_XRGB8888);
    dispi_set(VBE_DISPI_INDEX_XRES, 1280);
    CHECK(setcrtc(id_big, &big, 0, 0) == -EINVAL && dispi(VBE_DISPI_INDEX_XRES) == 1280 &&
              b->plane.state->fb->base.id == id_a,
          "a mode whose two frames pass 16 MiB is refused, the mode kept");
    zaps = 0;
    CHECK(setcrtc(id_big, mode_of(1280, 800), 0, 0) == -ENOMEM &&
              b->plane.state->fb->base.id == id_a && shows(bo_a->offset, 5120),
          "a buffer too large beside the one shown: ENOMEM, the screen kept");
    CHECK(zaps == 2 && zapped[0] == bo_e && zapped[1] == bo_c && bo_a->in_vram,
          "its pin moved the unpinned buffers out, the least recently shown first");
    CHECK(setcrtc(0, NULL, 0, 0) == 0 && mmio[BOCHS_MMIO_VGA] == 0 && bo_a->pin_count == 0,
          "the CRTC off blanks the screen and unpins");
}

/** SETCRTC, a flip, BGRX8888, a mode too large and the CRTC off. */
static void row_setcrtc(void)
{
    setcrtc_on();
    setcrtc_flip();
    setcrtc_refusals();
    unbind();
}

#define BIG_SIZE (1920u * 1080u * 4u)

/** Two buffers of 1920x1080 that fill video memory, one more and one of
 * 1920x1200 in system memory, each filled with its own byte, and the first
 * two shown in turn. */
static void moves_setup(void)
{
    uint32_t ha, hb, hc, hd, nprops;
    uintptr_t phys;

    model_reset(1);
    CHECK(bind() == 0, "std-vga binds");
    take_device();
    get_modes(&nprops);
    bo_a = dumb(1920, 1080, &ha);
    bo_b = dumb(1920, 1080, &hb);
    bo_c = dumb(1920, 1080, &hc);
    bo_d = dumb(1920, 1200, &hd);
    CHECK(bo_a->in_vram && bo_b->in_vram && !bo_c->in_vram && !bo_d->in_vram,
          "two 1920x1080 buffers fill video memory, the next go to system memory");
    memset(vram + bo_a->offset, 0xa1, BIG_SIZE);
    memset(vram + bo_b->offset, 0xb2, BIG_SIZE);
    CHECK(bochs_vm_page(bo_c, 0, &phys) == 0, "a page of a buffer in system memory is given");
    memset(shm_bytes(&bo_c->pages, 0), 0xc3, BIG_SIZE);
    id_a = addfb(ha, 1920, 1080, DRM_FORMAT_XRGB8888);
    id_b = addfb(hb, 1920, 1080, DRM_FORMAT_XRGB8888);
    id_c = addfb(hc, 1920, 1080, DRM_FORMAT_XRGB8888);
    id_d = addfb(hd, 1920, 1200, DRM_FORMAT_XRGB8888);
    CHECK(setcrtc(id_a, mode_of(1920, 1080), 0, 0) == 0 && flip(id_b) == 0,
          "SETCRTC and a flip in video memory");
}

/** A flip to the buffer in system memory, which moves the first out and
 * itself in. */
static void moves_scanout(void)
{
    zaps = 0;
    CHECK(flip(id_c) == 0, "a flip to a buffer in system memory");
    CHECK(!bo_a->in_vram && all(shm_bytes(&bo_a->pages, 0), BIG_SIZE, 0xa1),
          "the least recently shown buffer moves out with its bytes");
    CHECK(bo_c->in_vram && bo_c->offset == bo_b->offset + BIG_SIZE &&
              all(vram + bo_c->offset, BIG_SIZE, 0xc3) && !shm_bytes(&bo_c->pages, 0),
          "the new buffer moves in with its bytes, its system pages freed");
    CHECK(zaps == 2 && zapped[0] == bo_a && zapped[1] == bo_c,
          "both moves zap the buffer's mappings");
    CHECK(shows(bo_c->offset, 7680) && bo_c->pin_count == 1 && bo_b->pin_count == 0,
          "the moved buffer is shown");
}

/** A move out with no system memory, and a buffer that cannot fit beside
 * the one shown. */
static void moves_refusals(void)
{
    uint32_t b_off = bo_b->offset;

    shmem_fail = 1;
    CHECK(flip(id_a) == -ENOMEM && bo_b->in_vram && bo_b->offset == b_off &&
              all(vram + bo_b->offset, BIG_SIZE, 0xb2) && !bo_a->in_vram,
          "a move out that finds no system memory leaves the buffer: ENOMEM");
    shmem_fail = 0;
    CHECK(b->plane.state->fb->base.id == id_c && shows(bo_c->offset, 7680) && bo_c->pin_count == 1,
          "the refused flip leaves the screen");
    CHECK(flip(id_d) == -ENOMEM && !bo_d->in_vram && bo_d->pin_count == 0 && bo_c->pin_count == 1 &&
              b->plane.state->fb->base.id == id_c && shows(bo_c->offset, 7680),
          "a buffer that cannot fit beside the pinned one: ENOMEM, the screen kept");
    CHECK(!bo_b->in_vram && all(shm_bytes(&bo_b->pages, 0), BIG_SIZE, 0xb2),
          "the unpinned buffer moved out for it keeps its bytes");
}

/** The pages a mapping gets, in video memory and in system memory. */
static void moves_pages(void)
{
    uintptr_t phys;

    CHECK(bochs_vm_page(bo_c, 3, &phys) == 0 && phys == VRAM_PHYS + bo_c->offset + 3 * PAGE_SIZE,
          "a page of a buffer in video memory is its page there");
    CHECK(bochs_vm_page(bo_a, 3, &phys) == 0 &&
              phys == (uintptr_t)shm_bytes(&bo_a->pages, 0) + 3 * PAGE_SIZE,
          "a page of a buffer in system memory is its system page");
    CHECK(bochs_vm_page(bo_a, BIG_SIZE / PAGE_SIZE, &phys) != 0,
          "a page past the buffer is refused");
    CHECK(bochs_bo_mmap(&bo_a->base, NULL) == 0 && mapped_ops == &bochs_vm_ops &&
              mapped_cache == VM_PROT_WC,
          "a buffer maps write-combining through the page hook");
}

/** Buffers past video memory, the moves a scanout makes, the refusals and
 * the mappings' pages. */
static void row_moves(void)
{
    moves_setup();
    moves_scanout();
    moves_refusals();
    moves_pages();
    unbind();
}

/** The PCI probe: BAR 0 too small refused, `nomodeset`, and a bind that
 * removes the firmware's framebuffer from each BAR and maps both. */
static void row_probe(void)
{
    int base = allocations;

    model_reset(1);
    fn_present = 1;
    vga_fn.bar[2] = 0x81000000u;
    (void)kcmdline_capture("nomodeset");
    CHECK(bochs_probe() == -ENODEV && nremoved == 0 && registered == 0, "nomodeset binds nothing");
    (void)kcmdline_capture("");
    vram_bar_size = 2u << 20;
    CHECK(bochs_probe() == -ENODEV && nremoved == 0 && registered == 0,
          "a function with 2 MiB of video memory is refused");
    vram_bar_size = VRAM_SIZE;
    CHECK(bochs_probe() == 1 && registered == 1, "the probe binds the function");
    CHECK(nremoved == 2 && removed_base[0] == VRAM_PHYS && removed_size[0] == VRAM_SIZE &&
              removed_base[1] == 0x81000000u && removed_size[1] == MMIO_SIZE,
          "the probe removes the firmware's framebuffer from BAR 0 and BAR 2");
    CHECK(mapped_bars[0] == 1 && mapped_bars[2] == 1, "the probe maps both BARs");
    take_device();
    CHECK(b->mmio == (uintptr_t)mmio && b->vram == (uintptr_t)vram && b->vram_phys == VRAM_PHYS,
          "the device has the windows");
    unbind();
    kfree(windows);
    CHECK(allocations == base, "the probe's windows are all it keeps");
    fn_present = 0;
}

/* Boot lines and whether the probe binds under them: `nomodeset` matched by
 * its first bytes, as Linux's obsolete_checksetup, and `bochs.modeset` read
 * as Linux's int module parameter, the last word whose value kstrtoint
 * takes in base 0 counting. */
static const struct {
    const char *line;
    int binds;
} modesets[] = {
    {"", 1},
    {"nomodeset", 0},
    {"nomodeset=1", 0},
    {"nomodesetx", 0},
    {"bochs.modeset=0", 0},
    {"bochs.modeset=0x0", 0},
    {"bochs.modeset=x", 1},
    {"nomodeset bochs.modeset=1", 1},
    {"nomodeset bochs.modeset=0x10", 1},
    {"nomodeset bochs-modeset=1", 0}, /* another name: `-` is `_` only */
    {"nomodeset bochs.modeset=0", 0},
    {"nomodeset bochs.modeset=0x0", 0},
    {"nomodeset bochs.modeset=-1", 0},
    {"nomodeset bochs.modeset=x", 0},          /* no int: -1 stays */
    {"nomodeset bochs.modeset", 0},            /* bare: parse_one refuses it */
    {"nomodeset bochs.modeset=", 0},           /* empty: kstrtoint refuses it */
    {"nomodeset bochs.modeset=1x", 0},         /* trailing bytes: refused */
    {"nomodeset bochs.modeset=4294967297", 0}, /* past an int: refused */
    {"nomodeset bochs.modeset=1 bochs.modeset=x", 1},
    {"nomodeset bochs.modeset=1 bochs.modeset=0", 0},
    {"nomodeset bochs.modeset=0 bochs.modeset=1", 1},
    {"nomodeset bochs.modeset=\"1\"", 1},
    {"nomodeset -- bochs.modeset=1", 0}, /* init's words */
};

/** The probe under each line of `modesets`, which binds and drops the
 * function or binds nothing. */
static void row_modeset(void)
{
    unsigned int i;
    int32_t rc;
    int base = allocations;

    fn_present = 1;
    vga_fn.bar[2] = 0x81000000u;
    for (i = 0; i < sizeof(modesets) / sizeof(modesets[0]); i++) {
        model_reset(1);
        (void)kcmdline_capture(modesets[i].line);
        registered = 0;
        nremoved = 0;
        rc = bochs_probe();
        if (rc != (modesets[i].binds ? 1 : -ENODEV) || registered != modesets[i].binds) {
            failures++;
            printf("%s: fail: \"%s\" should %s the driver\n", test_name, modesets[i].line,
                   modesets[i].binds ? "bind" : "not bind");
        }
        if (rc == 1) {
            take_device();
            unbind();
            kfree(windows);
        }
    }
    (void)kcmdline_capture("");
    CHECK(allocations == base, "the binds under the lines keep nothing");
    fn_present = 0;
}

/** A registration that fails releases the device whole. */
static void row_register_fails(void)
{
    int base = allocations;

    model_reset(1);
    register_rc = -ENOSPC;
    released = 0;
    CHECK(bind() == -ENOSPC && released == 1 && allocations == base,
          "a failed registration releases the device and its EDID");
    register_rc = 0;
}

int main(void)
{
    int base;

    model_start("bochs-test");
    vram = (uint8_t *)malloc(VRAM_SIZE);
    base = allocations;
    row_refusal();
    row_edid();
    registered = 0;
    row_no_edid();
    row_setcrtc();
    row_moves();
    registered = 0;
    row_probe();
    row_modeset();
    row_register_fails();
    CHECK(allocations == base, "every allocation freed");
    free(vram);
    if (failures) {
        return 1;
    }
    printf("bochs-test: ok\n");
    return 0;
}
