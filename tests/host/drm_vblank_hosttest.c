/*
 * drm_vblank_hosttest.c: vblanks, their events and commit fences,
 * rach2/sys/drm/drm_vblank.c and drm_atomic.c's fences, events and commit
 * worker, against a fake driver whose CRTCs count vblanks from a timer;
 * drm_vblank64_hosttest.c runs the same rows with x86-64's structures.
 *
 * host-deps: rach2/sys/tick.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/fd_flat.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/atomic_host.c tests/host/compat/resv_host.c
 * host-deps: tests/host/compat/mutex_none.c
 *
 * The fake driver has two CRTCs, each with a primary plane, an encoder and
 * a connector with the Lenovo panel's EDID; the first also has CRCs. A
 * CRTC's timer runs while its vblank interrupt is on, and each step of the
 * test's clock by a frame of 60 Hz fires the vblank of every CRTC whose
 * timer runs, as vkms's timer does; a wait that would sleep steps the clock
 * until its condition holds, so a wait for a vblank sees one come. The
 * fences are the kernel's own, with sync files in a model descriptor table
 * and a sw_sync timeline the test advances. The rows: a modeset whose
 * blocking commit returns after the flip's event at the next vblank, and
 * the interrupt turned off five seconds after the last holder (76-5b.1);
 * WAIT_VBLANK relative, absolute, next on a miss, by the high CRTC bits
 * and SECONDARY, and its refusals; CRTC_GET_SEQUENCE and
 * CRTC_QUEUE_SEQUENCE; VBLANK, CRTC_SEQUENCE and FLIP_COMPLETE events read
 * in the order their vblanks send them, a nonblocking flip and EBUSY for
 * one that would get ahead of it, and the events a CRTC turned off sends
 * at once (76-5b.2); a nonblocking flip on an in-fence of sw_sync that
 * signals late, which flips, signals its out-fence and sends its event
 * only after the fence, a blocking flip waited on a late fence, and a
 * blocking commit that waits for the queued one ahead of it (76-5b.3);
 * a driver's bound on the wait for each in-fence, i915's 10 s, past which
 * a nonblocking and a blocking flip flip without it (80-72.2);
 * two planes' independent fences listened to in order, with the second
 * engine finishing first, each wait's own timeout and no later wait after
 * a timeout, even if the first signals before the worker runs (80-78);
 * connector forcing, the EDID override, and the CRC files' source,
 * entries, lines and overflow (76-5b.4); times of vblanks that never go
 * back, from a frame counter and a scanout position whose second reading
 * of a frame lies before its first (80-86), with the count of times that
 * went back read from debugfs's crtc-N/vblank_backwards, through
 * drm_debugfs.c and fs/debugfs.c, which are included whole (80-88); and an
 * unplug that sends what is queued with -ENODEV on its fences. Every
 * allocation is given back.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#define KMS_MODEL_FENCES
#include "drm_kms_model.c"
#include "dmabuf/fence.c"
#include "dmabuf/sync_file.c"
#include "dmabuf/sw_sync.c"
#include "fs/debugfs.c"
#include "drm/drm_debugfs.c"

extern int memcmp(const void *a, const void *b, unsigned long n);

/* One frame at 60 Hz, in nanoseconds. */
#define FRAME_NS 16666667ll

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

static struct drm_device dev;
static struct drm_plane primary[2];
static struct drm_crtc crtc[2];
static struct drm_encoder enc[2];
static struct drm_connector conn[2];
static struct drm_file fa;
static struct drm_mode_config *mc = &dev.mode_config;

/* The model's descriptor table, which the sync files and sw_sync use. */
static fd_table_t table;
static fd_entry_t slots[64];
static struct open_file descriptions[64];

/* The fake driver's state: each CRTC's timer, the hooks it saw, the
 * framebuffers its primary planes were given in order, and the CRC source
 * that runs. */
static int timer_on[2], timer_starts[2], timer_stops[2], enable_rc;
static int force_calls, conn1_no_edid;
static uint32_t shown[64];
static int nshown;
static const char *crc_running;
static uint32_t crc_frame;

/* A sw_sync timeline the next wait advances by one, as a producer that
 * finishes while the caller sleeps. */
static fd_entry_t *late_timeline;

/* ---- the descriptor table ---- */

/** Give out the lowest free descriptor of the model's table. */
int fd_alloc(fd_table_t *t)
{
    int i;

    for (i = 0; i < 64; i++) {
        if (t->chunks[0][i].type == FD_TYPE_NONE) {
            k_memset(&descriptions[i], 0, sizeof(descriptions[i]));
            t->chunks[0][i].ofd = &descriptions[i];
            return i;
        }
    }
    return -EMFILE;
}

/** A live descriptor of the model's table, or NULL. */
fd_entry_t *fd_get(fd_table_t *t, int fd)
{
    return fd >= 0 && fd < 64 && t->chunks[0][fd].type != FD_TYPE_NONE ? &t->chunks[0][fd] : NULL;
}

/** Close a descriptor, releasing its object. */
int fd_free(fd_table_t *t, int fd)
{
    fd_entry_t *e = fd_get(t, fd);

    if (!e) {
        return -EBADF;
    }
    if (e->ops && e->ops->close) {
        e->ops->close(e);
    }
    k_memset(e, 0, sizeof(*e));
    return 0;
}

/** A fixed wall clock for debugfs's nodes. */
void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1700000000;
    *nanoseconds = 0;
}

/** No process table: debugfs's clients file, which no row reads, names
 * none. */
process_t *get_process(uint32_t pid)
{
    (void)pid;
    return NULL;
}

/* ---- the fake driver ---- */

/** The index of a CRTC of the device. */
static int idx(const struct drm_crtc *c)
{
    return (int)c->index;
}

/** Start the CRTC's timer. */
static int32_t vblank_on(struct drm_crtc *c)
{
    if (enable_rc) {
        return enable_rc;
    }
    timer_on[idx(c)] = 1;
    timer_starts[idx(c)]++;
    return 0;
}

/** Stop the CRTC's timer. */
static void vblank_off(struct drm_crtc *c)
{
    timer_on[idx(c)] = 0;
    timer_stops[idx(c)]++;
}

/** The CRC sources: "auto" and "plane1", two values each. */
static int32_t crc_verify(struct drm_crtc *c, const char *name, uint32_t *values)
{
    (void)c;
    if (k_strlen(name) == 4 && memcmp(name, "auto", 4) == 0) {
        *values = 2;
        return 0;
    }
    if (k_strlen(name) == 6 && memcmp(name, "plane1", 6) == 0) {
        *values = 2;
        return 0;
    }
    return -EINVAL;
}

/** Start a source, or stop for NULL. */
static int32_t crc_set(struct drm_crtc *c, const char *name)
{
    (void)c;
    crc_running = name;
    crc_frame = 0;
    return 0;
}

/** A plane shown: record its framebuffer. */
static void plane_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
    (void)state;
    if (nshown < (int)(sizeof(shown) / sizeof(shown[0]))) {
        shown[nshown++] = plane->state->fb ? plane->state->fb->base.id : 0;
    }
}

/** A plane's check: unscaled, covering its CRTC. */
static int32_t fake_plane_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
    struct drm_plane_state *ps = state->planes_new[plane->index];
    const struct drm_crtc_state *cs;

    if (!ps->crtc) {
        return 0;
    }
    cs = state->crtcs_new[ps->crtc->index] ? state->crtcs_new[ps->crtc->index] : ps->crtc->state;
    return drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
                                               0, 1);
}

/** Every connector is plugged in. */
static int detect(struct drm_connector *connector, int force_probe)
{
    (void)connector;
    (void)force_probe;
    return DRM_CONNECTOR_CONNECTED;
}

/** The panel's EDID, but none on the second while `conn1_no_edid`. */
static int get_modes(struct drm_connector *connector)
{
    if (connector == &conn[1] && conn1_no_edid) {
        return drm_connector_update_edid(connector, NULL, 0);
    }
    return drm_connector_update_edid(connector, panel_edid, sizeof(panel_edid));
}

/** The debug force changed. */
static void forced(struct drm_connector *connector)
{
    (void)connector;
    force_calls++;
}

static const struct drm_plane_funcs plane_funcs = {fake_plane_check, plane_update, NULL};
static const struct drm_crtc_funcs crtc0_funcs = {
    NULL, NULL, NULL, NULL, NULL, NULL, vblank_on, vblank_off, crc_set, crc_verify, NULL};
static const struct drm_crtc_funcs crtc1_funcs = {NULL,      NULL,       NULL, NULL, NULL, NULL,
                                                  vblank_on, vblank_off, NULL, NULL, NULL};
static const struct drm_connector_funcs conn_funcs = {detect, get_modes, NULL, forced};
static const uint32_t xrgb[] = {DRM_FORMAT_XRGB8888};

/**
 * One frame of the clock: each CRTC whose timer runs adds a CRC entry when
 * a source runs, and has its vblank; then the commit worker runs, as a
 * thread woken meanwhile would.
 */
static void tick(void)
{
    uint32_t crcs[2];
    int i;

    model_now_ns += FRAME_NS;
    if (crc_running) {
        crcs[0] = 0xabc0u + crc_frame;
        crcs[1] = 0xdef0u + crc_frame;
        (void)drm_crtc_add_crc_entry(&crtc[0], 1, ++crc_frame, crcs);
    }
    for (i = 0; i < 2; i++) {
        if (timer_on[i]) {
            (void)drm_crtc_handle_vblank(&crtc[i]);
        }
    }
    (void)drm_atomic_commit_work();
}

/** `n` frames. */
static void ticks(int n)
{
    while (n-- > 0) {
        tick();
    }
}

/** A sleep: the producer of `late_timeline` finishes first, then frames
 * pass until the wait's condition holds. */
static void wait_hook(wq_cond_fn cond, void *arg)
{
    uint32_t one = 1;
    fd_entry_t *t;
    int i;

    for (i = 0; i < 1000 && !cond(arg); i++) {
        if (late_timeline) {
            t = late_timeline;
            late_timeline = NULL;
            CHECK(sw_ioctl(t, SW_SYNC_IOC_INC, (uintptr_t)&one) == 0, "the late producer signals");
            (void)drm_atomic_commit_work();
            continue;
        }
        tick();
    }
}

/** Make the fake device and open the master's file. */
static void device_init(void)
{
    static struct drm_driver driver;
    int i;

    driver.ident.name = "fake";
    dev.driver = &driver;
    dev.features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC;
    dev.refs = 1;
    CHECK(drm_mode_config_init(&dev) == 0, "the mode configuration is made");
    mc->min_width = mc->min_height = 1;
    mc->max_width = mc->max_height = 4096;
    for (i = 0; i < 2; i++) {
        CHECK(drm_universal_plane_init(&dev, &primary[i], 1u << i, &plane_funcs, xrgb, 1, NULL,
                                       DRM_PLANE_TYPE_PRIMARY) == 0,
              "a primary plane is made");
    }
    CHECK(drm_crtc_init_with_planes(&dev, &crtc[0], &primary[0], NULL, &crtc0_funcs) == 0 &&
              drm_crtc_init_with_planes(&dev, &crtc[1], &primary[1], NULL, &crtc1_funcs) == 0,
          "the CRTCs are made");
    for (i = 0; i < 2; i++) {
        CHECK(drm_encoder_init(&dev, &enc[i], DRM_MODE_ENCODER_TMDS) == 0, "an encoder is made");
        enc[i].possible_crtcs = 1u << i;
        CHECK(drm_connector_init(&dev, &conn[i], &conn_funcs, DRM_MODE_CONNECTOR_HDMIA) == 0,
              "a connector is made");
        drm_connector_attach_encoder(&conn[i], &enc[i]);
        CHECK(drm_helper_probe_single_connector_modes(&conn[i], 0, 0) == 1, "the panel's one mode");
    }
    file_open(&fa, &dev);
    fa.atomic = fa.universal_planes = 1;
    dev.master = &fa;
    fa.is_master = 1;
    table.chunks[0] = slots;
    table.cap = 64;
    kms_self.fds = &table;
    model_wait = wait_hook;
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

/** ATOMIC of `r` with `flags` and `user_data`. */
static int32_t req_commit(const struct req *r, uint32_t flags, uint64_t user_data)
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
    return drm_mode_atomic_ioctl(&dev, &a, &fa);
}

/** A request that shows `fb_id` on CRTC `i`'s primary plane, alone. */
static void req_flip(struct req *r, int i, uint32_t fb_id)
{
    k_memset(r, 0, sizeof(*r));
    req_add(r, &primary[i].base, mc->prop_fb_id, fb_id);
}

/** A request that lights CRTC `i` from `fb_id` with the mode blob `mode`,
 * or turns it off for 0. */
static void req_mode(struct req *r, int i, uint32_t mode, uint32_t fb_id)
{
    k_memset(r, 0, sizeof(*r));
    req_add(r, &crtc[i].base, mc->prop_mode_id, mode);
    req_add(r, &crtc[i].base, mc->prop_active, mode != 0);
    req_add(r, &conn[i].base, mc->prop_crtc_id, mode ? crtc[i].base.id : 0);
    req_add(r, &primary[i].base, mc->prop_fb_id, fb_id);
    req_add(r, &primary[i].base, mc->prop_crtc_id, mode ? crtc[i].base.id : 0);
    req_add(r, &primary[i].base, mc->prop_src_w, mode ? 1366u << 16 : 0);
    req_add(r, &primary[i].base, mc->prop_src_h, mode ? 768u << 16 : 0);
    req_add(r, &primary[i].base, mc->prop_crtc_w, mode ? 1366 : 0);
    req_add(r, &primary[i].base, mc->prop_crtc_h, mode ? 768 : 0);
}

/** A framebuffer of 1366x768 over buffer object `i`. */
static uint32_t fb_new(int i)
{
    struct drm_mode_fb_cmd2 r;

    k_memset(&r, 0, sizeof(r));
    r.width = 1366;
    r.height = 768;
    r.pixel_format = DRM_FORMAT_XRGB8888;
    r.handles[0] = gem_new(&fa, i, 1366u * 4 * 768 + 4096);
    r.pitches[0] = 1366 * 4;
    CHECK(drm_mode_addfb2_ioctl(&dev, &r, &fa) == 0, "a framebuffer is made");
    return r.fb_id;
}

/** WAIT_VBLANK of `type` and `sequence` with `signal`: the answer, and the
 * reply in `*w`. */
static int32_t wait_vblank(union drm_wait_vblank *w, uint32_t type, uint32_t sequence,
                           uintptr_t signal)
{
    k_memset(w, 0, sizeof(*w));
    w->request.type = (enum drm_vblank_seq_type)type;
    w->request.sequence = sequence;
    w->request.signal = signal;
    return drm_wait_vblank_ioctl(&dev, w, &fa);
}

static uint32_t mode_id, fb_a, fb_b, fb_c;

/* ---- 76-5b.1: counts, waits and the off-delay ---- */

/** Light both CRTCs; the blocking commit returns after the vblank that
 * sends its event. */
static void row_modeset(void)
{
    struct drm_mode_create_blob b;
    struct req r;
    int64_t at;

    k_memset(&b, 0, sizeof(b));
    b.data = UPTR(&conn[0].modes->info);
    b.length = sizeof(conn[0].modes->info);
    CHECK(drm_mode_createblob_ioctl(&dev, &b, &fa) == 0, "a mode blob");
    mode_id = b.blob_id;
    fb_a = fb_new(0);
    fb_b = fb_new(1);
    fb_c = fb_new(2);
    CHECK(drm_dev_has_vblank(&dev), "the device has vblanks");
    CHECK(crtc[0].vblank.inmodeset && crtc[0].vblank.refs == 1 && !timer_on[0],
          "a CRTC starts off, with the hold that keeps it so");
    CHECK(drm_crtc_vblank_get(&crtc[0]) == -EINVAL, "no hold on a CRTC that is off");
    req_mode(&r, 0, mode_id, fb_a);
    at = model_now_ns;
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT, 11) == 0,
          "CRTC 0 lit with an event");
    CHECK(nsent == 1 && sent[0].base.type == DRM_EVENT_FLIP_COMPLETE && sent[0].user_data == 11 &&
              sent[0].crtc_id == crtc[0].base.id,
          "its FLIP_COMPLETE");
    CHECK(sent[0].sequence == 2 && crtc[0].vblank.count == 2,
          "one count for the CRTC coming on, one for the vblank that showed it");
    CHECK(model_now_ns == at + FRAME_NS &&
              sent[0].tv_sec == (uint32_t)(model_now_ns / 1000000000) &&
              sent[0].tv_usec == (uint32_t)(model_now_ns % 1000000000 / 1000),
          "the blocking commit returned at the vblank, stamped with its time");
    CHECK(!crtc[0].flip_pending && timer_on[0] && crtc[0].vblank.refs == 0,
          "no flip pending; the interrupt stays on with no holder");
    req_mode(&r, 1, mode_id, fb_b);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == 0 && nsent == 1,
          "CRTC 1 lit; the core's own event is not the file's");
    CHECK(!crtc[1].flip_pending && timer_on[1], "its flip done at its vblank");
}

/** The interrupt goes off at the first vblank five seconds after the last
 * holder let go, and the count stays. */
static void row_offdelay(void)
{
    union drm_wait_vblank w;
    int64_t put = crtc[0].vblank.put_ns;
    uint64_t count;
    int n = 0;

    while (timer_on[0] && n < 400) {
        tick();
        n++;
    }
    CHECK(!timer_on[0] && timer_stops[0] == 1, "the interrupt went off");
    CHECK(model_now_ns - put >= 5000000000ll && model_now_ns - put < 5000000000ll + FRAME_NS,
          "at the first vblank past the off-delay");
    count = crtc[0].vblank.count;
    ticks(3);
    CHECK(crtc[0].vblank.count == count, "no count while off");
    CHECK(wait_vblank(&w, _DRM_VBLANK_RELATIVE, 0, 0) == 0 && w.reply.sequence == count &&
              timer_on[0],
          "a query turns it on again and answers the count");
    enable_rc = -EIO;
    ticks(400);
    CHECK(!timer_on[0], "off again");
    CHECK(wait_vblank(&w, _DRM_VBLANK_RELATIVE, 1, 0) == -EIO && crtc[0].vblank.refs == 0,
          "the driver's refusal to turn it on, with no hold kept");
    enable_rc = 0;
}

/** WAIT_VBLANK's forms and refusals. */
static void row_wait(void)
{
    union drm_wait_vblank w;
    uint64_t count;

    CHECK(wait_vblank(&w, _DRM_VBLANK_RELATIVE, 1, 0) == 0, "wait for the next vblank");
    count = crtc[0].vblank.count;
    CHECK(w.reply.sequence == count && w.reply.tval_sec == (intptr_t)(model_now_ns / 1000000000) &&
              w.reply.tval_usec == (intptr_t)(model_now_ns % 1000000000 / 1000),
          "the reply has the count reached and its time");
    CHECK(crtc[0].vblank.refs == 0, "the wait's hold let go");
    CHECK(wait_vblank(&w, _DRM_VBLANK_RELATIVE, 2, 0) == 0 && w.reply.sequence == count + 2,
          "two vblanks on");
    count += 2;
    CHECK(wait_vblank(&w, _DRM_VBLANK_ABSOLUTE, (uint32_t)count - 1, 0) == 0 &&
              w.reply.sequence == count,
          "an absolute count passed answers at once");
    CHECK(wait_vblank(&w, _DRM_VBLANK_ABSOLUTE | _DRM_VBLANK_NEXTONMISS, (uint32_t)count - 1, 0) ==
                  0 &&
              w.reply.sequence == count + 1,
          "NEXTONMISS waits for the next");
    count++;
    CHECK(wait_vblank(&w, _DRM_VBLANK_ABSOLUTE, (uint32_t)count + 1, 0) == 0 &&
              w.reply.sequence == count + 1,
          "an absolute count ahead");
    CHECK(wait_vblank(&w, _DRM_VBLANK_RELATIVE | (1u << _DRM_VBLANK_HIGH_CRTC_SHIFT), 1, 0) == 0 &&
              w.reply.sequence == crtc[1].vblank.count,
          "the high CRTC bits choose CRTC 1");
    CHECK(wait_vblank(&w, _DRM_VBLANK_RELATIVE | _DRM_VBLANK_SECONDARY, 0, 0) == 0 &&
              w.reply.sequence == crtc[1].vblank.count,
          "SECONDARY chooses CRTC 1");
    CHECK(wait_vblank(&w, _DRM_VBLANK_RELATIVE | (2u << _DRM_VBLANK_HIGH_CRTC_SHIFT), 1, 0) ==
              -EINVAL,
          "a CRTC the device has not: EINVAL");
    CHECK(wait_vblank(&w, _DRM_VBLANK_RELATIVE | _DRM_VBLANK_SIGNAL, 1, 0) == -EINVAL,
          "SIGNAL: EINVAL");
    CHECK(wait_vblank(&w, _DRM_VBLANK_RELATIVE | 0x40, 1, 0) == -EINVAL, "an unknown bit: EINVAL");
    CHECK(crtc[0].vblank.refs == 0 && crtc[1].vblank.refs == 0, "no hold kept");
}

/** CRTC_GET_SEQUENCE and its refusals. */
static void row_get_sequence(void)
{
    struct drm_crtc_get_sequence g;

    k_memset(&g, 0, sizeof(g));
    g.crtc_id = crtc[0].base.id;
    CHECK(drm_crtc_get_sequence_ioctl(&dev, &g, &fa) == 0 && g.active == 1 &&
              g.sequence == crtc[0].vblank.count && g.sequence_ns == crtc[0].vblank.time_ns,
          "CRTC_GET_SEQUENCE: enabled, its count and the time of its last vblank");
    g.crtc_id = primary[0].base.id;
    CHECK(drm_crtc_get_sequence_ioctl(&dev, &g, &fa) == -ENOENT, "a plane's ID: ENOENT");
}

/* ---- 76-5b.2: events ---- */

/** VBLANK, CRTC_SEQUENCE and FLIP_COMPLETE events come in the order of
 * their counts, and in the order queued at one count. */
static void row_events(void)
{
    struct drm_crtc_queue_sequence q;
    union drm_wait_vblank w;
    struct req r;
    uint64_t count;
    int base;

    CHECK(wait_vblank(&w, _DRM_VBLANK_RELATIVE, 1, 0) == 0, "the interrupt on");
    count = crtc[0].vblank.count;
    base = nsent;
    CHECK(wait_vblank(&w, _DRM_VBLANK_RELATIVE | _DRM_VBLANK_EVENT, 2, 21) == 0 &&
              w.reply.sequence == count + 2 && nsent == base,
          "a VBLANK event two vblanks on, queued");
    k_memset(&q, 0, sizeof(q));
    q.crtc_id = crtc[0].base.id;
    q.flags = DRM_CRTC_SEQUENCE_RELATIVE;
    q.sequence = 1;
    q.user_data = 22;
    CHECK(drm_crtc_queue_sequence_ioctl(&dev, &q, &fa) == 0 && q.sequence == count + 1,
          "a CRTC_SEQUENCE event at the next");
    req_flip(&r, 0, fb_b);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, 23) == 0 &&
              nsent == base && crtc[0].flip_pending == DRM_FLIP_ARMED,
          "a nonblocking flip returns before its vblank");
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, 24) == -EBUSY,
          "a second nonblocking flip before it: EBUSY");
    CHECK(crtc[0].vblank.refs == 3, "each queued event holds the vblanks");
    tick();
    CHECK(nsent == base + 2 && sent_seq[base].base.type == DRM_EVENT_CRTC_SEQUENCE &&
              sent_seq[base].user_data == 22 && sent_seq[base].sequence == count + 1 &&
              sent_seq[base].time_ns == model_now_ns,
          "first the CRTC_SEQUENCE event, with its count and time in nanoseconds");
    CHECK(sent[base + 1].base.type == DRM_EVENT_FLIP_COMPLETE && sent[base + 1].user_data == 23 &&
              sent[base + 1].sequence == (uint32_t)(count + 1) && !crtc[0].flip_pending,
          "then the flip's FLIP_COMPLETE, queued after it");
    tick();
    CHECK(nsent == base + 3 && sent[base + 2].base.type == DRM_EVENT_VBLANK &&
              sent[base + 2].user_data == 21 && sent[base + 2].sequence == (uint32_t)(count + 2) &&
              sent[base + 2].crtc_id == crtc[0].base.id,
          "then the VBLANK event at its count");
    CHECK(crtc[0].vblank.refs == 0 && reserved == 0, "every hold and reservation given back");
    q.flags = DRM_CRTC_SEQUENCE_NEXT_ON_MISS;
    q.sequence = 1;
    CHECK(drm_crtc_queue_sequence_ioctl(&dev, &q, &fa) == 0 &&
              q.sequence == crtc[0].vblank.count + 1,
          "NEXT_ON_MISS of a count passed: the next");
    q.flags = 0;
    q.sequence = 1;
    CHECK(drm_crtc_queue_sequence_ioctl(&dev, &q, &fa) == 0 && nsent == base + 4 &&
              q.sequence == crtc[0].vblank.count,
          "a count passed is sent at once");
    q.flags = 4;
    CHECK(drm_crtc_queue_sequence_ioctl(&dev, &q, &fa) == -EINVAL, "an unknown flag: EINVAL");
    tick();
    CHECK(nsent == base + 5 && reserved == 0, "the queued one at its count");
    CHECK(wait_vblank(&w, _DRM_VBLANK_ABSOLUTE | _DRM_VBLANK_EVENT,
                      (uint32_t)crtc[0].vblank.count - 3, 25) == 0 &&
              nsent == base + 6 && w.reply.sequence == crtc[0].vblank.count,
          "a VBLANK event of a count passed is sent at once");
}

/** Turning a CRTC off sends what it had queued at once, with the count
 * reached, and its vblanks refuse holders until it comes on again. */
static void row_off(void)
{
    struct drm_crtc_get_sequence g;
    union drm_wait_vblank w;
    struct req r;
    uint64_t count;
    int base;

    CHECK(wait_vblank(&w, _DRM_VBLANK_RELATIVE | _DRM_VBLANK_SECONDARY | _DRM_VBLANK_EVENT, 50,
                      31) == 0,
          "a VBLANK event far ahead on CRTC 1");
    count = crtc[1].vblank.count;
    base = nsent;
    req_mode(&r, 1, 0, 0);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT, 32) == 0,
          "CRTC 1 off with an event");
    CHECK(nsent == base + 2 && sent[base].user_data == 31 && sent[base].sequence == count &&
              sent[base + 1].user_data == 32,
          "the queued event goes at once, then the commit's");
    CHECK(!timer_on[1] && crtc[1].vblank.inmodeset && !crtc[1].flip_pending,
          "its interrupt off and held off");
    k_memset(&g, 0, sizeof(g));
    g.crtc_id = crtc[1].base.id;
    CHECK(drm_crtc_get_sequence_ioctl(&dev, &g, &fa) == -EINVAL,
          "CRTC_GET_SEQUENCE of a CRTC that is off: EINVAL");
    CHECK(wait_vblank(&w, _DRM_VBLANK_RELATIVE | _DRM_VBLANK_SECONDARY, 1, 0) == -EINVAL,
          "WAIT_VBLANK on it: EINVAL");
    req_mode(&r, 1, mode_id, fb_b);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == 0 &&
              crtc[1].vblank.count == count + 2 && !crtc[1].vblank.inmodeset,
          "on again: one count for coming on, one for its flip's vblank");
}

/* ---- 76-5b.3: fences ---- */

/** A sw_sync timeline on `t` and a point at `value` on it: the point's sync
 * file. */
static int32_t sw_point(fd_entry_t *t, uint32_t value)
{
    struct sw_sync_create_fence_data data;

    k_memset(&data, 0, sizeof(data));
    data.value = value;
    CHECK(sw_ioctl(t, SW_SYNC_IOC_CREATE_FENCE, (uintptr_t)&data) == 0, "a sw_sync point");
    return data.fence;
}

/** The status of the fence of sync file `fd`: 0 pending, 1 signalled, or
 * its error. */
static int32_t fd_status(int32_t fd)
{
    struct dma_fence *f = sync_file_get_fence(fd);
    int32_t st;

    if (!f) {
        return -EBADF;
    }
    st = dma_fence_status(f);
    dma_fence_put(f);
    return st;
}

/* The fences the core told the driver a deferred commit's tail would
 * wait for (`commit_fence_wait`), the last of them in `tail_wait`. */
static int tail_waits;
static struct dma_fence *tail_wait;
/* A signaller holding the driver's interrupt and engine locks. */
static int plane_irq_held;

/** The driver's `commit_fence_wait`, counted. */
static void count_tail_wait(struct drm_device *d, struct dma_fence *fence)
{
    (void)d;
    CHECK(!plane_irq_held, "planes: the wait hook runs outside the driver's interrupt locks");
    tail_waits++;
    tail_wait = fence;
}

/** A nonblocking flip on a late in-fence flips, signals its out-fence and
 * sends its event only after the fence, and tells the driver of the fence
 * its tail waits for once, from the worker; a blocking flip waits for
 * its fence; a blocking commit waits for the queued one ahead of it. */
static void row_fences(void)
{
    struct sync_file_info info;
    struct dma_fence *in;
    fd_entry_t timeline;
    int32_t in_fd, out_fd = 77, out2 = 77, base;
    struct req r;
    uint32_t one = 1;
    int at;

    k_memset(&timeline, 0, sizeof(timeline));
    CHECK(sw_sync_open(&timeline) == 0, "a sw_sync timeline");
    in_fd = sw_point(&timeline, 1);
    base = nsent;
    at = nshown;
    req_flip(&r, 0, fb_c);
    req_add(&r, &primary[0].base, mc->prop_in_fence_fd, (uint64_t)in_fd);
    req_add(&r, &crtc[0].base, mc->prop_out_fence_ptr, UPTR(&out_fd));
    mc->commit_fence_wait = count_tail_wait;
    tail_waits = 0;
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, 41) == 0,
          "a nonblocking flip on an in-fence that has not signalled");
    CHECK(tail_waits == 0, "queuing leaves the fence wait to the worker");
    CHECK(drm_atomic_commit_work() == 0, "the worker starts the deferred wait in process context");
    in = sync_file_get_fence(in_fd);
    CHECK(tail_waits == 1 && tail_wait == in,
          "the driver is told of the in-fence its tail waits for");
    dma_fence_put(in);
    CHECK(out_fd >= 0 && out_fd != 77 && fd_status(out_fd) == 0, "its out-fence, pending");
    k_memset(&info, 0, sizeof(info));
    CHECK(sync_ioctl(fd_get(&table, out_fd), SYNC_IOC_FILE_INFO, (uintptr_t)&info) == 0 &&
              memcmp(info.name, "fake-CRTC:", 10) == 0,
          "the out-fence's sync file is named as Linux names it");
    CHECK(crtc[0].flip_pending == DRM_FLIP_QUEUED && nshown == at &&
              primary[0].state->fb->base.id == fb_c,
          "the state is swapped in, and the plane not yet programmed");
    req_flip(&r, 0, fb_a);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_NONBLOCK, 0) == -EBUSY, "a commit behind it: EBUSY");
    ticks(5);
    CHECK(nshown == at && nsent == base && fd_status(out_fd) == 0,
          "five vblanks pass with the fence pending: no flip, no event, no out-fence");
    CHECK(sw_ioctl(&timeline, SW_SYNC_IOC_INC, (uintptr_t)&one) == 0, "the fence signals");
    CHECK(drm_atomic_commit_work() == 1 && nshown == at + 1 && shown[at] == fb_c &&
              crtc[0].flip_pending == DRM_FLIP_ARMED && nsent == base,
          "the worker flips once it has signalled, and the event waits for the vblank");
    CHECK(tail_waits == 1, "with no plane's fence left, the driver is told of none");
    mc->commit_fence_wait = NULL;
    tick();
    CHECK(nsent == base + 1 && sent[base].user_data == 41 && fd_status(out_fd) == 1 &&
              !crtc[0].flip_pending,
          "the vblank sends the event and signals the out-fence");
    fd_free(&table, in_fd);
    fd_free(&table, out_fd);

    in_fd = sw_point(&timeline, 2);
    late_timeline = &timeline;
    at = nshown;
    req_flip(&r, 0, fb_a);
    req_add(&r, &primary[0].base, mc->prop_in_fence_fd, (uint64_t)in_fd);
    req_add(&r, &crtc[0].base, mc->prop_out_fence_ptr, UPTR(&out2));
    CHECK(req_commit(&r, 0, 0) == 0 && !late_timeline && nshown == at + 1 && shown[at] == fb_a,
          "a blocking flip waits for its late fence, then flips");
    CHECK(out2 >= 0 && fd_status(out2) == 1, "and returns with its out-fence signalled");
    fd_free(&table, in_fd);
    fd_free(&table, out2);

    in_fd = sw_point(&timeline, 3);
    at = nshown;
    req_flip(&r, 0, fb_b);
    req_add(&r, &primary[0].base, mc->prop_in_fence_fd, (uint64_t)in_fd);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_NONBLOCK, 0) == 0 &&
              crtc[0].flip_pending == DRM_FLIP_QUEUED,
          "a queued flip");
    late_timeline = &timeline;
    req_flip(&r, 0, fb_c);
    CHECK(req_commit(&r, 0, 0) == 0 && nshown == at + 2 && shown[at] == fb_b &&
              shown[at + 1] == fb_c && primary[0].state->fb->base.id == fb_c,
          "a blocking commit behind it waits until the queued one has flipped");
    fd_free(&table, in_fd);
    sw_close(&timeline);
    CHECK(!works, "no commit left queued");
}

/** A request that shows `fb_id` on primary plane 1, on CRTC 1, with the
 * in-fence `in_fd`, and changes nothing of the CRTC. */
static void req_plane1(struct req *r, uint32_t fb_id, int32_t in_fd)
{
    k_memset(r, 0, sizeof(*r));
    req_add(r, &primary[1].base, mc->prop_fb_id, fb_id);
    req_add(r, &primary[1].base, mc->prop_crtc_id, crtc[1].base.id);
    req_add(r, &primary[1].base, mc->prop_src_w, 1366u << 16);
    req_add(r, &primary[1].base, mc->prop_src_h, 768u << 16);
    req_add(r, &primary[1].base, mc->prop_crtc_w, 1366);
    req_add(r, &primary[1].base, mc->prop_crtc_h, 768);
    if (in_fd >= 0) {
        req_add(r, &primary[1].base, mc->prop_in_fence_fd, (uint64_t)in_fd);
    }
}

/** A commit queued on an in-fence over a CRTC that is off has no flip, so
 * only its objects tell a later commit to stall: a nonblocking one of the
 * same plane answers EBUSY, and a blocking one finishes it first, without
 * the worker, before its own tail frees the queued commit's states. */
static void row_queued_off(void)
{
    fd_entry_t timeline;
    struct req r;
    int32_t in_fd;

    req_mode(&r, 1, 0, 0);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == 0 && !crtc[1].state->active,
          "CRTC 1 off");
    k_memset(&timeline, 0, sizeof(timeline));
    CHECK(sw_sync_open(&timeline) == 0, "a sw_sync timeline");
    in_fd = sw_point(&timeline, 1);
    req_plane1(&r, fb_a, in_fd);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_NONBLOCK, 0) == 0 && works && !crtc[1].flip_pending,
          "a plane queued on its fence over the CRTC that is off, with no flip");
    req_plane1(&r, fb_b, -1);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_NONBLOCK, 0) == -EBUSY,
          "a nonblocking commit of the same plane: EBUSY");
    late_timeline = &timeline;
    CHECK(req_commit(&r, 0, 0) == 0 && !late_timeline && !works &&
              primary[1].state->fb->base.id == fb_b,
          "a blocking one waits for the fence and finishes the queued commit first");
    fd_free(&table, in_fd);
    sw_close(&timeline);
    req_mode(&r, 1, mode_id, fb_b);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == 0 && crtc[1].state->active,
          "CRTC 1 on again");
}

/**
 * 80-72.2: under the driver's `commit_fence_timeout_ms`, as i915's, a
 * nonblocking flip whose in-fence never signals flips once 10 s have
 * passed, its callback taken back, and a blocking one waits 10 s, then
 * flips, as Linux's intel_atomic_commit_fence_wait programs the commit
 * after i915_fence_timeout.
 */
static void row_fence_timeout(void)
{
    fd_entry_t timeline;
    struct dma_fence *in;
    struct req r;
    int32_t in_fd;
    int64_t t0;
    int at;

    k_memset(&timeline, 0, sizeof(timeline));
    CHECK(sw_sync_open(&timeline) == 0, "timeout: a sw_sync timeline");
    mc->commit_fence_timeout_ms = 10000;
    in_fd = sw_point(&timeline, 1);
    at = nshown;
    t0 = model_now_ns;
    req_flip(&r, 0, fb_a);
    req_add(&r, &primary[0].base, mc->prop_in_fence_fd, (uint64_t)in_fd);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_NONBLOCK, 0) == 0 &&
              crtc[0].flip_pending == DRM_FLIP_QUEUED,
          "timeout: a nonblocking flip on a fence that never signals");
    while (model_now_ns - t0 < 9900000000ll) {
        tick();
    }
    CHECK(works && nshown == at, "timeout: no flip before 10 s");
    while (works && model_now_ns - t0 < 10100000000ll) {
        tick();
    }
    in = sync_file_get_fence(in_fd);
    CHECK(!works && nshown == at + 1 && shown[at] == fb_a && in && !in->signaled && !in->cbs,
          "timeout: at 10 s the worker flips without it, its callback taken back");
    dma_fence_put(in);
    tick();
    CHECK(!crtc[0].flip_pending, "timeout: the flip ends at its vblank");
    fd_free(&table, in_fd);

    in_fd = sw_point(&timeline, 2);
    at = nshown;
    t0 = model_now_ns;
    req_flip(&r, 0, fb_b);
    req_add(&r, &primary[0].base, mc->prop_in_fence_fd, (uint64_t)in_fd);
    CHECK(req_commit(&r, 0, 0) == 0 && nshown == at + 1 && shown[at] == fb_b &&
              model_now_ns - t0 >= 10000000000ll,
          "timeout: a blocking flip waits 10 s for a fence that never signals, then flips");
    fd_free(&table, in_fd);
    mc->commit_fence_timeout_ms = 0;
    sw_close(&timeline);
    CHECK(!works, "timeout: no commit left queued");
}

/* An independent engine's completion, visible without enabling its
 * signal; complete_on_enable models completion during callback setup. */
struct plane_fence {
    struct dma_fence base;
    int done, complete_on_enable, enables;
};

/** Read the engine's completion without listening for it. */
static int plane_fence_signaled(struct dma_fence *fence)
{
    return ((struct plane_fence *)fence)->done;
}

/** Count enabling the engine's signal; zero if it completes meanwhile. */
static int plane_fence_enable(struct dma_fence *fence)
{
    struct plane_fence *f = (struct plane_fence *)fence;

    CHECK(!plane_irq_held, "planes: listening begins outside the driver's interrupt locks");
    f->enables++;
    if (f->complete_on_enable) {
        f->done = 1;
    }
    return !f->done;
}

/** The row owns the fence's storage. */
static void plane_fence_release(struct dma_fence *fence)
{
    (void)fence;
}

static const struct dma_fence_ops plane_fence_ops = {
    NULL, NULL, plane_fence_enable, plane_fence_release, plane_fence_signaled, NULL, 0};

/** Signal an engine's completion while its interrupt locks are held. */
static void plane_fence_signal(struct plane_fence *f)
{
    f->done = 1;
    plane_irq_held = 1;
    CHECK(dma_fence_signal(&f->base) == 0, "planes: the engine signals");
    plane_irq_held = 0;
}

/* The fences and descriptors of one two-plane commit. */
struct plane_wait_case {
    struct plane_fence f[2];
    int32_t fd[2];
    int shown;
    uint64_t deadline;
};

/** Queue two planes, then let the worker begin the first fence wait. */
static void plane_wait_begin(struct plane_wait_case *c, uint32_t timeout_ms)
{
    struct req r;
    int i;

    k_memset(c, 0, sizeof(*c));
    for (i = 0; i < 2; i++) {
        dma_fence_init(&c->f[i].base, &plane_fence_ops, dma_fence_context_alloc(1), 1);
        c->fd[i] = sync_file_create(&c->f[i].base, "plane");
        CHECK(c->fd[i] >= 0, "planes: an independent engine's fence exported");
    }
    mc->commit_fence_timeout_ms = timeout_ms;
    tail_waits = 0;
    c->shown = nshown;
    req_flip(&r, 0, fb_a);
    req_add(&r, &primary[0].base, mc->prop_in_fence_fd, (uint64_t)c->fd[0]);
    req_add(&r, &primary[1].base, mc->prop_fb_id, fb_b);
    req_add(&r, &primary[1].base, mc->prop_in_fence_fd, (uint64_t)c->fd[1]);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_NONBLOCK, 0) == 0 && works && tail_waits == 0 &&
              !c->f[0].base.listened && !c->f[1].base.listened,
          "planes: queuing leaves both waits to the worker");
    CHECK(drm_atomic_commit_work() == 0, "planes: the worker begins the first wait");
    CHECK(c->f[0].base.listened && c->f[0].enables == 1 && c->f[0].base.cbs &&
              !c->f[1].base.listened && !c->f[1].enables && !c->f[1].base.cbs && tail_waits == 1 &&
              tail_wait == &c->f[0].base,
          "planes: only the first engine is listened to and handed to the wait hook");
    c->deadline = works ? works->deadline : 0;
}

/** Check a finished commit and give back its descriptors and fence holds. */
static void plane_wait_end(struct plane_wait_case *c)
{
    int i;

    CHECK(!works && nshown == c->shown + 2 && !c->f[0].base.cbs && !c->f[1].base.cbs,
          "planes: both planes programmed with no work or callback left");
    tick();
    CHECK(!crtc[0].flip_pending && !crtc[1].flip_pending,
          "planes: both flips end at their vblanks");
    for (i = 0; i < 2; i++) {
        fd_free(&table, c->fd[i]);
        CHECK(c->f[i].base.refs == 1, "planes: only the row's fence reference remains");
        dma_fence_put(&c->f[i].base);
    }
}

/** A timeout ends both waits whether the first signal precedes or follows
 * the worker. No next-plane hook runs under the signaller's locks. */
static void plane_wait_timeout(int signal_first)
{
    struct plane_wait_case c;

    plane_wait_begin(&c, 10000);
    model_now_ns = (int64_t)c.deadline * TICK_NS;
    if (signal_first) {
        plane_fence_signal(&c.f[0]);
    }
    CHECK(drm_atomic_commit_work() == 1 && !c.f[0].base.cbs,
          "planes: timeout removes the current callback and finishes the commit");
    if (!signal_first) {
        plane_fence_signal(&c.f[0]);
    }
    CHECK(!c.f[1].base.listened && !c.f[1].enables && !c.f[1].base.cbs && tail_waits == 1,
          "planes: a signal after timeout never arms or boosts the second fence");
    plane_wait_end(&c);
}

/** Each pending plane gets a full wait, including when the worker resumes
 * after a deadline whose fence completed in time; zero has no timeout. */
static void plane_wait_next(uint32_t timeout_ms, int delayed_worker)
{
    struct plane_wait_case c;

    plane_wait_begin(&c, timeout_ms);
    model_now_ns += 5000000000ll;
    plane_fence_signal(&c.f[0]);
    CHECK(!c.f[1].base.listened && !c.f[1].enables && tail_waits == 1,
          "planes: the callback leaves the next wait to process context");
    if (delayed_worker) {
        model_now_ns = (int64_t)(c.deadline + 1) * 1000000ll;
    }
    CHECK(drm_atomic_commit_work() == 0 && c.f[1].base.listened && c.f[1].enables == 1 &&
              c.f[1].base.cbs && tail_waits == 2 && tail_wait == &c.f[1].base && works &&
              works->deadline == (timeout_ms ? rtc_ticks_now() + MS_TO_TICKS(timeout_ms) + 1u : 0),
          "planes: the second pending fence gets its callback, boost and full timeout");
    if (timeout_ms && !delayed_worker) {
        model_now_ns = (int64_t)c.deadline * TICK_NS;
        CHECK(drm_atomic_commit_work() == 0 && nshown == c.shown,
              "planes: the first wait's deadline does not end the second wait");
    }
    plane_fence_signal(&c.f[1]);
    CHECK(drm_atomic_commit_work() == 1, "planes: the worker finishes after the second fence");
    plane_wait_end(&c);
}

/** Two independent engines finish in either order; only the current
 * plane is listened to, from process context, and timeout stops the waits. */
static void row_plane_fences(void)
{
    struct plane_wait_case c;

    mc->commit_fence_wait = count_tail_wait;
    plane_wait_begin(&c, 10000);
    c.f[1].done = 1;
    CHECK(drm_atomic_commit_work() == 0 && nshown == c.shown && !c.f[1].base.listened &&
              !c.f[1].base.signaled && !c.f[1].base.cbs,
          "planes: the second engine finishes first without listening or a flip");
    plane_fence_signal(&c.f[0]);
    CHECK(drm_atomic_commit_work() == 1 && c.f[1].base.signaled && !c.f[1].base.listened &&
              !c.f[1].enables && !c.f[1].base.cbs && tail_waits == 1,
          "planes: the completed second fence needs no callback or wait boost");
    plane_wait_end(&c);

    plane_wait_next(10000, 0);
    plane_wait_next(10000, 1);
    plane_wait_next(0, 0);
    plane_wait_timeout(1);
    plane_wait_timeout(0);

    plane_wait_begin(&c, 10000);
    c.f[1].complete_on_enable = 1;
    plane_fence_signal(&c.f[0]);
    CHECK(drm_atomic_commit_work() == 1 && c.f[1].base.signaled && c.f[1].enables == 1 &&
              !c.f[1].base.cbs && tail_waits == 1,
          "planes: completion during callback setup leaves no wait or boost");
    plane_wait_end(&c);
    mc->commit_fence_wait = NULL;
    mc->commit_fence_timeout_ms = 0;
}

/* The OUT_FENCE_PTR target of row_out_fence_fault. */
static int32_t *out_fence_target;

/** Before the copy of the descriptor: undo the -1 the property's copy
 * wrote, so that only the error path's write can leave -1. */
static void out_fence_spoil(void)
{
    *out_fence_target = 77;
}

/** Before the property's copy of -1: spoil what it writes at the next. */
static void out_fence_arm(void)
{
    copy_hook = out_fence_spoil;
}

/** An out-fence descriptor that cannot be written refuses the commit
 * before it takes effect, as Linux's setup_out_fence, and leaves no sync
 * file or descriptor and -1 where OUT_FENCE_PTR points. */
static void row_out_fence_fault(void)
{
    struct req r;
    uint32_t before = primary[0].state->fb->base.id;
    int32_t out_fd = 77;
    int at = nshown, made = allocations;

    req_flip(&r, 0, before == fb_a ? fb_b : fb_a);
    req_add(&r, &crtc[0].base, mc->prop_out_fence_ptr, UPTR(&out_fd));
    out_fence_target = &out_fd;
    copy_hook = out_fence_arm;
    copy_fault_after = 1; /* the -1 of the property passes; the descriptor faults */
    CHECK(req_commit(&r, 0, 0) == -EFAULT, "an out-fence that cannot be written: EFAULT");
    copy_fault_after = -1;
    ticks(2);
    CHECK(primary[0].state->fb->base.id == before && nshown == at, "and no flip");
    copy_hook = NULL;
    CHECK(out_fd == -1 && allocations == made, "-1 written, and no sync file left");
}

/* ---- 76-5b.4: connector forcing, the EDID override and CRCs ---- */

/** Force a connector off and back, as debugfs's `force` does. */
static void row_force(void)
{
    int calls = force_calls;

    CHECK(drm_connector_force_write(&conn[1], "off\n", 4) == 4 && conn[1].force == DRM_FORCE_OFF,
          "force off");
    CHECK(drm_helper_probe_single_connector_modes(&conn[1], 0, 0) == 0 &&
              conn[1].status == DRM_CONNECTOR_DISCONNECTED && !conn[1].modes && !conn[1].edid &&
              force_calls == calls + 1,
          "the probe finds it disconnected and tells the driver");
    CHECK(drm_connector_force_write(&conn[1], " on ", 4) == -EINVAL &&
              drm_connector_force_write(&conn[1], "on\n\n", 4) == -EINVAL,
          "spaces or two newlines about the word: EINVAL, as sysfs_streq");
    CHECK(drm_connector_force_write(&conn[1], "on", 2) == 2 && conn[1].force == DRM_FORCE_ON,
          "force on");
    CHECK(drm_helper_probe_single_connector_modes(&conn[1], 0, 0) == 1 &&
              conn[1].status == DRM_CONNECTOR_CONNECTED && conn[1].edid,
          "connected again with its modes");
    CHECK(drm_connector_force_write(&conn[1], "digital", 7) == 7 &&
              conn[1].force == DRM_FORCE_ON_DIGITAL,
          "force on, digital");
    CHECK(drm_connector_force_write(&conn[1], "detect", 6) == -EINVAL &&
              drm_connector_force_write(&conn[1], "unspecified ", 12) == -EINVAL,
          "an unknown word or one past eleven bytes: EINVAL");
    CHECK(drm_connector_force_write(&conn[1], "unspecified", 11) == 11 &&
              conn[1].force == DRM_FORCE_UNSPECIFIED,
          "back to detection");
    CHECK(drm_helper_probe_single_connector_modes(&conn[1], 0, 0) == 1 && force_calls == calls + 2,
          "the probe detects again");
}

/** The EDID override stands in for a sink with none. */
static void row_edid_override(void)
{
    uint8_t bad[128];

    conn1_no_edid = 1;
    CHECK(drm_helper_probe_single_connector_modes(&conn[1], 0, 0) > 1 && !conn[1].edid,
          "without an EDID the DMT modes");
    k_memcpy(bad, panel_edid, sizeof(bad));
    bad[127] ^= 1;
    CHECK(drm_edid_override_set(&conn[1], bad, sizeof(bad)) == -EINVAL &&
              drm_edid_override_set(&conn[1], panel_edid, 100) == -EINVAL && !conn[1].edid_override,
          "a bad checksum or a short EDID: EINVAL");
    CHECK(drm_edid_override_set(&conn[1], panel_edid, sizeof(panel_edid)) == 0, "the override");
    CHECK(drm_helper_probe_single_connector_modes(&conn[1], 0, 0) == 1 && conn[1].edid &&
              conn[1].modes->info.hdisplay == 1366 && conn[1].mm_width == 340,
          "the probe takes the override's mode, EDID and size");
    drm_edid_override_reset(&conn[1]);
    CHECK(drm_helper_probe_single_connector_modes(&conn[1], 0, 0) > 1 && !conn[1].edid,
          "reset: the DMT modes again");
    conn1_no_edid = 0;
    CHECK(drm_helper_probe_single_connector_modes(&conn[1], 0, 0) == 1, "the sink's own EDID");
}

/** A CRC entry of CRTC 0 without a frame counter, values 5 and 6: the
 * driver's answer. */
static int32_t add_frameless(void)
{
    static const uint32_t v[2] = {5, 6};

    return drm_crtc_add_crc_entry(&crtc[0], 0, 0, v);
}

/** crc/control, crc/data's open, lines, overflow and release. */
static void row_crc(void)
{
    char line[64];
    int32_t n;
    int i;

    CHECK(drm_crtc_crc_set_source(&crtc[1], "auto", 4) == -EINVAL, "a CRTC without CRCs: EINVAL");
    CHECK(k_strlen(crtc[0].crc.source) == 4 && memcmp(crtc[0].crc.source, "auto", 4) == 0,
          "the source is auto at first");
    CHECK(drm_crtc_crc_set_source(&crtc[0], "bogus\n", 6) == -EINVAL, "an unknown source: EINVAL");
    CHECK(drm_crtc_crc_set_source(&crtc[0], "plane1\n", 7) == 7 &&
              k_strlen(crtc[0].crc.source) == 6,
          "plane1, its newline dropped");
    CHECK(drm_crtc_crc_set_source(&crtc[0], "", 0) == 0, "an empty write changes nothing");
    CHECK(drm_crtc_crc_open(&crtc[0]) == 0 && crc_running == crtc[0].crc.source &&
              crtc[0].crc.values == 2,
          "crc/data's open starts plane1");
    CHECK(drm_crtc_crc_open(&crtc[0]) == -EBUSY, "a second open: EBUSY");
    CHECK(drm_crtc_crc_set_source(&crtc[0], "auto", 4) == -EBUSY, "a source while open: EBUSY");
    CHECK(drm_crtc_crc_read(&crtc[0], line, sizeof(line), 1) == -EAGAIN &&
              !drm_crtc_crc_ready(&crtc[0]),
          "no entry: EAGAIN");
    tick();
    CHECK(drm_crtc_crc_ready(&crtc[0]), "an entry at the frame");
    CHECK(drm_crtc_crc_read(&crtc[0], line, 33, 1) == -EINVAL, "a buffer short of a line: EINVAL");
    n = drm_crtc_crc_read(&crtc[0], line, sizeof(line), 1);
    CHECK(n == 34 && memcmp(line, "0x00000001 0x0000abc0 0x0000def0\n", 34) == 0,
          "the line: frame, two values, newline and NUL");
    CHECK(add_frameless() == 0, "an entry without a frame counter");
    n = drm_crtc_crc_read(&crtc[0], line, sizeof(line), 0);
    CHECK(n == 34 && memcmp(line, "XXXXXXXXXX 0x00000005 0x00000006\n", 34) == 0,
          "ten X's for no frame counter");
    for (i = 0; i < DRM_CRC_ENTRIES_NR; i++) {
        tick();
    }
    CHECK(crtc[0].crc.overflow && add_frameless() == -ENOBUFS,
          "past 127 waiting entries one is dropped");
    drm_crtc_crc_release(&crtc[0]);
    CHECK(!crc_running && !crtc[0].crc.entries && !crtc[0].crc.opened, "release stops the source");
    tick();
    CHECK(drm_crtc_crc_read(&crtc[0], line, sizeof(line), 1) == -EAGAIN,
          "nothing comes after the release");
}

/* ---- 80-86: a vblank's time never goes back ---- */

/* CRTC 1's frame counter and scanout position while row_stamps gives it
 * hooks: the counter, its value when the row began, the time the vertical
 * blank of that frame ends, the readings of the time so far, a vblank that
 * begins between the counter's read and the next reading of the time, and
 * a driver's time two frames back. */
static uint32_t hw_frame, hw_frame0;
static int64_t hw_stamp0;
static int hw_readings, hw_vblank_between, hw_back;

/** CRTC 1's frame counter. */
static uint32_t stamp_counter(struct drm_crtc *c)
{
    (void)c;
    return hw_frame;
}

/**
 * The time CRTC 1's latest vertical blank ended, from where it scans. Each
 * reading comes out a microsecond earlier than the one before it, within
 * the line of some 20 us by which i915's scanline counter places it, so a
 * second reading of a frame lies before the first.
 */
static int stamp_time(struct drm_crtc *c, int64_t *time_ns, int in_vblank_irq)
{
    (void)c;
    (void)in_vblank_irq;
    if (hw_vblank_between) {
        hw_vblank_between = 0;
        hw_frame++;
    }
    hw_readings++;
    *time_ns = hw_stamp0 + (int64_t)(hw_frame - hw_frame0) * FRAME_NS - hw_readings * 1000ll -
               (hw_back ? 2 * FRAME_NS : 0);
    return 1;
}

/** CRTC 1's count and the time of its last vblank in microseconds, as
 * WAIT_VBLANK's query answers them, the query Weston starts its repaint
 * with. */
static int64_t query1(uint32_t *seq)
{
    union drm_wait_vblank w;

    CHECK(wait_vblank(&w, _DRM_VBLANK_RELATIVE | _DRM_VBLANK_SECONDARY, 0, 0) == 0,
          "WAIT_VBLANK's query on CRTC 1");
    *seq = w.reply.sequence;
    return (int64_t)w.reply.tval_sec * 1000000 + w.reply.tval_usec;
}

/**
 * Read dri/fake/crtc-`i`/vblank_backwards as a read(2) of it does, through
 * debugfs's lookup of each name, its open and its read, into `buf` with
 * its NUL: the bytes read, or -1 when a name is not there.
 */
static int32_t read_backwards(uint32_t i, char *buf, uint32_t cap)
{
    const char *names[4] = {"dri", "fake", NULL, "vblank_backwards"};
    char crtc_name[8] = "crtc-0";
    struct open_file ofd;
    fd_entry_t e;
    void *n = NULL, *up[4];
    int32_t got = 0, rc = 0;
    int depth, found;

    crtc_name[5] = (char)('0' + i);
    names[2] = crtc_name;
    for (depth = 0; depth < 4; depth++) {
        if (dfs_lookup(NULL, n, names[depth], &up[depth]) != 0) {
            break;
        }
        n = up[depth];
    }
    found = depth == 4;
    if (found) {
        k_memset(&ofd, 0, sizeof(ofd));
        k_memset(&e, 0, sizeof(e));
        e.type = FD_TYPE_FILE;
        e.ofd = &ofd;
        if (debugfs_bind(n, &e, 0) == 0) {
            while (rc >= 0 && (uint32_t)got + 1u < cap &&
                   (rc = e.ops->read(&e, buf + got, cap - 1u - (uint32_t)got)) > 0) {
                got += rc;
            }
            e.ops->close(&e);
        }
    }
    buf[got] = '\0';
    while (depth-- > 0) {
        dfs_put(NULL, up[depth]);
    }
    return !found || rc < 0 ? -1 : got;
}

/**
 * The times that events and replies carry never go back, as Weston asserts
 * in weston_output_finish_frame: an interrupt whose frame the count already
 * took in keeps the count's time, and a vblank that begins between the
 * frame counter's read and the time's is read again, at an interrupt and
 * when the CRTC comes on, as Linux 6.12.115's drm_update_vblank_count and
 * drm_reset_vblank_timestamp do; a driver's time before an earlier one is
 * counted in `backwards`, which debugfs's crtc-1/vblank_backwards gives.
 */
static void row_stamps(void)
{
    static const struct drm_crtc_funcs stamp_funcs = {NULL, NULL,          NULL,       NULL, NULL,
                                                      NULL, vblank_on,     vblank_off, NULL, NULL,
                                                      NULL, stamp_counter, stamp_time};
    struct drm_crtc *c = &crtc[1];
    uint32_t seq, seq2;
    int64_t t, t2;
    char text[16];

    k_memcpy(dev.unique, "fake", 5);
    drm_debugfs_init();
    drm_debugfs_register(&dev);
    CHECK(dev.debugfs, "the device's debugfs files are made");
    c->funcs = &stamp_funcs;
    hw_frame = hw_frame0 = c->vblank.last;
    hw_stamp0 = model_now_ns + FRAME_NS;
    CHECK(drm_crtc_vblank_get(c) == 0 && c->vblank.enabled && c->vblank.backwards == 0,
          "a hold keeps CRTC 1's vblanks on");
    hw_frame++;
    (void)drm_crtc_handle_vblank(c);
    t = query1(&seq);
    CHECK(c->vblank.time_ns == hw_stamp0 + FRAME_NS - hw_readings * 1000ll,
          "a vblank stamped from where the CRTC scans");
    (void)drm_crtc_handle_vblank(c);
    t2 = query1(&seq2);
    CHECK(seq2 == seq && t2 == t, "an interrupt of a frame already counted keeps its time");
    hw_vblank_between = 1;
    (void)drm_crtc_handle_vblank(c);
    t = query1(&seq);
    CHECK(seq == seq2 + 1 && t > t2,
          "a vblank begun between the counter's read and the time's is counted with its time");
    (void)drm_crtc_handle_vblank(c);
    t2 = query1(&seq2);
    CHECK(seq2 == seq && t2 == t, "and its own interrupt keeps that time");
    drm_crtc_vblank_off(c);
    hw_vblank_between = 1;
    drm_crtc_vblank_on(c);
    t = query1(&seq);
    CHECK(seq == seq2 + 1 && t > t2 && c->vblank.last == hw_frame,
          "a CRTC turned on as a vblank begins takes in the counter of its time");
    (void)drm_crtc_handle_vblank(c);
    t2 = query1(&seq2);
    CHECK(seq2 == seq && t2 == t, "and that vblank's interrupt keeps it");
    CHECK(c->vblank.backwards == 0, "no time went back");
    CHECK(read_backwards(1, text, sizeof(text)) == 2 && memcmp(text, "0\n", 3) == 0,
          "crtc-1/vblank_backwards reads 0");
    hw_frame++;
    hw_back = 1;
    (void)drm_crtc_handle_vblank(c);
    CHECK(c->vblank.backwards == 1, "a driver's time before an earlier one is counted");
    CHECK(read_backwards(1, text, sizeof(text)) == 2 && memcmp(text, "1\n", 3) == 0,
          "and crtc-1/vblank_backwards reads 1");
    CHECK(read_backwards(0, text, sizeof(text)) == 2 && memcmp(text, "0\n", 3) == 0,
          "while crtc-0's reads 0");
    hw_back = 0;
    drm_crtc_vblank_put(c);
    c->funcs = &crtc1_funcs;
    drm_debugfs_unregister(&dev);
    debugfs_remove("dri");
    CHECK(read_backwards(1, text, sizeof(text)) == -1 && dev.refs == 1,
          "the files go with the device's unregistration, and their holds on it");
}

/** An unplug turns the CRTCs off: what was queued is sent, its fences
 * with -ENODEV. */
static void row_unplug(void)
{
    union drm_wait_vblank w;
    int32_t out_fd = 77;
    struct req r;
    int base = nsent;

    CHECK(wait_vblank(&w, _DRM_VBLANK_RELATIVE | _DRM_VBLANK_EVENT, 9, 51) == 0,
          "a VBLANK event queued");
    req_flip(&r, 0, fb_a);
    req_add(&r, &crtc[0].base, mc->prop_out_fence_ptr, UPTR(&out_fd));
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_NONBLOCK, 0) == 0 && fd_status(out_fd) == 0,
          "a flip with an out-fence armed");
    drm_vblank_unplug(&dev);
    CHECK(nsent == base + 1 && sent[base].user_data == 51, "the queued event sent");
    CHECK(fd_status(out_fd) == -ENODEV && !crtc[0].flip_pending, "the out-fence signals ENODEV");
    CHECK(!timer_on[0] && !timer_on[1] && crtc[0].vblank.inmodeset, "every interrupt off");
    fd_free(&table, out_fd);
}

/** Run the rows. */
int main(void)
{
    int base;

    model_start("drm-vblank-test");
    base = allocations;
    device_init();
    row_modeset();
    row_offdelay();
    row_wait();
    row_get_sequence();
    row_events();
    row_off();
    row_fences();
    row_queued_off();
    row_fence_timeout();
    row_plane_fences();
    row_out_fence_fault();
    row_force();
    row_edid_override();
    row_crc();
    row_stamps();
    row_unplug();
    drm_mode_file_release(&fa);
    CHECK(reserved == 0 && !mc->fbs, "the file's release leaves no event or framebuffer");
    drm_mode_config_cleanup(&dev);
    CHECK(allocations == base, "every allocation freed");
    CHECK(dev.refs == 1, "no hold on the device left");
    if (failures) {
        return 1;
    }
    printf("drm-vblank-test: ok\n");
    return 0;
}
