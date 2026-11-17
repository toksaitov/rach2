/*
 * vkms_hosttest.c: the software display, rach2/sys/drm/vkms.c, with the
 * mode-setting core whole, against a model clock, buffer objects in host
 * memory and the test's own composition and CRC of each expected frame;
 * vkms64_hosttest.c runs the same rows with x86-64's structures.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/resv_host.c
 * host-deps: tests/host/compat/mutex_none.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * The rows: no device without `rach2.vkms`; a device with Linux's objects,
 * a primary and a cursor plane of five formats with rotation, a CRTC with
 * a gamma table of 256 entries, a virtual connector of the DMT modes with
 * 1024x768 preferred; eight overlays and no cursor through vkms's module
 * parameters; a second probe refused and a failed registration released
 * whole (76-12.4). A mode set at 640x480 with the cursor over the primary
 * plane, composed with the cursor's opaque, clear and half-covered pixels
 * blended, the CRC equal to the test's own of the frame it expects
 * (76-12.1). Vblanks from the tick at the mode's rate over the model's
 * clock, none before their time, each stamped with the end of its frame
 * however late its tick, the next deadline published, a tick that comes
 * frames late counting them all at once with the latest's time and sending
 * an event asked for meanwhile at that time, and the timer off five seconds
 * after the last holder (76-12.2).
 * Equal frames with equal CRCs and consecutive frame numbers, a composer
 * that fell behind entering each frame, a flip that changes the CRC and
 * one back that restores it, a gamma table of zeros that makes the frame
 * black, a table of 257 entries refused, the primary turned by 180
 * degrees and the cursor by 90 and reflected, and the cursor in RGB565 and
 * ARGB16161616 (76-12.3).
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#define KMS_MODEL_DEVICES
#define KMS_MODEL_KERNEL
#include "drm_kms_model.c"
#include "drm/vkms.c"

/** Divide with the host's 64-bit division, for any divisor. */
uint64_t tick_div64(uint64_t n, uint64_t d, uint64_t *rem)
{
    if (rem) {
        *rem = n % d;
    }
    return n / d;
}

extern int strncmp(const char *a, const char *b, unsigned long n);

/* The fine service deadline vkms last published, in monotonic ns. */
static int64_t wake_at_ns;

/** Keep the deadline, which the rows compare with the timer's. */
void timer_service_wake_at(int64_t ns)
{
    wake_at_ns = ns;
}

/* ---- the kernel around the driver ---- */

/** No log. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/* The boot line the row sets, words separated by spaces. */
static const char *cmdline = "";

/** Whether `word` is a word of the line. */
int kcmdline_has(const char *word)
{
    const char *p = cmdline;
    size_t n = k_strlen(word);

    while (*p) {
        if (strncmp(p, word, n) == 0 && (p[n] == ' ' || p[n] == '\0')) {
            return 1;
        }
        while (*p && *p != ' ') {
            p++;
        }
        while (*p == ' ') {
            p++;
        }
    }
    return 0;
}

/** The value of `key=` on the line. */
int kcmdline_value(const char *key, char *out, unsigned int outlen)
{
    const char *p = cmdline;
    size_t n = k_strlen(key), i;

    out[0] = '\0';
    while (*p) {
        if (strncmp(p, key, n) == 0 && p[n] == '=') {
            for (i = 0; p[n + 1 + i] && p[n + 1 + i] != ' '; i++) {
                if (i + 1 >= outlen) {
                    out[0] = '\0';
                    return 0;
                }
                out[i] = p[n + 1 + i];
            }
            out[i] = '\0';
            return 1;
        }
        while (*p && *p != ' ') {
            p++;
        }
        while (*p == ' ') {
            p++;
        }
    }
    return 0;
}

static int irq_depth;

/** Interrupts off, counted. */
uint32_t kirq_save(void)
{
    irq_depth++;
    return 0;
}

/** Interrupts back. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
    CHECK(irq_depth > 0, "interrupts are restored no more than saved");
    irq_depth--;
}

/* The device's record as the core keeps it. */
static int register_rc, released;

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

/** Never called: the rows make their buffers through the model. */
int32_t drm_gem_shmem_dumb_create(struct drm_file *file, struct drm_device *d,
                                  struct drm_mode_create_dumb *args)
{
    (void)file;
    (void)d;
    (void)args;
    return -ENOSYS;
}

/* Each model buffer object's bytes. */
static uint8_t *bytes[MAX_GEM];

/** The page `pgoff` of a model object: an address in its bytes. */
uintptr_t drm_gem_shmem_page(struct drm_gem_shmem_object *obj, uint32_t pgoff)
{
    int i;

    for (i = 0; i < MAX_GEM; i++) {
        if ((struct drm_gem_object *)obj == &gem[i] && bytes[i]) {
            return (uintptr_t)(bytes[i] + ((uintptr_t)pgoff << PAGE_SHIFT));
        }
    }
    return 0;
}

/** A frame's mapping is its address. */
void *kmap_temp(uintptr_t phys)
{
    return (void *)phys;
}

/** Nothing to unmap. */
void kunmap_temp(void *va)
{
    (void)va;
}

/* ---- the test's own frame and CRC ---- */

#define W 640
#define H 480

/* The expected frame, alpha, red, green and blue of 16 bits a pixel. */
static uint16_t want[H][W][4];

/** CRC-32 of `len` bytes from `crc`, reflected, bit by bit, no
 * inversion. */
static uint32_t crc_bits(uint32_t crc, const uint8_t *p, size_t len)
{
    int k;

    while (len-- > 0) {
        crc ^= *p++;
        for (k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

/** The CRC of `want`, row by row, each pixel's channels little-endian. */
static uint32_t want_crc(void)
{
    uint8_t row[W * 8];
    uint32_t crc = 0;
    int x, y, c;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            for (c = 0; c < 4; c++) {
                row[8 * x + 2 * c] = (uint8_t)want[y][x][c];
                row[8 * x + 2 * c + 1] = (uint8_t)(want[y][x][c] >> 8);
            }
        }
        crc = crc_bits(crc, row, sizeof(row));
    }
    return crc;
}

/** Fill `want` opaque black. */
static void want_black(void)
{
    int x, y;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            want[y][x][0] = 0xffff;
            want[y][x][1] = want[y][x][2] = want[y][x][3] = 0;
        }
    }
}

/** Blend the premultiplied pixel `px` over `want` at (x, y), rounding to
 * the nearest, in 64 bits. */
static void want_over(int x, int y, const uint16_t *px)
{
    int c;
    uint64_t v;

    want[y][x][0] = 0xffff;
    for (c = 1; c < 4; c++) {
        v = (uint64_t)px[c] * 65535u + (uint64_t)want[y][x][c] * (65535u - px[0]);
        want[y][x][c] = (uint16_t)((v + 32767u) / 65535u);
    }
}

/* ---- the device and its buffers ---- */

static struct vkms_device *v;
static struct drm_file fa;
static struct drm_mode_config *mc;
static struct drm_plane *primary, *cursor;
static int64_t period;

/** The clock to the next vblank, a millisecond at a time, the tick at
 * each. */
static void frame(void)
{
    uint64_t count = v->crtc.vblank.count;
    int i;

    for (i = 0; i < 1000 && v->crtc.vblank.count == count; i++) {
        model_now_ns += 1000000;
        vkms_tick();
    }
}

/** A sleep: the clock runs a millisecond at a time, ticking, until the
 * wait's condition holds. */
static void wait_hook(wq_cond_fn cond, void *arg)
{
    int i;

    for (i = 0; i < 100000 && !cond(arg); i++) {
        model_now_ns += 1000000;
        vkms_tick();
    }
}

/* A composition the composer thread took before a commit, which a wait of
 * the commit lets end, as the thread would while the committer sleeps; and
 * whether the commit's wait for the composer ran. */
static struct vkms_job *in_flight;
static int flush_waited;

/** `wait_hook`, which first ends `in_flight` when the commit waits for the
 * composer (`composer_caught_up`). */
static void flush_hook(wq_cond_fn cond, void *arg)
{
    if (cond == composer_caught_up) {
        flush_waited = 1;
        if (in_flight) {
            vkms_composer_finish(v, in_flight);
            in_flight = NULL;
        }
    }
    wait_hook(cond, arg);
}

/** Read the next entry's frame and value; 0 when there is none. */
static int next_entry(uint32_t *frame_no, uint32_t *value)
{
    char line[64];
    uint32_t vals[2] = {0, 0};
    int i, k = 0;

    if (drm_crtc_crc_read(&v->crtc, line, sizeof(line), 1) <= 0) {
        return 0;
    }
    for (i = 0; line[i] && k < 2; i++) {
        if (line[i] == 'x') {
            for (i++; (line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'f');
                 i++) {
                vals[k] =
                    vals[k] << 4 | (uint32_t)(line[i] <= '9' ? line[i] - '0' : line[i] - 'a' + 10);
            }
            k++;
        }
    }
    *frame_no = vals[0];
    *value = vals[1];
    return k == 2;
}

/** Compose what the vblanks asked for and read every entry. */
static void drain(void)
{
    char line[64];

    vkms_composer_run(v);
    while (drm_crtc_crc_read(&v->crtc, line, sizeof(line), 1) > 0) {
    }
}

/** A framebuffer of `w` by `h` in `format` over buffer object `i`, its
 * bytes zeroed in `bytes[i]`, rows of `pitch` bytes; its ID. */
static uint32_t fb_new(int i, uint32_t w, uint32_t h, uint32_t format, uint32_t pitch)
{
    struct drm_mode_fb_cmd2 r;
    uint64_t size = ((uint64_t)pitch * h + 4095u) & ~4095ull;

    free(bytes[i]);
    bytes[i] = (uint8_t *)calloc(1, (unsigned long)size);
    k_memset(&r, 0, sizeof(r));
    r.width = w;
    r.height = h;
    r.pixel_format = format;
    r.handles[0] = gem_new(&fa, i, size);
    r.pitches[0] = pitch;
    CHECK(drm_mode_addfb2_ioctl(&v->drm, &r, &fa) == 0, "a framebuffer is made");
    return r.fb_id;
}

/** Write the XRGB8888 pixel (x, y) of object `i` with rows of `pitch`. */
static void put32(int i, uint32_t pitch, int x, int y, uint32_t argb)
{
    uint8_t *p = bytes[i] + (size_t)y * pitch + (size_t)x * 4;

    p[0] = (uint8_t)argb;
    p[1] = (uint8_t)(argb >> 8);
    p[2] = (uint8_t)(argb >> 16);
    p[3] = (uint8_t)(argb >> 24);
}

/** The primary's pattern `seed` at (x, y), opaque. */
static uint32_t pattern(int seed, int x, int y)
{
    return 0xff000000u | (uint32_t)((x * 3 + seed) & 0xff) << 16 |
           (uint32_t)((y * 5 + seed * 7) & 0xff) << 8 | (uint32_t)((x ^ y) & 0xff);
}

/** Fill object `i`, a W by H XRGB8888 framebuffer, with `pattern`. */
static void fill_primary(int i, int seed)
{
    int x, y;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            put32(i, W * 4, x, y, pattern(seed, x, y) & 0x00ffffffu);
        }
    }
}

/** The cursor's premultiplied ARGB8888 pixel at (x, y) of 64 by 64: an
 * opaque red band, a clear band and a half-covered blue band. */
static uint32_t cursor_px(int x, int y)
{
    (void)x;
    if (y < 20) {
        return 0xffff0000u;
    }
    if (y < 40) {
        return 0;
    }
    return 0x80000080u | (uint32_t)(x & 0x3f) << 8;
}

/** Put the W by H primary pattern `seed` into `want`, its pixel (u, v) on
 * screen taken from the framebuffer's (fx, fy) as `rot` maps it. */
static void want_primary(int seed, int rot180)
{
    uint16_t px[4];
    uint32_t c;
    int x, y;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            c = rot180 ? pattern(seed, W - 1 - x, H - 1 - y) : pattern(seed, x, y);
            px[0] = 0xffff;
            px[1] = (uint16_t)(((c >> 16) & 0xff) * 257);
            px[2] = (uint16_t)(((c >> 8) & 0xff) * 257);
            px[3] = (uint16_t)((c & 0xff) * 257);
            want[y][x][0] = px[0];
            want[y][x][1] = px[1];
            want[y][x][2] = px[2];
            want[y][x][3] = px[3];
        }
    }
}

/** Blend the cursor, 64 by 64 at (cx, cy), over `want`; with `turned`, the
 * screen's (u, v) shows the cursor's pixel of rotation 90 reflected in x,
 * which is (v, u). */
static void want_cursor(int cx, int cy, int turned)
{
    uint16_t px[4];
    uint32_t c;
    int u, w;

    for (w = 0; w < 64; w++) {
        for (u = 0; u < 64; u++) {
            c = turned ? cursor_px(w, u) : cursor_px(u, w);
            px[0] = (uint16_t)((c >> 24) * 257);
            px[1] = (uint16_t)(((c >> 16) & 0xff) * 257);
            px[2] = (uint16_t)(((c >> 8) & 0xff) * 257);
            px[3] = (uint16_t)((c & 0xff) * 257);
            want_over(cx + u, cy + w, px);
        }
    }
}

/* ---- requests ---- */

/* An atomic request being built: objects, each with its properties. */
struct req {
    uint32_t objs[8], counts[8], props[32];
    uint64_t values[32];
    uint32_t nobj, nprop;
};

/** Add `prop` = `value` of object `obj` to `r`. */
static void req_add(struct req *r, struct drm_mode_object *obj, struct drm_property *prop,
                    uint64_t value)
{
    if (r->nobj == 0 || r->objs[r->nobj - 1] != obj->id) {
        r->objs[r->nobj] = obj->id;
        r->counts[r->nobj++] = 0;
    }
    r->counts[r->nobj - 1]++;
    r->props[r->nprop] = prop->base.id;
    r->values[r->nprop++] = value;
}

/** ATOMIC of `r` with `flags`. */
static int32_t req_commit(const struct req *r, uint32_t flags)
{
    struct drm_mode_atomic a;

    k_memset(&a, 0, sizeof(a));
    a.flags = flags;
    a.count_objs = r->nobj;
    a.objs_ptr = UPTR(r->objs);
    a.count_props_ptr = UPTR(r->counts);
    a.props_ptr = UPTR(r->props);
    a.prop_values_ptr = UPTR(r->values);
    return drm_mode_atomic_ioctl(&v->drm, &a, &fa);
}

/* A plane's place on the CRTC, in pixels. */
struct place {
    int32_t x, y;
    uint32_t w, h;
};

/** Add to `r` plane `p` showing `fb` whole at `at` on the CRTC, turned by
 * `rotation`. */
static void req_plane(struct req *r, struct drm_plane *p, uint32_t fb, struct place at,
                      uint32_t rotation)
{
    req_add(r, &p->base, mc->prop_fb_id, fb);
    req_add(r, &p->base, mc->prop_crtc_id, fb ? v->crtc.base.id : 0);
    req_add(r, &p->base, mc->prop_crtc_x, (uint64_t)(int64_t)at.x);
    req_add(r, &p->base, mc->prop_crtc_y, (uint64_t)(int64_t)at.y);
    req_add(r, &p->base, mc->prop_crtc_w, at.w);
    req_add(r, &p->base, mc->prop_crtc_h, at.h);
    req_add(r, &p->base, mc->prop_src_w, (uint64_t)at.w << 16);
    req_add(r, &p->base, mc->prop_src_h, (uint64_t)at.h << 16);
    req_add(r, &p->base, p->rotation_property, rotation);
}

/** The place `w` by `h` at (x, y). */
static struct place at(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    struct place pl;

    pl.x = x;
    pl.y = y;
    pl.w = w;
    pl.h = h;
    return pl;
}

/** The CRC of the next frame: a frame of the clock, the composer, and the
 * one entry read, whose frame number goes to `*frame_no`. */
static uint32_t next_crc(uint32_t *frame_no)
{
    char line[64];
    uint32_t vals[2] = {0, 0};
    int32_t n;
    int i, k = 0;

    frame();
    vkms_composer_run(v);
    n = drm_crtc_crc_read(&v->crtc, line, sizeof(line), 1);
    CHECK(n > 0 && !drm_crtc_crc_ready(&v->crtc), "one entry a frame");
    for (i = 0; n > 0 && line[i] && k < 2; i++) {
        if (line[i] == 'x') {
            for (i++; (line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'f');
                 i++) {
                vals[k] =
                    vals[k] << 4 | (uint32_t)(line[i] <= '9' ? line[i] - '0' : line[i] - 'a' + 10);
            }
            k++;
        }
    }
    CHECK(k == 2, "an entry is a frame and one value");
    if (frame_no) {
        *frame_no = vals[0];
    }
    return vals[1];
}

/* ---- 76-12.4: the device ---- */

static uint32_t mode_id, fb_a, fb_b, fb_cur, fb_565, fb_16, crc_first;
static const struct drm_mode_modeinfo *vga;

/** No device without the option; with it, Linux's objects. */
static void row_probe(void)
{
    cmdline = "quiet";
    CHECK(vkms_probe() == -ENODEV && !vkms_dev, "no device without rach2.vkms");
    cmdline = "quiet rach2.vkms";
    CHECK(vkms_probe() == 1 && vkms_dev, "one device with it");
    v = vkms_dev;
    mc = &v->drm.mode_config;
    CHECK(v->drm.registered && v->drm.driver_priv == v &&
              strcmp(v->drm.driver->ident.name, "vkms") == 0 &&
              v->drm.driver->ident.bus == DRM_BUS_PLATFORM &&
              v->drm.features == (DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_GEM),
          "registered as Linux's vkms on the platform bus");
    CHECK(vkms_probe() == -EBUSY, "a second probe: EBUSY");
}

/** The objects as Linux's vkms makes them by default. */
static void row_objects(void)
{
    CHECK(mc->num_plane == 2 && mc->num_crtc == 1 && mc->num_encoder == 1 && mc->num_connector == 1,
          "a primary and a cursor plane, a CRTC, an encoder and a connector");
    primary = mc->planes[0];
    cursor = mc->planes[1];
    CHECK(primary->type == DRM_PLANE_TYPE_PRIMARY && cursor->type == DRM_PLANE_TYPE_CURSOR &&
              v->crtc.primary == primary && v->crtc.cursor == cursor,
          "the primary first, the cursor last, both the CRTC's");
    CHECK(primary->format_count == 5 && primary->formats[0] == DRM_FORMAT_ARGB8888 &&
              primary->formats[4] == DRM_FORMAT_RGB565 && !primary->modifiers &&
              primary->possible_crtcs == 1,
          "five formats, linear, on the one CRTC");
    CHECK(primary->rotation_property && cursor->rotation_property &&
              primary->rotation_property != cursor->rotation_property,
          "each plane has its rotation property");
    CHECK(v->crtc.gamma_size == 256 && mc->gamma_lut_size_property && mc->max_width == 8192 &&
              mc->min_width == 10 && mc->cursor_width == 512 && mc->preferred_depth == 0,
          "a gamma table of 256, Linux's limits");
    CHECK(v->connector.connector_type == DRM_MODE_CONNECTOR_VIRTUAL &&
              v->encoder.encoder_type == DRM_MODE_ENCODER_VIRTUAL &&
              v->encoder.possible_crtcs == 1 && strcmp(v->connector.name, "Virtual-1") == 0,
          "Virtual-1 on a virtual encoder");
}

/** The connector's modes: the DMT modes to 61 Hz, 1024x768 preferred. */
static void row_modes(void)
{
    struct drm_display_mode *m;
    int n;

    n = drm_helper_probe_single_connector_modes(&v->connector, 8192, 8192);
    CHECK(n > 10 && v->connector.modes && v->connector.modes->info.hdisplay == 1024 &&
              v->connector.modes->info.vdisplay == 768 &&
              (v->connector.modes->info.type & DRM_MODE_TYPE_PREFERRED),
          "the DMT modes, 1024x768 first and preferred");
    for (m = v->connector.modes; m; m = m->next) {
        if (m->info.hdisplay == W && m->info.vdisplay == H && m->info.clock == 25175) {
            vga = &m->info;
        }
        CHECK(drm_mode_vrefresh(&m->info) <= 61, "no mode above 61 Hz");
    }
    CHECK(vga != NULL, "640x480 at 60 Hz is a mode");
}

/** Eight overlays and no cursor through the module parameters; a failed
 * registration releases the device whole. */
static void row_params(void)
{
    struct vkms_device *first = vkms_dev;
    int before = allocations, i, ok = 1;

    vkms_dev = NULL;
    cmdline = "rach2.vkms vkms.enable_overlay=1 vkms.enable_cursor=0";
    CHECK(vkms_probe() == 1 && vkms_dev && vkms_dev != first, "a device with overlays");
    if (vkms_dev && vkms_dev != first) {
        CHECK(vkms_dev->nplanes == 9 && !vkms_dev->crtc.cursor, "nine planes, no cursor");
        for (i = 1; i < 9; i++) {
            ok &= vkms_dev->planes[i].type == DRM_PLANE_TYPE_OVERLAY;
        }
        CHECK(ok, "the eight above the primary are overlays");
        vkms_dev->drm.registered = 0;
        drm_dev_put(&vkms_dev->drm);
    }
    CHECK(allocations == before, "its release gives back every allocation");
    vkms_dev = NULL;
    register_rc = -ENOSPC;
    cmdline = "rach2.vkms vkms.enable_cursor=off";
    released = 0;
    CHECK(vkms_probe() == -ENOSPC && !vkms_dev && released == 1 && allocations == before,
          "a failed registration: its error, the device released whole");
    register_rc = 0;
    vkms_dev = first;
}

/* ---- 76-12.1: composition ---- */

/** Light the CRTC at 640x480 from `fb_a` with the cursor at (100, 50); the
 * CRC of the first frame is that of the frame the test composes. */
static void row_compose(void)
{
    struct drm_mode_create_blob b;
    struct req r;
    uint32_t frame_no, x, y;

    file_open(&fa, &v->drm);
    fa.atomic = fa.universal_planes = 1;
    v->drm.master = &fa;
    fa.is_master = 1;
    k_memset(&b, 0, sizeof(b));
    b.data = UPTR(vga);
    b.length = sizeof(*vga);
    CHECK(drm_mode_createblob_ioctl(&v->drm, &b, &fa) == 0, "a mode blob");
    mode_id = b.blob_id;
    fb_a = fb_new(0, W, H, DRM_FORMAT_XRGB8888, W * 4);
    fb_b = fb_new(1, W, H, DRM_FORMAT_XRGB8888, W * 4);
    fb_cur = fb_new(2, 64, 64, DRM_FORMAT_ARGB8888, 64 * 4);
    fill_primary(0, 1);
    fill_primary(1, 2);
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            put32(2, 64 * 4, (int)x, (int)y, cursor_px((int)x, (int)y));
        }
    }
    k_memset(&r, 0, sizeof(r));
    req_add(&r, &v->crtc.base, mc->prop_mode_id, mode_id);
    req_add(&r, &v->crtc.base, mc->prop_active, 1);
    req_add(&r, &v->connector.base, mc->prop_crtc_id, v->crtc.base.id);
    req_plane(&r, primary, fb_a, at(0, 0, W, H), DRM_MODE_ROTATE_0);
    req_plane(&r, cursor, fb_cur, at(100, 50, 64, 64), DRM_MODE_ROTATE_0);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_ALLOW_MODESET) == 0, "the mode set");
    CHECK(v->scene.active && v->scene.width == W && v->scene.height == H && v->scene.nframes == 2 &&
              v->scene.frames[0].fb->base.id == fb_a && v->scene.frames[1].dst.x1 == 100 &&
              v->scene.frames[1].dst.y2 == 114,
          "the scene: the primary, then the cursor at (100, 50)");
    period = v->period_ns;
    CHECK(drm_crtc_crc_open(&v->crtc) == 0 && v->composer_enabled && v->composer_hold &&
              v->composer_started,
          "crc/data open: the composer on, holding vblanks");
    want_primary(1, 0);
    want_cursor(100, 50, 0);
    CHECK(next_crc(&frame_no) == want_crc(), "the frame composed is the one expected");
    CHECK(frame_no == (uint32_t)v->crtc.vblank.count, "its entry is this vblank's");
    CHECK(irq_depth == 0, "interrupts back on after the composer");
}

/* ---- 76-12.2: vblanks ---- */

/** WAIT_VBLANK for an event at the next vblank: the answer. */
static int32_t event_next_vblank(void)
{
    union drm_wait_vblank w;

    k_memset(&w, 0, sizeof(w));
    w.request.type = (enum drm_vblank_seq_type)(_DRM_VBLANK_RELATIVE | _DRM_VBLANK_EVENT);
    w.request.sequence = 1;
    return drm_wait_vblank_ioctl(&v->drm, &w, &fa);
}

/** Vblanks at the mode's rate over the model clock. */
static void row_vblank(void)
{
    uint64_t count;
    int64_t start, next, asked, n;
    int i;

    CHECK(period == 16683217, "a frame of 640x480 at 25.175 MHz: 800 by 525 pixels");
    CHECK(v->timer_on && drm_crtc_vblank_get(&v->crtc) == 0, "the timer runs; a hold taken");
    count = v->crtc.vblank.count;
    start = model_now_ns;
    next = v->next_ns;
    model_now_ns = next - 1;
    vkms_tick();
    CHECK(v->crtc.vblank.count == count, "none a nanosecond early");
    model_now_ns = next;
    vkms_tick();
    CHECK(v->crtc.vblank.count == count + 1 && v->next_ns == next + period, "one at its time");
    CHECK(v->crtc.vblank.time_ns == next && wake_at_ns == v->next_ns,
          "stamped with its frame's end; the next deadline published");
    count = v->crtc.vblank.count;
    start = model_now_ns;
    for (i = 0; i < 1000; i++) {
        model_now_ns += 1000000;
        vkms_tick();
    }
    CHECK(v->crtc.vblank.count == count + (uint64_t)((model_now_ns - start) / period),
          "a second of ticks: one vblank each frame");
    next = v->next_ns;
    model_now_ns = next + 3000000;
    vkms_tick();
    CHECK(v->crtc.vblank.time_ns == next && wake_at_ns == next + period,
          "a tick 3 ms late: the vblank stamped with its frame's end, not the tick's time");
    count = v->crtc.vblank.count;
    next = v->next_ns;
    model_now_ns = next + period / 2;
    nsent = 0;
    CHECK(event_next_vblank() == 0,
          "an event asked for half a frame after a vblank not yet served");
    asked = model_now_ns;
    model_now_ns = next + 9 * period + 1000;
    vkms_tick();
    CHECK(v->crtc.vblank.count == count + 10 && v->overruns == 1 &&
              v->next_ns == next + 10 * period && v->crtc.vblank.time_ns == next + 9 * period,
          "a tick nine frames late: ten counted at once, stamped with the latest, on the grid");
    CHECK(nsent == 1 && sent[0].sequence == (uint32_t)v->crtc.vblank.count &&
              (int64_t)sent[0].tv_sec * 1000000000ll + (int64_t)sent[0].tv_usec * 1000 ==
                  (next + 9 * period) / 1000 * 1000 &&
              next + 9 * period >= asked,
          "the event sent at that count and time, none before it was asked for");
    count = v->crtc.vblank.count;
    next = v->next_ns;
    model_now_ns += 1000000000ll;
    vkms_tick();
    n = (model_now_ns - next) / period + 1;
    CHECK(v->crtc.vblank.count == count + (uint64_t)n && v->overruns == 2 &&
              v->next_ns == next + n * period && v->next_ns > model_now_ns,
          "a tick a second late: every frame gone counted, the timer on its grid");
    drain();
    drm_crtc_vblank_put(&v->crtc);
    drm_crtc_crc_release(&v->crtc);
    CHECK(!v->composer_enabled && !v->composer_hold, "crc/data closed: the composer off");
    for (i = 0; i < 6000 && v->timer_on; i++) {
        model_now_ns += 1000000;
        vkms_tick();
    }
    CHECK(!v->timer_on && i >= 5000, "the timer off five seconds after the last holder");
}

/* ---- 76-12.3: CRCs ---- */

/** Commit `r`, which waits for its flip, then drop the entries of the
 * frames meanwhile. */
static void commit_flip(struct req *r, const char *what)
{
    CHECK(req_commit(r, 0) == 0, what);
    drain();
}

/** Equal frames give equal CRCs, a change changes it, and the gamma table,
 * rotations and formats reach the frame. */
static void row_crc(void)
{
    struct req r;
    uint32_t crc_a, crc_b, f1, f2, x, e[3];
    char line[64];
    int i;

    CHECK(drm_crtc_crc_open(&v->crtc) == 0 && v->timer_on, "crc/data open again: the timer on");
    crc_a = next_crc(&f1);
    crc_first = crc_a;
    CHECK(next_crc(&f2) == crc_a && f2 == f1 + 1, "equal frames, equal CRCs, the next frame");
    want_primary(1, 0);
    want_cursor(100, 50, 0);
    CHECK(crc_a == want_crc(), "the frame still the one expected");

    frame();
    frame();
    frame();
    vkms_composer_run(v);
    for (i = 0; i < 3; i++) {
        CHECK(drm_crtc_crc_read(&v->crtc, line, sizeof(line), 1) > 0, "an entry");
        e[i] = 0;
        for (x = 2; x < 10; x++) {
            e[i] = e[i] << 4 | (uint32_t)(line[x] <= '9' ? line[x] - '0' : line[x] - 'a' + 10);
        }
    }
    CHECK(e[0] == f2 + 1 && e[1] == f2 + 2 && e[2] == f2 + 3 && !drm_crtc_crc_ready(&v->crtc),
          "a composer three frames behind enters each of them");

    k_memset(&r, 0, sizeof(r));
    req_add(&r, &primary->base, mc->prop_fb_id, fb_b);
    commit_flip(&r, "a flip to the second pattern");
    crc_b = next_crc(NULL);
    want_primary(2, 0);
    want_cursor(100, 50, 0);
    CHECK(crc_b != crc_a && crc_b == want_crc(), "a new frame, a new CRC, the one expected");
    k_memset(&r, 0, sizeof(r));
    req_add(&r, &primary->base, mc->prop_fb_id, fb_a);
    commit_flip(&r, "a flip back");
    CHECK(next_crc(NULL) == crc_a, "the first frame again, the first CRC");
}

/** A commit ends the old scene's compositions before it returns, as
 * Linux's vkms_atomic_commit_tail flushes the old state's composer work:
 * frames asked for before it get the old scene's CRC, and a composition the
 * composer began from the old scene ends in the commit's wait. */
static void row_commit_flush(void)
{
    struct req r;
    struct vkms_job j;
    uint32_t crc_a, crc_b, f, f0, val;
    int n_old = 0, n_new = 0, order = 1;

    want_primary(1, 0);
    want_cursor(100, 50, 0);
    crc_a = want_crc();
    want_primary(2, 0);
    want_cursor(100, 50, 0);
    crc_b = want_crc();
    CHECK(next_crc(&f0) == crc_a, "flush/data: the first pattern shown");

    frame();
    frame();
    k_memset(&r, 0, sizeof(r));
    req_add(&r, &primary->base, mc->prop_fb_id, fb_b);
    CHECK(req_commit(&r, 0) == 0, "flush/data: a flip with two frames asked for");
    vkms_composer_run(v);
    while (next_entry(&f, &val)) {
        if (f <= f0 + 2u) {
            n_old += val == crc_a;
            order &= n_new == 0;
        } else {
            n_new += val == crc_b;
        }
    }
    CHECK(n_old == 2 && n_new >= 1 && order,
          "flush/data: the frames asked before the flip get the old CRC, those after the new");

    frame();
    vkms_composer_take(v, &j);
    CHECK(j.pending && v->compose_begun != v->compose_ended, "flush/data: a composition in flight");
    in_flight = &j;
    flush_waited = 0;
    model_wait = flush_hook;
    k_memset(&r, 0, sizeof(r));
    req_add(&r, &primary->base, mc->prop_fb_id, fb_a);
    CHECK(req_commit(&r, 0) == 0, "flush/data: a flip while the composer composes");
    model_wait = wait_hook;
    CHECK(flush_waited && in_flight == NULL && v->compose_begun == v->compose_ended,
          "flush/data: the flip waited for the old scene's composition to end");
    CHECK(next_entry(&f, &val) && val == crc_b,
          "flush/data: that composition's CRC is the old scene's");
    vkms_composer_run(v);
    n_new = 0;
    while (next_entry(&f, &val)) {
        n_new += val == crc_a;
    }
    CHECK(n_new >= 1, "flush/data: the frames after the flip get the new CRC");
}

/** A gamma table of zeros makes the frame black; 257 entries are
 * refused. */
static void row_gamma(void)
{
    struct drm_mode_create_blob b;
    struct drm_color_lut *lut;
    struct req r;

    lut = (struct drm_color_lut *)calloc(257, sizeof(*lut));
    k_memset(&b, 0, sizeof(b));
    b.data = UPTR(lut);
    b.length = 256 * sizeof(*lut);
    CHECK(drm_mode_createblob_ioctl(&v->drm, &b, &fa) == 0, "a gamma table of zeros");
    k_memset(&r, 0, sizeof(r));
    req_add(&r, &v->crtc.base, mc->gamma_lut_property, b.blob_id);
    commit_flip(&r, "the gamma table set");
    want_black();
    CHECK(next_crc(NULL) == want_crc(), "a table of zeros: a black frame");
    k_memset(&b, 0, sizeof(b));
    b.data = UPTR(lut);
    b.length = 257 * sizeof(*lut);
    CHECK(drm_mode_createblob_ioctl(&v->drm, &b, &fa) == 0, "a table of 257 entries");
    k_memset(&r, 0, sizeof(r));
    req_add(&r, &v->crtc.base, mc->gamma_lut_property, b.blob_id);
    CHECK(req_commit(&r, 0) == -EINVAL, "more entries than GAMMA_LUT_SIZE: EINVAL");
    k_memset(&r, 0, sizeof(r));
    req_add(&r, &v->crtc.base, mc->gamma_lut_property, 0);
    commit_flip(&r, "the gamma table cleared");
    CHECK(next_crc(NULL) == crc_first, "no table: the first CRC");
    free(lut);
}

/** The primary turned by 180 degrees and the cursor by 90 and
 * reflected. */
static void row_turn(void)
{
    struct req r;

    k_memset(&r, 0, sizeof(r));
    req_plane(&r, primary, fb_a, at(0, 0, W, H), DRM_MODE_ROTATE_180);
    req_plane(&r, cursor, fb_cur, at(300, 200, 64, 64), DRM_MODE_ROTATE_90 | DRM_MODE_REFLECT_X);
    commit_flip(&r, "the primary turned 180, the cursor 90 and reflected");
    want_primary(1, 1);
    want_cursor(300, 200, 1);
    CHECK(next_crc(NULL) == want_crc(), "both turned as the test turns them");
}

/** The cursor in RGB565 and ARGB16161616. */
static void row_formats(void)
{
    struct req r;
    uint32_t x, y;
    uint8_t *p;

    fb_565 = fb_new(3, 64, 64, DRM_FORMAT_RGB565, 64 * 2);
    fb_16 = fb_new(4, 64, 64, DRM_FORMAT_ARGB16161616, 64 * 8);
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            p = bytes[3] + y * 128 + x * 2;
            p[0] = (uint8_t)(x & 0x1f);
            p[1] = (uint8_t)((y & 0x1f) << 3);
            p = bytes[4] + y * 512 + x * 8;
            p[0] = p[2] = p[4] = 0;
            p[1] = p[3] = 0;
            p[5] = (uint8_t)(x * 2);
            p[6] = 0;
            p[7] = (uint8_t)(x * 2 + 1);
        }
    }
    k_memset(&r, 0, sizeof(r));
    req_plane(&r, primary, fb_a, at(0, 0, W, H), DRM_MODE_ROTATE_0);
    req_plane(&r, cursor, fb_565, at(10, 20, 64, 64), DRM_MODE_ROTATE_0);
    commit_flip(&r, "an RGB565 cursor");
    want_primary(1, 0);
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            uint16_t px[4];

            px[0] = 0xffff;
            px[1] = (uint16_t)(((y & 0x1f) * 65535u + 15u) / 31u);
            px[2] = 0;
            px[3] = (uint16_t)(((x & 0x1f) * 65535u + 15u) / 31u);
            want_over((int)(10 + x), (int)(20 + y), px);
        }
    }
    CHECK(next_crc(NULL) == want_crc(), "RGB565 widened as Linux widens it");
    k_memset(&r, 0, sizeof(r));
    req_plane(&r, cursor, fb_16, at(500, 400, 64, 64), DRM_MODE_ROTATE_0);
    commit_flip(&r, "an ARGB16161616 cursor");
    want_primary(1, 0);
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            uint16_t px[4];

            px[0] = (uint16_t)((x * 2 + 1) << 8);
            px[1] = (uint16_t)((x * 2) << 8);
            px[2] = px[3] = 0;
            want_over((int)(500 + x), (int)(400 + y), px);
        }
    }
    CHECK(next_crc(NULL) == want_crc(), "ARGB16161616 read whole");
    drm_crtc_crc_release(&v->crtc);
    CHECK(!v->composer_enabled && irq_depth == 0, "closed again");
}

int main(void)
{
    model_start("vkms-test");
    model_wait = wait_hook;
    row_probe();
    row_objects();
    row_modes();
    if (vga) {
        row_params();
        row_compose();
        row_vblank();
        row_crc();
        row_commit_flush();
        row_gamma();
        row_turn();
        row_formats();
    }
    if (failures == 0) {
        printf("vkms-test: ok\n");
    }
    return failures ? 1 : 0;
}
