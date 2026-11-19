/**
 * backlight_hosttest.c: the backlight class's blank, through
 * rach2/sys/display/backlight.c, over three model drivers.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kctypes.c tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/kirq_none.c
 *
 * The rows: a console blank turns a backlight with a switch off by it,
 * its level kept, and gives one without a switch the level 0; one whose
 * level 0 still lights the panel, as an ACPI _BCL whose least level is
 * above 0, keeps its level through the blank, as Linux's acpi_video does,
 * and is not counted as darkening the screen; the unblank gives each its
 * level again, and the panic's unblank does too, through each driver's
 * `panic_on`, leaving a backlight whose driver has none, or that bl_power
 * turned off, as it is; and a driver's own change of the level runs under
 * the class's lock, which a write of the level waits for, and the class
 * takes the level it returns, or keeps its own for a negative return.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "display/backlight.c"

extern int printf(const char *format, ...);

static int failures;

#define CHECK(condition, detail)                          \
    do {                                                  \
        if (!(condition)) {                               \
            printf("backlight-test: fail: %s\n", detail); \
            failures++;                                   \
        }                                                 \
    } while (0)

/* A model driver: the level it was last given and whether its switch is
 * on. */
struct model {
    int level;
    int on;
};

static struct model switched = {500, 1}, plain = {40, 1}, acpi = {12, 1}, nopanic = {30, 1};

/** The model's level. */
static int model_get(void *ctx)
{
    return ((struct model *)ctx)->level;
}

/** Give the model a level. */
static void model_set(void *ctx, int level)
{
    ((struct model *)ctx)->level = level;
}

/** Turn the model's switch. */
static void model_power(void *ctx, int on)
{
    ((struct model *)ctx)->on = on;
}

/** The model's `panic_on`: the level, then the switch on. */
static void model_panic_on(void *ctx, int level)
{
    model_set(ctx, level);
    model_power(ctx, 1);
}

static const struct backlight_ops switch_ops = {model_get, model_set, model_power, model_panic_on};
static const struct backlight_ops plain_ops = {model_get, model_set, NULL, model_set};
static const struct backlight_ops nopanic_ops = {model_get, model_set, NULL, NULL};

/** No log in the test. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** No driver to probe. */
void intelbl_init(void)
{}

/** The three backlights through a blank, an unblank and a panic's
 * unblank. */
static void row_blank(void)
{
    CHECK(backlight_register("intel_backlight", 937, &switch_ops, &switched) == 0 &&
              backlight_register("plain", 100, &plain_ops, &plain) == 1 &&
              backlight_register("acpi_video0", 20, &plain_ops, &acpi) == 2,
          "three backlights registered");
    backlight_set_lit_at_zero(2);
    CHECK(backlight_can_darken(), "the switched and the plain backlights darken the screen");
    backlight_console_blank(1);
    CHECK(!switched.on && switched.level == 500, "the blank turns the switch off, the level kept");
    CHECK(plain.level == 0, "the blank gives a backlight with no switch the level 0");
    CHECK(acpi.level == 12, "the blank leaves a backlight whose level 0 is lit at its level");
    backlight_console_blank(0);
    CHECK(switched.on && switched.level == 500 && plain.level == 40 && acpi.level == 12,
          "the unblank gives each its level again");
    CHECK(backlight_register("nopanic", 100, &nopanic_ops, &nopanic) == 3,
          "a backlight whose driver cannot light it from a panic registered");
    backlight_console_blank(1);
    CHECK(nopanic.level == 0, "the blank gives it the level 0");
    backlight_panic_unblank();
    CHECK(switched.on && plain.level == 40 && acpi.level == 12,
          "the panic's unblank gives each its level again");
    CHECK(nopanic.level == 0, "the panic leaves a backlight with no panic_on as it is");
    backlight_console_blank(0);
    CHECK(backlight_store(1, BACKLIGHT_POWER, "4", 1) == 0 && plain.level == 0,
          "bl_power 4 turns the plain backlight off");
    backlight_console_blank(1);
    backlight_panic_unblank();
    CHECK(plain.level == 0, "the panic leaves a backlight bl_power turned off dark");
    backlight_console_blank(0);
    CHECK(backlight_store(1, BACKLIGHT_POWER, "0", 1) == 0 && plain.level == 40,
          "bl_power 0 lights it again");
}

/* What the model driver's change of the level saw and returns. */
static int change_locked, change_level;

/** A driver's change of the level, as i915's enable: notes whether the
 * class's lock is held, and returns `change_level`. */
static int model_change(void *ctx)
{
    change_locked = !spin_trylock(&lock);
    if (!change_locked) {
        spin_unlock(&lock);
    }
    ((struct model *)ctx)->level = change_level < 0 ? ((struct model *)ctx)->level : change_level;
    return change_level;
}

/** A driver's raise runs under the class's lock and is shown by the class;
 * a change that raised nothing leaves the class's level, and so does an
 * index that names no backlight. */
static void row_driver_change(void)
{
    char out[16];

    nlights = 0;
    switched.level = 10;
    CHECK(backlight_register("intel_backlight", 937, &switch_ops, &switched) == 0,
          "a backlight registered");
    change_level = 937;
    backlight_driver_change(0, model_change, &switched, "raised");
    CHECK(change_locked && lights[0].level == 937 &&
              backlight_show(0, BACKLIGHT_BRIGHTNESS, out, sizeof(out)) > 0 && out[0] == '9' &&
              out[1] == '3' && out[2] == '7',
          "the raise runs under the class's lock and brightness reads it");
    CHECK(backlight_store(0, BACKLIGHT_BRIGHTNESS, "0", 1) == 0 && switched.level == 0,
          "root writes 0");
    change_level = -1;
    backlight_driver_change(0, model_change, &switched, "raised");
    CHECK(lights[0].level == 0 && switched.level == 0,
          "a change that raised nothing leaves root's 0");
    change_locked = 0;
    backlight_driver_change(5, model_change, &switched, "raised");
    CHECK(!change_locked && lights[0].level == 0, "an index of no backlight runs nothing");
}

/** With only a backlight whose level 0 is lit, nothing darkens the
 * screen. */
static void row_lit_only(void)
{
    nlights = 0;
    CHECK(backlight_register("acpi_video0", 20, &plain_ops, &acpi) == 0,
          "a lone backlight registered");
    backlight_set_lit_at_zero(0);
    CHECK(!backlight_can_darken(), "a backlight whose level 0 is lit does not darken the screen");
}

int main(void)
{
    row_blank();
    row_lit_only();
    row_driver_change();
    if (failures == 0) {
        printf("backlight-test: result: pass\n");
    }
    return failures ? 1 : 0;
}
