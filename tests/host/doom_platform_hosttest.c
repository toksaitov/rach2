/**
 * Exercise Doom startup arguments, zone sizing and normal quit.
 *
 * host-deps:
 * host-cflags: -I ports/doom/src -DNORMALUNIX -DCMAP256
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <setjmp.h>
#include <unistd.h>

static int test_open(const char *path, int flags, ...);
static int test_close(int fd);
static int test_read(int fd, void *buf, size_t count);
static int test_write(int fd, const void *buf, size_t count);
static int test_ioctl(int fd, unsigned long request, ...);
static int test_printf(const char *fmt, ...);
static void *test_malloc(size_t size);
static void test_exit(int status) __attribute__((noreturn));

#define open         test_open
#define close        test_close
#define read         test_read
#define write        test_write
#define ioctl        test_ioctl
#define gettimeofday test_gettimeofday
#define printf       test_printf
#define malloc       test_malloc
#define exit         doom_exit
#define _exit        test_exit
#define timeval      doom_timeval
#define main         doom_main
#include "../../ports/doom/doomgeneric_rach2.c"
#undef main
#include "../../ports/doom/src/i_system.c"
#include "../../ports/doom/src/m_argv.c"
#undef open
#undef close
#undef read
#undef write
#undef ioctl
#undef gettimeofday
#undef printf
#undef malloc
#undef exit
#undef _exit

static jmp_buf quit_env;
static int failures, checks, free_kib, zone_mib, expected_mib, reports;
static int closed_mouse, closed_fb, engine_closed, expected_argc;
static char **expected_argv;
static void *allocations[16];
static int allocation_count;
static const char *expected_wad;
static size_t reject_size;
static int callback_exits;
pixel_t *DG_ScreenBuffer;
boolean palette_changed;
struct color colors[256];

#define CHECK(name, condition)                                           \
    do {                                                                 \
        checks++;                                                        \
        if (!(condition)) {                                              \
            fprintf(stderr, "doom-platform-hosttest: fail: %s\n", name); \
            failures++;                                                  \
        }                                                                \
    } while (0)

/** Allocate through the host and retain pointers for per-case cleanup. */
static void *test_malloc(size_t size)
{
    void *ptr;

    if (size == reject_size) {
        return NULL;
    }
    ptr = malloc(size);
    if (ptr == NULL || allocation_count == 16) {
        fprintf(stderr, "doom-platform-hosttest: allocation failed\n");
        exit(2);
    }
    allocations[allocation_count++] = ptr;
    return ptr;
}

/** Supply memory information and require absolute device paths. */
static int test_open(const char *path, int flags, ...)
{
    if (strcmp(path, "/proc/meminfo") == 0) {
        return free_kib < 0 ? -1 : 8;
    }
    if (strcmp(path, "/fb") == 0) {
        CHECK("framebuffer mode", flags == O_RDWR);
        return 9;
    }
    if (strcmp(path, "/mouse") == 0) {
        CHECK("mouse mode", flags == O_RDONLY);
        return 10;
    }
    CHECK("absolute device path", 0);
    return -1;
}

/** Record device closure and require engine cleanup to precede it. */
static int test_close(int fd)
{
    if (fd == 9 || fd == 10) {
        CHECK("engine cleanup precedes device close", engine_closed == 1);
        if (fd == 9) {
            closed_fb++;
        } else {
            closed_mouse++;
        }
    }
    return 0;
}

/** Return one bounded meminfo record. */
static int test_read(int fd, void *buf, size_t count)
{
    if (fd == 8) {
        return snprintf(buf, count, "MemFree: %d kB\n", free_kib);
    }
    return 0;
}

/** Accept writes without a display device. */
static int test_write(int fd, const void *buf, size_t count)
{
    (void)fd;
    (void)buf;
    return (int)count;
}

/** Accept device requests without hardware. */
static int test_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    (void)request;
    return 0;
}

/** Supply a fixed time; the test does not judge elapsed time. */
int test_gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    tv->tv_sec = 1;
    tv->tv_usec = 0;
    return 0;
}

/** Check report order and the allocated zone size. */
static int test_printf(const char *fmt, ...)
{
    if (strncmp(fmt, "doom: ", 6) == 0) {
        CHECK("report follows device close", closed_fb == 1 && closed_mouse == 1);
        CHECK("report names allocated zone", DG_zone_mib == expected_mib);
        reports++;
    }
    return 0;
}

/** Check the quit status and return control to the row. */
static void test_exit(int status)
{
    CHECK("normal quit status", status == 0);
    longjmp(quit_env, 1);
}

/** Require engine cleanup while both platform descriptors remain open. */
static void engine_shutdown(void)
{
    CHECK("devices open during engine cleanup", fb_fd == 9 && mouse_fd == 10);
    engine_closed++;
    if (callback_exits) {
        doom_exit(0);
    }
}

/** Replace game loading while using the real platform and zone allocator. */
void doomgeneric_Create(int argc, char **argv)
{
    int i, size, p;

    myargc = argc;
    myargv = argv;
    CHECK("argument count", argc == expected_argc + 2);
    CHECK("argument terminator", argv[argc] == NULL);
    for (i = 0; i < expected_argc && i < argc; i++) {
        CHECK("forwarded argument", strcmp(argv[i], expected_argv[i]) == 0);
    }
    p = M_CheckParmWithArgs("-iwad", 1);
    CHECK("selected wad", p > 0 && strcmp(argv[p + 1], expected_wad) == 0);
    DG_Init();
    (void)I_ZoneBase(&size);
    zone_mib = size / (1024 * 1024);
    CHECK("allocated zone", zone_mib == expected_mib);
    I_AtExit(engine_shutdown, false);
}

/** Invoke the same quit entry point as the menu and reject a return. */
void doomgeneric_Tick(void)
{
    I_Quit();
    CHECK("quit must exit", 0);
    longjmp(quit_env, 1);
}

/** Discard input events; these rows do not draw or read input. */
void D_PostEvent(event_t *event)
{
    (void)event;
}

/** Supply integer parsing for the unused memory-access emulation path. */
boolean M_StrToInt(const char *str, int *result)
{
    return sscanf(str, "%i", result) == 1;
}

/** Supply the engine's formatting dependency through host libc. */
int M_vsnprintf(char *buf, size_t len, const char *fmt, va_list args)
{
    return vsnprintf(buf, len, fmt, args);
}

/** Supply the engine's variadic formatting dependency through host libc. */
int M_snprintf(char *buf, size_t len, const char *fmt, ...)
{
    int result;
    va_list args;

    va_start(args, fmt);
    result = vsnprintf(buf, len, fmt, args);
    va_end(args);
    return result;
}

/** Run one startup and quit, then release every host allocation. */
static void run_case(int argc, char **argv, int kib, int mib, const char *wad)
{
    int i;

    expected_argc = argc;
    expected_argv = argv;
    expected_wad = wad;
    expected_mib = mib;
    free_kib = kib;
    reports = closed_fb = closed_mouse = engine_closed = 0;
    DG_zone_mib = 0;
    if (setjmp(quit_env) == 0) {
        (void)doom_main(argc, argv);
        CHECK("main must quit through exit", 0);
    }
    CHECK("one engine cleanup", engine_closed == 1);
    CHECK("both devices closed once", closed_fb == 1 && closed_mouse == 1);
    CHECK("one final report", reports == 1);
    for (i = 0; i < allocation_count; i++) {
        free(allocations[i]);
    }
    allocation_count = 0;
    exit_funcs = NULL;
    fb_fd = mouse_fd = -1;
}

/** Cover defaults, the low-memory branch, retries and explicit options. */
int main(void)
{
    char *plain[] = {"doom", NULL};
    char *override[] = {"doom", "-mb", "10", "-nosound", NULL};
    char *wad[] = {"doom", "-iwad", "/tmp/other.wad", "-mb", "6", NULL};

    run_case(1, plain, 20 * 1024, 12, "/demo/doom1.wad");
    run_case(1, plain, 13 * 1024, 16, "/demo/doom1.wad");
    run_case(1, plain, 24 * 1024, 16, "/demo/doom1.wad");
    run_case(1, plain, -1, 16, "/demo/doom1.wad");
    reject_size = 16 * 1024 * 1024;
    run_case(1, plain, -1, 15, "/demo/doom1.wad");
    reject_size = 0;
    run_case(4, override, 20 * 1024, 10, "/demo/doom1.wad");
    run_case(5, wad, 20 * 1024, 6, "/tmp/other.wad");
    callback_exits = 1;
    run_case(1, plain, 20 * 1024, 12, "/demo/doom1.wad");
    printf("doom-platform-hosttest: result: %s: %d checks, %d failures\n",
           failures ? "fail" : "pass", checks, failures);
    return failures != 0;
}
