/* Proc-format observations for the status reader's GNU-host fixture. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(__linux__)
#include <sys/sysmacros.h>
#endif
#include <unistd.h>
#include "etude/ledger.h"
#include "../../cases/host/fixture.h"
#include "status_proc_host.h"

/** Copy a running process's actual stat file; omit a process observed dead. */
static int process_stat(const char *root, pid_t pid)
{
    struct etude_pid observed;
    char source[128], destination[4096];
    int state = etude_process_state((long)pid, &observed), n;
    if (!state)
        return 1;
    if (state != 1 || !observed.start_known)
        return 0;
    n = snprintf(destination, sizeof(destination), "%s/%ld", root, (long)pid);
    if (n < 0 || n >= (int)sizeof(destination) || !fx_mkdir(destination))
        return 0;
    n = snprintf(destination, sizeof(destination), "%s/%ld/stat", root, (long)pid);
    snprintf(source, sizeof(source), "/proc/%ld/stat", (long)pid);
    return n >= 0 && n < (int)sizeof(destination) && fx_copy(source, destination);
}

/** Record the conflicting lock's kernel identity, device and inode. */
static int lock_stat(const char *root, const char *path)
{
    struct flock lock;
    struct stat st;
    char destination[4096], text[512];
    int fd = open(path, O_RDWR), ok, n;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    ok = fd >= 0 && !fstat(fd, &st) && !fcntl(fd, F_GETLK, &lock) && lock.l_type == F_WRLCK &&
         lock.l_pid > 0;
    if (fd >= 0)
        close(fd);
    if (!ok)
        return 0;
    n = snprintf(destination, sizeof(destination), "%s/locks", root);
    if (n < 0 || n >= (int)sizeof(destination))
        return 0;
    n = snprintf(text, sizeof(text), "1: POSIX ADVISORY WRITE %ld %x:%x:%llu 0 EOF\n",
                 (long)lock.l_pid, (unsigned)major(st.st_dev), (unsigned)minor(st.st_dev),
                 (unsigned long long)st.st_ino);
    return n >= 0 && n < (int)sizeof(text) && fx_text(destination, text);
}

int status_proc_capture(const char *root, const pid_t *pids, size_t count, const char *lock)
{
    char destination[4096];
    size_t i;
    int n;
    if (!fx_mkdir(root) || !lock_stat(root, lock))
        return 0;
    for (i = 0; i < count; i++)
        if (!process_stat(root, pids[i]))
            return 0;
    if (access("/proc/sys/kernel/random/boot_id", F_OK))
        return errno == ENOENT;
    n = snprintf(destination, sizeof(destination), "%s/sys/kernel/random", root);
    if (n < 0 || n >= (int)sizeof(destination) || !fx_mkdir(destination))
        return 0;
    n = snprintf(destination, sizeof(destination), "%s/sys/kernel/random/boot_id", root);
    return n >= 0 && n < (int)sizeof(destination) &&
           fx_copy("/proc/sys/kernel/random/boot_id", destination);
}
