/* Shared error reporting, the output file Proton passes to the probe and the
 * hold in which the runner reads the probe's process. */
#ifndef D3D_PROBE_H
#define D3D_PROBE_H

/**
 * Wait, when PROBE_HOLD names a file, until that file exists, having printed
 * "hold". The runner reads the probe's process meanwhile, its map and its
 * executable, since Wine's server opens a file for its clients and the probe
 * reading Z:\proc\self would read the server's.
 */
static void probe_hold(void)
{
    const char *path = getenv("PROBE_HOLD");

    if (path == NULL)
        return;
    puts("hold");
    while (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        Sleep(20);
}

/**
 * Redirect stdout, unbuffered, to the file PROBE_OUT names, if any, and hold
 * while PROBE_HOLD asks; stop if the file cannot be opened.
 */
static void probe_output(void)
{
    const char *path = getenv("PROBE_OUT");
    if (path != NULL && freopen(path, "w", stdout) == NULL) {
        perror("d3d: output");
        exit(1);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    probe_hold();
}

/** Stop with status 1 on a failed API call and report the HRESULT on stderr. */
static void check(HRESULT hr, const char *what)
{
    if (FAILED(hr)) {
        fprintf(stderr, "d3d: %s: hr=0x%08lx\n", what, (unsigned long)hr);
        exit(1);
    }
}
#endif
