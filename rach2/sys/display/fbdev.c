/*
 * fbdev.c: the /fb device's operations: the display's owner, the writes of
 * pixels at byte offsets, and the requests that change the mode.
 */

#include "fb.h"
#include "blank.h"
#include "../kio.h"
#include "../kd.h"
#include "../io/poll.h"
#include "../proc/wait_queue.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../proc/process.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"

/* The bytes a write copies from the caller at a time, into the kernel's
 * memory, before it stores them with the console's lock held. */
#define FB_CHUNK 4096u

/* The big kernel lock serializes owners and descriptions. The console lock
 * also excludes interrupt output and keyboard events during a transfer. */
struct fb_owner {
    struct fb_owner *previous;
    uint32_t pid;             /* thread group id, not a reusable process pointer */
    unsigned int references;  /* open descriptions, not dup/fork descriptors */
    int linked;               /* still on the ownership stack */
    struct kd_state keyboard; /* this owner's settings while suspended */
    gfx_mode_t mode;
    uint8_t palette[768];
    uint64_t pending;        /* alternating LOST, REGAINED events not read */
    unsigned int next_event; /* the first pending event's type */
};

/* One per open description; dup and fork share this indirection, including
 * when FB_MODE_13H takes an unowned screen again through a stale handle. */
struct fb_handle {
    struct fb_owner *owner;
};

volatile uint32_t fb_owner_pid;
static struct fb_owner *fb_top;
static struct kd_state fb_console;
/* A released handle may take the screen again while another thread polls
 * it. Keep the queue alive independently of the replaced owner record. */
static wait_queue_t fb_wq;
static uint32_t fb_epoch; /* changes at every transfer or change of mode */
static int fb_held;       /* a DRM master has the screen: writes wait */

/** The owner attached to `e`, or NULL before its open has taken the screen. */
static struct fb_owner *fb_client(const fd_entry_t *e)
{
    struct fb_handle *h = (struct fb_handle *)e->priv;

    return h ? h->owner : NULL;
}

/** Whether the description and calling process own the display now. */
static int fb_owned(const fd_entry_t *e)
{
    return fb_top && fb_client(e) == fb_top && current_proc && fb_top->pid == current_proc->tgid;
}

/** Whether the caller descends from the owner, through any number of forks.
 * The walk is bounded by the process limit even for a damaged parent chain. */
static int fb_descendant(uint32_t owner)
{
    process_t *p = current_proc;
    unsigned int n;

    for (n = 0; p && n < PROC_COUNT_POLICY; n++) {
        if (p->tgid == owner) {
            return 1;
        }
        if (!p->parent_pid || p->parent_pid == p->pid) {
            break;
        }
        p = get_process(p->parent_pid);
    }
    return 0;
}

/** Append the next alternating ownership event and wake readers and pollers.
 * A counter keeps every transition without allocating in the release path. */
static void fb_event(struct fb_owner *o)
{
    o->pending++;
    wq_wake(&fb_wq, WQ_DATA | WQ_ROOM);
}

/** Remove `o` from the stack, restoring the next owner or the console when
 * it was on top. The caller holds the console lock; this does not sleep. */
static void fb_unlink(struct fb_owner *o)
{
    struct fb_owner **at = &fb_top;
    int top = o == fb_top;

    while (*at && *at != o) {
        at = &(*at)->previous;
    }
    if (!*at) {
        return;
    }
    *at = o->previous;
    o->linked = 0;
    o->previous = NULL;
    wq_wake_all(&fb_wq);
    if (!top) {
        return;
    }
    fb_epoch++;
    fb_owner_pid = fb_top ? fb_top->pid : 0;
    if (fb_top) {
        if (fb_top->mode == GFX_MODE_NATIVE) {
            fb_enter_native();
        } else {
            fb_enter();
        }
        fb_set_palette(fb_top->palette);
        kd_state_restore(&fb_top->keyboard);
        /* Under a DRM master the screen comes back at `fb_unhold`. */
        if (!fb_held) {
            fb_event(fb_top);
        }
    } else {
        kd_state_restore(&fb_console);
        fb_exit();
    }
}

/** Drop one description's reference to `o`. The caller holds the console
 * lock. A last close releases even when a forked child closes it last. */
static void fb_drop(struct fb_owner *o)
{
    if (!o || --o->references != 0) {
        return;
    }
    if (o->linked) {
        fb_unlink(o);
    }
    kfree(o);
}

/** Take the screen for the caller and attach its description, as fb.h
 * specifies. Allocations precede any change of ownership. */
int32_t fb_take(fd_entry_t *e)
{
    struct fb_handle *h = (struct fb_handle *)e->priv;
    struct fb_owner *o;
    uint32_t flags;

    if (fb_top && !fb_descendant(fb_top->pid)) {
        return -EBUSY;
    }
    if (fb_held && (!fb_top || fb_top->pid != current_proc->tgid)) {
        return -EBUSY;
    }
    if (!h) {
        h = (struct fb_handle *)kmalloc(sizeof(*h));
        if (!h) {
            return -ENOMEM;
        }
        h->owner = NULL;
    }
    o = fb_top;
    if (!o || o->pid != current_proc->tgid) {
        o = (struct fb_owner *)kmalloc(sizeof(*o));
        if (!o) {
            if (!e->priv) {
                kfree(h);
            }
            return -ENOMEM;
        }
        k_memset(o, 0, sizeof(*o));
        o->pid = current_proc->tgid;
        o->next_event = FB_EVENT_LOST;
        {
            unsigned int i;

            for (i = 0; i < 16; i++) {
                unsigned int bright = (i & 8u) ? 85u : 0u;

                o->palette[i * 3] = (uint8_t)((i & 4u ? 170u : 0u) + bright);
                o->palette[i * 3 + 1] = (uint8_t)((i & 2u ? 170u : 0u) + bright);
                o->palette[i * 3 + 2] = (uint8_t)((i & 1u ? 170u : 0u) + bright);
            }
            o->palette[6 * 3 + 1] = 85; /* CGA's dark yellow is brown. */
        }
    }
    console_unblank_now();
    flags = console_lock();
    if (o != fb_top) {
        if (fb_top) {
            kd_state_save(&fb_top->keyboard);
            fb_top->mode = gfx_current.mode;
            fb_event(fb_top);
        } else {
            kd_state_save(&fb_console);
        }
        o->previous = fb_top;
        o->linked = 1;
        fb_top = o;
        fb_owner_pid = o->pid;
        fb_epoch++;
        fb_enter();
        fb_set_palette(o->palette);
    }
    o->references++;
    fb_drop(h->owner);
    h->owner = o;
    e->priv = h;
    console_unlock(flags);
    return 0;
}

/** Remove an ending thread group, even when a descendant has the screen.
 * The descriptions keep the detached record alive until their last close. */
void fb_release_if_owner(uint32_t pid)
{
    struct fb_owner *o;
    uint32_t flags = console_lock();

    for (o = fb_top; o; o = o->previous) {
        if (o->pid == pid) {
            fb_unlink(o);
            break;
        }
    }
    console_unlock(flags);
}

/** Whether events wait, or the owner has permanently left the stack. */
static int fb_readable(void *arg)
{
    struct fb_owner *o = fb_client((fd_entry_t *)arg);

    return !o || o->pid != current_proc->tgid || o->pending != 0 || !o->linked;
}

/** Read one whole event, waiting interruptibly unless O_NONBLOCK is set.
 * Returns sizeof(struct fb_event), 0 after release, or -errno. As eventfd
 * does, it consumes before the user copy, so -EFAULT loses that event. */
static int32_t fb_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct fb_owner *o;
    struct fb_event event;

    if (count == 0) {
        return 0;
    }
    if (count < sizeof(event)) {
        return -EINVAL;
    }
    while (!fb_readable(e)) {
        if (e->ofd && (e->ofd->status_flags & OFD_NONBLOCK)) {
            return -EAGAIN;
        }
        if (wq_wait_event(&fb_wq, fb_readable, e) < 0) {
            return -EINTR;
        }
    }
    o = fb_client(e);
    if (!o || o->pid != current_proc->tgid) {
        return -EBUSY;
    }
    if (!o->pending) {
        return 0;
    }
    event.type = o->next_event;
    o->next_event = event.type == FB_EVENT_LOST ? FB_EVENT_REGAINED : FB_EVENT_LOST;
    o->pending--;
    if (copy_to_user((void __user *)buf, &event, sizeof(event)) != 0) {
        return -EFAULT;
    }
    return (int32_t)sizeof(event);
}

/** Report queued events as POLLIN, release as POLLHUP, and writes as
 * POLLOUT only for the active owner. Inherited handles report POLLERR. */
static short fb_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct fb_owner *o = fb_client(e);
    short mask = 0;

    if (wq) {
        *wq = &fb_wq;
    }
    if (!o || o->pid != current_proc->tgid) {
        return POLLERR;
    }
    if (o->pending) {
        mask |= POLLIN;
    }
    if (!o->linked) {
        return (mask & events) | POLLHUP;
    }
    if (fb_owned(e) && !fb_held) {
        mask |= POLLOUT;
    }
    return mask & events;
}

/** Draw the mode of the owner on top again on a screen the console was just
 * given: its pixels where they are now, its palette and its picture. The
 * caller holds the console lock. */
static void fb_redraw_top(void)
{
    if (!fb_top) {
        return;
    }
    fb_redraw();
    fb_set_palette(fb_top->palette);
    fb_present();
}

void fb_hold(void)
{
    if (fb_held) {
        return;
    }
    fb_held = 1;
    fb_epoch++;
    if (fb_top) {
        fb_event(fb_top);
    }
}

void fb_unhold(void)
{
    if (!fb_held) {
        return;
    }
    fb_held = 0;
    fb_epoch++;
    fb_redraw_top();
    if (fb_top) {
        fb_event(fb_top);
    }
    wq_wake_all(&fb_wq);
}

int fb_is_held(void)
{
    return fb_held;
}

void fb_screen_moved(void)
{
    if (fb_held || !fb_top) {
        return;
    }
    fb_epoch++;
    fb_redraw_top();
    if (gfx_current.mode == GFX_MODE_NATIVE) {
        fb_event(fb_top);
        fb_event(fb_top);
    }
}

/** Whether a write may go on: no DRM master has the screen, or `arg`'s
 * description has lost it, which the write then reports. */
static int fb_writable(void *arg)
{
    return !fb_held || !fb_owned((fd_entry_t *)arg);
}

/** The byte extent of the active mode, for seeks and bounded writes. */
uint32_t fb_size(void)
{
    return gfx_current.size;
}

/**
 * Wait while a DRM master has the screen and `e` owns it. 0 when the write
 * may go on, -EAGAIN at once on a nonblocking description, or -EINTR when
 * a signal ends the wait. Process context.
 */
static int32_t fb_wait_screen(fd_entry_t *e)
{
    while (fb_held && fb_owned(e)) {
        if (e->ofd && (e->ofd->status_flags & OFD_NONBLOCK)) {
            return -EAGAIN;
        }
        if (wq_wait_event(&fb_wq, fb_writable, e) < 0) {
            return -EINTR;
        }
    }
    return 0;
}

/* What a write found when it began: the mode in force, its size in bytes,
 * the byte offset it writes at, and `fb_epoch`. */
struct fb_shot {
    gfx_mode_t mode;
    uint32_t size;
    uint32_t at;
    uint32_t epoch;
};

/**
 * Copy `count` bytes of `buf` to the pixels at `shot->at` in pieces of
 * FB_CHUNK through `chunk`, each stored under the console's lock while `e`
 * still owns the screen as `shot` found it. Returns the bytes stored, fewer
 * when the copy from the caller faults or the screen changes.
 */
static uint32_t fb_store(fd_entry_t *e, const char __user *buf, uint32_t count, uint8_t *chunk,
                         const struct fb_shot *shot)
{
    uint32_t done = 0;

    while (done < count) {
        uint32_t n = count - done < FB_CHUNK ? count - done : FB_CHUNK;
        uint32_t flags;
        int same;

        if (copy_from_user(chunk, (const void __user *)(buf + done), n) != 0) {
            break;
        }
        flags = console_lock();
        same = fb_owned(e) && gfx_current.mode == shot->mode && gfx_current.size == shot->size &&
               fb_epoch == shot->epoch;
        if (same) {
            k_memcpy(gfx_current.framebuffer + shot->at + done, chunk, n);
            if (shot->mode == GFX_MODE_NATIVE) {
                fb_native_damage(shot->at + done, n);
            }
        }
        console_unlock(flags);
        if (!same) {
            break;
        }
        done += n;
    }
    return done;
}

/**
 * One pass of `fb_write` in the mode in force: the bytes written, or
 * -errno; 0 when a DRM master took the screen before the first byte, for
 * the caller to wait and start again.
 */
static int32_t fb_write_pass(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    gfx_mode_t mode = gfx_current.mode;
    uint64_t at = e->ofd ? e->ofd->offset : 0;
    uint32_t size = gfx_current.size;
    uint32_t epoch = fb_epoch;
    struct fb_shot shot;
    uint32_t done;
    uint8_t *chunk;

    if (!fb_owned(e) || mode == GFX_MODE_NONE) {
        return -EBUSY;
    }
    if (at > size) {
        return -EFBIG;
    }
    if (count > size - (uint32_t)at) {
        count = size - (uint32_t)at;
    }
    if (count == 0) {
        return -ENOSPC;
    }
    chunk = (uint8_t *)kmalloc(FB_CHUNK);
    if (chunk == NULL) {
        return -ENOMEM;
    }
    shot.mode = mode;
    shot.size = size;
    shot.at = (uint32_t)at;
    shot.epoch = epoch;
    done = fb_store(e, buf, count, chunk, &shot);
    kfree(chunk);
    if (done == 0 && fb_held && fb_owned(e)) {
        return 0;
    }
    if (done == 0) {
        return fb_owned(e) && fb_epoch == epoch ? -EFAULT : -EBUSY;
    }
    if (mode == GFX_MODE_NATIVE && e->ofd) {
        e->ofd->offset = at + done;
    }
    if (mode == GFX_MODE_VGA_13H && fb_owned(e) && fb_epoch == epoch) {
        fb_present();
    }
    return (int32_t)done;
}

/**
 * write(2) and pwrite(2) on /fb: copy `count` bytes of `buf` to the pixels
 * of the mode in force, at the byte the description's offset names, counted
 * from the top left pixel in rows of `pitch` bytes. Returns the bytes
 * written, or a negative errno: -EBUSY for a caller that is not the owner,
 * among them a child that inherited the descriptor; -EFBIG for an offset
 * past the mode's end and -ENOSPC for one at it; -EFAULT when no byte could
 * be read from `buf`; -ENOMEM.
 *
 * A write that would pass the end stops there, as Linux's fbdev writes. At
 * the native mode the offset moves past what was written, as on a file, so
 * lseek(2) and write(2) place rows as pwrite(2) does; pwrite(2) works on a
 * copy of the description (`pio_view` in syscall_fs.c) and leaves it alone.
 * In mode 13h the offset stays, so that each write(2) from 0 is a whole
 * frame, as Doom writes them, and the picture is shown after the copy
 * (`fb_present`).
 *
 * The bytes come in through a buffer of the kernel's, since a fault on the
 * caller's memory may sleep, and go to the pixels with the console's lock
 * held, under which the mode and the owner are checked again: a mode left
 * meanwhile by another thread of the owner ends the write, so that no pixel
 * lands on the console's text.
 *
 * While a DRM master has the screen (`fb_hold`) a write waits until the
 * console has it back, interruptibly, or answers -EAGAIN on a nonblocking
 * description; one that a master's arrival stops before its first byte
 * waits and starts again.
 */
static int32_t fb_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    int32_t r;

    for (;;) {
        if ((r = fb_wait_screen(e)) != 0) {
            return r;
        }
        if ((r = fb_write_pass(e, buf, count)) != 0) {
            return r;
        }
    }
}

/** Put the description's offset back at the first pixel, as a change of
 * mode does, so that the writes after it start at the new mode's first. */
static void fb_rewind(fd_entry_t *e)
{
    if (e->ofd) {
        fd_ofd_lock(e->ofd);
        e->ofd->offset = 0;
        fd_ofd_unlock(e->ofd);
    }
}

/**
 * ioctl(2) on /fb, the requests of <rach2/fb.h>: FB_MODE_13H, FB_MODE_TEXT,
 * FB_SETPALETTE, FB_GETINFO and FB_MODE_NATIVE. Returns 0 or a negative
 * errno: -ENOTTY for any other request; -EBUSY for a caller that is not the
 * owner, a descriptor inherited by fork or kept after the display was
 * released, except for FB_MODE_13H on a display nobody owns, which takes
 * it; -EFAULT for an argument outside the caller's memory; -ENODEV for
 * FB_GETINFO on the VGA's text mode and for FB_MODE_NATIVE there and on a
 * framebuffer that is not RGB at 15, 16, 24 or 32 bits a pixel.
 *
 * A mode request from the mode it names changes nothing; one that changes the
 * mode, or takes the screen back, puts the description's offset at 0.
 * FB_MODE_TEXT gives the display up to the previous owner or, last, the
 * console. FB_SETPALETTE reads mode 13h's 768 bytes, 256 colors of 8-bit red,
 * green and blue, in either mode. FB_GETINFO reports the framebuffer whatever
 * the mode, in a struct fb_screen.
 */
static int32_t fb_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    uint32_t flags;
    uint32_t epoch = fb_epoch;
    int32_t rc;

    if (request != FB_MODE_13H && request != FB_MODE_TEXT && request != FB_SETPALETTE &&
        request != FB_GETINFO && request != FB_MODE_NATIVE) {
        return -ENOTTY;
    }
    if (!fb_owned(e) && (fb_owner_pid != 0 || request != FB_MODE_13H)) {
        return -EBUSY;
    }
    switch (request) {
    case FB_MODE_13H:
        if (!fb_owned(e)) {
            rc = fb_take(e);
            if (rc == 0) {
                fb_rewind(e);
            }
            return rc;
        }
        if (gfx_current.mode != GFX_MODE_VGA_13H) {
            console_unblank_now();
            flags = console_lock();
            fb_enter();
            fb_set_palette(fb_top->palette);
            fb_epoch++;
            console_unlock(flags);
            fb_rewind(e);
        }
        return 0;
    case FB_MODE_NATIVE:
        if (gfx_current.mode != GFX_MODE_NATIVE) {
            uint8_t *pixels;

            flags = console_lock();
            pixels = fb_enter_native();
            if (pixels) {
                fb_epoch++;
            }
            console_unlock(flags);
            if (pixels == NULL) {
                return -ENODEV;
            }
            fb_rewind(e);
        }
        return 0;
    case FB_MODE_TEXT:
        flags = console_lock();
        fb_unlink(fb_top);
        console_unlock(flags);
        return 0;
    case FB_GETINFO: {
        struct fb_screen screen;

        if (fb_screen(&screen) != 0) {
            return -ENODEV;
        }
        if (!arg || copy_to_user((void __user *)arg, &screen, sizeof(screen)) != 0) {
            return -EFAULT;
        }
        return 0;
    }
    default: {
        uint8_t palette[768];

        if (!arg || copy_from_user(palette, (const void __user *)arg, sizeof(palette)) != 0) {
            return -EFAULT;
        }
        flags = console_lock();
        if (!fb_owned(e) || fb_epoch != epoch) {
            console_unlock(flags);
            return -EBUSY;
        }
        k_memcpy(fb_top->palette, palette, sizeof(palette));
        fb_set_palette(palette);
        console_unlock(flags);
        return 0;
    }
    }
}

/** Release the description's owner reference, wherever the caller sits in
 * the process tree. Fork and dup count in the open description itself, so
 * the file layer calls this only for its last descriptor. */
static void fb_close(fd_entry_t *e)
{
    struct fb_handle *h = (struct fb_handle *)e->priv;
    uint32_t flags;

    if (!h) {
        return;
    }
    flags = console_lock();
    fb_drop(h->owner);
    console_unlock(flags);
    kfree(h);
    e->priv = NULL;
}

const struct file_ops fb_file_ops = {fb_read, fb_write, fb_poll, fb_ioctl, fb_close, NULL, NULL};
