/* Native directory operations shared by host fixtures. */
#ifndef RACH2_HOST_TREE_H
#define RACH2_HOST_TREE_H
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
/* The most characters of a wide path, its terminator among them. */
#define HOST_TREE_WIDE 32768

/**
 * Remove the file or tree at `path`, a wide path of the `\\?\` form in a
 * buffer of HOST_TREE_WIDE characters, which holds each entry's path in turn
 * and its own again on return; 0, or -1 when something is left.
 */
static inline int host_remove_tree_wide(wchar_t *path)
{
    WIN32_FIND_DATAW entry;
    DWORD attributes = GetFileAttributesW(path);
    size_t n = wcslen(path), name;
    HANDLE search;
    int failed = 0;
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(path) ? 0 : -1;
    }
    /* Remove reparse points as entries, without visiting their targets. */
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return RemoveDirectoryW(path) ? 0 : -1;
    if (n + 3 > HOST_TREE_WIDE)
        return -1;
    wcscpy(path + n, L"\\*");
    search = FindFirstFileW(path, &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L".."))
                continue;
            name = wcslen(entry.cFileName);
            if (n + name + 2 > HOST_TREE_WIDE) {
                failed = 1;
                continue;
            }
            wcscpy(path + n + 1, entry.cFileName);
            if (host_remove_tree_wide(path) != 0)
                failed = 1;
        } while (FindNextFileW(search, &entry));
        if (GetLastError() != ERROR_NO_MORE_FILES)
            failed = 1;
        FindClose(search);
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND)
        failed = 1;
    path[n] = L'\0';
    return failed ? -1 : RemoveDirectoryW(path) ? 0 : -1;
}
#endif

/**
 * Remove our own fixture tree without starting a shell. On Windows the path,
 * in the ANSI code page as the C runtime gives it, may pass 260 characters.
 */
static inline int host_remove_tree(const char *path)
{
#ifdef _WIN32
    wchar_t *wide = malloc((2 * HOST_TREE_WIDE + 8) * sizeof *wide), *full, *p;
    DWORD n;
    int rc = -1;
    if (wide == NULL)
        return -1;
    /* The full path goes after room for the longest prefix, \\?\UNC\. */
    full = wide + HOST_TREE_WIDE + 8;
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wide, HOST_TREE_WIDE) == 0) {
        free(wide);
        return -1;
    }
    for (p = wide; *p != L'\0'; p++)
        if (*p == L'/')
            *p = L'\\';
    if (wcsncmp(wide, L"\\\\?\\", 4) == 0)
        rc = host_remove_tree_wide(wide);
    else if ((n = GetFullPathNameW(wide, HOST_TREE_WIDE - 8, full, NULL)) > 0 &&
             n < HOST_TREE_WIDE - 8) {
        if (wcsncmp(full, L"\\\\", 2) == 0)
            p = memcpy(full - 6, L"\\\\?\\UNC", 7 * sizeof *p);
        else
            p = memcpy(full - 4, L"\\\\?\\", 4 * sizeof *p);
        rc = host_remove_tree_wide(p);
    }
    free(wide);
    return rc;
#else
    char child[4096];
    int failed = 0;
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
