/* Compiled commands used by etude_step_hosttest. Included after its file helpers. */
static char step_self[PATH_LEN];

/** Build a role's path relative to the root supplied by the step. */
static const char *role_path(const char *name)
{
    static char paths[8][PATH_LEN];
    static unsigned next;
    const char *root = getenv("ETUDE_STEP_ROOT");
    char *out = paths[next++ % 8];
    fix_format(out, sizeof(paths[0]), "%s/%s", root && *root ? root : ".", name);
    return out;
}

static void role_dir(const char *path)
{
    make_parents(path);
    if (mkdir(path, 0777) && errno != EEXIST)
        exit(125);
}

static void role_append(const char *path, const char *text)
{
    FILE *f = fopen(path, "a");
    if (!f || fputs(text, f) == EOF || fclose(f))
        exit(125);
}

/** Execute one compiler name through the path installed by the step. */
static int role_compiler(const char *name)
{
    pid_t pid = fork();
    int status;
    if (!pid) {
        execlp(name, name, (char *)NULL);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid)
        return 125;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/** Observe both compiler aliases and every ccache environment variable. */
static int role_ccache(void)
{
    const char *search = getenv("PATH"), *cache = getenv("ETUDE_STEP_CCACHE");
    const char *base = getenv("CCACHE_BASEDIR"), *hash = getenv("CCACHE_NOHASHDIR");
    int fd = open("seen", O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0)
        return 125;
    close(fd);
    if (role_compiler("cc") || role_compiler("i686-linux-gnu-gcc"))
        return 125;
    printf("path %.*s\ncc %s\nbase %s\nhash %s\n", (int)strcspn(search ? search : "", ":"),
           search ? search : "", cache ? cache : "", base ? base : "", hash ? hash : "");
    return 0;
}

/** Write the link-mode toolchain and the port that uses it. */
static int role_link(const char *name)
{
    char text[PATH_LEN + 2];
    const char *root = getenv("ETUDE_STEP_ROOT");
    if (!strcmp(name, "tc")) {
        role_dir(role_path("cross"));
        role_dir(role_path("work/x"));
        fix_format(text, sizeof(text), "%s\n", root);
        put(role_path("cross/where"), text);
        role_append("../ran-tc", "built\n");
    } else if (!strcmp(name, "ports")) {
        role_dir(role_path("root"));
        role_dir(role_path("work/keep"));
        role_dir(role_path("work/drop"));
        get(role_path("cross/where"), text, sizeof(text));
        put(role_path("root/w"), text);
    } else if (!strcmp(name, "here-tc")) {
        put(role_path("cross/where"), "here\n");
    } else if (!strcmp(name, "here-ports")) {
        put(role_path("root/w"), "local\n");
    } else if (!strcmp(name, "cut")) {
        role_dir(role_path("c"));
        if (!exists("../go"))
            return 1;
        put(role_path("c/f"), "ok\n");
    } else if (!strcmp(name, "expire-tc")) {
        role_dir(role_path("cross"));
    } else if (!strcmp(name, "expire-ports")) {
        role_dir(role_path("root"));
    } else {
        return 124;
    }
    return 0;
}

/** Hold the first concurrent build until its parent releases the fixture. */
static int role_together(void)
{
    char path[PATH_LEN];
    const char *root = getenv("ETUDE_STEP_FIXTURE");
    if (!root)
        return 125;
    fix_format(path, sizeof(path), "%s/started", root);
    put(path, "");
    fix_format(path, sizeof(path), "%s/go-two", root);
    while (!exists(path))
        usleep(10000); /* interval: inspect the parent's release every 10 ms */
    fix_format(path, sizeof(path), "%s/runs", root);
    role_append(path, "run\n");
    put("t", "t\n");
    return 0;
}

/** Run one recipe without an external interpreter or fixture program. */
static int step_role_main(const char *name)
{
    char text[PATH_LEN + 32], cwd[PATH_LEN];
    const char *key = getenv("ETUDE_STEP_KEY");
    int link = role_link(name);
    if (link != 124)
        return link;
    if (!strcmp(name, "copy")) {
        put("out/sub/a.o", "object\n");
        put("out/sub/a.h", "header\n");
        put("flag", "yes\n");
        role_append("ran", "built\n");
    } else if (!strcmp(name, "nokey")) {
        put("nk", "x\n");
    } else if (!strcmp(name, "leak")) {
        if (!getcwd(cwd, sizeof(cwd)))
            return 125;
        fix_format(text, sizeof(text), "%s/vendor\n", cwd);
        put("lk/p", text);
    } else if (!strcmp(name, "key")) {
        fix_format(text, sizeof(text), "etude-step 1 %s here\n", key ? key : "");
        put("ek", text);
    } else if (!strcmp(name, "unkeyed")) {
        fix_format(text, sizeof(text), "%s\n", key ? key : "none");
        put("uk", text);
    } else if (!strcmp(name, "fail")) {
        put("f", "x\n");
        return 3;
    } else if (!strcmp(name, "ccache")) {
        return role_ccache();
    } else if (!strcmp(name, "ccache-off")) {
        put("seen2", getenv("ETUDE_STEP_CCACHE") ? "1\n" : "0\n");
    } else if (!strcmp(name, "together")) {
        return role_together();
    } else if (!strcmp(name, "demo")) {
        put("dm", "d\n");
    } else if (strcmp(name, "none")) {
        return 125;
    }
    return 0;
}
