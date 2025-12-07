typedef unsigned int size_t;

#define NULL ((void *)0)

#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define O_RDONLY    0
#define O_DIRECTORY 0200000

#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10

#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100

#define MAX_PATH 1024
#define BUF_SIZE 1024

#define WRITE_STR(fd, str) sys_write((fd), (str), (sizeof(str) - 1))

struct linux_dirent64 {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[];
};

struct stat64 {
    unsigned long long st_dev;
    unsigned char      __pad0[4];
    unsigned int       __st_ino;
    unsigned int       st_mode;
    unsigned int       st_nlink;
    unsigned int       st_uid;
    unsigned int       st_gid;
    unsigned long long st_rdev;
    unsigned char      __pad3[4];
    long long          st_size;
    unsigned int       st_blksize;
    unsigned long long st_blocks;
    unsigned int       st_atime;
    unsigned int       st_atime_nsec;
    unsigned int       st_mtime;
    unsigned int       st_mtime_nsec;
    unsigned int       st_ctime;
    unsigned int       st_ctime_nsec;
    unsigned long long st_ino;
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

static int sys_stat64(const char *path, struct stat64 *buf)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(195), "b"(path), "c"(buf)
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

static void print_file_info(const char *dir_path, const char *filename, unsigned char d_type)
{
    char full_path[MAX_PATH];
    size_t dir_len = strlen(dir_path);
    size_t name_len = strlen(filename);

    if (dir_len + 1 + name_len >= MAX_PATH) {
        return;
    }

    memcpy(full_path, dir_path, dir_len);
    if (dir_len > 0 && dir_path[dir_len - 1] != '/') {
        full_path[dir_len] = '/';
        dir_len++;
    }
    memcpy(full_path + dir_len, filename, name_len + 1);

    struct stat64 st;
    if (sys_stat64(full_path, &st) < 0) {
        return;
    }

    if (d_type == DT_DIR) WRITE_STR(STDOUT_FILENO, "d");
    else if (d_type == DT_REG) WRITE_STR(STDOUT_FILENO, "-");
    else if (d_type == DT_LNK) WRITE_STR(STDOUT_FILENO, "l");
    else WRITE_STR(STDOUT_FILENO, "?");

    sys_write(STDOUT_FILENO, (st.st_mode & S_IRUSR) ? "r" : "-", 1);
    sys_write(STDOUT_FILENO, (st.st_mode & S_IWUSR) ? "w" : "-", 1);
    sys_write(STDOUT_FILENO, (st.st_mode & S_IXUSR) ? "x" : "-", 1);

    WRITE_STR(STDOUT_FILENO, " ");
    sys_write(STDOUT_FILENO, filename, name_len);
    WRITE_STR(STDOUT_FILENO, "\n");
}

int main(int argc, char *argv[])
{
    const char *dir_path = (argc > 1) ? argv[1] : ".";

    int fd = sys_open(dir_path, O_RDONLY | O_DIRECTORY, 0);
    if (fd == -1) {
        WRITE_STR(STDERR_FILENO, "ls: cannot open directory\n");
        return 1;
    }

    char buf[BUF_SIZE];
    int nread;
    struct linux_dirent64 *d;
    int bpos;
    unsigned char d_type;
    for (;;) {
        nread = sys_getdents64(fd, buf, BUF_SIZE);
        if (nread == -1) {
            WRITE_STR(STDERR_FILENO, "ls: getdents64 error\n");
            return 1;
        } else if (nread == 0) {
            break;
        }

        for (bpos = 0; bpos < nread;) {
            d = (struct linux_dirent64 *) (buf + bpos);
            d_type = d->d_type;
            if (d->d_name[0] != '.') {
                print_file_info(dir_path, d->d_name, d_type);
            }

            bpos += d->d_reclen;
        }
    }
    sys_close(fd);

    return 0;
}
