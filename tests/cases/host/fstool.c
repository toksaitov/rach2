/* Independent formatters and checkers in the declared GNU Rach2 helper. */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int number(const char *text, uint64_t *value)
{
    char *end;
    if (!*text || strspn(text, "0123456789") != strlen(text))
        return 0;
    errno = 0;
    *value = strtoull(text, &end, 10);
    return !errno && !*end;
}

static uint64_t le32(const unsigned char *p)
{
    return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 | (uint64_t)p[3] << 24;
}

/* The formatter can write only the requested existing MBR partition. */
static int partition(char **argv, char path[64])
{
    unsigned char mbr[512];
    uint64_t part, start, count;
    int fd = open("/dev/disk1", O_RDONLY);
    ssize_t n = fd < 0 ? -1 : read(fd, mbr, sizeof(mbr));
    off_t size = fd < 0 ? -1 : lseek(fd, 0, SEEK_END);
    if (fd >= 0)
        close(fd);
    if (n != 512 || size < 0 || mbr[510] != 0x55 || mbr[511] != 0xaa ||
        !number(argv[3], &part) || part < 1 || part > 4 || !number(argv[4], &start) ||
        !number(argv[5], &count) || !count || start > (uint64_t)size / 512 ||
        count > (uint64_t)size / 512 - start)
        return 0;
    const unsigned char *entry = mbr + 446 + (part - 1) * 16;
    if (entry[4] != 7 || le32(entry + 8) != start || le32(entry + 12) != count)
        return 0;
    snprintf(path, 64, "/dev/disk1p%u", (unsigned)part);
    return 1;
}

/* Children stay in etude-guest's owned attempt group. */
static int run(char *const argv[])
{
    int status;
    pid_t pid = fork();
    if (pid < 0)
        return 1;
    if (!pid) {
        execv(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int main(int argc, char **argv)
{
    char device[64];
    const char *tool;
    int format = argc > 1 && !strcmp(argv[1], "exfat-mkfs"), status;
    if (argc < 3 || strcmp(argv[2], "/dev/disk1"))
        return 2;
    if (format) {
        if (argc != 7 || !partition(argv, device))
            return 2;
        tool = "/usr/sbin/mkfs.exfat";
    } else {
        if ((argc != 3 && argc != 4) || (argc == 4 && strcmp(argv[3], "repair")))
            return 2;
        if (!strcmp(argv[1], "exfat-fsck"))
            tool = "/usr/sbin/fsck.exfat";
        else if (!strcmp(argv[1], "fat-fsck") && argc == 3)
            tool = "/usr/sbin/fsck.fat";
        else
            return 2;
        strcpy(device, "/dev/disk1");
    }
    if (access(tool, X_OK)) {
        printf("fstool: absent: %s\n", tool);
        return 3;
    }
    printf("fstool: tool: %s in the GNU Rach2 helper\n", strrchr(tool, '/') + 1);
    fflush(stdout);
    if (format) {
        char *args[] = {(char *)tool, "-L", argv[6], device, NULL};
        status = run(args);
    } else {
        char *args[] = {(char *)tool, "-n", device, NULL};
        if (argc == 4) {
            args[1] = "-y";
            status = run(args);
            printf("fstool: the repair ended with status %d\n", status);
            fflush(stdout);
            args[1] = "-n";
        }
        status = run(args);
    }
    return status ? 1 : 0;
}
