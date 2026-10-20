/*
 * winsys.c: what etude's Windows adapter needs of the system besides
 * processes: error messages, UTF-8 paths of any length, clocks and loopback
 * sockets.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "winexec.h"
#include "winfmt.h"

void etude_win_strerror(DWORD err, char *buf, size_t size)
{
    DWORD n;

    n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err,
                       MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), buf, (DWORD)size, NULL);
    if (n == 0)
        n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0,
                           buf, (DWORD)size, NULL);
    if (n == 0) {
        snprintf(buf, size, "error %lu", (unsigned long)err);
        return;
    }
    while (n > 0 &&
           (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == '.' || buf[n - 1] == ' '))
        buf[--n] = '\0';
}

wchar_t *etude_win_wide(const char *s)
{
    wchar_t *w;
    int n;

    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    w = malloc((size_t)n * sizeof *w);
    if (w == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) {
        free(w);
        return NULL;
    }
    return w;
}

char *etude_win_utf8(const wchar_t *s)
{
    char *u;
    int n;

    n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    u = malloc((size_t)n);
    if (u == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, u, n, NULL, NULL) != n) {
        free(u);
        return NULL;
    }
    return u;
}

/** Return a new string of PREFIX and then REST, from malloc. */
static wchar_t *join(const wchar_t *prefix, const wchar_t *rest)
{
    size_t a = wcslen(prefix);
    size_t b = wcslen(rest);
    wchar_t *w;

    w = malloc((a + b + 1) * sizeof *w);
    if (w == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    memcpy(w, prefix, a * sizeof *w);
    memcpy(w + a, rest, (b + 1) * sizeof *w);
    return w;
}

wchar_t *etude_win_long_path(const char *path)
{
    wchar_t *w;
    wchar_t *full = NULL;
    wchar_t *res = NULL;
    wchar_t *p;
    DWORD n;

    w = etude_win_wide(path);
    if (w == NULL)
        return NULL;
    for (p = w; *p != L'\0'; p++) {
        if (*p == L'/')
            *p = L'\\';
    }
    if (wcsncmp(w, L"\\\\?\\", 4) == 0)
        return w;
    n = GetFullPathNameW(w, 0, NULL, NULL);
    if (n == 0)
        goto out;
    full = malloc(n * sizeof *full);
    if (full == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        goto out;
    }
    if (GetFullPathNameW(w, n, full, NULL) >= n)
        goto out;
    if (wcsncmp(full, L"\\\\", 2) == 0)
        res = join(L"\\\\?\\UNC\\", full + 2);
    else
        res = join(L"\\\\?\\", full);
out:
    free(full);
    free(w);
    return res;
}

/** The performance counter's ticks a second, read once. */
static uint64_t counter_freq(void)
{
    static uint64_t freq;
    LARGE_INTEGER f;

    if (freq == 0) {
        QueryPerformanceFrequency(&f);
        freq = (uint64_t)f.QuadPart;
    }
    return freq;
}

uint64_t etude_win_mono_us(void)
{
    LARGE_INTEGER c;

    QueryPerformanceCounter(&c);
    return etude_win_ticks_us((uint64_t)c.QuadPart, counter_freq());
}

uint64_t etude_win_mono_resolution_ns(void)
{
    uint64_t ns = 1000000000u / counter_freq();

    return ns > 0 ? ns : 1;
}

int64_t etude_win_utc_us(void)
{
    FILETIME ft;

    GetSystemTimePreciseAsFileTime(&ft);
    return etude_win_filetime_unix_us((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime);
}

int etude_win_net_start(void)
{
    static int started;
    WSADATA d;
    int err;

    if (started)
        return 0;
    err = WSAStartup(MAKEWORD(2, 2), &d);
    if (err == 0)
        started = 1;
    return err;
}

/** Make a TCP socket that no child inherits. */
static SOCKET new_socket(void)
{
    return WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                      WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}

/** Close S, keeping the error that came before. */
static void close_socket(SOCKET s)
{
    int err = WSAGetLastError();

    closesocket(s);
    WSASetLastError(err);
}

/**
 * Wait up to TIMEOUT milliseconds for S to be ready: to read when READ is
 * set, to write otherwise.
 *
 * @return 0, or -1 with WSAETIMEDOUT or the error in WSAGetLastError.
 */
static int wait_socket(SOCKET s, int read, DWORD timeout)
{
    fd_set rw;
    fd_set ex;
    struct timeval tv;
    int n;

    FD_ZERO(&rw);
    FD_ZERO(&ex);
    FD_SET(s, &rw);
    FD_SET(s, &ex);
    tv.tv_sec = (long)(timeout / 1000);
    tv.tv_usec = (long)(timeout % 1000 * 1000);
    n = select(0, read ? &rw : NULL, read ? NULL : &rw, &ex, &tv);
    if (n == SOCKET_ERROR)
        return -1;
    if (n == 0) {
        WSASetLastError(WSAETIMEDOUT);
        return -1;
    }
    return 0;
}

SOCKET etude_win_listen(unsigned short *port)
{
    struct sockaddr_in a;
    int len = sizeof a;
    BOOL on = TRUE;
    SOCKET s;

    s = new_socket();
    if (s == INVALID_SOCKET)
        return s;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof on) != 0 ||
        bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, SOMAXCONN) != 0 ||
        getsockname(s, (struct sockaddr *)&a, &len) != 0) {
        close_socket(s);
        return INVALID_SOCKET;
    }
    *port = ntohs(a.sin_port);
    return s;
}

SOCKET etude_win_accept(SOCKET listener, DWORD timeout)
{
    SOCKET s;

    if (wait_socket(listener, 1, timeout) != 0)
        return INVALID_SOCKET;
    s = accept(listener, NULL, NULL);
    if (s == INVALID_SOCKET)
        return s;
    if (!SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0)) {
        closesocket(s);
        WSASetLastError(WSAEINVAL);
        return INVALID_SOCKET;
    }
    return s;
}

SOCKET etude_win_connect(unsigned short port, DWORD timeout)
{
    struct sockaddr_in a;
    u_long nonblock = 1;
    int soerr = 0;
    int len = sizeof soerr;
    SOCKET s;

    s = new_socket();
    if (s == INVALID_SOCKET)
        return s;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (ioctlsocket(s, FIONBIO, &nonblock) != 0)
        goto fail;
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK || wait_socket(s, 0, timeout) != 0)
            goto fail;
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &len) != 0)
            goto fail;
        if (soerr != 0) {
            WSASetLastError(soerr);
            goto fail;
        }
    }
    nonblock = 0;
    if (ioctlsocket(s, FIONBIO, &nonblock) != 0)
        goto fail;
    return s;
fail:
    close_socket(s);
    return INVALID_SOCKET;
}
