#include "cmdline.h"
#include "lib/kstring.h"

/* Long enough for any realistic GRUB or QEMU kernel command line; a longer one
 * is truncated, which at worst drops a trailing flag. */
#define KCMDLINE_MAX 256

static char kcmdline[KCMDLINE_MAX];

void kcmdline_capture(const char *cmdline)
{
    size_t i;

    if (cmdline == 0) {
        kcmdline[0] = '\0';
        return;
    }
    for (i = 0; i + 1 < KCMDLINE_MAX && cmdline[i] != '\0'; i++) {
        kcmdline[i] = cmdline[i];
    }
    kcmdline[i] = '\0';
}

int kcmdline_has(const char *flag)
{
    size_t flen = k_strlen(flag);
    const char *p = kcmdline;

    while (*p != '\0') {
        const char *start;
        size_t n, i;

        if (*p == ' ' || *p == '\t') {
            p++;
            continue;
        }
        start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
        n = (size_t)(p - start);
        if (n == flen) {
            for (i = 0; i < flen && start[i] == flag[i]; i++) {
                /* match each byte of the token against the flag */
            }
            if (i == flen) {
                return 1;
            }
        }
    }
    return 0;
}
