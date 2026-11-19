/**
 * setterm: set the console's blank time, or blank, unblank or ask it, as
 * util-linux's setterm(1) does with its `--blank` option.
 *
 * `setterm --blank N` writes `ESC [ 9 ; N ]` to the standard output, which
 * the console takes as N minutes without input before it blanks, 0 to 60, 0
 * for never. `--blank force` blanks the console until `--blank poke`
 * unblanks it, and fails when the console stays shown, as it does under a
 * picture on /fb that nothing darkens, where TIOCLINUX's blank returns 0
 * as Linux's does; `--blank` alone prints 1 while it is blank and 0
 * otherwise; these three are TIOCLINUX requests on the standard input,
 * which must be the console. The option may be spelled `-blank`, as
 * util-linux allows. setterm's other options are not here: BusyBox has no
 * setterm, and the console's blank is what Rach2's needs it for.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* The Linux console's request and the subcodes in its first byte, with
 * Linux's numbers (<linux/tiocl.h>, which the guest's musl lacks). */
#define TIOCLINUX           0x541C
#define TIOCL_UNBLANKSCREEN 4
#define TIOCL_BLANKSCREEN   14
#define TIOCL_BLANKEDSCREEN 15

/** Print how to call this program to `out`. */
static void usage(FILE *out)
{
    fputs("usage: setterm --blank [0-60|force|poke]\n", out);
}

/**
 * Send the TIOCLINUX subcode `sub` to the console on the standard input.
 * Returns the request's result, or -1 after a diagnostic.
 */
static int tioclinux(int sub)
{
    char arg = (char)sub;
    int rc = ioctl(STDIN_FILENO, TIOCLINUX, &arg);

    if (rc < 0) {
        fprintf(stderr, "setterm: blank: %s\n", strerror(errno));
    }
    return rc;
}

/**
 * Carry out `--blank` with its argument `word`, NULL for none. Returns the
 * exit status.
 */
static int blank(const char *word)
{
    char *end;
    long minutes;
    int rc;

    if (word == NULL) {
        rc = tioclinux(TIOCL_BLANKEDSCREEN);
        if (rc < 0) {
            return 1;
        }
        printf("%d\n", rc);
        return 0;
    }
    if (strcmp(word, "force") == 0) {
        if (tioclinux(TIOCL_BLANKSCREEN) < 0 || (rc = tioclinux(TIOCL_BLANKEDSCREEN)) < 0) {
            return 1;
        }
        if (rc == 0) {
            fputs("setterm: blank: refused: nothing darkens the screen\n", stderr);
            return 1;
        }
        return 0;
    }
    if (strcmp(word, "poke") == 0) {
        return tioclinux(TIOCL_UNBLANKSCREEN) < 0;
    }
    errno = 0;
    minutes = strtol(word, &end, 10);
    if (end == word || *end != '\0' || errno != 0 || minutes < 0 || minutes > 60) {
        fprintf(stderr, "setterm: blank: argument error: %s\n", word);
        return 1;
    }
    printf("\033[9;%ld]", minutes);
    return fflush(stdout) != 0;
}

int main(int argc, char **argv)
{
    const char *opt;

    if (argc < 2 || argc > 3) {
        usage(stderr);
        return 1;
    }
    opt = argv[1];
    if (strcmp(opt, "--help") == 0 || strcmp(opt, "-h") == 0) {
        usage(stdout);
        return 0;
    }
    if (strncmp(opt, "--blank=", 8) == 0 && argc == 2) {
        return blank(opt + 8);
    }
    if (strcmp(opt, "--blank") == 0 || strcmp(opt, "-blank") == 0) {
        return blank(argc == 3 ? argv[2] : NULL);
    }
    fprintf(stderr, "setterm: unknown option: %s\n", opt);
    usage(stderr);
    return 1;
}
