/* Prerequisite reports, installs, malformed recipes and firmware probes. */
#include "build_support.h"
#include <stdarg.h>

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
                 fx_run(f, a, f->work, NULL, NULL) == 2 && output_has(f, "prereq: ") &&
                     output_has(f, path),
                 bad[i]);
    }
    fx_text(path, "platform macos\nneed x\nprobe y\nuser z\n");
    const char *a[] = {"/bin/sh", script, "--recipe", path, "--platform", "linux", NULL};
    fx_check(f, fx_run(f, a, f->work, NULL, NULL) == 2 && output_has(f, "no platform linux"),
             "absent platform refused");
    return 1;
}

static int permissive(const char *path)
{
    struct stat st;
    if (lstat(path, &st))
        return 0;
    if (!S_ISDIR(st.st_mode))
        return chmod(path, st.st_mode & 0111 ? 0755 : 0644) == 0;
    if (chmod(path, 0755))
        return 0;
    DIR *d = opendir(path);
    struct dirent *e;
    int ok = d != NULL;
    while (d && (e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            ok &= permissive(bp(path, e->d_name));
    if (d)
        closedir(d);
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
    if (!state(f, "") || !fx_mkdir(bp(f->work, "fix/firmware/bin")) ||
        !fx_text(bp(f->work, "fix/firmware/share/qemu/edk2-x86_64-code.fd"), "") ||
        !fx_text(bp(f->work, "fix/firmware/share/qemu/edk2-i386-vars.fd"), "") ||
        !fx_text(bp(f->work, "fix/unreadable"), ""))
        return 0;
    if (!permissive(f->work) || chmod(f->root, 0755) || chmod(bp(f->work, "fix/unreadable"), 0000))
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
    const char *macos = " unshare ip codex ";
    const char *windows =
        " unshare ip codex mkfs.exfat fsck.exfat mformat mmd mcopy mdir mtype mdel mdeltree minfo "
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
            fx_check(f, fx_has(bp(f->tree, "tools/etude/tooling/tools"), target), target);
        } else
            fx_check(f, choice_present(have[p], platforms[p], need), need);
    }
}
static int version_coverage(struct fixture *f)
{
    char *agent = fx_read(bp(f->tree, "tests/cases/host/agentapi.sh"));
    char *at = agent ? strstr(agent, "CODEX_VERSION=") : NULL;
    if (!at) {
        free(agent);
        return 0;
    }
    at += 14;
    size_t n = strcspn(at, "\n");
    char version[128];
    if (n >= sizeof(version)) {
        free(agent);
        return 0;
    }
    memcpy(version, at, n);
    version[n] = 0;
    char *list = NULL;
    int ok = listed(f, "linux", &list);
    char wanted[512];
    snprintf(wanted, sizeof(wanted), "\tadmin\tnpm install -g @openai/codex@%s\n", version);
    fx_check(f, ok && list && strstr(list, wanted),
             "Linux recipe installs the agents' Codex version");
    free(list);
    snprintf(wanted, sizeof(wanted), "CODEX_VERSION=%s\n", version);
    fx_check(f, fx_has(bp(f->tree, "tools/etude/tooling/provision.sh"), wanted),
             "tooling Codex version matches agent cases");
    free(agent);
    return 1;
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
        const char *skip = p == 1 ? "unshare ip codex"
                                  : "unshare ip codex mkfs.exfat fsck.exfat mformat mmd mcopy mdir "
                                    "mtype mdel mdeltree minfo xorriso mke2fs debugfs e2fsck "
                                    "mandoc cppcheck shellcheck clang clang-format";
        char *words = strdup(skip), *parts;
        for (char *word = strtok_r(words, " ", &parts); word; word = strtok_r(NULL, " ", &parts)) {
            char token[512];
            snprintf(token, sizeof(token), "%s\n", word);
            fx_check(f, recipe && strstr(recipe, token), word);
        }
        free(words);
        free(recipe);
    }
    fx_check(f, name_present(have[3], "cc"), "Rach2 prerequisite recipe declares cc");
    int ok = version_coverage(f) && recipe_sources(f);
    for (size_t i = 0; i < 4; i++)
        free(have[i]);
    return ok && !f->failed;
done:
    for (size_t i = 0; i < 4; i++)
        free(have[i]);
    return 0;
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
    return ok && !f->failed;
}
