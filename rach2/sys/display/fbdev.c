/*
 * fbdev.c: the /fb device's operations: the display's owner, the writes of
 * pixels at byte offsets, and the requests that change the mode.
 */

#include "fb.h"
#include "blank.h"
#include "../kio.h"
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

/* The thread group id of the one process that owns the display, 0 for none.
 * Text mode is restored when the owner exits, when it asks for text mode
 * and when it closes the last descriptor of its description; a close of one
 * descriptor of a shared description leaves the mode as it is, since the
 * description is still open. */
volatile uint32_t fb_owner_pid = 0;

/** Whether the calling process owns the display. */
static int fb_owned(void)
{
    return current_proc && fb_owner_pid != 0 && fb_owner_pid == current_proc->tgid;
}

int32_t fb_take(void)
{
    uint32_t flags;

    if (fb_owner_pid != 0 && fb_owner_pid != current_proc->tgid) {
        return -EBUSY;
    }
    console_unblank_now();
    flags = console_lock();
    fb_enter();
    fb_owner_pid = current_proc->tgid;
    console_unlock(flags);
    return 0;
}

uint32_t fb_size(void)
{
    return gfx_current.size;
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
 */
static int32_t fb_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    gfx_mode_t mode = gfx_current.mode;
    uint64_t at = e->ofd ? e->ofd->offset : 0;
    uint32_t size = gfx_current.size;
    uint32_t done = 0;
    uint8_t *chunk;

    if (!fb_owned() || mode == GFX_MODE_NONE) {
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
    while (done < count) {
        uint32_t n = count - done < FB_CHUNK ? count - done : FB_CHUNK;
        uint32_t flags;
        int same;

        if (copy_from_user(chunk, (const void __user *)(buf + done), n) != 0) {
            break;
        }
        flags = console_lock();
        same = fb_owned() && gfx_current.mode == mode && gfx_current.size == size;
        if (same) {
            k_memcpy(gfx_current.framebuffer + (uint32_t)at + done, chunk, n);
        }
        console_unlock(flags);
        if (!same) {
            break;
        }
        done += n;
    }
    kfree(chunk);
    if (done == 0) {
        return fb_owned() ? -EFAULT : -EBUSY;
    }
    if (mode == GFX_MODE_NATIVE && e->ofd) {
        e->ofd->offset = at + done;
    }
    if (mode == GFX_MODE_VGA_13H) {
        fb_present();
    }
    return (int32_t)done;
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
 * A mode request from the mode it names changes nothing; one that changes
 * the mode puts the description's offset back at 0. FB_MODE_TEXT gives the
 * display up, and the console draws its text again. FB_SETPALETTE reads mode
 * 13h's 768 bytes, 256 colors of 8-bit red, green and blue, in either mode.
 * FB_GETINFO reports the framebuffer whatever the mode, in a struct
 * fb_screen.
 */
static int32_t fb_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    uint32_t flags;

    if (request != FB_MODE_13H && request != FB_MODE_TEXT && request != FB_SETPALETTE &&
        request != FB_GETINFO && request != FB_MODE_NATIVE) {
        return -ENOTTY;
    }
    if (!fb_owned() && (fb_owner_pid != 0 || request != FB_MODE_13H)) {
        return -EBUSY;
    }
    switch (request) {
    case FB_MODE_13H:
        if (gfx_current.mode != GFX_MODE_VGA_13H) {
            console_unblank_now();
            flags = console_lock();
            fb_enter();
            console_unlock(flags);
            fb_rewind(e);
        }
        fb_owner_pid = current_proc->tgid;
        return 0;
    case FB_MODE_NATIVE:
        if (gfx_current.mode != GFX_MODE_NATIVE) {
            uint8_t *pixels;

            flags = console_lock();
            pixels = fb_enter_native();
            console_unlock(flags);
            if (pixels == NULL) {
                return -ENODEV;
            }
            fb_rewind(e);
        }
        return 0;
    case FB_MODE_TEXT:
        flags = console_lock();
        fb_exit();
        fb_owner_pid = 0;
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
        static uint8_t palette[768];

        if (!arg || copy_from_user(palette, (const void __user *)arg, sizeof(palette)) != 0) {
            return -EFAULT;
        }
        flags = console_lock();
        fb_set_palette(palette);
        console_unlock(flags);
        return 0;
    }
    }
}

/**
 * The last close of a /fb description: the display returns to text mode
 * when the caller owns it, so a child that inherited the descriptor and
 * closes it or exits does not end its parent's mode.
 */
static void fb_close(fd_entry_t *e)
{
    uint32_t flags;

    (void)e;
    if (!fb_owned()) {
        return;
    }
    flags = console_lock();
    fb_exit();
    fb_owner_pid = 0;
    console_unlock(flags);
}

const struct file_ops fb_file_ops = {
    NULL,     /* read: the owner reads its keys from the console */
    fb_write, /* write */
    NULL,     /* poll */
    fb_ioctl, /* ioctl */
    fb_close  /* close */
};
