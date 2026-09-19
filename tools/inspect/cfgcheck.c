/**
 * cfgcheck: check the two configuration files a sealed disk depends on.
 *
 * `cfgcheck grubembed CONFIG` checks the configuration built into a GRUB
 * core image. Two builders write core images for this project:
 * tools/image/mkdisk_grub.sh writes the one a disk ships with, and
 * tools/image/mkgrubcore.sh writes the ones `bootctl grub` installs over
 * it. Both embed tools/image/grub-embed.cfg, and what that file says is
 * the difference between a machine that can be repaired when its boot
 * partition is damaged and one that cannot: two `configfile` calls,
 * partition 1 first and partition 3 second. The order is the whole of it,
 * since the first call opens a menu that never returns, so a file naming
 * partition 3 first would send every healthy boot to recovery. The early
 * parser these run through has no `if`, which is why they are a pair and
 * not a test. It is checked on the file and not on the image because
 * grub-mkimage compresses what it embeds. What is required is the whole
 * command list and not the presence of two lines in it: GRUB takes `;` as
 * a command separator, so anything looking only for the two expected lines
 * would pass a file whose first command sent every healthy machine to
 * recovery. A commented-out line reads as an absent one.
 *
 * `cfgcheck sealednet ADDRESS PREFIX MASK GATEWAY SCRIPT` checks the
 * sealed disk's fixed address against the script that falls back to it.
 * The address is named twice, in two shapes: the Makefile's SEALED_ADDRESS,
 * SEALED_PREFIX and SEALED_GATEWAY, which the disk's documentation and the
 * host's procedures name, and krffs/rescue-static.script, which has the
 * same address as a dotted mask and applies it from userland when a
 * recovery boot gets no lease. Two shapes of one fact drift, and a machine
 * whose record and fallback disagree is unreachable by the wrong one. So
 * they are held together before a disk is written: the prefix and the mask
 * must describe the same subnet, the address must be a host in it, the
 * gateway must be a different host in the same subnet, and the script must
 * RUN exactly those numbers. What is compared is the script's commands and
 * not its text, and the reading fails closed: a segment that mentions
 * `ifconfig` or `route` at all must be one of three exact forms, so a
 * command hidden behind `command`, `eval`, a subshell or backticks is
 * refused rather than ignored.
 *
 * `--selftest` runs both checkers over the fixtures they carry.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TEXT 65536

#define GRUB_LINES 4
static const char *const GRUB_WANT[GRUB_LINES] = {
    "search --no-floppy --label RACH2BOOT --set=root",
    "configfile ($root)/boot/grub/grub.cfg",
    "search --no-floppy --label RACH2RECOV --set=root",
    "configfile ($root)/boot/grub/recovery.cfg"
};

static char why[512];

static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
}

static char *trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
                       end[-1] == '\n'))
        *--end = '\0';
    return s;
}

/* ---- grubembed ---- */

/* Nonzero when `text` is exactly the two searches and two configfile
   calls, in order. */
static int grub_ok(const char *text)
{
    char buf[MAX_TEXT];
    char *line, *save;
    int n = 0;

    if (strlen(text) >= sizeof(buf)) {
        say("the configuration is longer than this checker reads");
        return 0;
    }
    strcpy(buf, text);
    for (line = strtok_r(buf, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        char *cmd = trim(line);

        if (*cmd == '\0' || *cmd == '#')
            continue;
        if (n >= GRUB_LINES || strcmp(cmd, GRUB_WANT[n]) != 0) {
            say("the commands are not exactly '%s', '%s', '%s' then '%s'", GRUB_WANT[0],
                GRUB_WANT[1], GRUB_WANT[2], GRUB_WANT[3]);
            return 0;
        }
        n++;
    }
    if (n != GRUB_LINES) {
        say("the commands are not exactly '%s', '%s', '%s' then '%s'", GRUB_WANT[0],
            GRUB_WANT[1], GRUB_WANT[2], GRUB_WANT[3]);
        return 0;
    }
    return 1;
}

/* ---- sealednet ---- */

/* A dotted quad as a 32-bit value, or -1. */
static long quad(const char *text)
{
    unsigned long value = 0;
    int parts = 0;

    while (parts < 4) {
        unsigned long n = 0;
        int digits = 0;

        while (isdigit((unsigned char) *text)) {
            n = n * 10 + (unsigned long) (*text++ - '0');
            if (++digits > 3)
                return -1;
        }
        if (digits == 0 || n > 255)
            return -1;
        value = value << 8 | n;
        parts++;
        if (parts < 4) {
            if (*text != '.')
                return -1;
            text++;
        }
    }
    return *text == '\0' ? (long) value : -1;
}

/* Split a segment into whitespace tokens, with `(){}` and backticks read
   as whitespace so a command in a subshell or backticks is seen. Returns
   the count. */
static int tokens_of(char *segment, char **out, int max)
{
    int n = 0;
    char *p = segment;

    for (; *p != '\0'; p++)
        if (strchr("(){}`", *p) != NULL)
            *p = ' ';
    p = segment;
    while (n < max) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;
        out[n++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t')
            p++;
        if (*p != '\0')
            *p++ = '\0';
    }
    return n;
}

static int same_tokens(char **got, int n, const char *const *want, int wn)
{
    int i;

    if (n != wn)
        return 0;
    for (i = 0; i < n; i++)
        if (strcmp(got[i], want[i]) != 0)
            return 0;
    return 1;
}

/* The network commands the script would run must be exactly the two this
   machine is supposed to run, in order. Every segment of every uncommented
   line is read, not the first of each: a line can carry more than one
   command. Returns nonzero when the script agrees. */
static int script_ok(const char *text, const char *address, const char *mask,
                     const char *gateway)
{
    static const char *read_form[2] = { "ifconfig", "eth0" };
    const char *want0[5], *want1[5];
    char buf[MAX_TEXT];
    char *line, *lsave;
    int matched = 0;

    want0[0] = "ifconfig";
    want0[1] = "eth0";
    want0[2] = address;
    want0[3] = "netmask";
    want0[4] = mask;
    want1[0] = "route";
    want1[1] = "add";
    want1[2] = "default";
    want1[3] = "gw";
    want1[4] = gateway;

    if (strlen(text) >= sizeof(buf)) {
        say("is longer than this checker reads");
        return 0;
    }
    strcpy(buf, text);
    for (line = strtok_r(buf, "\n", &lsave); line != NULL;
         line = strtok_r(NULL, "\n", &lsave)) {
        char *cmd = trim(line);
        char *seg = cmd;

        if (*cmd == '\0' || *cmd == '#')
            continue;
        /* Segment on the shell's separators: || && ; | then else do. The
           search is textual, the way the separators appear in this
           script's grammar. */
        while (seg != NULL && *seg != '\0') {
            char *cut = NULL, *at;
            static const char *const seps[] = { "||", "&&", ";", "|",
                                                " then ", " else ",
                                                " do " };
            char *tok[16];
            int n, i, mentions;
            size_t seplen = 0;

            for (i = 0; i < 7; i++) {
                at = strstr(seg, seps[i]);
                if (at != NULL && (cut == NULL || at < cut)) {
                    cut = at;
                    seplen = strlen(seps[i]);
                }
            }
            if (cut != NULL)
                *cut = '\0';
            n = tokens_of(seg, tok, 16);
            /* The keywords are looked for in the TOKENS and not in the
               segment: tokenizing ends the segment string at its first
               token, so a search of the segment afterwards would see only
               that, and `command ifconfig ...` would read as a segment
               with no network command in it at all. */
            mentions = 0;
            for (i = 0; i < n; i++)
                if (strstr(tok[i], "ifconfig") != NULL ||
                    strstr(tok[i], "route") != NULL)
                    mentions = 1;
            if (mentions) {
                if (same_tokens(tok, n, read_form, 2)) {
                    /* The read of the current address the fallback
                       tests. */
                } else if (matched == 0 && same_tokens(tok, n, want0, 5)) {
                    matched = 1;
                } else if (matched == 1 && same_tokens(tok, n, want1, 5)) {
                    matched = 2;
                } else {
                    say("has a network command this does not recognise, or "
                        "runs the two it expects out of order");
                    return 0;
                }
            }
            seg = cut != NULL ? cut + seplen : NULL;
        }
    }
    if (matched != 2) {
        say("does not run exactly 'ifconfig eth0 %s netmask %s' and then "
            "'route add default gw %s'", address, mask, gateway);
        return 0;
    }
    return 1;
}

/* ---- the selftest ---- */

static int st_bad, st_ran;

static void st_grub(const char *name, const char *text, int want_ok)
{
    int ok = grub_ok(text);

    st_ran++;
    if (ok != want_ok) {
        fprintf(stderr, "check-grubembed: selftest: fail: %s\n", name);
        st_bad++;
    }
}

static void st_net(const char *name, const char *text, int want_ok)
{
    int ok = script_ok(text, "10.0.0.5", "255.0.0.0", "10.0.0.1");

    st_ran++;
    if (ok != want_ok) {
        fprintf(stderr, "check-sealednet: selftest: fail: %s\n", name);
        st_bad++;
    }
}

static int selftest(void)
{
    const char *both = "search --no-floppy --label RACH2BOOT --set=root\n"
                       "configfile ($root)/boot/grub/grub.cfg\n"
                       "search --no-floppy --label RACH2RECOV --set=root\n"
                       "configfile ($root)/boot/grub/recovery.cfg\n";

    st_bad = 0;
    st_ran = 0;
    st_grub("the configuration itself", both, 1);
    st_grub("comments and blank lines around it",
            "# a comment\n\nsearch --no-floppy --label RACH2BOOT --set=root\n"
            "configfile ($root)/boot/grub/grub.cfg\n"
            "search --no-floppy --label RACH2RECOV --set=root\n"
            "configfile ($root)/boot/grub/recovery.cfg\n\n# another\n",
            1);
    st_grub("the recovery pair commented out",
            "search --no-floppy --label RACH2BOOT --set=root\n"
            "configfile ($root)/boot/grub/grub.cfg\n"
            "# search --no-floppy --label RACH2RECOV --set=root\n"
            "# configfile ($root)/boot/grub/recovery.cfg\n", 0);
    st_grub("the recovery pair missing",
            "search --no-floppy --label RACH2BOOT --set=root\n"
            "configfile ($root)/boot/grub/grub.cfg\n", 0);
    st_grub("the two pairs the other way round",
            "search --no-floppy --label RACH2RECOV --set=root\n"
            "configfile ($root)/boot/grub/recovery.cfg\n"
            "search --no-floppy --label RACH2BOOT --set=root\n"
            "configfile ($root)/boot/grub/grub.cfg\n", 0);
    st_grub("the old fixed paths",
            "configfile (hd0,msdos1)/boot/grub/grub.cfg\n"
            "configfile (hd0,msdos3)/boot/grub/recovery.cfg\n", 0);
    st_grub("a search without its configfile",
            "search --no-floppy --label RACH2BOOT --set=root\n"
            "search --no-floppy --label RACH2RECOV --set=root\n"
            "configfile ($root)/boot/grub/grub.cfg\n"
            "configfile ($root)/boot/grub/recovery.cfg\n", 0);
    st_grub("a fifth command",
            "search --no-floppy --label RACH2BOOT --set=root\n"
            "configfile ($root)/boot/grub/grub.cfg\n"
            "search --no-floppy --label RACH2RECOV --set=root\n"
            "configfile ($root)/boot/grub/recovery.cfg\n"
            "configfile (hd0,msdos2)/x.cfg\n", 0);
    st_grub("a command before them",
            "echo hello\nsearch --no-floppy --label RACH2BOOT --set=root\n"
            "configfile ($root)/boot/grub/grub.cfg\n"
            "search --no-floppy --label RACH2RECOV --set=root\n"
            "configfile ($root)/boot/grub/recovery.cfg\n", 0);
    st_grub("a command hidden behind a semicolon",
            "echo hello; search --no-floppy --label RACH2BOOT --set=root\n"
            "configfile ($root)/boot/grub/grub.cfg\n"
            "search --no-floppy --label RACH2RECOV --set=root\n"
            "configfile ($root)/boot/grub/recovery.cfg\n", 0);
    st_grub("nothing at all", "", 0);

    st_net("the script as it is",
           "ifconfig eth0 10.0.0.5 netmask 255.0.0.0 || exit 1\n"
           "route add default gw 10.0.0.1 || exit 1\n", 1);
    st_net("comments and a pipeline around it",
           "# a comment\nifconfig eth0 | grep -q x || exit 0\n"
           "ifconfig eth0 10.0.0.5 netmask 255.0.0.0 || exit 1\n"
           "route add default gw 10.0.0.1 || exit 1\n", 1);
    st_net("a second command after a semicolon",
           "ifconfig eth0 10.0.0.5 netmask 255.0.0.0; "
           "ifconfig eth0 10.0.0.6 netmask 255.0.0.0\n"
           "route add default gw 10.0.0.1 || exit 1\n", 0);
    st_net("a second command after &&",
           "ifconfig eth0 10.0.0.5 netmask 255.0.0.0 && "
           "ifconfig eth0 10.0.0.6 netmask 255.0.0.0\n"
           "route add default gw 10.0.0.1 || exit 1\n", 0);
    st_net("the address commented out",
           "# ifconfig eth0 10.0.0.5 netmask 255.0.0.0\n"
           "route add default gw 10.0.0.1 || exit 1\n", 0);
    st_net("a different live gateway",
           "ifconfig eth0 10.0.0.5 netmask 255.0.0.0\n"
           "route add default gw 10.0.0.2\n", 0);
    st_net("a trailing token",
           "ifconfig eth0 10.0.0.5 netmask 255.0.0.0 mtu 1400\n"
           "route add default gw 10.0.0.1\n", 0);
    st_net("a route removed as well",
           "ifconfig eth0 10.0.0.5 netmask 255.0.0.0\n"
           "route add default gw 10.0.0.1\n"
           "route del default gw 10.0.0.1\n", 0);
    st_net("a command hidden behind `command`",
           "ifconfig eth0 10.0.0.5 netmask 255.0.0.0\n"
           "route add default gw 10.0.0.1\n"
           "command ifconfig eth0 10.0.0.6 netmask 255.0.0.0\n", 0);
    st_net("a command in a subshell",
           "ifconfig eth0 10.0.0.5 netmask 255.0.0.0\n"
           "route add default gw 10.0.0.1\n"
           "(ifconfig eth0 10.0.0.6 netmask 255.0.0.0)\n", 0);
    st_net("a command through eval",
           "ifconfig eth0 10.0.0.5 netmask 255.0.0.0\n"
           "route add default gw 10.0.0.1\n"
           "eval ifconfig eth0 10.0.0.6 netmask 255.0.0.0\n", 0);
    st_net("a command in backticks",
           "ifconfig eth0 10.0.0.5 netmask 255.0.0.0\n"
           "route add default gw 10.0.0.1\n"
           "X=`ifconfig eth0 10.0.0.6 netmask 255.0.0.0`\n", 0);
    st_net("nothing at all", "", 0);

    if (st_bad) {
        fprintf(stderr, "cfgcheck: selftest: %d of %d cases failed\n",
                st_bad, st_ran);
        return 1;
    }
    printf("cfgcheck: selftest: ok: %d cases, both checkers\n", st_ran);
    return 0;
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long len;

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t) len + 1);
    if (buf == NULL || fread(buf, 1, (size_t) len, f) != (size_t) len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--selftest") == 0)
        return selftest();

    if (argc == 3 && strcmp(argv[1], "grubembed") == 0) {
        char *text = read_text(argv[2]);

        if (text == NULL) {
            fprintf(stderr, "check-grubembed: cannot read %s\n", argv[2]);
            return 1;
        }
        if (!grub_ok(text)) {
            fprintf(stderr, "check-grubembed: %s: %s\n", argv[2], why);
            return 1;
        }
        printf("check-grubembed: OK (%s reaches partition 1 and then "
               "partition 3)\n", argv[2]);
        free(text);
        return 0;
    }

    if (argc == 7 && strcmp(argv[1], "sealednet") == 0) {
        const char *address_s = argv[2], *prefix_s = argv[3];
        const char *mask_s = argv[4], *gateway_s = argv[5];
        const char *path = argv[6];
        long address = quad(address_s), mask = quad(mask_s);
        long gateway = quad(gateway_s), prefix;
        uint32_t from_prefix, inverted;
        char *end, *text;

        if (address < 0) {
            fprintf(stderr, "check-sealednet: %s is not an address\n",
                    address_s);
            return 1;
        }
        if (mask < 0) {
            fprintf(stderr, "check-sealednet: %s is not a netmask\n", mask_s);
            return 1;
        }
        if (gateway < 0) {
            fprintf(stderr, "check-sealednet: %s is not an address\n",
                    gateway_s);
            return 1;
        }
        prefix = strtol(prefix_s, &end, 10);
        if (*end != '\0' || prefix < 1 || prefix > 30) {
            fprintf(stderr, "check-sealednet: %s is not a usable prefix "
                    "length\n", prefix_s);
            return 1;
        }
        from_prefix = (uint32_t) (0xFFFFFFFFul << (32 - prefix));
        if (from_prefix != (uint32_t) mask) {
            fprintf(stderr, "check-sealednet: /%ld is not %s; the menu and "
                    "the recovery script would put this machine on two "
                    "different subnets\n", prefix, mask_s);
            return 1;
        }
        inverted = ~(uint32_t) mask;
        if ((uint32_t) address == ((uint32_t) address & (uint32_t) mask) ||
            (uint32_t) address == ((uint32_t) address | inverted)) {
            fprintf(stderr, "check-sealednet: %s is this subnet's network "
                    "or broadcast address\n", address_s);
            return 1;
        }
        if (gateway == address) {
            fprintf(stderr, "check-sealednet: the gateway is the machine's "
                    "own address\n");
            return 1;
        }
        if (((uint32_t) gateway & (uint32_t) mask) !=
                ((uint32_t) address & (uint32_t) mask) ||
            (uint32_t) gateway == ((uint32_t) gateway & (uint32_t) mask) ||
            (uint32_t) gateway == ((uint32_t) gateway | inverted)) {
            fprintf(stderr, "check-sealednet: %s is not a host on %s/%ld, "
                    "so the kernel would refuse the static entries\n",
                    gateway_s, address_s, prefix);
            return 1;
        }
        text = read_text(path);
        if (text == NULL) {
            fprintf(stderr, "check-sealednet: cannot read %s\n", path);
            return 1;
        }
        if (!script_ok(text, address_s, mask_s, gateway_s)) {
            fprintf(stderr, "check-sealednet: %s %s\n", path, why);
            return 1;
        }
        printf("check-sealednet: OK (%s/%ld through %s, and %s says the "
               "same)\n", address_s, prefix, gateway_s, path);
        free(text);
        return 0;
    }

    fprintf(stderr, "usage: cfgcheck grubembed CONFIG | cfgcheck sealednet "
            "ADDRESS PREFIX MASK GATEWAY SCRIPT | cfgcheck --selftest\n");
    return 2;
}
