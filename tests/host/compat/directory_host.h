/* Count actual directory entries without invoking a command interpreter. */
#ifndef RACH2_HOST_DIRECTORY_H
#define RACH2_HOST_DIRECTORY_H
#include <string.h>
#ifdef _WIN32
#include <stdio.h>
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#endif

/* Return -1 on an enumeration error, otherwise the matching entry count. */
static inline int host_directory_count(const char *path, const char *contains)
{
    int count = 0;
#ifdef _WIN32
    char pattern[4096];
    WIN32_FIND_DATAA entry;
    HANDLE search;
    DWORD error;
    if (snprintf(pattern, sizeof pattern, "%s/*", path) >= (int)sizeof pattern)
        return -1;
    search = FindFirstFileA(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    do {
        if (strcmp(entry.cFileName, ".") && strcmp(entry.cFileName, "..") &&
            (contains == NULL || strstr(entry.cFileName, contains) != NULL))
            count++;
    } while (FindNextFileA(search, &entry));
    error = GetLastError();
    FindClose(search);
    return error == ERROR_NO_MORE_FILES ? count : -1;
#else
    DIR *directory = opendir(path);
    struct dirent *entry;
    int error;
    if (directory == NULL)
        return -1;
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..") &&
            (contains == NULL || strstr(entry->d_name, contains) != NULL))
            count++;
    }
    error = errno;
    if (closedir(directory) != 0)
        return -1;
    return error == 0 ? count : -1;
#endif
}
#endif
