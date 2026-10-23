/*
 * netcase.h: the operations the network scenarios of tests/cases/machine
 * share, over libetude-case: the console's text, lines typed and logins at
 * it, host tools such as ssh run against the guest, and the assertions that
 * follow from them.
 */
#ifndef NETCASE_H
#define NETCASE_H

#include <stddef.h>
#include <stdint.h>

#include "etude/scenario.h"

#ifdef _WIN32
#include "../../../tools/etude/win/files.h"
#endif

/* The debug shell's greeting, at the boot and after each program. */
#define NET_SHELL "Type 'help' for available commands."

/* The words that begin each ssh a network scenario runs: it reads no file of
 * the account that runs it, keeps no record of the host's key, and prints its
 * errors alone. */
#define NET_SSH                                                       \
    "ssh", "-F", "/dev/null", "-o", "StrictHostKeyChecking=no", "-o", \
        "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR"

/* The words after NET_SSH of a login in batch mode by the key at `key`
 * alone. */
#define NET_SSH_KEY(key) "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes", "-i", key

/* A file the scenario wrote in the attempt's evidence itself. */
struct net_made {
    struct net_made *next;
    char path[4096];
};

/*
 * One attempt of a case, and the first operation of it that failed. Once
 * one has failed, every later operation returns its status at once without
 * acting, the next assertion fails with the reason, and those after it are
 * reported not run.
 */
struct net_case {
    struct etude_case *c;
    enum etude_status st;  /* ETUDE_OK until an operation fails */
    char why[200];         /* what failed */
    int told;              /* an assertion has failed with `why` */
    unsigned failures;     /* the assertions that failed */
    struct etude_case_machine *client; /* declared Rach2 SSH client, when used */
    unsigned client_files;
    struct net_made *made; /* the files net_path gave, the newest first */
};

/* One boot of a machine, and how far the scenario has read its console. */
struct net_boot {
    struct net_case *u;
    struct etude_case_machine *m;
    char console[4096]; /* the boot's console evidence, which the library
                           writes as QEMU writes the console */
    uint64_t at;        /* the offset just past the last text waited for */
};

/* A host tool's run: its end and what it wrote. */
struct net_run {
    struct etude_termination end;
    char *out; /* its standard output, from malloc */
    char *err; /* its standard error, from malloc */
};

/** Begin the attempt `u` of case `c`. When the manifest declares the
 * channel-client machine, run its first step of that name and boot it. */
void net_case(struct net_case *u, struct etude_case *c);

/** Say whether every operation of `u` so far succeeded. */
int net_ok(const struct net_case *u);

/** Fail the attempt's work with the reason `format` makes, as a failed
 * operation does. */
void net_fail(struct net_case *u, const char *format, ...);

/** Begin step `step` of the manifest. */
enum etude_status net_step(struct net_case *u, const char *step);

/**
 * Report assertion `id`: held when `held` is nonzero, else failed with the
 * detail `format` makes. After a failed operation it fails, or is not run,
 * as struct net_case says, whatever `held` is.
 */
void net_assert(struct net_case *u, const char *id, int held, const char *format, ...);

/**
 * End the attempt `u`, and give the value etude_scenario returns: 0 when
 * every operation succeeded. When an operation failed and no assertion
 * reported why, write the reason on standard error as `netcase: failed:
 * WHY`. When every assertion held as well, remove the files net_path gave,
 * which the library does not know of; otherwise keep them with the rest of
 * the evidence.
 */
int net_end(struct net_case *u);

/**
 * Give the path of evidence `name`, a file the scenario or a tool it runs
 * writes itself, in `buf` of `size` bytes, and record it for net_end; give
 * NULL after failing the attempt when the name is not a file name or the
 * path does not fit.
 */
const char *net_path(struct net_case *u, const char *name, char *buf, size_t size);

/**
 * Boot `machine` of the case with `disks`, as etude_boot takes them, as
 * the boot `n` of that machine in the attempt, counted from 1. The console
 * is read from its start.
 */
enum etude_status net_boot(struct net_boot *b, struct net_case *u, const char *machine, unsigned n,
                           const char *const disks[]);

/** Wait for `text` after the offset read so far, and move past it. */
enum etude_status net_wait(struct net_boot *b, const char *text);

/** Wait for `text` anywhere on the console since the boot began, and leave
 * the offset read as it was. */
enum etude_status net_seen(struct net_boot *b, const char *text);

/** Wait until `text` has appeared `n` times on the console since the boot
 * began, give the offset just past the last in `at`, and leave the offset
 * read as it was. */
enum etude_status net_times(struct net_boot *b, const char *text, unsigned n, uint64_t *at);

/** Type `line` and a carriage return at the serial console. */
enum etude_status net_line(struct net_boot *b, const char *line);

/**
 * Log in at the console as `user` with `password`: wait for the prompt
 * `login:` after the offset read so far, type the name, wait for
 * `Password:`, type the password, and wait for the shell's `prompt`, such as
 * `# ` for root.
 */
enum etude_status net_login(struct net_boot *b, const char *user, const char *password,
                            const char *prompt);

/**
 * Type `command` at the shell and wait for `marker`, which the command
 * prints at its end, and give the console between the typed line and the
 * marker in `text`, from malloc. Write the marker in the command with an
 * empty pair of quotes inside it, `DONE""MARK`, so that the line's echo is
 * not taken for it.
 */
enum etude_status net_command(struct net_boot *b, const char *command, const char *marker,
                              char **text);

/**
 * Run host tool or artifact `argv[0]` with its arguments. ssh, scp, sftp
 * and ptydrive instead run on the declared channel-client Rach2 machine,
 * with their local files transferred through its framed channel. Keep
 * standard output as evidence `output` and standard error as
 * `output`.err, and give its end and both texts in `r`, whatever its exit
 * status. Free them with net_run_free.
 */
enum etude_status net_host(struct net_case *u, char *const argv[], const char *output,
                           struct net_run *r);

/** Run an installed guest program through its already-open channel and
 * read the ordinary result evidence into R. */
enum etude_status net_guest(struct net_case *u, struct etude_case_machine *m,
                            const char *const argv[], const char *const env[],
                            const char *input, const char *output, struct net_run *r);

/** Run an SSH client on the declared channel-client Rach2 machine, with
 * INPUT, a host file, as its standard input. net_host selects the same path
 * for ssh, scp, sftp and ptydrive. The actual SSH protocol still connects to
 * the server through its forwarded port. */
enum etude_status net_ssh_input(struct net_case *u, char *const argv[], const char *input,
                                const char *output, struct net_run *r);

/** Start an SSH client workload and wait for READY on its output. Collect
 * it with net_ssh_finish, while other tools may run in between. */
enum etude_status net_ssh_start(struct net_case *u, char *const argv[], const char *ready);
enum etude_status net_ssh_finish(struct net_case *u, const char *output, struct net_run *r);

/** Free the texts of `r`. */
void net_run_free(struct net_run *r);

/** Say whether `r` exited 0. */
int net_exited0(const struct net_run *r);

/**
 * Give the first line of artifact `name`, a password, in `buf` of `size`
 * bytes; fail the attempt when it is missing, empty or does not fit.
 */
const char *net_secret(struct net_case *u, const char *name, char *buf, size_t size);

/**
 * Make a private copy of artifact `name` that only its owner can read, as
 * ssh requires of a private key, and give its path in `buf` of `size`
 * bytes, or NULL after failing the attempt.
 */
const char *net_key(struct net_case *u, const char *name, char *buf, size_t size);

/** Give the console's bytes from `from` to `to`, from malloc, with a
 * terminating NUL; an empty text when they cannot be read. */
char *net_text(const struct net_boot *b, uint64_t from, uint64_t to);

/** Give the bytes of the file at `path`, from malloc, with a terminating
 * NUL; an empty text when it cannot be read. */
char *net_file(const char *path);

/**
 * Give the first line of `text` that has `pattern` in it, or NULL: each `*`
 * of the pattern matches any run of bytes, each `#` a run of one or more
 * digits, and every other byte itself. A line ends at a carriage return or
 * a newline.
 */
const char *net_find(const char *text, const char *pattern);

/** Count the lines of `text` that have `pattern` in them. */
unsigned net_count(const char *text, const char *pattern);

/** Copy the line that begins at `line` into `buf` of `size` bytes, without
 * its end, and give `buf`; an empty text for NULL. */
const char *net_copy_line(const char *line, char *buf, size_t size);

/** Give the number after the last `prefix` in `text`, or -1 when there is
 * none or no digit follows it. */
long net_number_after(const char *text, const char *prefix);

#endif
