/**
 * passwd: change an account's password in `/etc/shadow`.
 *
 * Of the set-id programs here, `su`, `sudo` and this one, it is the only one
 * that writes. It is setuid root because `/etc/shadow` is 0600 root, and it
 * decides what its caller may do from the real uid: this binary is 04755, so
 * `geteuid()` is 0 for everyone who runs it, and a test of the effective uid
 * would let everyone through. `sudo` follows the same rule: a set-id program
 * takes its authority from the real uid and uses the effective one only to
 * act.
 *
 * Usage: `passwd [--] [user]`, the account defaulting to the caller's own.
 * Naming another account needs a real uid of 0. Root changes any password
 * without giving the old one; everyone else gives the account's current
 * password first.
 *
 * The program reads and authenticates before it takes the credential lock.
 * Under the lock it reads both databases again and checks again that the
 * target is named once, that a caller other than root still has the target's
 * uid, and that the stored hash is the one read before the prompts. It builds
 * the replacement from the current shadow file, keeping every other field,
 * and checks the resulting record before it publishes it.
 *
 * A name that appears more than once in either database is an error, which
 * names the file that has it, and nothing is changed. musl's parsers all stop
 * at the first match, so which line authenticates a login and which one a
 * change rewrites would be decided by file order, the two can be different
 * lines, and the account would go on taking a password this program reported
 * as changed.
 *
 * The salt is made after the old password is accepted and before the new one
 * is asked for, so a machine with no trusted entropy fails before it collects
 * a secret it cannot use. Nothing here hashes with an untrusted salt, and
 * nothing falls back to one. The new password is read twice and must match,
 * and an empty one is not accepted, since it would let anyone in.
 *
 * The credential library manages the signal dispositions during password
 * input and during the transaction.
 */

#define _GNU_SOURCE /* crypt */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/types.h>

#include "pwauth.h"

#define PASSWD_FILE "/etc/passwd"
#define SHADOW_FILE "/etc/shadow"

/* Room for either database read whole. The staged images hold a handful of
 * accounts; a larger file is an error, not truncated. */
#define TEXT_MAX 16384

/* The longest shadow record, without its newline, that the readers can use.
 * musl's `getspnam` reads a line into a 256-byte buffer and fails with
 * `ERANGE` on a matching record whose last character is not a newline: a line
 * too long comes back from `fgets` without one, and so does a well-formed last
 * line in a file that has no final newline. Either way the caller gets NULL,
 * which `pwauth_verify` and Dropbear both take as "no such account". So a
 * record of 255 bytes or more before its newline, or a target line with no
 * newline after it, is one this program must not publish and call changed. */
#define SHADOW_RECORD_MAX 254

/* A shadow line is a name, a hash and seven numeric fields. The hash is the
 * only one this program rewrites, and the rest are copied unchanged. */
#define SHADOW_FIELDS 9

/* A passwd record musl will parse: six colons, so seven fields. */
#define PASSWD_FIELDS 7

/**
 * Read the whole of `path` into `buffer`, of `size` bytes, terminating it.
 * Returns the length, or -1 after a diagnostic for a file that cannot be
 * read, does not fit, or has a NUL byte.
 *
 * The file is read whole because every question here is about the whole of it:
 * how many lines have a name, and what the other lines are, since they must
 * come through byte for byte.
 *
 * Read through `read` and not `fread`, to keep the credential text out of a
 * second buffer that nothing here would clear. A file with a NUL byte is an
 * error, not shortened: every parse below stops at the first NUL, so the
 * rewrite would publish the database with the rest cut off.
 */
static long slurp(const char *path, char *buffer, size_t size)
{
    size_t held = 0;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        fprintf(stderr, "passwd: cannot read %s\n", path);
        return -1;
    }
    for (;;) {
        ssize_t got = read(fd, buffer + held, size - held);

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            fprintf(stderr, "passwd: cannot read %s\n", path);
            return -1;
        }
        if (got == 0) {
            break;
        }
        held += (size_t)got;
        if (held + 1 >= size) {
            close(fd);
            fprintf(stderr, "passwd: %s is too large to rewrite safely\n", path);
            return -1;
        }
    }
    close(fd);
    buffer[held] = '\0';
    if (strlen(buffer) != held) {
        fprintf(stderr, "passwd: %s has a NUL byte and is not a text database\n", path);
        return -1;
    }

    return (long)held;
}

/**
 * How many lines of `text` name `user` in their first colon-separated field.
 *
 * It counts rather than finds, because only a count of one makes the line
 * found mean anything, and the parse is the same either way.
 */
static int count_entries(const char *text, const char *user)
{
    size_t len = strlen(user);
    const char *line = text;
    int found = 0;

    while (*line) {
        const char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);

        if (span > len && strncmp(line, user, len) == 0 && line[len] == ':') {
            found++;
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }

    return found;
}

/**
 * Whether `text` is a non-empty run of decimal digits.
 */
static int all_digits(const char *text)
{
    const char *walk = text;

    if (*walk == '\0') {
        return 0;
    }
    while (*walk) {
        if (*walk < '0' || *walk > '9') {
            return 0;
        }
        walk++;
    }

    return 1;
}

/**
 * Read the uid out of the `n` colon-separated fields of a passwd record into
 * `*out`. Returns 1 when the record is one musl's parser accepts, and so one
 * every reader in the image resolves, and 0 otherwise.
 *
 * `__getpwent_a` needs six colons, and reads the uid and the gid with a digit
 * loop that must end exactly at the next colon, so a record with fewer
 * fields, or one whose uid reads `1000junk`, is skipped entirely: `getpwnam`
 * and `getpwuid` answer as if the account were not there. A parser here that
 * accepted it would authenticate against a name no reader can resolve and
 * publish a password for an account nobody can enter, then report success. The
 * arithmetic wraps as musl's does instead of saturating as `strtoul` does, so
 * the two agree about which uid an absurd field names. An empty numeric field
 * is rejected although musl reads it as 0, because a credential writer should
 * not take a blank uid to mean root.
 */
static int record_uid(char *const *field, int n, uid_t *out)
{
    const char *walk;
    unsigned long value = 0;

    if (n != PASSWD_FIELDS || !all_digits(field[2]) || !all_digits(field[3])) {
        return 0;
    }
    for (walk = field[2]; *walk; walk++) {
        value = value * 10 + (unsigned long)(*walk - '0');
    }
    *out = (uid_t)value;

    return 1;
}

/**
 * Copy the name of the account whose uid is `uid` out of `text` into `out`.
 * Returns 0 when exactly one line has it, and -1 when none or several do or
 * the name does not fit in `out`; a name too long returns -1, not a match, so
 * a 0 always leaves `out` set. A line of 512 bytes or more is skipped.
 *
 * The caller's name is taken from the same text the replacement is built
 * from, so it agrees with that text, where a `getpwuid` racing another
 * writer's rewrite could answer from a different file.
 */
static int name_of_uid(const char *text, uid_t uid, char *out, size_t size)
{
    const char *line = text;
    int found = 0;

    while (*line) {
        const char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);
        char copy[512];
        char *field[PASSWD_FIELDS];
        int n = 0;
        char *walk;
        uid_t parsed = 0;

        if (span > 0 && span < sizeof(copy)) {
            memcpy(copy, line, span);
            copy[span] = '\0';
            walk = copy;
            while (n < PASSWD_FIELDS) {
                field[n++] = walk;
                walk = strchr(walk, ':');
                if (!walk) {
                    break;
                }
                *walk++ = '\0';
            }
            if (record_uid(field, n, &parsed) && parsed == uid) {
                if (strlen(field[0]) >= size) {
                    return -1;
                }
                if (found++ == 0) {
                    snprintf(out, size, "%s", field[0]);
                }
            }
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }

    return found == 1 ? 0 : -1;
}

/**
 * Copy the first line of `text` that names `user` into `out`, without its
 * newline. Returns 0 when a line named it and the line fit, and -1 otherwise.
 */
static int line_of(const char *text, const char *user, char *out, size_t size)
{
    size_t len = strlen(user);
    const char *line = text;

    while (*line) {
        const char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);

        if (span > len && strncmp(line, user, len) == 0 && line[len] == ':') {
            if (span >= size) {
                return -1;
            }
            memcpy(out, line, span);
            out[span] = '\0';

            return 0;
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }

    return -1;
}

/**
 * True when `user`'s line in `text` is followed by a newline.
 *
 * It catches a file whose last line has no terminator: the reader rejects a
 * matching record that does not end in one, so publishing it would leave an
 * account that no password opens while this program reported the password
 * changed.
 */
static int line_terminated(const char *text, const char *user)
{
    size_t len = strlen(user);
    const char *line = text;

    while (*line) {
        const char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);

        if (span > len && strncmp(line, user, len) == 0 && line[len] == ':') {
            return end != NULL;
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }

    return 0;
}

/**
 * True when `line` is a shadow record this program may rewrite: nine
 * colon-separated fields, the ninth ending the line, with every field after
 * the hash empty or decimal.
 *
 * A record that does not parse is left as it is, not replaced, because a
 * replacement that keeps a malformed line's shape publishes a file that this
 * program calls changed and that every reader goes on rejecting: `getspnam`
 * needs the nine fields, so the account would take no password at all and the
 * user would be told their new one was set. Only the target's own line is
 * checked. The rest come through byte for byte and are not this program's to
 * judge, and failing the whole file for a line nobody asked about would let
 * one bad record stop every account's password change.
 */
static int shadow_line_ok(const char *line)
{
    const char *field = line;
    int n;

    for (n = 0; n < SHADOW_FIELDS; n++) {
        const char *stop = strchr(field, ':');
        size_t span = stop ? (size_t)(stop - field) : strlen(field);

        if (n >= 2) {
            size_t i;

            for (i = 0; i < span; i++) {
                if (field[i] < '0' || field[i] > '9') {
                    return 0;
                }
            }
        }
        if (!stop) {
            return n == SHADOW_FIELDS - 1;
        }
        field = stop + 1;
    }

    return 0;
}

/**
 * Copy the hash field of the first line of the shadow `text` that names
 * `user` into `out`. Returns 0, or -1 if the account is absent or the hash
 * does not fit.
 */
static int hash_of(const char *text, const char *user, char *out, size_t size)
{
    size_t len = strlen(user);
    const char *line = text;

    while (*line) {
        const char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);

        if (span > len && strncmp(line, user, len) == 0 && line[len] == ':') {
            const char *hash = line + len + 1;
            const char *stop = memchr(hash, ':', span - len - 1);
            size_t hlen = stop ? (size_t)(stop - hash) : span - len - 1;

            if (hlen >= size) {
                return -1;
            }
            memcpy(out, hash, hlen);
            out[hlen] = '\0';

            return 0;
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }

    return -1;
}

/**
 * Write into `out` the shadow `text` with `user`'s hash field replaced by
 * `hash`. Returns the length, or -1 when it would not fit.
 *
 * Every other byte of the file comes through unchanged, the other fields of
 * the changed line included: this program changes one field of a file it
 * must otherwise reproduce exactly.
 */
static long replace_hash(const char *text, const char *user, const char *hash, char *out,
                         size_t size)
{
    size_t len = strlen(user);
    const char *line = text;
    size_t off = 0;

    while (*line) {
        const char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);
        int mine = span > len && strncmp(line, user, len) == 0 && line[len] == ':';

        if (mine) {
            const char *rest = memchr(line + len + 1, ':', span - len - 1);
            size_t tail = rest ? span - (size_t)(rest - line) : 0;

            if (off + len + 1 + strlen(hash) + tail + 1 >= size) {
                return -1;
            }
            memcpy(out + off, line, len + 1);
            off += len + 1;
            memcpy(out + off, hash, strlen(hash));
            off += strlen(hash);
            if (rest) {
                memcpy(out + off, rest, tail);
                off += tail;
            }
        } else {
            if (off + span + 1 >= size) {
                return -1;
            }
            memcpy(out + off, line, span);
            off += span;
        }
        if (end) {
            out[off++] = '\n';
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }
    out[off] = '\0';

    return (long)off;
}

/**
 * Change the password of the caller's account, or of the one named:
 * `passwd [--] [user]`. The end-of-options `--` hands over a name that begins
 * with a dash, which is otherwise a usage error. Naming one's own account is
 * the same request as naming none; naming another needs real uid 0.
 *
 * The lock is taken once nothing is left to type, and held across a second
 * read, a second check and the publish, which take microseconds, not however
 * long a person takes to choose a password. Under it the caller's claim is
 * checked again, not only the record: a caller that is not root must still
 * have the target's uid, since while the password was typed the account may
 * have been removed, renamed or had its uid given to somebody else. The target
 * must still be named by exactly one passwd line, since `getpwnam` takes the
 * first of two and the hash would then authenticate somebody else's uid, and
 * its stored hash must be the one read before the prompts.
 *
 * The record about to be published is measured and parsed as a reader will
 * read it: a longer hash can push it past `SHADOW_RECORD_MAX`, and the file
 * may have been edited meanwhile in a way that leaves the hash alone (an aging
 * field turned to junk, the last newline removed), which `replace_hash` would
 * copy through.
 *
 * Exits 0 when the password was changed and the transaction ended cleanly, 2
 * for a usage error, and 1 otherwise, a change that is live but not flushed
 * included.
 */
int main(int argc, char **argv)
{
    static char passwd_text[TEXT_MAX];
    static char shadow_text[TEXT_MAX];
    static char rebuilt[TEXT_MAX];
    char user[64];
    char stored[256];
    char current[256];
    char salt[PWAUTH_SALT_LEN + 1];
    char setting[PWAUTH_SALT_LEN + 8];
    char first[PWAUTH_MAX_PASSWORD + 1], second[PWAUTH_MAX_PASSWORD + 1];
    const char *target;
    char *hashed;
    long length;
    int rc, ended;
    int end_of_options = 0;

    if (argc > 1 && strcmp(argv[1], "--") == 0) {
        argv++;
        argc--;
        end_of_options = 1;
    }
    if (argc > 2 || (argc == 2 && !end_of_options && argv[1][0] == '-')) {
        fprintf(stderr, "usage: passwd [--] [user]\n");
        return 2;
    }

    if (slurp(PASSWD_FILE, passwd_text, sizeof(passwd_text)) < 0 ||
        slurp(SHADOW_FILE, shadow_text, sizeof(shadow_text)) < 0) {
        return 1;
    }

    if (name_of_uid(passwd_text, getuid(), user, sizeof(user)) != 0) {
        fprintf(stderr, "passwd: no account for uid %d\n", (int)getuid());
        return 1;
    }
    target = user;
    if (argc == 2) {
        if (getuid() != 0 && strcmp(argv[1], user) != 0) {
            fprintf(stderr, "passwd: only root may change another account's password\n");
            return 1;
        }
        target = argv[1];
    }

    if (count_entries(passwd_text, target) != 1) {
        fprintf(stderr, "passwd: %s names %s more than once\n", PASSWD_FILE, target);
        return 1;
    }
    if (count_entries(shadow_text, target) != 1) {
        fprintf(stderr, "passwd: %s names %s more than once\n", SHADOW_FILE, target);
        return 1;
    }
    if (hash_of(shadow_text, target, stored, sizeof(stored)) != 0) {
        fprintf(stderr, "passwd: no shadow entry for %s\n", target);
        return 1;
    }
    {
        char record[512];

        if (line_of(shadow_text, target, record, sizeof(record)) != 0 || !shadow_line_ok(record)) {
            fprintf(stderr, "passwd: %s: the entry for %s is not a shadow record\n", SHADOW_FILE,
                    target);
            return 1;
        }
        if (!line_terminated(shadow_text, target)) {
            fprintf(stderr,
                    "passwd: %s: the entry for %s has no newline after it, which no "
                    "reader accepts\n",
                    SHADOW_FILE, target);
            return 1;
        }
    }

    if (getuid() != 0) {
        char old[PWAUTH_MAX_PASSWORD + 1];

        if (pwauth_read_password("Old password: ", old, sizeof(old)) != 0 ||
            !pwauth_check(stored, old)) {
            fprintf(stderr, "passwd: authentication failure\n");
            return 1;
        }
    }

    if (pwauth_mint_salt(salt, sizeof(salt)) != 0) {
        fprintf(stderr, "passwd: password unchanged\n");
        return 1;
    }

    if (pwauth_read_password("New password: ", first, sizeof(first)) != 0 ||
        pwauth_read_password("Retype new password: ", second, sizeof(second)) != 0) {
        fprintf(stderr, "passwd: password unchanged\n");
        return 1;
    }
    if (strcmp(first, second) != 0) {
        fprintf(stderr, "passwd: the two entries differ; password unchanged\n");
        return 1;
    }
    if (first[0] == '\0') {
        fprintf(stderr, "passwd: an empty password would admit everyone; password unchanged\n");
        return 1;
    }

    snprintf(setting, sizeof(setting), "$6$%s$", salt);
    hashed = crypt(first, setting);
    if (!hashed || hashed[0] != '$') {
        fprintf(stderr, "passwd: cannot hash the new password\n");
        return 1;
    }

    if (cred_txn_begin("passwd") != 0) {
        return 1;
    }
    if (slurp(PASSWD_FILE, passwd_text, sizeof(passwd_text)) < 0 ||
        slurp(SHADOW_FILE, shadow_text, sizeof(shadow_text)) < 0) {
        cred_txn_end();
        return 1;
    }
    {
        char now[64];

        if (count_entries(passwd_text, target) != 1) {
            fprintf(stderr, "passwd: %s no longer names %s once; password unchanged\n", PASSWD_FILE,
                    target);
            cred_txn_end();
            return 1;
        }
        if (getuid() != 0 && (name_of_uid(passwd_text, getuid(), now, sizeof(now)) != 0 ||
                              strcmp(now, target) != 0)) {
            fprintf(stderr, "passwd: uid %d no longer names %s; password unchanged\n",
                    (int)getuid(), target);
            cred_txn_end();
            return 1;
        }
    }
    if (count_entries(shadow_text, target) != 1 ||
        hash_of(shadow_text, target, current, sizeof(current)) != 0) {
        fprintf(stderr, "passwd: %s: %s is no longer named once there\n", SHADOW_FILE, target);
        cred_txn_end();
        return 1;
    }
    if (strcmp(current, stored) != 0) {
        fprintf(stderr,
                "passwd: %s changed while the password was being entered; "
                "password unchanged\n",
                SHADOW_FILE);
        cred_txn_end();
        return 1;
    }

    length = replace_hash(shadow_text, target, hashed, rebuilt, sizeof(rebuilt));
    if (length < 0) {
        fprintf(stderr, "passwd: the new %s would not fit\n", SHADOW_FILE);
        cred_txn_end();
        return 1;
    }
    {
        char published[512];

        if (line_of(rebuilt, target, published, sizeof(published)) != 0 ||
            strlen(published) > SHADOW_RECORD_MAX) {
            fprintf(stderr, "passwd: the new entry for %s would be too long for a reader\n",
                    target);
            cred_txn_end();
            return 1;
        }
        if (!shadow_line_ok(published) || !line_terminated(rebuilt, target)) {
            fprintf(stderr, "passwd: %s: %s's record changed shape; password unchanged\n",
                    SHADOW_FILE, target);
            cred_txn_end();
            return 1;
        }
    }

    rc = cred_rewrite(SHADOW_FILE, rebuilt, (size_t)length);
    ended = cred_txn_end();
    if (rc == CRED_UNCHANGED) {
        fprintf(stderr, "passwd: password unchanged\n");
        return 1;
    }
    if (rc == CRED_UNFLUSHED) {
        fprintf(stderr, "passwd: the new password is live but may not survive a power cut\n");
        return 1;
    }
    if (ended != 0) {
        fprintf(stderr,
                "passwd: the password was changed but the transaction did not end cleanly\n");
        return 1;
    }
    printf("passwd: password changed for %s\n", target);

    return 0;
}
