typedef unsigned int size_t;

#define NULL ((void *)0)

#define STDERR_FILENO 2

#define O_WRONLY   01
#define O_CREAT    0100
#define O_NOCTTY   0400
#define O_NONBLOCK 04000

#define WRITE_STR(fd, str) sys_write(fd, str, sizeof(str) - 1)

static int sys_write(int fd, const void *buf, size_t count)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(4), "b"(fd), "c"(buf), "d"(count)
        : "memory"
    );
    return result;
}

static int sys_open(const char *path, int flags, int mode)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(5), "b"(path), "c"(flags), "d"(mode)
        : "memory"
    );
    return result;
}

static int sys_close(int fd)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(6), "b"(fd)
        : "memory"
    );
    return result;
}

static int sys_utimensat(int dirfd, const char *path, const void *times, int flags)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(320), "b"(dirfd), "c"(path), "d"(times), "S"(flags)
        : "memory"
    );
    return result;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        WRITE_STR(STDERR_FILENO, "Usage: touch <file> [...]\n");
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        int fd = sys_open(argv[i], O_WRONLY | O_CREAT | O_NOCTTY | O_NONBLOCK, 0666);
        if (fd != -1) {
            sys_utimensat(fd, NULL, NULL, 0);
            sys_close(fd);
        }
    }

    return 0;
}
