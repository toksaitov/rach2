/* flow_release.c: resume records, reruns of failures, the build's results,
 * the install's move of the kernel slots, the recovery trial's fallback, the
 * trial boot's command line, acceptance order and the preparation of -p. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sha256.h"
#define H          "1111111111111111111111111111111111111111"
#define C          "2222222222222222222222222222222222222222"
#define T          "2026-10-10T00:00:00Z"
#define BRANCH     "milestone/07-seven"
/* The digests release.sha256 gives the kernel and the root in a row of the
 * install, which the machine's /dev/shm then holds. */
#define KERNEL_SUM "3333333333333333333333333333333333333333333333333333333333333333"
#define ROOT_SUM   "4444444444444444444444444444444444444444444444444444444444444444"
/* The bootctl the build made, which the recovery trial's fallback sends. */
#define BOOTCTL    "the build's bootctl\n"
/* The suites of the mock registry that a machine's run takes, less style. */
#define SUITES     "x86_64:agents i686:fs x86_64:fs x86_64:bigmem i686:gnu x86_64:gnu"
static const char *ev(const char *n)
{
    const char *v = getenv(n);
    return v ? v : "";
}
/**
 * Wait until the release's own holder, in the run directory that
 * FLOW_WORK/state/run names, has written its end: the Mac's run, which
 * calls this from its make, then still runs once the release and its
 * holder's drain have ended, as a run on a slow machine does.
 */
static void release_ended(void)
{
    char path[4200], *dir;
    snprintf(path, sizeof(path), "%s/state/run", ev("FLOW_WORK"));
    dir = fx_read(path);
    if (!dir)
        return;
    dir[strcspn(dir, "\n")] = 0;
    snprintf(path, sizeof(path), "%s/end", dir);
    free(dir);
    while (!fx_exists(path))
        fx_pause();
}
static void append(const char *file, const char *text)
{
    FILE *f = fopen(file, "ab");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}
static const char *rp(const char *suffix)
{
    static char p[16][4096];
    static unsigned i;
    char *v = p[i++ % 16];
    snprintf(v, 4096, "%s%s", ev("RELEASE_TEST_CALLS"), suffix);
    return v;
}
static char *joined(int argc, char **argv, int first)
{
    size_t n = 2;
    int i;
    char *s;
    for (i = first; i < argc; i++)
        n += strlen(argv[i]) + 1;
    s = malloc(n);
    if (!s)
        return NULL;
    s[0] = 0;
    for (i = first; i < argc; i++) {
        if (i > first)
            strcat(s, " ");
        strcat(s, argv[i]);
    }
    strcat(s, "\n");
    return s;
}
static void call(const char *prefix, int argc, char **argv, int first)
{
    char *s = joined(argc, argv, first), *v;
    if (!s)
        return;
    v = malloc(strlen(prefix) + strlen(s) + 1);
    if (v) {
        strcpy(v, prefix);
        strcat(v, s);
        append(ev("RELEASE_TEST_CALLS"), v);
        free(v);
    }
    free(s);
}
static int sh_mock(int argc, char **argv)
{
    const char *name = argc > 1 ? strrchr(argv[1], '/') : NULL;
    name = name ? name + 1 : argc > 1 ? argv[1] : "";
    if (!strcmp(name, "board.sh")) {
        call("board ", argc, argv, 2);
        if (argc > 2 && !strcmp(argv[2], "milestone")) {
            if (!strcmp(ev("RELEASE_TEST_MILESTONE"), "lost"))
                return 1;
            if (!strcmp(ev("RELEASE_TEST_MILESTONE"), "closed"))
                return 3;
            puts(BRANCH);
        }
        return 0;
    }
    if (!strcmp(name, "testing.sh")) {
        char prefix[5000];
        snprintf(prefix, sizeof(prefix), "testing %s ", ev("RACH2_TESTING_COPY"));
        call(prefix, argc, argv, 2);
        if (!strcmp(ev("RELEASE_TEST_TESTING"), "pass"))
            return 0;
        puts("testing: the host did not answer");
        return 1;
    }
    if (!strcmp(name, "machine_identity.sh")) {
        call("identity ", argc, argv, 2);
        return 0;
    }
    if (!strcmp(name, "metal_accept.sh")) {
        call("accept ", argc, argv, 2);
        if (!strcmp(ev("RELEASE_TEST_ACCEPT"), "pass")) {
            puts("metal-accept: PASS");
            return 0;
        }
        puts("  FAIL: the hub section found no hub\nmetal-accept: FAIL (1)");
        return 1;
    }
    argv[0] = "/bin/sh";
    execv(argv[0], argv);
    return 127;
}
/**
 * Answer `gh api -X`, the milestone's closing, and `gh api --paginate` of a
 * milestone's open issues, which RELEASE_TEST_OPEN gives as `N:A B` for
 * milestone N; any other milestone has none. An item `pA` is a pull request,
 * left out only when the query's --jq selects on pull_request as gh would,
 * and `garbage` stands for an error body that gh printed to stdout. `retry`
 * fails the first query as GitHub's 503 does, with its body on stdout, and
 * answers no issues on the next.
 */
static int gh_api(int argc, char **argv)
{
    const char *open = ev("RELEASE_TEST_OPEN"), *m;
    int prs = 1, i;
    size_t n;
    if (argc > 3 && !strcmp(argv[2], "-X"))
        return strcmp(ev("RELEASE_TEST_CLOSE"), "fail") == 0;
    if (argc < 4 || strcmp(argv[2], "--paginate") || !(m = strstr(argv[3], "/issues?milestone=")))
        return 1;
    for (i = 4; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--jq") && strstr(argv[i + 1], "select(.pull_request == null)"))
            prs = 0;
    m += strlen("/issues?milestone=");
    n = strcspn(m, "&");
    if (!strcmp(open, "7:retry")) {
        char *calls = fx_read(ev("RELEASE_TEST_CALLS")), *c = calls;
        int asked = 0;
        while (c && (c = strstr(c, "gh api --paginate ")) != NULL && ++asked)
            c++;
        free(calls);
        if (asked > 1)
            return 0;
        printf("{\"message\":\"unavailable\"}\n");
        fprintf(stderr, "gh: unavailable (HTTP 503)\n");
        return 1;
    }
    if (!strncmp(open, m, n) && open[n] == ':')
        for (open += n + 1; *open; open += strspn(open, " ")) {
            n = strcspn(open, " ");
            if (!strncmp(open, "garbage", n))
                printf("{\"message\":\"unavailable\"}\n");
            else if (*open != 'p')
                printf("%.*s\n", (int)n, open);
            else if (prs)
                printf("%.*s\n", (int)n - 1, open + 1);
            open += n;
        }
    return 0;
}
/** Answer `gh run download ID ... --dir DIR` with RELEASE_TEST_FAILURES. */
static int gh_download(int argc, char **argv)
{
    char file[4200];
    if (!*ev("RELEASE_TEST_FAILURES") || argc < 2 || strcmp(argv[argc - 2], "--dir"))
        return 1;
    snprintf(file, sizeof(file), "%s/failures", argv[argc - 1]);
    mkdir(argv[argc - 1], 0700);
    return !fx_text(file, ev("RELEASE_TEST_FAILURES"));
}
static int gh_mock(int argc, char **argv)
{
    char *all = joined(argc, argv, 1), *line;
    int status = 0;
    if (argc < 3 || !all) {
        free(all);
        return 1;
    }
    line = malloc(strlen(all) + 4);
    if (line) {
        sprintf(line, "gh %s", all);
        append(ev("RELEASE_TEST_CALLS"), line);
        free(line);
    }
    if (!strcmp(argv[1], "api"))
        status = gh_api(argc, argv);
    else if (!strcmp(argv[1], "run") && !strcmp(argv[2], "list"))
        status = strcmp(ev("RELEASE_TEST_RUNS"), "none") ||
                 fx_has(ev("RELEASE_TEST_CALLS"), "dispatch ");
    else if (!strcmp(argv[1], "run") && !strcmp(argv[2], "download"))
        status = gh_download(argc, argv);
    else if (!strcmp(argv[1], "workflow") && !strcmp(argv[2], "run")) {
        char *dispatch = malloc(strlen(all) + 10);
        if (!dispatch)
            status = 1;
        else {
            sprintf(dispatch, "dispatch %s", all);
            append(ev("RELEASE_TEST_CALLS"), dispatch);
            free(dispatch);
        }
    } else
        status = 1;
    free(all);
    return status;
}
/**
 * Succeed when the claim whose runner claim_start starts by the line that
 * begins START has been granted and has not ended, as the calls so far
 * record: that line is recorded, and the release has not touched the claim's
 * end file, whose name ends END, since. Repeated run identifiers must not let
 * a look use a claim from an earlier attempt. In `probe` mode, reject the
 * first look, the WSL line `line`, after each start is recorded.
 */
static int granted(const char *start, const char *end, const char *line)
{
    char probe[4200], *calls = fx_read(ev("RELEASE_TEST_CALLS"));
    const char *last = NULL, *at = calls;
    int made;
    while (at && (at = strstr(at, start)) != NULL)
        last = at++;
    made = last && !strstr(last, end);
    if (made && !strcmp(ev("RELEASE_TEST_HOLD"), "probe")) {
        snprintf(probe, sizeof(probe), "wsl %s", line);
        at = strstr(last, probe);
        made = at && strstr(at + strlen(probe), probe) != NULL;
    }
    free(calls);
    return made;
}
/**
 * Say whether WSL's admission refuses a claim as RELEASE_TEST_HOLD says:
 * `refused`, it refuses the whole host, `alone`; `claim`, the preparation's.
 */
static int refused(int alone)
{
    return !strcmp(ev("RELEASE_TEST_HOLD"), alone ? "refused" : "claim");
}
/**
 * Print the launcher's refusal when the WSL line B is a `cat` of a claim
 * attempt's output, LOCKS/ID.KIND.N.out, and WSL's admission refuses that
 * claim.
 */
static void refusal(const char *b)
{
    size_t n = strcspn(b, "\n");
    if (strncmp(b, "cat ", 4))
        return;
    if (n > 4 && !strncmp(b + n - 4, ".out", 4))
        n -= 4;
    /* The attempt's number, `.N`, ends the output's name. */
    while (n > 4 && isdigit((unsigned char)b[n - 1]))
        n--;
    if (n > 4 && b[n - 1] == '.')
        n--;
    if (n > 5 && ((n > 6 && !strncmp(b + n - 6, ".claim", 6) && refused(0)) ||
                  (!strncmp(b + n - 5, ".hold", 5) && refused(1))))
        puts("etude-launch: result: refused: admission: no room in 20400 seconds");
}
/**
 * Answer a script for WSL, whose lines are recorded but those of claim.pl
 * that a line `... << 'END_OF_CLAIM'` feeds: claim_state's, whose line `perl
 * - state LOCKS ID KIND N` names the claim, prints `granted`, `refused` or,
 * while the claim waits, as the whole host's always does in `wait` mode,
 * nothing; `cat` of a claim attempt's output, LOCKS/ID.KIND.N.out, prints
 * the launcher's refusal when the claim is refused. A beat's minute,
 * `printf . >> ID.alive; sleep 60`, passes as a second, so that the beat's
 * loop leaves the processor to the run it beats for. Any other script, which
 * starts or stops a claim, touches a file or tests that a claim has ended,
 * succeeds.
 */
static int wsl_mock(void)
{
    char line[20000], b[4096], locks[4096], id[4096], kind[64], start[9000], end[4200],
        probe[4096] = "";
    int alone, body = 0, named = 0, beat = 0;
    while (fgets(b, sizeof(b), stdin)) {
        if (body && strcmp(b, "END_OF_CLAIM\n"))
            continue;
        body = !body && strstr(b, "<< 'END_OF_CLAIM'") != NULL;
        beat |= strstr(b, ".alive; sleep 60") != NULL;
        snprintf(line, sizeof(line), "wsl %s", b);
        append(ev("RELEASE_TEST_CALLS"), line);
        if (!named && sscanf(b, "perl - state %4095s %4095s %63s", locks, id, kind) == 3) {
            named = 1;
            snprintf(probe, sizeof(probe), "%s", b);
        }
        refusal(b);
    }
    if (beat) {
        struct timespec minute = {1, 0};
        /* a beat's minute passes as a second */
        nanosleep(&minute, NULL);
    }
    if (!named)
        return 0;
    alone = !strcmp(kind, "hold");
    snprintf(start, sizeof(start), "wsl cd /tmp && setsid nohup perl %s/%s.%s.", locks, id, kind);
    snprintf(end, sizeof(end), "/%s.%s\n", id, alone ? "end" : "prepared");
    if (refused(alone))
        puts("refused");
    else if (!(alone && !strcmp(ev("RELEASE_TEST_HOLD"), "wait")) && granted(start, end, probe))
        puts("granted");
    return 0;
}
/**
 * Answer winclean.sh's gate, `-g ... ID`, and its list, `-l`: while
 * RELEASE_TEST_HOLD begins with `stuck`, as when winclean.sh cannot stop
 * them, each run whose preparation a start of the calls so far began, but
 * the gate's own ID, as `left ID` for the gate, failing then, and as
 * `orphan ID` for the list; otherwise none. The list fails, as when OUT
 * cannot be read, while RELEASE_TEST_HOLD is `unlisted`.
 */
static int gate_mock(const char *command, int gate)
{
    static const char start[] = "\nwindows b=D:/o/";
    const char *own = strrchr(command, ' ');
    char *s, *p, *e;
    int left = 0;
    if (!gate && !strcmp(ev("RELEASE_TEST_HOLD"), "unlisted"))
        return 1;
    if (strncmp(ev("RELEASE_TEST_HOLD"), "stuck", 5))
        return 0;
    own = own ? own + 1 : "";
    s = fx_read(ev("RELEASE_TEST_CALLS"));
    for (p = s; p && (p = strstr(p, start)) != NULL; p = e) {
        p += strlen(start);
        e = strchr(p, '\n');
        if (!e || e - p <= 8 || strncmp(e - 8, ".prepare", 8))
            continue;
        if (gate && (size_t)(e - p - 8) == strlen(own) && !strncmp(p, own, strlen(own)))
            continue;
        printf("%s %.*s\n", gate ? "left" : "orphan", (int)(e - p - 8), p);
        left = 1;
    }
    free(s);
    return gate && left;
}
/**
 * Answer a script for Windows' shell, whose lines are recorded: the start of
 * a part that windows_detach runs apart from its session prints its start's
 * status, and a poll of the part, whose first line names the part's files,
 * prints the part's output, RELEASE_TEST_PREPARE, `status 0` by default, for
 * the preparation and RELEASE_TEST_WINDOWS for the cases, as `poll done`
 * when it has a status line and as `poll ended`, with an empty console, when
 * it has none. winclean.sh fails, as when a part it stops still runs, while
 * RELEASE_TEST_HOLD begins with `stuck`; its gate, -g in COMMAND, and its
 * list, -l, whose lines are recorded as `gate LINE` and `list LINE`, are
 * gate_mock's. Any other script prints nothing.
 */
static int windows_mock(const char *command)
{
    char line[20000], b[4096];
    const char *out;
    int start = 0, poll = 0, prepare = 0, clean = 0;
    int gate = strstr(command, "bash.exe -l -s -- -g ") != NULL;
    int list = strstr(command, "bash.exe -l -s -- -l ") != NULL;
    while (fgets(b, sizeof(b), stdin)) {
        snprintf(line, sizeof(line), "%s %s", gate ? "gate" : list ? "list" : "windows", b);
        append(ev("RELEASE_TEST_CALLS"), line);
        start |= !strncmp(b, "END_OF_RUN", 10);
        poll |= !strncmp(b, "echo \"poll $a\"", 14);
        prepare |= !strncmp(b, "b=", 2) && strstr(b, ".prepare c=") != NULL;
        clean |= !strncmp(b, "# winclean.sh: ", 15);
    }
    if (clean && (gate || list))
        return gate_mock(command, gate);
    if (clean)
        return !strncmp(ev("RELEASE_TEST_HOLD"), "stuck", 5);
    if (start)
        puts("start: status 0");
    if (!poll)
        return 0;
    out = !prepare                      ? ev("RELEASE_TEST_WINDOWS")
          : *ev("RELEASE_TEST_PREPARE") ? ev("RELEASE_TEST_PREPARE")
                                        : "status 0";
    if (!strncmp(out, "status ", 7) || strstr(out, "\nstatus "))
        printf("poll done\n%s\n", out);
    else
        printf("poll ended\n%s\npoll console\n", out);
    return 0;
}
/**
 * Answer a command for the testing host: a script for Windows' shell or for
 * WSL's, read from stdin and recorded line by line, or any other command,
 * recorded as given; or, while RELEASE_TEST_DOWN is set, fail as ssh does
 * when the host does not answer.
 */
static int testing_mock(const char *command)
{
    char line[20000];
    /* A host that does not answer, while RELEASE_TEST_DOWN is set. */
    if (*ev("RELEASE_TEST_DOWN")) {
        snprintf(line, sizeof(line), "down %s\n", command);
        append(ev("RELEASE_TEST_CALLS"), line);
        return 255;
    }
    if (strstr(command, "bash.exe -l -s"))
        return windows_mock(command);
    if (strstr(command, "wsl.exe -- bash -s"))
        return wsl_mock();
    /* A script for WSL as root, stop.sh's or -p's preparation's, runs here
     * while RELEASE_TEST_WSL is set, and is recorded as wsl_mock records
     * one otherwise. */
    if (strstr(command, "wsl.exe -u root -- sh -s")) {
        if (!*ev("RELEASE_TEST_WSL"))
            return wsl_mock();
        execl("/bin/sh", "sh", "-s", (char *)NULL);
        return 127;
    }
    snprintf(line, sizeof(line), "ssh testing %s\n", command);
    append(ev("RELEASE_TEST_CALLS"), line);
    return 0;
}

/** Answer the write of the Lenovo's release record as RELEASE_TEST_WRITES
 * says. */
static int release_write(void)
{
    FILE *f;
    char b[4096];
    size_t n;
    const char *how = ev("RELEASE_TEST_WRITES");
    if (!strcmp(how, "no")) {
        append(ev("RELEASE_TEST_CALLS"), "machine write refused\n");
        return 1;
    }
    append(ev("RELEASE_TEST_CALLS"), "machine write release\n");
    f = !strcmp(how, "lost") ? NULL : fopen(rp(".release"), "wb");
    while ((n = fread(b, 1, sizeof(b), stdin)))
        if (f && fwrite(b, 1, n, f) != n) {
            fclose(f);
            return 1;
        }
    if (f)
        fclose(f);
    if (!strcmp(how, "unsynced"))
        puts("sync: error syncing '/etc/rach2/release': Input/output error\nstatus 1");
    else
        puts("status 0");
    return 0;
}
/** Succeed when RELEASE_TEST_SLOTS is `reset` and the release has asked the
 * machine to reboot, which then answers from its recovery system. */
static int rebooted(void)
{
    return !strcmp(ev("RELEASE_TEST_SLOTS"), "reset") &&
           fx_has(ev("RELEASE_TEST_CALLS"), "; reboot\"");
}
/** Print the SHA-256 of FILE as sha256sum does for NAME. */
static int sent_sum(const char *file, const char *name)
{
    struct sha256 s;
    unsigned char digest[32];
    char hex[65], *text = fx_read(file);
    if (!text)
        return 1;
    sha256_init(&s);
    sha256_update(&s, (const unsigned char *)text, strlen(text));
    sha256_final(&s, digest);
    sha256_hex(digest, hex);
    printf("%s  %s\n", hex, name);
    free(text);
    return 0;
}
/**
 * Answer the commands of the install step on a machine whose installer
 * RELEASE_TEST_SLOTS describes: `old`, it has no --slots; `move`, --slots
 * moves the disk to the larger slots; `full`, --slots finds no room; and
 * `reset`, the slots were moved before a reset that left the old root
 * running, whose bootctl cannot arm the trial of recovery, and a reboot
 * comes up in recovery. /dev/shm holds the build's kernel and root, and the
 * installer, once started, ends 1, which ends the row before the trial boot.
 */
static int install_mock(const char *command)
{
    const char *how = ev("RELEASE_TEST_SLOTS");
    char line[20000], b[4096];
    size_t n;
    FILE *f;
    static const char probe[] = "cat /dev/shm/install.status 2> /dev/null\n",
                      installer[] = "setsid sh -c 'rach2-install --console term --update ";
    if (!strncmp(command, probe, strlen(probe))) {
        append(ev("RELEASE_TEST_CALLS"), "machine read\n");
        puts("answered");
        return 0;
    }
    if (!strcmp(command, "cat /dev/shm/install.status 2> /dev/null; true")) {
        append(ev("RELEASE_TEST_CALLS"), "machine read\n");
        puts("1");
        return 0;
    }
    if (!strcmp(command, "sha256sum /dev/shm/root.ext2 /dev/shm/kernel 2> /dev/null")) {
        append(ev("RELEASE_TEST_CALLS"), "machine read\n");
        puts(ROOT_SUM "  /dev/shm/root.ext2\n" KERNEL_SUM "  /dev/shm/kernel");
        return 0;
    }
    if (!strcmp(command, "grep -q -e \"--slots DISK\" /bin/rach2-install")) {
        append(ev("RELEASE_TEST_CALLS"), "machine read\n");
        return !strcmp(how, "old");
    }
    if (!strcmp(command, "rach2-install --slots /dev/disk0")) {
        append(ev("RELEASE_TEST_CALLS"), "machine write rach2-install --slots /dev/disk0\n");
        if (!strcmp(how, "full")) {
            puts("rach2-install: /dev/disk0p1 has no room for the larger slots; the machine stays "
                 "saved to its recovery system");
            return 1;
        }
        puts(!strcmp(how, "reset")
                 ? "rach2-install: /dev/disk0p1 has the slots of this version already"
                 : "rach2-install: moved /dev/disk0p1 to the slots of this version; the saved "
                   "entry is candidate again");
        return 0;
    }
    if (!strcmp(command, "bootctl kernel /dev/shm/kernel")) {
        append(ev("RELEASE_TEST_CALLS"), "machine write bootctl kernel\n");
        puts("bootctl: installed: candidate");
        return 0;
    }
    if (!strcmp(command, "bootctl trial recovery")) {
        append(ev("RELEASE_TEST_CALLS"), "machine write bootctl trial recovery\n");
        if (strcmp(how, "reset"))
            return 0;
        fputs("bootctl: /dev/disk0p1: a file's size does not match its chain\n", stderr);
        return 1;
    }
    if (!strcmp(command, "cat > /dev/shm/bootctl")) {
        append(ev("RELEASE_TEST_CALLS"), "machine write cat > /dev/shm/bootctl\n");
        if ((f = fopen(rp(".bootctl"), "wb")) == NULL)
            return 1;
        while ((n = fread(b, 1, sizeof(b), stdin)))
            fwrite(b, 1, n, f);
        return fclose(f) != 0;
    }
    if (!strcmp(command, "sha256sum /dev/shm/bootctl")) {
        append(ev("RELEASE_TEST_CALLS"), "machine read\n");
        return sent_sum(rp(".bootctl"), "/dev/shm/bootctl");
    }
    if (!strcmp(command, "chmod 755 /dev/shm/bootctl && /dev/shm/bootctl trial recovery")) {
        append(ev("RELEASE_TEST_CALLS"), "machine write /dev/shm/bootctl trial recovery\n");
        return 0;
    }
    if (!strncmp(command, installer, strlen(installer))) {
        append(ev("RELEASE_TEST_CALLS"), "machine write rach2-install --update\n");
        return 0;
    }
    snprintf(line, sizeof(line), "machine write %s\n", command);
    append(ev("RELEASE_TEST_CALLS"), line);
    return 1;
}
static int ssh_mock(int argc, char **argv)
{
    const char *host = argc > 2 ? argv[argc - 2] : "", *command = argc > 1 ? argv[argc - 1] : "";
    char line[20000];
    if (!strcmp(host, "testing"))
        return testing_mock(command);
    if (!strncmp(command, "cat /sys/class/net/eth0/address;",
                 strlen("cat /sys/class/net/eth0/address;"))) {
        append(ev("RELEASE_TEST_CALLS"), "machine read\n");
        if (!strcmp(ev("RELEASE_TEST_LENOVO"), "off"))
            return 255;
        /* The mock's reboot takes no time, so the new boot's uptime is below
         * zero: shorter than the time since the release asked for it. */
        if (rebooted())
            puts("28:d2:44:7e:e0:af\nroot=/dev/disk0p3 rach2.slot=recovery\n-1.00 1.00\n"
                 "identity 0 nvme S6P2NX0T381638Z");
        else
            printf("28:d2:44:7e:e0:af\n%s\n5000.00 9000.00\nidentity 0 nvme S6P2NX0T381638Z\n",
                   *ev("RELEASE_TEST_CMDLINE") ? ev("RELEASE_TEST_CMDLINE")
                                               : "root=/dev/disk0p3 rach2.slot=candidate");
        return 0;
    }
    if (!strncmp(command, "test -f ", 8)) {
        append(ev("RELEASE_TEST_CALLS"), "machine read\n");
        return 1;
    }
    /* While RELEASE_TEST_BOOT is set, the console's login 4711 stands in for
     * term in the boot whose ID is aaaa, and the boot is another once it is
     * `again`. */
    if (strstr(command, "kill -HUP ")) {
        if (!strcmp(ev("RELEASE_TEST_BOOT"), "again") || !strstr(command, "= 'aaaa' ]") ||
            !strstr(command, "= 'login' ]") || !strstr(command, "kill -HUP 4711")) {
            append(ev("RELEASE_TEST_CALLS"), "machine read\n");
            return 1;
        }
        append(ev("RELEASE_TEST_CALLS"), "machine write hup 4711\n");
        return 0;
    }
    if (strstr(command, "(term)")) {
        append(ev("RELEASE_TEST_CALLS"), "machine write stop term\n");
        if (*ev("RELEASE_TEST_BOOT"))
            puts("4711 login aaaa");
        return 0;
    }
    if (!strcmp(command, "bootctl status")) {
        append(ev("RELEASE_TEST_CALLS"), "machine read\n");
        puts("bootctl: next_entry: empty");
        return 0;
    }
    if (!strcmp(command, "bootctl commit")) {
        append(ev("RELEASE_TEST_CALLS"), "machine write bootctl commit\n");
        puts("bootctl: committed: candidate");
        return 0;
    }
    if (!strcmp(command, "bootctl fallback")) {
        append(ev("RELEASE_TEST_CALLS"), "machine write bootctl fallback\n");
        puts(
            "bootctl: fallback: 16777216 bytes written from the committed candidate and read back");
        return 0;
    }
    if (!strcmp(command, "cat /etc/rach2/release 2> /dev/null; echo answered")) {
        char *s = fx_read(rp(".release"));
        append(ev("RELEASE_TEST_CALLS"), "machine read\n");
        fputs(s, stdout);
        free(s);
        puts("answered");
        return 0;
    }
    if (!strncmp(command, "{ mkdir -p /etc/rach2 && cat > /etc/rach2/release.new &&",
                 strlen("{ mkdir -p /etc/rach2 && cat > /etc/rach2/release.new &&")))
        return release_write();
    if (*ev("RELEASE_TEST_SLOTS"))
        return install_mock(command);
    snprintf(line, sizeof(line), "machine write %s\n", command);
    append(ev("RELEASE_TEST_CALLS"), line);
    return 1;
}
/**
 * Answer `git merge-base --is-ancestor A B`: fail where RELEASE_TEST_ANCESTOR
 * says that SHA is not on the branch, `branch`, or lacks master, `master`.
 */
static int git_ancestor(int argc, char **argv)
{
    const char *how = ev("RELEASE_TEST_ANCESTOR");
    if (argc < 5)
        return 1;
    if (!strcmp(how, "branch") && strstr(argv[4], "refs/remotes/origin/milestone/"))
        return 1;
    if (!strcmp(how, "master") && !strcmp(argv[3], "refs/remotes/origin/master"))
        return 1;
    return 0;
}
static int git_mock(int argc, char **argv)
{
    /* The tree's own .git, whose parent holds build/flow/lenovo, the lock;
     * a read the rows do not count among the calls. */
    if (argc == 3 && !strcmp(argv[1], "rev-parse") && !strcmp(argv[2], "--git-common-dir")) {
        puts(".git");
        return 0;
    }
    call("git ", argc, argv, 1);
    if (!*ev("RELEASE_TEST_GIT"))
        return 1;
    if (argc > 2 && !strcmp(argv[1], "-C")) {
        argv += 2;
        argc -= 2;
    }
    if (argc < 2)
        return 1;
    if (!strcmp(argv[1], "fetch") || !strcmp(argv[1], "bundle") || !strcmp(argv[1], "checkout"))
        return 0;
    if (!strcmp(argv[1], "push"))
        return !strcmp(ev("RELEASE_TEST_PUSH"), "fail");
    if (!strcmp(argv[1], "merge-base"))
        return git_ancestor(argc, argv);
    if (!strcmp(argv[1], "rev-parse")) {
        puts(*ev("RELEASE_TEST_MASTER") ? ev("RELEASE_TEST_MASTER")
                                        : "0000000000000000000000000000000000000000");
        return 0;
    }
    if (!strcmp(argv[1], "show") && argc > 2 && strstr(argv[2], ":tests/registry")) {
        puts("# the "
             "suites\nagents|qemu|full||4096|x86_64\nfs|qemu|full\nbigmem|qemu|full||73728|x86_"
             "64\ngnu|qemu|full\nstyle|cppcheck|full");
        return 0;
    }
    if (!strcmp(argv[1], "worktree")) {
        if (argc > 5 && !strcmp(argv[2], "add")) {
            char path[4096];
            snprintf(path, sizeof(path), "%s/tools/dev/testing.sh", argv[5]);
            return fx_text(path, "# native testing command fixture\n") ? 0 : 1;
        }
        return 0;
    }
    return 1;
}
/**
 * Stop the release while a Windows part runs, as RELEASE_TEST_HOLD says:
 * `stop` and `stuck` while the preparation runs, `stuck-cases` while the
 * cases run. The first sleep of the part's poll that windows_detach waits for
 * asks the release's hold.pl, the leader of the process group that the
 * fixture starts, to send the release TERM, as stop.sh -r does, by creating
 * the stop of the run directory that STATE/run names, STATE being the
 * directory `state` beside the calls, and lasts until the release has ended,
 * so that the part is still running when the release stops it.
 */
static void stop_release(void)
{
    const char *hold = ev("RELEASE_TEST_HOLD"), *calls = ev("RELEASE_TEST_CALLS");
    const char *slash = strrchr(calls, '/');
    struct timespec pause = {0, 20000000};
    pid_t release = getpgrp();
    char stop[4200], *dir;
    size_t n;
    int i;
    if (strcmp(hold, "stop") && strcmp(hold, "stuck") && strcmp(hold, "stuck-cases"))
        return;
    if (!fx_has(ev("RELEASE_TEST_CALLS"),
                strcmp(hold, "stuck-cases") ? " run routine-prepare " : " run routine-collect ") ||
        fx_has(ev("RELEASE_TEST_CALLS"), "\nrelease stopped\n"))
        return;
    append(ev("RELEASE_TEST_CALLS"), "release stopped\n");
    snprintf(stop, sizeof(stop), "%.*s/state/run", slash ? (int)(slash - calls) : 1,
             slash ? calls : ".");
    dir = fx_read(stop);
    n = dir ? strcspn(dir, "\n") : 0;
    if (n)
        snprintf(stop, sizeof(stop), "%.*s/stop", (int)n, dir);
    free(dir);
    if (!n)
        return;
    fx_text(stop, "");
    for (i = 0; i < 6000 && kill(release, 0) == 0; i++) {
        /* interval: poll for the release's end every 20 ms */
        nanosleep(&pause, NULL);
    }
}
static unsigned count(const char *path, const char *pattern);
/**
 * Answer winjob.exe for winclean.sh: `state NAME` prints the state of the job
 * NAME, Global\rach2-ID.PART, which the file rach2-ID.PART under
 * RELEASE_TEST_JOBS holds, `active N`, or `absent` when there is none; `stop
 * NAME` is recorded as `winjob stop rach2-ID.PART token=TEXT`, TEXT being
 * what the part's token in the current directory, OUT, then holds, and ends
 * the job, unless RELEASE_TEST_WINCLEAN is `stuck`. Both print `unknown`
 * while RELEASE_TEST_WINCLEAN is `unknown`.
 */
static int winjob_mock(int argc, char **argv)
{
    char path[4200], token[300], line[1000], *text;
    const char *name, *how = ev("RELEASE_TEST_WINCLEAN");
    size_t n;
    if (argc != 3)
        return 2;
    name = strchr(argv[2], '\\');
    name = name ? name + 1 : argv[2];
    snprintf(path, sizeof(path), "%s/%s", ev("RELEASE_TEST_JOBS"), name);
    if (!strcmp(argv[1], "stop")) {
        snprintf(token, sizeof(token), "%s.token", strncmp(name, "rach2-", 6) ? name : name + 6);
        text = fx_read(token);
        n = text ? strcspn(text, "\n") : 0;
        snprintf(line, sizeof(line), "winjob stop %s token=%.*s\n", name, (int)n, text ? text : "");
        append(ev("RELEASE_TEST_CALLS"), line);
        free(text);
        if (strcmp(how, "stuck") && strcmp(how, "unknown"))
            remove(path);
    }
    if (!strcmp(how, "unknown")) {
        puts("unknown");
        return 1;
    }
    text = fx_read(path);
    if (!text || !*text) {
        puts("absent");
        free(text);
        return 0;
    }
    fputs(text, stdout);
    free(text);
    return 1;
}
/**
 * Answer rm: while the calls so far record fewer refusals than
 * RELEASE_TEST_RM says, refuse, as Windows refuses to remove a file that a
 * process still holds, whatever the account; otherwise the system's rm.
 */
static int rm_mock(int argc, char **argv)
{
    if (*ev("RELEASE_TEST_RM") &&
        count(ev("RELEASE_TEST_CALLS"), "^rm refused$") < (unsigned)atoi(ev("RELEASE_TEST_RM"))) {
        append(ev("RELEASE_TEST_CALLS"), "rm refused\n");
        return 1;
    }
    (void)argc;
    argv[0] = "rm";
    execv("/bin/rm", argv);
    return 127;
}
/**
 * Answer sleep at once, recording it: the release's wait of 30 seconds ends
 * the release under RELEASE_TEST_HOLD `wait`, and a wait of 60 stops it as
 * stop_release does.
 */
static int sleep_mock(int argc, char **argv)
{
    call("sleep ", argc, argv, 1);
    if (argc > 1 && !strcmp(argv[1], "30") && !strcmp(ev("RELEASE_TEST_HOLD"), "wait") &&
        fx_has(ev("RELEASE_TEST_CALLS"), "etude-launch alone "))
        kill(getppid(), SIGTERM);
    if (argc > 1 && !strcmp(argv[1], "60"))
        stop_release();
    return 0;
}
int flow_release_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "sh"))
        return sh_mock(argc, argv);
    if (!strcmp(name, "gh"))
        return gh_mock(argc, argv);
    if (!strcmp(name, "ssh"))
        return ssh_mock(argc, argv);
    if (!strcmp(name, "git"))
        return git_mock(argc, argv);
    if (!strcmp(name, "make") || !strcmp(name, "scp")) {
        call(!strcmp(name, "make") ? "make " : "scp ", argc, argv, 1);
        if (!strcmp(name, "make") && *ev("RELEASE_TEST_DETACH") && argc > 1 &&
            !strcmp(argv[1], "build/tools/etude/etude"))
            release_ended();
        return 0;
    }
    if (!strcmp(name, "etude")) {
        char prefix[9000];
        snprintf(prefix, sizeof(prefix), "etude site=%s clean=%s export=%s ", ev("ETUDE_SITE"),
                 ev("ETUDE_PREPARATION_CLEAN"), ev("ETUDE_PHASE_SELECTION_EXPORT"));
        call(prefix, argc, argv, 1);
        return 0;
    }
    if (!strcmp(name, "native-deploy")) {
        char logs[4200];
        call("native-deploy ", argc, argv, 1);
        if (argc < 2 || strcmp(argv[1], "run") || !strcmp(ev("RELEASE_TEST_NATIVE"), "pass"))
            return 0;
        if (argc > 9) {
            snprintf(logs, sizeof(logs), "%s/collection-2.out", argv[9]);
            mkdir(argv[9], 0700);
            fx_text(logs,
                    "native: credprobe: pass; evidence e\nnative: dirprobe.ext2: failed: exited "
                    "1\nnative: result: 2 cases, 1 failed or incomplete\n");
        }
        return 1;
    }
    if (!strcmp(name, "sleep"))
        return sleep_mock(argc, argv);
    if (!strcmp(name, "winjob"))
        return winjob_mock(argc, argv);
    if (!strcmp(name, "rm"))
        return rm_mock(argc, argv);
    return 97;
}
struct release {
    struct fixture *f;
    char tree[4096], script[4096], retired[4096];
    char head[2000], built[3000], installing[4000], tried[5000], accepted[6000], finished[7000],
        ended[2000];
};
struct release_row {
    const char *record, *sha, *writes, *runs, *git, *accept, *machines, *windows, *native,
        *failures, *milestone, *ancestor, *master, *push, *lenovo, *prepare, *hold, *testing,
        *cmdline, *open, *slots;
    const char *detach; /* the Mac's run waits until the release has ended */
    const char *again;  /* the machine of -a, or NULL for a release */
    int rerun, preparing, only;
};

static int setup(struct release *r, struct fixture *f)
{
    char path[4096], source[200];
    const char *windows[] = {"winclean.sh", "winjob.sh", "winjob.c", "bound.pl",
                             "claim.pl",    "hold.pl",   "procs.pl"};
    unsigned i;
    const char *mock[] = {"sh", "gh", "ssh", "git", "make", "sleep", "scp", "winjob", "rm"};
    memset(r, 0, sizeof(*r));
    r->f = f;
    snprintf(r->tree, sizeof(r->tree), "%s/tree", f->work);
    snprintf(r->script, sizeof(r->script), "%s/tools/flow/release.sh", r->tree);
    if (!fx_copy(flow_source(f, "tools/flow/release.sh"), r->script))
        return 0;
    snprintf(path, sizeof(path), "%s/tools/ci/site.sh", r->tree);
    if (!fx_copy(flow_source(f, "tools/ci/site.sh"), path))
        return 0;
    /* windows_clean feeds the tree's winclean.sh to Windows' shell, after
     * the lines of winjob.sh, which hold winjob.c; the claims feed claim.pl
     * to WSL's; bound.pl bounds each; hold.pl holds the release and its
     * jobs; prepare_host feeds procs.pl to WSL's shell. */
    for (i = 0; i < sizeof(windows) / sizeof(windows[0]); i++) {
        snprintf(path, sizeof(path), "%s/tools/flow/%s", r->tree, windows[i]);
        snprintf(source, sizeof(source), "tools/flow/%s", windows[i]);
        if (!fx_copy(flow_source(f, source), path))
            return 0;
    }
    flow_put(f, "tree/krffs/rach2_demo_ed25519", "");
    flow_put(f, "tree/tools/flow/board.sh", "# native board fixture\n");
    flow_put(f, "tree/tools/flow/sweep.sh", "# native sweep fixture\n");
    if (!fx_mkdir(flow_path(f, "tree/.git")) || !fx_mkdir(flow_path(f, "tree/build/flow")))
        return 0;
    for (i = 0; i < sizeof(mock) / sizeof(mock[0]); i++)
        if (!fx_mock(f, mock[i]))
            return 0;
    fx_env("FLOW_RELEASE", "1");
    snprintf(r->head, sizeof(r->head),
             "milestone 7\nsha %s\nbranch %s\nchosen %s\ndispatched 1760054400\nrun 101 "
             "success\nfull done %s run 101\ntouched %s\n",
             H, BRANCH, T, T, T);
    snprintf(r->built, sizeof(r->built),
             "%sbuild done %s kernel k root r\nrecovery done %s kernel k image i\ntransfer done %s "
             "kernel and root in /dev/shm\nkernel done %s candidate slot\n",
             r->head, T, T, T, T);
    snprintf(r->installing, sizeof(r->installing), "%sinstall started %s\n", r->built, T);
    snprintf(
        r->tried, sizeof(r->tried),
        "%sinstall done %s root r\ntrial done %s the new kernel runs from the candidate slot\n",
        r->installing, T, T);
    snprintf(r->accepted, sizeof(r->accepted),
             "%saccept done %s metal-accept: PASS\ncommit done %s committed and copied to the "
             "fallback slot\n",
             r->tried, T, T);
    snprintf(r->finished, sizeof(r->finished), "%srecord done %s /etc/rach2/release names %s\n",
             r->accepted, T, H);
    snprintf(r->ended, sizeof(r->ended),
             "milestone 7\nsha %s\nbranch %s\nchosen %s\ntouched %s\nfailed build: the build "
             "failed\n",
             H, BRANCH, T, T);
    return 1;
}
/** Set the environment variable NAME to VALUE, or to the empty string when
 * VALUE is NULL. */
static void spec_env(const char *name, const char *value)
{
    fx_env(name, value ? value : "");
}
static void run(struct release *r, const struct release_row *spec)
{
    struct fixture *f = r->f;
    const char *files[] = {"calls", "calls.release", "calls.bootctl"};
    const char *a[10] = {"/bin/sh", r->script, "-s", flow_path(f, "state")};
    unsigned i, n = 4;
    fx_remove(flow_path(f, "state"));
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        fx_remove(flow_path(f, files[i]));
    fx_remove(flow_path(f, "tree/build/flow/lenovo"));
    flow_put(f, "state/wt/krffs/rach2_demo_ed25519", "");
    flow_put(f, "state/wt/build/x86_64/images/release.sha256",
             spec->slots ? KERNEL_SUM "  rach2/kernel\n" ROOT_SUM "  images/os-real-gnu.ext2\n"
                         : "");
    flow_put(f, "state/wt/build/x86_64/utils/sbin/bootctl", BOOTCTL);
    flow_put(f, "state/wt/tests/system/accept/metal_accept.sh", "# native acceptance fixture\n");
    flow_put(f, "calls", "");
    flow_put(f, "tree/tools/flow/machines", spec->machines ? spec->machines : "# none\n");
    if (spec->record)
        flow_put(f, "state/record", spec->record);
    flow_put(f, "before", spec->record ? spec->record : "");
    fx_env("RELEASE_TEST_CALLS", flow_path(f, "calls"));
    fx_env("RELEASE_TEST_WRITES", spec->writes ? spec->writes : "no");
    spec_env("RELEASE_TEST_RUNS", spec->runs);
    spec_env("RELEASE_TEST_GIT", spec->git);
    spec_env("RELEASE_TEST_ACCEPT", spec->accept);
    spec_env("RELEASE_TEST_WINDOWS", spec->windows);
    spec_env("RELEASE_TEST_NATIVE", spec->native);
    spec_env("RELEASE_TEST_FAILURES", spec->failures);
    spec_env("RELEASE_TEST_MILESTONE", spec->milestone);
    spec_env("RELEASE_TEST_ANCESTOR", spec->ancestor);
    spec_env("RELEASE_TEST_MASTER", spec->master);
    spec_env("RELEASE_TEST_PUSH", spec->push);
    spec_env("RELEASE_TEST_LENOVO", spec->lenovo);
    spec_env("RELEASE_TEST_PREPARE", spec->prepare);
    spec_env("RELEASE_TEST_HOLD", spec->hold);
    spec_env("RELEASE_TEST_TESTING", spec->testing);
    spec_env("RELEASE_TEST_CMDLINE", spec->cmdline);
    spec_env("RELEASE_TEST_OPEN", spec->open);
    spec_env("RELEASE_TEST_SLOTS", spec->slots);
    spec_env("RELEASE_TEST_DETACH", spec->detach);
    if (spec->native) {
        flow_put(f, "state/wt/build/tools/dev/native-hostpeer", "");
        fx_copy(f->self, flow_path(f, "state/wt/build/tools/dev/native-deploy"));
    }
    if (spec->rerun)
        a[n++] = "-r";
    if (spec->preparing)
        a[n++] = "-p";
    if (spec->only)
        a[n++] = "-o";
    if (spec->again) {
        a[n++] = "-a";
        a[n++] = spec->again;
    }
    a[n++] = "7";
    a[n++] = spec->sha ? spec->sha : H;
    a[n] = NULL;
    fx_run(f, a, r->tree, NULL, NULL);
}
static int called(struct release *r, const char *pattern)
{
    char *s = fx_read(flow_path(r->f, "calls"));
    int held = fx_regex(s, pattern);
    free(s);
    return held;
}
/** Succeed when `first` is called, and before `second` when that is given. */
static int before(struct release *r, const char *first, const char *second)
{
    char *s = fx_read(flow_path(r->f, "calls")), *a = strstr(s, first), *b;
    int held;
    b = second ? strstr(s, second) : NULL;
    held = a && (!second || (b && a < b));
    free(s);
    return held;
}
static int archived(struct release *r, const char *prefix)
{
    DIR *d = opendir(flow_path(r->f, "state"));
    struct dirent *e;
    unsigned count = 0;
    if (!d)
        return 0;
    while ((e = readdir(d)))
        if (!strncmp(e->d_name, prefix, strlen(prefix)) && !strstr(e->d_name, ".sha256") &&
            !strstr(e->d_name, ".sweep")) {
            snprintf(r->retired, sizeof(r->retired), "%s/state/%s", r->f->work, e->d_name);
            count++;
        }
    closedir(d);
    return count == 1;
}
static char *last_line(const char *path)
{
    char *s = fx_read(path), *end, *at;
    if (!s)
        return strdup("");
    end = s + strlen(s);
    while (end > s && end[-1] == '\n')
        *--end = 0;
    at = strrchr(s, '\n');
    at = strdup(at ? at + 1 : s);
    free(s);
    return at;
}
static int last_is(const char *path, const char *text)
{
    char *line = last_line(path);
    int held = !strcmp(line, text);
    free(line);
    return held;
}
/** Succeed when the last line the script printed is `text`. */
static int ended_with(struct release *r, const char *text)
{
    const char *out = r->f->out ? r->f->out : "";
    size_t n = strlen(out), m = strlen(text);
    while (n && out[n - 1] == '\n')
        n--;
    return n >= m && !strncmp(out + n - m, text, m) && (n == m || out[n - m - 1] == '\n');
}
static unsigned count(const char *path, const char *pattern)
{
    char *s = fx_read(path), *line, *save = NULL;
    unsigned n = 0;
    for (line = strtok_r(s, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        if (fx_regex(line, pattern))
            n++;
    free(s);
    return n;
}
static int check(struct release *r, int held, const char *label)
{
    return fx_check(r->f, held, label);
}
static const char *record(struct release *r)
{
    return flow_path(r->f, "state/record");
}
static int finished_rows(struct release *r)
{
    struct fixture *f = r->f;
    char *line;
    int ok = 1, held;
    run(r, &(struct release_row){.record = r->finished, .git = "yes"});
    {
        char sum[4200];
        held = !f->status && archived(r, "7-11111111-");
        snprintf(sum, sizeof(sum), "%s.sha256", r->retired);
        line = last_line(r->retired);
        held = held && !strncmp(line, "closed ", 7) && count(r->retired, " done ") == 10 &&
               count(r->retired, "^closing ") == 1 &&
               before(r, "push origin " H ":refs/heads/master\n",
                      "gh api -X PATCH repos/toksaitov/rach2/milestones/7 -f state=closed\n") &&
               before(r, "milestones/7 -f state=closed\n", "board archive 7\n") && fx_exists(sum) &&
               called(r, "^git -C .* worktree remove --force .*/state/wt$") &&
               !called(r, "^(machine|ssh|gh run|gh workflow|gh issue|dispatch)") &&
               !fx_exists(record(r)) && ended_with(r, "release: passed " H);
        free(line);
        ok = check(r, held, "a finished release pushes master, closes and archives") && ok;
    }
    {
        char also[9000];
        snprintf(also, sizeof(also), "milestone 7\nalso 8\n%s", strchr(r->finished, '\n') + 1);
        run(r, &(struct release_row){.record = also, .git = "yes"});
        ok =
            check(r,
                  !f->status &&
                      before(
                          r, "milestones/7 -f state=closed\n",
                          "gh api -X PATCH repos/toksaitov/rach2/milestones/8 -f state=closed\n") &&
                      before(r, "milestones/8 -f state=closed\n", "board archive 7\n") &&
                      before(r, "board archive 7\n", "board archive 8\n") &&
                      ended_with(r, "release: passed " H),
                  "a release with -c closes and archives both milestones") &&
            ok;
    }
    run(r, &(struct release_row){.record = r->finished, .git = "yes", .master = H});
    return check(r, !f->status && !called(r, " push ") && called(r, "^board archive 7$"),
                 "master already at the commit is not pushed again") &&
           ok;
}
/** A milestone with an open issue other than the release's cards stays open. */
static int open_rows(struct release *r)
{
    struct fixture *f = r->f;
    int ok = 1;
    char cards[9000];
    snprintf(cards, sizeof(cards), "%scard release 40\ncard close 41\n", r->finished);
    run(r, &(struct release_row){.record = cards, .git = "yes", .open = "7:41 12 40 9"});
    ok = check(r,
               !f->status && called(r, " push origin " H ":") &&
                   !called(r, "^gh api -X PATCH repos/toksaitov/rach2/milestones/7 ") &&
                   called(r, "^board archive 7$") &&
                   ended_with(r, "release: passed " H "; milestone 7 stays open for #9 #12"),
               "a milestone with an open issue stays open and its issues are named") &&
         ok;
    run(r, &(struct release_row){.record = cards, .git = "yes", .open = "7:40 41"});
    ok = check(r,
               !f->status && called(r, "^gh api -X PATCH repos/toksaitov/rach2/milestones/7 ") &&
                   ended_with(r, "release: passed " H),
               "the release's own open cards do not keep the milestone open") &&
         ok;
    snprintf(cards, sizeof(cards), "milestone 7\nalso 8\n%s", strchr(r->finished, '\n') + 1);
    run(r, &(struct release_row){.record = cards, .git = "yes", .open = "8:5"});
    ok = check(r,
               !f->status && called(r, "^gh api -X PATCH repos/toksaitov/rach2/milestones/7 ") &&
                   !called(r, "^gh api -X PATCH repos/toksaitov/rach2/milestones/8 ") &&
                   called(r, "^board archive 8$") &&
                   ended_with(r, "release: passed " H "; milestone 8 stays open for #5"),
               "a joined milestone with an open issue stays open while M closes") &&
         ok;
    run(r, &(struct release_row){.record = r->finished, .git = "yes", .open = "7:p13"});
    ok = check(r,
               !f->status && called(r, "^gh api -X PATCH repos/toksaitov/rach2/milestones/7 ") &&
                   ended_with(r, "release: passed " H),
               "an open pull request does not keep the milestone open") &&
         ok;
    snprintf(cards, sizeof(cards), "%sclosing %s\n", r->finished, T);
    run(r, &(struct release_row){.record = cards, .git = "yes", .open = "7:garbage"});
    ok = check(r,
               f->status == 75 && fx_same(flow_path(f, "before"), record(r)) &&
                   !called(r, "^gh api -X PATCH repos/toksaitov/rach2/milestones/7 "),
               "an answer that is not a list of issues waits with the record unchanged") &&
         ok;
    run(r, &(struct release_row){.record = r->finished, .git = "yes", .open = "7:retry"});
    ok = check(r,
               !f->status && count(flow_path(f, "calls"), "^gh api --paginate ") == 2 &&
                   called(r, "^gh api -X PATCH repos/toksaitov/rach2/milestones/7 ") &&
                   ended_with(r, "release: passed " H),
               "a failed query's output is not kept when its retry finds no open issue") &&
         ok;
    return ok;
}
static int closing_rows(struct release *r)
{
    struct fixture *f = r->f;
    char closing[9000], *line;
    int ok = 1, held;
    snprintf(closing, sizeof(closing), "%sclosing %s\n", r->finished, T);
    run(r, &(struct release_row){.record = closing, .git = "yes", .push = "fail"});
    ok = check(r,
               f->status == 75 && fx_same(flow_path(f, "before"), record(r)) &&
                   !called(r, "^gh api -X PATCH") && !called(r, "^board "),
               "a failed push waits with the record unchanged") &&
         ok;
    {
        char changed[9000];
        snprintf(changed, sizeof(changed), "milestone 6\n%s", strchr(closing, '\n') + 1);
        run(r, &(struct release_row){.record = changed, .sha = C, .git = "yes"});
        held = !f->status && archived(r, "6-11111111-");
        line = last_line(r->retired);
        ok = check(r,
                   held && !fx_exists(record(r)) && !strncmp(line, "closed ", 7) &&
                       called(r, "^gh api -X PATCH repos/toksaitov/rach2/milestones/6 ") &&
                       called(r, "^board archive 6$") && called(r, " push origin " H ":") &&
                       !called(r, "^(machine|dispatch)") && ended_with(r, "release: passed " H),
                   "a closing release of another milestone is finished first") &&
             ok;
        free(line);
    }
    run(r, &(struct release_row){.record = r->head});
    ok = check(r,
               f->status == 75 && count(record(r), "^full done ") == 1 &&
                   called(r, "^git -C .* fetch") && !called(r, "^(machine write|gh run|dispatch)"),
               "full run not repeated") &&
         ok;
    return ok;
}
static int argument_rows(struct release *r)
{
    struct fixture *f = r->f;
    int ok = 1;
    run(r, &(struct release_row){.git = "yes", .ancestor = "branch"});
    ok = check(r, f->status == 2 && !fx_exists(record(r)) && strstr(f->err, "is not on " BRANCH),
               "a commit that is not on the milestone's branch is refused") &&
         ok;
    run(r, &(struct release_row){.git = "yes", .ancestor = "master"});
    ok = check(r,
               f->status == 2 && !fx_exists(record(r)) &&
                   strstr(f->err, "does not contain origin/master"),
               "a commit that lacks master is refused") &&
         ok;
    run(r, &(struct release_row){.git = "yes", .milestone = "closed"});
    ok = check(r,
               f->status == 2 && !fx_exists(record(r)) && strstr(f->err, "names no branch") &&
                   called(r, "^board milestone 7$"),
               "a closed milestone is refused") &&
         ok;
    run(r, &(struct release_row){.git = "yes", .milestone = "lost"});
    ok = check(r, f->status == 75 && !fx_exists(record(r)) && !called(r, "^(machine|dispatch)"),
               "a milestone that cannot be read waits") &&
         ok;
    run(r, &(struct release_row){.sha = "1111", .git = "yes"});
    ok = check(r, f->status == 2 && !fx_exists(record(r)) && fx_equal(flow_path(f, "calls"), ""),
               "a short hash is refused") &&
         ok;
    run(r, &(struct release_row){.git = "yes", .rerun = 1});
    ok = check(r, f->status == 2 && !fx_exists(record(r)) && strstr(f->err, "-r: "),
               "-r without an earlier release is refused") &&
         ok;
    return ok;
}
static int new_record_rows(struct release *r)
{
    struct fixture *f = r->f;
    char expected[1000];
    int ok = 1;
    run(r, &(struct release_row){.record = r->ended, .sha = C, .git = "yes"});
    ok = check(r,
               f->status == 75 && archived(r, "7-11111111-") &&
                   fx_same(flow_path(f, "before"), r->retired) && !called(r, "^machine write") &&
                   count(record(r), ".") == 5 &&
                   fx_has(record(r), "milestone 7\nsha " C "\nbranch " BRANCH "\n") &&
                   last_is(record(r), "touched " T),
               "an ended record is copied aside and keeps touched") &&
         ok;
    run(r, &(struct release_row){.record = r->ended, .sha = C, .git = "yes", .runs = "none"});
    snprintf(expected, sizeof(expected),
             "^dispatch workflow run full.yml --repo toksaitov/rach2 --ref " BRANCH
             " -f sha=%s -f title=Release tests of 22222222 for milestone 7$",
             C);
    ok = check(r,
               f->status == 75 && called(r, expected) &&
                   count(flow_path(f, "calls"), "^dispatch ") == 1,
               "a new release runs every suite on the milestone's workflow") &&
         ok;
    return ok;
}
static int interrupted_install_rows(struct release *r)
{
    struct fixture *f = r->f;
    char failed[9000];
    int ok = 1;
    snprintf(failed, sizeof(failed), "%sfailed install: rach2-install ended 1: no space\n",
             r->installing);
    run(r, &(struct release_row){.record = failed, .sha = C, .git = "yes"});
    ok = check(r,
               f->status == 1 && fx_same(flow_path(f, "before"), record(r)) && !archived(r, "7-") &&
                   fx_equal(flow_path(f, "calls"), "") &&
                   (strstr(f->out, "delete the record's `install started` line") ||
                    strstr(f->err, "delete the record's `install started` line")),
               "failed install requires manual record repair") &&
         ok;
    run(r, &(struct release_row){.record = r->installing});
    ok = check(r,
               f->status == 1 &&
                   last_is(record(r),
                           "failed install: the machine runs system candidate after the installer "
                           "was started, so a reboot came before the release saw its result; check "
                           "the root by hand") &&
                   !called(r, "^(machine write|board)"),
               "install reboot does not write machine") &&
         ok;
    return ok;
}
static int machine_record_rows(struct release *r)
{
    struct fixture *f = r->f;
    unsigned i;
    const char *writes[] = {"yes", "lost", "unsynced"};
    int ok = interrupted_install_rows(r);
    for (i = 0; i < 3; i++) {
        run(r, &(struct release_row){.record = r->accepted, .git = "yes", .writes = writes[i]});
        if (!i) {
            ok = check(r,
                       !f->status && archived(r, "7-11111111-") &&
                           fx_equal(flow_path(f, "calls.release"),
                                    "commit " H "\nmilestone 7\nfull 101\nroot \n") &&
                           count(r->retired, "^record done ") == 1 &&
                           before(r, "machine write release\n", " push origin "),
                       "the machine's record is written before master moves") &&
                 ok;
        } else {
            const char *last =
                i == 1 ? "failed record: /etc/rach2/release on the machine does not name " H ": "
                       : "failed record: writing /etc/rach2/release failed: sync: error syncing "
                         "'/etc/rach2/release': Input/output error status 1";
            ok = check(r,
                       f->status == 1 && last_is(record(r), last) &&
                           !fx_has(record(r), "record done ") && !called(r, " push ") &&
                           !called(r, "^gh api -X PATCH"),
                       i == 1 ? "lost read-back blocks closing" : "failed flush blocks closing") &&
                 ok;
        }
    }
    return ok;
}
#define MAC "mac accel tcg\nmac env JOBS=8\n"
#define WINDOWS                                                                                \
    "windows accel whpx\nwindows tree D:/w\nwindows out D:/o\nwindows site C:/s\nwindows env " \
    "JOBS=30\nwindows path /ucrt64/bin\n"
#define LENOVO                                                                              \
    "lenovo target /var/native\nlenovo peer 192.168.0.100\nlenovo echo-port 55090\nlenovo " \
    "network "                                                                              \
    "192.168.0.106/24\nlenovo interface eth0\nlenovo gateway 192.168.0.1\nlenovo far "      \
    "1.1.1.1\nlenovo empty 192.168.0.250\nlenovo silent 192.168.0.252\n"
#define MAC_FAILED "milestone7-111111111111-mac-1029-000000"
static int machine_line(struct release *r, const char *prefix, char *rest, size_t size)
{
    char *s = fx_read(record(r)), *line, *save = NULL;
    int found = 0;
    for (line = strtok_r(s, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        if (!strncmp(line, prefix, strlen(prefix))) {
            snprintf(rest, size, "%s", line + strlen(prefix));
            found = 1;
        }
    free(s);
    return found;
}
/**
 * Wait for the end of the run that a hold.pl holds in the run directory DIR,
 * one that the release started detached: until that hold.pl has released
 * DIR/live, which it holds while anything of the run's group runs. The wait
 * has no clock of its own, so that a slow machine's run is judged whole.
 */
static void held_ended(const char *dir)
{
    char live[4300], end[4300];
    int fd;
    snprintf(live, sizeof(live), "%s/live", dir);
    snprintf(end, sizeof(end), "%s/end", dir);
    while (fx_exists(dir) && !fx_exists(live) && !fx_exists(end))
        fx_pause();
    fd = open(live, O_RDONLY);
    if (fd >= 0) {
        flock(fd, LOCK_SH);
        close(fd);
    }
}
/**
 * Find the Mac run that the record says started, copy its identity to `id`,
 * wait for its end, as held_ended does for the run directory that the Mac's
 * run file names for it, and succeed when the run's times file records its
 * end.
 */
static int mac_ran(struct release *r, char *id, size_t size)
{
    char path[4200], dir[4200] = "", *run;
    if (!machine_line(r, "mac started ", id, size))
        return 0;
    snprintf(path, sizeof(path), "%s/tree/build/machines/mac/runs/%s.times", r->f->work, id);
    run = fx_read(flow_path(r->f, "tree/build/machines/mac/run"));
    if (run && strchr(run, ' ') && !strncmp(strchr(run, ' ') + 1, id, strlen(id)))
        snprintf(dir, sizeof(dir), "%.*s", (int)strcspn(run, " "), run);
    free(run);
    if (*dir)
        held_ended(dir);
    return fx_exists(path) && fx_has(path, "\nend ");
}
/** Make the Mac's copy and admission site, with etude's build in the copy. */
static int mac_tree(struct release *r)
{
    struct fixture *f = r->f;
    fx_remove(flow_path(f, "tree/build/machines"));
    return flow_put(f, "tree/build/machines/mac/tree/Readme.md", "") &&
           flow_put(f, "tree/build/machines/mac/site/etude-site", "etude-site 2\n") &&
           flow_put(f, "tree/build/machines/mac/site/bin/Readme.md", "") &&
           flow_put(f, "tree/build/machines/mac/tree/build/tools/etude/etude-volume", "") &&
           fx_copy(f->self, flow_path(f, "tree/build/machines/mac/tree/build/tools/etude/etude"));
}
static int rerun_rows(struct release *r)
{
    struct fixture *f = r->f;
    char old[4000], expected[1000];
    int ok = 1;
    snprintf(old, sizeof(old),
             "milestone 7\nsha %s\nbranch %s\nchosen %s\ntouched %s\ndispatched 1760054400\nrun "
             "101 failure\nfailure full 101 x86_64:agents x86_64:bigmem\nmac done %s run m\nfailed "
             "tests: full run 101: x86_64:agents x86_64:bigmem\n",
             H, BRANCH, T, T, T);
    run(r, &(struct release_row){
               .record = old, .sha = C, .git = "yes", .runs = "none", .machines = MAC, .rerun = 1});
    snprintf(expected, sizeof(expected),
             "^dispatch workflow run full.yml --repo toksaitov/rach2 --ref " BRANCH
             " -f sha=%s -f title=Release tests of 22222222 for milestone 7: again only agents on "
             "x86_64, bigmem on x86_64 -f suites=x86_64:agents x86_64:bigmem$",
             C);
    ok = check(r,
               f->status == 75 && fx_has(record(r), "sha " C "\n") &&
                   fx_has(record(r), "\nagain full x86_64:agents x86_64:bigmem\n") &&
                   count(record(r), "^mac done ") == 1 && !fx_has(record(r), "run 101") &&
                   !fx_has(record(r), "mac started") && called(r, expected),
               "-r keeps the Mac's pass and runs only the failed suites") &&
         ok;
    snprintf(old, sizeof(old),
             "milestone 7\nsha %s\nbranch %s\nchosen %s\ntouched %s\nrun 101 failure\nfailure "
             "full 101 x86_64:agents ia32:fs\nfailed tests: full run 101: x86_64:agents "
             "ia32:fs\n",
             H, BRANCH, T, T);
    run(r,
        &(struct release_row){.record = old, .sha = C, .git = "yes", .runs = "none", .rerun = 1});
    ok = check(r,
               f->status == 75 && fx_has(record(r), "\nagain full x86_64:agents ia32:fs\n") &&
                   called(r, "-f title=Release tests of 22222222 for milestone 7: again only "
                             "agents on x86_64, fs on ia32 -f suites=x86_64:agents ia32:fs$"),
               "-r runs again only the failed suites of the ia32 profile") &&
         ok;
    snprintf(old, sizeof(old),
             "milestone 7\nsha %s\nbranch %s\nchosen %s\ntouched %s\nrun 101 failure\nfailure "
             "full 101 ia32:agents\nfailed tests: full run 101: ia32:agents\n",
             H, BRANCH, T, T);
    run(r,
        &(struct release_row){.record = old, .sha = C, .git = "yes", .runs = "none", .rerun = 1});
    ok = check(r,
               f->status == 75 && !fx_has(record(r), "again ") &&
                   called(r, "-f title=Release tests of 22222222 for milestone 7$"),
               "-r runs every suite again after an ia32 word that is no i686 suite") &&
         ok;
    snprintf(old, sizeof(old),
             "milestone 7\nsha %s\nbranch %s\nchosen %s\ntouched %s\nrun 101 failure\nfailure "
             "full 101 x86_64:agents x86_64:runner\nfailed tests: full run 101: x86_64:agents "
             "x86_64:runner\n",
             H, BRANCH, T, T);
    run(r,
        &(struct release_row){.record = old, .sha = C, .git = "yes", .runs = "none", .rerun = 1});
    ok = check(r,
               f->status == 75 && !fx_has(record(r), "again ") &&
                   called(r, "-f title=Release tests of 22222222 for milestone 7$"),
               "-r runs every suite again after a failure that is no suite") &&
         ok;
    snprintf(old, sizeof(old),
             "milestone 7\nsha %s\nbranch %s\nchosen %s\nagain full x86_64:agents\ntouched "
             "%s\nrun 101 failure\nfailure full 101 runner\nfailed tests: full run 101: "
             "runner\n",
             H, BRANCH, T, T);
    run(r,
        &(struct release_row){.record = old, .sha = C, .git = "yes", .runs = "none", .rerun = 1});
    ok = check(r,
               f->status == 75 && fx_has(record(r), "\nagain full x86_64:agents\n") &&
                   called(r, " -f suites=x86_64:agents$"),
               "-r runs again a rerun's suites after a failure that is no suite") &&
         ok;
    snprintf(old, sizeof(old),
             "milestone 7\nsha %s\nbranch %s\nchosen %s\ntouched %s\nrun 101 failure\nfailure "
             "full 101 x86_64:agents\nwindows done %s run w\nfailed tests: full run 101: "
             "x86_64:agents\n",
             H, BRANCH, T, T, T);
    run(r, &(struct release_row){.record = old, .sha = C, .git = "yes", .runs = "none"});
    ok = check(r,
               f->status == 75 && !fx_has(record(r), "again ") && !fx_has(record(r), " done ") &&
                   called(r, "-f title=Release tests of 22222222 for milestone 7$"),
               "without -r everything runs again") &&
         ok;
    return ok;
}
static int machine_rerun_rows(struct release *r)
{
    struct fixture *f = r->f;
    char old[4000], id[200];
    int ok = 1;
    if (!mac_tree(r))
        return 0;
    snprintf(old, sizeof(old),
             "%smac started %s\nfailure mac %s i686:gnu x86_64:gnu\nfailed tests: mac run %s: "
             "i686:gnu x86_64:gnu\n",
             r->head, MAC_FAILED, MAC_FAILED, MAC_FAILED);
    run(r,
        &(struct release_row){.record = old, .sha = C, .git = "yes", .machines = MAC, .rerun = 1});
    ok = check(r,
               f->status == 75 && fx_has(record(r), "\nagain mac i686:gnu x86_64:gnu\n") &&
                   count(record(r), "^full done ") == 1 &&
                   count(record(r), "^run 101 success$") == 1 && mac_ran(r, id, sizeof(id)) &&
                   !strncmp(id, "milestone7-222222222222-mac-", 28) && !called(r, "^dispatch ") &&
                   called(r, "^etude .* run routine-sequence phase_first_run .* tcg i686:gnu "
                             "x86_64:gnu$") &&
                   called(r, "^testing "),
               "-r runs again only the Mac's failures and keeps the full pass") &&
         ok;
    snprintf(old, sizeof(old),
             "%slenovo started l\nfailure lenovo l dirprobe.ext2\nfailed tests: lenovo run l: "
             "dirprobe.ext2\n",
             r->tried);
    run(r, &(struct release_row){
               .record = old, .sha = C, .git = "yes", .machines = LENOVO, .rerun = 1});
    return check(r,
                 f->status == 75 && fx_has(record(r), "\nagain lenovo dirprobe.ext2\n") &&
                     count(record(r), "^full done ") == 1 && !fx_has(record(r), "trial done ") &&
                     !fx_has(record(r), "lenovo done ") && called(r, "^testing .* run ") &&
                     called(r, "^ +ARCH=x86_64 make native-assets && cd "),
                 "-r runs again only the native failure on a new install") &&
           ok;
}
static int build_rows(struct release *r)
{
    struct fixture *f = r->f;
    char expected[1000];
    int ok;
    run(r, &(struct release_row){.record = r->head, .git = "yes"});
    snprintf(expected, sizeof(expected),
             "^testing %s/rach2/build/worktrees/release run \\{ cp /etc/rach2/doom1.wad "
             "ports/doom/doom1.wad \\|\\|$",
             FLOW_OWNER_HOME);
    ok = check(r,
               f->status == 75 && !fx_has(record(r), "build done ") && called(r, expected) &&
                   called(r, "^ +sh ports/doom/fetch_wad.sh && "
                             "ARCH=x86_64 make machine GNU_AGENTS=all && ") &&
                   !called(r, "make clean"),
               "machine build takes the host's WAD and builds incrementally, with no make "
               "clean");
    run(r, &(struct release_row){.record = r->head, .git = "yes", .testing = "pass"});
    return check(
               r,
               f->status == 1 && !fx_has(record(r), "build done ") &&
                   called(r,
                          "^ +sha256sum rach2/kernel rach2-recovery/kernel utils/sbin/bootctl ") &&
                   called(r,
                          "^testing .* get build/x86_64/rach2/kernel "
                          "build/x86_64/rach2-recovery/kernel build/x86_64/utils/sbin/bootctl ") &&
                   last_is(record(r), "failed build: the root did not arrive whole"),
               "the build sums and fetches its bootctl, which the recovery trial may need") &&
           ok;
}
/** The trial boot goes on only with the slot and the console it was given. */
static int trial_rows(struct release *r)
{
    struct fixture *f = r->f;
    char installed[6000];
    int ok = 1;
    snprintf(installed, sizeof(installed), "%sinstall done %s root r\n", r->installing, T);
    run(r, &(struct release_row){.record = installed,
                                 .git = "yes",
                                 .writes = "yes",
                                 .accept = "pass",
                                 .cmdline = "root=/dev/disk0p3 rach2.slot=candidate "
                                            "rach2.console=term"});
    ok = check(r,
               !f->status && archived(r, "7-11111111-") &&
                   count(r->retired, "^trial done .* the new kernel runs from the candidate slot "
                                     "with term as its console$") == 1 &&
                   before(r, "identity ", "accept 192.168.0.106\n") &&
                   before(r, "accept 192.168.0.106\n", "machine write bootctl commit\n"),
               "a trial boot with the candidate slot and term goes on to its commit") &&
         ok;
    run(r, &(struct release_row){.record = installed,
                                 .git = "yes",
                                 .writes = "yes",
                                 .accept = "pass",
                                 .cmdline = "root=/dev/disk0p3 rach2.slot=candidate"});
    ok = check(r,
               f->status == 1 &&
                   last_is(record(r), "failed trial: the trial boot's command line lacks "
                                      "rach2.slot=candidate or rach2.console=term: "
                                      "root=/dev/disk0p3 rach2.slot=candidate") &&
                   !called(r, "^(identity|accept|machine write)") && !called(r, " push "),
               "a trial boot without rach2.console=term fails trial and is not committed") &&
         ok;
    return ok;
}
/**
 * The install step moves a disk to the build's kernel slots before its
 * bootctl kernel, when the recovery system's rach2-install has --slots, and
 * a machine whose old root cannot arm the trial of recovery is armed with
 * the build's bootctl.
 */
static int slots_rows(struct release *r)
{
    struct fixture *f = r->f;
    char sent[9000], *line;
    int ok = 1;
    const char *recovery = "root=/dev/disk0p3 rach2.slot=recovery";
    snprintf(sent, sizeof(sent),
             "%sbuild done %s kernel k root r\nrecovery done %s kernel k image i\ntransfer done %s "
             "kernel and root in /dev/shm\n",
             r->head, T, T, T);
    run(r, &(struct release_row){.record = sent, .cmdline = recovery, .slots = "move"});
    ok = check(r,
               f->status == 1 &&
                   before(r, "machine write rach2-install --slots /dev/disk0\n",
                          "machine write bootctl kernel\n") &&
                   before(r, "machine write bootctl kernel\n",
                          "machine write rach2-install --update\n") &&
                   count(record(r), "^kernel done .* candidate slot$") == 1 &&
                   fx_has(flow_path(f, "state/log"), "moved /dev/disk0p1 to the slots") &&
                   fx_has(record(r), "\nfailed install: rach2-install ended 1") &&
                   !called(r, "trial recovery$"),
               "the install moves the disk to the build's slots before bootctl kernel") &&
         ok;
    run(r, &(struct release_row){.record = sent, .cmdline = recovery, .slots = "full"});
    line = last_line(record(r));
    ok = check(r,
               f->status == 1 &&
                   fx_regex(line, "^failed kernel: rach2-install --slots: .*no room for the larger "
                                  "slots") &&
                   called(r, "^machine write rach2-install --slots /dev/disk0$") &&
                   !called(r, "^machine write bootctl kernel$") &&
                   !fx_has(record(r), "kernel done "),
               "a move of the slots that fails stops the release before bootctl kernel") &&
         ok;
    free(line);
    run(r, &(struct release_row){.record = sent, .cmdline = recovery, .slots = "old"});
    ok = check(r,
               f->status == 1 && !called(r, "--slots /dev/") &&
                   count(record(r), "^kernel done ") == 1,
               "a recovery system without --slots goes straight to bootctl kernel") &&
         ok;
    run(r, &(struct release_row){.record = sent, .slots = "reset"});
    ok =
        check(r,
              f->status == 1 &&
                  before(r, "machine write bootctl trial recovery\n",
                         "machine write cat > /dev/shm/bootctl\n") &&
                  before(r, "machine write cat > /dev/shm/bootctl\n",
                         "machine write /dev/shm/bootctl trial recovery\n") &&
                  before(r, "machine write /dev/shm/bootctl trial recovery\n", "; reboot\"") &&
                  before(r, "; reboot\"", "machine write rach2-install --slots /dev/disk0\n") &&
                  fx_equal(flow_path(f, "calls.bootctl"), BOOTCTL) &&
                  count(record(r), "^kernel done ") == 1 &&
                  fx_has(record(r), "\nfailed install: rach2-install ended 1"),
              "an old root that cannot arm the recovery trial is armed with the build's bootctl") &&
        ok;
    return ok;
}
static int acceptance_rows(struct release *r)
{
    struct fixture *f = r->f;
    int ok = 1;
    char *line;
    run(r,
        &(struct release_row){.record = r->tried, .git = "yes", .writes = "yes", .accept = "pass"});
    ok =
        check(r,
              !f->status && archived(r, "7-11111111-") &&
                  before(r, "accept 192.168.0.106\n", "machine write bootctl commit\n") &&
                  before(r, "machine write bootctl commit\n", "machine write bootctl fallback\n") &&
                  before(r, "machine write bootctl fallback\n", "machine write release\n") &&
                  before(r, "machine write release\n", " push origin ") &&
                  count(flow_path(f, "calls"), "^(accept|machine write)") == 4 &&
                  count(r->retired, "^accept done .* metal-accept: PASS$") == 1 &&
                  count(r->retired, "^commit done .* committed and copied to the fallback "
                                    "slot$") == 1 &&
                  !called(r, "^gh issue"),
              "acceptance commits trial, copies it to the fallback slot, then records and "
              "releases") &&
        ok;
    run(r,
        &(struct release_row){.record = r->tried, .git = "yes", .writes = "yes", .accept = "fail"});
    line = last_line(record(r));
    ok = check(r,
               f->status == 1 &&
                   fx_regex(line, "^failed accept: metal_accept.sh failed: .*FAIL: the hub "
                                  "section found no hub") &&
                   called(r, "^accept 192.168.0.106$") && !called(r, "^machine write") &&
                   !called(r, " push ") &&
                   strstr(f->out, "release: failed: accept: metal_accept.sh failed: "),
               "failed acceptance does not commit trial") &&
         ok;
    free(line);
    return ok;
}
static int failed_mac(struct release *r)
{
    return fx_has(record(r), "\nfailure mac ");
}
static const char *mac_failed(struct release *r, char *text, size_t size)
{
    char path[4200];
    snprintf(path, sizeof(path), "tree/build/machines/mac/runs/%s.times", MAC_FAILED);
    flow_put(r->f, path, "start 2026-10-29T00:00:00+0600\nrc=1\nend 2026-10-29T02:00:00+0600\n");
    snprintf(path, sizeof(path), "tree/build/machines/mac/runs/%s.console", MAC_FAILED);
    flow_put(r->f, path, "run: result: failed\nrun: result: failed: i686:gnu x86_64:gnu\n");
    snprintf(text, size, "%smac started %s\n", r->built, MAC_FAILED);
    return MAC_FAILED;
}
/**
 * Succeed when the Mac's run ID ended with status 0 after it checked out C,
 * built etude, allocated the selection of every case and ran the phase on
 * it with the build its copy kept, in that order.
 */
static int mac_passed(struct release *r, const char *id)
{
    char times[4200], selection[2000], phase[2000];
    snprintf(times, sizeof(times), "%s/tree/build/machines/mac/runs/%s.times", r->f->work, id);
    snprintf(selection, sizeof(selection),
             "^etude site=.*/mac/site/etude-site clean= export= run -N -t .*/mac/tree -a both -A "
             "tcg -J .*/mac/runs/%s\\.selection\\.json " SUITES "$",
             id);
    snprintf(phase, sizeof(phase),
             "^etude site= clean= export=.*/mac/runs/%s\\.selection\\.json run routine-sequence "
             "phase_first_run .*/mac/tree .*/mac/runs/%s both %s .*/mac/site/etude-site "
             ".*/mac/site/bin/etude tcg " SUITES "$",
             id, id, id);
    return fx_has(times, "\nrc=0\n") && called(r, selection) && called(r, phase) &&
           before(r, "\ngit checkout -q -f --detach " C "\n",
                  "\nmake build/tools/etude/etude build/tools/etude/etude-volume\n") &&
           before(r, "\nmake build/tools/etude/etude build/tools/etude/etude-volume\n",
                  " run -N -t ") &&
           before(r, " run -N -t ", " run routine-sequence phase_first_run ");
}
static int mac_rows(struct release *r)
{
    struct fixture *f = r->f;
    char text[9000], id[200], last[400];
    const char *failed;
    int ok = 1;
    if (!mac_tree(r))
        return 0;
    snprintf(text, sizeof(text), "milestone 7\nsha %s\nbranch %s\nchosen %s\ntouched %s\n", C,
             BRANCH, T, T);
    run(r, &(struct release_row){.record = text,
                                 .sha = C,
                                 .runs = "none",
                                 .git = "yes",
                                 .machines = MAC,
                                 .detach = "yes"});
    ok = check(r,
               f->status == 75 && count(record(r), "^mac started ") == 1 &&
                   mac_ran(r, id, sizeof(id)) && !strncmp(id, "milestone7-222222222222-mac-", 28) &&
                   fx_has(flow_path(f, "tree/build/machines/mac/run"), id) &&
                   count(flow_path(f, "calls"), "^dispatch ") == 1 && mac_passed(r, id),
               "mac run starts beside the full run and passes") &&
         ok;
    snprintf(text, sizeof(text), "%smac started %s\n", r->built, id);
    snprintf(last, sizeof(last), "^mac done .* run %s$", id);
    run(r, &(struct release_row){.record = text, .machines = MAC});
    ok = check(r, f->status == 1 && count(record(r), last) == 1 && !failed_mac(r),
               "a passed mac run is marked done") &&
         ok;
    failed = mac_failed(r, text, sizeof(text));
    run(r, &(struct release_row){.record = text, .machines = MAC});
    snprintf(last, sizeof(last), "failed tests: mac run %s: i686:gnu x86_64:gnu", failed);
    ok = check(r,
               f->status == 1 && last_is(record(r), last) &&
                   fx_has(record(r), "\nfailure mac " MAC_FAILED " i686:gnu x86_64:gnu\n") &&
                   ended_with(r, "release: failed: mac run " MAC_FAILED ": i686:gnu x86_64:gnu") &&
                   !called(r, "^(machine write|gh issue)"),
               "mac failure is recorded and ends the release before the install") &&
         ok;
    return ok;
}
#define WINDOWS_ID    "milestone7-111111111111-windows-"
#define WINDOWS_HELD  "/mnt/d/o/" WINDOWS_ID
/* The directory of the claims' files, on ext4 in the owner's clone in WSL. */
#define CLAIMS        "/home/testing/rach2/build/flow/claims"
#define WINDOWS_LOCKS CLAIMS "/" WINDOWS_ID
/**
 * Succeed when Windows' preparation ran under its own claim in WSL, whose
 * runner, a copy of claim.pl that the start creates, runs in a session of its
 * own, and whose holder lasts while the part and the release append to
 * ID.alive, and ends once the preparation does.
 */
static int windows_prepared(struct release *r)
{
    return before(r, " env JOBS=30 /usr/local/libexec/rach2/etude-launch build env \\\n",
                  "\nwindows cd D:/w || ") &&
           called(r, "^wsl mkdir -p " CLAIMS " && \\(set -C && cat > " WINDOWS_LOCKS
                     "[0-9a-f-]+\\.claim\\.1\\.pl\\) << 'END_OF_CLAIM' \\|\\| exit 1$") &&
           called(r, "^wsl cd /tmp && setsid nohup perl " WINDOWS_LOCKS
                     "[0-9a-f-]+\\.claim\\.1\\.pl run " CLAIMS " /mnt/d/o " WINDOWS_ID
                     "[0-9a-f-]+ claim 1 env JOBS=30 ") &&
           called(r, "^wsl +perl " WINDOWS_LOCKS "[0-9a-f-]+\\.claim\\.1\\.pl hold " CLAIMS
                     " /mnt/d/o " WINDOWS_ID "[0-9a-f-]+ claim 1 " WINDOWS_ID
                     "[0-9a-f-]+\\.prepared > " WINDOWS_LOCKS "[0-9a-f-]+\\.claim\\.1\\.out ") &&
           called(r, "^windows while kill -0 \\$\\$ 2> /dev/null; do printf \\. >> D:/o/" WINDOWS_ID
                     "[0-9a-f-]+\\.alive; sleep 60; done &$") &&
           before(r, "\nwsl perl - state " CLAIMS " " WINDOWS_ID, "\nwindows cd D:/w || ") &&
           called(r, "^windows .* run routine-prepare phase_first_run D:/w "
                     "D:/o/" WINDOWS_ID "[0-9a-f-]+ both " WINDOWS_ID
                     "[0-9a-f-]+ C:/s .* whpx " SUITES "  > ") &&
           before(r, " run routine-prepare ", ".prepared\n") &&
           called(r, "^wsl touch " WINDOWS_HELD "[0-9a-f-]+\\.prepared$") &&
           called(r, "^windows git fetch -q D:/o/release.bundle 'refs/remotes/origin/" BRANCH
                     "' && git checkout -q -f --detach " H " ") &&
           called(r, "^windows export JOBS='30'$");
}
/**
 * Succeed when Windows' cases ran while WSL's whole host was held, a hold that
 * runs in a session of its own, lasts while the release beats and ends once
 * the cases do.
 */
static int windows_collected(struct release *r)
{
    return before(r, ".prepared\n", " env /usr/local/libexec/rach2/etude-launch alone \\\n") &&
           called(r, "^wsl cd /tmp && setsid nohup perl " WINDOWS_LOCKS
                     "[0-9a-f-]+\\.hold\\.1\\.pl run " CLAIMS " /mnt/d/o " WINDOWS_ID
                     "[0-9a-f-]+ hold 1 env /usr/local/libexec/rach2/etude-launch alone \\\\$") &&
           called(r,
                  "^wsl +perl " WINDOWS_LOCKS "[0-9a-f-]+\\.hold\\.1\\.pl hold " CLAIMS
                  " /mnt/d/o " WINDOWS_ID "[0-9a-f-]+ hold 1 " WINDOWS_ID "[0-9a-f-]+\\.end > ") &&
           called(r, "^wsl printf \\. >> " WINDOWS_HELD "[0-9a-f-]+\\.alive; sleep 60$") &&
           before(r, " env /usr/local/libexec/rach2/etude-launch alone \\\n",
                  " run routine-collect ") &&
           called(r, "^windows .* run routine-collect phase_first_run D:/w "
                     "D:/o/" WINDOWS_ID "[0-9a-f-]+ both " WINDOWS_ID
                     "[0-9a-f-]+ C:/s .* whpx " SUITES "  > ") &&
           called(r, "^wsl touch " WINDOWS_HELD "[0-9a-f-]+\\.end$") &&
           !called(r, "PREPARATION_CLEAN=1");
}
/**
 * Succeed when the preparation's start writes its token, failing when one
 * exists, before it reads the gate's run, and writes the part's script
 * before Win32_Process.Create starts winjob.exe with the token and its
 * nonce, which makes the part's job before it reads the token.
 */
static int windows_launch_ordered(struct release *r)
{
    static const char token[] =
        "\nwindows if ! (set -C && echo \"$n\" > \"$b.token\") 2> /dev/null; then\n";
    static const char gate[] =
        "\nwindows if [ \"$(cat \"$o/windows.current\" 2> /dev/null)\" != \"$id\" ]; then\n";
    return before(r, token, gate) && before(r, gate, "\nwindows END_OF_RUN\n") &&
           before(r, "\nwindows END_OF_RUN\n", " Invoke-CimMethod ") &&
           called(r, "CommandLine='\\$winjob run \\$j \\$b\\.token \\$n "
                     "C:/msys64/usr/bin/bash\\.exe -l \\$b\\.sh'") &&
           called(r, "^windows winjob=D:/o/winjob-[0-9]+\\.exe$");
}
static int windows_pass_row(struct release *r)
{
    struct fixture *f = r->f;
    run(r, &(struct release_row){
               .record = r->head, .git = "yes", .machines = WINDOWS, .windows = "status 0"});
    return check(r,
                 f->status == 75 && windows_prepared(r) && windows_collected(r) &&
                     windows_launch_ordered(r) &&
                     called(r, "^git -C .* bundle create .*/state/windows.bundle "
                               "refs/remotes/origin/" BRANCH "$") &&
                     called(r, "^scp -q -o BatchMode=yes .*/state/windows.bundle "
                               "testing:D:/o/release.bundle$") &&
                     count(record(r), "^windows done .* run " WINDOWS_ID) == 1 &&
                     called(r, "^testing "),
                 "windows prepares under its own claim, then runs its cases while WSL is held");
}
/**
 * A failed run, a run without a status, a preparation without a status that
 * winclean.sh cannot confirm stopped and a failed preparation.
 */
static int windows_failure_rows(struct release *r)
{
    struct fixture *f = r->f;
    char last[400], id[200];
    int ok = 1;
    run(r, &(struct release_row){.record = r->head,
                                 .git = "yes",
                                 .machines = WINDOWS,
                                 .windows = "run: result: failed: x86_64:agents\nstatus 1"});
    machine_line(r, "windows started ", id, sizeof(id));
    snprintf(last, sizeof(last), "failed tests: windows run %s: x86_64:agents", id);
    ok = check(r,
               f->status == 1 && last_is(record(r), last) && called(r, "^wsl touch ") &&
                   !called(r, "^testing "),
               "windows failure is recorded and ends the release before the build") &&
         ok;
    run(r, &(struct release_row){
               .record = r->head, .git = "yes", .machines = WINDOWS, .hold = "probe"});
    machine_line(r, "windows started ", id, sizeof(id));
    snprintf(last, sizeof(last), "failure windows %s runner", id);
    ok = check(r,
               f->status == 1 && count(flow_path(f, "calls"), " run routine-collect ") == 3 &&
                   count(flow_path(f, "calls"), " run routine-prepare ") == 3 &&
                   count(flow_path(f, "calls"), "^sleep 30$") >= 6 && fx_has(record(r), last),
               "windows run without status runs three times after missed claim probes") &&
         ok;
    run(r, &(struct release_row){.record = r->head,
                                 .git = "yes",
                                 .machines = WINDOWS,
                                 .hold = "stuck-clean",
                                 .prepare = "building",
                                 .windows = "status 0"});
    machine_line(r, "windows started ", id, sizeof(id));
    snprintf(last, sizeof(last), "waiting: the Windows runs %s are not confirmed stopped there",
             id);
    ok = check(r,
               f->status == 75 && count(flow_path(f, "calls"), " run routine-prepare ") == 1 &&
                   count(flow_path(f, "calls"), "^windows # winclean.sh: ") == 1 &&
                   count(flow_path(f, "calls"), "^gate # winclean.sh: ") == 2 &&
                   !called(r, " run routine-collect ") && strstr(f->out, last) != NULL,
               "a preparation without a status that winclean.sh cannot confirm stopped keeps "
               "the release waiting, and no other starts in its copy") &&
         ok;
    run(r, &(struct release_row){.record = r->head,
                                 .git = "yes",
                                 .machines = WINDOWS,
                                 .prepare = "etude prepare: routine preparation failed\nstatus 2",
                                 .windows = "status 0"});
    machine_line(r, "windows started ", id, sizeof(id));
    snprintf(last, sizeof(last), "failed tests: windows run %s: runner", id);
    ok = check(r,
               f->status == 1 && last_is(record(r), last) &&
                   called(r, "^wsl touch " WINDOWS_HELD "[0-9a-f-]+\\.prepared$") &&
                   !called(r, "etude-launch alone") && !called(r, " run routine-collect "),
               "a failed preparation ends its claim and takes no whole host") &&
         ok;
    return ok;
}
/** WSL's admission refuses either hold, or the release stops in its wait. */
static int windows_hold_rows(struct release *r)
{
    struct fixture *f = r->f;
    int ok = 1;
    run(r, &(struct release_row){.record = r->head,
                                 .git = "yes",
                                 .machines = WINDOWS,
                                 .hold = "refused",
                                 .windows = "status 0"});
    ok = check(r,
               f->status == 1 &&
                   fx_has(record(r), "\nfailed windows: WSL's admission did not "
                                     "give the whole host: etude-launch: result: "
                                     "refused: ") &&
                   before(r, ".prepared\n", " env /usr/local/libexec/rach2/etude-launch alone ") &&
                   called(r, "^wsl touch " WINDOWS_HELD "[0-9a-f-]+\\.end$") &&
                   !called(r, " run routine-collect "),
               "a whole host that WSL refuses ends the release with nothing left on Windows") &&
         ok;
    run(r, &(struct release_row){
               .record = r->head, .git = "yes", .machines = WINDOWS, .hold = "claim"});
    ok = check(r,
               f->status == 1 &&
                   fx_has(record(r), "\nfailed windows: WSL's admission did not give the "
                                     "preparation's claim: etude-launch: result: refused: ") &&
                   called(r, "^gate # winclean.sh: ") && !called(r, "^windows b=") &&
                   !called(r, "etude-launch alone"),
               "a preparation claim that WSL refuses ends the release before Windows starts") &&
         ok;
    run(r, &(struct release_row){
               .record = r->head, .git = "yes", .machines = WINDOWS, .hold = "wait"});
    ok = check(r,
               f->status != 0 && f->status != 1 && f->status != 75 &&
                   called(r, "^wsl touch " WINDOWS_HELD "[0-9a-f-]+\\.prepared$") &&
                   called(r, "^wsl touch " WINDOWS_HELD "[0-9a-f-]+\\.end$") &&
                   !called(r, " run routine-collect ") && !fx_has(record(r), "\nfailed ") &&
                   !fx_has(record(r), "\nwindows done "),
               "a release stopped while it waits for the whole host ends its hold") &&
         ok;
    return ok;
}
/** Succeed when the release ended by the TERM that stop_release sent. */
static int stopped_by_term(struct release *r)
{
    return r->f->status != 0 && r->f->status != 1 && r->f->status != 75 &&
           called(r, "^release stopped$");
}
/**
 * A release stopped while a Windows part runs ends the part's claim in WSL
 * only once winclean.sh has confirmed that the part has stopped, and keeps
 * the claim while it cannot.
 */
static int windows_stop_rows(struct release *r)
{
    struct fixture *f = r->f;
    int ok = 1;
    run(r, &(struct release_row){.record = r->head,
                                 .git = "yes",
                                 .machines = WINDOWS,
                                 .hold = "stop",
                                 .windows = "status 0"});
    ok = check(r,
               stopped_by_term(r) && before(r, "\nwindows # winclean.sh: ", ".prepared\n") &&
                   before(r, "\nwindows # winclean.sh: ", "\nwsl perl - stop " CLAIMS " ") &&
                   called(r, "^wsl perl - stop " CLAIMS " " WINDOWS_ID
                             "[0-9a-f-]+ claim << 'END_OF_CLAIM'$") &&
                   !called(r, " run routine-collect "),
               "a release stopped while Windows prepares stops the preparation before it "
               "ends the preparation's claim") &&
         ok;
    run(r, &(struct release_row){.record = r->head,
                                 .git = "yes",
                                 .machines = WINDOWS,
                                 .hold = "stuck",
                                 .windows = "status 0"});
    ok = check(r,
               stopped_by_term(r) && called(r, "^windows # winclean.sh: ") &&
                   !called(r, "^wsl touch " WINDOWS_HELD "[0-9a-f-]+\\.prepared$") &&
                   !called(r, "^wsl perl - stop ") &&
                   strstr(f->out, " is not confirmed stopped on Windows; ") != NULL,
               "a preparation that winclean.sh cannot confirm stopped keeps its claim") &&
         ok;
    run(r, &(struct release_row){.record = r->head,
                                 .git = "yes",
                                 .machines = WINDOWS,
                                 .hold = "stuck-cases",
                                 .windows = "status 0"});
    ok = check(r,
               stopped_by_term(r) && called(r, " run routine-collect ") &&
                   called(r, "^windows # winclean.sh: ") &&
                   !called(r, "^wsl touch " WINDOWS_HELD "[0-9a-f-]+\\.end$") &&
                   !called(r, "^wsl perl - stop " CLAIMS " " WINDOWS_ID "[0-9a-f-]+ hold ") &&
                   strstr(f->out, " is not confirmed stopped on Windows; ") != NULL,
               "cases that winclean.sh cannot confirm stopped keep the whole host's claim") &&
         ok;
    return ok;
}
/*
 * The driver of claim_rows: it runs release.sh's claim functions here, with
 * ssh, setsid, etude-launch and etude from DIR/bin, on DIR's files, so that
 * claim.pl runs as in WSL, and prints what scenario SCENARIO saw. No process
 * of a claim is its child, so it signals none by number: the orphan row's
 * runner is killed by the supervisor that setsid keeps as its unreaped
 * parent, and at its exit the end files end the fake waits, the claim's
 * tombstone ends the rest, and it prints a line `left` unless every
 * attempt's locks, which the claim's processes hold, and the supervisor's
 * lock are free within a minute. Usage: sh driver DIR RELEASE SCENARIO.
 */
static const char claim_driver[] =
    "W=$1 R=$2 how=$3\n"
    "repo=$(cd \"$(dirname \"$R\")/../..\" && pwd)\n"
    "PATH=$W/bin:/usr/bin:/bin FAKE_DIR=$W RACH2_CLAIM_LEDGER=$W/ledger "
    "RACH2_CLAIM_ETUDE=$W/bin/etude\n"
    "FAKE_HOLD='' FAKE_ETUDE_RC=0\n"
    "case $how in\n"
    "dead | cleanup | ledger | probe) FAKE_LAUNCH=grant ;;\n"
    "late | retry) FAKE_LAUNCH=wait ;;\n"
    "orphan) FAKE_LAUNCH=stubborn ;;\n"
    "*) FAKE_LAUNCH=$how ;;\n"
    "esac\n"
    "[ \"$how\" != cleanup ] || FAKE_LAUNCH=linger FAKE_HOLD=1\n"
    "FAKE_SUPERVISE=\n"
    "[ \"$how\" != orphan ] || FAKE_SUPERVISE=1\n"
    "export PATH FAKE_DIR FAKE_LAUNCH FAKE_HOLD FAKE_ETUDE_RC FAKE_SUPERVISE RACH2_CLAIM_LEDGER "
    "RACH2_CLAIM_ETUDE\n"
    "wheld=$W/held wlock=$W/locks state=$W/state WSL=wsl\n"
    "rm -rf \"$wheld\" \"$wlock\" \"$state\" \"$W/ledger\" \"$W/launcher\" \"$W/end\" \"$W/ended\" "
    "\"$W/launch-go\" \"$W/etude-go\" \"$W/kill-runner\" \"$W/runner-gone\" \"$W/supervisor\"\n"
    "mkdir -p \"$wheld\" \"$state\" \"$W/ledger\"\n"
    "pause() { perl -e 'select(undef, undef, undef, 0.1)'; }\n"
    "gone() {\n"
    "    i=0\n"
    "    while kill -0 \"$1\" 2> /dev/null && [ \"$i\" -lt 150 ]; do i=$((i + 1)); pause; done\n"
    "    kill -0 \"$1\" 2> /dev/null || echo \"gone $2\"\n"
    "}\n"
    "# settled LOCK: succeed once no process holds LOCK, a minute at most.\n"
    "settled() {\n"
    "    [ ! -e \"$1\" ] || perl -MFcntl=:flock -e 'open(L, \"<\", shift) or exit 0;\n"
    "        for (1 .. 600) { exit 0 if flock(L, LOCK_EX | LOCK_NB); select(undef, undef, undef, "
    "0.1) }\n"
    "        exit 1' \"$1\"\n"
    "}\n"
    "# No process of a claim is this driver's child, so none is signalled by its\n"
    "# number here: the claim's tombstone and the end files end them all, and the\n"
    "# attempts' locks, which they hold, say when they have.\n"
    "trap 'touch \"$W/end\" \"$W/launch-go\" \"$W/etude-go\"\n"
    "    [ ! -d \"$wlock\" ] || : > \"$wlock/m7.claim.stop\"\n"
    "    for l in \"$wlock\"/*.live \"$wlock\"/*.granted; do\n"
    "        settled \"$l\" || echo \"left: a process of the claim holds ${l##*/}\"\n"
    "    done\n"
    "    settled \"$W/supervisor\" || echo \"left: the supervisor of the runner\"' EXIT\n"
    "eval \"$(sed -n '/^tssh()$/,/^}$/p' \"$R\")\"\n"
    "eval \"$(sed -n '/^# claim_pl ARG/,/^# beat ID/p' \"$R\")\"\n"
    "look() {\n"
    "    i=0\n"
    "    while s=$(claim_state m7 claim \"${2:-1}\"); do\n"
    "        case $1 in +) [ -z \"$s\" ] || break ;; *) [ \"$s\" != \"$1\" ] || break ;; esac\n"
    "        [ \"$i\" -lt 150 ] || break\n"
    "        i=$((i + 1))\n"
    "        pause\n"
    "    done\n"
    "    echo \"state ${s:-none}\"\n"
    "}\n"
    "until_file() { i=0; until [ -e \"$1\" ] || [ \"$i\" -ge 150 ]; do i=$((i + 1)); pause; done; "
    "}\n"
    "start() { claim_start m7 claim \"$1\" 'etude-launch alone' m7.prepared > /dev/null 2>&1; }\n"
    "# late N: run attempt N's runner again, as a start that a lost link delayed,\n"
    "# and say whether it started a launcher.\n"
    "late() {\n"
    "    rm -f \"$W/launcher\"\n"
    "    perl \"$wlock/m7.claim.$1.pl\" run \"$wlock\" \"$wheld\" m7 claim \"$1\" env etude-launch "
    "alone true > /dev/null 2>&1\n"
    "    [ -e \"$W/launcher\" ] || echo 'late started nothing'\n"
    "}\n"
    "echo m7 > \"$wheld/windows.current\"\n"
    "printf . > \"$wheld/m7.alive\"\n"
    "case $how in\n"
    "bound)\n"
    "    # A child that ignores TERM and ALRM ends by bound.pl's KILL.\n"
    "    perl \"$repo/tools/flow/bound.pl\" 2 perl -e '$SIG{TERM} = $SIG{ALRM} = \"IGNORE\"; sleep "
    "60' 2> /dev/null\n"
    "    echo \"bound $?\"\n"
    "    exit ;;\n"
    "probe)\n"
    "    # A looker that stalls while it holds attempt 1's lock shared; it\n"
    "    # makes W/ready only once it holds the lock.\n"
    "    mkdir -p \"$wlock\"\n"
    "    perl -MFcntl=:flock -e 'open(L, \">>\", shift) && flock(L, LOCK_SH) or exit 1;\n"
    "        open(R, \">\", shift) && close(R) or exit 1;\n"
    "        my $e = shift; select(undef, undef, undef, 0.1) until -e $e' "
    "\"$wlock/m7.claim.1.live\" "
    "\"$W/ready\" \"$W/end\" &\n"
    "    until_file \"$W/ready\"\n"
    "    start 1\n"
    "    s=$(claim_state m7 claim 1)\n"
    "    echo \"state ${s:-none}\"\n"
    "    claim_stop m7 claim 1\n"
    "    echo \"stop $?\"\n"
    "    touch \"$W/end\"\n"
    "    wait\n"
    "    rm -f \"$W/end\" \"$W/ready\"\n"
    "    start 2\n"
    "    until_file \"$W/launcher\"\n"
    "    read -r p l < \"$W/launcher\"\n"
    "    look granted 2\n"
    "    claim_stop m7 claim\n"
    "    echo \"stop $?\"\n"
    "    gone \"$l\" launcher\n"
    "    exit ;;\n"
    "esac\n"
    "start 1\n"
    "until_file \"$W/launcher\"\n"
    "read -r p l < \"$W/launcher\"\n"
    "case $how in\n"
    "grant)\n"
    "    look granted\n"
    "    claim_stop m7 claim\n"
    "    echo \"stop $?\"\n"
    "    [ ! -e \"$wlock/m7.claim.1.live\" ] || [ ! -e \"$wlock/m7.claim.1.granted\" ] || [ ! -e "
    "\"$wlock/m7.claim.stop\" ] ||\n"
    "        echo 'files kept'\n"
    "    gone \"$l\" launcher ;;\n"
    "dead)\n"
    "    look granted\n"
    "    touch \"$wheld/m7.prepared\"\n"
    "    look dead ;;\n"
    "cleanup)\n"
    "    look granted\n"
    "    touch \"$wheld/m7.prepared\"\n"
    "    until_file \"$W/ended\"\n"
    "    s=$(claim_state m7 claim 1)\n"
    "    echo \"linger ${s:-none}\"\n"
    "    touch \"$W/launch-go\"\n"
    "    until_file \"$wlock/m7.claim.1.reaped\"\n"
    "    s=$(claim_state m7 claim 1)\n"
    "    echo \"cleanup ${s:-none}\"\n"
    "    touch \"$W/etude-go\"\n"
    "    look dead ;;\n"
    "ledger)\n"
    "    look granted\n"
    "    touch \"$wheld/m7.prepared\"\n"
    "    until_file \"$wlock/m7.claim.1.rc\"\n"
    "    printf 'etude-ledger 1\\nclaim launch:x\\nowner %s 123 0\\n' \"$l\" > "
    "\"$W/ledger/claims\"\n"
    "    o=$(claim_stop m7 claim)\n"
    "    echo \"stop $? $(printf '%s\\n' \"$o\" | sed 's/^process [0-9]* keeps a claim or request "
    "in .*/kept/')\"\n"
    "    rm -f \"$W/ledger/claims\"\n"
    "    FAKE_ETUDE_RC=1\n"
    "    o=$(claim_stop m7 claim)\n"
    "    echo \"stop $? $(printf '%s\\n' \"$o\" | sed 's/^etude status could not recover "
    ".*/unrecovered/')\"\n"
    "    FAKE_ETUDE_RC=0\n"
    "    claim_stop m7 claim\n"
    "    echo \"stop $?\" ;;\n"
    "wait)\n"
    "    pause\n"
    "    s=$(claim_state m7 claim 1)\n"
    "    echo \"state ${s:-none}\"\n"
    "    claim_stop m7 claim\n"
    "    echo \"stop $?\"\n"
    "    gone \"$l\" launcher ;;\n"
    "refuse)\n"
    "    look + ;;\n"
    "orphan)\n"
    "    # The supervisor kills the runner, its own unreaped child.\n"
    "    touch \"$W/kill-runner\"\n"
    "    until_file \"$W/runner-gone\"\n"
    "    [ ! -e \"$W/runner-gone\" ] || echo 'gone runner'\n"
    "    s=$(claim_state m7 claim 1)\n"
    "    echo \"orphan ${s:-none}\"\n"
    "    touch \"$W/end\"\n"
    "    look dead ;;\n"
    "late)\n"
    "    claim_stop m7 claim\n"
    "    echo \"stop $?\"\n"
    "    gone \"$l\" launcher\n"
    "    late 1\n"
    "    rm -f \"$W/launcher\"\n"
    "    start 2\n"
    "    [ -e \"$W/launcher\" ] || echo 'retry started nothing'\n"
    "    look + 2 ;;\n"
    "retry)\n"
    "    claim_stop m7 claim 1\n"
    "    echo \"stop $?\"\n"
    "    rm -f \"$W/launcher\"\n"
    "    start 2\n"
    "    until_file \"$W/launcher\"\n"
    "    read -r p l < \"$W/launcher\"\n"
    "    late 1\n"
    "    kill -0 \"$l\" 2> /dev/null && echo 'retry waits'\n"
    "    claim_stop m7 claim\n"
    "    echo \"stop $?\"\n"
    "    gone \"$l\" launcher ;;\n"
    "esac\n";
/* The launcher of claim_rows, as FAKE_LAUNCH says: `grant` runs its command,
 * `linger` too, as a child, and then waits for DIR/launch-go, as etude's
 * launcher outlives its command until it has released its admission, `wait`
 * waits for admission, `stubborn` too, ignoring TERM from before it writes
 * its number, since its runner's death sends it TERM, and `refuse` refuses.
 * It writes its parent, the attempt's runner, and itself; each wait ends at
 * DIR/end too. */
static const char claim_launcher[] =
    "#!/bin/sh\n"
    "[ \"$FAKE_LAUNCH\" != stubborn ] || trap '' TERM\n"
    "echo \"$PPID $$\" > \"$FAKE_DIR/launcher.part\" && mv \"$FAKE_DIR/launcher.part\" "
    "\"$FAKE_DIR/launcher\"\n"
    "case $1 in build) shift 2 ;; *) shift ;; esac\n"
    "case $FAKE_LAUNCH in\n"
    "grant) exec \"$@\" ;;\n"
    "linger)\n"
    "    \"$@\"\n"
    "    : > \"$FAKE_DIR/ended\"\n"
    "    while [ ! -e \"$FAKE_DIR/launch-go\" ]; do sleep 1; done ;;\n"
    "wait) while [ ! -e \"$FAKE_DIR/end\" ]; do sleep 1; done ;;\n"
    "stubborn) while [ ! -e \"$FAKE_DIR/end\" ]; do sleep 1; done ;;\n"
    "*) echo 'etude-launch: result: refused: admission: no room'; exit 1 ;;\n"
    "esac\n";
/* The etude of claim_rows: `status` succeeds, or exits FAKE_ETUDE_RC, once
 * DIR/etude-go exists while FAKE_HOLD is set, half a minute at most. */
static const char claim_etude[] =
    "#!/bin/sh\n"
    "i=0\n"
    "while [ -n \"$FAKE_HOLD\" ] && [ ! -e \"$FAKE_DIR/etude-go\" ] && [ \"$i\" -lt 300 ]; do\n"
    "    perl -e 'select(undef, undef, undef, 0.1)'\n"
    "    i=$((i + 1))\n"
    "done\n"
    "exit \"${FAKE_ETUDE_RC:-0}\"\n";
/* The setsid of claim_rows: while FAKE_SUPERVISE is set, it stays the
 * parent of its command, the attempt's runner, and reaps it only once it has
 * ended, so that its KILL on DIR/kill-runner goes to that process alone; it
 * then writes DIR/runner-gone. It holds a lock of DIR/supervisor while it
 * lives. */
static const char claim_setsid[] =
    "#!/bin/sh\n"
    "[ -n \"$FAKE_SUPERVISE\" ] || exec perl -MPOSIX -e 'POSIX::setsid(); exec @ARGV or exit 127' "
    "\"$@\"\n"
    "exec perl -MPOSIX -MFcntl=:flock -e '\n"
    "    POSIX::setsid();\n"
    "    my $d = $ENV{FAKE_DIR};\n"
    "    open(L, \">>\", \"$d/supervisor\") && flock(L, LOCK_EX) or exit 127;\n"
    "    defined(my $c = fork()) or exit 127;\n"
    "    if (!$c) { close(L); exec @ARGV or exit 127 }\n"
    "    my $sent = 0;\n"
    "    while (waitpid($c, WNOHANG) == 0) {\n"
    "        if (!$sent && -e \"$d/kill-runner\") { kill(\"KILL\", $c); $sent = 1 }\n"
    "        select(undef, undef, undef, 0.1);\n"
    "    }\n"
    "    open(G, \">\", \"$d/runner-gone\") and close(G);' \"$@\"\n";
/** Run claim_driver's SCENARIO, and succeed when it printed WANT whole. */
static int claim_saw(struct release *r, const char *scenario, const char *want)
{
    struct fixture *f = r->f;
    const char *a[] = {"/bin/sh", NULL, NULL, NULL, scenario, NULL};
    a[1] = flow_path(f, "claims/driver");
    a[2] = flow_path(f, "claims");
    a[3] = flow_source(f, "tools/flow/release.sh");
    fx_run(f, a, f->work, NULL, NULL);
    if (f->out && !strcmp(f->out, want))
        return 1;
    fprintf(stderr, "claims: %s printed:\n%s", scenario, f->out ? f->out : "");
    return 0;
}
/**
 * release.sh's claims of WSL's admission, through claim.pl, on real locks: a
 * granted claim reads granted only while its grant lock is held, neither
 * while its launcher outlives its command nor while its runner cleans up
 * after the launcher (j90 3), and reads dead once its holder has ended (j79
 * 3); claim_stop ends a granted claim and one that waits, by its tombstone,
 * removes nothing, and confirms the end by the locks and by a recovered
 * ledger that names no attempt's launcher, failing while it names one or
 * recovery fails (j90 4); a launcher whose runner was killed, by the
 * supervisor whose unreaped child it is, keeps the attempt's lock, so the
 * claim reads neither free nor dead until it ends (j79 4, j102 1); a late
 * runner of a stopped attempt, and a start after the claim's stop, start
 * nothing, and an attempt's stop leaves a retry to run (j90 2, j102 2,
 * j110 1); a looker that stalls while it holds an attempt's lock shared
 * costs that attempt and keeps neither a look nor a stop waiting (84-148);
 * bound.pl kills a child that ignores TERM and ALRM (j116 5 and 6); and a
 * refused claim reads refused.
 */
static int claim_rows(struct release *r)
{
    struct fixture *f = r->f;
    int ok;
    fx_remove(flow_path(f, "claims"));
    if (!flow_put(f, "claims/driver", claim_driver) ||
        !fx_write(flow_path(f, "claims/bin/etude-launch"), claim_launcher, strlen(claim_launcher),
                  0700) ||
        !fx_write(flow_path(f, "claims/bin/etude"), claim_etude, strlen(claim_etude), 0700) ||
        !fx_write(flow_path(f, "claims/bin/ssh"), "#!/bin/sh\nexec /bin/sh -s\n", 26, 0700) ||
        !fx_write(flow_path(f, "claims/bin/setsid"), claim_setsid, strlen(claim_setsid), 0700))
        return check(r, 0, "the claim rows' files are written");
    ok = check(r, claim_saw(r, "grant", "state granted\nstop 0\nfiles kept\ngone launcher\n"),
               "a granted claim reads granted, and claim_stop ends it and removes nothing");
    ok = check(r, claim_saw(r, "dead", "state granted\nstate dead\n"),
               "a granted claim whose holder ended reads dead") &&
         ok;
    ok = check(r, claim_saw(r, "cleanup", "state granted\nlinger none\ncleanup none\nstate dead\n"),
               "a claim whose command has ended reads neither granted nor dead while its "
               "launcher or its runner still holds the attempt's lock") &&
         ok;
    ok =
        check(r, claim_saw(r, "ledger", "state granted\nstop 1 kept\nstop 1 unrecovered\nstop 0\n"),
              "claim_stop confirms an end only once recovery succeeded and the ledger names "
              "no attempt's launcher") &&
        ok;
    ok = check(r, claim_saw(r, "wait", "state none\nstop 0\ngone launcher\n"),
               "a claim that waits for admission reads nothing, and claim_stop ends it") &&
         ok;
    ok = check(r, claim_saw(r, "refuse", "state refused\n"), "a refused claim reads refused") && ok;
    ok = check(r, claim_saw(r, "orphan", "gone runner\norphan none\nstate dead\n"),
               "a launcher whose runner was killed keeps the attempt's lock until it ends") &&
         ok;
    ok = check(r,
               claim_saw(r, "late",
                         "stop 0\ngone launcher\nlate started nothing\nretry started nothing\n"
                         "state unstarted\n"),
               "after the claim's stop, a late runner and a new attempt start nothing, and the "
               "attempt reads unstarted") &&
         ok;
    ok = check(r,
               claim_saw(r, "retry",
                         "stop 0\nlate started nothing\nretry waits\nstop 0\ngone launcher\n"),
               "an attempt's stop keeps a late runner of that attempt from starting and leaves "
               "the retry to run") &&
         ok;
    ok = check(r,
               claim_saw(r, "probe",
                         "state unstarted\nstop 0\nstate granted\nstop 0\ngone launcher\n"),
               "a looker that stalls while it holds an attempt's lock shared costs only that "
               "attempt, which a look and a stop read at once, and the retry is granted") &&
         ok;
    ok = check(r, claim_saw(r, "bound", "bound 137\n"),
               "bound.pl kills its child that ignores TERM and ALRM at its bound (j116 5)") &&
         ok;
    fx_remove(flow_path(f, "claims"));
    return ok;
}
/*
 * The driver of hold_rows: it runs tools/flow/hold.pl on a run directory of
 * its own, DIR/run, made new for each scenario, and prints what scenario
 * SCENARIO saw. The `fast` scenario's job is forked as release.sh's `held`
 * forks one, behind the gate DIR/run/go, and a perl that sleeps a second
 * stands between the fork and hold.pl, so that a job that could end at once
 * would have ended before its holder ran. The `lost` scenario's holder is
 * killed by the perl whose unreaped child it is, and so is `lostgroup`'s,
 * a -g holder whose job then runs in its group, beside a process that left
 * the group by setsid. `groupkill` stops a -g job
 * that ignores TERM; `adopt` runs a job whose child, which ignores TERM, has
 * a child that ignores it too. The `jobfast` and `jobstop` scenarios run
 * release.sh's own job_dir, held, job_wait and job_stop, the first with a
 * perl on the path that waits a second before it runs, and `jobwriter` the
 * same with held's job opening its reader a second late; `drain` runs its
 * mac_live and held_gone on a run whose job has ended and left a process in
 * its group; `session` runs lib.sh's wait_session and session_state, and
 * `lostsession` session.pl, wait_session and session_state after a
 * supervisor is killed while its session runs, and `lostrefresh` lib.sh's
 * refresh in a clone of its own then; `wtlost` runs release.sh's wt_new
 * beside a job of an earlier release whose holder ended without its end,
 * and `discardlost` its job_wait and discard on such a job of this run; and
 * `prepare`, `recovery` and `keep` run prepare_host's script for WSL here,
 * through an ssh that runs it with sh, `prepare` with an etude that recovers
 * the ledger, `recovery` with one that fails and a scan of the processes
 * that succeeds and finds nothing, and `keep` with both succeeding. Usage:
 * sh driver DIR HOLD SCENARIO RELEASE, with HOLD_PATH the PATH of
 * `lostrefresh` and `wtlost`.
 */
static const char hold_driver[] =
    "H=$1 P=$2 how=$3 R=$4\n"
    "d=$H/run repo=${P%/tools/flow/hold.pl}\n"
    "# lostrefresh and wtlost work in clones of their own, with git itself and\n"
    "# not the release rows' mock, which HOLD_PATH leaves out.\n"
    "case $how in lostrefresh | wtlost) PATH=$HOLD_PATH ;; esac\n"
    "# A scenario that orphans processes runs below a perl that adopts and reaps\n"
    "# them, as init does, since a runner above may adopt them and reap none.\n"
    "case $how in\n"
    "lostgroup | lostsession | lostrefresh)\n"
    "    [ -n \"${REAPED-}\" ] || exec env REAPED=1 perl -MConfig -e '\n"
    "        my %n = (x86_64 => 157, aarch64 => 167, i686 => 172, i386 => 172);\n"
    "        my ($m) = $Config{archname} =~ /^([^-]+)/;\n"
    "        syscall($n{$m}, 36, 1, 0, 0, 0) if $^O eq \"linux\" && defined $m && $n{$m};\n"
    "        my $c = fork(); defined $c or exit 1;\n"
    "        exec(@ARGV) or exit 127 if !$c;\n"
    "        while ((my $p = waitpid(-1, 0)) > 0) { exit($? >> 8) if $p == $c }\n"
    "        exit 1' /bin/sh \"$0\" \"$@\" ;;\n"
    "esac\n"
    "rm -rf \"$d\" \"$H/ran\" \"$H/ready\" \"$H/state\" \"$H/bin\" \"$H/left\" \"$H/copy\" "
    "\"$H/ledger\" \"$H/survived\" \"$H/git\" \"$H/origin.git\" \"$H/wt\" \"$H/mac\" \"$H/f\" "
    "\"$H/escaped\"\n"
    "mkdir \"$d\" || exit 1\n"
    "look() { perl \"$P\" state \"$d\"; }\n"
    "pause() { perl -e 'select(undef, undef, undef, 0.1)'; }\n"
    "case $how in\n"
    "early)\n"
    "    : > \"$d/stop\"\n"
    "    perl \"$P\" run \"$d\" sh -c ': > \"$1/ran\"' sh \"$H\"\n"
    "    echo \"status $?\"\n"
    "    look\n"
    "    [ -e \"$H/ran\" ] || echo 'ran nothing' ;;\n"
    "fast | gated)\n"
    "    mkfifo \"$d/go\" || exit 1\n"
    "    [ \"$how\" != gated ] || : > \"$d/stop\"\n"
    "    (\n"
    "        exec 6<> \"$d/go\"\n"
    "        { exec 5< \"$d/go\" 6<&-; IFS= read -r g <&5 && [ \"$g\" = go ] || exit 143; : > "
    "\"$H/ran\"; exit 3; } &\n"
    "        exec perl -e 'sleep 1; exec @ARGV' perl \"$P\" run \"$d\" -p \"$!\" < /dev/null\n"
    "    )\n"
    "    echo \"status $?\"\n"
    "    look\n"
    "    [ -e \"$H/ran\" ] || echo 'ran nothing' ;;\n"
    "kill)\n"
    "    perl \"$P\" run -k 1 \"$d\" perl -e '$SIG{TERM} = \"IGNORE\"; open(R, \">\", shift) && "
    "close(R); sleep 60' \"$H/ready\" &\n"
    "    i=0\n"
    "    until [ -e \"$H/ready\" ] || [ \"$i\" -ge 150 ]; do i=$((i + 1)); pause; done\n"
    "    perl \"$P\" stop \"$d\" 30\n"
    "    echo \"stop $?\" ;;\n"
    "groupkill)\n"
    "    perl \"$P\" run -g -k 1 \"$d\" perl -e '$SIG{TERM} = \"IGNORE\"; open(R, \">\", shift) && "
    "close(R); sleep 60' \"$H/ready\" &\n"
    "    i=0\n"
    "    until [ -e \"$H/ready\" ] || [ \"$i\" -ge 150 ]; do i=$((i + 1)); pause; done\n"
    "    perl \"$P\" stop \"$d\" 30\n"
    "    echo \"stop $?\"\n"
    "    wait ;;\n"
    "adopt)\n"
    "    # The job's child ignores TERM and has a child that ignores it too and\n"
    "    # would write survived after twenty seconds; the job then ends.\n"
    "    printf '%s\\n' '$SIG{TERM} = \"IGNORE\";' 'my $g = fork(); defined $g or exit 1;' \\\n"
    "        'if (!$g) { sleep 20; open(S, \">\", \"$ARGV[0]/survived\"); exit 0 }' \\\n"
    "        'open(R, \">\", \"$ARGV[0]/ready\") && close(R); sleep 60;' > \"$H/left\"\n"
    "    perl \"$P\" run -k 1 \"$d\" sh -c 'perl \"$1/left\" \"$1\" &\n"
    "        until [ -e \"$1/ready\" ]; do perl -e \"select(undef, undef, undef, 0.05)\"; done' sh "
    "\"$H\" 2> /dev/null\n"
    "    echo \"status $?\"\n"
    "    look\n"
    "    [ ! -e \"$H/survived\" ] || echo survived ;;\n"
    "again)\n"
    "    perl \"$P\" run \"$d\" true\n"
    "    perl \"$P\" run \"$d\" true 2> /dev/null\n"
    "    echo \"status $?\"\n"
    "    look ;;\n"
    "lost)\n"
    "    perl -e 'my $c = fork(); defined $c or exit 1; if (!$c) { exec @ARGV or exit 127 }\n"
    "        for (1 .. 150) { last if -e \"$ENV{RUN}/live\"; select(undef, undef, undef, 0.1) }\n"
    "        kill(\"KILL\", $c); waitpid($c, 0)' perl \"$P\" run \"$d\" sleep 2\n"
    "    look ;;\n"
    "lostgroup)\n"
    "    # A -g holder killed outright reads lost while its job runs in its group,\n"
    "    # and stays lost once the group is empty, both while a process that left\n"
    "    # the group by setsid runs and after; a DIR/boot of another boot ends it.\n"
    "    printf '%s\\n' 'POSIX::setsid() or exit 1; open(R, \">\", \"$ARGV[0].part\") or exit 1;' "
    "'print R $$; close(R); rename(\"$ARGV[0].part\", $ARGV[0]); sleep 60;' > \"$H/left\"\n"
    "    perl -e 'my $c = fork(); defined $c or exit 1; if (!$c) { exec @ARGV or exit 127 }\n"
    "        for (1 .. 150) { last if -s \"$ENV{RUN}/../ready\"; select(undef, undef, undef, 0.1) "
    "}\n"
    "        kill(\"KILL\", $c); waitpid($c, 0)' perl \"$P\" run -g \"$d\" sh -c 'perl -MPOSIX "
    "\"$1/left\" \"$1/escaped\" > /dev/null 2>&1 &\n"
    "        until [ -s \"$1/escaped\" ]; do perl -e \"select(undef, undef, undef, 0.05)\"; done\n"
    "        echo $$ > \"$1/ready.part\"; mv \"$1/ready.part\" \"$1/ready\"; exec sleep 60' sh "
    "\"$H\"\n"
    "    look\n"
    "    g=$(cat \"$d/group\")\n"
    "    kill -KILL \"$(cat \"$H/ready\")\"\n"
    "    i=0\n"
    "    while ps -A -o pgid= | grep -qx \" *$g\" && [ \"$i\" -lt 150 ]; do i=$((i + 1)); pause; "
    "done\n"
    "    look\n"
    "    e=$(cat \"$H/escaped\")\n"
    "    kill -KILL \"$e\"\n"
    "    i=0\n"
    "    while kill -0 \"$e\" 2> /dev/null && [ \"$i\" -lt 150 ]; do i=$((i + 1)); pause; done\n"
    "    look\n"
    "    echo another-boot > \"$d/boot\"\n"
    "    look ;;\n"
    "jobwriter)\n"
    "    # held's job opens its reader a second late, after hold.pl has written\n"
    "    # its go and closed its end of the gate.\n"
    "    job=$(sed -n '/^# job_dir NAME:/,/^# tssh ARG/p' \"$R\")\n"
    "    late=$(printf '%s\\n' \"$job\" | sed 's/^\\( *\\)exec 5< /\\1perl -e \"sleep 1\"; exec 5< "
    "/')\n"
    "    [ \"$late\" != \"$job\" ] || echo 'no reader of its own'\n"
    "    eval \"$late\"\n"
    "    RACH2_HOLD_DIR=$H/state HOLD_KILL=1\n"
    "    j=$(job_dir t) || exit 1\n"
    "    held \"$j\" sh -c 'exit 3' &\n"
    "    job_wait \"$! $j\"\n"
    "    echo \"status $?\"\n"
    "    [ -d \"$j\" ] || echo removed ;;\n"
    "jobfast | jobstop)\n"
    "    eval \"$(sed -n '/^# job_dir NAME:/,/^# tssh ARG/p' \"$R\")\"\n"
    "    RACH2_HOLD_DIR=$H/state HOLD_KILL=1\n"
    "    j=$(job_dir t) || exit 1\n"
    "    if [ \"$how\" = jobfast ]; then\n"
    "        # A perl that waits a second before it runs holds back hold.pl's\n"
    "        # start, while the job, which ends at once, waits at its gate.\n"
    "        real=$(command -v perl)\n"
    "        mkdir \"$H/bin\" || exit 1\n"
    "        printf '#!/bin/sh\\n\"%s\" -e \"select(undef, undef, undef, 1)\"\\nexec \"%s\" "
    "\"$@\"\\n' \"$real\" \"$real\" > \"$H/bin/perl\"\n"
    "        chmod +x \"$H/bin/perl\"\n"
    "        (PATH=$H/bin:$PATH; held \"$j\" sh -c 'exit 3') &\n"
    "        job=\"$! $j\"\n"
    "        job_wait \"$job\"\n"
    "    else\n"
    "        held \"$j\" perl -e '$SIG{TERM} = \"IGNORE\"; open(R, \">\", shift) && close(R); "
    "sleep 60' \"$H/ready\" 2> /dev/null &\n"
    "        job=\"$! $j\"\n"
    "        i=0\n"
    "        until [ -e \"$H/ready\" ] || [ \"$i\" -ge 150 ]; do i=$((i + 1)); pause; done\n"
    "        job_stop \"$job\"\n"
    "    fi\n"
    "    echo \"status $?\"\n"
    "    [ -d \"$j\" ] || echo removed ;;\n"
    "drain)\n"
    "    eval \"$(sed -n '/^# mac_live DIR:/,/^# mac_start:/p' \"$R\")\"\n"
    "    # The job leaves a process in its group that ignores TERM and writes its\n"
    "    # number to ready, and then ends.\n"
    "    printf '%s\\n' '$SIG{TERM} = \"IGNORE\";' 'open(R, \">\", \"$ARGV[0].part\") or exit 1;' "
    "'print R $$; close(R); rename(\"$ARGV[0].part\", $ARGV[0]); sleep 60;' > \"$H/left\"\n"
    "    perl \"$P\" run -g -k 60 \"$d\" sh -c 'perl \"$1/left\" \"$1/ready\" &\n"
    "        until [ -s \"$1/ready\" ]; do perl -e \"select(undef, undef, undef, 0.05)\"; done' sh "
    "\"$H\" > /dev/null 2>&1 &\n"
    "    i=0\n"
    "    until [ -s \"$H/ready\" ] || [ \"$i\" -ge 150 ]; do i=$((i + 1)); pause; done\n"
    "    pause\n"
    "    ! mac_live \"$d\" || echo live\n"
    "    held_gone \"$d\" || echo kept\n"
    "    [ -e \"$d/end\" ] || echo 'no end'\n"
    "    kill -KILL \"$(cat \"$H/ready\")\"\n"
    "    perl \"$P\" stop \"$d\" 30\n"
    "    wait\n"
    "    held_gone \"$d\" && [ ! -d \"$d\" ] && echo removed ;;\n"
    "session)\n"
    "    flowbin=$repo/tools/flow\n"
    "    . \"$flowbin/lib.sh\"\n"
    "    state=$H/state\n"
    "    mkdir \"$state\" || exit 1\n"
    "    # A record of another boot.\n"
    "    echo '99999999 another-boot' > \"$state/session\"\n"
    "    wait_session\n"
    "    echo \"wait $?\"\n"
    "    [ -e \"$state/session\" ] || echo 'record removed'\n"
    "    perl -MFcntl=:flock -e 'open(L, \">>\", shift) && flock(L, LOCK_EX) or exit 1;\n"
    "        open(R, \">\", shift) && close(R); sleep 60' \"$state/session.live\" \"$H/ready\" &\n"
    "    i=0\n"
    "    until [ -e \"$H/ready\" ] || [ \"$i\" -ge 150 ]; do i=$((i + 1)); pause; done\n"
    "    session_state\n"
    "    kill \"$!\"\n"
    "    wait\n"
    "    session_state ;;\n"
    "lostsession | lostrefresh)\n"
    "    # A session whose supervisor is killed while its group runs keeps its\n"
    "    # record, and session.pl -q says lost: wait_session waits until\n"
    "    # STATE/stop, refresh changes nothing in the clone, and no other session\n"
    "    # starts, until its record names another boot.\n"
    "    flowbin=$repo/tools/flow\n"
    "    . \"$flowbin/lib.sh\"\n"
    "    state=$H/state S=$flowbin/session.pl\n"
    "    mkdir \"$state\" || exit 1\n"
    "    if [ \"$how\" = lostrefresh ]; then\n"
    "        g() { git -c user.name=t -c user.email=t@invalid -c core.hooksPath=/dev/null -c "
    "commit.gpgsign=false \"$@\"; }\n"
    "        { g init -q --bare \"$H/origin.git\" && mkdir \"$H/git\" && cd \"$H/git\" && g init "
    "-q &&\n"
    "            g symbolic-ref HEAD refs/heads/master && g commit -q --allow-empty -m one &&\n"
    "            g remote add origin \"$H/origin.git\" && g push -q origin master &&\n"
    "            g switch -q -c task && g push -q origin task; } > /dev/null 2>&1 || exit 1\n"
    "    fi\n"
    "    perl \"$S\" \"$state/session\" \"$state/log\" 600 sh -c 'echo $$ > \"$1/ready.part\"\n"
    "        mv \"$1/ready.part\" \"$1/ready\"; exec sleep 60' sh \"$H\" &\n"
    "    sup=$!\n"
    "    i=0\n"
    "    until [ -s \"$H/ready\" ] || [ \"$i\" -ge 150 ]; do i=$((i + 1)); pause; done\n"
    "    kill -KILL \"$sup\"\n"
    "    wait \"$sup\" 2> /dev/null\n"
    "    if [ \"$how\" = lostrefresh ]; then\n"
    "        refresh\n"
    "        echo \"refresh $?\"\n"
    "        git symbolic-ref --short HEAD\n"
    "    else\n"
    "        : > \"$state/stop\"\n"
    "        wait_session\n"
    "        echo \"wait $?\"\n"
    "        rm -f \"$state/stop\"\n"
    "        [ ! -e \"$state/session\" ] || echo kept\n"
    "        session_state | cut -d ' ' -f 1\n"
    "        perl \"$S\" \"$state/session\" \"$state/log\" 600 sh -c ': > \"$1/ran\"' sh \"$H\"\n"
    "        echo \"status $?\"\n"
    "        [ -e \"$H/ran\" ] || echo 'ran nothing'\n"
    "    fi\n"
    "    g=$(cut -d ' ' -f 1 \"$state/session\")\n"
    "    kill -KILL \"$(cat \"$H/ready\")\"\n"
    "    i=0\n"
    "    while ps -A -o pgid= | grep -qx \" *$g\" && [ \"$i\" -lt 150 ]; do i=$((i + 1)); pause; "
    "done\n"
    "    # An empty group proves nothing, since a process may have left it; a\n"
    "    # record of another boot is the one proof.\n"
    "    if [ \"$how\" = lostrefresh ]; then\n"
    "        refresh\n"
    "        echo \"refresh $?\"\n"
    "        git symbolic-ref --short HEAD\n"
    "    else\n"
    "        session_state | cut -d ' ' -f 1\n"
    "    fi\n"
    "    echo \"$g another-boot\" > \"$state/session\"\n"
    "    if [ \"$how\" = lostrefresh ]; then\n"
    "        refresh\n"
    "        echo \"refresh $?\"\n"
    "        git symbolic-ref --short HEAD\n"
    "    else\n"
    "        perl \"$S\" \"$state/session\" \"$state/log\" 600 sh -c ': > \"$1/ran\"' sh \"$H\"\n"
    "        echo \"status $?\"\n"
    "        [ -e \"$H/ran\" ] && [ ! -e \"$state/session\" ] && echo 'ran; record removed'\n"
    "    fi ;;\n"
    "wtlost)\n"
    "    # A worktree stays while a job of an earlier release, under its run's\n"
    "    # directory, is lost, and is made new once that job's end is proved.\n"
    "    eval \"$(sed -n '/^# mac_live DIR:/,/^# mac_start:/p; /^# job_dir NAME:/,/^# tssh ARG/p' "
    "\"$R\")\"\n"
    "    say() { :; }\n"
    "    g() { git -c user.name=t -c user.email=t@invalid -c core.hooksPath=/dev/null -c "
    "commit.gpgsign=false \"$@\"; }\n"
    "    repo=$H/git state=$H/state macdir=$H/mac RACH2_HOLD_DIR=\n"
    "    { mkdir -p \"$repo/tools/flow\" && cp \"$P\" \"$repo/tools/flow/hold.pl\" && cd \"$repo\" "
    "&&\n"
    "        g init -q && g commit -q --allow-empty -m one; } > /dev/null 2>&1 || exit 1\n"
    "    r=$state/held/release-1\n"
    "    mkdir -p \"$r/jobs/wsl.1\" \"$H/wt\" || exit 1\n"
    "    : > \"$r/live\"\n"
    "    echo 0 > \"$r/end\"\n"
    "    : > \"$r/jobs/wsl.1/live\"\n"
    "    : > \"$H/wt/mine\"\n"
    "    wt_new \"$H/wt\" HEAD\n"
    "    echo \"wt_new $?\"\n"
    "    [ ! -e \"$H/wt/mine\" ] || echo kept\n"
    "    echo 0 > \"$r/jobs/wsl.1/end\"\n"
    "    wt_new \"$H/wt\" HEAD 2> /dev/null\n"
    "    echo \"wt_new $?\"\n"
    "    [ ! -e \"$H/wt/mine\" ] && [ -e \"$H/wt/.git\" ] && [ ! -d \"$r\" ] && echo replaced ;;\n"
    "discardlost)\n"
    "    # A job whose holder ended without its end keeps its directory, and\n"
    "    # discard keeps a part's files until that job's end is proved.\n"
    "    eval \"$(sed -n '/^# job_dir NAME:/,/^# tssh ARG/p' \"$R\")\"\n"
    "    say() { :; }\n"
    "    RACH2_HOLD_DIR=$H/state\n"
    "    j=$(job_dir t) || exit 1\n"
    "    : > \"$j/live\"\n"
    "    : > \"$H/f\"\n"
    "    true &\n"
    "    job_wait \"$! $j\"\n"
    "    echo \"job_wait $?\"\n"
    "    [ ! -d \"$j\" ] || echo stays\n"
    "    discard \"$H/f\"\n"
    "    echo \"discard $?\"\n"
    "    [ ! -e \"$H/f\" ] || echo kept\n"
    "    echo 0 > \"$j/end\"\n"
    "    discard \"$H/f\"\n"
    "    echo \"discard $?\"\n"
    "    [ -e \"$H/f\" ] || echo removed ;;\n"
    "prepare | recovery | keep)\n"
    "    # WSL's script runs here, as a user whose scan of the copy's processes\n"
    "    # cannot read every process's directory, or /proc at all, with an etude\n"
    "    # and a ledger of its own; recovery's etude fails.\n"
    "    # The scan of recovery and keep succeeds and finds nothing, so that\n"
    "    # only the ledger's recovery can fail recovery's.\n"
    "    scan=\n"
    "    [ \"$how\" = prepare ] || scan='s|cat \"$repo/tools/flow/procs.pl\"|echo \"exit 0;\"|'\n"
    "    eval \"$(sed -n '/^prepare_host()$/,/^}$/p' \"$R\" |\n"
    "        sed -e \"s|/usr/local/libexec/rach2/etude|$H/bin/etude|\" -e "
    "\"s|/var/lib/rach2-etude/ledger|$H/ledger|\" ${scan:+-e \"$scan\"})\"\n"
    "    PREPARE_COPY=$H/copy\n"
    "    mkdir -p \"$H/copy/build/held\" \"$H/bin\" \"$H/ledger\" || exit 1\n"
    "    echo 'etude-ledger 1' > \"$H/ledger/claims\"\n"
    "    printf '#!/bin/sh\\nexec /bin/sh -s\\n' > \"$H/bin/ssh\"\n"
    "    printf '#!/bin/sh\\nshift\\nexec \"$@\"\\n' > \"$H/bin/timeout\"\n"
    "    printf '#!/bin/sh\\nexit %s\\n' \"$([ \"$how\" = recovery ] && echo 1 || echo 0)\" > "
    "\"$H/bin/etude\"\n"
    "    chmod +x \"$H/bin/ssh\" \"$H/bin/timeout\" \"$H/bin/etude\"\n"
    "    out=$( (PATH=$H/bin:$PATH; prepare_host) 2> /dev/null)\n"
    "    echo \"status $?\"\n"
    "    printf '%s\\n' \"$out\" | sed \"s|$H|H|g\"\n"
    "    [ ! -d \"$H/copy\" ] || echo kept ;;\n"
    "esac\n";
/**
 * Succeed when a scan of this machine's processes by procs.pl -d cannot read
 * every process's directory, as for a user who is not root, or has no /proc.
 */
static int prepare_scan_fails(void)
{
    char link[4096];
    return readlink("/proc/1/cwd", link, sizeof(link)) < 0;
}
/** Run hold_driver's SCENARIO, and succeed when it printed WANT whole. */
static int hold_saw(struct release *r, const char *scenario, const char *want)
{
    struct fixture *f = r->f;
    const char *a[] = {"/bin/sh", NULL, NULL, NULL, scenario, NULL, NULL};
    const char *at = getenv("PATH") ? getenv("PATH") : "";
    size_t n = strlen(f->bin);
    char path[8200];
    a[1] = flow_path(f, "hold/driver");
    a[2] = flow_path(f, "hold");
    a[3] = flow_source(f, "tools/flow/hold.pl");
    a[5] = flow_source(f, "tools/flow/release.sh");
    /* HOLD_PATH is PATH without the mocks' directory, which leads it. */
    if (!strncmp(at, f->bin, n) && at[n] == ':')
        at += n + 1;
    snprintf(path, sizeof(path), "%s", at);
    fx_env("HOLD_PATH", path);
    fx_env("RUN", flow_path(f, "hold/run"));
    fx_run(f, a, f->work, NULL, NULL);
    fx_env("RUN", NULL);
    fx_env("HOLD_PATH", NULL);
    if (f->out && !strcmp(f->out, want))
        return 1;
    fprintf(stderr, "hold: %s printed:\n%s", scenario, f->out ? f->out : "");
    return 0;
}
/**
 * hold.pl's own protocol, on a run directory of its own: a stop that exists
 * before the start runs nothing and ends 143 (j141 2); a job behind held's
 * gate cannot end before its holder runs, so a fast job's status is its own
 * and not 127 (j141 4), and a stop before the gate opens runs nothing; a job
 * that ignores TERM ends by the KILL of -k, and hold.pl stop waits for it
 * (medium 3), and with -g so does the group's, which the holder outlives to
 * write the end (j154 2); a second holder of the same directory is refused
 * and its `end` stays the first one's (j141 3); a holder killed outright
 * reads lost, and a -g one stays lost once its group is empty, while a
 * process that left the group runs and after, until its boot is another
 * (j154 1, j168 5); on
 * Linux a drain KILLs a child it adopts after the KILL's turn (j154 4).
 * release.sh's own jobs do the same through held, job_wait and job_stop,
 * and a job that opens its reader after hold.pl has written its go still
 * reads it (j154 3); a Mac run is live, and its directory stays, until what
 * its job left in its group has ended, so that no new run starts beside it;
 * a STATE/session of another boot goes, and one whose supervisor was killed
 * while its group runs stays: session.pl -q says lost, wait_session waits,
 * refresh leaves the clone's HEAD where it was, and session.pl starts no
 * session, still once the group is empty, until the record names another
 * boot (j154 1, j163 1, j168 5); a
 * worktree stays while a job of an earlier release is lost under its run's
 * directory, and job_wait and discard keep a lost job's directory and its
 * part's files (j163 2, 3); and -p's check of its copy fails when the scan
 * of its processes fails, or when the ledger's recovery fails while the scan
 * succeeds (j154 5, j163 5), and keeps the copy, with its build, when both
 * succeed.
 */
static int hold_rows(struct release *r)
{
    struct fixture *f = r->f;
    int ok;
    fx_remove(flow_path(f, "hold"));
    if (!flow_put(f, "hold/driver", hold_driver))
        return check(r, 0, "the hold rows' files are written");
    ok = check(r, hold_saw(r, "early", "status 143\nended 143\nran nothing\n"),
               "a stop that exists before the start runs nothing, and the run ends 143");
    ok = check(r, hold_saw(r, "fast", "status 3\nended 3\n"),
               "a job behind held's gate that ends at once ends with its own status") &&
         ok;
    ok = check(r, hold_saw(r, "gated", "status 143\nended 143\nran nothing\n"),
               "a stop before the gate opens runs nothing behind it") &&
         ok;
    ok = check(r, hold_saw(r, "kill", "ended 137\nstop 0\n"),
               "a job that ignores TERM ends by the KILL of -k, which hold.pl stop waits for") &&
         ok;
    ok = check(r, hold_saw(r, "again", "status 125\nended 0\n"),
               "a second holder of a run's directory is refused and leaves the first end") &&
         ok;
    ok = check(r, hold_saw(r, "groupkill", "ended 137\nstop 0\n"),
               "a -g job that ignores TERM ends by the group's KILL, which the holder outlives "
               "to write the end") &&
         ok;
    ok = check(r, hold_saw(r, "lost", "lost\n"), "a holder killed outright reads lost") && ok;
    ok = check(r, hold_saw(r, "lostgroup", "lost\nlost\nlost\nended 137\n"),
               "a -g holder killed outright reads lost while its group runs, and stays lost "
               "once the group is empty, while a process that left it runs and after, until "
               "its boot is another (j168 5)") &&
         ok;
#ifdef __linux__
    ok = check(r, hold_saw(r, "adopt", "status 0\nended 0\n"),
               "a drain KILLs a child it adopts after the KILL's turn, and ends once it has "
               "no child") &&
         ok;
#endif
    ok = check(r, hold_saw(r, "jobfast", "status 3\nremoved\n"),
               "release.sh's held job that ends at once, while its hold.pl is slow to start, "
               "ends with its own status, and job_wait removes its directory") &&
         ok;
    ok = check(r, hold_saw(r, "jobwriter", "status 3\nremoved\n"),
               "release.sh's held job that opens its reader after hold.pl has written its go "
               "and closed its end still reads it") &&
         ok;
    ok = check(r, hold_saw(r, "jobstop", "status 137\nremoved\n"),
               "release.sh's job_stop ends a job that ignores TERM by the KILL of HOLD_KILL") &&
         ok;
    ok = check(r, hold_saw(r, "drain", "live\nkept\nno end\nended 0\nremoved\n"),
               "a Mac run whose job has ended stays live, without its end, while a process it "
               "left runs in its group, and held_gone keeps its directory until the run ends") &&
         ok;
    ok = check(r, hold_saw(r, "session", "wait 0\nrecord removed\nrunning\nended\n"),
               "wait_session removes a STATE/session of another boot, and "
               "session_state reads a session as running while a process holds its lock") &&
         ok;
    ok = check(r,
               hold_saw(r, "lostsession",
                        "wait 1\nkept\nlost\nstatus 125\nran nothing\nlost\nstatus 0\n"
                        "ran; record removed\n"),
               "a session whose supervisor was killed keeps its record, which session_state "
               "reads as lost, wait_session waits until STATE/stop, and session.pl starts no "
               "other, still once its group is empty, until the record names another boot, "
               "and then removes the record (j168 5)") &&
         ok;
    ok = check(r,
               hold_saw(r, "lostrefresh", "refresh 1\ntask\nrefresh 1\ntask\nrefresh 0\nmaster\n"),
               "refresh leaves HEAD where it was while a session whose supervisor was killed "
               "may still run, still once its group is empty, and moves the clone to master "
               "once its record names another boot (j163 1, j168 5)") &&
         ok;
    ok = check(r, hold_saw(r, "wtlost", "wt_new 1\nkept\nwt_new 0\nreplaced\n"),
               "wt_new leaves a worktree while a job of an earlier release is lost under its "
               "run's directory, and makes it new once that job's end is proved (j163 2)") &&
         ok;
    ok = check(r,
               hold_saw(r, "discardlost",
                        "job_wait 127\nstays\ndiscard 1\nkept\ndiscard 0\nremoved\n"),
               "job_wait gives 127 and keeps the directory of a job whose holder ended "
               "without its end, and discard keeps a part's files until that end is proved "
               "(j163 3)") &&
         ok;
    if (prepare_scan_fails())
        ok = check(r,
                   hold_saw(r, "prepare",
                            "status 1\nthe processes in H/copy could not be read\n"
                            "what runs in H/copy is not confirmed stopped\nkept\n"),
                   "-p's check of its copy in WSL fails when the scan of its processes "
                   "fails") &&
             ok;
    ok = check(r,
               hold_saw(r, "recovery",
                        "status 1\netude status did not recover H/ledger/claims\n"
                        "what runs in H/copy is not confirmed stopped\nkept\n"),
               "-p's check of its copy in WSL fails when the ledger's recovery fails while "
               "the scan of its processes succeeds and finds nothing (j163 5)") &&
         ok;
    ok = check(r, hold_saw(r, "keep", "status 0\n\nkept\n"),
               "-p's check of its copy in WSL succeeds when nothing runs there, and keeps the "
               "copy with its build for the next -p") &&
         ok;
    fx_remove(flow_path(f, "hold"));
    return ok;
}
/*
 * The driver of observer_rows: it runs release.sh's windows_detach, with its
 * jobs under DIR/tools/flow/hold.pl in the run directory DIR/state/held, the
 * start read as started and each poll run here by sh, or sweep.sh's outputs,
 * with ssh running Windows' script here on DIR/build, and the jobs that
 * winjob from DIR/bin reads in DIR/jobs, and prints what SCENARIO saw. Usage:
 * sh driver DIR RELEASE SWEEP SCENARIO.
 */
static const char observer_driver[] =
    "W=$1 R=$2 S=$3 how=$4\n"
    "PATH=$W/bin:/usr/bin:/bin FAKE_DIR=$W\n"
    "export PATH FAKE_DIR\n"
    "rm -rf \"$W/out\" \"$W/state\" \"$W/jobs\" \"$W/build\" \"$W/repo\" \"$W/polls\" \"$W/seen\" "
    "\"$W/file\" \"$W/locks\" \"$W/end\" \"$W/locked\"\n"
    "mkdir -p \"$W/out\" \"$W/state\" \"$W/jobs\"\n"
    "case $how in\n"
    "poll)\n"
    "    eval \"$(sed -n '/^# job_dir NAME:/,/^# tssh ARG/p; "
    "/^# windows_detach ID PART FILE CONSOLE:/,/^}$/p' \"$R\")\"\n"
    "    wout=$W/out state=$W/state repo=$W RACH2_HOLD_DIR=$W/state/held\n"
    "    # The start reads as started; each poll runs here.\n"
    "    tssh() {\n"
    "        s=$(cat)\n"
    "        case $s in *END_OF_RUN*) echo 'start: status 0' ;; *) printf '%s\\n' \"$s\" | sh -s "
    ";; esac\n"
    "    }\n"
    "    # Each wait before a poll notes whether the observer has given its\n"
    "    # result yet; the job ends before the third poll. job_wait's own\n"
    "    # waits of a second are no polls.\n"
    "    sleep() {\n"
    "        [ \"$1\" = 60 ] || { command sleep \"$@\"; return; }\n"
    "        n=$(($(cat \"$W/polls\" 2> /dev/null || echo 0) + 1))\n"
    "        echo \"$n\" > \"$W/polls\"\n"
    "        if [ \"$n\" -ge 2 ]; then\n"
    "            if grep -q '^status ' \"$W/file\"; then echo early; else echo waits; fi >> "
    "\"$W/seen\"\n"
    "        fi\n"
    "        [ \"$n\" -lt 3 ] || rm -f \"$W/jobs/rach2-m7.prepare\"\n"
    "        [ \"$n\" -lt 20 ] || kill \"$$\"\n"
    "    }\n"
    "    printf 'building\\nstatus 0\\n' > \"$W/out/m7.prepare.out\"\n"
    "    echo 0 > \"$W/out/m7.prepare.rc\"\n"
    "    echo 'active 1' > \"$W/jobs/rach2-m7.prepare\"\n"
    "    echo 'echo status 0' | windows_detach m7 prepare \"$W/file\" m7.prepare.console\n"
    "    cat \"$W/seen\" \"$W/file\" 2> /dev/null\n"
    "    echo \"polls $(cat \"$W/polls\")\" ;;\n"
    "sweep)\n"
    "    eval \"$(sed -n \"/^stale='\\$/,/^'\\$/p; /^# closed: print/,/^# clones: run/p\" "
    "\"$S\")\"\n"
    "    HOMES='' TESTING=testing days=1 repo=$W/repo\n"
    "    for x in a b c; do\n"
    "        mkdir -p \"$W/build/tmp/milestone7-$x/tmp\" \"$W/build/release\"\n"
    "        : > \"$W/build/tmp/milestone7-$x/tmp/scratch\"\n"
    "        echo n > \"$W/build/release/milestone7-$x.prepare.token\"\n"
    "        echo 0 > \"$W/build/release/milestone7-$x.prepare.rc\"\n"
    "    done\n"
    "    echo 'active 1' > \"$W/jobs/rach2-milestone7-a.prepare\"\n"
    "    : > \"$W/build/release/milestone7-c.prepare.stopped\"\n"
    "    # Each run is years old.\n"
    "    find \"$W/build/tmp\" -exec touch -t 202001010000 {} +\n"
    "    outputs > /dev/null 2>&1\n"
    "    for x in a b c; do\n"
    "        if [ -d \"$W/build/tmp/milestone7-$x\" ]; then echo \"kept $x\"; else echo \"removed "
    "$x\"; fi\n"
    "    done ;;\n"
    "claims)\n"
    "    eval \"$(sed -n \"/^stale='\\$/,/^'\\$/p; /^# closed: print/,/^# clones: run/p\" "
    "\"$S\")\"\n"
    "    HOMES='' TESTING=testing days=1 repo=$W/repo\n"
    "    mkdir -p \"$W/repo/build/flow/release\" \"$W/locks\" \"$W/build/release\"\n"
    "    echo 'closed 1' > \"$W/repo/build/flow/release/7-1\"\n"
    "    echo milestone7-a-windows-1 > \"$W/build/release/windows.current\"\n"
    "    for x in 7-a 7-b 7-c 8-d; do\n"
    "        : > \"$W/locks/milestone$x-windows-1.claim.1.live\"\n"
    "        : > \"$W/locks/milestone$x-windows-1.claim.stop\"\n"
    "    done\n"
    "    # A process holds 7-b's attempt lock until DIR/end.\n"
    "    perl -MFcntl=:flock -e 'open(L, \"<\", shift) && flock(L, LOCK_EX) or exit 1;\n"
    "        open(T, \">\", shift) && close(T); my $e = shift; select(undef, undef, undef, 0.1) "
    "until -e $e' "
    "\\\n"
    "        \"$W/locks/milestone7-b-windows-1.claim.1.live\" \"$W/locked\" \"$W/end\" &\n"
    "    i=0\n"
    "    until [ -e \"$W/locked\" ] || [ \"$i\" -ge 100 ]; do i=$((i + 1)); perl -e 'select(undef, "
    "undef, undef, 0.1)'; done\n"
    "    outputs > /dev/null 2>&1\n"
    "    touch \"$W/end\"\n"
    "    wait\n"
    "    for x in 7-a 7-b 7-c 8-d; do\n"
    "        if ls \"$W/locks\"/milestone$x-* > /dev/null 2>&1; then echo \"kept $x\"; else echo "
    "\"removed $x\"; fi\n"
    "    done ;;\n"
    "esac\n";
/* The winjob of observer_rows: `state NAME` prints DIR/jobs/rach2-ID.PART,
 * or `absent` when it is empty or missing. */
static const char observer_winjob[] =
    "#!/bin/sh\n"
    "n=${2##*\\\\}\n"
    "if [ -s \"$FAKE_DIR/jobs/$n\" ]; then cat \"$FAKE_DIR/jobs/$n\"; else echo absent; fi\n";
/* The ssh of observer_rows: WSL's script runs here with DIR/locks for the
 * claims' files and DIR/build/release for Windows' output directory, and
 * Windows' script with DIR/build for the host's build. */
static const char observer_ssh[] =
    "#!/bin/sh\n"
    "for a; do last=$a; done\n"
    "case $last in\n"
    "*wsl.exe*) sed \"s,/home/testing/rach2/build/flow/claims,$FAKE_DIR/locks,g; "
    "s,/mnt/d/o/,$FAKE_DIR/build/release/,g\" | sh -s ;;\n"
    "*) sed \"s,/d/rach2/build,$FAKE_DIR/build,g\" | sh -s ;;\n"
    "esac\n";
/* The winjob.sh of observer_rows: winjob is DIR/bin/winjob. */
static const char observer_winjob_sh[] = "#!/bin/sh\n"
                                         "echo \"winjob=$FAKE_DIR/bin/winjob\"\n";
/** Run observer_driver's SCENARIO, and succeed when it printed WANT whole. */
static int observer_saw(struct release *r, const char *scenario, const char *want)
{
    struct fixture *f = r->f;
    const char *a[] = {"/bin/sh", NULL, NULL, NULL, NULL, scenario, NULL};
    char driver[4200], dir[4200], release[4200];
    snprintf(driver, sizeof(driver), "%s", flow_path(f, "observer/driver"));
    snprintf(dir, sizeof(dir), "%s", flow_path(f, "observer"));
    snprintf(release, sizeof(release), "%s", flow_source(f, "tools/flow/release.sh"));
    a[1] = driver;
    a[2] = dir;
    a[3] = release;
    a[4] = flow_source(f, "tools/flow/sweep.sh");
    fx_run(f, a, f->work, NULL, NULL);
    if (f->out && !strcmp(f->out, want))
        return 1;
    fprintf(stderr, "observer: %s printed:\n%s", scenario, f->out ? f->out : "");
    return 0;
}
/**
 * A Windows part whose status file exists while its job is still active has
 * not ended (j102 3): windows_detach's poll reads it as running, so that the
 * observer gives no result, and its caller ends no claim in WSL, until a
 * poll finds the job absent; sweep.sh keeps the scratch of a run whose part
 * has a status file and a token and no stopped file, whether its job is
 * active or absent, and removes it once a stop has written the stopped file;
 * and it removes the claim files of a closed milestone's runs in WSL but the
 * current run's and a held claim's.
 */
static int observer_rows(struct release *r)
{
    struct fixture *f = r->f;
    int ok;
    fx_remove(flow_path(f, "observer"));
    if (!flow_put(f, "observer/driver", observer_driver) ||
        !flow_put(f, "observer/machines", "windows out D:/o\n") ||
        !fx_write(flow_path(f, "observer/bin/winjob"), observer_winjob, strlen(observer_winjob),
                  0700) ||
        !fx_write(flow_path(f, "observer/bin/ssh"), observer_ssh, strlen(observer_ssh), 0700) ||
        !fx_write(flow_path(f, "observer/bin/git"), "#!/bin/sh\nexit 0\n", 17, 0700) ||
        !fx_write(flow_path(f, "observer/winjob.sh"), observer_winjob_sh,
                  strlen(observer_winjob_sh), 0700) ||
        !fx_write(flow_path(f, "observer/tools/flow/winjob.sh"), observer_winjob_sh,
                  strlen(observer_winjob_sh), 0700) ||
        !fx_copy(flow_source(f, "tools/flow/bound.pl"), flow_path(f, "observer/bound.pl")) ||
        !fx_copy(flow_source(f, "tools/flow/hold.pl"), flow_path(f, "observer/tools/flow/hold.pl")))
        return check(r, 0, "the observer rows' files are written");
    ok = check(
        r, observer_saw(r, "poll", "waits\nwaits\nbuilding\noutput: status 0\nstatus 0\npolls 3\n"),
        "the observer reads a part whose status file exists beside an active job as "
        "running, and gives its result only once the job is absent (j102 3)");
    ok = check(r, observer_saw(r, "sweep", "kept a\nkept b\nremoved c\n"),
               "sweep.sh keeps the scratch of a run whose part has a status file but no stopped "
               "file, and removes it once a stop has confirmed the part ended (j102 3)") &&
         ok;
    ok = check(r, observer_saw(r, "claims", "kept 7-a\nkept 7-b\nremoved 7-c\nkept 8-d\n"),
               "sweep.sh removes a closed milestone's claim files in WSL, but those of the run "
               "that windows.current names and those of a claim whose lock is held (84-148)") &&
         ok;
    fx_remove(flow_path(f, "observer"));
    return ok;
}
#define WINCLEAN_ID  "milestone7-111111111111-windows-20261010-ab12"
#define WINCLEAN_OUT "windows/out/" WINCLEAN_ID
#define WINCLEAN_JOB "rach2-" WINCLEAN_ID
/**
 * Run winclean.sh with ARGS, a NULL-ended list after TREE and OUT, which are
 * under the work directory, with the mock winjob.exe, as RELEASE_TEST_WINCLEAN
 * says. The jobs that winjob.exe sees are the files of `jobs`.
 */
static void winclean_run(struct release *r, const char *how, const char *const *args)
{
    struct fixture *f = r->f;
    const char *a[12] = {"/bin/sh"};
    char helper[4200];
    unsigned n = 1, i;
    a[n++] = flow_source(f, "tools/flow/winclean.sh");
    for (i = 0; args[i] && n < 11; i++)
        a[n++] = args[i];
    a[n] = NULL;
    snprintf(helper, sizeof(helper), "%s/winjob", f->bin);
    fx_env("RELEASE_TEST_CALLS", flow_path(f, "calls"));
    fx_env("RELEASE_TEST_HOLD", "");
    fx_env("RELEASE_TEST_JOBS", flow_path(f, "jobs"));
    fx_env("RELEASE_TEST_WINCLEAN", how);
    fx_env("winjob", helper);
    fx_run(f, a, f->work, NULL, NULL);
    fx_env("RELEASE_TEST_WINCLEAN", NULL);
    fx_env("winjob", NULL);
}
/**
 * Run winclean.sh on the run WINCLEAN_ID as RELEASE_TEST_WINCLEAN says, its
 * preparation's job JOB, `active N` or none when NULL, its status file RC,
 * missing when NULL, and its token TOKEN, missing when NULL.
 */
static void winclean(struct release *r, const char *how, const char *job, const char *rc,
                     const char *token)
{
    struct fixture *f = r->f;
    const char *args[] = {NULL, NULL, WINCLEAN_ID, NULL};
    args[0] = flow_path(f, "windows/tree");
    args[1] = flow_path(f, "windows/out");
    fx_remove(flow_path(f, "windows"));
    fx_remove(flow_path(f, "jobs"));
    fx_mkdir(flow_path(f, "jobs"));
    flow_put(f, "calls", "");
    if (job)
        flow_put(f, "jobs/" WINCLEAN_JOB ".prepare", job);
    if (rc)
        flow_put(f, WINCLEAN_OUT ".prepare.rc", rc);
    if (token)
        flow_put(f, WINCLEAN_OUT ".prepare.token", token);
    flow_put(f, WINCLEAN_OUT ".prepare.out", "building\n");
    flow_put(f, "windows/tree/build/tmp/" WINCLEAN_ID "/tmp/scratch", "");
    winclean_run(r, how, args);
}
/** Succeed when winclean.sh removed the run's scratch and exited 0. */
static int winclean_removed(struct release *r)
{
    struct fixture *f = r->f;
    return f->status == 0 && !fx_exists(flow_path(f, "windows/tree/build/tmp/" WINCLEAN_ID));
}
/** Succeed when winclean.sh kept the run's scratch and exited 1, saying why. */
static int winclean_kept(struct release *r, const char *why)
{
    struct fixture *f = r->f;
    return f->status == 1 &&
           fx_exists(flow_path(f, "windows/tree/build/tmp/" WINCLEAN_ID "/tmp/scratch")) &&
           f->err && strstr(f->err, why) &&
           strstr(f->err, "winclean: " WINCLEAN_ID " still runs; ");
}
/** Succeed when the part PART of WINCLEAN_ID has a token of `stop`. */
static int winclean_token_stops(struct release *r, const char *part)
{
    char path[200], *text;
    int held;
    snprintf(path, sizeof(path), WINCLEAN_OUT ".%s.token", part);
    text = fx_read(flow_path(r->f, path));
    held = text && !strcmp(text, "stop\n");
    free(text);
    return held;
}
static int winclean_more_rows(struct release *r);
static int winclean_mode_rows(struct release *r);
/**
 * winclean.sh replaces a part's token by rename and never truncates it in
 * place: a start that has created the token and not yet written its nonce
 * holds the file open, and its write must not land in the token after a stop
 * has confirmed the part ended (j90 1). A second link to the token stands for
 * that open file, and keeps the nonce.
 */
static int winclean_renamed_row(struct release *r)
{
    struct fixture *f = r->f;
    const char *args[] = {NULL, NULL, WINCLEAN_ID, NULL};
    char token[4200], *held;
    int linked, ok;
    args[0] = flow_path(f, "windows/tree");
    args[1] = flow_path(f, "windows/out");
    fx_remove(flow_path(f, "windows"));
    fx_remove(flow_path(f, "jobs"));
    fx_mkdir(flow_path(f, "jobs"));
    flow_put(f, "calls", "");
    flow_put(f, WINCLEAN_OUT ".prepare.token", "a1b2c3\n");
    snprintf(token, sizeof(token), "%s", flow_path(f, WINCLEAN_OUT ".prepare.token"));
    linked = link(token, flow_path(f, "windows/held")) == 0;
    winclean_run(r, "", args);
    held = fx_read(flow_path(f, "windows/held"));
    ok = check(r,
               linked && f->status == 0 && winclean_token_stops(r, "prepare") && held &&
                   !strcmp(held, "a1b2c3\n") &&
                   called(r, "^winjob stop " WINCLEAN_JOB ".prepare token=stop$"),
               "winclean.sh puts a new stop token in place of the part's token, leaving the file "
               "that a start may hold open as it was");
    free(held);
    return ok;
}
/**
 * winclean.sh stops a run's part by its job, after it has written `stop` to
 * the part's token, so that a start in flight starts nothing; it marks the
 * part stopped and removes the run's scratch once the job is absent, and
 * keeps the scratch and fails while the job stays or Windows cannot say.
 */
static int winclean_rows(struct release *r)
{
    struct fixture *f = r->f;
    int ok;
    winclean(r, "", "active 3\n", NULL, "a1b2c3\n");
    ok = check(r,
               winclean_removed(r) &&
                   called(r, "^winjob stop " WINCLEAN_JOB ".prepare token=stop$") &&
                   called(r, "^winjob stop " WINCLEAN_JOB ".cases token=stop$") &&
                   !fx_exists(flow_path(f, "jobs/" WINCLEAN_JOB ".prepare")) &&
                   fx_exists(flow_path(f, WINCLEAN_OUT ".prepare.stopped")) &&
                   fx_exists(flow_path(f, WINCLEAN_OUT ".cases.stopped")),
               "winclean.sh writes each part's stop token before it ends the part's job, and "
               "removes the scratch once both jobs are absent");
    winclean(r, "stuck", "active 3\n", NULL, "a1b2c3\n");
    ok = check(r,
               winclean_kept(r, "winclean: the prepare of " WINCLEAN_ID
                                " may still run: winjob says active 3") &&
                   winclean_token_stops(r, "prepare") &&
                   !fx_exists(flow_path(f, WINCLEAN_OUT ".prepare.stopped")),
               "winclean.sh fails and keeps the scratch of a preparation whose job stays") &&
         ok;
    winclean(r, "unknown", "active 3\n", NULL, "a1b2c3\n");
    ok = check(r,
               winclean_kept(r, "may still run: winjob says unknown") &&
                   !fx_exists(flow_path(f, WINCLEAN_OUT ".prepare.stopped")),
               "winclean.sh takes a job that Windows cannot read as one that may still run") &&
         ok;
    winclean(r, "", NULL, NULL, NULL);
    ok = check(r,
               winclean_removed(r) && winclean_token_stops(r, "prepare") &&
                   winclean_token_stops(r, "cases") &&
                   fx_exists(flow_path(f, WINCLEAN_OUT ".prepare.stopped")),
               "winclean.sh leaves a stop token for a part that has not started, so that its "
               "start, which fails when the token exists, starts nothing") &&
         ok;
    ok = winclean_renamed_row(r) && ok;
    ok = winclean_more_rows(r) && ok;
    return winclean_mode_rows(r) && ok;
}
/**
 * winclean.sh fails without its helper, keeps a kept run's scratch, and judges
 * a removal by the last try's own result, whatever the account.
 */
static int winclean_more_rows(struct release *r)
{
    struct fixture *f = r->f;
    const char *a[] = {"/bin/sh", NULL, NULL, NULL, WINCLEAN_ID, NULL};
    int ok;
    a[1] = flow_source(f, "tools/flow/winclean.sh");
    a[2] = flow_path(f, "windows/tree");
    a[3] = flow_path(f, "windows/out");
    fx_mkdir(flow_path(f, "windows/out"));
    fx_env("winjob", NULL);
    fx_run(f, a, f->work, NULL, NULL);
    ok = check(r, f->status == 1 && f->err && strstr(f->err, "winclean: no winjob program"),
               "winclean.sh fails when it has no winjob program to ask");
    fx_env("RELEASE_TEST_RM", "6");
    winclean(r, "", NULL, NULL, NULL);
    ok = check(r,
               winclean_removed(r) && count(flow_path(f, "calls"), "^rm refused$") == 6 &&
                   count(flow_path(f, "calls"), "^sleep 5$") == 6,
               "winclean.sh succeeds when the removal of a run's scratch succeeds on its last "
               "try") &&
         ok;
    fx_env("RELEASE_TEST_RM", "7");
    winclean(r, "", NULL, NULL, NULL);
    fx_env("RELEASE_TEST_RM", NULL);
    ok = check(r,
               f->status == 1 && f->err && strstr(f->err, "winclean: cannot remove:") &&
                   count(flow_path(f, "calls"), "^rm refused$") == 7,
               "winclean.sh fails when the last removal of a run's scratch fails") &&
         ok;
    fx_remove(flow_path(f, "windows"));
    flow_put(f, "windows/tree/build/tmp/" WINCLEAN_ID "/KEPT", "failed: x; evidence: y\n");
    {
        const char *args[] = {NULL, NULL, WINCLEAN_ID, NULL};
        args[0] = flow_path(f, "windows/tree");
        args[1] = flow_path(f, "windows/out");
        fx_mkdir(flow_path(f, "windows/out"));
        flow_put(f, "calls", "");
        winclean_run(r, "", args);
    }
    ok = check(r,
               f->status == 0 &&
                   fx_exists(flow_path(f, "windows/tree/build/tmp/" WINCLEAN_ID "/KEPT")),
               "winclean.sh keeps the scratch of a run that holds KEPT") &&
         ok;
    return ok;
}
/**
 * winclean.sh -a makes the current run none and stops every run that may
 * still run, the last gate's among them, so that a start a dead release left
 * on its way starts nothing after stop.sh (j90 5). X is its arguments.
 */
static int winclean_none_row(struct release *r, const char *const *x)
{
    struct fixture *f = r->f;
    char *current;
    int ok;
    fx_remove(flow_path(f, "windows"));
    flow_put(f, "windows/out/windows.current", "milestone7-new\n");
    flow_put(f, "windows/out/milestone7-new.prepare.token", "n\n");
    flow_put(f, "windows/out/milestone7-e.cases.token", "n\n");
    winclean_run(r, "", x);
    current = fx_read(flow_path(f, "windows/out/windows.current"));
    ok = check(r,
               f->status == 0 && f->out && !*f->out && current && !strcmp(current, "none\n") &&
                   fx_exists(flow_path(f, "windows/out/milestone7-new.prepare.stopped")) &&
                   fx_exists(flow_path(f, "windows/out/milestone7-e.cases.stopped")),
               "winclean.sh -a makes the current run none and stops every run that may still "
               "run, the last gate's among them (j90 5)");
    free(current);
    return ok;
}
/**
 * winclean.sh -l lists each run of OUT with a part whose token exists and
 * that has not ended, and fails when OUT cannot be read; -s says each part's
 * state; -g writes the gate's run before it stops every other listed run, and
 * names each that stays; -a writes none and stops every listed run.
 */
static int winclean_mode_rows(struct release *r)
{
    struct fixture *f = r->f;
    const char *a[] = {"/bin/sh", NULL, "-l", NULL, NULL, NULL};
    const char *s[] = {"-s", NULL, NULL, "milestone7-a", NULL};
    const char *g[] = {"-g", NULL, NULL, "milestone7-new", NULL};
    const char *x[] = {"-a", NULL, NULL, NULL};
    char script[4200], tree[4200], out[4200], *current;
    int ok;
    /* flow_path's results are reused after a few dozen calls. */
    snprintf(script, sizeof(script), "%s", flow_source(f, "tools/flow/winclean.sh"));
    snprintf(tree, sizeof(tree), "%s", flow_path(f, "windows/tree"));
    snprintf(out, sizeof(out), "%s", flow_path(f, "windows/out"));
    a[1] = script;
    a[3] = tree;
    a[4] = out;
    fx_remove(flow_path(f, "windows"));
    fx_remove(flow_path(f, "jobs"));
    fx_mkdir(flow_path(f, "jobs"));
    flow_put(f, "windows/out/milestone7-a.prepare.token", "n\n");
    flow_put(f, "windows/out/milestone7-a.prepare.sh", "");
    flow_put(f, "windows/out/milestone7-b.cases.token", "n\n");
    flow_put(f, "windows/out/milestone7-b.cases.rc", "0\n");
    flow_put(f, "windows/out/milestone7-c.prepare.sh", "");
    flow_put(f, "windows/out/milestone7-d.prepare.token", "stop\n");
    flow_put(f, "windows/out/milestone7-d.prepare.stopped", "");
    flow_put(f, "windows/out/milestone7-new.prepare.token", "n\n");
    fx_run(f, a, f->work, NULL, NULL);
    ok = check(r,
               f->status == 0 && f->out &&
                   !strcmp(f->out, "orphan milestone7-a\norphan milestone7-b\n"
                                   "orphan milestone7-new\n"),
               "winclean.sh -l lists the runs of OUT with a token and no stopped file, one "
               "with a status file among them, since its job may outlive it (j90 6)");
    flow_put(f, "jobs/rach2-milestone7-a.prepare", "active 2\n");
    s[1] = a[3];
    s[2] = a[4];
    winclean_run(r, "", s);
    ok = check(r,
               f->status == 0 && f->out &&
                   !strcmp(f->out, "state milestone7-a.prepare active 2\n"
                                   "state milestone7-a.cases absent\n"),
               "winclean.sh -s says the state of each part's job") &&
         ok;
    g[1] = x[1] = a[3];
    g[2] = x[2] = a[4];
    flow_put(f, "calls", "");
    winclean_run(r, "stuck", g);
    current = fx_read(flow_path(f, "windows/out/windows.current"));
    ok = check(r,
               f->status == 1 && f->out && !strcmp(f->out, "left milestone7-a\n") && current &&
                   !strcmp(current, "milestone7-new\n") &&
                   called(r, "^winjob stop rach2-milestone7-a.prepare token=stop$") &&
                   !called(r, "milestone7-new.prepare token=") &&
                   fx_exists(flow_path(f, "jobs/rach2-milestone7-a.prepare")),
               "winclean.sh -g names the gate's run and names an earlier run whose job stays") &&
         ok;
    free(current);
    winclean_run(r, "", g);
    ok = check(r,
               f->status == 0 && f->out && !*f->out &&
                   fx_exists(flow_path(f, "windows/out/milestone7-a.prepare.stopped")),
               "winclean.sh -g stops every earlier run that may still run") &&
         ok;
    ok = winclean_none_row(r, x) && ok;
    fx_remove(flow_path(f, "windows"));
    fx_run(f, a, f->work, NULL, NULL);
    ok = check(r, f->status == 1, "winclean.sh -l fails when it cannot read OUT") && ok;
    return ok;
}
/** Count the times TEXT holds WORD. */
static unsigned count_text(const char *text, const char *word)
{
    unsigned n = 0;
    while ((text = strstr(text, word)) != NULL) {
        n++;
        text += strlen(word);
    }
    return n;
}
static int stop_unknown_rows(struct release *r);
static int stop_wsl_rows(struct release *r);
/**
 * Run stop.sh with FLAG, or with none when FLAG is NULL, from the work tree,
 * whose build/flow/release holds the release's records.
 */
static void stop_run(struct release *r, const char *flag)
{
    struct fixture *f = r->f;
    const char *a[] = {"/bin/sh", NULL, NULL, NULL};
    char path[4096];
    snprintf(path, sizeof(path), "%s/tools/flow/stop.sh", r->tree);
    a[1] = path;
    a[2] = flag;
    flow_put(f, "calls", "");
    fx_env("RELEASE_TEST_CALLS", flow_path(f, "calls"));
    fx_run(f, a, f->work, NULL, NULL);
}
/** Run stop.sh -n, which changes nothing, as stop_run does. */
static void stop_list(struct release *r)
{
    stop_run(r, "-n");
}
/**
 * Start perl with CODE, which prints a line in each process it makes once
 * that process is ready, wait for LINES lines and return its process, or -1.
 */
static pid_t stop_perl(const char *code, int lines)
{
    int fd[2];
    pid_t c;
    char ch;
    if (pipe(fd))
        return -1;
    c = fork();
    if (c == 0) {
        dup2(fd[1], 1);
        close(fd[0]);
        close(fd[1]);
        execlp("perl", "perl", "-e", code, (char *)NULL);
        _exit(127);
    }
    close(fd[1]);
    while (c > 0 && lines > 0 && read(fd[0], &ch, 1) == 1)
        if (ch == '\n')
            lines--;
    close(fd[0]);
    return c > 0 && !lines ? c : -1;
}
/**
 * Start the work tree's hold.pl on the run directory DIR, made new here, in
 * a process group of its own, for a perl that creates DIR.ready and sleeps,
 * ignoring TERM when IGNORE is set; return the holder once its job is
 * ready, or -1.
 */
static pid_t stop_holder(struct release *r, const char *dir, int ignore)
{
    char hold[4200], ready[4300];
    pid_t c;
    int i;
    snprintf(hold, sizeof(hold), "%s/tools/flow/hold.pl", r->tree);
    snprintf(ready, sizeof(ready), "%s.ready", dir);
    fx_remove(dir);
    remove(ready);
    if (!fx_mkdir(dir))
        return -1;
    c = fork();
    if (c == 0) {
        int null = open("/dev/null", O_RDWR);
        setpgid(0, 0);
        if (null < 0 || dup2(null, 0) < 0 || dup2(null, 1) < 0 || dup2(null, 2) < 0)
            _exit(126);
        execlp("perl", "perl", hold, "run", dir, "perl", "-e",
               "$SIG{TERM} = 'IGNORE' if shift; open(R, '>', shift) && close(R); sleep 600",
               ignore ? "1" : "0", ready, (char *)NULL);
        _exit(127);
    }
    if (c > 0)
        setpgid(c, c);
    for (i = 0; c > 0 && i < 1500 && !fx_exists(ready); i++)
        fx_pause();
    return c > 0 && fx_exists(ready) ? c : -1;
}
/** Kill the process group that the holder HOLDER leads, and reap it. */
static void stop_reap(pid_t holder)
{
    if (holder > 0) {
        kill(-holder, SIGKILL);
        waitpid(holder, NULL, 0);
    }
}
/**
 * stop.sh without -n, while the release whose directory is RELEASE runs
 * under its holder HELD: it lists and counts a process on the Mac that no
 * hold.pl holds and leaves it running, leaves the release, and halt asks each
 * loop and its session to stop by their files, counts a lock it cannot probe
 * as held and reports a STATE/session whose supervisor is gone.
 */
static int stop_loop_rows(struct release *r, const char *release, pid_t held)
{
    struct fixture *f = r->f;
    char path[4300], text[9000], w1[4200], w2[4200], w3[4200];
    int ok, unreadable, stopped;
    pid_t loose;
    snprintf(w1, sizeof(w1), "%s", flow_path(f, "tree/tools/flow/build/flow/w1"));
    snprintf(w2, sizeof(w2), "%s", flow_path(f, "tree/tools/flow/build/flow/w2"));
    snprintf(w3, sizeof(w3), "%s", flow_path(f, "tree/tools/flow/build/flow/w3"));
    snprintf(path, sizeof(path), "%s/session.live", w1);
    flow_put(f, "tree/tools/flow/build/flow/w1/loop.log", "");
    fx_text(path, "");
    chmod(path, 0);
    unreadable = access(path, R_OK) != 0;
    flow_put(f, "tree/tools/flow/build/flow/w2/loop.log", "");
    /* w2's names this process's group, which runs; w3's another boot. */
    snprintf(text, sizeof(text), "%ld -\n", (long)getpgrp());
    flow_put(f, "tree/tools/flow/build/flow/w2/session", text);
    flow_put(f, "tree/tools/flow/build/flow/w3/loop.log", "");
    flow_put(f, "tree/tools/flow/build/flow/w3/session", "99999999 another-boot\n");
    loose = stop_perl("$| = 1; $0 = 'qemu-system-i386 stoprow'; print qq(\\n); sleep 600", 1);
    stop_run(r, NULL);
    snprintf(text, sizeof(text), "stop: left: mac: %ld qemu-system-i386 stoprow\n", (long)loose);
    ok = check(r,
               loose > 0 && f->status == 1 && f->out && strstr(f->out, text) && kill(loose, 0) == 0,
               "stop.sh lists and counts a process on the Mac that no hold.pl holds, and "
               "leaves it running");
    snprintf(text, sizeof(text), "stop: left: mac: release %s: running\n", release);
    ok = check(r, f->out && strstr(f->out, text) && held > 0 && kill(held, 0) == 0,
               "stop.sh without -r leaves a running release and counts it") &&
         ok;
    snprintf(path, sizeof(path), "%s/stop", w1);
    stopped = fx_exists(path);
    snprintf(path, sizeof(path), "%s/session.stop", w2);
    stopped = stopped && fx_exists(path);
    snprintf(text, sizeof(text),
             "/build/flow/w2/session names process group %ld, whose session.pl was killed, and "
             "its session is not proved ended; a person stops what is left of it and removes "
             "the record\n",
             (long)getpgrp());
    snprintf(path, sizeof(path), "%s/session", w3);
    ok = check(r,
               stopped && f->out &&
                   (!unreadable ||
                    strstr(f->out, "/build/flow/w1: its session.pl has not ended in ninety "
                                   "seconds\n")) &&
                   strstr(f->out, text) && strstr(f->out, "/build/flow/w3 ended\n") &&
                   !fx_exists(path),
               "halt asks each loop and session to stop by its files, counts a lock it cannot "
               "probe as held, reports a STATE/session whose supervisor is gone while its "
               "group runs, and removes one of another boot") &&
         ok;
    snprintf(path, sizeof(path), "%s/session.live", w1);
    chmod(path, 0600);
    fx_remove(w1);
    fx_remove(w2);
    fx_remove(w3);
    if (loose > 0) {
        kill(loose, SIGKILL);
        waitpid(loose, NULL, 0);
    }
    return ok;
}
/**
 * stop.sh -r, while the release whose directory is RELEASE runs, and beside
 * it a Mac run whose job ignores TERM: the release ends through its hold.pl,
 * and the Mac run, which -r cannot end in its ten minutes, is listed and
 * counted.
 */
static int stop_release_rows(struct release *r, const char *release)
{
    struct fixture *f = r->f;
    char text[9000], mac[4200];
    int kept, left, ended;
    pid_t held_mac;
    snprintf(mac, sizeof(mac), "%s", flow_path(f, "tree/tools/flow/build/machines/mac/held/m1"));
    held_mac = stop_holder(r, mac, 1);
    snprintf(text, sizeof(text), "%s m1\n", mac);
    flow_put(f, "tree/tools/flow/build/machines/mac/run", text);
    stop_run(r, "-r");
    snprintf(text, sizeof(text), "stop: left: mac: mac %s: running\n", mac);
    kept = held_mac > 0 && f->status == 1 && f->out && strstr(f->out, text) &&
           strstr(f->out, "stop: the release did not end in ten minutes; it stays\n");
    left = f->out && strstr(f->out, "stop: left: mac: release ") != NULL;
    snprintf(text, sizeof(text), "perl tree/tools/flow/hold.pl state '%s'", release);
    fx_sh(f, text);
    ended = f->out && !strcmp(f->out, "ended 143\n");
    stop_reap(held_mac);
    return check(r, kept && !left && ended,
                 "stop.sh -r ends a release through its hold.pl, and lists and counts a Mac run "
                 "whose job ignores TERM");
}
/**
 * stop.sh's selection, G12 and G19 of dev-log/84-locks/84-111-lifecycle.md:
 * a command line that holds tools/flow/release.sh is never one of the
 * processes it stops; a release is a run whose hold.pl holds its run
 * directory's lock, as STATE/run and the Mac's run file name it, and is
 * listed as left until it ends; a stop without -n as stop_loop_rows says,
 * with -r as stop_release_rows says, and in WSL as stop_wsl_rows says.
 */
static int stop_rows(struct release *r)
{
    struct fixture *f = r->f;
    char path[4096], text[9000], release[4200];
    int fd[2], ok;
    pid_t c, held_release;
    const char *files[] = {"stop.sh",  "lib.sh",   "machines", "winclean.sh", "winjob.sh",
                           "winjob.c", "bound.pl", "procs.pl", "hold.pl",     "session.pl"};
    unsigned i;
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/tools/flow/%s", r->tree, files[i]);
        snprintf(text, sizeof(text), "tools/flow/%s", files[i]);
        if (!fx_copy(flow_source(f, text), path))
            return check(r, 0, "stop.sh's files are copied");
    }
    /* stop.sh finds its main working tree by git's common directory, which
     * the git mock names .git beside it. */
    if (!fx_mkdir(flow_path(f, "tree/tools/flow/.git")))
        return check(r, 0, "stop.sh's tree has a .git");
    if (pipe(fd))
        return check(r, 0, "a pipe for stop.sh's rows");
    c = fork();
    if (c == 0) {
        const char *env[] = {"PATH=/bin:/usr/bin", NULL};
        dup2(fd[0], 0);
        close(fd[0]);
        close(fd[1]);
        execle("/bin/sh", "sh", "-c", "while read x; do :; done", "sh",
               "/p/etude /q/tools/flow/release.sh", (char *)NULL, env);
        _exit(127);
    }
    close(fd[0]);
    fx_remove(flow_path(f, "tree/tools/flow/build"));
    stop_list(r);
    snprintf(text, sizeof(text), "stop: left: mac: %ld ", (long)c);
    ok = check(r, c > 0 && f->out && !strstr(f->out, text),
               "stop.sh takes no process for its command line's release.sh");
    snprintf(release, sizeof(release), "%s",
             flow_path(f, "tree/tools/flow/build/flow/release/held/release-1"));
    held_release = stop_holder(r, release, 0);
    snprintf(text, sizeof(text), "%s\n", release);
    flow_put(f, "tree/tools/flow/build/flow/release/run", text);
    stop_list(r);
    snprintf(text, sizeof(text), "stop: left: mac: release %s: running\n", release);
    ok = check(r, held_release > 0 && f->out && strstr(f->out, text) && f->status == 1,
               "stop.sh -n lists and counts as left a release whose hold.pl holds its run's "
               "lock") &&
         ok;
    ok = stop_loop_rows(r, release, held_release) && ok;
    ok = stop_release_rows(r, release) && ok;
    stop_reap(held_release);
    close(fd[1]);
    if (c > 0) {
        kill(c, SIGKILL);
        waitpid(c, NULL, 0);
    }
    fx_remove(flow_path(f, "tree/tools/flow/build"));
    return stop_wsl_rows(r) && stop_unknown_rows(r) && ok;
}
#ifdef __linux__
/** Return how many processes /proc lists. */
static int proc_count(void)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d)) != NULL)
        n += isdigit((unsigned char)e->d_name[0]) != 0;
    if (d)
        closedir(d);
    return n;
}
/**
 * Put in BUF, of N bytes, field 22 of the proc(5) stat of process PID, its
 * start in clock ticks after the boot, or `?` when it cannot be read.
 */
static void proc_start(pid_t pid, char *buf, size_t n)
{
    char path[64], *text, *p;
    int field;
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    text = fx_read(path);
    p = text ? strrchr(text, ')') : NULL;
    /* P is at the end of field 2; each space before it opens the next. */
    for (field = 2; p && field < 22; field++)
        p = strchr(p + 1, ' ');
    snprintf(buf, n, "?");
    if (p)
        snprintf(buf, n, "%.*s", (int)strcspn(p + 1, " \n"), p + 1);
    free(text);
}
/** Succeed when a line of OUT that begins `stop: left: wsl: ` holds TEXT. */
static int wsl_line(const char *out, const char *text)
{
    const char *p, *e;
    for (p = out; p && *p; p = *e ? e + 1 : e) {
        e = strchr(p, '\n');
        if (!e)
            e = p + strlen(p);
        if (!strncmp(p, "stop: left: wsl: ", 17)) {
            const char *t = strstr(p, text);
            if (t && t < e)
                return 1;
        }
    }
    return 0;
}
#endif
/**
 * stop.sh's selection in WSL, G15 and G19 of
 * dev-log/84-locks/84-111-lifecycle.md, which procs.pl makes from /proc
 * through pidfds, here on this machine's own processes: a QEMU is one of
 * them, a claim's session, by its runner as leader, is spared (F3), and so
 * is a command line that holds release.sh; and a scan that cannot read every
 * process fails whole. Linux alone has pidfds; elsewhere the row passes
 * unchecked.
 */
static int stop_wsl_rows(struct release *r)
{
#ifdef __linux__
    struct fixture *f = r->f;
    struct passwd *pw = getpwuid(getuid());
    char text[400], start[32], command[5000];
    int ok;
    pid_t claim, qemu, release;
    claim = stop_perl("use POSIX; POSIX::setsid(); $| = 1; "
                      "$0 = 'perl /x/build/flow/claims/m7.claim.1.pl run'; "
                      "if (!fork) { $0 = '/usr/local/libexec/rach2/etude launch build env'; "
                      "print qq(\\n); sleep 600; exit } print qq(\\n); sleep 600",
                      2);
    qemu = stop_perl("$| = 1; $0 = 'qemu-system-x86_64 -x /etude z'; print qq(\\n); sleep 600", 1);
    release = stop_perl("$| = 1; $0 = 'sh /x/tools/flow/release.sh /etude y'; print qq(\\n); "
                        "sleep 600",
                        1);
    fx_env("RELEASE_TEST_WSL", "1");
    stop_list(r);
    fx_env("RELEASE_TEST_WSL", NULL);
    proc_start(qemu, start, sizeof(start));
    snprintf(text, sizeof(text), "stop: left: wsl: %ld %s %s qemu-system-x86_64 -x /etude z\n",
             (long)qemu, start, pw ? pw->pw_name : "?");
    ok = check(r,
               claim > 0 && qemu > 0 && release > 0 && pw && f->out && strstr(f->out, text) &&
                   !wsl_line(f->out, "etude launch build env") &&
                   !wsl_line(f->out, "release.sh /etude y") && !strstr(f->out, "unknown"),
               "stop.sh takes a QEMU in WSL, by a pidfd, and spares a claim's session, by its "
               "runner as leader, and a command line that holds release.sh");
    if (claim > 0) {
        kill(-claim, SIGKILL);
        waitpid(claim, NULL, 0);
    }
    if (qemu > 0) {
        kill(qemu, SIGKILL);
        waitpid(qemu, NULL, 0);
    }
    if (release > 0) {
        kill(release, SIGKILL);
        waitpid(release, NULL, 0);
    }
    /* Each process that -e selects keeps its pidfd until the scan ends, so
     * that sixteen descriptors run out among more than forty processes. */
    if (proc_count() > 40) {
        snprintf(command, sizeof(command), "ulimit -n 16 && exec perl '%s' -e .",
                 flow_source(f, "tools/flow/procs.pl"));
        fx_sh(f, command);
        ok =
            check(r, f->status == 1 && (!f->out || !*f->out) && f->err && strstr(f->err, "procs: "),
                  "procs.pl fails a scan that runs out of descriptors whole, and prints "
                  "nothing") &&
            ok;
    }
    return ok;
#else
    (void)r;
    return 1;
#endif
}
/**
 * stop.sh counts as left what it cannot read: a testing host that does not
 * answer, and a list of Windows' release runs that fails (V9).
 */
static int stop_unknown_rows(struct release *r)
{
    struct fixture *f = r->f;
    int ok;
    fx_env("RELEASE_TEST_DOWN", "1");
    stop_list(r);
    fx_env("RELEASE_TEST_DOWN", NULL);
    ok = check(
        r,
        f->status != 0 && f->out &&
            strstr(f->out, "stop: left: wsl: unknown: the testing host did not answer\n") &&
            strstr(f->out, "stop: left: windows: unknown: the testing host did not answer\n") &&
            strstr(f->out, "stop: left: windows: unknown: the release runs that may still "
                           "run could not be listed\n") &&
            !strstr(f->out, "stop: everything is stopped"),
        "stop.sh counts a testing host that does not answer as left");
    fx_env("RELEASE_TEST_HOLD", "unlisted");
    stop_list(r);
    fx_env("RELEASE_TEST_HOLD", NULL);
    ok = check(r,
               f->status != 0 && f->out &&
                   strstr(f->out, "stop: left: windows: unknown: the release runs that may still "
                                  "run could not be listed\n") &&
                   called(r, "^list # winclean.sh: "),
               "stop.sh counts a list of Windows' release runs that fails as left") &&
         ok;
    return ok;
}
/** The Windows run's rows, its claims, holds, observer, winclean and stop.sh
 * among them, and a release whose full run and Windows run both fail, which
 * collects the failures of every run before it ends. */
static int windows_rows(struct release *r)
{
    struct fixture *f = r->f;
    char last[400], id[200], text[4000];
    int ok = windows_pass_row(r);
    ok = windows_failure_rows(r) && ok;
    ok = windows_hold_rows(r) && ok;
    ok = windows_stop_rows(r) && ok;
    ok = claim_rows(r) && ok;
    ok = hold_rows(r) && ok;
    ok = observer_rows(r) && ok;
    ok = winclean_rows(r) && ok;
    ok = stop_rows(r) && ok;
    snprintf(text, sizeof(text),
             "milestone 7\nsha %s\nbranch %s\nchosen %s\ntouched %s\ndispatched "
             "1760054400\nrun 101 failure\n",
             H, BRANCH, T, T);
    run(r, &(struct release_row){.record = text,
                                 .git = "yes",
                                 .machines = WINDOWS,
                                 .failures = "i686:fs\n",
                                 .windows = "run: result: failed: x86_64:agents\nstatus 1"});
    machine_line(r, "windows started ", id, sizeof(id));
    snprintf(last, sizeof(last),
             "failed tests: full run 101: i686:fs; windows run %s: x86_64:agents", id);
    ok = check(r,
               f->status == 1 && last_is(record(r), last) &&
                   called(r, "^gh run download 101 --repo toksaitov/rach2 --name failures ") &&
                   !called(r, "^(testing|dispatch|gh issue)"),
               "the failures of every run are collected before the release ends") &&
         ok;
    return ok;
}
/** The Lenovo's preflight comes after the runs on the other machines, and
 * before the build. */
static int lenovo_off_row(struct release *r)
{
    struct fixture *f = r->f;
    run(r, &(struct release_row){.record = r->head,
                                 .git = "yes",
                                 .machines = WINDOWS,
                                 .windows = "status 0",
                                 .lenovo = "off"});
    return check(r,
                 f->status == 75 && count(record(r), "^windows done ") == 1 &&
                     before(r, "\nwindows cd D:/w || ", "\nmachine read\n") &&
                     !called(r, "^testing ") && strstr(f->out, "does not answer"),
                 "a Lenovo that does not answer holds up the build, not the Windows run");
}
static int lenovo_rows(struct release *r)
{
    struct fixture *f = r->f;
    char last[400], id[200];
    int ok = 1;
    run(r, &(struct release_row){.record = r->tried,
                                 .git = "yes",
                                 .writes = "yes",
                                 .accept = "pass",
                                 .machines = LENOVO,
                                 .native = "pass"});
    ok = check(r,
               !f->status && archived(r, "7-11111111-") &&
                   before(r, "accept 192.168.0.106\n", "native-deploy stage ") &&
                   before(r, "native-deploy stage ", "native-deploy run ") &&
                   before(r, "native-deploy run ", "machine write bootctl commit\n") &&
                   called(r, "^native-deploy run .*/state/wt/build/x86_64/images/../tests/native/"
                             "prepared 192.168.0.106 candidate .*/rach2/kernel .* /var/native "
                             "milestone7-111111111111-lenovo-[0-9a-f-]+ .* --hostpeer "
                             "build/tools/dev/native-hostpeer --peer 192.168.0.100 .* --purpose "
                             "phase_first_run --$") &&
                   count(r->retired, "^lenovo done ") == 1,
               "acceptance reads the trial boot's log before the native cases run, and the "
               "trial is committed after both") &&
         ok;
    run(r, &(struct release_row){
               .record = r->tried, .accept = "pass", .machines = LENOVO, .native = "fail"});
    machine_line(r, "lenovo started ", id, sizeof(id));
    snprintf(last, sizeof(last), "failed tests: lenovo run %s: dirprobe.ext2", id);
    /* Stopping the console's term for the run is the one write expected. */
    ok = check(r,
               f->status == 1 && last_is(record(r), last) && called(r, "^accept 192.168.0.106$") &&
                   count(flow_path(f, "calls"), "^machine write") ==
                       count(flow_path(f, "calls"), "^machine write stop term$") &&
                   !called(r, "^gh issue"),
               "native failure is recorded and keeps the trial uncommitted") &&
         ok;
    fx_env("RELEASE_TEST_BOOT", "same");
    run(r, &(struct release_row){
               .record = r->tried, .accept = "pass", .machines = LENOVO, .native = "fail"});
    ok = check(r, called(r, "^machine write hup 4711$"),
               "the console's login that took term's place is ended by its number in the boot "
               "whose ID the release read") &&
         ok;
    fx_env("RELEASE_TEST_BOOT", "again");
    run(r, &(struct release_row){
               .record = r->tried, .accept = "pass", .machines = LENOVO, .native = "fail"});
    fx_env("RELEASE_TEST_BOOT", NULL);
    ok = check(r,
               !called(r, "^machine write hup") && f->out &&
                   strstr(f->out, "lenovo: the console's login 4711 was gone, "
                                  "or the machine had booted again"),
               "no signal goes to the login's number once the Lenovo has booted again") &&
         ok;
    return ok;
}
/** Succeed when directory `dir` has no entry whose name holds `text`. */
static int none_named(const char *dir, const char *text)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int none = 1;
    if (!d)
        return 1;
    while ((e = readdir(d)) != NULL)
        none = none && !strstr(e->d_name, text);
    closedir(d);
    return none;
}
#define PREPARED "milestone7-111111111111-"
/**
 * -p prepares each machine at once and removes what it made but the copy
 * on the testing host, which keeps its build for the next -p, writing no
 * record and touching neither the full run nor the Lenovo; a failed part
 * is named with the lines that say why.
 */
static int prepare_rows(struct release *r)
{
    struct fixture *f = r->f;
    int ok;
    if (!mac_tree(r))
        return 0;
    run(r, &(struct release_row){
               .git = "yes", .machines = MAC WINDOWS, .testing = "pass", .preparing = 1});
    ok = check(r,
               !f->status && strstr(f->out, "release: prepare: passed " H "\n") &&
                   strstr(f->out, "release: prepare: wsl: passed\n") &&
                   strstr(f->out, "release: prepare: windows: passed\n") &&
                   strstr(f->out, "release: prepare: mac: passed\n") &&
                   called(r, "^testing " FLOW_OWNER_HOME "/rach2/build/worktrees/prepare run "
                             "\\{ cp /etc/rach2/doom1.wad ") &&
                   called(r, "^ +ARCH='i686 x86_64' tests/run style host$") &&
                   called(r, "^windows .* run routine-prepare phase_first_run D:/w "
                             "D:/o/" PREPARED "windows-prepare-[0-9a-f-]+ both ") &&
                   called(r, "^etude .* run routine-prepare phase_first_run .*/mac/tree "
                             ".*/mac/runs/" PREPARED "mac-prepare-[0-9a-f-]+ both ") &&
                   !called(r, "routine-(sequence|collect)|^dispatch|^machine |^gh ") &&
                   called(r, "^windows cd D:/o && rm -rf " PREPARED "windows-prepare-") &&
                   !called(r, "^wsl rm -rf ") && !fx_exists(flow_path(f, "state/record")) &&
                   !fx_exists(flow_path(f, "state/prepare/wt")) &&
                   !fx_exists(flow_path(f, "tree/build/machines/mac/run")) &&
                   none_named(flow_path(f, "tree/build/machines/mac/runs"), "prepare"),
               "-p prepares each machine at once and removes what it made but the host's "
               "copy");
    run(r, &(struct release_row){.git = "yes", .preparing = 1});
    ok = check(r,
               f->status == 1 && strstr(f->out, "release: prepare: wsl: failed; ") &&
                   strstr(f->out, "\n    testing: the host did not answer\n") &&
                   strstr(f->out, "release: prepare: failed: wsl\n") &&
                   !fx_exists(flow_path(f, "state/record")),
               "-p names the part that failed and the lines that say why") &&
         ok;
    return ok;
}
/**
 * -p stopped while Windows prepares keeps the claim in WSL and the files on
 * Windows of a preparation that winclean.sh cannot confirm stopped.
 */
static int prepare_stop_row(struct release *r)
{
    struct fixture *f = r->f;
    run(r,
        &(struct release_row){
            .git = "yes", .machines = WINDOWS, .testing = "pass", .preparing = 1, .hold = "stuck"});
    return check(r,
                 stopped_by_term(r) && called(r, "^windows # winclean.sh: ") &&
                     !called(r, "^windows cd D:/o && rm -rf " PREPARED "windows-prepare-") &&
                     !called(r, "^wsl touch /mnt/d/o/" PREPARED "windows-prepare-[0-9a-f-]+"
                                "\\.prepared$") &&
                     !called(r, "^wsl perl - stop " CLAIMS " " PREPARED "windows-prepare-") &&
                     strstr(f->out, "release: prepare: windows: the run " PREPARED
                                    "windows-prepare-") != NULL &&
                     strstr(f->out, ", and its files in D:/o stay\n") != NULL,
                 "-p stopped while Windows prepares keeps the claim and the files of a "
                 "preparation that winclean.sh cannot confirm stopped");
}
/** -o leaves out the Mac and Windows, which the machines file names. */
static int only_rows(struct release *r)
{
    struct fixture *f = r->f;
    char text[1000];
    int ok = 1;
    run(r, &(struct release_row){.record = r->ended,
                                 .sha = C,
                                 .git = "yes",
                                 .runs = "none",
                                 .machines = MAC WINDOWS,
                                 .only = 1});
    ok = check(r,
               f->status == 75 && fx_has(record(r), "\nonly wsl lenovo\n") &&
                   count(flow_path(f, "calls"), "^dispatch ") == 1 && !called(r, " run routine-") &&
                   !fx_has(record(r), "\nmac started") && !fx_has(record(r), "\nwindows started"),
               "-o leaves out the Mac and Windows and says so in the record") &&
         ok;
    run(r,
        &(struct release_row){
            .git = "yes", .machines = MAC WINDOWS, .testing = "pass", .preparing = 1, .only = 1});
    ok = check(r,
               !f->status && strstr(f->out, "release: prepare: passed " H "\n") &&
                   strstr(f->out, "release: prepare: wsl: passed\n") &&
                   !strstr(f->out, "release: prepare: windows:") &&
                   !strstr(f->out, "release: prepare: mac:") && !called(r, "routine-prepare"),
               "-p -o prepares only WSL") &&
         ok;
    snprintf(text, sizeof(text),
             "milestone 7\nsha %s\nbranch %s\nchosen %s\nonly wsl lenovo\ntouched %s\ndispatched "
             "1760054400\nrun 101 failure\n",
             H, BRANCH, T, T);
    run(r, &(struct release_row){.record = text,
                                 .git = "yes",
                                 .machines = MAC WINDOWS,
                                 .failures = "i686:fs\n",
                                 .only = 1});
    ok = check(r,
               f->status == 1 && fx_has(record(r), "\nfailed tests: full run 101: i686:fs") &&
                   !called(r, " run routine-") && !fx_has(record(r), "\nmac started") &&
                   !fx_has(record(r), "\nwindows started"),
               "-o leaves them out after a full run that failed") &&
         ok;
    run(r, &(struct release_row){.git = "yes", .again = "mac", .only = 1});
    ok = check(r, f->status == 2 && !fx_exists(record(r)), "-o with -a is refused") && ok;
    return ok;
}
/** Wait for the end of the Windows run that -a started detached, in the run
 * directory that STATE/windows.run names, as held_ended does. */
static void windows_ended(struct release *r)
{
    char *dir = fx_read(flow_path(r->f, "state/windows.run"));
    if (dir) {
        dir[strcspn(dir, "\n")] = 0;
        if (*dir)
            held_ended(dir);
    }
    free(dir);
}
/** -a refuses a run that passed and a commit off the branch. */
static int restart_refusal_rows(struct release *r)
{
    struct fixture *f = r->f;
    char text[9000];
    int ok = 1;
    snprintf(text, sizeof(text), "%smac started m\nmac done %s run m\n", r->built, T);
    run(r, &(struct release_row){
               .record = text, .sha = C, .git = "yes", .machines = MAC, .again = "mac"});
    ok = check(r,
               f->status == 2 && fx_equal(record(r), text) &&
                   strstr(f->err, "release: refused: mac: its run passed"),
               "-a refuses a machine whose run passed") &&
         ok;
    snprintf(text, sizeof(text), "%smac started m\nfailure mac m i686:gnu\n", r->built);
    run(r, &(struct release_row){.record = text,
                                 .sha = C,
                                 .git = "yes",
                                 .machines = MAC,
                                 .ancestor = "branch",
                                 .again = "mac"});
    ok = check(r,
               f->status == 2 && fx_equal(record(r), text) &&
                   strstr(f->err, "release: refused: " C " is not on " BRANCH),
               "-a refuses a commit that is not on the milestone's branch") &&
         ok;
    return ok;
}
/**
 * -a starts the Mac's failed run at a newer commit; the release under way
 * takes its pass and ends before the build, and -r at that commit keeps it.
 */
static int restart_mac_rows(struct release *r)
{
    struct fixture *f = r->f;
    char text[9000], id[200], last[600];
    char *kept;
    int ok = 1;
    /* A run of mac_rows in the same second would have this run's name. */
    if (!mac_tree(r))
        return 0;
    mac_failed(r, text, sizeof(text));
    run(r, &(struct release_row){
               .record = text, .sha = C, .git = "yes", .machines = MAC, .again = "mac"});
    ok = check(r,
               !f->status && mac_ran(r, id, sizeof(id)) &&
                   !strncmp(id, "milestone7-222222222222-mac-", 28) && mac_passed(r, id) &&
                   count(record(r), "^failure newer mac milestone7-2") == 1 &&
                   count(record(r), "^mac started ") == 2 && !called(r, "^(dispatch|testing)"),
               "-a starts the Mac's failed run again at a newer commit") &&
         ok;
    kept = fx_read(record(r));
    run(r, &(struct release_row){.record = kept, .machines = MAC});
    free(kept);
    snprintf(last, sizeof(last),
             "release: failed: mac run %s: tested " C
             ", not the release's commit; release that commit with -r",
             id);
    ok = check(r,
               f->status == 1 && count(record(r), "^mac done .* run milestone7-2") == 1 &&
                   !failed_mac(r) && ended_with(r, last) && !called(r, "^(testing|machine)"),
               "the release under way takes the run of -a and ends before the build") &&
         ok;
    kept = fx_read(record(r));
    run(r,
        &(struct release_row){.record = kept, .sha = C, .git = "yes", .machines = MAC, .rerun = 1});
    ok = check(r,
               f->status == 75 && count(record(r), "^mac done .* run milestone7-2") == 1 &&
                   !fx_has(record(r), "mac started") && !fx_has(record(r), "again mac") &&
                   !fx_has(record(r), "failure newer") && called(r, "^testing "),
               "-r at the commit of -a keeps the Mac's pass") &&
         ok;
    run(r, &(struct release_row){
               .record = kept, .git = "yes", .runs = "none", .machines = MAC, .rerun = 1});
    free(kept);
    ok = check(r,
               f->status == 75 && !fx_has(record(r), "mac done ") && mac_ran(r, id, sizeof(id)) &&
                   !strncmp(id, "milestone7-111111111111-mac-", 28),
               "-r at the release's own commit runs the Mac again") &&
         ok;
    return ok;
}
/** -a starts Windows' failed run in a process of its own, which records it. */
static int restart_windows_row(struct release *r)
{
    struct fixture *f = r->f;
    char text[4000];
    snprintf(text, sizeof(text),
             "%swindows started w\nfailure windows w runner\nfailed tests: windows run w: "
             "runner\n",
             r->head);
    run(r, &(struct release_row){.record = text,
                                 .git = "yes",
                                 .machines = WINDOWS,
                                 .windows = "status 0",
                                 .again = "windows"});
    windows_ended(r);
    return check(r,
                 !f->status && count(record(r), "^windows started " WINDOWS_ID) == 1 &&
                     count(record(r), "^windows done .* run " WINDOWS_ID) == 1 &&
                     !fx_has(record(r), "failure newer") && called(r, " run routine-collect ") &&
                     called(r, " run routine-prepare "),
                 "-a runs Windows' failed run again detached and records its pass");
}
/**
 * install_row() - Check that the release asks for the term console, and for
 * the Intel driver when the build's installer has --i915, when it updates the
 * Lenovo's installed root, and then checks the menu and the trial boot's
 * command line for rach2.i915.
 * @r: the release under test.
 *
 * Return: 1 when the row passes.
 */
static int install_row(struct release *r)
{
    return check(
        r,
        fx_has(r->script, "install_words='--console term'\n") &&
            fx_has(r->script, "grep -q -e '\\[--i915\\]' \"$state/wt/utils/rach2-install\"") &&
            fx_has(r->script, "if i915; then\n"
                              "            install_words=\"$install_words --i915\"") &&
            fx_has(r->script, "setsid sh -c 'rach2-install $install_words --update "
                              "/dev/shm/root.ext2 /dev/disk$disk") &&
            fx_has(r->script, "fail install \"the installed menu on $p1 lacks "
                              "rach2.i915: $bare\"") &&
            fx_has(r->script, "fail trial \"the trial boot's command line lacks "
                              "rach2.i915: "),
        "the release requests term, and the Intel driver when the build's installer "
        "has --i915, and checks the menu and the trial boot for rach2.i915");
}

int flow_release(struct fixture *f)
{
    struct release r;
    int ok = 1;
    if (!setup(&r, f))
        return 0;
    ok = install_row(&r);
    ok = finished_rows(&r) && ok;
    ok = open_rows(&r) && ok;
    ok = closing_rows(&r) && ok;
    ok = argument_rows(&r) && ok;
    ok = new_record_rows(&r) && ok;
    ok = machine_record_rows(&r) && ok;
    ok = rerun_rows(&r) && ok;
    ok = machine_rerun_rows(&r) && ok;
    ok = build_rows(&r) && ok;
    ok = trial_rows(&r) && ok;
    ok = slots_rows(&r) && ok;
    ok = acceptance_rows(&r) && ok;
    ok = windows_rows(&r) && ok;
    ok = lenovo_off_row(&r) && ok;
    ok = lenovo_rows(&r) && ok;
    ok = mac_rows(&r) && ok;
    ok = prepare_rows(&r) && ok;
    ok = prepare_stop_row(&r) && ok;
    ok = only_rows(&r) && ok;
    ok = restart_refusal_rows(&r) && ok;
    ok = restart_mac_rows(&r) && ok;
    ok = restart_windows_row(&r) && ok;
    fx_env("FLOW_RELEASE", NULL);
    return ok;
}
