/* Prerequisite reports, installs, malformed recipes, firmware probes and elf
   compiler wrappers. */
#include "build_support.h"
#include <stdarg.h>
#include <utime.h>

static const char recipe_text[] =
    "platform linux\nneed alpha beta\nprobe [ -e \"$FIX/have/@TOOL@\" ]\n"
    "admin touch \"$FIX/have/alpha\" \"$FIX/have/beta\"\n"
    "need gamma\nprobe [ -e \"$FIX/have/gamma\" ]\nuser touch \"$FIX/have/gamma\"\n"
    "user echo @TREE@ @USER@ > \"$FIX/subst\"\n"
    "need delta\nprobe [ -e \"$FIX/have/delta\" ]\n"
    "admin touch \"$FIX/have/alpha\" \"$FIX/have/beta\"\nmanual touch \"$FIX/have/delta\"\n"
    "need scratch1 scratch2\nprobe [ -f tools/setup/prereq.sh ] && [ -z \"$(ls -A "
    "\"$PREREQ_TMP\")\" ] && : > \"$PREREQ_TMP/@TOOL@\"\nuser true\n"
    "platform windows\nneed wa\nprobe Have wa\nuser Get wa\n"
    "need wb\nprobe Have wb\nadmin Get wb\nneed wc\nprobe Have wc\nmanual Get wc\n";

static void add(char *text, size_t size, const char *format, ...)
{
    va_list ap;
    size_t n = strlen(text);
    va_start(ap, format);
    int z = vsnprintf(text + n, size - n, format, ap);
    va_end(ap);
    if (z < 0 || (size_t)z >= size - n)
        abort();
}

static int state(struct fixture *f, const char *names)
{
    const char *fix = bp(f->work, "fix");
    if (!fx_remove(fix) || !fx_mkdir(bp(fix, "have")) || !fx_text(bp(fix, "log"), ""))
        return 0;
    char *copy = strdup(names), *save;
    for (char *name = strtok_r(copy, " ", &save); name; name = strtok_r(NULL, " ", &save))
        if (!fx_text(bp(!strcmp(name, "root") || !strcmp(name, "sudo") || !strcmp(name, "elevated")
                            ? fix
                            : bp(fix, "have"),
                        name),
                     "")) {
            free(copy);
            return 0;
        }
    free(copy);
    return 1;
}

static void report(char *out, const char *present)
{
    const char *names[] = {"alpha", "beta", "gamma", "delta", "scratch1", "scratch2"};
    for (size_t i = 0; i < 6; i++)
        add(out, 32768, "  [%s]%s%s\n", strstr(present, names[i]) ? "PASS" : "MISSING",
            strstr(present, names[i]) ? "     " : "  ", names[i]);
}

static void expected_linux(struct fixture *f, int row, char out[32768])
{
    const char *rights = row == 2 ? "root" : row == 4 ? "sudo" : "no";
    add(out, 32768, "prereq: linux; administrator rights: %s\n", rights);
    report(out, row == 0   ? "alpha beta gamma delta scratch1 scratch2"
                : row == 2 ? "gamma scratch1 scratch2"
                           : "scratch1 scratch2");
    if (row == 0) {
        add(out, 32768, "prereq: result: pass: 6 tools present on linux\n");
        return;
    }
    if (row >= 3) {
        if (row == 4)
            add(out, 32768, "prereq: running: touch \"$FIX/have/alpha\" \"$FIX/have/beta\"\n");
        add(out, 32768,
            "prereq: running: touch \"$FIX/have/gamma\"\nprereq: running: echo %s/tree fixuser > "
            "\"$FIX/subst\"\nprereq: probing again\n",
            f->work);
        report(out, row == 4 ? "alpha beta gamma scratch1 scratch2" : "gamma scratch1 scratch2");
    }
    if (row == 1)
        add(out, 32768,
            "prereq: this account runs:\n    touch \"$FIX/have/gamma\"\n    echo %s/tree fixuser > "
            "\"$FIX/subst\"\n",
            f->work);
    if (row == 2 || row == 4)
        add(out, 32768,
            "prereq: this account runs, as an administrator:\n    %stouch \"$FIX/have/alpha\" "
            "\"$FIX/have/beta\"\n",
            row == 4 ? "sudo " : "");
    if (row == 1 || row == 3)
        add(out, 32768,
            "prereq: an administrator runs:\n    sudo touch \"$FIX/have/alpha\" "
            "\"$FIX/have/beta\"\n");
    add(out, 32768,
        "prereq: a person at the machine runs:\n    touch \"$FIX/have/delta\"\nprereq: result: "
        "missing: %d of 6 tools on linux\n",
        row == 1   ? 4
        : row == 4 ? 1
                   : 3);
}

static void expected_windows(int row, char out[32768])
{
    add(out, 32768, "prereq: windows; administrator rights: %s\n", row == 7 ? "elevated" : "no");
    add(out, 32768, "  [%s]%swa\n  [MISSING]  wb\n  [MISSING]  wc\n", row == 5 ? "PASS" : "MISSING",
        row == 5 ? "     " : "  ");
    if (row >= 6) {
        add(out, 32768, "prereq: running: Get wa\n");
        if (row == 7)
            add(out, 32768, "prereq: running: Get wb\n");
        add(out, 32768, "prereq: probing again\n  [PASS]     wa\n  [%s]%swb\n  [MISSING]  wc\n",
            row == 7 ? "PASS" : "MISSING", row == 7 ? "     " : "  ");
    }
    if (row != 7)
        add(out, 32768,
            "prereq: an administrator runs, in an elevated PowerShell on the Windows machine:\n    "
            "Get wb\n");
    add(out, 32768,
        "prereq: a person at the Windows machine runs:\n    Get wc\nprereq: result: missing: %d of "
        "3 tools on windows\n",
        row == 7 ? 1 : 2);
}

static int prereq_rows(struct fixture *f)
{
    const char *names[] = {"linux-present",   "linux-report",    "linux-root",
                           "linux-install",   "linux-sudo",      "windows-report",
                           "windows-install", "windows-elevated"};
    const char *states[] = {
        "alpha beta gamma delta", "", "root gamma", "", "sudo", "wa", "", "elevated"};
    char script[4096], recipe[4096];
    snprintf(script, sizeof(script), "%s", bp(f->work, "tree/tools/setup/prereq.sh"));
    snprintf(recipe, sizeof(recipe), "%s", bp(f->work, "recipe"));
    for (int row = 0; row < 8; row++) {
        if (!state(f, states[row]))
            return 0;
        char expected[32768] = "";
        if (row < 5)
            expected_linux(f, row, expected);
        else
            expected_windows(row, expected);
        const char *args[] = {"/bin/sh",
                              script,
                              "--recipe",
                              recipe,
                              "--platform",
                              row < 5 ? "linux" : "windows",
                              row == 3 || row == 4 || row == 6 || row == 7 ? "--install" : NULL,
                              NULL};
        int status = fx_run(f, args, f->work, NULL, NULL);
        char *actual = calloc(strlen(f->out ? f->out : "") + strlen(f->err ? f->err : "") + 1, 1);
        if (!actual)
            abort();
        strcat(actual, f->out ? f->out : "");
        strcat(actual, f->err ? f->err : "");
        fx_check(f, status == (row == 0 ? 0 : 1) && !strcmp(actual, expected), names[row]);
        free(actual);
        if (row == 1)
            fx_check(f, fx_empty(bp(f->work, "fix/have")) && fx_equal(bp(f->work, "fix/log"), ""),
                     "report makes no change");
        if (row == 3) {
            char value[8192];
            snprintf(value, sizeof(value), "%s/tree fixuser\n", f->work);
            fx_check(f,
                     fx_equal(bp(f->work, "fix/subst"), value) &&
                         fx_equal(bp(f->work, "fix/log"), ""),
                     "user install and placeholders");
        }
        if (row == 4)
            fx_check(f,
                     fx_equal(bp(f->work, "fix/log"),
                              "sudo sh -c touch \"$FIX/have/alpha\" \"$FIX/have/beta\"\n"),
                     "sudo executes only authorized installer");
        if (row == 6)
            fx_check(f, fx_equal(bp(f->work, "fix/log"), "pwsh Get wa\n"),
                     "unelevated Windows installs only user tool");
    }
    return 1;
}

/* A refusal of a recipe line begins `prereq: FILE:LINE: `. */
static int line_refused(const char *text, const char *path)
{
    char prefix[4200];
    snprintf(prefix, sizeof(prefix), "prereq: %s:", path);
    size_t n = strlen(prefix);
    for (const char *p = text; p && *p;) {
        if (!strncmp(p, prefix, n)) {
            size_t digits = strspn(p + n, "0123456789");
            if (digits && !strncmp(p + n + digits, ": ", 2))
                return 1;
        }
        p = strchr(p, '\n');
        p = p ? p + 1 : NULL;
    }
    return 0;
}

static int malformed(struct fixture *f)
{
    const char *bad[] = {"probe",
                         "need",
                         "user x",
                         "need x|probe p|user u|need x",
                         "need x|user y",
                         "need x|probe y|probe z|user w",
                         "need x|user y|probe z",
                         "platform beos",
                         NULL};
    const char *path = bp(f->work, "bad"), *script = bp(f->work, "tree/tools/setup/prereq.sh");
    for (size_t i = 0; bad[i]; i++) {
        char text[4096];
        snprintf(text, sizeof(text), "platform linux\n%s\n", bad[i]);
        for (char *p = text; *p; p++)
            if (*p == '|')
                *p = '\n';
        if (!fx_text(path, text))
            return 0;
        const char *a[] = {"/bin/sh", script, "--recipe", path, "--platform", "linux", NULL};
        fx_check(f,
                 fx_run(f, a, f->work, NULL, NULL) == 2 &&
                     (line_refused(f->out, path) || line_refused(f->err, path)),
                 bad[i]);
    }
    fx_text(path, "platform macos\nneed x\nprobe y\nuser z\n");
    const char *a[] = {"/bin/sh", script, "--recipe", path, "--platform", "linux", NULL};
    fx_check(f, fx_run(f, a, f->work, NULL, NULL) == 2 && output_has(f, "no platform linux"),
             "absent platform refused");
    return 1;
}

static int setup_error(const char *operation, const char *path)
{
    fprintf(stderr, "prereq: setup: %s %s: %s (uid %lu)\n", operation, path, strerror(errno),
            (unsigned long)geteuid());
    return 0;
}

static int setup_mode(const char *path, mode_t mode)
{
    return chmod(path, mode) == 0 || setup_error("chmod", path);
}

static int permissive(const char *path)
{
    struct stat st;
    if (lstat(path, &st))
        return setup_error("lstat", path);
    if (S_ISLNK(st.st_mode))
        return 1;
    if (!S_ISDIR(st.st_mode))
        return setup_mode(path, st.st_mode & 0111 ? 0755 : 0644);
    if (!setup_mode(path, 0755))
        return 0;
    DIR *d = opendir(path);
    struct dirent *e;
    int ok = 1;
    if (!d)
        return setup_error("opendir", path);
    for (;;) {
        char child[4096];
        int n;
        errno = 0;
        e = readdir(d);
        if (!e) {
            if (errno)
                ok = setup_error("readdir", path);
            break;
        }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        n = snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) {
            errno = ENAMETOOLONG;
            ok = setup_error("join directory entry", path);
        } else
            ok &= permissive(child);
    }
    if (closedir(d))
        ok = setup_error("closedir", path);
    return ok;
}

static int firmware(struct fixture *f, const char *platform)
{
    char *recipe = fx_read(bp(f->tree, "tools/setup/prereqs")), *save;
    int chosen = 0, selected = 0;
    FILE *out = fopen(bp(f->work, "ovmf-recipe"), "w");
    if (!recipe || !out) {
        free(recipe);
        if (out)
            fclose(out);
        return 0;
    }
    fprintf(out, "platform %s\n", platform);
    for (char *line = strtok_r(recipe, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "platform ", 9)) {
            chosen = !strcmp(line + 9, platform);
            selected = 0;
        }
        if (!strncmp(line, "need ", 5))
            selected = chosen && !strcmp(line, "need OVMF");
        if (selected)
            fprintf(out, "%s\n", line);
    }
    free(recipe);
    fclose(out);
    if (!state(f, ""))
        return setup_error("reset firmware state", bp(f->work, "fix"));
    const char *files[] = {"fix/firmware/share/qemu/edk2-x86_64-code.fd",
                           "fix/firmware/share/qemu/edk2-i386-vars.fd", "fix/unreadable"};
    if (!fx_mkdir(bp(f->work, "fix/firmware/bin")))
        return setup_error("mkdir", bp(f->work, "fix/firmware/bin"));
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        if (!fx_text(bp(f->work, files[i]), ""))
            return setup_error("write", bp(f->work, files[i]));
    if (!permissive(f->work) || !setup_mode(f->root, 0755) ||
        !setup_mode(bp(f->work, "fix/unreadable"), 0000))
        return 0;
    const char *variants[] = {"missing", "directory", "unreadable", "readable", "discovered"};
    const char *override[] = {"fix/missing", "fix/firmware", "fix/unreadable",
                              "fix/firmware/share/qemu/edk2-x86_64-code.fd", NULL};
    for (size_t i = 0; i < 5; i++) {
        char code[8192], vars[] = "RACH2_OVMF_VARS=", path[16384];
        snprintf(code, sizeof(code), "RACH2_OVMF_CODE=%s",
                 override[i] ? bp(f->work, override[i]) : "");
        snprintf(path, sizeof(path), "PATH=%s:%s:%s", f->bin, bp(f->work, "fix/firmware/bin"),
                 getenv("PATH"));
        const char *env[] = {code, vars, path, "TMPDIR=/tmp", NULL};
        const char *args[] = {f->self,      "--unprivileged",
                              "/bin/sh",    bp(f->work, "tree/tools/setup/prereq.sh"),
                              "--recipe",   bp(f->work, "ovmf-recipe"),
                              "--platform", platform,
                              NULL};
        int expected = i >= 3 ? 0 : 1;
        fx_check(f,
                 fx_run(f, args, f->work, env, NULL) == expected &&
                     output_has(f, expected ? "  [MISSING]  OVMF" : "  [PASS]     OVMF"),
                 variants[i]);
    }
    return 1;
}

static int listed(struct fixture *f, const char *platform, char **result)
{
    const char *a[] = {
        "/bin/sh", bp(f->tree, "tools/setup/prereq.sh"), "--list", "--platform", platform, NULL};
    if (fx_run(f, a, f->tree, NULL, NULL))
        return 0;
    *result = strdup(f->out ? f->out : "");
    return *result != NULL;
}
static int line_in(const char *text, const char *line)
{
    size_t n = strlen(line);
    for (const char *p = text; p && *p;) {
        const char *end = strchr(p, '\n');
        size_t z = end ? (size_t)(end - p) : strlen(p);
        if (z == n && !memcmp(p, line, n))
            return 1;
        p += z + (end != NULL);
    }
    return 0;
}
static int file_line(const char *path, const char *line)
{
    char *text = fx_read(path);
    int found = text && line_in(text, line);
    free(text);
    return found;
}
static int name_present(const char *list, const char *name)
{
    char *copy = strdup(list), *save;
    int found = 0;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "N\t", 2))
            continue;
        char *field = strchr(line + 2, '\t');
        if (!field)
            continue;
        char *end = strchr(++field, '\t');
        if (end)
            *end = 0;
        found |= !strcmp(field, name);
    }
    free(copy);
    return found;
}

static int skipped(const char *platform, const char *name)
{
    const char *macos = " unshare ip ";
    const char *windows =
        " unshare ip mkfs.exfat fsck.exfat mformat mmd mcopy mdir mtype mdel mdeltree minfo "
        "xorriso mke2fs debugfs e2fsck mandoc cppcheck shellcheck clang clang-format ";
    char token[512];
    snprintf(token, sizeof(token), " %s ", name);
    return strstr(!strcmp(platform, "macos")     ? macos
                  : !strcmp(platform, "windows") ? windows
                                                 : "",
                  token) != NULL;
}
static int choice_present(const char *have, const char *platform, const char *need)
{
    char *text = strdup(need), *save, *component, *parts;
    if (!text)
        return 0;
    int any = 0;
    for (char *choice = strtok_r(text, "/", &save); choice; choice = strtok_r(NULL, "/", &save)) {
        int all = 1;
        for (component = strtok_r(choice, "+", &parts); component;
             component = strtok_r(NULL, "+", &parts))
            all &= skipped(platform, component) || name_present(have, component);
        any |= all;
    }
    free(text);
    return any;
}
static void required(struct fixture *f, char *have[4], const char *need)
{
    const char *platforms[] = {"linux", "macos", "windows"};
    for (size_t p = 0; p < 3; p++) {
        if (skipped(platforms[p], need))
            continue;
        if (p == 2 && (!strncmp(need, "i686-elf-", 9) || !strncmp(need, "x86_64-elf-", 11))) {
            const char *target =
                !strncmp(need, "i686", 4) ? "i686-linux-gnu-gcc" : "x86_64-linux-gnu-gcc";
            fx_check(f, file_line(bp(f->tree, "tools/etude/tooling/tools"), target), target);
        } else
            fx_check(f, choice_present(have[p], platforms[p], need), need);
    }
}
static int recipe_sources(struct fixture *f)
{
    char *text = fx_read(bp(f->tree, "tools/setup/prereqs")), *at = text;
    if (!text)
        return 0;
    while ((at = strstr(at, "tools/setup/"))) {
        size_t n = strspn(at, "abcdefghijklmnopqrstuvwxyz_/.");
        if (n > 3 && !strncmp(at + n - 3, ".sh", 3)) {
            char path[512];
            if (n >= sizeof(path)) {
                free(text);
                return 0;
            }
            memcpy(path, at, n);
            path[n] = 0;
            fx_check(f, fx_exists(bp(f->tree, path)), path);
        }
        at += n ? n : 1;
    }
    free(text);
    return 1;
}
static void registry_coverage(struct fixture *f, char *have[4])
{
    char *save;
    char *text = fx_read(bp(f->tree, "tests/registry"));
    for (char *line = text ? strtok_r(text, "\n", &save) : NULL; line;
         line = strtok_r(NULL, "\n", &save)) {
        if (*line == '#')
            continue;
        char *field = strchr(line, '|');
        if (!field)
            continue;
        field++;
        char *end = strchr(field, '|');
        if (end)
            *end = 0;
        char *fields;
        for (char *name = strtok_r(field, ",", &fields); name; name = strtok_r(NULL, ",", &fields))
            if (strcmp(name, "-") && strcmp(name, "qemu"))
                required(f, have, name);
    }
    free(text);
}

static int coverage(struct fixture *f)
{
    char *have[4] = {0};
    const char *platforms[] = {"linux", "macos", "windows", "rach2"};
    for (size_t i = 0; i < 4; i++)
        if (!listed(f, platforms[i], &have[i]))
            goto done;
    char *text = fx_read(bp(f->tree, "tools/setup/preflight.sh")),
         *at = text ? strstr(text, "TOOLS=\"") : NULL;
    if (!at) {
        free(text);
        goto done;
    }
    at += 7;
    char *end = strchr(at, '"');
    if (!end) {
        free(text);
        goto done;
    }
    *end = 0;
    char *save;
    int found_cc = 0, found_fs = 0;
    for (char *name = strtok_r(at, " \\\n", &save); name; name = strtok_r(NULL, " \\\n", &save)) {
        char expanded[256];
        if (strstr(name, "${CROSS}")) {
            snprintf(expanded, sizeof(expanded), "i686-elf-%s", name + 8);
            required(f, have, expanded);
            found_cc |= !strcmp(expanded, "i686-elf-gcc");
            snprintf(expanded, sizeof(expanded), "x86_64-elf-%s", name + 8);
            required(f, have, expanded);
        } else {
            required(f, have, name);
            found_fs |= !strcmp(name, "mke2fs");
        }
    }
    free(text);
    fx_check(f, found_cc && found_fs, "preflight required list was read");
    const char *tools[] = {"cppcheck", "shellcheck",       "clang-format",       "clang", "OVMF",
                           "qemu-img", "qemu-system-i386", "qemu-system-x86_64", NULL};
    for (size_t i = 0; tools[i]; i++)
        required(f, have, tools[i]);
    registry_coverage(f, have);
    const char *needs[] = {"qemu-system-x86_64", "qemu-img", "curl"};
    for (size_t p = 1; p < 3; p++) {
        for (size_t i = 0; i < 3; i++)
            fx_check(f, name_present(have[p], needs[i]), needs[i]);
        char *recipe = fx_read(bp(f->tree, "tools/etude/tooling/tools"));
        const char *skip = p == 1 ? "unshare ip"
                                  : "unshare ip mkfs.exfat fsck.exfat mformat mmd mcopy mdir "
                                    "mtype mdel mdeltree minfo xorriso mke2fs debugfs e2fsck "
                                    "mandoc cppcheck shellcheck clang clang-format";
        char *words = strdup(skip), *parts;
        for (char *word = strtok_r(words, " ", &parts); word; word = strtok_r(NULL, " ", &parts))
            fx_check(f, recipe && line_in(recipe, word), word);
        free(words);
        free(recipe);
    }
    fx_check(f, name_present(have[3], "cc"), "Rach2 prerequisite recipe declares cc");
    int ok = recipe_sources(f);
    for (size_t i = 0; i < 4; i++)
        free(have[i]);
    return ok && !f->failed;
done:
    for (size_t i = 0; i < 4; i++)
        free(have[i]);
    return 0;
}

/* The date elfwrap_age gives a wrapper, which only a replaced one loses. */
#define ELFWRAP_AGE 1000000000

static int elfwrap_age(const char *path)
{
    struct utimbuf t = {ELFWRAP_AGE, ELFWRAP_AGE};
    return !utime(path, &t);
}

static int elfwrap_kept(const char *path)
{
    struct stat st;
    return !stat(path, &st) && st.st_mtime == ELFWRAP_AGE && (st.st_mode & 0777) == 0755;
}

/* Succeed when PATH has the date of the newer of the files GCC and CC1. */
static int elfwrap_dated(const char *path, const char *gcc, const char *cc1)
{
    struct stat w, g, c;
    return !stat(path, &w) && !stat(gcc, &g) && !stat(cc1, &c) &&
           w.st_mtime == (g.st_mtime > c.st_mtime ? g.st_mtime : c.st_mtime);
}

static int elfwrap_run(struct fixture *f, const char *arch, const char *dir)
{
    const char *args[] = {"/bin/sh", bp(f->tree, "tools/setup/elfwrap.sh"), arch, dir, NULL};
    return fx_run(f, args, f->work, NULL, NULL) == 0;
}

/* The paths of one architecture's elfwrap.sh rows. */
struct elfwrap_paths {
    const char *arch;
    char dir[4096], gcc[4096], cc1[4096], wrapper[4096];
    char first[4096]; /* a copy of the wrapper the last change wrote */
};

/* Make a stand-in Linux toolchain for W's architecture under FROM. */
static int elfwrap_toolchain(struct fixture *f, struct elfwrap_paths *w, const char *from)
{
    const char *tools[] = {"ld",      "ar", "objcopy", "strip", "nm",      "objdump",
                           "readelf", "as", "ranlib",  "size",  "strings", "c++filt"};
    snprintf(w->gcc, sizeof(w->gcc), "%s/%s-linux-gnu-gcc", from, w->arch);
    snprintf(w->cc1, sizeof(w->cc1), "%s.cc1", w->gcc);
    snprintf(w->wrapper, sizeof(w->wrapper), "%s/%s-elf-gcc", w->dir, w->arch);
    snprintf(w->first, sizeof(w->first), "%s/%s-elf-gcc.first", f->work, w->arch);
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        char tool[4096];
        snprintf(tool, sizeof(tool), "%s/%s-linux-gnu-%s", from, w->arch, tools[i]);
        if (!fx_write(tool, "", 0, 0755))
            return 0;
    }
    /* The fixture's own program may be read-only, as a prepared product is;
     * the stand-in compiler must take the byte that elfwrap_grow appends. */
    return fx_copy(f->self, w->gcc) && !chmod(w->gcc, 0755) && fx_text(w->cc1, "cc1 1\n");
}

static void elfwrap_check(struct fixture *f, const struct elfwrap_paths *w, int held,
                          const char *what)
{
    char label[128];
    snprintf(label, sizeof(label), "elfwrap %s: %s", w->arch, what);
    fx_check(f, held, label);
}

/* Run elfwrap.sh over an aged wrapper and succeed when it kept the wrapper. */
static int elfwrap_keeps(struct fixture *f, const struct elfwrap_paths *w)
{
    return elfwrap_age(w->wrapper) && elfwrap_run(f, w->arch, w->dir) && elfwrap_kept(w->wrapper) &&
           fx_same(w->wrapper, w->first);
}

/* Run elfwrap.sh over an aged wrapper and succeed when it wrote a new one. */
static int elfwrap_replaces(struct fixture *f, const struct elfwrap_paths *w)
{
    return elfwrap_age(w->wrapper) && elfwrap_run(f, w->arch, w->dir) &&
           !elfwrap_kept(w->wrapper) && !fx_same(w->wrapper, w->first) &&
           fx_copy(w->wrapper, w->first);
}

/* Append a byte to the program PATH, which leaves it runnable; say on
 * stderr why it could not. */
static int elfwrap_grow(const char *path)
{
    FILE *s = fopen(path, "ab");
    int ok = s && fputc(0, s) == 0;
    if (s && fclose(s))
        ok = 0;
    if (!ok)
        fprintf(stderr, "elfwrap: %s: %s\n", path, strerror(errno));
    return ok;
}

/**
 * Check elfwrap.sh for ARCH against a stand-in Linux toolchain: a second run
 * keeps the wrapper, so mk/fingerprint.mk sees the same file; a run in an
 * empty directory, as in a fresh tooling machine, writes the same bytes with
 * the compiler's date; and a changed cc1 or compiler driver replaces it.
 */
static int elfwrap(struct fixture *f, const char *arch)
{
    char from[4096];
    struct elfwrap_paths w = {arch, "", "", "", "", ""};
    snprintf(from, sizeof(from), "%s/cross", f->work);
    snprintf(w.dir, sizeof(w.dir), "%s/elf", f->work);
    if (!elfwrap_toolchain(f, &w, from))
        return 0;
    fx_env("RACH2_ELFWRAP_FROM", from);
    char *text = elfwrap_run(f, arch, w.dir) ? fx_read(w.wrapper) : NULL;
    elfwrap_check(f, &w,
                  text && strstr(text, w.gcc) && strstr(text, w.cc1) &&
                      elfwrap_dated(w.wrapper, w.gcc, w.cc1) && fx_copy(w.wrapper, w.first),
                  "the first run writes the wrapper");
    free(text);
    elfwrap_check(f, &w,
                  fx_remove(w.dir) && elfwrap_run(f, arch, w.dir) && fx_same(w.wrapper, w.first) &&
                      elfwrap_dated(w.wrapper, w.gcc, w.cc1),
                  "a fresh directory gets the same wrapper");
    elfwrap_check(f, &w, elfwrap_keeps(f, &w) && output_has(f, "the wrapper unchanged"),
                  "a second run keeps the wrapper");
    elfwrap_check(f, &w, fx_text(w.cc1, "cc1 2\n") && elfwrap_replaces(f, &w),
                  "a changed cc1 replaces the wrapper");
    elfwrap_check(f, &w, elfwrap_grow(w.gcc) && elfwrap_replaces(f, &w),
                  "a changed driver replaces the wrapper");
    elfwrap_check(f, &w, elfwrap_keeps(f, &w), "a run after the change keeps the wrapper");
    fx_env("RACH2_ELFWRAP_FROM", NULL);
    return !f->failed;
}

int build_prereq(struct fixture *f)
{
    const char *files[] = {"prereq.sh", "e2fsprogs_path.sh", "ovmf_path.sh", NULL};
    for (size_t i = 0; files[i]; i++)
        if (!fx_copy(bp(bp(f->tree, "tools/setup"), files[i]),
                     bp(bp(f->work, "tree/tools/setup"), files[i])))
            return 0;
    if (!fx_text(bp(f->work, "recipe"), recipe_text) || !fx_mock(f, "id") || !fx_mock(f, "sudo") ||
        !fx_mock(f, "pwsh"))
        return 0;
    fx_env("FIX", bp(f->work, "fix"));
    char ps[8192];
    snprintf(ps, sizeof(ps), "%s/pwsh -EncodedCommand", f->bin);
    fx_env("RACH2_PREREQ_POWERSHELL", ps);
    int ok = prereq_rows(f);
    ok &= malformed(f);
    ok &= coverage(f);
    ok &= firmware(f, "linux");
    ok &= firmware(f, "macos");
    ok &= elfwrap(f, "i686");
    ok &= elfwrap(f, "x86_64");
    return ok && !f->failed;
}
