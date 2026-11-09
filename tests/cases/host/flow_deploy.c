/* flow_deploy.c: deployment keeps clones, credentials and active ledgers. */
#define _DARWIN_C_SOURCE 1
#define _DEFAULT_SOURCE  1
#define _POSIX_C_SOURCE  200809L
#include "flow_check.h"
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
static const char *ev(const char *n)
{
    const char *v = getenv(n);
    return v ? v : "";
}
static const char *dp(const char *name)
{
    static char b[32][4096];
    static unsigned i;
    char *v = b[i++ % 32];
    snprintf(v, 4096, "%s/%s", ev("FIX"), name);
    return v;
}
static void append(const char *path, const char *s)
{
    FILE *f = fopen(path, "ab");
    if (f) {
        fputs(s, f);
        fclose(f);
    }
}
static void args_text(char *v, size_t size, int argc, char **argv)
{
    int i;
    v[0] = 0;
    for (i = 1; i < argc; i++) {
        if (i > 1)
            strncat(v, " ", size - strlen(v) - 1);
        strncat(v, argv[i], size - strlen(v) - 1);
    }
    strncat(v, "\n", size - strlen(v) - 1);
}
static unsigned lines(const char *path)
{
    char *s = fx_read(path), *p;
    unsigned n = 0;
    for (p = s; *p; p++)
        if (*p == '\n')
            n++;
    free(s);
    return n;
}
static int powershell(const char *encoded)
{
    char decoded[10000], script[5000], *line, *end;
    unsigned bits = 0, value = 0, n = 0, i, k = 0;
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (i = 0; encoded[i] && encoded[i] != '='; i++) {
        const char *p = strchr(alphabet, encoded[i]);
        if (!p)
            return 2;
        value = (value << 6) | (unsigned)(p - alphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n == sizeof(decoded))
                return 2;
            decoded[n++] = (char)(value >> bits);
        }
    }
    for (i = 0; i + 1 < n; i += 2) {
        if (decoded[i + 1] || k + 1 == sizeof(script))
            return 2;
        script[k++] = decoded[i];
    }
    script[k] = 0;
    line = strchr(script, '\n');
    if (!line)
        return 2;
    line++;
    end = strchr(line, '\n');
    if (end)
        *end = 0;
    if (strstr(line, "IsInRole"))
        return fx_exists(dp("elevated")) ? 0 : 1;
    if (!strncmp(line, "if (Have ", 9)) {
        char name[500];
        snprintf(name, sizeof(name), "have/%s", line + 9);
        end = strchr(name, ')');
        if (end)
            *end = 0;
        return fx_exists(dp(name)) ? 0 : 1;
    }
    if (!strncmp(line, "Get ", 4)) {
        char name[500], log[1000];
        snprintf(name, sizeof(name), "have/%s", line + 4);
        snprintf(log, sizeof(log), "pwsh %s\n", line);
        append(dp("log"), log);
        return fx_text(dp(name), "") ? 0 : 1;
    }
    fprintf(stderr, "powershell: unknown line: %s\n", line);
    return 2;
}
static int ssh_mock(int argc, char **argv)
{
    char command[20000], log[20000], path[24000], *drop;
    unsigned i;
    args_text(log, sizeof(log), argc, argv);
    append(dp("ssh.log"), log);
    if (argc < 7 || strcmp(argv[1], "-T") || strcmp(argv[2], "-o") ||
        strcmp(argv[3], "BatchMode=yes") || strcmp(argv[4], "--"))
        return 255;
    drop = fx_read(dp("drop"));
    if (*drop && (unsigned)atoi(drop) == lines(dp("ssh.log"))) {
        free(drop);
        fputs("ssh: connection lost\n", stderr);
        return 255;
    }
    free(drop);
    command[0] = 0;
    for (i = 6; i < (unsigned)argc; i++) {
        if (i > 6)
            strncat(command, " ", sizeof(command) - strlen(command) - 1);
        strncat(command, argv[i], sizeof(command) - strlen(command) - 1);
    }
    if (!strncmp(command, "powershell -NoProfile -NonInteractive -EncodedCommand ",
                 strlen("powershell -NoProfile -NonInteractive -EncodedCommand ")))
        return powershell(strrchr(command, ' ') + 1);
    if (!strcmp(command, "C:/msys64/usr/bin/env.exe PATH=/usr/bin:/ucrt64/bin sh -s")) {
        strcpy(command, "sh -s");
        snprintf(path, sizeof(path), "%s/windows:%s/mbin:%s", ev("FLOW_WORK"), ev("FLOW_WORK"),
                 ev("PATH"));
        fx_env("LOCALAPPDATA", "C:\\Users\\fix\\AppData\\Local");
        fx_env("ProgramFiles", "C:\\Program Files");
        fx_env("FLOW_DEPLOY_WINDOWS", "1");
    } else {
        snprintf(path, sizeof(path), "%s/mbin:%s", ev("FLOW_WORK"), ev("PATH"));
        fx_env("FLOW_DEPLOY_WINDOWS", NULL);
    }
    fx_env("HOME", dp("home"));
    fx_env("TMPDIR", dp("tmp"));
    fx_env("XDG_STATE_HOME", "");
    fx_env("PATH", path);
    setpgid(0, 0);
    {
        char *a[] = {"/bin/sh", "-c", command, NULL};
        execv(a[0], a);
    }
    return 127;
}
static int uname_mock(int argc, char **argv)
{
    if (!strcmp(ev("FLOW_DEPLOY_WINDOWS"), "1")) {
        if (argc != 2)
            return 1;
        if (!strcmp(argv[1], "-s"))
            puts("MINGW64_NT-10.0-26100");
        else if (!strcmp(argv[1], "-m"))
            puts("x86_64");
        else if (!strcmp(argv[1], "-r"))
            puts("3.6.1-1.x86_64");
        else if (!strcmp(argv[1], "-n"))
            puts("winbox");
        else
            return 1;
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "-s")) {
        puts("Linux");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "-r")) {
        puts("6.0.0-fixture");
        return 0;
    }
    argv[0] = "/usr/bin/uname";
    execv(argv[0], argv);
    return 127;
}
static int cygpath_mock(int argc, char **argv)
{
    if (argc != 3)
        return 1;
    if (!strcmp(argv[1], "-u") && !strcmp(argv[2], "C:\\Users\\fix\\AppData\\Local"))
        puts(dp("appdata"));
    else if (!strcmp(argv[1], "-u") && !strcmp(argv[2], "C:\\Program Files"))
        puts(dp("programfiles"));
    else if (!strcmp(argv[1], "-m") &&
             strstr(argv[2], "/programfiles/qemu/share/edk2-x86_64-code.fd"))
        puts("C:/Program Files/qemu/share/edk2-x86_64-code.fd");
    else if (!strcmp(argv[1], "-m") && argv[2][0] == '/')
        puts(argv[2]);
    else
        return 1;
    return 0;
}
static int id_mock(int argc, char **argv)
{
    if (argc != 2)
        return 1;
    if (!strcmp(argv[1], "-u"))
        puts("1000");
    else if (!strcmp(argv[1], "-un"))
        puts("fixuser");
    else
        return 1;
    return 0;
}
int flow_deploy_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    char text[16000];
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "ssh"))
        return ssh_mock(argc, argv);
    if (!strcmp(name, "fakecc"))
        return flow_version_mock(argc, argv);
    if (!strcmp(name, "uname"))
        return uname_mock(argc, argv);
    if (!strcmp(name, "cygpath"))
        return cygpath_mock(argc, argv);
    if (!strcmp(name, "id"))
        return id_mock(argc, argv);
    if (!strcmp(name, "sudo")) {
        if (argc < 2 || strcmp(argv[1], "-n") || !fx_exists(dp("sudo"))) {
            fputs("sudo: a password is required\n", stderr);
            return 1;
        }
        if (argc == 3 && !strcmp(argv[2], "true"))
            return 0;
        args_text(text, sizeof(text), argc - 1, argv + 1);
        {
            char log[18000];
            snprintf(log, sizeof(log), "sudo %s", text);
            append(dp("log"), log);
        }
        execvp(argv[2], argv + 2);
        return 127;
    }
    if (!strcmp(name, "qemu-system-x86_64") || !strcmp(name, "qemu-system-x86_64.exe")) {
        if (argc != 2 || strcmp(argv[1], "--version"))
            return 1;
        if (strstr(name, ".exe"))
            printf("QEMU emulator version 9.9.8 (windows fixture)\r\n");
        else
            puts("QEMU emulator version 9.9.9 (fixture)\nCopyright (c) 2003-2026 Fabrice Bellard "
                 "and the QEMU Project developers");
        return 0;
    }
    if (!strcmp(name, "mv")) {
        const char *last = argc > 1 ? strrchr(argv[argc - 1], '/') : NULL;
        last = last ? last + 1 : argc > 1 ? argv[argc - 1] : "";
        if (fx_exists(dp("interrupt-record")) && !strcmp(last, "machine")) {
            unlink(dp("interrupt-record"));
            kill(0, SIGTERM);
            return 1;
        }
        argv[0] = "/bin/mv";
        execv(argv[0], argv);
        return 127;
    }
    return 97;
}
struct deploy {
    struct fixture *f;
    char tree[4096], fix[4096], script[4096], ovmf[4096], key[100], dir[4096], before[65];
    long cpus;
    unsigned long long memory;
    char host[256], arch[100];
};
static int git(struct deploy *d, const char *cwd, const char *const args[])
{
    const char *a[40] = {"/usr/bin/git",
                         "-c",
                         "user.name=test",
                         "-c",
                         "user.email=test@example.invalid",
                         "-c",
                         "commit.gpgsign=false",
                         "-c",
                         "maintenance.auto=false",
                         "-c",
                         "gc.auto=0"};
    unsigned n = 11, i;
    for (i = 0; args[i] && n < 39; i++)
        a[n++] = args[i];
    a[n] = NULL;
    return !fx_run(d->f, a, cwd, NULL, NULL);
}
static int setup(struct deploy *d, struct fixture *f)
{
    char target[4096], source[4096];
    const char *copy[] = {"prereq.sh", "e2fsprogs_path.sh", "ovmf_path.sh", "elfwrap.sh"};
    const char *files[] = {"tests/cases/host/host_script_inventory.c",
                           "tests/cases/host/host_script_inventory.h", "utils/sha256.c",
                           "utils/sha256.h"};
    unsigned i;
    struct utsname u;
    memset(d, 0, sizeof(*d));
    d->f = f;
    snprintf(d->tree, sizeof(d->tree), "%s/tree", f->work);
    snprintf(d->fix, sizeof(d->fix), "%s/fix", f->work);
    snprintf(d->script, sizeof(d->script), "%s/tools/etude/provision_machine.sh", d->tree);
    snprintf(target, sizeof(target), "%s/tools/etude", d->tree);
    if (!fx_tree(flow_source(f, "tools/etude"), target))
        return 0;
    for (i = 0; i < 4; i++) {
        snprintf(source, sizeof(source), "tools/setup/%s", copy[i]);
        snprintf(target, sizeof(target), "%s/tools/setup/%s", d->tree, copy[i]);
        if (!fx_copy(flow_source(f, source), target))
            return 0;
    }
    for (i = 0; i < sizeof(files) / sizeof(*files); i++) {
        snprintf(target, sizeof(target), "%s/%s", d->tree, files[i]);
        if (!fx_copy(flow_source(f, files[i]), target))
            return 0;
    }
    snprintf(target, sizeof(target), "%s/tools/etude/ledger.c", d->tree);
    if (!flow_version_mock_source(d->tree) ||
        !fx_copy(flow_source(f, "tools/etude/ledger.c"), target))
        return 0;
    fx_env("TREE", d->tree);
    fx_env("FIX", d->fix);
    fx_env("FLOW_DEPLOY", "1");
    fx_env("FLOW_DEPLOY_WINDOWS", NULL);
    fx_mkdir(flow_path(f, "ctl"));
    fx_mkdir(flow_path(f, "mbin"));
    fx_mkdir(flow_path(f, "windows"));
    if (!fx_mock(f, "ssh") || !fx_mock(f, "fakecc"))
        return 0;
    {
        const char *m[] = {"id", "sudo", "qemu-system-x86_64", "mv", "uname"};
        for (i = 0; i < 5; i++) {
            snprintf(target, sizeof(target), "%s/mbin/%s", f->work, m[i]);
            if (!fx_copy(f->self, target))
                return 0;
        }
    }
    {
        const char *m[] = {"uname", "cygpath"};
        for (i = 0; i < 2; i++) {
            snprintf(target, sizeof(target), "%s/windows/%s", f->work, m[i]);
            if (!fx_copy(f->self, target))
                return 0;
        }
    }
    {
        FILE *recipe;
        snprintf(target, sizeof(target), "%s/tools/setup/prereqs", d->tree);
        recipe = fopen(target, "wb");
        if (!recipe)
            return 0;
        for (i = 0; i < 3; i++)
            fprintf(recipe,
                    "platform %s\n\nneed user-tool\nprobe [ -e \"$FIX/have/user-tool\" ]\nuser "
                    "touch \"$FIX/have/user-tool\"\n\nneed admin-tool\nprobe [ -e "
                    "\"$FIX/have/admin-tool\" ]\nadmin touch \"$FIX/have/admin-tool\"\n\nneed "
                    "scratch\nprobe [ -f tools/setup/prereq.sh ] && : > probed\nuser true\n\n",
                    i == 0   ? "linux"
                    : i == 1 ? "macos"
                             : "rach2");
        fputs(
            "platform windows\n\nneed user-tool\nprobe Have user-tool\nuser Get user-tool\n\nneed "
            "whpx\nprobe Have whpx\nmanual # enable the Windows Hypervisor Platform\n",
            recipe);
        fclose(recipe);
    }
    flow_put(f, "home-template/clone/file", "committed\n");
    snprintf(source, sizeof(source), "%s/home-template/clone", f->work);
    {
        const char *a[] = {"init", "-q", NULL}, *b[] = {"add", "file", NULL},
                   *c[] = {"commit", "-qm", "clone", NULL};
        if (!git(d, source, a) || !git(d, source, b) || !git(d, source, c))
            return 0;
    }
    flow_put(f, "home-template/clone/new", "untracked\n");
    flow_put(f, "home-template/.ssh/authorized_keys", "ssh-ed25519 AAAAfixture fix@machine\n");
    flow_put(f, "home-template/.ssh/id_ed25519", "fixture private key\n");
    flow_put(f, "home-template/.ssh/known_hosts", "machine ssh-ed25519 AAAAfixture\n");
    d->cpus = sysconf(_SC_NPROCESSORS_ONLN);
    {
        char *info = fx_read("/proc/meminfo"), *at = strstr(info, "MemTotal:");
        d->memory = at ? strtoull(at + 9, NULL, 10) * 1024 : 0;
        free(info);
    }
    if (!d->memory || uname(&u))
        return 0;
    snprintf(d->host, sizeof(d->host), "%s", u.nodename);
    snprintf(d->arch, sizeof(d->arch), "%s", u.machine);
    return 1;
}
static int state(struct deploy *d)
{
    const char *paths[] = {dp("home/clone"), dp("home/.ssh"), NULL};
    fx_remove(d->fix);
    fx_mkdir(dp("have"));
    fx_mkdir(dp("tmp"));
    if (!fx_tree(flow_path(d->f, "home-template"), dp("home")))
        return 0;
    fx_text(dp("have/admin-tool"), "");
    fx_text(dp("have/user-tool"), "");
    fx_text(dp("cc.log"), "");
    fx_text(dp("ssh.log"), "");
    fx_text(dp("log"), "");
    fx_text(dp("firmware code.fd"), "");
    snprintf(d->ovmf, sizeof(d->ovmf), "%s", dp("firmware code.fd"));
    return flow_snapshot(paths, d->before);
}
static int run(struct deploy *d, const char *const extra[])
{
    char path[18000];
    const char *a[40] = {"/bin/sh", d->script, "-c", "fakecc", "-C", dp("cache")};
    unsigned n = 6, i;
    int result;
    for (i = 0; extra[i] && n < 39; i++)
        a[n++] = extra[i];
    a[n] = NULL;
    snprintf(path, sizeof(path), "%s:%s/mbin:/usr/bin:/bin:/usr/sbin:/sbin", d->f->bin, d->f->work);
    fx_env("PATH", path);
    fx_env("TMPDIR", flow_path(d->f, "ctl"));
    fx_env("RACH2_OVMF_CODE", d->ovmf);
    result = !fx_run(d->f, a, d->tree, NULL, NULL);
    return result && fx_empty(flow_path(d->f, "ctl")) && fx_empty(dp("tmp"));
}
static int machine(struct deploy *d)
{
    const char *a[] = {"machine", NULL};
    return run(d, a);
}
static int deployed(struct deploy *d)
{
    char *at = d->f->out, *last = NULL, wanted[300];
    while ((at = strstr(at, "etude provision: result: "))) {
        char how[50], key[100], dir[4096];
        if (sscanf(at, "etude provision: result: %49s %99s %4095s", how, key, dir) == 3 &&
            (!strcmp(how, "built") || !strcmp(how, "reused"))) {
            snprintf(d->key, sizeof(d->key), "%s", key);
            snprintf(d->dir, sizeof(d->dir), "%s", dir);
        }
        last = at++;
    }
    snprintf(wanted, sizeof(wanted), "etude provision: result: deployed %s\n", d->key);
    return !d->f->status && last && !strcmp(last, wanted) && fx_empty(flow_path(d->f, "ctl")) &&
           fx_empty(dp("tmp"));
}
static int incomplete(struct deploy *d, const char *why)
{
    const char *at = d->f->out, *last = NULL;
    while ((at = strstr(at, "etude provision: result: incomplete: "))) {
        last = at++;
    }
    return d->f->status == 1 && last && (!why || strstr(last, why)) &&
           fx_empty(flow_path(d->f, "ctl")) && fx_empty(dp("tmp"));
}
static int untouched(struct deploy *d)
{
    const char *paths[] = {dp("home/clone"), dp("home/.ssh"), NULL};
    char after[65];
    return flow_snapshot(paths, after) && !strcmp(d->before, after);
}
static int ledger(const char *root)
{
    char lock[4096], verdicts[4096];
    snprintf(lock, sizeof(lock), "%s/lock", root);
    snprintf(verdicts, sizeof(verdicts), "%s/verdicts", root);
    return fx_count(root) == 2 && fx_equal(lock, "") && fx_empty(verdicts);
}
static int record(struct deploy *d, const char *file, const char *ledger_path, int windows,
                  const char *ovmf, const char *accels)
{
    char wanted[16000];
    snprintf(wanted, sizeof(wanted),
             "etude-machine 1\nhost %s\nplatform %s %s\ncpus %ld\nmemory_bytes %llu\nqemu QEMU "
             "emulator version %s\novmf_x86_64 %s\naccelerators %s\nadmission etude-ledger "
             "1\nledger %s\nversion %s %s\n",
             windows ? "winbox" : d->host, windows ? "windows" : "linux", d->arch, d->cpus,
             d->memory, windows ? "9.9.8 (windows fixture)" : "9.9.9 (fixture)",
             ovmf ? ovmf : dp("firmware code.fd"),
             accels    ? accels
             : windows ? "whpx tcg"
                       : "tcg",
             ledger_path, d->key, d->dir);
    return fx_equal(file, wanted);
}
static int home_record(struct deploy *d)
{
    return record(d, dp("home/.local/state/etude/machine"), dp("home/.local/state/etude/ledger"), 0,
                  NULL, NULL);
}
static int basic_rows(struct deploy *d)
{
    struct fixture *f = d->f;
    char before[65], after[65], path[4096];
    int ok = 1;
    if (!state(d))
        return 0;
    unlink(dp("have/user-tool"));
    machine(d);
    ok = fx_check(f,
                  deployed(d) && fx_exists(dp("have/user-tool")) &&
                      strstr(f->out, "prereq: result: pass: 3 tools present on ") &&
                      lines(dp("ssh.log")) == 3 && home_record(d) &&
                      ledger(dp("home/.local/state/etude/ledger")) &&
                      fx_count(dp("home/.local/state/etude")) == 2 && untouched(d),
                  "deploy and private state") &&
         ok;
    {
        char *calls = fx_read(dp("ssh.log"));
        const char *p = calls, *wanted = "-T -o BatchMode=yes -- machine sh -s\n";
        while (*p) {
            if (strncmp(p, wanted, strlen(wanted)))
                ok = 0;
            p = strchr(p, '\n');
            if (!p)
                break;
            p++;
        }
        free(calls);
    }
    snprintf(path, sizeof(path), "%s/cache/builds/%s.", d->fix, d->key);
    ok = fx_check(f, !strncmp(d->dir, path, strlen(path)) && strlen(d->dir + strlen(path)) == 6,
                  "version directory contract") &&
         ok;
    if (!state(d) || !machine(d) || !deployed(d))
        return 0;
    {
        const char *paths[] = {dp("home"), dp("cache"), dp("have"), NULL};
        if (!flow_snapshot(paths, before))
            return 0;
        machine(d);
        if (!flow_snapshot(paths, after))
            return 0;
    }
    ok = fx_check(f,
                  deployed(d) && strstr(f->out, "etude provision: result: reused ") &&
                      strstr(f->out, "etude provision: machine: unchanged ") &&
                      lines(dp("cc.log")) == 1 && !strcmp(before, after),
                  "repeat deployment changes nothing") &&
         ok;
    return ok;
}
static int prerequisite_rows(struct deploy *d)
{
    struct fixture *f = d->f;
    int ok = 1;
    if (!state(d))
        return 0;
    unlink(dp("have/admin-tool"));
    machine(d);
    ok = fx_check(f,
                  incomplete(d, "is deployed on machine, and a prerequisite is missing") &&
                      strstr(f->out, "prereq: an administrator runs:\n") &&
                      strstr(f->out, "    sudo touch \"$FIX/have/admin-tool\"\n") &&
                      !fx_exists(dp("have/admin-tool")),
                  "administrator instructions") &&
         ok;
    fx_text(dp("have/admin-tool"), "");
    machine(d);
    ok = fx_check(f, deployed(d) && home_record(d) && untouched(d),
                  "administrator installed prerequisite") &&
         ok;
    if (!state(d))
        return 0;
    unlink(dp("have/admin-tool"));
    fx_text(dp("sudo"), "");
    machine(d);
    ok = fx_check(f,
                  deployed(d) && fx_exists(dp("have/admin-tool")) &&
                      fx_has(dp("log"), "sudo sh -c touch \"$FIX/have/admin-tool\"\n"),
                  "sudo prerequisite") &&
         ok;
    if (!state(d))
        return 0;
    unlink(dp("have/user-tool"));
    {
        const char *a[] = {"-n", "machine", NULL};
        run(d, a);
    }
    ok = fx_check(f,
                  incomplete(d, "is deployed on machine, and a prerequisite is missing") &&
                      !fx_exists(dp("have/user-tool")) &&
                      strstr(f->out, "prereq: this account runs:\n") &&
                      strstr(f->out, "    touch \"$FIX/have/user-tool\"\n"),
                  "check mode installs nothing") &&
         ok;
    machine(d);
    return fx_check(f, deployed(d), "deployment after check") && ok;
}
static char *change(struct deploy *d)
{
    char path[4096], *saved;
    snprintf(path, sizeof(path), "%s/tools/etude/json.c", d->tree);
    saved = fx_read(path);
    append(path, "/* A change. */\n");
    return saved;
}
static void restore(struct deploy *d, char *text)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/tools/etude/json.c", d->tree);
    fx_text(path, text);
    free(text);
}
static int busy_row(struct deploy *d)
{
    struct fixture *f = d->f;
    struct fx_child child;
    char oldkey[100], olddir[4096], version[4096], before[65], after[65], *saved;
    int ok;
    const char *a[] = {f->self, "--hold", NULL};
    if (!state(d) || !machine(d) || !deployed(d))
        return 0;
    snprintf(oldkey, sizeof(oldkey), "%s", d->key);
    snprintf(olddir, sizeof(olddir), "%s", d->dir);
    fx_text(dp("home/.local/state/etude/ledger/claims"),
            "etude-ledger 1\nclaim run-1.case-1.1\nsystem linux\nstate running\nend\n");
    fx_text(dp("home/.local/state/etude/ledger/verdicts/run-1.case-1.1"), "confirmed\n");
    snprintf(version, sizeof(version), "%s/cache/versions/%s", d->fix, oldkey);
    {
        const char *paths[] = {dp("home/.local/state/etude/ledger"), version, olddir, NULL};
        if (!flow_snapshot(paths, before))
            return 0;
    }
    if (!fx_start(f, &child, a, f->work, NULL, NULL))
        return 0;
    saved = change(d);
    machine(d);
    restore(d, saved);
    {
        const char *paths[] = {dp("home/.local/state/etude/ledger"), version, olddir, NULL};
        if (!flow_snapshot(paths, after))
            return 0;
    }
    ok = fx_check(f,
                  deployed(d) && strcmp(d->key, oldkey) &&
                      strstr(f->out, "etude provision: result: built ") && home_record(d) &&
                      !strcmp(before, after) && !fx_poll(&child) && untouched(d),
                  "active test ledger, process and old version unchanged");
    fx_stop(&child, SIGTERM);
    fx_wait(f, &child);
    return ok;
}
static int format_rights_rows(struct deploy *d)
{
    struct fixture *f = d->f;
    char before[65], after[65], *saved;
    int ok = 1;
    if (!state(d) || !machine(d) || !deployed(d))
        return 0;
    fx_text(dp("home/.local/state/etude/ledger/claims"), "etude-ledger 2\n");
    {
        const char *paths[] = {dp("home/.local/state/etude"), NULL};
        if (!flow_snapshot(paths, before))
            return 0;
        saved = change(d);
        machine(d);
        restore(d, saved);
        if (!flow_snapshot(paths, after))
            return 0;
    }
    ok = fx_check(f,
                  incomplete(d, "the record of machine was not written: status 1") &&
                      strstr(f->err, "is of the format 'etude-ledger 2'") &&
                      strstr(f->err, "admits by 'etude-ledger 1'; the machine keeps its version") &&
                      !strcmp(before, after),
                  "ledger format preserves machine state") &&
         ok;
    if (!state(d))
        return 0;
    fx_mkdir(dp("shared"));
    chmod(dp("shared"), 0555);
    {
        const char *a[] = {"-L", dp("shared"), "machine", NULL};
        run(d, a);
    }
    ok = fx_check(f,
                  incomplete(d, "the record of machine was not written: status 1") &&
                      strstr(f->err, "this account cannot use the ledger ") &&
                      strstr(f->err, "chmod a+rwx '") && fx_empty(dp("shared")) &&
                      !fx_exists(dp("home/.local/state/etude/machine")),
                  "ledger rights refusal") &&
         ok;
    chmod(dp("shared"), 0755);
    {
        const char *a[] = {"-L", dp("shared"), "machine", NULL};
        run(d, a);
    }
    return fx_check(
               f,
               deployed(d) &&
                   record(d, dp("home/.local/state/etude/machine"), dp("shared"), 0, NULL, NULL) &&
                   ledger(dp("shared")),
               "ledger rights repair") &&
           ok;
}
static int interrupted_rows(struct deploy *d)
{
    struct fixture *f = d->f;
    unsigned i;
    int ok = 1;
    char number[10], oldkey[100], olddir[4096], *saved;
    for (i = 1; i <= 3; i++) {
        if (!state(d))
            return 0;
        snprintf(number, sizeof(number), "%u\n", i);
        fx_text(dp("drop"), number);
        machine(d);
        unlink(dp("drop"));
        ok = fx_check(f,
                      incomplete(d, i == 2 ? "etude was not built on machine" : NULL) &&
                          !strstr(f->out, "result: deployed") &&
                          !fx_exists(dp("home/.local/state/etude/machine")),
                      "lost connection publishes no machine") &&
             ok;
        machine(d);
        ok = fx_check(f,
                      deployed(d) && home_record(d) &&
                          ledger(dp("home/.local/state/etude/ledger")) && untouched(d),
                      "resume after lost connection") &&
             ok;
    }
    if (!state(d))
        return 0;
    fx_text(dp("interrupt"), "");
    machine(d);
    ok = fx_check(f,
                  incomplete(d, "etude was not built on machine") &&
                      !fx_exists(dp("home/.local/state/etude")),
                  "stopped build") &&
         ok;
    machine(d);
    if (!deployed(d))
        return 0;
    snprintf(oldkey, sizeof(oldkey), "%s", d->key);
    snprintf(olddir, sizeof(olddir), "%s", d->dir);
    saved = change(d);
    fx_text(dp("interrupt-record"), "");
    machine(d);
    ok = fx_check(f,
                  incomplete(d, "the record of machine was not written: status 143") &&
                      fx_count(dp("home/.local/state/etude")) == 2,
                  "stopped record removes scratch") &&
         ok;
    snprintf(d->key, sizeof(d->key), "%s", oldkey);
    snprintf(d->dir, sizeof(d->dir), "%s", olddir);
    ok = fx_check(f, home_record(d), "stopped record keeps old machine") && ok;
    machine(d);
    restore(d, saved);
    return fx_check(f,
                    deployed(d) && (strcmp(d->key, oldkey) || strcmp(d->dir, olddir)) &&
                        home_record(d),
                    "resume after stopped record") &&
           ok;
}
static int local_revision_rows(struct deploy *d)
{
    struct fixture *f = d->f;
    char before[65], after[65], path[4096], *saved;
    int ok = 1;
    const char *init[] = {"init", "-q", NULL}, *add[] = {"add", "-A", NULL},
               *commit[] = {"commit", "-qm", "source", NULL};
    if (!state(d) || !git(d, d->tree, init) || !git(d, d->tree, add) || !git(d, d->tree, commit))
        return 0;
    snprintf(path, sizeof(path), "%s/untracked", d->tree);
    fx_text(path, "untracked\n");
    {
        const char *paths[] = {d->tree, NULL};
        if (!flow_snapshot(paths, before))
            return 0;
        const char *a[] = {"-S", dp("state"), "-l", NULL};
        run(d, a);
        if (!flow_snapshot(paths, after))
            return 0;
    }
    unlink(path);
    snprintf(path, sizeof(path), "%s/.git", d->tree);
    fx_remove(path);
    ok = fx_check(f,
                  deployed(d) && fx_equal(dp("ssh.log"), "") &&
                      record(d, dp("state/machine"), dp("state/ledger"), 0, NULL, NULL) &&
                      !strcmp(before, after),
                  "local deployment leaves source clone unchanged") &&
         ok;
    if (!state(d) || !git(d, d->tree, init) || !git(d, d->tree, add) || !git(d, d->tree, commit))
        return 0;
    snprintf(path, sizeof(path), "%s/tools/etude/ledger.c", d->tree);
    saved = fx_read(path);
    if (!fx_replace(path, path, "#define LEDGER_MAGIC \"etude-ledger 1\"",
                    "#define LEDGER_MAGIC \"etude-ledger 9\""))
        return 0;
    {
        const char *a[] = {"-r", "HEAD", "machine", NULL};
        run(d, a);
    }
    fx_text(path, saved);
    free(saved);
    snprintf(path, sizeof(path), "%s/.git", d->tree);
    fx_remove(path);
    return fx_check(f, deployed(d) && home_record(d), "committed deployment revision") && ok;
}
static int feature_rows(struct deploy *d)
{
    struct fixture *f = d->f;
    char source[4096], target[4096], *saved;
    unsigned i;
    int ok = 1;
    snprintf(source, sizeof(source), "%s/tools/etude/provision_machine.sh", d->tree);
    saved = fx_read(source);
    for (i = 0; i < 3; i++) {
        if (!state(d) || !fx_replace(source, source, "/dev/kvm", dp("kvm")))
            return 0;
        if (i == 0)
            fx_text(dp("kvm"), "device data\n");
        else if (i == 1)
            fx_mkdir(dp("kvm"));
        machine(d);
        fx_text(source, saved);
        ok = fx_check(f,
                      deployed(d) &&
                          record(d, dp("home/.local/state/etude/machine"),
                                 dp("home/.local/state/etude/ledger"), 0, NULL,
                                 i == 0 ? "kvm tcg" : "tcg") &&
                          untouched(d),
                      i == 0   ? "KVM open"
                      : i == 1 ? "KVM unavailable"
                               : "KVM missing") &&
             ok;
        if (i == 0)
            ok = fx_check(f, fx_equal(dp("kvm"), "device data\n"), "KVM device data unchanged") &&
                 ok;
        if (i == 2)
            ok = fx_check(f, !fx_exists(dp("kvm")), "missing KVM not created") && ok;
    }
    free(saved);
    if (!state(d))
        return 0;
    flow_put(f, "share/qemu/edk2-x86_64-code.fd", "");
    flow_put(f, "share/qemu/edk2-i386-vars.fd", "");
    d->ovmf[0] = 0;
    machine(d);
    fx_remove(flow_path(f, "share"));
    snprintf(target, sizeof(target), "%s/mbin/../share/qemu/edk2-x86_64-code.fd", f->work);
    ok = fx_check(f,
                  deployed(d) && record(d, dp("home/.local/state/etude/machine"),
                                        dp("home/.local/state/etude/ledger"), 0, target, NULL),
                  "OVMF discovered beside QEMU") &&
         ok;
    if (!state(d))
        return 0;
    snprintf(d->ovmf, sizeof(d->ovmf), "%s", dp("absent.fd"));
    machine(d);
    return fx_check(f,
                    deployed(d) && record(d, dp("home/.local/state/etude/machine"),
                                          dp("home/.local/state/etude/ledger"), 0, "-", NULL),
                    "missing OVMF override") &&
           ok;
}
static int windows_row(struct deploy *d)
{
    char target[4096];
    int ok;
    if (!state(d))
        return 0;
    unlink(dp("have/user-tool"));
    fx_text(dp("have/whpx"), "");
    fx_mkdir(dp("programfiles/qemu"));
    snprintf(target, sizeof(target), "%s", dp("programfiles/qemu/qemu-system-x86_64.exe"));
    if (!fx_copy(d->f->self, target) ||
        !fx_text(dp("programfiles/qemu/share/edk2-x86_64-code.fd"), ""))
        return 0;
    d->ovmf[0] = 0;
    {
        const char *a[] = {"-w", "machine", NULL};
        run(d, a);
    }
    ok = deployed(d) && fx_has(dp("log"), "pwsh Get user-tool\n") && lines(dp("ssh.log")) >= 2 &&
         record(d, dp("appdata/etude-state/machine"), dp("appdata/etude-state/ledger"), 1,
                "C:/Program Files/qemu/share/edk2-x86_64-code.fd", NULL);
    {
        char *calls = fx_read(dp("ssh.log"));
        const char *p = calls, *wanted = "-T -o BatchMode=yes -- machine C:/msys64/usr/bin/env.exe "
                                         "PATH=/usr/bin:/ucrt64/bin sh -s\n";
        unsigned n = 0;
        while ((p = strstr(p, wanted))) {
            n++;
            p += strlen(wanted);
        }
        free(calls);
        ok = ok && n == 2;
    }
    snprintf(target, sizeof(target), "%s/cache/builds/%s.", d->fix, d->key);
    return fx_check(d->f,
                    ok && !strncmp(d->dir, target, strlen(target)) &&
                        strlen(d->dir + strlen(target)) == 6,
                    "Windows deployment and machine record");
}
int flow_deploy(struct fixture *f)
{
    struct deploy d;
    int ok = 1;
    if (!setup(&d, f))
        return 0;
    ok = basic_rows(&d) && ok;
    ok = prerequisite_rows(&d) && ok;
    ok = busy_row(&d) && ok;
    ok = format_rights_rows(&d) && ok;
    ok = interrupted_rows(&d) && ok;
    ok = local_revision_rows(&d) && ok;
    ok = feature_rows(&d) && ok;
    ok = windows_row(&d) && ok;
    fx_env("FLOW_DEPLOY", NULL);
    return ok;
}
