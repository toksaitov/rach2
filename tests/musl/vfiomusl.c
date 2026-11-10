/**
 * vfiomusl: check VFIO's files as a program sees them: the container, the
 * groups' nodes and their entries of /sys.
 *
 * The program opens /dev/vfio/vfio, 10:196, and checks the API version,
 * the Type1 extensions and the refusal of SET_IOMMU and MAP_DMA in a
 * container with no group. For each group of /sys/kernel/iommu_groups it
 * checks /dev/vfio/N, a character device that opens and answers
 * GET_STATUS; the group's type, identity; its reserved_regions, each line
 * a start, an end and a kind, with the message window among them; and each
 * device's iommu_group link back to the group and its driver_override.
 * vfio-pci's bind and drivers_probe must be there. With no IOMMU the
 * container and vfio-pci are there and no group is, as on Linux.
 *
 * It prints "VFIOMUSL OK (N groups)" and exits 0, or "VFIOMUSL FAIL
 * (detail)" and exits 1 at the first failure. It binds nothing.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/* The ioctls of linux/vfio.h this uses; the port has no such header. */
#define VFIO_GET_API_VERSION  0x3B64
#define VFIO_CHECK_EXTENSION  0x3B65
#define VFIO_SET_IOMMU        0x3B66
#define VFIO_GROUP_GET_STATUS 0x3B67
#define VFIO_IOMMU_MAP_DMA    0x3B71
#define VFIO_TYPE1_IOMMU      1
#define VFIO_TYPE1v2_IOMMU    3
#define VFIO_NOIOMMU_IOMMU    8

/* The arguments are words: vfio_group_status is argsz and flags, and
 * vfio_iommu_type1_dma_map is argsz and flags in its first eight bytes,
 * little-endian, then vaddr, iova and size. */

/** Print the failure `detail` about `what` and return 1. */
static int fail(const char *detail, const char *what)
{
    printf("VFIOMUSL FAIL (%s: %s: errno %d)\n", detail, what, errno);
    return 1;
}

/** Read the file `path` into `buf`, NUL-terminated: its length, or -1. */
static int slurp(const char *path, char *buf, int cap)
{
    int fd = open(path, O_RDONLY), n;

    if (fd < 0) {
        return -1;
    }
    n = (int)read(fd, buf, (size_t)cap - 1);
    close(fd);
    if (n >= 0) {
        buf[n] = '\0';
    }
    return n;
}

/** The container's rows. */
static int container_rows(void)
{
    unsigned long long map[4] = {32ull | 3ull << 32, 0x10000, 0x100000, 0x1000};
    struct stat st;
    int fd = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);

    if (fd < 0 || fstat(fd, &st) != 0) {
        return fail("open", "/dev/vfio/vfio");
    }
    if (!S_ISCHR(st.st_mode) || major(st.st_rdev) != 10 || minor(st.st_rdev) != 196) {
        return fail("not the character device 10:196", "/dev/vfio/vfio");
    }
    if (ioctl(fd, VFIO_GET_API_VERSION, 0) != 0) {
        return fail("API version not 0", "/dev/vfio/vfio");
    }
    if (ioctl(fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) != 1 ||
        ioctl(fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1v2_IOMMU) != 1 ||
        ioctl(fd, VFIO_CHECK_EXTENSION, VFIO_NOIOMMU_IOMMU) != 0) {
        return fail("the extensions are not Type1's", "/dev/vfio/vfio");
    }
    if (ioctl(fd, VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU) != -1 || errno != EINVAL) {
        return fail("SET_IOMMU with no group is not EINVAL", "/dev/vfio/vfio");
    }
    if (ioctl(fd, VFIO_IOMMU_MAP_DMA, map) != -1 || errno != EINVAL) {
        return fail("MAP_DMA with no IOMMU set is not EINVAL", "/dev/vfio/vfio");
    }
    close(fd);
    return 0;
}

/** The reserved_regions of the group directory `dir`: well formed, with
 * the message window. */
static int regions_rows(const char *dir)
{
    char path[400], buf[2048], kind[32];
    unsigned long long start, end;
    const char *line;
    int msi = 0;

    snprintf(path, sizeof(path), "%s/reserved_regions", dir);
    if (slurp(path, buf, sizeof(buf)) < 0) {
        return fail("read", path);
    }
    for (line = buf; *line != '\0'; line = strchr(line, '\n') + 1) {
        if (sscanf(line, "0x%llx 0x%llx %31s", &start, &end, kind) != 3 || start > end ||
            (strcmp(kind, "direct") != 0 && strcmp(kind, "direct-relaxable") != 0 &&
             strcmp(kind, "msi") != 0) ||
            strchr(line, '\n') == NULL) {
            return fail("a line is not START END KIND", path);
        }
        msi += strcmp(kind, "msi") == 0 && start == 0xFEE00000ull && end == 0xFEEFFFFFull;
    }
    return msi == 1 ? 0 : fail("no message window", path);
}

/** The devices of the group `name` at `dir`: each one's iommu_group link
 * names the group, and its driver_override reads as Linux's. */
static int devices_rows(const char *dir, const char *name)
{
    char path[400], link[320], buf[64];
    struct dirent *e;
    DIR *d;
    int n = 0;
    ssize_t len;

    snprintf(path, sizeof(path), "%s/devices", dir);
    d = opendir(path);
    if (d == NULL) {
        return fail("opendir", path);
    }
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", e->d_name);
        len = readlink(path, link, sizeof(link) - 1);
        if (len <= 0) {
            closedir(d);
            return fail("readlink", path);
        }
        link[len] = '\0';
        if (strrchr(link, '/') == NULL || strcmp(strrchr(link, '/') + 1, name) != 0) {
            closedir(d);
            return fail("the link names another group", path);
        }
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/driver_override", e->d_name);
        if (slurp(path, buf, sizeof(buf)) < 0 ||
            (strcmp(buf, "(null)\n") != 0 && strcmp(buf, "vfio-pci\n") != 0)) {
            closedir(d);
            return fail("driver_override is neither (null) nor vfio-pci", path);
        }
        n++;
    }
    closedir(d);
    return n > 0 ? 0 : fail("a group with no device", dir);
}

/** The rows of the group `name`. */
static int group_rows(const char *name)
{
    unsigned int gs[2] = {8, 0};
    char dir[320], path[400], buf[64];
    struct stat st;
    int fd;

    snprintf(dir, sizeof(dir), "/sys/kernel/iommu_groups/%s", name);
    snprintf(path, sizeof(path), "%s/type", dir);
    if (slurp(path, buf, sizeof(buf)) < 0 || strcmp(buf, "identity\n") != 0) {
        return fail("type is not identity", path);
    }
    if (regions_rows(dir) != 0 || devices_rows(dir, name) != 0) {
        return 1;
    }
    snprintf(path, sizeof(path), "/dev/vfio/%s", name);
    if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode)) {
        return fail("no character device", path);
    }
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return errno == EBUSY ? 0 : fail("open", path);
    }
    if (ioctl(fd, VFIO_GROUP_GET_STATUS, gs) != 0 || (gs[1] & ~3u) != 0) {
        close(fd);
        return fail("GET_STATUS", path);
    }
    close(fd);
    return 0;
}

int main(void)
{
    struct stat st;
    struct dirent *e;
    DIR *d;
    int groups = 0;

    if (container_rows() != 0) {
        return 1;
    }
    if (stat("/sys/bus/pci/drivers/vfio-pci/bind", &st) != 0 ||
        stat("/sys/bus/pci/drivers_probe", &st) != 0) {
        return fail("stat", "vfio-pci's bind or drivers_probe");
    }
    d = opendir("/sys/kernel/iommu_groups");
    if (d == NULL) {
        return fail("opendir", "/sys/kernel/iommu_groups");
    }
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        if (group_rows(e->d_name) != 0) {
            closedir(d);
            return 1;
        }
        groups++;
    }
    closedir(d);
    printf("VFIOMUSL OK (%d groups)\n", groups);
    return 0;
}
