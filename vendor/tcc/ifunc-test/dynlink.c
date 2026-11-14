/* Link a dynamic executable with TinyCC's library, for the host check of an
   exported IFUNC, since tcc's command line refuses a link without -static. */
#include "libtcc.c"

/**
 * Link the files argv[2] on into the dynamic executable argv[1], with
 * -nostdlib and -rdynamic, which exports every global symbol.
 *
 * Built with -DONE_SOURCE and the target's TCC_TARGET_ macro in the
 * patched source tree, as the Makefile builds i386-tcc and x86_64-tcc.
 * Returns 0 when the executable is written, 1 when a file or the link
 * fails, and 2 on a wrong command line.
 */
int main(int argc, char **argv)
{
    TCCState *s;
    int i, status;

    if (argc < 3) {
        fprintf(stderr, "usage: dynlink OUTPUT FILE...\n");
        return 2;
    }
    s = tcc_new();
    tcc_set_options(s, "-nostdlib -rdynamic");
    tcc_set_output_type(s, TCC_OUTPUT_EXE);
    status = 0;
    for (i = 2; i < argc && !status; i++)
        if (tcc_add_file(s, argv[i]) < 0)
            status = 1;
    if (!status && tcc_output_file(s, argv[1]) < 0)
        status = 1;
    tcc_delete(s);
    return status;
}
