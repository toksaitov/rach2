/*
 * etude_remote.c: etude-remote, the responder of remote execution on Linux
 * and macOS. A controller runs it through ssh(1) without a terminal and
 * speaks frames on its standard input and output.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../json.h"
#include "etude/remote.h"

/** Print the usage and return 2. */
static int usage(void)
{
    fputs("usage: etude-remote --stdio -d DIR [-b BOOT] [-m MAXFRAME]\n", stderr);
    return 2;
}

int main(int argc, char **argv)
{
    struct etude_serve_conf conf;
    char boot[ETUDE_FRAME_TOKEN_LEN + 1];
    int i, stdio = 0;

    json_program = "etude-remote";
    memset(&conf, 0, sizeof(conf));
    conf.in = 0;
    conf.out = 1;
    conf.log = stderr;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stdio") == 0)
            stdio = 1;
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            conf.state = argv[++i];
        else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
            conf.boot = argv[++i];
        else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
            conf.max_frame = (size_t)strtoul(argv[++i], NULL, 10);
        else
            return usage();
    }
    if (!stdio || conf.state == NULL)
        return usage();
    if (conf.boot == NULL) {
        if (etude_boot_token(boot) != 0) {
            fputs("etude-remote: this system gives no boot identity; give one with -b\n", stderr);
            return 2;
        }
        conf.boot = boot;
    }
    return etude_serve(&conf);
}
