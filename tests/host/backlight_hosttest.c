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
 * level again, and the panic's unblank does too.
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

static struct model switched = {500, 1}, plain = {40, 1}, acpi = {12, 1};

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

static const struct backlight_ops switch_ops = {model_get, model_set, model_power};
static const struct backlight_ops plain_ops = {model_get, model_set, NULL};

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
    backlight_console_blank(1);
    backlight_panic_unblank();
    CHECK(switched.on && plain.level == 40 && acpi.level == 12,
          "the panic's unblank gives each its level again");
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
    if (failures == 0) {
        printf("backlight-test: result: pass\n");
    }
    return failures ? 1 : 0;
}
