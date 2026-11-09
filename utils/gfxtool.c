/* Inspect a Haswell HD 4400 through /dev/mem: an unbound one, or one a
 * driver owns, read only. */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE   200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PCI_PATH      "/sys/bus/pci/devices/0000:00:02.0/"
#define MMIO_SIZE     0x400000u
#define APERTURE_SIZE 0x10000000u
#define FORCEWAKE_MT  0xA188u
#define FORCEWAKE_ACK 0x130044u

/* Linux 6.12: i915_reg.h, gt/intel_gt_regs.h, gt/intel_engine_regs.h,
 * display/i9xx_plane_regs.h and display/intel_pipe_crc_regs.h. */
struct reg_name {
    const char *name;
    uint32_t reg;
};
static const struct reg_name fixed[] = {{"HSW_PWR_WELL_CTL1", 0x45400},
                                        {"HSW_PWR_WELL_CTL2", 0x45404},
                                        {"HSW_PWR_WELL_CTL3", 0x45408},
                                        {"HSW_PWR_WELL_CTL4", 0x4540C},
                                        {"FORCEWAKE_MT", FORCEWAKE_MT},
                                        {"FORCEWAKE_ACK_HSW", FORCEWAKE_ACK},
                                        {"GEN6_GT_THREAD_STATUS_REG", 0x13805C},
                                        {"GFX_MODE", 0x2520},
                                        {"GAB_CTL", 0x24000},
                                        {"GAM_ECOCHK", 0x4090},
                                        {"BLC_PWM_CPU_CTL2", 0x48250},
                                        {"BLC_PWM_CPU_CTL", 0x48254},
                                        {"BLC_PWM_PCH_CTL1", 0xC8250},
                                        {"BLC_PWM_PCH_CTL2", 0xC8254},
                                        {"PP_STATUS", 0xC7200},
                                        {"PP_CONTROL", 0xC7204},
                                        {"PP_ON_DELAYS", 0xC7208},
                                        {"PP_OFF_DELAYS", 0xC720C},
                                        {"TRANS_DDI_FUNC_CTL_EDP", 0x6F400},
                                        {"TRANSCONF_EDP", 0x7F008},
                                        {"DDI_BUF_CTL_A", 0x64000},
                                        {"DDI_BUF_CTL_B", 0x64100},
                                        {"DDI_BUF_CTL_C", 0x64200},
                                        {"DDI_BUF_CTL_D", 0x64300},
                                        {"PORT_CLK_SEL_A", 0x46100},
                                        {"PORT_CLK_SEL_B", 0x46104},
                                        {"PORT_CLK_SEL_C", 0x46108},
                                        {"PORT_CLK_SEL_D", 0x4610C},
                                        {"FPGA_DBG", 0x42300},
                                        {"GTFIFODBG", 0x120000},
                                        {"GTFIFOCTL", 0x120008},
                                        {"GEN6_STOLEN_RESERVED", 0x1082C0},
                                        {"RENDER_HWS_PGA_GEN7", 0x4080},
                                        {"BLT_HWS_PGA_GEN7", 0x4280},
                                        {"DEIMR", 0x44004},
                                        {"DEIIR", 0x44008},
                                        {"DEIER", 0x4400C},
                                        {"GTIMR", 0x44014},
                                        {"GTIIR", 0x44018},
                                        {"GTIER", 0x4401C},
                                        {"GEN6_PMIMR", 0x44024},
                                        {"GEN6_PMIIR", 0x44028},
                                        {"GEN6_PMIER", 0x4402C},
                                        {"GEN7_ERR_INT", 0x44040},
                                        {"SDEIMR", 0xC4004},
                                        {"SDEIIR", 0xC4008},
                                        {"SDEIER", 0xC400C},
                                        {"SERR_INT", 0xC4040}};
static volatile uint32_t *mmio;
static int memfd = -1;
static int wake_changed, crc_changed;
static uint32_t crc_reg, crc_saved;
static volatile sig_atomic_t interrupted;
static uint32_t aperture;
static int owned;       /* a driver owns the GPU: nothing is written */
static char owner[160]; /* why the GPU counts as owned */

/** Read one aligned register of the known four-mebibyte BAR. */
static uint32_t rd(uint32_t reg)
{
    return mmio[reg / 4];
}

/** Write and post one register; callers restore every temporary change. */
static void wr(uint32_t reg, uint32_t value)
{
    mmio[reg / 4] = value;
    (void)rd(reg);
}

/** Restore temporary register changes and release all process resources. */
static void cleanup(void)
{
    if (mmio) {
        if (crc_changed) {
            wr(crc_reg, crc_saved);
        }
        if (wake_changed) {
            wr(FORCEWAKE_MT, 0x10000u);
        }
        munmap((void *)mmio, MMIO_SIZE);
    }
    if (memfd >= 0) {
        close(memfd);
    }
}

/** Defer a signal to the main path, whose exit restores the registers. */
static void stop(int number)
{
    (void)number;
    interrupted = 1;
}

/** Decode a little-endian configuration-space dword. */
static uint32_t le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
           (uint32_t)bytes[3] << 24;
}

/** Find a bound PCI function, by its driver link or uevent: -1, with the
 * reason in `owner`, when bound or when its state cannot be read, so that a
 * permission failure grants nothing. */
static int unbound(void)
{
    struct stat st;
    char text[4096];
    int fd;
    ssize_t n;

    if (lstat(PCI_PATH "driver", &st) == 0 || errno != ENOENT) {
        snprintf(owner, sizeof(owner), "a driver owns the GPU or its state cannot be read");
        return -1;
    }
    fd = open(PCI_PATH "uevent", O_RDONLY);
    if (fd < 0) {
        snprintf(owner, sizeof(owner), "the GPU's driver state cannot be read: %s",
                 strerror(errno));
        return -1;
    }
    n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n < 0 || n == (ssize_t)sizeof(text) - 1) {
        snprintf(owner, sizeof(owner), "the GPU's whole uevent cannot be read");
        return -1;
    }
    text[n] = '\0';
    if (!strncmp(text, "DRIVER=", 7) || strstr(text, "\nDRIVER=")) {
        snprintf(owner, sizeof(owner), "a driver owns the GPU");
        return -1;
    }
    return 0;
}

/** Find DRM ownership even on a kernel whose PCI sysfs lacks driver
 * links, as unbound() does. GET_UNIQUE identifies PCI devices without
 * taking DRM master. */
static int no_drm_owner(void)
{
    DIR *dir = opendir("/dev/dri");
    struct dirent *entry;
    int result = 0;

    if (!dir) {
        if (errno == ENOENT) {
            return 0;
        }
        fprintf(stderr, "gfxtool: cannot read DRM ownership: %s\n", strerror(errno));
        return -1;
    }
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        struct {
            size_t len;
            char *name;
        } unique;
        char path[256], name[128];
        int fd;

        if (strncmp(entry->d_name, "card", 4) && strncmp(entry->d_name, "renderD", 7)) {
            continue;
        }
        if (snprintf(path, sizeof(path), "/dev/dri/%s", entry->d_name) >= (int)sizeof(path)) {
            result = -1;
            break;
        }
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            result = -1;
            break;
        }
        memset(name, 0, sizeof(name));
        unique.len = sizeof(name) - 1;
        unique.name = name;
        if (ioctl(fd, _IOWR('d', 1, unique), &unique) < 0 || unique.len >= sizeof(name) ||
            !strcmp(name, "pci:0000:00:02.0") || !strcmp(name, "pci:0000:00:02.00")) {
            result = -1;
        }
        close(fd);
        if (result) {
            break;
        }
        errno = 0;
    }
    if (errno) {
        result = -1;
    }
    closedir(dir);
    if (result) {
        snprintf(owner, sizeof(owner), "DRM owns the GPU or ownership cannot be read");
    }
    return result;
}

/** Map only 8086:0a16 with enabled, below-4-GiB memory BARs. No register is
 * touched until identity and both ownership checks have passed; a GPU that
 * fails them is refused unless `reading`, which reads it and writes no
 * register. */
static int attach(int reading)
{
    unsigned char config[256];
    uint32_t bar, bar2;
    ssize_t n;
    int fd = open(PCI_PATH "config", O_RDONLY);
    void *map;

    if (fd < 0) {
        fprintf(stderr, "gfxtool: refused: no HD 4400 at 0000:00:02.0\n");
        return -1;
    }
    n = pread(fd, config, sizeof(config), 0);
    close(fd);
    if (n != sizeof(config) || le32(config) != 0x0A168086u || config[11] != 3) {
        fprintf(stderr, "gfxtool: refused: GPU is not 8086:0a16 (HD 4400)\n");
        return -1;
    }
    if (unbound() || no_drm_owner()) {
        if (!reading) {
            fprintf(stderr, "gfxtool: refused: %s\n", owner);
            return -1;
        }
        owned = 1;
        fprintf(stderr, "gfxtool: reading only, without forcewake: %s\n", owner);
    }
    bar = le32(config + 0x10);
    bar2 = le32(config + 0x18);
    if (!(config[4] & 2) || (bar & 7) != 4 || le32(config + 0x14) || (bar2 & 7) != 4 ||
        le32(config + 0x1C) || !(bar & ~0xFu) || !(bar2 & ~0xFu) || (bar & ~0xFu) > 0xFFC00000u ||
        (bar2 & ~0xFu) > 0xF0000000u || (config[0x62] & 6) != 2) {
        fprintf(stderr, "gfxtool: refused: disabled or unsupported GPU BAR layout\n");
        return -1;
    }
    aperture = bar2 & ~0xFu;
    /* An owned GPU is mapped for reading alone, so no store can reach it. */
    memfd = open("/dev/mem", (owned ? O_RDONLY : O_RDWR) | O_SYNC | O_CLOEXEC);
    if (memfd < 0) {
        fprintf(stderr, "gfxtool: cannot open /dev/mem: %s\n", strerror(errno));
        return -1;
    }
    map = mmap(NULL, MMIO_SIZE, owned ? PROT_READ : PROT_READ | PROT_WRITE, MAP_SHARED, memfd,
               bar & ~0xFu);
    if (map == MAP_FAILED) {
        fprintf(stderr, "gfxtool: cannot map GPU registers: %s\n", strerror(errno));
        return -1;
    }
    mmio = map;
    return 0;
}

/** Wait at most a second and at most 10000 reads for a register change;
 * a stopped counter, a signal or a read error ends it with -1. */
static int wait_reg(uint32_t reg, uint32_t mask, uint32_t value, int different)
{
    struct timespec start, now, delay = {0, 100000};
    unsigned tries;

    if (clock_gettime(CLOCK_MONOTONIC, &start)) {
        return -1;
    }
    for (tries = 0; tries < 10000 && !interrupted; tries++) {
        uint32_t got = rd(reg);

        if (got != 0xFFFFFFFFu && (((got & mask) != value) == different)) {
            return 0;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec - start.tv_sec > 1 ||
            (now.tv_sec - start.tv_sec == 1 && now.tv_nsec >= start.tv_nsec)) {
            break;
        }
        nanosleep(&delay, NULL);
    }
    fprintf(stderr, "gfxtool: register %06x did not change\n", reg);
    return -1;
}

/** Wake the GT for ring register reads, keeping a prior request in place. */
static int forcewake(void)
{
    uint32_t old = rd(FORCEWAKE_MT);

    if (old == 0xFFFFFFFFu) {
        return -1;
    }
    if (!(old & 1)) {
        wake_changed = 1;
        wr(FORCEWAKE_MT, 0x10001);
    }
    return wait_reg(FORCEWAKE_ACK, 1, 1, 0) || wait_reg(0x13805C, 7, 0, 0) ? -1 : 0;
}

/** Print one named register in the register dump's stable text format. */
static void print_reg(const char *name, uint32_t reg)
{
    printf("%-28s %06x %08x\n", name, reg, rd(reg));
}

/** Dump display, power and interrupt registers, GT ring state and the GGTT
 * entries of the CPU-visible aperture, without changing ring or display
 * state. An unbound GPU's ring reads are made under forcewake; an owned
 * GPU's are made as the GT is, since its driver keeps forcewake. */
static int dump(void)
{
    static const struct reg_name pipe_regs[] = {
        {"PIPESRC", 0x6001C},   {"PIPECONF", 0x70008},    {"PIPE_FRMCOUNT", 0x70040},
        {"DSPCNTR", 0x70180},   {"DSPSTRIDE", 0x70188},   {"DSPSURF", 0x7019C},
        {"DSPOFFSET", 0x701A4}, {"DSPSURFLIVE", 0x701AC}, {"PIPE_CRC_CTL", 0x60050}};
    static const struct reg_name rings[] = {
        {"RCS", 0x2000}, {"VCS", 0x12000}, {"BCS", 0x22000}, {"VECS", 0x1A000}};
    static const struct reg_name ring_regs[] = {
        {"TAIL", 0x30},  {"HEAD", 0x34},  {"START", 0x38},  {"CTL", 0x3C},     {"ACTHD", 0x74},
        {"IPEIR", 0x64}, {"IPEHR", 0x68}, {"HWSTAM", 0x98}, {"MI_MODE", 0x9C}, {"IMR", 0xA8}};
    unsigned i, j, pipe;
    char name[64];

    print_reg("FORCEWAKE_MT_SAVED", FORCEWAKE_MT);
    print_reg("FORCEWAKE_ACK_HSW_SAVED", FORCEWAKE_ACK);
    if (!owned && forcewake()) {
        return 1;
    }
    for (i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        print_reg(fixed[i].name, fixed[i].reg);
    }
    for (pipe = 0; pipe < 3; pipe++) {
        for (i = 0; i < sizeof(pipe_regs) / sizeof(pipe_regs[0]); i++) {
            snprintf(name, sizeof(name), "%s_%c", pipe_regs[i].name, 'A' + pipe);
            print_reg(name, pipe_regs[i].reg + pipe * 0x1000);
        }
    }
    for (i = 0; i < sizeof(rings) / sizeof(rings[0]); i++) {
        for (j = 0; j < sizeof(ring_regs) / sizeof(ring_regs[0]); j++) {
            snprintf(name, sizeof(name), "%s_%s", rings[i].name, ring_regs[j].name);
            print_reg(name, rings[i].reg + ring_regs[j].reg);
        }
    }
    for (i = 0; i < APERTURE_SIZE / 4096 && !interrupted; i++) {
        printf("GTT[%05x] %08x\n", i, rd(0x200000 + i * 4));
    }
    return interrupted || ferror(stdout);
}

/** Read CRCs after whole frames, discarding the first two as Linux does.
 * The exit handler restores PIPE_CRC_CTL on success, timeout or signal. */
static int crc(unsigned pipe, unsigned count)
{
    uint32_t frame_reg = 0x70040 + pipe * 0x1000, frame;
    unsigned i, j;

    if (pipe == 0 && (rd(0x6F400) & 0x80007000u) == 0x80000000u) {
        fprintf(stderr,
                "gfxtool: refused: the eDP route bypasses pipe A's CRC; a mode set is required\n");
        return 1;
    }
    if (!(rd(0x70180 + pipe * 0x1000) & 0x80000000u)) {
        fprintf(stderr, "gfxtool: the primary plane is disabled\n");
        return 1;
    }
    crc_reg = 0x60050 + pipe * 0x1000;
    crc_saved = rd(crc_reg);
    if (crc_saved & 0x80000000u) {
        fprintf(stderr, "gfxtool: refused: pipe CRC is already enabled\n");
        return 1;
    }
    crc_changed = 1;
    wr(crc_reg, 0xC0000000u);
    for (i = 0; i < count + 2; i++) {
        frame = rd(frame_reg);
        if (wait_reg(frame_reg, 0xFFFFFFFFu, frame, 1)) {
            return 1;
        }
        if (i >= 2) {
            uint32_t values[5], before = rd(frame_reg);

            for (j = 0; j < 5; j++) {
                values[j] = rd(0x60064 + pipe * 0x1000 + j * 4);
            }
            if (before != rd(frame_reg)) {
                fprintf(stderr, "gfxtool: a frame changed while reading its CRC\n");
                return 1;
            }
            printf("%08x %08x %08x %08x %08x %08x\n", before, values[0], values[1], values[2],
                   values[3], values[4]);
        }
    }
    return ferror(stdout) != 0;
}

/** Bytes per pixel of an enabled, linear, unrotated primary plane, or 0
 * for a format this capture cannot decode. */
static unsigned capture_bpp(uint32_t ctl)
{
    unsigned format = (ctl >> 26) & 15;

    if (!(ctl & 0x80000000u) || (ctl & ((1u << 10) | (1u << 15)))) {
        return 0;
    }
    if (format == 5) {
        return 2;
    }
    return format == 6 || format == 10 || format == 8 || format == 14 ? 4 : 0;
}

/** Convert one supported packed pixel to three eight-bit RGB bytes. */
static void pixel_rgb(const unsigned char *src, unsigned format, unsigned char *dst)
{
    uint32_t pixel = format == 5 ? src[0] | (uint32_t)src[1] << 8 : le32(src);
    unsigned red, green, blue;

    if (format == 5) {
        red = ((pixel >> 11) & 31) * 255 / 31;
        green = ((pixel >> 5) & 63) * 255 / 63;
        blue = (pixel & 31) * 255 / 31;
    } else if (format == 8 || format == 10) {
        red = ((pixel >> 20) & 1023) * 255 / 1023;
        green = ((pixel >> 10) & 1023) * 255 / 1023;
        blue = (pixel & 1023) * 255 / 1023;
    } else {
        red = (pixel >> 16) & 255;
        green = (pixel >> 8) & 255;
        blue = pixel & 255;
    }
    dst[0] = (unsigned char)((format == 8 || format == 14) ? blue : red);
    dst[1] = (unsigned char)green;
    dst[2] = (unsigned char)((format == 8 || format == 14) ? red : blue);
}

/** Capture a linear primary plane's pixels through the aperture, including
 * its source offset. Refuse tiled, rotated or unknown formats before output. */
static int capture(unsigned pipe, const char *path)
{
    uint32_t base = pipe * 0x1000, ctl = rd(0x70180 + base);
    uint32_t surface = rd(0x7019C + base) & 0xFFFFF000u;
    uint32_t stride = rd(0x70188 + base) & 0xFFFFFFC0u;
    uint32_t offset = rd(0x701A4 + base), size = rd(0x6001C + base);
    unsigned width = (size >> 16) + 1, height = (size & 0xFFFF) + 1;
    unsigned xoff = offset & 0xFFFF, yoff = offset >> 16;
    unsigned format = (ctl >> 26) & 15, bpp = capture_bpp(ctl);
    unsigned x, y;
    uint64_t first = (uint64_t)surface + (uint64_t)yoff * stride + xoff * bpp;
    uint64_t last = first + (uint64_t)(height - 1) * stride + width * bpp;
    unsigned char *row = NULL, *rgb = NULL;
    FILE *out = NULL;
    int result = 1;

    if (!bpp || !stride || width > 8192 || height > 8192 ||
        (uint64_t)(xoff + width) * bpp > stride || last > APERTURE_SIZE) {
        fprintf(stderr,
                "gfxtool: refused: disabled, tiled, rotated or unsupported primary plane\n");
        return 1;
    }
    row = malloc(width * bpp);
    rgb = malloc(width * 3);
    if (!row || !rgb || !(out = fopen(path, "wx"))) {
        fprintf(stderr, "gfxtool: cannot create capture: %s\n", strerror(errno));
        goto done;
    }
    if (fprintf(out, "P6\n%u %u\n255\n", width, height) < 0) {
        goto done;
    }
    for (y = 0; y < height && !interrupted; y++) {
        if (pread(memfd, row, width * bpp, (off_t)(aperture + first + (uint64_t)y * stride)) !=
            (ssize_t)(width * bpp)) {
            fprintf(stderr, "gfxtool: cannot read the scanout aperture: %s\n", strerror(errno));
            goto done;
        }
        for (x = 0; x < width; x++) {
            pixel_rgb(row + x * bpp, format, rgb + x * 3);
        }
        if (fwrite(rgb, 3, width, out) != width) {
            goto done;
        }
    }
    if (!interrupted && rd(0x70180 + base) == ctl &&
        (rd(0x7019C + base) & 0xFFFFF000u) == surface && rd(0x70188 + base) == stride &&
        rd(0x701A4 + base) == offset && rd(0x6001C + base) == size) {
        result = 0;
    } else {
        fprintf(stderr, "gfxtool: capture interrupted or scanout changed\n");
    }
done:
    if (out) {
        if (fclose(out)) {
            result = 1;
        }
        if (result) {
            unlink(path);
        }
    }
    free(row);
    free(rgb);
    return result;
}

/** Parse a command before accessing hardware; return dump (0), CRC (1),
 * capture (2), or -1 for invalid arguments, and fill its pipe and count. */
static int arguments(int argc, char **argv, unsigned *pipe, unsigned *count)
{
    char *end;
    unsigned long parsed;
    int mode;

    if (argc == 2 && !strcmp(argv[1], "dump")) {
        return 0;
    } else if (argc == 4 && (!strcmp(argv[1], "crc") || !strcmp(argv[1], "capture")) &&
               strlen(argv[2]) == 1 && argv[2][0] >= 'A' && argv[2][0] <= 'C') {
        mode = !strcmp(argv[1], "crc") ? 1 : 2;
        *pipe = (unsigned)(argv[2][0] - 'A');
        if (mode == 1) {
            errno = 0;
            parsed = strtoul(argv[3], &end, 10);
            if (errno || *end || argv[3][0] < '0' || argv[3][0] > '9' || !parsed ||
                parsed > 1000000) {
                return -1;
            }
            *count = (unsigned)parsed;
        }
    } else {
        return -1;
    }
    return mode;
}

/** Validate the command before opening hardware; restore registers at exit. */
int main(int argc, char **argv)
{
    struct sigaction action;
    unsigned pipe = 0, count = 0;
    int mode = arguments(argc, argv, &pipe, &count), result;

    if (mode < 0) {
        fprintf(stderr,
                "gfxtool: usage: gfxtool dump | crc A|B|C FRAMES | capture A|B|C FILE.ppm\n");
        return 2;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    if (atexit(cleanup) || sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL) ||
        sigaction(SIGHUP, &action, NULL) || sigaction(SIGPIPE, &action, NULL)) {
        fprintf(stderr, "gfxtool: cannot install cleanup\n");
        return 1;
    }
    if (attach(mode != 1)) {
        return 1;
    }
    result = mode == 0 ? dump() : mode == 1 ? crc(pipe, count) : capture(pipe, argv[3]);
    if (fflush(stdout)) {
        result = 1;
    }
    return result;
}
