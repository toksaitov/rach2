/**
 * pty_secret: the ssh that accept/password_test.sh puts on the PATH, which
 * asks for a password on the pseudo-terminal runner/ptydrive gives it and
 * then runs the command su_command.sh passed.
 *
 * It writes that it received one answer to $PTY_SECRET_AUDIT/first, never
 * the answer itself, and runs the command through the test's su, which
 * needs no privilege.
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
 * Prompt for a password as the case in $PTY_SECRET_CASE says, read one line
 * as the answer, and run the last argument with `sh -c`.
 *
 * The prompt is `Password:` in two writes 20 ms apart; in `joined` it is two
 * prompts in one write, and in `long` 4096 dots follow it. Echo is off while
 * the answer is read, except in `echo`. In `prefix` the fixture writes the
 * answer back without its last byte and exits 0. Otherwise it puts the
 * answer, its first half and its second half in $PTY_DUMMY_RECEIVED,
 * $PTY_DUMMY_FIRST and $PTY_DUMMY_LAST for the command to print. The
 * password's own variable, RACH2_ACCEPT_PASSWORD, must not reach this
 * program, and an empty or overlong answer fails.
 */
int main(int argc, char **argv)
{
    struct termios terminal;
    const char *mode = getenv("PTY_SECRET_CASE");
    const char *audit = getenv("PTY_SECRET_AUDIT");
    char password[128];
    char path[1024];
    size_t length = 0;
    FILE *record;

    if (argc < 2 || mode == NULL || audit == NULL || getenv("RACH2_ACCEPT_PASSWORD") != NULL ||
        tcgetattr(0, &terminal) != 0)
        fail();
    if (strcmp(mode, "echo") != 0)
        terminal.c_lflag &= (tcflag_t)~ECHO;
    if (tcsetattr(0, TCSANOW, &terminal) != 0)
        fail();
    if (strcmp(mode, "joined") == 0) {
        emit("Password:Password:", 18);
    } else {
        emit("Pass", 4);
        usleep(20000);
        emit("word:", 5);
    }
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
