/* The GNU host checks' step: its tree and its key come from one list. */
#include "build_support.h"

/* Fixture files under the paths that vendor/gnu/host_checks_files.sh lists. */
static const char *const carried[] = {"AGENTS.md",
                                      "CLAUDE.md",
                                      "Readme.md",
                                      "mk/host-check.mk",
                                      "tests/host/a_hosttest.c",
                                      "tests/cases/machine.usb.json",
                                      "tools/etude/a.c",
                                      "rach2/sys/a.h",
                                      "ports/doom/a.c",
                                      NULL};

/* Files outside them, and a compressed file inside them, which no check
 * reads. */
static const char *const outside[] = {"Makefile",
                                      "dev-log/a.md",
                                      "man/man1/a.1",
                                      "tools/flow/a.sh",
                                      "rach2/sources.mk",
                                      "ports/a/a.c",
                                      "tests/fuzz/corpus/a/a.gz",
                                      NULL};

/* The scripts the rule names beside the list. */
static const char *const scripts[] = {"vendor/gnu/build_host_inventory.sh",
                                      "vendor/gnu/host_checks_files.sh", NULL};

/**
 * Write each file of `names` under `repo`, its own name and then `extra`.
 * Return 1, or 0 when a file cannot be written.
 */
static int write_files(const char *repo, const char *const names[], const char *extra)
{
    for (size_t i = 0; names[i]; i++) {
        char text[512];
        snprintf(text, sizeof(text), "%s\n%s", names[i], extra);
        if (!fx_text(bp(repo, names[i]), text))
            return 0;
    }
    return 1;
}

/** Count the regular files under `path`. */
static int files_under(const char *path)
{
    struct stat st;
    if (lstat(path, &st))
        return 0;
    if (!S_ISDIR(st.st_mode))
        return S_ISREG(st.st_mode);
    DIR *d = opendir(path);
    struct dirent *e;
    int n = 0;
    char child[4096];
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        n += files_under(child);
    }
    if (d)
        closedir(d);
    return n;
}

/**
 * Lay out the fixture's tree in `repo`: the step's make file and scripts from
 * the source tree, the carried and outside files, and print.mk, which prints
 * GNU_HOST_CHECKS_SOURCES. Return 1, or 0 when a file cannot be written.
 */
static int repo_tree(struct fixture *f, const char *repo)
{
    static const char *const copied[] = {"mk/gnu-host-checks.mk",
                                         "vendor/gnu/build_host_inventory.sh",
                                         "vendor/gnu/host_checks_files.sh", NULL};
    for (size_t i = 0; copied[i]; i++)
        if (!fx_copy(bp(f->tree, copied[i]), bp(repo, copied[i])))
            return 0;
    return write_files(repo, carried, "") && write_files(repo, outside, "") &&
           fx_text(bp(repo, "print.mk"), "print:\n\t@printf '%s\\n' $(GNU_HOST_CHECKS_SOURCES)\n");
}

/**
 * Whether GNU make's GNU_HOST_CHECKS_SOURCES in `repo` is the list and the
 * scripts; the make first on the helper's PATH is pdpmake, which has no
 * $(shell).
 */
static int sources_are_list(struct fixture *f, const char *repo)
{
    const char *make[] = {"/usr/bin/make", "-s",    "-f", "mk/gnu-host-checks.mk", "-f",
                          "print.mk",      "print", NULL};
    if (fx_run(f, make, repo, NULL, NULL) || !f->out)
        return 0;
    int n = 0, found = 1;
    for (const char *p = f->out; *p; p += strcspn(p, "\n") + (p[strcspn(p, "\n")] != 0))
        n++;
    for (size_t i = 0; carried[i]; i++)
        found &= strstr(f->out, carried[i]) != NULL;
    for (size_t i = 0; scripts[i]; i++)
        found &= strstr(f->out, scripts[i]) != NULL;
    for (size_t i = 0; outside[i]; i++)
        found &= strstr(f->out, outside[i]) == NULL;
    return found && n == 11;
}

/**
 * Copy the list into a new tree; whether the tree has the carried files
 * alone.
 */
static int tree_is_list(struct fixture *f, const char *repo)
{
    char tree[4096];
    snprintf(tree, sizeof(tree), "%.4000s/tree", f->work);
    const char *copy[] = {"/bin/sh", "vendor/gnu/host_checks_files.sh", tree, NULL};
    if (!fx_remove(tree) || fx_run(f, copy, repo, NULL, NULL))
        return 0;
    int ok = files_under(tree) == 9;
    for (size_t i = 0; carried[i]; i++)
        ok &= fx_same(bp(tree, carried[i]), bp(repo, carried[i]));
    return ok;
}

/**
 * Put the step's key in `repo` over make's sources in `key`, 65 bytes.
 * Return 1, or 0 on failure.
 */
static int step_key(struct fixture *f, const char *repo, char key[65])
{
    const char *make[] = {"/usr/bin/make", "-s",    "-f", "mk/gnu-host-checks.mk", "-f",
                          "print.mk",      "print", NULL};
    if (fx_run(f, make, repo, NULL, NULL) || !f->out)
        return 0;
    char *names = strdup(f->out), *save = NULL;
    const char *argv[64];
    size_t n = 0;
    argv[n++] = product_path(f, "etude");
    argv[n++] = "build";
    argv[n++] = "-p";
    argv[n++] = "-n";
    argv[n++] = "gnu-host-checks";
    argv[n++] = "-t";
    argv[n++] = "host-checks.stamp";
    argv[n++] = "-a";
    argv[n++] = "x86_64";
    for (char *name = strtok_r(names, "\n", &save); name && n < 56;
         name = strtok_r(NULL, "\n", &save)) {
        argv[n++] = "-i";
        argv[n++] = name;
    }
    argv[n++] = "-o";
    argv[n++] = "host-checks";
    argv[n++] = "--";
    argv[n++] = "true";
    argv[n] = NULL;
    int ok = fx_run(f, argv, repo, NULL, NULL) == 0 && f->out;
    const char *line = ok ? strstr(f->out, "\nkey ") : NULL;
    ok = line && strlen(line + 5) >= 64;
    if (ok) {
        memcpy(key, line + 5, 64);
        key[64] = 0;
    }
    free(names);
    return ok;
}

/** Whether a change to each file of `names` changes the key, as `changes` says;
 * each file has its own bytes again afterwards. */
static int changes_key(struct fixture *f, const char *repo, const char *const names[], int changes,
                       const char *base)
{
    int ok = 1;
    for (size_t i = 0; ok && names[i]; i++) {
        char key[65], again[65], *text = fx_read(bp(repo, names[i]));
        size_t n = text ? strlen(text) : 0;
        char *changed = text ? malloc(n + 9) : NULL;
        if (changed)
            snprintf(changed, n + 9, "%schanged\n", text);
        int moved = changed && fx_write(bp(repo, names[i]), changed, n + 8, 0644) &&
                    step_key(f, repo, key) && (strcmp(key, base) != 0) == changes;
        int back = text && fx_write(bp(repo, names[i]), text, n, 0644) &&
                   step_key(f, repo, again) && !strcmp(again, base);
        ok = moved && back;
        if (!ok)
            fprintf(stderr, "host-build-check: key row: %s\n", names[i]);
        free(changed);
        free(text);
    }
    return ok;
}

/**
 * Remove the fixture's git checkout and list its files in
 * build/gnu-source-files, as in the tooling machine, which has no git.
 * Return 1, or 0 on failure.
 */
static int source_files(const char *repo)
{
    char list[8192];
    size_t used = 0;
    const char *const *groups[] = {carried, outside, scripts};
    for (size_t g = 0; g < 3; g++)
        for (size_t i = 0; groups[g][i]; i++) {
            size_t n = strlen(groups[g][i]) + 1;
            if (used + n > sizeof(list))
                return 0;
            memcpy(list + used, groups[g][i], n);
            used += n;
        }
    return fx_remove(bp(repo, ".git")) &&
           fx_write(bp(repo, "build/gnu-source-files"), list, used, 0644);
}

/**
 * Check the GNU host checks' step in a fixture tree, with git and without:
 * make's sources are the list and its scripts, the tree carries the listed
 * files alone, and the key follows those files and no others.
 * Return 1 when every row passes.
 */
int build_host_checks(struct fixture *f)
{
    char repo[4096];
    snprintf(repo, sizeof(repo), "%.4000s/repo", f->work);
    const char *init[] = {"git", "init", "-q", NULL}, *add[] = {"git", "add", "-A", NULL};
    char base[65];
    fx_env("GIT_CEILING_DIRECTORIES", f->work);
    if (!repo_tree(f, repo) || fx_run(f, init, repo, NULL, NULL) ||
        fx_run(f, add, repo, NULL, NULL))
        return 0;
    fx_check(f, sources_are_list(f, repo), "the key's files are the list and its two scripts");
    fx_check(f, tree_is_list(f, repo), "the tree carries the files of the list and no others");
    if (!step_key(f, repo, base))
        return 0;
    fx_check(f, changes_key(f, repo, carried, 1, base),
             "a change to each file the tree carries changes the key");
    fx_check(f, changes_key(f, repo, scripts, 1, base),
             "a change to either script changes the key");
    fx_check(f, changes_key(f, repo, outside, 0, base),
             "a change to each file outside the tree keeps the key");
    if (!source_files(repo))
        return 0;
    fx_check(f, sources_are_list(f, repo), "without git the key's files are the same list");
    fx_check(f, tree_is_list(f, repo), "without git the tree carries the same files");
    char again[65];
    fx_check(f, step_key(f, repo, again) && !strcmp(again, base),
             "without git the key is the same");
    return !f->failed;
}
