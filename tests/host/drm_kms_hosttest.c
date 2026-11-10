/*
 * drm_kms_hosttest.c: kernel mode setting, rach2/sys/drm/drm_mode.c,
 * drm_atomic.c and drm_legacy.c, through their ioctl handlers against a
 * fake driver; drm_kms64_hosttest.c runs the same rows with x86-64's
 * structures.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The fake driver has two CRTCs: the first with a primary plane of three
 * formats and two modifiers, a cursor plane and a gamma table of 256
 * entries as GAMMA_LUT, the second with a primary plane alone; an overlay
 * plane for either; and an HDMI connector with the Lenovo panel's EDID,
 * which the first CRTC alone can drive, and a VGA connector, unplugged,
 * for the second. Its hooks count what a commit does, and its checks are
 * Linux's helpers: a plane clipped to its CRTC, a primary plane covering
 * it, and a CRTC that is on with its primary plane. The rows:
 * GETRESOURCES, the planes each client cap shows, GETPLANE, the properties
 * and their values, GETPROPERTY's enums, GETCONNECTOR's probe and
 * GETENCODER (76-5a.1); blobs made, read and destroyed by their owner
 * alone; framebuffers from ADDFB and ADDFB2 with modifiers, IN_FORMATS,
 * GETFB and GETFB2 by privilege, RMFB, CLOSEFB and the lowest free ID
 * (76-5a.4); atomic commits refused without ALLOW_MODESET, tested,
 * committed with an event, refused for bad flags, objects, properties and
 * coordinates, waited on an in-fence, and NONBLOCK (76-5a.2); SETCRTC,
 * SETPLANE, the cursor, PAGE_FLIP, DIRTYFB, DPMS through OBJ_SETPROPERTY
 * and SETPROPERTY, and SETGAMMA, each as a commit (76-5a.3); and a file's
 * release, which takes its framebuffer off the screen and frees its blobs.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "drm_kms_model.c"

/* The Lenovo's eDP panel, as Linux's modetest read it
 * (tests/fixtures/lenovo-gpu/modetest-connectors.out): one mode,
 * 1366x768. */
static const uint8_t panel_edid[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x06, 0xaf, 0xec, 0x40, 0x00, 0x00, 0x00, 0x00,
    0x29, 0x17, 0x01, 0x04, 0x95, 0x22, 0x13, 0x78, 0x02, 0xbb, 0xf5, 0x94, 0x55, 0x54, 0x90, 0x27,
    0x23, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xce, 0x1d, 0x56, 0xc0, 0x50, 0x00, 0x30, 0x30, 0x08, 0x0a,
    0x31, 0x00, 0x58, 0xc1, 0x10, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0xfe, 0x00, 0x41,
    0x55, 0x4f, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfe,
    0x00, 0x42, 0x31, 0x35, 0x36, 0x58, 0x54, 0x4e, 0x30, 0x34, 0x2e, 0x30, 0x20, 0x0a, 0x00, 0xee,
};

/* Intel's X-tiled layout, a modifier of a vendor's, for the rows of
 * modifiers. */
#define MOD_X_TILED (((uint64_t)1 << 56) | 1)

static struct drm_device dev;
static struct drm_plane primary0, cursor0, primary1, overlay;
static struct drm_crtc crtc0, crtc1;
static struct drm_encoder tmds, dac;
static struct drm_connector hdmi, vga;
static struct drm_file fa, fb_, fc;
static struct drm_mode_config *mc = &dev.mode_config;

/* What the hooks saw. */
static int enables, disables, updates, plane_offs, flushes;

/** A CRTC turned on. */
static void crtc_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    (void)crtc;
    (void)state;
    enables++;
}

/** A CRTC turned off. */
static void crtc_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    (void)crtc;
    (void)state;
    disables++;
}

/** A CRTC's planes written. */
static void crtc_flush(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    (void)crtc;
    (void)state;
    flushes++;
}

/** A plane shown. */
static void plane_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
    (void)plane;
    (void)state;
    updates++;
}

/** A plane hidden. */
static void plane_disable(struct drm_plane *plane, struct drm_atomic_state *state)
{
    (void)plane;
    (void)state;
    plane_offs++;
}

/** A plane's check, as a driver's: clipped to its CRTC unscaled, a primary
 * plane covering it, Linux's drm_atomic_helper_check_plane_state. */
static int32_t fake_plane_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
    struct drm_plane_state *ps = state->planes_new[plane->index];
    const struct drm_crtc_state *cs;

    if (!ps->crtc) {
        return 0;
    }
    cs = state->crtcs_new[ps->crtc->index] ? state->crtcs_new[ps->crtc->index] : ps->crtc->state;
    return drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
                                               plane->type != DRM_PLANE_TYPE_PRIMARY, 1);
}

/** A CRTC's check, Linux's drm_atomic_helper_check_crtc_primary_plane: a
 * CRTC that is on needs its primary plane. */
static int32_t fake_crtc_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    const struct drm_crtc_state *cs = state->crtcs_new[crtc->index];

    return cs->enable && !(cs->plane_mask & drm_plane_mask(crtc->primary)) ? -EINVAL : 0;
}

/** HDMI is plugged in, VGA is not. */
static int detect(struct drm_connector *connector, int force_probe)
{
    (void)force_probe;
    return connector == &hdmi ? DRM_CONNECTOR_CONNECTED : DRM_CONNECTOR_DISCONNECTED;
}

/** The panel's EDID on HDMI. */
static int get_modes(struct drm_connector *connector)
{
    return drm_connector_update_edid(connector, panel_edid, sizeof(panel_edid));
}

static const struct drm_plane_funcs plane_funcs = {fake_plane_check, plane_update, plane_disable};
static const struct drm_crtc_funcs crtc_funcs = {fake_crtc_check,
                                                 crtc_enable,
                                                 crtc_disable,
                                                 NULL,
                                                 crtc_flush,
                                                 NULL,
                                                 NULL,
                                                 NULL,
                                                 NULL,
                                                 NULL,
                                                 NULL,
                                                 NULL};
static const struct drm_connector_funcs conn_funcs = {detect, get_modes, NULL, NULL};
static const uint32_t formats0[] = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_RGB565};
static const uint64_t modifiers0[] = {DRM_FORMAT_MOD_LINEAR, MOD_X_TILED, DRM_FORMAT_MOD_INVALID};
static const uint32_t argb[] = {DRM_FORMAT_ARGB8888};
static const uint32_t xrgb[] = {DRM_FORMAT_XRGB8888};

/** Make the fake device, its objects in the order a driver makes them,
 * and open three files on it, the first its master. */
static void device_init(void)
{
    static struct drm_driver driver;

    driver.ident.name = "fake";
    dev.driver = &driver;
    dev.features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC;
    CHECK(drm_mode_config_init(&dev) == 0, "the mode configuration is made");
    mc->min_width = mc->min_height = 1;
    mc->max_width = mc->max_height = 4096;
    CHECK(drm_universal_plane_init(&dev, &primary0, 1, &plane_funcs, formats0, 3, modifiers0,
                                   DRM_PLANE_TYPE_PRIMARY) == 0 &&
              drm_universal_plane_init(&dev, &cursor0, 1, &plane_funcs, argb, 1, NULL,
                                       DRM_PLANE_TYPE_CURSOR) == 0 &&
              drm_universal_plane_init(&dev, &primary1, 2, &plane_funcs, xrgb, 1, NULL,
                                       DRM_PLANE_TYPE_PRIMARY) == 0 &&
              drm_universal_plane_init(&dev, &overlay, 3, &plane_funcs, xrgb, 1, NULL,
                                       DRM_PLANE_TYPE_OVERLAY) == 0,
          "the planes are made");
    drm_plane_enable_fb_damage_clips(&primary0);
    CHECK(drm_crtc_init_with_planes(&dev, &crtc0, &primary0, &cursor0, &crtc_funcs) == 0 &&
              drm_crtc_init_with_planes(&dev, &crtc1, &primary1, NULL, &crtc_funcs) == 0,
          "the CRTCs are made");
    CHECK(drm_mode_crtc_set_gamma_size(&crtc0, 256) == 0, "the gamma table is made");
    drm_crtc_enable_color_mgmt(&crtc0, 0, 0, 256);
    CHECK(drm_encoder_init(&dev, &tmds, DRM_MODE_ENCODER_TMDS) == 0 &&
              drm_encoder_init(&dev, &dac, DRM_MODE_ENCODER_DAC) == 0,
          "the encoders are made");
    tmds.possible_crtcs = 1;
    dac.possible_crtcs = 2;
    CHECK(drm_connector_init(&dev, &hdmi, &conn_funcs, DRM_MODE_CONNECTOR_HDMIA) == 0 &&
              drm_connector_init(&dev, &vga, &conn_funcs, DRM_MODE_CONNECTOR_VGA) == 0,
          "the connectors are made");
    drm_connector_attach_encoder(&hdmi, &tmds);
    drm_connector_attach_encoder(&vga, &dac);
    file_open(&fa, &dev);
    file_open(&fb_, &dev);
    file_open(&fc, &dev);
    dev.master = &fa;
    fa.is_master = 1;
}

/* ---- 76-5a.1: mode objects and properties ---- */

/** GETRESOURCES: the counts first, then the IDs, and the size limits. */
static void row_resources(void)
{
    struct drm_mode_card_res r;
    uint32_t crtcs[2], conns[2], encs[2];

    k_memset(&r, 0, sizeof(r));
    CHECK(drm_mode_getresources(&dev, &r, &fa) == 0, "GETRESOURCES counts");
    CHECK(r.count_crtcs == 2 && r.count_connectors == 2 && r.count_encoders == 2 &&
              r.count_fbs == 0,
          "two CRTCs, connectors and encoders, no framebuffer");
    CHECK(r.min_width == 1 && r.max_width == 4096 && r.max_height == 4096, "the size limits");
    r.crtc_id_ptr = UPTR(crtcs);
    r.connector_id_ptr = UPTR(conns);
    r.encoder_id_ptr = UPTR(encs);
    CHECK(drm_mode_getresources(&dev, &r, &fa) == 0, "GETRESOURCES IDs");
    CHECK(crtcs[0] == crtc0.base.id && crtcs[1] == crtc1.base.id && conns[0] == hdmi.base.id &&
              conns[1] == vga.base.id && encs[0] == tmds.base.id && encs[1] == dac.base.id,
          "the IDs in the order the driver made them");
    CHECK(crtc0.base.id < crtc1.base.id && primary0.base.id < cursor0.base.id,
          "IDs rise as objects are made");
    CHECK(!strcmp(hdmi.name, "HDMI-A-1") && !strcmp(vga.name, "VGA-1") &&
              !strcmp(crtc1.name, "crtc-1"),
          "Linux's names");
}

/** GETPLANERESOURCES: the overlay alone without the universal planes cap,
 * all four with it; GETPLANE of the first primary. */
static void row_planes(void)
{
    struct drm_mode_get_plane_res r;
    struct drm_mode_get_plane p;
    uint32_t ids[4], fmts[3];

    k_memset(&r, 0, sizeof(r));
    CHECK(drm_mode_getplane_res(&dev, &r, &fb_) == 0 && r.count_planes == 1,
          "without the cap, the overlay alone");
    fb_.universal_planes = 1;
    r.count_planes = 0;
    CHECK(drm_mode_getplane_res(&dev, &r, &fb_) == 0 && r.count_planes == 4,
          "with the cap, every plane");
    r.plane_id_ptr = UPTR(ids);
    CHECK(drm_mode_getplane_res(&dev, &r, &fb_) == 0 && ids[0] == primary0.base.id &&
              ids[3] == overlay.base.id,
          "the planes' IDs");
    k_memset(&p, 0, sizeof(p));
    p.plane_id = primary0.base.id;
    CHECK(drm_mode_getplane(&dev, &p, &fb_) == 0 && p.count_format_types == 3 &&
              p.possible_crtcs == 1 && p.crtc_id == 0 && p.fb_id == 0,
          "GETPLANE counts the formats");
    p.format_type_ptr = UPTR(fmts);
    CHECK(drm_mode_getplane(&dev, &p, &fb_) == 0 && fmts[0] == DRM_FORMAT_XRGB8888 &&
              fmts[2] == DRM_FORMAT_RGB565,
          "GETPLANE lists the formats");
    p.plane_id = crtc0.base.id;
    CHECK(drm_mode_getplane(&dev, &p, &fb_) == -ENOENT, "GETPLANE of a CRTC: ENOENT");
}

/** Whether `obj`'s properties for `file` include `prop`, with its value in
 * `*value`; the count in `*count`. */
static int has_prop(struct drm_file *file, struct drm_mode_object *obj, struct drm_property *prop,
                    uint64_t *value, uint32_t *count)
{
    struct drm_mode_obj_get_properties r;
    uint32_t ids[64], i;
    uint64_t values[64];

    k_memset(&r, 0, sizeof(r));
    r.obj_id = obj->id;
    r.obj_type = obj->type;
    r.count_props = 64;
    r.props_ptr = UPTR(ids);
    r.prop_values_ptr = UPTR(values);
    if (drm_mode_obj_get_properties_ioctl(&dev, &r, file) != 0) {
        return 0;
    }
    *count = r.count_props;
    for (i = 0; i < r.count_props; i++) {
        if (ids[i] == prop->base.id) {
            *value = values[i];
            return 1;
        }
    }
    return 0;
}

/** The properties a plane shows each client, their values, and the
 * names of an enum. */
static void row_properties(void)
{
    struct drm_mode_obj_get_properties r;
    struct drm_mode_get_property gp;
    struct drm_mode_property_enum enums[3];
    uint64_t value = 0, values[3];
    uint32_t legacy = 0, atomic = 0;

    CHECK(has_prop(&fb_, &primary0.base, mc->plane_type_property, &value, &legacy) &&
              value == DRM_PLANE_TYPE_PRIMARY,
          "the plane's type, Primary");
    CHECK(!has_prop(&fb_, &primary0.base, mc->prop_fb_id, &value, &legacy),
          "FB_ID hidden without the atomic cap");
    fb_.atomic = 1;
    CHECK(has_prop(&fb_, &primary0.base, mc->prop_fb_id, &value, &atomic) && value == 0,
          "FB_ID shown with the atomic cap, 0");
    CHECK(atomic > legacy, "the atomic cap shows more properties");
    CHECK(has_prop(&fb_, &crtc0.base, mc->gamma_lut_size_property, &value, &atomic) && value == 256,
          "GAMMA_LUT_SIZE is 256");
    k_memset(&r, 0, sizeof(r));
    r.obj_id = crtc0.base.id;
    r.obj_type = DRM_MODE_OBJECT_PLANE;
    CHECK(drm_mode_obj_get_properties_ioctl(&dev, &r, &fb_) == -ENOENT,
          "a CRTC's ID as a plane: ENOENT");
    k_memset(&gp, 0, sizeof(gp));
    gp.prop_id = mc->plane_type_property->base.id;
    CHECK(drm_mode_getproperty_ioctl(&dev, &gp, &fb_) == 0 && gp.count_enum_blobs == 3 &&
              gp.flags == (DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE) &&
              !strcmp(gp.name, "type"),
          "GETPROPERTY counts an enum's names");
    gp.enum_blob_ptr = UPTR(enums);
    gp.values_ptr = UPTR(values);
    CHECK(drm_mode_getproperty_ioctl(&dev, &gp, &fb_) == 0 && !strcmp(enums[0].name, "Overlay") &&
              !strcmp(enums[1].name, "Primary") && enums[2].value == DRM_PLANE_TYPE_CURSOR,
          "GETPROPERTY gives the names");
    gp.prop_id = crtc0.base.id;
    CHECK(drm_mode_getproperty_ioctl(&dev, &gp, &fb_) == -ENOENT, "GETPROPERTY of a CRTC: ENOENT");
}

/** GETCONNECTOR by the master probes; GETENCODER. */
static void row_connectors(void)
{
    struct drm_mode_get_connector c;
    struct drm_mode_get_encoder e;
    struct drm_mode_modeinfo modes[2];
    uint32_t encs[1];

    k_memset(&c, 0, sizeof(c));
    c.connector_id = hdmi.base.id;
    CHECK(drm_mode_getconnector(&dev, &c, &fa) == 0, "GETCONNECTOR");
    CHECK(c.count_modes == 1 && c.connection == DRM_CONNECTOR_CONNECTED && c.mm_width == 340 &&
              c.mm_height == 190 && c.count_encoders == 1 && c.encoder_id == 0 &&
              c.connector_type == DRM_MODE_CONNECTOR_HDMIA && c.connector_type_id == 1,
          "the master's GETCONNECTOR probes the panel");
    c.modes_ptr = UPTR(modes);
    c.encoders_ptr = UPTR(encs);
    c.count_props = 0;
    CHECK(drm_mode_getconnector(&dev, &c, &fa) == 0 && modes[0].hdisplay == 1366 &&
              (modes[0].type & DRM_MODE_TYPE_PREFERRED) && encs[0] == tmds.base.id,
          "the panel's mode, preferred, and its encoder");
    CHECK(hdmi.edid && hdmi.edid->length == 128, "the EDID blob");
    k_memset(&c, 0, sizeof(c));
    c.connector_id = vga.base.id;
    CHECK(drm_mode_getconnector(&dev, &c, &fa) == 0 && c.count_modes == 0 &&
              c.connection == DRM_CONNECTOR_DISCONNECTED,
          "VGA is disconnected, with no modes");
    k_memset(&e, 0, sizeof(e));
    e.encoder_id = dac.base.id;
    CHECK(drm_mode_getencoder(&dev, &e, &fa) == 0 && e.encoder_type == DRM_MODE_ENCODER_DAC &&
              e.possible_crtcs == 2 && e.crtc_id == 0,
          "GETENCODER");
}

/* ---- blobs ---- */

/** Make a blob of `len` bytes from `data` as `file`; its ID, or 0. */
static uint32_t blob_new(struct drm_file *file, const void *data, uint32_t len)
{
    struct drm_mode_create_blob c;

    k_memset(&c, 0, sizeof(c));
    c.data = UPTR(data);
    c.length = len;
    return drm_mode_createblob_ioctl(&dev, &c, file) == 0 ? c.blob_id : 0;
}

/** Destroy blob `id` as `file`. */
static int32_t blob_destroy(struct drm_file *file, uint32_t id)
{
    struct drm_mode_destroy_blob d;

    d.blob_id = id;
    return drm_mode_destroyblob_ioctl(&dev, &d, file);
}

/** CREATEPROPBLOB, GETPROPBLOB and DESTROYPROPBLOB by its owner alone. */
static void row_blobs(void)
{
    static const char bytes[16] = "fifteen bytes..";
    struct drm_mode_create_blob c;
    struct drm_mode_get_blob g;
    char out[16];
    uint32_t id;

    k_memset(&c, 0, sizeof(c));
    c.data = UPTR(bytes);
    CHECK(drm_mode_createblob_ioctl(&dev, &c, &fb_) == -EINVAL, "an empty blob: EINVAL");
    id = blob_new(&fb_, bytes, sizeof(bytes));
    CHECK(id != 0, "CREATEPROPBLOB");
    k_memset(&g, 0, sizeof(g));
    g.blob_id = id;
    CHECK(drm_mode_getblob_ioctl(&dev, &g, &fc) == 0 && g.length == 16, "GETPROPBLOB's length");
    g.data = UPTR(out);
    CHECK(drm_mode_getblob_ioctl(&dev, &g, &fc) == 0 && !strcmp(out, bytes),
          "GETPROPBLOB's bytes, to any file");
    CHECK(blob_destroy(&fc, id) == -EPERM, "DESTROYPROPBLOB by another file: EPERM");
    CHECK(blob_destroy(&fb_, id + 100) == -EINVAL, "DESTROYPROPBLOB of no blob: EINVAL");
    CHECK(blob_destroy(&fb_, id) == 0, "DESTROYPROPBLOB by its owner");
    CHECK(drm_mode_getblob_ioctl(&dev, &g, &fc) == -ENOENT, "a destroyed blob: ENOENT");
}

/* ---- 76-5a.4: framebuffers ---- */

static int32_t fb_rc; /* the last addfb2's answer */

/** ADDFB2 as the master of `w` by `h` in `format` with `modifier` (0: none
 * given) over `handle`, `pitch` bytes a row; the ID, with the answer in
 * `fb_rc`. */
static uint32_t addfb2(uint32_t handle, uint32_t w, uint32_t h, uint32_t format, uint32_t pitch,
                       uint64_t modifier)
{
    struct drm_mode_fb_cmd2 r;

    k_memset(&r, 0, sizeof(r));
    r.width = w;
    r.height = h;
    r.pixel_format = format;
    r.handles[0] = handle;
    r.pitches[0] = pitch;
    if (modifier) {
        r.flags = DRM_MODE_FB_MODIFIERS;
        r.modifier[0] = modifier;
    }
    fb_rc = drm_mode_addfb2_ioctl(&dev, &r, &fa);
    return r.fb_id;
}

/** Remove framebuffer `id` as `file`. */
static int32_t rmfb(struct drm_file *file, uint32_t id)
{
    return drm_mode_rmfb_ioctl(&dev, &id, file);
}

static uint32_t fb_main, fb_other, fb_565, h_main, h_other, h_cursor;

/** The IN_FORMATS blob: three formats, and each modifier with all three. */
static void row_in_formats(void)
{
    const struct drm_format_modifier_blob *b;
    const struct drm_format_modifier *m;
    const uint32_t *f;
    struct drm_property_blob *blob;
    uint64_t id = 0;

    CHECK(drm_object_property_get_value(&primary0.base, mc->modifiers_property, &id) == 0 && id,
          "the primary plane has IN_FORMATS");
    blob = drm_property_lookup_blob(&dev, (uint32_t)id);
    CHECK(blob != NULL, "IN_FORMATS names a blob");
    if (!blob) {
        return;
    }
    b = (const struct drm_format_modifier_blob *)blob->data;
    f = (const uint32_t *)((const char *)b + b->formats_offset);
    m = (const struct drm_format_modifier *)((const char *)b + b->modifiers_offset);
    CHECK(b->version == FORMAT_BLOB_CURRENT && b->count_formats == 3 && b->count_modifiers == 2 &&
              b->formats_offset == sizeof(*b) && b->modifiers_offset % 8 == 0,
          "IN_FORMATS's header");
    CHECK(f[0] == DRM_FORMAT_XRGB8888 && f[2] == DRM_FORMAT_RGB565, "IN_FORMATS's formats");
    CHECK(m[0].modifier == DRM_FORMAT_MOD_LINEAR && m[0].formats == 7 && m[0].offset == 0 &&
              m[1].modifier == MOD_X_TILED && m[1].formats == 7,
          "IN_FORMATS's modifiers");
    drm_property_blob_put(blob);
}

/** ADDFB2 and ADDFB, their refusals, GETFB and GETFB2, RMFB, CLOSEFB and
 * the lowest free ID. */
static void row_framebuffers(void)
{
    struct drm_mode_fb_cmd old;
    struct drm_mode_fb_cmd2 r2;
    struct drm_mode_closefb cl;
    uint32_t id, again;

    h_main = gem_new(&fa, 0, 1366u * 768u * 4u);
    h_other = gem_new(&fa, 1, 1366u * 768u * 4u);
    h_cursor = gem_new(&fa, 2, 64u * 64u * 4u);
    CHECK((fb_main = addfb2(h_main, 1366, 768, DRM_FORMAT_XRGB8888, 1366 * 4, 0), fb_rc) == 0,
          "ADDFB2");
    fb_other = addfb2(h_other, 1366, 768, DRM_FORMAT_XRGB8888, 1366 * 4, MOD_X_TILED);
    CHECK(fb_rc == 0, "ADDFB2 with a modifier a plane takes");
    id = addfb2(h_other, 1366, 768, DRM_FORMAT_XRGB8888, 1366 * 4, ((uint64_t)1 << 56) | 99);
    CHECK(fb_rc == -EINVAL, "ADDFB2 with a modifier no plane takes: EINVAL");
    CHECK((id = addfb2(h_other, 1366, 768, DRM_FORMAT_NV12, 1366, 0), fb_rc) == -EINVAL,
          "ADDFB2 in a format no plane takes: EINVAL");
    CHECK((id = addfb2(h_other, 1366, 768, DRM_FORMAT_XRGB8888, 100, 0), fb_rc) == -EINVAL,
          "ADDFB2 with a short pitch: EINVAL");
    CHECK((id = addfb2(99, 1366, 768, DRM_FORMAT_XRGB8888, 1366 * 4, 0), fb_rc) == -ENOENT,
          "ADDFB2 of no object: ENOENT");
    CHECK((id = addfb2(h_other, 8192, 8, DRM_FORMAT_XRGB8888, 8192 * 4, 0), fb_rc) == -EINVAL,
          "ADDFB2 past the width limit: EINVAL");
    CHECK((id = addfb2(h_cursor, 64, 64, DRM_FORMAT_XRGB8888, 64 * 4, 0), fb_rc) == 0 &&
              rmfb(&fa, id) == 0,
          "ADDFB2 and RMFB of a small one");
    CHECK((again = addfb2(h_cursor, 64, 64, DRM_FORMAT_XRGB8888, 64 * 4, 0), fb_rc) == 0 &&
              again == id,
          "the lowest free ID again");
    CHECK(rmfb(&fa, again) == 0, "RMFB");
    k_memset(&old, 0, sizeof(old));
    old.width = 1366;
    old.height = 768;
    old.pitch = 1366 * 2;
    old.bpp = 16;
    old.depth = 16;
    old.handle = h_other;
    CHECK(drm_mode_addfb_ioctl(&dev, &old, &fa) == 0, "ADDFB of 16 bits");
    fb_565 = old.fb_id;
    k_memset(&r2, 0, sizeof(r2));
    r2.fb_id = fb_565;
    CHECK(drm_mode_getfb2_ioctl(&dev, &r2, &fa) == 0 && r2.pixel_format == DRM_FORMAT_RGB565 &&
              r2.flags == DRM_MODE_FB_MODIFIERS && r2.modifier[0] == DRM_FORMAT_MOD_LINEAR &&
              r2.handles[0] != 0,
          "GETFB2 of ADDFB's: RGB565, linear, a handle for the master");
    drm_gem_handle_delete(&fa, r2.handles[0]);
    k_memset(&r2, 0, sizeof(r2));
    r2.fb_id = fb_other;
    kms_self.euid = 1000;
    CHECK(drm_mode_getfb2_ioctl(&dev, &r2, &fc) == 0 && r2.modifier[0] == MOD_X_TILED &&
              r2.handles[0] == 0 && r2.pitches[0] == 1366 * 4,
          "GETFB2 for another user: the modifier, no handle");
    k_memset(&old, 0, sizeof(old));
    old.fb_id = fb_main;
    CHECK(drm_mode_getfb(&dev, &old, &fc) == 0 && old.handle == 0 && old.bpp == 32 &&
              old.depth == 24 && old.pitch == 1366 * 4,
          "GETFB for another user: no handle");
    kms_self.euid = 0;
    CHECK(drm_mode_getfb(&dev, &old, &fc) == 0 && old.handle != 0, "GETFB for root: a handle");
    drm_gem_handle_delete(&fc, old.handle);
    CHECK(rmfb(&fc, fb_main) == -ENOENT, "RMFB by another file: ENOENT");
    k_memset(&cl, 0, sizeof(cl));
    cl.fb_id = fb_565;
    CHECK(drm_mode_closefb_ioctl(&dev, &cl, &fa) == 0, "CLOSEFB");
    CHECK(rmfb(&fa, fb_565) == -ENOENT, "a closed framebuffer is no longer the file's");
    row_in_formats();
}

/* ---- 76-5a.2: atomic commits ---- */

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

/** ATOMIC of `r` with `flags` as `file`. */
static int32_t req_commit(struct drm_file *file, const struct req *r, uint32_t flags,
                          uint64_t user_data)
{
    struct drm_mode_atomic a;

    k_memset(&a, 0, sizeof(a));
    a.flags = flags;
    a.count_objs = r->nobj;
    a.objs_ptr = UPTR(r->objs);
    a.count_props_ptr = UPTR(r->counts);
    a.props_ptr = UPTR(r->props);
    a.prop_values_ptr = UPTR(r->values);
    a.user_data = user_data;
    return drm_mode_atomic_ioctl(&dev, &a, file);
}

/** A plane's framebuffer, source and place: all of `fb_id` on all of the
 * panel. */
static void req_plane(struct req *r, struct drm_plane *plane, uint32_t fb_id, uint32_t crtc_id)
{
    req_add(r, &plane->base, mc->prop_fb_id, fb_id);
    req_add(r, &plane->base, mc->prop_crtc_id, crtc_id);
    req_add(r, &plane->base, mc->prop_src_x, 0);
    req_add(r, &plane->base, mc->prop_src_y, 0);
    req_add(r, &plane->base, mc->prop_src_w, fb_id ? 1366u << 16 : 0);
    req_add(r, &plane->base, mc->prop_src_h, fb_id ? 768u << 16 : 0);
    req_add(r, &plane->base, mc->prop_crtc_x, 0);
    req_add(r, &plane->base, mc->prop_crtc_y, 0);
    req_add(r, &plane->base, mc->prop_crtc_w, fb_id ? 1366 : 0);
    req_add(r, &plane->base, mc->prop_crtc_h, fb_id ? 768 : 0);
}

/** The request that lights the panel from `fb_id` with the mode blob
 * `mode_id`. */
static void req_on(struct req *r, uint32_t mode_id, uint32_t fb_id)
{
    k_memset(r, 0, sizeof(*r));
    req_add(r, &crtc0.base, mc->prop_mode_id, mode_id);
    req_add(r, &crtc0.base, mc->prop_active, 1);
    req_add(r, &hdmi.base, mc->prop_crtc_id, crtc0.base.id);
    req_plane(r, &primary0, fb_id, crtc0.base.id);
}

/** The request that turns the panel off. */
static void req_off(struct req *r)
{
    k_memset(r, 0, sizeof(*r));
    req_add(r, &crtc0.base, mc->prop_mode_id, 0);
    req_add(r, &crtc0.base, mc->prop_active, 0);
    req_add(r, &hdmi.base, mc->prop_crtc_id, 0);
    req_plane(r, &primary0, 0, 0);
}

/** A commit's refusals of flags, objects, properties and values. */
static void row_atomic_refusals(uint32_t mode_id)
{
    struct req r;
    int32_t out_fence = 7;

    req_on(&r, mode_id, fb_main);
    CHECK(req_commit(&fc, &r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == -EINVAL,
          "a file without the atomic cap: EINVAL");
    CHECK(req_commit(&fa, &r, 0x8000, 0) == -EINVAL, "an unknown flag: EINVAL");
    CHECK(req_commit(&fa, &r, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_PAGE_FLIP_EVENT, 0) == -EINVAL,
          "TEST_ONLY with an event: EINVAL");
    CHECK(req_commit(&fa, &r, DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_ATOMIC_ALLOW_MODESET, 0) ==
              -EINVAL,
          "ASYNC on a device without it: EINVAL");
    r.objs[0] = tmds.base.id;
    CHECK(req_commit(&fa, &r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == -ENOENT, "an encoder: ENOENT");
    req_on(&r, mode_id, fb_main);
    r.props[0] = mc->prop_fb_id->base.id;
    CHECK(req_commit(&fa, &r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == -ENOENT,
          "a property the object has not: ENOENT");
    req_on(&r, mode_id, fb_main);
    r.values[1] = 2;
    CHECK(req_commit(&fa, &r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == -EINVAL,
          "ACTIVE out of its range: EINVAL");
    req_on(&r, mode_id, fb_main);
    r.values[7] = 2000u << 16;
    CHECK(req_commit(&fa, &r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == -ENOSPC,
          "a source past the framebuffer: ENOSPC");
    req_on(&r, mode_id, fb_main);
    r.values[3] = fb_565;
    CHECK(req_commit(&fa, &r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == -EINVAL,
          "FB_ID of an ID no framebuffer has: EINVAL");
    req_on(&r, mode_id, fb_main);
    r.values[2] = crtc1.base.id;
    CHECK(req_commit(&fa, &r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == -EINVAL,
          "HDMI on a CRTC its encoder cannot take: EINVAL");
    k_memset(&r, 0, sizeof(r));
    req_add(&r, &overlay.base, mc->prop_fb_id, 0);
    CHECK(req_commit(&fa, &r, DRM_MODE_PAGE_FLIP_EVENT, 0) == -EINVAL,
          "an event with no CRTC: EINVAL");
    req_on(&r, mode_id, fb_main);
    req_add(&r, &primary0.base, mc->prop_in_fence_fd, 9);
    CHECK(req_commit(&fa, &r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == -EINVAL,
          "IN_FENCE_FD of no sync file: EINVAL");
    req_on(&r, mode_id, fb_main);
    req_add(&r, &crtc0.base, mc->prop_out_fence_ptr, UPTR(&out_fence));
    CHECK(req_commit(&fa, &r, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == 0 &&
              out_fence == -1,
          "OUT_FENCE_PTR is written -1");
    CHECK(enables == 0 && updates == 0 && !crtc0.state->active, "no refusal reached the hooks");
    CHECK(reserved == 0 && fence_refs == 0, "no refusal kept an event or a fence");
}

/** Flips on the lit panel: one waited on an in-fence, a NONBLOCK one with
 * its event, then ACTIVE off refused without ALLOW_MODESET, and off. */
static void row_atomic_flips(void)
{
    struct req r;

    k_memset(&r, 0, sizeof(r));
    req_add(&r, &primary0.base, mc->prop_fb_id, fb_other);
    req_add(&r, &primary0.base, mc->prop_in_fence_fd, 5);
    CHECK(req_commit(&fa, &r, 0, 0) == 0, "a flip with an in-fence");
    CHECK(fence_waits == 1 && fence_refs == 0, "the fence waited and dropped");
    CHECK(primary0.state->fb->base.id == fb_other && enables == 1 && updates == 2,
          "the flip shows the other framebuffer without a modeset");
    k_memset(&r, 0, sizeof(r));
    req_add(&r, &primary0.base, mc->prop_fb_id, fb_main);
    CHECK(req_commit(&fa, &r, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, 78) == 0 &&
              nsent == 2 && sent[1].user_data == 78 && primary0.state->fb->base.id == fb_main,
          "a NONBLOCK flip completes with its event");
    k_memset(&r, 0, sizeof(r));
    req_add(&r, &crtc0.base, mc->prop_active, 0);
    CHECK(req_commit(&fa, &r, 0, 0) == -EINVAL, "ACTIVE off without ALLOW_MODESET: EINVAL");
    req_off(&r);
    CHECK(req_commit(&fa, &r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == 0, "off");
    CHECK(!crtc0.state->active && !crtc0.state->enable && !primary0.state->fb &&
              !hdmi.state->crtc && disables == 1 && plane_offs == 1,
          "everything off");
}

/** A modeset: refused without ALLOW_MODESET, tested, committed with an
 * event; then the flips, and off. */
static void row_atomic(void)
{
    struct req r;
    struct drm_mode_crtc g;
    uint32_t mode_id;
    int ok;

    fa.atomic = fa.universal_planes = 1;
    mode_id = blob_new(&fa, &hdmi.modes->info, sizeof(hdmi.modes->info));
    CHECK(mode_id != 0, "a mode blob");
    row_atomic_refusals(mode_id);
    req_on(&r, mode_id, fb_main);
    CHECK(req_commit(&fa, &r, 0, 0) == -EINVAL, "a modeset without ALLOW_MODESET: EINVAL");
    CHECK(req_commit(&fa, &r, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == 0,
          "TEST_ONLY passes");
    CHECK(!crtc0.state->active && enables == 0, "TEST_ONLY changes nothing");
    CHECK(req_commit(&fa, &r, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT, 77) == 0,
          "the modeset");
    CHECK(crtc0.state->active && crtc0.state->enable && crtc0.state->mode.hdisplay == 1366 &&
              crtc0.state->mode_blob && crtc0.state->mode_blob->base.id == mode_id,
          "the CRTC is on in the panel's mode");
    CHECK(primary0.state->fb && primary0.state->fb->base.id == fb_main &&
              primary0.state->crtc == &crtc0 && primary0.state->visible,
          "the primary plane shows the framebuffer");
    CHECK(hdmi.state->crtc == &crtc0 && hdmi.state->best_encoder == &tmds &&
              hdmi.encoder == &tmds && tmds.crtc == &crtc0,
          "HDMI routed through its encoder");
    CHECK(enables == 1 && updates == 1 && flushes == 1, "the hooks ran once");
    ok = nsent == 1 && sent[0].base.type == DRM_EVENT_FLIP_COMPLETE && sent[0].user_data == 77 &&
         sent[0].crtc_id == crtc0.base.id && sent[0].tv_sec == 5 && sent[0].tv_usec == 250000;
    CHECK(ok, "FLIP_COMPLETE with the user data, the CRTC and the time");
    k_memset(&g, 0, sizeof(g));
    g.crtc_id = crtc0.base.id;
    CHECK(drm_mode_getcrtc(&dev, &g, &fa) == 0 && g.mode_valid && g.fb_id == fb_main &&
              g.mode.hdisplay == 1366 && g.gamma_size == 256,
          "GETCRTC");

    row_atomic_flips();
    CHECK(blob_destroy(&fa, mode_id) == 0, "the mode blob destroyed");
    CHECK(reserved == 0, "no event left reserved");
}

/* ---- 76-5a.3: the legacy calls ---- */

/** SETCRTC of crtc0 from `fb_id` at `x`, with HDMI in the panel's mode,
 * or off for `fb_id` 0. */
static int32_t setcrtc(uint32_t fb_id, uint32_t x)
{
    struct drm_mode_crtc s;
    uint32_t conn = hdmi.base.id;

    k_memset(&s, 0, sizeof(s));
    s.crtc_id = crtc0.base.id;
    s.fb_id = fb_id;
    s.x = x;
    if (fb_id) {
        s.mode_valid = 1;
        s.mode = hdmi.modes->info;
        s.set_connectors_ptr = UPTR(&conn);
        s.count_connectors = 1;
    }
    return drm_mode_setcrtc(&dev, &s, &fa);
}

/** SETCRTC on, its refusals, and off. */
static void row_setcrtc(void)
{
    CHECK(setcrtc(fb_main, 0x10000) == -ERANGE, "SETCRTC past 16 bits of x: ERANGE");
    CHECK(setcrtc(fb_main, 8) == -ENOSPC, "SETCRTC past the framebuffer: ENOSPC");
    CHECK(setcrtc(0xffffffffu, 0) == -EINVAL, "SETCRTC of the current framebuffer, none: EINVAL");
    CHECK(setcrtc(fb_main, 0) == 0, "SETCRTC");
    CHECK(crtc0.state->active && primary0.state->fb->base.id == fb_main &&
              hdmi.state->crtc == &crtc0 && primary0.state->src_w == 1366u << 16,
          "SETCRTC lights the panel through a commit");
    CHECK(setcrtc(0xffffffffu, 0) == 0 && primary0.state->fb->base.id == fb_main,
          "SETCRTC keeps the current framebuffer for -1");
}

/** SETPLANE of the overlay, on and off. */
static void row_setplane(void)
{
    struct drm_mode_set_plane s;

    k_memset(&s, 0, sizeof(s));
    s.plane_id = overlay.base.id;
    s.crtc_id = crtc0.base.id;
    s.fb_id = fb_main;
    s.crtc_x = 10;
    s.crtc_y = 20;
    s.crtc_w = 100;
    s.crtc_h = 50;
    s.src_w = 100u << 16;
    s.src_h = 50u << 16;
    CHECK(drm_mode_setplane(&dev, &s, &fa) == 0 && overlay.state->crtc == &crtc0 &&
              overlay.state->crtc_x == 10 && overlay.state->crtc_h == 50,
          "SETPLANE shows the overlay");
    s.src_w = 2000u << 16;
    CHECK(drm_mode_setplane(&dev, &s, &fa) == -ENOSPC, "SETPLANE past the framebuffer: ENOSPC");
    s.src_w = 100u << 16;
    s.crtc_w = 0x80000000u;
    CHECK(drm_mode_setplane(&dev, &s, &fa) == -ERANGE, "SETPLANE past INT_MAX: ERANGE");
    s.fb_id = fb_other;
    s.crtc_w = 100;
    s.plane_id = primary1.base.id;
    CHECK(drm_mode_setplane(&dev, &s, &fa) == -EINVAL,
          "SETPLANE on a CRTC the plane cannot take: EINVAL");
    s.plane_id = overlay.base.id;
    s.fb_id = 0;
    CHECK(drm_mode_setplane(&dev, &s, &fa) == 0 && !overlay.state->crtc && !overlay.state->fb,
          "SETPLANE of framebuffer 0 turns it off");
}

/** CURSOR and CURSOR2 through the cursor plane. */
static void row_cursor(void)
{
    struct drm_mode_cursor c;
    struct drm_mode_cursor2 c2;

    k_memset(&c, 0, sizeof(c));
    c.crtc_id = crtc0.base.id;
    c.flags = DRM_MODE_CURSOR_BO;
    c.width = c.height = 64;
    c.handle = h_cursor;
    CHECK(drm_mode_cursor_ioctl(&dev, &c, &fa) == 0 && cursor0.state->fb &&
              cursor0.state->fb->format == DRM_FORMAT_ARGB8888 && cursor0.state->crtc_w == 64,
          "CURSOR sets a 64x64 ARGB8888 framebuffer on the cursor plane");
    c.flags = DRM_MODE_CURSOR_MOVE;
    c.x = 30;
    c.y = 40;
    CHECK(drm_mode_cursor_ioctl(&dev, &c, &fa) == 0 && cursor0.state->crtc_x == 30 &&
              cursor0.state->crtc_y == 40 && crtc0.cursor_x == 30,
          "CURSOR moves it");
    c.flags = 4;
    CHECK(drm_mode_cursor_ioctl(&dev, &c, &fa) == -EINVAL, "an unknown cursor flag: EINVAL");
    c.flags = DRM_MODE_CURSOR_BO;
    c.crtc_id = crtc1.base.id;
    CHECK(drm_mode_cursor_ioctl(&dev, &c, &fa) == -ENXIO, "CURSOR on a CRTC with none: ENXIO");
    k_memset(&c2, 0, sizeof(c2));
    c2.crtc_id = crtc0.base.id;
    c2.flags = DRM_MODE_CURSOR_BO;
    CHECK(drm_mode_cursor2_ioctl(&dev, &c2, &fa) == 0 && !cursor0.state->fb && !cursor0.state->crtc,
          "CURSOR2 of handle 0 hides it");
}

/** PAGE_FLIP and its refusals. */
static void row_page_flip(void)
{
    struct drm_mode_crtc_page_flip f;
    int before = nsent;

    k_memset(&f, 0, sizeof(f));
    f.crtc_id = crtc0.base.id;
    f.fb_id = fb_other;
    f.flags = DRM_MODE_PAGE_FLIP_EVENT;
    f.user_data = 99;
    CHECK(drm_mode_page_flip_ioctl(&dev, &f, &fa) == 0 && primary0.state->fb->base.id == fb_other &&
              nsent == before + 1 && sent[before].user_data == 99,
          "PAGE_FLIP with an event");
    f.flags = DRM_MODE_PAGE_FLIP_TARGET_RELATIVE;
    CHECK(drm_mode_page_flip_ioctl(&dev, &f, &fa) == -EINVAL, "PAGE_FLIP to a target: EINVAL");
    f.flags = DRM_MODE_PAGE_FLIP_ASYNC;
    CHECK(drm_mode_page_flip_ioctl(&dev, &f, &fa) == -EINVAL,
          "PAGE_FLIP ASYNC on a device without it: EINVAL");
    f.flags = 0;
    f.crtc_id = crtc1.base.id;
    CHECK(drm_mode_page_flip_ioctl(&dev, &f, &fa) == -EBUSY, "PAGE_FLIP of a CRTC off: EBUSY");
    f.crtc_id = crtc0.base.id;
    f.fb_id = fb_main;
    CHECK(drm_mode_page_flip_ioctl(&dev, &f, &fa) == 0 && primary0.state->fb->base.id == fb_main,
          "PAGE_FLIP back without an event");
}

/** DIRTYFB: ENOSYS for a device that needs no flush, damage clips for one
 * that does. */
static void row_dirtyfb(void)
{
    struct drm_mode_fb_dirty_cmd d;
    struct drm_clip_rect clips[3] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}};
    const struct drm_mode_rect *rects;

    k_memset(&d, 0, sizeof(d));
    d.fb_id = fb_main;
    d.num_clips = 2;
    d.clips_ptr = UPTR(clips);
    CHECK(drm_mode_dirtyfb_ioctl(&dev, &d, &fa) == -ENOSYS, "DIRTYFB without fb_dirty: ENOSYS");
    mc->fb_dirty = 1;
    CHECK(drm_mode_dirtyfb_ioctl(&dev, &d, &fa) == 0, "DIRTYFB");
    rects = primary0.state->fb_damage_clips
                ? (const struct drm_mode_rect *)primary0.state->fb_damage_clips->data
                : NULL;
    CHECK(rects && primary0.state->fb_damage_clips->length == 2 * sizeof(*rects) &&
              rects[1].x1 == 5 && rects[1].y2 == 8,
          "DIRTYFB gives the primary plane its rectangles");
    d.flags = DRM_MODE_FB_DIRTY_ANNOTATE_COPY;
    d.num_clips = 3;
    CHECK(drm_mode_dirtyfb_ioctl(&dev, &d, &fa) == -EINVAL, "an odd count of copies: EINVAL");
    d.flags = 0;
    d.clips_ptr = 0;
    CHECK(drm_mode_dirtyfb_ioctl(&dev, &d, &fa) == -EINVAL, "clips with no pointer: EINVAL");
    mc->fb_dirty = 0;
}

/** DPMS through OBJ_SETPROPERTY and SETPROPERTY, and no flip while off. */
static void row_dpms(void)
{
    struct drm_mode_obj_set_property o;
    struct drm_mode_connector_set_property s;
    struct drm_mode_crtc_page_flip f;
    int before = disables;

    k_memset(&o, 0, sizeof(o));
    o.obj_id = hdmi.base.id;
    o.obj_type = DRM_MODE_OBJECT_CONNECTOR;
    o.prop_id = mc->dpms_property->base.id;
    o.value = DRM_MODE_DPMS_OFF;
    CHECK(drm_mode_obj_set_property_ioctl(&dev, &o, &fa) == 0 && !crtc0.state->active &&
              crtc0.state->enable && disables == before + 1 && hdmi.dpms == DRM_MODE_DPMS_OFF,
          "DPMS off turns the CRTC inactive");
    k_memset(&s, 0, sizeof(s));
    s.connector_id = hdmi.base.id;
    s.prop_id = mc->dpms_property->base.id;
    s.value = DRM_MODE_DPMS_STANDBY;
    CHECK(drm_connector_property_set_ioctl(&dev, &s, &fa) == 0 && !crtc0.state->active,
          "SETPROPERTY of standby is off");
    k_memset(&f, 0, sizeof(f));
    f.crtc_id = crtc0.base.id;
    f.fb_id = fb_other;
    CHECK(drm_mode_page_flip_ioctl(&dev, &f, &fa) == -EINVAL &&
              primary0.state->fb->base.id == fb_main,
          "PAGE_FLIP of a CRTC DPMS left off: EINVAL, as Linux's page_flip_common");
    s.value = DRM_MODE_DPMS_ON;
    CHECK(drm_connector_property_set_ioctl(&dev, &s, &fa) == 0 && crtc0.state->active &&
              hdmi.dpms == DRM_MODE_DPMS_ON,
          "SETPROPERTY of DPMS on turns it back on");
    o.obj_id = crtc0.base.id;
    o.obj_type = DRM_MODE_OBJECT_CRTC;
    CHECK(drm_mode_obj_set_property_ioctl(&dev, &o, &fa) == -EINVAL, "DPMS on a CRTC: EINVAL");
    o.prop_id = mc->prop_active->base.id;
    o.value = 0;
    CHECK(drm_mode_obj_set_property_ioctl(&dev, &o, &fa) == 0 && !crtc0.state->active,
          "ACTIVE through OBJ_SETPROPERTY, which allows a modeset as Linux's does");
    o.value = 1;
    CHECK(drm_mode_obj_set_property_ioctl(&dev, &o, &fa) == 0 && crtc0.state->active,
          "ACTIVE back on");
}

/** SETGAMMA as a GAMMA_LUT commit, and GETGAMMA. */
static void row_gamma(void)
{
    static uint16_t r[256], g[256], b[256], back[3][256];
    struct drm_mode_crtc_lut l;
    const struct drm_color_lut *lut;
    int i;

    for (i = 0; i < 256; i++) {
        r[i] = (uint16_t)(i * 257);
        g[i] = (uint16_t)(65535 - i * 257);
        b[i] = (uint16_t)i;
    }
    k_memset(&l, 0, sizeof(l));
    l.crtc_id = crtc0.base.id;
    l.gamma_size = 256;
    l.red = UPTR(back[0]);
    l.green = UPTR(back[1]);
    l.blue = UPTR(back[2]);
    CHECK(drm_mode_gamma_get_ioctl(&dev, &l, &fa) == 0 && back[0][1] == 0x100 &&
              back[2][255] == 0xff00,
          "GETGAMMA gives Linux's first ramp");
    l.red = UPTR(r);
    l.green = UPTR(g);
    l.blue = UPTR(b);
    CHECK(drm_mode_gamma_set_ioctl(&dev, &l, &fa) == 0, "SETGAMMA");
    lut =
        crtc0.state->gamma_lut ? (const struct drm_color_lut *)crtc0.state->gamma_lut->data : NULL;
    CHECK(lut && crtc0.state->gamma_lut->length == 256 * sizeof(*lut) && lut[1].red == 257 &&
              lut[1].green == 65535 - 257 && lut[255].blue == 255,
          "SETGAMMA commits GAMMA_LUT");
    l.red = UPTR(back[0]);
    l.green = UPTR(back[1]);
    l.blue = UPTR(back[2]);
    CHECK(drm_mode_gamma_get_ioctl(&dev, &l, &fa) == 0 && back[1][1] == 65535 - 257,
          "GETGAMMA gives it back");
    l.gamma_size = 255;
    CHECK(drm_mode_gamma_set_ioctl(&dev, &l, &fa) == -EINVAL, "SETGAMMA of a wrong size: EINVAL");
    l.crtc_id = crtc1.base.id;
    CHECK(drm_mode_gamma_set_ioctl(&dev, &l, &fa) == -ENOSYS,
          "SETGAMMA on a CRTC without gamma: ENOSYS");
}

/** A file's release takes its framebuffer off the screen and frees its
 * blobs; the device is then freed whole. */
static void row_release(void)
{
    uint32_t id = blob_new(&fa, "x", 1);
    int before = disables;

    CHECK(id != 0 && primary0.state->fb && primary0.state->fb->owner == &fa,
          "the panel shows the file's framebuffer");
    drm_mode_file_release(&fa);
    CHECK(!primary0.state->fb && !crtc0.state->active && disables == before + 1,
          "release turns the CRTC off when its primary plane loses the framebuffer");
    CHECK(drm_property_lookup_blob(&dev, id) == NULL, "release frees the file's blobs");
    CHECK(!mc->fbs, "no framebuffer left");
}

/** Run the rows. */
int main(void)
{
    int base, i;

    model_start("drm-kms-test");
    base = allocations;
    device_init();
    row_resources();
    row_planes();
    row_properties();
    row_connectors();
    row_blobs();
    row_framebuffers();
    row_atomic();
    row_setcrtc();
    row_setplane();
    row_cursor();
    row_page_flip();
    row_dirtyfb();
    row_dpms();
    row_gamma();
    row_release();
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
    printf("drm-kms-test: ok\n");
    return 0;
}
