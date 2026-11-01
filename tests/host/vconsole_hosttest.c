/* Console settings parsing and command ordering, including partial-load
 * recovery.
 * host-deps: utils/vconsole.c tests/host/compat/fixdir_host.c
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../utils/vconsole.h"
#include "compat/fixdir_host.h"

static int failed, calls, fail_at;
static char commands[8][1024];
#define CHECK(c, m)                               \
    do {                                          \
        if (!(c)) {                               \
            fprintf(stderr, "vconsole: %s\n", m); \
            failed++;                             \
        }                                         \
    } while (0)

/** Read an in-memory fixture through a temporary stream, closed on every
 * path. */
static int parse(const void *text, size_t length, struct vconsole *conf)
{
    FILE *stream = fopen(fix_path("input"), "w+b");
    int rc;
    if (!stream) {
        fprintf(stderr, "vconsole: cannot create fixture\n");
        exit(1);
    }
    if (fwrite(text, 1, length, stream) != length || fseek(stream, 0, SEEK_SET)) {
        fclose(stream);
        fprintf(stderr, "vconsole: cannot write fixture\n");
        exit(1);
    }
    rc = vconsole_read(stream, conf);
    fclose(stream);
    return rc;
}

/** Record the exact argument order and fail the selected invocation, if any. */
static int record(char **argv, void *data)
{
    int i;
    size_t used = 0;
    (void)data;
    if (calls >= 8)
        return -1;
    commands[calls][0] = 0;
    for (i = 0; argv[i]; i++) {
        int n = snprintf(commands[calls] + used, sizeof(commands[0]) - used, "%s%s", i ? "|" : "",
                         argv[i]);
        if (n < 0 || (size_t)n >= sizeof(commands[0]) - used)
            return -1;
        used += (size_t)n;
    }
    calls++;
    return calls == fail_at ? -1 : 0;
}

/** Refuse malformed input as a whole, including settings before the bad
 * line. */
static void refused(const char *text)
{
    struct vconsole conf;
    CHECK(parse(text, strlen(text), &conf) < 0, "malformed file was accepted");
    CHECK(!strcmp(conf.keymap, "us") && conf.numlock == -1 && !*conf.rate,
          "rejected file left partial settings");
}

/** Check defaults, quoting, bounds, argument vectors and recovery after each
 * tool. */
int main(void)
{
    struct vconsole conf;
    const char good[] = " # comment\n; comment\nFONT=ignored\nKEYMAP=ru\n"
                        "KEYMAP = 'kg'\r\nKEYMAP_TOGGLE=\"alt-shift\"\n"
                        "KEYMAP_OPTIONS='caps-ctrl caps-escape'\nREPEAT_DELAY=500\n"
                        "REPEAT_RATE=30\nNUMLOCK=yes";
    char longline[1200];
    const char nul[] = "KEYMAP=kg\n\0NUMLOCK=yes\n";
    int i;
    CHECK(!parse("", 0, &conf) && !strcmp(conf.keymap, "us"), "empty defaults");
    CHECK(!parse(good, sizeof(good) - 1, &conf), "valid assignments");
    CHECK(!strcmp(conf.keymap, "kg") && !strcmp(conf.toggle, "alt-shift") && conf.numlock == 1,
          "quotes, CRLF and last assignment");
    calls = fail_at = 0;
    CHECK(!vconsole_apply(&conf, record, NULL) && calls == 3, "three commands");
    CHECK(!strcmp(commands[0], "/bin/loadkeys|-u|kg|alt-shift|caps-ctrl|caps-escape"),
          "map arguments");
    CHECK(!strcmp(commands[1], "/bin/kbdrate|-d|500|-r|30"), "repeat arguments");
    CHECK(!strcmp(commands[2], "/bin/setleds|-D|+num"), "lock arguments");
    for (i = 1; i <= 3; i++) {
        calls = 0;
        fail_at = i;
        CHECK(vconsole_apply(&conf, record, NULL) < 0 && calls == i + 1,
              "failure stops the remaining commands");
        CHECK(!strcmp(commands[calls - 1], "/bin/loadkeys|-u|us"), "fallback reloads us");
    }
    refused("KEYMAP=kg\nREPEAT_DELAY=0\n");
    refused("KEYMAP=kg\nREPEAT_DELAY=60001\n");
    refused("REPEAT_RATE=1001\n");
    refused("REPEAT_RATE=999999999999999999999999\n");
    refused("REPEAT_RATE=-1\n");
    refused("REPEAT_RATE=30x\n");
    refused("NUMLOCK=maybe\n");
    refused("KEYMAP='kg\n");
    refused("KEYMAP='kg' trailing\n");
    refused("KEYMAP=-q\n");
    refused("KEYMAP=$(bad)\n");
    refused("KEYMAP=kg ru\n");
    refused("KEYMAP=../ru\n");
    refused("KEYMAP_OPTIONS='a b c d e f g h i j k l m n o p q'\n");
    refused("KEYMAP\n");
    memset(longline, 'a', sizeof(longline));
    memcpy(longline, "KEYMAP=", 7);
    CHECK(parse(longline, sizeof(longline), &conf) < 0, "long line refused");
    CHECK(parse(nul, sizeof(nul) - 1, &conf) < 0, "embedded NUL refused");
    CHECK(!parse("KEYMAP=\nNUMLOCK=no\nREPEAT_RATE=\n",
                 strlen("KEYMAP=\nNUMLOCK=no\nREPEAT_RATE=\n"), &conf),
          "empty settings");
    calls = fail_at = 0;
    CHECK(!vconsole_apply(&conf, record, NULL) && calls == 2, "empty repeat left alone");
    CHECK(!strcmp(commands[0], "/bin/loadkeys|-u|us"), "empty map means us");
    CHECK(!strcmp(commands[1], "/bin/setleds|-D|-num"), "num lock off");
    if (!failed)
        fix_passed();
    return failed ? 1 : 0;
}
