/*
 * doomgeneric_rach2.c - Doom platform layer for Rach2 OS
 *
 * Implements the doomgeneric interface for running Doom on the Rach2
 * hobby operating system using VGA Mode 13h graphics, PS/2 keyboard,
 * and PS/2 mouse.
 */

#include "doomgeneric.h"
#include "doomkeys.h"
#include "d_event.h"
#include "i_video.h"

#include <stdint.h>
#include <stddef.h>

/* Syscall wrappers (defined in crt0.c) */
extern int open(const char *path, int flags, ...);
extern int close(int fd);
extern int read(int fd, void *buf, size_t count);
extern int write(int fd, const void *buf, size_t count);
extern int ioctl(int fd, unsigned long request, ...);

struct timeval {
    long tv_sec;
    long tv_usec;
};
extern int gettimeofday(struct timeval *tv, void *tz);
extern void *memset(void *s, int c, size_t n);

/* Open flags */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2

/* ioctl request codes */
#define KIOCSOUND     0x4B2F
#define FB_SETPALETTE 0x4603

/* VGA Mode 13h dimensions */
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 200

/* File descriptors */
static int fb_fd = -1;     /* Framebuffer device */
static int console_fd = 0; /* Console for keyboard input */
static int mouse_fd = -1;  /* Mouse device */

/*
 * Mouse packet structure - must match kernel's mouse.h
 */
struct mouse_packet {
    uint8_t buttons;  /* bit 0=left, 1=right, 2=middle */
    int8_t  dx;       /* X movement delta */
    int8_t  dy;       /* Y movement delta */
};

/* Current mouse button state (for tracking changes) */
static uint8_t mouse_button_state = 0;

/* Framebuffer for Mode 13h (8-bit indexed color) */
static uint8_t screen_buffer[SCREEN_WIDTH * SCREEN_HEIGHT];

/* VGA palette buffer (256 colors * 3 bytes RGB) */
static uint8_t vga_palette[768];

/* Start time for tick calculation */
static uint32_t start_time_ms = 0;

/*
 * Keyboard input queue
 * PS/2 scancodes are read from console and converted to Doom keys
 */
#define KEY_QUEUE_SIZE 32
static struct {
    int pressed;
    unsigned char doomkey;
} key_queue[KEY_QUEUE_SIZE];
static int key_queue_head = 0;
static int key_queue_tail = 0;

/*
 * PS/2 Scancode Set 1 to Doom key mapping
 */
static unsigned char scancode_to_doomkey(uint8_t scancode)
{
    switch (scancode & 0x7F) {
        case 0x01: return KEY_ESCAPE;
        case 0x02: return '1';
        case 0x03: return '2';
        case 0x04: return '3';
        case 0x05: return '4';
        case 0x06: return '5';
        case 0x07: return '6';
        case 0x08: return '7';
        case 0x09: return '8';
        case 0x0A: return '9';
        case 0x0B: return '0';
        case 0x0C: return KEY_MINUS;
        case 0x0D: return KEY_EQUALS;
        case 0x0E: return KEY_BACKSPACE;
        case 0x0F: return KEY_TAB;
        case 0x10: return 'q';
        case 0x11: return 'w';
        case 0x12: return 'e';
        case 0x13: return 'r';
        case 0x14: return 't';
        case 0x15: return 'y';
        case 0x16: return 'u';
        case 0x17: return 'i';
        case 0x18: return 'o';
        case 0x19: return 'p';
        case 0x1A: return '[';
        case 0x1B: return ']';
        case 0x1C: return KEY_ENTER;
        case 0x1D: return KEY_FIRE;        /* Left Ctrl = Fire */
        case 0x1E: return 'a';
        case 0x1F: return 's';
        case 0x20: return 'd';
        case 0x21: return 'f';
        case 0x22: return 'g';
        case 0x23: return 'h';
        case 0x24: return 'j';
        case 0x25: return 'k';
        case 0x26: return 'l';
        case 0x27: return ';';
        case 0x28: return '\'';
        case 0x29: return '`';
        case 0x2A: return KEY_RSHIFT;      /* Left Shift = Run */
        case 0x2B: return '\\';
        case 0x2C: return 'z';
        case 0x2D: return 'x';
        case 0x2E: return 'c';
        case 0x2F: return 'v';
        case 0x30: return 'b';
        case 0x31: return 'n';
        case 0x32: return 'm';
        case 0x33: return ',';
        case 0x34: return '.';
        case 0x35: return '/';
        case 0x36: return KEY_RSHIFT;      /* Right Shift = Run */
        case 0x39: return KEY_USE;         /* Space = Use/Open */
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x3F: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        case 0x47: return KEY_HOME;        /* Numpad 7 = Home (not used) */
        case 0x48: return KEY_UPARROW;     /* Up arrow / Numpad 8 */
        case 0x49: return KEY_PGUP;        /* Numpad 9 = Page Up (not used) */
        case 0x4B: return KEY_LEFTARROW;   /* Left arrow / Numpad 4 */
        case 0x4D: return KEY_RIGHTARROW;  /* Right arrow / Numpad 6 */
        case 0x4F: return KEY_END;         /* Numpad 1 = End (not used) */
        case 0x50: return KEY_DOWNARROW;   /* Down arrow / Numpad 2 */
        case 0x51: return KEY_PGDN;        /* Numpad 3 = Page Down (not used) */
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;
        default:   return 0;
    }
}

/*
 * Add a key event to the queue
 */
static void queue_key_event(int pressed, unsigned char doomkey)
{
    int next_tail = (key_queue_tail + 1) % KEY_QUEUE_SIZE;
    if (next_tail != key_queue_head) {
        key_queue[key_queue_tail].pressed = pressed;
        key_queue[key_queue_tail].doomkey = doomkey;
        key_queue_tail = next_tail;
    }
}

/*
 * Poll keyboard and queue events
 * Non-blocking read from console fd
 */
static void poll_keyboard(void)
{
    uint8_t scancode;
    int ret;

    /* Try to read scancodes (non-blocking in Rach2) */
    while ((ret = read(console_fd, &scancode, 1)) > 0) {
        unsigned char doomkey = scancode_to_doomkey(scancode);
        if (doomkey != 0) {
            int pressed = !(scancode & 0x80);
            queue_key_event(pressed, doomkey);
        }
    }
}

/*
 * Poll mouse and post events directly via D_PostEvent
 * Non-blocking read from mouse device
 */
static void poll_mouse(void)
{
    struct mouse_packet pkt;
    int ret;
    event_t event;

    if (mouse_fd < 0) return;

    /* Read mouse packets (non-blocking) */
    while ((ret = read(mouse_fd, &pkt, sizeof(pkt))) == sizeof(pkt)) {
        /* Update button state */
        mouse_button_state = pkt.buttons;

        /* Post mouse event to Doom */
        event.type = ev_mouse;
        event.data1 = mouse_button_state;
        event.data2 = pkt.dx;  /* X movement (turn) */
        event.data3 = pkt.dy;  /* Y movement (forward/backward) */
        event.data4 = 0;

        D_PostEvent(&event);
    }
}

/*
 * DG_Init - Initialize the platform
 * Opens framebuffer device (switches to VGA Mode 13h) and mouse device
 */
void DG_Init(void)
{
    struct timeval tv;

    /* Open framebuffer device - this switches to Mode 13h */
    fb_fd = open("fb", O_RDWR);
    if (fb_fd < 0) {
        /* Can't proceed without framebuffer */
        write(2, "Failed to open framebuffer\n", 27);
        return;
    }

    /* Open mouse device (optional - game works without it) */
    mouse_fd = open("mouse", O_RDONLY);
    /* No error message if mouse not available - keyboard still works */

    /* Clear screen buffer */
    memset(screen_buffer, 0, sizeof(screen_buffer));

    /* Record start time */
    gettimeofday(&tv, NULL);
    start_time_ms = tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/*
 * DG_DrawFrame - Render a frame to the screen
 * Called when DG_ScreenBuffer is ready to display
 *
 * DG_ScreenBuffer contains 32-bit ARGB pixels when not using CMAP256.
 * With CMAP256 defined, it contains 8-bit palette indices directly.
 */
void DG_DrawFrame(void)
{
    if (fb_fd < 0) return;

#ifdef CMAP256
    /* Check if palette needs to be updated */
    if (palette_changed) {
        int i;
        /* Convert colors[] (struct with r,g,b,a) to packed RGB for VGA */
        for (i = 0; i < 256; i++) {
            vga_palette[i * 3 + 0] = colors[i].r;
            vga_palette[i * 3 + 1] = colors[i].g;
            vga_palette[i * 3 + 2] = colors[i].b;
        }
        ioctl(fb_fd, FB_SETPALETTE, (unsigned long)vga_palette);
        palette_changed = 0;
    }

    /* CMAP256 mode: DG_ScreenBuffer already has 8-bit indices */
    write(fb_fd, DG_ScreenBuffer, SCREEN_WIDTH * SCREEN_HEIGHT);
#else
    /* Convert 32-bit ARGB to 8-bit palette index */
    /* This is a simple conversion - just extract the red channel */
    /* For proper rendering, the Doom palette should match VGA palette */
    uint32_t *src = DG_ScreenBuffer;
    uint8_t *dst = screen_buffer;
    int i;

    for (i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        /* Extract palette index from low byte */
        dst[i] = (uint8_t)(src[i] & 0xFF);
    }

    write(fb_fd, screen_buffer, SCREEN_WIDTH * SCREEN_HEIGHT);
#endif

    /* Poll input after each frame */
    poll_keyboard();
    poll_mouse();
}

/*
 * DG_SleepMs - Sleep for a number of milliseconds. Since we don't have a real
 * sleep syscall, just busy-wait or return immediately
 */
void DG_SleepMs(uint32_t ms)
{
    /* Busy-wait implementation using gettimeofday */
    if (ms > 0) {
        struct timeval tv;
        uint32_t start, now;

        gettimeofday(&tv, NULL);
        start = tv.tv_sec * 1000 + tv.tv_usec / 1000;

        do {
            gettimeofday(&tv, NULL);
            now = tv.tv_sec * 1000 + tv.tv_usec / 1000;
        } while (now - start < ms);
    }
}

/*
 * DG_GetTicksMs - Get milliseconds since DG_Init
 * Used for timing game logic
 */
uint32_t DG_GetTicksMs(void)
{
    struct timeval tv;
    uint32_t now_ms;

    gettimeofday(&tv, NULL);
    now_ms = tv.tv_sec * 1000 + tv.tv_usec / 1000;

    return now_ms - start_time_ms;
}

/*
 * DG_GetKey - Get the next key event from the queue
 * Returns 1 if an event was available, 0 otherwise
 */
int DG_GetKey(int *pressed, unsigned char *doomkey)
{
    if (key_queue_head == key_queue_tail) {
        /* Queue empty - poll for new events */
        poll_keyboard();
        if (key_queue_head == key_queue_tail) {
            return 0;
        }
    }

    *pressed = key_queue[key_queue_head].pressed;
    *doomkey = key_queue[key_queue_head].doomkey;
    key_queue_head = (key_queue_head + 1) % KEY_QUEUE_SIZE;

    return 1;
}

/*
 * DG_SetWindowTitle - Set window title (no-op on Rach2)
 */
void DG_SetWindowTitle(const char *title)
{
    (void)title;
    /* No window title in VGA Mode 13h */
}

/*
 * Platform-specific cleanup
 * Called when Doom exits (if we add proper cleanup)
 */
void DG_Shutdown(void)
{
    if (mouse_fd >= 0) {
        close(mouse_fd);
        mouse_fd = -1;
    }
    if (fb_fd >= 0) {
        close(fb_fd);
        fb_fd = -1;
    }
}

/*
 * Main entry point for Doom on Rach2
 */
int main(int argc, char **argv)
{
    /* Force -iwad argument to find doom1.wad directly */
    static char *doom_argv[] = { "doom", "-iwad", "doom1.wad", NULL };
    int doom_argc = 3;

    (void)argc;
    (void)argv;

    /* Initialize doomgeneric engine with fixed args */
    doomgeneric_Create(doom_argc, doom_argv);

    /* Main game loop */
    while (1) {
        doomgeneric_Tick();
    }

    /* Never reached, but for completeness */
    DG_Shutdown();
    return 0;
}
