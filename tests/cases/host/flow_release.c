/* flow_release.c: resume records, reruns of failures, the build's results,
 * the install's move of the kernel slots, the recovery trial's fallback, the
 * trial boot's command line, acceptance order and the preparation of -p. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
 * Succeed when the claim whose file `got` a granted claim touches has been
 * granted and has not ended, as the calls so far record: the claim's script,
 * which claim_start writes line by line, touches `got`, and the release has
 * not touched the claim's end file since. Repeated run identifiers must not
 * let a probe use a claim from an earlier attempt. In `probe` mode, reject
 * the first probe, the WSL line `line`, after each grant is recorded.
 */
static int granted(const char *got, const char *line)
{
    char touch[4200], end[4200], probe[4200], *calls = fx_read(ev("RELEASE_TEST_CALLS"));
    const char *dot = strrchr(got, '.'), *last = NULL, *at = calls;
    int made;
    if (!dot) {
        free(calls);
        return 0;
    }
    snprintf(touch, sizeof(touch), "touch %s ", got);
    snprintf(end, sizeof(end), "wsl touch %.*s.%s\n", (int)(dot - got), got,
             !strcmp(dot, ".claimed") ? "prepared" : "end");
    while (at && (at = strstr(at, touch)) != NULL)
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
 * Answer a script for WSL, whose lines are recorded: claim_state's, whose
 * first line `if [ -e GOT ] && ...; then echo granted` names the file a
 * granted claim touches, prints `granted`, `refused` or, while the claim
 * waits, as the whole host's always does in `wait` mode, nothing; `cat` of a
 * claim attempt's output, OUT/ID.KIND.N, prints the launcher's refusal when
 * the claim is refused. Any other script, which starts or stops a claim,
 * touches a file or tests that a claim has ended, succeeds.
 */
static int wsl_mock(void)
{
    char line[20000], b[4096], got[4200] = "", probe[4096] = "";
    int alone;
    size_t n;
    while (fgets(b, sizeof(b), stdin)) {
        snprintf(line, sizeof(line), "wsl %s", b);
        append(ev("RELEASE_TEST_CALLS"), line);
        if (!*got && !strncmp(b, "if [ -e ", 8) && strstr(b, "; then echo granted")) {
            snprintf(got, sizeof(got), "%.*s", (int)strcspn(b + 8, " "), b + 8);
            snprintf(probe, sizeof(probe), "%s", b);
        }
        n = strcspn(b, "\n");
        /* The attempt's number, `.N`, ends the output's name. */
        while (n > 4 && isdigit((unsigned char)b[n - 1]))
            n--;
        if (n > 4 && b[n - 1] == '.')
            n--;
        if (!strncmp(b, "cat ", 4) && n > 5 &&
            ((n > 6 && !strncmp(b + n - 6, ".claim", 6) && refused(0)) ||
             (!strncmp(b + n - 5, ".hold", 5) && refused(1))))
            puts("etude-launch: result: refused: admission: no room in 20400 seconds");
    }
    if (!*got)
        return 0;
    alone = strlen(got) > 5 && !strcmp(got + strlen(got) - 5, ".held");
    if (refused(alone))
        puts("refused");
    else if (!(alone && !strcmp(ev("RELEASE_TEST_HOLD"), "wait")) && granted(got, probe))
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
    int gate = strstr(command, "bash.exe -l -s -g ") != NULL;
    int list = strstr(command, "bash.exe -l -s -l ") != NULL;
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
    /* stop.sh's script for WSL, run here on RELEASE_TEST_PS's processes. */
    if (strstr(command, "wsl.exe -u root -- sh -s") && *ev("RELEASE_TEST_PS")) {
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
    if (strstr(command, "(term)")) {
        append(ev("RELEASE_TEST_CALLS"), "machine write stop term\n");
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
 * sends TERM to the release, the leader of the process group that the
 * fixture starts, and lasts until the release has ended, so that the part is
 * still running when the release stops it.
 */
static void stop_release(void)
{
    const char *hold = ev("RELEASE_TEST_HOLD");
    struct timespec pause = {0, 20000000};
    pid_t release = getpgrp();
    int i;
    if (strcmp(hold, "stop") && strcmp(hold, "stuck") && strcmp(hold, "stuck-cases"))
        return;
    if (!fx_has(ev("RELEASE_TEST_CALLS"),
                strcmp(hold, "stuck-cases") ? " run routine-prepare " : " run routine-collect ") ||
        fx_has(ev("RELEASE_TEST_CALLS"), "\nrelease stopped\n"))
        return;
    append(ev("RELEASE_TEST_CALLS"), "release stopped\n");
    kill(release, SIGTERM);
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
 * Answer ps: WSL's `ps -eo pid=,sid=,user=,args=` of stop.sh prints
 * RELEASE_TEST_PS while that is set; any other ps is the system's.
 */
static int ps_mock(int argc, char **argv)
{
    if (*ev("RELEASE_TEST_PS") && argc == 3 && !strcmp(argv[1], "-eo")) {
        fputs(ev("RELEASE_TEST_PS"), stdout);
        return 0;
    }
    argv[0] = "ps";
    execv("/bin/ps", argv);
    execv("/usr/bin/ps", argv);
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
    if (!strcmp(name, "ps"))
        return ps_mock(argc, argv);
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
    const char *again; /* the machine of -a, or NULL for a release */
    int rerun, preparing, only;
};

static int setup(struct release *r, struct fixture *f)
{
    char path[4096], source[200];
    const char *windows[] = {"winclean.sh", "winjob.sh", "winjob.c"};
    unsigned i;
    const char *mock[] = {"sh", "gh", "ssh", "git", "make", "sleep", "scp", "winjob", "rm", "ps"};
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
     * the lines of winjob.sh, which hold winjob.c. */
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
               called(r, "^git worktree remove --force .*/state/wt$") &&
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
 * Find the Mac run that the record says started, copy its identity to `id`,
 * and wait up to ten seconds for its times file to record its end, since the
 * release starts the run detached.
 */
static int mac_ran(struct release *r, char *id, size_t size)
{
    char path[4200];
    struct timespec pause = {0, 50000000};
    unsigned i;
    if (!machine_line(r, "mac started ", id, size))
        return 0;
    snprintf(path, sizeof(path), "%s/tree/build/machines/mac/runs/%s.times", r->f->work, id);
    for (i = 0; i < 200; i++) {
        if (fx_exists(path) && fx_has(path, "\nend "))
            return 1;
        /* interval: poll the Mac run's times file every 50 ms */
        nanosleep(&pause, NULL);
    }
    return 0;
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
                   called(r, "^ +sh ports/doom/fetch_wad.sh && ARCH=x86_64 make clean && "
                             "ARCH=x86_64 make machine GNU_AGENTS=all && "),
               "machine build takes the host's WAD and starts clean");
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
    run(r, &(struct release_row){
               .record = text, .sha = C, .runs = "none", .git = "yes", .machines = MAC});
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
/* The claims' locks and tokens, on ext4 in the owner's clone in WSL. */
#define WINDOWS_LOCKS "/home/testing/rach2/build/flow/claims/" WINDOWS_ID
/**
 * Succeed when Windows' preparation ran under its own claim in WSL, whose
 * runner runs in a session of its own under the claim's lock and token, and
 * whose hold lasts while the release beats and the token is the attempt's,
 * and ends once the preparation does.
 */
static int windows_prepared(struct release *r)
{
    return before(r, " env JOBS=30 /usr/local/libexec/rach2/etude-launch build env \\\n",
                  "\nwindows cd D:/w || ") &&
           called(r, "^wsl cd /tmp && setsid nohup perl " WINDOWS_HELD
                     "[0-9a-f-]+\\.claim\\.1\\.pl " WINDOWS_LOCKS
                     "[0-9a-f-]+\\.claim\\.lock " WINDOWS_LOCKS
                     "[0-9a-f-]+\\.claim\\.token 'go 1' ") &&
           called(r, "^wsl +sh -c 'touch " WINDOWS_HELD "[0-9a-f-]+\\.claimed ") &&
           called(r, "^wsl +while \\[ ! -e " WINDOWS_HELD
                     "[0-9a-f-]+\\.prepared ] && \\[ \"\\$\\(cat " WINDOWS_LOCKS
                     "[0-9a-f-]+\\.claim\\.token 2> /dev/null\\)\" = \"go 1\" ] &&$") &&
           called(r, "^wsl +find " WINDOWS_HELD "[0-9a-f-]+\\.alive -mmin -10 ") &&
           before(r, "\nwsl if [ -e " WINDOWS_HELD, "\nwindows cd D:/w || ") &&
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
           called(r, "^wsl cd /tmp && setsid nohup perl " WINDOWS_HELD
                     "[0-9a-f-]+\\.hold\\.1\\.pl " WINDOWS_LOCKS "[0-9a-f-]+\\.hold\\.lock ") &&
           called(r, "^wsl +while \\[ ! -e " WINDOWS_HELD "[0-9a-f-]+\\.end ] && \\[ ") &&
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
                   before(r, "\nwindows # winclean.sh: ", "\nwsl k=" WINDOWS_LOCKS) &&
                   called(r, "^wsl k=" WINDOWS_LOCKS "[0-9a-f-]+\\.claim c=") &&
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
                   !called(r, "^wsl k=" WINDOWS_LOCKS) &&
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
                   !called(r, "^wsl k=" WINDOWS_LOCKS "[0-9a-f-]+\\.hold c=") &&
                   strstr(f->out, " is not confirmed stopped on Windows; ") != NULL,
               "cases that winclean.sh cannot confirm stopped keep the whole host's claim") &&
         ok;
    return ok;
}
/*
 * The driver of claim_rows: it runs release.sh's claim functions here, with
 * ssh, setsid and etude-launch from DIR/bin, on DIR's files, and prints what
 * scenario SCENARIO saw. Usage: sh driver DIR RELEASE SCENARIO.
 */
static const char claim_driver[] =
    "W=$1 R=$2 how=$3\n"
    "PATH=$W/bin:/usr/bin:/bin FAKE_DIR=$W RACH2_CLAIM_LEDGER=$W/ledger RACH2_CLAIM_ETUDE=true\n"
    "case $how in dead) FAKE_LAUNCH=grant ;; late) FAKE_LAUNCH=wait ;; orphan) "
    "FAKE_LAUNCH=stubborn ;; *) FAKE_LAUNCH=$how ;; esac\n"
    "export PATH FAKE_DIR FAKE_LAUNCH RACH2_CLAIM_LEDGER RACH2_CLAIM_ETUDE\n"
    "wheld=$W/held wlock=$W/locks state=$W/state WSL=wsl\n"
    "rm -rf \"$wheld\" \"$wlock\" \"$state\" \"$W/launcher\"\n"
    "mkdir -p \"$wheld\" \"$state\"\n"
    "trap 'if read -r p l < \"$W/launcher\" 2> /dev/null; then kill -KILL \"$l\" \"$p\" 2> "
    "/dev/null; "
    "fi' EXIT\n"
    "eval \"$(sed -n '/^tssh()$/,/^}$/p' \"$R\")\"\n"
    "eval \"$(sed -n '/^# claim_runner: print/,/^# beat ID/p' \"$R\")\"\n"
    "pause() { perl -e 'select(undef, undef, undef, 0.1)'; }\n"
    "look() {\n"
    "    i=0\n"
    "    while s=$(claim_state m7 claim 1 m7.claimed); do\n"
    "        case $1 in +) [ -z \"$s\" ] || break ;; *) [ \"$s\" != \"$1\" ] || break ;; esac\n"
    "        [ \"$i\" -lt 150 ] || break\n"
    "        i=$((i + 1))\n"
    "        pause\n"
    "    done\n"
    "    echo \"state ${s:-none}\"\n"
    "}\n"
    "until_file() { i=0; until [ -e \"$1\" ] || [ \"$i\" -ge 150 ]; do i=$((i + 1)); pause; done; "
    "}\n"
    "gone() {\n"
    "    i=0\n"
    "    while kill -0 \"$1\" 2> /dev/null && [ \"$i\" -lt 150 ]; do i=$((i + 1)); pause; done\n"
    "    kill -0 \"$1\" 2> /dev/null || echo \"gone $2\"\n"
    "}\n"
    "touch \"$wheld/m7.alive\"\n"
    "claim_start m7 claim 1 'etude-launch alone' m7.claimed m7.prepared > /dev/null 2>&1\n"
    "until_file \"$W/launcher\"\n"
    "read -r p l < \"$W/launcher\"\n"
    "case $how in\n"
    "grant)\n"
    "    look granted\n"
    "    claim_stop m7 claim\n"
    "    echo \"stop $?\"\n"
    "    [ -e \"$wlock/m7.claim.lock\" ] || [ -e \"$wlock/m7.claim.token\" ] || echo 'lock "
    "removed'\n"
    "    gone \"$l\" launcher ;;\n"
    "dead)\n"
    "    look granted\n"
    "    rm -f \"$wheld/m7.alive\"\n"
    "    look dead ;;\n"
    "wait)\n"
    "    pause\n"
    "    s=$(claim_state m7 claim 1 m7.claimed)\n"
    "    echo \"state ${s:-none}\"\n"
    "    claim_stop m7 claim\n"
    "    echo \"stop $?\"\n"
    "    gone \"$l\" launcher ;;\n"
    "refuse)\n"
    "    look + ;;\n"
    "orphan)\n"
    "    kill -KILL \"$p\"\n"
    "    gone \"$p\" runner\n"
    "    s=$(claim_state m7 claim 1 m7.claimed)\n"
    "    echo \"orphan ${s:-none}\"\n"
    "    kill -KILL \"$l\"\n"
    "    look dead ;;\n"
    "late)\n"
    "    claim_stop m7 claim\n"
    "    echo \"stop $?\"\n"
    "    rm -f \"$wheld/m7.claim.1.started\" \"$wheld/m7.claim.1.rc\" \"$W/launcher\"\n"
    "    perl \"$wheld/m7.claim.1.pl\" \"$wlock/m7.claim.lock\" \"$wlock/m7.claim.token\" 'go 1' "
    "\"$wheld/m7.claim.1\" env etude-launch alone true\n"
    "    [ -e \"$wheld/m7.claim.1.started\" ] || [ -e \"$W/launcher\" ] || echo 'late started "
    "nothing'\n"
    "    look + ;;\n"
    "esac\n";
/* The launcher of claim_rows, as FAKE_LAUNCH says: `grant` runs its command,
 * `wait` waits for admission, `stubborn` too, ignoring TERM, and `refuse`
 * refuses. It writes its parent, the attempt's runner, and itself. */
static const char claim_launcher[] =
    "#!/bin/sh\n"
    "echo \"$PPID $$\" > \"$FAKE_DIR/launcher.part\" && mv \"$FAKE_DIR/launcher.part\" "
    "\"$FAKE_DIR/launcher\"\n"
    "case $1 in build) shift 2 ;; *) shift ;; esac\n"
    "case $FAKE_LAUNCH in\n"
    "grant) exec \"$@\" ;;\n"
    "wait) exec sleep 1000 ;;\n"
    "stubborn) trap '' TERM; while :; do sleep 1; done ;;\n"
    "*) echo 'etude-launch: result: refused: admission: no room'; exit 1 ;;\n"
    "esac\n";
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
 * release.sh's claims of WSL's admission on a real lock: a granted claim
 * reads granted only while its lock is held, and reads dead once its holder
 * has ended though GOT stays (j79 3); claim_stop ends a granted claim and one
 * that waits, through the token, and confirms it by the lock; a launcher
 * whose runner was killed keeps the lock, so the claim reads neither free nor
 * dead until it ends (j79 4); a late runner of a stopped attempt starts
 * nothing; and a refused claim reads refused.
 */
static int claim_rows(struct release *r)
{
    struct fixture *f = r->f;
    int ok;
    fx_remove(flow_path(f, "claims"));
    if (!flow_put(f, "claims/driver", claim_driver) ||
        !fx_write(flow_path(f, "claims/bin/etude-launch"), claim_launcher, strlen(claim_launcher),
                  0700) ||
        !fx_write(flow_path(f, "claims/bin/ssh"), "#!/bin/sh\nexec /bin/sh -s\n", 26, 0700) ||
        !fx_write(
            flow_path(f, "claims/bin/setsid"),
            "#!/bin/sh\nexec perl -MPOSIX -e 'POSIX::setsid(); exec @ARGV or exit 127' "
            "\"$@\"\n",
            strlen("#!/bin/sh\nexec perl -MPOSIX -e 'POSIX::setsid(); exec @ARGV or exit 127' "
                   "\"$@\"\n"),
            0700))
        return check(r, 0, "the claim rows' files are written");
    ok = check(r, claim_saw(r, "grant", "state granted\nstop 0\nlock removed\ngone launcher\n"),
               "a granted claim reads granted, and claim_stop ends it and removes its lock");
    ok = check(r, claim_saw(r, "dead", "state granted\nstate dead\n"),
               "a granted claim whose holder ended reads dead, though GOT stays") &&
         ok;
    ok = check(r, claim_saw(r, "wait", "state none\nstop 0\ngone launcher\n"),
               "a claim that waits for admission reads nothing, and claim_stop ends it") &&
         ok;
    ok = check(r, claim_saw(r, "refuse", "state refused\n"), "a refused claim reads refused") && ok;
    ok = check(r, claim_saw(r, "orphan", "gone runner\norphan none\nstate dead\n"),
               "a launcher whose runner was killed keeps the claim's lock until it ends") &&
         ok;
    ok = check(r, claim_saw(r, "late", "stop 0\nlate started nothing\nstate unstarted\n"),
               "a late runner of a stopped attempt starts nothing, and the attempt reads "
               "unstarted") &&
         ok;
    fx_remove(flow_path(f, "claims"));
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
 * winclean.sh -l lists each run of OUT with a part whose token exists and
 * that has not ended, and fails when OUT cannot be read; -s says each part's
 * state; -g writes the gate's run before it stops every other listed run, and
 * names each that stays.
 */
static int winclean_mode_rows(struct release *r)
{
    struct fixture *f = r->f;
    const char *a[] = {"/bin/sh", NULL, "-l", NULL, NULL, NULL};
    const char *s[] = {"-s", NULL, NULL, "milestone7-a", NULL};
    const char *g[] = {"-g", NULL, NULL, "milestone7-new", NULL};
    char *current;
    int ok;
    a[1] = flow_source(f, "tools/flow/winclean.sh");
    a[3] = flow_path(f, "windows/tree");
    a[4] = flow_path(f, "windows/out");
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
                   !strcmp(f->out, "orphan milestone7-a\norphan milestone7-new\n"),
               "winclean.sh -l lists the runs of OUT with a token and no end");
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
    g[1] = a[3];
    g[2] = a[4];
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
/**
 * Run stop.sh -n, which changes nothing, from the work tree, whose
 * build/flow/release holds the release's records.
 */
static void stop_list(struct release *r)
{
    struct fixture *f = r->f;
    const char *a[] = {"/bin/sh", NULL, "-n", NULL};
    char path[4096];
    snprintf(path, sizeof(path), "%s/tools/flow/stop.sh", r->tree);
    a[1] = path;
    flow_put(f, "calls", "");
    fx_env("RELEASE_TEST_CALLS", flow_path(f, "calls"));
    fx_run(f, a, f->work, NULL, NULL);
}
/**
 * stop.sh's selection, G12 and G15 of dev-log/84-locks/84-111-lifecycle.md:
 * a command line that holds tools/flow/release.sh is never one of the
 * processes it stops, and is a release only while STATE/pid names it with
 * its start (F2); and in WSL the sessions of several claims are each spared
 * (F3), as is a command line that holds release.sh.
 */
static int stop_rows(struct release *r)
{
    struct fixture *f = r->f;
    char path[4096], text[400], command[8000];
    int fd[2], ok;
    pid_t c;
    const char *files[] = {"stop.sh", "lib.sh", "machines", "winclean.sh", "winjob.sh", "winjob.c"};
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
    snprintf(command, sizeof(command),
             "mkdir -p tree/tools/flow/build/flow/release && echo \"%ld $(ps -o lstart= -p %ld | "
             "awk '{ $1 = $1; print }')\" > tree/tools/flow/build/flow/release/pid",
             (long)c, (long)c);
    fx_sh(f, command);
    stop_list(r);
    ok = check(r, f->out && count_text(f->out, text) == 1,
               "stop.sh takes as a release the process that STATE/pid names with its start") &&
         ok;
    snprintf(command, sizeof(command),
             "echo '%ld Thu Jan  1 00:00:00 1970' > tree/tools/flow/build/flow/release/pid",
             (long)c);
    fx_sh(f, command);
    stop_list(r);
    ok = check(r, f->out && !strstr(f->out, text),
               "stop.sh takes as no release a number whose start is not the record's") &&
         ok;
    close(fd[1]);
    if (c > 0) {
        kill(c, SIGKILL);
        waitpid(c, NULL, 0);
    }
    fx_remove(flow_path(f, "tree/tools/flow/build"));
    fx_env("RELEASE_TEST_PS",
           "101 101 testing perl /mnt/d/rach2/build/release/m7.claim.1.pl /h/m7.claim.lock\n"
           "111 101 root /usr/local/libexec/rach2/etude launch build env\n"
           "202 202 testing perl /mnt/d/rach2/build/release/m7.hold.1.pl /h/m7.hold.lock\n"
           "222 202 root /usr/local/libexec/rach2/etude launch alone\n"
           "303 909 testing perl /x/m7.claim.1.pl /h/m7.claim.lock\n"
           "333 303 root qemu-system-x86_64 -x /etude z\n"
           "444 404 root sh /x/tools/flow/release.sh /etude y\n");
    stop_list(r);
    fx_env("RELEASE_TEST_PS", NULL);
    ok = check(r,
               f->out &&
                   strstr(f->out, "stop: left: wsl: 333 root qemu-system-x86_64 -x /etude z\n") &&
                   !strstr(f->out, "wsl: 111 ") && !strstr(f->out, "wsl: 222 ") &&
                   !strstr(f->out, "wsl: 444 ") && !strstr(f->out, "unknown"),
               "stop.sh spares in WSL the sessions of two claims, by their runners as leaders, "
               "and a command line that holds release.sh") &&
         ok;
    return stop_unknown_rows(r) && ok;
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
static int windows_rows(struct release *r)
{
    struct fixture *f = r->f;
    char last[400], id[200], text[4000];
    int ok = windows_pass_row(r);
    ok = windows_failure_rows(r) && ok;
    ok = windows_hold_rows(r) && ok;
    ok = windows_stop_rows(r) && ok;
    ok = claim_rows(r) && ok;
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
 * -p prepares each machine at once and removes what it made, writing no
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
                   called(r, "^wsl rm -rf \"\\$copy\"$") &&
                   !fx_exists(flow_path(f, "state/record")) &&
                   !fx_exists(flow_path(f, "state/prepare/wt")) &&
                   !fx_exists(flow_path(f, "tree/build/machines/mac/run")) &&
                   none_named(flow_path(f, "tree/build/machines/mac/runs"), "prepare"),
               "-p prepares each machine at once and removes what it made");
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
                     !called(r, "^wsl k=/home/testing/rach2/build/flow/claims/" PREPARED
                                "windows-prepare-") &&
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
/** Succeed once `path` holds `text`, waiting up to ten seconds for a run that
 * -a started detached. */
static int waited(const char *path, const char *text)
{
    struct timespec pause = {0, 50000000};
    unsigned i;
    for (i = 0; i < 200; i++) {
        if (fx_has(path, text))
            return 1;
        /* interval: poll the detached run's result every 50 ms */
        nanosleep(&pause, NULL);
    }
    return 0;
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
    return check(r,
                 !f->status && count(record(r), "^windows started " WINDOWS_ID) == 1 &&
                     waited(record(r), "\nwindows done ") &&
                     count(record(r), "^windows done .* run " WINDOWS_ID) == 1 &&
                     !fx_has(record(r), "failure newer") &&
                     waited(flow_path(f, "calls"), " run routine-collect ") &&
                     called(r, " run routine-prepare "),
                 "-a runs Windows' failed run again detached and records its pass");
}
/**
 * install_row() - Check that the release asks for the term console when it
 * updates the Lenovo's installed root.
 * @r: the release under test.
 *
 * Return: 1 when the row passes.
 */
static int install_row(struct release *r)
{
    return check(r,
                 fx_has(r->script, "install_words='--console term'\n") &&
                     fx_has(r->script, "setsid sh -c 'rach2-install $install_words --update "
                                       "/dev/shm/root.ext2 /dev/disk$disk"),
                 "the release requests term when it updates the installed root");
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
