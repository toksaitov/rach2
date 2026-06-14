#include "syscall.h"
#include "serial.h"
#include "kio.h"
#include "kbd.h"
#include "mouse.h"
#include "proc/process.h"
#include "proc/sched.h"
#include "proc/elf.h"
#include "proc/uimage.h"
#include "proc/tls.h"
#include "tables/gdt.h"
#include "lib/krandom.h"
#include "lib/kerrno.h"
#include "lib/ksignal.h"
#include "fs/fd.h"
#include "fs/krffs.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "mm/usercopy.h"
#include "mm/vm.h"
#include "lib/kuser.h"
#include "lib/kstring.h"
#include "display/vga_gfx.h"
#include "speaker.h"
#include "audio/audio.h"
#include "network/socket/socket.h"
#include "network/udp/udp.h"
#include "network/tcp/tcp.h"
#include "network/ip/icmp.h"

/* User stack base address (stack grows down from here) */
#define USER_STACK_BASE 0xBFFFF000

/* Validate a user-space address range (defined with the socketcall helpers). */
static int user_access_ok(uint32_t addr, uint32_t len);

/* Validate AND fault in a user buffer before the kernel hands the raw pointer to
 * a lower layer that dereferences it directly (krffs, tcp, udp, drivers). 'write'
 * is 1 if that layer writes into the buffer. Defined with user_access_ok. */
static int user_buffer_ok(uint32_t addr, uint32_t len, int write);

/*
 * Framebuffer device tracking.
 * Only one process can own the framebuffer at a time.
 * When the owner exits or closes fb, text mode is restored.
 */
static volatile uint32_t fb_owner_pid = 0;

/*
 * Release framebuffer if owned by specified PID.
 * Called when a process is killed (CTRL+C) to restore text mode.
 */
void fb_release_if_owner(uint32_t pid)
{
    if (fb_owner_pid == pid && fb_owner_pid != 0) {
        vga_mode13h_exit();
        fb_owner_pid = 0;
    }
}

/*
 * ioctl request codes (Linux compatible subset)
 */
#define KIOCSOUND      0x4B2F  /* Set PC speaker frequency (0 = off) */
#define TCGETS         0x5401  /* Get terminal attributes (termios) */
#define TIOCGWINSZ     0x5413  /* Get terminal window size */
#define FB_MODE_13H    0x4601  /* Switch to VGA Mode 13h */
#define FB_MODE_TEXT   0x4602  /* Switch back to text mode */
#define FB_SETPALETTE  0x4603  /* Set 256-color palette (768 bytes) */

/*
 * Audio ioctl request codes
 */
#define AUDIO_GET_INFO    0x4101  /* Get audio driver info */
#define AUDIO_START       0x4102  /* Start audio playback */
#define AUDIO_STOP        0x4103  /* Stop audio playback */
#define AUDIO_WRITE       0x4104  /* Write audio samples */
#define AUDIO_SET_VOLUME  0x4105  /* Set volume (0-100) */

/*
 * Audio info structure returned by AUDIO_GET_INFO
 */
struct audio_info {
    uint32_t driver_type;   /* AUDIO_DRIVER_NONE, SPEAKER, SB16, HDA */
    uint32_t has_real_audio; /* 1 if real audio (not PC speaker) */
};

/*
 * Audio start parameters for AUDIO_START
 */
struct audio_start_params {
    uint32_t sample_rate;   /* e.g., 11025, 22050, 44100 */
    uint32_t format;        /* 0 = U8, 1 = S16LE */
    uint32_t channels;      /* 1 = mono, 2 = stereo */
};

/*
 * Audio write parameters for AUDIO_WRITE
 */
struct audio_write_params {
    const uint8_t *buffer;
    uint32_t size;
};

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
 * Init process tracking from kernel.c.
 * When init exits, we return to the kernel debug shell.
 */
extern volatile uint32_t init_pid;

/* Shell loop from kernel.c - called when init exits */
extern void shell_loop(void);

/* Kernel stack from loader.s */
extern char stack[];
#define KERNEL_STACK_SIZE_LOCAL 0x4000

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
 * If init process exits, returns to kernel debug shell.
 */
static int32_t sys_exit(isr_params_t *params)
{
    int status = (int)params->ebx;
    process_t *parent;
    bool is_init = false;
    uint32_t exiting_pid;

    /* Check if this process owns the framebuffer - restore text mode */
    if (current_proc && fb_owner_pid == current_proc->pid) {
        vga_mode13h_exit();
        fb_owner_pid = 0;
    }

    /* Release any sockets this process still holds open, so the slots,
     * port bindings, and TCBs are not leaked. */
    if (current_proc) {
        int fdi;
        for (fdi = 0; fdi < MAX_FDS; fdi++) {
            fd_entry_t *e = &current_proc->fds.entries[fdi];
            if (e->type == FD_TYPE_SOCKET && e->sock) {
                socket_unref(e->sock);
                e->sock = NULL;
                e->type = FD_TYPE_NONE;
            }
        }
    }

    k_printf("\nProcess exited with status %d\n", status);
    serial_printf("\nProcess exited with status %d\n", status);

    if (!current_proc) {
        __asm__ __volatile__("cli; hlt");
        return 0;
    }

    exiting_pid = current_proc->pid;
    is_init = (exiting_pid == init_pid && init_pid != 0);

#ifdef RACH2_HARNESS
    /* Harness build: the instant the probe (run as init) exits, report its status
     * to QEMU's isa-debug-exit device and stop -- BEFORE schedule() can switch to
     * a still-READY child and bypass this path, so the runner always reads a real
     * exit code. */
    if (is_init) {
        harness_exit((uint8_t)status);
    }
#endif

    /* Mark process a zombie and store the Linux wait-status encoding of a normal
     * exit (the parent's waitpid/wait4 copies exit_code straight out): the low 8
     * bits of the code go in bits 8-15, so WIFEXITED is true and WEXITSTATUS
     * returns the code. (harness_exit above still gets the raw status byte.) */
    current_proc->state = PROC_ZOMBIE;
    current_proc->exit_code = (status & 0xFF) << 8;

    /* Wake up parent if it's blocked (waiting for us) */
    parent = get_process(current_proc->parent_pid);
    if (parent && parent->state == PROC_BLOCKED) {
        parent->state = PROC_READY;
    }

    /* Schedule another process */
    schedule();

    /*
     * If we get here, there are no other READY processes.
     * If init exited, return to kernel debug shell.
     */
    if (is_init) {
        serial_printf("sys_exit: Init process %d exited, returning to kernel shell\n",
                      exiting_pid);
        k_puts("");
        k_puts("Init process exited. Returning to kernel debug shell.");

        /* Clean up the init process resources. Switch off its address space
         * BEFORE freeing the page directory: free_page_directory refuses to free
         * whatever CR3 currently points at, so freeing while still loaded would
         * leak the directory and its private page tables. */
        if (current_proc->cr3 != 0) {
            page_dir_t *init_dir;
            init_dir = (page_dir_t *)(current_proc->cr3 - 1024 * sizeof(page_table_t *));
            free_user_pages(init_dir);
            switch_page_directory(_kernel_dir);
            free_page_directory(init_dir);
        } else {
            switch_page_directory(_kernel_dir);
        }
        free_process(current_proc);

        /* Reset init tracking */
        init_pid = 0;

        /* Reset to kernel process 0 */
        current_proc = &proc_table[0];
        current_proc->state = PROC_RUNNING;

        /* Disable scheduler while in kernel shell */
        sched_disable();

        /* Re-enable interrupts and return to shell via longjmp-like mechanism.
         * We reset ESP to the kernel stack and jump to shell_loop.
         * This is safe because we're in kernel mode and init has been cleaned up.
         */
        __asm__ __volatile__(
            "movl %0, %%esp\n\t"    /* Reset to fresh kernel stack */
            "sti\n\t"               /* Re-enable interrupts */
            "jmp *%1\n\t"           /* Jump to shell_loop */
            :
            : "r"((uint32_t)stack + KERNEL_STACK_SIZE_LOCAL),
              "r"((uint32_t)shell_loop)
            : "memory"
        );

        /* Never reached */
    }

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
static int32_t write_fd(int fd, const char __user *buf, uint32_t count)
{
    fd_entry_t *entry;

    if (!current_proc) {
        return -ESRCH;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry) {
        return -EBADF;
    }

    /* Socket fd - send to the network */
    if (entry->type == FD_TYPE_SOCKET) {
        if (!entry->sock) {
            return -EBADF;
        }
        if (entry->sock->type == SOCK_STREAM) {
            if (!user_buffer_ok((uint32_t)buf, count, 0)) {
                return -EFAULT;
            }
            /* tcp_send copies the user buffer into socket send buffers itself;
             * __force strips the __user tag at this boundary. */
            return tcp_send(entry->sock, (const uint8_t __force *)buf, (int)count);
        }
        /* Datagram sockets send via sendto(). */
        return -EOPNOTSUPP;
    }

    /* Console fd - write to VGA and serial */
    if (entry->type == FD_TYPE_CONSOLE) {
        char kbuf[256];
        uint32_t done = 0;

        if (!(entry->flags & FD_FLAG_WRITE)) {
            return -EBADF;  /* Not writable (e.g., stdin) */
        }

        /* Pull the user bytes in through a small kernel bounce buffer, then emit
         * from kernel memory, looping until count bytes are done. A bad user
         * pointer yields -EFAULT only if nothing has been written yet (otherwise
         * report the bytes already emitted, matching the return-count convention). */
        while (done < count) {
            uint32_t chunk = count - done;
            uint32_t i;

            if (chunk > sizeof(kbuf)) {
                chunk = sizeof(kbuf);
            }
            if (copy_from_user(kbuf, (const void __user *)(buf + done), chunk) != 0) {
                return (done > 0) ? (int32_t)done : -EFAULT;
            }
            for (i = 0; i < chunk; i++) {
                k_putc(kbuf[i]);
                serial_putc(kbuf[i]);
            }
            done += chunk;
        }
        return (int32_t)count;
    }

    /* File fd - write to KRFFS file */
    if (entry->type == FD_TYPE_FILE) {
        int written;

        if (!(entry->flags & FD_FLAG_WRITE)) {
            return -EBADF;  /* Not opened for writing */
        }

        if (!entry->node) {
            return -EBADF;
        }

        /* krffs_write reads the user buffer directly, so range-check AND fault
         * its pages in first (as the socket path does) before stripping __user. */
        if (!user_buffer_ok((uint32_t)(__force uintptr_t)buf, count, 0)) {
            return -EFAULT;
        }
        /* Write via the node the descriptor already resolved (a subdir file's
         * node->name is only its basename, so re-resolving by name would fail).
         * krffs_write_node updates entry->node if a grow relocates the file. */
        written = krffs_write_node(&entry->node, (const char __force *)buf, count,
                                   entry->offset);
        if (written < 0) {
            return -EIO;  /* krffs reports failure with a bare -1 */
        }
        entry->offset += (uint32_t)written;
        return written;
    }

    /* Framebuffer fd - copy pixels to VGA memory */
    if (entry->type == FD_TYPE_FRAMEBUFFER) {
        uint32_t to_copy;

        if (!vga_graphics_mode) {
            return -EBADF;  /* Not in graphics mode */
        }

        /* Limit to framebuffer size */
        to_copy = count;
        if (to_copy > VGA_MODE13H_SIZE) {
            to_copy = VGA_MODE13H_SIZE;
        }

        /* Copy pixel data to framebuffer */
        if (copy_from_user((void *)VGA_FRAMEBUFFER, (const void __user *)buf, to_copy) != 0) {
            return -EFAULT;
        }

        return (int32_t)to_copy;
    }

    return -EBADF;
}

/*
 * sys_write - write to a file descriptor (write(2))
 * Args: EBX = fd, ECX = buf, EDX = count
 * Returns: number of bytes written, or a negative errno
 */
static int32_t sys_write(isr_params_t *params)
{
    return write_fd((int)params->ebx, (const char __user *)params->ecx, params->edx);
}

/*
 * sys_writev - gather write (writev(2)). musl's buffered stdio (printf) emits
 * its output through this call.
 * Args: EBX = fd, ECX = pointer to struct iovec[], EDX = iovcnt
 * Returns: total bytes written across the iovecs, or a negative errno.
 */
struct kernel_iovec {
    uint32_t iov_base;
    uint32_t iov_len;
};

#define IOV_MAX_LOCAL 1024

static int32_t sys_writev(isr_params_t *params)
{
    int fd = (int)params->ebx;
    uint32_t iov_user = params->ecx;
    int iovcnt = (int)params->edx;
    struct kernel_iovec iov;
    int32_t total = 0;
    int i;

    if (iovcnt < 0 || iovcnt > IOV_MAX_LOCAL) {
        return -EINVAL;
    }

    for (i = 0; i < iovcnt; i++) {
        int32_t r;

        /* On any fault once bytes were already emitted, report the partial total
         * (a caller that retried would otherwise duplicate output); only report
         * the error itself when nothing has been written yet. */
        if (copy_from_user(&iov,
                           (const void __user *)(iov_user +
                               (uint32_t)i * sizeof(struct kernel_iovec)),
                           sizeof(iov)) != 0) {
            return (total > 0) ? total : -EFAULT;
        }
        if (iov.iov_len == 0) {
            continue;
        }
        /* An individual length past the signed range, or a running total that
         * would overflow it, is rejected as Linux does (-EINVAL). */
        if (iov.iov_len > 0x7FFFFFFFu ||
            (uint32_t)total > 0x7FFFFFFFu - iov.iov_len) {
            return (total > 0) ? total : -EINVAL;
        }
        if (!user_access_ok(iov.iov_base, iov.iov_len)) {
            return (total > 0) ? total : -EFAULT;
        }

        r = write_fd(fd, (const char __user *)iov.iov_base, iov.iov_len);
        if (r < 0) {
            return (total > 0) ? total : r;
        }
        total += r;
        if ((uint32_t)r < iov.iov_len) {
            break;  /* short write -- stop, as writev does */
        }
    }

    return total;
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
    const char __user *path = (const char __user *)params->ebx;
    int flags = (int)params->ecx;
    fd_entry_t *entry;
    krffs_node_t *node;
    char pathbuf[256];
    const char *name;
    int len;
    int fd;

    if (!current_proc || !path) {
        return -EFAULT;
    }

    /* Copy the path into a kernel buffer; a bad user pointer or an over-long
     * path is reported with the errno strncpy_from_user returns. */
    len = strncpy_from_user(pathbuf, path, sizeof(pathbuf));
    if (len < 0) {
        return len;
    }

    /* Skip leading '/' if present */
    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }

    /* Check if opening framebuffer device "fb" */
    if (name[0] == 'f' && name[1] == 'b' && name[2] == '\0') {
        /* Only one process can own the framebuffer */
        if (fb_owner_pid != 0 && fb_owner_pid != current_proc->pid) {
            return -EBUSY;  /* Already in use */
        }

        /* Allocate fd for framebuffer */
        fd = fd_alloc(&current_proc->fds);
        if (fd < 0) {
            return -EMFILE;
        }

        entry = &current_proc->fds.entries[fd];
        entry->type = FD_TYPE_FRAMEBUFFER;
        entry->node = NULL;
        entry->offset = 0;
        entry->flags = FD_FLAG_READ | FD_FLAG_WRITE;

        /* Switch to Mode 13h and record owner */
        vga_mode13h_enter();
        fb_owner_pid = current_proc->pid;

        /* Clear any stale scancodes from shell */
        kbd_clear_scancodes();

        return fd;
    }

    /* Check if opening mouse device "mouse" */
    if (name[0] == 'm' && name[1] == 'o' && name[2] == 'u' &&
        name[3] == 's' && name[4] == 'e' && name[5] == '\0') {
        /* Check if mouse is available */
        if (!mouse_is_available()) {
            return -ENODEV;  /* Mouse not initialized */
        }

        /* Allocate fd for mouse */
        fd = fd_alloc(&current_proc->fds);
        if (fd < 0) {
            return -EMFILE;
        }

        entry = &current_proc->fds.entries[fd];
        entry->type = FD_TYPE_MOUSE;
        entry->node = NULL;
        entry->offset = 0;
        entry->flags = FD_FLAG_READ;

        /* Clear any stale mouse packets */
        mouse_clear_packets();

        return fd;
    }

    /* Check if opening root directory "/", ".", or ".." (cwd is always "/" in KRFFS) */
    if (name[0] == '\0' ||
        (name[0] == '.' && name[1] == '\0') ||
        (name[0] == '.' && name[1] == '.' && name[2] == '\0')) {
        /* Opening root directory - allocate fd as directory type */
        fd = fd_alloc(&current_proc->fds);
        if (fd < 0) {
            return -EMFILE;
        }

        entry = &current_proc->fds.entries[fd];
        entry->type = FD_TYPE_DIR;
        entry->node = NULL;  /* NULL indicates root directory */
        entry->offset = 0;   /* Position in directory listing */
        entry->flags = FD_FLAG_READ;

        return fd;
    }

    /* Find the file in KRFFS */
    node = krffs_resolve(name);
    if (!node) {
        /* File not found - check if O_CREAT is set */
        if (flags & O_CREAT) {
            /* Create the file */
            if (krffs_create(name) != 0) {
                return -ENOSPC;  /* Failed to create file */
            }
            /* Now find the newly created node */
            node = krffs_resolve(name);
            if (!node) {
                return -EIO;  /* Shouldn't happen, but check anyway */
            }
        } else {
            return -ENOENT;  /* File not found and O_CREAT not set */
        }
    } else if (flags & O_TRUNC) {
        /* File exists and O_TRUNC set - truncate to 0 length */
        krffs_truncate(name, 0);
    }

    /* Allocate a file descriptor */
    fd = fd_alloc(&current_proc->fds);
    if (fd < 0) {
        return -EMFILE;  /* No free fds */
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
    fd_entry_t *entry;

    if (!current_proc) {
        return -ESRCH;
    }

    /* Don't allow closing stdin/stdout/stderr */
    if (fd < 3) {
        return -EBADF;
    }

    /* Check if this is the framebuffer - need to restore text mode */
    entry = fd_get(&current_proc->fds, fd);
    if (entry && entry->type == FD_TYPE_FRAMEBUFFER) {
        /* Restore text mode and clear ownership */
        vga_mode13h_exit();
        fb_owner_pid = 0;
    }

    /* Drop this descriptor's reference to its socket. */
    if (entry && entry->type == FD_TYPE_SOCKET && entry->sock) {
        socket_unref(entry->sock);
    }

    /* fd_free reports a bad/unallocated descriptor with -1; translate to the
     * close(2) errno. */
    if (fd_free(&current_proc->fds, fd) != 0) {
        return -EBADF;
    }
    return 0;
}

/*
 * sys_lseek - reposition file offset
 * Args: EBX = fd, ECX = offset, EDX = whence (0=SET, 1=CUR, 2=END)
 * Returns: new offset on success, -1 on error
 */
static int32_t sys_lseek(isr_params_t *params)
{
    int fd = (int)params->ebx;
    int32_t offset = (int32_t)params->ecx;
    int whence = (int)params->edx;
    fd_entry_t *entry;
    uint32_t file_size;
    int32_t new_offset;

    if (!current_proc) {
        return -ESRCH;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_FILE) {
        return -EBADF;
    }

    /* Get file size from the node */
    if (!entry->node) {
        return -EBADF;
    }
    file_size = (uint32_t)entry->node->data_size;

    /* Calculate new offset */
    switch (whence) {
        case 0:  /* SEEK_SET */
            new_offset = offset;
            break;
        case 1:  /* SEEK_CUR */
            new_offset = (int32_t)entry->offset + offset;
            break;
        case 2:  /* SEEK_END */
            new_offset = (int32_t)file_size + offset;
            break;
        default:
            return -EINVAL;
    }

    /* Validate new offset */
    if (new_offset < 0) {
        return -EINVAL;
    }

    entry->offset = (uint32_t)new_offset;
    return new_offset;
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
    char __user *buf = (char __user *)params->ecx;
    uint32_t count = params->edx;
    fd_entry_t *entry;
    uint32_t bytes_to_copy;

    if (!current_proc || !buf) {
        return -EFAULT;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry) {
        return -EBADF;
    }

    /* Socket fd - receive from the network */
    if (entry->type == FD_TYPE_SOCKET) {
        if (!entry->sock || !user_buffer_ok((uint32_t)buf, count, 1)) {
            return -EFAULT;
        }
        /* socket_recv/socket_tcp_recv write straight into the validated user
         * buffer; __force strips the __user tag at the boundary. */
        if (entry->sock->type == SOCK_DGRAM) {
            return socket_recv(entry->sock, (uint8_t __force *)buf, (int)count,
                               NULL, NULL);
        }
        return socket_tcp_recv(entry->sock, (uint8_t __force *)buf, (int)count);
    }

    /* Console fd (stdin) - blocking read with line buffering */
    if (entry->type == FD_TYPE_CONSOLE) {
        if (!(entry->flags & FD_FLAG_READ)) {
            return -EBADF;  /* Not readable (e.g., stdout/stderr) */
        }

        /*
         * If the process owns the framebuffer (graphics mode), use non-blocking
         * raw scancode input for game controls.
         */
        if (fb_owner_pid == current_proc->pid) {
            /* Non-blocking raw scancode read for games. kbd_read_scancodes
             * writes into the user buffer; range-check it first, then __force
             * strips the __user tag at the boundary. */
            if (!user_buffer_ok((uint32_t)buf, count, 1)) {
                return -EFAULT;
            }
            return kbd_read_scancodes((uint8_t __force *)buf, (int)count);
        }

        /*
         * Stdin read implementation (text mode):
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

        /*
         * Block and wait for input using hlt.
         * We can't rely on schedule() because there may be no other READY
         * processes. Instead, we halt waiting for keyboard or serial input.
         * The keyboard handler will set stdin_ready=true when Enter is pressed.
         * We also poll serial input here to support piped commands via QEMU.
         */
        current_proc->state = PROC_BLOCKED;
        while (!stdin_ready) {
            /* Enable interrupts and halt until next interrupt */
            __asm__ __volatile__("sti; hlt");

            /*
             * After waking from hlt, check for serial input.
             * Process serial characters like keyboard: handle backspace,
             * newline, echo back to serial, and add to stdin_buffer.
             */
            while (serial_data_available()) {
                int ch = serial_try_getc();
                if (ch < 0) break;

                /* Handle CR (0x0D) as newline - terminals send CR on Enter */
                if (ch == '\r') ch = '\n';

                if (ch == '\n') {
                    /* Newline - mark input ready */
                    if (stdin_pos < STDIN_BUFFER_SIZE - 1) {
                        stdin_buffer[stdin_pos++] = '\n';
                    }
                    stdin_buffer[stdin_pos] = '\0';
                    stdin_ready = true;
                    k_putc('\n');
                    serial_putc('\r');
                    serial_putc('\n');
                    break;
                } else if (ch == '\b' || ch == 0x7F) {
                    /* Backspace or DEL - remove last character */
                    if (stdin_pos > 0) {
                        --stdin_pos;
                        k_putc('\b');
                        k_putc(' ');
                        k_putc('\b');
                        /* Echo backspace to serial */
                        serial_putc('\b');
                        serial_putc(' ');
                        serial_putc('\b');
                    }
                } else if (ch >= 0x20 && ch < 0x7F) {
                    /* Printable character - add to buffer and echo */
                    if (stdin_pos < STDIN_BUFFER_SIZE - 1) {
                        stdin_buffer[stdin_pos++] = (char)ch;
                        k_putc((char)ch);
                        serial_putc((char)ch);
                    }
                }
                /* Ignore other control characters */
            }
        }
        current_proc->state = PROC_RUNNING;

        /* Clear the waiting process */
        stdin_waiting_proc = NULL;

        /* Copy data to user buffer, limited by count */
        bytes_to_copy = stdin_pos;
        if (bytes_to_copy > count) {
            bytes_to_copy = count;
        }

        if (copy_to_user(buf, stdin_buffer, bytes_to_copy) != 0) {
            /* Reset stdin state even on a bad user buffer. */
            stdin_pos = 0;
            stdin_ready = false;
            return -EFAULT;
        }

        /* Reset stdin state */
        stdin_pos = 0;
        stdin_ready = false;

        return (int32_t)bytes_to_copy;
    }

    /* Mouse fd - non-blocking read of mouse packets */
    if (entry->type == FD_TYPE_MOUSE) {
        int packets_requested;
        int packets_read;

        /* Calculate how many complete packets can fit in user buffer */
        packets_requested = (int)(count / sizeof(struct mouse_packet));
        if (packets_requested <= 0) {
            return 0;  /* Buffer too small for even one packet */
        }

        /* Read packets from the mouse ring buffer. mouse_read_packets writes
         * into the user buffer; range-check it, then __force at the boundary. */
        if (!user_buffer_ok((uint32_t)buf,
                            (uint32_t)packets_requested * sizeof(struct mouse_packet),
                            1)) {
            return -EFAULT;
        }
        packets_read = mouse_read_packets((struct mouse_packet __force *)buf,
                                          packets_requested);

        return packets_read * (int32_t)sizeof(struct mouse_packet);
    }

    /* File fd - read from KRFFS file */
    if (entry->type == FD_TYPE_FILE) {
        int bytes_read;

        if (!(entry->flags & FD_FLAG_READ)) {
            return -EBADF;  /* Not opened for reading */
        }

        if (!entry->node) {
            return -EBADF;
        }

        /* krffs_read writes straight into the user buffer, so range-check AND
         * fault its pages in (write) before stripping the __user tag. */
        if (!user_buffer_ok((uint32_t)(__force uintptr_t)buf, count, 1)) {
            return -EFAULT;
        }
        /* Read via the resolved node (see the write path) -- the descriptor's
         * node->name is only the basename for a file in a subdirectory. */
        bytes_read = krffs_read_node(entry->node, (char __force *)buf, count,
                                     entry->offset);
        if (bytes_read < 0) {
            return -EIO;  /* krffs reports failure with a bare -1 */
        }
        entry->offset += (uint32_t)bytes_read;
        return bytes_read;
    }

    return -EBADF;
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
        return -ESRCH;
    }

    /* Validate oldfd */
    old_entry = fd_get(&current_proc->fds, oldfd);
    if (!old_entry) {
        return -EBADF;
    }

    /* Validate newfd range */
    if (newfd < 0 || newfd >= MAX_FDS) {
        return -EBADF;
    }

    /* If oldfd == newfd, just return newfd */
    if (oldfd == newfd) {
        return newfd;
    }

    /* Close newfd if it's open (ignore errors), releasing any socket it held. */
    if (fd_is_valid(&current_proc->fds, newfd)) {
        new_entry = fd_get(&current_proc->fds, newfd);
        if (new_entry && new_entry->type == FD_TYPE_SOCKET && new_entry->sock) {
            socket_unref(new_entry->sock);
        }
        fd_free(&current_proc->fds, newfd);
    }

    /* Copy the entry. A duplicated socket gains another reference. */
    new_entry = &current_proc->fds.entries[newfd];
    new_entry->type = old_entry->type;
    new_entry->node = old_entry->node;
    new_entry->sock = old_entry->sock;
    new_entry->offset = old_entry->offset;
    new_entry->flags = old_entry->flags;

    if (new_entry->type == FD_TYPE_SOCKET && new_entry->sock) {
        socket_ref(new_entry->sock);
    }

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
    const char __user *upath = (const char __user *)params->ebx;
    char pathbuf[256];
    char *path;
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }

    /* Copy the path into a kernel buffer and work on the copy. */
    len = strncpy_from_user(pathbuf, upath, sizeof(pathbuf));
    if (len < 0) {
        return len;
    }
    path = pathbuf;

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
        return -ENOTDIR;  /* ENOTDIR */
    }

    /* Path doesn't exist */
    return -ENOENT;  /* ENOENT */
}

/*
 * sys_getcwd - get current working directory
 * Args: EBX = buf, ECX = size
 * Returns: buf pointer on success, NULL (0) on error
 */
static int32_t sys_getcwd(isr_params_t *params)
{
    char __user *buf = (char __user *)params->ebx;
    uint32_t size = params->ecx;
    uint32_t cwd_len;

    if (!current_proc) {
        return -ESRCH;
    }
    if (!buf) {
        return -EFAULT;
    }
    if (size == 0) {
        return -EINVAL;
    }

    cwd_len = k_strlen(current_proc->cwd);
    if (cwd_len + 1 > size) {
        return -ERANGE;  /* buffer too small for the path + NUL */
    }

    /* Copy cwd to user buffer */
    if (copy_to_user(buf, current_proc->cwd, cwd_len + 1) != 0) {
        return -EFAULT;
    }

    /* Linux getcwd(2) returns the length including the NUL on success. (Returning
     * the buffer pointer would be misread as an errno for high user addresses
     * whose top bit is set.) */
    return (int32_t)(cwd_len + 1);
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
    const char __user *path = (const char __user *)params->ebx;
    struct linux_stat64 __user *st = (struct linux_stat64 __user *)params->ecx;
    struct linux_stat64 kst;
    char pathbuf[256];
    const char *name;
    krffs_node_t *node;
    int len;

    if (!current_proc || !path || !st) {
        return -EFAULT;
    }

    /* Copy the path into a kernel buffer first. */
    len = strncpy_from_user(pathbuf, path, sizeof(pathbuf));
    if (len < 0) {
        return len;
    }

    /* Build the result in a kernel struct, then copy it out in one shot. */
    k_memset(&kst, 0, sizeof(kst));

    /* Handle root directory "/" */
    if ((pathbuf[0] == '/' && pathbuf[1] == '\0') || pathbuf[0] == '\0') {
        kst.st_mode = S_IFDIR | 0755;  /* drwxr-xr-x */
        kst.st_nlink = 1;
        kst.st_ino = 0;  /* Root inode is 0 */
        kst.st_blksize = 4096;
        if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
            return -EFAULT;
        }
        return 0;
    }

    /* Skip leading '/' if present */
    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }

    /* Find the file in KRFFS */
    if (!krffs_fs) {
        return -ENOENT;
    }

    node = krffs_resolve(name);
    if (!node) {
        return -ENOENT;  /* File not found */
    }

    /* Fill stat structure */
    kst.st_mode = S_IFREG | (node->mode & 0777);  /* Regular file + permissions */
    kst.st_nlink = 1;                              /* Always 1 for KRFFS */
    kst.st_uid = node->uid;
    kst.st_gid = node->gid;
    kst.st_size = (long long)node->data_size;
    kst.st_blksize = 4096;
    kst.st_blocks = (node->data_size + 511) / 512;

    /* Use node's DJB hash as inode number */
    kst.st_ino = node->id;
    kst.__st_ino = (unsigned long)node->id;

    if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
        return -EFAULT;
    }

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
    const char __user *path = (const char __user *)params->ebx;
    struct linux_stat __user *st = (struct linux_stat __user *)params->ecx;
    struct linux_stat kst;
    char pathbuf[256];
    const char *name;
    krffs_node_t *node;
    int len;

    if (!current_proc || !path || !st) {
        return -EFAULT;
    }

    /* Copy the path into a kernel buffer first. */
    len = strncpy_from_user(pathbuf, path, sizeof(pathbuf));
    if (len < 0) {
        return len;
    }

    /* Build the result in a kernel struct, then copy it out in one shot. */
    k_memset(&kst, 0, sizeof(kst));

    /* Handle root directory "/" */
    if ((pathbuf[0] == '/' && pathbuf[1] == '\0') || pathbuf[0] == '\0') {
        kst.st_mode = S_IFDIR | 0755;  /* drwxr-xr-x */
        kst.st_nlink = 1;
        kst.st_ino = 0;  /* Root inode is 0 */
        kst.st_blksize = 4096;
        if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
            return -EFAULT;
        }
        return 0;
    }

    /* Skip leading '/' if present */
    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }

    /* Find the file in KRFFS */
    if (!krffs_fs) {
        return -ENOENT;
    }

    node = krffs_resolve(name);
    if (!node) {
        return -ENOENT;  /* File not found */
    }

    /* Fill stat structure */
    kst.st_mode = S_IFREG | (node->mode & 0777);  /* Regular file + permissions */
    kst.st_nlink = 1;                              /* Always 1 for KRFFS */
    kst.st_uid = (unsigned short)node->uid;
    kst.st_gid = (unsigned short)node->gid;
    kst.st_size = (unsigned long)node->data_size;
    kst.st_blksize = 4096;
    kst.st_blocks = (node->data_size + 511) / 512;

    /* Use node's DJB hash as inode number */
    kst.st_ino = (unsigned long)node->id;

    if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
        return -EFAULT;
    }

    return 0;
}

/*
 * copy_user_pages - duplicate the parent's user address space into the child
 * COPY-ON-WRITE. Iterates the parent's page directory and, for each present user
 * page, maps the SAME physical frame into the child (bumping the frame refcount)
 * rather than copying it; writable pages are write-protected and PTE_COW-marked
 * in both directories so the first write faults into cow_fault for a private
 * copy. Returns 0 on success, -1 on failure.
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

            /* Check if page is present and user-accessible */
            if (!(pte & 0x1) || !(pte & 0x4)) {
                continue;  /* Not present or not user page */
            }

            virt_addr = (dir_idx << 22) | (page_idx << 12);
            parent_phys = pte & ~0xFFF;

            /* Skip the identity-mapped kernel region (first 32MB) and any bogus
             * mapping. */
            if (virt_addr < HARD_MEMORY_LIMIT || parent_phys == 0) {
                continue;
            }

            /*
             * Copy-on-write: share the frame instead of copying it. The child
             * maps the same physical frame, and the frame's reference count is
             * bumped so it survives until both sides drop it. A WRITABLE parent
             * page is write-protected and marked PTE_COW in BOTH directories, so
             * the first write on either side faults into cow_fault (which makes a
             * private copy and re-grants write). A READ-ONLY page (ELF text,
             * PROT_READ, or an already-COW page from an earlier fork) is shared
             * with its exact flags -- no copy; a write to genuine read-only text
             * still segfaults because it carries no PTE_COW.
             */
            if (pte & 0x2) {
                uint32_t cow = 0x1u | 0x4u | PTE_COW; /* present + user + COW (RO) */
                if (map_user_frame(child_dir, virt_addr, parent_phys, cow) != 0) {
                    return -1;
                }
                parent_table->pages[page_idx] = (pte & ~0x2u) | PTE_COW;
                paging_flush_tlb(virt_addr); /* the parent is the current dir */
            } else if (map_user_frame(child_dir, virt_addr, parent_phys,
                                      pte & 0xFFFu) != 0) {
                return -1;
            }
            frame_ref(parent_phys);
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
 * Increased to 512KB to support Doom (368KB).
 */
/* elf_read_fn over a KRFFS file (ctx is the path): lets the segment loader pull
 * the ELF header, program headers, and each PT_LOAD straight from the file. */
static int exec_krffs_read(void *ctx, void *dst, uint32_t count, uint32_t off)
{
    return krffs_read((const char *)ctx, (char *)dst, count, off);
}

static char _exec_path_buf[64];
static char _exec_argv_storage[512];
static char *_exec_argv_bufs[16];
static char _exec_envp_storage[512];
static char *_exec_envp_bufs[16];

/* A failed exec has already torn down the old address space, so the process
 * cannot continue. Mark it a zombie, wake any parent waiting on it (giving the
 * scheduler a target), switch away, and -- as a backstop -- never iret back
 * into the destroyed space. */
static void exec_fail_destroyed(void)
{
    process_t *parent;
    page_dir_t *dir;

    current_proc->state = PROC_ZOMBIE;
    /* A failed exec destroyed the image; report it as a SIGKILL death (Linux
     * wait-status: low 7 bits = the signal) so the parent's waitpid sees an
     * abnormal termination rather than a bogus exit code. */
    current_proc->exit_code = SIGKILL & 0x7F;

    /* Wake a parent blocked in wait() so the scheduler has a target. */
    parent = get_process(current_proc->parent_pid);
    if (parent && parent->state == PROC_BLOCKED) {
        parent->state = PROC_READY;
    }
    schedule();

    /* schedule() returned -> nothing else is runnable. The address space is
     * gone, so we cannot iret back. Tear the dead process down and resume the
     * kernel debug shell on a fresh kernel stack, exactly as the init-exit path
     * does -- never deadlock, never return into freed userspace. */
    if (current_proc->pid == init_pid) {
        init_pid = 0;
    }
    /* Switch to the kernel directory BEFORE freeing the dead one, otherwise
     * free_page_directory() refuses the still-active directory and leaks it. */
    sched_disable();
    switch_page_directory(_kernel_dir);
    if (current_proc->cr3 != 0) {
        dir = (page_dir_t *)(current_proc->cr3 - 1024 * sizeof(page_table_t *));
        free_user_pages(dir);
        free_page_directory(dir);
    }
    free_process(current_proc);
    current_proc = &proc_table[0];
    current_proc->state = PROC_RUNNING;
    __asm__ __volatile__("movl %0, %%esp\n\t"
                         "sti\n\t"
                         "jmp *%1\n\t"
                         :
                         : "r"((uint32_t)stack + KERNEL_STACK_SIZE_LOCAL),
                           "r"((uint32_t)shell_loop)
                         : "memory");
}

static int32_t sys_execve(isr_params_t *params)
{
    const char __user *user_path = (const char __user *)params->ebx;
    char *const __user *user_argv = (char *const __user *)params->ecx;
    char *const __user *user_envp = (char *const __user *)params->edx;

    int argc = 0;
    int envc = 0;
    uint32_t argv_offset = 0;
    uint32_t envp_offset = 0;

    uint32_t entry_point;
    elf_image_t img;
    uint8_t random16[16];
    page_dir_t *proc_dir;
    uintptr_t stack_phys;
    const char *name;
    int len;

    if (!current_proc) {
        return -ESRCH;
    }

    /* Copy the path from user space with the fault-safe byte-by-byte helper. A
     * bad user pointer or an over-long path is rejected by its return value, so a
     * kernel-range pointer cannot leak kernel memory. Same 63-char bound as
     * before (sizeof(_exec_path_buf) == 64, NUL included). */
    if (!user_path) {
        return -EFAULT;
    }
    len = strncpy_from_user(_exec_path_buf, user_path, sizeof(_exec_path_buf));
    if (len < 0) {
        return len;
    }

    /* Copy argv from user space, range-checking every user pointer and bounding
     * the cumulative offset so _exec_argv_storage cannot overflow. An argument
     * that does not fit fails the exec rather than being silently truncated. The
     * pointer array and each string are now read through the copy helpers, which
     * fault-check each access; the bounds and validation order are unchanged. */
    if (user_argv) {
        for (argc = 0; argc < 15; argc++) {
            uint32_t arg_ptr = 0;
            int r;

            /* Read the argv[argc] pointer out of the user array. */
            if (copy_from_user(&arg_ptr, (const void __user *)&user_argv[argc],
                               sizeof(arg_ptr)) != 0) {
                return -EFAULT;
            }
            if (arg_ptr == 0) {
                break;  /* NULL terminator */
            }
            if (argv_offset >= sizeof(_exec_argv_storage)) {
                return -EFAULT;
            }
            _exec_argv_bufs[argc] = &_exec_argv_storage[argv_offset];
            r = strncpy_from_user(&_exec_argv_storage[argv_offset],
                                  (const char __user *)arg_ptr,
                                  sizeof(_exec_argv_storage) - argv_offset);
            if (r == -EFAULT) {
                return -EFAULT;
            }
            if (r < 0) {
                return -E2BIG; /* did not fit */
            }
            argv_offset += (uint32_t)r + 1;
        }
    }
    _exec_argv_bufs[argc] = NULL;

    /* Copy the real envp the same way. */
    if (user_envp) {
        for (envc = 0; envc < 15; envc++) {
            uint32_t env_ptr = 0;
            int r;

            if (copy_from_user(&env_ptr, (const void __user *)&user_envp[envc],
                               sizeof(env_ptr)) != 0) {
                return -EFAULT;
            }
            if (env_ptr == 0) {
                break;  /* NULL terminator */
            }
            if (envp_offset >= sizeof(_exec_envp_storage)) {
                return -EFAULT;
            }
            _exec_envp_bufs[envc] = &_exec_envp_storage[envp_offset];
            r = strncpy_from_user(&_exec_envp_storage[envp_offset],
                                  (const char __user *)env_ptr,
                                  sizeof(_exec_envp_storage) - envp_offset);
            if (r == -EFAULT) {
                return -EFAULT;
            }
            if (r < 0) {
                return -E2BIG; /* did not fit */
            }
            envp_offset += (uint32_t)r + 1;
        }
    }
    _exec_envp_bufs[envc] = NULL;

    /* Skip leading '/' if present */
    name = _exec_path_buf;
    if (name[0] == '/') {
        name++;
    }

    /* Check file system is mounted */
    if (!krffs_fs) {
        return -ENOENT;
    }

    /* Check file exists */
    if (krffs_open(name) != 0) {
        return -ENOENT;
    }

    /* Get current process's page directory */
    if (current_proc->cr3 == 0) {
        proc_dir = _kernel_dir;
    } else {
        proc_dir = (page_dir_t *)(current_proc->cr3 - 1024 * sizeof(page_table_t *));
    }

    /* Validate the ELF header BEFORE tearing down the old image, so the common
     * "not a valid executable" failure leaves the calling process intact. */
    {
        Elf32_Ehdr ehdr;

        if (exec_krffs_read((void *)name, &ehdr, sizeof(ehdr), 0) != (int)sizeof(ehdr) ||
            !elf_valid_exec(&ehdr, sizeof(ehdr))) {
            return -ENOEXEC;
        }
    }

    /* Free old user pages, then stream the new program in (header + program
     * headers + each PT_LOAD; no 512 KB whole-file buffer). Past this point the
     * old image is gone, so any failure terminates the process rather than
     * returning into a destroyed address space. */
    free_user_pages(proc_dir);

    if (elf_load_image(exec_krffs_read, (void *)name, proc_dir, &img) != 0) {
        exec_fail_destroyed();
        __builtin_unreachable();
    }
    entry_point = img.entry;

    /* Allocate user stack */
    stack_phys = alloc_user_page(proc_dir, USER_STACK_BASE);
    if (stack_phys == 0) {
        exec_fail_destroyed();
        __builtin_unreachable();
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

    /* Build the SysV/i386 initial stack -- argc, the argv and real envp pointer
     * arrays, and the auxv -- in the freshly zeroed page. build_user_stack copies
     * the strings in and 16-byte-aligns the entry ESP; the layout is verified
     * host-side (tests/host/uimage_hosttest.c). */
    krandom_bytes(random16, 16);
    {
        uint32_t user_esp = build_user_stack((void *)stack_phys, USER_STACK_BASE,
                                             PAGE_SIZE, argc, _exec_argv_bufs,
                                             _exec_envp_bufs, &img, random16);

        if (user_esp == 0) {
            exec_fail_destroyed();
            __builtin_unreachable();
        }

        /* Modify the ISR return frame so iret resumes at the new program's
         * entry point on its fresh stack. */
        params->eip = entry_point;
        params->user_esp = user_esp;

        /* Reset thread-local storage for the new image. exec replaces the
         * program in place, so the trapframe still holds the old program's
         * segment selectors (its %gs may be the 0x33 TLS selector) and the PCB
         * still holds its TLS base. A fresh image has no TLS until its libc
         * calls set_thread_area, so restore the plain user-data selector (0x23,
         * as enter_usermode does for a brand-new process) in all four slots and
         * clear the base in both the PCB and the live GDT descriptor. */
        params->gs = 0x23;
        params->fs = 0x23;
        params->es = 0x23;
        params->ds = 0x23;
        current_proc->tls_base = 0;
        gdt_set_tls(0);

        /* Build the address-space map (VMAs) for the new image so the Phase 4
         * fault handler can demand-page, grow the stack, or segfault. The ELF
         * code/data pages themselves are still eagerly loaded above. */
        elf_setup_vmas(&current_proc->vm, &img, USER_STACK_BASE,
                       VM_STACK_MAX_GROWTH);
    }

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
        return -ESRCH;
    }

    /* Allocate child process slot */
    child = alloc_process();
    if (!child) {
        return -EAGAIN;
    }

    /* Clone page directory */
    child_dir = clone_page_directory();
    if (!child_dir) {
        free_process(child);
        return -ENOMEM;
    }

    /* Copy user pages from parent to child. On failure, release the frames
     * copy_user_pages already allocated and the cloned directory -- not just the
     * PCB -- or each failed fork leaks a directory and its private page tables.
     * child_dir is never the running (parent) directory, so it frees cleanly. */
    if (copy_user_pages(parent, child_dir) != 0) {
        free_user_pages(child_dir);
        free_page_directory(child_dir);
        free_process(child);
        return -ENOMEM;
    }

    /* Allocate kernel stack for child */
    child_kstack_base = mem_reserve(KERNEL_STACK_SIZE, true);
    if (!child_kstack_base) {
        free_user_pages(child_dir);
        free_page_directory(child_dir);
        free_process(child);
        return -ENOMEM;
    }
    child->kernel_stack = child_kstack_base + KERNEL_STACK_SIZE;

    /* Set up child's page directory and parent link */
    child->cr3 = get_page_dir_phys(child_dir);
    child->parent_pid = parent->pid;

    /* Inherit the parent's TLS base: the child's copied trapframe keeps the
     * parent's %gs selector, and its address space is a copy at the same
     * virtual addresses, so the same base is correct. Without this the context
     * switch would reprogram the TLS descriptor to 0 for the child and its
     * %gs-relative reads (errno, the stack canary) would fault or read garbage. */
    child->tls_base = parent->tls_base;

    /* The child's address space is a copy of the parent's at the same virtual
     * addresses (copy_user_pages duplicated the pages), so copy the VMA map too
     * -- otherwise the child would have no regions and the first heap/stack
     * fault would segfault it. (k_memcpy, not struct assignment, so the
     * freestanding kernel does not need a compiler-emitted memcpy.) */
    k_memcpy(&child->vm, &parent->vm, sizeof(child->vm));

    /* Copy file descriptor table. Inherited sockets gain a reference each so
     * the parent and child release independently. */
    k_memcpy(&child->fds, &parent->fds, sizeof(fd_table_t));
    {
        int fdi;
        for (fdi = 0; fdi < MAX_FDS; fdi++) {
            if (child->fds.entries[fdi].type == FD_TYPE_SOCKET &&
                child->fds.entries[fdi].sock) {
                socket_ref(child->fds.entries[fdi].sock);
            }
        }
    }

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
/* waitpid/wait4 options. */
#define WNOHANG 1

/* Bytes of struct rusage to zero-fill for wait4 (i386: two 8-byte timevals plus
 * 14 longs). We do not track per-process resource usage yet, so it is reported
 * as all zero. */
#define RUSAGE_BYTES 72

/*
 * Shared core of waitpid(7) and wait4(114). 'status' (may be NULL) receives the
 * Linux-encoded wait status that sys_exit/terminate already stored in exit_code;
 * 'rusage_ptr' (0 for waitpid, may be 0 for wait4) is zero-filled on reap. With
 * WNOHANG, returns 0 instead of blocking when a child exists but has not exited.
 */
static int32_t do_wait(int pid, int __user *status, int options, uint32_t rusage_ptr)
{
    process_t *child = NULL;
    uint32_t child_pid;
    int exit_code;
    uint32_t i;

    if (!current_proc) {
        return -ESRCH;
    }

    /* Validate AND fault in the status (and rusage) pointers up front (a NULL
     * status means "don't report status"), so a bad or not-yet-present address is
     * rejected before any child is reaped -- otherwise the copy_to_user below
     * could fail after free_process() has destroyed the zombie, losing it.
     * Prefault for write, since that is how the results are delivered. */
    if (status &&
        !user_buffer_ok((uint32_t)(__force uintptr_t)status, sizeof(int), 1)) {
        return -EFAULT;
    }
    if (rusage_ptr && !user_buffer_ok(rusage_ptr, RUSAGE_BYTES, 1)) {
        return -EFAULT;
    }

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

                /* Free the child's user pages and page directory */
                if (child->cr3 != 0) {
                    page_dir_t *child_dir;
                    child_dir = (page_dir_t *)(child->cr3 - 1024 * sizeof(page_table_t *));
                    free_user_pages(child_dir);
                    free_page_directory(child_dir);
                }

                /* Free the child process slot */
                free_process(child);

                /* Deliver the (already Linux-encoded) status and a zeroed rusage;
                 * the pointers were validated/prefaulted above. */
                if (status) {
                    copy_to_user((void __user *)status, &exit_code,
                                 sizeof(exit_code));
                }
                if (rusage_ptr) {
                    uint8_t zero[RUSAGE_BYTES];
                    k_memset(zero, 0, sizeof(zero));
                    copy_to_user((void __user *)rusage_ptr, zero, RUSAGE_BYTES);
                }

                return (int32_t)child_pid;
            }

            /* Found a non-zombie child, will wait for it */
            break;
        }

        /* No matching child found */
        if (!child) {
            return -ECHILD;
        }

        /* A child exists but has not exited. WNOHANG returns immediately (0);
         * otherwise block until it exits and the scheduler wakes us. */
        if (options & WNOHANG) {
            return 0;
        }
        current_proc->state = PROC_BLOCKED;
        schedule();
    }
}

/*
 * sys_waitpid - wait for a child to change state.
 * Args: EBX = pid (-1 = any child), ECX = status pointer, EDX = options.
 * Returns the reaped child's PID, 0 (WNOHANG, none ready), or -errno.
 */
static int32_t sys_waitpid(isr_params_t *params)
{
    return do_wait((int)params->ebx, (int __user *)params->ecx,
                   (int)params->edx, 0);
}

/*
 * sys_wait4 - like waitpid plus an optional rusage out-pointer (zero-filled).
 * Args: EBX = pid, ECX = status, EDX = options, ESI = rusage. musl's wait/waitpid
 * are implemented on top of this.
 */
static int32_t sys_wait4(isr_params_t *params)
{
    return do_wait((int)params->ebx, (int __user *)params->ecx,
                   (int)params->edx, params->esi);
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
    const char __user *path = (const char __user *)params->ebx;
    uint32_t mode = params->ecx;
    fd_entry_t *entry;
    krffs_node_t *node;
    char pathbuf[256];
    const char *name;
    int len;
    int fd;

    if (!current_proc || !path) {
        return -EFAULT;
    }

    /* Copy the path into a kernel buffer. */
    len = strncpy_from_user(pathbuf, path, sizeof(pathbuf));
    if (len < 0) {
        return len;
    }

    /* Skip leading '/' if present */
    name = pathbuf;
    if (name[0] == '/') {
        name++;
    }

    /* Empty filename is invalid */
    if (name[0] == '\0') {
        return -EINVAL;
    }

    /* Check if file system is mounted */
    if (!krffs_fs) {
        return -ENOENT;
    }

    /* Create the file (or open if exists) */
    if (krffs_create(name) != 0) {
        return -ENOSPC;
    }

    /* Find the node to set mode */
    node = krffs_resolve(name);
    if (!node) {
        return -EIO;
    }

    /* Set file mode */
    node->mode = (uint16_t)(mode & 0777);

    /* Allocate a file descriptor */
    fd = fd_alloc(&current_proc->fds);
    if (fd < 0) {
        return -EMFILE;
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
    const char __user *upath = (const char __user *)params->ebx;
    char pathbuf[256];
    const char *path;
    const char *name;
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }

    /* Copy the path into a kernel buffer and work on the copy. */
    len = strncpy_from_user(pathbuf, upath, sizeof(pathbuf));
    if (len < 0) {
        return len;
    }
    path = pathbuf;

    /* Can't unlink root directory */
    if (path[0] == '/' && path[1] == '\0') {
        return -EISDIR;
    }

    /* Skip leading '/' if present */
    name = path;
    if (name[0] == '/') {
        name++;
    }

    /* Empty filename is invalid */
    if (name[0] == '\0') {
        return -EINVAL;
    }

    /* Check if file system is mounted */
    if (!krffs_fs) {
        return -ENOENT;
    }

    return krffs_unlink(name);
}

/*
 * sys_mkdir - create a directory.
 * Args: EBX = path, ECX = mode (ignored -- no permission model). Returns 0 or
 * -errno (-EEXIST if it exists, -ENOENT if a parent is missing).
 */
static int32_t sys_mkdir(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
    char pathbuf[256];
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    if (!krffs_fs) {
        return -ENOENT;
    }
    len = strncpy_from_user(pathbuf, upath, sizeof(pathbuf));
    if (len < 0) {
        return len;
    }
    if (krffs_resolve(pathbuf)) {
        return -EEXIST;
    }
    if (krffs_mkdir(pathbuf) != 0) {
        return -ENOENT; /* a parent directory does not exist */
    }
    return 0;
}

/*
 * sys_rmdir - remove an empty directory.
 * Args: EBX = path. Returns 0 or -errno (-ENOENT, -ENOTDIR, -ENOTEMPTY).
 */
static int32_t sys_rmdir(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
    krffs_node_t *node;
    char pathbuf[256];
    int len;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    if (!krffs_fs) {
        return -ENOENT;
    }
    len = strncpy_from_user(pathbuf, upath, sizeof(pathbuf));
    if (len < 0) {
        return len;
    }
    node = krffs_resolve(pathbuf);
    if (!node) {
        return -ENOENT;
    }
    if (!KRFFS_IS_DIR(node->mode)) {
        return -ENOTDIR;
    }
    if (krffs_rmdir(pathbuf) != 0) {
        return -ENOTEMPTY; /* still has children */
    }
    return 0;
}

/*
 * sys_rename - rename a file
 * Args: EBX = old path, ECX = new path
 * Returns: 0 on success, -1 on error
 */
static int32_t sys_rename(isr_params_t *params)
{
    const char __user *old_path = (const char __user *)params->ebx;
    const char __user *new_path = (const char __user *)params->ecx;
    char old_buf[256];
    char new_buf[256];
    const char *old_name;
    const char *new_name;
    int len;

    if (!current_proc || !old_path || !new_path) {
        return -EFAULT;
    }

    /* Copy both paths into kernel buffers. */
    len = strncpy_from_user(old_buf, old_path, sizeof(old_buf));
    if (len < 0) {
        return len;
    }
    len = strncpy_from_user(new_buf, new_path, sizeof(new_buf));
    if (len < 0) {
        return len;
    }

    /* Skip leading '/' if present */
    old_name = old_buf;
    if (old_name[0] == '/') {
        old_name++;
    }
    new_name = new_buf;
    if (new_name[0] == '/') {
        new_name++;
    }

    /* Empty filenames are invalid */
    if (old_name[0] == '\0' || new_name[0] == '\0') {
        return -EINVAL;
    }

    /* Check if file system is mounted */
    if (!krffs_fs) {
        return -ENOENT;
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

/* Timer tick frequency (100 Hz) */
#define TIMER_FREQUENCY 100

/* Global tick counter from kernel.c - incremented at 100 Hz */
extern volatile uint32_t kernel_ticks;

/*
 * get_current_time - convert kernel ticks to seconds/nanoseconds
 * Ticks are at 100 Hz, so each tick is 10ms = 10,000,000 ns
 */
static void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t ticks = kernel_ticks;
    *seconds = ticks / TIMER_FREQUENCY;
    *nanoseconds = (ticks % TIMER_FREQUENCY) * 10000000LL;  /* 10ms per tick */
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
    const char __user *user_path = (const char __user *)params->ecx;
    struct kernel_timespec __user *user_times =
        (struct kernel_timespec __user *)params->edx;
    /* int flags = (int)params->esi; */  /* Ignored for now */
    char pathbuf[256];
    struct kernel_timespec ktimes[2];
    const struct kernel_timespec *times = NULL;
    const char *name;
    krffs_node_t *node;
    int64_t now_sec, now_nsec;
    int len;

    if (!current_proc || !user_path) {
        return -EFAULT;
    }

    /* Copy the path and (if given) the two timespecs into kernel memory through
     * the fault-safe helpers before touching them. */
    len = strncpy_from_user(pathbuf, user_path, sizeof(pathbuf));
    if (len < 0) {
        return len;
    }
    if (user_times) {
        if (copy_from_user(ktimes, user_times, sizeof(ktimes)) != 0) {
            return -EFAULT;
        }
        times = ktimes;
    }

    /* Skip leading '/' if present */
    name = pathbuf;
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
        return -ENOENT;
    }

    /* Find the file */
    node = krffs_resolve(name);
    if (!node) {
        return -ENOENT;  /* File not found */
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
        return -EFAULT;
    }

    /* Records are packed straight into the user buffer below, so range-check AND
     * fault in the whole [buf, buf+count) span once -- every record write then
     * stays inside present, validated user memory. */
    if (!user_buffer_ok((uint32_t)buf, count, 1)) {
        return -EFAULT;
    }

    /* Get and validate fd */
    entry = fd_get(&current_proc->fds, fd_num);
    if (!entry) {
        return -EBADF;
    }

    /* Must be a directory fd */
    if (entry->type != FD_TYPE_DIR) {
        return -ENOTDIR;
    }

    /* Check file system is mounted */
    if (!krffs_fs) {
        return -EIO;
    }

    /* entry->offset tracks which file index we're at */
    target_index = entry->offset;

    /* Iterate through KRFFS nodes */
    node = krffs_fs->node;
    do {
        if (node->type == KRFFS_RESERVED_NODE) {
            /* Found a file - check if we've reached our target position */
            if (file_index >= target_index) {
                /* Staging buffer for one record (a KRFFS name is at most
                 * KRFFS_FILE_NAME_BUFFER_SIZE bytes, so this always fits). */
                uint8_t recbuf[KRFFS_FILE_NAME_BUFFER_SIZE + 32];

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
                        return -EINVAL;
                    }
                    break;
                }

                /* Build the record in kernel memory, then copy it to the user
                 * buffer through the helper (no raw write into user space). */
                k_memset(recbuf, 0, rec_len);
                dirent = (struct linux_dirent64 *)recbuf;
                dirent->d_ino = node->id;                      /* Use DJB hash as inode */
                dirent->d_off = (int64_t)(file_index + 1);     /* Next position */
                dirent->d_reclen = (uint16_t)rec_len;
                dirent->d_type = DT_REG;                       /* Regular file */
                k_memcpy(dirent->d_name, node->name, name_len + 1);

                if (copy_to_user((void __user *)(buf + bytes_written),
                                 recbuf, rec_len) != 0) {
                    return (bytes_written > 0) ?
                           (int32_t)bytes_written : -EFAULT;
                }

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
 * timeval structure for gettimeofday
 */
struct kernel_timeval {
    int32_t tv_sec;   /* seconds */
    int32_t tv_usec;  /* microseconds */
};

/*
 * sys_gettimeofday - get time since boot
 * Args: EBX = tv (timeval*), ECX = tz (ignored)
 * Returns: 0 on success, -1 on error
 *
 * Returns time since boot based on kernel_ticks counter (100 Hz).
 */
static int32_t sys_gettimeofday(isr_params_t *params)
{
    struct kernel_timeval __user *tv = (struct kernel_timeval __user *)params->ebx;
    struct kernel_timeval ktv;

    if (!tv) {
        return -EFAULT;
    }

    /* Build the result in a kernel struct, then copy it out (this also closes a
     * previously unchecked raw write to the user pointer).
     * kernel_ticks runs at 100 Hz, so 1 tick = 10ms = 10000us */
    ktv.tv_sec = (int32_t)(kernel_ticks / 100);
    ktv.tv_usec = (int32_t)((kernel_ticks % 100) * 10000);

    if (copy_to_user(tv, &ktv, sizeof(ktv)) != 0) {
        return -EFAULT;
    }

    return 0;
}

/*
 * sys_ioctl - device control
 * Args: EBX = fd, ECX = request, EDX = arg
 * Returns: 0 on success, -1 on error
 *
 * Supported requests:
 * - KIOCSOUND: Set PC speaker frequency (arg = freq, 0 = off)
 * - FB_SETPALETTE: Set 256-color palette (arg = pointer to 768 bytes)
 */
static int32_t sys_ioctl(isr_params_t *params)
{
    int fd_num = (int)params->ebx;
    uint32_t request = params->ecx;
    uint32_t arg = params->edx;
    fd_entry_t *entry;

    if (!current_proc) {
        return -ESRCH;
    }

    /* Validate the fd first, so a bad fd reports -EBADF rather than a request-
     * specific status (e.g. ioctl(-1, TCGETS) must be -EBADF, not -ENOTTY). */
    entry = fd_get(&current_proc->fds, fd_num);
    if (!entry) {
        return -EBADF;
    }

    /* Terminal attribute queries on a valid fd: Rach2 has no termios layer, so
     * report "not a terminal" exactly as Linux does for a non-tty fd. musl's
     * isatty() then returns 0 and stdio chooses block buffering. */
    if (request == TCGETS || request == TIOCGWINSZ) {
        return -ENOTTY;
    }

    /* Handle KIOCSOUND on stdout (console) - Linux convention */
    if (fd_num == FD_STDOUT || fd_num == FD_STDERR) {
        if (request == KIOCSOUND) {
            if (arg == 0) {
                speaker_off();
            } else {
                speaker_on(arg);
            }
            return 0;
        }
    }

    /* Handle framebuffer ioctls */
    if (entry->type == FD_TYPE_FRAMEBUFFER) {
        switch (request) {
            case FB_SETPALETTE:
                if (arg != 0) {
                    /* 256 RGB triples = 768 bytes read straight from user. */
                    if (!user_buffer_ok(arg, 768, 0)) {
                        return -EFAULT;
                    }
                    vga_mode13h_set_palette((const uint8_t *)arg);
                    return 0;
                }
                return -EFAULT;

            case FB_MODE_13H:
                /* Already in mode 13h if fb is open */
                return 0;

            case FB_MODE_TEXT:
                /* Switch back to text mode */
                vga_mode13h_exit();
                fb_owner_pid = 0;
                return 0;

            default:
                return -ENOTTY;
        }
    }

    /* Handle console ioctls */
    if (entry->type == FD_TYPE_CONSOLE) {
        if (request == KIOCSOUND) {
            if (arg == 0) {
                speaker_off();
            } else {
                speaker_on(arg);
            }
            return 0;
        }

        /* Audio ioctls on console fd */
        switch (request) {
            case AUDIO_GET_INFO: {
                struct audio_info kinfo;
                if (arg) {
                    k_memset(&kinfo, 0, sizeof(kinfo));
                    kinfo.driver_type = audio ? audio->type : AUDIO_DRIVER_NONE;
                    kinfo.has_real_audio = audio_has_real_output() ? 1 : 0;
                    if (copy_to_user((void __user *)arg, &kinfo,
                                     sizeof(kinfo)) != 0) {
                        return -EFAULT;
                    }
                }
                return 0;
            }

            case AUDIO_START: {
                struct audio_start_params kp;
                if (!audio || !arg) {
                    return -EFAULT;
                }
                if (copy_from_user(&kp, (const void __user *)arg,
                                   sizeof(kp)) != 0) {
                    return -EFAULT;
                }
                return audio->start(kp.sample_rate, (audio_format_t)kp.format,
                                    (int)kp.channels);
            }

            case AUDIO_STOP:
                if (audio) {
                    audio->stop();
                    return 0;
                }
                return -ENODEV;

            case AUDIO_WRITE: {
                struct audio_write_params kp;
                if (!audio || !arg) {
                    return -EFAULT;
                }
                /* Copy the param struct in through the helper, then range-check
                 * the PCM buffer it points at before the driver reads it (the
                 * bulk copy is the driver's, as with the framebuffer/socket
                 * data paths). */
                if (copy_from_user(&kp, (const void __user *)arg,
                                   sizeof(kp)) != 0) {
                    return -EFAULT;
                }
                if (!kp.buffer ||
                    !user_buffer_ok((uint32_t)(uintptr_t)kp.buffer, kp.size, 0)) {
                    return -EFAULT;
                }
                return audio->write(kp.buffer, kp.size);
            }

            case AUDIO_SET_VOLUME:
                if (audio) {
                    audio->set_volume((int)arg);
                    return 0;
                }
                return -ENODEV;
        }
    }

    return -ENOTTY;  /* Unknown request */
}

/* --- Network socketcall (SYS_SOCKETCALL = 102) ---------------------------
 *
 * Linux i386 convention: EBX is the sub-call number, ECX points to an array of
 * 32-bit arguments in user memory. Pointer arguments index into user memory,
 * so every user address is range-checked before it is dereferenced.
 */

/*
 * User address window. The kernel is identity-mapped in the low 32 MB and user
 * space lives above it, below 0xC0000000. A user pointer and the whole length
 * it spans must fall inside this window; anything else is rejected so a syscall
 * argument cannot read or write kernel memory. The window and the check now
 * live in mm/usercopy (USER_ADDR_MIN/MAX, user_range_ok); user_access_ok is a
 * thin alias kept for the existing call sites.
 */
static int user_access_ok(uint32_t addr, uint32_t len)
{
    return user_range_ok(addr, len);
}

/*
 * Range-check a user buffer AND back its pages before the kernel passes the raw
 * pointer to a lower layer (krffs, tcp, udp, the drivers) that dereferences it
 * directly. With demand paging a numerically valid pointer may still be unmapped
 * (a never-touched heap page, or a wild address in no VMA); vm_prefault_range
 * faults the valid pages in here and rejects the bad ones, so the lower layer's
 * raw access can never fault in kernel mode and panic. 'write' is 1 when that
 * layer writes into the buffer (so the pages need write permission).
 */
static int user_buffer_ok(uint32_t addr, uint32_t len, int write)
{
    return user_access_ok(addr, len) && vm_prefault_range(addr, len, write);
}

/* Read and write big-endian sockaddr fields (sin_port, sin_addr). */
#define SC_BE16(p) \
    ((uint16_t)(((uint8_t *)(p))[0] << 8 | ((uint8_t *)(p))[1]))
#define SC_BE32(p) \
    (((uint32_t)((uint8_t *)(p))[0] << 24) | \
     ((uint32_t)((uint8_t *)(p))[1] << 16) | \
     ((uint32_t)((uint8_t *)(p))[2] <<  8) | \
     ((uint32_t)((uint8_t *)(p))[3]))
#define SC_WBE16(p, v) do { \
    ((uint8_t *)(p))[0] = (uint8_t)((v) >> 8); \
    ((uint8_t *)(p))[1] = (uint8_t)(v); \
} while (0)
#define SC_WBE32(p, v) do { \
    ((uint8_t *)(p))[0] = (uint8_t)((v) >> 24); \
    ((uint8_t *)(p))[1] = (uint8_t)((v) >> 16); \
    ((uint8_t *)(p))[2] = (uint8_t)((v) >>  8); \
    ((uint8_t *)(p))[3] = (uint8_t)(v); \
} while (0)

/* socketcall sub-function numbers (Linux standard). */
#define SC_SOCKET    1
#define SC_BIND      2
#define SC_CONNECT   3
#define SC_LISTEN    4
#define SC_ACCEPT    5
#define SC_SEND      9
#define SC_RECV     10
#define SC_SENDTO   11
#define SC_RECVFROM 12
#define SC_PING     15

/* recv flags: only MSG_DONTWAIT is honored. */
#define MSG_DONTWAIT 0x40

/* We read sin_family/sin_port/sin_addr from the first 8 sockaddr bytes. */
#define SOCKADDR_MIN 8

/*
 * Write an AF_INET source/peer sockaddr back to a caller's (addr, addrlen) pair,
 * respecting the caller's declared buffer capacity: read the in/out socklen,
 * copy at most that many of the 16 sockaddr_in bytes, and report the true length
 * (16) -- the recvfrom(2)/accept(2) truncation semantics. Returns 0 or -EFAULT.
 */
static int copy_sockaddr_to_user(uint32_t sa_ptr, uint32_t salen_ptr,
                                 uint16_t port, uint32_t ip)
{
    uint8_t  sa[16];
    uint32_t caller_len;
    uint32_t copy_len;

    if (!sa_ptr || !salen_ptr) {
        return 0;  /* caller does not want the address back */
    }

    if (copy_from_user(&caller_len, (const void __user *)salen_ptr,
                       sizeof(caller_len)) != 0) {
        return -EFAULT;
    }

    k_memset(sa, 0, sizeof(sa));
    SC_WBE16(sa + 0, 0x0002);   /* AF_INET */
    SC_WBE16(sa + 2, port);
    SC_WBE32(sa + 4, ip);

    copy_len = (caller_len < sizeof(sa)) ? caller_len : (uint32_t)sizeof(sa);
    if (copy_len > 0 &&
        copy_to_user((void __user *)sa_ptr, sa, copy_len) != 0) {
        return -EFAULT;
    }

    /* Report the real sockaddr_in length even when the caller's buffer was
     * shorter (and thus the address was truncated). */
    caller_len = (uint32_t)sizeof(sa);
    if (copy_to_user((void __user *)salen_ptr, &caller_len,
                     sizeof(caller_len)) != 0) {
        return -EFAULT;
    }
    return 0;
}

static int32_t do_socket(int domain, int type, int protocol)
{
    struct socket *sock;
    fd_entry_t    *entry;
    int            fd;

    (void)protocol;

    /* Only IPv4 (AF_INET == 2) is implemented; reject other families instead of
     * silently creating an unusable socket. */
    if (domain != 2) {
        return -EAFNOSUPPORT;
    }
    if (type != SOCK_DGRAM && type != SOCK_STREAM) {
        return -EINVAL;
    }

    sock = socket_alloc(type);
    if (!sock) {
        return -ENOMEM;
    }

    fd = fd_alloc(&current_proc->fds);
    if (fd < 0) {
        socket_unref(sock);
        return -EMFILE;
    }

    entry         = &current_proc->fds.entries[fd];
    entry->type   = FD_TYPE_SOCKET;
    entry->sock   = sock;
    entry->node   = NULL;
    entry->offset = 0;
    entry->flags  = FD_FLAG_READ | FD_FLAG_WRITE;
    return fd;
}

static int32_t do_bind(int fd, uint32_t sa_ptr, int addrlen)
{
    fd_entry_t *entry;
    uint16_t    port;
    uint8_t     sa[SOCKADDR_MIN];

    /* The caller's declared sockaddr must be at least the header we read; a
     * short or negative addrlen must not make us read past it. */
    if (addrlen < SOCKADDR_MIN) {
        return -EINVAL;
    }

    /* Copy the sockaddr header into a kernel buffer; the BE helpers below then
     * operate on the kernel copy. */
    if (copy_from_user(sa, (const void __user *)sa_ptr, SOCKADDR_MIN) != 0) {
        return -EFAULT;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }

    port = SC_BE16(sa + 2);
    entry->sock->local_port = port;

    if (entry->sock->type == SOCK_DGRAM) {
        if (udp_bind(port, entry->sock) < 0) {
            return -EADDRINUSE;
        }
    }
    return 0;
}

static int32_t do_sendto(int fd, uint32_t buf_ptr, int len,
                         int flags, uint32_t sa_ptr, int addrlen)
{
    fd_entry_t *entry;
    uint16_t    dst_port;
    uint32_t    dst_ip;
    uint8_t     sa[SOCKADDR_MIN];

    (void)flags;

    if (addrlen < SOCKADDR_MIN) {
        return -EINVAL;  /* declared sockaddr shorter than the header we read */
    }
    if (len < 0 || !user_buffer_ok(buf_ptr, (uint32_t)len, 0)) {
        return -EFAULT;
    }
    /* Copy the destination sockaddr into a kernel buffer for the BE helpers. */
    if (copy_from_user(sa, (const void __user *)sa_ptr, SOCKADDR_MIN) != 0) {
        return -EFAULT;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }

    /* A datagram socket needs a source port so replies can come back. */
    if (entry->sock->local_port == 0) {
        udp_bind_ephemeral(entry->sock);
    }

    dst_port = SC_BE16(sa + 2);
    dst_ip   = SC_BE32(sa + 4);

    /* udp_tx reads the payload itself; __force strips __user at the boundary. */
    udp_tx(entry->sock->local_port, dst_ip, dst_port,
           (const uint8_t __force *)(const void __user *)buf_ptr, (uint16_t)len);
    return len;
}

static int32_t do_recvfrom(int fd, uint32_t buf_ptr, int len,
                           int flags, uint32_t sa_ptr, uint32_t salen_ptr)
{
    fd_entry_t *entry;
    uint32_t    src_ip;
    uint16_t    src_port;
    int         bytes;

    if (len < 0 || !user_buffer_ok(buf_ptr, (uint32_t)len, 1)) {
        return -EFAULT;
    }
    /* The source-address writeback is validated and truncated to the caller's
     * declared length by copy_sockaddr_to_user below. */

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }

    /* socket_recv/socket_try_recv write straight into the validated user buffer;
     * __force strips __user at the boundary. */
    if (flags & MSG_DONTWAIT) {
        bytes = socket_try_recv(entry->sock,
                                (uint8_t __force *)(void __user *)buf_ptr, len,
                                &src_ip, &src_port);
        if (bytes < 0) {
            return bytes;   /* SOCK_EAGAIN: leave the caller's sockaddr alone */
        }
    } else {
        bytes = socket_recv(entry->sock,
                            (uint8_t __force *)(void __user *)buf_ptr, len,
                            &src_ip, &src_port);
    }

    /* Report the source address, truncated to the caller's declared capacity. */
    if (copy_sockaddr_to_user(sa_ptr, salen_ptr, src_port, src_ip) != 0) {
        return -EFAULT;
    }
    return bytes;
}

static int32_t do_ping(uint32_t dst_ip)
{
    return (int32_t)icmp_ping(dst_ip);
}

static int32_t do_connect(int fd, uint32_t sa_ptr, int addrlen)
{
    fd_entry_t *entry;
    uint32_t    dst_ip;
    uint16_t    dst_port;
    uint8_t     sa[SOCKADDR_MIN];

    if (addrlen < SOCKADDR_MIN) {
        return -EINVAL;  /* declared sockaddr shorter than the header we read */
    }

    /* Copy the sockaddr header into a kernel buffer for the BE helpers. */
    if (copy_from_user(sa, (const void __user *)sa_ptr, SOCKADDR_MIN) != 0) {
        return -EFAULT;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    if (entry->sock->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }

    dst_port = SC_BE16(sa + 2);
    dst_ip   = SC_BE32(sa + 4);

    if (tcp_connect(entry->sock, dst_ip, dst_port) < 0) {
        return -ECONNREFUSED;
    }

    /* Block until the handshake completes, fails, or times out. */
    entry->sock->waiting_proc = current_proc;
    current_proc->state       = PROC_BLOCKED;
    while (entry->sock->tcb != NULL &&
           entry->sock->tcb->state != TCP_ESTABLISHED &&
           entry->sock->tcb->state != TCP_CLOSED) {
        __asm__ __volatile__("sti; hlt");
    }
    entry->sock->waiting_proc = NULL;
    current_proc->state       = PROC_RUNNING;

    if (!entry->sock->tcb || entry->sock->tcb->state != TCP_ESTABLISHED) {
        return -ECONNREFUSED;
    }
    return 0;
}

static int32_t do_listen(int fd, int backlog)
{
    fd_entry_t *entry;

    (void)backlog;   /* a fixed-size accept queue is used */

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    if (entry->sock->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }

    entry->sock->listening = 1;
    return tcp_listen(entry->sock, entry->sock->local_port);
}

static int32_t do_accept(int fd, uint32_t sa_ptr, uint32_t salen_ptr)
{
    fd_entry_t    *entry;
    fd_entry_t    *new_entry;
    struct socket *listen_sock;
    struct socket *conn_sock;
    int            new_fd;

    /* The peer-address writeback is validated and truncated to the caller's
     * declared length by copy_sockaddr_to_user below. */

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    listen_sock = entry->sock;

    /* Block until a connection is queued. */
    listen_sock->waiting_proc = current_proc;
    current_proc->state       = PROC_BLOCKED;
    while (listen_sock->accept_head == listen_sock->accept_tail) {
        __asm__ __volatile__("sti; hlt");
    }
    listen_sock->waiting_proc = NULL;
    current_proc->state       = PROC_RUNNING;

    conn_sock = listen_sock->accept_queue[listen_sock->accept_head];
    listen_sock->accept_head =
        (listen_sock->accept_head + 1) & (SOCK_ACCEPT_BACKLOG - 1);

    /* The queued socket already holds the reference created when tcp_rx
     * accepted it; the new descriptor takes ownership of that reference. */
    new_fd = fd_alloc(&current_proc->fds);
    if (new_fd < 0) {
        socket_unref(conn_sock);
        return -EMFILE;
    }
    new_entry         = &current_proc->fds.entries[new_fd];
    new_entry->type   = FD_TYPE_SOCKET;
    new_entry->sock   = conn_sock;
    new_entry->node   = NULL;
    new_entry->offset = 0;
    new_entry->flags  = FD_FLAG_READ | FD_FLAG_WRITE;

    /* Report the peer address, truncated to the caller's declared capacity. On a
     * bad user pointer, tear down the descriptor we just installed (fd_free only
     * clears the slot, so the socket reference must be dropped explicitly) rather
     * than leaking the accepted fd and its socket. */
    if (copy_sockaddr_to_user(sa_ptr, salen_ptr, conn_sock->remote_port,
                              conn_sock->remote_ip) != 0) {
        fd_free(&current_proc->fds, new_fd);
        socket_unref(conn_sock);
        return -EFAULT;
    }
    return new_fd;
}

static int32_t do_send(int fd, uint32_t buf_ptr, int len, int flags)
{
    fd_entry_t *entry;

    (void)flags;

    if (len < 0 || !user_buffer_ok(buf_ptr, (uint32_t)len, 0)) {
        return -EFAULT;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    if (entry->sock->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }

    /* tcp_send copies the payload itself; __force strips __user at the boundary. */
    return (int32_t)tcp_send(entry->sock,
                             (const uint8_t __force *)(const void __user *)buf_ptr,
                             len);
}

static int32_t do_recv(int fd, uint32_t buf_ptr, int len, int flags)
{
    fd_entry_t *entry;

    (void)flags;

    if (len < 0 || !user_buffer_ok(buf_ptr, (uint32_t)len, 1)) {
        return -EFAULT;
    }

    entry = fd_get(&current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return -ENOTSOCK;
    }
    if (entry->sock->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }

    /* socket_tcp_recv writes into the validated user buffer; __force strips
     * __user at the boundary. */
    return (int32_t)socket_tcp_recv(entry->sock,
                                    (uint8_t __force *)(void __user *)buf_ptr,
                                    len);
}

static int32_t sys_socketcall(isr_params_t *params)
{
    int      call = (int)params->ebx;
    uint32_t argp = params->ecx;
    uint32_t args[6];
    int      nargs;

    switch (call) {
    case SC_PING:                       nargs = 1; break;
    case SC_LISTEN:                     nargs = 2; break;
    case SC_SOCKET: case SC_BIND:
    case SC_CONNECT: case SC_ACCEPT:    nargs = 3; break;
    case SC_SEND: case SC_RECV:         nargs = 4; break;
    case SC_SENDTO: case SC_RECVFROM:   nargs = 6; break;
    default:
        return -EINVAL;
    }

    /* Pull the argument array out of user memory in one fault-checked copy. */
    if (copy_from_user(args, (const void __user *)argp,
                       (uint32_t)nargs * 4) != 0) {
        return -EFAULT;
    }

    switch (call) {
    case SC_SOCKET:
        return do_socket((int)args[0], (int)args[1], (int)args[2]);
    case SC_BIND:
        return do_bind((int)args[0], args[1], (int)args[2]);
    case SC_SENDTO:
        return do_sendto((int)args[0], args[1], (int)args[2],
                         (int)args[3], args[4], (int)args[5]);
    case SC_RECVFROM:
        return do_recvfrom((int)args[0], args[1], (int)args[2],
                           (int)args[3], args[4], args[5]);
    case SC_PING:
        return do_ping(args[0]);
    case SC_CONNECT:
        return do_connect((int)args[0], args[1], (int)args[2]);
    case SC_LISTEN:
        return do_listen((int)args[0], (int)args[1]);
    case SC_ACCEPT:
        return do_accept((int)args[0], args[1], args[2]);
    case SC_SEND:
        return do_send((int)args[0], args[1], (int)args[2], (int)args[3]);
    case SC_RECV:
        return do_recv((int)args[0], args[1], (int)args[2], (int)args[3]);
    default:
        return -EINVAL;
    }
}

/*
 * set_thread_area(2) -- Linux i386 thread-local storage. EBX points at a
 * struct user_desc. We validate it, program the shared TLS GDT descriptor
 * (entry GDT_TLS_ENTRY) and this process's PCB base, then write the chosen
 * entry number back so the C library can load (entry_number << 3) | 3 into
 * %gs. The base is reapplied to the GDT on every context switch (switch.s).
 * Returns 0 on success or a negative errno on a bad pointer/request.
 */
static int32_t sys_set_thread_area(isr_params_t *params)
{
    uint32_t uptr = params->ebx;
    user_desc_t desc;
    uint32_t base = 0;

    if (!current_proc) {
        return -ESRCH;  /* TLS is only meaningful for a user process */
    }

    /* Read the descriptor through the fault-checked helper (it range-checks the
     * full struct internally, so the prior explicit user_access_ok is redundant). */
    if (copy_from_user(&desc, (const void __user *)uptr, sizeof(user_desc_t)) != 0) {
        return -EFAULT;
    }

    if (tls_resolve_base(&desc, &base) != 0) {
        return -EINVAL;
    }

    /* Program the descriptor now and remember the base so the context switch
     * can reprogram entry GDT_TLS_ENTRY for this process when it is resumed. */
    gdt_set_tls(base);
    current_proc->tls_base = base;

    /* Report the allocated slot back (musl reads user_desc.entry_number). */
    desc.entry_number = GDT_TLS_ENTRY;
    if (copy_to_user((void __user *)uptr, &desc.entry_number,
                     sizeof(desc.entry_number)) != 0) {
        return -EFAULT;
    }

    return 0;
}

/*
 * sys_getpid - return the calling process's PID (getpid(2)).
 */
static int32_t sys_getpid(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
    return (int32_t)current_proc->pid;
}

/*
 * sys_set_tid_address - record the clear-child-tid pointer and return the tid
 * (set_tid_address(2)). Rach2 has no threads, so we only need to return the
 * caller's PID as its thread id; the pointer (EBX) is accepted and ignored.
 */
static int32_t sys_set_tid_address(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
    return (int32_t)current_proc->pid;
}

/*
 * sys_uname - report system identification (uname(2)). Fills the six 65-byte
 * fields of struct utsname (new-style, as musl expects) in user memory.
 * Args: EBX = struct utsname *
 */
struct kernel_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

/* Copy a C string into a fixed 65-byte utsname field (always NUL-terminated). */
static void uname_field(char *dst, const char *src)
{
    uint32_t n = k_strlen(src);

    if (n > 64) {
        n = 64;
    }
    k_memcpy(dst, src, n);
    dst[n] = '\0';
}

static int32_t sys_uname(isr_params_t *params)
{
    struct kernel_utsname __user *user_uts =
        (struct kernel_utsname __user *)params->ebx;
    struct kernel_utsname uts;

    k_memset(&uts, 0, sizeof(uts));
    uname_field(uts.sysname, "Rach2");
    uname_field(uts.nodename, "rach2");
    uname_field(uts.release, "1.0.0");
    uname_field(uts.version, "Rach2 musl port");
    uname_field(uts.machine, "i686");
    uname_field(uts.domainname, "(none)");

    if (copy_to_user(user_uts, &uts, sizeof(uts)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/*
 * mmap2 protection bits and flags (Linux i386 ABI). We support PROT_* as VMA
 * permissions and only anonymous private mappings -- file-backed mmap would need
 * a page cache.
 */
#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define PROT_EXEC     0x4
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* Round 'v' up to the next page boundary. */
#define PAGE_UP(v) (((v) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

/*
 * sys_brk - set the program break (heap top).
 * Arg: EBX = requested break. Returns the resulting break: the new value on
 * success, or the unchanged current break if the request is out of range. This
 * is the contract musl's __brk relies on (it compares the return to its request).
 * The heap is a single demand-paged VMA [heap_start, page_up(brk)); growing only
 * extends the region (pages fault in lazily), shrinking frees the pages above.
 */
static int32_t sys_brk(isr_params_t *params)
{
    uint32_t req = params->ebx;
    vm_space_t *vm;
    vma_t *heap;
    uint32_t i, old_top, new_top;

    if (!current_proc) {
        return -ESRCH;
    }
    vm = &current_proc->vm;

    /* brk(0) queries the break; below the heap base or into the stack window is
     * refused with the break left unchanged. */
    if (req < vm->heap_start || req > vm->stack_limit) {
        return (int32_t)vm->brk;
    }

    old_top = PAGE_UP(vm->brk);
    new_top = PAGE_UP(req);

    /* Find the heap region, if the heap has ever grown above its base. */
    heap = NULL;
    for (i = 0; i < vm->count; i++) {
        if (vm->vmas[i].type == VM_HEAP) {
            heap = &vm->vmas[i];
            break;
        }
    }

    if (new_top > old_top) {
        /* Grow: the new pages [old_top, new_top) must not collide with mmap or
         * any other region. The heap's own [heap_start, old_top) abuts that span
         * without overlapping, so vm_overlaps tests only the *other* regions. */
        if (vm_overlaps(vm, old_top, new_top)) {
            return (int32_t)vm->brk;
        }
        /* Extend the region, or create it on the first non-empty break. */
        if (heap) {
            heap->end = new_top;
        } else if (vm_insert(vm, vm->heap_start, new_top,
                             VM_PROT_READ | VM_PROT_WRITE, VM_HEAP) != 0) {
            return (int32_t)vm->brk;
        }
    } else if (new_top < old_top) {
        /* Shrink: free the pages above the new top, then trim or drop the VMA. */
        unmap_user_range(_curr_dir, new_top, old_top);
        if (heap) {
            if (new_top <= vm->heap_start) {
                vm_remove(vm, heap->start, heap->end);
            } else {
                heap->end = new_top;
            }
        }
    }

    vm->brk = req;
    return (int32_t)req;
}

/*
 * sys_mmap2 - map anonymous memory.
 * Args: EBX=addr (hint, ignored), ECX=length, EDX=prot, ESI=flags, EDI=fd,
 * EBP=pgoffset. Returns the mapped base address, or -errno. Only
 * MAP_ANONYMOUS|MAP_PRIVATE is supported (what musl's allocator uses); the region
 * is placed by bumping mmap_top down and is demand-paged + zero-filled on touch.
 */
static int32_t sys_mmap2(isr_params_t *params)
{
    uint32_t length = params->ecx;
    uint32_t prot = params->edx;
    uint32_t flags = params->esi;
    vm_space_t *vm;
    uint32_t len, base, vmprot;

    if (!current_proc) {
        return -ESRCH;
    }
    vm = &current_proc->vm;

    if (length == 0) {
        return -EINVAL;
    }
    if (!(flags & MAP_ANONYMOUS)) {
        return -ENOSYS; /* file-backed mmap is not implemented */
    }
    if (flags & MAP_FIXED) {
        /* We choose the address ourselves; honoring a fixed hint would need
         * overlap/replace handling we do not have. Reject rather than silently
         * return a different address the caller would then fault on. */
        return -ENOSYS;
    }

    len = PAGE_UP(length);

    vmprot = 0;
    if (prot & PROT_READ) {
        vmprot |= VM_PROT_READ;
    }
    if (prot & PROT_WRITE) {
        /* On i386 a present page is always readable -- there is no write-only
         * PTE -- so writable implies readable. Record that in the VMA prot so the
         * fault check agrees with the hardware (a read of a PROT_WRITE page does
         * not wrongly segfault). */
        vmprot |= VM_PROT_WRITE | VM_PROT_READ;
    }
    if (prot & PROT_EXEC) {
        vmprot |= VM_PROT_EXEC;
    }

    /* Bump down from mmap_top; refuse if it would underflow or hit the heap. The
     * addr hint is ignored (no MAP_FIXED support), which suits musl's malloc. */
    if (len > vm->mmap_top || vm->mmap_top - len < PAGE_UP(vm->brk)) {
        return -ENOMEM;
    }
    base = vm->mmap_top - len;

    if (vm_insert(vm, base, base + len, vmprot, VM_ANON) != 0) {
        return -ENOMEM;
    }
    vm->mmap_top = base;
    return (int32_t)base;
}

/*
 * sys_munmap - unmap a range.
 * Args: EBX=addr (page-aligned), ECX=length. Returns 0, or -errno. Drops the VMA
 * bookkeeping (trimming or splitting partial overlaps) and frees the backing
 * frames; unmapping never-faulted or never-mapped pages is a no-op, as on Linux.
 */
static int32_t sys_munmap(isr_params_t *params)
{
    uint32_t addr = params->ebx;
    uint32_t length = params->ecx;
    vm_space_t *vm;
    uint32_t start, end;

    if (!current_proc) {
        return -ESRCH;
    }
    vm = &current_proc->vm;

    if (length == 0 || (addr & (PAGE_SIZE - 1)) != 0) {
        return -EINVAL;
    }
    /* Reject a range that leaves user space or wraps, so PAGE_UP(addr + length)
     * below cannot overflow to a smaller end and silently no-op. */
    if (addr >= USER_ADDR_MAX || length > USER_ADDR_MAX - addr) {
        return -EINVAL;
    }

    start = addr;
    end = PAGE_UP(addr + length);

    if (vm_remove(vm, start, end) != 0) {
        return -ENOMEM; /* a hole-punch split would exceed VMA_MAX */
    }
    unmap_user_range(_curr_dir, start, end);
    return 0;
}

/*
 * Syscall handler - dispatches to appropriate syscall function
 * Stores return value in params->eax so it's returned to userspace
 */
static void _syscall_handler(isr_params_t *params)
{
    uint32_t syscall_num = params->eax;
    /* Default to -ENOSYS so an unimplemented or out-of-range number reports the
     * real "no such syscall" errno; musl relies on -ENOSYS to fall back to an
     * alternate path rather than treating it as a generic -EPERM failure. */
    int32_t ret = -ENOSYS;

    /* Mark this process non-preemptible BEFORE enabling interrupts. The ISR
     * stub keeps IF=0 through entry (segments/%gs are still being saved), so
     * setting in_syscall here and only then doing sti closes the window in
     * which a timer IRQ could reschedule a process whose trapframe is still
     * half-built. The timer handler skips scheduling while in_syscall is set. */
    if (current_proc) {
        current_proc->in_syscall = 1;
    }

    /* Enable interrupts for the syscall body so the PIT/HDA keep ticking during
     * long syscalls (e.g. Doom's framebuffer writes); preemption stays disabled
     * via in_syscall above. The stub no longer does sti -- iret restores IF. */
    __asm__ __volatile__("sti");

    if (syscall_num < SYSCALL_TABLE_SIZE && _syscall_table[syscall_num]) {
        ret = _syscall_table[syscall_num](params);
    }

    /* Disable interrupts again so the epilogue restores the trapframe (segments
     * and %gs) atomically; iret then restores the caller's IF. */
    __asm__ __volatile__("cli");

    if (current_proc) {
        current_proc->in_syscall = 0;
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
    _syscall_table[SYS_WAIT4] = sys_wait4;
    _syscall_table[SYS_CREAT] = sys_creat;
    _syscall_table[SYS_UNLINK] = sys_unlink;
    _syscall_table[SYS_MKDIR] = sys_mkdir;
    _syscall_table[SYS_RMDIR] = sys_rmdir;
    _syscall_table[SYS_EXECVE] = sys_execve;
    _syscall_table[SYS_CHDIR] = sys_chdir;
    _syscall_table[SYS_BRK] = sys_brk;
    _syscall_table[SYS_MMAP2] = sys_mmap2;
    _syscall_table[SYS_MUNMAP] = sys_munmap;
    _syscall_table[SYS_LSEEK] = sys_lseek;
    _syscall_table[SYS_RENAME] = sys_rename;
    _syscall_table[SYS_IOCTL] = sys_ioctl;
    _syscall_table[SYS_DUP2] = sys_dup2;
    _syscall_table[SYS_GETTIMEOFDAY] = sys_gettimeofday;
    _syscall_table[SYS_STAT] = sys_stat;
    _syscall_table[SYS_GETCWD] = sys_getcwd;
    _syscall_table[SYS_STAT64] = sys_stat64;
    _syscall_table[SYS_GETDENTS64] = sys_getdents64;
    _syscall_table[SYS_UTIMENSAT] = sys_utimensat;
    _syscall_table[SYS_SOCKETCALL] = sys_socketcall;
    _syscall_table[SYS_SET_THREAD_AREA] = sys_set_thread_area;
    _syscall_table[SYS_GETPID] = sys_getpid;
    _syscall_table[SYS_WRITEV] = sys_writev;
    _syscall_table[SYS_UNAME] = sys_uname;
    _syscall_table[SYS_SET_TID_ADDRESS] = sys_set_tid_address;
    /* musl's exit() issues exit_group(); for a single-threaded process it is
     * exactly _exit(). */
    _syscall_table[SYS_EXIT_GROUP] = sys_exit;

    /* Register the interrupt handler */
    register_isr_handler(SYSCALL_VECTOR, _syscall_handler);
}
