/* Load the DirectInput process-detach reproducer. */
#include <windows.h>
#include <stdio.h>

/**
 * Acquire through the DLL, then unload it if given an argument and exit.
 * Return the setup result, 10 if loading fails, or 11 if start is absent.
 */
int main(int argc, char **argv)
{
    HMODULE module = LoadLibraryA("dinput-probe.dll");
    int (*start)(void);
    int result;
    if (!module) return 10;
    start = (void *)GetProcAddress(module, "start");
    if (!start) return 11;
    result = start();
    printf("dinput-probe: started: %d\n", result);
    fflush(stdout);
    if (argc > 1) FreeLibrary(module);
    (void)argv;
    ExitProcess(result);
}
