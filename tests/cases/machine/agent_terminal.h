/* agent_terminal.h: recognize the acceptance shell's failed exit line. */
#ifndef AGENT_TERMINAL_H
#define AGENT_TERMINAL_H
#include <string.h>

static int agent_terminal_failed(const char *screen)
{
    const char prefix[] = "rach2-accept: ended with status ";
    const char *at = screen;

    while (at && *at) {
        const char *end = strchr(at, '\n');
        size_t n = end ? (size_t)(end - at) : strlen(at);
        unsigned status = 0;
        size_t i = sizeof(prefix) - 1;
        if (n && at[n - 1] == '\r')
            n--;
        if (n > i && !memcmp(at, prefix, i)) {
            for (; i < n && at[i] >= '0' && at[i] <= '9' && status <= 255; i++)
                status = status * 10 + (unsigned)(at[i] - '0');
            if (i == n && status > 0 && status <= 255)
                return (int)status;
        }
        at = end ? end + 1 : NULL;
    }
    return 0;
}
#endif
