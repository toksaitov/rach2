/**
 * The program half of the credential probes: report the identity, working
 * directory, `argv[0]` and environment an exec handed this process, and exit 0
 * only if they match the expectation named on the command line.
 *
 * It is the shell of the test image's `probe` account, so every path that
 * reaches it reaches it as a shell would: `su` execs the target's shell as
 * `execle(shell, arg0, "-c", command, 0, env)`, which hands this program `-c`
 * and then the string, and in login mode an `argv[0]` of a `-` followed by the
 * shell's base name besides. The expectation is therefore accepted either as a
 * bare argument or after a `-c`, and a login `su` is told apart from a
 * non-login one by nothing but what it says here.
 *
 * The report is one line on standard error in the same `key=value` vocabulary
 * the expectation uses, so a run's output is a valid expectation string:
 *
 *     idreport: uid=0 euid=0 suid=0 gid=0 egid=0 sgid=0 arg0=-idreport
 *     cwd=/home/probe env=PATH=/bin:/usr/bin env=HOME=/home/probe ...
 *
 * The items are separated by blanks, `uid`, `euid`, `suid`, `gid`, `egid` and
 * `sgid` are decimal, and `env` is repeated once per variable. The environment
 * is compared as an exact set: every `env=` item must be present and their
 * count must be the whole environment, so a variable leaked from the caller
 * fails just as a missing one does. An expectation naming no item at all is a
 * usage error, not a vacuous pass, since a driver whose argument went
 * missing would otherwise be told everything matched.
 *
 * The exit status names what failed: 2 for a usage error, 10 to 15 for the six
 * ids in order, 20 for the working directory, 21 for `argv[0]`, 30 for a
 * variable the environment does not hold, 31 for an environment holding more
 * than was named, and 40 for an unknown key.
 *
 * Freestanding; direct int 0x80. Staged 0755 at `/idreport` by the test image's
 * `exec-dir` sweep, a helper like `suidhelper` and not a probe: it is run by
 * `tests/musl/pwauthmusl.c` and prints no success token of its own.
 */

typedef unsigned int u32;

#define SYS_EXIT        1
#define SYS_WRITE       4
#define SYS_GETCWD      183
#define SYS_GETRESUID32 209
#define SYS_GETRESGID32 211

#define EXIT_USAGE     2
#define EXIT_UID       10
#define EXIT_CWD       20
#define EXIT_ARG0      21
#define EXIT_ENV       30
#define EXIT_ENV_EXTRA 31
#define EXIT_KEY       40

static int syscall1(int num, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(num), "b"(a) : "memory");
    return r;
}

static int syscall3(int num, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(num), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static void done(int code)
{
    syscall1(SYS_EXIT, code);
}

static int streq(const char *a, const char *b)
{
    u32 i = 0;

    while (a[i] && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

/**
 * True if the string `s` is exactly the `n` bytes at `text`.
 *
 * The expectation arrives as one argument holding many items, so nothing in it
 * is terminated where an item ends; every comparison against it is therefore
 * bounded by the caller's length, not by a NUL.
 */
static int eq_n(const char *s, const char *text, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        if (s[i] == '\0' || s[i] != text[i]) {
            return 0;
        }
    }
    return s[n] == '\0';
}

/* The report is built up in one buffer and written once, so it cannot be
 * interleaved with another process's serial output. */
static char Line[2048];
static u32 Filled;

static void put(const char *s)
{
    u32 i = 0;

    while (s[i] && Filled + 1 < sizeof(Line)) {
        Line[Filled++] = s[i++];
    }
    Line[Filled] = '\0';
}

static void put_u32(u32 value)
{
    char digits[12];
    int i = 0;

    do {
        digits[i++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (i > 0 && Filled + 1 < sizeof(Line)) {
        Line[Filled++] = digits[--i];
    }
    Line[Filled] = '\0';
}

static void flush(void)
{
    syscall3(SYS_WRITE, 2, (int)Line, (int)Filled);
}

/**
 * Compare one `key=value` expectation item against what this process was
 * given, exiting on a mismatch.
 *
 * `item` points at the key and `end` one past the item's last character, since
 * the expectation is one argument holding many items and nothing may be copied
 * out of it. A value is everything after the first `=`, so an `env` item's own
 * `=` is part of the value.
 */
static void match(const char *item, const char *end, const u32 *ids, const char *arg0,
                  const char *cwd, char **envp, u32 *env_named)
{
    const char *value = item;
    u32 key_len;
    u32 i;

    while (value < end && *value != '=') {
        value++;
    }
    if (value == end) {
        done(EXIT_KEY);
    }
    key_len = (u32)(value - item);
    value++;

    {
        /* The six ids, in the order the exit statuses count them. */
        static const char *const names[6] = {"uid", "euid", "suid", "gid", "egid", "sgid"};

        for (i = 0; i < 6; i++) {
            if (eq_n(names[i], item, key_len)) {
                u32 want = 0;
                const char *p = value;

                if (p == end) {
                    done(EXIT_KEY);
                }
                while (p < end) {
                    if (*p < '0' || *p > '9') {
                        done(EXIT_KEY);
                    }
                    want = want * 10 + (u32)(*p++ - '0');
                }
                if (want != ids[i]) {
                    done((int)(EXIT_UID + i));
                }
                return;
            }
        }
    }

    if (eq_n("arg0", item, key_len)) {
        if (!eq_n(arg0, value, (u32)(end - value))) {
            done(EXIT_ARG0);
        }
        return;
    }
    if (eq_n("cwd", item, key_len)) {
        if (!eq_n(cwd, value, (u32)(end - value))) {
            done(EXIT_CWD);
        }
        return;
    }
    if (eq_n("env", item, key_len)) {
        for (i = 0; envp[i]; i++) {
            if (eq_n(envp[i], value, (u32)(end - value))) {
                (*env_named)++;
                return;
            }
        }
        done(EXIT_ENV);
    }
    done(EXIT_KEY);
}

void probe_main(u32 *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(u32 *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = (char **)(sp + 1 + argc + 1);
    char cwd[256];
    u32 ids[6];
    const char *expectation = 0;
    u32 env_count = 0;
    u32 env_named = 0;
    u32 items = 0;
    int i;

    for (i = 0; i < 6; i++) {
        ids[i] = 0;
    }
    if (syscall3(SYS_GETRESUID32, (int)&ids[0], (int)&ids[1], (int)&ids[2]) != 0 ||
        syscall3(SYS_GETRESGID32, (int)&ids[3], (int)&ids[4], (int)&ids[5]) != 0) {
        done(EXIT_USAGE);
    }
    cwd[0] = '\0';
    if (syscall3(SYS_GETCWD, (int)cwd, (int)sizeof(cwd), 0) < 0) {
        cwd[0] = '\0';
    }
    while (envp[env_count]) {
        env_count++;
    }

    put("idreport: uid=");
    put_u32(ids[0]);
    put(" euid=");
    put_u32(ids[1]);
    put(" suid=");
    put_u32(ids[2]);
    put(" gid=");
    put_u32(ids[3]);
    put(" egid=");
    put_u32(ids[4]);
    put(" sgid=");
    put_u32(ids[5]);
    put(" arg0=");
    put(argc > 0 ? argv[0] : "");
    put(" cwd=");
    put(cwd);
    for (i = 0; (u32)i < env_count; i++) {
        put(" env=");
        put(envp[i]);
    }
    put("\n");
    flush();

    /* A `-c` is the shell calling convention su reaches this program through;
     * a bare operand is the same string handed over directly. */
    for (i = 1; i < argc; i++) {
        if (streq(argv[i], "-c")) {
            continue;
        }
        expectation = argv[i];
        break;
    }
    if (expectation) {
        const char *p = expectation;

        while (*p) {
            const char *item;

            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (!*p) {
                break;
            }
            item = p;
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }
            match(item, p, ids, argc > 0 ? argv[0] : "", cwd, envp, &env_named);
            items++;
        }
    }
    if (items == 0) {
        done(EXIT_USAGE);
    }
    if (env_named != env_count) {
        done(EXIT_ENV_EXTRA);
    }
    done(0);
}
