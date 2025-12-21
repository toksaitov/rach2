typedef unsigned int size_t;

#define STDERR_FILENO 2

#define O_RDONLY    0
#define O_DIRECTORY 0200000

#define DT_DIR 4

#define MAX_PATH 256
#define BUF_SIZE 512

#define WRITE_STR(fd, str) sys_write(fd, str, sizeof(str) - 1)

struct linux_dirent64 {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[];
};

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

static int sys_unlink(const char *path)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(10), "b"(path)
        : "memory"
    );
    return result;
}

static int sys_rmdir(const char *path)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(40), "b"(path)
        : "memory"
    );
    return result;
}

static int sys_getdents64(int fd, void *buf, size_t count)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(220), "b"(fd), "c"(buf), "d"(count)
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

static void *memcpy(void *dest, const void *src, size_t n)
{
    char *d = dest;
    const char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

static int remove_recursive(const char *path);

static int remove_dir_contents(const char *dir_path)
{
    int fd = sys_open(dir_path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        return -1;
    }

    char buf[BUF_SIZE];
    char full_path[MAX_PATH];
    size_t dir_len = strlen(dir_path);
    int nread;
    struct linux_dirent64 *d;
    int bpos;
    int result = 0;

    for (;;) {
        nread = sys_getdents64(fd, buf, BUF_SIZE);
        if (nread < 0) {
            result = -1;
            break;
        } else if (nread == 0) {
            break;
        }

        for (bpos = 0; bpos < nread;) {
            d = (struct linux_dirent64 *)(buf + bpos);

            if (d->d_name[0] == '.') {
                if (d->d_name[1] == '\0' ||
                    (d->d_name[1] == '.' && d->d_name[2] == '\0')) {
                    bpos += d->d_reclen;
                    continue;
                }
            }

            size_t name_len = strlen(d->d_name);
            if (dir_len + 1 + name_len >= MAX_PATH) {
                bpos += d->d_reclen;
                continue;
            }

            memcpy(full_path, dir_path, dir_len);
            size_t pos = dir_len;
            if (dir_len > 0 && dir_path[dir_len - 1] != '/') {
                full_path[pos] = '/';
                pos++;
            }
            memcpy(full_path + pos, d->d_name, name_len + 1);

            if (remove_recursive(full_path) < 0) {
                result = -1;
            }

            bpos += d->d_reclen;
        }
    }

    sys_close(fd);
    return result;
}

static int remove_recursive(const char *path)
{
    if (sys_unlink(path) == 0) {
        return 0;
    }

    if (remove_dir_contents(path) < 0) {
        return -1;
    }

    return sys_rmdir(path);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        WRITE_STR(STDERR_FILENO, "Usage: rm [-r] <file> [...]\n");
        return 1;
    }

    int recursive = 0;
    int start = 1;

    if (argv[1][0] == '-' && argv[1][1] == 'r' && argv[1][2] == '\0') {
        recursive = 1;
        start = 2;
    }

    if (start >= argc) {
        WRITE_STR(STDERR_FILENO, "Usage: rm [-r] <file> [...]\n");
        return 1;
    }

    for (int i = start; i < argc; ++i) {
        int result;
        if (recursive) {
            result = remove_recursive(argv[i]);
        } else {
            result = sys_unlink(argv[i]);
        }

        if (result < 0) {
            WRITE_STR(STDERR_FILENO, "rm: failed to remove '");
            sys_write(STDERR_FILENO, argv[i], strlen(argv[i]));
            WRITE_STR(STDERR_FILENO, "'\n");
        }
    }

    return 0;
}
