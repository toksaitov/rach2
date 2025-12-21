typedef unsigned int size_t;

#define STDERR_FILENO 2

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

static int sys_mkdir(const char *path, int mode)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(39), "b"(path), "c"(mode)
        : "memory"
    );
    return result;
}

static size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

int main(int argc, char *argv[])
{
    int failed = 0;

    if (argc < 2) {
        WRITE_STR(STDERR_FILENO, "Usage: mkdir <dir> [...]\n");
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if (sys_mkdir(argv[i], 0777) < 0) {
            WRITE_STR(STDERR_FILENO, "mkdir: failed to create directory '");
            sys_write(STDERR_FILENO, argv[i], strlen(argv[i]));
            WRITE_STR(STDERR_FILENO, "'\n");
            failed = 1;
        }
    }

    return failed;
}
