/*
 * The i386 system call wrappers of ish, included by `ish_syscalls.c`.
 *
 * Each wrapper issues `int $0x80` with the call number in EAX and the
 * arguments in EBX, ECX and EDX in that order, and returns EAX, where the
 * kernel puts the result or the negated error number. The numbers are the
 * Linux i386 ABI's: `exit` 1, `fork` 2, `read` 3, `write` 4, `open` 5,
 * `close` 6, `waitpid` 7, `creat` 8, `execve` 11, `chdir` 12, `dup2` 63 and
 * `stat` 106.
 */
long ish_read(int file_descriptor, void *buffer, unsigned long buffer_size)
{
    long result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(3), "b"(file_descriptor), "c"(buffer), "d"(buffer_size)
                         : "memory");
    return result;
}

int ish_chdir(const char *path)
{
    int result;
    __asm__ __volatile__("int $0x80" : "=a"(result) : "0"(12), "b"(path) : "memory");
    return result;
}

void ish_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(1), "b"(status) : "memory");
}

int ish_stat(const char *path, void *stat_result)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(106), "b"(path), "c"(stat_result)
                         : "memory");
    return result;
}

int ish_open(const char *path, int flags)
{
    int result;
    __asm__ __volatile__("int $0x80" : "=a"(result) : "0"(5), "b"(path), "c"(flags) : "memory");
    return result;
}

int ish_creat(const char *path, unsigned int mode)
{
    int result;
    __asm__ __volatile__("int $0x80" : "=a"(result) : "0"(8), "b"(path), "c"(mode) : "memory");
    return result;
}

int ish_dup2(int old_file_descriptor, int new_file_descriptor)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(63), "b"(old_file_descriptor), "c"(new_file_descriptor)
                         : "memory");
    return result;
}

int ish_close(int file_descriptor)
{
    int result;
    __asm__ __volatile__("int $0x80" : "=a"(result) : "0"(6), "b"(file_descriptor) : "memory");
    return result;
}

int ish_fork()
{
    int result;
    __asm__ __volatile__("int $0x80" : "=a"(result) : "0"(2) : "memory");
    return result;
}

int ish_execve(const char *path, char *const arguments[], char *const environment[])
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(11), "b"(path), "c"(arguments), "d"(environment)
                         : "memory");
    return result;
}

int ish_waitpid(int pid, int *status, int options)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(7), "b"(pid), "c"(status), "d"(options)
                         : "memory");
    return result;
}

long ish_write(int file_descriptor, const void *buffer, unsigned long buffer_size)
{
    long result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(4), "b"(file_descriptor), "c"(buffer), "d"(buffer_size)
                         : "memory");
    return result;
}
