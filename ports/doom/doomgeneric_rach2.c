/*
 * doomgeneric_rach2.c: the platform layer of Doom on Rach2, the functions
 * doomgeneric asks of a platform. It draws through /fb, reads the
 * keyboard's scan codes from the console and the mouse's packets from
 * /mouse, keeps time with gettimeofday, and reports where the time went.
 */

#include "doomgeneric.h"
#include "doomkeys.h"
#include "d_event.h"
#include "i_video.h"
#include "i_system.h"

#include <stdint.h>
#include <string.h>
extern int printf(const char *fmt, ...);
#include <stddef.h>

/* The system calls of crt0.c. */
extern int open(const char *path, int flags, ...);
extern int close(int fd);
extern int read(int fd, void *buf, size_t count);
extern int write(int fd, const void *buf, size_t count);
extern int ioctl(int fd, unsigned long request, ...);
extern void _exit(int status);

struct timeval {
    long tv_sec;
    long tv_usec;
};
extern int gettimeofday(struct timeval *tv, void *tz);
extern void *memset(void *s, int c, size_t n);

/* The open flags, with Linux's values. */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2

/* The requests of the console and the framebuffer (rach2/sys/syscall.c). */
#define KIOCSOUND     0x4B2F
#define FB_SETPALETTE 0x4603

/* The frame the engine draws, mode 13h's 320 by 200. */
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 200

static int fb_fd = -1;     /* /fb, or -1 when it is not open */
static int console_fd = 0; /* standard input, the console's scan codes */
static int mouse_fd = -1;  /* /mouse, or -1 when the machine has none */

/* A packet of /mouse, as rach2/sys/mouse.h defines it. */
struct mouse_packet {
    uint8_t buttons;  /* bit 0 left, bit 1 right, bit 2 middle */
    int8_t  dx;       /* the motion since the last packet */
    int8_t  dy;
};

/* The buttons of the last packet. */
static uint8_t mouse_button_state = 0;

/* The frame as palette indices, when the engine draws 32-bit pixels. */
static uint8_t screen_buffer[SCREEN_WIDTH * SCREEN_HEIGHT];

/* The palette as FB_SETPALETTE takes it: 256 triples of red, green, blue. */
static uint8_t vga_palette[768];

/* The clock at DG_Init, in milliseconds. */
static uint32_t start_time_ms = 0;

/*
 * The key transitions read from the console and not yet given to the
 * engine, a ring that is empty when head equals tail, so it holds
 * KEY_QUEUE_SIZE - 1.
 */
#define KEY_QUEUE_SIZE 32
static struct {
    int pressed;             /* 1 for a press, 0 for a release */
    unsigned char doomkey;
} key_queue[KEY_QUEUE_SIZE];
static int key_queue_head = 0;
static int key_queue_tail = 0;

/**
 * The Doom key of a set 1 scan code, by its low seven bits, so that a press
 * and its release map alike; 0 for a code with no key, such as Alt's or the
 * 0xE0 prefix. An arrow key sends the keypad's code after 0xE0, so the two
 * share a key.
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
        case 0x1D: return KEY_FIRE;        /* Ctrl */
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
        case 0x2A: return KEY_RSHIFT;      /* left Shift */
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
        case 0x36: return KEY_RSHIFT;      /* right Shift */
        case 0x39: return KEY_USE;         /* Space */
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
        case 0x47: return KEY_HOME;        /* keypad 7, Home */
        case 0x48: return KEY_UPARROW;     /* keypad 8, up */
        case 0x49: return KEY_PGUP;        /* keypad 9, Page Up */
        case 0x4B: return KEY_LEFTARROW;   /* keypad 4, left */
        case 0x4D: return KEY_RIGHTARROW;  /* keypad 6, right */
        case 0x4F: return KEY_END;         /* keypad 1, End */
        case 0x50: return KEY_DOWNARROW;   /* keypad 2, down */
        case 0x51: return KEY_PGDN;        /* keypad 3, Page Down */
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;
        default:   return 0;
    }
}

/** Queue one key transition; one that finds the ring full is dropped. */
static void queue_key_event(int pressed, unsigned char doomkey)
{
    int next_tail = (key_queue_tail + 1) % KEY_QUEUE_SIZE;
    if (next_tail != key_queue_head) {
        key_queue[key_queue_tail].pressed = pressed;
        key_queue[key_queue_tail].doomkey = doomkey;
        key_queue_tail = next_tail;
    }
}

/**
 * Read the console's scan codes a byte at a time until a read gives none,
 * and queue each that has a key. While the game owns /fb, the console gives
 * the raw codes and does not wait when it has none.
 */
static void poll_keyboard(void)
{
    uint8_t scancode;
    int ret;

    while ((ret = read(console_fd, &scancode, 1)) > 0) {
        unsigned char doomkey = scancode_to_doomkey(scancode);
        if (doomkey != 0) {
            int pressed = !(scancode & 0x80);
            queue_key_event(pressed, doomkey);
        }
    }
}

/**
 * Post each whole packet /mouse has to the engine as a mouse event, with
 * the buttons and the X and Y motion. /mouse does not wait when it has no
 * packet. Does nothing without a mouse.
 */
static void poll_mouse(void)
{
    struct mouse_packet pkt;
    int ret;
    event_t event;

    if (mouse_fd < 0) return;

    while ((ret = read(mouse_fd, &pkt, sizeof(pkt))) == sizeof(pkt)) {
        mouse_button_state = pkt.buttons;

        event.type = ev_mouse;
        event.data1 = mouse_button_state;
        event.data2 = pkt.dx;
        event.data3 = pkt.dy;
        event.data4 = 0;

        D_PostEvent(&event);
    }
}

/* The zone's size this layer suggests to I_ZoneBase, in MiB, or 0 for none
 * (src/i_system.c). */
extern int DG_zone_mib;

/**
 * Suggest the zone this machine can hold: MemFree from /proc/meminfo less
 * 8 MiB for the rest of the game and whatever else runs, in whole MiB.
 * DG_zone_mib is left alone when the file cannot be read or the result is
 * not positive. The engine takes the suggestion when it is at least the
 * minimum and below its default, so a 32 MiB machine gets a zone of some
 * 12 MiB and no more than it has; a zone larger than memory would end the
 * game at the first page the kernel has no frame for.
 */
static void size_zone(void)
{
    char buf[512];
    int fd, n, i;

    fd = open("/proc/meminfo", 0);
    if (fd < 0) {
        return;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';
    for (i = 0; i + 8 < n; i++) {
        if (memcmp(buf + i, "MemFree:", 8) == 0) {
            int kb = atoi(buf + i + 8);
            int mib = kb / 1024 - 8;

            if (mib > 0) {
                DG_zone_mib = mib;
            }
            return;
        }
    }
}

/*
 * The report of where the time goes: the frames drawn, the time from the
 * first to the last and the longest between two, and the time the music
 * emulator and the mixer took. It is printed every ten seconds while the
 * game draws, and when the game ends.
 */
static uint32_t frames, longest_frame_ms, last_frame_ms, first_frame_ms;
/* By DG_TimeSpent's `what`: 0 the music emulator's microseconds, 1 the
 * mixer's, 2 the emulator's calls, 3 the samples it made, 4 the song's
 * events run. */
static uint32_t spent_us[5];

/** The wall clock in microseconds, modulo 2^32; the difference of two
 * readings less than 71 minutes apart is the time between them. */
uint32_t DG_NowUs(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (uint32_t)tv.tv_sec * 1000000u + (uint32_t)tv.tv_usec;
}

/**
 * Add `us` to the report's count `what`, 0 to 4 as spent_us has them; any
 * other `what` is ignored. The sound module and the music player call it.
 */
void DG_TimeSpent(int what, uint32_t us)
{
    if (what >= 0 && what < 5) {
        spent_us[what] += us;
    }
}

/* The clock of the last report, in milliseconds since DG_Init. */
static uint32_t reported_ms;

/** Print the report on one line on standard output; ports/doom/Readme.md
 * describes its fields. */
static void report(void)
{
    uint32_t span_ms = last_frame_ms - first_frame_ms;

    printf("doom: %u frames in %u ms, the longest %u ms; music emulator %u ms in %u calls for %u samples after %u events, mixer %u ms; zone %d MiB\n",
           (unsigned)frames, (unsigned)span_ms, (unsigned)longest_frame_ms,
           (unsigned)(spent_us[0] / 1000u), (unsigned)spent_us[2], (unsigned)spent_us[3],
           (unsigned)spent_us[4], (unsigned)(spent_us[1] / 1000u), DG_zone_mib);
}

/** Count a frame drawn now, keep the longest gap, and print the report when
 * ten seconds have passed since the last. */
static void count_frame(void)
{
    uint32_t now = DG_GetTicksMs();

    if (frames == 0) {
        first_frame_ms = now;
        reported_ms = now;
    } else if (now - last_frame_ms > longest_frame_ms) {
        longest_frame_ms = now - last_frame_ms;
    }
    last_frame_ms = now;
    frames++;
    if (now - reported_ms >= 10000u) {
        reported_ms = now;
        report();
    }
}

/**
 * Set the platform up: suggest the zone's size, open /fb for reading and
 * writing, which puts the display in mode 13h, and /mouse for reading, and
 * start the clock. The names are absolute, since the game may be started
 * from any directory. Without /fb it writes `Failed to open framebuffer` to
 * standard error and returns before it starts the clock, and the game runs
 * without drawing; without /mouse it goes on with the keyboard alone.
 */
void DG_Init(void)
{
    struct timeval tv;

    size_zone();

    fb_fd = open("/fb", O_RDWR);
    if (fb_fd < 0) {
        write(2, "Failed to open framebuffer\n", 27);
        return;
    }

    mouse_fd = open("/mouse", O_RDONLY);

    memset(screen_buffer, 0, sizeof(screen_buffer));

    gettimeofday(&tv, NULL);
    start_time_ms = tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/**
 * Show the frame the engine has drawn in DG_ScreenBuffer, then count it and
 * read the keyboard and the mouse. With CMAP256, which the build defines,
 * the frame is 64,000 palette indices, written to /fb as they are, after
 * the palette, through FB_SETPALETTE, when the engine has changed it.
 * Without CMAP256 the frame is 32-bit pixels, and the low byte of each is
 * taken as its index. Does nothing without /fb.
 */
void DG_DrawFrame(void)
{
    if (fb_fd < 0) return;

#ifdef CMAP256
    if (palette_changed) {
        int i;
        for (i = 0; i < 256; i++) {
            vga_palette[i * 3 + 0] = colors[i].r;
            vga_palette[i * 3 + 1] = colors[i].g;
            vga_palette[i * 3 + 2] = colors[i].b;
        }
        ioctl(fb_fd, FB_SETPALETTE, (unsigned long)vga_palette);
        palette_changed = 0;
    }

    write(fb_fd, DG_ScreenBuffer, SCREEN_WIDTH * SCREEN_HEIGHT);
#else
    uint32_t *src = DG_ScreenBuffer;
    uint8_t *dst = screen_buffer;
    int i;

    for (i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        dst[i] = (uint8_t)(src[i] & 0xFF);
    }

    write(fb_fd, screen_buffer, SCREEN_WIDTH * SCREEN_HEIGHT);
#endif

    count_frame();

    poll_keyboard();
    poll_mouse();
}

/**
 * Wait `ms` milliseconds by reading the clock until they have passed. The
 * process does not sleep, so it keeps a processor busy while it waits.
 */
void DG_SleepMs(uint32_t ms)
{
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

/** The milliseconds since DG_Init, modulo 2^32: the engine's clock. */
uint32_t DG_GetTicksMs(void)
{
    struct timeval tv;
    uint32_t now_ms;

    gettimeofday(&tv, NULL);
    now_ms = tv.tv_sec * 1000 + tv.tv_usec / 1000;

    return now_ms - start_time_ms;
}

/**
 * Give the engine the next queued key transition, in `*pressed`, 1 for a
 * press and 0 for a release, and `*doomkey`, reading the keyboard first
 * when the queue is empty. Returns 1 when it gave one, and 0 when there was
 * none.
 */
int DG_GetKey(int *pressed, unsigned char *doomkey)
{
    if (key_queue_head == key_queue_tail) {
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

/** Does nothing: the game has no window to name. */
void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

/**
 * Close the mouse and /fb, and then print the report. The last close of /fb
 * by its owner puts the display back in text mode, so the report appears on
 * the text screen.
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
    report();
}

/**
 * Every way out of the game comes here: I_Quit, I_Error, and an exit
 * callback that calls exit itself, as D_Endoom does, before I_Quit has run
 * the rest of its list. So the platform is restored whichever way the game
 * ends.
 */
void exit(int status)
{
    DG_Shutdown();
    _exit(status);
}

/**
 * Run the engine with the caller's arguments and `-iwad /demo/doom1.wad`
 * after them, and then its loop, one doomgeneric_Tick a pass. The engine
 * takes the first -iwad, so one the caller gives is used. The engine keeps
 * the argument array, which is never freed. The game ends only through
 * exit.
 */
int main(int argc, char **argv)
{
    int i;
    int doom_argc = (argc > 0 ? argc : 1) + 2;
    char **doom_argv = malloc((doom_argc + 1) * sizeof(*doom_argv));

    if (doom_argv == NULL) {
        I_Error("doom: cannot allocate arguments");
    }
    doom_argv[0] = argc > 0 ? argv[0] : "doom";
    for (i = 1; i < argc; i++) {
        doom_argv[i] = argv[i];
    }
    doom_argv[doom_argc - 2] = "-iwad";
    doom_argv[doom_argc - 1] = "/demo/doom1.wad";
    doom_argv[doom_argc] = NULL;

    doomgeneric_Create(doom_argc, doom_argv);

    while (1) {
        doomgeneric_Tick();
    }
}
