/* flow_version.c: version publication, private shells, interrupted builds. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include "sha256.h"
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static const char *ev(const char *n)
{
    const char *v = getenv(n);
    return v ? v : "";
}
static const char *vp(const char *name)
{
    static char paths[32][4096];
    static unsigned next;
    char *p = paths[next++ % 32];
    snprintf(p, 4096, "%s/%s", ev("FIX"), name);
    return p;
}
static void append(const char *p, const char *s)
{
    FILE *f = fopen(p, "ab");
    if (f) {
        fputs(s, f);
        fclose(f);
    }
}
static void arguments(char *buf, size_t cap, int argc, char **argv)
{
    int i;
    buf[0] = 0;
    for (i = 1; i < argc; i++) {
        if (i > 1)
            strncat(buf, " ", cap - strlen(buf) - 1);
        strncat(buf, argv[i], cap - strlen(buf) - 1);
    }
    strncat(buf, "\n", cap - strlen(buf) - 1);
}
static int hash_file(struct sha256 *h, const char *p)
{
    FILE *f = fopen(p, "rb");
    unsigned char b[8192];
    size_t n;
    int ok;
    if (!f)
        return 0;
    while ((n = fread(b, 1, sizeof(b), f)))
        sha256_update(h, b, n);
    ok = !ferror(f);
    fclose(f);
    return ok;
}
static int digest(const char *p, char out[65])
{
    struct sha256 h;
    unsigned char b[32];
    sha256_init(&h);
    if (!hash_file(&h, p))
        return 0;
    sha256_final(&h, b);
    sha256_hex(b, out);
    return 1;
}
static int version_ssh_mock(int argc, char **argv)
{
    char log[16000];
    arguments(log, sizeof(log), argc, argv);
    char *shell = fx_read(vp("shell")), *prefix = fx_read(vp("path")), path[18000], *home;
    const char *command;
    append(vp("ssh.log"), log);
    if (argc != 7 || strcmp(argv[1], "-T") || strcmp(argv[2], "-o") ||
        strcmp(argv[3], "BatchMode=yes") || strcmp(argv[4], "--")) {
        free(shell);
        free(prefix);
        return 255;
    }
    command = argv[6];
    if (!*shell) {
        free(shell);
        shell = strdup("sh");
    }
    shell[strcspn(shell, "\r\n")] = 0;
    prefix[strcspn(prefix, "\r\n")] = 0;
    if (!strcmp(command, "C:\\msys64\\usr\\bin\\env.exe PATH=/usr/bin:/ucrt64/bin sh -s")) {
        command = "sh -s";
        snprintf(path, sizeof(path), "%s/windows:%s%s%s", ev("FLOW_WORK"), prefix,
                 *prefix ? ":" : "", ev("PATH"));
        fx_env("LOCALAPPDATA", "C:\\Users\\fix\\AppData\\Local");
    } else
        snprintf(path, sizeof(path), "%s%s%s", prefix, *prefix ? ":" : "", ev("PATH"));
    home = strdup(vp("home"));
    fx_env("HOME", home);
    snprintf(log, sizeof(log), "%s/.bashrc", home);
    fx_env("BASH_ENV", log);
    fx_env("PATH", path);
    setpgid(0, 0);
    {
        char *a[] = {shell, "-c", (char *)command, NULL};
        execvp(shell, a);
    }
    free(home);
    free(shell);
    free(prefix);
    return 127;
}
static int version_fakecc_mock(int argc, char **argv)
{
    char log[16000];
    arguments(log, sizeof(log), argc, argv);
    char *version = fx_read(vp("ccversion")), *out = NULL, hex[65];
    struct sha256 h;
    unsigned char bytes[32];
    int i;
    version[strcspn(version, "\r\n")] = 0;
    if (argc == 2 && !strcmp(argv[1], "-v")) {
        fprintf(stderr, "fakecc version %s\n", *version ? version : "1");
        free(version);
        return 0;
    }
    free(version);
    append(vp("cc.log"), log);
    snprintf(log, sizeof(log), "%s\n", ev("MAKEFLAGS"));
    append(vp("flags.log"), log);
    if (fx_exists(vp("fail"))) {
        fprintf(stderr, "fakecc: error: planted failure\n");
        return 1;
    }
    if (fx_exists(vp("interrupt"))) {
        char *sig = fx_read(vp("interrupt"));
        unlink(vp("interrupt"));
        kill(0, !strncmp(sig, "KILL", 4) ? SIGKILL : SIGTERM);
        free(sig);
        return 1;
    }
    if (fx_exists(vp("race"))) {
        struct fixture f;
        char script[4096];
        const char *a[] = {"/bin/sh", script, "-c", "fakecc", "-C", vp("cache"), "machine", NULL};
        unlink(vp("race"));
        memset(&f, 0, sizeof(f));
        snprintf(f.work, sizeof(f.work), "%s/race-commands", ev("FLOW_WORK"));
        fx_mkdir(f.work);
        snprintf(script, sizeof(script), "%s/tools/etude/provision_version.sh", ev("TREE"));
        if (fx_run(&f, a, ev("TREE"), NULL, NULL))
            return 1;
        fx_text(vp("race.out"), f.out);
        fx_text(vp("race.err"), f.err);
        fx_destroy(&f);
    }
    sha256_init(&h);
    for (i = 1; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (n >= 2 && !strcmp(argv[i] + n - 2, ".c"))
            if (!hash_file(&h, argv[i]))
                return 1;
    }
    if (!out)
        return 1;
    sha256_final(&h, bytes);
    sha256_hex(bytes, hex);
    snprintf(log, sizeof(log), "fakecc\n%s\n", hex);
    return fx_text(out, log) ? 0 : 1;
}
static int version_uname_mock(int argc, char **argv)
{
    if (argc != 2)
        return 1;
    if (!strcmp(argv[1], "-s"))
        puts("MINGW64_NT-10.0-26100");
    else if (!strcmp(argv[1], "-m"))
        puts("x86_64");
    else if (!strcmp(argv[1], "-r"))
        puts("3.6.1-1.x86_64");
    else
        return 1;
    return 0;
}
static int version_cygpath_mock(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "-u") || strcmp(argv[2], "C:\\Users\\fix\\AppData\\Local"))
        return 1;
    puts(vp("appdata"));
    return 0;
}
int flow_version_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "ssh"))
        return version_ssh_mock(argc, argv);
    if (!strcmp(name, "fakecc"))
        return version_fakecc_mock(argc, argv);
    if (!strcmp(name, "uname"))
        return version_uname_mock(argc, argv);
    if (!strcmp(name, "cygpath"))
        return version_cygpath_mock(argc, argv);
    return 97;
}

struct version {
    struct fixture *f;
    char tree[4096], fix[4096], key[100], dir[4096], script[4096];
};
static int state(struct version *v)
{
    unsigned i;
    const char *files[] = {".bashrc", ".zshenv", ".cshrc", ".tcshrc", ".config/fish/config.fish"};
    char path[4096], text[1000];
    fx_remove(v->fix);
    fx_mkdir(v->fix);
    fx_mkdir(vp("home/.config/fish"));
    fx_text(vp("cc.log"), "");
    fx_text(vp("flags.log"), "");
    fx_text(vp("ssh.log"), "");
    for (i = 0; i < 5; i++) {
        snprintf(path, sizeof(path), "home/%s", files[i]);
        snprintf(
            text, sizeof(text),
            "echo noise from %s\necho @etude-version 0123456789abcdef built 00 /nowhere\ncd /\n",
            files[i]);
        if (!fx_text(vp(path), text))
            return 0;
    }
    return 1;
}
static int invoke(struct version *v, const char *const extra[])
{
    const char *a[40] = {"/bin/sh", v->script};
    unsigned n = 2, i;
    for (i = 0; extra[i] && n < 39; i++)
        a[n++] = extra[i];
    a[n] = NULL;
    return !fx_run(v->f, a, v->tree, NULL, NULL);
}
static int run(struct version *v)
{
    const char *a[] = {"-c", "fakecc", "-C", vp("cache"), "machine", NULL};
    return invoke(v, a);
}
static unsigned lines(const char *text)
{
    unsigned n = 0;
    for (; *text; text++)
        if (*text == '\n')
            n++;
    return n;
}
static unsigned compiles(void)
{
    char *s = fx_read(vp("cc.log"));
    unsigned n = lines(s);
    free(s);
    return n;
}
static int result(struct version *v, const char *how, const char *root)
{
    char word[100], key[100], dir[4096], extra[20], prefix[4096], version[4096], published[4096];
    int n;
    if (v->f->status || lines(v->f->out) != 1)
        return 0;
    n = sscanf(v->f->out, "etude provision: result: %99s %99s %4095s %19s", word, key, dir, extra);
    if (n != 3 || strcmp(word, how) || strlen(key) != 64 || strspn(key, "0123456789abcdef") != 64)
        return 0;
    snprintf(prefix, sizeof(prefix), "%s/builds/%s.", root ? root : vp("cache"), key);
    if (strncmp(dir, prefix, strlen(prefix)) || strlen(dir + strlen(prefix)) != 6)
        return 0;
    snprintf(version, sizeof(version), "%s/version", dir);
    snprintf(published, sizeof(published), "%s/versions/%s", root ? root : vp("cache"), key);
    if (!fx_same(version, published))
        return 0;
    snprintf(v->key, sizeof(v->key), "%s", key);
    snprintf(v->dir, sizeof(v->dir), "%s", dir);
    return 1;
}
static int refused(struct version *v)
{
    return v->f->status && !strstr(v->f->out, "result:");
}
static int empty(const char *path)
{
    int n = fx_count(path);
    return n == 0 || n == -1;
}
static int record(struct version *v)
{
    char file[4096], binary[4096], hex[65], line[200], *text, *at;
    struct sha256 h;
    unsigned char bytes[32];
    int ok;
    unsigned n = 0;
    snprintf(file, sizeof(file), "%s/version", v->dir);
    snprintf(binary, sizeof(binary), "%s/bin/etude", v->dir);
    if (!digest(binary, hex))
        return 0;
    snprintf(line, sizeof(line), "program etude %s\n", hex);
    text = fx_read(file);
    ok = strstr(text, line) && strstr(text, "flags -std=c99 -O2\n") && strstr(text, "cc fakecc\n");
    at = text;
    while (*at && n < 5)
        if (*at++ == '\n')
            n++;
    sha256_init(&h);
    sha256_update(&h, (const unsigned char *)text, (size_t)(at - text));
    sha256_final(&h, bytes);
    sha256_hex(bytes, hex);
    free(text);
    return ok && n == 5 && !strcmp(hex, v->key);
}
static int shell_row(struct version *v, const char *shell, const char *path)
{
    int noise = !strcmp(shell, "bash") || !strcmp(shell, "zsh") || !strcmp(shell, "tcsh") ||
                !strcmp(shell, "csh") || !strcmp(shell, "fish"),
        ok;
    if (!state(v) || !fx_text(vp("shell"), shell) || (path && !fx_text(vp("path"), path)))
        return 0;
    if (!run(v) || !result(v, "built", NULL) || !run(v) || !result(v, "reused", NULL))
        return 0;
    ok = compiles() == 1;
    if (noise)
        ok = ok && strstr(v->f->err, "noise from ");
    return ok;
}
static int snapshots(struct version *v, char hex[65])
{
    struct sha256 h;
    unsigned char bytes[32];
    char file[4096];
    sha256_init(&h);
    snprintf(file, sizeof(file), "%s/version", v->dir);
    if (!hash_file(&h, file))
        return 0;
    snprintf(file, sizeof(file), "%s/bin/etude", v->dir);
    if (!hash_file(&h, file))
        return 0;
    sha256_final(&h, bytes);
    sha256_hex(bytes, hex);
    return 1;
}
static int version_setup(struct version *value, struct fixture *f)
{
    struct version v;
    char target[4096];
    memset(&v, 0, sizeof(v));
    v.f = f;
    snprintf(v.tree, sizeof(v.tree), "%s/tree", f->work);
    snprintf(v.fix, sizeof(v.fix), "%s/fix", f->work);
    snprintf(v.script, sizeof(v.script), "%s/tools/etude/provision_version.sh", v.tree);
    snprintf(target, sizeof(target), "%s/tools/etude", v.tree);
    if (!fx_tree(flow_source(f, "tools/etude"), target))
        return 0;
    snprintf(target, sizeof(target), "%s/tests/native", v.tree);
    if (!fx_tree(flow_source(f, "tests/native"), target))
        return 0;
    snprintf(target, sizeof(target), "%s/utils/sha256.c", v.tree);
    if (!fx_copy(flow_source(f, "utils/sha256.c"), target))
        return 0;
    snprintf(target, sizeof(target), "%s/utils/sha256.h", v.tree);
    if (!fx_copy(flow_source(f, "utils/sha256.h"), target))
        return 0;
    fx_env("TREE", v.tree);
    fx_env("FIX", v.fix);
    fx_env("FLOW_VERSION", "1");
    fx_env("MAKEFLAGS", NULL);
    fx_env("MFLAGS", NULL);
    fx_env("MAKELEVEL", NULL);
    if (!fx_mock(f, "ssh") || !fx_mock(f, "fakecc"))
        return 0;
    fx_mkdir(flow_path(f, "windows"));
    snprintf(target, sizeof(target), "%s/windows/uname", f->work);
    if (!fx_copy(f->self, target))
        return 0;
    snprintf(target, sizeof(target), "%s/windows/cygpath", f->work);
    if (!fx_copy(f->self, target))
        return 0;
    *value = v;
    return 1;
}
static int version_reuse_identity(struct version *value)
{
    struct version v = *value;
    struct fixture *f = v.f;
    int ok = 1;
    char source[4096];
    char target[4096];
    char first[5000];
    char second[100];
    char *saved;
    if (!state(&v))
        return 0;
    run(&v);
    ok = fx_check(f,
                  result(&v, "built", NULL) &&
                      fx_equal(vp("ssh.log"), "-T -o BatchMode=yes -- machine sh -s\n") &&
                      fx_count(vp("cache/builds")) == 1 && fx_count(vp("cache/versions")) == 1 &&
                      compiles() == 1 && record(&v),
                  "build record and program") &&
         ok;
    if (!state(&v) || !run(&v) || !result(&v, "built", NULL))
        return 0;
    snprintf(first, sizeof(first), "%s %s", v.key, v.dir);
    snprintf(target, sizeof(target), "%s/tools/etude/Readme.md", v.tree);
    saved = fx_read(target);
    append(target, "A change of the Readme.\n");
    snprintf(source, sizeof(source), "%s/tools/etude/fixtures/new.c", v.tree);
    fx_text(source, "int fixture;\n");
    run(&v);
    fx_text(target, saved);
    free(saved);
    unlink(source);
    ok =
        fx_check(f, result(&v, "reused", NULL) && compiles() == 1, "reuse unrelated changes") && ok;
    snprintf(second, sizeof(second), "%s", v.key);
    ok = fx_check(f, !strncmp(first, second, 64) && !strcmp(first + 65, v.dir),
                  "reuse same version path") &&
         ok;
    return ok;
}
static int version_publication_identity(struct version *value)
{
    struct version v = *value;
    struct fixture *f = v.f;
    char source[4096], first[5000], second[100], before[65], after[65], *saved;
    int ok = version_reuse_identity(value);
    if (!state(&v) || !run(&v) || !result(&v, "built", NULL) || !snapshots(&v, before))
        return 0;
    snprintf(first, sizeof(first), "%s %s", v.key, v.dir);
    snprintf(source, sizeof(source), "%s/tools/etude/json.c", v.tree);
    saved = fx_read(source);
    append(source, "/* A change. */\n");
    run(&v);
    fx_text(source, saved);
    free(saved);
    if (!result(&v, "built", NULL))
        return 0;
    snprintf(second, sizeof(second), "%s", v.key);
    fx_text(vp("ccversion"), "2\n");
    run(&v);
    unlink(vp("ccversion"));
    ok = fx_check(f,
                  result(&v, "built", NULL) && strcmp(v.key, second) && strncmp(v.key, first, 64) &&
                      strncmp(second, first, 64),
                  "distinct source and compiler versions") &&
         ok;
    run(&v);
    ok = fx_check(f,
                  result(&v, "reused", NULL) && !strncmp(first, v.key, 64) &&
                      !strcmp(first + 65, v.dir) && compiles() == 3 && snapshots(&v, after) &&
                      !strcmp(before, after) && fx_count(vp("cache/versions")) == 3 &&
                      fx_count(vp("cache/builds")) == 3,
                  "side by side and first version unchanged") &&
         ok;
    return ok;
}
static int version_native_sources(struct version *value)
{
    struct version v = *value;
    struct fixture *f = v.f;
    char source[4096], staged[4096], first[100], *saved;
    int ok;
    if (!state(&v) || !run(&v) || !result(&v, "built", NULL))
        return 0;
    snprintf(first, sizeof(first), "%s", v.key);
    snprintf(source, sizeof(source), "%s/tests/native/collection.c", v.tree);
    snprintf(staged, sizeof(staged), "%s/src/tests/native/collection.c", v.dir);
    ok = fx_check(f, fx_same(source, staged), "native collection source reaches the builder");
    saved = fx_read(source);
    append(source, "/* A native collection change. */\n");
    run(&v);
    fx_text(source, saved);
    free(saved);
    ok = fx_check(f, result(&v, "built", NULL) && strcmp(first, v.key) && compiles() == 2,
                  "native collection source changes the version") &&
         ok;
    run(&v);
    return fx_check(f, result(&v, "reused", NULL) && !strcmp(first, v.key) && compiles() == 2,
                    "restored native source reuses its version") &&
           ok;
}
static int version_account_shells(struct version *value)
{
    struct version v = *value;
    struct fixture *f = v.f;
    int ok = 1;
    char source[4096];
    char target[4096];
    unsigned i;
    {
        const char *shells[] = {"sh",   "bash", "dash", "zsh",  "ksh",
                                "mksh", "yash", "fish", "tcsh", "csh"};
        for (i = 0; i < 10; i++) {
            snprintf(source, sizeof(source), "/usr/bin/%s", shells[i]);
            if (i < 2 || !access(source, X_OK))
                ok = fx_check(f, shell_row(&v, shells[i], NULL), shells[i]) && ok;
        }
    }
    {
        const char *a[] = {"/bin/busybox", "--list", NULL};
        char *applets, *line, *save = NULL;
        fx_run(f, a, f->work, NULL, NULL);
        if (f->status)
            return 0;
        applets = strdup(f->out);
        fx_mkdir(flow_path(f, "busybox"));
        for (line = strtok_r(applets, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if (!strcmp(line, "make") || !strcmp(line, "cc") || !strcmp(line, "env") ||
                !strcmp(line, "perl"))
                continue;
            snprintf(target, sizeof(target), "%s/busybox/%s", f->work, line);
            symlink("/bin/busybox", target);
        }
        free(applets);
        snprintf(target, sizeof(target), "%s/busybox/sh", f->work);
        ok = fx_check(f, shell_row(&v, target, flow_path(f, "busybox")),
                      "BusyBox shell and applets") &&
             ok;
    }
    return ok;
}
static int version_interruption(struct version *value)
{
    struct version v = *value;
    struct fixture *f = v.f;
    int ok = 1;
    char target[4096];
    unsigned i;
    for (i = 0; i < 2; i++) {
        char killed[4096] = "";
        DIR *d;
        struct dirent *e;
        if (!state(&v) || !fx_text(vp("interrupt"), i ? "KILL\n" : "TERM\n"))
            return 0;
        run(&v);
        ok = fx_check(f,
                      refused(&v) && empty(vp("cache/versions")) &&
                          (i ? fx_count(vp("cache/builds")) == 1 : empty(vp("cache/builds"))),
                      i ? "killed build unpublished" : "stopped build removed") &&
             ok;
        if (i) {
            d = opendir(vp("cache/builds"));
            if (!d)
                return 0;
            while ((e = readdir(d)))
                if (e->d_name[0] != '.') {
                    snprintf(killed, sizeof(killed), "%s/%s", vp("cache/builds"), e->d_name);
                    break;
                }
            closedir(d);
        }
        run(&v);
        ok = fx_check(f,
                      result(&v, "built", NULL) &&
                          (!i || (strcmp(v.dir, killed) && fx_exists(killed))),
                      "build after interrupted attempt") &&
             ok;
        if (i) {
            snprintf(target, sizeof(target), "%s/version", killed);
            ok = fx_check(f, !fx_exists(target), "killed build has no version record") && ok;
        }
    }
    if (!state(&v))
        return 0;
    fx_text(vp("fail"), "");
    run(&v);
    ok = fx_check(f,
                  refused(&v) &&
                      fx_regex(f->err, "^etude provision: the build of [0-9a-f]+ failed$") &&
                      empty(vp("cache/builds")) && empty(vp("cache/versions")),
                  "failed compile publishes nothing") &&
         ok;
    return ok;
}
static int version_truncation(struct version *value)
{
    struct version v = *value;
    struct fixture *f = v.f;
    int ok = 1;
    char *saved;
    unsigned i;
    if (!state(&v))
        return 0;
    {
        const char *a[] = {"-c", "fakecc", "-C", vp("cache"), "-s", NULL};
        invoke(&v, a);
    }
    saved = strdup(f->out);
    {
        size_t len = strlen(saved), sizes[4] = {64, len / 2, len > 40 ? len - 40 : 0, 0};
        char *last = strrchr(saved, '\n');
        if (last && last > saved) {
            char *p = last - 1;
            while (p > saved && *p != '\n')
                p--;
            sizes[3] = (size_t)(p - saved + 1);
        }
        for (i = 0; i < 4; i++) {
            const char *a[] = {"/bin/sh", "-s", NULL};
            fx_write(flow_path(f, "script"), saved, sizes[i], 0600);
            {
                const char *env[] = {NULL};
                fx_env("HOME", vp("home"));
                fx_run(f, a, f->work, env, flow_path(f, "script"));
            }
            ok = fx_check(f, !fx_exists(vp("cache")) && !*f->out && !compiles(),
                          "truncated script changes nothing") &&
                 ok;
        }
        flow_put(f, "script", saved);
        {
            const char *a[] = {"/bin/sh", "-s", NULL};
            fx_run(f, a, f->work, NULL, flow_path(f, "script"));
            ok = fx_check(f, fx_regex(f->out, "^@etude-version [0-9a-f]+ built "),
                          "whole generated script builds") &&
                 ok;
        }
    }
    free(saved);
    return ok;
}
static int version_source_damage(struct version *value)
{
    struct version v = *value;
    struct fixture *f = v.f;
    int ok = 1;
    char *saved;
    if (!state(&v))
        return 0;
    {
        const char *a[] = {"-c", "fakecc", "-C", vp("cache"), "-s", NULL};
        invoke(&v, a);
    }
    saved = strdup(f->out);
    {
        char *at = strstr(saved, "    cat > \"$1/tools/etude/json.c\" <<"),
             *end = at ? strchr(at, '\n') : NULL, *damaged;
        if (!end) {
            free(saved);
            return 0;
        }
        damaged = malloc(strlen(saved) + 50);
        snprintf(damaged, (size_t)(end - saved) + 2, "%s", saved);
        strcat(damaged, "/* damage */\n");
        strcat(damaged, end + 1);
        flow_put(f, "script", damaged);
        free(damaged);
    }
    {
        const char *a[] = {"/bin/sh", "-s", NULL};
        fx_run(f, a, f->work, NULL, flow_path(f, "script"));
        ok =
            fx_check(f,
                     refused(&v) && strstr(f->err, "etude provision: the source arrived damaged") &&
                         empty(vp("cache/builds")) && empty(vp("cache/versions")) && !compiles(),
                     "damaged source not compiled") &&
            ok;
    }
    free(saved);
    return ok;
}
static int version_racing_publication(struct version *value)
{
    struct version v = *value;
    struct fixture *f = v.f;
    int ok = 1;
    if (!state(&v))
        return 0;
    fx_text(vp("race"), "");
    run(&v);
    ok = fx_check(f, result(&v, "reused", NULL) && fx_count(vp("cache/builds")) == 1,
                  "concurrent publication reuses one build") &&
         ok;
    {
        char wanted[5000];
        snprintf(wanted, sizeof(wanted), "etude provision: result: built %s %s\n", v.key, v.dir);
        ok = fx_check(f, fx_equal(vp("race.out"), wanted),
                      "racing request publishes selected build") &&
             ok;
    }
    return ok;
}
static int version_make_settings(struct version *value)
{
    struct version v = *value;
    struct fixture *f = v.f;
    int ok = 1;
    char *saved;
    if (!state(&v))
        return 0;
    fx_env("MAKEFLAGS", "-j4 -- ETUDE_PLANTED=1");
    {
        const char *a[] = {"-l", "-c", "fakecc", "-C", vp("cache"), NULL};
        invoke(&v, a);
    }
    fx_env("MAKEFLAGS", NULL);
    saved = fx_read(vp("flags.log"));
    ok = fx_check(f,
                  result(&v, "built", NULL) &&
                      !fx_regex(saved, "-j([^1]|1[0-9]|$)|jobserver|ETUDE_PLANTED"),
                  "local make settings isolated") &&
         ok;
    free(saved);
    return ok;
}
static int version_windows_version(struct version *value)
{
    struct version v = *value;
    struct fixture *f = v.f;
    int ok = 1;
    char target[4096];
    if (!state(&v))
        return 0;
    {
        const char *a[] = {"-w", "-c", "fakecc", "machine", NULL};
        invoke(&v, a);
    }
    ok = fx_check(f,
                  result(&v, "built", vp("appdata/etude")) &&
                      fx_equal(vp("ssh.log"),
                               "-T -o BatchMode=yes -- machine C:\\msys64\\usr\\bin\\env.exe "
                               "PATH=/usr/bin:/ucrt64/bin sh -s\n"),
                  "Windows remote command") &&
         ok;
    snprintf(target, sizeof(target), "%s/version", v.dir);
    ok = fx_check(
             f, fx_has(target, "platform windows x86_64\n") && fx_has(target, "program etude.exe "),
             "Windows program and platform") &&
         ok;
    return ok;
}
static int version_revision(struct version *value)
{
    struct version v = *value;
    struct fixture *f = v.f;
    int ok = 1;
    char source[4096];
    char first[5000];
    char *saved;
    /* The selected revision uses committed source after the tree changes. */
    if (!state(&v))
        return 0;
    {
        const char *a[] = {
            "/usr/bin/git", "-c", "user.name=test", "-c", "user.email=test@example.invalid", "init",
            "-q",           NULL};
        fx_run(f, a, v.tree, NULL, NULL);
        const char *add[] = {"/usr/bin/git", "add", "-A", NULL};
        fx_run(f, add, v.tree, NULL, NULL);
        const char *commit[] = {"/usr/bin/git",
                                "-c",
                                "user.name=test",
                                "-c",
                                "user.email=test@example.invalid",
                                "-c",
                                "commit.gpgsign=false",
                                "commit",
                                "-qm",
                                "source",
                                NULL};
        if (fx_run(f, commit, v.tree, NULL, NULL))
            return 0;
    }
    snprintf(source, sizeof(source), "%s/tests/native/collection.c", v.tree);
    saved = fx_read(source);
    append(source, "/* A change. */\n");
    {
        const char *a[] = {"-r", "HEAD", "-c", "fakecc", "-C", vp("cache"), "machine", NULL};
        invoke(&v, a);
    }
    if (!result(&v, "built", NULL)) {
        fx_text(source, saved);
        free(saved);
        return 0;
    }
    snprintf(first, sizeof(first), "%s %s", v.key, v.dir);
    run(&v);
    fx_text(source, saved);
    free(saved);
    ok = fx_check(f,
                  result(&v, "built", NULL) &&
                      (strncmp(first, v.key, 64) || strcmp(first + 65, v.dir)),
                  "working source differs from committed revision") &&
         ok;
    run(&v);
    fx_remove(flow_path(f, "tree/.git"));
    ok = fx_check(f,
                  result(&v, "reused", NULL) && !strncmp(first, v.key, 64) &&
                      !strcmp(first + 65, v.dir) && compiles() == 2,
                  "working source restored to revision") &&
         ok;
    return ok;
}
static int version_real_build(struct version *value)
{
    struct version v = *value;
    struct fixture *f = v.f;
    int ok = 1;
    char target[4096];
    unsigned i;
    /* Compile the product with the GNU compiler, then invoke its real verbs. */
    if (!state(&v))
        return 0;
    {
        const char *a[] = {"-l", "-C", vp("cache"), NULL};
        invoke(&v, a);
    }
    if (!fx_check(f, result(&v, "built", NULL), "real compiler build"))
        return 0;
    snprintf(target, sizeof(target), "%s/bin/etude", v.dir);
    {
        char schemas[4096];
        snprintf(schemas, sizeof(schemas), "%s/src/tools/etude/schemas", v.dir);
        const char *a[] = {target,
                           "check",
                           "-s",
                           schemas,
                           "schema",
                           "case",
                           flow_source(f, "tools/etude/schemas/case.host.example.json"),
                           NULL};
        ok = fx_check(f, !fx_run(f, a, f->work, NULL, NULL),
                      "built schema checker accepts example") &&
             ok;
    }
    {
        char schema[4096];
        snprintf(schema, sizeof(schema), "%s/src/tools/etude/schemas/history.schema.json", v.dir);
        const char *a[] = {target,
                           "check",
                           "history",
                           schema,
                           flow_source(f, "tools/etude/schemas/history.example.jsonl"),
                           NULL};
        ok = fx_check(f, !fx_run(f, a, f->work, NULL, NULL),
                      "built history checker accepts example") &&
             ok;
    }
    {
        const char *verbs[] = {"serve", "admit", "lock", "lock", "build"};
        const char *sub[] = {NULL, NULL, "tree", "ports", NULL};
        for (i = 0; i < 5; i++) {
            const char *a[] = {target, verbs[i], sub[i], NULL};
            char wanted[200];
            fx_run(f, a, f->work, NULL, NULL);
            snprintf(wanted, sizeof(wanted), "usage: etude %s%s%s ", verbs[i], sub[i] ? " " : "",
                     sub[i] ? sub[i] : "");
            ok = fx_check(f, f->status == 2 && strstr(f->err, wanted), wanted) && ok;
        }
    }
    return ok;
}
int flow_version(struct fixture *f)
{
    struct version v;
    int ok = 1;
    if (!version_setup(&v, f))
        return 0;
    ok = version_publication_identity(&v) && ok;
    ok = version_native_sources(&v) && ok;
    ok = version_account_shells(&v) && ok;
    ok = version_interruption(&v) && ok;
    ok = version_truncation(&v) && ok;
    ok = version_source_damage(&v) && ok;
    ok = version_racing_publication(&v) && ok;
    ok = version_make_settings(&v) && ok;
    ok = version_windows_version(&v) && ok;
    ok = version_revision(&v) && ok;
    ok = version_real_build(&v) && ok;
    fx_env("FLOW_VERSION", NULL);
    return ok;
}
