/**
 * Authenticate a dummy SSH session on the real driver's pseudo-terminal.
 *
 * Only a test supplies this executable as ssh. It records receipt counts,
 * never input bytes, and runs the su helper's actual quoted command through
 * the test's unprivileged su substitute after receiving the first answer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/**
 * Report a fixture failure without printing any received credential bytes.
 */
static void fail(void)
{
    fputs("pty-secret-fixture: failed\n", stderr);
    exit(2);
}

/**
 * Write fixture output as separate transfers to exercise split prompts.
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
 * Run one dummy authentication before executing the helper's command.
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
