/**
 * blank_hosttest.c: the console's blank, rach2/sys/display/blank.c, against
 * records of the screen, the backlights, the log and the clock.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The rows: `consoleblank=` sets the time; the text console blanks once
 * that time has passed without a poke, painting the screen black and
 * turning the backlights off, and a poke unblanks both at the next tick;
 * while a DRM master holds the screen the countdown passes the time, a
 * forced blank is asked for and an unblank too, and neither the screen nor
 * a backlight changes and nothing is recorded; once the console has the
 * screen back and the countdown starts again, the blank comes at the time
 * and not before; and a program's /fb keeps the console from blanking.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "display/blank.c"

extern int printf(const char *format, ...);

static int failures;

#define CHECK(condition, detail)                      \
    do {                                              \
        if (!(condition)) {                           \
            printf("blank-test: fail: %s\n", detail); \
            failures++;                               \
        }                                             \
    } while (0)

/** Whether string `s` begins with `prefix`. */
static int begins(const char *s, const char *prefix)
{
    while (*prefix != '\0') {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

/* ---- the kernel's records ---- */

volatile uint64_t kernel_ticks;
gfx_info_t gfx_current;

/* The console's lock; the DRM master's hold of the screen; the boot
 * line's consoleblank= value, NULL for none; and what was asked of the
 * screen, the backlights and the log since a row's reset. */
static int locked, held;
static const char *cmdline_blank;
static int screen_blanks, screen_unblanks, light_offs, light_ons, records, held_unlocked;
static const char *last_format;

/** Take the console's lock. */
uint32_t console_lock(void)
{
    CHECK(!locked, "the console's lock is not taken twice");
    locked = 1;
    return 0;
}

/** Give the console's lock back. */
void console_unlock(uint32_t flags)
{
    (void)flags;
    CHECK(locked, "the console's lock is held when given back");
    locked = 0;
}

/** Whether a DRM master holds the screen, read under the console's lock. */
int fb_is_held(void)
{
    if (!locked) {
        held_unlocked++;
    }
    return held;
}

/** Record the screen painted black or shown again. */
void textcon_blank(int blank)
{
    CHECK(locked, "the screen is blanked under the console's lock");
    if (blank) {
        screen_blanks++;
    } else {
        screen_unblanks++;
    }
}

/** Record the backlights turned off or given their levels again. */
void backlight_console_blank(int blank)
{
    CHECK(!locked, "the backlights change outside the console's lock");
    if (blank) {
        light_offs++;
    } else {
        light_ons++;
    }
}

/** No backlight to probe. */
void backlight_init(void)
{}

/** No panic here. */
void backlight_panic_unblank(void)
{}

/** Give the row's consoleblank= value. */
int kcmdline_value(const char *key, char *out, unsigned int outlen)
{
    if (cmdline_blank == NULL || !begins(key, "consoleblank") || key[12] != '\0') {
        return 0;
    }
    k_strncpy(out, cmdline_blank, outlen - 1);
    out[outlen - 1] = '\0';
    return 1;
}

/** Record a line of the log by its format. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    CHECK(begins(subsystem, "console") && subsystem[7] == '\0', "the records are the console's");
    records++;
    last_format = format;
}

/* ---- the rows ---- */

/** Forget what the row before asked for. */
static void reset(void)
{
    screen_blanks = screen_unblanks = light_offs = light_ons = records = 0;
    last_format = NULL;
}

/** Run the boot processor's tick `n` times. */
static void ticks(unsigned int n)
{
    while (n-- > 0) {
        kernel_ticks++;
        blank_tick();
    }
}

/** Whether the last record began with `word`. */
static int recorded(const char *word)
{
    return last_format != NULL && begins(last_format, word);
}

/** consoleblank= sets the time, and the text console blanks after it. */
static void row_text_console(void)
{
    cmdline_blank = "5";
    blank_init();
    CHECK(blank_secs == 5 && recorded("blank: after"), "consoleblank=5 sets five seconds");
    console_poke();
    reset();
    ticks(5 * TICK_HZ - 1);
    CHECK(!console_is_blank() && screen_blanks == 0 && light_offs == 0, "no blank before the time");
    ticks(1);
    CHECK(console_is_blank() && screen_blanks == 1 && light_offs == 1 && recorded("blanked"),
          "the screen and the backlights go dark at the time, with a record");
    reset();
    console_poke();
    ticks(1);
    CHECK(!console_is_blank() && screen_unblanks == 1 && light_ons == 1 && recorded("unblanked"),
          "a poke shows the screen and lights the backlights at the next tick");
}

/** A DRM master's hold leaves the screen and the backlights alone. */
static void row_drm_master(void)
{
    reset();
    held = 1;
    ticks(20 * TICK_HZ);
    CHECK(!console_is_blank() && screen_blanks == 0 && light_offs == 0 && records == 0,
          "no blank past the time while a master holds the screen");
    console_blank_now();
    CHECK(!console_is_blank() && screen_blanks == 0 && light_offs == 0 && records == 0,
          "no forced blank while a master holds the screen");
    console_unblank_now();
    ticks(1);
    CHECK(screen_unblanks == 0 && light_ons == 0 && records == 0,
          "an unblank changes nothing that was not blanked");
    CHECK(held_unlocked == 0, "the hold is read under the console's lock");

    /* The take-back: the console has the screen and its countdown starts. */
    held = 0;
    console_poke();
    ticks(5 * TICK_HZ - 1);
    CHECK(!console_is_blank() && screen_blanks == 0, "no blank before the time after the master");
    ticks(1);
    CHECK(console_is_blank() && screen_blanks == 1 && light_offs == 1,
          "the console blanks at the time once it has the screen back");
    console_poke();
    ticks(1);
}

/** A program's /fb keeps the console from blanking. */
static void row_fb_owner(void)
{
    reset();
    gfx_current.mode = GFX_MODE_NATIVE;
    ticks(20 * TICK_HZ);
    CHECK(!console_is_blank() && screen_blanks == 0 && light_offs == 0,
          "no blank while a program owns /fb");
    gfx_current.mode = GFX_MODE_NONE;
}

int main(void)
{
    row_text_console();
    row_drm_master();
    row_fb_owner();
    if (failures == 0) {
        printf("blank-test: result: pass\n");
    }
    return failures == 0 ? 0 : 1;
}
