/* Shared error reporting, the output file Proton passes to the probe and the
 * Unix half of Wine it runs on. */
#ifndef D3D_PROBE_H
#define D3D_PROBE_H
#include <string.h>

/**
 * Print the Unix half of Wine that the process runs on, read from its own map
 * through drive Z: i386 for an i386 program under Proton's default mode,
 * x86_64 for an x86-64 program or for an i386 one under WoW64, and none off
 * Wine.
 */
static void probe_unix(void)
{
    char line[1024];
    const char *side = "none";
    FILE *maps = fopen("Z:\\proc\\self\\maps", "r");

    if (maps != NULL) {
        while (fgets(line, sizeof line, maps) != NULL) {
            if (strstr(line, "/i386-unix/ntdll.so") != NULL)
                side = "i386";
            else if (strstr(line, "/x86_64-unix/ntdll.so") != NULL)
                side = "x86_64";
        }
        fclose(maps);
    }
    printf("wine-unix: %s\n", side);
}

/**
 * Redirect stdout, unbuffered, to the file PROBE_OUT names, if any, and print
 * the Unix half of Wine; stop if the file cannot be opened.
 */
static void probe_output(void)
{
    const char *path = getenv("PROBE_OUT");
    if (path != NULL && freopen(path, "w", stdout) == NULL) {
        perror("d3d: output");
        exit(1);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    probe_unix();
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
