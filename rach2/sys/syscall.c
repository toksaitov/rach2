#include "syscall.h"
#include "serial.h"
#include "kio.h"
#include "proc/process.h"
#include "proc/sched.h"
#include "proc/elf.h"
#include "fs/fd.h"
#include "fs/krffs.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "lib/kstring.h"

/* User stack base address (stack grows down from here) */
#define USER_STACK_BASE 0xBFFFF000

/* Kernel stack size (16KB) */
#define KERNEL_STACK_SIZE 0x4000

/* Fork child return point in isr_handler.s */
extern void isr_return(void);

/*
 * Stdin buffer from kernel.c - used for userspace stdin reads.
 * The keyboard handler fills this buffer when a process is waiting.
 */
#define STDIN_BUFFER_SIZE 256
extern char stdin_buffer[STDIN_BUFFER_SIZE];
extern volatile size_t stdin_pos;
extern volatile bool stdin_ready;
extern volatile process_t *stdin_waiting_proc;

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
 * Marks process as ZOMBIE, wakes parent if waiting, and reschedules.
 */
static int32_t sys_exit(isr_params_t *params)
{
    int status = (int)params->ebx;
    process_t *parent;

    serial_printf("sys_exit: Process %d exited with status %d\n",
                  current_proc ? (int)current_proc->pid : -1, status);
    k_printf("\nProcess exited with status %d\n", status);

    if (!current_proc) {
        __asm__ __volatile__("cli; hlt");
        return 0;
    }

    /* Mark process as zombie and store exit code */
    current_proc->state = PROC_ZOMBIE;
    current_proc->exit_code = status;

    /* Wake up parent if it's blocked (waiting for us) */
    parent = get_process(current_proc->parent_pid);
    if (parent && parent->state == PROC_BLOCKED) {
        parent->state = PROC_READY;
    }

    /* Schedule another process */
    schedule();

    /* If no other processes, halt */
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
 * sys_open - open a file or directory
 * Args: EBX = path, ECX = flags (O_RDONLY=0, O_WRONLY=1, O_RDWR=2)
 * Returns: fd number on success, -1 on error
 *
 * Special case: opening "/" opens the root directory for getdents64.
 */
/* Linux open flags */
#define O_CREAT  0100
#define O_TRUNC  01000

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

    /* Check if opening root directory "/", ".", or ".." (cwd is always "/" in KRFFS) */
    if (name[0] == '\0' ||
        (name[0] == '.' && name[1] == '\0') ||
        (name[0] == '.' && name[1] == '.' && name[2] == '\0')) {
        /* Opening root directory - allocate fd as directory type */
        fd = fd_alloc(&current_proc->fds);
        if (fd < 0) {
            return -1;
        }

        entry = &current_proc->fds.entries[fd];
        entry->type = FD_TYPE_DIR;
        entry->node = NULL;  /* NULL indicates root directory */
        entry->offset = 0;   /* Position in directory listing */
        entry->flags = FD_FLAG_READ;

        return fd;
    }

    /* Find the file in KRFFS */
    node = krffs_find_reserved_node_by_name(krffs_fs, name);
    if (!node) {
        /* File not found - check if O_CREAT is set */
        if (flags & O_CREAT) {
            /* Create the file */
            if (krffs_create(name) != 0) {
                return -1;  /* Failed to create file */
            }
            /* Now find the newly created node */
            node = krffs_find_reserved_node_by_name(krffs_fs, name);
            if (!node) {
                return -1;  /* Shouldn't happen, but check anyway */
            }
        } else {
            return -1;  /* File not found and O_CREAT not set */
        }
    } else if (flags & O_TRUNC) {
        /* File exists and O_TRUNC set - truncate to 0 length */
        krffs_truncate(name, 0);
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
 *
 * For stdin (fd 0, console with READ flag):
 * - Blocks the process until Enter is pressed
 * - Returns the line of input (including newline)
 * - Line buffering: keyboard handler fills stdin_buffer until Enter
 */
static int32_t sys_read(isr_params_t *params)
{
    int fd = (int)params->ebx;
    char *buf = (char *)params->ecx;
    uint32_t count = params->edx;
    fd_entry_t *entry;
    uint32_t bytes_to_copy;

    if (!current_proc || !buf) {
        return -1;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry) {
        return -1;
    }

    /* Console fd (stdin) - blocking read with line buffering */
    if (entry->type == FD_TYPE_CONSOLE) {
        if (!(entry->flags & FD_FLAG_READ)) {
            return -1;  /* Not readable (e.g., stdout/stderr) */
        }

        /*
         * Stdin read implementation:
         * 1. Reset stdin buffer state
         * 2. Register current process as waiting for stdin
         * 3. Block until Enter is pressed (keyboard handler wakes us)
         * 4. Copy data to user buffer
         * 5. Return bytes read
         */

        /* Reset stdin state for new input */
        stdin_pos = 0;
        stdin_ready = false;
        stdin_waiting_proc = current_proc;

        serial_printf("sys_read: PID %d blocking on stdin\n", current_proc->pid);

        /*
         * Block and wait for input using hlt.
         * We can't rely on schedule() because there may be no other READY
         * processes. Instead, we halt waiting for keyboard interrupt.
         * The keyboard handler will set stdin_ready=true when Enter is pressed.
         */
        current_proc->state = PROC_BLOCKED;
        while (!stdin_ready) {
            /* Enable interrupts and halt until next interrupt */
            __asm__ __volatile__("sti; hlt");
        }
        current_proc->state = PROC_RUNNING;

        /* We've been woken up - stdin_ready should be true now */
        serial_printf("sys_read: PID %d woken up, stdin_pos=%d\n",
                      current_proc->pid, (int)stdin_pos);

        /* Clear the waiting process */
        stdin_waiting_proc = NULL;

        /* Copy data to user buffer, limited by count */
        bytes_to_copy = stdin_pos;
        if (bytes_to_copy > count) {
            bytes_to_copy = count;
        }

        k_memcpy(buf, stdin_buffer, bytes_to_copy);

        /* Reset stdin state */
        stdin_pos = 0;
        stdin_ready = false;

        return (int32_t)bytes_to_copy;
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
 * sys_chdir - change current working directory
 * Args: EBX = path
 * Returns: 0 on success, -1 on error
 *
 * Since KRFFS has no directory hierarchy (only root "/"):
 * - "/" and "." and ".." succeed (stay in root)
 * - File names fail with -ENOTDIR
 * - Nonexistent paths fail with -ENOENT
 */
static int32_t sys_chdir(isr_params_t *params)
{
    const char *path = (const char *)params->ebx;

    if (!current_proc || !path) {
        return -1;
    }

    /* Handle root directory */
    if (path[0] == '/' && path[1] == '\0') {
        return 0;  /* Already in root */
    }

    /* Handle "." - stay in current directory */
    if (path[0] == '.' && path[1] == '\0') {
        return 0;
    }

    /* Handle ".." - parent of root is root */
    if (path[0] == '.' && path[1] == '.' && path[2] == '\0') {
        return 0;
    }

    /* Skip leading '/' if present */
    if (path[0] == '/') {
        path++;
    }

    /* Empty path after stripping '/' means root */
    if (path[0] == '\0') {
        return 0;
    }

    /* Check if path is a file (ENOTDIR) or doesn't exist (ENOENT) */
    if (krffs_fs && krffs_open(path) == 0) {
        /* File exists - it's a file, not a directory */
        return -1;  /* ENOTDIR */
    }

    /* Path doesn't exist */
    return -1;  /* ENOENT */
}

/*
 * sys_getcwd - get current working directory
 * Args: EBX = buf, ECX = size
 * Returns: buf pointer on success, NULL (0) on error
 */
static int32_t sys_getcwd(isr_params_t *params)
{
    char *buf = (char *)params->ebx;
    uint32_t size = params->ecx;
    uint32_t cwd_len;

    if (!current_proc || !buf || size == 0) {
        return 0;  /* NULL */
    }

    cwd_len = k_strlen(current_proc->cwd);
    if (cwd_len + 1 > size) {
        return 0;  /* Buffer too small */
    }

    /* Copy cwd to user buffer */
    k_memcpy(buf, current_proc->cwd, cwd_len + 1);

    return (int32_t)buf;
}

/*
 * Linux 32-bit stat structure (used by syscall 106)
 * This is the older structure used by ISH and some programs.
 */
struct linux_stat {
    unsigned long  st_dev;
    unsigned long  st_ino;
    unsigned short st_mode;
    unsigned short st_nlink;
    unsigned short st_uid;
    unsigned short st_gid;
    unsigned long  st_rdev;
    unsigned long  st_size;
    unsigned long  st_blksize;
    unsigned long  st_blocks;
    unsigned long  st_atime;
    unsigned long  st_atime_nsec;
    unsigned long  st_mtime;
    unsigned long  st_mtime_nsec;
    unsigned long  st_ctime;
    unsigned long  st_ctime_nsec;
    unsigned long  __unused4;
    unsigned long  __unused5;
};

/*
 * Linux stat64 structure for 32-bit systems
 */
struct linux_stat64 {
    unsigned long long st_dev;
    unsigned char      __pad0[4];
    unsigned long      __st_ino;
    unsigned int       st_mode;
    unsigned int       st_nlink;
    unsigned long      st_uid;
    unsigned long      st_gid;
    unsigned long long st_rdev;
    unsigned char      __pad3[4];
    long long          st_size;
    unsigned long      st_blksize;
    unsigned long long st_blocks;
    unsigned long      st_atime;
    unsigned long      st_atime_nsec;
    unsigned long      st_mtime;
    unsigned long      st_mtime_nsec;
    unsigned long      st_ctime;
    unsigned long      st_ctime_nsec;
    unsigned long long st_ino;
};

/* File type bits for st_mode */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000

/*
 * sys_stat64 - get file status
 * Args: EBX = path, ECX = stat64 buffer
 * Returns: 0 on success, -1 on error
 */
static int32_t sys_stat64(isr_params_t *params)
{
    const char *path = (const char *)params->ebx;
    struct linux_stat64 *st = (struct linux_stat64 *)params->ecx;
    const char *name;
    krffs_node_t *node;

    if (!current_proc || !path || !st) {
        return -1;
    }

    /* Clear the stat structure */
    k_memset(st, 0, sizeof(struct linux_stat64));

    /* Handle root directory "/" */
    if ((path[0] == '/' && path[1] == '\0') || path[0] == '\0') {
        st->st_mode = S_IFDIR | 0755;  /* drwxr-xr-x */
        st->st_nlink = 1;
        st->st_ino = 0;  /* Root inode is 0 */
        st->st_blksize = 4096;
        return 0;
    }

    /* Skip leading '/' if present */
    name = path;
    if (name[0] == '/') {
        name++;
    }

    /* Find the file in KRFFS */
    if (!krffs_fs) {
        return -1;
    }

    node = krffs_find_reserved_node_by_name(krffs_fs, name);
    if (!node) {
        return -1;  /* File not found */
    }

    /* Fill stat structure */
    st->st_mode = S_IFREG | (node->mode & 0777);  /* Regular file + permissions */
    st->st_nlink = 1;                              /* Always 1 for KRFFS */
    st->st_uid = node->uid;
    st->st_gid = node->gid;
    st->st_size = (long long)node->data_size;
    st->st_blksize = 4096;
    st->st_blocks = (node->data_size + 511) / 512;

    /* Use node's DJB hash as inode number */
    st->st_ino = node->id;
    st->__st_ino = (unsigned long)node->id;

    return 0;
}

/*
 * sys_stat - get file status (32-bit structure)
 * Args: EBX = path, ECX = stat buffer
 * Returns: 0 on success, -1 on error
 *
 * This is the older 32-bit stat syscall (106), used by ISH and some programs.
 * Uses struct linux_stat instead of struct linux_stat64.
 */
static int32_t sys_stat(isr_params_t *params)
{
    const char *path = (const char *)params->ebx;
    struct linux_stat *st = (struct linux_stat *)params->ecx;
    const char *name;
    krffs_node_t *node;

    if (!current_proc || !path || !st) {
        return -1;
    }

    /* Clear the stat structure */
    k_memset(st, 0, sizeof(struct linux_stat));

    /* Handle root directory "/" */
    if ((path[0] == '/' && path[1] == '\0') || path[0] == '\0') {
        st->st_mode = S_IFDIR | 0755;  /* drwxr-xr-x */
        st->st_nlink = 1;
        st->st_ino = 0;  /* Root inode is 0 */
        st->st_blksize = 4096;
        return 0;
    }

    /* Skip leading '/' if present */
    name = path;
    if (name[0] == '/') {
        name++;
    }

    /* Find the file in KRFFS */
    if (!krffs_fs) {
        return -1;
    }

    node = krffs_find_reserved_node_by_name(krffs_fs, name);
    if (!node) {
        return -1;  /* File not found */
    }

    /* Fill stat structure */
    st->st_mode = S_IFREG | (node->mode & 0777);  /* Regular file + permissions */
    st->st_nlink = 1;                              /* Always 1 for KRFFS */
    st->st_uid = (unsigned short)node->uid;
    st->st_gid = (unsigned short)node->gid;
    st->st_size = (unsigned long)node->data_size;
    st->st_blksize = 4096;
    st->st_blocks = (node->data_size + 511) / 512;

    /* Use node's DJB hash as inode number */
    st->st_ino = (unsigned long)node->id;

    return 0;
}

/*
 * copy_user_pages - copy all user pages from parent to child
 * Iterates through parent's page directory and copies user-accessible pages.
 * Returns 0 on success, -1 on failure.
 */
static int copy_user_pages(process_t *parent, page_dir_t *child_dir)
{
    page_dir_t *parent_dir;
    uint32_t dir_idx, page_idx;
    page_table_t *parent_table;

    /* Get parent's page directory */
    if (parent->cr3 == 0) {
        parent_dir = _kernel_dir;
    } else {
        /*
         * cr3 stores &page_dir->phys_page_tables (from get_page_dir_phys).
         * To get the page_dir_t*, we subtract the offset of phys_page_tables.
         * page_dir_t layout: logical_page_tables[1024] then phys_page_tables[1024]
         * So offset = 1024 * sizeof(page_table_t*) = 4096 bytes.
         */
        parent_dir = (page_dir_t *)(parent->cr3 - 1024 * sizeof(page_table_t *));
    }

    /* Iterate through all page directory entries */
    for (dir_idx = 0; dir_idx < 1024; dir_idx++) {
        parent_table = parent_dir->logical_page_tables[dir_idx];
        if (!parent_table) {
            continue;
        }

        /* Check each page in this table */
        for (page_idx = 0; page_idx < 1024; page_idx++) {
            uint32_t pte = parent_table->pages[page_idx];
            uint32_t virt_addr;
            uint32_t parent_phys;
            uint32_t child_phys;

            /* Check if page is present and user-accessible */
            if (!(pte & 0x1) || !(pte & 0x4)) {
                continue;  /* Not present or not user page */
            }

            virt_addr = (dir_idx << 22) | (page_idx << 12);
            parent_phys = pte & ~0xFFF;

            /* Skip identity-mapped kernel region (first 16MB) */
            if (virt_addr < HARD_MEMORY_LIMIT) {
                continue;
            }

            /* Allocate a new page for child at same virtual address */
            child_phys = alloc_user_page(child_dir, virt_addr);
            if (child_phys == 0) {
                serial_printf("copy_user_pages: alloc failed at 0x%X\n", virt_addr);
                return -1;  /* Out of memory */
            }

            /* Copy page content (both phys addrs are identity-mapped) */
            k_memcpy((void *)child_phys, (void *)parent_phys, PAGE_SIZE);
        }
    }

    return 0;
}

/*
 * sys_execve - replace current process image with a new program
 * Args: EBX = path, ECX = argv[], EDX = envp[]
 * Returns: does not return on success, -1 on error
 *
 * 1. Copy path and argv from user space
 * 2. Read ELF file from KRFFS
 * 3. Free old user pages
 * 4. Load new ELF segments
 * 5. Set up user stack with argc/argv
 * 6. Return to new entry point via modified iret frame
 */
/*
 * Static buffer for ELF loading - must not be on stack!
 * Kernel stack is only 16KB, so large buffers cause stack overflow.
 */
static char _exec_elf_buffer[16384];
static char _exec_path_buf[64];
static char _exec_argv_storage[512];
static char *_exec_argv_bufs[16];

static int32_t sys_execve(isr_params_t *params)
{
    const char *user_path = (const char *)params->ebx;
    char *const *user_argv = (char *const *)params->ecx;
    /* char *const *user_envp = (char *const *)params->edx; */  /* Unused for now */

    int argc = 0;
    uint32_t argv_offset = 0;

    uint32_t file_size;
    int bytes_read;
    uint32_t entry_point;
    page_dir_t *proc_dir;
    uintptr_t stack_phys;
    uint32_t *sp;
    uint32_t strings_start;
    uint32_t argv_ptrs_start;
    const char *name;
    int i;

    if (!current_proc) {
        return -1;
    }

    serial_printf("sys_execve: PID %d exec'ing\n", current_proc->pid);

    /* Copy path from user space */
    if (!user_path) {
        serial_printf("sys_execve: NULL path\n");
        return -1;
    }
    for (i = 0; i < 63 && user_path[i]; i++) {
        _exec_path_buf[i] = user_path[i];
    }
    _exec_path_buf[i] = '\0';

    serial_printf("sys_execve: path='%s'\n", _exec_path_buf);

    /* Copy argv from user space */
    if (user_argv) {
        for (argc = 0; argc < 15 && user_argv[argc]; argc++) {
            const char *arg = user_argv[argc];
            int len = 0;

            /* Copy string to static argv storage */
            _exec_argv_bufs[argc] = &_exec_argv_storage[argv_offset];
            while (arg[len] && argv_offset + len < 511) {
                _exec_argv_storage[argv_offset + len] = arg[len];
                len++;
            }
            _exec_argv_storage[argv_offset + len] = '\0';
            argv_offset += len + 1;
        }
    }
    _exec_argv_bufs[argc] = NULL;

    serial_printf("sys_execve: argc=%d\n", argc);
    for (i = 0; i < argc; i++) {
        serial_printf("sys_execve: argv[%d]='%s'\n", i, _exec_argv_bufs[i]);
    }

    /* Skip leading '/' if present */
    name = _exec_path_buf;
    if (name[0] == '/') {
        name++;
    }

    /* Check file system is mounted */
    if (!krffs_fs) {
        serial_printf("sys_execve: No file system\n");
        return -1;
    }

    /* Check file exists */
    if (krffs_open(name) != 0) {
        serial_printf("sys_execve: File not found: %s\n", name);
        return -1;
    }

    /* Read ELF file */
    file_size = krffs_get_file_size(name);
    if (file_size > sizeof(_exec_elf_buffer)) {
        serial_printf("sys_execve: File too large: %d\n", file_size);
        return -1;
    }

    bytes_read = krffs_read(name, _exec_elf_buffer, file_size, 0);
    if (bytes_read != (int)file_size) {
        serial_printf("sys_execve: Read error\n");
        return -1;
    }

    /* Validate ELF */
    if (!elf_check(_exec_elf_buffer, file_size)) {
        serial_printf("sys_execve: Invalid ELF\n");
        return -1;
    }

    entry_point = elf_entry_point(_exec_elf_buffer);
    serial_printf("sys_execve: ELF valid, entry=0x%X\n", entry_point);

    /* Get current process's page directory */
    if (current_proc->cr3 == 0) {
        proc_dir = _kernel_dir;
    } else {
        proc_dir = (page_dir_t *)(current_proc->cr3 - 1024 * sizeof(page_table_t *));
    }

    /* Free old user pages */
    free_user_pages(proc_dir);
    serial_printf("sys_execve: Freed old user pages\n");

    /* Load new ELF segments */
    if (elf_load(_exec_elf_buffer, file_size, proc_dir) != 0) {
        serial_printf("sys_execve: ELF load failed\n");
        /* Can't recover - process is in broken state */
        current_proc->state = PROC_ZOMBIE;
        current_proc->exit_code = -1;
        schedule();
        return -1;
    }
    serial_printf("sys_execve: ELF loaded\n");

    /* Allocate user stack */
    stack_phys = alloc_user_page(proc_dir, USER_STACK_BASE);
    if (stack_phys == 0) {
        serial_printf("sys_execve: Stack alloc failed\n");
        current_proc->state = PROC_ZOMBIE;
        current_proc->exit_code = -1;
        schedule();
        return -1;
    }

    /* Zero the stack page */
    k_memset((void *)stack_phys, 0, PAGE_SIZE);

    /*
     * Set up user stack with argc, argv pointers, and strings.
     * Stack layout (growing down from USER_STACK_BASE + PAGE_SIZE):
     *
     *   [arg strings]   - null-terminated strings
     *   [NULL]          - envp terminator
     *   [NULL]          - argv terminator
     *   [argv[argc-1]]  - pointers to arg strings
     *   ...
     *   [argv[0]]
     *   [argc]          <- ESP points here
     */

    /* Calculate where to put strings (near top of stack page) */
    strings_start = USER_STACK_BASE + PAGE_SIZE - 256;  /* Reserve 256 bytes for strings */

    /* Copy arg strings to stack (using physical address for now) */
    {
        uint32_t phys_strings = stack_phys + PAGE_SIZE - 256;
        uint32_t str_offset = 0;

        for (i = 0; i < argc; i++) {
            int len = k_strlen(_exec_argv_bufs[i]) + 1;
            k_memcpy((void *)(phys_strings + str_offset), _exec_argv_bufs[i], len);
            /* Save virtual address for argv pointer */
            _exec_argv_bufs[i] = (char *)(strings_start + str_offset);
            str_offset += len;
        }
    }

    /* Calculate where to put argv pointers and argc */
    argv_ptrs_start = strings_start - 4;  /* NULL terminator for envp */
    argv_ptrs_start -= 4;                  /* NULL terminator for argv */
    argv_ptrs_start -= argc * 4;           /* argv pointers */
    argv_ptrs_start -= 4;                  /* argc */

    /* Align to 16 bytes (required for some ABIs) */
    argv_ptrs_start &= ~0xF;

    /* Set up the stack using physical address */
    sp = (uint32_t *)(stack_phys + (argv_ptrs_start - USER_STACK_BASE));

    /* Push argc */
    *sp++ = argc;

    /* Push argv pointers */
    for (i = 0; i < argc; i++) {
        *sp++ = (uint32_t)_exec_argv_bufs[i];
    }
    *sp++ = 0;  /* NULL terminator for argv */

    *sp++ = 0;  /* NULL terminator for envp */

    serial_printf("sys_execve: Stack setup complete, esp=0x%X\n", argv_ptrs_start);

    /* Debug: print what's on the stack */
    {
        uint32_t *debug_sp = (uint32_t *)(stack_phys + (argv_ptrs_start - USER_STACK_BASE));
        serial_printf("sys_execve: Stack[0]=0x%X (argc)\n", debug_sp[0]);
        serial_printf("sys_execve: Stack[1]=0x%X (argv[0])\n", debug_sp[1]);
        if (argc > 1) {
            serial_printf("sys_execve: Stack[2]=0x%X (argv[1])\n", debug_sp[2]);
        }
        if (argc > 2) {
            serial_printf("sys_execve: Stack[3]=0x%X (argv[2])\n", debug_sp[3]);
        }
    }

    /*
     * Modify the ISR return frame to jump to new code.
     * When the syscall returns via iret, it will:
     * - Pop EIP = entry_point
     * - Pop ESP = new user stack
     * - Continue in the new program
     */
    params->eip = entry_point;
    params->user_esp = argv_ptrs_start;

    /*
     * Flush TLB to ensure new page mappings take effect.
     * After freeing old user pages and allocating new stack/code pages,
     * the TLB may have stale entries from the parent process (e.g., ISH).
     * Reloading CR3 flushes the entire TLB.
     */
    __asm__ __volatile__ (
        "movl %%cr3, %%eax\n\t"
        "movl %%eax, %%cr3"
        ::: "eax", "memory"
    );

    serial_printf("sys_execve: Returning to entry=0x%X esp=0x%X\n",
                  params->eip, params->user_esp);

    /* Return value doesn't matter - we won't use it */
    return 0;
}

/*
 * sys_fork - create a child process
 * Returns: 0 to child, child's PID to parent, -1 on error
 *
 * Clones the current process: PCB, page directory, user pages, fd table.
 * Child is set up to return from fork with value 0.
 */
static int32_t sys_fork(isr_params_t *params)
{
    process_t *parent = current_proc;
    process_t *child;
    page_dir_t *child_dir;
    uintptr_t child_kstack_base;
    uint32_t *child_sp;
    isr_params_t *child_frame;

    if (!parent) {
        return -1;
    }

    serial_printf("sys_fork: Parent PID %d forking\n", parent->pid);

    /* Allocate child process slot */
    child = alloc_process();
    if (!child) {
        serial_printf("sys_fork: alloc_process failed\n");
        return -1;
    }

    /* Clone page directory */
    child_dir = clone_page_directory();
    if (!child_dir) {
        serial_printf("sys_fork: clone_page_directory failed\n");
        free_process(child);
        return -1;
    }

    /* Copy user pages from parent to child */
    if (copy_user_pages(parent, child_dir) != 0) {
        serial_printf("sys_fork: copy_user_pages failed\n");
        free_process(child);
        return -1;
    }

    /* Allocate kernel stack for child */
    child_kstack_base = mem_reserve(KERNEL_STACK_SIZE, true);
    if (!child_kstack_base) {
        serial_printf("sys_fork: kernel stack alloc failed\n");
        free_process(child);
        return -1;
    }
    child->kernel_stack = child_kstack_base + KERNEL_STACK_SIZE;

    /* Set up child's page directory and parent link */
    child->cr3 = get_page_dir_phys(child_dir);
    child->parent_pid = parent->pid;

    /* Copy file descriptor table */
    k_memcpy(&child->fds, &parent->fds, sizeof(fd_table_t));

    /*
     * Set up child's kernel stack for returning from fork.
     *
     * Stack layout (growing down from kernel_stack):
     *   [ISR frame copied from parent, with eax=0]
     *   [return address = isr_return]
     *   [saved registers for switch_context: ebp, ebx, esi, edi]
     *
     * When child is scheduled via switch_context:
     * 1. Pops edi, esi, ebx, ebp
     * 2. ret jumps to isr_return
     * 3. isr_return restores segments, popal (eax=0), iret to userspace
     */
    child_sp = (uint32_t *)child->kernel_stack;

    /* Copy ISR frame from parent */
    child_sp -= sizeof(isr_params_t) / sizeof(uint32_t);
    child_frame = (isr_params_t *)child_sp;
    k_memcpy(child_frame, params, sizeof(isr_params_t));

    /* Set child's return value to 0 */
    child_frame->eax = 0;

    /* Push return address (isr_return) */
    --child_sp;
    *child_sp = (uint32_t)isr_return;

    /* Push saved registers for switch_context (EBP, EBX, ESI, EDI) */
    --child_sp;
    *child_sp = 0;  /* EBP */
    --child_sp;
    *child_sp = 0;  /* EBX */
    --child_sp;
    *child_sp = 0;  /* ESI */
    --child_sp;
    *child_sp = 0;  /* EDI */

    /* Set child's ESP for switch_context */
    child->regs.esp = (uint32_t)child_sp;
    child->state = PROC_READY;

    serial_printf("sys_fork: Child PID %d created, CR3=0x%X\n",
                  child->pid, child->cr3);

    /* Parent returns child's PID */
    return (int32_t)child->pid;
}

/*
 * sys_waitpid - wait for child process to exit
 * Args: EBX = pid (-1 for any child), ECX = status pointer, EDX = options
 * Returns: PID of exited child, or -1 on error
 *
 * If child is ZOMBIE, collects exit status and frees the process.
 * If child is still running, blocks until it exits.
 */
static int32_t sys_waitpid(isr_params_t *params)
{
    int pid = (int)params->ebx;
    int *status = (int *)params->ecx;
    /* int options = (int)params->edx; */  /* Unused for now */
    process_t *child = NULL;
    uint32_t child_pid;
    int exit_code;
    uint32_t i;

    if (!current_proc) {
        return -1;
    }

    serial_printf("sys_waitpid: PID %d waiting for %d\n", current_proc->pid, pid);

    while (1) {
        child = NULL;

        /* Search for matching child process */
        for (i = 0; i < MAX_PROCS; i++) {
            process_t *p = &proc_table[i];

            if (p->state == PROC_UNUSED) {
                continue;
            }
            if (p->parent_pid != current_proc->pid) {
                continue;
            }

            /* If pid == -1, any child matches; otherwise must match pid */
            if (pid != -1 && (int)p->pid != pid) {
                continue;
            }

            child = p;

            /* If child is zombie, collect status and clean up */
            if (child->state == PROC_ZOMBIE) {
                child_pid = child->pid;
                exit_code = child->exit_code;

                serial_printf("sys_waitpid: Child %d is zombie, status=%d\n",
                              child_pid, exit_code);

                /* Free the child's user pages and page directory */
                if (child->cr3 != 0) {
                    page_dir_t *child_dir;
                    child_dir = (page_dir_t *)(child->cr3 - 1024 * sizeof(page_table_t *));
                    free_user_pages(child_dir);
                    free_page_directory(child_dir);
                    serial_printf("sys_waitpid: Freed pages and page dir for child %d\n", child_pid);
                }

                /* Free the child process slot */
                free_process(child);

                /* Store exit status if pointer is valid */
                if (status) {
                    *status = exit_code;
                }

                return (int32_t)child_pid;
            }

            /* Found a non-zombie child, will wait for it */
            break;
        }

        /* No matching child found */
        if (!child) {
            serial_printf("sys_waitpid: No matching child found\n");
            return -1;
        }

        /* Child exists but not zombie - block and wait */
        serial_printf("sys_waitpid: Blocking PID %d waiting for child\n",
                      current_proc->pid);
        current_proc->state = PROC_BLOCKED;

        /* Yield to scheduler - will be woken when child exits */
        schedule();

        /* After being woken up, re-check if child is zombie */
        serial_printf("sys_waitpid: PID %d woken up, rechecking\n",
                      current_proc->pid);
    }
}

/*
 * sys_creat - create a new file
 * Args: EBX = path, ECX = mode
 * Returns: fd on success, -1 on error
 *
 * Creates a new file and returns an open file descriptor.
 * If file already exists, opens it for writing (truncates).
 */
static int32_t sys_creat(isr_params_t *params)
{
    const char *path = (const char *)params->ebx;
    uint32_t mode = params->ecx;
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

    /* Empty filename is invalid */
    if (name[0] == '\0') {
        return -1;
    }

    /* Check if file system is mounted */
    if (!krffs_fs) {
        return -1;
    }

    /* Create the file (or open if exists) */
    if (krffs_create(name) != 0) {
        return -1;
    }

    /* Find the node to set mode */
    node = krffs_find_reserved_node_by_name(krffs_fs, name);
    if (!node) {
        return -1;
    }

    /* Set file mode */
    node->mode = (uint16_t)(mode & 0777);

    /* Allocate a file descriptor */
    fd = fd_alloc(&current_proc->fds);
    if (fd < 0) {
        return -1;
    }

    /* Set up the fd entry for writing */
    entry = &current_proc->fds.entries[fd];
    entry->type = FD_TYPE_FILE;
    entry->node = node;
    entry->offset = 0;
    entry->flags = FD_FLAG_READ | FD_FLAG_WRITE;

    return fd;
}

/*
 * sys_unlink - delete a file
 * Args: EBX = path
 * Returns: 0 on success, -1 on error
 */
static int32_t sys_unlink(isr_params_t *params)
{
    const char *path = (const char *)params->ebx;
    const char *name;

    if (!current_proc || !path) {
        return -1;
    }

    /* Can't unlink root directory */
    if (path[0] == '/' && path[1] == '\0') {
        return -1;
    }

    /* Skip leading '/' if present */
    name = path;
    if (name[0] == '/') {
        name++;
    }

    /* Empty filename is invalid */
    if (name[0] == '\0') {
        return -1;
    }

    /* Check if file system is mounted */
    if (!krffs_fs) {
        return -1;
    }

    return krffs_unlink(name);
}

/*
 * sys_rename - rename a file
 * Args: EBX = old path, ECX = new path
 * Returns: 0 on success, -1 on error
 */
static int32_t sys_rename(isr_params_t *params)
{
    const char *old_path = (const char *)params->ebx;
    const char *new_path = (const char *)params->ecx;
    const char *old_name;
    const char *new_name;

    if (!current_proc || !old_path || !new_path) {
        return -1;
    }

    /* Skip leading '/' if present */
    old_name = old_path;
    if (old_name[0] == '/') {
        old_name++;
    }
    new_name = new_path;
    if (new_name[0] == '/') {
        new_name++;
    }

    /* Empty filenames are invalid */
    if (old_name[0] == '\0' || new_name[0] == '\0') {
        return -1;
    }

    /* Check if file system is mounted */
    if (!krffs_fs) {
        return -1;
    }

    return krffs_rename(old_name, new_name);
}

/*
 * Timespec structure for utimensat
 */
struct kernel_timespec {
    int32_t tv_sec;
    int32_t tv_nsec;
};

/* Special nsec values */
#define UTIME_NOW  ((1L << 30) - 1L)
#define UTIME_OMIT ((1L << 30) - 2L)

/* PIT frequency - timer ticks at 100 Hz */
#define PIT_FREQUENCY 100

/* Global tick counter from kernel.c - incremented at 100 Hz */
extern volatile uint32_t kernel_ticks;

/*
 * get_current_time - convert kernel ticks to seconds/nanoseconds
 * Ticks are at 100 Hz, so each tick is 10ms = 10,000,000 ns
 */
static void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t ticks = kernel_ticks;
    *seconds = ticks / PIT_FREQUENCY;
    *nanoseconds = (ticks % PIT_FREQUENCY) * 10000000LL;  /* 10ms per tick */
}

/*
 * sys_utimensat - update file timestamps
 * Args: EBX = dirfd (ignored, always use cwd), ECX = path, EDX = times, ESI = flags
 * Returns: 0 on success, -1 on error
 *
 * times is array of 2 timespec: [atime, mtime]
 * If times is NULL, set both to current time
 * If tv_nsec is UTIME_NOW, set to current time
 * If tv_nsec is UTIME_OMIT, leave unchanged
 */
static int32_t sys_utimensat(isr_params_t *params)
{
    /* int dirfd = (int)params->ebx; */  /* Ignored - always use cwd */
    const char *path = (const char *)params->ecx;
    struct kernel_timespec *times = (struct kernel_timespec *)params->edx;
    /* int flags = (int)params->esi; */  /* Ignored for now */
    const char *name;
    krffs_node_t *node;
    int64_t now_sec, now_nsec;

    if (!current_proc || !path) {
        return -1;
    }

    /* Skip leading '/' if present */
    name = path;
    if (name[0] == '/') {
        name++;
    }

    /* Empty filename - check if it's root "/" */
    if (name[0] == '\0') {
        /* Can't modify root timestamps, but return success */
        return 0;
    }

    /* Check if file system is mounted */
    if (!krffs_fs) {
        return -1;
    }

    /* Find the file */
    node = krffs_find_reserved_node_by_name(krffs_fs, name);
    if (!node) {
        return -1;  /* File not found */
    }

    /* Get current time (ticks since boot converted to seconds) */
    get_current_time(&now_sec, &now_nsec);

    /* Update timestamps */
    if (times == NULL) {
        /* NULL means set both to current time */
        node->access_time.seconds = now_sec;
        node->access_time.nanoseconds = now_nsec;
        node->modification_time.seconds = now_sec;
        node->modification_time.nanoseconds = now_nsec;
    } else {
        /* Update access time */
        if (times[0].tv_nsec == UTIME_OMIT) {
            /* Leave unchanged */
        } else if (times[0].tv_nsec == UTIME_NOW) {
            node->access_time.seconds = now_sec;
            node->access_time.nanoseconds = now_nsec;
        } else {
            node->access_time.seconds = times[0].tv_sec;
            node->access_time.nanoseconds = times[0].tv_nsec;
        }

        /* Update modification time */
        if (times[1].tv_nsec == UTIME_OMIT) {
            /* Leave unchanged */
        } else if (times[1].tv_nsec == UTIME_NOW) {
            node->modification_time.seconds = now_sec;
            node->modification_time.nanoseconds = now_nsec;
        } else {
            node->modification_time.seconds = times[1].tv_sec;
            node->modification_time.nanoseconds = times[1].tv_nsec;
        }
    }

    /* Update attribute change time (ctime is always updated on metadata change) */
    node->attribute_change_time.seconds = now_sec;
    node->attribute_change_time.nanoseconds = now_nsec;

    return 0;
}

/*
 * Linux dirent64 structure for getdents64 syscall
 */
struct linux_dirent64 {
    uint64_t       d_ino;     /* 64-bit inode number */
    int64_t        d_off;     /* offset to next entry */
    uint16_t       d_reclen;  /* Size of this dirent */
    uint8_t        d_type;    /* File type */
    char           d_name[1]; /* Filename (variable length, null-terminated) */
};

/* Directory entry types */
#define DT_REG  8   /* Regular file */
#define DT_DIR  4   /* Directory */

/*
 * sys_getdents64 - read directory entries
 * Args: EBX = fd, ECX = dirp (buffer), EDX = count (buffer size)
 * Returns: bytes read on success, 0 at end, -1 on error
 *
 * Fills the buffer with linux_dirent64 structures for each file.
 * Uses fd->offset to track position in directory (file index).
 */
static int32_t sys_getdents64(isr_params_t *params)
{
    int fd_num = (int)params->ebx;
    char *buf = (char *)params->ecx;
    uint32_t count = params->edx;
    fd_entry_t *entry;
    krffs_node_t *node;
    uint32_t bytes_written = 0;
    uint32_t file_index = 0;
    uint32_t target_index;
    uint32_t name_len;
    uint32_t rec_len;
    struct linux_dirent64 *dirent;

    if (!current_proc || !buf || count == 0) {
        return -1;
    }

    /* Get and validate fd */
    entry = fd_get(&current_proc->fds, fd_num);
    if (!entry) {
        return -1;
    }

    /* Must be a directory fd */
    if (entry->type != FD_TYPE_DIR) {
        return -1;
    }

    /* Check file system is mounted */
    if (!krffs_fs) {
        return -1;
    }

    /* entry->offset tracks which file index we're at */
    target_index = entry->offset;

    /* Iterate through KRFFS nodes */
    node = krffs_fs->node;
    do {
        if (node->type == KRFFS_RESERVED_NODE) {
            /* Found a file - check if we've reached our target position */
            if (file_index >= target_index) {
                /* Calculate dirent size with proper alignment */
                name_len = k_strlen((const char *)node->name);
                /* Size = fixed fields (19 bytes) + name + null + padding to 8-byte alignment */
                rec_len = sizeof(uint64_t) + sizeof(int64_t) + sizeof(uint16_t) + sizeof(uint8_t) + name_len + 1;
                /* Align to 8 bytes */
                rec_len = (rec_len + 7) & ~7;

                /* Check if entry fits in remaining buffer */
                if (bytes_written + rec_len > count) {
                    /* No more room - return what we have */
                    if (bytes_written == 0) {
                        /* Buffer too small for even one entry */
                        return -1;
                    }
                    break;
                }

                /* Fill in the dirent structure */
                dirent = (struct linux_dirent64 *)(buf + bytes_written);
                dirent->d_ino = node->id;                      /* Use DJB hash as inode */
                dirent->d_off = (int64_t)(file_index + 1);     /* Next position */
                dirent->d_reclen = (uint16_t)rec_len;
                dirent->d_type = DT_REG;                       /* Regular file */

                /* Copy filename */
                k_memcpy(dirent->d_name, node->name, name_len + 1);

                bytes_written += rec_len;
                entry->offset = file_index + 1;  /* Update position for next call */
            }
            file_index++;
        }
    } while ((node = krffs_get_next_node(krffs_fs, node)) != krffs_fs->node);

    /* If we wrote nothing and went through all files, we're at end */
    if (bytes_written == 0 && file_index > 0 && target_index >= file_index) {
        return 0;  /* End of directory */
    }

    return (int32_t)bytes_written;
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
    _syscall_table[SYS_FORK] = sys_fork;
    _syscall_table[SYS_READ] = sys_read;
    _syscall_table[SYS_WRITE] = sys_write;
    _syscall_table[SYS_OPEN] = sys_open;
    _syscall_table[SYS_CLOSE] = sys_close;
    _syscall_table[SYS_WAITPID] = sys_waitpid;
    _syscall_table[SYS_CREAT] = sys_creat;
    _syscall_table[SYS_UNLINK] = sys_unlink;
    _syscall_table[SYS_EXECVE] = sys_execve;
    _syscall_table[SYS_CHDIR] = sys_chdir;
    _syscall_table[SYS_RENAME] = sys_rename;
    _syscall_table[SYS_DUP2] = sys_dup2;
    _syscall_table[SYS_STAT] = sys_stat;
    _syscall_table[SYS_GETCWD] = sys_getcwd;
    _syscall_table[SYS_STAT64] = sys_stat64;
    _syscall_table[SYS_GETDENTS64] = sys_getdents64;
    _syscall_table[SYS_UTIMENSAT] = sys_utimensat;

    /* Register the interrupt handler */
    register_isr_handler(SYSCALL_VECTOR, _syscall_handler);
}
