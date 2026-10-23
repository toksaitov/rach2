/* Compiled observer, preparation and collection roles for the Windows owner. */
#ifdef _WIN32
#include "observe_sequence.h"
#include "win/winexec.h"

static int sequence_fixture_observer(int argc, char **argv)
{
    const char *trace_path = NULL, *stop = NULL, *run = NULL;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-o"))
            trace_path = argv[i + 1];
        if (!strcmp(argv[i], "-s"))
            stop = argv[i + 1];
        if (!strcmp(argv[i], "-r"))
            run = argv[i + 1];
    }
    if (!trace_path || !stop || !run)
        return 2;
    FILE *f = fopen(trace_path, "wb");
    if (!f)
        return 2;
    fputs("start\n", f);
    fflush(f);
    if (!strcmp(getenv("SEQUENCE_FIXTURE_MODE"), "observer-failure")) {
        fclose(f);
        return 8;
    }
    puts("etude observe: ready");
    fflush(stdout);
    int status = 9;
    for (;;) {
        DWORD available;
        if (!PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), NULL, 0, NULL, &available, NULL) ||
            available)
            break;
        FILE *request = fopen(stop, "rb");
        if (request) {
            char text[256];
            size_t n = fread(text, 1, sizeof(text), request);
            int ok = !ferror(request) && n == strlen(run) + 1 && !memcmp(text, run, n - 1) &&
                     text[n - 1] == '\n';
            fclose(request);
            if (ok) {
                fputs("end\n", f);
                status = 0;
            }
            break;
        }
        Sleep(10); /* interval: poll the fixture stop request every 10 ms */
    }
    if (fclose(f))
        status = 9;
    return status;
}

/* Return -1 for the ordinary host-test entry. */
static int sequence_fixture_role(int argc, char **argv)
{
    if (argc < 2)
        return -1;
    if (!strcmp(argv[1], "observe")) {
        if (argc > 2 && !strcmp(argv[2], "sequence"))
            return etude_observe_sequence_main(argc - 1, argv + 1);
        return sequence_fixture_observer(argc, argv);
    }
    if (!strcmp(argv[1], "admit"))
        return !strcmp(getenv("SEQUENCE_FIXTURE_MODE"), "preparation-failure") ? 7 : 0;
    if (!strcmp(argv[1], "run") && argc == 4) {
        FILE *f = fopen(argv[2], "wb");
        if (!f)
            return 2;
        int ok = fprintf(f, "%s\n", argv[3]) > 0;
        return fclose(f) || !ok ? 2 : 0;
    }
    return -1;
}

static char *sequence_fixture_read(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *bytes = calloc(65536, 1);
    if (!f || !bytes) {
        if (f)
            fclose(f);
        free(bytes);
        return NULL;
    }
    size_t n = fread(bytes, 1, 65535, f);
    int bad = ferror(f) || !feof(f);
    bytes[n] = 0;
    if (fclose(f) || bad) {
        free(bytes);
        return NULL;
    }
    return bytes;
}

static void sequence_fixture_row(const char *mode, unsigned expected)
{
    char self[32768], receipt[1200], trace_path[1200], stop[1200];
    CHECK(GetModuleFileNameA(NULL, self, sizeof(self)) > 0);
    snprintf(receipt, sizeof(receipt), "%s/%s.receipt", fix_dir(), mode);
    snprintf(trace_path, sizeof(trace_path), "%s/%s.trace", fix_dir(), mode);
    snprintf(stop, sizeof(stop), "%s/%s.stop", fix_dir(), mode);
    CHECK(_putenv_s("SEQUENCE_FIXTURE_MODE", mode) == 0);
    const char *path = getenv("PATH");
    const char *args[] = {self, "observe", "sequence", receipt, "fixture", trace_path, stop,
                          self, path,      path,       self,    "2",       self,       "admit",
                          self, "4",       self,       "run",   stop,      "fixture",  NULL};
    struct etude_win_sup sup;
    struct etude_win_spawn spawn = {0};
    struct etude_win_child *child = NULL;
    DWORD error = etude_win_sup_open(&sup);
    CHECK(error == 0);
    if (error)
        return;
    spawn.program = self;
    spawn.argv = args;
    error = etude_win_start(&sup, &spawn, &child);
    CHECK(error == 0);
    if (!error) {
        while (!child->ended && !error)
            if (etude_win_poll(&sup, 100) < 0)
                error = GetLastError();
        CHECK(!error && child->status == expected);
    }
    CHECK(etude_win_sup_close(&sup, 30000) == 0);
    char *text = sequence_fixture_read(receipt);
    CHECK(text && strstr(text, "\nactive 0\ncleanup confirmed\n"));
    if (text) {
        CHECK(strstr(text, "\nrun fixture\n") != NULL);
        CHECK((strstr(text, "phase-begin fixture collection ") != NULL) == (expected == 0));
        free(text);
    }
    text = sequence_fixture_read(trace_path);
    CHECK(text && (!strcmp(mode, "observer-failure") || strstr(text, "start\nend\n")));
    free(text);
}

static void sequence_fixture(void)
{
    sequence_fixture_row("pass", 0);
    sequence_fixture_row("preparation-failure", 7);
    sequence_fixture_row("observer-failure", 125);
    CHECK(_putenv_s("SEQUENCE_FIXTURE_MODE", "") == 0);
}
#endif
