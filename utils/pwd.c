typedef unsigned int size_t;

#define NULL ((void *)0)

#define STDOUT_FILENO 1

#define PATH_MAX 256

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

static int sys_getcwd(char *buf, size_t size)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(183), "b"(buf), "c"(size)
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

int main(void)
{
    char cwd[PATH_MAX];
    /* getcwd returns buf pointer on success (non-zero), 0 on error */
    if (sys_getcwd(cwd, sizeof(cwd)) != 0) {
        sys_write(STDOUT_FILENO, cwd, strlen(cwd));
        sys_write(STDOUT_FILENO, "\n", 1);
        return 0;
    } else {
        return 1;
    }
}
