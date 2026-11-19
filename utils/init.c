/**
 * Rach2 PID 1: it reaps orphans and keeps a console session running.
 *
 * The kernel starts this as the first user process, so it is init, and the
 * kernel reparents every orphaned process to it. Its core job is three things
 * a system needs for as long as it runs: reap whatever children land on it
 * (the console session and every orphan the kernel hands over), respawn that
 * session when it exits, so the console is never left without one, and
 * continue a child that is stopped, since nothing else can. Beside that it
 * clears `/var/run`, writes the boot's utmp record, sets the resource limits,
 * applies the kernel settings files, keeps each backlight's level and the
 * mixer's across a reboot, and keeps, on an image with its configuration,
 * BusyBox's acpid running, which acts on the power, volume and brightness
 * keys. The kernel writes dirty data back by itself, so no periodic sync runs
 * here.
 * It never exits; if it did, the kernel would have no reaper, so the kernel
 * panics if init dies.
 *
 * Four artifacts are built from this one source, and none of them is named
 * plain `init`: `init-shell` runs a shell on the console, `init-login` puts
 * that console behind `/bin/login`, `init-metal` is `init-login` plus a
 * tmpfs at /tmp, the network, ifplugd and ndwatch, which follow its link and
 * its routers,
 * wpa_supplicant on a machine with a Wi-Fi radio, a supervised SSH server, the
 * system log, BusyBox's NTP daemon where /etc/ntp.conf names a server and, on
 * the GNU system's root, D-Bus's system bus and the cron daemon, and
 * `init-recovery` is the recovery system's, a shell on the console with the
 * metal build's network, services and file system check beside it. The metal
 * build exists because the machine it is for is across the room: nobody types a
 * command at its console to start a daemon, so the daemon has to be running
 * when the machine finishes booting. The console login is the same as
 * `init-login`'s beside it, and is the way back in when the network is what is
 * broken.
 *
 * On `init-login` and `init-metal`, a boot line with `rach2.console=term`
 * puts that login inside `term`, the terminal on the whole framebuffer,
 * where the screen has one; the kernel console's login comes back when
 * `term` fails (`session_ended`).
 *
 * Built like the other utils, linked against the ported musl (see
 * utils/Makefile), and staged at /sbin/init by the image manifests.
 */
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <net/if.h>
#ifdef INIT_RECOVERY
#include "gptdisk.h"
#endif

#include "vconsole.h"
#include "ip6secret.h"

extern char **environ;

/* The path of init and of every program it starts, BusyBox init's: the
 * kernel gives init `/bin:/usr/bin:/`, and services such as acpid run actions
 * that call sbin programs (rfkill, ifconfig, poweroff) by name. A console
 * login sets its own from /etc/profile. */
#define SERVICE_PATH "/sbin:/usr/sbin:/bin:/usr/bin"

#ifdef INIT_CONSOLE_LOGIN
/* The one program the console session may exec in this build. There is no
 * list and no fallback: a fall-through to a shell on any exec failure hands
 * the console to whoever is standing at it, and narrowing the fall-through to
 * ENOENT inverts the danger instead of removing it, since ENOENT is the case
 * of an image built without /bin/login, and that image above all must not
 * give a shell. So the fallback is compiled out: this artifact has no code
 * path and no string that names a shell. */
#define LOGIN_PROGRAM "/bin/login"

/* The pause, in seconds, before a session that died within two seconds is
 * started again. Longer than the shell build's one second, so a permanent
 * failure leaves its reason on the console instead of scrolling it away. */
#define LOGIN_RESPAWN_PAUSE 5

/* The console session of a boot with `TERM_OPTION` on a framebuffer: the
 * terminal on the whole screen, whose `-l` runs LOGIN_PROGRAM inside it, so
 * this path too has no shell without a login. */
#define TERM_PROGRAM "/bin/term"
#define TERM_OPTION  "rach2.console=term"

/* FB_GETINFO and the size of the struct fb_screen it fills, eleven 32-bit
 * numbers, from <rach2/fb.h>, which the utils do not include. */
#define INIT_FB_GETINFO     0x4604
#define INIT_FB_SCREEN_INTS 11
#else
/* The login shell, tried in order: /bin/sh then /ish. The recovery system's
 * init and the `init-shell` build take this path; the other images put the
 * console behind a login, and `init-shell` boots only the test images of the
 * login and shutdown suites. The matching argv[0] has a leading '-', which
 * makes ash a login shell that reads /etc/profile. */
static const char *const Shells[] = {"/bin/sh", "/ish", 0};
static const char *const Shell_Arg0[] = {"-sh", "-ish", 0};
#endif

#ifdef INIT_METAL
/*
 * The SSH server this build supervises, and how it is asked to run. An image
 * that has OpenSSH's server at `OPENSSH_PROGRAM`, which the GNU system's root
 * has, runs that; every other image runs Dropbear's.
 *
 * `dropbearmulti` dispatches on its first argument, so the program name is
 * argv[1] and the flags follow. `-F` keeps it in the foreground, which is what
 * makes it supervisable at all: without it dropbear forks and the parent
 * exits, and init would see that exit as a death and respawn it forever
 * against a daemon that is in fact running. `-R` generates the host keys that
 * are missing, which on a RAM root is every boot and needs the kernel's
 * entropy to be trusted, so such a machine whose CPU offers neither RDSEED nor
 * RDRAND will not get an SSH server (and will still get its console login,
 * which is why both are kept); an installed root keeps its keys. `-g` bars
 * root's password from the exchange, so root arrives by key, unless
 * `/etc/rach2/sshd.conf` permits it (`root_password_login`).
 *
 * OpenSSH's `sshd` takes `-D` for the same foreground and `-e` to write its
 * log to standard error, which is the service log, readable only by root, as
 * Linux keeps its authentication log from users; the system log is readable
 * by all. Its argv[0] is its absolute path, without which it does not
 * start, since a SIGHUP makes it execute itself again by that name to read
 * its configuration anew. It makes no host keys: `ssh-keygen -A` runs
 * before each start and makes those missing from `/etc/ssh`, every one on a
 * first boot, with the same need for the kernel's entropy. The same policy
 * file decides root's password, which `PermitRootLogin prohibit-password`
 * bars and `PermitRootLogin yes` permits.
 */
#define SSHD_PROGRAM    "/bin/dropbearmulti"
#define OPENSSH_PROGRAM "/usr/sbin/sshd"
#define OPENSSH_KEYGEN  "/usr/bin/ssh-keygen"

/*
 * D-Bus's system bus, which the GNU system's root has: init runs it where
 * the image has `DBUS_PROGRAM` and the user `DBUS_USER`, and no other image
 * has either. The system's programs, the browser among them, connect to it
 * at /run/dbus/system_bus_socket, and /run is a link to /var/run there, so
 * the bus's directory is `DBUS_DIR`. `MACHINE_ID` is the machine's identity,
 * which the bus and its clients read.
 */
#define DBUS_PROGRAM "/usr/bin/dbus-daemon"
#define DBUS_USER    "messagebus"
#define DBUS_DIR     "/var/run/dbus"
#define MACHINE_ID   "/etc/machine-id"

/*
 * The system log: BusyBox's syslogd in the foreground, run as its applet of
 * `BUSYBOX_PROGRAM`, which every image has, so it needs no link. It binds
 * its socket where /dev/log leads, /var/run/log, and writes what logger(1)
 * and syslog(3) send to `SYSLOG_FILE`, kept under 200 KiB with one older
 * file beside it.
 */
#define SYSLOG_FILE "/var/log/messages"

/*
 * cronie's cron daemon, which the GNU system's root has and no other image:
 * init runs it in the foreground where the image has `CRON_PROGRAM`. It
 * logs to the system log, where it also sends what a job prints, since no
 * image has a mail transfer agent.
 */
#define CRON_PROGRAM "/usr/sbin/crond"

/*
 * BusyBox's ntpd, which keeps the clock by the servers `NTP_CONF` names:
 * init runs it in the foreground, as an applet of `BUSYBOX_PROGRAM`, while
 * that file has a server line (`ntp_servers`). Of the images built here
 * only the GNU system's root installs the file. ntpd reads it when it
 * starts, and writes each step of the clock and each error to the service
 * log.
 */
#define NTP_CONF "/etc/ntp.conf"

/*
 * The interface whose link and routers init has followed where the boot line
 * hands the network to userland (`boot_wants_userland_network`): BusyBox's
 * ifplugd takes it down and up with its link through `IFPLUGD_ACTION`, so a
 * cable moved to another network gives the new network's lease at once, and
 * ndwatch(8) follows its routers' DNS servers and DHCPv6 flags. Each runs on
 * an image that has its program and while the interface exists.
 */
#define NETWORK_IFACE   "eth0"
#define IFPLUGD_ACTION  "/etc/ifplugd/ifplugd.action"
#define NDWATCH_PROGRAM "/sbin/ndwatch"

/*
 * The Wi-Fi interface, which init follows while it exists, whatever the boot
 * line says, since the kernel never configures it: wpa_supplicant joins its
 * networks with `WPA_CONF`, which wifi(1) changes through the control socket
 * `WPA_CTRL`, and `wpa_cli -a` runs `WPA_ACTION` as it joins and leaves a
 * network, which starts and stops the DHCP client on it; ndwatch follows its
 * routers as on `NETWORK_IFACE`. ifplugd is not run on it: the action's
 * `ifdown` would take down the card wpa_supplicant owns. The programs are in
 * /bin on the musl systems, whose manifests stage every program there, and
 * in /usr/sbin on the GNU root, where its port installs them; init runs the
 * first of `WPA_SUPPLICANT_PATHS` and of `WPA_CLI_PATHS` the image has.
 */
#define WIFI_IFACE           "wlan0"
#define WPA_SUPPLICANT_PATHS {"/bin/wpa_supplicant", "/usr/sbin/wpa_supplicant", 0}
#define WPA_CLI_PATHS        {"/bin/wpa_cli", "/usr/sbin/wpa_cli", 0}
#define WPA_CONF             "/etc/wpa_supplicant/wlan0.conf"
#define WPA_ACTION           "/etc/wpa_supplicant/wpa_action"
#define WPA_CTRL_DIR         "/var/run/wpa_supplicant"
#define WPA_CTRL             WPA_CTRL_DIR "/" WIFI_IFACE

/*
 * The metric of the IPv6 default route each router gives an interface,
 * written to its `ra_defrtr_metric` once the interface is there: 100 for
 * `NETWORK_IFACE` and 600 for `WIFI_IFACE`, as NetworkManager sets them and
 * as the lease script sets IPv4's, so that IPv6 too leaves by the cable
 * while it is plugged in and by Wi-Fi when it is not.
 */
#define RA_METRIC_PATH    "/proc/sys/net/ipv6/conf/%s/ra_defrtr_metric"
#define NETWORK_RA_METRIC "100"
#define WIFI_RA_METRIC    "600"
#endif

/* BusyBox, whose applets init runs by the name in argv[0]: the system log,
 * ntpd, acpid, ifplugd and sysctl. */
#define BUSYBOX_PROGRAM "/bin/busybox"
/* The tool that edits the boot partition's GRUB environment block. */
#define BOOTCTL_PROGRAM "/bin/bootctl"

/*
 * BusyBox's acpid, which runs the actions under /etc/acpi for the keys the
 * kernel reports on /dev/input/eventN: the power button, the volume keys
 * and the brightness keys. init runs it in the foreground, as an applet of
 * `BUSYBOX_PROGRAM`, in every build, on an image that has `ACPID_CONF` and
 * while the kernel has `ACPID_INPUT`: acpid opens the event devices there
 * are when it starts, and exits at once when there is none.
 */
#define ACPID_CONF  "/etc/acpid.conf"
#define ACPID_INPUT "/dev/input/event0"

/*
 * The kernel settings an image keeps, lines of `NAME = VALUE` as Linux's
 * sysctl.conf(5) has them, which init applies at boot with BusyBox's
 * `sysctl -p` (`apply_sysctl`). The installed systems set IPv6's temporary
 * addresses there.
 */
#define SYSCTL_CONF "/etc/sysctl.conf"

/*
 * The directories of settings files, NAME.conf in the same format, in the
 * order in which a file masks one of the same name in a directory after it,
 * as procps's `sysctl --system` and systemd-sysctl read them; the GNU root's
 * /usr/lib/sysctl.d has Debian's vm.max_map_count.
 */
static const char *const sysctl_dirs[] = {"/etc/sysctl.d", "/run/sysctl.d",
                                          "/usr/local/lib/sysctl.d", "/usr/lib/sysctl.d",
                                          "/lib/sysctl.d"};

/*
 * Each backlight's level is kept across a reboot, as systemd-backlight keeps
 * it on Linux: init writes it back from `BACKLIGHT_KEPT`/NAME at boot, saves
 * it there within a second of each change (`save_changed_levels`), so that
 * a reset or a power cut keeps the level set, and saves it again when it
 * stops the machine. The backlights are found at boot, at most
 * `BACKLIGHT_MAX` of them, since the kernel registers its backlights before
 * it starts init.
 */
#define BACKLIGHT_CLASS "/sys/class/backlight"
#define BACKLIGHT_KEPT  "/var/lib/backlight"
#define BACKLIGHT_MAX   4

/* A backlight whose level init keeps: the sysfs path, the saved file and
 * its two replacement paths, and the text last saved, empty if none. The
 * paths are made at boot for the shutdown handler. */
struct backlight {
    char level[128];
    char kept[128];
    char fresh[128];
    char pending[128];
    char saved[24];
};

static struct backlight backlights[BACKLIGHT_MAX];
static int backlight_count;

/*
 * The mixer's levels are kept across a reboot, as alsactl store and restore
 * keep them on Linux: init sets each channel of `/dev/mixer`'s device mask
 * from `MIXER_KEPT` at boot, saves them there within a second of each
 * change, as alsactl's daemon mode does, and again when it stops the
 * machine, one line `NAME L:R` a channel, NAME being OSS's name of it.
 * mixer(1)'s record of a mute is the file's last line, `mute L:R` with the
 * level before the mute or `mute none`, so that one rename keeps a muted
 * volume together with the level that unmutes it. `MIXER_OLD_MUTE` is
 * where an older init kept the record: read when the file has no `mute`
 * line, and removed once the file has one.
 */
#define MIXER_DEVICE   "/dev/mixer"
#define MIXER_KEPT     "/var/lib/mixer"
#define MIXER_FRESH    "/var/lib/mixer.new"
#define MIXER_PENDING  "/var/lib/mixer.tmp"
#define MIXER_MUTE     "/var/run/mixer.mute"
#define MIXER_OLD_MUTE "/var/lib/mixer.mute"
#define MIXER_LOCK     "/var/run/mixer.lock"

/* OSS's requests, with Linux's numbers: channel N's level, the left
 * channel's percentage in the low byte and the right's in the next, and the
 * mask of the channels the device has. */
#define MIXER_CHANNELS     25 /* SOUND_MIXER_NRDEVICES */
#define MIXER_VOLUME       0
#define MIXER_READ(n)      _IOR('M', n, int)
#define MIXER_WRITE(n)     _IOWR('M', n, int)
#define MIXER_READ_DEVMASK MIXER_READ(0xFE)

/* OSS's names of its channels, SOUND_DEVICE_NAMES, by number. */
static const char *const mixer_names[MIXER_CHANNELS] = {
    "vol",  "bass", "treble", "synth", "pcm",   "speaker", "line",   "mic",   "cd",
    "mix",  "pcm2", "rec",    "igain", "ogain", "line1",   "line2",  "line3", "dig1",
    "dig2", "dig3", "phin",   "phout", "video", "radio",   "monitor"};

/* The channels init keeps, read from the device mask at boot: 0 on a
 * machine with no mixer, which then keeps nothing. */
static int mixer_mask;

/* The most bytes of `MIXER_KEPT`, a line for each channel and the mute's,
 * and of a record of a mute. */
#define MIXER_TEXT      (MIXER_CHANNELS * sizeof("monitor 100:100\n") + sizeof("mute 100:100\n"))
#define MUTE_RECORD_MAX sizeof("100:100\n")

/* The text `MIXER_KEPT` holds, empty for a file there is not, so that a
 * save writes only what changed; and whether `MIXER_OLD_MUTE` is still to
 * be removed. */
static char mixer_saved[MIXER_TEXT];
static int mixer_old_mute;

#ifdef INIT_CONSOLE_LOGIN
/* Whether a session child has already failed to exec the login program in
 * this boot. init sets it in `session_ended`; the children forked after that
 * inherit it, which is how the rescue hint is printed once per boot. */
static int login_hint_given;

/**
 * Say on the console that the login program could not be run, and, the first
 * time in a boot, how to get back in.
 *
 * Written with `write`, not `printf`, because this runs in a forked child
 * that has just failed an `execve`, and a write to a descriptor depends on
 * none of the stdio state it was forked with. The second line gives the way
 * back in: `rach2.rescue` boots the kernel debug shell with the file system
 * mounted and writable. On the login build it is the only way back in, since
 * nothing there starts a network service; the metal build also has its SSH
 * server. The child exits at once, so it cannot record that the hint was
 * given; the flag it reads is init's, copied at the fork.
 */
static void report_no_login(void)
{
    static const char first[] = "init: cannot exec /bin/login\n";
    static const char second[] = "init: boot rach2.rescue for the kernel debug shell\n";

    (void)write(STDERR_FILENO, first, sizeof(first) - 1);
    if (!login_hint_given) {
        (void)write(STDERR_FILENO, second, sizeof(second) - 1);
    }
}

/* Whether the console session is `term`: the boot line asked for it and
 * the screen has a framebuffer, decided once a boot (`want_term`). */
static int term_wanted;

/* Whether the session now running, or the next one, is `term`. A failed
 * `term` clears it for one session, the kernel console's login, and the end
 * of that session sets it again. */
static int term_now;

/**
 * Say on the console how the `term` session ended, from its wait status
 * `status`, `lived` seconds after its fork, when that was a failure: an
 * exec error, a status other than 0, a signal, or an exit within two
 * seconds of the start. Returns 1 for a failure, 0 for a clean end.
 *
 * `term -l` exits 0 once the login inside it has ended, however it ended,
 * so a status other than 0 is `term`'s own failure; an exit within two
 * seconds is one, such as a font that would not load, that would repeat at
 * once if `term` were started again.
 */
static int term_failed(int status, time_t lived)
{
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "init: console: term: killed by signal %d; login on the kernel console\n",
                WTERMSIG(status));
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 127 && lived < 2) {
        fprintf(stderr, "init: console: term: not run; login on the kernel console\n");
    } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        fprintf(stderr, "init: console: term: status %d; login on the kernel console\n",
                WEXITSTATUS(status));
    } else if (lived < 2) {
        fprintf(stderr, "init: console: term: ended at its start; login on the kernel console\n");
    } else {
        return 0;
    }
    return 1;
}

/**
 * Record how the console session ended, from its wait status `status`,
 * `lived` seconds after its fork, and choose the next one. A session child
 * that exited with 127 in under two seconds is one whose exec of the login
 * program failed, and it has printed the rescue hint, so the children forked
 * after it leave the hint out.
 *
 * Returns 1 when the next session starts at once: the kernel console's
 * login after a failed `term`, which may have failed at its start and must
 * not wait out the pause of a failed login. A clean end of `term`, a
 * logout, starts `term` again, and so does any end of the login that stood
 * in for a failed one.
 */
static int session_ended(int status, time_t lived)
{
    if (term_now) {
        if (term_failed(status, lived)) {
            term_now = 0;
            return 1;
        }
        return 0;
    }
    term_now = term_wanted;
    if (lived < 2 && WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        login_hint_given = 1;
    }
    return 0;
}
#endif

/**
 * Run one keyboard tool with standard input on the physical console.
 * Return 0 only after a successful exit; init keeps its own descriptors.
 */
static int console_tool(char **argv, void *data)
{
    pid_t child;
    int status, fd;
    (void)data;
    child = fork();
    if (child < 0)
        return -1;
    if (!child) {
        fd = open("/dev/console", O_RDWR);
        if (fd < 0 || dup2(fd, STDIN_FILENO) < 0)
            _exit(126);
        if (fd > STDERR_FILENO)
            close(fd);
        execv(argv[0], argv);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/** Read the keyboard settings once, before any console session starts. */
static void configure_console(void)
{
    struct vconsole conf;
    FILE *stream = fopen("/etc/vconsole.conf", "r");
    int bad;
    if (!stream && errno == ENOENT)
        return;
    vconsole_defaults(&conf);
    bad = !stream || vconsole_read(stream, &conf);
    if (stream && fclose(stream))
        bad = 1;
    if (bad) {
        fprintf(stderr, "init: invalid or unreadable /etc/vconsole.conf; loading us\n");
        vconsole_defaults(&conf);
    }
    if (vconsole_apply(&conf, console_tool, NULL)) {
        fprintf(stderr, "init: console keyboard setup failed; tried us\n");
    }
}

/**
 * Read at most `size` - 1 bytes of the file `path` into `text` and end them
 * with a terminator. Returns the count, or -1 when the file cannot be read.
 * Uses only open, read and close, so the shutdown handler may call it.
 */
static int read_text(const char *path, char *text, int size)
{
    int fd = open(path, O_RDONLY);
    int n, error;

    if (fd < 0) {
        return -1;
    }
    n = (int)read(fd, text, (size_t)(size - 1));
    error = errno;
    close(fd);
    if (n < 0) {
        errno = error;
        return -1;
    }
    text[n] = '\0';
    return n;
}

/**
 * Read the settings file `kept` into `text`, as `read_text` does, or, when
 * there is none, its replacement `fresh`, when that ends in a newline:
 * KRFFS's rename removes the old file before the new one takes its name,
 * so a stop between can leave only `fresh`, which `replace_file` flushes
 * before touching `kept`. Returns the count, or -1 for neither; sets
 * `*from_fresh` when the text is `fresh`'s.
 */
static int read_kept(const char *kept, const char *fresh, char *text, int size, int *from_fresh)
{
    int n = read_text(kept, text, size);

    *from_fresh = 0;
    if (n >= 0 || errno != ENOENT) {
        return n;
    }
    n = read_text(fresh, text, size);
    if (n <= 0 || text[n - 1] != '\n') {
        text[0] = '\0';
        return -1;
    }
    *from_fresh = 1;
    return n;
}

/**
 * Read the decimal number at the start of `text` into `*value`. Returns 0,
 * or -1 when `text` does not hold a number, not negative, and a newline or
 * its end.
 */
static int parse_number(const char *text, long *value)
{
    char *end;

    errno = 0;
    *value = strtol(text, &end, 10);
    if (end == text || (*end != '\0' && *end != '\n') || errno == ERANGE || *value < 0) {
        return -1;
    }
    return 0;
}

/**
 * Read the decimal number in the file `path` into `*value`. Returns 0, or -1
 * when the file cannot be read or does not hold a number and a newline.
 */
static int read_number(const char *path, long *value)
{
    char text[24];

    if (read_text(path, text, sizeof(text)) <= 0) {
        return -1;
    }
    return parse_number(text, value);
}

/**
 * Write the level kept for the backlight `b`, named `name`, back to it, and
 * note the file's text in `b->saved`. The level is kept between a twentieth
 * of the backlight's `max_brightness`, and at least 1, and that maximum, as
 * systemd-backlight keeps it, so that a level saved at 0 does not start the
 * machine with its screen dark. A backlight with no level kept is left as
 * the kernel found it. A level read from the replacement (`read_kept`) is
 * noted as no text, so that the next save writes the file again.
 */
static void restore_backlight(struct backlight *b, const char *name)
{
    char path[sizeof(BACKLIGHT_CLASS) + NAME_MAX + sizeof("/max_brightness")];
    long level, max, least;
    FILE *f;
    int ok = 0, from_fresh;

    if (read_kept(b->kept, b->fresh, b->saved, sizeof(b->saved), &from_fresh) < 0) {
        b->saved[0] = '\0';
        return;
    }
    ok = parse_number(b->saved, &level) == 0;
    if (from_fresh) {
        b->saved[0] = '\0';
    }
    if (!ok) {
        return;
    }
    ok = 0;
    snprintf(path, sizeof(path), BACKLIGHT_CLASS "/%s/max_brightness", name);
    if (read_number(path, &max) != 0 || max <= 0) {
        return;
    }
    least = max / 20 > 0 ? max / 20 : 1;
    level = level < least ? least : level > max ? max : level;
    f = fopen(b->level, "w");
    if (f) {
        ok = fprintf(f, "%ld\n", level) >= 0;
        ok = fclose(f) == 0 && ok;
    }
    if (!f || !ok) {
        fprintf(stderr, "init: backlight: %s: cannot restore: %s\n", name, strerror(errno));
    }
}

/**
 * Find the backlights in `BACKLIGHT_CLASS`, make each one's paths in
 * `backlights` and write its kept level back (`restore_backlight`). A
 * backlight whose name makes a path too long for `struct backlight` is not
 * kept.
 */
static void keep_backlights(void)
{
    DIR *d = opendir(BACKLIGHT_CLASS);
    struct dirent *e;

    if (!d) {
        return;
    }
    while (backlight_count < BACKLIGHT_MAX && (e = readdir(d)) != NULL) {
        struct backlight *b = &backlights[backlight_count];

        if (e->d_name[0] == '.') {
            continue;
        }
        if (snprintf(b->level, sizeof(b->level), BACKLIGHT_CLASS "/%s/brightness", e->d_name) >=
                (int)sizeof(b->level) ||
            snprintf(b->kept, sizeof(b->kept), BACKLIGHT_KEPT "/%s", e->d_name) >=
                (int)sizeof(b->kept) ||
            snprintf(b->fresh, sizeof(b->fresh), BACKLIGHT_KEPT "/%s.new", e->d_name) >=
                (int)sizeof(b->fresh) ||
            snprintf(b->pending, sizeof(b->pending), BACKLIGHT_KEPT "/%s.tmp", e->d_name) >=
                (int)sizeof(b->pending)) {
            continue;
        }
        backlight_count++;
        restore_backlight(b, e->d_name);
    }
    closedir(d);
}

/**
 * Make `path` if missing and flush the entry in `parent`. Flush an existing
 * entry too, since the last attempt may have made it and failed to flush it.
 * Returns 0 once the entry is durable, or -1 on failure.
 */
static int keep_directory(const char *path, const char *parent)
{
    int fd, rc;

    if ((mkdir(path, 0755) != 0 && errno != EEXIST) ||
        (fd = open(parent, O_RDONLY | O_DIRECTORY)) < 0) {
        return -1;
    }
    rc = fsync(fd);
    return close(fd) == 0 ? rc : -1;
}

/**
 * Make a new file at `path` and flush its `n` bytes of `text`. The caller
 * holds another copy of any settings that are needed from that path.
 * Returns 0, or -1 on failure.
 */
static int write_replacement(const char *path, const char *text, int n)
{
    int fd, ok;

    if (unlink(path) != 0 && errno != ENOENT) {
        return -1;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        return -1;
    }
    ok = write(fd, text, (size_t)n) == n && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (!ok) {
        (void)unlink(path);
    }
    return ok ? 0 : -1;
}

/**
 * Replace `kept` with `n` bytes of `text`, using `pending` and `fresh` in
 * the same open directory `dir`. KRFFS can lose both names in a rename
 * over an existing file. Flush a separate copy at `fresh` before that
 * rename, and preserve an existing `fresh` when `kept` is missing. Return
 * 0 once the final directory flush succeeds, or -1 on failure.
 */
static int replace_file(int dir, const char *kept, const char *fresh, const char *pending,
                        const char *text, int n)
{
    struct stat st;

    if (stat(kept, &st) == 0) {
        if (fsync(dir) != 0 || write_replacement(fresh, text, n) != 0) {
            return -1;
        }
    } else if (errno != ENOENT) {
        return -1;
    }
    if (write_replacement(pending, text, n) != 0 || fsync(dir) != 0 || rename(pending, kept) != 0 ||
        fsync(dir) != 0) {
        return -1;
    }
    (void)unlink(fresh);
    return 0;
}

/**
 * Whether the `n` bytes of `text` differ from the string `saved`. Uses no
 * library call, so the shutdown handler may call it.
 */
static int text_differs(const char *text, int n, const char *saved)
{
    int i;

    for (i = 0; i < n; i++) {
        if (saved[i] != text[i]) {
            return 1;
        }
    }
    return saved[n] != '\0';
}

/**
 * Note the `n` bytes of `text` in `saved`, of `size` bytes, as the text its
 * file now holds, or an empty text when they do not fit, which a later save
 * then writes again. Uses no library call.
 */
static void note_saved(char *saved, int size, const char *text, int n)
{
    int i;

    if (n >= size) {
        n = 0;
    }
    for (i = 0; i < n; i++) {
        saved[i] = text[i];
    }
    saved[n] = '\0';
}

/**
 * Save each kept backlight's level to its file under `BACKLIGHT_KEPT`
 * through `replace_file`, making the directory when it is missing: every
 * level when `all`, as the shutdown handler asks, else only a level that
 * differs from what its file holds. Makes only system calls a handler may
 * make; a backlight whose level cannot be read keeps the file it had, and
 * one whose save fails is noted as unsaved, so that the next pass writes it
 * whatever its level then.
 */
static void save_backlights(int all)
{
    int dir = -1, i;

    for (i = 0; i < backlight_count; i++) {
        struct backlight *b = &backlights[i];
        char text[24];
        int n = read_text(b->level, text, sizeof(text));

        if (n > 0 && (all || text_differs(text, n, b->saved))) {
            if (dir < 0 && (keep_directory("/var/lib", "/var") != 0 ||
                            keep_directory(BACKLIGHT_KEPT, "/var/lib") != 0 ||
                            (dir = open(BACKLIGHT_KEPT, O_RDONLY | O_DIRECTORY)) < 0)) {
                return;
            }
            /* A failure may follow the rename, which leaves the file's
             * text unknown: none is noted, so the next pass writes again. */
            note_saved(b->saved, (int)sizeof(b->saved), text,
                       replace_file(dir, b->kept, b->fresh, b->pending, text, n) == 0 ? n : 0);
        }
    }
    if (dir >= 0) {
        close(dir);
    }
}

/**
 * Parse a percentage, 0 to 100, from the text at `*p`, and move `*p` past
 * it. Returns it, or -1 when there is none there.
 */
static int mixer_percentage(const char **p)
{
    const char *s = *p;
    int v = 0;

    if (*s < '0' || *s > '9') {
        return -1;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
        if (v > 100) {
            return -1;
        }
    }
    *p = s;
    return v;
}

/**
 * Whether `p` is a record of a mute as mixer(1) writes it, `L:R` and a
 * newline and nothing after. Uses no library call, so the shutdown handler
 * may call it.
 */
static int mute_record(const char *p)
{
    return mixer_percentage(&p) >= 0 && *p++ == ':' && mixer_percentage(&p) >= 0 && *p++ == '\n' &&
           *p == '\0';
}

/**
 * Parse the levels kept in `text`, lines of `NAME L:R` and `mute L:R` or
 * `mute none`, into `levels`, a level in OSS's form for each channel
 * named, -1 for the rest, and the record of a mute into `mute`, of
 * `MUTE_RECORD_MAX` bytes, empty for none; `*has_mute` says whether a
 * `mute` line was there. Returns 0, or -1 when a line is neither.
 */
static int parse_mixer(const char *text, int levels[MIXER_CHANNELS], char *mute, int *has_mute)
{
    const char *p = text;
    int i;

    for (i = 0; i < MIXER_CHANNELS; i++) {
        levels[i] = -1;
    }
    mute[0] = '\0';
    *has_mute = 0;
    while (*p != '\0') {
        size_t len = strcspn(p, " ");
        const char *start;
        int left, right;

        if (len == 4 && strncmp(p, "mute ", 5) == 0) {
            p += 5;
            *has_mute = 1;
            if (strncmp(p, "none\n", 5) == 0) {
                mute[0] = '\0';
                p += 5;
                continue;
            }
            start = p;
            if (mixer_percentage(&p) < 0 || *p++ != ':' || mixer_percentage(&p) < 0 ||
                *p++ != '\n' || (size_t)(p - start) >= MUTE_RECORD_MAX) {
                return -1;
            }
            memcpy(mute, start, (size_t)(p - start));
            mute[p - start] = '\0';
            continue;
        }
        for (i = 0; i < MIXER_CHANNELS; i++) {
            if (strlen(mixer_names[i]) == len && strncmp(p, mixer_names[i], len) == 0) {
                break;
            }
        }
        if (i == MIXER_CHANNELS || p[len] != ' ') {
            return -1;
        }
        p += len + 1;
        if ((left = mixer_percentage(&p)) < 0 || *p++ != ':' ||
            (right = mixer_percentage(&p)) < 0 || *p++ != '\n') {
            return -1;
        }
        levels[i] = left | right << 8;
    }
    return 0;
}

/**
 * Find the channels of `MIXER_DEVICE` in its device mask, keep them in
 * `mixer_mask`, and set each one kept in `MIXER_KEPT`, or its replacement
 * (`read_kept`), to its kept level; then put mixer(1)'s record of a mute
 * back from the file's `mute` line, or from `MIXER_OLD_MUTE` when it has
 * none. A machine with no mixer, or a missing file, leaves the driver's
 * levels alone; so does a malformed file, after a diagnostic. Called after
 * `/var/run` is emptied, which removes the record a boot would otherwise
 * inherit.
 */
static void keep_mixer(void)
{
    char text[MIXER_TEXT];
    char mute[MUTE_RECORD_MAX];
    int levels[MIXER_CHANNELS];
    int fd = open(MIXER_DEVICE, O_RDWR | O_CLOEXEC);
    int mask, i, n, has_mute, from_fresh;

    if (fd < 0) {
        return;
    }
    if (ioctl(fd, MIXER_READ_DEVMASK, &mask) != 0 || (mask &= (1 << MIXER_CHANNELS) - 1) == 0) {
        close(fd);
        return;
    }
    mixer_mask = mask;
    mixer_old_mute = access(MIXER_OLD_MUTE, F_OK) == 0;
    n = read_kept(MIXER_KEPT, MIXER_FRESH, text, sizeof(text), &from_fresh);
    if (n < 0) {
        close(fd);
        return;
    }
    if (!from_fresh) {
        note_saved(mixer_saved, (int)sizeof(mixer_saved), text, n);
    }
    if (n == (int)sizeof(text) - 1 || parse_mixer(text, levels, mute, &has_mute) != 0) {
        fprintf(stderr, "init: mixer: " MIXER_KEPT ": malformed; levels left alone\n");
        close(fd);
        return;
    }
    for (i = 0; i < MIXER_CHANNELS; i++) {
        int level = levels[i];

        if ((mask & 1 << i) && level >= 0 && ioctl(fd, MIXER_WRITE(i), &level) != 0) {
            fprintf(stderr, "init: mixer: %s: cannot restore: %s\n", mixer_names[i],
                    strerror(errno));
        }
    }
    close(fd);
    if (!has_mute && (read_text(MIXER_OLD_MUTE, mute, sizeof(mute)) <= 0 || !mute_record(mute))) {
        mute[0] = '\0';
    }
    n = (int)strlen(mute);
    if (n > 0 && (fd = open(MIXER_MUTE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) >= 0) {
        if (write(fd, mute, (size_t)n) != n) {
            fprintf(stderr, "init: mixer: " MIXER_MUTE ": cannot restore: %s\n", strerror(errno));
        }
        close(fd);
    }
}

/**
 * Write the decimal `v`, 0 to 999, at `p`. Returns the end of what was
 * written. snprintf is not one of the functions a signal handler may call.
 */
static char *put_decimal(char *p, int v)
{
    if (v >= 100) {
        *p++ = (char)('0' + v / 100);
    }
    if (v >= 10) {
        *p++ = (char)('0' + v / 10 % 10);
    }
    *p++ = (char)('0' + v % 10);
    return p;
}

/**
 * Read the level of each channel in `mixer_mask`, and mixer(1)'s record of
 * a mute as the `mute` line, into `text`, of `MIXER_TEXT` bytes, as
 * `MIXER_KEPT` holds them. Hold a shared flock on `MIXER_LOCK` while
 * reading them, which root's mixer(1) changes under an exclusive one; the
 * file is mode 0600, so that no ordinary process can hold it, as it can
 * hold `/dev/mixer`, and stop the saves. With no lock file to open, the
 * levels are read unlocked. Returns the text's length, or -1 for a lock
 * held, a device that cannot be read or a record that cannot be. Makes
 * only system calls a handler may make.
 */
static int mixer_snapshot(char *text)
{
    char mute[MUTE_RECORD_MAX + 1];
    const char *q = "none\n";
    char *p = text;
    int fd, lock, i, n, volume = -1, ok = 1;

    if (mixer_mask == 0 || (fd = open(MIXER_DEVICE, O_RDWR)) < 0) {
        return -1;
    }
    lock = open(MIXER_LOCK, O_RDWR | O_CREAT, 0600);
    if (lock >= 0 && flock(lock, LOCK_SH | LOCK_NB) != 0) {
        close(lock);
        close(fd);
        return -1;
    }
    for (i = 0; i < MIXER_CHANNELS; i++) {
        const char *name = mixer_names[i];
        int level;

        if (!(mixer_mask & 1 << i)) {
            continue;
        }
        if (ioctl(fd, MIXER_READ(i), &level) != 0) {
            ok = 0;
            break;
        }
        if (i == MIXER_VOLUME) {
            volume = level;
        }
        while (*name != '\0') {
            *p++ = *name++;
        }
        *p++ = ' ';
        p = put_decimal(p, level & 0xFF);
        *p++ = ':';
        p = put_decimal(p, level >> 8 & 0xFF);
        *p++ = '\n';
    }
    if (ok && volume == 0) {
        n = read_text(MIXER_MUTE, mute, sizeof(mute));
        if (n < 0 && errno != ENOENT) {
            ok = 0;
        } else if (n > 0 && n < (int)sizeof(mute) - 1 && mute_record(mute)) {
            q = mute;
        }
    }
    if (lock >= 0) {
        close(lock);
    }
    close(fd);
    if (!ok) {
        return -1;
    }
    for (i = 0; i < 5; i++) {
        *p++ = "mute "[i];
    }
    while (*q != '\0') {
        *p++ = *q++;
    }
    return (int)(p - text);
}

/**
 * Save the mixer's levels and its record of a mute (`mixer_snapshot`) to
 * `MIXER_KEPT` in one file: when `all`, as the shutdown handler asks, else
 * only when it differs from what the file holds. Once the file is written,
 * `MIXER_OLD_MUTE` goes. A save that fails is noted as unsaved, so that the
 * next pass writes it whatever the levels then. A lock held is left to the
 * next pass. Makes only system calls a handler may make.
 */
static void save_mixer(int all)
{
    char text[MIXER_TEXT];
    int dir, n = mixer_snapshot(text);

    if (n < 0 || (!all && !text_differs(text, n, mixer_saved)) ||
        keep_directory("/var", "/") != 0 || keep_directory("/var/lib", "/var") != 0 ||
        (dir = open("/var/lib", O_RDONLY | O_DIRECTORY)) < 0) {
        return;
    }
    if (replace_file(dir, MIXER_KEPT, MIXER_FRESH, MIXER_PENDING, text, n) != 0) {
        /* The file's text is unknown after a failure past the rename. */
        mixer_saved[0] = '\0';
    } else {
        note_saved(mixer_saved, (int)sizeof(mixer_saved), text, n);
        if (mixer_old_mute) {
            (void)unlink(MIXER_OLD_MUTE);
            mixer_old_mute = 0;
        }
    }
    close(dir);
}

static int shutdown_block(sigset_t *saved);

/**
 * Save the backlights' and the mixer's levels that changed since they were
 * last saved, at most once a second: init's loop calls this on each turn,
 * so that a level set survives a reset or a power cut as well as a
 * shutdown. Linux's systemd-backlight saves only at the shutdown; alsactl's
 * daemon mode saves the mixer when it changes, as this does for both.
 */
static void save_changed_levels(void)
{
    static time_t last;
    time_t now = time(0);
    sigset_t saved;

    if (now == last || shutdown_block(&saved) != 0) {
        return;
    }
    last = now;
    save_backlights(0);
    save_mixer(0);
    (void)sigprocmask(SIG_SETMASK, &saved, NULL);
}

/* How long a shutdown waits for the processes to end, in milliseconds:
 * after SIGTERM, which an interactive shell ignores, and then after
 * SIGKILL, which a process in a disk wait takes only when the wait ends. */
#define END_TERM_MS 1000
#define END_KILL_MS 5000

/**
 * Reap init's children until none is left, every process having been
 * reparented to init as its parent ended, or until `ms` milliseconds have
 * passed. Async-signal-safe, for `shutdown_handler`.
 */
static void reap_for(long ms)
{
    struct timespec step = {0, 10000000L};
    long waited;
    pid_t pid;

    for (waited = 0; waited <= ms; waited += 10) {
        while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        }
        if (pid < 0 && errno == ECHILD) {
            return;
        }
        nanosleep(&step, NULL);
    }
}

/**
 * End every other process before a shutdown, as Linux's inits do: SIGTERM,
 * a wait of END_TERM_MS for them to end, then SIGKILL and a wait of
 * END_KILL_MS, each wait over once none is left. A file a process held
 * open is closed at its end, and one removed while open is freed then, so
 * that the kernel can mark each volume clean. Async-signal-safe.
 */
static void end_processes(void)
{
    if (kill(-1, SIGTERM) == 0) {
        reap_for(END_TERM_MS);
    }
    if (kill(-1, SIGKILL) == 0) {
        reap_for(END_KILL_MS);
    }
}

/**
 * Stop the machine as the signal `sig` selects. Does not return.
 *
 * Installed for the three signals busybox's `halt`, `poweroff` and `reboot`
 * send to pid 1, with busybox's own mapping: SIGUSR1 halts, SIGUSR2 powers off,
 * SIGTERM restarts.
 *
 * The periodic save blocks these signals while it replaces the files. This
 * handler saves the final levels with system calls alone and calls `reboot`.
 * A failed `reboot` falls to `_exit`, and the kernel panics when init dies.
 * All three shutdown signals are blocked while the handler runs.
 *
 * Every other process is ended first (`end_processes`), so that its files
 * and mixer locks are released before the final save. The kernel disables
 * scheduling and writes out each file system on a disk, the root last,
 * before it stops the machine, and stops it even when a write fails.
 */
static void shutdown_handler(int sig)
{
    int command = RB_AUTOBOOT;

    if (sig == SIGUSR1) {
        command = RB_HALT_SYSTEM;
    } else if (sig == SIGUSR2) {
        command = RB_POWER_OFF;
    }
    end_processes();
    save_backlights(1);
    save_mixer(1);
    reboot(command);
    _exit(1);
}

/**
 * Install `shutdown_handler` for SIGUSR1, SIGUSR2 and SIGTERM, with all three
 * blocked while it runs.
 */
static void catch_shutdown(void)
{
    struct sigaction stop;

    memset(&stop, 0, sizeof(stop));
    stop.sa_handler = shutdown_handler;
    sigemptyset(&stop.sa_mask);
    sigaddset(&stop.sa_mask, SIGUSR1);
    sigaddset(&stop.sa_mask, SIGUSR2);
    sigaddset(&stop.sa_mask, SIGTERM);
    (void)sigaction(SIGUSR1, &stop, NULL);
    (void)sigaction(SIGUSR2, &stop, NULL);
    (void)sigaction(SIGTERM, &stop, NULL);
}

/* Where a service's output goes once the console belongs to the person at it.
 *
 * The kernel stops writing to the screen when init starts, and a service that
 * kept the console would undo that from the other side: the DHCP client and
 * the SSH server would print over the login prompt. They write here instead,
 * and a person reads the file.
 *
 * It is bounded, not rotated: `bound_service_log` empties it in place when it
 * reaches the cap, which the services' append descriptors survive. Rotation
 * would keep the older records instead of dropping them, and needs a collector
 * that owns the writing, because renaming a file does not move the descriptors
 * running services already hold.
 *
 * Nor does it hold everything a service says. BusyBox's `udhcpc` is started
 * with `-b`, and once it has a lease it backgrounds itself and replaces its
 * own descriptors with /dev/null, so renewals and lease losses reach neither
 * this file nor the screen. What is here is what a service printed before it
 * backgrounded. Both limits are written down in utils/Readme.md.
 */
#define SERVICE_LOG             "/var/log/services"
#define SERVICE_LOG_CAP         (256 * 1024)
/* Turns of init's loop between two looks at the service log. An idle turn is a
   second, so this is about a minute on a quiet machine and less on a busy one. */
#define SERVICE_LOG_CHECK_TURNS 60

/* The service log and `/dev/null`, both close-on-exec, or -1 when not open. */
static int service_log_fd = -1;
static int service_null_fd = -1;

/**
 * Empty the service log when it has reached `SERVICE_LOG_CAP` bytes, and
 * write a line that says so. Nothing is done when the log is not open.
 *
 * The services hold descriptors opened `O_APPEND`, so truncating the file to
 * nothing is enough: their next write lands at the start of it and nothing has
 * to be reopened or renamed. Every record the log held is lost, and in return
 * one service that repeats itself cannot fill the root, which on the recovery
 * system is memory.
 */
static void bound_service_log(void)
{
    struct stat st;
    static const char note[] = "init: the service log reached its limit and was emptied\n";

    if (service_log_fd < 0 || fstat(service_log_fd, &st) != 0) {
        return;
    }
    if (st.st_size < SERVICE_LOG_CAP) {
        return;
    }
    if (ftruncate(service_log_fd, 0) == 0) {
        (void)write(service_log_fd, note, sizeof(note) - 1);
    }
}

/**
 * Open the service log, removing it first when the last boots left it at the
 * cap, and `/dev/null` as its fallback. On a machine with no writable /var
 * the log's descriptor stays -1 and the services write to `/dev/null`, never
 * to the console.
 *
 * `/dev/null` is opened read-write because it stands in for the log when there
 * is no log, and a service redirected onto a read-only descriptor would fail
 * every write rather than discard it. Both descriptors are close-on-exec: a
 * child gets them only through `service_output`.
 */
static void open_service_log(void)
{
    struct stat st;

    service_null_fd = open("/dev/null", O_RDWR);
    if (service_null_fd >= 0) {
        (void)fcntl(service_null_fd, F_SETFD, FD_CLOEXEC);
    }

    if (stat(SERVICE_LOG, &st) == 0 && st.st_size >= SERVICE_LOG_CAP) {
        (void)unlink(SERVICE_LOG);
    }
    service_log_fd = open(SERVICE_LOG, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (service_log_fd >= 0) {
        (void)fcntl(service_log_fd, F_SETFD, FD_CLOEXEC);
    }
}

/** The service log's descriptor, or `/dev/null`'s when the log is not open,
 * or -1 when neither is. */
static int service_sink(void)
{
    return service_log_fd >= 0 ? service_log_fd : service_null_fd;
}

/**
 * In a child, point standard input at `/dev/null` and standard output and
 * error at the service log, or at `/dev/null` when there is no log. A child
 * whose redirection fails exits with status 126.
 */
static void service_output(void)
{
    int sink = service_sink();

    if (service_null_fd < 0 || sink < 0 || dup2(service_null_fd, 0) < 0 || dup2(sink, 1) < 0 ||
        dup2(sink, 2) < 0) {
        _exit(126);
    }
}

#ifdef INIT_METAL
/* Root's authorized keys, which `anyone_to_let_in` reads and `take_disk_keys`
 * adds to. */
#define RECOVERY_KEYS "/root/.ssh/authorized_keys"

/**
 * Whether this image has anybody to let in over SSH: 1 when it has, so the
 * SSH server is started, and 0 when it has not.
 *
 * On the recovery build the server starts where root has an authorized key
 * and nowhere else, which makes a credential a property of the image rather
 * than a switch in the code. A stick built for development has the owner's
 * key and can update a machine remotely; one built for anybody else has none,
 * starts no server, and is reachable only after a person puts a key on it. The
 * recovery partition of an installed disk is the same: it starts no server
 * unless its image has a key or the installer put one beside it, which
 * `take_disk_keys` adds to root's before this is first called.
 *
 * Root's own file is the one that counts. `--update` and a repair are done as
 * root, and an image that let an unprivileged account in but had no root key
 * could not do either. The file must be a regular one with a line that is
 * neither blank nor a comment. What such a line says is for the server to
 * judge: an entry may begin with options (`no-port-forwarding
 * ssh-ed25519 ...`), and a closer reading here would fail entries Dropbear
 * accepts and pass lines it does not.
 *
 * The metal build always returns 1. An installed system's policy is its own:
 * the person installing decides who may log in, and the installer puts the
 * account's key in the account's own directory and writes the SSH policy for
 * root. Root's directory is empty by design, so a test of root's keys would
 * leave every freshly installed machine with no server at all.
 */
static int anyone_to_let_in(void)
{
#ifndef INIT_RECOVERY
    return 1;
#else
    struct stat st;
    FILE *f;
    char line[512];
    int usable = 0;

    if (stat(RECOVERY_KEYS, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return 0;
    }
    f = fopen(RECOVERY_KEYS, "r");
    if (!f) {
        return 0;
    }
    while (!usable && fgets(line, sizeof(line), f)) {
        char *p = line;

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p != '\0' && *p != '\n' && *p != '\r' && *p != '#') {
            usable = 1;
        }
    }
    fclose(f);
    return usable;
#endif
}
#endif /* INIT_METAL */

/**
 * Fork the console session and return its pid (-1 on failure): `/bin/login`
 * on the login builds, or `term -l` there while `term_now` says so, and the
 * first of `Shells` that executes on the others. A child that executes
 * nothing exits with status 127, whatever the error, and on the login builds
 * `session_ended` reads that status.
 *
 * The child does what getty does before exec: `setsid` makes it a new
 * session's leader (with no controlling terminal), then `ioctl(TIOCSCTTY)`
 * claims the console as that session's controlling terminal. The program thus
 * starts as a session leader that already owns the terminal, so a
 * job-control shell like busybox ash, which checks `tcgetpgrp` at startup,
 * finds itself the foreground. That is why init never has the console as
 * its own controlling terminal: it gives the console to each session it
 * starts, as a Unix init does through getty.
 *
 * On the login builds the child sets `LOGIN_TIMEOUT=0` before the exec.
 * BusyBox's login, the `/bin/login` of the images that stage BusyBox,
 * otherwise gives up after 60 seconds at its prompt and exits, and the login
 * init starts next would draw a new prompt on an idle console every minute.
 * The os-simple image's own login has no timeout and does not read it.
 * `term` passes the variable on to the login it runs.
 */
static pid_t spawn_shell(void)
{
    pid_t pid = fork();
#ifndef INIT_CONSOLE_LOGIN
    int i;
#endif

    if (pid != 0) {
        return pid;
    }

    setsid();
    ioctl(STDIN_FILENO, TIOCSCTTY, (void *)0);
#ifdef INIT_CONSOLE_LOGIN
    (void)setenv("LOGIN_TIMEOUT", "0", 1);
    if (term_now) {
        static const char gone[] = "init: cannot exec /bin/term\n";
        char *argv[3];

        argv[0] = (char *)"term";
        argv[1] = (char *)"-l";
        argv[2] = 0;
        execve(TERM_PROGRAM, argv, environ);
        (void)write(STDERR_FILENO, gone, sizeof(gone) - 1);
        _exit(127);
    }
    {
        char *argv[2];
        argv[0] = (char *)"login";
        argv[1] = 0;
        execve(LOGIN_PROGRAM, argv, environ);
    }
    report_no_login();
    _exit(127);
#else
    for (i = 0; Shells[i] != 0; i++) {
        char *argv[2];
        argv[0] = (char *)Shell_Arg0[i];
        argv[1] = 0;
        execve(Shells[i], argv, environ);
    }
    _exit(127);
#endif
    return -1; /* unreachable; quiets the compiler */
}

/**
 * Block init's shutdown signals, SIGUSR1, SIGUSR2 and SIGTERM, before a fork,
 * and store the mask as it was in `saved`. Returns 0, or -1 when the mask
 * cannot be changed. A child forked with them blocked cannot run init's
 * handlers, which would shut the machine down, before `shutdown_defaults`.
 */
static int shutdown_block(sigset_t *saved)
{
    sigset_t shutdown;

    sigemptyset(&shutdown);
    sigaddset(&shutdown, SIGUSR1);
    sigaddset(&shutdown, SIGUSR2);
    sigaddset(&shutdown, SIGTERM);
    return sigprocmask(SIG_BLOCK, &shutdown, saved);
}

/**
 * In a child forked under `shutdown_block`, give the shutdown signals their
 * default actions and put back the mask `saved`, so that one sent to the
 * child ends it. Returns 0, or -1 on failure.
 */
static int shutdown_defaults(const sigset_t *saved)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0 || sigaction(SIGUSR2, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0 || sigprocmask(SIG_SETMASK, saved, NULL) != 0) {
        return -1;
    }
    return 0;
}

/* The pause, in seconds, after a service that died almost at once, the SSH
 * server among them (`service_ended`). Long enough that a server failing for
 * a permanent reason (no entropy, no network, a port already taken) does not
 * respawn at full speed, and short enough that a machine reachable only over
 * the network is reachable again quickly. */
#define SERVICE_RESPAWN_PAUSE 5

/* A service init keeps running: its pid, -1 while it is not running, and
 * when it was last started. */
struct service {
    pid_t pid;
    time_t spawned;
};

/**
 * Whether `reaped`, what `waitpid` returned, is the pid of the service `s`.
 * When it is, the service is marked not running, and when it ended within
 * two seconds of its start, the call first sleeps `SERVICE_RESPAWN_PAUSE`
 * seconds. A `reaped` of 0 or less never matches, since -1 is both a failed
 * wait and the pid of a service not running.
 */
static int service_ended(struct service *s, pid_t reaped)
{
    if (reaped <= 0 || reaped != s->pid) {
        return 0;
    }
    s->pid = -1;
    if (time(0) - s->spawned < 2) {
        sleep(SERVICE_RESPAWN_PAUSE);
    }
    return 1;
}

/** Whether to run acpid now: the image has `ACPID_CONF` and the kernel has
 * an event device. Asked on each turn that finds acpid not running. */
static int acpid_wanted(void)
{
    return access(ACPID_CONF, R_OK) == 0 && access(ACPID_INPUT, F_OK) == 0;
}

/**
 * Fork BusyBox's acpid and return its pid (-1 on failure), with the shutdown
 * signals blocked across the fork as for the SSH server. `-d` keeps it in
 * the foreground and writes each action it runs to standard error, the
 * service log, where what the action prints goes too. acpid reads its map
 * and its actions, /etc/acpi.map and `ACPID_CONF`, when it starts.
 */
static pid_t spawn_acpid(void)
{
    sigset_t saved;
    pid_t pid;

    if (shutdown_block(&saved) != 0) {
        return -1;
    }
    pid = fork();
    if (pid != 0) {
        (void)sigprocmask(SIG_SETMASK, &saved, NULL);
        return pid;
    }
    if (shutdown_defaults(&saved) != 0) {
        _exit(126);
    }
    service_output();
    {
        static const char msg[] = "init: cannot exec " BUSYBOX_PROGRAM " acpid\n";
        char *argv[3];

        argv[0] = (char *)"acpid";
        argv[1] = (char *)"-d";
        argv[2] = 0;
        execve(BUSYBOX_PROGRAM, argv, environ);
        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
    _exit(127);
    return -1; /* unreachable; quiets the compiler */
}

/** Start acpid, the service `s`, when it is not running and `acpid_wanted`.
 * A failed fork is left for the next turn. */
static void start_acpid(struct service *s)
{
    if (s->pid < 0 && acpid_wanted()) {
        s->pid = spawn_acpid();
        s->spawned = time(0);
    }
}

/**
 * Read the first 512 bytes of the superblock of the partition node `node`,
 * which starts at byte 1024, into `sb`. Returns 1 when the node opens and the
 * superblock has ext2's magic at its byte 56, and 0 otherwise.
 */
static int ext2_superblock(const char *node, unsigned char sb[512])
{
    int fd = open(node, O_RDONLY);
    int ext2;

    if (fd < 0) {
        return 0;
    }
    ext2 = pread(fd, sb, 512, 1024) == 512 && sb[56] == 0x53 && sb[57] == 0xEF;
    close(fd);
    return ext2;
}

/**
 * Put in `uuid` the UUID of the ext2 volume whose superblock, from its byte
 * 0, is `sb`, as `blkid` prints it: 36 bytes of lower-case hexadecimal and
 * dashes, and a NUL.
 */
static void ext2_uuid(const unsigned char *sb, char uuid[37])
{
    const unsigned char *u = sb + 104; /* s_uuid */

    snprintf(uuid, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0],
             u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13],
             u[14], u[15]);
}

#ifdef INIT_METAL
/* The kernel's own limits on disks and on a disk's partition numbers, DISK_MAX
 * in rach2/sys/block/disk.h and PART_MAX in rach2/sys/block/partition.h. */
#define DISK_NODES 8
#define PART_NODES 15

/** The place of `uuid` among the comma-separated UUIDs of `list`, counted
 *  from 0, or -1 when it is not there. */
static int uuid_place(const char *list, const char *uuid)
{
    int place = 0;

    while (*list != '\0') {
        if (strncmp(list, uuid, 36) == 0 && (list[36] == ',' || list[36] == '\0')) {
            return place;
        }
        list = strchr(list, ',');
        if (!list) {
            break;
        }
        list++;
        place++;
    }
    return -1;
}

/**
 * Run `e2fsck -p` on the partition node `node`, wait for it and say its
 * status on the console. Returns the status, 8 for a check that was not
 * waited for or did not exit, or -1 when no fork succeeded: a fork that fails
 * is retried up to five times a second apart, because a check skipped is a
 * repair not made.
 */
static int run_e2fsck(char *node)
{
    pid_t pid;
    int status = 0, tries;

    for (tries = 0; (pid = fork()) < 0 && tries < 5; tries++) {
        sleep(1);
    }
    if (pid == 0) {
        char *argv[4];

        argv[0] = (char *)"e2fsck";
        argv[1] = (char *)"-p";
        argv[2] = node;
        argv[3] = 0;
        execve("/sbin/e2fsck", argv, environ);
        _exit(127);
    }
    if (pid < 0) {
        fprintf(stderr, "init: e2fsck: %s: fork failed\n", node);
        return -1;
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            status = 8 << 8; /* not waited for: reported as e2fsck's own error */
            break;
        }
    }
    status = WIFEXITED(status) ? WEXITSTATUS(status) : 8;
    fprintf(stderr, "init: e2fsck: %s: status %d\n", node, status);
    return status;
}

/**
 * Say on the console each of the first 32 comma-separated UUIDs of `list`
 * whose bit in `seen` is clear. Returns 0 when there is none, and -1
 * otherwise.
 */
static int report_unseen(const char *list, unsigned long seen)
{
    const char *p;
    int i, rc = 0;

    for (p = list, i = 0; p != NULL && i < 32; p = strchr(p, ','), i++) {
        p += *p == ',';
        if (!(seen & 1ul << i)) {
            fprintf(stderr, "init: e2fsck: %.36s: no volume of this UUID checked below status 4\n",
                    p);
            rc = -1;
        }
    }
    return rc;
}

/**
 * Check ext2 partitions with `e2fsck -p` before anything runs from the disk:
 * every one of every disk when `only` is NULL, and otherwise those whose
 * volume's UUID is among the comma-separated UUIDs of `only`, at most 32.
 *
 * An image without `/sbin/e2fsck` skips this, which leaves the recovery
 * system, whose root needs nothing from the disk, as the one that runs it.
 * `set_up` calls it only when the boot line has `rach2.fsck` or
 * `rach2.fsck=`, since a person who booted recovery to look, or an installer
 * that booted it to write, did not ask for a repair. The partitions are the
 * nodes `/dev/diskNpM` for every disk number and partition number the kernel
 * can give, disk by disk, and a partition is an ext2 one when its superblock
 * has the magic (`ext2_superblock`).
 *
 * `e2fsck -p` repairs what needs no question and leaves the rest for a
 * person; its status is the record, on the console and so in the boot log: 0
 * clean, 1 repaired, 2 repaired and a reboot wanted, 4 left with faults, 8 an
 * error (`run_e2fsck`). Returns 0 when every volume `only` names was found
 * and each check ended with a status below 4, and -1 otherwise, having said
 * which volume was not checked so.
 */
static int check_ext2_partitions(const char *only)
{
    unsigned long seen = 0;
    int i, status, ok = 1;

    if (access("/sbin/e2fsck", X_OK) != 0) {
        if (only) {
            fprintf(stderr, "init: e2fsck: this image has no /sbin/e2fsck\n");
        }
        return -1;
    }
    for (i = 0; i < DISK_NODES * PART_NODES; i++) {
        unsigned char sector[512]; /* a raw window reads whole sectors */
        char node[24], uuid[37];
        int place = -1;

        snprintf(node, sizeof(node), "/dev/disk%dp%d", i / PART_NODES, i % PART_NODES + 1);
        if (!ext2_superblock(node, sector)) {
            continue;
        }
        if (only) {
            ext2_uuid(sector, uuid);
            place = uuid_place(only, uuid);
            if (place < 0) {
                continue;
            }
        }
        status = run_e2fsck(node);
        if (status < 0 || status >= 4) {
            ok = 0;
        } else if (place >= 0 && place < 32) {
            seen |= 1ul << place;
        }
    }
    if (only && report_unseen(only, seen) != 0) {
        ok = 0;
    }
    return ok ? 0 : -1;
}
#endif /* INIT_METAL */

/**
 * Whether the boot line has `want` as a whole token.
 *
 * A whole token and not a prefix: `rach2.ip=none` and `rach2.ip=none.x` are
 * different things. The first 511 bytes of `/proc/cmdline` are read at each
 * call, of which a boot makes a few.
 */
static int boot_line_has(const char *want)
{
    char line[512];
    int fd = open("/proc/cmdline", O_RDONLY);
    ssize_t n;
    size_t wlen = strlen(want);
    size_t i;

    if (fd < 0) {
        return 0;
    }
    n = read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    line[n] = '\0';
    for (i = 0; i + wlen <= (size_t)n; i++) {
        if ((i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t' || line[i - 1] == '\n') &&
            strncmp(&line[i], want, wlen) == 0) {
            char after = line[i + wlen];

            if (after == '\0' || after == ' ' || after == '\t' || after == '\n') {
                return 1;
            }
        }
    }
    return 0;
}

#ifdef INIT_RECOVERY
/**
 * Put in `value`, of `size` bytes, the value of the first `name=VALUE` token
 * of the boot line. Returns 1, or 0 when the line has none or its value does
 * not fit. The first 511 bytes of `/proc/cmdline` are read.
 */
static int boot_line_value(const char *name, char *value, size_t size)
{
    char line[512];
    int fd = open("/proc/cmdline", O_RDONLY);
    ssize_t n;
    size_t nlen = strlen(name), i, len;

    if (fd < 0) {
        return 0;
    }
    n = read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    line[n] = '\0';
    for (i = 0; i + nlen < (size_t)n; i++) {
        if ((i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t' || line[i - 1] == '\n') &&
            strncmp(&line[i], name, nlen) == 0 && line[i + nlen] == '=') {
            i += nlen + 1;
            len = strcspn(&line[i], " \t\n");
            if (len >= size) {
                return 0;
            }
            memcpy(value, &line[i], len);
            value[len] = '\0';
            return 1;
        }
    }
    return 0;
}

/**
 * Check the volumes the boot line names with `rach2.fsck=UUID,...` and
 * restart the machine once each has been checked to a status below 4, so
 * that its next boot takes the saved default.
 *
 * `bootctl fsck` writes that line on a one-boot entry for the installed
 * system's init (`check_unclean_volumes`), whose kernel left the volumes
 * read-only for e2fsck. The restart is that system's way back. A volume not
 * found, or one e2fsck left with faults, keeps recovery up instead, with the
 * console saying so, since a person must look at it.
 */
static void check_named_volumes(void)
{
    char list[512];

    if (!boot_line_value("rach2.fsck", list, sizeof(list))) {
        return;
    }
    if (check_ext2_partitions(list) != 0) {
        fprintf(stderr, "init: e2fsck: %s: not every volume checked; recovery stays up\n", list);
        return;
    }
    fprintf(stderr, "init: e2fsck: %s: checked; restarting\n", list);
    shutdown_handler(SIGTERM);
}
#endif

#ifdef INIT_CONSOLE_LOGIN
/**
 * Decide whether the console session is `term`: the boot line has
 * `TERM_OPTION` and /fb answers FB_GETINFO, which it refuses with ENODEV on
 * the VGA's text mode. A text boot thus starts the kernel console's login
 * at once and pays no failed start of `term`.
 *
 * Opening /fb takes the screen, so init holds it for the one request and
 * gives it back at the close, before any session starts: the console comes
 * back as it was, its input flushed, which this early holds nothing. The
 * screen's kind does not change while the machine runs, so init asks once.
 */
static void want_term(void)
{
    unsigned int info[INIT_FB_SCREEN_INTS];
    int fd;

    if (!boot_line_has(TERM_OPTION)) {
        return;
    }
    fd = open("/fb", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENODEV) {
            fprintf(stderr, "init: console: /fb: %s; login on the kernel console\n",
                    strerror(errno));
        }
        return;
    }
    if (ioctl(fd, INIT_FB_GETINFO, info) == 0) {
        term_wanted = 1;
    } else if (errno != ENODEV) {
        fprintf(stderr, "init: console: /fb: %s; login on the kernel console\n", strerror(errno));
    }
    close(fd);
    term_now = term_wanted;
}
#endif

#ifdef INIT_METAL
/**
 * Whether the boot line handed the network to userland.
 *
 * `rach2.ip=none` means the kernel came up with the device probed and IPv4
 * unset because a program will set it. Any other boot line either named an
 * address or accepted the default, and in both cases the machine is already
 * configured: running a DHCP client over that would begin by unconfiguring the
 * address somebody put on the command line, and on a network with no server it
 * would never put one back.
 */
static int boot_wants_userland_network(void)
{
    return boot_line_has("rach2.ip=none");
}

#ifdef INIT_RECOVERY
/* The file `rach2-install` writes on the recovery partition, the directory the
 * partition is mounted on while it is read, and the most the file may hold. */
#define DISK_KEYS_FILE  "/boot/recovery_authorized_keys"
#define DISK_KEYS_MOUNT "/mnt/recovery-partition"
#define DISK_KEYS_MAX   16384
/* The recovery partition's label as it is in its FAT32 boot sector, at byte
 * 71, padded to 11 bytes: tools/image/mkdisk_grub.sh gives it, and the boot
 * loader's core finds the partition by it when the boot partition does not
 * read. */
#define RECOVERY_LABEL  "RACH2RECOV "

/** The first `rach2.boot=` token of `line` from `from` on, or NULL. */
static char *boot_token(char *line, char *from)
{
    static const char key[] = "rach2.boot=";
    char *at;

    for (at = strstr(from, key); at != NULL; at = strstr(at + 1, key)) {
        if (at == line || at[-1] == ' ' || at[-1] == '\t' || at[-1] == '\n') {
            return at;
        }
    }
    return NULL;
}

/* The disk identity a boot line names: `MBRID=` and an MBR's signature, or
 * `GPTID=` and a GUID table's disk GUID. `text` is the form the boot line
 * gave, for the messages. */
struct disk_identity {
    int gpt;                /* 1 for `GPTID=`, 0 for `MBRID=` */
    unsigned long id;       /* the MBR signature; set only when `gpt` is 0 */
    unsigned char guid[16]; /* the disk GUID as the table stores it */
    char text[44];          /* `MBRID=...` or `GPTID=...`, without `,1` */
};

/**
 * Read the disk identity the boot line's `rach2.boot=MBRID=XXXXXXXX,1` or
 * `rach2.boot=GPTID=GUID,1` names into `*want`.
 *
 * Returns 1 when the first `rach2.boot=` token is exactly one of those,
 * eight hexadecimal digits or a GUID in its 36-character text form and `,1`,
 * the boot partition of the disk's own menu, and no second token follows; 0
 * otherwise. The kernel boots from the first token, and a key taken from a
 * disk another token names would not be the disk this recovery system came
 * from.
 */
static int boot_disk_id(struct disk_identity *want)
{
    static const char key[] = "rach2.boot=";
    char line[512];
    int fd = open("/proc/cmdline", O_RDONLY);
    ssize_t n;
    char *at, *end;
    size_t length;
    int i;

    if (fd < 0) {
        return 0;
    }
    n = read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    line[n] = '\0';
    at = boot_token(line, line);
    if (at == NULL || boot_token(line, at + 1) != NULL) {
        return 0;
    }
    at += sizeof(key) - 1;
    for (end = at; *end != '\0' && *end != ' ' && *end != '\t' && *end != '\n'; end++) {
    }
    length = (size_t)(end - at);
    if (length < 2 || length - 2 >= sizeof(want->text) || strncmp(end - 2, ",1", 2) != 0) {
        return 0;
    }
    memcpy(want->text, at, length - 2);
    want->text[length - 2] = '\0';
    want->gpt = strncmp(want->text, "GPTID=", 6) == 0;
    if (want->gpt) {
        return gptdisk_guid_parse(want->text + 6, want->guid) == 0;
    }
    if (strncmp(want->text, "MBRID=", 6) != 0 || strlen(want->text) != 14) {
        return 0;
    }
    want->id = 0;
    for (i = 6; i < 14; i++) {
        int c = want->text[i];
        int v;

        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else {
            return 0;
        }
        want->id = (want->id << 4) | (unsigned long)v;
    }
    return 1;
}

/**
 * Whether the disk open on `fd`, number `n`, has the identity `want`: the
 * four bytes at offset 440 of an MBR that is not a GUID table's protective
 * one, or the disk GUID of a GUID table, as the kernel resolves
 * `rach2.boot=` by them.
 */
static int disk_has_identity(int fd, int n, const struct disk_identity *want)
{
    static struct gptdisk table;
    unsigned char sector[512];
    unsigned long on_disk;
    uint64_t sectors;

    if (read(fd, sector, sizeof(sector)) != (ssize_t)sizeof(sector) || sector[510] != 0x55 ||
        sector[511] != 0xAA) {
        return 0;
    }
    if (want->gpt) {
        return gptdisk_sectors(n, &sectors) == 0 && gptdisk_read(fd, sectors, &table) == 1 &&
               memcmp(table.header + 56, want->guid, 16) == 0;
    }
    on_disk = (unsigned long)sector[440] | (unsigned long)sector[441] << 8 |
              (unsigned long)sector[442] << 16 | (unsigned long)sector[443] << 24;
    return sector[446 + 4] != 0xEE && on_disk == want->id;
}

/**
 * Return the number of the one disk with the identity `want`, -1 when no
 * disk has it and -2 when more than one does. A clone of the disk beside it
 * makes the answer ambiguous, and an ambiguous disk gives no key.
 */
static int disk_with_id(const struct disk_identity *want)
{
    int found = -1;
    int n;

    for (n = 0; n < DISK_NODES; n++) {
        char node[16];
        int fd, has;

        snprintf(node, sizeof(node), "/dev/disk%d", n);
        fd = open(node, O_RDONLY);
        if (fd < 0) {
            continue;
        }
        has = disk_has_identity(fd, n, want);
        close(fd);
        if (!has) {
            continue;
        }
        if (found >= 0) {
            return -2;
        }
        found = n;
    }
    return found;
}

/**
 * Whether the partition node `node` is a recovery partition by its label.
 *
 * The sector is read raw, before anything is mounted, so init never mounts
 * the boot partition or the root, however damaged they are.
 */
static int is_recovery_partition(const char *node)
{
    unsigned char sector[512];
    int fd = open(node, O_RDONLY);
    ssize_t got;

    if (fd < 0) {
        return 0;
    }
    got = read(fd, sector, sizeof(sector));
    close(fd);
    return got == (ssize_t)sizeof(sector) && sector[510] == 0x55 && sector[511] == 0xAA &&
           memcmp(sector + 71, RECOVERY_LABEL, 11) == 0;
}

/**
 * Why the `len` bytes of `keys`, followed by a NUL, will not do as keys, or
 * NULL when they will: they are not text (a NUL is among them), they hold a
 * private key, or every line of them is blank or a comment.
 */
static const char *keys_fault(const char *keys, long len)
{
    long i;
    int line_start = 1;

    if ((long)strlen(keys) != len) {
        return "the key file is not text";
    }
    if (strstr(keys, "PRIVATE KEY") != NULL) {
        return "the key file holds a private key";
    }
    for (i = 0; i < len; i++) {
        char c = keys[i];

        if (c == '\n') {
            line_start = 1;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            continue;
        } else if (line_start && c != '#') {
            return NULL;
        } else {
            line_start = 0;
        }
    }
    return "no line of the key file is a key";
}

/**
 * Read the key file under the mounted partition into `keys`, which holds
 * `DISK_KEYS_MAX + 1` bytes, and return its length.
 *
 * Returns -1 with `*why` set when the file is there and cannot be used: it
 * cannot be opened or read, is not a regular file, is larger than
 * `DISK_KEYS_MAX`, or fails `keys_fault`. Returns 0 with `*why` NULL when
 * there is no file, which is a disk installed without a key.
 */
static long read_disk_keys(char *keys, const char **why)
{
    struct stat st;
    long len = 0;
    int fd;

    *why = NULL;
    fd = open(DISK_KEYS_MOUNT DISK_KEYS_FILE, O_RDONLY);
    if (fd < 0) {
        if (errno != ENOENT) {
            *why = "the key file cannot be opened";
            return -1;
        }
        return 0;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        *why = "the key file is not a regular file";
        return -1;
    }
    for (;;) {
        ssize_t got = read(fd, keys + len, (size_t)(DISK_KEYS_MAX + 1 - len));

        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            close(fd);
            *why = "the key file cannot be read";
            return -1;
        }
        if (got == 0 || len + got > DISK_KEYS_MAX) {
            len += got;
            break;
        }
        len += got;
    }
    close(fd);
    if (len > DISK_KEYS_MAX) {
        *why = "the key file is larger than 16384 bytes";
        return -1;
    }
    keys[len] = '\0';
    *why = keys_fault(keys, len);
    return *why != NULL ? -1 : len;
}

/**
 * Append the `len` bytes of `keys`, `len` at least 1, to root's
 * `authorized_keys`, making `/root/.ssh` and the file when they are missing.
 * A newline goes before them when the file does not end with one, and after
 * them when they do not. Returns 0 on success, -1 otherwise.
 */
static int append_root_keys(const char *keys, long len)
{
    char last = '\n';
    off_t end;
    long done = 0;
    int fd;

    (void)mkdir("/root/.ssh", 0700);
    fd = open(RECOVERY_KEYS, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        return -1;
    }
    end = lseek(fd, 0, SEEK_END);
    if (end < 0 || (end > 0 && (lseek(fd, end - 1, SEEK_SET) != end - 1 ||
                                read(fd, &last, 1) != 1 || lseek(fd, 0, SEEK_END) != end))) {
        close(fd);
        return -1;
    }
    if (last != '\n' && write(fd, "\n", 1) != 1) {
        close(fd);
        return -1;
    }
    while (done < len) {
        ssize_t put = write(fd, keys + done, (size_t)(len - done));

        if (put < 0 && errno == EINTR) {
            continue;
        }
        if (put <= 0) {
            close(fd);
            return -1;
        }
        done += put;
    }
    if (keys[len - 1] != '\n' && write(fd, "\n", 1) != 1) {
        close(fd);
        return -1;
    }
    return close(fd);
}

/**
 * Add the key the installer put on this disk's recovery partition to root's
 * keys. Returns 1 when a key was added and 0 otherwise.
 *
 * `rach2-install` writes `DISK_KEYS_FILE` on the recovery partition when the
 * person installing gives it a key, so that a machine installed from a stick
 * with no key can still be updated over the network: a root update runs from
 * recovery. Only a boot from that partition reads it, which the recovery
 * entries of the disk's own menu say with `rach2.slot=recovery`. On the
 * stick's recovery system the image alone decides, so a stick built with no
 * key starts no server wherever it is booted. The disk is the one
 * `rach2.boot=` names by its identity, `MBRID=` or `GPTID=`, and the
 * partition is its second, where both installed layouts keep recovery, and
 * only when its FAT32 boot sector has the recovery label and it holds
 * `/boot/recovery`, the file the installer finds the partition by; the boot
 * partition and the root are never mounted, whatever their labels say.
 *
 * The partition is mounted read-only and unmounted as soon as the file is
 * read, before anything else runs, so a boot changes nothing on the disk
 * and leaves no mount on it to stop `reflash` or the installer. The file is
 * not used, and a message says why, for the reasons `read_disk_keys` gives;
 * what its lines say beyond that is the server's to judge, as in
 * `anyone_to_let_in`.
 */
static int take_disk_keys(void)
{
    static char keys[DISK_KEYS_MAX + 1];
    struct disk_identity want;
    char node[24];
    const char *why;
    long len;
    int added = 0;
    int disk;

    if (!boot_line_has("rach2.slot=recovery") || !boot_disk_id(&want)) {
        return 0;
    }
    disk = disk_with_id(&want);
    if (disk == -2) {
        fprintf(stderr, "init: recovery key: two disks have identity %s; no key taken\n",
                want.text);
        return 0;
    }
    if (disk < 0) {
        fprintf(stderr, "init: recovery key: no disk has identity %s\n", want.text);
        return 0;
    }
    (void)mkdir("/mnt", 0755);
    if (mkdir(DISK_KEYS_MOUNT, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "init: recovery key: %s cannot be made\n", DISK_KEYS_MOUNT);
        return 0;
    }
    snprintf(node, sizeof(node), "/dev/disk%dp2", disk);
    if (!is_recovery_partition(node) ||
        mount(node, DISK_KEYS_MOUNT, "vfat", MS_RDONLY, "umask=077") != 0) {
        (void)rmdir(DISK_KEYS_MOUNT);
        fprintf(stderr, "init: recovery key: /dev/disk%d has no recovery partition\n", disk);
        return 0;
    }
    if (access(DISK_KEYS_MOUNT "/boot/recovery", F_OK) != 0) {
        if (umount(DISK_KEYS_MOUNT) != 0) {
            fprintf(stderr, "init: recovery key: %s: would not unmount\n", node);
            return 0;
        }
        (void)rmdir(DISK_KEYS_MOUNT);
        fprintf(stderr, "init: recovery key: /dev/disk%d has no recovery partition\n", disk);
        return 0;
    }
    len = read_disk_keys(keys, &why);
    if (len < 0) {
        fprintf(stderr, "init: recovery key: %s: refused: %s\n", node, why);
    } else if (len > 0 && append_root_keys(keys, len) != 0) {
        fprintf(stderr, "init: recovery key: %s: cannot be written\n", RECOVERY_KEYS);
    } else if (len > 0) {
        added = 1;
    }
    if (umount(DISK_KEYS_MOUNT) != 0) {
        fprintf(stderr, "init: recovery key: %s: would not unmount\n", node);
        return added;
    }
    (void)rmdir(DISK_KEYS_MOUNT);
    if (added) {
        printf("init: recovery key: taken from %s\n", node);
    }
    return added;
}

/**
 * Take the recovery partition's key and say on the console whether SSH is on,
 * and why. A recovery image built to work on and one built for anybody else
 * are identical from outside, so the machine says which it is, and whether
 * the disk it booted from gave it a key.
 */
static void report_recovery_keys(void)
{
    int built_in = anyone_to_let_in();
    int from_disk = take_disk_keys();

    if (built_in) {
        printf("init: a key was built into this recovery image, so SSH is on\n");
    } else if (from_disk) {
        printf("init: no key was built into this recovery image, and its recovery "
               "partition's key turns SSH on\n");
    } else {
        printf("init: no key was built into this recovery image, so SSH is off\n");
    }
}

/**
 * Name on the console each disk whose GUID table was read from its backup.
 *
 * The kernel and the tools here take a table's backup at the disk's end
 * when its primary header or array does not check. GRUB's BIOS core reads
 * only the primary, so such a disk boots by UEFI and not by BIOS until the
 * primary is written again. Recovery changes nothing it was not asked to,
 * so this says which disk it is and the command that rewrites the primary
 * from the backup.
 */
static void report_backup_tables(void)
{
    static struct gptdisk table;
    char node[16];
    uint64_t sectors;
    int n, fd;

    for (n = 0; n < DISK_NODES; n++) {
        if (gptdisk_sectors(n, &sectors) != 0) {
            continue;
        }
        snprintf(node, sizeof(node), "/dev/disk%d", n);
        fd = open(node, O_RDONLY);
        if (fd < 0) {
            continue;
        }
        if (gptdisk_read(fd, sectors, &table) == 1 && table.from_backup) {
            printf("init: %s: its primary GUID table does not check and was read from the "
                   "backup; diskctl relocate %s writes it again\n",
                   node, node);
        }
        close(fd);
    }
}
#endif

/**
 * Configure the network with `ifup -a`, wait for it, and report a failure on
 * standard error. `start_network` calls it once, in a child.
 *
 * A machine across the room is reached over the network, and on a machine that
 * is carried between two buildings the address is not something the boot line
 * can name: it comes from whatever server is on the wire there. So init runs
 * `ifup -a`, which reads `/etc/network/interfaces` and does whatever it says,
 * a lease or a fixed address.
 *
 * It runs after the console login has been started and before the SSH server,
 * which is the order that matters: a network that fails to come up must not
 * take the local way in with it, and a server started before there is an
 * address would be listening on one it is about to lose.
 *
 * It runs only where the boot line said userland owns the address. A machine
 * told its address on the command line is configured already, and a DHCP
 * client would begin by taking that address off.
 *
 * The result is reported and not acted on. There is nothing useful to do with
 * a failure here: the machine is up, the console works, and a later `ifup` by
 * hand is the repair.
 *
 * `/var/run/ifstate`, where `ifup` records the interfaces it has brought up,
 * is removed first: on a disk root the previous boot's record survives, and
 * `ifup -a` would take eth0 as already configured and bring up nothing.
 *
 * `ifup` runs its commands, `ifconfig` and `udhcpc` among them, through
 * root's login shell, with init's path, SERVICE_PATH. BusyBox's ash runs its
 * applets by name whatever the path, but bash, root's login shell on the GNU
 * system, finds them only through the path. `ifup` is not given a shell by
 * `SHELL`, which it would also take: `check-setid` requires that `init-metal`
 * have no shell's path in it.
 */
static void configure_network(void)
{
    pid_t pid;
    int status = 0;

    unlink("/var/run/ifstate");
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "init: network: cannot fork ifup\n");
        return;
    }
    if (pid == 0) {
        char *argv[3];

        argv[0] = (char *)"ifup";
        argv[1] = (char *)"-a";
        argv[2] = NULL;
        execv("/bin/ifup", argv);
        execv("/bin/busybox", argv); /* the applet, where there is no /bin/ifup */
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFEXITED(status)) {
            fprintf(stderr,
                    "init: network: ifup -a did not configure the interface (status %d%s)\n",
                    WEXITSTATUS(status), WEXITSTATUS(status) == 127 ? ", not executed" : "");
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "init: network: ifup -a was killed by signal %d\n", WTERMSIG(status));
        } else {
            fprintf(stderr, "init: network: ifup -a neither exited nor was killed (0x%x)\n",
                    (unsigned)status);
        }
    }
}

/**
 * Run `configure_network` in a child whose output goes to the service log,
 * and wait for the child. A failed fork or a failure to block the shutdown
 * signals skips the configuration.
 *
 * The redirection is the child's, so init keeps the console descriptors the
 * later sessions inherit, and a redirection that fails ends only the child.
 * The shutdown signals are blocked across the fork until the child has their
 * default actions, so a signal sent to the child ends it and does not run
 * init's handler.
 */
static void start_network(void)
{
    sigset_t saved;
    pid_t network;

    if (shutdown_block(&saved) != 0) {
        return;
    }
    network = fork();

    if (network == 0) {
        if (shutdown_defaults(&saved) != 0) {
            _exit(126);
        }
        service_output();
        configure_network();
        _exit(0);
    }
    (void)sigprocmask(SIG_SETMASK, &saved, NULL);
    if (network > 0) {
        while (waitpid(network, NULL, 0) < 0 && errno == EINTR) {
        }
    }
}

/**
 * Return 1 when `/etc/rach2/sshd.conf` has a line containing exactly
 * `root-password-login yes`, apart from trailing whitespace. Return 0 if the
 * file is unavailable, cannot be read, or contains no such line.
 */
static int root_password_login(void)
{
    FILE *f = fopen("/etc/rach2/sshd.conf", "r");
    static const char setting[] = "root-password-login yes";
    size_t matched = 0;
    int c, bad = 0, yes = 0;

    if (!f) {
        return 0;
    }
    for (;;) {
        c = fgetc(f);
        if (c == '\n' || c == EOF) {
            if (!bad && matched == sizeof(setting) - 1) {
                yes = 1;
            }
            if (c == EOF) {
                break;
            }
            matched = 0;
            bad = 0;
        } else if (matched < sizeof(setting) - 1) {
            if (c == setting[matched]) {
                matched++;
            } else {
                bad = 1;
            }
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\v' && c != '\f') {
            bad = 1;
        }
    }
    if (ferror(f)) {
        yes = 0;
    }
    fclose(f);
    return yes;
}

/**
 * Fill `argv`, which has room for eight pointers, with the SSH server's
 * command line, and return the program to execute: OpenSSH's when `openssh`
 * is set and Dropbear's when it is not, with root's password barred unless
 * `password_login` is set.
 */
static const char *sshd_command(int openssh, int password_login, char **argv)
{
    int n = 0;

    if (openssh) {
        argv[n++] = (char *)OPENSSH_PROGRAM;
        argv[n++] = (char *)"-D";
        argv[n++] = (char *)"-e";
        argv[n++] = (char *)"-o";
        argv[n++] =
            (char *)(password_login ? "PermitRootLogin=yes" : "PermitRootLogin=prohibit-password");
        argv[n] = 0;
        return OPENSSH_PROGRAM;
    }
    argv[n++] = (char *)"dropbearmulti";
    argv[n++] = (char *)"dropbear";
    argv[n++] = (char *)"-F";
    argv[n++] = (char *)"-R";
    if (!password_login) {
        argv[n++] = (char *)"-g";
    }
    argv[n++] = (char *)"-p";
    argv[n++] = (char *)"22";
    argv[n] = 0;
    return SSHD_PROGRAM;
}

/**
 * Make the host keys OpenSSH's server lacks with `ssh-keygen -A`, and wait
 * for it.
 *
 * It runs in the server's child after the redirection, so what it prints goes
 * to the service log and init's own loop goes on reaping while a key is made,
 * which takes seconds on an emulated machine. A key it could not make is
 * reported by the server, which does not start without one.
 */
static void make_host_keys(void)
{
    pid_t pid = fork();
    int status;

    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        char *argv[3];

        argv[0] = (char *)"ssh-keygen";
        argv[1] = (char *)"-A";
        argv[2] = 0;
        execve(OPENSSH_KEYGEN, argv, environ);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

/**
 * Fork the SSH server and return its pid (-1 on failure).
 *
 * Unlike the console session this child does not call `setsid` or claim a
 * controlling terminal. The server makes a session per login and gives each
 * one a pty of its own; handing it the console as well would put two sessions
 * on one terminal, and the console login is the recovery path precisely
 * because nothing else is on it. Shutdown signals are blocked across the fork
 * until the child has default dispositions, since the child waits for
 * `ssh-keygen` on a first boot before its exec, and a signal to the service's
 * pid in that time must end the child, not run init's handler.
 */
static pid_t spawn_sshd(void)
{
    int password_login = root_password_login();
    int openssh = access(OPENSSH_PROGRAM, X_OK) == 0;
    sigset_t saved;
    pid_t pid;

    if (shutdown_block(&saved) != 0) {
        return -1;
    }
    pid = fork();
    if (pid != 0) {
        (void)sigprocmask(SIG_SETMASK, &saved, NULL);
        return pid; /* init (the parent), or -1 if fork failed */
    }
    if (shutdown_defaults(&saved) != 0) {
        _exit(126);
    }
    service_output();
    {
        static const char msg[] = "init: cannot exec ";
        char *argv[8];
        const char *program = sshd_command(openssh, password_login, argv);

        if (openssh) {
            make_host_keys();
        }
        execve(program, argv, environ);
        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)write(STDERR_FILENO, program, strlen(program));
        (void)write(STDERR_FILENO, "\n", 1);
    }
    _exit(127);
    return -1; /* unreachable; quiets the compiler */
}
#endif

#ifdef INIT_METAL
/**
 * Make /etc/machine-id, the 128 random bits that name this machine to D-Bus
 * and its clients, as 32 lowercase hex digits and a newline, when the file
 * is missing, which is the first boot of a root, as systemd makes it on
 * Linux. The bits are the kernel's. The file is written under a temporary
 * name and renamed into place, so that a power loss leaves it whole or
 * absent.
 */
static void make_machine_id(void)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[16];
    char text[33];
    int fd, i, ok;

    if (access(MACHINE_ID, F_OK) == 0) {
        return;
    }
    fd = open("/dev/urandom", O_RDONLY);
    ok = fd >= 0 && read(fd, raw, sizeof(raw)) == (ssize_t)sizeof(raw);
    if (fd >= 0) {
        close(fd);
    }
    if (!ok) {
        return;
    }
    for (i = 0; i < 16; i++) {
        text[2 * i] = hex[raw[i] >> 4];
        text[2 * i + 1] = hex[raw[i] & 15];
    }
    text[32] = '\n';
    fd = open(MACHINE_ID ".new", O_WRONLY | O_CREAT | O_TRUNC, 0444);
    if (fd < 0) {
        return;
    }
    ok = write(fd, text, sizeof(text)) == (ssize_t)sizeof(text) && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(MACHINE_ID ".new", MACHINE_ID) != 0) {
        unlink(MACHINE_ID ".new");
    }
}

/* The ids of the bus's user, which `bus_user` finds at boot. */
static uid_t bus_uid;
static gid_t bus_gid;

/**
 * Fork D-Bus's system bus as its user (`bus_uid`, `bus_gid`), and return its
 * pid (-1 on failure).
 *
 * The child makes the machine's identity on a first boot
 * (`make_machine_id`) and the bus's directory, owned by the bus's user so
 * that the bus can make its socket in it, then takes the user's ids and runs
 * `dbus-daemon --system` in the foreground, with its messages on standard
 * error, which is the service log. The bus starts as its user and not as
 * root: started as root, it changes to its user through libcap-ng, which
 * keeps a part of root's capabilities across the change, and this kernel
 * keeps all of a process's capabilities or none (capset(2)). The bus needs
 * none, since it serves a socket. The shutdown signals are blocked across the
 * fork as for the SSH server (`spawn_sshd`).
 */
static pid_t spawn_bus(void)
{
    sigset_t saved;
    pid_t pid;

    if (shutdown_block(&saved) != 0) {
        return -1;
    }
    pid = fork();
    if (pid != 0) {
        (void)sigprocmask(SIG_SETMASK, &saved, NULL);
        return pid;
    }
    if (shutdown_defaults(&saved) != 0) {
        _exit(126);
    }
    service_output();
    make_machine_id();
    if ((mkdir(DBUS_DIR, 0755) != 0 && errno != EEXIST) || chown(DBUS_DIR, bus_uid, bus_gid) != 0 ||
        setgroups(0, NULL) != 0 || setgid(bus_gid) != 0 || setuid(bus_uid) != 0) {
        static const char msg[] = "init: dbus: cannot make " DBUS_DIR " or take its user\n";

        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
        _exit(126);
    }
    {
        static const char msg[] = "init: cannot exec " DBUS_PROGRAM "\n";
        char *argv[6];

        argv[0] = (char *)DBUS_PROGRAM;
        argv[1] = (char *)"--system";
        argv[2] = (char *)"--nofork";
        argv[3] = (char *)"--nopidfile";
        argv[4] = (char *)"--nosyslog";
        argv[5] = 0;
        execve(DBUS_PROGRAM, argv, environ);
        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
    _exit(127);
    return -1; /* unreachable; quiets the compiler */
}

/**
 * Fork the system log (`BUSYBOX_PROGRAM`'s syslogd) and return its pid (-1
 * on failure), its own messages on the service log, with the shutdown
 * signals blocked across the fork as for the SSH server (`spawn_sshd`).
 */
static pid_t spawn_syslog(void)
{
    sigset_t saved;
    pid_t pid;

    if (shutdown_block(&saved) != 0) {
        return -1;
    }
    pid = fork();
    if (pid != 0) {
        (void)sigprocmask(SIG_SETMASK, &saved, NULL);
        return pid;
    }
    if (shutdown_defaults(&saved) != 0) {
        _exit(126);
    }
    service_output();
    {
        static const char msg[] = "init: cannot exec " BUSYBOX_PROGRAM " syslogd\n";
        char *argv[9];

        argv[0] = (char *)"syslogd";
        argv[1] = (char *)"-n";
        argv[2] = (char *)"-O";
        argv[3] = (char *)SYSLOG_FILE;
        argv[4] = (char *)"-s";
        argv[5] = (char *)"200";
        argv[6] = (char *)"-b";
        argv[7] = (char *)"1";
        argv[8] = 0;
        execve(BUSYBOX_PROGRAM, argv, environ);
        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
    _exit(127);
    return -1; /* unreachable; quiets the compiler */
}

/**
 * Fork the cron daemon (`CRON_PROGRAM`) in the foreground and return its pid
 * (-1 on failure), its own messages on the service log, with the shutdown
 * signals blocked across the fork as for the SSH server (`spawn_sshd`).
 */
static pid_t spawn_cron(void)
{
    sigset_t saved;
    pid_t pid;

    if (shutdown_block(&saved) != 0) {
        return -1;
    }
    pid = fork();
    if (pid != 0) {
        (void)sigprocmask(SIG_SETMASK, &saved, NULL);
        return pid;
    }
    if (shutdown_defaults(&saved) != 0) {
        _exit(126);
    }
    service_output();
    {
        static const char msg[] = "init: cannot exec " CRON_PROGRAM "\n";
        char *argv[3];

        argv[0] = (char *)CRON_PROGRAM;
        argv[1] = (char *)"-n";
        argv[2] = 0;
        execve(CRON_PROGRAM, argv, environ);
        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
    _exit(127);
    return -1; /* unreachable; quiets the compiler */
}

/**
 * Return 1 when `NTP_CONF` has a line whose first word is `server` and has a
 * second word, the line ntpd takes a server from, and 0 when it has none or
 * cannot be read. A `#` begins a comment, as it does for ntpd. Given no
 * server, ntpd prints its usage and exits, so init does not start it.
 */
static int ntp_servers(void)
{
    FILE *f = fopen(NTP_CONF, "r");
    char line[512];
    int found = 0;

    if (!f) {
        return 0;
    }
    while (!found && fgets(line, sizeof(line), f)) {
        char *p = line + strspn(line, " \t");

        if (strncmp(p, "server", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
            p += 6 + strspn(p + 6, " \t");
            found = *p != '\0' && *p != '\n' && *p != '\r' && *p != '#';
        }
    }
    fclose(f);
    return found;
}

/**
 * Fork BusyBox's ntpd in the foreground and return its pid (-1 on failure),
 * its messages on the service log, with the shutdown signals blocked across
 * the fork as for the SSH server (`spawn_sshd`).
 */
static pid_t spawn_ntpd(void)
{
    sigset_t saved;
    pid_t pid;

    if (shutdown_block(&saved) != 0) {
        return -1;
    }
    pid = fork();
    if (pid != 0) {
        (void)sigprocmask(SIG_SETMASK, &saved, NULL);
        return pid;
    }
    if (shutdown_defaults(&saved) != 0) {
        _exit(126);
    }
    service_output();
    {
        static const char msg[] = "init: cannot exec " BUSYBOX_PROGRAM " ntpd\n";
        char *argv[3];

        argv[0] = (char *)"ntpd";
        argv[1] = (char *)"-n";
        argv[2] = 0;
        execve(BUSYBOX_PROGRAM, argv, environ);
        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
    _exit(127);
    return -1; /* unreachable; quiets the compiler */
}

/**
 * Fork the program `path` with `argv`, which stays in the foreground, and
 * return its pid (-1 on failure), its messages on the service log, with the
 * shutdown signals blocked across the fork as for the SSH server
 * (`spawn_sshd`).
 */
static pid_t spawn_service(const char *path, char *const argv[])
{
    sigset_t saved;
    pid_t pid;

    if (shutdown_block(&saved) != 0) {
        return -1;
    }
    pid = fork();
    if (pid != 0) {
        (void)sigprocmask(SIG_SETMASK, &saved, NULL);
        return pid;
    }
    if (shutdown_defaults(&saved) != 0) {
        _exit(126);
    }
    service_output();
    execve(path, argv, environ);
    fprintf(stderr, "init: cannot exec %s %s: %s\n", path, argv[0], strerror(errno));
    _exit(127);
    return -1; /* unreachable; quiets the compiler */
}

/**
 * Whether to follow `NETWORK_IFACE` with the program `path`: the boot line
 * hands the network to userland, the image has the program and the
 * interface exists. Asked on each turn that finds the program not running.
 */
static int follow_wanted(const char *path)
{
    return boot_wants_userland_network() && access(path, X_OK) == 0 &&
           if_nametoindex(NETWORK_IFACE) != 0;
}

/**
 * Start BusyBox's ifplugd for `NETWORK_IFACE` when `follow_wanted`: in the
 * foreground (`-n`), writing to the service log rather than the system log
 * (`-s`), told of the link's changes by netlink (`-M`) and reading the link
 * from `IFF_RUNNING` (`-m iff`). It runs `IFPLUGD_ACTION` at once on each
 * change (`-u 0 -d 0`), since a link down for a moment may come back on
 * another network, goes on when the action fails (`-I`), and runs nothing
 * when it starts (`-p`), since `ifup -a` has brought the interface up.
 */
static void start_ifplugd(struct service *s)
{
    static char *const argv[] = {(char *)"ifplugd",
                                 (char *)"-n",
                                 (char *)"-s",
                                 (char *)"-I",
                                 (char *)"-M",
                                 (char *)"-p",
                                 (char *)"-i",
                                 (char *)NETWORK_IFACE,
                                 (char *)"-r",
                                 (char *)IFPLUGD_ACTION,
                                 (char *)"-m",
                                 (char *)"iff",
                                 (char *)"-u",
                                 (char *)"0",
                                 (char *)"-d",
                                 (char *)"0",
                                 0};

    if (s->pid < 0 && access(BUSYBOX_PROGRAM, X_OK) == 0 && follow_wanted(IFPLUGD_ACTION)) {
        s->pid = spawn_service(BUSYBOX_PROGRAM, argv);
        s->spawned = time(0);
    }
}

/** Start ndwatch(8) for `NETWORK_IFACE` when `follow_wanted`. */
static void start_ndwatch(struct service *s)
{
    static char *const argv[] = {(char *)"ndwatch", (char *)NETWORK_IFACE, 0};

    if (s->pid < 0 && follow_wanted(NDWATCH_PROGRAM)) {
        s->pid = spawn_service(NDWATCH_PROGRAM, argv);
        s->spawned = time(0);
    }
}

/** The first of the paths `paths`, ended by a null pointer, that the image
 * has as an executable, or a null pointer when it has none. */
static const char *first_program(const char *const *paths)
{
    for (; *paths; paths++) {
        if (access(*paths, X_OK) == 0) {
            return *paths;
        }
    }
    return 0;
}

/** wpa_supplicant's path on this image, or a null pointer. */
static const char *wpa_supplicant_program(void)
{
    static const char *const paths[] = WPA_SUPPLICANT_PATHS;

    return first_program(paths);
}

/** wpa_cli's path on this image, or a null pointer. */
static const char *wpa_cli_program(void)
{
    static const char *const paths[] = WPA_CLI_PATHS;

    return first_program(paths);
}

/** Whether to follow `WIFI_IFACE`: the image has wpa_supplicant, wpa_cli,
 * the action and the configuration, and the interface exists. */
static int wifi_wanted(void)
{
    return wpa_supplicant_program() && wpa_cli_program() && access(WPA_ACTION, X_OK) == 0 &&
           access(WPA_CONF, R_OK) == 0 && if_nametoindex(WIFI_IFACE) != 0;
}

/**
 * Start wpa_supplicant on `WIFI_IFACE` when `wifi_wanted`, in the foreground
 * with the nl80211 driver, each line of its log stamped with the time (`-t`),
 * by which a drop is matched to the AP's log. `-W` holds it until a monitor
 * attaches to its control socket, which `start_wpa_cli` starts once the
 * socket is there, so that wpa_cli sees the first connection. It runs with a
 * umask of 077, since it writes the networks' passphrases into `WPA_CONF`
 * when wifi(1) saves them.
 */
static void start_wpa_supplicant(struct service *s)
{
    static char *const argv[] = {(char *)"wpa_supplicant",
                                 (char *)"-t",
                                 (char *)"-W",
                                 (char *)"-D",
                                 (char *)"nl80211",
                                 (char *)"-i",
                                 (char *)WIFI_IFACE,
                                 (char *)"-c",
                                 (char *)WPA_CONF,
                                 0};
    const char *program;
    mode_t mask;

    if (s->pid < 0 && wifi_wanted() && (program = wpa_supplicant_program()) != 0) {
        mask = umask(077);
        s->pid = spawn_service(program, argv);
        (void)umask(mask);
        s->spawned = time(0);
    }
}

/**
 * Start `wpa_cli -a WPA_ACTION` on `WIFI_IFACE` while wpa_supplicant runs
 * and its control socket is there. wpa_cli exits when it loses the
 * supplicant, and is started again with the next one.
 */
static void start_wpa_cli(struct service *s, const struct service *supplicant)
{
    static char *const argv[] = {
        (char *)"wpa_cli",    (char *)"-i", (char *)WIFI_IFACE, (char *)"-p",
        (char *)WPA_CTRL_DIR, (char *)"-a", (char *)WPA_ACTION, 0};
    const char *program;

    if (s->pid < 0 && supplicant->pid >= 0 && access(WPA_CTRL, F_OK) == 0 &&
        (program = wpa_cli_program()) != 0) {
        s->pid = spawn_service(program, argv);
        s->spawned = time(0);
    }
}

/**
 * Write `metric` to the `ra_defrtr_metric` of the interface `name` once for
 * each time it appears: `*written` keeps the index of the interface last
 * written, so a value an administrator writes later stands, and a radio
 * that comes back as a new interface is written again. A failed write is
 * reported once and not tried again for that interface.
 */
static void set_ra_metric(const char *name, const char *metric, unsigned *written)
{
    unsigned index = if_nametoindex(name);
    char path[64];
    int fd, ok;

    if (index == 0 || index == *written) {
        return;
    }
    *written = index;
    snprintf(path, sizeof(path), RA_METRIC_PATH, name);
    fd = open(path, O_WRONLY | O_CLOEXEC);
    ok = fd >= 0 && write(fd, metric, strlen(metric)) == (ssize_t)strlen(metric);
    if (!ok) {
        fprintf(stderr, "init: %s: cannot set ra_defrtr_metric: %s\n", name, strerror(errno));
    }
    if (fd >= 0) {
        close(fd);
    }
}

/** Start ndwatch(8) for `WIFI_IFACE` when `wifi_wanted` and the image has
 * ndwatch. */
static void start_wifi_ndwatch(struct service *s)
{
    static char *const argv[] = {(char *)"ndwatch", (char *)WIFI_IFACE, 0};

    if (s->pid < 0 && access(NDWATCH_PROGRAM, X_OK) == 0 && wifi_wanted()) {
        s->pid = spawn_service(NDWATCH_PROGRAM, argv);
        s->spawned = time(0);
    }
}

/**
 * Whether this image runs D-Bus's system bus: when it has `DBUS_PROGRAM`
 * and the user `DBUS_USER`, whose ids go in `bus_uid` and `bus_gid`. An
 * image with the program and not the user is reported once.
 */
static int bus_user(void)
{
    const struct passwd *pw;

    if (access(DBUS_PROGRAM, X_OK) != 0) {
        return 0;
    }
    pw = getpwnam(DBUS_USER);
    if (!pw) {
        fprintf(stderr, "init: dbus: no user %s\n", DBUS_USER);
        return 0;
    }
    bus_uid = pw->pw_uid;
    bus_gid = pw->pw_gid;
    return 1;
}

/* The services, and whether the image has the bus and the cron daemon,
 * which `services_found` decides at boot. */
struct services {
    struct service log, sshd, bus, cron, ntp, plug, nd, wpa, wcli, wnd, desktop;
    int bus_wanted, cron_wanted;
    unsigned wired_metric, wifi_metric; /* the indices `set_ra_metric` wrote */
};

/**
 * Set `*s` to no service running, and find which of the optional services
 * this image runs: D-Bus's system bus (`bus_user`) and the cron daemon, where
 * the image has `CRON_PROGRAM`.
 */
static void services_found(struct services *s)
{
    memset(s, 0, sizeof(*s));
    s->log.pid = s->sshd.pid = s->bus.pid = s->cron.pid = s->ntp.pid = -1;
    s->plug.pid = s->nd.pid = s->wpa.pid = s->wcli.pid = s->wnd.pid = -1;
    s->desktop.pid = -1;
    s->bus_wanted = bus_user();
    s->cron_wanted = access(CRON_PROGRAM, X_OK) == 0;
}

/**
 * Start each service that is not running: the system log, the SSH server
 * where `anyone_to_let_in` allows one, reported on the console, D-Bus's
 * system bus and the cron daemon where the image has them, ntpd while
 * `NTP_CONF` names a server, ifplugd and ndwatch where init follows
 * `NETWORK_IFACE`, and wpa_supplicant, wpa_cli and ndwatch where it follows
 * `WIFI_IFACE`; and give each of the two the metric of its routers' IPv6
 * default routes (`set_ra_metric`). A failed fork is left for the next turn.
 *
 * The NTP configuration is read again on each turn that finds ntpd not
 * running, so a server an administrator writes into the file is used within
 * a second, and one written while ntpd runs is used once ntpd is killed.
 */
static void start_services(struct services *s)
{
    if (s->log.pid < 0) {
        s->log.pid = spawn_syslog();
        s->log.spawned = time(0);
    }
    if (s->sshd.pid < 0 && anyone_to_let_in()) {
        s->sshd.pid = spawn_sshd();
        if (s->sshd.pid >= 0) {
            s->sshd.spawned = time(0);
            printf("init: sshd: started as pid %ld\n", (long)s->sshd.pid);
            fflush(stdout);
        }
    }
    if (s->bus.pid < 0 && s->bus_wanted) {
        s->bus.pid = spawn_bus();
        s->bus.spawned = time(0);
    }
    if (s->cron.pid < 0 && s->cron_wanted) {
        s->cron.pid = spawn_cron();
        s->cron.spawned = time(0);
    }
    if (s->ntp.pid < 0 && ntp_servers()) {
        s->ntp.pid = spawn_ntpd();
        s->ntp.spawned = time(0);
    }
    if (s->desktop.pid < 0 && access("/usr/libexec/rach2-desktop", X_OK) == 0) {
        char *argv[] = {(char *)"rach2-desktop", NULL};

        s->desktop.pid = spawn_service("/usr/libexec/rach2-desktop", argv);
        s->desktop.spawned = time(0);
    }
    set_ra_metric(NETWORK_IFACE, NETWORK_RA_METRIC, &s->wired_metric);
    set_ra_metric(WIFI_IFACE, WIFI_RA_METRIC, &s->wifi_metric);
    start_ifplugd(&s->plug);
    start_ndwatch(&s->nd);
    start_wpa_supplicant(&s->wpa);
    start_wpa_cli(&s->wcli, &s->wpa);
    start_wifi_ndwatch(&s->wnd);
}

/** Whether `reaped` is the pid of one of the services `*s`, as for
 * `service_ended`. */
static int services_ended(struct services *s, pid_t reaped)
{
    return service_ended(&s->log, reaped) || service_ended(&s->sshd, reaped) ||
           service_ended(&s->bus, reaped) || service_ended(&s->cron, reaped) ||
           service_ended(&s->ntp, reaped) || service_ended(&s->plug, reaped) ||
           service_ended(&s->nd, reaped) || service_ended(&s->wpa, reaped) ||
           service_ended(&s->wcli, reaped) || service_ended(&s->wnd, reaped) ||
           service_ended(&s->desktop, reaped);
}

#endif

/**
 * Empty the directory `dir` of what a boot leaves there and, `depth` levels
 * down, each directory in it, which is then removed (`clear_var_run`). Only
 * an entry that is itself a directory is walked, so a link that could not
 * be unlinked is not followed out of `dir`. A directory that cannot be
 * opened, as on an image without one, is left alone.
 */
static void clear_dir(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d) {
        return;
    }
    while ((e = readdir(d)) != NULL) {
        char path[2 * NAME_MAX + 16];
        const char *n = e->d_name;
        struct stat st;

        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'))) {
            continue;
        }
        if (strchr(n, '/')) {
            fprintf(stderr, "init: %s: refusing a name that is not one: %s\n", dir, n);
            continue;
        }
        if (snprintf(path, sizeof(path), "%s/%s", dir, n) >= (int)sizeof(path)) {
            fprintf(stderr, "init: %s: name too long to remove: %s\n", dir, n);
            continue;
        }
        if (unlink(path) == 0) {
            continue;
        }
        if (depth > 0 && lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            clear_dir(path, depth - 1);
        }
        if (rmdir(path) != 0) {
            fprintf(stderr, "init: %s: cannot remove %s\n", dir, n);
        }
    }
    closedir(d);
}

/**
 * Empty /var/run.
 *
 * What is in it describes the machine as it was running, not as it is: `ifup`
 * records there which interfaces it has configured, and a DHCP client leaves
 * its pid. On a machine whose /var/run survives a reboot, the next `ifup -a`
 * reads the last boot's record, takes the interface as already configured,
 * and takes no lease at all, while a stale pid file names a process that is
 * either gone or somebody else. Linux keeps this directory in memory for the
 * same reason. This file system has nowhere to put it that a reboot clears, so
 * init clears it, first, before anything reads it.
 *
 * It removes two levels: each entry is unlinked, and a directory is emptied
 * of its own entries the same way and removed, so that the system bus's
 * directory goes with the socket in it; a directory one level deeper that is
 * not empty is reported, not descended into. Nothing else runs while init
 * does this, so no entry changes between the unlink and the walk. A name
 * with a slash in it is skipped with a message, since a name read from a
 * damaged file system would otherwise be joined into a path outside this
 * directory, and pid 1 would delete a file elsewhere at every boot.
 */
static void clear_var_run(void)
{
    clear_dir("/var/run", 1);
}

#ifdef INIT_METAL
/**
 * Mount a tmpfs at /tmp with the options of Debian 13's tmp.mount: mode
 * 1777, strictatime, nosuid and nodev, at most half the machine's memory
 * and 1048576 inodes.
 *
 * What programs keep in /tmp, a compiler's temporary files or a shader
 * cache that a browser builds at each start, is then made and removed in
 * memory, not through the root's creates and their ordered writes, and a
 * reboot clears it. What the root's own /tmp holds is hidden while the
 * mount lasts. A failure is reported and leaves /tmp on the root.
 */
static void mount_tmp(void)
{
    if (mount("tmpfs", "/tmp", "tmpfs", MS_STRICTATIME | MS_NOSUID | MS_NODEV,
              "mode=1777,size=50%,nr_inodes=1m") != 0) {
        fprintf(stderr, "init: /tmp: tmpfs not mounted: %s\n", strerror(errno));
    }
}
#endif

/* glibc's struct utmp, on i386 and on x86-64 alike: 384 bytes, its times 32
 * bits wide. */
#define UTMP_SIZE 384
#define UTMP_LINE 8   /* ut_line, 32 bytes */
#define UTMP_ID   40  /* ut_id, 4 bytes */
#define UTMP_USER 44  /* ut_user, 32 bytes */
#define UTMP_HOST 76  /* ut_host, 256 bytes */
#define UTMP_TV   340 /* ut_tv.tv_sec */
#define UTMP_BOOT 2   /* BOOT_TIME, the ut_type */

/** Put the 32-bit value `v` at `p`, least significant byte first. */
static void put_le32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/** The second the kernel started, the btime of /proc/stat, or now when that
 * cannot be read. */
static unsigned long boot_second(void)
{
    char text[4096];
    const char *at;
    int fd = open("/proc/stat", O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, text, sizeof(text) - 1) : -1;

    if (fd >= 0) {
        close(fd);
    }
    text[n > 0 ? n : 0] = '\0';
    at = strstr(text, "\nbtime ");
    return at ? strtoul(at + 7, NULL, 10) : (unsigned long)time(NULL);
}

/**
 * Write the record of this boot, as sysvinit writes it, to the descriptor
 * `fd`: its type BOOT_TIME, line "~", id "~~", user "reboot" and host the
 * kernel's release, at the second the kernel started. coreutils' `who -b`
 * and `uptime` take the boot time from this record in utmp, and without one
 * from the time utmp was last changed, which is the last login; `last -x`
 * lists the one in wtmp.
 */
static void write_boot_record(int fd)
{
    unsigned char r[UTMP_SIZE];
    struct utsname u;

    memset(r, 0, sizeof(r));
    r[0] = UTMP_BOOT;
    r[UTMP_LINE] = '~';
    memcpy(r + UTMP_ID, "~~", 2);
    memcpy(r + UTMP_USER, "reboot", 6);
    if (uname(&u) == 0) {
        strncpy((char *)r + UTMP_HOST, u.release, 255);
    }
    put_le32(r + UTMP_TV, boot_second());
    if (write(fd, r, sizeof(r)) != (ssize_t)sizeof(r)) {
        fprintf(stderr, "init: boot record: %s\n", strerror(errno));
    }
}

/**
 * Make /var/run/utmp, where glibc's programs keep the records of who is
 * logged in, with only the record of this boot: OpenSSH's server on the GNU
 * system's root adds a session's record there, and `who`, `users` and `w`
 * read them. /var/log/wtmp and /var/log/btmp are kept across boots, the
 * first given this boot's record too: OpenSSH adds the history of logins
 * and of failed ones there, which util-linux's `last` and `lastb` read.
 * glibc writes a record only into a file that exists, which a Linux system
 * makes at boot as this does. The musl programs keep no records, so on the
 * other images the files have only the boot records.
 */
static void make_utmp(void)
{
    int fd = open("/var/run/utmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd >= 0) {
        write_boot_record(fd);
        close(fd);
    }
    fd = open("/var/log/wtmp", O_WRONLY | O_CREAT | O_APPEND, 0664);
    if (fd >= 0) {
        write_boot_record(fd);
        close(fd);
    }
    fd = open("/var/log/btmp", O_WRONLY | O_CREAT, 0600);
    if (fd >= 0) {
        close(fd);
    }
}

/**
 * Parse one limit value of `/etc/rach2/limits` into `*value`: a decimal
 * number, or `unlimited`, which is `RLIM_INFINITY`. Returns 0, or -1 for any
 * other word, a number with a sign, or a number at or past `RLIM_INFINITY`.
 */
static int limit_value(const char *word, rlim_t *value)
{
    char *end;
    unsigned long long v;

    if (strcmp(word, "unlimited") == 0) {
        *value = RLIM_INFINITY;
        return 0;
    }
    if (word[0] == '-' || word[0] == '+') {
        return -1;
    }
    errno = 0;
    v = strtoull(word, &end, 10);
    if (*end != '\0' || end == word || errno == ERANGE || v >= (unsigned long long)RLIM_INFINITY) {
        return -1;
    }
    *value = (rlim_t)v;
    return 0;
}

/**
 * Set the resource limits every session starts with, from
 * `/etc/rach2/limits`: one line per limit, `nproc`, `nofile` or `as`, then
 * the soft and the hard value, a number or `unlimited`. A line that begins
 * with `#` or is empty is skipped.
 *
 * They are set on init itself, and fork and exec hand them down to every
 * login and everything it starts, root's included, and root may raise them.
 * A line that does not parse, or a limit `setrlimit` does not take, is
 * reported and skipped; a missing file leaves the kernel's own defaults.
 */
static void apply_limits(void)
{
    FILE *f = fopen("/etc/rach2/limits", "r");
    char line[128];
    int lineno = 0;

    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char name[16], soft[32], hard[32];
        struct rlimit rl;
        int resource;

        lineno++;
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        if (sscanf(line, "%15s %31s %31s", name, soft, hard) != 3) {
            fprintf(stderr, "init: limits: line %d: not a name and two values\n", lineno);
            continue;
        }
        if (strcmp(name, "nproc") == 0) {
            resource = RLIMIT_NPROC;
        } else if (strcmp(name, "nofile") == 0) {
            resource = RLIMIT_NOFILE;
        } else if (strcmp(name, "as") == 0) {
            resource = RLIMIT_AS;
        } else {
            fprintf(stderr, "init: limits: line %d: %s is not a limit\n", lineno, name);
            continue;
        }
        if (limit_value(soft, &rl.rlim_cur) != 0 || limit_value(hard, &rl.rlim_max) != 0) {
            fprintf(stderr, "init: limits: line %d: a value is not a number\n", lineno);
            continue;
        }
        if (setrlimit(resource, &rl) != 0) {
            fprintf(stderr, "init: limits: line %d: %s refused: %s\n", lineno, name,
                    strerror(errno));
        }
    }
    fclose(f);
}

/**
 * Apply one settings file with BusyBox's `sysctl -q -p`, and wait for it.
 * sysctl reports a line it cannot apply on standard error and goes on with
 * the rest, and init reports that it failed and goes on too.
 */
static void apply_sysctl_file(const char *path)
{
    char *argv[5];
    int status;
    pid_t pid;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "init: sysctl: cannot fork\n");
        return;
    }
    if (pid == 0) {
        argv[0] = (char *)"sysctl";
        argv[1] = (char *)"-q";
        argv[2] = (char *)"-p";
        argv[3] = (char *)path;
        argv[4] = 0;
        execve(BUSYBOX_PROGRAM, argv, environ);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "init: sysctl: %s was not applied whole (status 0x%x)\n", path,
                (unsigned)status);
    }
}

/* A settings file `apply_sysctl` found: its name, and its directory's index
 * in `sysctl_dirs`. */
struct sysctl_file {
    char name[NAME_MAX + 1];
    int dir;
};

/** Order two `sysctl_file` entries by name, for qsort(3). */
static int sysctl_file_order(const void *a, const void *b)
{
    return strcmp(((const struct sysctl_file *)a)->name, ((const struct sysctl_file *)b)->name);
}

/**
 * Apply the image's kernel settings as procps's `sysctl --system` does: each
 * file NAME.conf of `sysctl_dirs`, in the order of the names, where a name
 * found in an earlier directory masks the same name in a later one, and
 * then `SYSCTL_CONF`. A name masks whatever it is, so a link to /dev/null
 * masks the file and applies nothing: only a regular file is applied. A
 * directory or file the image lacks is passed over. The list of names grows
 * as needed; a name it has no memory for is reported and not applied.
 */
static void apply_sysctl(void)
{
    struct sysctl_file *files = NULL;
    struct sysctl_file *grown;
    char path[PATH_MAX];
    struct dirent *e;
    struct stat st;
    size_t len;
    unsigned d;
    int room = 0;
    int n = 0;
    int i;
    DIR *dir;

    for (d = 0; d < sizeof(sysctl_dirs) / sizeof(sysctl_dirs[0]); d++) {
        dir = opendir(sysctl_dirs[d]);
        if (!dir) {
            continue;
        }
        while ((e = readdir(dir)) != NULL) {
            len = strlen(e->d_name);
            if (e->d_name[0] == '.' || len < 6 || strcmp(e->d_name + len - 5, ".conf") != 0) {
                continue;
            }
            for (i = 0; i < n && strcmp(files[i].name, e->d_name) != 0; i++) {
            }
            if (i < n) {
                continue;
            }
            if (n == room) {
                grown = realloc(files, (size_t)(room ? 2 * room : 32) * sizeof(files[0]));
                if (!grown) {
                    fprintf(stderr, "init: sysctl: out of memory; %s/%s not applied\n",
                            sysctl_dirs[d], e->d_name);
                    continue;
                }
                files = grown;
                room = room ? 2 * room : 32;
            }
            memcpy(files[n].name, e->d_name, len + 1);
            files[n].dir = (int)d;
            n++;
        }
        closedir(dir);
    }
    if (n > 0) {
        qsort(files, (size_t)n, sizeof(files[0]), sysctl_file_order);
    }
    for (i = 0; i < n; i++) {
        if (snprintf(path, sizeof(path), "%s/%s", sysctl_dirs[files[i].dir], files[i].name) <
                (int)sizeof(path) &&
            stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            apply_sysctl_file(path);
        }
    }
    free(files);
    if (access(SYSCTL_CONF, R_OK) == 0) {
        apply_sysctl_file(SYSCTL_CONF);
    }
}

#ifndef INIT_RECOVERY
/**
 * Clear GRUB's `recordfail` on the boot partition, now that this boot has
 * reached user space, as Ubuntu's boot does with `grub-editenv unset
 * recordfail`: the disk's menu sets it before it boots the candidate, and
 * boots the fallback while it stays set, so that a candidate that resets
 * before it gets here is not booted again (bootctl(8)). Only a boot from a
 * kernel slot of the disk's menu, whose line has `rach2.slot=candidate` or
 * `rach2.slot=fallback`, does it, through `bootctl booted`, which writes
 * nothing when the flag is not set; init waits for it and says when it
 * failed.
 */
static void clear_recordfail(void)
{
    char *argv[3];
    int status;
    pid_t pid;

    if (!boot_line_has("rach2.slot=candidate") && !boot_line_has("rach2.slot=fallback")) {
        return;
    }
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "init: recordfail: cannot fork\n");
        return;
    }
    if (pid == 0) {
        argv[0] = (char *)"bootctl";
        argv[1] = (char *)"booted";
        argv[2] = 0;
        execve(BOOTCTL_PROGRAM, argv, environ);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "init: recordfail: bootctl booted failed (status 0x%x)\n",
                (unsigned)status);
    }
}
#endif

#ifndef INIT_RECOVERY
/**
 * Whether the ext2 volume on the partition node `node`, mounted read-only on
 * `point`, needs e2fsck: its superblock's valid bit is clear or its error
 * bit set. Puts its UUID in `uuid` and says so on the console when it does.
 *
 * A read-only mount writes nothing, the superblock's state among it, so the
 * state on the disk is the one the kernel found at the mount, whose check
 * ran and stopped, or which another reason kept read-only.
 */
static int ext2_needs_check(const char *node, const char *point, char uuid[37])
{
    unsigned char sb[512]; /* a raw window reads whole sectors */
    unsigned state;

    if (!ext2_superblock(node, sb)) {
        return 0;
    }
    state = (unsigned)sb[58] | (unsigned)sb[59] << 8; /* s_state */
    if ((state & 1) != 0 && (state & 2) == 0) {
        return 0;
    }
    ext2_uuid(sb, uuid);
    fprintf(stderr, "init: fsck: %s on %s: read-only and %s (UUID %s)\n", node, point,
            (state & 2) ? "with errors" : "not clean", uuid);
    return 1;
}

/**
 * Send the machine to recovery's `e2fsck -p` when the kernel left an ext2
 * volume of a disk read-only for e2fsck, as a Linux distribution checks a
 * volume that is not clean at boot before it mounts it writable.
 *
 * The kernel checks a volume not cleanly unmounted when it mounts it, and
 * when its check stops at a fault or at its own limit, it mounts the volume
 * read-only and frees nothing on it, so that a damaged volume would lose
 * space until e2fsck ran. This kernel takes no raw write of a partition, so
 * e2fsck runs in the recovery system: each ext2 mount of a `/dev/diskNpM`
 * that /proc/mounts lists as `ro` and whose superblock says it needs e2fsck
 * (`ext2_needs_check`) is named by its UUID to `bootctl fsck`, which arms
 * one boot of recovery's check, and init restarts the machine at once, before
 * anything has run from the volume. Recovery restarts it again when the
 * check is done, and the boot after it takes the saved default and mounts
 * the volume writable. `bootctl` refuses a second arming within the hour of
 * the first, so that a fault the check does not mend leaves the volume
 * read-only instead of restarting the machine for ever; init then says so
 * and goes on.
 *
 * Only a boot from a slot of a disk's menu, whose line has `rach2.slot=` of
 * the candidate, the fallback or a trial root, does this, since the menu
 * slot is where the check is armed. init waits for `bootctl`.
 */
static void check_unclean_volumes(void)
{
    char line[512], list[160];
    size_t used = 0;
    char *argv[4];
    int status;
    pid_t pid;
    FILE *f;

    if (!boot_line_has("rach2.slot=candidate") && !boot_line_has("rach2.slot=fallback") &&
        !boot_line_has("rach2.slot=trialroot")) {
        return;
    }
    f = fopen("/proc/mounts", "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char source[64], point[256], type[16], options[128], uuid[37];

        if (sscanf(line, "%63s %255s %15s %127s", source, point, type, options) != 4 ||
            strcmp(type, "ext2") != 0 || strncmp(source, "/dev/disk", 9) != 0 ||
            strncmp(options, "ro", 2) != 0 || (options[2] != '\0' && options[2] != ',') ||
            !ext2_needs_check(source, point, uuid) || used + 1 + 36 >= sizeof(list)) {
            continue;
        }
        if (used > 0) {
            list[used++] = ',';
        }
        memcpy(list + used, uuid, 37);
        used += 36;
    }
    fclose(f);
    if (used == 0) {
        return;
    }
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "init: fsck: cannot fork; the volume stays read-only\n");
        return;
    }
    if (pid == 0) {
        argv[0] = (char *)"bootctl";
        argv[1] = (char *)"fsck";
        argv[2] = list;
        argv[3] = 0;
        execve(BOOTCTL_PROGRAM, argv, environ);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr,
                "init: fsck: bootctl fsck failed (status 0x%x); the volume stays read-only\n",
                (unsigned)status);
        return;
    }
    fprintf(stderr, "init: fsck: restarting into recovery's e2fsck -p\n");
    shutdown_handler(SIGTERM);
}
#endif

/**
 * Set the machine up before the first child: empty `/var/run` and give it a
 * `utmp` with only this boot's record, set the limits, open the service log
 * and write the backlights' and the mixer's kept levels back
 * (`keep_backlights`, `keep_mixer`), apply the image's kernel settings
 * (`apply_sysctl`), and put IPv6's stable secret, kept in /var/lib, in force
 * (`ip6secret_keep`), so that the addresses the routers' prefixes give stay
 * the same from one boot to the next, whoever configures the network. The
 * settings come first, since a card that took a prefix before init is
 * restarted with the secret, and so makes its addresses again under them.
 * The recovery build takes the key the installer put on the recovery
 * partition it booted from, says on the console whether a key was built into
 * its image and whether the partition's turned SSH on, since the two kinds of
 * image look identical from outside, and names each disk whose GUID table was
 * read from its backup. A metal boot whose line has `rach2.fsck` then checks
 * its ext2 partitions, and recovery checks those `rach2.fsck=` names and
 * restarts (`check_named_volumes`). Every other boot from a slot of a disk's
 * menu then clears GRUB's `recordfail` (`clear_recordfail`), and sends the
 * machine to recovery's check when the kernel left a volume read-only for
 * e2fsck (`check_unclean_volumes`); the order lets the restart take the saved
 * default and not the fallback.
 */
static void set_up(void)
{
#ifdef INIT_METAL
    mount_tmp();
#endif
    clear_var_run();
    make_utmp();
    apply_limits();
    open_service_log();
    keep_backlights();
    keep_mixer();
    apply_sysctl();
    (void)ip6secret_keep("init: ipv6");
#ifdef INIT_RECOVERY
    report_recovery_keys();
    report_backup_tables();
#endif
#ifdef INIT_METAL
    if (boot_line_has("rach2.fsck")) {
        (void)check_ext2_partitions(NULL);
    }
#endif
#ifdef INIT_RECOVERY
    check_named_volumes();
#else
    clear_recordfail();
    check_unclean_volumes();
#endif
}

/**
 * Reap a child that has ended or stopped, waiting for one at most a second.
 * Returns what `waitpid` returned, with its status in `*status`: a pid, 0
 * when no child changed within the second, or -1 with errno set.
 *
 * SIGCHLD is blocked from before the first `waitpid` until after the
 * `sigtimedwait`, so a child that changes between the two leaves its signal
 * pending and the wait returns at once. The mask is put back before the
 * return, and init forks with SIGCHLD unblocked, so no child inherits it
 * blocked. A SIGCHLD that is still pending then is discarded, its default
 * action being to ignore it.
 */
static pid_t child_wait(int *status)
{
    static const struct timespec turn = {1, 0};
    sigset_t child, saved;
    pid_t reaped;

    sigemptyset(&child);
    sigaddset(&child, SIGCHLD);
    (void)sigprocmask(SIG_BLOCK, &child, &saved);
    reaped = waitpid(-1, status, WUNTRACED | WNOHANG);
    if (reaped == 0) {
        (void)sigtimedwait(&child, NULL, &turn);
        reaped = waitpid(-1, status, WUNTRACED | WNOHANG);
    }
    (void)sigprocmask(SIG_SETMASK, &saved, NULL);
    return reaped;
}

/**
 * Act on the child `child`, which `child_wait` reported stopped: continue it
 * when it is the console session `shell`, since `^Z` at the console prompt
 * stops the session and there is no shell above it to type `fg`. Any other
 * child stays stopped until something sends it SIGCONT, as under Linux's
 * inits, which do not wait for stopped children: a daemon, such as the DHCP
 * client `udhcpc -b` leaves in the background, is init's child once its
 * first parent has gone, and a process that stops it on purpose must find it
 * stopped.
 */
static void stopped_child(pid_t child, pid_t shell)
{
    if (child == shell) {
        (void)kill(child, SIGCONT);
    }
}

/**
 * Set the machine up, then reap, respawn and supervise for as long as it runs.
 *
 * The three shutdown handlers go in before the first child, so a request that
 * arrives early is answered and does not meet a terminating default, which for
 * pid 1 is a panic. Then init's path becomes SERVICE_PATH, which every child
 * inherits, the machine is set up (`set_up`), the login builds choose the
 * console session (`want_term`), and the keyboard is set.
 *
 * Each turn respawns what is missing: the console session, whose failed fork
 * (a full process table) is retried after a second so the loop does not spin,
 * and each start of which as `term` is said on the console, which COM1 shows;
 * acpid while `acpid_wanted`; and on the metal builds the
 * network, once, after the console
 * session and before the SSH server that needs it, then the server wherever
 * `anyone_to_let_in` allows one, D-Bus's system bus and the cron daemon
 * where the image has them (`services_found`), and ntpd while
 * /etc/ntp.conf names a server. A failed fork of a service is left for
 * the next turn, so the loop keeps reaping the orphans that filled the
 * process table.
 *
 * The wait is `child_wait`, which reaps a child as soon as its SIGCHLD comes
 * and otherwise ends the turn after a second, because init must also bound
 * the service log while its children live and save the backlights' and the
 * mixer's levels that changed (`save_changed_levels`). The log is looked at
 * every `SERVICE_LOG_CHECK_TURNS` turns, busy or idle, since a machine whose
 * children keep dying never takes the idle path. `WUNTRACED` reports a
 * stopped child, which `stopped_child` continues only when it is the console
 * session.
 *
 * A console session or a service that dies within two seconds of its start
 * is taken to have a permanent reason (a missing or crashing binary, no
 * entropy for the host key, no network, a port already taken), and is
 * respawned only after a pause, so the loop does not spin; an interactive
 * session outlives that window, so an ordinary `exit` brings the prompt back
 * at once. A failed `term` is followed at once by the kernel console's
 * login, which a person needs then, and that login's own failures pause.
 */
int main(void)
{
    pid_t shell = -1;
    time_t spawned = 0;
#ifdef INIT_METAL
    struct services services;
    int network_done = 0;
#endif
    struct service acpid = {-1, 0};
    unsigned turns = 0;

    catch_shutdown();
    (void)setenv("PATH", SERVICE_PATH, 1);
    set_up();
#ifdef INIT_CONSOLE_LOGIN
    want_term();
#endif
    configure_console();
#ifdef INIT_METAL
    services_found(&services);
#endif

    for (;;) {
        int status;
        pid_t reaped;

        if (shell < 0) {
            shell = spawn_shell();
            if (shell < 0) {
                sleep(1);
                continue;
            }
            spawned = time(0);
#ifdef INIT_CONSOLE_LOGIN
            if (term_now) {
                fprintf(stderr, "init: console: starting term\n");
            }
#endif
        }
        start_acpid(&acpid);

#ifdef INIT_METAL
        if (!network_done) {
            network_done = 1;
            if (boot_wants_userland_network()) {
                start_network();
            }
        }

        start_services(&services);
#endif

        save_changed_levels();
        if (++turns >= SERVICE_LOG_CHECK_TURNS) {
            turns = 0;
            bound_service_log();
        }
        reaped = child_wait(&status);
        if (reaped == 0) {
            continue;
        }
        if (reaped < 0 && errno == EINTR) {
            continue;
        }
        if (reaped > 0 && WIFSTOPPED(status)) {
            stopped_child(reaped, shell);
            continue;
        }
        if (service_ended(&acpid, reaped)) {
            continue;
        }
#ifdef INIT_METAL
        if (services_ended(&services, reaped)) {
            continue;
        }
#endif
        if (reaped > 0 && reaped == shell) {
            shell = -1;
#ifdef INIT_CONSOLE_LOGIN
            if (session_ended(status, time(0) - spawned)) {
                continue;
            }
#endif
            if (time(0) - spawned < 2) {
#ifdef INIT_CONSOLE_LOGIN
                sleep(LOGIN_RESPAWN_PAUSE);
#else
                sleep(1);
#endif
            }
        }
    }
    return 0; /* unreachable */
}
