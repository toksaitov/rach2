/* agent_accept.c: SSH sessions, terminal changes, persistent sign-in. */
#include "agent_accept.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
struct acceptance {
    struct net_case *u;
    const char *work, *agent, *ready, *busy, *loading, *mark, *quit, *after_cancel, *signin;
    char assets[4096], key[4096], root_key[4096], port[20], log[4096], target[100];
    char *screen;
    unsigned output;
    int model_selected, window;
};

static struct acceptance state;
static void pause_read(void)
{
#ifdef _WIN32
    Sleep(2000);
#else
    struct timespec t = {2, 0};
    /* interval: poll the agent pane every two seconds */
    nanosleep(&t, NULL);
#endif
}
static int save_file(const char *name, const char *text)
{
    FILE *f = fopen(name, "wb");
    int ok;
    if (!f)
        return 0;
    ok = fputs(text, f) >= 0;
    if (fclose(f))
        ok = 0;
    return ok;
}
/* Compose one path only when every fragment and the terminator fit. */
static int accept_path(char *out, size_t capacity, const char *const parts[])
{
    size_t used = 0, i, n;
    if (!capacity)
        return 0;
    out[0] = 0;
    for (i = 0; parts[i]; i++) {
        n = strlen(parts[i]);
        if (n >= capacity - used) {
            out[0] = 0;
            fprintf(stderr, "agents-accept: path exceeds %lu bytes\n", (unsigned long)capacity);
            return 0;
        }
        memcpy(out + used, parts[i], n);
        used += n;
    }
    out[used] = 0;
    return 1;
}

static void recorded(struct acceptance *s, const char *label, const char *text)
{
    FILE *f = fopen(s->log, "ab");
    if (!f) {
        net_fail(s->u, "the acceptance log could not be written");
        return;
    }
    fprintf(f, "agents-accept: %s:\n%s\n", label, text);
    fclose(f);
}
static void said(struct acceptance *s, const char *text)
{
    printf("agents-accept: %s: %s\n", s->agent, text);
    recorded(s, s->agent, text);
}
static int host(struct acceptance *s, char *const args[], const char *label)
{
    struct net_run r;
    int ok;
    if (net_host(s->u, args, label, &r) != ETUDE_OK)
        return 0;
    ok = net_exited0(&r);
    fputs(r.out, stdout);
    fputs(r.err, stderr);
    net_run_free(&r);
    return ok;
}
static int remote(struct acceptance *s, int root, const char *command, const char *input,
                  struct net_run *r)
{
    char label[100];
    char *args[] = {NET_SSH, NET_SSH_KEY(root ? s->root_key : s->key),   "-p",
                    s->port, root ? "root@127.0.0.1" : "test@127.0.0.1", (char *)command,
                    NULL};
    snprintf(label, sizeof(label), "accept-ssh.%u", ++s->output);
    if (net_ssh_input(s->u, args, input, label, r) != ETUDE_OK)
        return 0;
    recorded(s, command, r->out);
    return 1;
}
static int remote_ok(struct acceptance *s, int root, const char *command, const char *input)
{
    struct net_run r;
    int ok;
    if (!remote(s, root, command, input, &r))
        return 0;
    ok = net_exited0(&r);
    if (!ok)
        fprintf(stderr, "agents-accept: %s: %s", command, r.err);
    net_run_free(&r);
    return ok;
}
static char *guest(struct acceptance *s, const char *verb, const char *extra)
{
    char command[1500];
    struct net_run r;
    char *text;
    snprintf(command, sizeof(command), "bash ~/rach2-accept/guest.sh %s %s%s%s", verb,
             extra ? "" : s->agent, extra ? extra : "", "");
    if (!remote(s, 0, command, NULL, &r))
        return NULL;
    if (!net_exited0(&r)) {
        net_run_free(&r);
        return NULL;
    }
    text = r.out;
    free(r.err);
    return text;
}
static char *client(struct acceptance *s, const char *const args[])
{
    char label[100], error[110];
    struct etude_guest_result r;
    char *text;
    snprintf(label, sizeof(label), "accept-client.%u", ++s->output);
    if (etude_guest_command(s->u->client, args, NULL, NULL, label, &r) != ETUDE_OK ||
        r.termination.end != ETUDE_END_EXITED || r.termination.value != 0) {
        snprintf(error, sizeof(error), "%s.err", label);
        text = net_file(etude_evidence(s->u->c, error));
        fprintf(stderr, "agents-accept: client operation failed: %s\n", text);
        free(text);
        return NULL;
    }
    return net_file(etude_evidence(s->u->c, label));
}
static int tmux(struct acceptance *s, const char *const extra[])
{
    const char *args[12] = {"/usr/bin/tmux", "-L", "etude-agent-accept"};
    unsigned i = 0;
    char *text;
    int ok;
    while (i < 8 && extra[i]) {
        args[i + 3] = extra[i];
        i++;
    }
    args[i + 3] = NULL;
    text = client(s, args);
    ok = text != NULL;
    free(text);
    return ok;
}

static int keys(struct acceptance *s, int literal, const char *key)
{
    const char *args[] = {"/usr/bin/tmux",
                          "-L",
                          "etude-agent-accept",
                          "send-keys",
                          "-t",
                          s->target,
                          literal ? "-l" : key,
                          literal ? key : NULL,
                          NULL};
    char *text = client(s, args);
    int ok = text != NULL;
    free(text);
    return ok;
}
static char *get_screen(struct acceptance *s, int all)
{
    char extra[100], *screen;
    snprintf(extra, sizeof(extra), "%s all", s->agent);
    screen = guest(s, "screen", all ? extra : NULL);
    if (!screen)
        return NULL;
    if (!all) {
        free(s->screen);
        s->screen = screen;
    }
    return screen;
}
static int shows(struct acceptance *s, const char *text)
{
    return s->screen && strstr(s->screen, text) != NULL;
}
static int settle(struct acceptance *s, const char *key, const char *wanted, const char *question)
{
    char *last = strdup(""), *pressed = strdup("");
    int drawn = 1, shown = 0, result = 0;
    while (net_ok(s->u) && get_screen(s, 0)) {
        if (strcmp(s->screen, pressed))
            drawn = 1;
        if (question && !shows(s, question)) {
            result = 2;
            break;
        }
        if (shows(s, wanted)) {
            if (shown++) {
                result = 1;
                break;
            }
        } else {
            shown = 0;
            if (!strcmp(s->screen, last) && drawn) {
                if (!keys(s, 0, key))
                    break;
                free(pressed);
                pressed = strdup(s->screen);
                drawn = 0;
            }
        }
        free(last);
        last = strdup(s->screen);
        pause_read();
    }
    free(last);
    free(pressed);
    return result;
}
static int choose(struct acceptance *s, const char *answer, const char *key)
{
    char wanted[300];
    int result;
    snprintf(wanted, sizeof(wanted), "%s %s", s->mark, answer);
    result = settle(s, key, wanted, answer);
    return result == 2 || (result == 1 && keys(s, 0, "Enter"));
}
static int leave(struct acceptance *s, const char *question)
{
    char *last = strdup(""), *pressed = strdup("");
    int drawn = 1, ok = 0;
    while (net_ok(s->u) && get_screen(s, 0)) {
        if (!shows(s, question)) {
            ok = 1;
            break;
        }
        if (strcmp(s->screen, pressed))
            drawn = 1;
        if (!strcmp(s->screen, last) && drawn) {
            if (!keys(s, 0, "Enter"))
                break;
            free(pressed);
            pressed = strdup(s->screen);
            drawn = 0;
        }
        free(last);
        last = strdup(s->screen);
        pause_read();
    }
    free(last);
    free(pressed);
    return ok;
}
/* Return one for a handled question, zero for none, and -1 for failure. */
static int claude_question(struct acceptance *s)
{
    int ok = 1;
    if (!strcmp(s->agent, "claude") && shows(s, "Choose the text style"))
        ok = leave(s, "Choose the text style");
    else if (!strcmp(s->agent, "claude") && shows(s, "Do you want to use this API key?"))
        ok = choose(s, "Yes", "Up");
    else if (!strcmp(s->agent, "claude") && shows(s, "Press Enter to continue"))
        ok = leave(s, "Press Enter to continue");
    else if (!strcmp(s->agent, "claude") && shows(s, "Yes, I trust this folder"))
        ok = choose(s, "Yes, I trust this folder", "Down");
    else if (!strcmp(s->agent, "claude") && shows(s, "Yes, I accept"))
        ok = choose(s, "Yes, I accept", "Down");
    else
        return 0;
    return ok ? 1 : -1;
}
static int question(struct acceptance *s)
{
    int ok = 1;
    if (!strcmp(s->agent, "claude"))
        return claude_question(s);
    else if (!strcmp(s->agent, "codex") && shows(s, "Trust and continue"))
        ok = choose(s, "1. Trust and continue", "Up");
    else if (!strcmp(s->agent, "codex") && shows(s, "Use existing model")) {
        ok = choose(s, "2. Use existing model", "Down");
        if (ok)
            s->model_selected = 1;
    } else if (!strcmp(s->agent, "agy") && shows(s, "Choose your color scheme"))
        ok = leave(s, "Choose your color scheme");
    else if (!strcmp(s->agent, "agy") && shows(s, "Terms of Service & Data Use"))
        ok = settle(s, "Enter", "[ ] Yes, I agree", NULL) &&
             settle(s, "Down", ">  Previous", NULL) && settle(s, "Right", ">  Done", NULL) &&
             leave(s, "Terms of Service & Data Use");
    else if (!strcmp(s->agent, "agy") && shows(s, "Yes, I trust this folder"))
        ok = leave(s, "Yes, I trust this folder");
    else if (!strcmp(s->agent, "muse") && shows(s, "Do you trust this workspace?"))
        ok = leave(s, "Do you trust this workspace?");
    else
        return 0;
    return ok ? 1 : -1;
}
static int idle(struct acceptance *s)
{
    unsigned calm = 0;
    while (net_ok(s->u) && get_screen(s, 0)) {
        int q;
        if (shows(s, s->signin))
            return 0;
        q = question(s);
        if (q < 0)
            return 0;
        if (q) {
            calm = 0;
            continue;
        }
        if (s->model_selected && shows(s, s->ready) && !shows(s, s->busy) &&
            (!*s->loading || !shows(s, s->loading))) {
            if (++calm == 2)
                return 1;
        } else
            calm = 0;
        pause_read();
    }
    return 0;
}
static int witness(struct acceptance *s, unsigned kind)
{
    char *text;
    int ok;
    if (kind < 2 && shows(s, s->busy))
        return 1;
    text = guest(s, kind == 0 ? "work" : kind == 3 ? "pid" : "sleeps", NULL);
    if (!text)
        return 0;
    ok = kind == 0 ? strncmp(text, "commits 1\n", 10) != 0 : kind == 1 ? *text != 0 : *text == 0;
    free(text);
    return ok;
}
static int send_text(struct acceptance *s, const char *text, const char *tail, unsigned kind)
{
    int asked = 0;
    if (!keys(s, 1, text))
        return 0;
    while (net_ok(s->u) && get_screen(s, 0)) {
        int q = question(s);
        unsigned n;
        if (q < 0)
            return 0;
        if (q)
            asked = 1;
        else if (shows(s, tail)) {
            if (!keys(s, 0, "Enter"))
                return 0;
            for (n = 0; n < 10; n++) {
                pause_read();
                if (!get_screen(s, 0))
                    return 0;
                if (witness(s, kind))
                    return 1;
                q = question(s);
                if (q < 0)
                    return 0;
                if (q) {
                    asked = 1;
                    break;
                }
            }
        } else if (asked) {
            if (!keys(s, 1, text))
                return 0;
            asked = 0;
        }
        pause_read();
    }
    return 0;
}
static int wait_guest(struct acceptance *s, const char *verb, const char *wanted, int some)
{
    while (net_ok(s->u)) {
        char *text = guest(s, verb, NULL);
        int ok;
        if (!text)
            return 0;
        {
            size_t n = strlen(text);
            while (n && (text[n - 1] == '\n' || text[n - 1] == '\r'))
                text[--n] = 0;
        }
        ok = some ? *text != 0 : !strcmp(text, wanted);
        free(text);
        if (ok)
            return 1;
        pause_read();
    }
    return 0;
}
static int attach(struct acceptance *s)
{
    char command[2000];
    snprintf(
        command, sizeof(command),
        "exec ssh -tt -p %s -i /tmp/etude-accept.key -o BatchMode=yes "
        "-o StrictHostKeyChecking=no -F /dev/null -o IdentitiesOnly=yes -o "
        "UserKnownHostsFile=/dev/null "
        "-o ControlPath=none -o LogLevel=ERROR test@10.0.2.2 "
        "tmux -u -L rach2-accept new-session -A -s rach2-accept-%s 2>/tmp/etude-accept-client.log",
        s->port, s->agent);
    if (s->window)
        return tmux(s, (const char *const[]){"respawn-pane", "-k", "-t", s->target, command, NULL});
    s->window = 1;
    return tmux(s, (const char *const[]){"new-window", "-d", "-t", "accept", "-n", s->agent,
                                         command, NULL});
}
static int attached(struct acceptance *s)
{
    unsigned lost = 0;
    while (net_ok(s->u)) {
        const char *args[] = {"/usr/bin/tmux",
                              "-L",
                              "etude-agent-accept",
                              "display-message",
                              "-p",
                              "-t",
                              s->target,
                              "#{pane_dead} #{pane_dead_status}",
                              NULL};
        char *text = guest(s, "pane", NULL), *dead;
        if (!text)
            return 0;
        if (!strcmp(text, "120x39 120x39 1\n")) {
            free(text);
            return 1;
        }
        free(text);
        dead = client(s, args);
        if (!dead)
            return 0;
        if (!strncmp(dead, "1 ", 2)) {
            free(dead);
            if (++lost == 5 || !attach(s))
                return 0;
        } else
            free(dead);
        pause_read();
    }
    return 0;
}
static int quit(struct acceptance *s)
{
    char *all, *at, *last = NULL;
    int ok;
    if (!send_text(s, s->quit, s->quit, 3))
        return 0;
    all = get_screen(s, 1);
    if (!all)
        return 0;
    for (at = all; (at = strstr(at, "rach2-accept: ended with status")); at++)
        last = at;
    ok = last &&
         !strncmp(last, "rach2-accept: ended with status 0",
                  strlen("rach2-accept: ended with status 0")) &&
         (last[strlen("rach2-accept: ended with status 0")] == '\n' ||
          last[strlen("rach2-accept: ended with status 0")] == 0);
    free(all);
    return ok;
}
static int has_line(const char *text, const char *wanted)
{
    size_t n = strlen(wanted);
    while (*text) {
        const char *end = strchr(text, '\n');
        size_t len = end ? (size_t)(end - text) : strlen(text);
        if (len && text[len - 1] == '\r')
            len--;
        if (len == n && !memcmp(text, wanted, n))
            return 1;
        if (!end)
            break;
        text = end + 1;
    }
    return 0;
}
static int session_start(struct acceptance *s)
{
    char *text, command[300];
    unsigned i;
    const char *init[] = {"/usr/bin/tmux",
                          "-L",
                          "etude-agent-accept",
                          "new-session",
                          "-d",
                          "-s",
                          "accept",
                          "-x",
                          "120",
                          "-y",
                          "40",
                          "-n",
                          "script",
                          NULL};
    if (etude_guest_put(s->u->client, s->key, "/tmp/etude-accept.key", "0600") != ETUDE_OK)
        return 0;
    text = client(s, init);
    if (!text)
        return 0;
    free(text);
    if (!tmux(s, (const char *const[]){"set-option", "-g", "remain-on-exit", "on", NULL}) ||
        !tmux(s,
              (const char *const[]){"set-option", "-g", "default-terminal", "tmux-256color", NULL}))
        return 0;
    text = guest(s, "prepare", NULL);
    if (!text)
        return 0;
    free(text);
    if (!attach(s) || !attached(s))
        return 0;
    snprintf(command, sizeof(command), "bash ~/rach2-accept/guest.sh run %s start", s->agent);
    if (!keys(s, 1, command) || !keys(s, 0, "Enter") || !idle(s))
        return 0;
    text = guest(s, "exe", NULL);
    if (!text)
        return 0;
    snprintf(command, sizeof(command), "/opt/%s/bin/%s\n", s->agent, s->agent);
    i = !strcmp(text, command);
    free(text);
    if (!i)
        return 0;
    return 1;
}
static int session_work(struct acceptance *s)
{
    char *text;
    unsigned i;
    if (!send_text(
            s,
            "Read notes.txt and do what it says. Check the change with check.sh, then commit it.",
            "then commit it.", 0))
        return 0;
    for (;;) {
        text = guest(s, "work", NULL);
        if (!text)
            return 0;
        i = !strncmp(text, "commits 2\n", 10);
        free(text);
        if (i)
            break;
        pause_read();
    }
    if (!idle(s))
        return 0;
    text = guest(s, "work", NULL);
    if (!text)
        return 0;
    i = has_line(text, "commits 2") && has_line(text, "greeting hello, rach2") &&
        has_line(text, "check check: ok") && has_line(text, "status ");
    free(text);
    if (!i)
        return 0;
    said(s, "read, changed, tested and committed");
    return 1;
}
static int session_cancel(struct acceptance *s)
{
    if (!send_text(s,
                   "Run the shell command sleep 600 in the foreground and wait for it to finish.",
                   "to finish.", 1) ||
        !wait_guest(s, "sleeps", NULL, 1) || !keys(s, 0, "Escape") || !idle(s))
        return 0;
    if (*s->after_cancel && (!send_text(s, s->after_cancel, s->after_cancel, 2) || !idle(s)))
        return 0;
    if (!wait_guest(s, "sleeps", "", 0) || !wait_guest(s, "pid", NULL, 1))
        return 0;
    said(s, "cancelled");
    if (!tmux(s, (const char *const[]){"resize-window", "-t", s->target, "-x", "100", "-y", "30",
                                       NULL}) ||
        !wait_guest(s, "pane", "100x29 100x29 1", 0) || !idle(s) ||
        !tmux(s, (const char *const[]){"resize-window", "-t", s->target, "-x", "120", "-y", "40",
                                       NULL}) ||
        !wait_guest(s, "pane", "120x39 120x39 1", 0))
        return 0;
    said(s, "terminal size followed both changes");
    return 1;
}
static int session_reconnect(struct acceptance *s)
{
    char *text;
    {
        const char *args[] = {"/usr/bin/tmux",   "-L",          "etude-agent-accept",
                              "display-message", "-p",          "-t",
                              s->target,         "#{pane_pid}", NULL};
        char *pid = client(s, args);
        const char *kill_args[] = {"/bin/kill", "-9", pid, NULL};
        if (!pid)
            return 0;
        pid[strcspn(pid, "\r\n")] = 0;
        text = client(s, kill_args);
        free(pid);
        if (!text)
            return 0;
        free(text);
    }
    if (!wait_guest(s, "pane", "120x39 120x39 0", 0) || !wait_guest(s, "pid", NULL, 1) ||
        !attach(s) || !attached(s) || !idle(s))
        return 0;
    said(s, "survived the lost SSH client and reconnected");
    return 1;
}
static int session_resume(struct acceptance *s)
{
    char command[300];
    unsigned i;
    if (!quit(s))
        return 0;
    snprintf(command, sizeof(command), "bash ~/rach2-accept/guest.sh run %s resume", s->agent);
    if (!keys(s, 1, command) || !keys(s, 0, "Enter") || !idle(s))
        return 0;
    for (;;) {
        char *all, *at, *last;
        int q;
        if (!get_screen(s, 0))
            return 0;
        q = question(s);
        if (q < 0)
            return 0;
        if (q)
            continue;
        all = get_screen(s, 1);
        if (!all)
            return 0;
        last = all;
        for (at = all; (at = strstr(at, "rach2-accept: ended with status")); at++) {
            char *next = strchr(at, '\n');
            if (next)
                last = next + 1;
        }
        i = strstr(last, "sleep 600") != NULL;
        free(all);
        if (i)
            break;
        pause_read();
    }
    said(s, "reopened the conversation with its cancelled request");
    if (!quit(s))
        return 0;
    if (!tmux(s, (const char *const[]){"kill-window", "-t", s->target, NULL}))
        return 0;
    if (!remote_ok(s, 0, "tmux -L rach2-accept kill-server", NULL))
        return 0;
    return tmux(s, (const char *const[]){"kill-server", NULL});
}
static int session(struct acceptance *s)
{
    return session_start(s) && session_work(s) && session_cancel(s) && session_reconnect(s) &&
           session_resume(s);
}

static int version(struct acceptance *s, const char *program, int after)
{
    char recipe[4096], path[4096], extra[100], expected[300], *source, *at, *text, *saved;
    size_t n;
    int ok;
    snprintf(extra, sizeof(extra), "%s", program);
    text = guest(s, "version", extra);
    if (!text)
        return 0;
    if (!accept_path(path, sizeof(path),
                     (const char *const[]){s->work, "/version.", program, NULL})) {
        free(text);
        return 0;
    }
    if (after) {
        saved = net_file(path);
        ok = !strcmp(saved, text);
        free(saved);
        free(text);
        return ok;
    }
    if (!accept_path(recipe, sizeof(recipe),
                     (const char *const[]){s->assets, "/vendor/gnu/agents/packages/", program,
                                           ".sh", NULL})) {
        free(text);
        return 0;
    }
    source = net_file(recipe);
    at = strstr(source, "\nVERSION=");
    if (at)
        at += 9;
    else if (!strncmp(source, "VERSION=", 8))
        at = source + 8;
    if (!at) {
        free(source);
        free(text);
        return 0;
    }
    n = strcspn(at, "\r\n");
    if (n > 90) {
        free(source);
        free(text);
        return 0;
    }
    memcpy(extra, at, n);
    extra[n] = 0;
    free(source);
    if (!strcmp(program, "claude"))
        snprintf(expected, sizeof(expected), "%s (Claude Code)\n", extra);
    else if (!strcmp(program, "codex"))
        snprintf(expected, sizeof(expected), "codex-cli %s\n", extra);
    else if (!strcmp(program, "agy"))
        snprintf(expected, sizeof(expected), "%s\n", extra);
    else if (!strcmp(program, "muse")) {
        char short_version[100];
        strcpy(short_version, extra);
        short_version[strcspn(short_version, "-")] = 0;
        snprintf(expected, sizeof(expected), "Muse Code %s (%s)\n", short_version, extra);
    } else
        snprintf(expected, sizeof(expected), "gh version %s (", extra);
    ok = !strncmp(text, expected, strlen(expected));
    snprintf(expected, sizeof(expected), " /opt/%s/bin/%s\n", program, program);
    at = strchr(text, '\n');
    ok = ok && at && strstr(at + 1, expected) != NULL;
    if (ok)
        ok = save_file(path, text);
    free(text);
    return ok;
}
static int prepare(struct acceptance *s)
{
    const char *archive = etude_artifact(s->u->c, "agent-assets");
    char agent_source[4096], gh_source[4096];
    char *unpack[] = {"agent_server", "unpack", (char *)archive, s->assets, NULL};
    char *render[] = {"agent_server",  "accept-prepare", agent_source, gh_source,
                      (char *)s->work, (char *)s->agent, NULL};
    if (!accept_path(agent_source, sizeof(agent_source),
                     (const char *const[]){s->assets, "/tests/system/accept/agents/", s->agent,
                                           ".json", NULL}) ||
        !accept_path(gh_source, sizeof(gh_source),
                     (const char *const[]){s->assets, "/tests/system/accept/agents/gh.json", NULL}))
        return 0;
    return archive && host(s, unpack, "accept-assets") && host(s, render, "accept-scripts") &&
           save_file(s->log, "");
}
static int upload(struct acceptance *s, const char *source, const char *target)
{
    char command[600];
    snprintf(command, sizeof(command), "cat > ~/rach2-accept/%s", target);
    return remote_ok(s, 0, command, source);
}
static int upload_files(struct acceptance *s)
{
    char path[4096], extra[200];
    if (!accept_path(
            path, sizeof(path),
            (const char *const[]){s->assets, "/tests/system/accept/agents_guest.sh", NULL}))
        return 0;
    if (!upload(s, path, "guest.sh"))
        return 0;
    {
        const char *files[] = {"notes.txt", "greeting.txt", "check.sh"};
        unsigned i;
        for (i = 0; i < 3; i++) {
            if (!accept_path(path, sizeof(path),
                             (const char *const[]){s->assets, "/tests/system/fakeapi/repo/",
                                                   files[i], NULL}))
                return 0;
            snprintf(extra, sizeof(extra), "fixture/%s", files[i]);
            if (!upload(s, path, extra))
                return 0;
        }
    }
    if (!accept_path(path, sizeof(path), (const char *const[]){s->work, "/ca/ca.pem", NULL}))
        return 0;
    if (!upload(s, path, "ca.pem"))
        return 0;
    return 1;
}

static int setup(struct acceptance *s)
{
    char path[4096], line[20000], *hosts, *at, *save = NULL, extra[200], *text;
    size_t used;
    unsigned count;
    if (!remote_ok(s, 0, "test ! -d ~/rach2-accept || test -f ~/rach2-accept/.rach2-accept",
                   NULL) ||
        !remote_ok(s, 0,
                   "rm -rf ~/rach2-accept && mkdir -p ~/rach2-accept/fixture && touch "
                   "~/rach2-accept/.rach2-accept",
                   NULL))
        return 0;
    if (!accept_path(path, sizeof(path), (const char *const[]){s->work, "/hosts", NULL}))
        return 0;
    hosts = net_file(path);
    used = (size_t)snprintf(line, sizeof(line), "printf '");
    for (at = strtok_r(hosts, "\r\n", &save); at; at = strtok_r(NULL, "\r\n", &save)) {
        const char *p;
        for (p = at; *p; p++)
            if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-", *p)) {
                free(hosts);
                return 0;
            }
        if (used >= sizeof(line)) {
            free(hosts);
            return 0;
        }
        used += (size_t)snprintf(line + used, sizeof(line) - used, "10.0.2.100 %s\\n", at);
    }
    free(hosts);
    if (used + 20 >= sizeof(line))
        return 0;
    strcpy(line + used, "' >> /etc/hosts");
    if (!remote_ok(s, 1, line, NULL))
        return 0;
    if (!upload_files(s))
        return 0;
    text = guest(s, "setup", "");
    if (!text)
        return 0;
    free(text);
    if (!version(s, s->agent, 0) || !version(s, "gh", 0))
        return 0;
    snprintf(extra, sizeof(extra), "qemu %s", s->agent);
    text = guest(s, "credentials", extra);
    if (!text)
        return 0;
    if (!accept_path(path, sizeof(path),
                     (const char *const[]){s->work, "/credentials.before", NULL})) {
        free(text);
        return 0;
    }
    count = *text && !strstr(text, " missing");
    if (count)
        count = save_file(path, text);
    free(text);
    if (!count)
        return 0;
    text = guest(s, "filtered", extra);
    if (!text)
        return 0;
    count = 0;
    used = sscanf(text, "0 of %u", &count);
    free(text);
    return used == 1 && count > 0;
}
static int credentials(struct acceptance *s)
{
    char extra[200], path[4096], *text, *before;
    int ok;
    snprintf(extra, sizeof(extra), "qemu %s", s->agent);
    text = guest(s, "credentials", extra);
    if (!text)
        return 0;
    if (!accept_path(path, sizeof(path),
                     (const char *const[]){s->work, "/credentials.before", NULL})) {
        free(text);
        return 0;
    }
    before = net_file(path);
    ok = !strcmp(before, text);
    free(before);
    free(text);
    return ok;
}
static int signed_in(struct acceptance *s, int github)
{
    const char *program = github ? "gh" : s->agent;
    char extra[200], *text, *p;
    int ok;
    if (!version(s, program, 1))
        return 0;
    snprintf(extra, sizeof(extra), "%s once", program);
    text = guest(s, "run", extra);
    if (!text)
        return 0;
    ok = strstr(text, "rach2-accept: ended with status 0") != NULL;
    if (!github) {
        for (p = text; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        ok = ok && strstr(text, "signed in") != NULL;
    }
    free(text);
    return ok;
}
static int finish(struct acceptance *s)
{
    char command[400];
    struct net_run r;
    int ok;
    snprintf(command, sizeof(command), "bash ~/rach2-accept/guest.sh secrets qemu %s", s->agent);
    if (!remote(s, 0, command, s->log, &r))
        return 0;
    ok = net_exited0(&r) && !strncmp(r.out, "0 of ", 5);
    net_run_free(&r);
    ok = remote_ok(s, 0, "rm -rf ~/rach2-accept && test ! -e ~/rach2-accept", NULL) && ok;
    free(s->screen);
    s->screen = NULL;
    return ok;
}
int agent_accept_action(struct net_case *u, const char *work, const char *agent, const char *verb)
{
    struct acceptance *s = &state;
    int ok;
    if (!s->u) {
        memset(s, 0, sizeof(*s));
        s->work = work;
        s->agent = agent;
        if (!accept_path(s->assets, sizeof(s->assets),
                         (const char *const[]){work, "/assets", NULL}) ||
            !accept_path(s->log, sizeof(s->log), (const char *const[]){work, "/accept.log", NULL}))
            return 0;
        s->u = u;
        snprintf(s->port, sizeof(s->port), "%u", etude_port(u->c, 0));
        snprintf(s->target, sizeof(s->target), "accept:%s", agent);
        net_key(u, "demo-key", s->root_key, sizeof(s->root_key));
        net_key(u, "test-key", s->key, sizeof(s->key));
        s->model_selected = strcmp(agent, "codex") != 0;
        s->loading = "";
        s->after_cancel = "";
        s->ready = !strcmp(agent, "claude") ? "bypass permissions on"
                   : !strcmp(agent, "muse") ? "YOLO"
                                            : "? for shortcuts";
        s->busy = !strcmp(agent, "agy") ? "esc to cancel" : "esc to interrupt";
        s->mark = !strcmp(agent, "codex") ? "\342\200\272"
                  : !strcmp(agent, "agy") ? ">"
                                          : "\342\235\257";
        s->quit = !strcmp(agent, "codex") ? "/quit" : "/exit";
        s->signin = !strcmp(agent, "claude")  ? "Select login method"
                    : !strcmp(agent, "codex") ? "Sign in with ChatGPT"
                    : !strcmp(agent, "agy")   ? "Google OAuth"
                                              : "Log in with browser";
        if (!strcmp(agent, "codex")) {
            s->after_cancel = "/stop";
            s->loading = "loading";
        }
    }
    if (!strcmp(verb, "prepare"))
        ok = prepare(s);
    else if (!strcmp(verb, "setup"))
        ok = setup(s);
    else if (!strcmp(verb, "session"))
        ok = session(s);
    else if (!strcmp(verb, "credentials"))
        ok = credentials(s);
    else if (!strcmp(verb, "after"))
        ok = signed_in(s, 0);
    else if (!strcmp(verb, "gh"))
        ok = signed_in(s, 1);
    else if (!strcmp(verb, "finish"))
        ok = finish(s);
    else
        ok = 0;
    if (!ok)
        fprintf(stderr, "agents-accept: %s: %s failed\n", agent, verb);
    else
        said(s, verb);
    return ok;
}
