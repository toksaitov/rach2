/**
 * Run the AF_UNIX and pipe calls with another thread's call inside their
 * sleeps.
 *
 * host-deps: tests/host/compat/park_host.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * `ipc/unix.c`, `ipc/pipe.c` and the checked user copy are the kernel's,
 * included whole to reach their static state. The descriptor table the
 * threads share, the heap, the wait queues and the file system's socket
 * files are models; user memory is eight model pages. A copy that reaches a
 * page not yet present sleeps while the page comes in, and there the model
 * runs the call a row gives the other thread, the sibling; so do the file
 * system's making of a socket file and the release of a file's last
 * description, which can sleep too. A sibling that must wait for a lock the
 * sleeping call holds parks (`compat/park_host.c`): its turn ends there, the
 * sleeping call goes on, and the row runs the sibling again once the lock
 * is free, as the woken thread would.
 *
 * The descriptors accept, socketpair, a receive of SCM_RIGHTS and pipe2
 * make are reserved while their numbers, and for accept the peer's name,
 * are copied out: a sibling's close of a reserved number is EBADF, a
 * sibling's open takes another number, and a copy that fails frees only
 * what the call reserved. A connect whose copy of the address, or lookup of
 * a path, sleeps while a sibling connects the socket fails with EISCONN,
 * and the listener has the sibling's connection alone. A second bind of a
 * socket waits while the first sleeps in the file system and then fails
 * with EINVAL; one socket file is made, and the close lets it go. A receive
 * while another's copy of a message sleeps waits for the receive lock and
 * then takes the next message, on a datagram pair and on a stream pair. A
 * send whose copy sleeps while a sibling shuts the socket down for writing
 * queues nothing and fails with EPIPE, on both. A KRFFS file with no name
 * whose other holder is a descriptor a receive has taken off its queue is
 * not freed by a sibling's close of its last descriptor, while the receive
 * installs that descriptor or closes it with the others it has no room for;
 * the release of a descriptor in flight that is its file's last holder
 * frees the file; and a descriptor the receive has installed is no longer
 * counted in flight. Each row ends with nothing allocated.
 */
#include "ipc/unix.c"
#include "ipc/pipe.c"
#include "ipc/pipe_ring.c"
#include "mm/usercopy.c"

extern void *malloc(unsigned long n);
extern void free(void *p);
extern void exit(int status);
extern int dprintf(int fd, const char *format, ...);

int park_run(void (*call)(void));
int park_active(void);
void park(void);

#define USER_BASE 0x04000000u
#define PAGES     8
#define SLOTS     16
#define FILES     4
#define SOCKFILES 4

/* The user address `off` bytes into model page `page`. */
#define AT(page, off) (USER_BASE + (uint32_t)(page) * PAGE_SIZE + (uint32_t)(off))

/* A model page is present; missing, which the first copy to reach it brings
 * in after the sibling runs; lost, which that copy then finds unmapped; or
 * absent, which every copy finds unmapped. */
enum { PAGE_PRESENT, PAGE_MISSING, PAGE_LOST, PAGE_ABSENT };

/* A socket file bind has made, and whether a socket holds it. */
struct sockfile {
    char path[16];
    int made;
    int held;
};

static process_t self;
process_t *current_proc = &self;
volatile uint32_t kernel_ticks;
static struct fs_struct dirs;
static fd_table_t shared;
static fd_entry_t slots[SLOTS];
static uint8_t memory[PAGES * PAGE_SIZE];
static int pages[PAGES];

/* KRFFS files that lost their names while open: the release of the last
 * holder of one frees it, which `freed` counts. */
static krffs_node_t nodes[FILES];
static int freed[FILES];

static struct sockfile sockfiles[SOCKFILES];
static struct vfs_mount sockmount;
static int files_made;

/* The sibling's call, run once at the sleep it is armed for: a copy's page
 * fault, the file system's making of a socket file, or the release of a
 * file's last description. */
static void (*page_sibling)(void);
static void (*fs_sibling)(void);
static void (*release_sibling)(void);
static int ran, parked;
static wait_queue_t *parked_on;
static uint32_t wakes_at_park;
/* Set by a row whose sleeping copy reads what a sibling that got through
 * would have freed: that copy fails, rather than read freed memory. */
static int copy_needs_park;

/* What a row's sibling acts on and what it found. */
static int reserved[2];
static int32_t sibling_rc[2];
static int seen[FILES];
static uint32_t name_len;

static int failures, live, sigpipes;
static uint32_t inodes;

/** Report `detail` when `condition` is false, and return the condition, so
 * that a row can skip the checks a failure leaves without meaning. */
static int check(int condition, const char *detail)
{
    if (!condition) {
        dprintf(2, "unix-hosttest: fail: %s\n", detail);
        failures++;
    }
    return condition;
}

/** The host address of a model user address, or `p` for kernel memory. */
static void *host(const void *p)
{
    unsigned long a = (unsigned long)p;

    if (a >= USER_BASE && a < USER_BASE + sizeof(memory)) {
        return memory + (a - USER_BASE);
    }
    return (void *)p;
}

void *k_memcpy(void *dest, const void *src, size_t n)
{
    uint8_t *d = host(dest);
    const uint8_t *s = host(src);
    size_t i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

void *k_memset(void *ptr, int c, size_t n)
{
    uint8_t *d = host(ptr);
    size_t i;

    for (i = 0; i < n; i++) {
        d[i] = (uint8_t)c;
    }
    return ptr;
}

/** The kernel heap over the host's; `live` counts the blocks it has out. */
void *kmalloc(size_t size)
{
    void *p = malloc(size ? size : 1u);

    if (p) {
        live++;
    }
    return p;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);

    if (p) {
        k_memset(p, 0, size);
    }
    return p;
}

void kfree(void *p)
{
    if (p) {
        live--;
        free(p);
    }
}

/**
 * Run the sibling call armed in `*slot`, once, as the scheduler runs another
 * thread while this one sleeps. A sibling that parks leaves nothing behind
 * but the entry a receive puts on the transit list, which was on its stack
 * and comes off again.
 */
static void run_sibling(void (**slot)(void))
{
    void (*call)(void) = *slot;
    struct unix_in *was = transits;

    *slot = NULL;
    if (!call) {
        return;
    }
    ran++;
    parked_on = NULL;
    parked = !park_run(call);
    if (parked) {
        transits = was;
    }
}

/**
 * Bring in the pages of [addr, addr + len) for a copy: 1, or 0 when one is
 * unmapped. A missing or lost page is a sleep, in which the sibling runs; a
 * lost page is unmapped after it, and so is a missing one when the row needs
 * the sibling parked and it got through.
 */
int vm_prefault_range(uint32_t addr, uint32_t len, int write)
{
    uint32_t page, last;

    (void)write;
    if (len == 0) {
        return 1;
    }
    if (addr < USER_BASE || addr - USER_BASE >= sizeof(memory) ||
        len > sizeof(memory) - (addr - USER_BASE)) {
        return 0;
    }
    last = (addr - USER_BASE + len - 1u) / PAGE_SIZE;
    for (page = (addr - USER_BASE) / PAGE_SIZE; page <= last; page++) {
        if (pages[page] == PAGE_MISSING || pages[page] == PAGE_LOST) {
            int lost = pages[page] == PAGE_LOST;

            pages[page] = PAGE_PRESENT;
            run_sibling(&page_sibling);
            if (lost || (copy_needs_park && !parked)) {
                pages[page] = PAGE_ABSENT;
            }
        }
        if (pages[page] == PAGE_ABSENT) {
            return 0;
        }
    }
    return 1;
}

/** Whether a buffer lies in user memory, as `user_buffer_ok` asks, without
 * bringing it in. */
int user_buffer_ok(uint32_t addr, uint32_t len, int write)
{
    (void)write;
    return addr >= USER_BASE && addr - USER_BASE <= sizeof(memory) &&
           len <= sizeof(memory) - (addr - USER_BASE);
}

/**
 * A wait: it ends at once when `cond` holds. A sibling whose condition does
 * not hold parks; the row's own thread is ended by a signal, since nothing
 * else runs.
 */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    if (cond(arg)) {
        return 0;
    }
    if (park_active()) {
        parked_on = wq;
        wakes_at_park = wq->wakes;
        park();
    }
    return -EINTR;
}

/** An uninterruptible wait, which only a sibling can end by parking. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    if (cond(arg)) {
        return 0;
    }
    if (park_active()) {
        parked_on = wq;
        wakes_at_park = wq->wakes;
        park();
    }
    dprintf(2, "unix-hosttest: fail: an uninterruptible wait that no call can end\n");
    exit(1);
    return 0;
}

void wq_wake_all(wait_queue_t *wq)
{
    wq->wakes++;
}

void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
    wq->wakes = 0;
}

void fd_ofd_ref(struct open_file *ofd)
{
    if (ofd) {
        ofd->refcount++;
    }
}

/** Drop a reference to a description. The last of a file's closes the file
 * system's description, which can sleep, and a row's sibling can run there. */
void fd_ofd_unref(struct open_file *ofd)
{
    if (!ofd || ofd->refcount == 0 || --ofd->refcount != 0) {
        return;
    }
    if (ofd->vnode) {
        run_sibling(&release_sibling);
    }
    kfree(ofd);
}

/**
 * Reserve the lowest free slot, as `fd_alloc_from` does: it gets a new
 * description and keeps FD_TYPE_NONE until its caller gives it a type, so
 * that no lookup finds it.
 */
int fd_alloc(fd_table_t *table)
{
    int i;

    for (i = 0; i < table->cap; i++) {
        fd_entry_t *e = &table->entries[i];

        if (e->type == FD_TYPE_NONE && !e->ofd) {
            e->ofd = kzalloc(sizeof(*e->ofd));
            if (!e->ofd) {
                return -ENOMEM;
            }
            e->ofd->refcount = 1;
            e->ops = NULL;
            e->priv = NULL;
            e->dev = NULL;
            return i;
        }
    }
    return -EMFILE;
}

/** Free a slot, reserved or not, and drop its description, as `fd_free`
 * does: 0, or -1 for a slot already free. */
int fd_free(fd_table_t *table, int fd)
{
    fd_entry_t *e;

    if (fd < 0 || fd >= table->cap) {
        return -1;
    }
    e = &table->entries[fd];
    if (e->type == FD_TYPE_NONE && !e->ofd) {
        return -1;
    }
    fd_ofd_unref(e->ofd);
    k_memset(e, 0, sizeof(*e));
    return 0;
}

/** The descriptor `fd`, or NULL for a slot that is free, reserved or
 * closing. */
fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    fd_entry_t *e;

    if (fd < 0 || fd >= table->cap) {
        return NULL;
    }
    e = &table->entries[fd];
    return e->type == FD_TYPE_NONE || (e->flags & FD_FLAG_CLOSING) ? NULL : e;
}

/** The holders of the file `node` other than `except`, as `fd_node_holders`
 * counts them: the table's descriptors of it, and one for any in flight. */
static int holders(const krffs_node_t *node, const fd_entry_t *except)
{
    int n = unix_inflight_holds(node) ? 1 : 0;
    int i;

    for (i = 0; i < shared.cap; i++) {
        const fd_entry_t *e = &shared.entries[i];

        if (e != except && e->type == FD_TYPE_FILE && e->node == node) {
            n++;
        }
    }
    return n;
}

/**
 * Release what a descriptor holds, as `fd_release_objects` does for the
 * kinds the rows make: a file is freed by the release of its last holder,
 * and a socket or a pipe end is closed with its description's last
 * descriptor.
 */
void fd_release_backing(fd_entry_t *e)
{
    if (!e) {
        return;
    }
    if (e->type == FD_TYPE_FILE && e->node && holders(e->node, e) == 0) {
        freed[e->node - nodes]++;
        e->node = NULL;
    }
    if (e->ops && e->ops->close && e->ofd && e->ofd->refcount == 1) {
        e->ops->close(e);
    }
}

uint32_t socket_inode_next(void)
{
    return ++inodes;
}

void socket_ref(struct socket *sock)
{
    (void)sock;
    check(0, "no row sends a network socket");
}

void signal_post_thread(process_t *t, int signo, const struct k_siginfo *info)
{
    (void)t;
    (void)info;
    if (signo == SIGPIPE) {
        sigpipes++;
    }
}

static uint32_t length(const char *s)
{
    uint32_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static int same(const char *a, const char *b)
{
    uint32_t i;

    for (i = 0; a[i] && a[i] == b[i]; i++) {
    }
    return a[i] == b[i];
}

/**
 * Make the socket file `path`. The file system sleeps first, and the sibling
 * armed there runs, as another thread would while this one waits for the
 * disk. EADDRINUSE when the path has a file.
 */
int32_t fs_socket_bind(const char *path, uint32_t mode, struct vfs_ref *ref, uint32_t *ino)
{
    int i;

    (void)mode;
    run_sibling(&fs_sibling);
    for (i = 0; i < SOCKFILES; i++) {
        if (sockfiles[i].made && same(sockfiles[i].path, path)) {
            return -EADDRINUSE;
        }
    }
    for (i = 0; i < SOCKFILES && sockfiles[i].made; i++) {
    }
    if (i == SOCKFILES || length(path) >= sizeof(sockfiles[i].path)) {
        return -ENOSPC;
    }
    k_memcpy(sockfiles[i].path, path, length(path) + 1u);
    sockfiles[i].made = 1;
    sockfiles[i].held = 1;
    files_made++;
    ref->mnt = &sockmount;
    ref->node = &sockfiles[i];
    *ino = 100u + (uint32_t)i;
    return 0;
}

/** Find the socket file `path`, after a sleep in which the sibling armed
 * there runs, as in `fs_socket_bind`. ENOENT when there is none. */
int32_t fs_socket_lookup(const char *path, struct vfs_mount **mnt, uint32_t *ino)
{
    int i;

    run_sibling(&fs_sibling);
    for (i = 0; i < SOCKFILES; i++) {
        if (sockfiles[i].made && same(sockfiles[i].path, path)) {
            *mnt = &sockmount;
            *ino = 100u + (uint32_t)i;
            return 0;
        }
    }
    return -ENOENT;
}

/** Let go of a socket file, which stays until it is removed, as in Linux. */
void fs_socket_unbind(struct vfs_ref *ref)
{
    ((struct sockfile *)ref->node)->held = 0;
}

static uint8_t *user(uint32_t addr)
{
    return memory + (addr - USER_BASE);
}

static uint32_t word(uint32_t addr)
{
    uint32_t w;

    k_memcpy(&w, user(addr), sizeof(w));
    return w;
}

static void set_word(uint32_t addr, uint32_t w)
{
    k_memcpy(user(addr), &w, sizeof(w));
}

static void set_bytes(uint32_t addr, const char *s)
{
    k_memcpy(user(addr), s, length(s));
}

static int bytes_are(uint32_t addr, const char *s)
{
    uint32_t i;

    for (i = 0; s[i]; i++) {
        if (user(addr)[i] != (uint8_t)s[i]) {
            return 0;
        }
    }
    return 1;
}

/** A sockaddr_un at `addr` with the abstract name `name`: its length. */
static uint32_t abstract_at(uint32_t addr, const char *name)
{
    uint8_t *p = user(addr);

    p[0] = AF_UNIX;
    p[1] = 0;
    p[2] = 0;
    k_memcpy(p + 3, name, length(name));
    return 3u + length(name);
}

/** A sockaddr_un at `addr` with the path `path`: its length with the NUL. */
static uint32_t path_at(uint32_t addr, const char *path)
{
    uint8_t *p = user(addr);

    p[0] = AF_UNIX;
    p[1] = 0;
    k_memcpy(p + 2, path, length(path) + 1u);
    return 3u + length(path);
}

static fd_entry_t *fd_at(int fd)
{
    return &shared.entries[fd];
}

static struct unix_sock *sock_at(int fd)
{
    return (struct unix_sock *)shared.entries[fd].priv;
}

/** close(2) in the shared table, as `close_fd` closes a descriptor no other
 * call holds: 0, or -EBADF for a slot with no descriptor, a reserved one
 * among them. */
static int32_t do_close(int fd)
{
    fd_entry_t *e = fd_get(&shared, fd);

    if (!e) {
        return -EBADF;
    }
    e->flags |= FD_FLAG_CLOSING | FD_FLAG_RELEASING;
    fd_release_backing(e);
    fd_free(&shared, fd);
    return 0;
}

/** open(2) for reading and writing of the file `nodes[n]`, a KRFFS file
 * with no name whose description names its node: the descriptor. */
static int open_orphan(int n)
{
    int fd = fd_alloc(&shared);
    fd_entry_t *e;

    if (fd < 0) {
        return fd;
    }
    e = &shared.entries[fd];
    e->node = &nodes[n];
    e->ofd->vnode = &nodes[n];
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    e->type = FD_TYPE_FILE;
    return fd;
}

/** A socketpair of `type`, its numbers stored on page 5. */
static int32_t pair(int type)
{
    return unix_socketpair(type, 0, 0, AT(5, 0));
}

/** Send the bytes of `s` over `fd` from page 5. */
static int32_t send_text(int fd, const char *s)
{
    set_bytes(AT(5, 64), s);
    return unix_sendto(fd_at(fd), AT(5, 64), length(s), 0, 0, 0);
}

/** Whether a byte sent over `from` is received on `to`. */
static int crosses(int from, int to)
{
    user(AT(5, 128))[0] = 0;
    return send_text(from, "x") == 1 &&
           unix_recvfrom(fd_at(to), AT(5, 128), 1, MSG_DONTWAIT, 0, 0) == 1 &&
           user(AT(5, 128))[0] == 'x';
}

/** sendmsg(2) over `fd` of one byte and the descriptors `fds[0..n)` with
 * SCM_RIGHTS, from page 6. */
static int32_t send_rights(int fd, const int *fds, uint32_t n)
{
    uint32_t msg = AT(6, 0), iov = AT(6, 64), byte = AT(6, 96), ctl = AT(6, 128);
    uint32_t i;

    set_word(msg + 4u * MH_NAME, 0);
    set_word(msg + 4u * MH_NAMELEN, 0);
    set_word(msg + 4u * MH_IOV, iov);
    set_word(msg + 4u * MH_IOVLEN, 1);
    set_word(msg + 4u * MH_CONTROL, ctl);
    set_word(msg + 4u * MH_CONTROLLEN, CMSG_HDR + 4u * n);
    set_word(msg + 4u * MH_FLAGS, 0);
    set_word(iov, byte);
    set_word(iov + 4u, 1);
    user(byte)[0] = 'F';
    set_word(ctl, CMSG_HDR + 4u * n);
    set_word(ctl + 4u, SOL_SOCKET);
    set_word(ctl + 8u, SCM_RIGHTS);
    for (i = 0; i < n; i++) {
        set_word(ctl + CMSG_HDR + 4u * i, (uint32_t)fds[i]);
    }
    return unix_sendmsg(fd_at(fd), msg, 0);
}

/** recvmsg(2) over `fd` of one byte, with `room` bytes of control buffer at
 * `ctl`, or none for 0; the header is on page 7. */
static int32_t recv_rights(int fd, uint32_t ctl, uint32_t room)
{
    uint32_t msg = AT(7, 0), iov = AT(7, 64), byte = AT(7, 96);

    set_word(msg + 4u * MH_NAME, 0);
    set_word(msg + 4u * MH_NAMELEN, 0);
    set_word(msg + 4u * MH_IOV, iov);
    set_word(msg + 4u * MH_IOVLEN, 1);
    set_word(msg + 4u * MH_CONTROL, room ? ctl : 0u);
    set_word(msg + 4u * MH_CONTROLLEN, room);
    set_word(msg + 4u * MH_FLAGS, 0);
    set_word(iov, byte);
    set_word(iov + 4u, 1);
    return unix_recvmsg(fd_at(fd), msg, 0);
}

/** The flags recvmsg stored in the header on page 7. */
static uint32_t recv_flags(void)
{
    return word(AT(7, 4u * MH_FLAGS));
}

/** The start of a row: every page present, no sibling armed, no file freed
 * and no socket file made. */
static void setup(void)
{
    int i;

    k_memset(memory, 0, sizeof(memory));
    for (i = 0; i < PAGES; i++) {
        pages[i] = PAGE_PRESENT;
    }
    for (i = 0; i < FILES; i++) {
        freed[i] = 0;
        seen[i] = 0;
    }
    k_memset(sockfiles, 0, sizeof(sockfiles));
    files_made = 0;
    page_sibling = NULL;
    fs_sibling = NULL;
    release_sibling = NULL;
    ran = 0;
    parked = 0;
    parked_on = NULL;
    copy_needs_park = 0;
    reserved[0] = reserved[1] = -1;
    sibling_rc[0] = sibling_rc[1] = 0;
    sigpipes = 0;
}

/** The end of the row `name`: every descriptor is closed, and nothing is
 * left, no socket, pipe, message, receive or socket file held. */
static void end_row(const char *name)
{
    int i;

    for (i = 0; i < SLOTS; i++) {
        do_close(i);
        fd_free(&shared, i);
    }
    for (i = 0; i < SOCKFILES; i++) {
        if (sockfiles[i].held) {
            break;
        }
    }
    if (socks || pipes || transits || reap_head || live != 0 || i < SOCKFILES) {
        dprintf(2, "unix-hosttest: fail: the %s row leaves something allocated\n", name);
        failures++;
    }
}

/** The sibling's close of the first number the call reserved. */
static void close_reserved(void)
{
    sibling_rc[0] = do_close(reserved[0]);
}

/** The sibling's close of both numbers the call reserved. */
static void close_reserved_pair(void)
{
    sibling_rc[0] = do_close(reserved[0]);
    sibling_rc[1] = do_close(reserved[1]);
}

/** The sibling's close of the first number the call reserved, then an open
 * of a file of its own. */
static void close_and_open(void)
{
    sibling_rc[0] = do_close(reserved[0]);
    sibling_rc[1] = open_orphan(1);
}

/** The sibling's close of `reserved[0]`, noting which files had been freed
 * when it was done. */
static void close_and_look(void)
{
    int i;

    sibling_rc[0] = do_close(reserved[0]);
    for (i = 0; i < FILES; i++) {
        seen[i] = freed[i];
    }
}

/** A listener on 0 and a client on 1, named, whose connect is queued. */
static void queue_connection(void)
{
    uint32_t len;

    check(unix_socket(SOCK_STREAM, 0, 0) == 0 && unix_socket(SOCK_STREAM, 0, 0) == 1,
          "accept: two stream sockets");
    len = abstract_at(AT(1, 0), "listen");
    check(unix_bind(fd_at(0), AT(1, 0), len) == 0 && unix_listen(fd_at(0), 4) == 0,
          "accept: a listener");
    check(unix_bind(fd_at(1), AT(1, 128), abstract_at(AT(1, 128), "client")) == 0 &&
              unix_connect(fd_at(1), AT(1, 0), len) == 0,
          "accept: a named client's connect");
    set_word(AT(0, 0), 64);
}

/**
 * accept(2): the copy of the peer's name sleeps, and a sibling's close of
 * the number accept reserved must find nothing; the connection is published
 * once the name is copied. A copy that fails frees the number and ends the
 * connection, and leaves the file the sibling opened.
 */
static void accept_row(void)
{
    int32_t fd;

    setup();
    queue_connection();
    pages[0] = PAGE_MISSING;
    reserved[0] = 2;
    page_sibling = close_reserved;
    fd = unix_accept(fd_at(0), AT(1, 256), AT(0, 0), 0);
    if (check(ran == 1 && sibling_rc[0] == -EBADF,
              "a sibling's close of the number accept reserved finds nothing while the copy of "
              "the peer's name sleeps") &&
        check(fd == 2 && fd_at(2)->type == FD_TYPE_UNIX && sock_at(2)->peer == sock_at(1),
              "accept publishes the connection once the peer's name is copied")) {
        check(word(AT(0, 0)) == 9 && user(AT(1, 258))[0] == 0 && bytes_are(AT(1, 259), "client"),
              "accept reports the client's name");
        check(crosses(1, 2) && crosses(2, 1), "a byte crosses the accepted connection both ways");
    }
    end_row("accept");

    setup();
    queue_connection();
    pages[0] = PAGE_LOST;
    reserved[0] = 2;
    page_sibling = close_and_open;
    fd = unix_accept(fd_at(0), AT(1, 256), AT(0, 0), 0);
    if (check(ran == 1 && sibling_rc[0] == -EBADF && sibling_rc[1] == 3,
              "a sibling can neither close nor take the number accept reserved")) {
        check(fd == -EFAULT && !fd_at(2)->ofd && fd_at(3)->type == FD_TYPE_FILE &&
                  !sock_at(0)->acc_head && sock_at(1)->shut == SHUT_BOTH,
              "an accept whose copy fails frees the number it reserved and ends the connection, "
              "and leaves the sibling's file");
    }
    end_row("failed accept");
}

/**
 * socketpair(2): the copy of the two numbers sleeps, and a sibling's close
 * of either must find nothing; both ends are published once the numbers
 * are copied. A copy that fails frees both numbers and both sockets, and
 * leaves the file the sibling opened.
 */
static void socketpair_row(void)
{
    int32_t rc;

    setup();
    pages[0] = PAGE_MISSING;
    reserved[0] = 0;
    reserved[1] = 1;
    page_sibling = close_reserved_pair;
    rc = unix_socketpair(SOCK_STREAM, 0, 0, AT(0, 0));
    if (check(ran == 1 && sibling_rc[0] == -EBADF && sibling_rc[1] == -EBADF,
              "a sibling's close of the numbers socketpair reserved finds nothing while their copy "
              "sleeps") &&
        check(rc == 0 && word(AT(0, 0)) == 0 && word(AT(0, 4)) == 1 &&
                  fd_at(0)->type == FD_TYPE_UNIX && fd_at(1)->type == FD_TYPE_UNIX &&
                  sock_at(0)->peer == sock_at(1),
              "socketpair publishes both ends once their numbers are copied")) {
        check(crosses(0, 1) && crosses(1, 0), "a byte crosses the pair both ways");
    }
    end_row("socketpair");

    setup();
    pages[0] = PAGE_LOST;
    reserved[0] = 0;
    page_sibling = close_and_open;
    rc = unix_socketpair(SOCK_STREAM, 0, 0, AT(0, 0));
    if (check(ran == 1 && sibling_rc[0] == -EBADF && sibling_rc[1] == 2,
              "a sibling can neither close nor take a number socketpair reserved")) {
        check(rc == -EFAULT && !fd_at(0)->ofd && !fd_at(1)->ofd && fd_at(2)->type == FD_TYPE_FILE &&
                  !socks,
              "a socketpair whose copy fails frees what it reserved and its sockets, and leaves "
              "the sibling's file");
    }
    end_row("failed socketpair");
}

/**
 * A receive of SCM_RIGHTS: the copy of the new number sleeps, and a
 * sibling's close of it must find nothing; the descriptor is installed once
 * its number is copied. A copy that fails frees the number, closes the
 * descriptor in flight and sets MSG_CTRUNC, and leaves the file the sibling
 * opened.
 */
static void rights_row(void)
{
    int32_t rc;
    int f;

    setup();
    check(pair(SOCK_STREAM) == 0, "rights: a stream pair");
    f = open_orphan(0);
    check(f == 2 && send_rights(0, &f, 1) == 1, "rights: a descriptor sent with SCM_RIGHTS");
    pages[2] = PAGE_MISSING;
    reserved[0] = 3;
    page_sibling = close_reserved;
    rc = recv_rights(1, AT(2, 0), CMSG_HDR + 4u);
    if (check(ran == 1 && sibling_rc[0] == -EBADF,
              "a sibling's close of the number a receive reserved finds nothing while the copy of "
              "the number sleeps")) {
        check(rc == 1 && word(AT(2, CMSG_HDR)) == 3 && fd_at(3)->type == FD_TYPE_FILE &&
                  fd_at(3)->node == &nodes[0] && fd_at(3)->ofd == fd_at(2)->ofd,
              "a receive installs the descriptor once its number is copied");
    }
    end_row("rights");

    setup();
    check(pair(SOCK_STREAM) == 0, "rights: a stream pair");
    f = open_orphan(0);
    check(f == 2 && send_rights(0, &f, 1) == 1, "rights: a descriptor sent with SCM_RIGHTS");
    pages[2] = PAGE_LOST;
    reserved[0] = 3;
    page_sibling = close_and_open;
    rc = recv_rights(1, AT(2, 0), CMSG_HDR + 4u);
    if (check(ran == 1 && sibling_rc[0] == -EBADF && sibling_rc[1] == 4,
              "a sibling can neither close nor take the number a receive reserved")) {
        check(rc == 1 && (recv_flags() & MSG_CTRUNC) && !fd_at(3)->ofd &&
                  fd_at(4)->type == FD_TYPE_FILE && fd_at(4)->node == &nodes[1] &&
                  fd_at(2)->ofd->refcount == 1,
              "a receive whose copy of the number fails frees the number and the descriptor in "
              "flight, and leaves the sibling's file");
    }
    end_row("failed rights");
}

/**
 * pipe2(2): the copy of the two numbers sleeps, and a sibling's close of
 * either must find nothing; both ends are given once the numbers are
 * copied. A copy that fails frees both numbers and the pipe, and leaves the
 * file the sibling opened.
 */
static void pipe_row(void)
{
    isr_params_t params;
    int32_t rc;

    setup();
    k_memset(&params, 0, sizeof(params));
    params.ebx = AT(0, 0);
    pages[0] = PAGE_MISSING;
    reserved[0] = 0;
    reserved[1] = 1;
    page_sibling = close_reserved_pair;
    rc = sys_pipe2(&params);
    if (check(ran == 1 && sibling_rc[0] == -EBADF && sibling_rc[1] == -EBADF,
              "a sibling's close of the numbers pipe2 reserved finds nothing while their copy "
              "sleeps")) {
        check(rc == 0 && word(AT(0, 0)) == 0 && word(AT(0, 4)) == 1 &&
                  fd_at(0)->type == FD_TYPE_PIPE && fd_at(1)->type == FD_TYPE_PIPE &&
                  fd_at(0)->priv == fd_at(1)->priv && (fd_at(0)->flags & FD_FLAG_READ) &&
                  (fd_at(1)->flags & FD_FLAG_WRITE),
              "pipe2 gives both descriptors their ends once the numbers are copied");
    }
    end_row("pipe2");

    setup();
    k_memset(&params, 0, sizeof(params));
    params.ebx = AT(0, 0);
    pages[0] = PAGE_LOST;
    reserved[0] = 0;
    page_sibling = close_and_open;
    rc = sys_pipe2(&params);
    if (check(ran == 1 && sibling_rc[0] == -EBADF && sibling_rc[1] == 2,
              "a sibling can neither close nor take a number pipe2 reserved")) {
        check(rc == -EFAULT && !fd_at(0)->ofd && !fd_at(1)->ofd && fd_at(2)->type == FD_TYPE_FILE &&
                  !pipes,
              "a pipe2 whose copy fails frees what it reserved and the pipe, and leaves the "
              "sibling's file");
    }
    end_row("failed pipe2");
}

/** The sibling's connect of socket 1 to the listener on 0. */
static void connect_sibling(void)
{
    sibling_rc[0] = unix_connect(fd_at(1), AT(1, 0), name_len);
}

/**
 * connect(2) to an abstract name, whose copy of the address sleeps, and to a
 * path, whose lookup of the socket file sleeps, while a sibling connects the
 * socket. The connect must fail with EISCONN and leave the listener with
 * the sibling's connection alone.
 */
static void connect_row(int by_path)
{
    int32_t rc;

    setup();
    check(unix_socket(SOCK_STREAM, 0, 0) == 0 && unix_socket(SOCK_STREAM, 0, 0) == 1,
          "connect: two stream sockets");
    name_len = by_path ? path_at(AT(1, 0), "/tmp/server") : abstract_at(AT(1, 0), "server");
    check(unix_bind(fd_at(0), AT(1, 0), name_len) == 0 && unix_listen(fd_at(0), 4) == 0,
          "connect: a listener");
    if (by_path) {
        fs_sibling = connect_sibling;
        rc = unix_connect(fd_at(1), AT(1, 0), name_len);
    } else {
        abstract_at(AT(0, 0), "server");
        pages[0] = PAGE_MISSING;
        page_sibling = connect_sibling;
        rc = unix_connect(fd_at(1), AT(0, 0), name_len);
    }
    check(ran == 1 && sibling_rc[0] == 0, "connect: the sibling connects the socket");
    if (check(rc == -EISCONN, by_path ? "a connect whose lookup of the path sleeps while a "
                                        "sibling connects the socket fails with EISCONN"
                                      : "a connect whose copy of the address sleeps while a "
                                        "sibling connects the socket fails with EISCONN") &&
        check(sock_at(0)->acc_len == 1 && sock_at(1)->peer == sock_at(0)->acc_head,
              "the listener has the sibling's connection alone")) {
        check(unix_accept(fd_at(0), 0, 0, 0) == 2 && crosses(1, 2) && crosses(2, 1),
              "a byte crosses the sibling's connection both ways");
    }
    end_row("connect");
}

/** The sibling's bind of socket 0 to the second path. */
static void bind_sibling(void)
{
    sibling_rc[0] = unix_bind(fd_at(0), AT(1, 128), name_len);
}

/**
 * bind(2) to a path: the file system's making of the socket file sleeps,
 * and a sibling's bind of the same socket must wait for the bind lock. The
 * first bind names the socket; the one that waited then fails with EINVAL,
 * so one socket file is made, and the socket's close lets it go.
 */
static void bind_row(void)
{
    struct unix_sock *s;
    uint32_t len;
    int32_t rc;

    setup();
    check(unix_socket(SOCK_STREAM, 0, 0) == 0, "bind: a stream socket");
    s = sock_at(0);
    len = path_at(AT(1, 0), "/tmp/first");
    name_len = path_at(AT(1, 128), "/tmp/second");
    fs_sibling = bind_sibling;
    rc = unix_bind(fd_at(0), AT(1, 0), len);
    check(ran == 1 && parked && parked_on == &s->lock_wq,
          "a second bind of a socket waits for the bind lock while the first sleeps in the file "
          "system");
    check(rc == 0 && s->bound && s->bound_fs && s->fs_ref.node == &sockfiles[0] &&
              same(sockfiles[0].path, "/tmp/first"),
          "the first bind names the socket with the file it made");
    if (parked) {
        check(s->lock_wq.wakes != wakes_at_park, "the first bind's end wakes the bind that waits");
        check(park_run(bind_sibling) && sibling_rc[0] == -EINVAL,
              "the bind that waited finds the socket named and fails with EINVAL");
    }
    check(files_made == 1, "the two binds make one socket file");
    do_close(0);
    check(!sockfiles[0].held && !sockfiles[1].held,
          "the socket's close lets go of every socket file it was given");
    end_row("bind");
}

/** The sibling's nonblocking receive on socket 1. */
static void receive_sibling(void)
{
    sibling_rc[0] = unix_recvfrom(fd_at(1), AT(1, 256), 16, MSG_DONTWAIT, 0, 0);
}

/**
 * A receive whose copy of the first of two messages sleeps: a sibling's
 * receive must wait for the receive lock, the first receive must return its
 * message whole, and the sibling, run again, the second.
 */
static void receive_row(int type, const char *waits)
{
    int32_t rc;

    setup();
    check(pair(type) == 0 && send_text(0, "one") == 3 && send_text(0, "two") == 3,
          "receive: a pair with two messages queued");
    pages[0] = PAGE_MISSING;
    copy_needs_park = 1;
    page_sibling = receive_sibling;
    rc = unix_recvfrom(fd_at(1), AT(0, 0), 3, 0, 0, 0);
    check(ran == 1 && parked && parked_on == &sock_at(1)->lock_wq, waits);
    check(rc == 3 && bytes_are(AT(0, 0), "one"),
          "the receive whose copy slept returns its message whole");
    if (parked) {
        check(park_run(receive_sibling) && sibling_rc[0] == 3 && bytes_are(AT(1, 256), "two"),
              "the receive that waited takes the next message");
    }
    check(!sock_at(1)->rq_head, "receive: both messages are taken");
    end_row("receive");
}

/** The sibling's shutdown of socket 0 for writing. */
static void shutdown_sibling(void)
{
    sibling_rc[0] = unix_shutdown(fd_at(0), 1);
}

/**
 * A send whose copy of the caller's bytes sleeps while a sibling shuts the
 * socket down for writing: nothing may be queued, and the send fails with
 * EPIPE, with SIGPIPE on a stream.
 */
static void send_row(int type, const char *stops)
{
    int32_t rc;

    setup();
    check(pair(type) == 0, "send: a pair");
    set_bytes(AT(0, 0), "xyz");
    pages[0] = PAGE_MISSING;
    page_sibling = shutdown_sibling;
    rc = unix_sendto(fd_at(0), AT(0, 0), 3, 0, 0, 0);
    check(ran == 1 && sibling_rc[0] == 0, "send: the sibling shuts the socket down");
    check(rc == -EPIPE && !sock_at(1)->rq_head && sigpipes == (type == SOCK_STREAM), stops);
    end_row("send");
}

/**
 * A receive's descriptors in flight, off their queue: the holder count
 * finds them until they are installed or closed. A sibling's close of a
 * file's last descriptor while the receive installs the file's descriptor,
 * or closes it with another it has no room for, must not free the file;
 * the release of the descriptor in flight that is its file's last holder
 * frees it; and a descriptor the receive has installed is no longer counted
 * in flight, so the close of it frees its file while the receive installs
 * the next.
 */
static void transit_row(void)
{
    int32_t rc;
    int f[2];

    setup();
    check(pair(SOCK_STREAM) == 0, "transit: a stream pair");
    f[0] = open_orphan(0);
    check(f[0] == 2 && send_rights(0, f, 1) == 1, "transit: a file's descriptor sent");
    pages[2] = PAGE_MISSING;
    reserved[0] = 2;
    page_sibling = close_and_look;
    rc = recv_rights(1, AT(2, 0), CMSG_HDR + 4u);
    check(ran == 1 && sibling_rc[0] == 0 && seen[0] == 0,
          "a file is not freed by the close of its last descriptor while a receive installs its "
          "descriptor in flight");
    check(rc == 1 && fd_at(3)->type == FD_TYPE_FILE && fd_at(3)->node == &nodes[0],
          "transit: the receive installs the descriptor");
    do_close(3);
    check(freed[0] == 1, "transit: the close of the installed descriptor frees the file once");
    end_row("transit while installing");

    setup();
    check(pair(SOCK_STREAM) == 0, "transit: a stream pair");
    f[0] = open_orphan(1);
    f[1] = open_orphan(2);
    check(f[0] == 2 && f[1] == 3 && send_rights(0, f, 2) == 1, "transit: two descriptors sent");
    check(do_close(2) == 0 && freed[1] == 0,
          "a file whose descriptor is queued in flight outlives the close of its last descriptor");
    reserved[0] = 3;
    release_sibling = close_and_look;
    rc = recv_rights(1, 0, 0);
    check(ran == 1 && seen[1] == 1,
          "the release of a descriptor in flight that is its file's last holder frees the file");
    check(ran == 1 && sibling_rc[0] == 0 && seen[2] == 0,
          "a file is not freed by the close of its last descriptor while a receive closes its "
          "descriptor in flight with another");
    check(rc == 1 && (recv_flags() & MSG_CTRUNC) && freed[1] == 1 && freed[2] == 1,
          "a receive with no room for descriptors closes them, and frees each file once");
    end_row("transit while closing");

    setup();
    check(pair(SOCK_STREAM) == 0, "transit: a stream pair");
    f[0] = open_orphan(3);
    f[1] = open_orphan(0);
    check(f[0] == 2 && f[1] == 3 && send_rights(0, f, 2) == 1 && do_close(2) == 0 &&
              do_close(3) == 0,
          "transit: two files whose only holders are in flight");
    pages[3] = PAGE_MISSING;
    reserved[0] = 2;
    page_sibling = close_and_look;
    rc = recv_rights(1, AT(2, PAGE_SIZE - CMSG_HDR - 4u), CMSG_HDR + 8u);
    check(ran == 1 && sibling_rc[0] == 0 && seen[3] == 1,
          "a descriptor a receive has installed is not counted in flight: its close frees the "
          "file while the receive installs the next");
    check(rc == 1 && fd_at(3)->type == FD_TYPE_FILE && fd_at(3)->node == &nodes[0],
          "transit: the receive installs the second descriptor");
    do_close(3);
    check(freed[0] == 1 && freed[3] == 1, "transit: each file is freed once");
    end_row("transit after installing");
}

int main(void)
{
    unix_init();
    pipe_init_pool();
    shared.entries = slots;
    shared.cap = SLOTS;
    shared.refs = 2;
    dirs.umask = 022;
    self.fs = &dirs;
    self.fds = &shared;
    self.tgid = 100;
    self.ruid = self.euid = self.suid = 1000;
    self.rgid = self.egid = self.sgid = 1000;
    accept_row();
    socketpair_row();
    rights_row();
    pipe_row();
    connect_row(0);
    connect_row(1);
    bind_row();
    receive_row(SOCK_DGRAM, "a datagram receive while another's copy of a message sleeps waits "
                            "for the receive lock");
    receive_row(SOCK_STREAM, "a stream receive while another's copy of a message sleeps waits "
                             "for the receive lock");
    send_row(SOCK_STREAM, "a stream send whose copy sleeps while the socket is shut down for "
                          "writing queues nothing and fails with EPIPE");
    send_row(SOCK_DGRAM, "a datagram send whose copy sleeps while the socket is shut down for "
                         "writing queues nothing and fails with EPIPE");
    transit_row();
    dprintf(1, "unix-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
