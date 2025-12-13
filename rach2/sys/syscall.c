#include "syscall.h"
#include "serial.h"
#include "kio.h"
#include "proc/process.h"
#include "proc/sched.h"
#include "fs/fd.h"
#include "fs/krffs.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "lib/kstring.h"

/* Kernel stack size (16KB) */
#define KERNEL_STACK_SIZE 0x4000

/* Fork child return point in isr_handler.s */
extern void isr_return(void);

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
    _syscall_table[SYS_DUP2] = sys_dup2;

    /* Register the interrupt handler */
    register_isr_handler(SYSCALL_VECTOR, _syscall_handler);
}
