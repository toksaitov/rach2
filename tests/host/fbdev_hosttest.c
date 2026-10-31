/**
 * fbdev_hosttest.c: the /fb device's operations against a model display: the
 * requests' arguments, the bounds of a write's offset in each mode, and the
 * owner's check, which a child that inherited the descriptor fails; nested
 * owners, their events and restoration across exec and signal teardown.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * `display/fbdev.c` is included whole. A user pointer is an offset into the
 * array `user`, which every copy checks; the display is two arrays, mode
 * 13h's picture and a native screen of 64 by 8 pixels at 32 bits, whose mode
 * changes the model counts.
 */
#include "display/fbdev.c"

extern int printf(const char *format, ...);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);
extern int memcmp(const void *a, const void *b, unsigned long n);

/* The calling process; its thread group id is the one the owner is by. */
static process_t proc;
struct percpu cpu_table[NR_CPUS] = {{.current = &proc}};

gfx_info_t gfx_current;

#define NATIVE_PITCH  256u /* 64 pixels of 4 bytes */
#define NATIVE_HEIGHT 8u
#define NATIVE_SIZE   (NATIVE_PITCH * NATIVE_HEIGHT)

static uint8_t picture[64000];
static uint8_t native[NATIVE_SIZE];
static uint8_t user[16 + 70000 + 64];
static int failures, enters, natives, exits, palettes, presents, unblanks, locked;
static int text_console; /* 1: the console is on the VGA's text mode */
static int indexed;      /* 1: the framebuffer is indexed */
static int allocations, fail_alloc;
static int copies, leave_at; /* copies from the caller; the one that leaves the mode */

#define CHECK(c, s)                              \
    do {                                         \
        if (!(c)) {                              \
            printf("fbdev-test: fail: %s\n", s); \
            failures++;                          \
        }                                        \
    } while (0)

/* The keyboard state is modelled here; kd_hosttest checks its real save,
 * restore, LED, repeat, termios and input-flush operations. */
static struct kd_state keyboard;
static int restores;
static uint8_t palette_seen[768];
static process_t relatives[4];

/** Save every keyboard and terminal field of the model. */
void kd_state_save(struct kd_state *st)
{
    *st = keyboard;
}

/** Restore the model's settings and count the input flush it entails. */
void kd_state_restore(const struct kd_state *st)
{
    keyboard = *st;
    restores++;
}

/** Find one ancestor in the model's process table. */
process_t *get_process(uint32_t pid)
{
    unsigned int i;

    for (i = 0; i < 4; i++) {
        if (relatives[i].pid == pid) {
            return &relatives[i];
        }
    }
    return NULL;
}

/** Start an empty event queue. */
void wait_queue_init(wait_queue_t *wq)
{
    k_memset(wq, 0, sizeof(*wq));
}

/** Count the kinds of readiness change. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    if (kinds & WQ_DATA) {
        wq->wakes[0]++;
    }
    if (kinds & WQ_ROOM) {
        wq->wakes[1]++;
    }
}

/** Count a hangup wake. */
void wq_wake_all(wait_queue_t *wq)
{
    wq_wake(wq, WQ_DATA | WQ_ROOM);
}

/** An empty blocking read receives a signal in this model. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    return cond(arg) ? 0 : -EINTR;
}

/** A block of the heap, zeroed. */
void *kmalloc(size_t size)
{
    if (++allocations == fail_alloc) {
        return NULL;
    }
    return calloc(1, size);
}

/** Free a block from `kmalloc`. */
void kfree(void *p)
{
    free(p);
}

/** Whether `len` bytes at offset `at` lie inside the user memory. */
static int in_user(uint32_t at, size_t len)
{
    return at != 0 && at <= sizeof(user) && len <= sizeof(user) - at;
}

/** Copy `len` bytes to the offset `dst` of user memory; 0, or -EFAULT
 * outside it. */
int copy_to_user(void __user *dst, const void *src, size_t len)
{
    uint32_t at = (uint32_t)(unsigned long)dst;

    if (!in_user(at, len)) {
        return -EFAULT;
    }
    k_memcpy(user + at, src, len);
    return 0;
}

/** Copy `len` bytes from the offset `src` of user memory; 0, or -EFAULT
 * outside it. The copy numbered `leave_at` gives the display up first, as
 * another thread of the owner would while this one sleeps in a fault. */
int copy_from_user(void *dst, const void __user *src, size_t len)
{
    uint32_t at = (uint32_t)(unsigned long)src;

    if (++copies == leave_at) {
        fb_release_if_owner(current_proc->tgid);
    }
    if (!in_user(at, len)) {
        return -EFAULT;
    }
    k_memcpy(dst, user + at, len);
    return 0;
}

/** Take the console's lock, which the model counts. */
uint32_t console_lock(void)
{
    locked++;
    return 0;
}

/** Give back the console's lock. */
void console_unlock(uint32_t flags)
{
    (void)flags;
    locked--;
}

/** Count an unblank, which a take of the display makes. */
void console_unblank_now(void)
{
    unblanks++;
}

/** Hold the description, which no other caller shares here. */
void fd_ofd_lock(struct open_file *ofd)
{
    (void)ofd;
}

/** Let the description go. */
void fd_ofd_unlock(struct open_file *ofd)
{
    (void)ofd;
}

/** Enter mode 13h under the console's lock and count it. */
uint8_t *fb_enter(void)
{
    CHECK(locked == 1, "mode 13h is entered under the console's lock");
    if (gfx_current.mode != GFX_MODE_VGA_13H) {
        enters++;
    }
    gfx_current.mode = GFX_MODE_VGA_13H;
    gfx_current.framebuffer = picture;
    gfx_current.size = sizeof(picture);
    gfx_current.pitch = 320;
    return picture;
}

/** Enter the native mode under the console's lock and count it; NULL on
 * the text console and on an indexed framebuffer. */
uint8_t *fb_enter_native(void)
{
    CHECK(locked == 1, "the native mode is entered under the console's lock");
    if (text_console || indexed) {
        return NULL;
    }
    if (gfx_current.mode != GFX_MODE_NATIVE) {
        natives++;
    }
    gfx_current.mode = GFX_MODE_NATIVE;
    gfx_current.framebuffer = native;
    gfx_current.size = NATIVE_SIZE;
    gfx_current.pitch = NATIVE_PITCH;
    return native;
}

/** The model's framebuffer, 64 by 8 pixels of 32 bits; -1 on the text
 * console. */
int fb_screen(struct fb_screen *s)
{
    if (text_console) {
        return -1;
    }
    k_memset(s, 0, sizeof(*s));
    s->width = 64;
    s->height = NATIVE_HEIGHT;
    s->pitch = NATIVE_PITCH;
    s->bpp = indexed ? 8 : 32;
    s->kind = indexed ? FB_KIND_INDEXED : FB_KIND_RGB;
    if (!indexed) {
        s->red.offset = 16;
        s->red.width = 8;
        s->green.offset = 8;
        s->green.width = 8;
        s->blue.width = 8;
    }
    return 0;
}

/** Leave either mode and count it. */
void fb_exit(void)
{
    if (gfx_current.mode != GFX_MODE_NONE) {
        exits++;
    }
    gfx_current.mode = GFX_MODE_NONE;
    gfx_current.framebuffer = NULL;
    gfx_current.size = 0;
    gfx_current.pitch = 0;
}

/** Count a palette, which must come with the console's lock held. */
void fb_set_palette(const uint8_t *data)
{
    k_memcpy(palette_seen, data, sizeof(palette_seen));
    CHECK(locked == 1, "the palette is loaded under the console's lock");
    palettes++;
}

/** Count a present of mode 13h's picture. */
void fb_present(void)
{
    presents++;
}

static struct open_file description;
static fd_entry_t entry;

/** Be the process `pid`, a thread of thread group `tgid`. */
static void be(uint32_t pid, uint32_t tgid)
{
    proc.pid = pid;
    proc.tgid = tgid;
}

/** Write `count` bytes of user memory at offset 16 through `e`, as write(2)
 * does. */
static int32_t put(fd_entry_t *e, uint32_t count)
{
    return fb_file_ops.write(e, (const char __user *)(uintptr_t)16, count);
}

/** Send `request` with `arg` through `e`. */
static int32_t ask(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    return fb_file_ops.ioctl(e, request, arg);
}

/**
 * The open takes the display at mode 13h, unblanked first, and a second open
 * by another process is -EBUSY. The requests: an unknown one is -ENOTTY; a
 * mode request from the mode it names changes nothing; FB_GETINFO reports
 * the screen whatever the mode, -EFAULT for a NULL or short buffer and
 * -ENODEV on the text console; FB_MODE_NATIVE is -ENODEV there and on an
 * indexed framebuffer; FB_SETPALETTE is -EFAULT for a NULL or short buffer
 * and is taken in either mode; FB_MODE_TEXT gives the display up.
 */
static void requests(void)
{
    struct fb_screen info;
    uint32_t at = sizeof(user) - sizeof(info);

    be(1, 1);
    CHECK(fb_take(&entry) == 0 && fb_owner_pid == 1 && enters == 1 && unblanks == 1 && locked == 0,
          "the open takes the display at mode 13h");
    be(2, 2);
    CHECK(fb_take(&entry) == -EBUSY && fb_owner_pid == 1, "a second process cannot take it");
    be(1, 1);
    CHECK(ask(&entry, 0x4600, 0) == -ENOTTY, "an unknown request is refused");
    CHECK(ask(&entry, FB_MODE_13H, 0) == 0 && enters == 1, "mode 13h again changes nothing");
    CHECK(ask(&entry, FB_GETINFO, 0) == -EFAULT, "FB_GETINFO into NULL is refused");
    CHECK(ask(&entry, FB_GETINFO, at + 1) == -EFAULT, "and into a buffer that is short");
    CHECK(ask(&entry, FB_GETINFO, at) == 0, "FB_GETINFO in mode 13h reports the screen");
    k_memcpy(&info, user + at, sizeof(info));
    CHECK(info.width == 64 && info.height == NATIVE_HEIGHT && info.pitch == NATIVE_PITCH &&
              info.bpp == 32 && info.kind == FB_KIND_RGB && info.red.offset == 16 &&
              info.green.width == 8,
          "its size, pitch, depth and channels");
    indexed = 1;
    CHECK(ask(&entry, FB_MODE_NATIVE, 0) == -ENODEV && gfx_current.mode == GFX_MODE_VGA_13H,
          "an indexed framebuffer has no native mode");
    indexed = 0;
    text_console = 1;
    CHECK(ask(&entry, FB_GETINFO, at) == -ENODEV, "the text console has no screen to report");
    CHECK(ask(&entry, FB_MODE_NATIVE, 0) == -ENODEV && natives == 0, "nor a native mode");
    text_console = 0;
    CHECK(ask(&entry, FB_MODE_NATIVE, 0) == 0 && natives == 1 &&
              gfx_current.mode == GFX_MODE_NATIVE,
          "the native mode is entered");
    CHECK(ask(&entry, FB_MODE_NATIVE, 0) == 0 && natives == 1, "and again changes nothing");
    CHECK(ask(&entry, FB_SETPALETTE, 0) == -EFAULT, "a NULL palette is refused");
    CHECK(ask(&entry, FB_SETPALETTE, sizeof(user) - 700) == -EFAULT,
          "and one shorter than 768 bytes");
    CHECK(ask(&entry, FB_SETPALETTE, 16) == 0 && palettes == 2,
          "a palette is taken at the native mode");
    CHECK(ask(&entry, FB_MODE_13H, 0) == 0 && enters == 2, "mode 13h follows the native mode");
    CHECK(ask(&entry, FB_MODE_TEXT, 0) == 0 && fb_owner_pid == 0 && exits == 1 &&
              gfx_current.mode == GFX_MODE_NONE,
          "FB_MODE_TEXT gives the display up");
    CHECK(ask(&entry, FB_MODE_NATIVE, 0) == -EBUSY && ask(&entry, FB_GETINFO, at) == -EBUSY,
          "a display nobody owns answers no request but FB_MODE_13H");
    CHECK(put(&entry, 1) == -EBUSY, "nor a write");
    be(2, 2);
    CHECK(ask(&entry, FB_MODE_13H, 0) == 0 && fb_owner_pid == 2 && enters == 3,
          "FB_MODE_13H takes a display nobody owns");
    CHECK(ask(&entry, FB_MODE_TEXT, 0) == 0 && fb_owner_pid == 0, "which is given up again");
    CHECK(locked == 0, "every request gives the console's lock back");
}

/**
 * A write's offset at the native mode, 2048 bytes of 8 rows of 256: a write
 * from the offset moves it, one that would pass the end stops there, one at
 * the end is -ENOSPC and one past it -EFBIG; pwrite's copy of the description
 * moves only the copy. A change of mode puts the offset back at 0, and a
 * fault on the caller's buffer before any byte is -EFAULT.
 */
static void native_offsets(void)
{
    struct open_file copy;
    fd_entry_t view;

    be(1, 1);
    CHECK(fb_take(&entry) == 0 && ask(&entry, FB_MODE_NATIVE, 0) == 0,
          "the native mode for writes");
    description.offset = 99;
    CHECK(ask(&entry, FB_MODE_NATIVE, 0) == 0 && description.offset == 99,
          "a request of the mode in force leaves the offset");
    CHECK(ask(&entry, FB_MODE_13H, 0) == 0 && description.offset == 0,
          "a change of mode puts the offset back at 0");
    description.offset = 99;
    CHECK(ask(&entry, FB_MODE_NATIVE, 0) == 0 && description.offset == 0, "either way");
    CHECK(put(&entry, NATIVE_PITCH) == (int32_t)NATIVE_PITCH &&
              description.offset == NATIVE_PITCH && native[0] == user[16] &&
              native[NATIVE_PITCH - 1] == user[16 + NATIVE_PITCH - 1],
          "a row written from the offset moves it a row");
    CHECK(put(&entry, 5000) == (int32_t)(NATIVE_SIZE - NATIVE_PITCH) &&
              description.offset == NATIVE_SIZE &&
              native[NATIVE_SIZE - 1] == user[16 + NATIVE_SIZE - NATIVE_PITCH - 1],
          "a write that would pass the end stops at it");
    CHECK(put(&entry, 1) == -ENOSPC && description.offset == NATIVE_SIZE,
          "a write at the end is -ENOSPC");
    description.offset = NATIVE_SIZE + 1;
    CHECK(put(&entry, 1) == -EFBIG && description.offset == NATIVE_SIZE + 1,
          "and one past it -EFBIG");
    copy = description;
    copy.offset = 3 * NATIVE_PITCH;
    view = entry;
    view.ofd = &copy;
    native[3 * NATIVE_PITCH] = 0;
    CHECK(put(&view, 4) == 4 && native[3 * NATIVE_PITCH] == user[16] &&
              copy.offset == 3 * NATIVE_PITCH + 4 && description.offset == NATIVE_SIZE + 1,
          "pwrite's copy of the description leaves the description alone");
    description.offset = 0;
    CHECK(fb_file_ops.write(&entry, (const char __user *)(uintptr_t)(sizeof(user) + 8), 4) ==
                  -EFAULT &&
              description.offset == 0,
          "a buffer outside the caller's memory is -EFAULT, the offset kept");
    CHECK(ask(&entry, FB_MODE_TEXT, 0) == 0 && locked == 0, "the native rows give the display up");
}

/**
 * Mode 13h's writes: the offset stays, so that each write from 0 is a whole
 * frame, which is presented; a longer write stops at the picture's end, and
 * one from a seek at the offset. A mode left by another thread while a write
 * is under way ends the write at the chunk before, and one left before the
 * first chunk is -EBUSY.
 */
static void frame_offsets(void)
{
    be(1, 1);
    CHECK(fb_take(&entry) == 0 && gfx_current.mode == GFX_MODE_VGA_13H, "mode 13h for writes");
    description.offset = 0;
    presents = 0;
    CHECK(put(&entry, 64000) == 64000 && description.offset == 0 && presents == 1 &&
              picture[63999] == user[16 + 63999],
          "a frame in mode 13h leaves the offset at 0 and is presented");
    CHECK(put(&entry, 70000) == 64000 && description.offset == 0, "a longer one stops at its end");
    description.offset = 63999;
    CHECK(put(&entry, 2) == 1 && description.offset == 63999 && picture[63999] == user[16],
          "and one from a seek at the offset");
    description.offset = 0;
    copies = 0;
    leave_at = 3;
    CHECK(put(&entry, 64000) == (int32_t)(2 * FB_CHUNK) && fb_owner_pid == 0,
          "a mode left meanwhile ends the write at the chunk before");
    CHECK(fb_take(&entry) == 0, "mode 13h again");
    copies = 0;
    leave_at = 1;
    CHECK(put(&entry, 64000) == -EBUSY && fb_owner_pid == 0, "and before the first chunk, -EBUSY");
    leave_at = 0;
    CHECK(locked == 0, "every write gives the console's lock back");
}

/** Model fork's shared open description, whose object is closed only after
 * both copies are gone. */
static fd_entry_t fork_view(fd_entry_t *e)
{
    e->ofd->refcount++;
    return *e;
}

/** Model the file layer's last-reference close of a descriptor. */
static void close_view(fd_entry_t *e)
{
    if (e->ofd->refcount == 1) {
        fb_file_ops.close(e);
    }
    e->ofd->refcount--;
    e->priv = NULL;
}

/** Model exec's close-on-exec pass over these descriptors. The framebuffer
 * receives only last closes, with no separate notification of exec. */
static void exec_views(fd_entry_t *a, fd_entry_t *b)
{
    if (a->flags & FD_FLAG_CLOEXEC) {
        close_view(a);
    }
    if (b && (b->flags & FD_FLAG_CLOEXEC)) {
        close_view(b);
    }
}

/**
 * The owner's check across fork: a child that inherited the descriptor, of
 * a thread group of its own, cannot write, ask or give the display up, not
 * even with FB_MODE_13H, and its close of the description leaves the
 * parent's mode; another thread of the owner writes; the owner's close gives
 * the display back.
 */
static void forks(void)
{
    fd_entry_t inherited;
    uint32_t at = sizeof(user) - sizeof(struct fb_screen);

    be(1, 1);
    CHECK(fb_take(&entry) == 0 && ask(&entry, FB_MODE_NATIVE, 0) == 0,
          "the parent owns the display");
    inherited = fork_view(&entry);
    be(5, 5);
    CHECK(put(&entry, 4) == -EBUSY, "the child cannot write");
    CHECK(ask(&entry, FB_MODE_TEXT, 0) == -EBUSY && ask(&entry, FB_MODE_13H, 0) == -EBUSY &&
              ask(&entry, FB_MODE_NATIVE, 0) == -EBUSY && ask(&entry, FB_GETINFO, at) == -EBUSY &&
              ask(&entry, FB_SETPALETTE, 16) == -EBUSY,
          "nor ask anything");
    close_view(&inherited);
    CHECK(fb_owner_pid == 1 && gfx_current.mode == GFX_MODE_NATIVE,
          "and its close leaves the parent's mode");
    be(7, 1);
    description.offset = 0;
    CHECK(put(&entry, 4) == 4, "another thread of the owner writes");
    be(1, 1);
    fb_file_ops.close(&entry);
    CHECK(fb_owner_pid == 0 && gfx_current.mode == GFX_MODE_NONE,
          "the owner's close gives the display back");
    be(5, 5);
    CHECK(put(&entry, 4) == -EBUSY, "after which the child still cannot write");
    CHECK(locked == 0, "every close gives the console's lock back");
}

/** Read a queued event from `e` through the device operations. */
static unsigned int event_from(fd_entry_t *e)
{
    struct fb_event event;

    CHECK(fb_file_ops.read(e, (char __user *)(uintptr_t)16, sizeof(event)) == sizeof(event),
          "one whole event is read");
    k_memcpy(&event, user + 16, sizeof(event));
    return event.type;
}

/** Set up a separate open description for the current process. */
static void open_view(fd_entry_t *e, struct open_file *ofd)
{
    k_memset(e, 0, sizeof(*e));
    k_memset(ofd, 0, sizeof(*ofd));
    e->ofd = ofd;
    ofd->refcount = 1;
    e->ops = &fb_file_ops;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    CHECK(fb_take(e) == 0, "a descendant opens its own description");
}

/**
 * Descendants take the screen through their own opens, preserve ownership
 * across exec, and restore the suspended owner's mode, palette and whole
 * keyboard state on close. A dead suspended owner is removed without
 * changing the child, including when the bottom owner dies first. The last
 * owner's signal teardown restores the console's settings and flushes its
 * input. Separate opens and inherited descriptions do not release early.
 */
static void owners(void)
{
    fd_entry_t child, grandchild, second, inherited;
    struct open_file child_ofd, grandchild_ofd, second_ofd;
    struct kd_state original, parent_state;
    struct wait_queue *wq;
    int before;

    be(10, 10);
    proc.parent_pid = 0;
    k_memset(&keyboard, 0x35, sizeof(keyboard));
    original = keyboard;
    CHECK(fb_take(&entry) == 0 && ask(&entry, FB_MODE_NATIVE, 0) == 0,
          "parent takes the native screen");
    k_memset(&keyboard, 0x62, sizeof(keyboard));
    parent_state = keyboard;
    open_view(&second, &second_ofd);
    fb_file_ops.close(&second);
    CHECK(fb_owner_pid == 10, "closing one of two opens leaves the owner");
    CHECK(fb_file_ops.poll(&entry, POLLIN | POLLOUT, &wq) == POLLOUT && wq != NULL,
          "active owner is writable and names its event queue");
    description.status_flags = OFD_NONBLOCK;
    CHECK(fb_file_ops.read(&entry, (char __user *)(uintptr_t)16, 4) == -EAGAIN,
          "empty nonblocking read is EAGAIN");
    description.status_flags = 0;
    CHECK(fb_file_ops.read(&entry, (char __user *)(uintptr_t)16, 4) == -EINTR,
          "blocking event read is interruptible");
    CHECK(fb_file_ops.read(&entry, (char __user *)(uintptr_t)16, 3) == -EINVAL,
          "short event buffer is refused");
    relatives[0] = proc;
    be(20, 20);
    proc.parent_pid = 10;
    inherited = entry;
    CHECK(put(&inherited, 1) == -EBUSY, "inheriting does not take the screen");
    open_view(&child, &child_ofd);
    CHECK(fb_owner_pid == 20, "descendant owns the display after open");
    be(10, 10);
    CHECK(fb_file_ops.poll(&entry, POLLIN | POLLOUT, &wq) == POLLIN,
          "suspended owner is readable and not writable");
    CHECK(event_from(&entry) == FB_EVENT_LOST && put(&entry, 1) == -EBUSY,
          "parent sees loss and cannot draw");
    be(21, 20);
    CHECK(put(&child, 1) == 1, "another thread of the child can draw");
    be(20, 20);
    k_memset(&keyboard, 0x17, sizeof(keyboard));
    CHECK(ask(&child, FB_SETPALETTE, 16) == 0, "child changes its palette");
    open_view(&second, &second_ofd);
    second.flags |= FD_FLAG_CLOEXEC;
    exec_views(&child, &second);
    CHECK(fb_owner_pid == 20 && put(&child, 1) == 1,
          "exec closes a marked open and keeps ownership through the other");
    fb_file_ops.close(&child);
    CHECK(fb_owner_pid == 10 && gfx_current.mode == GFX_MODE_NATIVE &&
              memcmp(&keyboard, &parent_state, sizeof(keyboard)) == 0,
          "close restores the parent's mode and all keyboard settings");
    CHECK(palette_seen[6 * 3] == 170 && palette_seen[6 * 3 + 1] == 85 && palette_seen[255 * 3] == 0,
          "return restores even the default palette");
    be(10, 10);
    CHECK(event_from(&entry) == FB_EVENT_REGAINED, "parent sees return");
    be(20, 20);
    proc.parent_pid = 10;
    open_view(&child, &child_ofd);
    relatives[1] = proc;
    be(30, 30);
    proc.parent_pid = 20;
    open_view(&grandchild, &grandchild_ofd);
    before = restores;
    fb_release_if_owner(20);
    CHECK(fb_owner_pid == 30 && restores == before,
          "dead suspended owner leaves grandchild and keyboard alone");
    fb_file_ops.close(&child);
    fb_file_ops.close(&grandchild);
    CHECK(fb_owner_pid == 10 && memcmp(&keyboard, &parent_state, sizeof(keyboard)) == 0,
          "grandchild returns past dead parent");
    be(10, 10);
    CHECK(event_from(&entry) == FB_EVENT_LOST && event_from(&entry) == FB_EVENT_REGAINED,
          "fast transfers retain both events");
    be(20, 20);
    proc.parent_pid = 10;
    open_view(&child, &child_ofd);
    fb_release_if_owner(10);
    fb_file_ops.close(&entry);
    CHECK(fb_owner_pid == 20, "dead bottom owner is skipped");
    before = restores;
    fb_release_if_owner(20);
    CHECK(fb_owner_pid == 0 && gfx_current.mode == GFX_MODE_NONE && restores == before + 1 &&
              memcmp(&keyboard, &original, sizeof(keyboard)) == 0,
          "SIGKILL of final owner restores and flushes the original console");
    CHECK(fb_file_ops.poll(&child, POLLIN, &wq) & POLLHUP, "released handle reports hangup");
    fb_file_ops.close(&child);
    be(10, 10);
    proc.parent_pid = 0;
    fail_alloc = allocations + 2;
    CHECK(fb_take(&entry) == -ENOMEM && fb_owner_pid == 0,
          "failed owner allocation leaves the console alone");
    fail_alloc = 0;
    CHECK(fb_take(&entry) == 0, "parent takes the screen after allocation failure");
    inherited = fork_view(&entry);
    close_view(&entry);
    CHECK(fb_owner_pid == 10, "parent close leaves the inherited description alive");
    be(20, 20);
    close_view(&inherited);
    CHECK(fb_owner_pid == 0, "last inherited description close releases its vanished owner");
    CHECK(locked == 0, "all ownership paths release the console lock");
}

/** Run the rows: 0 when every check passes, 1 otherwise. */
int main(void)
{
    uint32_t i;

    for (i = 0; i < sizeof(user); i++) {
        user[i] = (uint8_t)(i * 7 + 1);
    }
    entry.type = FD_TYPE_FRAMEBUFFER;
    entry.ops = &fb_file_ops;
    entry.ofd = &description;
    description.refcount = 1;
    entry.flags = FD_FLAG_READ | FD_FLAG_WRITE;
    requests();
    native_offsets();
    frame_offsets();
    forks();
    owners();
    fb_file_ops.close(&entry);
    if (failures) {
        printf("fbdev-test: %d failure(s)\n", failures);
        return 1;
    }
    printf("fbdev-test: pass: requests, offsets and the owner across fork\n");
    return 0;
}
