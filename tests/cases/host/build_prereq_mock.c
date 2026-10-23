/* Native identity, privilege and PowerShell substitutes for prerequisite rows. */
#include "build_support.h"

static int log_line(const char *prefix, int argc, char **argv)
{
    FILE *out = fopen(bp(getenv("FIX"), "log"), "a");
    if (!out)
        return 0;
    fputs(prefix, out);
    for (int i = 0; i < argc; i++)
        fprintf(out, " %s", argv[i]);
    fputc('\n', out);
    return fclose(out) == 0;
}

static char *decode(const char *encoded)
{
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned value = 0;
    int bits = 0;
    size_t n = 0;
    unsigned char bytes[65536];
    for (const char *p = encoded; *p && *p != '='; p++) {
        const char *digit = strchr(digits, *p);
        if (!digit)
            return NULL;
        value = (value << 6) | (unsigned)(digit - digits);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n == sizeof(bytes))
                return NULL;
            bytes[n++] = (unsigned char)(value >> bits);
        }
    }
    if (n % 2)
        return NULL;
    char *text = calloc(n / 2 + 1, 1);
    if (!text)
        return NULL;
    for (size_t i = 0; i < n; i += 2) {
        if (bytes[i + 1]) {
            free(text);
            return NULL;
        }
        text[i / 2] = (char)bytes[i];
    }
    return text;
}

static int powershell(int argc, char **argv)
{
    char cwd[4096];
    if (argc != 3 || strcmp(argv[1], "-EncodedCommand") || !getcwd(cwd, sizeof(cwd)) ||
        strcmp(cwd, "/"))
        return 92;
    char *text = decode(argv[2]);
    if (!text)
        return 93;
    char *line = strchr(text, '\n');
    if (!line || !strstr(text, "function Runs(") || !strstr(text, "function Has(")) {
        free(text);
        return 94;
    }
    line++;
    char *end = strchr(line, '\n');
    if (end)
        *end = 0;
    int status = 95;
    if (strstr(line, "IsInRole"))
        status = fx_exists(bp(getenv("FIX"), "elevated")) ? 0 : 1;
    else if (!strncmp(line, "if (Have ", 9)) {
        char *endname = strchr(line + 9, ')');
        if (endname) {
            *endname = 0;
            status = fx_exists(bp(bp(getenv("FIX"), "have"), line + 9)) ? 0 : 1;
        }
    } else if (!strncmp(line, "Get ", 4)) {
        char *args[] = {line};
        status = log_line("pwsh", 1, args) && fx_text(bp(bp(getenv("FIX"), "have"), line + 4), "")
                     ? 0
                     : 96;
    }
    free(text);
    return status;
}

int build_prereq_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (strcmp(name, "id") && strcmp(name, "sudo") && strcmp(name, "pwsh"))
        return -1;
    const char *fix = getenv("FIX");
    if (!fix)
        return 90;
    if (!strcmp(name, "id")) {
        if (argc != 2)
            return 1;
        if (!strcmp(argv[1], "-u")) {
            puts(fx_exists(bp(fix, "root")) ? "0" : "1000");
            return 0;
        }
        if (!strcmp(argv[1], "-un")) {
            puts("fixuser");
            return 0;
        }
        return 1;
    }
    if (!strcmp(name, "pwsh"))
        return powershell(argc, argv);
    if (argc < 3 || strcmp(argv[1], "-n") || !fx_exists(bp(fix, "sudo"))) {
        fputs("sudo: a password is required\n", stderr);
        return 1;
    }
    if (argc == 3 && !strcmp(argv[2], "true"))
        return 0;
    if (!log_line("sudo", argc - 2, argv + 2))
        return 97;
    execvp(argv[2], argv + 2);
    return 98;
}
