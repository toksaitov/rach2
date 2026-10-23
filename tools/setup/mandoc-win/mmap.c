/* Read-only mappings used by the unchanged mandoc input reader. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <io.h>
#include "sys/mman.h"

void *mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset)
{
    HANDLE mapping;
    void *view;

    if (address || !length || protection != PROT_READ || flags != MAP_SHARED || offset) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    mapping = CreateFileMappingW((HANDLE)_get_osfhandle(fd), NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mapping) {
        errno = EIO;
        return MAP_FAILED;
    }
    view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, length);
    CloseHandle(mapping);
    if (!view) {
        errno = EIO;
        return MAP_FAILED;
    }
    return view;
}

int munmap(void *address, size_t length)
{
    (void)length;
    if (UnmapViewOfFile(address)) {
        return 0;
    }
    errno = EIO;
    return -1;
}
