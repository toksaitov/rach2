/**
 * Check read and seek failures after a complete interface stanza.
 *
 * host-deps: utils/ipcheck.c
 *
 * The stream model reports an error after four valid lines in either pass.
 * A failed seek leaves the stream at EOF, as the unchecked rewind did.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int pass, lines, read_fault, seek_fault, stream_error;
static int failures;

#define CHECK(condition, detail)                                   \
    do {                                                           \
        if (!(condition)) {                                        \
            fprintf(stderr, "netcfg-io-test: fail: %s\n", detail); \
            failures++;                                            \
        }                                                          \
    } while (0)

/** Open a real stream containing two complete stanzas. */
static FILE *model_fopen(const char *path, const char *mode)
{
    FILE *f = tmpfile();

    (void)path;
    (void)mode;
    if (!f) {
        exit(2);
    }
    fputs("auto eth0\niface eth0 inet static\n"
          " address 10.0.0.2\n netmask 255.255.255.0\n"
          "auto eth1\niface eth1 inet dhcp\n",
          f);
    rewind(f);
    pass = 1;
    lines = 0;
    stream_error = 0;
    return f;
}

/** Fail a read after one complete stanza. */
static char *model_fgets(char *buffer, int size, FILE *f)
{
    if (read_fault == pass && lines == 4) {
        stream_error = 1;
        errno = EIO;
        return NULL;
    }
    lines++;
    return fgets(buffer, size, f);
}

/** Report the injected stream error. */
static int model_ferror(FILE *f)
{
    return stream_error || ferror(f);
}

/** Clear stream state on a successful seek, or leave it at EOF on failure. */
static int model_fseek(FILE *f, long offset, int whence)
{
    pass++;
    lines = 0;
    stream_error = 0;
    if (seek_fault) {
        errno = ESPIPE;
        return -1;
    }
    return fseek(f, offset, whence);
}

#define fopen  model_fopen
#define fgets  model_fgets
#define ferror model_ferror
#define fseek  model_fseek
#include "netcfg.c"
#undef fopen
#undef fgets
#undef ferror
#undef fseek

/** Require each I/O error to reject all selected configuration. */
int main(void)
{
    struct netcfg_iface want[2];
    int count;

    memset(want, 0, sizeof(want));
    count = netcfg_read("interfaces", want, 0, 2, 1);
    CHECK(count == 2 && want[0].found && want[1].found, "ordinary two-pass read");
    read_fault = 1;
    memset(want, 0, sizeof(want));
    CHECK(netcfg_read("interfaces", want, 0, 2, 1) == -1, "first-pass read error");
    read_fault = 2;
    memset(want, 0, sizeof(want));
    CHECK(netcfg_read("interfaces", want, 0, 2, 1) == -1, "second-pass read error");
    read_fault = 0;
    seek_fault = 1;
    memset(want, 0, sizeof(want));
    CHECK(netcfg_read("interfaces", want, 0, 2, 1) == -1, "seek error");
    if (failures) {
        return 1;
    }
    puts("netcfg-io-test: result: pass");
    return 0;
}
