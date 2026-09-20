/**
 * Exercise the mount entry point and FUSE callbacks over a private mapping.
 *
 * host-deps: krffs/krffs_fuse_operations.c krffs/krffs_allocator.c
 * host-deps: krffs/krffs_node.c krffs/krffs_file_system.c
 * host-deps: krffs/krffs_utilities.c krffs/krffs_platform.c
 * host-cflags: -I tests/host/fuse
 */
#define main krffs_mount_main
#include "../../krffs/krffs.c"
#undef main

static int failures;
static int mounted;
static struct krffs_file_system fixture;
static char image_path[] = "/tmp/rach2-fuse.XXXXXX";

/** Record an assertion without stopping later independent rows. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "krffs-fuse-test: fail: %s\n", what);
        failures++;
    }
}

/** Replace only FUSE dispatch; the mount entry point maps the real image. */
int fuse_main(int argc, char **argv, const struct fuse_operations *ops, void *data)
{
    (void)argc;
    (void)argv;
    (void)ops;
    (void)data;
    mounted++;
    return 0;
}

/** Supply the identity FUSE would attach to a request. */
struct fuse_context *fuse_get_context(void)
{
    static struct fuse_context context = {123, 456};
    return &context;
}

/** Initialize a valid ring with nonzero unused bytes. */
static void reset_image(void)
{
    memset(fixture.node, 0xa5, (size_t)fixture.size);
    krffs_initialize_file_system(&fixture);
    KRFFS_File_System = &fixture;
}

/** Require invalid images to stop before FUSE receives the mapping. */
static void mount_result(int valid, const char *what)
{
    char *args[] = {"krffs", image_path, "/unused", NULL};
    int result;

    mounted = 0;
    result = krffs_mount_main(3, args);
    check(valid ? result == 0 && mounted == 1 : result != 0 && mounted == 0, what);
    KRFFS_File_System = &fixture;
}

/** Cover every ring field used by an unchecked callback walk. */
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

/** Check a hole byte for byte, including the old free-node header. */
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

/** A file with an allocated neighbour keeps slack after shrinking. */
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

/** The minus-one ownership sentinel preserves the corresponding field. */
static void owner_rows(void)
{
    struct krffs_node *node;

    reset_image();
    check(krffs_create("/a", 0600, NULL) == 0, "create owned file");
    node = krffs_find_reserved_node_by_name(&fixture, "a");
    check(krffs_chown("/a", (uid_t)-1, 789) == 0 && node->uid == 123 && node->gid == 789,
          "group-only chown preserves uid");
    check(krffs_chown("/a", 321, (gid_t)-1) == 0 && node->uid == 321 && node->gid == 789,
          "owner-only chown preserves gid");
    check(krffs_chown("/a", (uid_t)-1, (gid_t)-1) == 0 && node->uid == 321 && node->gid == 789,
          "unchanged ownership");
}

/** Explicit times, omitted fields and current-time requests stay distinct. */
static void time_rows(void)
{
    struct timespec times[2] = {{946684800, 123}, {946684900, 456}};
    struct krffs_node *node;
    struct krffs_node_time before;

    reset_image();
    check(krffs_create("/a", 0600, NULL) == 0, "create timed file");
    node = krffs_find_reserved_node_by_name(&fixture, "a");
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

/** Run all callback rows and release the private backing file. */
int main(void)
{
    int fd = mkstemp(image_path);

    if (fd < 0) {
        return 2;
    }
    if (ftruncate(fd, 65536) != 0) {
        close(fd);
        unlink(image_path);
        return 2;
    }
    fixture.size = 65536;
    fixture.node = krffs_map_file(fd, 0, (size_t)fixture.size);
    close(fd);
    if (fixture.node == (void *)-1) {
        unlink(image_path);
        return 2;
    }
    ring_rows();
    hole_row(0);
    hole_row(1);
    slack_row();
    owner_rows();
    time_rows();
    krffs_unmap_file(fixture.node, (size_t)fixture.size);
    unlink(image_path);
    printf("krffs-fuse-test: result: %d failures\n", failures);
    return failures != 0;
}
