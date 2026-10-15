/*
 * krffs_fuse_hosttest.c: the FUSE mount of KRFFS, the entry point of
 * krffs/krffs.c and the callbacks of krffs/krffs_fuse_operations.c, over a
 * mapping of a scratch image file, with tests/host/fuse/fuse.h in place of
 * libfuse's header, and with krffs/stageimage.krffs.c to stage nodes as the
 * image scripts do. The image file is in a directory of the test's own
 * (compat/fixdir_host.c), kept when a row fails. The warnings that
 * krffs/Makefile makes errors are errors here too, so a driver that
 * `make -C krffs mount` cannot build fails the test.
 *
 * host-deps: krffs/krffs_fuse_operations.c krffs/krffs_allocator.c
 * host-deps: krffs/krffs_node.c krffs/krffs_file_system.c
 * host-deps: krffs/krffs_utilities.c krffs/krffs_platform.c
 * host-deps: tests/host/compat/fixdir_host.c
 * host-cflags: -I tests/host/fuse -Wall -Wextra -Werror
 */
#define main krffs_mount_main
#include "../../krffs/krffs.c"
#undef main
#define main krffs_stage_main
#include "../../krffs/stageimage.krffs.c"
#undef main

#include "fixdir_host.h"

static int failures;
static int mounted;
static int no_request;
static struct krffs_file_system fixture;
static const char *image_path;
static char listing[512];

/** Count and print a failed row; the rows after it still run. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "krffs-fuse-test: fail: %s\n", what);
        failures++;
    }
}

/**
 * Count the call in `mounted` and return 0, in place of FUSE's dispatch.
 * The mount entry point has mapped and checked the image before it calls
 * this, so a row learns from `mounted` whether a bad image reached FUSE.
 */
int fuse_main(int argc, char **argv, const struct fuse_operations *ops, void *data)
{
    (void)argc;
    (void)argv;
    (void)ops;
    (void)data;
    mounted++;
    return 0;
}

/**
 * The identity of a request, as FUSE gives it: user 123, group 456, or NULL
 * while `no_request` is set.
 */
struct fuse_context *fuse_get_context(void)
{
    static struct fuse_context context = {123, 456};
    return no_request ? NULL : &context;
}

/**
 * Append `name` to `listing`, after a blank unless it is the first, in place
 * of FUSE's filler.
 */
static int list_name(void *buffer, const char *name, const struct stat *stat_buffer, off_t offset)
{
    (void)buffer;
    (void)stat_buffer;
    (void)offset;
    if (listing[0] != '\0') {
        strncat(listing, " ", sizeof(listing) - strlen(listing) - 1);
    }
    strncat(listing, name, sizeof(listing) - strlen(listing) - 1);
    return 0;
}

/** Return the names `krffs_readdir` lists for `path`, or "error". */
static const char *list(const char *path)
{
    listing[0] = '\0';
    if (krffs_readdir(path, NULL, list_name, 0, NULL) != 0) {
        return "error";
    }
    return listing;
}

/**
 * Fill the image with 0xa5 and lay a fresh file system over it, so that a
 * valid node ring has bytes no node uses that are not zero.
 */
static void reset_image(void)
{
    memset(fixture.node, 0xa5, (size_t)fixture.size);
    krffs_initialize_file_system(&fixture);
    KRFFS_File_System = &fixture;
}

/**
 * Run the mount entry point over the image file and require it, when
 * `valid`, to succeed and reach FUSE once, and otherwise to fail before
 * FUSE gets the mapping.
 */
static void mount_result(int valid, const char *what)
{
    char *args[] = {"krffs", (char *)image_path, "/unused", NULL};
    int result;

    mounted = 0;
    result = krffs_mount_main(3, args);
    check(valid ? result == 0 && mounted == 1 : result != 0 && mounted == 0, what);
    KRFFS_File_System = &fixture;
}

/**
 * Check that the mount fails on a fault in each field of the node ring a
 * callback's walk reads unchecked: a root size past the image, a zero
 * size, a bad magic, a first node that is not the root or a second root,
 * a backward or closing link that is wrong, data larger than its node, a
 * name with no end, and a last header cut short; and, on a ring with a
 * file, the file's data past its node and its name with no end. A free
 * node that keeps the length of deleted data mounts, as does the ring with
 * the file before its faults.
 */
static void ring_rows(void)
{
    struct krffs_node *root = fixture.node;
    struct krffs_node *next = root + 1;

    reset_image();
    mount_result(1, "valid ring mounts");
    root->size = (uint64_t)fixture.size * 3;
    mount_result(0, "oversized root rejected before fuse");
    reset_image();
    next->size = 0;
    mount_result(0, "zero node size rejected before fuse");
    reset_image();
    next->magic = 0;
    mount_result(0, "bad node magic rejected");
    reset_image();
    root->type = KRFFS_Free_Node;
    mount_result(0, "non-root first node rejected");
    reset_image();
    next->type = KRFFS_Root_Node;
    mount_result(0, "second root rejected");
    reset_image();
    next->previous_node_size++;
    mount_result(0, "bad backward link rejected");
    reset_image();
    root->previous_node_size++;
    mount_result(0, "bad closing link rejected");
    reset_image();
    root->data_size = 1;
    mount_result(0, "oversized data rejected");
    reset_image();
    memset(root->name, 'x', sizeof(root->name));
    mount_result(0, "unterminated name rejected");
    reset_image();
    next->size--;
    mount_result(0, "partial final header rejected");
    reset_image();
    next->data_size = (uint64_t)-1;
    mount_result(1, "free node may retain deleted data size");
    reset_image();
    check(krffs_create("/a", 0600, NULL) == 0, "create mount fixture");
    check(krffs_write("/a", "abc", 3, 0, NULL) == 3, "write mount fixture");
    mount_result(1, "allocated file ring mounts");
    next->data_size = next->size;
    mount_result(0, "reserved data outside allocation rejected");
    next->data_size = 3;
    memset(next->name, 'x', sizeof(next->name));
    mount_result(0, "unterminated reserved name rejected");
}

/**
 * Write one byte at 8192 of a new file and require the bytes before it to
 * read as zeros, the old free node's header among them. With
 * `relocate`, a second file right after the first makes the growth move
 * the file.
 */
static void hole_row(int relocate)
{
    char buffer[8193] = {0};
    size_t i;
    int zero = 1;

    reset_image();
    check(krffs_create("/a", 0600, NULL) == 0, "create hole file");
    if (relocate) {
        check(krffs_create("/b", 0600, NULL) == 0, "block adjacent growth");
    }
    check(krffs_write("/a", "x", 1, 8192, NULL) == 1, "write after eof");
    check(krffs_read("/a", buffer, sizeof(buffer), 0, NULL) == sizeof(buffer), "read hole");
    for (i = 0; i < sizeof(buffer) - 1; i++) {
        if (buffer[i] != 0) {
            zero = 0;
        }
    }
    check(zero && buffer[8192] == 'x', "write gap is zero");
}

/**
 * Check that a file shrunk beside an allocated neighbour keeps its slack: a
 * write inside the slack leaves zeros in the gap, and a truncate that grows
 * back to the capacity reads the new byte as zero.
 */
static void slack_row(void)
{
    char buffer[16] = {0};

    reset_image();
    check(krffs_create("/a", 0600, NULL) == 0, "create slack file");
    check(krffs_write("/a", "abcdefghijklmnop", 16, 0, NULL) == 16, "fill slack file");
    check(krffs_create("/b", 0600, NULL) == 0, "reserve neighbour");
    check(krffs_truncate("/a", 1) == 0, "shrink inside allocation");
    check(krffs_write("/a", "x", 1, 15, NULL) == 1, "write inside allocation");
    check(krffs_read("/a", buffer, 16, 0, NULL) == 16, "read retained allocation");
    check(buffer[0] == 'a' && buffer[15] == 'x' &&
              memcmp(buffer + 1, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 14) == 0,
          "slack write gap is zero");
    check(krffs_truncate("/a", 15) == 0, "truncate to fifteen");
    check(krffs_truncate("/a", 16) == 0, "truncate back to capacity");
    check(krffs_read("/a", buffer, 16, 0, NULL) == 16, "capacity growth updates eof");
    check(buffer[15] == 0, "truncate growth clears new byte");
}

/**
 * Check that an owner or a group of -1 in chown leaves that field alone, and
 * that a node made with no request has the test's effective user and group,
 * not the bytes of the free node it came from.
 */
static void owner_rows(void)
{
    struct krffs_node *node;

    reset_image();
    check(krffs_create("/a", 0600, NULL) == 0, "create owned file");
    node = krffs_find_node_by_path(&fixture, "/a", 2);
    check(krffs_chown("/a", (uid_t)-1, 789) == 0 && node->uid == 123 && node->gid == 789,
          "group-only chown preserves uid");
    check(krffs_chown("/a", 321, (gid_t)-1) == 0 && node->uid == 321 && node->gid == 789,
          "owner-only chown preserves gid");
    check(krffs_chown("/a", (uid_t)-1, (gid_t)-1) == 0 && node->uid == 321 && node->gid == 789,
          "unchanged ownership");
    no_request = 1;
    check(krffs_create("/b", 0600, NULL) == 0, "create with no request");
    no_request = 0;
    node = krffs_find_node_by_path(&fixture, "/b", 2);
    check(node && node->uid == geteuid() && node->gid == getegid(),
          "no request gives the driver's owner");
}

/**
 * Check utimens with explicit times, with `UTIME_OMIT` and `UTIME_NOW` in
 * either field, and with no times, which sets both to now.
 */
static void time_rows(void)
{
    struct timespec times[2] = {{946684800, 123}, {946684900, 456}};
    struct krffs_node *node;
    struct krffs_node_time before;

    reset_image();
    check(krffs_create("/a", 0600, NULL) == 0, "create timed file");
    node = krffs_find_node_by_path(&fixture, "/a", 2);
    check(krffs_utimens("/a", times) == 0 && node->access_time.seconds == 946684800 &&
              node->access_time.nanoseconds == 123 &&
              node->modification_time.seconds == 946684900 &&
              node->modification_time.nanoseconds == 456,
          "explicit timestamps retained");
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_nsec = UTIME_OMIT;
    node->attribute_change_time.seconds = 42;
    check(krffs_utimens("/a", times) == 0 && node->access_time.seconds == 946684800 &&
              node->modification_time.seconds == 946684900 &&
              node->attribute_change_time.seconds == 42,
          "omit both keeps all times");
    before = krffs_get_current_node_time();
    times[1].tv_nsec = UTIME_NOW;
    check(krffs_utimens("/a", times) == 0 && node->access_time.seconds == 946684800 &&
              node->modification_time.seconds >= before.seconds &&
              node->attribute_change_time.seconds >= before.seconds,
          "omit access and set now");
    times[0].tv_nsec = UTIME_NOW;
    times[1].tv_nsec = UTIME_OMIT;
    node->modification_time.seconds = 77;
    check(krffs_utimens("/a", times) == 0 && node->access_time.seconds >= before.seconds &&
              node->modification_time.seconds == 77,
          "set access and omit modification");
    check(krffs_utimens("/a", NULL) == 0 && node->access_time.seconds >= before.seconds &&
              node->modification_time.seconds >= before.seconds,
          "null times set now");
}

/**
 * Check the block counts of statfs: the 64 blocks of the image, the free
 * space rounded down to whole blocks, and no free block once a file has
 * taken every free byte.
 */
static void statfs_rows(void)
{
    struct statvfs buffer;
    uint64_t free_space;

    reset_image();
    free_space = krffs_calculate_total_free_space(&fixture);
    check(krffs_statfs("/", &buffer) == 0 && buffer.f_blocks == 64 &&
              buffer.f_bfree == free_space / 1024,
          "free blocks rounded down");
    check(krffs_create("/a", 0600, NULL) == 0, "create the filling file");
    free_space = krffs_calculate_total_free_space(&fixture);
    check(krffs_truncate("/a", (off_t)(free_space + sizeof(struct krffs_node))) == 0 &&
              krffs_count_free_nodes(&fixture) == 0,
          "take every free byte");
    check(krffs_statfs("/", &buffer) == 0 && buffer.f_bfree == 0 && buffer.f_bavail == 0,
          "full image has no free block");
}

/**
 * Stage `/d/f` and then `/f` with the image stager, and require the ids the
 * kernel gives: the hash of the name for each file, and for `/d` the hash
 * mixed with the root's id. Then require the mount to find each by its
 * directory and name: each reads its own bytes, `/d` is a directory, and each
 * directory lists its own names once.
 */
static void staged_rows(void)
{
    struct stat stat_buffer;
    char buffer[8] = {0};
    struct krffs_node *inner;
    struct krffs_node *outer;
    struct krffs_node *directory;
    uint64_t hash = krffs_calculate_djb_hash((uint8_t *)"f");

    reset_image();
    check(stage_file(&fixture, "/d/f", 0100644, 0, 0, "inner", 5) &&
              stage_file(&fixture, "/f", 0100644, 0, 0, "outer", 5),
          "stage two files named f");
    inner = krffs_find_node_by_path(&fixture, "/d/f", 4);
    outer = krffs_find_node_by_path(&fixture, "/f", 2);
    directory = krffs_find_node_by_path(&fixture, "/d", 2);
    check(inner != NULL && outer != NULL && inner->id == hash && outer->id == hash,
          "staged file has the hash of its name as its id");
    check(directory != NULL && directory->id == krffs_mix_id(0, "d"),
          "staged directory has its mixed id");
    check(krffs_getattr("/d", &stat_buffer) == 0 && S_ISDIR(stat_buffer.st_mode),
          "staged directory is a directory");
    check(krffs_open("/d/f", NULL) == 0, "staged file opens");
    check(krffs_read("/d/f", buffer, sizeof(buffer), 0, NULL) == 5 &&
              memcmp(buffer, "inner", 5) == 0,
          "staged file in a directory reads");
    check(krffs_read("/f", buffer, sizeof(buffer), 0, NULL) == 5 && memcmp(buffer, "outer", 5) == 0,
          "staged file of the same name at the root reads");
    check(strcmp(list("/"), ". .. d f") == 0 && strcmp(list("/d"), ". .. f") == 0,
          "each staged directory lists its own names");
}

/**
 * Check the directories the mount makes: their mode and owner, a file in one
 * apart from a file of the same name at the root, the root's link count,
 * creation under a missing directory and under a file, the refusals of
 * `rmdir`, `unlink` and `rename` that the kernel makes, a rename across
 * directories, a rename to the same name, and `mkdir` refusing `/cb`, whose
 * id `/bC` has.
 */
static void directory_rows(void)
{
    struct stat stat_buffer;
    char buffer[4] = {0};

    reset_image();
    check(krffs_mkdir("/d", 0750) == 0, "mkdir");
    check(krffs_getattr("/d", &stat_buffer) == 0 && S_ISDIR(stat_buffer.st_mode) &&
              (stat_buffer.st_mode & 07777) == 0750 && stat_buffer.st_uid == 123 &&
              stat_buffer.st_nlink == 2,
          "made directory attributes");
    check(krffs_create("/d/a", 0100600, NULL) == 0 && krffs_write("/d/a", "in", 2, 0, NULL) == 2,
          "create in a directory");
    check(krffs_create("/a", 0100600, NULL) == 0 && krffs_write("/a", "out", 3, 0, NULL) == 3,
          "create the same name at the root");
    check(krffs_read("/d/a", buffer, sizeof(buffer), 0, NULL) == 2 && memcmp(buffer, "in", 2) == 0,
          "directory keeps its own file");
    check(strcmp(list("/"), ". .. d a") == 0 && strcmp(list("/d"), ". .. a") == 0,
          "each made directory lists its own names");
    check(krffs_getattr("/", &stat_buffer) == 0 && stat_buffer.st_nlink == 3,
          "root counts its directory");
    check(krffs_create("/x/a", 0100600, NULL) == -ENOENT, "create under a missing directory");
    check(krffs_create("/a/b", 0100600, NULL) == -ENOTDIR, "create under a file");
    check(krffs_rmdir("/d") == -ENOTEMPTY, "rmdir refuses a directory with a file");
    check(krffs_unlink("/d") == -EISDIR, "unlink refuses a directory");
    check(krffs_rename("/d", "/e") == -EPERM, "rename refuses a directory");
    check(krffs_rename("/a", "/d") == -EISDIR, "rename does not replace a directory");
    check(krffs_rename("/d/a", "/b") == 0 && krffs_getattr("/d/a", &stat_buffer) == -ENOENT &&
              krffs_read("/b", buffer, sizeof(buffer), 0, NULL) == 2 &&
              memcmp(buffer, "in", 2) == 0,
          "rename across directories");
    check(krffs_rename("/b", "/b") == 0 && krffs_read("/b", buffer, sizeof(buffer), 0, NULL) == 2 &&
              memcmp(buffer, "in", 2) == 0,
          "rename to the same name keeps the file");
    check(krffs_rmdir("/d") == 0 && krffs_getattr("/d", &stat_buffer) == -ENOENT,
          "rmdir an empty directory");
    check(krffs_mkdir("/bC", 0755) == 0 && krffs_mkdir("/cb", 0755) == -EINVAL,
          "mkdir refuses a taken directory id");
}

/**
 * Grow a file in a directory with a neighbour after it, which moves the file
 * to a new node, and require it to stay in its directory.
 */
static void moved_row(void)
{
    struct stat stat_buffer;

    reset_image();
    check(krffs_mkdir("/d", 0755) == 0 && krffs_create("/d/a", 0100600, NULL) == 0 &&
              krffs_create("/d/b", 0100600, NULL) == 0,
          "make a file and its neighbour");
    check(krffs_write("/d/a", "x", 1, 8191, NULL) == 1, "grow past the neighbour");
    check(krffs_getattr("/d/a", &stat_buffer) == 0 && stat_buffer.st_size == 8192,
          "moved file keeps its directory");
}

/**
 * Map a scratch image file of 64 KiB in the fixture directory and run the
 * rows. Returns 0 when every row passed, 1 when one failed, and 2 when the
 * image cannot be made.
 */
int main(void)
{
    int fd;

    image_path = fix_path("krffs-fuse.img");
    fd = open(image_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return 2;
    }
    if (ftruncate(fd, 65536) != 0) {
        close(fd);
        return 2;
    }
    fixture.size = 65536;
    fixture.node = krffs_map_file(fd, 0, (size_t)fixture.size);
    close(fd);
    if (fixture.node == (void *)-1) {
        return 2;
    }
    ring_rows();
    hole_row(0);
    hole_row(1);
    slack_row();
    owner_rows();
    statfs_rows();
    time_rows();
    staged_rows();
    directory_rows();
    moved_row();
    krffs_unmap_file(fixture.node, (size_t)fixture.size);
    printf("krffs-fuse-test: result: %d failures\n", failures);
    if (failures == 0) {
        fix_passed();
    }
    return failures != 0;
}
