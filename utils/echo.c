typedef unsigned int size_t;

#define STDOUT_FILENO 1

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
    for (int i = 1; i < argc; ++i) {
        sys_write(STDOUT_FILENO, argv[i], strlen(argv[i]));
        if (i < argc - 1) {
            sys_write(STDOUT_FILENO, " ", 1);
        }
    }
    sys_write(STDOUT_FILENO, "\n", 1);

    return 0;
}
