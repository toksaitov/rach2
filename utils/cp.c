typedef unsigned int size_t;
typedef int ssize_t;

#define STDERR_FILENO 2

#define O_RDONLY 0
#define O_WRONLY 01
#define O_CREAT  0100
#define O_TRUNC  01000

#define WRITE_STR(fd, str) sys_write(fd, str, sizeof(str) - 1)

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

int main(int argc, char *argv[])
{
    if (argc != 3) {
        WRITE_STR(STDERR_FILENO, "Usage: cp <source> <destination>\n");
        return 1;
    }

    int fd_src = sys_open(argv[1], O_RDONLY, 0);
    if (fd_src < 0) {
        WRITE_STR(STDERR_FILENO, "cp: cannot open the source file\n");
        return 1;
    }
    int fd_dest = sys_open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd_dest < 0) {
        WRITE_STR(STDERR_FILENO, "cp: cannot create the destination file\n");
        sys_close(fd_src);
        return 1;
    }

    ssize_t bytes_read, bytes_written;
    while ((bytes_read = sys_read(fd_src, buffer, BUFFER_SIZE)) > 0) {
        bytes_written = sys_write(fd_dest, buffer, bytes_read);
        if (bytes_written != bytes_read) {
            WRITE_STR(STDERR_FILENO, "cp: write error\n");
            sys_close(fd_src);
            sys_close(fd_dest);
            return 1;
        }
    }
    sys_close(fd_src);
    sys_close(fd_dest);

    return 0;
}
