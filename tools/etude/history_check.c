/* history_check.c: validate the records of a closed history stream. */
#include "include/etude/history.h"
#include <stdio.h>

/** Check shape and stream order; return 2 for invalid or lost history. */
int main(int argc, char **argv)
{
    uint64_t line;
    int status;
    if (argc != 3) {
        fprintf(stderr, "history_check: usage: history_check schema stream\n");
        return 2;
    }
    status = etude_history_read(argv[2], argv[1], NULL, NULL, &line);
    if (status) {
        fprintf(stderr, "history_check: %s at line %llu\n",
                status == 1 ? "lost final record" : "invalid stream", (unsigned long long)line);
        return 2;
    }
    printf("history_check: result: valid stream\n");
    return 0;
}
