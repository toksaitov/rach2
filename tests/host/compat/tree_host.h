/* Native directory operations shared by host fixtures. */
#ifndef RACH2_HOST_TREE_H
#define RACH2_HOST_TREE_H
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* Remove our own fixture tree without starting a shell. */
static inline int host_remove_tree(const char *path)
{
    char child[4096];
    int failed = 0;
#ifdef _WIN32
    WIN32_FIND_DATAA entry;
    DWORD attributes = GetFileAttributesA(path);
    HANDLE search;
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
        return DeleteFileA(path) ? 0 : -1;
    }
    /* Reparse points are removed as entries; their targets are never visited. */
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return RemoveDirectoryA(path) ? 0 : -1;
    if (snprintf(child, sizeof(child), "%s/*", path) >= (int)sizeof(child))
        return -1;
    search = FindFirstFileA(child, &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(entry.cFileName, ".") || !strcmp(entry.cFileName, ".."))
                continue;
            if (snprintf(child, sizeof(child), "%s/%s", path, entry.cFileName) >=
                    (int)sizeof(child) ||
                host_remove_tree(child) != 0)
                failed = 1;
        } while (FindNextFileA(search, &entry));
        if (GetLastError() != ERROR_NO_MORE_FILES)
            failed = 1;
        FindClose(search);
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND)
        failed = 1;
    return failed ? -1 : RemoveDirectoryA(path) ? 0 : -1;
#else
    struct stat st;
    DIR *directory;
    struct dirent *entry;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    directory = opendir(path);
    if (directory == NULL)
        return -1;
    while ((entry = readdir(directory)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child) ||
            host_remove_tree(child) != 0)
            failed = 1;
    }
    closedir(directory);
    return failed ? -1 : rmdir(path);
#endif
}

#endif
