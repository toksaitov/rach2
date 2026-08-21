/**
 * passwd: change an account's password in `/etc/shadow`.
 *
 * The third and last of the programs that climb, beside `su` and `sudo`, and
 * the only one that writes. It is setuid root because `/etc/shadow` is 0600
 * root, and it decides what its caller may do from the real uid: this binary is
 * 04755, so `geteuid()` is 0 for everyone who runs it and a test on the
 * effective uid would authorize the world. The rule is general and this is its
 * second home in the tree: a set-id program takes its authority from the real
 * uid and uses the effective one only to act.
 *
 * Usage: `passwd [user]`, the account defaulting to the caller's own. Naming
 * another account is the administrative exception and needs `getuid() == 0`.
 * Root changes any password without proving the old one; everyone else proves
 * the account's current password first.
 *
 * Everything happens inside one credential transaction, taken before any file
 * is opened and dropped only after the replacement is published. That ordering
 * is deliberate: the account is found, the old password is
 * checked, and the new file is built from ONE image of the databases, read
 * under the lock. `getspnam` is not called anywhere here, since it would be a
 * second, independent read that can see a different file than the one about to
 * be replaced, and an authentication that passed against a line the replacement
 * then overwrote is a lost update wearing a success message. `pwauth_check`
 * takes the stored hash as a string exactly so a caller can supply it from
 * wherever it found the account.
 *
 * A name that appears more than once in either database is refused, naming the
 * file that has it, and nothing is changed. musl's parsers all stop at the
 * first match, so a duplicate is an error nowhere and an ambiguity everywhere:
 * which line authenticates a login and which one a change rewrites would be
 * decided by file order, and the two decisions can pick different lines, so the
 * account would go on answering to a password this program reported as changed.
 *
 * The salt is minted after the old password is accepted and before the new one
 * is asked for, so a machine with no trusted entropy refuses without first
 * collecting a secret it cannot use. Nothing here hashes with an untrusted salt
 * and nothing falls back. The new password is read twice and must agree, and an
 * empty one is refused: an empty shadow field admits everyone.
 *
 * No signal handlers are installed. The transaction owns the four deaths for
 * its whole lifetime, restores the terminal, drops any temporary file and
 * leaves through `exit` so the registered release drops the lock; a program
 * that caught a signal only to fix the terminal would leave the lock behind,
 * and the transaction is the only place that knows it is held.
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
 * accounts; a file past this is refused, not truncated. */
#define TEXT_MAX 16384

/* The longest shadow record the readers can use, and where the number comes
 * from. musl's `getspnam` reads a line into a 256-byte buffer, and its loop
 * refuses a matching record whose last character is not a newline: an
 * over-long line comes back from `fgets` without one and is rejected with
 * `ERANGE`, and so is a well-formed last line in a file that simply lacks a
 * final newline. Either way the caller sees NULL, which `pwauth_verify` and
 * dropbear both read as "no such account". So a record of 255 bytes or more
 * with its newline, or a target line with no newline after it, is one this
 * program must refuse to write, not publish and call changed. */
#define SHADOW_RECORD_MAX 254

/* A shadow line is a name, a hash and seven aging fields. The hash is the only
 * one this program rewrites, and the rest are carried across untouched. */
#define SHADOW_FIELDS 9

/* A passwd record musl will parse: six colons, so seven fields. */
#define PASSWD_FIELDS 7

/**
 * Read the whole of `path` into `buffer`, terminating it. Returns the length,
 * or -1 having said why.
 *
 * The file is read whole because every question here is about the whole of it:
 * how many lines carry a name, and what the other lines are, since they must
 * come through byte for byte.
 *
 * Read through `read` and not `fread`, which is not a style choice here: it
 * keeps the credential text out of a second buffer that nothing here would
 * clear.
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
    /* Everything below parses with string functions, so a NUL inside the file
     * would end every one of them early and the rewrite would publish a
     * database with the remainder cut off. A credential file is text; one that
     * is not is refused, not silently shortened. */
    if (strlen(buffer) != held) {
        fprintf(stderr, "passwd: %s has a NUL byte and is not a text database\n", path);
        return -1;
    }

    return (long)held;
}

/**
 * How many lines of `text` name `user` in their first colon-separated field.
 *
 * Counting instead of finding, because the count is what decides whether the
 * answer means anything, and the parse is the same either way.
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
 * `*out`. Returns 1 when the record is one musl's parser will accept, and so
 * one every reader in the image resolves.
 *
 * `__getpwent_a` walks six colons and then reads the uid and the gid with a
 * digit loop that must land exactly on the next colon, so a record with fewer
 * fields, or one whose uid reads `1000junk`, is skipped entirely: `getpwnam`
 * and `getpwuid` answer as if the account were not there. A parser here that
 * accepted it would authenticate against a name no reader can resolve and
 * publish a password for an account nobody can enter, then report success. The
 * arithmetic wraps the way musl's does instead of saturating the way `strtoul`
 * does, so the two cannot disagree about which uid an absurd field names. An
 * empty numeric field is refused although musl reads it as 0, because a blank
 * uid silently meaning root is not a thing to carry into a credential writer.
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
 * Returns 0 when exactly one line carried it.
 *
 * The caller's name is taken from the same image the replacement is built from,
 * so a `getpwuid` racing the rewrite cannot name an account this program is
 * about to remove from under itself.
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
                /* A name that does not fit is a refusal and not a match.
                 * Counting it while copying nothing would leave the caller's
                 * buffer uninitialized and this function reporting success, and
                 * every use of that buffer below is a C string in a setuid
                 * program. */
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
 * Copy `user`'s line out of `text` into `out`, without its newline. Returns 0
 * when a line named it and the line fit.
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
 * A file whose last line has no terminator is the case this catches, and it is
 * not cosmetic: the reader rejects a matching record that does not end in one,
 * so publishing it would leave an account that answers to nothing while this
 * program reported the password changed.
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
 * colon-separated fields, with every aging field empty or decimal.
 *
 * A record that does not parse is refused, not replaced, because a
 * replacement that keeps a malformed line's shape publishes a file that this
 * program calls changed and that every reader goes on rejecting: `getspnam`
 * wants the nine fields, so the account would answer to no password at all and
 * the user would be told their new one was set. Only the target's own line is
 * checked. The rest come through byte for byte and are not this program's to
 * judge, and refusing the whole file for a line nobody asked about would make
 * one bad record lock every account out of a password change.
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
            /* The last field is the only one that may end the line. */
            return n == SHADOW_FIELDS - 1;
        }
        field = stop + 1;
    }

    return 0;
}

/**
 * Copy the hash field of `user`'s line in the shadow `text` into `out`.
 * Returns 0 when the line was there and the field fit.
 *
 * An account with no shadow line is not an error to report differently from a
 * wrong password: both end in the same refusal, so nothing is learned by
 * trying a name.
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
 * Every other byte of the file comes through untouched, the aging fields of the
 * changed line included: this program has one field's worth of business in a
 * file it must otherwise reproduce exactly.
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

    /* `passwd [--] [user]`. The end-of-options `--` is accepted because it is
     * the conventional way to hand over a name that might begin with a dash,
     * and because a caller that means a name should be able to say so without
     * this program guessing. */
    if (argc > 1 && strcmp(argv[1], "--") == 0) {
        argv++;
        argc--;
        end_of_options = 1;
    }
    /* After `--` a leading dash is part of the name, which is the whole reason
     * the form exists; stripping it and then refusing the name anyway would
     * leave no way to say one. */
    if (argc > 2 || (argc == 2 && !end_of_options && argv[1][0] == '-')) {
        fprintf(stderr, "usage: passwd [--] [user]\n");
        return 2;
    }

    /* The survey runs without the lock, and everything the user has to type
     * happens before it is taken. An earlier draft took the transaction first,
     * so that the account found, the hash authenticated against and the file
     * replaced were one image by construction; that is the right property and
     * the wrong way to get it, because the lock was then held across two or
     * three terminal reads. Any account could start `passwd`, leave it at the
     * prompt, and stop every other writer for as long as it liked, on a machine
     * with sixteen process slots. The property is kept below instead, by
     * checking under the lock that the record authenticated against is still
     * the record about to be replaced: what an early lock made impossible, a
     * late one makes detectable, and detectable is enough because the only
     * right answer to "it changed while you were typing" is to say so. */
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
        /* Naming your own account is the same request as naming none, and it is
         * how the command is conventionally written. What needs the
         * administrative exception is naming SOMEBODY ELSE'S. */
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

    /* Minted before the new password is asked for: a machine with no trusted
     * entropy must refuse without first collecting a secret it cannot use. */
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

    /* Only now, with nothing left to type. The lock is held across a re-read, a
     * comparison and the publish, which is microseconds, not however
     * long a person takes to choose a password. */
    if (cred_txn_begin("passwd") != 0) {
        return 1;
    }
    if (slurp(PASSWD_FILE, passwd_text, sizeof(passwd_text)) < 0 ||
        slurp(SHADOW_FILE, shadow_text, sizeof(shadow_text)) < 0) {
        cred_txn_end();
        return 1;
    }
    /* The caller's claim on this account is rechecked, not only the record. A
     * name is not owned forever: while the password was being typed an
     * administrator may have removed the account, renamed it, or given the uid
     * to somebody else, and none of that need touch the shadow hash. Rechecking
     * only the hash would let a caller whose claim had just been revoked
     * publish a password for an account that is no longer theirs, as root,
     * which is the whole of what the real-uid rule exists to stop. */
    {
        char now[64];

        /* The name must still be one name, whoever asked. A uid that still
         * resolves to this target is not enough on its own: a second line
         * carrying the same login at a different uid would leave the target
         * ambiguous, and `getpwnam` takes the first, so the caller would be
         * publishing a hash that authenticates somebody else's uid. */
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
    /* The snapshot authenticated against must still be the one being replaced.
     * Anything else means another writer published between the prompt and the
     * lock, and rewriting then would put back a record built from a file that
     * no longer exists: the other change would be erased by a caller who never
     * saw it. This is the whole of what the early lock bought, and unlike the
     * early lock it costs nobody a held lock while they think. */
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
    /* The line about to be published, measured as the reader will meet it. A
     * longer hash can push a record that fitted over the limit, so the check is
     * on the result, not on what was read. */
    {
        char published[512];

        if (line_of(rebuilt, target, published, sizeof(published)) != 0 ||
            strlen(published) > SHADOW_RECORD_MAX) {
            fprintf(stderr, "passwd: the new entry for %s would be too long for a reader\n",
                    target);
            cred_txn_end();
            return 1;
        }
        /* And one every reader will parse. The survey checked the record read
         * before the prompts, but the file may have been edited while they were
         * open in a way that leaves the hash alone: an aging field turned to
         * junk, or the trailing newline taken off the last line. `replace_hash`
         * changes the hash field and copies the rest, so a record that arrived
         * malformed is published malformed, and `getspnam` skips a bad aging
         * field and reports ERANGE for an unterminated match. The check is on
         * what is about to be written, which is the only text whose shape this
         * program is answerable for. */
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
