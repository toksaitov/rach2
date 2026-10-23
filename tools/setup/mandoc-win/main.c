/* Batch entry for the unchanged mandoc 1.14.6 lint and render libraries. */
#include "config.h"
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mandoc.h"
#include "mandoc_aux.h"
#include "roff.h"
#include "mandoc_parse.h"
#include "mandoc_xr.h"
#include "manconf.h"
#include "main.h"

struct batch {
    const char *os;
    enum { ASCII, HTML, LINT } mode;
    int warning, had_output;
    void *output;
    struct manoutput options;
};

/* No pager or tag file is opened in batch mode, as in upstream ARG_FILE. */
void term_tag_write(struct roff_node *node, size_t line)
{
    (void)node;
    (void)line;
}

static int arguments(struct batch *b, int argc, char **argv)
{
    int option;

    while ((option = getopt(argc, argv, "cI:T:W:")) != -1) {
        switch (option) {
        case 'c':
            break;
        case 'I':
            if (strncmp(optarg, "os=", 3) || b->os) {
                return 0;
            }
            b->os = optarg + 3;
            break;
        case 'T':
            if (!strcmp(optarg, "ascii")) {
                b->mode = ASCII;
            } else if (!strcmp(optarg, "html")) {
                b->mode = HTML;
            } else if (!strcmp(optarg, "lint")) {
                b->mode = LINT;
                mandoc_msg_setoutfile(stdout);
                mandoc_msg_setmin(MANDOCERR_BASE);
                b->warning = 0;
            } else {
                return 0;
            }
            break;
        case 'W':
            if (strcmp(optarg, "warning")) {
                return 0;
            }
            mandoc_msg_setmin(MANDOCERR_WARNING);
            b->warning = 1;
            break;
        default:
            return 0;
        }
    }
    /* Lower levels need an installed manual database for cross references. */
    return b->mode != LINT || b->warning;
}

static void render(struct batch *b, struct mparse *parser)
{
    struct roff_meta *meta;

    if (!b->output && b->mode != LINT) {
        b->output = b->mode == HTML ? html_alloc(&b->options) : ascii_alloc(&b->options);
    } else if (b->mode == HTML) {
        html_reset(b->output);
    }
    mandoc_xr_reset();
    meta = mparse_result(parser);
    b->had_output = 1;
    if (b->mode == HTML) {
        if (meta->macroset == MACROSET_MDOC) {
            html_mdoc(b->output, meta);
        } else if (meta->macroset == MACROSET_MAN) {
            html_man(b->output, meta);
        }
    } else if (b->mode == ASCII) {
        if (meta->macroset == MACROSET_MDOC) {
            terminal_mdoc(b->output, meta);
        } else if (meta->macroset == MACROSET_MAN) {
            terminal_man(b->output, meta);
        }
    }
}

static void file(struct batch *b, struct mparse *parser, const char *path)
{
    int fd;

    mandoc_msg_setinfilename(path);
    fd = path ? mparse_open(parser, path) : STDIN_FILENO;
    if (fd == -1) {
        mandoc_msg(MANDOCERR_BADARG_BAD, 0, 0, "%s", strerror(errno));
        return;
    }
    if (b->had_output) {
        mparse_reset(parser);
        if (b->mode == ASCII) {
            terminal_sepline(b->output);
        }
    }
    mparse_readfd(parser, fd, path ? path : "<stdin>");
    if (fd != STDIN_FILENO) {
        close(fd);
    }
    render(b, parser);
    if (ferror(stdout)) {
        mandoc_msg(MANDOCERR_WRITE, 0, 0, "%s", strerror(errno));
    }
    mandoc_msg_setinfilename(NULL);
}

int main(int argc, char **argv)
{
    struct batch b;
    struct mparse *parser;
    int i;

    setprogname("mandoc");
    _set_fmode(_O_BINARY);
    _setmode(STDIN_FILENO, _O_BINARY);
    _setmode(STDOUT_FILENO, _O_BINARY);
    _setmode(STDERR_FILENO, _O_BINARY);
    mandoc_msg_setoutfile(stderr);
    memset(&b, 0, sizeof(b));
    if (!arguments(&b, argc, argv)) {
        fputs("mandoc: batch port supports -Tascii, -Thtml, -Tlint -Wwarning, -Ios=name\n", stderr);
        return MANDOCLEVEL_BADARG;
    }
    mchars_alloc();
    parser = mparse_alloc(MPARSE_SO | MPARSE_UTF8 | MPARSE_LATIN1 | MPARSE_VALIDATE,
                          MANDOC_OS_OTHER, b.os);
    if (optind == argc) {
        file(&b, parser, NULL);
    }
    for (i = optind; i < argc; i++) {
        file(&b, parser, argv[i]);
    }
    if (b.output) {
        if (b.mode == HTML) {
            html_free(b.output);
        } else {
            ascii_free(b.output);
        }
    }
    mandoc_xr_free();
    mparse_free(parser);
    mchars_free();
    if (b.had_output && b.mode != LINT) {
        mandoc_msg_summary();
    }
    return mandoc_msg_getrc();
}
