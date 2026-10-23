/* flow_space.c: disk admission and preserved Windows disk paths. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char *ev(const char *name)
{
    const char *v = getenv(name);
    return v ? v : "";
}
int flow_space_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "timeout")) {
        if (argc < 5 || strcmp(argv[1], "-k") || strcmp(argv[2], "5") || strcmp(argv[3], "60"))
            return 91;
        if (!strcmp(ev(!strcmp(argv[4], "df") ? "DRIVE" : "QUERY"), "timeout"))
            return 124;
        execvp(argv[4], argv + 4);
        return 127;
    }
    if (!strcmp(name, "df")) {
        printf("Filesystem 1024-blocks Used Available Capacity Mounted on\n");
        if (argc > 2 && !strncmp(argv[2], ev("RACH2_WSL_MOUNTS"), strlen(ev("RACH2_WSL_MOUNTS")))) {
            if (!fx_exists(argv[2]))
                return 1;
            printf("%s 999999999 0 %s 0%% %s/d\n", ev("WINDOWS_FS"), ev("WINDOWS_FREE"),
                   ev("RACH2_WSL_MOUNTS"));
        } else {
            if (!strcmp(ev("LINUX_FREE"), "unknown"))
                return 1;
            printf("fixture 999999999 0 %s 0%% /\n", ev("LINUX_FREE"));
        }
        return 0;
    }
    if (!strcmp(name, "powershell.exe")) {
        const char *query = ev("QUERY");
        char pwd[4096], input;
        if (!getcwd(pwd, sizeof(pwd)) || strcmp(pwd, "/") || strcmp(ev("RACH2_DISTRO"), "fixture"))
            return 92;
        if (strcmp(ev("WSLENV"), "KEPT/u:RACH2_DISTRO"))
            return 93;
        if (argc != 5 || strcmp(argv[1], "-NoProfile") || strcmp(argv[2], "-NonInteractive") ||
            strcmp(argv[3], "-Command"))
            return 94;
        if (read(0, &input, 1) > 0)
            return 95;
        if (!fx_text(ev("SPACE_LOG"), query))
            return 96;
        if (!strcmp(query, "good"))
            printf("D:\\WSL\\fixture\\ext4.vhdx\r\n");
        else if (!strcmp(query, "lower"))
            printf("d:\\WSL\\fixture\\ext4.vhdx\n");
        else if (!strcmp(query, "absent"))
            printf("D:\\WSL\\moved\\ext4.vhdx\n");
        else if (!strcmp(query, "empty"))
            return 0;
        else if (!strcmp(query, "root"))
            printf("\\\\server\\share\\ext4.vhdx\n");
        else if (!strcmp(query, "duplicate"))
            printf("D:\\WSL\\fixture\\ext4.vhdx\nD:\\WSL\\fixture\\ext4.vhdx\n");
        else if (!strcmp(query, "noise"))
            printf("error\nD:\\WSL\\fixture\\ext4.vhdx\n");
        else if (!strcmp(query, "partial")) {
            printf("D:\\WSL\\fixture\\ext4.vhdx\n");
            return 1;
        } else if (!strcmp(query, "failed"))
            return 1;
        else
            return 96;
        return 0;
    }
    return 97;
}
int flow_space(struct fixture *f)
{
    const char *rows[] = {"linux",         "disabled",      "linux-low",     "linux-unknown",
                          "missing",       "good",          "lower",         "edge",
                          "low",           "cached",        "cached-low",    "stale",
                          "stale-timeout", "timeout",       "failed",        "partial",
                          "empty",         "root",          "duplicate",     "noise",
                          "absent",        "drive-timeout", "drive-foreign", "drive-unknown"};
    const char *tools[] = {"awk", "mkdir", "mv", "rm", "tr", NULL};
    char windows[4096], path[9000], file[4096];
    unsigned i;
    int ok = 1;
    for (i = 0; tools[i]; i++) {
        snprintf(file, sizeof(file), "/usr/bin/%s", tools[i]);
        if (symlink(file, flow_path(f, "bin/tmp")))
            return 0;
        if (rename(flow_path(f, "bin/tmp"), flow_path(f, tools[i])))
            return 0;
    }
    /* Keep actual utilities in bin, beside the native command mocks. */
    for (i = 0; tools[i]; i++) {
        snprintf(file, sizeof(file), "%s/%s", f->bin, tools[i]);
        if (rename(flow_path(f, tools[i]), file))
            return 0;
    }
    if (!fx_mock(f, "df") || !fx_mock(f, "timeout"))
        return 0;
    snprintf(windows, sizeof(windows), "%s/windows", f->work);
    fx_mkdir(windows);
    snprintf(file, sizeof(file), "%s/powershell.exe", windows);
    if (!fx_copy(f->self, file))
        return 0;
    if (!flow_put(f, "mnt/d/WSL/fixture/ext4.vhdx", "") ||
        !flow_put(f, "input", "input must stay unread\n"))
        return 0;
    fx_env("FLOW_SPACE", "1");
    for (i = 0; i < 24; i++) {
        const char *row = rows[i], *floor = "40", *want_text = "", *kept = "", *query = "";
        int want = 0, linux_only = 0;
        fx_env("LINUX_FREE", "104857600");
        fx_env("WINDOWS_FREE", "42991616");
        fx_env("WINDOWS_FS", "D:\\");
        fx_env("QUERY", "good");
        fx_env("DRIVE", "good");
        fx_env("WSL_DISTRO_NAME", "fixture");
        fx_env("WSLENV", "KEPT/u");
        fx_env("SPACE_LOG", flow_path(f, "query"));
        fx_env("XDG_CACHE_HOME", flow_path(f, "cache"));
        fx_env("RACH2_WSL_MOUNTS", flow_path(f, "mnt"));
        if (!strcmp(row, "linux")) {
            fx_env("WSL_DISTRO_NAME", "");
            linux_only = 1;
        } else if (!strcmp(row, "disabled")) {
            floor = "0";
            fx_env("QUERY", "timeout");
            fx_env("LINUX_FREE", "unknown");
            linux_only = 1;
        } else if (!strcmp(row, "linux-low")) {
            fx_env("LINUX_FREE", "41943039");
            want = 1;
            want_text = "39 GiB free under";
            linux_only = 1;
        } else if (!strcmp(row, "linux-unknown")) {
            fx_env("LINUX_FREE", "unknown");
            want = 1;
            want_text = "unknown GiB free under";
            linux_only = 1;
        } else if (!strcmp(row, "missing")) {
            want = 1;
            want_text = "powershell.exe is missing";
        } else if (!strcmp(row, "good"))
            query = "good";
        else if (!strcmp(row, "lower")) {
            fx_env("QUERY", "lower");
            query = "lower";
        } else if (!strcmp(row, "edge")) {
            fx_env("WINDOWS_FREE", "41943040");
            query = "good";
        } else if (!strcmp(row, "low")) {
            fx_env("WINDOWS_FREE", "41943039");
            query = "good";
            want = 1;
            want_text = "39 GiB free on D:";
        } else if (!strcmp(row, "cached") || !strcmp(row, "cached-low")) {
            kept = "D:\\WSL\\fixture\\ext4.vhdx";
            fx_env("QUERY", "timeout");
            if (!strcmp(row, "cached-low")) {
                fx_env("WINDOWS_FREE", "41943039");
                want = 1;
                want_text = "39 GiB free on D:";
            }
        } else if (!strcmp(row, "stale") || !strcmp(row, "stale-timeout")) {
            kept = "D:\\WSL\\moved\\ext4.vhdx";
            if (!strcmp(row, "stale"))
                query = "good";
            else {
                fx_env("QUERY", "timeout");
                want = 1;
                want_text = "PowerShell status 124";
            }
        } else if (!strcmp(row, "timeout")) {
            fx_env("QUERY", "timeout");
            want = 1;
            want_text = "PowerShell status 124";
        } else if (!strcmp(row, "failed") || !strcmp(row, "partial")) {
            fx_env("QUERY", row);
            want = 1;
            want_text = "PowerShell status 1";
        } else if (!strncmp(row, "drive-", 6)) {
            want = 1;
            want_text = "no valid free-space reading";
            if (!strcmp(row, "drive-timeout"))
                fx_env("DRIVE", "timeout");
            else if (!strcmp(row, "drive-foreign"))
                fx_env("WINDOWS_FS", "fixture");
            else
                fx_env("WINDOWS_FREE", "unknown");
        } else {
            fx_env("QUERY", row);
            want = 1;
            want_text = "no valid free-space reading";
        }
        fx_remove(flow_path(f, "query"));
        fx_remove(flow_path(f, "cache"));
        if (*kept) {
            char line[4096];
            snprintf(line, sizeof(line), "%s\n", kept);
            flow_put(f, "cache/rach2/wsl-disk.fixture", line);
        }
        if (!strcmp(row, "missing"))
            snprintf(path, sizeof(path), "%s", f->bin);
        else
            snprintf(path, sizeof(path), "%s:%s", windows, f->bin);
        fx_env("PATH", path);
        {
            const char *a[] = {
                "/bin/sh", flow_source(f, "tools/setup/space.sh"), "run", "a run", floor, f->work,
                NULL};
            fx_run(f, a, f->work, NULL, flow_path(f, "input"));
        }
        ok = fx_check(f, f->status == want, row) && ok;
        if (*query)
            ok = fx_check(f, fx_equal(flow_path(f, "query"), query), "Windows query") && ok;
        else if (!want)
            ok = fx_check(f, !fx_exists(flow_path(f, "query")), "no query") && ok;
        if (!want)
            ok = fx_check(f, !*f->out && !*f->err, "quiet admission") && ok;
        else
            ok = fx_check(f,
                          (strstr(f->out, "run: result: refused: ") ||
                           strstr(f->err, "run: result: refused: ")) &&
                              (strstr(f->out, want_text) || strstr(f->err, want_text)),
                          "refusal diagnostic") &&
                 ok;
        if (linux_only)
            continue;
        if (*query)
            kept = !strcmp(query, "lower") ? "d:\\WSL\\fixture\\ext4.vhdx"
                                           : "D:\\WSL\\fixture\\ext4.vhdx";
        if (*kept) {
            char line[4096];
            snprintf(line, sizeof(line), "%s\n", kept);
            ok = fx_check(f,
                          fx_equal(flow_path(f, "cache/rach2/wsl-disk.fixture"), line) &&
                              fx_count(flow_path(f, "cache/rach2")) == 1,
                          "kept Windows path") &&
                 ok;
        } else
            ok = fx_check(f, !fx_exists(flow_path(f, "cache/rach2/wsl-disk.fixture")),
                          "no cached Windows path") &&
                 ok;
    }
    fx_env("FLOW_SPACE", NULL);
    return ok;
}
