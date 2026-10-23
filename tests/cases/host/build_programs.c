/* Exercise launch scripts and the commit hook through their product entries. */
#include "build_support.h"
#include "build_rows.h"

int build_commit_msg(struct fixture *f)
{
    const char *hook = bp(f->work, "commit-msg"), *msg = bp(f->work, "msg");
    if (!fx_copy(bp(f->tree, "tools/git-hooks/commit-msg"), hook))
        return 0;
    for (size_t i = 0; i < sizeof(message_rows) / sizeof(*message_rows); i++) {
        const struct message_row *r = &message_rows[i];
        fx_check(f, fx_text(msg, r->text), "commit message input");
        const char *args[] = {hook, msg, NULL};
        int status = fx_run(f, args, f->work, NULL, NULL);
        fx_check(f,
                 status == r->status &&
                     (!status ||
                      (f->err && strstr(f->err, "commit-msg: refused: attribution on line "))),
                 r->name);
    }
    return !f->failed;
}

static char *expand(struct fixture *f, const char *text)
{
    char *out = calloc(1, 65536);
    size_t used = 0;
    if (!out)
        abort();
    for (const char *p = text ? text : ""; *p;) {
        const char *part = !strncmp(p, "$W", 2) ? f->work : NULL;
        size_t n = part ? strlen(part) : 1;
        if (used + n >= 65536)
            abort();
        memcpy(out + used, part ? part : p, n);
        used += n;
        p += part ? 2 : 1;
    }
    return out;
}

static void run_settings(struct fixture *f, const char *settings)
{
    const char *clear[] = {"ARCH",         "CPU",          "NET",     "NIC",
                           "NET_HOSTFWD",  "DISK",         "DISKBUS", "DISKNOMOUNT",
                           "RESCUE",       "AUDIO_DEVICE", "DISPLAY", "WAYLAND_DISPLAY",
                           "DISPLAY_MODE", "UNAME_S",      NULL};
    for (size_t i = 0; clear[i]; i++)
        fx_env(clear[i], NULL);
    char *text = expand(f, settings), *save, *line = strtok_r(text, "\n", &save);
    for (; line; line = strtok_r(NULL, "\n", &save)) {
        char *value = strchr(line, '=');
        if (!value)
            abort();
        *value++ = 0;
        fx_env(line, value);
    }
    free(text);
}

static int arguments_match(struct fixture *f, const struct run_row *r, const char *path)
{
    char *want = expand(f, r->wanted), *actual = fx_read(path), *save;
    int ok = fx_exists(path) && actual != NULL;
    for (char *line = strtok_r(want, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        size_t n = strlen(line);
        int found = 0;
        for (const char *p = actual ? actual : ""; *p;) {
            const char *end = strchr(p, '\n');
            size_t z = end ? (size_t)(end - p) : strlen(p);
            found |= z == n && !memcmp(p, line, n);
            p += z + (end != NULL);
        }
        ok &= found;
    }
    if (r->absent && actual && strstr(actual, r->absent))
        ok = 0;
    free(want);
    free(actual);
    return ok;
}

static int run_tree(struct fixture *f)
{
    const char *scripts[] = {"run.sh",   "run_from_disk.sh", "run_from_iso.sh",
                             "debug.sh", "qemu_devices.sh",  NULL};
    const char *files[] = {"build/rach2/kernel",
                           "build/x86_64/rach2/kernel",
                           "build/images/test.krffs",
                           "build/x86_64/images/test.krffs",
                           "build/images/os-real-grub.disk",
                           "build/images/test.iso",
                           "disk.img",
                           NULL};
    for (size_t i = 0; scripts[i]; i++)
        if (!fx_copy(bp(bp(f->tree, "rach2"), scripts[i]), bp(bp(f->work, "rach2"), scripts[i])))
            return 0;
    for (size_t i = 0; files[i]; i++)
        if (!fx_text(bp(f->work, files[i]), ""))
            return 0;
    const char *mocks[] = {"qemu-system-i386", "qemu-system-x86_64", "uname", "x86_64-elf-gdb",
                           NULL};
    for (size_t i = 0; mocks[i]; i++)
        if (!fx_mock(f, mocks[i]))
            return 0;
    return 1;
}

int build_run_scripts(struct fixture *f)
{
    if (!run_tree(f))
        return 0;
    const char *output = bp(f->work, "arguments");
    fx_env("ARGS_OUT", output);
    for (size_t i = 0; i < sizeof(run_rows) / sizeof(*run_rows); i++) {
        const struct run_row *r = &run_rows[i];
        run_settings(f, r->settings);
        unlink(output);
        const char *args[] = {"/bin/sh", bp(bp(f->work, "rach2"), r->script), r->argument, r->extra,
                              NULL};
        int status = fx_run(f, args, f->work, NULL, NULL);
        int ok = status == r->status;
        if (r->diagnostic)
            ok &= !fx_exists(output) && output_has(f, r->diagnostic);
        else
            ok &= arguments_match(f, r, output);
        fx_check(f, ok, r->name);
    }
    return !f->failed;
}
