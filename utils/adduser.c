/**
 * adduser: create an account in `/etc/shadow`, `/etc/group` and `/etc/passwd`.
 *
 * Root's, and deliberately not setuid: it refuses a caller whose `getuid()` is
 * not 0 before it opens anything. Carrying no set-id bit means its real and
 * effective uids are the same and there is nothing here to confuse, but it
 * tests the real uid all the same, because that is the rule every program in
 * this tree that decides an identity question follows.
 *
 * Usage: `adduser [-u number] [-s shell] name`. The shell defaults to
 * `/bin/sh` and is not checked for existence, since an account may legitimately
 * be created before the program it logs in to is staged.
 *
 * It writes three databases and a home directory and holds one transaction
 * across all four. Every check is made against the one image read under the
 * lock: the name must be absent from all three files, the number must be free
 * as a uid and as a gid, and with no `-u` the lowest number at or above
 * `ADDUSER_FIRST_UID` free in both is chosen. Choosing a number outside the
 * lock is exactly how two concurrent runs hand out the same uid, so the read,
 * the choice and the writes are one transaction rather than three.
 *
 * `/etc/shadow` is in the absence check and not only in the write list. It is
 * published first, so a crash after that one rename leaves the name there and
 * nowhere else, and a run that tested only passwd and group would pass both
 * tests and append the name a second time. Nothing would then report it:
 * musl's parsers all stop at the first match, so which of two shadow lines
 * authenticates a login, and which one `passwd` rewrites, is decided by file
 * order, and the two can differ.
 *
 * The publication order is chosen so that every prefix of it is a consistent
 * system, because the lock excludes another writer and only the order survives
 * a crash: KRFFS has no journal, a replacement publishes one file at a time,
 * and a failure part way through publishes nothing further while undoing
 * nothing already published. So: shadow with a locked `!` field, then group,
 * then the home directory, then passwd. The account becomes visible only at the
 * last step, so no interruption leaves a passwd field of `x` with no shadow
 * entry, nor a gid that names no group. What each prefix means and how root
 * clears it is in `utils/Readme.md`.
 *
 * It never resumes a partial account and never rolls one back. Resuming would
 * mean deciding that a partial state is a residue rather than something an
 * operator wrote by hand, and adopting a uid it did not choose; rolling back
 * would mean a second series of replacements, each able to fail the way the
 * first did, on files another writer may since have changed. So a name present
 * in some of the three files is refused, naming exactly the files that hold it.
 *
 * The transaction is dropped before the password program is exec'd, which is
 * required rather than tidy: the lock is a file, it outlives `execve`, and a
 * transaction carried across the exec would deadlock `passwd` against its own
 * predecessor. Until that `passwd` succeeds the field stays `!` and the account
 * cannot be entered, which is why shadow is written first and why a failure
 * there needs nothing undone.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "pwauth.h"

#define PASSWD_FILE "/etc/passwd"
#define SHADOW_FILE "/etc/shadow"
#define GROUP_FILE  "/etc/group"

/* Room for a database read whole; a file past this is refused, not truncated. */
#define TEXT_MAX 16384

/* Where an automatic number search starts, and where it gives up. */
#define ADDUSER_FIRST_UID 1000
#define ADDUSER_LAST_UID  60000

/* The longest record any reader in the image will take, which is musl's
 * `getspnam` line buffer less its terminator and its newline. */
#define RECORD_MAX 254

/* The password program, and where it is staged. os-real has a `/bin` and the
 * flat test image does not, so a program that must run under both looks in
 * both, in that order. */
#define PASSWD_PROGRAM      "/bin/passwd"
#define PASSWD_PROGRAM_FLAT "/passwd"

/* What has been published, in order, so a failure can say what it left. */
#define PUBLISHED_SHADOW 1
#define PUBLISHED_GROUP  2
#define PUBLISHED_HOME   3
#define PUBLISHED_PASSWD 4

/**
 * Read the whole of `path` into `buffer`, terminating it. Returns the length,
 * or -1 having said why.
 *
 * Read through `read` and not `fread`: the file is wanted whole, and a direct
 * read is one call rather than a buffered layer over the same bytes.
 */
static long slurp(const char *path, char *buffer, size_t size)
{
    int fd = open(path, O_RDONLY);
    size_t total = 0;

    if (fd < 0) {
        fprintf(stderr, "adduser: %s: %s\n", path, strerror(errno));

        return -1;
    }
    for (;;) {
        ssize_t got = read(fd, buffer + total, size - total - 1);

        if (got < 0) {
            fprintf(stderr, "adduser: %s: %s\n", path, strerror(errno));
            close(fd);

            return -1;
        }
        if (got == 0) {
            break;
        }
        total += (size_t)got;
        if (total + 1 >= size) {
            fprintf(stderr, "adduser: %s is too large to rewrite safely\n", path);
            close(fd);

            return -1;
        }
    }
    close(fd);
    buffer[total] = '\0';
    if (memchr(buffer, '\0', total) != NULL) {
        fprintf(stderr, "adduser: %s holds a NUL byte\n", path);

        return -1;
    }

    return (long)total;
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
 * Whether a line of `text` begins with `name` followed by a colon.
 *
 * This is the question every reader's parser asks first, so it is the question
 * the absence check asks: a name is present in a database when a line there
 * starts with it, whatever the rest of the line turns out to be.
 */
static int names_line(const char *text, const char *name)
{
    size_t len = strlen(name);
    const char *line = text;

    while (*line) {
        const char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);

        if (span > len && strncmp(line, name, len) == 0 && line[len] == ':') {
            return 1;
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }

    return 0;
}

/**
 * Whether `number` appears as field `which` (counting from zero) of any line of
 * `text`.
 *
 * A field that is not a plain decimal number is not a match, and not an error
 * either: it is a record musl's own parser skips, so it claims no number and
 * this program must not act as though it did. The arithmetic wraps the way
 * musl's `atou` does, and the comparison is on the truncated `uid_t` both sides
 * end up with, so a field of `4294968296` is the same 1000 to this scan that it
 * is to `getpwuid`; a `strtoul` here would saturate instead and hand the number
 * out twice.
 */
static int number_taken(const char *text, int which, unsigned long number)
{
    const char *line = text;

    while (*line) {
        const char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);
        const char *field = line;
        int n = 0;

        while (n < which) {
            const char *stop = memchr(field, ':', span - (size_t)(field - line));

            if (!stop) {
                break;
            }
            field = stop + 1;
            n++;
        }
        if (n == which) {
            const char *stop = memchr(field, ':', span - (size_t)(field - line));
            size_t width = stop ? (size_t)(stop - field) : span - (size_t)(field - line);
            unsigned long value = 0;
            size_t i;

            for (i = 0; i < width; i++) {
                if (field[i] < '0' || field[i] > '9') {
                    break;
                }
                value = value * 10 + (unsigned long)(field[i] - '0');
            }
            if (width > 0 && i == width && (uid_t)value == (uid_t)number) {
                return 1;
            }
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }

    return 0;
}

/**
 * Append `line` to `text`, giving the text a final newline first if it lacks
 * one. Returns the new length, or -1 when it would not fit.
 *
 * The missing-newline repair is not politeness: a database whose last line has
 * no newline is one `getspnam` reports as `ERANGE` for that record, and an
 * append with no separator would join two records into one.
 */
static long append_line(char *text, size_t size, const char *line)
{
    size_t used = strlen(text);

    if (used > 0 && text[used - 1] != '\n') {
        if (used + 2 > size) {
            return -1;
        }
        text[used++] = '\n';
        text[used] = '\0';
    }
    if (used + strlen(line) + 1 > size) {
        return -1;
    }
    memcpy(text + used, line, strlen(line) + 1);

    return (long)(used + strlen(line));
}

/**
 * Say what was published and that none of it was undone, then return `status`.
 *
 * Where this program fails rather than crashing there is someone to tell, so it
 * tells them: a partial account is not repaired by the next run, and the files
 * it names are the ones root edits.
 */
static int report_partial(int published, int status)
{
    static const char *const what[] = {"", SHADOW_FILE, GROUP_FILE, "the home directory",
                                       PASSWD_FILE};
    int which;

    if (published < PUBLISHED_SHADOW) {
        return status;
    }
    fprintf(stderr, "adduser: failed after writing %s", what[PUBLISHED_SHADOW]);
    for (which = PUBLISHED_SHADOW + 1; which <= published; which++) {
        fprintf(stderr, "%s%s", which == published ? " and " : ", ", what[which]);
    }
    fprintf(stderr, "; nothing was rolled back\n");

    return status;
}

/**
 * Say what is live but may not be on the platter, and return this program's
 * failing status.
 *
 * `CRED_UNFLUSHED` is not a failed write: the replacement is already the file
 * the system authenticates against, and only the writeback is in doubt. So the
 * publication counts, and what stops is the rest of it, since building the next
 * file on one that may not survive a power cut would multiply the ways a reboot
 * could disagree with what this program just reported.
 */
static int report_unflushed(int published)
{
    (void)report_partial(published, 1);
    fprintf(stderr, "adduser: what was written is live but may not survive a power cut\n");

    return 1;
}

/**
 * Refuse `name` for the databases that already hold it, and say which.
 * Returns 1 when one did, which is this program's failing status.
 *
 * The refusal distinguishes a live account from a residue because the two want
 * different repairs: an account that exists is not this run's business, while a
 * name in some of the files is a crashed run's leftovers, and root removes
 * those lines by hand.
 */
static int refuse_present(const char *name, int in_shadow, int in_group, int in_passwd)
{
    const char *held[3];
    int n = 0;
    int which;

    if (in_shadow) {
        held[n++] = SHADOW_FILE;
    }
    if (in_group) {
        held[n++] = GROUP_FILE;
    }
    if (in_passwd) {
        held[n++] = PASSWD_FILE;
    }
    if (n == 0) {
        return 0;
    }
    if (n == 3) {
        fprintf(stderr, "adduser: %s already exists\n", name);

        return 1;
    }
    fprintf(stderr, "adduser: %s is in %s", name, held[0]);
    for (which = 1; which < n; which++) {
        fprintf(stderr, "%s%s", which == n - 1 ? " and " : ", ", held[which]);
    }
    fprintf(stderr, " but not %s; remove the leftover lines and retry\n",
            in_passwd ? (in_shadow ? GROUP_FILE : SHADOW_FILE) : PASSWD_FILE);

    return 1;
}

/**
 * Hand the new account to the password program, which is what gives it a
 * password at all. Does not return except on failure.
 *
 * The transaction is already released: the lock is a file and survives an
 * `execve`, so a program that kept it here would deadlock its own successor
 * against it for the ten tries the library makes before refusing.
 */
static int hand_to_passwd(const char *name)
{
    execl(PASSWD_PROGRAM, "passwd", "--", name, (char *)0);
    if (errno == ENOENT) {
        execl(PASSWD_PROGRAM_FLAT, "passwd", "--", name, (char *)0);
    }
    fprintf(stderr, "adduser: %s: %s\n", PASSWD_PROGRAM, strerror(errno));
    fprintf(stderr, "adduser: %s exists and cannot be entered; run passwd %s\n", name, name);

    return 1;
}

#ifdef ADDUSER_CRASH_POINT
/* The crash build, staged in the test image alone and never in os-real. It
 * takes a leading count and dies the instant that publication has landed,
 * inside the transaction and through `_exit`, so it leaves exactly the residue
 * and exactly the lock a real crash leaves. Everything it adds lives under this
 * one conditional, so the shipped binary has neither the argument nor the code.
 */
static int Crash_after = 0;

/**
 * Die if publication `published` is the one this build was told to stop after.
 */
static void crash_check(int published)
{
    if (Crash_after == published) {
        _exit(9);
    }
}
#else
#define crash_check(published) ((void)0)
#endif

/**
 * Create an account and hand it to `passwd`.
 */
int main(int argc, char **argv)
{
    char passwd_text[TEXT_MAX];
    char group_text[TEXT_MAX];
    char shadow_text[TEXT_MAX];
    char record[RECORD_MAX + 2];
    char home[256];
    const char *name = NULL;
    const char *shell = "/bin/sh";
    unsigned long number = 0;
    int chosen = 0;
    int published = 0;
    int end_of_options = 0;
    int argument;
    int rc;
    long length;

#ifdef ADDUSER_CRASH_POINT
    if (argc < 2 || !all_digits(argv[1])) {
        fprintf(stderr, "usage: adduser-crash after-publication [-u number] [-s shell] name\n");

        return 1;
    }
    Crash_after = atoi(argv[1]);
    argv++;
    argc--;
#endif
    for (argument = 1; argument < argc; argument++) {
        const char *word = argv[argument];

        if (!end_of_options && strcmp(word, "--") == 0) {
            end_of_options = 1;
        } else if (!end_of_options && strcmp(word, "-u") == 0 && argument + 1 < argc) {
            const char *value = argv[++argument];

            if (!all_digits(value)) {
                fprintf(stderr, "adduser: %s: not a number\n", value);

                return 1;
            }
            number = strtoul(value, NULL, 10);
            chosen = 1;
        } else if (!end_of_options && strcmp(word, "-s") == 0 && argument + 1 < argc) {
            shell = argv[++argument];
        } else if (!end_of_options && word[0] == '-' && word[1] != '\0') {
            fprintf(stderr, "adduser: %s: unknown option\n", word);

            return 1;
        } else if (name) {
            fprintf(stderr, "adduser: one name at a time\n");

            return 1;
        } else {
            name = word;
        }
    }
    if (!name) {
        fprintf(stderr, "usage: adduser [-u number] [-s shell] name\n");

        return 1;
    }
    /* The real uid, although this program carries no set-id bit and the two are
     * the same here. The rule is the rule: a program that decides an identity
     * question decides it from the identity of whoever ran it. */
    if (getuid() != 0) {
        fprintf(stderr, "adduser: only root may create an account\n");

        return 1;
    }
    if (strchr(name, ':') || strchr(name, '\n') || strchr(name, '/') || name[0] == '-' ||
        name[0] == '.' || name[0] == '\0') {
        fprintf(stderr, "adduser: %s: not a usable account name\n", name);

        return 1;
    }
    /* And short enough for the console to read: `login` refuses an over-long
     * line rather than answering for its prefix, so an account named past that
     * limit is one nobody could ever enter at the keyboard. */
    if (strlen(name) > PWAUTH_MAX_NAME) {
        fprintf(stderr, "adduser: %s: longer than the %d characters a login reads\n", name,
                PWAUTH_MAX_NAME);

        return 1;
    }
    if (chosen && number == 0) {
        fprintf(stderr, "adduser: 0 is root's number\n");

        return 1;
    }
    if (chosen && number > ADDUSER_LAST_UID) {
        fprintf(stderr, "adduser: %lu is outside the range this command hands out\n", number);

        return 1;
    }
    if (strchr(shell, ':') || strchr(shell, '\n')) {
        fprintf(stderr, "adduser: %s: not a usable shell\n", shell);

        return 1;
    }
    if ((size_t)snprintf(home, sizeof(home), "/home/%s", name) >= sizeof(home)) {
        fprintf(stderr, "adduser: %s: the home directory path is too long\n", name);

        return 1;
    }

    if (cred_txn_begin("adduser") != 0) {
        return 1;
    }
    if (slurp(PASSWD_FILE, passwd_text, sizeof(passwd_text)) < 0 ||
        slurp(GROUP_FILE, group_text, sizeof(group_text)) < 0 ||
        slurp(SHADOW_FILE, shadow_text, sizeof(shadow_text)) < 0) {
        cred_txn_end();

        return 1;
    }
    if (refuse_present(name, names_line(shadow_text, name), names_line(group_text, name),
                       names_line(passwd_text, name))) {
        cred_txn_end();

        return 1;
    }
    if (chosen) {
        if (number_taken(passwd_text, 2, number)) {
            fprintf(stderr, "adduser: %lu is already a uid in %s\n", number, PASSWD_FILE);
            cred_txn_end();

            return 1;
        }
        if (number_taken(group_text, 2, number)) {
            fprintf(stderr, "adduser: %lu is already a gid in %s\n", number, GROUP_FILE);
            cred_txn_end();

            return 1;
        }
    } else {
        for (number = ADDUSER_FIRST_UID; number <= ADDUSER_LAST_UID; number++) {
            if (!number_taken(passwd_text, 2, number) && !number_taken(group_text, 2, number)) {
                break;
            }
        }
        if (number > ADDUSER_LAST_UID) {
            fprintf(stderr, "adduser: no number below %d is free in both %s and %s\n",
                    ADDUSER_LAST_UID, PASSWD_FILE, GROUP_FILE);
            cred_txn_end();

            return 1;
        }
    }

    /* Shadow first, with a locked field: the account cannot be entered until
     * `passwd` replaces it, and this is the one publication that consumes no
     * number and names no group, so it is the smallest fact to begin with. */
    if ((size_t)snprintf(record, sizeof(record), "%s:!:::::::\n", name) >= sizeof(record)) {
        fprintf(stderr, "adduser: %s: the shadow record would be too long for a reader\n", name);
        cred_txn_end();

        return 1;
    }
    length = append_line(shadow_text, sizeof(shadow_text), record);
    if (length < 0) {
        fprintf(stderr, "adduser: the new %s would not fit\n", SHADOW_FILE);
        cred_txn_end();

        return 1;
    }
    rc = cred_rewrite(SHADOW_FILE, shadow_text, (size_t)length);
    if (rc == CRED_UNCHANGED) {
        fprintf(stderr, "adduser: %s was not written; nothing was changed\n", SHADOW_FILE);
        cred_txn_end();

        return 1;
    }
    published = PUBLISHED_SHADOW;
    if (rc == CRED_UNFLUSHED) {
        cred_txn_end();

        return report_unflushed(published);
    }
    crash_check(published);

    if ((size_t)snprintf(record, sizeof(record), "%s:x:%lu:\n", name, number) >= sizeof(record)) {
        fprintf(stderr, "adduser: %s: the group record would be too long for a reader\n", name);
        cred_txn_end();

        return report_partial(published, 1);
    }
    length = append_line(group_text, sizeof(group_text), record);
    if (length < 0) {
        fprintf(stderr, "adduser: the new %s would not fit\n", GROUP_FILE);
        cred_txn_end();

        return report_partial(published, 1);
    }
    rc = cred_rewrite(GROUP_FILE, group_text, (size_t)length);
    if (rc == CRED_UNCHANGED) {
        cred_txn_end();

        return report_partial(published, 1);
    }
    published = PUBLISHED_GROUP;
    if (rc == CRED_UNFLUSHED) {
        cred_txn_end();

        return report_unflushed(published);
    }
    crash_check(published);

    if (mkdir(home, 0700) != 0) {
        fprintf(stderr, "adduser: %s: %s\n", home, strerror(errno));
        cred_txn_end();

        return report_partial(published, 1);
    }
    /* Published the instant the directory exists, and not after it has been
     * given its owner: a failure or a kill between the two leaves the directory
     * behind, and a recovery that followed a diagnostic which did not name it
     * would trip over it on the retry. The chown comes before any mode change,
     * since setting an owner clears the set-id bits of whatever mode was
     * there. */
    published = PUBLISHED_HOME;
    if (chown(home, (uid_t)number, (gid_t)number) != 0 || chmod(home, 0700) != 0) {
        fprintf(stderr, "adduser: %s: %s\n", home, strerror(errno));
        cred_txn_end();

        return report_partial(published, 1);
    }
    crash_check(published);

    if ((size_t)snprintf(record, sizeof(record), "%s:x:%lu:%lu:%s:%s:%s\n", name, number, number,
                         name, home, shell) >= sizeof(record)) {
        fprintf(stderr, "adduser: %s: the passwd record would be too long for a reader\n", name);
        cred_txn_end();

        return report_partial(published, 1);
    }
    length = append_line(passwd_text, sizeof(passwd_text), record);
    if (length < 0) {
        fprintf(stderr, "adduser: the new %s would not fit\n", PASSWD_FILE);
        cred_txn_end();

        return report_partial(published, 1);
    }
    rc = cred_rewrite(PASSWD_FILE, passwd_text, (size_t)length);
    if (rc == CRED_UNCHANGED) {
        cred_txn_end();

        return report_partial(published, 1);
    }
    published = PUBLISHED_PASSWD;
    crash_check(published);
    if (rc == CRED_UNFLUSHED) {
        cred_txn_end();

        return report_unflushed(published);
    }

    if (cred_txn_end() != 0) {
        fprintf(stderr, "adduser: %s exists; the transaction did not end cleanly\n", name);

        return 1;
    }
    printf("adduser: %s created with uid %lu; set its password now\n", name, number);
    fflush(stdout);

    return hand_to_passwd(name);
}
