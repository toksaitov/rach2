/*
 * acpi_scenario.c: the scenario of the acpi cases (../machine.acpi.json).
 * ACPICA loads the firmware's namespace at boot with no error counted, and
 * the power button's press reaches it through the SCI and comes out as
 * KEY_POWER on the event device "Power Button"; a video bus of an SSDT of
 * the suite's own, fired through /proc/acpi/call, gives the brightness keys
 * on "Video Bus"; and with rach2.noacpica the kernel leaves ACPICA out and
 * powers off through the \_S5 its own reader of the tables found. One boot
 * of each case.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/** Say whether `p` begins with one or more digits, and give the end. */
static int digits(const char *p, const char **end)
{
    const char *q = p;

    while (*q >= '0' && *q <= '9')
        q++;
    *end = q;
    return q != p;
}

/**
 * Say whether some line of `w` is the kernel's record `pattern` whole: the
 * prefix `<L>[S.U] `, then the pattern, in which each `#` is a run of
 * digits, and then the line's end. A record matched whole cannot be a
 * longer one, such as a count of errors that only begins with 0.
 */
static int whole_line(const char *w, const char *pattern)
{
    const char *line, *next, *p, *q;

    for (line = w; line != NULL; line = (next = strchr(line, '\n')) != NULL ? next + 1 : NULL) {
        if (line[0] != '<' || !digits(line + 1, &q) || q[0] != '>' || q[1] != '[' ||
            !digits(q + 2, &q) || q[0] != '.' || !digits(q + 1, &q) || q[0] != ']' || q[1] != ' ')
            continue;
        p = q + 2;
        for (q = pattern; *q != '\0'; q++)
            if (*q == '#' ? !digits(p, &p) : *p++ != *q)
                break;
        if (*q == '\0' && (*p == '\r' || *p == '\n'))
            return 1;
    }
    return 0;
}

/**
 * Boot the test image to the debug shell, which the kernel starts after
 * ACPICA, and read the boot's records: ACPICA's count with no error, and the
 * SCI's line taken. Then start evprobe on the power button's event device,
 * press the button through QMP's system_powerdown once it asks, and wait
 * for the kernel's record of the event and for evprobe to read the key.
 */
static void up_case(struct lc *l)
{
    struct etude_case_machine *m;
    enum etude_status st;
    uint64_t at = 0, from;
    char *w, *reply = NULL;

    if ((m = lc_shell(l, "boot", "guest", &at)) == NULL ||
        (w = lc_console(l, "guest", 0, at)) == NULL)
        return;
    lc_assert(l, "up", whole_line(w, "acpi: acpica: # objects, 0 errors"),
              "no whole record says that ACPICA loaded the namespace with no error counted");
    lc_assert(l, "sci", whole_line(w, "acpi: sci: line #"),
              "no whole record says that ACPICA's handler took the SCI's line");
    free(w);
    if (!lc_step(l, "button"))
        return;
    from = at;
    if (!lc_type(l, m, "exec /evprobe button\n") ||
        !lc_wait(l, m, "EVPROBE: press the button", &at, NULL))
        return;
    st = etude_qmp(m, "system_powerdown", NULL, &reply);
    free(reply);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "QMP's system_powerdown");
        return;
    }
    if (!lc_wait(l, m, "acpi: event: power button", &at, "button"))
        return;
    if ((w = lc_handback(l, m, "guest", from, &at, "key")) != NULL) {
        lc_assert(l, "key", strstr(w, "EVPROBE OK") != NULL,
                  "evprobe did not read KEY_POWER on the power button's event device");
        free(w);
    }
    lc_quit(l, m, NULL);
}

/**
 * Boot the test image with the suite's SSDT, a video bus of one output, and
 * read the records of ACPICA with no error and of the bus found. Then run
 * evprobe, which calls the SSDT's methods through /proc/acpi/call and must
 * read the brightness keys that \RTST's Notify gives on "Video Bus".
 */
static void video_case(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0;
    char *w;

    if ((m = lc_shell(l, "boot", "guest", &at)) == NULL ||
        (w = lc_console(l, "guest", 0, at)) == NULL)
        return;
    lc_assert(l, "up", whole_line(w, "acpi: acpica: # objects, 0 errors"),
              "no whole record says that ACPICA loaded the namespace with no error counted");
    lc_assert(l, "bus", whole_line(w, "acpi: video: bus \\_SB.RVID, outputs 1"),
              "no whole record says that the SSDT's video bus was found with its output");
    free(w);
    if (!lc_step(l, "video"))
        return;
    if ((w = lc_run(l, m, "guest", "exec /evprobe video\n", &at, "keys")) != NULL) {
        lc_assert(l, "keys", strstr(w, "EVPROBE OK") != NULL,
                  "evprobe did not read the brightness keys or a call's result");
        free(w);
    }
    lc_quit(l, m, NULL);
}

/**
 * Boot the test image with rach2.noacpica to the debug shell: the kernel
 * says that it skipped ACPICA and loads no namespace. Then type poweroff,
 * which must end the machine with a shutdown through the sleep types of
 * \_S5 that acpi.c read from the DSDT.
 */
static void noacpica_case(struct lc *l)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    uint64_t at = 0, from;
    char detail[120], *w;

    if ((m = lc_shell(l, "boot", "guest", &at)) == NULL ||
        (w = lc_console(l, "guest", 0, at)) == NULL)
        return;
    lc_assert(l, "skipped",
              whole_line(w, "acpi: acpica: skipped: rach2.noacpica") &&
                  !whole_line(w, "acpi: acpica: # objects, # errors"),
              "the kernel did not say that it skipped ACPICA, or loaded a namespace");
    free(w);
    if (!lc_step(l, "poweroff"))
        return;
    from = at;
    if (!lc_type(l, m, "poweroff\n") || !lc_end(l, m, &end))
        return;
    snprintf(detail, sizeof(detail), "the machine ended as %d with %d, not by a shutdown",
             (int)end.end, end.value);
    lc_assert(l, "off", end.end == ETUDE_END_SHUTDOWN, detail);
    if ((w = lc_console(l, "guest", from, end.console_offset)) == NULL)
        return;
    lc_assert(l, "s5", whole_line(w, "poweroff: acpi: sleep types # and #"),
              "no whole record says that the power-off wrote \\_S5's sleep types");
    free(w);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const up_ids[] = {"up", "sci", "button", "key", NULL};
    static const char *const noacpica_ids[] = {"skipped", "off", "s5", NULL};
    static const char *const video_ids[] = {"up", "bus", "keys", NULL};
    struct lc l;

    if (strcmp(case_name(c), "noacpica") == 0) {
        lc_begin(&l, c, noacpica_ids);
        noacpica_case(&l);
    } else if (strcmp(case_name(c), "video") == 0) {
        lc_begin(&l, c, video_ids);
        video_case(&l);
    } else {
        lc_begin(&l, c, up_ids);
        up_case(&l);
    }
    return lc_finish(&l);
}
