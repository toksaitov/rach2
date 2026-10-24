/* Host tools share the native Windows file and directory adapter. */
#ifndef RACH2_HOST_FILES_H
#define RACH2_HOST_FILES_H
#ifdef _WIN32
#include "etude/win/files.h"
/* The adapter distinguishes reparse points from files and directories. */
#ifndef S_ISLNK
#define S_ISLNK(mode) (((mode) & S_IFMT) == 0120000)
#endif
#endif
#endif
