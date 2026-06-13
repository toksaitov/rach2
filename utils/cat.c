typedef unsigned int size_t;
typedef int ssize_t;

#define NULL ((void *)0)

#define STDIN_FILENO  0
#define STDOUT_FILENO 1

#define O_RDONLY 0

static int sys_read(int fd, void *buf, size_t count)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(3), "b"(fd), "c"(buf), "d"(count)
        : "memory"
    );
    return result;
}

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

#define BUFFER_SIZE 4096
static char buffer[BUFFER_SIZE];

static void cat_file(int fd)
{
    ssize_t bytes_read;
    while ((bytes_read = sys_read(fd, buffer, BUFFER_SIZE)) > 0) {
        sys_write(STDOUT_FILENO, buffer, bytes_read);
    }
}

int main(int argc, char *argv[])
{
    if (argc == 1) {
        cat_file(STDIN_FILENO);
    } else {
        for (int i = 1; i < argc; ++i) {
            int fd = sys_open(argv[i], O_RDONLY, 0);
            if (fd >= 0) {
                cat_file(fd);
                sys_close(fd);
            }
        }
    }

    return 0;
}
