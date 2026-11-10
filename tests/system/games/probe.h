/* Shared output, checksum and window code of the Wine probes. */
#ifndef GAMES_PROBE_H
#define GAMES_PROBE_H

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

/* The side of the square every probe draws and reads back, in pixels. */
#define PROBE_SIZE 64

/** Send stdout, unbuffered, to the file PROBE_OUT names; stop on failure. */
static inline void probe_output(void)
{
    const char *path = getenv("PROBE_OUT");

    if (path != NULL && freopen(path, "w", stdout) == NULL) {
        perror("probe: output");
        exit(1);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
}

/**
 * Fold N bytes into a CRC-32 (zlib's polynomial, reflected), starting from
 * CRC, which is 0 for the first call. Returns the new CRC.
 */
static inline unsigned long probe_crc32(unsigned long crc, const unsigned char *p, size_t n)
{
    int k;

    crc = ~crc & 0xffffffffUL;
    while (n-- > 0) {
        crc ^= *p++;
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320UL & (0UL - (crc & 1)));
    }
    return ~crc & 0xffffffffUL;
}

/**
 * Make a visible top-level window of PROBE_SIZE square named TITLE, with its
 * own device context, so that the X server maps it before the probe draws.
 * Returns NULL on failure.
 */
static inline HWND probe_window(const char *title)
{
    WNDCLASSA wc;
    HWND w;
    MSG msg;

    memset(&wc, 0, sizeof wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = title;
    if (!RegisterClassA(&wc))
        return NULL;
    w = CreateWindowA(title, title, WS_POPUP | WS_VISIBLE, 0, 0, PROBE_SIZE, PROBE_SIZE, NULL, NULL,
                      wc.hInstance, NULL);
    if (w == NULL)
        return NULL;
    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return w;
}

/**
 * Print the checksum line of a readback: the CRC-32 of the R, G, B and A
 * bytes of each row from the top, and the centre and corner pixels, each
 * given as R G B A. NAME is the probe's name in the line.
 */
static inline void probe_report(const char *name, unsigned long crc, const unsigned char *centre,
                                const unsigned char *corner)
{
    printf("%s: pixels crc32 %08lx centre %d %d %d %d corner %d %d %d %d\n", name, crc, centre[0],
           centre[1], centre[2], centre[3], corner[0], corner[1], corner[2], corner[3]);
}
#endif
