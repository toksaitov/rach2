/**
 * Configure unix_io.c for the host alignment fixture.
 *
 * Use host types and headers, with threads and optional device operations
 * disabled. The 64-bit calls are stubs even on hosts without those names.
 */

#ifndef RACH2_E2FSPROGS_TEST_CONFIG_H
#define RACH2_E2FSPROGS_TEST_CONFIG_H

#define HAVE_UNISTD_H       1
#define HAVE_ERRNO_H        1
#define HAVE_SYS_TYPES_H    1
#define HAVE_SYS_STAT_H     1
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_SYS_IOCTL_H    1
#define SIZEOF_TIME_T       8
#define SIZEOF_LONG         __SIZEOF_LONG__

#ifdef ALIGNMENT_TEST_64
#define HAVE_PREAD64  1
#define HAVE_PWRITE64 1
#else
#define HAVE_PREAD  1
#define HAVE_PWRITE 1
#endif

#endif
