/* Run a Direct3D row as test from a separate disk on the GNU root, on the
 * guest's lavapipe or, for the d3dvenus family, on Venus. */
#include <stdio.h>
#include <string.h>

#include "netcase.h"

/**
 * Run `argv` through the guest channel as a d3dvenus vkd3d part of `total`
 * tests, and report the witness `unit` as each of them ends, on run.py's
 * whole line `d3d: done: N of M`, M being `total`, so that the gap's watchdog
 * bounds the time between two tests' ends and not the whole part.
 */
static enum etude_status vkd3d_part(struct net_case *u, struct etude_case_machine *m,
                                    const char *const argv[], unsigned long total,
                                    struct net_run *r)
{
    char text[64];
    unsigned long n;
    size_t from = 0;

    if (net_guest_start(u, m, argv, NULL, "d3d: units: ") == ETUDE_OK) {
        /* run.py prints its environment's line first, so a newline comes
         * before the header; n is 0 for it. */
        for (n = 0; n <= total; n++) {
            if (n == 0)
                snprintf(text, sizeof(text), "\nd3d: units: %lu\n", total);
            else
                snprintf(text, sizeof(text), "\nd3d: done: %lu of %lu\n", n, total);
            if (etude_guest_output_wait_from(m, text, &from) != ETUDE_OK)
                break;
            from--; /* the newline that ends the line begins the next */
            if (n > 0)
                etude_progress(u->c, "unit", n);
        }
    }
    return net_guest_finish(u, m, 0, "direct3d", r);
}

/** Say whether the line at `line` is `want`, its end a newline. */
static int line_is(const char *line, const char *want)
{
    size_t n = strlen(want);

    return strncmp(line, want, n) == 0 && line[n] == '\n';
}

/**
 * Say whether a vkd3d part's standard output `out`, NULL when it was not
 * kept, has the whole progress of the part's `total` tests, its declared
 * count: one line `d3d: units: M`, M being `total`, and after it the lines
 * `d3d: done: N of M` for each N from 1 to M, in order. Write what is wrong to
 * `why` when it has not. A `total` of 0, no declared count, is never whole.
 */
static int progress_whole(const char *out, unsigned long total, char *why, size_t size)
{
    static const char units[] = "d3d: units: ", done[] = "d3d: done: ";
    char header[64], want[64], got[48];
    const char *line, *next;
    unsigned long n = 0;
    int seen = 0;

    snprintf(header, sizeof(header), "%s%lu", units, total);
    for (line = out != NULL ? out : ""; *line != '\0'; line = next) {
        next = strchr(line, '\n');
        next = next != NULL ? next + 1 : line + strlen(line);
        if (strncmp(line, units, sizeof(units) - 1) == 0) {
            if (seen || !line_is(line, header)) {
                snprintf(why, size, "\"%s\" is not the one header \"%s\"",
                         net_copy_line(line, got, sizeof(got)), header);
                return 0;
            }
            seen = 1;
        } else if (strncmp(line, done, sizeof(done) - 1) == 0) {
            snprintf(want, sizeof(want), "%s%lu of %lu", done, ++n, total);
            if (!seen || !line_is(line, want)) {
                snprintf(why, size, "\"%s\" where \"%s\" was due",
                         net_copy_line(line, got, sizeof(got)), seen ? want : header);
                return 0;
            }
        }
    }
    if (!seen || total == 0 || n != total) {
        snprintf(why, size, "%lu of %lu tests have ended", n, total);
        return 0;
    }
    return 1;
}

/**
 * Print what run.py printed in `r` and assert that it exited 0 with the
 * result token of the row `row` of `family` and, for a d3dvenus vkd3d part,
 * when `part` is nonzero, that its progress is whole for its `total` tests.
 */
static void judge(struct net_case *u, const struct net_run *r, const char *family, const char *row,
                  int part, unsigned long total)
{
    char marker[192], progress[160];
    int matched, whole;

    fputs(r->out, stdout);
    fputs(r->err, stderr);
    snprintf(marker, sizeof(marker), "D3DROW OK %s %s", family, row);
    matched = net_exited0(r) && net_find(r->out, marker) != NULL;
    whole = !part || progress_whole(r->out, total, progress, sizeof(progress));
    net_assert(u, "row", matched && whole, "%s%s%s%s",
               matched ? "" : "Direct3D row did not match Linux; see direct3d evidence",
               matched || whole ? "" : "; ", whole ? "" : "progress: ", whole ? "" : progress);
}

/**
 * Run one row and require its exit and complete result token to agree, and a
 * d3dvenus vkd3d part's progress to be whole.
 *
 * A d3dvenus case is probe-PATH-N, wine-PATH-EXE-UNIT or vkd3d-PART, run
 * with run.py's -a venus against the Linux guest's reference on Venus. A
 * probe's N may end in -wow64 or -i386, its i386 build in that Proton mode.
 */
int etude_scenario(struct etude_case *c)
{
    struct net_case u;
    struct net_boot b;
    struct net_run r;
    char password[128], mode[16], exe[32], unit[64];
    char family[16], row[128], command[2048], path[16], n[16];
    const char *id = etude_case_id(c), *tail = strrchr(id, '.');
    const char *argv[] = {"/bin/bash", "-c", command, NULL};
    const char *adapter = "lavapipe";
    unsigned long total = 0;
    int part;

    net_case(&u, c);
    memset(&b, 0, sizeof(b));
    b.u = &u;
    tail = tail != NULL ? tail + 1 : id;
    if (strncmp(id, "machine.d3dvenus.", 17) == 0) {
        adapter = "venus";
        if (sscanf(tail, "probe-%15[^-]-%15s", path, n) == 2) {
            snprintf(family, sizeof(family), "probe");
            snprintf(row, sizeof(row), "%s/%s", path, n);
        } else if (sscanf(tail, "wine-%15[^-]-%31[^-]-%63s", mode, exe, unit) == 3) {
            snprintf(family, sizeof(family), "wine");
            snprintf(row, sizeof(row), "%s/%s:%s", mode, exe, unit);
        } else if (sscanf(tail, "vkd3d-%15s", n) == 1) {
            snprintf(family, sizeof(family), "vkd3d");
            snprintf(row, sizeof(row), "%s", n);
        } else {
            net_fail(&u, "unknown Direct3D case %s", id);
            return net_end(&u);
        }
    } else if (strncmp(id, "machine.d3d-wine.", 17) == 0 &&
               sscanf(tail, "%15[^-]-%31[^-]-%63s", mode, exe, unit) == 3) {
        snprintf(family, sizeof(family), "wine");
        snprintf(row, sizeof(row), "%s/%s:%s", mode, exe, unit);
    } else if (strncmp(id, "machine.d3d-vkd3d.", 18) == 0) {
        snprintf(family, sizeof(family), "vkd3d");
        snprintf(row, sizeof(row), "%s", tail);
    } else if (strcmp(id, "machine.d3d.private") == 0) {
        snprintf(family, sizeof(family), "private");
        snprintf(row, sizeof(row), "files");
    } else if (strncmp(id, "machine.d3d.", 12) == 0) {
        snprintf(family, sizeof(family), "probe");
        snprintf(row, sizeof(row), "%s", tail);
    } else {
        net_fail(&u, "unknown Direct3D case %s", id);
        return net_end(&u);
    }
    net_secret(&u, "demo-password", password, sizeof(password));
    if (net_step(&u, "boot") == ETUDE_OK)
        net_boot(&b, &u, "gnu", 1, NULL);
    net_seen(&b, "ext2: root: /dev/disk0p1");
    net_seen(&b, "rach2 login");
    if (net_login(&b, "root", password, "# ") == ETUDE_OK && etude_guest_open(b.m) != ETUDE_OK)
        net_fail(&u, "the GNU guest command channel did not open");
    /* The row runs as test in video and render alone, the groups of the
     * Debian account that took the references. test is also in kvm, whose
     * /dev/udmabuf gives lavapipe sync files and so Proton the keyed mutex
     * the references were taken without; BusyBox su would give every group. */
    snprintf(command, sizeof(command),
             "mkdir -p /tmp/d3d /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix && "
             "mount -t ext2 -o ro /dev/disk1 /tmp/d3d && "
             "python3 -c 'import grp, os, pwd, sys; u = pwd.getpwnam(\"test\"); "
             "os.setgroups([grp.getgrnam(g).gr_gid for g in (\"video\", \"render\")]); "
             "os.setgid(u.pw_gid); os.setuid(u.pw_uid); "
             "os.environ.update(HOME=u.pw_dir, SHELL=u.pw_shell, USER=u.pw_name, "
             "LOGNAME=u.pw_name); os.execvp(sys.argv[1], sys.argv[1:])' "
             "python3 /tmp/d3d/run.py -a %s %s %s /tmp/d3d-results",
             adapter, family, row);
    part = strcmp(adapter, "venus") == 0 && strcmp(family, "vkd3d") == 0;
    /* The witness's limit in the manifest is the part's count of tests. */
    if (part)
        total = (unsigned long)etude_progress_limit(c, "unit");
    if (net_step(&u, "row") == ETUDE_OK &&
        (part ? vkd3d_part(&u, b.m, argv, total, &r)
              : net_guest(&u, b.m, argv, NULL, NULL, "direct3d", &r)) == ETUDE_OK) {
        judge(&u, &r, family, row, part, total);
        net_run_free(&r);
    } else
        net_assert(&u, "row", 0, "Direct3D row could not run");
    return net_end(&u);
}
