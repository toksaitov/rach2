/* Small compiled workloads transferred by the protocol host tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int etude_workload_guest(int argc, char **argv, const char *role);

#ifndef _WIN32
#include <unistd.h>

/** Wait for the controller to end this process. */
static int wait_for_stop(void)
{
    fflush(NULL);
    for (;;)
        pause();
}

static int remote_workload(int argc, char **argv)
{
    FILE *file;
    int i;

    if (argc != 3)
        return 125;
    printf("pid=%ld\n", (long)getpid());
    file = fopen(argv[1], "a");
    if (file == NULL)
        return 125;
    fputs("run\n", file);
    if (fclose(file) != 0)
        return 125;
    fputs("to stderr\n", stderr);
    file = fopen("out.txt", "w");
    if (file == NULL)
        return 125;
    fputs("made\n", file);
    if (fclose(file) != 0)
        return 125;
    if (strcmp(argv[2], "wait") == 0) {
        puts("waiting");
        return wait_for_stop();
    }
    for (i = 0; i < 100; i++)
        printf("line %d of the output\n", i);
    return atoi(argv[2]);
}

static int serial_workload(const char *role)
{
    int i;

    if (strcmp(role, "serial-fail") == 0) {
        fputs("broken\n", stderr);
        return 3;
    }
    if (strcmp(role, "serial-forever") == 0) {
        puts("ready");
        return wait_for_stop();
    }
    if (strcmp(role, "serial-pass") != 0)
        return 125;
    for (i = 0; i < 40; i++)
        printf("out line %d\n", i);
    fputs("to stderr\n", stderr);
    return 0;
}

static int native_show(int argc, char **argv)
{
    const char *kept = getenv("KEPT"), *gone = getenv("GONE");
    size_t newlines = 0;
    int c, i;

    fputs("show: args ", stdout);
    for (i = 1; i < argc; i++)
        printf("%s%s", i > 1 ? " " : "", argv[i]);
    fputs("\nshow: input ", stdout);
    while ((c = getchar()) != EOF) {
        if (c == '\n')
            newlines++;
        else {
            while (newlines > 0) {
                putchar('\n');
                newlines--;
            }
            putchar(c);
        }
    }
    printf("\nshow: kept %s, gone %s\n", kept ? kept : "unset", gone ? gone : "unset");
    return ferror(stdin) || ferror(stdout) ? 125 : 0;
}
#endif

int main(int argc, char **argv)
{
    const char *role = getenv("ETUDE_WORKLOAD_ROLE");

    if (role == NULL)
        return 125;
    if (strncmp(role, "guest-", 6) == 0)
        return etude_workload_guest(argc, argv, role);
#ifndef _WIN32
    if (strcmp(role, "remote") == 0)
        return remote_workload(argc, argv);
    if (strncmp(role, "serial-", 7) == 0)
        return serial_workload(role);
    if (strcmp(role, "native-show") == 0)
        return native_show(argc, argv);
    if (strcmp(role, "runtime-exec") == 0 && argc == 2)
        execl(argv[1], argv[1], "--case", "hang", (char *)NULL);
#endif
    return 125;
}
