/*
 * <rach2/fb.h>: the requests, structures and events of Rach2's /fb device,
 * one header for the kernel and for programs, staged as /include/rach2/fb.h.
 */
#ifndef _RACH2_FB_H_
#define _RACH2_FB_H_

/*
 * A program takes the screen by opening /fb, which enters mode 13h, and is
 * then its owner: the console draws nothing until the last owner leaves the
 * mode, closes its open descriptions or exits. A descendant may open /fb to
 * suspend the current owner and take the screen in turn. Every request but
 * FB_MODE_13H, which may take a screen nobody owns, and every write are
 * refused with EBUSY to any process but the owner, a child that inherited the
 * descriptor among them.
 *
 * Mode 13h is 320 by 200 pixels of one byte, an index into a palette of 256
 * colors, written whole with write(2) at 64000 bytes; there the offset does
 * not move, so each write from 0 is a frame. The native mode is the screen's
 * own framebuffer as FB_GETINFO reports it, written in rows of pixels with
 * pwrite(2) at byte offsets from the top left pixel, `pitch` bytes a row, or
 * with lseek(2) and write(2), which moves the offset as on a file. A write
 * stops at the mode's end, `pitch` times `height` bytes; one that starts past
 * it is refused with EFBIG, and one at it with ENOSPC. A mode request that
 * changes the mode, or takes the screen back, puts the offset back at 0.
 *
 * The header uses no type of a C library, so that the kernel and Doom's
 * freestanding port include it too: an unsigned int has 32 bits on i686 and
 * x86-64 alike.
 */

#define FB_MODE_13H    0x4601 /* enter mode 13h */
#define FB_MODE_TEXT   0x4602 /* leave the mode and give the screen back */
#define FB_SETPALETTE  0x4603 /* mode 13h's palette: 768 bytes, red, green, blue */
#define FB_GETINFO     0x4604 /* the framebuffer, into a struct fb_screen */
#define FB_MODE_NATIVE 0x4605 /* enter the native mode, at FB_GETINFO's format */

/* FB_GETINFO, which reports the framebuffer whatever the mode, is refused
 * with ENODEV on a text console; FB_MODE_NATIVE is refused with ENODEV there
 * and on a framebuffer that is not RGB at 15, 16, 24 or 32 bits a pixel. The
 * events below report changes of ownership. */

#define FB_KIND_RGB     1 /* each pixel holds its red, green and blue */
#define FB_KIND_INDEXED 2 /* each pixel is an index into a palette */

/* One color channel of an RGB pixel, in the pixel taken as a little-endian
 * number of `bpp` bits. */
struct fb_channel {
    unsigned int offset; /* the bit where the channel begins */
    unsigned int width;  /* its bits */
};

/* The framebuffer, as FB_GETINFO reports it. */
struct fb_screen {
    unsigned int width;    /* pixels */
    unsigned int height;   /* pixels */
    unsigned int pitch;    /* bytes from the start of one row to the next */
    unsigned int bpp;      /* bits a pixel: 8, 15, 16, 24 or 32 */
    unsigned int kind;     /* FB_KIND_RGB or FB_KIND_INDEXED */
    struct fb_channel red; /* the channels of FB_KIND_RGB; all 0 for indexed */
    struct fb_channel green;
    struct fb_channel blue;
};

/*
 * An owner of the screen that starts a program which takes it in turn is
 * suspended, and has the screen back when that program leaves the mode,
 * closes /fb or exits. Each change is an event, which read(2) on the owner's
 * /fb descriptor gives whole, and which poll(2) reports as POLLIN while one
 * waits. Events are shared by all of that owner's open descriptions; a
 * read returns one event, waits when none is queued, or gives EAGAIN with
 * O_NONBLOCK. A short buffer gives EINVAL; a failed copy gives EFAULT and
 * consumes the event. After release, queued events drain before EOF and
 * poll reports POLLHUP. An inherited descriptor gives EBUSY on read and
 * POLLERR on poll. Between FB_EVENT_LOST and FB_EVENT_REGAINED the owner's
 * writes and
 * requests are refused with EBUSY; at FB_EVENT_REGAINED it draws its whole
 * screen again. Its mode, palette, keyboard settings and console termios
 * are restored first. Dead suspended owners are skipped. The last return
 * restores the console's original settings, flushes input and redraws text.
 */
#define FB_EVENT_LOST     1 /* another program has the screen and the keyboard */
#define FB_EVENT_REGAINED 2 /* they are back, as the owner left them */

struct fb_event {
    unsigned int type; /* FB_EVENT_* */
};

#endif
