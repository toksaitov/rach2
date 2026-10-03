/**
 * The x86-64 Linux system call wrappers for ish.
 *
 * Each wrapper issues `syscall` with the syscall number in `rax` and the
 * arguments in `rdi`, `rsi`, `rdx`, `r10` in that order; the instruction
 * overwrites `rcx` with the return address and `r11` with the flags, so both
 * are clobbered. The kernel returns the result in `rax`, and a failure as the
 * negated error number, which the wrapper returns unchanged since there is no
 * `errno`. Each argument is widened to `long` before it reaches its register,
 * so a negative descriptor or process id arrives sign-extended. The numbers
 * are the Linux x86-64 ABI, as musl's arch/x86_64/bits/syscall.h.in has them:
 * `read` 0, `write` 1, `open` 2, `close` 3, `stat` 4, `dup2` 33, `fork` 57,
 * `execve` 59, `exit` 60, `wait4` 61, `chdir` 80, `creat` 85. x86-64 has no
 * `waitpid`; `ish_waitpid` is `wait4` with no resource usage.
 */
long ish_read(int file_descriptor, void *buffer, unsigned long buffer_size)
{
    long result;
    __asm__ __volatile__("syscall"
                         : "=a"(result)
                         : "0"(0L), "D"((long)file_descriptor), "S"(buffer), "d"(buffer_size)
                         : "rcx", "r11", "memory");
    return result;
}

int ish_chdir(const char *path)
{
    long result;
    __asm__ __volatile__("syscall" : "=a"(result) : "0"(80L), "D"(path) : "rcx", "r11", "memory");
    return (int)result;
}

void ish_exit(int status)
{
    __asm__ __volatile__("syscall" : : "a"(60L), "D"((long)status) : "rcx", "r11", "memory");
}

int ish_stat(const char *path, void *stat_result)
{
    long result;
    __asm__ __volatile__("syscall"
                         : "=a"(result)
                         : "0"(4L), "D"(path), "S"(stat_result)
                         : "rcx", "r11", "memory");
    return (int)result;
}

int ish_open(const char *path, int flags)
{
    long result;
    __asm__ __volatile__("syscall"
                         : "=a"(result)
                         : "0"(2L), "D"(path), "S"((long)flags), "d"(0L)
                         : "rcx", "r11", "memory");
    return (int)result;
}

int ish_creat(const char *path, unsigned int mode)
{
    long result;
    __asm__ __volatile__("syscall"
                         : "=a"(result)
                         : "0"(85L), "D"(path), "S"((long)mode)
                         : "rcx", "r11", "memory");
    return (int)result;
}

int ish_dup2(int old_file_descriptor, int new_file_descriptor)
{
    long result;
    __asm__ __volatile__("syscall"
                         : "=a"(result)
                         : "0"(33L), "D"((long)old_file_descriptor), "S"((long)new_file_descriptor)
                         : "rcx", "r11", "memory");
    return (int)result;
}

int ish_close(int file_descriptor)
{
    long result;
    __asm__ __volatile__("syscall"
                         : "=a"(result)
                         : "0"(3L), "D"((long)file_descriptor)
                         : "rcx", "r11", "memory");
    return (int)result;
}

int ish_fork()
{
    long result;
    __asm__ __volatile__("syscall" : "=a"(result) : "0"(57L) : "rcx", "r11", "memory");
    return (int)result;
}

int ish_execve(const char *path, char *const arguments[], char *const environment[])
{
    long result;
    __asm__ __volatile__("syscall"
                         : "=a"(result)
                         : "0"(59L), "D"(path), "S"(arguments), "d"(environment)
                         : "rcx", "r11", "memory");
    return (int)result;
}

int ish_waitpid(int pid, int *status, int options)
{
    long result;
    __asm__ __volatile__("xorq %%r10, %%r10\n\t"
                         "syscall"
                         : "=a"(result)
                         : "0"(61L), "D"((long)pid), "S"(status), "d"((long)options)
                         : "rcx", "r10", "r11", "memory");
    return (int)result;
}

long ish_write(int file_descriptor, const void *buffer, unsigned long buffer_size)
{
    long result;
    __asm__ __volatile__("syscall"
                         : "=a"(result)
                         : "0"(1L), "D"((long)file_descriptor), "S"(buffer), "d"(buffer_size)
                         : "rcx", "r11", "memory");
    return result;
}
