/* flow_basic.c: usage limits and the agent workload's credential commands. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include "sha256.h"
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int guest(struct fixture *f, const char *command, const char *mode, const char *agent,
                 const char *input)
{
    const char *a[] = {"/bin/bash", flow_source(f, "tests/system/accept/agents_guest.sh"),
                       command,     mode,
                       agent,       NULL};
    return fx_run(f, a, f->work, NULL, input);
}
static int text_input(struct fixture *f, const char *text)
{
    return flow_put(f, "input", text);
}
static int limited(struct fixture *f)
{
    const char *msg = "You've hit your session limit \302\267 resets 9:20am (Asia/Almaty)",
               *codex = "You've hit your usage limit. Try again at 3:05 PM.";
    const char *rows[] = {"claude-limit", "claude-limit-trailing", "codex-err",
                          "codex-event",  "claude-quote",          "claude-quote-keys",
                          "claude-tool",  "codex-quote",           "claude-earlier",
                          "empty"};
    char out[5000], err[1000], command[9000], want[1000], quote[1000];
    unsigned i;
    int ok = 1;
    snprintf(quote, sizeof(quote), "The loop logged %s after a session that ended 0.", msg);
    snprintf(command, sizeof(command), ". '%s'; limited '%s' '%s'",
             flow_source(f, "tools/flow/lib.sh"), flow_path(f, "out"), flow_path(f, "err"));
    for (i = 0; i < 10; i++) {
        out[0] = err[0] = want[0] = 0;
        if (i == 0)
            snprintf(out, sizeof(out),
                     "{\"type\":\"assistant\",\"message\":{\"model\":\"<synthetic>\",\"content\":[{"
                     "\"type\":\"text\",\"text\":\"%s\"}]},\"error\":\"rate_limit\"}\n{\"type\":"
                     "\"result\",\"subtype\":\"success\",\"is_error\":true,\"terminal_reason\":"
                     "\"api_error\",\"api_error\":\"usage_limit_reached\",\"result\":\"%s\"}\n",
                     msg, msg);
        if (i == 1)
            snprintf(out, sizeof(out),
                     "{\"type\":\"system\",\"subtype\":\"init\"}\n{\"type\":\"result\",\"subtype\":"
                     "\"success\",\"is_error\":true,\"terminal_reason\":\"api_error\",\"api_"
                     "error\":\"usage_limit_reached\",\"result\":\"%s\"}\n{\"type\":\"command_"
                     "lifecycle\",\"state\":\"cancelled\"}\n",
                     msg);
        if (i == 2) {
            strcpy(out, "{\"type\":\"thread.started\"}\n{\"type\":\"turn.started\"}\n");
            snprintf(err, sizeof(err), "ERROR: %s\n", codex);
        }
        if (i == 3)
            snprintf(out, sizeof(out),
                     "{\"type\":\"turn.started\"}\n{\"type\":\"turn.failed\",\"error\":{"
                     "\"message\":\"%s\"}}\n",
                     codex);
        if (i == 4)
            snprintf(out, sizeof(out),
                     "{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\","
                     "\"text\":\"%s\"}]}}\n{\"type\":\"result\",\"subtype\":\"success\",\"is_"
                     "error\":false,\"terminal_reason\":\"completed\",\"result\":\"%s\"}\n",
                     quote, quote);
        if (i == 5)
            snprintf(out, sizeof(out),
                     "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"terminal_"
                     "reason\":\"completed\",\"result\":\"The line had \\\"is_error\\\":true and "
                     "\\\"type\\\":\\\"error\\\": %s\"}\n",
                     quote);
        if (i == 6)
            snprintf(out, sizeof(out),
                     "{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"is_"
                     "error\":true,\"content\":\"%s\"}]}}\n{\"type\":\"assistant\",\"message\":{"
                     "\"content\":[{\"type\":\"text\",\"text\":\"Reading the log.\"}]}}\n",
                     quote);
        if (i == 7)
            snprintf(out, sizeof(out),
                     "{\"type\":\"item.completed\",\"item\":{\"type\":\"agent_message\",\"text\":"
                     "\"%s\"}}\n{\"type\":\"turn.completed\"}\n",
                     quote);
        if (i == 8)
            snprintf(out, sizeof(out),
                     "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":true,\"terminal_"
                     "reason\":\"api_error\",\"result\":\"%s\"}\n{\"type\":\"assistant\","
                     "\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Done.\"}]}}\n{"
                     "\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"terminal_"
                     "reason\":\"completed\",\"result\":\"Done.\"}\n",
                     msg);
        if (i < 2)
            snprintf(want, sizeof(want),
                     "hit your session limit \302\267 resets 9:20am (Asia/Almaty)\n");
        else if (i < 4)
            snprintf(want, sizeof(want), "hit your usage limit. Try again at 3:05 PM.\n");
        if (!flow_put(f, "out", out) || !flow_put(f, "err", err))
            return 0;
        fx_sh(f, command);
        ok =
            fx_check(f, f->status == (i < 4 ? 0 : 1) && f->out && !strcmp(f->out, want), rows[i]) &&
            ok;
    }
    return ok;
}
static const char *credential_paths[] = {".claude/.credentials.json", ".codex/auth.json",
                                         ".gemini/antigravity-cli/antigravity-oauth-token",
                                         ".config/muse/auth.json"};
static const char *agents[] = {"claude", "codex", "agy", "muse"};
static int credential_file(struct fixture *f, const char *path, const char *text)
{
    char file[4096];
    snprintf(file, sizeof(file), "%s/home/%s", f->work, path);
    return fx_text(file, text);
}
static int credentials(struct fixture *f)
{
    const char *dummy = "rach2-dummy-antigravity-token-00000000",
               *gh = "gho_dummy_github_token_00000000";
    char file[4096], token[200], line[5000], digest[65], path[300];
    unsigned mode, i, j;
    int ok = 1;
    struct sha256 hash;
    unsigned char bytes[32];
    sha256_init(&hash);
    sha256_update(&hash, (const unsigned char *)dummy, strlen(dummy));
    sha256_final(&hash, bytes);
    for (i = 0; i < 32; i++)
        snprintf(digest + i * 2, 3, "%02x", bytes[i]);
    if (!credential_file(f, credential_paths[2], dummy))
        return 0;
    guest(f, "credentials", "lenovo", NULL, NULL);
    snprintf(file, sizeof(file), "%s/home/%s", f->work, credential_paths[2]);
    snprintf(line, sizeof(line), "%s %s\n", file, digest);
    ok = fx_check(f, !f->status && strstr(f->out, line), "antigravity digest") && ok;
    unlink(file);
    guest(f, "credentials", "lenovo", NULL, NULL);
    snprintf(line, sizeof(line), "%s missing\n", file);
    ok = fx_check(f, !f->status && strstr(f->out, line), "antigravity absence") && ok;
    for (mode = 0; mode < 2; mode++) {
        const char *kind = mode ? "qemu" : "lenovo";
        for (i = 0; i < 4; i++) {
            snprintf(path, sizeof(path), mode ? "rach2-accept/keys/%s" : "%s",
                     mode ? agents[i] : credential_paths[i]);
            snprintf(token, sizeof(token), "dummy_%s_token_00000000000000", agents[i]);
            if (!credential_file(f, path, token) || !credential_file(f, ".config/gh/hosts.yml", gh))
                return 0;
            guest(f, "credentials", kind, agents[i], NULL);
            snprintf(file, sizeof(file), "%s/home/%s", f->work, path);
            ok = fx_check(f,
                          !f->status && !strstr(f->out, " missing") && strstr(f->out, file) &&
                              strstr(f->out, "/.config/gh/hosts.yml ") &&
                              fx_regex(f->out, "^[^\n]+\n[^\n]+\n$"),
                          "selected files") &&
                 ok;
            unlink(file);
            guest(f, "credentials", kind, agents[i], NULL);
            snprintf(line, sizeof(line), "%s missing\n", file);
            ok = fx_check(f, !f->status && strstr(f->out, line), "selected missing token") && ok;
            snprintf(file, sizeof(file), "%s/home/.config/gh/hosts.yml", f->work);
            unlink(file);
            guest(f, "credentials", kind, agents[i], NULL);
            ok = fx_check(f, !f->status && strstr(f->out, "/.config/gh/hosts.yml missing\n"),
                          "selected missing github") &&
                 ok;
        }
        for (i = 0; i < 4; i++) {
            snprintf(path, sizeof(path), mode ? "rach2-accept/keys/%s" : "%s",
                     mode ? agents[i] : credential_paths[i]);
            snprintf(token, sizeof(token), "dummy_%s_token_00000000000000", agents[i]);
            if (!credential_file(f, path, token))
                return 0;
        }
        if (!credential_file(f, ".config/gh/hosts.yml", gh))
            return 0;
        for (i = 0; i < 4; i++) {
            text_input(f, "clean");
            guest(f, "secrets", kind, agents[i], flow_path(f, "input"));
            ok = fx_check(f, !f->status && !strcmp(f->out, "0 of 2 strings\n"), "selected clean") &&
                 ok;
            guest(f, "filtered", kind, agents[i], NULL);
            ok = fx_check(f, !f->status && !strcmp(f->out, "0 of 2 strings\n"),
                          "selected filtered") &&
                 ok;
            for (j = 0; j < 3; j++) {
                if (j == 0)
                    snprintf(token, sizeof(token), "dummy_%s_token_00000000000000", agents[i]);
                if (j == 1)
                    snprintf(token, sizeof(token), "%s", gh);
                if (j == 2)
                    snprintf(token, sizeof(token), "dummy_%s_token_00000000000000",
                             agents[(i + 1) % 4]);
                text_input(f, token);
                guest(f, "secrets", kind, agents[i], flow_path(f, "input"));
                ok = fx_check(f,
                              !f->status &&
                                  !strcmp(f->out, j == 2 ? "0 of 2 strings\n" : "1 of 2 strings\n"),
                              "selected scan") &&
                     ok;
            }
        }
        text_input(f, "clean");
        guest(f, "secrets", kind, NULL, flow_path(f, "input"));
        ok =
            fx_check(f, !f->status && !strcmp(f->out, "0 of 5 strings\n"), "all credentials") && ok;
        {
            const char *a[] = {"/bin/bash", flow_source(f, "tests/system/accept/agents_guest.sh"),
                               "secrets",   kind,
                               "claude",    "agy",
                               NULL};
            fx_run(f, a, f->work, NULL, flow_path(f, "input"));
            ok = fx_check(f, !f->status && !strcmp(f->out, "0 of 3 strings\n"), "selected pair") &&
                 ok;
        }
    }
    /* The original single-token rows also verify redact's exact output. */
    fx_remove(flow_path(f, "home"));
    fx_mkdir(flow_path(f, "home"));
    for (mode = 0; mode < 2; mode++) {
        const char *kind = mode ? "qemu" : "lenovo";
        const char *p = mode ? "rach2-accept/keys/agy" : credential_paths[2];
        if (!credential_file(f, p, dummy))
            return 0;
        text_input(f, "clean");
        guest(f, "secrets", kind, NULL, flow_path(f, "input"));
        ok = fx_check(f, !f->status && !strcmp(f->out, "0 of 1 strings\n"), "single clean") && ok;
        text_input(f, dummy);
        guest(f, "secrets", kind, NULL, flow_path(f, "input"));
        ok = fx_check(f, !f->status && !strcmp(f->out, "1 of 1 strings\n"), "single scan") && ok;
        guest(f, "filtered", kind, "agy", NULL);
        ok =
            fx_check(f, !f->status && !strcmp(f->out, "0 of 1 strings\n"), "single filtered") && ok;
        guest(f, "redact", kind, NULL, flow_path(f, "input"));
        ok = fx_check(f, !f->status && !strcmp(f->out, "[credential]\n"), "redact exact") && ok;
        snprintf(file, sizeof(file), "%s/home/%s", f->work, p);
        unlink(file);
    }
    return ok;
}
int flow_basic_mock(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 97;
}
int flow_basic(struct fixture *f, const char *id)
{
    if (!strcmp(id, "agents_guest"))
        return flow_agents_guest(f);
    if (!strcmp(id, "limited"))
        return limited(f);
    if (!strcmp(id, "agents_credentials"))
        return credentials(f) && flow_credential_arguments(f);
    fprintf(stderr, "fixture: no controller for %s\n", id);
    return 0;
}
