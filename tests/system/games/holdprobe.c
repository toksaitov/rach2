/* holdprobe: report readiness and wait, so that the runner reads its maps. */
#include "probe.h"

/**
 * Print "hold: ready" with the process id, then wait until the file that
 * PROBE_GO names exists, polling every 10 ms. Returns 0, or 1 without
 * PROBE_GO.
 */
int main(void)
{
    const char *go = getenv("PROBE_GO");

    probe_output();
    if (go == NULL) {
        printf("hold: no PROBE_GO\n");
        return 1;
    }
    printf("hold: ready pid %lu pointer %u bits\n", GetCurrentProcessId(),
           (unsigned)(sizeof(void *) * 8));
    while (GetFileAttributesA(go) == INVALID_FILE_ATTRIBUTES)
        Sleep(10);
    puts("HOLD OK");
    return 0;
}
