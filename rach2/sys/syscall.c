#include "syscall.h"
#include "serial.h"
#include "kio.h"
#include "proc/process.h"
#include "proc/sched.h"
#include "fs/fd.h"
#include "fs/krffs.h"

/*
 * Syscall function type - takes pointer to params, returns int32_t
 */
typedef int32_t (*syscall_fn_t)(isr_params_t *);

/*
 * Syscall dispatch table
 */
static syscall_fn_t _syscall_table[SYSCALL_TABLE_SIZE];

/*
 * sys_exit - terminate current process
 * Args: EBX = exit status
 * Returns: does not return
 *
 * For now, this marks the process as zombie and halts.
 * Full implementation would clean up resources and reschedule.
 */
static int32_t sys_exit(isr_params_t *params)
{
    int status = (int)params->ebx;

    serial_printf("sys_exit: Process %d exited with status %d\n",
                  current_proc ? (int)current_proc->pid : -1, status);
    k_printf("\nProcess exited with status %d\n", status);

    /* Mark process as zombie */
    if (current_proc) {
        current_proc->state = PROC_ZOMBIE;
        current_proc->exit_code = status;
    }

    /* Halt the CPU - for now, sys_exit just stops everything */
    __asm__ __volatile__("cli; hlt");

    /* Never reached */
    return 0;
}

/*
 * sys_write - write to file descriptor
 * Args: EBX = fd, ECX = buf, EDX = count
 * Returns: number of bytes written, or -1 on error
 */
static int32_t sys_write(isr_params_t *params)
{
    int fd = (int)params->ebx;
    const char *buf = (const char *)params->ecx;
    uint32_t count = params->edx;
    fd_entry_t *entry;
    uint32_t i;

    if (!current_proc) {
        return -1;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry) {
        return -1;
    }

    /* Console fd - write to VGA and serial */
    if (entry->type == FD_TYPE_CONSOLE) {
        if (!(entry->flags & FD_FLAG_WRITE)) {
            return -1;  /* Not writable (e.g., stdin) */
        }
        for (i = 0; i < count; i++) {
            k_putc(buf[i]);
            serial_putc(buf[i]);
        }
        return (int32_t)count;
    }

    /* File fd - write to KRFFS file */
    if (entry->type == FD_TYPE_FILE) {
        int written;
        const char *path;

        if (!(entry->flags & FD_FLAG_WRITE)) {
            return -1;  /* Not opened for writing */
        }

        if (!entry->node) {
            return -1;
        }

        path = (const char *)entry->node->name;
        written = krffs_write(path, buf, count, entry->offset);
        if (written > 0) {
            entry->offset += (uint32_t)written;
        }
        return written;
    }

    return -1;
}

/*
 * sys_open - open a file
 * Args: EBX = path, ECX = flags (O_RDONLY=0, O_WRONLY=1, O_RDWR=2)
 * Returns: fd number on success, -1 on error
 */
static int32_t sys_open(isr_params_t *params)
{
    const char *path = (const char *)params->ebx;
    int flags = (int)params->ecx;
    fd_entry_t *entry;
    krffs_node_t *node;
    const char *name;
    int fd;

    if (!current_proc || !path) {
        return -1;
    }

    /* Skip leading '/' if present */
    name = path;
    if (name[0] == '/') {
        name++;
    }

    /* Find the file in KRFFS */
    node = krffs_find_reserved_node_by_name(krffs_fs, name);
    if (!node) {
        return -1;  /* File not found */
    }

    /* Allocate a file descriptor */
    fd = fd_alloc(&current_proc->fds);
    if (fd < 0) {
        return -1;  /* No free fds */
    }

    /* Set up the fd entry */
    entry = &current_proc->fds.entries[fd];
    entry->type = FD_TYPE_FILE;
    entry->node = node;
    entry->offset = 0;

    /* Convert Linux flags to internal flags */
    /* O_RDONLY=0, O_WRONLY=1, O_RDWR=2 */
    if (flags == 0) {
        entry->flags = FD_FLAG_READ;
    } else if (flags == 1) {
        entry->flags = FD_FLAG_WRITE;
    } else {
        entry->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    }

    return fd;
}

/*
 * sys_close - close a file descriptor
 * Args: EBX = fd
 * Returns: 0 on success, -1 on error
 */
static int32_t sys_close(isr_params_t *params)
{
    int fd = (int)params->ebx;

    if (!current_proc) {
        return -1;
    }

    /* Don't allow closing stdin/stdout/stderr */
    if (fd < 3) {
        return -1;
    }

    return fd_free(&current_proc->fds, fd);
}

/*
 * sys_read - read from file descriptor
 * Args: EBX = fd, ECX = buf, EDX = count
 * Returns: number of bytes read, or -1 on error
 */
static int32_t sys_read(isr_params_t *params)
{
    int fd = (int)params->ebx;
    char *buf = (char *)params->ecx;
    uint32_t count = params->edx;
    fd_entry_t *entry;

    if (!current_proc || !buf) {
        return -1;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry) {
        return -1;
    }

    /* Console fd (stdin) - not implemented yet, return 0 */
    if (entry->type == FD_TYPE_CONSOLE) {
        if (!(entry->flags & FD_FLAG_READ)) {
            return -1;  /* Not readable (e.g., stdout) */
        }
        /* stdin reading will be implemented in Phase 23 */
        return 0;
    }

    /* File fd - read from KRFFS file */
    if (entry->type == FD_TYPE_FILE) {
        int bytes_read;
        const char *path;

        if (!(entry->flags & FD_FLAG_READ)) {
            return -1;  /* Not opened for reading */
        }

        if (!entry->node) {
            return -1;
        }

        path = (const char *)entry->node->name;
        bytes_read = krffs_read(path, buf, count, entry->offset);
        if (bytes_read > 0) {
            entry->offset += (uint32_t)bytes_read;
        }
        return bytes_read;
    }

    return -1;
}

/*
 * sys_dup2 - duplicate file descriptor
 * Args: EBX = oldfd, ECX = newfd
 * Returns: newfd on success, -1 on error
 */
static int32_t sys_dup2(isr_params_t *params)
{
    int oldfd = (int)params->ebx;
    int newfd = (int)params->ecx;
    fd_entry_t *old_entry;
    fd_entry_t *new_entry;

    if (!current_proc) {
        return -1;
    }

    /* Validate oldfd */
    old_entry = fd_get(&current_proc->fds, oldfd);
    if (!old_entry) {
        return -1;
    }

    /* Validate newfd range */
    if (newfd < 0 || newfd >= MAX_FDS) {
        return -1;
    }

    /* If oldfd == newfd, just return newfd */
    if (oldfd == newfd) {
        return newfd;
    }

    /* Close newfd if it's open (ignore errors) */
    if (fd_is_valid(&current_proc->fds, newfd)) {
        fd_free(&current_proc->fds, newfd);
    }

    /* Copy the entry */
    new_entry = &current_proc->fds.entries[newfd];
    new_entry->type = old_entry->type;
    new_entry->node = old_entry->node;
    new_entry->offset = old_entry->offset;
    new_entry->flags = old_entry->flags;

    return newfd;
}

/*
 * Syscall handler - dispatches to appropriate syscall function
 * Stores return value in params->eax so it's returned to userspace
 */
static void _syscall_handler(isr_params_t *params)
{
    uint32_t syscall_num = params->eax;
    int32_t ret = -1;

    if (syscall_num < SYSCALL_TABLE_SIZE && _syscall_table[syscall_num]) {
        ret = _syscall_table[syscall_num](params);
    }

    /* Store return value in EAX - will be restored by popal */
    params->eax = (uint32_t)ret;
}

void init_syscall(void)
{
    uint32_t i;

    /* Clear dispatch table */
    for (i = 0; i < SYSCALL_TABLE_SIZE; i++) {
        _syscall_table[i] = NULL;
    }

    /* Register implemented syscalls */
    _syscall_table[SYS_EXIT] = sys_exit;
    _syscall_table[SYS_READ] = sys_read;
    _syscall_table[SYS_WRITE] = sys_write;
    _syscall_table[SYS_OPEN] = sys_open;
    _syscall_table[SYS_CLOSE] = sys_close;
    _syscall_table[SYS_DUP2] = sys_dup2;

    /* Register the interrupt handler */
    register_isr_handler(SYSCALL_VECTOR, _syscall_handler);
}
