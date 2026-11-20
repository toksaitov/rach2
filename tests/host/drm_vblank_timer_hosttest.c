/*
 * drm_vblank_timer_hosttest.c: the vblank timer of a CRTC without a vblank
 * interrupt, rach2/sys/drm/drm_vblank.c's drm_crtc_vblank_start_timer and
 * the drm_crtc_vblank_helper_*_timer hooks that fbdrm and bochs give their
 * CRTCs, as Linux's drm_vblank_helper.c gives them to bochs and its
 * simple drivers.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/resv_host.c
 * host-deps: tests/host/compat/mutex_none.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * The fake driver has one CRTC whose vblank hooks are the core's timer
 * helpers, a primary plane, and a connector with the Lenovo panel's EDID.
 * Nothing runs the timer but the test: a wait that would sleep moves the
 * model's clock to each deadline the timer asks for and runs
 * drm_vblank_timer_tick, as the boot processor's tick does. The rows: a
 * modeset whose timer first fires a frame of the mode after it starts and
 * asks for that deadline, and whose blocking commit returns with its event
 * stamped there; a flip in mid-frame whose event comes at the next frame of
 * the grid, and a nonblocking flip after one still pending refused with
 * EBUSY until that vblank; a tick that comes three frames late, which
 * counts the three in one vblank and stamps the last of them, on the grid,
 * and moves the expiry past the clock by whole frames; a tick before the
 * expiry, which counts none; WAIT_VBLANK; vscan and double scan, each
 * alone and together, in the counts and timestamps of a flip event;
 * the timer stopped by the off-delay from inside its own tick,
 * and a timer turned on again, whose first vblank is a frame from then;
 * the CRTC off, which stops the timer; and a device's release with the
 * timer running, which takes it off the tick's chain. Every allocation is
 * given back.
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

static struct drm_device dev;
static struct drm_plane primary;
static struct drm_crtc crtc;
static struct drm_encoder enc;
static struct drm_connector conn;
static struct drm_file fa;
static struct drm_mode_config *mc = &dev.mode_config;

/* A frame of the panel's mode, in nanoseconds, and the timer's deadline
 * and state when the current wait first slept. */
static int64_t frame_ns, first_wake_ns;
static int first_on, sleeps;

/* ---- the fake driver ---- */

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

/** The connector is plugged in. */
static int detect(struct drm_connector *connector, int force_probe)
{
    (void)connector;
    (void)force_probe;
    return DRM_CONNECTOR_CONNECTED;
}

/** The panel's EDID. */
static int get_modes(struct drm_connector *connector)
{
    return drm_connector_update_edid(connector, panel_edid, sizeof(panel_edid));
}

static const struct drm_plane_funcs plane_funcs = {fake_plane_check, NULL, NULL};
static const struct drm_crtc_funcs crtc_funcs = {
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    drm_crtc_vblank_helper_enable_vblank_timer,
    drm_crtc_vblank_helper_disable_vblank_timer,
    NULL,
    NULL,
    NULL,
    NULL,
    drm_crtc_vblank_helper_get_vblank_timestamp_from_timer};
static const struct drm_connector_funcs conn_funcs = {detect, get_modes, NULL, NULL};
static const uint32_t xrgb[] = {DRM_FORMAT_XRGB8888};

/* ---- the clock ---- */

/** Move the clock to the timer's deadline, when that lies ahead, and run
 * the timer's tick, then the commit worker, as a thread woken meanwhile
 * would. */
static void tick(void)
{
    if (model_wake_at_ns > model_now_ns) {
        model_now_ns = model_wake_at_ns;
    }
    drm_vblank_timer_tick();
    (void)drm_atomic_commit_work();
}

/** A sleep: the timer's state noted on the first, then ticks until the
 * wait's condition holds, at most ten seconds of frames. */
static void wait_hook(wq_cond_fn cond, void *arg)
{
    int i;

    if (sleeps++ == 0) {
        first_wake_ns = model_wake_at_ns;
        first_on = crtc.vblank.timer_on;
    }
    for (i = 0; i < 600 && !cond(arg); i++) {
        tick();
    }
}

/** Make the fake device and open the master's file. */
static void device_init(void)
{
    static struct drm_driver driver;

    driver.ident.name = "fake";
    dev.driver = &driver;
    dev.features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC;
    dev.refs = 1;
    CHECK(drm_mode_config_init(&dev) == 0, "the mode configuration is made");
    mc->min_width = mc->min_height = 1;
    mc->max_width = mc->max_height = 4096;
    CHECK(drm_universal_plane_init(&dev, &primary, 1, &plane_funcs, xrgb, 1, NULL,
                                   DRM_PLANE_TYPE_PRIMARY) == 0,
          "the primary plane is made");
    CHECK(drm_crtc_init_with_planes(&dev, &crtc, &primary, NULL, &crtc_funcs) == 0,
          "the CRTC is made");
    CHECK(drm_encoder_init(&dev, &enc, DRM_MODE_ENCODER_TMDS) == 0, "the encoder is made");
    enc.possible_crtcs = 1;
    CHECK(drm_connector_init(&dev, &conn, &conn_funcs, DRM_MODE_CONNECTOR_eDP) == 0,
          "the connector is made");
    drm_connector_attach_encoder(&conn, &enc);
    CHECK(drm_helper_probe_single_connector_modes(&conn, 0, 0) == 1, "the panel's one mode");
    file_open(&fa, &dev);
    fa.atomic = fa.universal_planes = 1;
    dev.master = &fa;
    fa.is_master = 1;
    model_wait = wait_hook;
    frame_ns = vblank_framedur(&conn.modes->info);
}

/* ---- requests ---- */

/* An atomic request being built: objects, each with its properties. */
struct req {
    uint32_t objs[4], counts[4], props[16];
    uint64_t values[16];
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

/** ATOMIC of `r` with `flags` and `user_data`; each wait it makes is a new
 * sleep. */
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
    sleeps = 0;
    return drm_mode_atomic_ioctl(&dev, &a, &fa);
}

/** A request that shows `fb_id` on the primary plane, alone. */
static void req_flip(struct req *r, uint32_t fb_id)
{
    k_memset(r, 0, sizeof(*r));
    req_add(r, &primary.base, mc->prop_fb_id, fb_id);
}

/** A request that lights the CRTC from `fb_id` with the mode blob `mode`,
 * or turns it off for 0. */
static void req_mode(struct req *r, uint32_t mode, uint32_t fb_id)
{
    k_memset(r, 0, sizeof(*r));
    req_add(r, &crtc.base, mc->prop_mode_id, mode);
    req_add(r, &crtc.base, mc->prop_active, mode != 0);
    req_add(r, &conn.base, mc->prop_crtc_id, mode ? crtc.base.id : 0);
    req_add(r, &primary.base, mc->prop_fb_id, fb_id);
    req_add(r, &primary.base, mc->prop_crtc_id, mode ? crtc.base.id : 0);
    req_add(r, &primary.base, mc->prop_src_w, mode ? 1366u << 16 : 0);
    req_add(r, &primary.base, mc->prop_src_h, mode ? 768u << 16 : 0);
    req_add(r, &primary.base, mc->prop_crtc_w, mode ? 1366 : 0);
    req_add(r, &primary.base, mc->prop_crtc_h, mode ? 768 : 0);
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

/** Whether sent event `i` is stamped `ns`, to the microsecond. */
static int stamped(int i, int64_t ns)
{
    return sent[i].tv_sec == (uint32_t)(ns / 1000000000) &&
           sent[i].tv_usec == (uint32_t)(ns % 1000000000 / 1000);
}

/** The CRTC's count, and the time of its latest vblank in `*t`. */
static uint64_t count(int64_t *t)
{
    return drm_crtc_vblank_count_and_time(&crtc, t);
}

static uint32_t mode_id, fb_a, fb_b;
/* When the timer last started: its frames lie on this time's grid. */
static int64_t grid_ns;

/* ---- the rows ---- */

/** Light the CRTC: the timer starts with it and fires a frame later. */
static void row_start(void)
{
    struct drm_mode_create_blob b;
    struct req r;
    int64_t t;

    k_memset(&b, 0, sizeof(b));
    b.data = UPTR(&conn.modes->info);
    b.length = sizeof(conn.modes->info);
    CHECK(drm_mode_createblob_ioctl(&dev, &b, &fa) == 0, "a mode blob");
    mode_id = b.blob_id;
    fb_a = fb_new(0);
    fb_b = fb_new(1);
    CHECK(frame_ns > 16000000 && frame_ns < 17000000, "the panel's mode is 60 Hz");
    CHECK(drm_crtc_has_vblank(&crtc) && !crtc.vblank.timer_on && !vblank_timers,
          "a CRTC with the timer's hooks has vblanks, its timer off while it is");
    req_mode(&r, mode_id, fb_a);
    grid_ns = model_now_ns;
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT, 1) == 0,
          "the CRTC lit with an event");
    CHECK(sleeps == 1 && first_on && first_wake_ns == grid_ns + frame_ns,
          "the blocking commit sleeps on a timer that asks for a frame after its start");
    CHECK(crtc.vblank.timer_on && vblank_timers == &crtc && crtc.vblank.timer_period_ns == frame_ns,
          "the timer runs at the mode's frame, on the tick's chain");
    CHECK(model_now_ns == grid_ns + frame_ns && nsent == 1 && sent[0].user_data == 1 &&
              sent[0].base.type == DRM_EVENT_FLIP_COMPLETE && stamped(0, grid_ns + frame_ns),
          "its event comes at the first firing, stamped with it");
    CHECK(count(&t) == sent[0].sequence && t == grid_ns + frame_ns,
          "the count and time are the event's");
    CHECK(model_wake_at_ns == grid_ns + 2 * frame_ns &&
              crtc.vblank.timer_next_ns == model_wake_at_ns,
          "the tick asks for the next frame");
}

/** Flips in mid-frame: each event at the grid's next frame. */
static void row_flips(void)
{
    struct req r;
    uint64_t c = count(NULL);

    model_now_ns += frame_ns / 3;
    req_flip(&r, fb_b);
    CHECK(req_commit(&r, DRM_MODE_PAGE_FLIP_EVENT, 2) == 0, "a blocking flip in mid-frame");
    CHECK(nsent == 2 && sent[1].user_data == 2 && sent[1].sequence == c + 1 &&
              stamped(1, grid_ns + 2 * frame_ns) && model_now_ns == grid_ns + 2 * frame_ns,
          "its event comes at the next frame of the grid");
    model_now_ns += frame_ns / 2;
    req_flip(&r, fb_a);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, 3) == 0,
          "a nonblocking flip");
    (void)drm_atomic_commit_work();
    req_flip(&r, fb_b);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, 4) == -EBUSY &&
              nsent == 2 && crtc.flip_pending,
          "another before its vblank: EBUSY");
    tick();
    CHECK(nsent == 3 && sent[2].user_data == 3 && stamped(2, grid_ns + 3 * frame_ns) &&
              !crtc.flip_pending,
          "the timer's next firing ends the flip");
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, 4) == 0,
          "the next flip is taken after it");
    (void)drm_atomic_commit_work();
    tick();
    CHECK(nsent == 4 && sent[3].user_data == 4 && stamped(3, grid_ns + 4 * frame_ns),
          "and ends at the frame after");
}

/** A tick that is late and one that is early. */
static void row_late(void)
{
    uint64_t c;
    int64_t last, t;
    int w0;

    CHECK(drm_crtc_vblank_get(&crtc) == 0, "a hold keeps the timer on");
    c = count(&last);
    CHECK(last == grid_ns + 4 * frame_ns, "the last vblank is on the grid");
    model_now_ns = last + 3 * frame_ns + frame_ns / 2;
    w0 = wakes;
    drm_vblank_timer_tick();
    CHECK(count(&t) == c + 3 && t == last + 3 * frame_ns,
          "a tick three frames late counts three, stamped with the last");
    CHECK(wakes == w0 + 1, "in one vblank, as Linux's timer handles it once");
    CHECK(crtc.vblank.timer_next_ns == last + 4 * frame_ns &&
              model_wake_at_ns == last + 4 * frame_ns,
          "and moves the expiry past the clock by whole frames");
    CHECK(crtc.vblank.backwards == 0, "no time went back");
    drm_vblank_timer_tick();
    CHECK(count(&t) == c + 3 && t == last + 3 * frame_ns, "a tick before the expiry counts none");
    drm_crtc_vblank_put(&crtc);
}

/** WAIT_VBLANK sleeps until the timer's vblank. */
static void row_wait(void)
{
    union drm_wait_vblank w;
    uint64_t c = count(NULL);
    int64_t next = crtc.vblank.timer_next_ns;

    k_memset(&w, 0, sizeof(w));
    w.request.type = _DRM_VBLANK_RELATIVE;
    w.request.sequence = 1;
    sleeps = 0;
    CHECK(drm_wait_vblank_ioctl(&dev, &w, &fa) == 0, "WAIT_VBLANK of the next vblank");
    CHECK(w.reply.sequence == (uint32_t)(c + 1) &&
              w.reply.tval_sec == (intptr_t)(next / 1000000000) &&
              w.reply.tval_usec == (intptr_t)(next % 1000000000 / 1000),
          "it answers with the timer's next vblank");
    CHECK((next - grid_ns) % frame_ns == 0, "which is on the grid");
}

/** The off-delay stops the timer from inside its own tick; a hold after
 * that starts it again, a frame from then. */
static void row_offdelay(void)
{
    union drm_wait_vblank w;
    int64_t put = crtc.vblank.put_ns, again;
    int i;

    CHECK(crtc.vblank.refs == 0 && crtc.vblank.enabled, "no holder, the timer on");
    for (i = 0; i < 400 && crtc.vblank.enabled; i++) {
        tick();
    }
    CHECK(!crtc.vblank.enabled && !crtc.vblank.timer_on && !vblank_timers &&
              !crtc.vblank.timer_busy,
          "the off-delay stops the timer and takes it off the chain");
    CHECK(model_now_ns - put >= 5000000000ll && model_now_ns - put < 5000000000ll + frame_ns,
          "five seconds after the last holder");
    model_now_ns += frame_ns / 4;
    again = model_now_ns;
    k_memset(&w, 0, sizeof(w));
    w.request.type = _DRM_VBLANK_RELATIVE;
    w.request.sequence = 1;
    sleeps = 0;
    CHECK(drm_wait_vblank_ioctl(&dev, &w, &fa) == 0 && sleeps == 1 && first_on &&
              first_wake_ns == again + frame_ns,
          "a wait starts the timer again, a frame from then");
    CHECK(w.reply.tval_sec == (intptr_t)((again + frame_ns) / 1000000000) &&
              w.reply.tval_usec == (intptr_t)((again + frame_ns) % 1000000000 / 1000),
          "and its vblank comes then");
    grid_ns = again;
}

/** The CRTC off stops the timer; a tick after counts nothing. */
static void row_off(void)
{
    struct req r;
    uint64_t c;

    req_mode(&r, 0, 0);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == 0, "the CRTC off");
    CHECK(!crtc.vblank.enabled && !crtc.vblank.timer_on && !vblank_timers,
          "its timer stops and leaves the chain");
    c = count(NULL);
    model_now_ns += 3 * frame_ns;
    drm_vblank_timer_tick();
    CHECK(count(NULL) == c, "a tick after counts nothing");
}

/** A flip in a repeated-scan mode advances the count at the mode's deadline.
 * The CRTC starts and ends off. `period` is the expected duration in ns.
 */
static void row_repeated_scan(uint16_t vscan, uint32_t flags, int64_t period)
{
    struct drm_mode_modeinfo mode = conn.modes->info;
    struct drm_mode_create_blob blob;
    struct drm_mode_destroy_blob destroy;
    struct req r;
    uint64_t c;
    int64_t t, start = model_now_ns;
    int event = nsent;

    mode.htotal = 1600;
    mode.vtotal = 800;
    mode.clock = 128000;
    mode.vscan = vscan;
    mode.flags |= flags;
    k_memset(&blob, 0, sizeof(blob));
    blob.data = UPTR(&mode);
    blob.length = sizeof(mode);
    CHECK(drm_mode_createblob_ioctl(&dev, &blob, &fa) == 0, "a repeated-scan mode blob");
    req_mode(&r, blob.blob_id, fb_a);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT, 5) == 0,
          "a repeated-scan modeset with an event");
    CHECK(nsent == event + 1 && stamped(event, start + period),
          "the modeset event uses the repeated-scan period");
    c = count(&t);
    CHECK(c == sent[event].sequence && t == start + period,
          "the first repeated-scan count and timestamp match the event");
    model_now_ns += period / 2;
    drm_vblank_timer_tick();
    CHECK(count(NULL) == c, "no extra vblank between repeated scans");
    req_flip(&r, fb_b);
    CHECK(req_commit(&r, DRM_MODE_PAGE_FLIP_EVENT, 6) == 0, "a flip in the repeated-scan mode");
    CHECK(nsent == event + 2 && sent[event + 1].sequence == c + 1 &&
              stamped(event + 1, start + 2 * period),
          "the flip event counts one vblank at the next repeated-scan deadline");
    req_mode(&r, 0, 0);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == 0, "the repeated-scan mode off");
    destroy.blob_id = blob.blob_id;
    CHECK(drm_mode_destroyblob_ioctl(&dev, &destroy, &fa) == 0, "the mode blob released");
}

/** The device let go with the timer running. */
static void row_release(void)
{
    struct req r;

    req_mode(&r, mode_id, fb_a);
    CHECK(req_commit(&r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == 0, "the CRTC lit again");
    CHECK(drm_crtc_vblank_get(&crtc) == 0 && crtc.vblank.timer_on && vblank_timers == &crtc,
          "a hold keeps its timer on");
    drm_crtc_vblank_put(&crtc);
    drm_mode_file_release(&fa);
    drm_mode_config_cleanup(&dev);
    CHECK(!crtc.vblank.timer_on && !vblank_timers && !crtc.vblank.timer_busy,
          "the release takes the running timer off the chain");
    model_wake_at_ns = 0;
    drm_vblank_timer_tick();
    CHECK(model_wake_at_ns == 0, "the tick publishes no deadline with no running timer");
}

/** Run the rows. */
int main(void)
{
    int base;

    model_start("drm-vblank-timer-test");
    base = allocations;
    device_init();
    row_start();
    row_flips();
    row_late();
    row_wait();
    row_offdelay();
    row_off();
    row_repeated_scan(3, 0, 30000000);
    row_repeated_scan(1, DRM_MODE_FLAG_DBLSCAN, 20000000);
    row_repeated_scan(3, DRM_MODE_FLAG_DBLSCAN, 60000000);
    row_release();
    CHECK(reserved == 0, "no event left reserved");
    CHECK(allocations == base, "every allocation freed");
    if (failures) {
        return 1;
    }
    printf("drm-vblank-timer-test: ok\n");
    return 0;
}
