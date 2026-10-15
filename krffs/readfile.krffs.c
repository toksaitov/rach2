/*
 * readfile.krffs.c: readfile.krffs, which copies one file out of a KRFFS
 * image.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "krffs_file_system.h"
#include "krffs_node.h"
#include "krffs_platform.h"
#include "krffs_reader.h"
#ifdef WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

/**
 * readfile.krffs IMAGE GUEST_PATH OUTPUT [--offset BYTES --size BYTES]:
 * copies the regular file at the absolute GUEST_PATH in the KRFFS image to
 * the host file OUTPUT and prints its size. The suites use it to take out a
 * file the guest wrote, such as the kernel `tests/run selfbuild` builds.
 *
 * The image is mapped for reading only, and the file is found with
 * `krffs_read_regular`, which fails on a damaged image with a reason and does
 * not read out of bounds. An OUTPUT that is the image itself, by device and
 * inode, is an error, since `fopen` with "wb" would empty the image before
 * the copy, and so is an argument after OUTPUT other than the region
 * options. Returns `EXIT_SUCCESS`, or `EXIT_FAILURE` with the reason on
 * stderr.
 */
int main(int argc, char **argv)
{
    int exit_status = EXIT_SUCCESS;

    struct krffs_file_system file_system = {NULL, 0};
    uint8_t *data = NULL;
    uint64_t size = 0;
    char reason[256];
    int file_descriptor = -1;
    off_t region_offset = 0;
    size_t region_size = 0;
    FILE *output = NULL;
    int i;

    if (argc < 4) {
        fprintf(stderr, "Usage: %s <image> <guest path> <output> [--offset N --size N]\n",
                argc >= 1 ? argv[0] : "readfile.krffs");
        return EXIT_FAILURE;
    }
    for (i = 4; i < argc; ++i) {
        if (strcmp(argv[i], "--offset") == 0 || strcmp(argv[i], "--size") == 0) {
            ++i;
        } else {
            fprintf(stderr, "readfile.krffs: unknown option '%s'\n", argv[i]);
            return EXIT_FAILURE;
        }
    }

    if ((file_descriptor = PLATFORM_PREFIX(open(argv[1], O_RDONLY))) == -1) {
        fprintf(stderr, "Failed to open the file system file at '%s'.\n", argv[1]);
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }

    struct stat file_info;
    if (fstat(file_descriptor, &file_info) == -1 || !S_ISREG(file_info.st_mode)) {
        fprintf(stderr, "The file system file at '%s' is missing or not a regular file.\n",
                argv[1]);
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }

    if (krffs_parse_region_args(argc, argv, (uint64_t)file_info.st_size,
                                sizeof(*file_system.node) * 2, &region_offset, &region_size) != 0) {
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }

    file_system.size = (off_t)region_size;
    file_system.node = krffs_map_file_readonly(file_descriptor, region_offset, region_size);
    if (file_system.node == NULL || file_system.node == (void *)-1) {
        fprintf(stderr, "Failed to map the file system file at '%s'.\n", argv[1]);
        file_system.node = NULL;
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }

    if (krffs_read_regular(&file_system, argv[2], &data, &size, reason, sizeof(reason)) != 0) {
        fprintf(stderr, "readfile.krffs: %s: %s\n", argv[2], reason);
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }

    {
        struct stat output_info;

        if (stat(argv[3], &output_info) == 0 && output_info.st_dev == file_info.st_dev &&
            output_info.st_ino == file_info.st_ino) {
            fprintf(stderr, "readfile.krffs: '%s' is the image itself\n", argv[3]);
            exit_status = EXIT_FAILURE;
            goto cleanup;
        }
    }

    output = fopen(argv[3], "wb");
    if (output == NULL || (size != 0 && fwrite(data, 1, (size_t)size, output) != size) ||
        fclose(output) != 0) {
        fprintf(stderr, "readfile.krffs: cannot write '%s': %s\n", argv[3], strerror(errno));
        output = NULL;
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }

    printf("readfile.krffs: ok: %llu bytes of %s\n", (unsigned long long)size, argv[2]);

cleanup:
    if (file_system.node != NULL) {
        krffs_unmap_file(file_system.node, file_system.size);
    }
    if (file_descriptor != -1) {
        PLATFORM_PREFIX(close(file_descriptor));
    }

    return exit_status;
}
