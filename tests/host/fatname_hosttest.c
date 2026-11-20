/*
 * fatname_hosttest.c: the FAT32 name rules of fat32name.c alone.
 *
 * host-deps: rach2/sys/fs/fat32name.c rach2/sys/fs/fatcommon.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/pressure_host.c
 *
 * The conversion from UTF-8 to UTF-16 and the names it fails, the 8.3 rule
 * with its case bits, the alias and its numbering, the code page 437 of
 * short names both ways, the checksum and the long entries' layout, and
 * names whose entries mtools wrote: where this driver's rule agrees with
 * what mtools writes and where it does not.
 */

#include "../../rach2/sys/fs/fat32name.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/proc/wait_queue.h"
#include "../../rach2/sys/rtc.h"

extern int printf(const char *fmt, ...);

static int fails;

/** Count and print a failed row. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("fatname_hosttest: FAIL: %s\n", what);
    }
}

/** Whether the 11 bytes of the short name `a` equal those of `b`. */
static int eq11(const uint8_t *a, const char *b)
{
    int i;

    for (i = 0; i < 11; i++) {
        if (a[i] != (uint8_t)b[i]) {
            return 0;
        }
    }
    return 1;
}

static uint16_t units[FAT_NAME_UNITS];
static uint8_t name11[11];

/** Fill `name11` with the alias of the UTF-8 `name`, which must convert. */
static void alias_of(const char *name)
{
    uint32_t n = 0;

    check(fat_name_units(name, units, &n) == 0, name);
    fat32_alias(units, n, name11);
}

/** Whether the short name `raw` with the case bits `case_bits` is shown
 * as the UTF-8 `want`. */
static int shown(const char *raw, uint8_t case_bits, const char *want)
{
    char out[FAT32_SHORT_UTF8];
    int i;

    fat32_short_utf8((const uint8_t *)raw, case_bits, out);
    for (i = 0; want[i] && out[i] == want[i]; i++) {
    }
    return out[i] == want[i];
}

/**
 * Run the rows of code page 437, Linux's vfat default: a short name's
 * bytes above 0x7F decoded to UTF-8 and lower-cased by the code page's
 * pairs, and a long name's characters encoded in its alias, upper-cased,
 * with `_` for each UTF-16 unit the code page does not hold.
 */
static void cp437_rows(void)
{
    check(shown("F\x9AR     TXT", 0, "F\xC3\x9CR.TXT"), "0x9A is U-umlaut, two bytes of UTF-8");
    check(shown("F\x9AR     TXT", 0x18, "f\xC3\xBCr.txt"),
          "the case bits lower-case U-umlaut to u-umlaut, 0x81, as Linux's nls_cp437");
    check(shown("CAF\x82    ", 0, "CAF\xC3\xA9") && shown("CAF\x8A    ", 0, "CAF\xC3\xA8"),
          "e-acute and e-grave are two names, where both were CAF?");
    check(shown("\xC9\xCD\xBB        ", 0, "\xE2\x95\x94\xE2\x95\x90\xE2\x95\x97"),
          "box drawing takes three bytes each");
    check(shown("\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB", 0,
                "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88"
                "\xE2\x96\x88\xE2\x96\x88.\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88"),
          "eleven three-byte characters and the dot fit FAT32_SHORT_UTF8");
    check(shown("\x05"
                "5FILE  TXT",
                0x18,
                "\xCF\x83"
                "5file.txt"),
          "a leading 0x05 is 0xE5, sigma, which has no upper case to lower");
    check(shown("AB\0CD   TXT", 0, "AB.TXT"), "a 0 byte ends its part, as Linux reads it");
    check(shown("\0          ", 0, "") && shown("ABC     \0XY", 0, "ABC"),
          "a part that starts with 0 is empty, and an empty extension has no dot");

    alias_of("f\xC3\xBCr.txt");
    check(eq11(name11, "F\x9AR~1   TXT"), "u-umlaut is 0x81, upper-cased to 0x9A in the alias");
    check(shown((const char *)name11, 0, "F\xC3\x9CR~1.TXT"), "and the alias reads back");
    alias_of("caf\xC3\xA9 au lait.txt");
    check(eq11(name11, "CAF\x90"
                       "AU~1TXT"),
          "e-acute is 0x82, upper-cased to E-acute, 0x90");
    alias_of("\xC3\xA2.txt");
    check(eq11(name11, "\x83~1     TXT"), "a-circumflex, 0x83, has no upper case and stays");
    alias_of("\xCF\x83 x");
    check(eq11(name11, "\xE4X~1       "),
          "sigma, 0xE5, is upper-cased to 0xE4 and never left 0xE5");
    alias_of("\xE2\x82\xAC.txt");
    check(eq11(name11, "_~1     TXT"), "the euro sign is not in code page 437 and becomes _");
    alias_of("\xE2\x95\x94x.\xC2\xB1");
    check(eq11(name11, "\xC9X~1    \xF1  "), "box drawing and plus-minus are encoded as they are");
    alias_of("long\x7fname.txt");
    check(eq11(name11, "LONG\x7FN~1TXT"),
          "DEL, 0x7F, is kept in the alias, as Linux's to_shortname_char");
    alias_of("a+b,c.t;x");
    check(eq11(name11, "A_B_C~1 T_X"), "ASCII outside the short set below 0x7F still becomes _");
}

/* What fatcommon.c needs of the kernel: the walk's lock and the clock,
   which the name rows never reach. */

/** Clear the queue. */
void wait_queue_init(wait_queue_t *wq)
{
    __builtin_memset(wq, 0, sizeof(*wq));
}
/** Do nothing: no process sleeps. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}
/** Return -1 at once, as an interrupted wait does. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    return -1;
}
/** As `wq_wait_event`. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait_event(wq, cond, arg);
}
/** A fixed time, 2020-03-04 12:00 UTC. */
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1583323200;
    *nanoseconds = 0;
}

/**
 * Run the rows: the conversion and the names it fails; the 8.3 rule and
 * the case bits, against what mtools wrote; the alias and its numbering;
 * the checksum and the long entries; and code page 437. Returns the count
 * of failed rows, at most 255.
 */
int main(void)
{
    uint32_t n;
    uint8_t lower, raw[32], sum;
    static char longname[300];
    int i;

    check(fat_name_units("hello.c", units, &n) == 0 && n == 7 && units[0] == 'h',
          "an ASCII name converts");
    check(fat_name_units("\xc3\xa9.txt", units, &n) == 0 && n == 5 && units[0] == 0xE9,
          "a two-byte character is one unit");
    check(fat_name_units("\xf0\x9f\x98\x80.txt", units, &n) == 0 && n == 6 && units[0] == 0xD83D &&
              units[1] == 0xDE00,
          "a character past the BMP is a surrogate pair");
    for (i = 0; i < 255; i++) {
        longname[i] = 'a';
    }
    longname[255] = '\0';
    check(fat_name_units(longname, units, &n) == 0 && n == 255, "255 characters fit");
    longname[255] = 'a';
    longname[256] = '\0';
    check(fat_name_units(longname, units, &n) == -ENAMETOOLONG, "256 do not");
    for (i = 0; i < 128; i++) {
        longname[2 * i] = (char)0xc3; /* U+00E9, two bytes each */
        longname[2 * i + 1] = (char)0xa9;
    }
    longname[256] = '\0';
    check(fat_name_units(longname, units, &n) == -ENAMETOOLONG,
          "128 two-byte characters are 128 units and 256 bytes: too long");
    check(fat_name_units("a/b", units, &n) == -EINVAL &&
              fat_name_units("a:b", units, &n) == -EINVAL &&
              fat_name_units("a\"b", units, &n) == -EINVAL &&
              fat_name_units("a?b", units, &n) == -EINVAL &&
              fat_name_units("a*b", units, &n) == -EINVAL &&
              fat_name_units("a|b", units, &n) == -EINVAL &&
              fat_name_units("a<b", units, &n) == -EINVAL &&
              fat_name_units("a\\b", units, &n) == -EINVAL &&
              fat_name_units("a\tb", units, &n) == -EINVAL,
          "the characters FAT forbids are EINVAL");
    check(fat_name_units("name.", units, &n) == -EINVAL &&
              fat_name_units("name ", units, &n) == -EINVAL,
          "a trailing dot or space is EINVAL");
    check(fat_name_units(".", units, &n) == -EINVAL && fat_name_units("..", units, &n) == -EINVAL &&
              fat_name_units("", units, &n) == -EINVAL,
          "the dot names and the empty name are EINVAL");
    check(fat_name_units("\xc3", units, &n) == -EINVAL &&
              fat_name_units("\xff", units, &n) == -EINVAL &&
              fat_name_units("\xc0\x80", units, &n) == -EINVAL &&
              fat_name_units("\xed\xa0\x80", units, &n) == -EINVAL,
          "malformed, overlong and surrogate UTF-8 are EINVAL");
    check(fat_name_units(".hidden", units, &n) == 0 && fat_name_units("a b", units, &n) == 0,
          "a leading dot and an inner space are names");

    check(fat32_name_short("hello.c", name11, &lower) == 1 && eq11(name11, "HELLO   C  ") &&
              lower == 0x18,
          "hello.c is short with both parts lower");
    check(fat32_name_short("UPPER.TXT", name11, &lower) == 1 && eq11(name11, "UPPER   TXT") &&
              lower == 0,
          "UPPER.TXT is short with no case bits");
    check(fat32_name_short("Hello.TXT", name11, &lower) == 0 &&
              fat32_name_short("hello.Txt", name11, &lower) == 0,
          "a part of mixed case needs a long entry");
    check(fat32_name_short("Mixed.c", name11, &lower) == 0 &&
              fat32_name_short("Upper2.TXT", name11, &lower) == 0,
          "Mixed.c and Upper2.TXT need long entries, as mtools wrote them");
    check(
        fat32_name_short("long name.txt", name11, &lower) == 0 &&
            fat32_name_short("a.b.c", name11, &lower) == 0 &&
            fat32_name_short("toolongname.txt", name11, &lower) == 0 &&
            fat32_name_short("file.abcd", name11, &lower) == 0 &&
            fat32_name_short("+plus", name11, &lower) == 0 &&
            fat32_name_short(".hidden", name11, &lower) == 0,
        "a space, two dots, a long part, or a character outside the short set needs a long entry");
    check(fat32_name_short("F$#.@~", name11, &lower) == 1 && eq11(name11, "F$#     @~ "),
          "the short set's other characters fit");
    check(fat32_name_short("NOEXT", name11, &lower) == 1 && eq11(name11, "NOEXT      "),
          "a name with no extension fits");

    alias_of("long name.txt");
    check(eq11(name11, "LONGNA~1TXT"), "long name.txt takes LONGNA~1.TXT, as mtools gave it");
    alias_of("Mixed.c");
    check(eq11(name11, "MIXED~1 C  "),
          "Mixed.c takes MIXED~1.C, where mtools kept MIXED.C without a number");
    alias_of("a.b.c");
    check(eq11(name11, "AB~1    C  "), "a.b.c drops the inner dot");
    alias_of("\xf0\x9f\x98\x80.txt");
    check(eq11(name11, "__~1    TXT"), "each half of a surrogate pair becomes an underscore");
    alias_of(".hidden");
    check(eq11(name11, "_~1     HID"),
          "a leading dot leaves an underscore base, where Windows keeps HIDDEN");
    alias_of("longname1.txt");
    fat32_alias_number(name11, 2);
    check(eq11(name11, "LONGNA~2TXT"), "the second number");
    fat32_alias_number(name11, 12);
    check(eq11(name11, "LONGN~12TXT"), "two digits take a base character");
    fat32_alias_number(name11, 999999);
    check(eq11(name11, "L~999999TXT"), "six digits leave one");

    check(fat_name_units("long name.txt", units, &n) == 0 && fat32_long_entries(n) == 1 &&
              fat32_long_entries(14) == 2 && fat32_long_entries(26) == 2 &&
              fat32_long_entries(27) == 3,
          "thirteen units per long entry");
    alias_of("long name.txt");
    sum = fat32_short_sum(name11);
    fat32_long_entry(raw, 1, 1, sum, units, n);
    check(raw[0] == 0x41 && raw[11] == 0x0F && raw[13] == sum && raw[1] == 'l' && raw[2] == 0 &&
              raw[28] == 'x' && raw[30] == 't',
          "one entry of exactly thirteen units: the last flag, the checksum, the units, no "
          "terminator");
    check(fat_name_units("fourteen chars", units, &n) == 0 && n == 14, "a fourteen-unit name");
    fat32_long_entry(raw, 2, 2, sum, units, n);
    check(raw[0] == 0x42 && raw[1] == 's' && raw[3] == 0 && raw[5] == 0xFF && raw[6] == 0xFF &&
              raw[30] == 0xFF,
          "the second entry: its one unit, the terminator, then the padding");
    fat32_long_entry(raw, 1, 2, sum, units, n);
    check(raw[0] == 0x01 && raw[1] == 'f' && raw[30] == 'r',
          "the first entry: thirteen units and no flag");
    check(fat32_short_sum((const uint8_t *)"BOOT       ") == 221,
          "the checksum of BOOT is 221, by the rotate-and-add rule");
    cp437_rows();

    if (fails == 0) {
        printf("fatname_hosttest: OK\n");
    }
    return fails > 255 ? 255 : fails;
}
