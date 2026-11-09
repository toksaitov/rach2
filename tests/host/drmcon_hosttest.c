/*
 * drmcon_hosttest.c: the console as a DRM client, rach2/sys/drm/drm_client.c,
 * over the real master role (drm_auth.c) and mode setting (drm_mode.c,
 * drm_atomic.c and drm_legacy.c, through drm_kms_model.c).
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * A fake device has one CRTC, its primary plane of XRGB8888 and a virtual
 * connector, connected, with 1280x800 preferred, 1024x768 and 800x600; its
 * dumb buffers are model objects whose `vmap` is memory of the test's. The
 * console, the /fb device and the keyboard are records of what the client
 * asked of them. The loader's screen is 1024x768.
 *
 * The rows: a device registered is taken and nothing is set on it; a first
 * open takes the screen, /fb and the keyboard from the console; a master
 * that sets its own mode and exits without dropping the role gives them
 * back, the console's buffer at the loader's size on the CRTC and the
 * console drawing there, the keyboard last; DROP_MASTER does the same on
 * the same buffer; a master that comes while the console's commit runs
 * keeps the screen; `video=` and a hotplug move the console to a buffer of
 * the size named; a buffer that cannot be mapped when the master leaves is
 * forgotten until the next place, or, on a device whose framebuffers are
 * flushed, drawn through a shadow whose damaged rows a flush copies and
 * commits; the firmware's device is taken back with
 * its CRTC off and the console on the loader's framebuffer; and every
 * allocation and reference is given back.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "drm_kms_model.c"
#include "drm/drm_auth.c"
#include "drm/drm_client.c"

/* ---- the master role's listeners, as drm.c keeps them ---- */

static struct drm_master_notifier *listeners;

/** Add a listener. */
void drm_master_notifier_register(struct drm_master_notifier *n)
{
    n->next = listeners;
    listeners = n;
}

/** Run every listener. */
void drm_master_changed(struct drm_device *d, struct drm_file *master)
{
    struct drm_master_notifier *n;

    for (n = listeners; n; n = n->next) {
        n->changed(d, master);
    }
}

/* ---- the device's references and the aperture ---- */

static int firmware;

/** Whether the device is the firmware's, as the row sets it. */
int drm_aperture_is_firmware(const struct drm_device *d)
{
    (void)d;
    return firmware;
}

/* ---- the log and the command line ---- */

/** Drop a line of the log. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

static const char *video_option;

/** The value of `video=`, when the row gives one. */
int kcmdline_value(const char *key, char *out, unsigned int outlen)
{
    out[0] = '\0';
    if (!video_option || strcmp(key, "video") != 0 || k_strlen(video_option) >= outlen) {
        return 0;
    }
    k_strncpy(out, video_option, outlen);
    return 1;
}

/* ---- the console, /fb and the keyboard, as records ---- */

static int locked, attaches, detaches, forgets, holds, unholds, moves, kd_holds, kd_unholds;
static int unhold_locked, kd_unhold_after_attach;
static struct boot_framebuffer attached;
static volatile uint8_t *attached_base;
static struct boot_framebuffer loader;

bool fbcon_active = true;

/** Take the console's lock. */
uint32_t console_lock(void)
{
    CHECK(!locked, "the console's lock is not taken twice");
    locked = 1;
    return 0;
}

/** Give it back. */
void console_unlock(uint32_t flags)
{
    (void)flags;
    CHECK(locked, "the console's lock is given back once");
    locked = 0;
}

/** Record a detach, and whether it forgets the screen. */
void fbcon_detach(int forget)
{
    CHECK(locked, "fbcon is detached under the console's lock");
    detaches++;
    forgets += forget != 0;
}

/** Record an attach and what it was given. */
int fbcon_attach(const struct boot_framebuffer *fb, volatile uint8_t *base)
{
    CHECK(locked, "fbcon is attached under the console's lock");
    attached = *fb;
    attached_base = base;
    attaches++;
    return 0;
}

/** The scale the log names. */
uint32_t fbcon_scale(void)
{
    return 1;
}

/** The loader's screen. */
const struct boot_framebuffer *fbcon_boot_screen(void)
{
    return &loader;
}

/** Record /fb's hold. */
void fb_hold(void)
{
    CHECK(locked, "/fb is held under the console's lock");
    holds++;
}

/** Record /fb's release. */
void fb_unhold(void)
{
    unhold_locked = locked;
    unholds++;
}

/** Record a move of the screen. */
void fb_screen_moved(void)
{
    CHECK(locked, "a move is told under the console's lock");
    moves++;
}

/* The damage fbcon would have recorded, which a row sets. */
static uint32_t damage_top, damage_end;

/** Give the row's damage once. */
int fbcon_take_damage(uint32_t *top, uint32_t *end)
{
    CHECK(locked, "the damage is taken under the console's lock");
    if (damage_top == damage_end) {
        return 0;
    }
    *top = damage_top;
    *end = damage_end;
    damage_top = damage_end = 0;
    return 1;
}

/** A frame's address is its own: the test's pages are its memory. */
void *kmap_temp(uintptr_t phys)
{
    return (void *)phys;
}

/** Nothing to unmap. */
void kunmap_temp(void *va)
{
    (void)va;
}

/** Record the keyboard's hold. */
void kd_hold(void)
{
    CHECK(!locked, "the keyboard is held outside the console's lock");
    kd_holds++;
}

/** Record the keyboard's release, and whether the screen came back first. */
void kd_unhold(void)
{
    CHECK(!locked, "the keyboard is given back outside the console's lock");
    kd_unhold_after_attach = attaches > 0;
    kd_unholds++;
}

/* ---- the fake device ---- */

static struct drm_device dev;
static struct drm_plane primary;
static struct drm_crtc crtc;
static struct drm_encoder encoder;
static struct drm_connector connector;
static struct drm_file fa, fb_, fc;
static struct drm_mode_config *mc = &dev.mode_config;

/* The video memory the dumb buffers' `vmap` gives, by object. */
static uint8_t vram[MAX_GEM][16];
static int next_gem, dumb_creates, intrude, unmapped, plane_updates;

/* The pages behind every buffer, as the `page` hook gives them: 4 MiB,
 * room for the largest buffer of the rows. */
#define BACKING_PAGES 1024u
static uint8_t backing[BACKING_PAGES * PAGE_SIZE];

/** A buffer's page `pgoff`: the test's backing, the same for every
 * buffer; 0 past it. */
static uintptr_t fake_page(struct drm_gem_object *obj, uint32_t pgoff)
{
    (void)obj;
    return pgoff < BACKING_PAGES ? (uintptr_t)(backing + pgoff * PAGE_SIZE) : 0;
}

/** Each buffer's address in the model's video memory; NULL while
 * `unmapped`, as for a buffer moved out of it. */
static volatile uint8_t *fake_vmap(struct drm_gem_object *obj)
{
    return unmapped ? NULL : vram[obj - gem];
}

static const struct drm_gem_object_funcs gem_funcs = {NULL, NULL, NULL, NULL, fake_page, fake_vmap};

/** The mode `w` by `h` at 60 Hz, preferred when `preferred` is set. */
static void add_mode(struct drm_connector *c, uint16_t w, uint16_t h, int preferred)
{
    struct drm_mode_modeinfo m;

    k_memset(&m, 0, sizeof(m));
    m.hdisplay = w;
    m.hsync_start = (uint16_t)(w + 16);
    m.hsync_end = (uint16_t)(w + 48);
    m.htotal = (uint16_t)(w + 160);
    m.vdisplay = h;
    m.vsync_start = (uint16_t)(h + 3);
    m.vsync_end = (uint16_t)(h + 9);
    m.vtotal = (uint16_t)(h + 30);
    m.clock = (uint32_t)m.htotal * m.vtotal * 60u / 1000u;
    m.type = DRM_MODE_TYPE_DRIVER | (preferred ? DRM_MODE_TYPE_PREFERRED : 0);
    drm_mode_set_name(&m);
    CHECK(drm_mode_probed_add(c, &m) == 0, "a mode is added");
}

/** The connector's three modes. */
static int get_modes(struct drm_connector *c)
{
    add_mode(c, 1280, 800, 1);
    add_mode(c, 1024, 768, 0);
    add_mode(c, 800, 600, 0);
    return 3;
}

/** Always connected. */
static int detect(struct drm_connector *c, int force)
{
    (void)c;
    (void)force;
    return DRM_CONNECTOR_CONNECTED;
}

/** A dumb buffer: a model object with the test's `vmap`, a row of `width`
 * pixels of 4 bytes. With `intrude` set, a master comes meanwhile, as one
 * may while the console's commit sleeps. */
static int32_t fake_dumb_create(struct drm_file *file, struct drm_device *d,
                                struct drm_mode_create_dumb *args)
{
    int i = next_gem++;

    (void)d;
    dumb_creates++;
    args->pitch = args->width * 4u;
    args->size = (uint64_t)args->pitch * args->height;
    gem[i].funcs = &gem_funcs;
    gem[i].dev = &dev;
    args->handle = gem_new(file, i, args->size);
    if (intrude) {
        intrude = 0;
        file_open(&fc, &dev);
        drm_master_open(&fc);
    }
    return 0;
}

/** A check that takes every state. */
static int32_t fake_plane_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
    (void)plane;
    (void)state;
    return 0;
}

/** Count the plane's updates, which a flush is. */
static void fake_plane_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
    (void)plane;
    (void)state;
    plane_updates++;
}

static const struct drm_plane_funcs plane_funcs = {fake_plane_check, fake_plane_update, NULL};
static const struct drm_crtc_funcs crtc_funcs = {NULL, NULL, NULL, NULL, NULL, NULL,
                                                 NULL, NULL, NULL, NULL, NULL};
static const struct drm_connector_funcs conn_funcs = {detect, get_modes, NULL, NULL};
static const uint32_t xrgb[] = {DRM_FORMAT_XRGB8888};

/** Make the fake device. */
static void device_init(void)
{
    static struct drm_driver driver;

    driver.ident.name = "fake";
    driver.dumb_create = fake_dumb_create;
    dev.driver = &driver;
    dev.features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC;
    CHECK(drm_mode_config_init(&dev) == 0, "the mode configuration is made");
    mc->min_width = mc->min_height = 1;
    mc->max_width = mc->max_height = 4096;
    CHECK(drm_universal_plane_init(&dev, &primary, 1, &plane_funcs, xrgb, 1, NULL,
                                   DRM_PLANE_TYPE_PRIMARY) == 0 &&
              drm_crtc_init_with_planes(&dev, &crtc, &primary, NULL, &crtc_funcs) == 0 &&
              drm_encoder_init(&dev, &encoder, DRM_MODE_ENCODER_VIRTUAL) == 0 &&
              drm_connector_init(&dev, &connector, &conn_funcs, DRM_MODE_CONNECTOR_VIRTUAL) == 0,
          "the display is made");
    encoder.possible_crtcs = 1;
    drm_connector_attach_encoder(&connector, &encoder);
}

/** Open `file` as the process the test is, which a later DROP_MASTER
 * needs. */
static void open_as_self(struct drm_file *file)
{
    file_open(file, &dev);
    file->pid = kms_self.tgid;
    drm_master_open(file);
}

/** The mode on the CRTC now, or NULL when it is off. */
static const struct drm_mode_modeinfo *crtc_mode(void)
{
    return crtc.state && crtc.state->enable ? &crtc.state->mode : NULL;
}

/* ---- the rows ---- */

/** A device registers: the console takes it and sets nothing on it. */
static void row_register(void)
{
    drm_client_dev_register(&dev);
    CHECK(con.dev == &dev && dev.refs == 1, "the console holds the device");
    CHECK(listeners == &con_notifier, "the console listens for the master role");
    CHECK(attaches == 0 && dumb_creates == 0 && !crtc_mode(),
          "the console stays on the loader's screen");
}

/** A master that sets its own mode and exits without dropping the role. */
static void row_master_exits(void)
{
    struct drm_mode_fb_cmd2 r;
    struct drm_mode_set set;
    struct drm_connector *list[1];
    const struct drm_mode_modeinfo *m;

    open_as_self(&fa);
    CHECK(dev.master == &fa, "the first open is the master");
    CHECK(detaches == 1 && holds == 1 && kd_holds == 1 && con.handed,
          "the screen, /fb and the keyboard go to the master");
    k_memset(&r, 0, sizeof(r));
    r.width = 800;
    r.height = 600;
    r.pixel_format = DRM_FORMAT_XRGB8888;
    r.pitches[0] = 3200;
    r.handles[0] = gem_new(&fa, next_gem++, 3200u * 600u);
    CHECK(drm_mode_addfb2_ioctl(&dev, &r, &fa) == 0, "the master makes its framebuffer");
    drm_helper_probe_single_connector_modes(&connector, 4096, 4096);
    m = &con_mode_of_size(&connector, 800, 600)->info;
    list[0] = &connector;
    k_memset(&set, 0, sizeof(set));
    set.crtc = &crtc;
    set.fb = drm_framebuffer_lookup(&dev, &fa, r.fb_id);
    set.mode = m;
    set.connectors = list;
    set.num_connectors = 1;
    drm_modeset_lock_nointr(&dev);
    CHECK(drm_atomic_helper_set_config(&set) == 0, "the master sets 800x600");
    drm_modeset_unlock(&dev);
    drm_framebuffer_put(set.fb);
    CHECK(crtc_mode() && crtc_mode()->hdisplay == 800, "the master's mode is on the CRTC");

    /* The exit: drm.c's release of the file. */
    drm_mode_file_release(&fa);
    drm_master_release(&fa);
    CHECK(!dev.master && !con.handed, "the role is free and the console has the screen");
    CHECK(dumb_creates == 1 && con.fb && con.fb->width == 1024 && con.fb->height == 768,
          "the console makes a buffer of the loader's size, not the preferred");
    CHECK(crtc_mode() && crtc_mode()->hdisplay == 1024 && crtc_mode()->vdisplay == 768 &&
              primary.state->fb == con.fb,
          "the console's buffer is on the CRTC at 1024x768");
    CHECK(attaches == 1 && attached_base == vram[con.obj - gem] && attached.width == 1024 &&
              attached.height == 768 && attached.pitch == 4096 && attached.bpp == 32 &&
              attached.type == BOOT_FB_RGB && attached.red_shift == 16 &&
              attached.green_shift == 8 && attached.blue_shift == 0,
          "fbcon draws on the buffer, XRGB8888");
    CHECK(unholds == 1 && unhold_locked, "/fb comes back under the console's lock");
    CHECK(kd_unholds == 1 && kd_unhold_after_attach, "the keyboard comes back last");
    CHECK(con.on_buffer, "the console is on its buffer");
}

/** DROP_MASTER gives the screen back on the same buffer. */
static void row_drop_master(void)
{
    struct drm_framebuffer *before = con.fb;

    open_as_self(&fb_);
    CHECK(dev.master == &fb_ && detaches == 2 && kd_holds == 2, "a second master takes over");
    CHECK(drm_dropmaster_ioctl(&dev, NULL, &fb_) == 0, "DROP_MASTER");
    CHECK(con.fb == before && dumb_creates == 1, "the same buffer, no new one");
    CHECK(attaches == 2 && unholds == 2 && kd_unholds == 2, "the screen and keyboard come back");
    drm_mode_file_release(&fb_);
    drm_master_release(&fb_);
    CHECK(kd_unholds == 2, "the release of a file that holds no role changes nothing");
}

/** A master that comes while the console's commit runs keeps the screen,
 * and its release gives it back. */
static void row_intruder(void)
{
    video_option = "Virtual-1:800x600@60";
    open_as_self(&fa);
    intrude = 1;
    drm_mode_file_release(&fa);
    drm_master_release(&fa);
    CHECK(dev.master == &fc && con.handed, "the newcomer holds the screen");
    CHECK(attaches == 2 && kd_unholds == 2 && kd_holds == 3,
          "the console neither draws nor gives the keyboard back meanwhile");
    drm_mode_file_release(&fc);
    drm_master_release(&fc);
    CHECK(!con.handed && attaches == 3 && kd_unholds == 3, "the newcomer's exit gives it back");
    CHECK(crtc_mode() && crtc_mode()->hdisplay == 800 && con.fb->width == 800,
          "video= names the console's mode");
}

/** A hotplug with no master sets the console's mode again. */
static void row_hotplug(void)
{
    video_option = "1280x800";
    drm_client_dev_hotplug(&dev);
    CHECK(crtc_mode() && crtc_mode()->hdisplay == 1280 && con.fb->width == 1280 && attaches == 4 &&
              moves == 1,
          "the console follows the new mode");
    video_option = NULL;
}

/** A buffer that cannot be mapped when the master leaves is forgotten,
 * and the next place that succeeds draws again. */
static void row_unmapped(void)
{
    int d = detaches, f = forgets, a = attaches, u = unholds, k = kd_unholds, m = moves;

    open_as_self(&fa);
    CHECK(con.handed && detaches == d + 1 && forgets == f, "a master takes the screen");
    unmapped = 1;
    drm_mode_file_release(&fa);
    drm_master_release(&fa);
    CHECK(!con.handed && con.lost && !con.on_buffer && detaches == d + 2 && forgets == f + 1 &&
              attaches == a && unholds == u + 1 && kd_unholds == k + 1,
          "a buffer out of reach is forgotten, and /fb and the keyboard come back");
    unmapped = 0;
    drm_client_dev_hotplug(&dev);
    CHECK(!con.lost && con.on_buffer && attaches == a + 1 && attached_base != NULL &&
              moves == m + 1,
          "the next place draws again, and /fb follows");
}

/** A buffer the kernel cannot map, on a device whose framebuffers are
 * flushed, is drawn through a shadow that a flush copies and commits. */
static void row_shadow(void)
{
    uint32_t pitch;
    int u;

    mc->fb_dirty = 1;
    unmapped = 1;
    open_as_self(&fa);
    drm_mode_file_release(&fa);
    drm_master_release(&fa);
    CHECK(con.on_buffer && con.shadow && attached_base == con.shadow && !con.next && !con.spare,
          "the console draws on a shadow");
    pitch = con.fb->pitches[0];
    CHECK(con.shadow_bytes == pitch * con.fb->height && con_flush_ready(NULL),
          "the shadow is the buffer's size and wants its flushes");
    k_memset(con.shadow + 2 * pitch, 0x5A, 2 * pitch);
    damage_top = 2;
    damage_end = 4;
    u = plane_updates;
    con_flush();
    CHECK(backing[2 * pitch] == 0x5A && backing[4 * pitch - 1] == 0x5A &&
              backing[2 * pitch - 1] == 0 && backing[4 * pitch] == 0 && plane_updates == u + 1,
          "a flush copies the damaged rows to the buffer's pages and commits them");
    open_as_self(&fb_);
    damage_top = 0;
    damage_end = 1;
    con_flush();
    CHECK(plane_updates == u + 1 && damage_end == 1 && !con_flush_ready(NULL),
          "nothing is flushed while a master holds the device");
    drm_mode_file_release(&fb_);
    drm_master_release(&fb_);
    CHECK(!con.handed && attached_base == con.shadow, "the shadow is kept for the next return");
    damage_top = damage_end = 0;
    k_memset(backing, 0, sizeof(backing));
    unmapped = 0;
    mc->fb_dirty = 0;
}

/** The firmware's device: the CRTC off, the console on the loader's
 * screen. */
static void row_firmware(void)
{
    int d = detaches, f = forgets, a = attaches, m = moves, c = dumb_creates;

    drm_client_dev_unregister(&dev);
    CHECK(!con.dev && !con.fb && dev.refs == 0, "the console lets the device go");
    CHECK(detaches == d + 1 && forgets == f + 1 && moves == m + 1,
          "a console on the device's buffer forgets it, and /fb follows");
    drm_client_dev_register(&dev);
    CHECK(attaches == a + 1 && dumb_creates == c + 1 && crtc_mode() &&
              crtc_mode()->hdisplay == 1024 && moves == m + 2,
          "the lost console is set again on the next device");
    drm_client_dev_unregister(&dev);
    firmware = 1;
    drm_client_dev_register(&dev);
    CHECK(attaches == a + 2 && attached_base == NULL && dumb_creates == c + 1,
          "the lost console goes to the loader's framebuffer on the firmware's device");
    open_as_self(&fa);
    drm_mode_file_release(&fa);
    drm_master_release(&fa);
    CHECK(!crtc_mode() && dumb_creates == c + 1,
          "the firmware's CRTC is turned off, no buffer made");
    CHECK(attached_base == NULL && attached.width == 1024 && attached.addr == loader.addr,
          "fbcon draws on the loader's framebuffer");
    drm_client_dev_unregister(&dev);
    CHECK(dev.refs == 0, "the device is let go");
}

/** Run the rows. */
int main(void)
{
    int base, i;

    model_start("drmcon-test");
    kms_self.tgid = 7;
    loader.addr = 0xFD000000u;
    loader.pitch = 4096;
    loader.width = 1024;
    loader.height = 768;
    loader.bpp = 32;
    loader.type = BOOT_FB_RGB;
    loader.red_shift = 16;
    loader.red_bits = 8;
    loader.green_shift = 8;
    loader.green_bits = 8;
    loader.blue_bits = 8;
    base = allocations;
    device_init();
    row_register();
    row_master_exits();
    row_drop_master();
    row_intruder();
    row_hotplug();
    row_unmapped();
    row_shadow();
    row_firmware();
    drm_mode_config_cleanup(&dev);
    CHECK(allocations == base, "every allocation freed");
    for (i = 0; i < MAX_GEM; i++) {
        uint32_t held = 0;
        int j;

        for (j = 0; j < MAX_HANDLES; j++) {
            held += handles[j].file && handles[j].obj == &gem[i];
        }
        CHECK(gem[i].refs == held, "a buffer object is held by its handles alone");
    }
    if (failures) {
        return 1;
    }
    printf("drmcon-test: ok\n");
    return 0;
}
