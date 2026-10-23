/*
 * gnu_scenario.c: the GNU system's cases (machine.gnu.json). Each case
 * boots an overlay of the prepared GNU root. The login and console cases
 * use OpenSSH in a declared Rach2 client machine; their credentials,
 * terminal windows, scp and sftp are the operations being tested. Other
 * rows log in at the target console, open etude-guest's framed channel,
 * transfer their inputs and run the installed GNU programs there.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/* The records of the boot the cases wait for, in the order the console
 * usually has them: the root mounted, the login prompt and the server
 * started. */
#define ROOT_MOUNTED "ext2: root: /dev/disk0p1"
#define LOGIN_PROMPT "rach2 login"
#define SSHD_STARTED "init: sshd: started as pid "

/* The time the ntpd row's SNTP server starts at, 2030-01-01 00:00:00 UTC,
 * which the row requires the machine's clock to be set to. */
#define NTP_EPOCH "1893456000"

/* The words of an scp or sftp that read no file of the account that runs
 * it, keep no record of the host's key, print their errors alone, and
 * offer the key at `key` alone, in batch mode. */
#define COPY_WORDS(program, key)                                        \
    program, "-F", "/dev/null", "-o", "StrictHostKeyChecking=no", "-o", \
        "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR", NET_SSH_KEY(key)

/* What one attempt shares among its steps. */
struct gnu_case {
    struct net_case u;
    struct net_boot b;
    const char *row;  /* the last part of the case's identity */
    const char *arch; /* the architecture booted, from ARCH */
    char port[16];    /* the forward to the guest's port 22 */
    char root_password[128];
    char test_password[128];
    char root_key[4096]; /* the path of the private copy of root's key */
    char test_key[4096]; /* and of the test account's */
};

/** Give the first line of `r`'s standard error, or of its output when that
 * is empty, in `buf` of `size` bytes, to show in a failure. */
static const char *said(const struct net_run *r, char *buf, size_t size)
{
    const char *text = r->err != NULL && r->err[0] != '\0' ? r->err : r->out;

    return net_copy_line(text != NULL ? text + strspn(text, "\r\n") : NULL, buf, size);
}

/** Say whether `text` has `a`, then `b` after it, then `c` after that. */
static int in_order(const char *text, const char *a, const char *b, const char *c)
{
    const char *at = strstr(text, a);

    at = at != NULL ? strstr(at + strlen(a), b) : NULL;
    return at != NULL && strstr(at + strlen(b), c) != NULL;
}

/** Say whether `text` has `line` as one whole line. */
static int has_line(const char *text, const char *line)
{
    size_t n = strlen(line);
    const char *at;

    for (at = strstr(text, line); at != NULL; at = strstr(at + 1, line))
        if ((at == text || at[-1] == '\n') && (at[n] == '\n' || at[n] == '\0'))
            return 1;
    return 0;
}

/** Give the line of `r`'s output that has `pattern`, or what said() gives,
 * in `buf` of `size` bytes. */
static const char *line_of(const struct net_run *r, const char *pattern, char *buf, size_t size)
{
    const char *line = net_find(r->out, pattern);

    return line != NULL ? net_copy_line(line, buf, size) : said(r, buf, size);
}

/**
 * Run `command` on the machine as `user` by the key at `key`, without a
 * terminal, keeping what it prints as evidence `output`.
 */
static enum etude_status as_user(struct gnu_case *s, const char *user, const char *key,
                                 const char *command, const char *output, struct net_run *r)
{
    char who[64];
    char *argv[] = {NET_SSH, NET_SSH_KEY((char *)key), "-T", "-p", s->port,
                    who,     (char *)command,          NULL};

    snprintf(who, sizeof(who), "%s@127.0.0.1", user);
    return net_host(&s->u, argv, output, r);
}

/** Run `command` as root by the demo key, as as_user does. */
static enum etude_status as_root(struct gnu_case *s, const char *command, const char *output,
                                 struct net_run *r)
{
    if (strcmp(s->row, "logins") != 0 && strcmp(s->row, "console") != 0) {
        const char *argv[] = {"/bin/bash", "-c", command, NULL};
        return net_guest(&s->u, s->b.m, argv, NULL, NULL, output, r);
    }
    return as_user(s, "root", s->root_key, command, output, r);
}

/**
 * Log in as `user`, root or test, by its password under ptydrive, with at
 * most `prompts` prompts, and run `command`. ptydrive does not pass on the
 * status of what it runs, so ssh runs under a shell that prints `SSHRC=N`,
 * ssh's own status, once ssh has ended; ptydrive prints `PROMPTED=N` after
 * it. ptydrive has no time limit of its own: the step's watchdog ends a
 * login that hangs.
 */
static enum etude_status ssh_password(struct gnu_case *s, const char *user, const char *prompts,
                                      const char *command, const char *output, struct net_run *r)
{
    char who[64], tries[48];
    char *argv[] = {"ptydrive",
                    "--rows",
                    "40",
                    "--cols",
                    "100",
                    "--",
                    "sh",
                    "-c",
                    "ssh \"$@\"; echo \"SSHRC=$?\"",
                    NET_SSH,
                    "-o",
                    "PubkeyAuthentication=no",
                    "-o",
                    "PreferredAuthentications=password",
                    "-o",
                    tries,
                    "-tt",
                    "-p",
                    s->port,
                    who,
                    (char *)command,
                    NULL};
    const char *password = strcmp(user, "root") == 0 ? s->root_password : s->test_password;
    enum etude_status st;

    snprintf(who, sizeof(who), "%s@127.0.0.1", user);
    snprintf(tries, sizeof(tries), "NumberOfPasswordPrompts=%s", prompts);
    if (setenv("RACH2_PASSWORD", password, 1) != 0) {
        net_fail(&s->u, "the password could not be put in the environment");
        return ETUDE_FAILED;
    }
    st = net_host(&s->u, argv, output, r);
    unsetenv("RACH2_PASSWORD");
    return st;
}

/** Copy artifact `name` to `remote` on the machine as root with scp,
 * keeping what scp prints as evidence `output`; give whether it exited 0. */
static int copy_to(struct gnu_case *s, const char *name, const char *remote, const char *output)
{
    const char *path = etude_artifact(s->u.c, name);
    char to[256];
    char *argv[] = {COPY_WORDS("scp", s->root_key), "-P", s->port, (char *)path, to, NULL};
    struct net_run r;
    int ok;

    if (path == NULL) {
        net_fail(&s->u, "artifact %s is not the case's", name);
        return 0;
    }
    if (strcmp(s->row, "logins") != 0) {
        enum etude_status st = etude_guest_put(s->b.m, path, remote, "0644");
        if (st != ETUDE_OK)
            net_fail(&s->u, "the row's input could not be transferred to the guest");
        return st == ETUDE_OK;
    }
    snprintf(to, sizeof(to), "root@127.0.0.1:%s", remote);
    if (net_host(&s->u, argv, output, &r) != ETUDE_OK)
        return 0;
    ok = net_exited0(&r);
    net_run_free(&r);
    return ok;
}

/** Wait until the server answers on the case's port, as sshpeer judges it:
 * a connection that has the server's identification. Each try follows the
 * guest's refusal of the one before. */
static void answered(struct gnu_case *s)
{
    char *argv[] = {"sshpeer", "wait", s->port, NULL};
    char line[160];
    struct net_run r;

    net_host(&s->u, argv, "answered", &r);
    if (net_ok(&s->u) && !(net_exited0(&r) && strstr(r.out, "sshpeer: answered") != NULL))
        net_fail(&s->u, "sshpeer: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
}

/**
 * Boot the machine and wait until its server answers: the root mounted, the
 * login prompt, init's record of starting the server, and the server's own
 * answer. With `report` set, each is an assertion of the case.
 */
static void up(struct gnu_case *s, int report)
{
    if (net_step(&s->u, "boot") == ETUDE_OK)
        net_boot(&s->b, &s->u, "gnu", 1, NULL);
    net_seen(&s->b, ROOT_MOUNTED);
    if (report)
        net_assert(&s->u, "root", 1, "");
    net_seen(&s->b, LOGIN_PROMPT);
    if (report)
        net_assert(&s->u, "login-prompt", 1, "");
    net_seen(&s->b, SSHD_STARTED);
    answered(s);
    if (report)
        net_assert(&s->u, "answers", 1, "");
}

/** Root and the test account by their keys, on a machine whose server made
 * its host keys on this boot. */
static void keys(struct gnu_case *s)
{
    char line[160];
    struct net_run r;

    if (net_step(&s->u, "keys") == ETUDE_OK)
        as_root(s, "id -u; ls /etc/ssh", "root-key", &r);
    if (net_ok(&s->u)) {
        net_assert(&s->u, "root-key", net_exited0(&r) && strncmp(r.out, "0\n", 2) == 0,
                   "root by key did not arrive as uid 0: %s", said(&r, line, sizeof(line)));
        net_assert(&s->u, "host-keys", net_find(r.out, "ssh_host_ed25519_key") != NULL,
                   "/etc/ssh has no ed25519 host key: %s", said(&r, line, sizeof(line)));
        net_run_free(&r);
    } else {
        net_assert(&s->u, "root-key", 0, "");
        net_assert(&s->u, "host-keys", 0, "");
    }
    as_user(s, "test", s->test_key, "id -u", "test-key", &r);
    if (!net_ok(&s->u)) {
        net_assert(&s->u, "test-key", 0, "");
        return;
    }
    net_assert(&s->u, "test-key", net_exited0(&r) && strcmp(r.out, "1000\n") == 0,
               "the test account by key did not arrive as uid 1000: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
}

/** The test account by its password, which /etc/shadow checks, and root's
 * password, which the server refuses. */
static void passwords(struct gnu_case *s)
{
    char line[160];
    struct net_run r;

    if (net_step(&s->u, "passwords") == ETUDE_OK)
        ssh_password(s, "test", "3", "id -u; echo PASS\"\"LOGIN", "test-password", &r);
    if (net_ok(&s->u)) {
        net_assert(&s->u, "test-password",
                   net_exited0(&r) && strstr(r.out, "PROMPTED=1") != NULL &&
                       in_order(r.out, "1000", "PASSLOGIN", "SSHRC=0"),
                   "no session as uid 1000 after one prompt: %s", said(&r, line, sizeof(line)));
        net_run_free(&r);
    } else {
        net_assert(&s->u, "test-password", 0, "");
    }
    ssh_password(s, "root", "1", "echo ROOT\"\"PASSWORD", "root-password", &r);
    if (!net_ok(&s->u)) {
        net_assert(&s->u, "root-password-refused", 0, "");
        return;
    }
    net_assert(&s->u, "root-password-refused",
               net_exited0(&r) && strstr(r.out, "ROOTPASSWORD") == NULL &&
                   (strstr(r.out, "Permission denied") != NULL ||
                    strstr(r.err, "Permission denied") != NULL) &&
                   strstr(r.out, "SSHRC=255") != NULL,
               "root's password was not refused by the server: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
}

/** The rows' script sent by scp and fetched back by sftp, whose server is
 * OpenSSH's sftp-server, and compared with the file sent. */
static void transfer(struct gnu_case *s)
{
    char batch[4096], back[4096], line[160];
    char *argv[] = {
        COPY_WORDS("sftp", s->root_key), "-P", s->port, "-b", batch, "root@127.0.0.1", NULL};
    char *sent, *got;
    struct net_run r;
    FILE *f;
    int ok;

    if (net_step(&s->u, "transfer") == ETUDE_OK &&
        as_root(s, "mkdir -p /tmp/gnu", "mkdir", &r) == ETUDE_OK) {
        if (!net_exited0(&r))
            net_fail(&s->u, "mkdir /tmp/gnu: %s", said(&r, line, sizeof(line)));
        net_run_free(&r);
    }
    ok = net_ok(&s->u) && copy_to(s, "gnu-rows", "/tmp/gnu/rows.sh", "scp");
    net_assert(&s->u, "scp", ok, "scp did not put the rows' script on the machine");
    if (net_path(&s->u, "sftp.batch", batch, sizeof(batch)) == NULL ||
        net_path(&s->u, "sftp.back", back, sizeof(back)) == NULL) {
        net_assert(&s->u, "sftp", 0, "");
        return;
    }
    if ((f = fopen(batch, "w")) == NULL || fprintf(f, "get /tmp/gnu/rows.sh %s\n", back) < 0 ||
        fclose(f) != 0) {
        net_fail(&s->u, "the batch of sftp could not be written");
        net_assert(&s->u, "sftp", 0, "");
        return;
    }
    net_host(&s->u, argv, "sftp", &r);
    if (!net_ok(&s->u)) {
        net_assert(&s->u, "sftp", 0, "");
        return;
    }
    sent = net_file(etude_artifact(s->u.c, "gnu-rows"));
    got = net_file(back);
    net_assert(&s->u, "sftp", net_exited0(&r) && sent[0] != '\0' && strcmp(sent, got) == 0,
               "sftp did not fetch the file scp sent, byte for byte: %s",
               said(&r, line, sizeof(line)));
    free(sent);
    free(got);
    net_run_free(&r);
}

/**
 * Root at the console, tmux there and a command in its pane. tmux's server
 * draws through the standard input its client passes it, which a login on
 * the console has from the kernel, so the pane's text must reach the
 * console; and the machine must still answer over SSH, where root then ends
 * the tmux server.
 */
static void console(struct gnu_case *s)
{
    char *text = NULL, line[160];
    struct net_run r;

    if (net_step(&s->u, "console") == ETUDE_OK &&
        net_login(&s->b, "root", s->root_password, "# ") == ETUDE_OK)
        net_command(&s->b, "echo CONSOLE\"\"LOGIN", "CONSOLELOGIN", &text);
    net_assert(&s->u, "console-login", text != NULL, "");
    free(text);
    text = NULL;
    if (net_step(&s->u, "tmux") == ETUDE_OK && net_line(&s->b, "tmux") == ETUDE_OK)
        net_command(&s->b, "echo TMUX\"\"PANE$((6*7))", "TMUXPANE42", &text);
    net_assert(&s->u, "tmux-pane", text != NULL, "");
    free(text);
    as_root(s, "tmux kill-server; echo CONSOLE\"\"DONE", "tmux-ended", &r);
    if (!net_ok(&s->u)) {
        net_assert(&s->u, "answers-under-tmux", 0, "");
        return;
    }
    net_assert(&s->u, "answers-under-tmux", strstr(r.out, "CONSOLEDONE") != NULL,
               "the machine did not answer with tmux on its console: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
}

/**
 * Put the rows' script on the machine, and the files the row reads beside
 * it: GNU sed's tarball for the autotools row, and for the ntpd row the
 * probe that reads the SNTP server's clock reference, with the server
 * started on the case's second port.
 */
static void stage(struct gnu_case *s, struct etude_case_service **sntp)
{
    char *args[] = {NTP_EPOCH, NULL, NULL};
    char line[160], ntp[16];
    struct etude_termination end;
    struct net_run r;

    if (net_step(&s->u, "stage") == ETUDE_OK &&
        as_root(s, "mkdir -p /tmp/gnu", "mkdir", &r) == ETUDE_OK) {
        if (!net_exited0(&r))
            net_fail(&s->u, "mkdir /tmp/gnu: %s", said(&r, line, sizeof(line)));
        net_run_free(&r);
    }
    if (net_ok(&s->u) && !copy_to(s, "gnu-rows", "/tmp/gnu/rows.sh", "scp.rows"))
        net_fail(&s->u, "scp did not put the rows' script on the machine");
    if (strcmp(s->row, "autotools") == 0 && net_ok(&s->u) &&
        !copy_to(s, "sed-tarball", "/tmp/gnu/sed.tar.xz", "scp.sed"))
        net_fail(&s->u, "scp did not put GNU sed's tarball on the machine");
    if (strcmp(s->row, "ntpd") != 0 || !net_ok(&s->u))
        return;
    if (!copy_to(s, "clockmusl", "/tmp/gnu/clockmusl", "scp.clockmusl")) {
        net_fail(&s->u, "scp did not put the clock reference probe on the machine");
        return;
    }
    if (as_root(s, "chmod 755 /tmp/gnu/clockmusl", "chmod", &r) != ETUDE_OK)
        return;
    if (!net_exited0(&r))
        net_fail(&s->u, "chmod of the clock reference probe: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
    if (!net_ok(&s->u))
        return;
    snprintf(ntp, sizeof(ntp), "%u", etude_port(s->u.c, 1));
    args[1] = ntp;
    if (etude_port(s->u.c, 1) == 0 ||
        etude_service(s->u.c, "sntp", args, sntp, &end) != ETUDE_OK) {
        *sntp = NULL;
        net_fail(&s->u, "the SNTP server did not start");
    }
}

/** Run the row as root under bash, in a session of its own; the ntpd row is
 * given its server, at the user network's gateway. */
static void row(struct gnu_case *s)
{
    char command[256], ok[64], pattern[80], line[160];
    unsigned ntp;
    struct net_run r;

    if (strcmp(s->row, "ntpd") == 0) {
        ntp = etude_port(s->u.c, 1);
        snprintf(command, sizeof(command),
                 "RACH2_NTP_SERVER=10.0.2.2:%u RACH2_NTP_PORT=%u /bin/bash /tmp/gnu/rows.sh %s %s",
                 ntp, ntp, s->row, s->arch);
    } else
        snprintf(command, sizeof(command), "/bin/bash /tmp/gnu/rows.sh %s %s", s->row, s->arch);
    if (net_step(&s->u, "row") == ETUDE_OK)
        as_root(s, command, "row", &r);
    if (!net_ok(&s->u)) {
        net_assert(&s->u, "row", 0, "");
        return;
    }
    snprintf(ok, sizeof(ok), "GNUROW OK %s", s->row);
    snprintf(pattern, sizeof(pattern), "GNUROW FAIL %s", s->row);
    if (has_line(r.out, ok))
        net_assert(&s->u, "row", net_exited0(&r),
                   "the row printed its OK, but its command did not exit with status 0");
    else
        net_assert(&s->u, "row", 0, "%s", line_of(&r, pattern, line, sizeof(line)));
    net_run_free(&r);
}

int etude_scenario(struct etude_case *c)
{
    struct etude_case_service *sntp = NULL;
    struct etude_termination end;
    struct gnu_case s;
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');
    unsigned port;

    memset(&s, 0, sizeof(s));
    net_case(&s.u, c);
    s.row = dot != NULL ? dot + 1 : id;
    s.arch = getenv("ARCH");
    if (s.arch == NULL || (strcmp(s.arch, "i686") != 0 && strcmp(s.arch, "x86_64") != 0))
        net_fail(&s.u, "ARCH names no architecture of the build");
    port = etude_port(c, 0);
    snprintf(s.port, sizeof(s.port), "%u", port);
    if (port == 0)
        net_fail(&s.u, "the case has no port");
    if (strcmp(s.row, "logins") == 0 || strcmp(s.row, "console") == 0)
        net_key(&s.u, "demo-key", s.root_key, sizeof(s.root_key));
    if (strcmp(s.row, "logins") == 0) {
        net_secret(&s.u, "demo-password", s.root_password, sizeof(s.root_password));
        net_secret(&s.u, "test-password", s.test_password, sizeof(s.test_password));
        net_key(&s.u, "test-key", s.test_key, sizeof(s.test_key));
        up(&s, 1);
        keys(&s);
        passwords(&s);
        transfer(&s);
    } else if (strcmp(s.row, "console") == 0) {
        net_secret(&s.u, "demo-password", s.root_password, sizeof(s.root_password));
        up(&s, 0);
        console(&s);
    } else {
        net_secret(&s.u, "demo-password", s.root_password, sizeof(s.root_password));
        up(&s, 0);
        if (net_login(&s.b, "root", s.root_password, "# ") == ETUDE_OK &&
            etude_guest_open(s.b.m) != ETUDE_OK)
            net_fail(&s.u, "the GNU guest command channel did not open");
        stage(&s, &sntp);
        row(&s);
        if (sntp != NULL && etude_service_stop(sntp, &end) != ETUDE_OK)
            net_fail(&s.u, "the SNTP server could not be stopped");
    }
    return net_end(&s.u);
}
