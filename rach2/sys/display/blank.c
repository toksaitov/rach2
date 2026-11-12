/*
 * blank.c: the console's blanking (blank.h), its countdown on the tick and
 * the requests that blank and unblank it at once.
 *
 * A key or a mouse move only records the tick it came at and asks for an
 * unblank, since it arrives in an interrupt handler and repainting the
 * screen takes milliseconds; the boot processor's tick does the blank and
 * the unblank, under the console's lock, as a byte written to the screen
 * is.
 */

#include "../idle_timer.h"
#include "blank.h"

#include "backlight.h"
#include "fb.h"
#include "textcon.h"
#include "../cmdline.h"
#include "../kio.h"
#include "../klog.h"
#include "../timer.h"

/* The ticks of a second, and the longest time kept, about 49 days, which
 * the ticks of an unsigned 32-bit count hold. */
#define BLANK_HZ       ((uint32_t)TICK_HZ)
#define BLANK_MAX_SECS (0xFFFFFFFFu / BLANK_HZ)

/* The seconds without input before a blank, 0 for never; the tick of the
 * last poke; whether the console is blank, and whether only
 * `console_unblank_now` may unblank it; whether a poke or the escape asked
 * for an unblank, and the cause it gives; and the blanks and unblanks since
 * boot. */
static volatile unsigned int blank_secs = 600;
static volatile uint32_t last_poke;
static volatile int blanked;
static volatile int forced;
static volatile int unblank_asked;
static const char *volatile unblank_why = "by a key or a mouse move";
static unsigned int nblanks;
static unsigned int nunblanks;

void console_poke(void)
{
    last_poke = (uint32_t)kernel_ticks;
    if (blanked && !forced) {
        unblank_why = "by a key or a mouse move";
        unblank_asked = 1;
    }
}

/**
 * Blank the console, forced or not, and record it with `why`; nothing when
 * it is blank already or the screen is a program's, through /fb or as a
 * DRM master's, as Linux leaves a console in graphics mode alone. Interrupts
 * may be on or off; the console's lock must not be held by this processor's
 * caller, since the record is written after it is given back.
 */
static void do_blank(int force, const char *why)
{
    uint32_t flags = console_lock();
    int did = 0;

    if (!blanked && !gfx_is_active() && !fb_is_held()) {
        textcon_blank(1);
        blanked = 1;
        did = 1;
    }
    if (blanked && force) {
        forced = 1;
    }
    console_unlock(flags);
    if (did) {
        backlight_console_blank(1);
        nblanks++;
        klog(KLOG_INFO, "console", "blanked: %u, %s", nblanks, why);
    }
}

/** Unblank the console, when it is blank, and record it with `why`, under
 * the rules of `do_blank`. */
static void do_unblank(const char *why)
{
    uint32_t flags = console_lock();
    int did = 0;

    unblank_asked = 0;
    if (blanked) {
        textcon_blank(0);
        blanked = 0;
        forced = 0;
        did = 1;
    }
    console_unlock(flags);
    if (did) {
        backlight_console_blank(0);
        nunblanks++;
        klog(KLOG_INFO, "console", "unblanked: %u, %s", nunblanks, why);
    }
}

void blank_init(void)
{
    char value[16];
    unsigned int v = 0;
    int i, ok = 1;

    if (kcmdline_value("consoleblank", value, sizeof(value))) {
        for (i = 0; value[i] != '\0'; i++) {
            if (value[i] < '0' || value[i] > '9' || v > BLANK_MAX_SECS / 10) {
                ok = 0;
                break;
            }
            v = v * 10 + (unsigned int)(value[i] - '0');
        }
        if (ok && i > 0 && v <= BLANK_MAX_SECS) {
            blank_secs = v;
        } else {
            klog(KLOG_WARN, "console", "consoleblank: refused: %s", value);
        }
    }
    if (blank_secs == 0) {
        klog(KLOG_INFO, "console", "blank: never");
    } else {
        klog(KLOG_INFO, "console", "blank: after %u seconds without input", blank_secs);
    }
    backlight_init();
}

void blank_tick(void)
{
    if (unblank_asked) {
        do_unblank(unblank_why);
    } else if (!blanked && blank_secs != 0 &&
               (uint32_t)kernel_ticks - last_poke >= (uint32_t)blank_secs * BLANK_HZ) {
        do_blank(0, "after the time without input");
    }
}

void console_set_blank_time(unsigned int seconds)
{
    blank_secs = seconds < BLANK_MAX_SECS ? seconds : BLANK_MAX_SECS;
    last_poke = (uint32_t)kernel_ticks;
    if (blanked && !forced) {
        unblank_why = "by the escape";
        unblank_asked = 1;
    }
}

void console_blank_now(void)
{
    do_blank(1, "on request");
}

void console_unblank_now(void)
{
    last_poke = (uint32_t)kernel_ticks;
    do_unblank("on request");
}

void console_panic_unblank(void)
{
    textcon_blank(0);
    blanked = 0;
    forced = 0;
    unblank_asked = 0;
    backlight_panic_unblank();
}

int console_is_blank(void)
{
    return blanked;
}

/** Return the next service deadline for the stopped idle tick. */
uint32_t blank_idle_ticks(void)
{
    uint32_t wait = TICK_HZ;

    if (unblank_asked) {
        return 1;
    }
    if (!blanked && blank_secs != 0 && !fb_is_held()) {
        uint32_t elapsed = (uint32_t)kernel_ticks - last_poke;
        uint32_t limit = (uint32_t)blank_secs * BLANK_HZ;
        uint32_t left = elapsed >= limit ? 1u : limit - elapsed;

        if (left < wait) {
            wait = left;
        }
    }
    return wait;
}
