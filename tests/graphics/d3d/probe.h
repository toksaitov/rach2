/* Shared error reporting and the output file Proton passes to the probe. */
#ifndef D3D_PROBE_H
#define D3D_PROBE_H

/** Redirect stdout when PROBE_OUT names a file; stop if it cannot be opened. */
static void probe_output(void)
{
    const char *path = getenv("PROBE_OUT");
    if (path != NULL && freopen(path, "w", stdout) == NULL) {
        perror("d3d: output");
        exit(1);
    }
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
