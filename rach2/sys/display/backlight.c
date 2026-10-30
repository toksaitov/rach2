/*
 * backlight.c: the backlight class (backlight.h). No driver registers a
 * backlight yet, so /sys/class/backlight is empty.
 */

#include "backlight.h"

#include "../lib/kdef.h"
#include "../lib/kerrno.h"

int backlight_count(void)
{
    return 0;
}

const char *backlight_name(int i)
{
    (void)i;
    return NULL;
}

int backlight_find(const char *name, int len)
{
    (void)name;
    (void)len;
    return -1;
}

int backlight_show(int i, int attr, char *out, int cap)
{
    (void)i;
    (void)attr;
    if (cap > 0) {
        out[0] = '\0';
    }
    return -ENODEV;
}

int backlight_store(int i, int attr, const char *text, int len)
{
    (void)i;
    (void)attr;
    (void)text;
    (void)len;
    return -ENODEV;
}
