/*
 * krffsmount_scenario.c: the scenario of machine.krffsmount, in which
 * krffsprobe mounts a KRFFS volume with mount(2) from the kernel's debug
 * shell, and the host judges what each boot left on the disk with
 * fsck.krffs: a volume read, written, read back, left by a machine killed
 * while writing and repaired; a volume broken on purpose; the volumes the
 * kernel refuses; a write the device fails; three volumes at once; and a
 * stick pulled from under a mounted volume and plugged in again.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "history_json.h"
#include "lifecycle.h"

/* The offset of the KRFFS partition, at LBA 2048, on every disk of the
 * family. */
#define PART_OFFSET 1048576L

/* One attempt, with what the scenario counts across its boots. */
struct km {
    struct lc l;
    int boots;        /* the boots of machine `fixture` so far */
    int checks;       /* the runs of fsck.krffs so far, the witness's units */
    char console[40]; /* the console of the boot that runs */
};

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Begin step `step`, boot machine `fixture` and wait for the debug shell's
 * prompt.
 *
 * @return the machine, or NULL when the scenario cannot go on.
 */
static struct etude_case_machine *shell(struct km *k, const char *step)
{
    struct etude_case_machine *m;
    uint64_t at = 0;

    if (!lc_step(&k->l, step) || (m = lc_boot(&k->l, "fixture")) == NULL)
        return NULL;
    snprintf(k->console, sizeof(k->console), "fixture.%d.console", ++k->boots);
    if (!lc_wait(&k->l, m, "Type 'help'", &at, NULL) || !lc_wait(&k->l, m, "\n> ", &at, NULL))
        return NULL;
    return m;
}

/**
 * Wait for krffsprobe's next line after `*at`, and copy what follows
 * `KRFFSPROBE ` on it into `line`.
 *
 * @return 1 when the line came, else 0, with the scenario halted.
 */
static int probe_line(struct km *k, struct etude_case_machine *m, uint64_t *at, char *line,
                      size_t size)
{
    uint64_t from = *at;

    if (!lc_wait(&k->l, m, "KRFFSPROBE ", at, NULL) || !lc_wait(&k->l, m, "\n", at, NULL))
        return 0;
    lc_text(&k->l, k->console, "KRFFSPROBE ", from, line, size);
    return 1;
}

/**
 * Report assertion `id` from the probe line that comes after `*at`: it
 * holds when the line is `want`. A wait that did not end with the line fails
 * it too, unless the case was stopped.
 *
 * @return 1 when the assertion held.
 */
static int expect_line(struct km *k, struct etude_case_machine *m, uint64_t *at, const char *want,
                       const char *id)
{
    char line[160], detail[200];

    if (!lc_ok(&k->l))
        return 0;
    if (!probe_line(k, m, at, line, sizeof(line))) {
        if (!k->l.stopped)
            lc_assert(&k->l, id, 0, k->l.why);
        return 0;
    }
    snprintf(detail, sizeof(detail), "krffsprobe says KRFFSPROBE %s", line);
    lc_assert(&k->l, id, strcmp(line, want) == 0, detail);
    return strcmp(line, want) == 0;
}

/**
 * Run `krffsprobe MODE` from the debug shell of `m` and report assertion
 * `id` from its last line, which must be `KRFFSPROBE OK (MODE)`; a line of
 * `KRFFSPROBE FAIL` fails it. The machine is then ended with QMP's quit.
 */
static void run_probe(struct km *k, struct etude_case_machine *m, const char *mode, const char *id)
{
    char command[64], want[64];
    uint64_t at;

    if (m == NULL)
        return;
    snprintf(command, sizeof(command), "exec /krffsprobe %s\r", mode);
    snprintf(want, sizeof(want), "OK (%s)", mode);
    at = 0;
    if (lc_type(&k->l, m, command))
        expect_line(k, m, &at, want, id);
    lc_quit(&k->l, m, NULL);
}

/**
 * Report assertion `id` from the console of `m`, which has ended: it holds
 * when a line has `key` and, after it on the same line, `rest`.
 */
static void find_line(struct km *k, struct etude_case_machine *m, const char *key, const char *rest,
                      const char *id)
{
    char line[160], detail[200];
    uint64_t from = 0, at;
    size_t n = strlen(key);

    if (!lc_ok(&k->l))
        return;
    while (etude_console_find(m, key, from, &at) == ETUDE_OK) {
        if (lc_text(&k->l, k->console, key, at - n, line, sizeof(line)) &&
            strstr(line, rest) != NULL) {
            lc_assert(&k->l, id, 1, NULL);
            return;
        }
        from = at;
    }
    snprintf(detail, sizeof(detail), "no line of the console says '%s' and then '%s'", key, rest);
    lc_assert(&k->l, id, 0, detail);
}

/** Give the size of file `path` in bytes, or -1 when it cannot be read. */
static long file_size(const char *path)
{
    FILE *f = path != NULL ? fopen(path, "rb") : NULL;
    long size = -1;

    if (f != NULL && fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    if (f != NULL)
        fclose(f);
    return size;
}

/** Say whether file `path` has `text`, reading at most its first MiB. */
static int file_has(const char *path, const char *text)
{
    FILE *f = path != NULL ? fopen(path, "rb") : NULL;
    size_t n;
    char *buf;
    int found;

    if (f == NULL)
        return 0;
    buf = malloc((1u << 20) + 1);
    n = buf != NULL ? fread(buf, 1, 1u << 20, f) : 0;
    fclose(f);
    if (buf == NULL)
        return 0;
    buf[n] = '\0';
    found = strstr(buf, text) != NULL;
    free(buf);
    return found;
}

/**
 * Report assertion `id`: the bytes of file `disk` from PART_OFFSET are
 * those of file `volume`, as many as it has.
 */
static void same_volume(struct km *k, const char *disk, const char *volume, const char *id)
{
    FILE *fd = disk != NULL ? fopen(disk, "rb") : NULL;
    FILE *fv = volume != NULL ? fopen(volume, "rb") : NULL;
    char bd[65536], bv[65536];
    size_t nd = 0, nv = 0;
    int same = 0;

    if (fd != NULL && fv != NULL && fseek(fd, PART_OFFSET, SEEK_SET) == 0) {
        do {
            nv = fread(bv, 1, sizeof(bv), fv);
            nd = fread(bd, 1, nv, fd);
        } while (nv > 0 && nd == nv && memcmp(bd, bv, nv) == 0);
        same = nv == 0 && ferror(fv) == 0 && ferror(fd) == 0;
    }
    if (fd != NULL)
        fclose(fd);
    if (fv != NULL)
        fclose(fv);
    lc_assert(&k->l, id, same,
              fd == NULL || fv == NULL ? "the copy or the volume cannot be read"
                                       : "the volume on the copy differs from the volume built");
}

/**
 * Run `fsck.krffs --strict` on the volume of the private copy of disk
 * artifact `disk`, which has the size of artifact `volume`, keeping its
 * output as evidence `output`. Assertion `id` holds, when `dirty` is clear,
 * when it exits 0, a volume that needs no repair; and when `dirty` is set,
 * when it exits with another status and says that the volume was not
 * cleanly unmounted.
 */
static void fsck_strict(struct km *k, const char *disk, int dirty, const char *output,
                        const char *id)
{
    static const char unclean[] = "not cleanly unmounted";
    char name[] = "fsck.krffs", strict[] = "--strict", offset[] = "--offset", where[24],
         size[] = "--size", bytes[24], path[1024], detail[200];
    char *argv[] = {name, path, strict, offset, where, size, bytes, NULL};
    const char *copy = etude_private_copy(k->l.c, disk);
    long volume = file_size(etude_artifact(k->l.c, "volume"));
    struct etude_termination end;
    enum etude_status st;
    char err[64];
    int said;

    if (!lc_ok(&k->l))
        return;
    if (copy == NULL || strlen(copy) >= sizeof(path) || volume <= 0) {
        lc_halt(&k->l, ETUDE_REFUSED, "the copy of the disk or the volume's size");
        return;
    }
    snprintf(path, sizeof(path), "%s", copy);
    snprintf(where, sizeof(where), "%ld", PART_OFFSET);
    snprintf(bytes, sizeof(bytes), "%ld", volume);
    etude_progress(k->l.c, "fsck", (uint64_t)k->checks);
    st = etude_host(k->l.c, argv, output, &end);
    if (st != ETUDE_OK) {
        lc_halt(&k->l, st, "fsck.krffs");
        return;
    }
    etude_progress(k->l.c, "fsck", (uint64_t)++k->checks);
    snprintf(err, sizeof(err), "%s.err", output);
    said = file_has(etude_evidence(k->l.c, output), unclean) ||
           file_has(etude_evidence(k->l.c, err), unclean);
    snprintf(detail, sizeof(detail), "fsck.krffs --strict ended %s %d, and %s",
             end.end == ETUDE_END_EXITED ? "with status" : "by signal", end.value,
             said ? "says the volume was not cleanly unmounted"
                  : "does not say the volume was not cleanly unmounted");
    if (end.end != ETUDE_END_EXITED)
        lc_assert(&k->l, id, 0, detail);
    else if (dirty)
        lc_assert(&k->l, id, end.value != 0 && said, detail);
    else
        lc_assert(&k->l, id, end.value == 0, detail);
}

/**
 * Run the case of one volume through six boots in the order a machine meets
 * them: read-only, written, read back, killed while writing, read back
 * again, and written by the mount that checks what the kill left. The host
 * checks the volume after each boot that wrote it.
 */
static void volume(struct km *k)
{
    struct etude_case_machine *m;
    uint64_t at = 0;

    if ((m = shell(k, "read")) != NULL) {
        run_probe(k, m, "read", "read-ok");
        find_line(k, m, "krffs: mount: ", " bytes, read-only", "read-only-mount");
    }
    if (lc_step(&k->l, "unchanged"))
        same_volume(k, etude_private_copy(k->l.c, "volume-disk"), etude_artifact(k->l.c, "volume"),
                    "unchanged");
    if ((m = shell(k, "write")) != NULL) {
        run_probe(k, m, "write", "write-ok");
        find_line(k, m, "krffs: mount: ", " bytes, read-write", "read-write-mount");
    }
    if (lc_step(&k->l, "write-fsck"))
        fsck_strict(k, "volume-disk", 0, "write.fsck", "write-fsck-clean");
    if ((m = shell(k, "readback")) != NULL)
        run_probe(k, m, "readback", "readback-ok");
    if ((m = shell(k, "crash")) != NULL && lc_type(&k->l, m, "exec /krffsprobe crash\r")) {
        expect_line(k, m, &at, "WRITING", "crash-writing");
        lc_quit(&k->l, m, NULL);
    }
    if (lc_step(&k->l, "crash-fsck"))
        fsck_strict(k, "volume-disk", 1, "crash.fsck", "crash-fsck-dirty");
    if ((m = shell(k, "crash-readback")) != NULL)
        run_probe(k, m, "readback", "crash-readback-ok");
    if ((m = shell(k, "repair")) != NULL) {
        run_probe(k, m, "write", "repair-ok");
        lc_find(&k->l, m, "repair-checked", "krffs: check: the volume was not unmounted cleanly", 0,
                1);
    }
    if (lc_step(&k->l, "repair-fsck"))
        fsck_strict(k, "volume-disk", 0, "repair.fsck", "repair-fsck-clean");
}

/**
 * Run the case of a volume broken on purpose: a wrong backward link, a
 * reserved node with no name and one name on two nodes, with the dirty flag
 * set, which the mount that writes must repair and count.
 */
static void repair(struct km *k)
{
    struct etude_case_machine *m;

    if ((m = shell(k, "write")) != NULL) {
        run_probe(k, m, "write", "probe-ok");
        lc_find(&k->l, m, "repaired",
                "krffs: check: repaired: 1 backward link(s), 2 orphan(s), 1 repeated name(s)", 0,
                1);
    }
    if (lc_step(&k->l, "fsck"))
        fsck_strict(k, "broken-disk", 0, "fsck", "fsck-clean");
}

/** Run the case of two writers on one mount. */
static void writers(struct km *k)
{
    run_probe(k, shell(k, "writers"), "writers", "probe-ok");
    if (lc_step(&k->l, "fsck"))
        fsck_strict(k, "volume-disk", 0, "fsck", "fsck-clean");
}

/** Run a case of a volume the kernel refuses with the line `line`. */
static void refuse(struct km *k, const char *line)
{
    struct etude_case_machine *m;

    if ((m = shell(k, "refuse")) != NULL) {
        run_probe(k, m, "refuse", "probe-ok");
        lc_find(&k->l, m, "refused", line, 0, 1);
    }
}

/**
 * Run the case of a write the device fails, which takes the mount
 * read-only; its unmount must leave the dirty flag set.
 */
static void failed(struct km *k)
{
    struct etude_case_machine *m;

    if ((m = shell(k, "failed")) != NULL) {
        run_probe(k, m, "failed", "probe-ok");
        lc_find(&k->l, m, "went-read-only", "vfs: read-only: ", 0, 1);
    }
    if (lc_step(&k->l, "fsck"))
        fsck_strict(k, "volume-disk", 1, "fsck", "fsck-dirty");
}

/**
 * Write into `out` the QMP arguments of the human monitor's `drive_add`
 * that makes drive `kst2` on file `path`: a JSON object whose command line
 * doubles each comma of the path, as QEMU's option syntax needs.
 *
 * The pulled stick's drive stays in QEMU with its device gone, and keeps its
 * lock on the file, so the second drive takes no lock of its own; without
 * that, QEMU refuses the second device the write lock.
 *
 * @return 1 when they fit and the path has no quote, backslash or control
 *         character, which the command line could not carry as it is.
 */
static int drive_add(const char *path, char *out, size_t size)
{
    static const char head[] = "{\"command-line\": \"drive_add 0 "
                               "if=none,id=kst2,format=raw,file.locking=off,file=";
    size_t n = strlen(head);

    if (path == NULL || size < n + 3)
        return 0;
    memcpy(out, head, n);
    for (; *path != '\0'; path++) {
        if (*path == '"' || *path == '\\' || (unsigned char)*path < 0x20 || n + 5 > size)
            return 0;
        if (*path == ',')
            out[n++] = ',';
        out[n++] = *path;
    }
    memcpy(out + n, "\"}", 3);
    return 1;
}

/**
 * Send QMP command `execute` with `arguments`, and say whether QMP returned
 * and, for the human monitor, whether the monitor's text is empty or begins
 * with OK, as `drive_add` says when it made the drive. What came back is put
 * in `detail`.
 */
static int qmp_ok(struct km *k, struct etude_case_machine *m, const char *execute,
                  const char *arguments, char *detail, size_t size)
{
    const struct eh_json *r = NULL;
    struct eh_json *doc = NULL;
    enum etude_status st;
    char *reply = NULL;
    int held;

    st = etude_qmp(m, execute, arguments, &reply);
    if (reply != NULL && (doc = eh_json_parse(reply)) != NULL)
        r = eh_json_get(doc, "return");
    held = st == ETUDE_OK && r != NULL &&
           (r->type != 's' || r->string[0] == '\0' || strncmp(r->string, "OK", 2) == 0);
    snprintf(detail, size, "QMP's %s answered %.120s", execute, reply != NULL ? reply : "nothing");
    eh_json_free(doc);
    free(reply);
    if (st != ETUDE_OK && st != ETUDE_FAILED)
        lc_halt(&k->l, st, execute);
    return held;
}

/**
 * Run the case of a stick pulled from under a mounted volume while the
 * probe writes it, and plugged in again, a second drive on the same file and
 * a second device, while the old mount is still there.
 */
static void pull(struct km *k)
{
    static const char device[] = "{\"driver\": \"usb-storage\", \"bus\": \"xhci.0\", "
                                 "\"drive\": \"kst2\", \"id\": \"kst1\"}";
    struct etude_case_machine *m;
    char add[1200], detail[200];
    uint64_t at = 0;
    int held;

    if ((m = shell(k, "pull")) == NULL || !lc_type(&k->l, m, "exec /krffsprobe pull\r"))
        return;
    if (expect_line(k, m, &at, "HOLDING", "holding")) {
        held = qmp_ok(k, m, "device_del", "{\"id\": \"kst0\"}", detail, sizeof(detail));
        if (!k->l.stopped)
            lc_assert(&k->l, "pulled", held, detail);
        if (held && expect_line(k, m, &at, "GONE", "gone")) {
            if (!drive_add(etude_private_copy(k->l.c, "stick"), add, sizeof(add))) {
                lc_halt(&k->l, ETUDE_REFUSED, "the path of the stick's copy");
                return;
            }
            held = qmp_ok(k, m, "human-monitor-command", add, detail, sizeof(detail)) &&
                   qmp_ok(k, m, "device_add", device, detail, sizeof(detail));
            if (!k->l.stopped)
                lc_assert(&k->l, "plugged", held, detail);
            if (held)
                expect_line(k, m, &at, "OK (pull)", "probe-ok");
        }
    }
    lc_quit(&k->l, m, NULL);
    lc_find(&k->l, m, "said-gone", "usbdisk: gone", 0, 1);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const volumes[] = {"read-ok",           "read-only-mount",
                                          "unchanged",         "write-ok",
                                          "read-write-mount",  "write-fsck-clean",
                                          "readback-ok",       "crash-writing",
                                          "crash-fsck-dirty",  "crash-readback-ok",
                                          "repair-ok",         "repair-checked",
                                          "repair-fsck-clean", NULL};
    static const char *const repairs[] = {"probe-ok", "repaired", "fsck-clean", NULL};
    static const char *const writes[] = {"probe-ok", "fsck-clean", NULL};
    static const char *const refusals[] = {"probe-ok", "refused", NULL};
    static const char *const failures[] = {"probe-ok", "went-read-only", "fsck-dirty", NULL};
    static const char *const threes[] = {"probe-ok", NULL};
    static const char *const pulls[] = {"holding",  "pulled",    "gone", "plugged",
                                        "probe-ok", "said-gone", NULL};
    const char *name = case_name(c);
    struct km k;

    memset(&k, 0, sizeof(k));
    if (strcmp(name, "volume") == 0) {
        lc_begin(&k.l, c, volumes);
        volume(&k);
    } else if (strcmp(name, "repair") == 0) {
        lc_begin(&k.l, c, repairs);
        repair(&k);
    } else if (strcmp(name, "writers") == 0) {
        lc_begin(&k.l, c, writes);
        writers(&k);
    } else if (strcmp(name, "refuse-magic") == 0) {
        lc_begin(&k.l, c, refusals);
        refuse(&k, "krffs: mount: refused: magic");
    } else if (strcmp(name, "refuse-chain") == 0 || strcmp(name, "refuse-region") == 0) {
        lc_begin(&k.l, c, refusals);
        refuse(&k, "krffs: mount: refused: the node chain is malformed");
    } else if (strcmp(name, "failed") == 0) {
        lc_begin(&k.l, c, failures);
        failed(&k);
    } else if (strcmp(name, "three") == 0) {
        lc_begin(&k.l, c, threes);
        run_probe(&k, shell(&k, "three"), "three", "probe-ok");
    } else if (strcmp(name, "pull") == 0) {
        lc_begin(&k.l, c, pulls);
        pull(&k);
    } else {
        return 1;
    }
    return lc_finish(&k.l);
}
