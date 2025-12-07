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

static int sys_rename(const char *oldpath, const char *newpath)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(38), "b"(oldpath), "c"(newpath)
        : "memory"
    );
    return result;
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        WRITE_STR(STDERR_FILENO, "Usage: mv <source> <destination>\n");
        return 1;
    }

    if (sys_rename(argv[1], argv[2]) < 0) {
        WRITE_STR(STDERR_FILENO, "mv: cannot move file\n");
        return 1;
    }

    return 0;
}
