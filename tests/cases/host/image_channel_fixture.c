/* image_channel_fixture.c: a compiled failing image-tool transport fixture. */
#include <stdio.h>
#include <string.h>

/** The image helper substitutes a bound raw disk and private output path.
 * Writing partial output before exit 7 tests copyback independently of a
 * real file system tool's treatment of an incomplete destination. */
int main(int argc, char **argv)
{
    FILE *out;
    if (argc != 5 || strcmp(argv[1], "-i") != 0 || strcmp(argv[3], "::/file") != 0)
        return 2;
    out = fopen(argv[4], "wb");
    if (out == NULL)
        return 3;
    if (fwrite("partial", 1, 7, out) != 7 || fclose(out) != 0)
        return 4;
    fputs("fixture stdout\n", stdout);
    fputs("fixture stderr\n", stderr);
    return 7;
}
