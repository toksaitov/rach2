/*
 * ssh_scenario.c: the scenario of the ssh cases, tests/cases/machine.ssh.json
 * and machine.ssh-e1000.json: logins to os-real over SSH with nobody at the
 * keyboard, on the card the case's machine has.
 *
 * The procedure is a person's: boot os-real from a copy of its disk, log in
 * as root at the console, start dropbear as the Readme's recipe does, and
 * connect from the host through the port QEMU forwards to the guest's port
 * 22. Root logs in by key, and the test account by password, which is the
 * login that reaches /etc/shadow. Then the logins that must be refused, and
 * a flood of connections held open while root logs in again at the console.
 *
 * Each login runs ssh as a host tool: by key in batch mode, and by password
 * under ptydrive, which gives ssh a terminal, answers its prompt from the
 * environment and counts the prompts. A refusal counts only when the server
 * said `Permission denied`, since a connection that failed, a server that
 * had died and an ssh that could not start print no session either.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/* The flood's connections, more than the fifteen dropbear's listener
 * holds while it takes them one at a time. */
#define FLOOD "40"

/* The text that reports the guest's count of established connections, and
 * the count it gives, or -1 when netstat failed. */
#define COUNT_LINE                                                                         \
    "if netstat -tn > /tmp/ns 2>&1; then echo FLOOD\"\"N=$(grep -c ESTABLISHED /tmp/ns); " \
    "else echo FLOOD\"\"N=none; fi; "

/* What one attempt shares among its steps. */
struct ssh_case {
    struct net_case u;
    struct net_boot b;
    char port[16];
    char root_password[128];
    char test_password[128];
    char key[4096]; /* the path of the private copy of root's key */
};

/**
 * Run ssh, as root by key with the private copy of the demo key, against
 * the forwarded port, and run `command`. ssh reads no file of the account
 * that runs it, offers that key alone, and keeps no record of the host's
 * key.
 */
static enum etude_status ssh_key(struct ssh_case *s, const char *command, const char *output,
                                 struct net_run *r)
{
    char *argv[] = {NET_SSH, NET_SSH_KEY(s->key), "-tt",           "-p",
                    s->port, "root@127.0.0.1",    (char *)command, NULL};

    return net_host(&s->u, argv, output, r);
}

/**
 * Log in as `user` by `password` under ptydrive and run `command`. ssh asks
 * for the password once. ptydrive sizes the terminal at 40 rows by 100
 * columns, takes the password from the environment, and prints `PROMPTED=N`
 * after the session's output.
 */
static enum etude_status ssh_password(struct ssh_case *s, const char *user, const char *password,
                                      const char *command, const char *output, struct net_run *r)
{
    char who[64];
    char *argv[] = {"ptydrive",
                    "--rows",
                    "40",
                    "--cols",
                    "100",
                    "--timeout",
                    "3600",
                    "--",
                    NET_SSH,
                    "-o",
                    "PubkeyAuthentication=no",
                    "-o",
                    "PreferredAuthentications=password",
                    "-o",
                    "NumberOfPasswordPrompts=1",
                    "-tt",
                    "-p",
                    s->port,
                    who,
                    (char *)command,
                    NULL};
    enum etude_status st;

    snprintf(who, sizeof(who), "%s@127.0.0.1", user);
    if (setenv("RACH2_PASSWORD", password, 1) != 0) {
        net_fail(&s->u, "the password could not be put in the environment");
        return ETUDE_FAILED;
    }
    st = net_host(&s->u, argv, output, r);
    unsetenv("RACH2_PASSWORD");
    return st;
}

/** Give the first line of `r`'s standard error, or of its output when that
 * is empty, in `buf` of `size` bytes, to show in a failure. */
static const char *said(const struct net_run *r, char *buf, size_t size)
{
    const char *text = r->err != NULL && r->err[0] != '\0' ? r->err : r->out;

    return net_copy_line(text != NULL ? text + strspn(text, "\r\n") : NULL, buf, size);
}

/** Say whether `r` is the server's refusal: `Permission denied` from ssh,
 * no time-out of ptydrive, and none of `ran`, the command's marker. */
static int refused(const struct net_run *r, const char *ran)
{
    return (strstr(r->out, "Permission denied") != NULL ||
            strstr(r->err, "Permission denied") != NULL) &&
           strstr(r->out, "TIMEDOUT") == NULL && strstr(r->out, ran) == NULL;
}

/** Boot the copy of the disk and log in as root at its console. */
static void console(struct ssh_case *s)
{
    if (net_step(&s->u, "boot") == ETUDE_OK)
        net_boot(&s->b, &s->u, "server", 1, NULL);
    if (net_step(&s->u, "console") == ETUDE_OK)
        net_login(&s->b, "root", s->root_password, "# ");
    net_assert(&s->u, "root-prompt", 1, "");
}

/** Start dropbear as the recipe does. -R makes the host key at its first
 * use, which needs the trusted entropy of the `max` processor; -g refuses
 * root's password. */
static void dropbear(struct ssh_case *s)
{
    if (net_step(&s->u, "dropbear") == ETUDE_OK &&
        net_line(&s->b, "dropbearmulti dropbear -R -g -p 22") == ETUDE_OK)
        net_wait(&s->b, "Running in background");
    net_assert(&s->u, "dropbear", 1, "");
}

/** Root by key, on a terminal: `-tt` and the `test -t` in front of the
 * command make it a pty session, which dropbear's pty path serves. */
static void key_login(struct ssh_case *s)
{
    struct net_run r;
    char line[160];

    if (net_step(&s->u, "key-login") == ETUDE_OK)
        ssh_key(s, "test -t 0 && test -t 1 && id && echo KEY\"\"LOGIN", "key-login", &r);
    if (!net_ok(&s->u)) {
        net_assert(&s->u, "key-login", 0, "");
        net_assert(&s->u, "key-root", 0, "");
        return;
    }
    net_assert(&s->u, "key-login", net_exited0(&r) && strstr(r.out, "KEYLOGIN") != NULL,
               "the command did not run on a terminal: %s", said(&r, line, sizeof(line)));
    net_assert(&s->u, "key-root", strstr(r.out, "uid=0(root)") != NULL,
               "the key login did not arrive as root: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
}

/** The test account by password, and the window and termios of its
 * session's terminal, which `stty size` (TIOCGWINSZ) and `stty -a` (TCGETS
 * as well) read off the slave side. */
static void password_login(struct ssh_case *s)
{
    struct net_run r;
    char line[160];

    if (net_step(&s->u, "password-login") == ETUDE_OK)
        ssh_password(s, "test", s->test_password,
                     "test -t 0 && test -t 1 && id && echo PASS\"\"LOGIN", "password-login", &r);
    if (net_ok(&s->u)) {
        net_assert(&s->u, "password-login",
                   net_exited0(&r) && strstr(r.out, "PROMPTED=1") != NULL &&
                       strstr(r.out, "PASSLOGIN") != NULL,
                   "no session after one prompt: %s", said(&r, line, sizeof(line)));
        net_assert(&s->u, "password-uid", strstr(r.out, "uid=1000(test)") != NULL,
                   "the password login did not arrive as the test account: %s",
                   said(&r, line, sizeof(line)));
        net_run_free(&r);
    } else {
        net_assert(&s->u, "password-login", 0, "");
        net_assert(&s->u, "password-uid", 0, "");
    }
    if (net_step(&s->u, "window") == ETUDE_OK)
        ssh_password(s, "test", s->test_password, "stty size; stty -a", "window", &r);
    if (!net_ok(&s->u)) {
        net_assert(&s->u, "window-size", 0, "");
        net_assert(&s->u, "window-termios", 0, "");
        return;
    }
    net_assert(&s->u, "window-size", net_exited0(&r) && net_find(r.out, "40 100") != NULL,
               "stty size did not report 40 rows and 100 columns: %s",
               said(&r, line, sizeof(line)));
    net_assert(&s->u, "window-termios",
               strstr(r.out, "rows 40; columns 100") != NULL && strstr(r.out, "icanon") != NULL,
               "stty -a did not report the window and the termios: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
}

/** The logins that must be refused, each by the server: no credentials at
 * all, the test account's password with a byte added, which tests that the
 * hash is compared, and root's right password, which -g refuses. */
static void refusals(struct ssh_case *s)
{
    char *none[] = {NET_SSH,
                    "-o",
                    "BatchMode=yes",
                    "-o",
                    "PubkeyAuthentication=no",
                    "-o",
                    "PasswordAuthentication=no",
                    "-o",
                    "KbdInteractiveAuthentication=no",
                    "-p",
                    s->port,
                    "test@127.0.0.1",
                    "echo NOAUTH\"\"RAN",
                    NULL};
    char wrong[160], line[160];
    struct net_run r;

    if (net_step(&s->u, "refusals") == ETUDE_OK)
        net_host(&s->u, none, "no-credentials", &r);
    if (net_ok(&s->u)) {
        net_assert(&s->u, "no-credentials-refused", !net_exited0(&r) && refused(&r, "NOAUTHRAN"),
                   "not refused by the server: %s", said(&r, line, sizeof(line)));
        net_run_free(&r);
    } else {
        net_assert(&s->u, "no-credentials-refused", 0, "");
    }
    snprintf(wrong, sizeof(wrong), "%sx", s->test_password);
    ssh_password(s, "test", wrong, "echo BADPASS\"\"RAN", "wrong-password", &r);
    if (net_ok(&s->u)) {
        net_assert(&s->u, "wrong-password-refused",
                   net_exited0(&r) && strstr(r.out, "PROMPTED=1") != NULL &&
                       refused(&r, "BADPASSRAN"),
                   "not asked once and refused by the server: %s", said(&r, line, sizeof(line)));
        net_run_free(&r);
    } else {
        net_assert(&s->u, "wrong-password-refused", 0, "");
    }
    ssh_password(s, "root", s->root_password, "echo ROOTPW\"\"RAN", "root-password", &r);
    if (!net_ok(&s->u)) {
        net_assert(&s->u, "root-password-refused", 0, "");
        return;
    }
    net_assert(&s->u, "root-password-refused", net_exited0(&r) && refused(&r, "ROOTPWRAN"),
               "not refused by the server: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
}

/** Type `loop`, then the count of the guest's established connections and
 * `echo ECHOED`, which prints `marker`, and give the count, or -1 when
 * netstat failed or the console did not answer. */
static long established(struct ssh_case *s, const char *loop, const char *echoed,
                        const char *marker)
{
    char command[512], *text = NULL;
    long n;

    snprintf(command, sizeof(command), "%s" COUNT_LINE "echo %s", loop, echoed);
    if (net_command(&s->b, command, marker, &text) != ETUDE_OK)
        return -1;
    n = net_number_after(text, "FLOODN=");
    free(text);
    return n;
}

/**
 * The flood: forty connections held without a word while root ends the
 * console session and logs in again. The console needs no socket and
 * root's processes are not counted, so the login must come back to a shell
 * that answers `id` with uid 0.
 *
 * The count is read once the server has answered a connection of the
 * flood, so it must be more than none. Once the flood is stopped, the guest
 * runs netstat until no connection is established, and the step's bound
 * ends a guest that keeps one.
 */
static void flood(struct ssh_case *s)
{
    struct etude_case_service *f = NULL;
    struct etude_termination end;
    char *args[] = {"flood", s->port, FLOOD, NULL}, *text = NULL;
    uint64_t at;
    long n = -1;

    if (net_step(&s->u, "flood") == ETUDE_OK &&
        etude_service(s->u.c, "flood", args, &f, &end) != ETUDE_OK)
        net_fail(&s->u, "the flood did not start");
    if (net_ok(&s->u) && etude_service_wait(f, "sshpeer: answered 1\n", 0, &at) != ETUDE_OK)
        net_fail(&s->u, "the server answered no connection of the flood");
    if (net_ok(&s->u))
        n = established(s, "", "FLOOD\"\"COUNT", "FLOODCOUNT");
    net_assert(&s->u, "flood-held", n > 0, "the guest holds %ld established connections", n);
    if (net_line(&s->b, "exit") == ETUDE_OK &&
        net_login(&s->b, "root", s->root_password, "# ") == ETUDE_OK)
        net_command(&s->b, "id; echo FLOOD\"\"LOGIN", "FLOODLOGIN", &text);
    net_assert(&s->u, "flood-console-login", text != NULL && strstr(text, "uid=0(root)") != NULL,
               "root's shell under the flood did not answer as uid 0");
    free(text);
    if (net_step(&s->u, "release") == ETUDE_OK && etude_service_stop(f, &end) != ETUDE_OK)
        net_fail(&s->u, "the flood could not be stopped");
    n = -1;
    if (net_ok(&s->u))
        n = established(s,
                        "while netstat -tn > /tmp/ns 2>&1 && grep -q ESTABLISHED /tmp/ns; "
                        "do :; done; ",
                        "FLOOD\"\"CLEAR", "FLOODCLEAR");
    net_assert(&s->u, "flood-released", n == 0,
               "the guest holds %ld established connections after the flood", n);
}

int etude_scenario(struct etude_case *c)
{
    struct ssh_case s;
    unsigned port;

    memset(&s, 0, sizeof(s));
    net_case(&s.u, c);
    port = etude_port(c, 0);
    snprintf(s.port, sizeof(s.port), "%u", port);
    if (port == 0)
        net_fail(&s.u, "the case has no port");
    net_secret(&s.u, "demo-password", s.root_password, sizeof(s.root_password));
    net_secret(&s.u, "test-password", s.test_password, sizeof(s.test_password));
    net_key(&s.u, "demo-key", s.key, sizeof(s.key));
    console(&s);
    dropbear(&s);
    key_login(&s);
    password_login(&s);
    refusals(&s);
    flood(&s);
    return net_end(&s.u);
}
