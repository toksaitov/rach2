/**
 * pty_secret: the ssh that accept/password_test.sh puts on the PATH, which
 * asks for a password on the pseudo-terminal runner/ptydrive gives it and
 * then runs the command su_command.sh passed.
 *
 * It writes that it received one answer to $PTY_SECRET_AUDIT/first, never
 * the answer itself, and runs the command through the test's su, which
 * needs no privilege. In two cases it ends as ssh does when the machine
 * resets or refuses the connection before the session begins.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/**
 * Report a failure of the fixture and exit 2; nothing it received is printed.
 */
static void fail(void)
{
    fputs("pty-secret-fixture: failed\n", stderr);
    exit(2);
}

/**
 * Write `length` bytes of `text` to standard output, or fail.
 */
static void emit(const char *text, size_t length)
{
    while (length != 0) {
        ssize_t n = write(STDOUT_FILENO, text, length);

        if (n <= 0)
            fail();
        text += n;
        length -= (size_t)n;
    }
}

/**
 * Count the lines of the file at `path`, which runner/ptydrive adds one to
 * for each read of this program's output; fail when it cannot be read.
 */
static long reads_seen(const char *path)
{
    FILE *file = fopen(path, "r");
    long lines = 0;
    int c;

    if (file == NULL)
        fail();
    while ((c = getc(file)) != EOF) {
        if (c == '\n')
            lines++;
    }
    fclose(file);
    return lines;
}

/**
 * Write the prompt of the case `mode`: two prompts in one write in `joined`,
 * and otherwise `Password:` in two writes, the second once the driver has
 * read the first, as a new line in the file `reads` shows.
 */
static void prompt(const char *mode, const char *reads)
{
    long before;

    if (strcmp(mode, "joined") == 0) {
        emit("Password:Password:", 18);
        return;
    }
    before = reads_seen(reads);
    emit("Pass", 4);
    while (reads_seen(reads) == before)
        usleep(1000);
    emit("word:", 5);
}

/**
 * End as ssh does when the connection fails before the session, for the
 * cases `reset` and `refused`, and return in any other case.
 *
 * `reset` fails so the first time, which it records as the file `reset` in
 * the directory `audit`, and returns on the next run. `refused` fails every
 * time and adds a line to the file `tries` for each. The message is written
 * and read by the driver, as a new line in the file `reads` shows, before
 * the program exits with ssh's status 255.
 */
static void unreachable(const char *mode, const char *audit, const char *reads)
{
    char path[1024];
    const char *message;
    FILE *record;
    long before;

    if (strcmp(mode, "reset") == 0) {
        message = "kex_exchange_identification: read: Connection reset by peer\r\n";
        if (snprintf(path, sizeof(path), "%s/reset", audit) >= (int)sizeof(path))
            fail();
        if (access(path, F_OK) == 0)
            return;
    } else if (strcmp(mode, "refused") == 0) {
        message = "ssh: connect to host fixture port 22: Connection refused\r\n";
        if (snprintf(path, sizeof(path), "%s/tries", audit) >= (int)sizeof(path))
            fail();
    } else {
        return;
    }
    record = fopen(path, "a");
    if (record == NULL)
        fail();
    fputs("try\n", record);
    if (fclose(record) != 0)
        fail();
    before = reads_seen(reads);
    emit(message, strlen(message));
    while (reads_seen(reads) == before)
        usleep(1000);
    exit(255);
}

/**
 * Prompt for a password as the case in $PTY_SECRET_CASE says, read one line
 * as the answer, and run the last argument with `sh -c`.
 *
 * `prompt` writes the prompt, given the driver's file $PTYDRIVE_READS; in
 * `long` 4096 dots follow it. Echo is off while
 * the answer is read, except in `echo`. In `prefix` the fixture writes the
 * answer back without its last byte and exits 0. Otherwise it puts the
 * answer, its first half and its second half in $PTY_DUMMY_RECEIVED,
 * $PTY_DUMMY_FIRST and $PTY_DUMMY_LAST for the command to print. The
 * password's own variable, RACH2_ACCEPT_PASSWORD, must not reach this
 * program, and an empty or overlong answer fails. `unreachable` ends the
 * cases of a failed connection first.
 */
int main(int argc, char **argv)
{
    struct termios terminal;
    const char *mode = getenv("PTY_SECRET_CASE");
    const char *audit = getenv("PTY_SECRET_AUDIT");
    const char *reads = getenv("PTYDRIVE_READS");
    char password[128];
    char path[1024];
    size_t length = 0;
    FILE *record;

    if (argc < 2 || mode == NULL || audit == NULL || reads == NULL ||
        getenv("RACH2_ACCEPT_PASSWORD") != NULL || tcgetattr(0, &terminal) != 0)
        fail();
    unreachable(mode, audit, reads);
    if (strcmp(mode, "echo") != 0)
        terminal.c_lflag &= (tcflag_t)~ECHO;
    if (tcsetattr(0, TCSANOW, &terminal) != 0)
        fail();
    prompt(mode, reads);
    if (strcmp(mode, "long") == 0) {
        char padding[4096];

        memset(padding, '.', sizeof(padding));
        emit(padding, sizeof(padding));
    }
    while (length + 1 < sizeof(password)) {
        char byte;

        if (read(0, &byte, 1) != 1)
            fail();
        if (byte == '\n' || byte == '\r')
            break;
        password[length++] = byte;
    }
    password[length] = '\0';
    if (length == 0 || length + 1 == sizeof(password))
        fail();
    if (snprintf(path, sizeof(path), "%s/first", audit) >= (int)sizeof(path))
        fail();
    record = fopen(path, "w");
    if (record == NULL)
        fail();
    fputs("one answer\n", record);
    if (fclose(record) != 0)
        fail();
    terminal.c_lflag |= ECHO;
    if (tcsetattr(0, TCSANOW, &terminal) != 0)
        fail();
    if (strcmp(mode, "prefix") == 0) {
        emit(password, length - 1);
        return 0;
    }
    if (setenv("PTY_DUMMY_RECEIVED", password, 1) != 0 ||
        setenv("PTY_DUMMY_LAST", password + length / 2, 1) != 0)
        fail();
    password[length / 2] = '\0';
    if (setenv("PTY_DUMMY_FIRST", password, 1) != 0)
        fail();
    execl("/bin/sh", "sh", "-c", argv[argc - 1], (char *)NULL);
    fail();
    return 2;
}
