/* Read a real private routine policy without granting or starting work. */
#include "../../../tools/etude/routine_entry.h"

static void routine_site_fixture(const char *extra, const char *capacity)
{
    char bytes[16384];
#if defined(__APPLE__)
    const char *endpoint = "macos";
#else
    const char *endpoint = "linux";
#endif
    int n = snprintf(bytes, sizeof bytes,
                     "etude-site 2\nendpoint %s\nlauncher %s\nledger %s\nlocks %s\n"
                     "etude %s\ncapacity %s\nwait 60\nseconds 86400\n%s",
                     endpoint, self, fix_path("routine-ledger"), fix_path("routine-locks"), self,
                     capacity, extra);
    CHECK(n > 0 && (size_t)n < sizeof bytes, "routine policy fits");
    FILE *file = fopen(fix_path("routine-site"), "wb");
    CHECK(file != NULL, "routine policy opens");
    if (file) {
        CHECK(fwrite(bytes, 1, (size_t)n, file) == (size_t)n, "routine policy writes");
        CHECK(fclose(file) == 0, "routine policy closes");
    }
}

static void test_routine_site(void)
{
    struct routine_site site;
    const char *value = getenv("JOBS");
    char *saved = value ? strdup(value) : NULL;
    CHECK(mkdir(fix_path("routine-ledger"), 0700) == 0 &&
              mkdir(fix_path("routine-locks"), 0700) == 0,
          "routine private directories");
    char claims[4096];
    snprintf(claims, sizeof claims, "%s/claims", fix_path("routine-ledger"));
    authority_put(claims, "etude-ledger 1\n");
    CHECK(setenv("JOBS", "4", 1) == 0, "routine worker selection");
    routine_site_fixture("boundary_lazy bigmem-boundary-v1\n", "8000,8589934592,85899345920");
    CHECK(routine_site_read(fix_path("routine-site"), NULL, &site) == 0,
          "real routine policy accepted");
    CHECK(site.jobs == 4 && site.wait == 60 && site.seconds == 86400 &&
              !strcmp(site.resources, "4000,5368709120,42949672960") && *site.account,
          "routine reservation derived from installed capacity");
    CHECK(setenv("JOBS", "9", 1) == 0 &&
              routine_site_read(fix_path("routine-site"), NULL, &site) != 0,
          "over-capacity preparation refused");
    CHECK(setenv("JOBS", "4", 1) == 0, "routine worker restore");
    routine_site_fixture("wait 10\n", "8000,8589934592,85899345920");
    CHECK(routine_site_read(fix_path("routine-site"), NULL, &site) != 0,
          "duplicate policy field refused");
    routine_site_fixture("unknown value\n", "8000,8589934592,85899345920");
    CHECK(routine_site_read(fix_path("routine-site"), NULL, &site) != 0,
          "unknown policy field refused");
    routine_site_fixture("", "8000,8589934592,0");
    CHECK(routine_site_read(fix_path("routine-site"), NULL, &site) != 0, "zero capacity refused");
    if (saved) {
        CHECK(setenv("JOBS", saved, 1) == 0, "restore caller jobs");
        free(saved);
    } else
        unsetenv("JOBS");
}
