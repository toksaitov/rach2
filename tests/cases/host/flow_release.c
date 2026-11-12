/* flow_release.c: resume records, reruns of failures, the trial boot's command
 * line, acceptance order and the preparation of -p. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define H      "1111111111111111111111111111111111111111"
#define C      "2222222222222222222222222222222222222222"
#define T      "2026-10-10T00:00:00Z"
#define BRANCH "milestone/07-seven"
/* The suites of the mock registry that a machine's run takes, less style. */
#define SUITES "x86_64:agents i686:fs x86_64:fs x86_64:bigmem i686:gnu x86_64:gnu"
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
 * Succeed once `end` appears at or after the claim's `since` byte offset in
 * the call log, so an earlier attempt cannot end this claim.
 */
static int hold_ended(const char *end, size_t since)
{
    char *calls = fx_read(ev("RELEASE_TEST_CALLS"));
    int ended = calls && strlen(calls) >= since && strstr(calls + since, end) != NULL;
    free(calls);
    return ended;
}
/**
 * Succeed when the WSL line `test -e PATH` probes a file that a claim or hold
 * has touched and has not ended, as the calls so far record. Repeated run
 * identifiers must not let a probe use a claim from an earlier attempt.
 * In `probe` mode, reject the first probe after each grant is recorded.
 */
static int touched(const char *probe)
{
    char touch[4200], end[4200], *calls = fx_read(ev("RELEASE_TEST_CALLS"));
    const char *suffix = strrchr(probe, '.'), *last = NULL, *at = calls;
    int made;
    snprintf(touch, sizeof(touch), "touch %.*s ", (int)strcspn(probe + 8, "\n"), probe + 8);
    snprintf(end, sizeof(end), "wsl touch %.*s.%s\n", (int)(suffix - probe - 8), probe + 8,
             !strncmp(suffix, ".claimed", 8) ? "prepared" : "end");
    while (at && (at = strstr(at, touch)) != NULL)
        last = at++;
    made = last && !strstr(last, end);
    if (made && !strcmp(ev("RELEASE_TEST_HOLD"), "probe")) {
        snprintf(touch, sizeof(touch), "wsl %s", probe);
        at = strstr(last, touch);
        made = at && strstr(at + strlen(touch), touch) != NULL;
    }
    free(calls);
    return made;
}
/**
 * Keep a granted claim alive until `end` appears after its starting byte
 * offset `since` in the call log, or until its parent exits.
 */
static int hold_wait(const char *end, size_t since)
{
    pid_t release = getppid();
    if (!*end)
        return 1;
    while (!hold_ended(end, since) && getppid() == release)
        fx_pause();
    return 0;
}
/**
 * Answer a WSL command as RELEASE_TEST_HOLD says: `refused`, WSL's admission
 * refuses the whole host; `claim`, it refuses the preparation's claim; and
 * `wait`, the whole host's hold never reports itself held. Granted claims
 * stay alive in hold_wait until the release ends them or is gone.
 */
static int wsl_answer(int alone, int claim, int held, int claimed)
{
    const char *how = ev("RELEASE_TEST_HOLD");
    if ((alone && !strcmp(how, "refused")) || (claim && !strcmp(how, "claim"))) {
        puts("etude-launch: result: refused: admission: no room in 20400 seconds");
        return 2;
    }
    return (held && (!strcmp(how, "refused") || !strcmp(how, "wait"))) ||
           (claimed && !strcmp(how, "claim"));
}
/**
 * Answer a command for the testing host: a script for Windows' shell or for
 * WSL's, read from stdin and recorded line by line, or any other command,
 * recorded as given.
 */
static int testing_mock(const char *command)
{
    char line[20000], b[4096], end[4200] = "";
    struct stat st;
    size_t since = !stat(ev("RELEASE_TEST_CALLS"), &st) ? (size_t)st.st_size : 0;
    int windows = strstr(command, "bash.exe -l -s") != NULL;
    int prepare = 0, alone = 0, claim = 0, held = 0, claimed = 0, made = 0;
    if (!windows && !strstr(command, "wsl.exe -- bash -s")) {
        snprintf(line, sizeof(line), "ssh testing %s\n", command);
        append(ev("RELEASE_TEST_CALLS"), line);
        return 0;
    }
    while (fgets(b, sizeof(b), stdin)) {
        snprintf(line, sizeof(line), "%s %s", windows ? "windows" : "wsl", b);
        append(ev("RELEASE_TEST_CALLS"), line);
        prepare |= strstr(b, " run routine-prepare ") != NULL;
        alone |= strstr(b, "etude-launch alone ") != NULL;
        claim |= strstr(b, "etude-launch build ") != NULL;
        held |= !strncmp(b, "test -e ", 8) && strstr(b, ".held") != NULL;
        claimed |= !strncmp(b, "test -e ", 8) && strstr(b, ".claimed") != NULL;
        made |= (held || claimed) && !strncmp(b, "test -e ", 8) && touched(b);
        if (!windows && strstr(b, "while [ ! -e ")) {
            const char *path = strstr(b, "while [ ! -e ") + 13;
            snprintf(end, sizeof(end), "wsl touch %.*s\n", (int)strcspn(path, " "), path);
        }
    }
    if (!windows) {
        int status = (held || claimed) && !made ? 1 : wsl_answer(alone, claim, held, claimed);
        return !status && (alone || claim) ? hold_wait(end, since) : status;
    }
    puts(prepare && !*ev("RELEASE_TEST_PREPARE") ? "status 0"
         : prepare                               ? ev("RELEASE_TEST_PREPARE")
                                                 : ev("RELEASE_TEST_WINDOWS"));
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
        puts("bootctl: fallback: 4194304 bytes written from the committed candidate and read back");
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
    if (!strcmp(name, "sleep")) {
        call("sleep ", argc, argv, 1);
        if (argc > 1 && !strcmp(argv[1], "30") && !strcmp(ev("RELEASE_TEST_HOLD"), "wait") &&
            fx_has(ev("RELEASE_TEST_CALLS"), "etude-launch alone "))
            kill(getppid(), SIGTERM);
        return 0;
    }
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
        *cmdline, *open;
    const char *again; /* the machine of -a, or NULL for a release */
    int rerun, preparing, only;
};

static int setup(struct release *r, struct fixture *f)
{
    char path[4096];
    unsigned i;
    const char *mock[] = {"sh", "gh", "ssh", "git", "make", "sleep", "scp"};
    memset(r, 0, sizeof(*r));
    r->f = f;
    snprintf(r->tree, sizeof(r->tree), "%s/tree", f->work);
    snprintf(r->script, sizeof(r->script), "%s/tools/flow/release.sh", r->tree);
    if (!fx_copy(flow_source(f, "tools/flow/release.sh"), r->script))
        return 0;
    snprintf(path, sizeof(path), "%s/tools/ci/site.sh", r->tree);
    if (!fx_copy(flow_source(f, "tools/ci/site.sh"), path))
        return 0;
    flow_put(f, "tree/krffs/rach2_demo_ed25519", "");
    flow_put(f, "tree/tools/flow/board.sh", "# native board fixture\n");
    flow_put(f, "tree/tools/flow/sweep.sh", "# native sweep fixture\n");
    for (i = 0; i < 7; i++)
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
    const char *files[] = {"calls", "calls.release"};
    const char *a[10] = {"/bin/sh", r->script, "-s", flow_path(f, "state")};
    unsigned i, n = 4;
    fx_remove(flow_path(f, "state"));
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        fx_remove(flow_path(f, files[i]));
    flow_put(f, "state/wt/krffs/rach2_demo_ed25519", "");
    flow_put(f, "state/wt/build/x86_64/images/release.sha256", "");
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
    run(r, &(struct release_row){.record = r->head, .git = "yes"});
    snprintf(expected, sizeof(expected),
             "^testing %s/rach2/build/worktrees/release run \\{ cp /etc/rach2/doom1.wad "
             "ports/doom/doom1.wad \\|\\|$",
             FLOW_OWNER_HOME);
    return check(r,
                 f->status == 75 && !fx_has(record(r), "build done ") && called(r, expected) &&
                     called(r, "^ +sh ports/doom/fetch_wad.sh && ARCH=x86_64 make clean && "
                               "ARCH=x86_64 make machine GNU_AGENTS=all && "),
                 "machine build takes the host's WAD and starts clean");
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
#define WINDOWS_ID   "milestone7-111111111111-windows-"
#define WINDOWS_HELD "/mnt/d/o/" WINDOWS_ID
/**
 * Succeed when Windows' preparation ran under its own claim in WSL, which
 * lasts while the release beats and ends once the preparation does.
 */
static int windows_prepared(struct release *r)
{
    return before(r,
                  "\nwsl cd /tmp && JOBS=30 exec /usr/local/libexec/rach2/etude-launch "
                  "build env sh -c 'touch " WINDOWS_HELD,
                  "\nwindows cd D:/w || ") &&
           called(r, "^wsl +while \\[ ! -e " WINDOWS_HELD
                     "[0-9-]+\\.prepared ] && find " WINDOWS_HELD "[0-9-]+\\.alive -mmin -10 ") &&
           before(r, "\nwsl test -e " WINDOWS_HELD, "\nwindows cd D:/w || ") &&
           called(r, "^windows .* run routine-prepare phase_first_run D:/w "
                     "D:/o/" WINDOWS_ID "[0-9-]+ both " WINDOWS_ID "[0-9-]+ C:/s .* whpx " SUITES
                     "  > ") &&
           before(r, " run routine-prepare ", ".prepared\n") &&
           called(r, "^wsl touch " WINDOWS_HELD "[0-9-]+\\.prepared$") &&
           called(r, "^windows git fetch -q D:/o/release.bundle 'refs/remotes/origin/" BRANCH
                     "' && git checkout -q -f --detach " H " ") &&
           called(r, "^windows export JOBS='30'$");
}
/**
 * Succeed when Windows' cases ran while WSL's whole host was held, a hold that
 * lasts while the release beats and ends once the cases do.
 */
static int windows_collected(struct release *r)
{
    return before(r, ".prepared\n",
                  "\nwsl exec /usr/local/libexec/rach2/etude-launch alone sh -c ") &&
           called(r, "^wsl +while \\[ ! -e " WINDOWS_HELD "[0-9-]+\\.end ] && find " WINDOWS_HELD
                     "[0-9-]+\\.alive -mmin -10 ") &&
           before(r, "\nwsl exec /usr/local/libexec/rach2/etude-launch alone sh -c ",
                  " run routine-collect ") &&
           called(r, "^windows .* run routine-collect phase_first_run D:/w "
                     "D:/o/" WINDOWS_ID "[0-9-]+ both " WINDOWS_ID "[0-9-]+ C:/s .* whpx " SUITES
                     "  > ") &&
           called(r, "^wsl touch " WINDOWS_HELD "[0-9-]+\\.end$") &&
           !called(r, "PREPARATION_CLEAN=1");
}
static int windows_pass_row(struct release *r)
{
    struct fixture *f = r->f;
    run(r, &(struct release_row){
               .record = r->head, .git = "yes", .machines = WINDOWS, .windows = "status 0"});
    return check(r,
                 f->status == 75 && windows_prepared(r) && windows_collected(r) &&
                     called(r, "^git -C .* bundle create .*/state/windows.bundle "
                               "refs/remotes/origin/" BRANCH "$") &&
                     called(r, "^scp -q -o BatchMode=yes .*/state/windows.bundle "
                               "testing:D:/o/release.bundle$") &&
                     count(record(r), "^windows done .* run " WINDOWS_ID) == 1 &&
                     called(r, "^testing "),
                 "windows prepares under its own claim, then runs its cases while WSL is held");
}
/** A failed run, a run without a status and a failed preparation. */
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
                                 .prepare = "etude prepare: routine preparation failed\nstatus 2",
                                 .windows = "status 0"});
    machine_line(r, "windows started ", id, sizeof(id));
    snprintf(last, sizeof(last), "failed tests: windows run %s: runner", id);
    ok = check(r,
               f->status == 1 && last_is(record(r), last) &&
                   called(r, "^wsl touch " WINDOWS_HELD "[0-9-]+\\.prepared$") &&
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
                   before(r, ".prepared\n",
                          "\nwsl exec /usr/local/libexec/rach2/etude-launch alone ") &&
                   called(r, "^wsl touch " WINDOWS_HELD "[0-9-]+\\.end$") &&
                   !called(r, " run routine-collect "),
               "a whole host that WSL refuses ends the release with nothing left on Windows") &&
         ok;
    run(r, &(struct release_row){
               .record = r->head, .git = "yes", .machines = WINDOWS, .hold = "claim"});
    ok = check(r,
               f->status == 1 &&
                   fx_has(record(r), "\nfailed windows: WSL's admission did not give the "
                                     "preparation's claim: etude-launch: result: refused: ") &&
                   !called(r, "^windows ") && !called(r, "etude-launch alone"),
               "a preparation claim that WSL refuses ends the release before Windows starts") &&
         ok;
    run(r, &(struct release_row){
               .record = r->head, .git = "yes", .machines = WINDOWS, .hold = "wait"});
    ok = check(r,
               f->status != 0 && f->status != 1 && f->status != 75 &&
                   called(r, "^wsl touch " WINDOWS_HELD "[0-9-]+\\.prepared$") &&
                   called(r, "^wsl touch " WINDOWS_HELD "[0-9-]+\\.end$") &&
                   !called(r, " run routine-collect ") && !fx_has(record(r), "\nfailed ") &&
                   !fx_has(record(r), "\nwindows done "),
               "a release stopped while it waits for the whole host ends its hold") &&
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
                             "milestone7-111111111111-lenovo-[0-9-]+ .* --hostpeer "
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
                             "D:/o/" PREPARED "windows-prepare-[0-9-]+ both ") &&
                   called(r, "^etude .* run routine-prepare phase_first_run .*/mac/tree "
                             ".*/mac/runs/" PREPARED "mac-prepare-[0-9-]+ both ") &&
                   !called(r, "routine-(sequence|collect)|^dispatch|^machine |^gh ") &&
                   called(r, "^windows rm -rf " PREPARED "windows-prepare-") &&
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
int flow_release(struct fixture *f)
{
    struct release r;
    int ok = 1;
    if (!setup(&r, f))
        return 0;
    ok = check(&r,
               fx_has(r.script, "install_words='--console term'") &&
                   fx_has(r.script, "grep -q -e \"\\[--i915\\]\" /bin/rach2-install'; then\n"
                                    "            install_words=\"$install_words --i915\"") &&
                   fx_has(r.script, "setsid sh -c 'rach2-install $install_words --update "
                                    "/dev/shm/root.ext2 /dev/disk$disk"),
               "the release requests term, and the Intel driver from an installer that has "
               "--i915, when it updates the installed root") &&
         ok;
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
    ok = acceptance_rows(&r) && ok;
    ok = windows_rows(&r) && ok;
    ok = lenovo_off_row(&r) && ok;
    ok = lenovo_rows(&r) && ok;
    ok = mac_rows(&r) && ok;
    ok = prepare_rows(&r) && ok;
    ok = only_rows(&r) && ok;
    ok = restart_refusal_rows(&r) && ok;
    ok = restart_mac_rows(&r) && ok;
    ok = restart_windows_row(&r) && ok;
    fx_env("FLOW_RELEASE", NULL);
    return ok;
}
