/* Link a dynamic executable or a shared library with TinyCC's library, for
   the host check of an exported IFUNC, since tcc's command line refuses a
   link without -static and -shared. */
#include "libtcc.c"

/**
 * Link the files after OUTPUT into the dynamic executable OUTPUT, with
 * -nostdlib and -rdynamic, which exports every global symbol, or with a
 * first argument -shared into the shared library OUTPUT, with -nostdlib.
 *
 * Built with -DONE_SOURCE and the target's TCC_TARGET_ macro in the
 * patched source tree, as the Makefile builds i386-tcc and x86_64-tcc.
 * Returns 0 when the output is written, 1 when a file or the link fails,
 * and 2 on a wrong command line.
 */
int main(int argc, char **argv)
{
    TCCState *s;
    int i, status, shared;

    shared = argc > 1 && !strcmp(argv[1], "-shared");
    argv += shared;
    argc -= shared;
    if (argc < 3) {
        fprintf(stderr, "usage: dynlink [-shared] OUTPUT FILE...\n");
        return 2;
    }
    s = tcc_new();
    tcc_set_options(s, shared ? "-nostdlib" : "-nostdlib -rdynamic");
    tcc_set_output_type(s, shared ? TCC_OUTPUT_DLL : TCC_OUTPUT_EXE);
    status = 0;
    for (i = 2; i < argc && !status; i++) {
        /* tcc_add_file keeps the type of the first file for the rest,
           as tcc's main does not let it */
        s->filetype = 0;
        if (tcc_add_file(s, argv[i]) < 0)
            status = 1;
    }
    if (!status && tcc_output_file(s, argv[1]) < 0)
        status = 1;
    tcc_delete(s);
    return status;
}
