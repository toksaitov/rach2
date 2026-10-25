/* Exercise native digest-cache reads and rewrites with real private files. */
static int digest_line_present(const char *cache, const char *path)
{
    char line[8192];
    size_t size = strlen(path), length;
    FILE *f = fopen(cache, "r");
    int found = 0, error;

    if (f == NULL)
        return 0;
    while (fgets(line, sizeof line, f) != NULL) {
        length = strcspn(line, "\r\n");
        line[length] = '\0';
        if (length > size && line[length - size - 1] == ' ' && !strcmp(line + length - size, path))
            found = 1;
    }
    error = ferror(f);
    if (fclose(f))
        error = 1;
    if (error)
        return 0;
    return found;
}

static void test_digest_paths(void)
{
    char cache[PATH_MAX], paths[2][PATH_MAX];
    char expected[2][ETUDE_DIGEST_HEX + 1] = {{0}, {0}};
    char actual[ETUDE_DIGEST_HEX + 1];
    long long settling = etude_store_settle_ns;
    unsigned long hashed;
    struct stat st;
    size_t i;

    fix_format(cache, sizeof cache, "%s", fix_path("native-digests"));
    fix_format(paths[0], sizeof paths[0], "%s", fix_path("native digest first"));
    fix_format(paths[1], sizeof paths[1], "%s", fix_path("native digest second"));
#ifdef _WIN32
    /* Both spellings must survive a rewrite, including the drive prefix. */
    for (i = 0; paths[1][i]; i++)
        if (paths[1][i] == '/')
            paths[1][i] = '\\';
#endif
    etude_store_settle_ns = 0;
    CHECK(!write_text(paths[0], "first cache input\n", 0644) &&
              !write_text(paths[1], "second cache input\n", 0644),
          "digests: native files are written");
    for (i = 0; i < 2; i++)
        CHECK(!etude_digest_file(NULL, paths[i], &st, expected[i]),
              "digests: independent whole-file hashes");
    hashed = etude_store_hashed;
    for (i = 0; i < 2; i++)
        CHECK(!etude_digest_file(cache, paths[i], &st, actual) && !strcmp(actual, expected[i]) &&
                  etude_store_hashed == hashed + i + 1,
              "digests: first read hashes each native file");
    CHECK(digest_line_present(cache, paths[0]) && digest_line_present(cache, paths[1]),
          "digests: a rewrite preserves both native paths");
    for (i = 0; i < 2; i++)
        CHECK(!etude_digest_file(cache, paths[i], &st, actual) && !strcmp(actual, expected[i]) &&
                  etude_store_hashed == hashed + 2,
              "digests: unchanged native files hit without hashing");
    etude_store_settle_ns = settling;
}
