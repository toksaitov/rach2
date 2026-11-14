/*
 * cases.c: the rules of `etude check` for case manifests, beyond the shape that
 * tools/etude/schemas/case.schema.json gives them.
 *
 * A schema checks one value at a time; these rules relate values to each
 * other: an identity given once, an alternative that names declared
 * assertions, an artifact that preparation makes, the disks of a profile
 * that the launcher attaches together, a role that matches the end it
 * expects, a bound inside its floor and ceiling, a fixture where a placement
 * has it, and a `where` that names a line of the tree. Each rule follows a
 * sentence of tools/etude/Readme.md.
 */
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "etude/processor.h"
#include "etude/qemu.h"

/* The case a rule reads, and the file it came from. */
struct rule {
    const char *file;
    const struct json *c;
    const char *domain;
    const char *role;
    const char *plan;
};

/**
 * Returns the line of the member `name` of `v`, or `v`'s own when it has none.
 */
static int line_at(const struct json *v, const char *name)
{
    const struct json *m = json_member(v, name);

    return m != NULL ? m->line : v->line;
}

/** Returns 1 when `a` and `b` are both strings and equal. */
static int same(const char *a, const char *b)
{
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

/**
 * Returns 1 when an object of `array` has the string `value` as its `field`.
 */
int has_member(const struct json *array, const char *field, const char *value)
{
    size_t i;

    for (i = 0; array != NULL && i < array->count; i++) {
        if (same(json_text(array->item[i], field), value)) {
            return 1;
        }
    }
    return 0;
}

/** Reports each value of `field` that two objects of `array` share. */
static void unique(const struct rule *r, const struct json *array, const char *field,
                   const char *what)
{
    size_t i, j;

    for (i = 0; array != NULL && i < array->count; i++) {
        const char *id = json_text(array->item[i], field);

        for (j = 0; j < i; j++) {
            if (same(id, json_text(array->item[j], field))) {
                finding(r->file, array->item[i]->line, "%s: %s %s is declared twice",
                        json_text(r->c, "case_id"), what, id);
                break;
            }
        }
    }
}

/** Returns the fixture `kind` of the list `fixtures`, or NULL. */
static const struct json *fixture(const struct json *fixtures, const char *kind)
{
    size_t i;

    for (i = 0; fixtures != NULL && i < fixtures->count; i++) {
        if (same(json_text(fixtures->item[i], "fixture"), kind)) {
            return fixtures->item[i];
        }
    }
    return NULL;
}

/** Returns the native fixtures of the case, or NULL. */
static const struct json *native_fixtures(const struct json *c)
{
    return json_member(json_member(c, "native"), "fixtures");
}

/** Returns 1 when the alternative `alt` names `assertion_id`. */
static int names_assertion(const struct json *alt, const char *assertion_id)
{
    const struct json *ids = json_member(alt, "assertions");
    size_t i;

    for (i = 0; ids != NULL && i < ids->count; i++) {
        if (same(ids->item[i]->string, assertion_id)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Returns the branch group that names `assertion_id` in an alternative, and
 * the alternative in `*alternative`, or NULL when the assertion is in none.
 */
const struct json *group_of(const struct json *c, const char *assertion_id,
                            const struct json **alternative)
{
    const struct json *groups = json_member(c, "branches");
    size_t g, a;

    for (g = 0; groups != NULL && g < groups->count; g++) {
        const struct json *alts = json_member(groups->item[g], "alternatives");

        for (a = 0; alts != NULL && a < alts->count; a++) {
            if (names_assertion(alts->item[a], assertion_id)) {
                if (alternative != NULL) {
                    *alternative = alts->item[a];
                }
                return groups->item[g];
            }
        }
    }
    return NULL;
}

/**
 * Returns 1 when some declared assertion of the case is given in report
 * records: the program's, or a machine scenario's, whose report
 * libetude-case writes on the scenario's standard output as a program's.
 */
int program_reports(const struct json *c)
{
    const struct json *list = json_member(c, "assertions");

    return has_member(list, "source", "program") || has_member(list, "source", "scenario");
}

/* ---- the rules ---- */

/** An assertion's source must be one the case's domain has. */
static void rule_sources(const struct rule *r)
{
    const struct json *list = json_member(r->c, "assertions");
    const char *case_id = json_text(r->c, "case_id");
    size_t i;

    unique(r, list, "assertion_id", "the assertion");
    for (i = 0; list != NULL && i < list->count; i++) {
        const char *source = json_text(list->item[i], "source");
        const char *id = json_text(list->item[i], "assertion_id");
        int machine = same(r->domain, "machine");

        if (same(source, "scenario") && !machine) {
            finding(r->file, list->item[i]->line,
                    "%s: %s has the source scenario, which only a machine case has", case_id, id);
        } else if (same(source, "program") && machine) {
            finding(r->file, list->item[i]->line,
                    "%s: %s has the source program; a machine scenario reports as scenario",
                    case_id, id);
        } else if (same(source, "executor") && same(r->domain, "host")) {
            finding(r->file, list->item[i]->line,
                    "%s: %s has the source executor, but a host case has no kernel to record it",
                    case_id, id);
        } else if (same(source, "executor") && same(r->plan, "native") &&
                   fixture(native_fixtures(r->c), "log") == NULL) {
            finding(r->file, list->item[i]->line,
                    "%s: %s is a kernel record, which a native run reads only with the log "
                    "fixture",
                    case_id, id);
        }
    }
}

/**
 * Each alternative's assertions must be declared, and in one alternative only.
 */
static void rule_alternatives(const struct rule *r, const struct json *group)
{
    const struct json *alts = json_member(group, "alternatives");
    const struct json *declared = json_member(r->c, "assertions");
    const char *case_id = json_text(r->c, "case_id");
    size_t a, i;

    unique(r, alts, "alternative_id", "the alternative");
    for (a = 0; alts != NULL && a < alts->count; a++) {
        const struct json *ids = json_member(alts->item[a], "assertions");

        for (i = 0; ids != NULL && i < ids->count; i++) {
            const struct json *alt = NULL;
            const char *id = ids->item[i]->string;

            if (!has_member(declared, "assertion_id", id)) {
                finding(r->file, ids->item[i]->line,
                        "%s: the alternative %s names %s, which "
                        "the case does not declare",
                        case_id, json_text(alts->item[a], "alternative_id"), id);
            } else if (group_of(r->c, id, &alt) != group || alt != alts->item[a]) {
                finding(r->file, ids->item[i]->line,
                        "%s: %s is in two alternatives; one of them runs and the others cannot",
                        case_id, id);
            }
        }
    }
}

/**
 * A group decided by features has one alternative for present and one for
 * absent.
 */
static void rule_feature_group(const struct rule *r, const struct json *group)
{
    const struct json *names = json_member(json_member(group, "decided_by"), "features");
    const struct json *features = json_member(json_member(r->c, "requirements"), "features");
    const struct json *alts = json_member(group, "alternatives");
    const char *case_id = json_text(r->c, "case_id");
    const char *group_id = json_text(group, "group_id");
    size_t i;

    for (i = 0; names != NULL && i < names->count; i++) {
        if (json_member(features, names->item[i]->string) == NULL) {
            finding(r->file, names->item[i]->line,
                    "%s: the group %s is decided by %s, which requirements.features does not "
                    "declare",
                    case_id, group_id, names->item[i]->string);
        }
    }
    if (alts->count != 2 || !has_member(alts, "when", "present") ||
        !has_member(alts, "when", "absent")) {
        finding(r->file, group->line,
                "%s: the group %s is decided by features, so it has two alternatives, one when "
                "present and one when absent",
                case_id, group_id);
    }
}

/**
 * A group's alternatives name declared assertions, decided as the group says.
 */
static void rule_branches(const struct rule *r)
{
    const struct json *groups = json_member(r->c, "branches");
    size_t g, a;

    unique(r, groups, "group_id", "the branch group");
    for (g = 0; groups != NULL && g < groups->count; g++) {
        const struct json *group = groups->item[g];
        const struct json *alts = json_member(group, "alternatives");

        rule_alternatives(r, group);
        if (json_member(group, "decided_by")->type == JSON_OBJECT) {
            rule_feature_group(r, group);
            continue;
        }
        for (a = 0; a < alts->count; a++) {
            if (json_member(alts->item[a], "when")->type != JSON_NULL) {
                finding(r->file, alts->item[a]->line,
                        "%s: the group %s is decided by the report, so its alternatives have no "
                        "when",
                        json_text(r->c, "case_id"), json_text(group, "group_id"));
            }
        }
    }
}

/** The role, the expected end and the native plan must agree. */
static void rule_role(const struct rule *r)
{
    const struct json *term = json_member(r->c, "termination");
    const char *end = json_text(term, "end");
    const char *native_class = json_text(json_member(r->c, "native"), "class");
    const char *case_id = json_text(r->c, "case_id");
    int init_end = same(end, "init_exited") || same(end, "init_killed");
    int machine_end =
        same(end, "panic") || same(end, "reset") || same(end, "shutdown") || same(end, "halted");

    if (init_end != same(r->role, "init")) {
        finding(r->file, term->line,
                "%s: the end %s needs the role init, and the role init one of its ends", case_id,
                init_end ? end : "init_exited or init_killed");
    }
    if (machine_end && !same(r->role, "boot") && !same(r->role, "init")) {
        finding(r->file, term->line,
                "%s: the end %s ends a machine, which only a guest case of the role boot or init "
                "may expect",
                case_id, end);
    }
    if ((same(r->role, "init") || same(r->role, "boot")) && !same(r->plan, "none")) {
        finding(r->file, line_at(r->c, "native"),
                "%s: a case of the role %s runs under QEMU alone, so its native plan is none",
                case_id, r->role);
    }
    if (same(native_class, "init") != same(r->role, "init")) {
        finding(r->file, line_at(r->c, "native"),
                "%s: the native class init is for a case of the role init, and that role's "
                "class",
                case_id);
    }
}

/** Reports `name` unless preparation makes an artifact of that name. */
static void prepared(const struct rule *r, const struct json *name, const char *what)
{
    if (name != NULL && name->type == JSON_STRING &&
        !has_member(json_member(r->c, "prepare"), "name", name->string)) {
        finding(r->file, name->line, "%s: %s %s is no artifact that prepare lists",
                json_text(r->c, "case_id"), what, name->string);
    }
}

/**
 * Returns the name of `v`, a disk of a profile's `disks`: the string itself,
 * or the object's `name`.
 */
static const struct json *disk_name(const struct json *v)
{
    return v->type == JSON_OBJECT ? json_member(v, "name") : v;
}

/**
 * Requires the artifacts of a QEMU profile; an empty disk is none, and an
 * overlay's is its base.
 */
static void prepared_qemu(const struct rule *r, const struct json *qemu)
{
    const struct json *disks = json_member(qemu, "disks");
    size_t i;

    if (qemu == NULL) {
        return;
    }
    prepared(r, json_member(qemu, "kernel"), "the kernel");
    prepared(r, json_member(qemu, "initrd"), "the initial RAM image");
    prepared(r, json_member(qemu, "acpi_table"), "the ACPI table");
    prepared(r, json_member(qemu, "audio_input"), "the audio input");
    prepared(r, json_member(qemu, "emulator"), "the emulator");
    for (i = 0; disks != NULL && i < disks->count; i++) {
        if (json_member(disks->item[i], "base") != NULL) {
            prepared(r, json_member(disks->item[i], "base"), "the overlay's base");
        } else if (json_member(disks->item[i], "size_bytes") == NULL) {
            prepared(r, disk_name(disks->item[i]), "the disk");
        }
    }
}

/** A machine creates empty disks and overlays in its private workspace. */
static int generated_disk(const struct rule *r, const char *name)
{
    const struct json *machines = json_member(r->c, "machines");
    size_t i, j;

    if (!same(r->domain, "machine"))
        return 0;
    for (i = 0; machines != NULL && i < machines->count; i++) {
        const struct json *disks = json_member(json_member(machines->item[i], "qemu"), "disks");

        for (j = 0; disks != NULL && j < disks->count; j++) {
            const struct json *disk = disks->item[j];

            if (same(json_text(disk, "name"), name) &&
                (json_member(disk, "size_bytes") != NULL || json_member(disk, "base") != NULL))
                return 1;
        }
    }
    return 0;
}

/** Requires prepared files, except disks that the machine creates itself. */
static void prepared_fixtures(const struct rule *r, const struct json *list)
{
    size_t i, j;

    for (i = 0; list != NULL && i < list->count; i++) {
        const struct json *names = json_member(list->item[i], "names");

        for (j = 0; names != NULL && j < names->count; j++) {
            if (same(json_text(list->item[i], "fixture"), "disk") &&
                generated_disk(r, names->item[j]->string))
                continue;
            prepared(r, names->item[j], "the fixture's file");
        }
    }
}

/**
 * Reports a machine whose profile names another emulator than the first
 * machine's, or none where it names one: the scenario's site has one program
 * for each architecture, so a case's machines run on the same one.
 */
static void one_emulator(const struct rule *r, const struct json *machines)
{
    const struct json *first, *e;
    const char *want, *got;
    size_t i;

    if (machines == NULL || machines->count == 0)
        return;
    first = json_member(json_member(machines->item[0], "qemu"), "emulator");
    want = first != NULL && first->type == JSON_STRING ? first->string : NULL;
    for (i = 1; i < machines->count; i++) {
        e = json_member(json_member(machines->item[i], "qemu"), "emulator");
        got = e != NULL && e->type == JSON_STRING ? e->string : NULL;
        if (want == NULL ? got != NULL : got == NULL || !same(got, want))
            finding(r->file, line_at(machines->item[i], "qemu"),
                    "%s: a machine names the emulator %s and another %s; the case's machines "
                    "run on one",
                    json_text(r->c, "case_id"), want != NULL ? want : "none",
                    got != NULL ? got : "none");
    }
}

/** Every artifact the case uses is one that prepare lists, once. */
static void rule_artifacts(const struct rule *r)
{
    const struct json *exec = json_member(r->c, "execution");
    const struct json *machines = json_member(r->c, "machines");
    size_t i;

    unique(r, json_member(r->c, "prepare"), "name", "the artifact");
    prepared(r, json_member(exec, "program"), "the program");
    prepared(r, json_member(exec, "stdin"), "the standard input");
    prepared_qemu(r, json_member(json_member(r->c, "requirements"), "qemu"));
    for (i = 0; machines != NULL && i < machines->count; i++) {
        prepared_qemu(r, json_member(machines->item[i], "qemu"));
    }
    one_emulator(r, machines);
    prepared_fixtures(r, json_member(exec, "isolation"));
    prepared_fixtures(r, native_fixtures(r->c));
}

/**
 * Fills `d` with the model and the network options of device `v` of a
 * profile, a name or a card object: its addresses, its hub, its restriction,
 * and how many forwards and guest forwards it has, whose list is not copied.
 */
static void card_of(const struct json *v, struct etude_qemu_device *d)
{
    const struct json *hub = json_member(v, "hub"), *restricted = json_member(v, "restrict");
    const struct json *forwards = json_member(v, "forwards");
    const struct json *guest_forwards = json_member(v, "guest_forwards");

    memset(d, 0, sizeof(*d));
    d->model = v->type == JSON_STRING ? v->string : json_text(v, "model");
    d->net = json_text(v, "net");
    d->host = json_text(v, "host");
    d->dhcpstart = json_text(v, "dhcpstart");
    d->dns = json_text(v, "dns");
    d->mac = json_text(v, "mac");
    d->hub = hub != NULL && hub->integer != 0;
    d->restricted = restricted != NULL && restricted->integer != 0;
    d->forward_count = forwards != NULL ? forwards->count : 0;
    d->guest_forward_count = guest_forwards != NULL ? guest_forwards->count : 0;
}

/** Check the network options of one QEMU profile with the launcher's rules. */
static void network_profile(const struct rule *r, const struct json *qemu)
{
    const struct json *list = json_member(qemu, "devices");
    struct etude_qemu_device *devices;
    size_t i;

    if (list == NULL)
        return;
    devices = json_alloc((list->count + 1) * sizeof(*devices));
    memset(devices, 0, (list->count + 1) * sizeof(*devices));
    for (i = 0; i < list->count; i++)
        card_of(list->item[i], &devices[i]);
    if (etude_qemu_devices_check(devices) != 0)
        finding(r->file, list->line,
                "%s: invalid QEMU network options or duplicate station address",
                json_text(r->c, "case_id"));
    free(devices);
}

/** Check user networks in the guest profile and every scenario machine. */
static void rule_network(const struct rule *r)
{
    const struct json *machines = json_member(r->c, "machines");
    size_t i;

    network_profile(r, json_member(json_member(r->c, "requirements"), "qemu"));
    for (i = 0; machines != NULL && i < machines->count; i++)
        network_profile(r, json_member(machines->item[i], "qemu"));
}

/**
 * Reports a device of the QEMU profile `qemu` whose bus no device before it
 * provides, or whose drive is no disk of the profile or is another device's,
 * a disk on a device that no device attaches, and an identity given twice.
 */
static void attached(const struct rule *r, const struct json *qemu)
{
    const struct json *disks = json_member(qemu, "disks");
    const struct json *devices = json_member(qemu, "devices");
    const char *case_id = json_text(r->c, "case_id");
    size_t i, j, len;

    if (disks == NULL || devices == NULL) {
        return;
    }
    unique(r, disks, "id", "the drive");
    unique(r, devices, "id", "the device");
    for (i = 0; i < disks->count; i++) {
        if (json_text(disks->item[i], "id") != NULL &&
            !has_member(devices, "drive", json_text(disks->item[i], "id"))) {
            finding(r->file, disks->item[i]->line, "%s: no device attaches the drive %s", case_id,
                    json_text(disks->item[i], "id"));
        }
    }
    for (i = 0; i < devices->count; i++) {
        const struct json *d = devices->item[i];
        const char *bus = json_text(d, "bus"), *drive = json_text(d, "drive");
        const char *model = d->type == JSON_OBJECT ? json_text(d, "model") : d->string;

        if (bus != NULL) {
            len = (size_t)(strrchr(bus, '.') != NULL ? strrchr(bus, '.') - bus : 0);
            for (j = 0; j < i; j++) {
                const char *id = json_text(devices->item[j], "id");

                if (same(json_text(devices->item[j], "model"), "pcie-root-port") && same(id, bus)) {
                    break;
                }
                if (id != NULL && strlen(id) == len && strncmp(id, bus, len) == 0) {
                    break;
                }
            }
            if (j == i) {
                finding(r->file, d->line,
                        "%s: the device %s is on the bus %s, which no device before it provides",
                        case_id, model, bus);
            }
        }
        if (drive != NULL && !has_member(disks, "id", drive)) {
            finding(r->file, d->line,
                    "%s: the device %s attaches the drive %s, which is no disk of its profile",
                    case_id, model, drive);
        }
        for (j = 0; drive != NULL && j < i; j++) {
            if (same(json_text(devices->item[j], "drive"), drive)) {
                finding(r->file, d->line, "%s: the drive %s is attached twice", case_id, drive);
                break;
            }
        }
        if (same(model, "vga") && d->type == JSON_OBJECT && d->count > 1) {
            finding(r->file, d->line, "%s: the device vga has no member but its model", case_id);
        }
    }
}

/** Every QEMU profile attaches its disks and devices as Readme.md says. */
static void rule_attached(const struct rule *r)
{
    const struct json *machines = json_member(r->c, "machines");
    size_t i;

    attached(r, json_member(json_member(r->c, "requirements"), "qemu"));
    for (i = 0; machines != NULL && i < machines->count; i++) {
        attached(r, json_member(machines->item[i], "qemu"));
    }
}

/**
 * Returns QEMU profile `i` of the case, counted from 0: the requirements'
 * first, then each machine's; NULL for one the case does not give.
 */
static const struct json *profile_at(const struct rule *r, size_t i)
{
    const struct json *machines = json_member(r->c, "machines");

    if (i == 0) {
        return json_member(json_member(r->c, "requirements"), "qemu");
    }
    return machines != NULL && i <= machines->count ? json_member(machines->item[i - 1], "qemu")
                                                    : NULL;
}

/** Returns the bus of `v`, a disk of a profile's `disks`; IDE by default. */
static enum etude_disk_bus disk_bus(const struct json *v)
{
    const char *bus = json_text(v, "bus");

    return same(bus, "nvme")     ? ETUDE_BUS_NVME
           : same(bus, "ahci")   ? ETUDE_BUS_AHCI
           : same(bus, "device") ? ETUDE_BUS_DEVICE
           : same(bus, "virtio") ? ETUDE_BUS_VIRTIO
                                 : ETUDE_BUS_IDE;
}

/**
 * Reports a disk that profile `qemu` gives twice, disks that the launcher
 * does not attach together, and a disk that boots first beside the direct
 * firmware's kernel, which QEMU boots before any disk. The identities of
 * disks that devices attach are the schema's and rule_attached's to check,
 * and each such disk is counted here under an identity of its place.
 */
static void profile_disks(const struct rule *r, const struct json *qemu)
{
    static const char *const places[ETUDE_QEMU_DISKS + 1] = {"d0", "d1", "d2", "d3", "d4",
                                                             "d5", "d6", "d7", "d8"};
    const struct json *disks = json_member(qemu, "disks");
    struct etude_qemu_disk d[ETUDE_QEMU_DISKS + 2];
    size_t i, j, n = disks != NULL ? disks->count : 0;

    memset(d, 0, sizeof d);
    for (i = 0; i < n; i++) {
        const struct json *v = disks->item[i], *boot = json_member(v, "boot");

        for (j = 0; j < i; j++) {
            if (same(disk_name(v)->string, disk_name(disks->item[j])->string)) {
                finding(r->file, v->line, "%s: the disk %s is given twice in a profile",
                        json_text(r->c, "case_id"), disk_name(v)->string);
                break;
            }
        }
        if (i <= ETUDE_QEMU_DISKS) {
            d[i].path = disk_name(v)->string;
            d[i].bus = disk_bus(v);
            d[i].cdrom = same(json_text(v, "media"), "cdrom");
            d[i].serial = json_text(v, "serial");
            d[i].id = d[i].bus == ETUDE_BUS_DEVICE ? places[i] : NULL;
            d[i].boot = boot != NULL && boot->integer != 0;
            d[i].qcow2 = json_member(v, "base") != NULL;
        }
        if (boot != NULL && boot->integer != 0 && same(json_text(qemu, "firmware"), "direct")) {
            finding(r->file, v->line, "%s: the disk %s boots first beside the direct firmware",
                    json_text(r->c, "case_id"), disk_name(v)->string);
        }
    }
    if (etude_qemu_disks_check(d) != 0) {
        finding(r->file, line_at(qemu, "disks"),
                "%s: the launcher attaches at most %d disks of a profile, four of them on IDE "
                "and six on AHCI, and one at most that boots first",
                json_text(r->c, "case_id"), ETUDE_QEMU_DISKS);
    }
}

/**
 * Reports disk `v`, the `j`th of profile `i`, when it is an empty disk that
 * prepare lists as an artifact, or that a disk before it gives another size.
 */
static void empty_disk(const struct rule *r, const struct json *v, size_t i, size_t j)
{
    const struct json *size = json_member(v, "size_bytes"), *w;
    const char *name = disk_name(v)->string;
    size_t k, l;

    if (size == NULL) {
        return;
    }
    if (has_member(json_member(r->c, "prepare"), "name", name)) {
        finding(r->file, v->line, "%s: the empty disk %s is an artifact that prepare lists",
                json_text(r->c, "case_id"), name);
    }
    for (k = 0; k <= i; k++) {
        const struct json *disks = json_member(profile_at(r, k), "disks");

        for (l = 0; disks != NULL && l < disks->count && (k < i || l < j); l++) {
            w = json_member(disks->item[l], "size_bytes");
            if (w != NULL && same(disk_name(disks->item[l])->string, name) &&
                w->integer != size->integer) {
                finding(r->file, v->line, "%s: the empty disk %s is given two sizes",
                        json_text(r->c, "case_id"), name);
                return;
            }
        }
    }
}

/**
 * Returns 1 when disks `a` and `b` are overlays of one base and cluster size.
 */
static int same_overlay(const struct json *a, const struct json *b)
{
    const struct json *ca = json_member(a, "cluster_bytes"), *cb = json_member(b, "cluster_bytes");

    return json_member(a, "base") != NULL && same(json_text(a, "base"), json_text(b, "base")) &&
           ca != NULL && cb != NULL && ca->integer == cb->integer;
}

/**
 * Reports disk `v`, of profile `i`, when it is an overlay that the
 * requirements' profile gives, since libetude-case makes it for a machine;
 * whose name is an artifact that prepare lists; or whose name another disk of
 * a profile gives as anything but the same overlay.
 */
static void overlay_disk(const struct rule *r, const struct json *v, size_t i)
{
    const char *name = disk_name(v)->string, *case_id = json_text(r->c, "case_id");
    const struct json *machines = json_member(r->c, "machines");
    size_t profiles = 1 + (machines != NULL ? machines->count : 0), k, l;

    if (json_member(v, "base") == NULL) {
        return;
    }
    if (i == 0) {
        finding(r->file, v->line, "%s: the overlay %s is not of a machine's profile", case_id,
                name);
    }
    if (has_member(json_member(r->c, "prepare"), "name", name)) {
        finding(r->file, v->line, "%s: the overlay %s has the name of an artifact", case_id, name);
    }
    for (k = 0; k < profiles; k++) {
        const struct json *disks = json_member(profile_at(r, k), "disks");

        for (l = 0; disks != NULL && l < disks->count; l++) {
            const struct json *w = disks->item[l];

            if (w != v && same(disk_name(w)->string, name) && !same_overlay(v, w)) {
                finding(r->file, v->line,
                        "%s: the overlay %s is given another base, cluster size or kind", case_id,
                        name);
                return;
            }
        }
    }
}

/**
 * The disks of each QEMU profile are ones the launcher attaches together,
 * each given once. An empty disk, one of `size_bytes`, is no artifact of
 * prepare, and every profile that gives it gives it one size. An overlay,
 * one of `base`, is a machine's, has a name no artifact of prepare has, and
 * is the same overlay wherever a profile gives its name.
 */
static void rule_disks(const struct rule *r)
{
    const struct json *machines = json_member(r->c, "machines");
    size_t profiles = 1 + (machines != NULL ? machines->count : 0), i, j;

    for (i = 0; i < profiles; i++) {
        const struct json *disks = json_member(profile_at(r, i), "disks");

        profile_disks(r, profile_at(r, i));
        for (j = 0; disks != NULL && j < disks->count; j++) {
            empty_disk(r, disks->item[j], i, j);
            overlay_disk(r, disks->item[j], i);
        }
    }
}

/** Reports the QEMU profile `qemu` unless it names a processor profile. */
static void known_processor(const struct rule *r, const struct json *qemu)
{
    const struct json *cpu = json_member(qemu, "cpu");

    if (cpu != NULL && cpu->type == JSON_STRING && etude_processor_find(cpu->string) == NULL) {
        finding(r->file, cpu->line, "%s: %s is no processor profile", json_text(r->c, "case_id"),
                cpu->string);
    }
}

/**
 * Every QEMU profile names a processor profile, and every feature the case
 * requires is one a profile can declare. The profile of a guest case's boot
 * declares each such feature as the case requires it, and either way for
 * `either`, so that a group decided by the feature is decided under QEMU.
 */
static void rule_processor(const struct rule *r)
{
    static const char *const states[] = {"", "present", "absent"};
    const struct json *req = json_member(r->c, "requirements");
    const struct json *qemu = json_member(req, "qemu");
    const struct json *features = json_member(req, "features");
    const struct json *machines = json_member(r->c, "machines");
    const struct etude_processor *p;
    size_t i;

    known_processor(r, qemu);
    for (i = 0; machines != NULL && i < machines->count; i++) {
        known_processor(r, json_member(machines->item[i], "qemu"));
    }
    p = etude_processor_find(json_text(qemu, "cpu"));
    for (i = 0; features != NULL && i < features->count; i++) {
        const struct json *f = features->item[i];
        enum etude_feature_state has;

        if (!etude_feature_known(f->name)) {
            finding(r->file, f->line, "%s: %s is no processor feature a profile can declare",
                    json_text(r->c, "case_id"), f->name);
            continue;
        }
        if (p == NULL || f->type != JSON_STRING) {
            continue;
        }
        has = etude_processor_feature(p, f->name);
        if (has == ETUDE_FEATURE_UNDECLARED) {
            finding(r->file, f->line,
                    "%s: the case requires %s %s, which its processor profile %s does not "
                    "declare",
                    json_text(r->c, "case_id"), f->name, f->string, p->name);
        } else if (!same(f->string, "either") && !same(f->string, states[has])) {
            finding(r->file, f->line,
                    "%s: the case requires %s %s, and its processor profile %s declares it %s",
                    json_text(r->c, "case_id"), f->name, f->string, p->name, states[has]);
        }
    }
}

/** Reports each capture of the QEMU profile `qemu` that names no card of its
 * devices. */
static void known_cards(const struct rule *r, const struct json *qemu)
{
    const struct json *devices = json_member(qemu, "devices");
    const struct json *captures = json_member(qemu, "captures");
    const struct json *d;
    const char *model;
    size_t i, cards = 0;

    for (i = 0; devices != NULL && i < devices->count; i++) {
        d = devices->item[i];
        model = d->type == JSON_STRING ? d->string : json_text(d, "model");
        if (model != NULL && etude_qemu_nic(model)) {
            cards++;
        }
    }
    for (i = 0; captures != NULL && i < captures->count; i++) {
        const struct json *card = captures->item[i];

        if (card->type == JSON_STRING && strncmp(card->string, "net", 3) == 0 &&
            strtoul(card->string + 3, NULL, 10) >= cards) {
            finding(r->file, card->line, "%s: the capture %s names no network card of %lu",
                    json_text(r->c, "case_id"), card->string, (unsigned long)cards);
        }
    }
}

/**
 * Each capture of a QEMU profile names one of its network cards, `net0` the
 * first of its `devices` that is a card, as the launcher numbers them.
 */
static void rule_captures(const struct rule *r)
{
    const struct json *machines = json_member(r->c, "machines");
    size_t i;

    known_cards(r, json_member(json_member(r->c, "requirements"), "qemu"));
    for (i = 0; machines != NULL && i < machines->count; i++) {
        known_cards(r, json_member(machines->item[i], "qemu"));
    }
}

/**
 * Reports audio of the QEMU profile `qemu` that the launcher refuses: a
 * backend or an input without a card, an input without the `sdl` backend,
 * and `sdl` without an input.
 */
static void known_audio(const struct rule *r, const struct json *qemu)
{
    const char *card = json_text(qemu, "audio");
    const char *backend = json_text(qemu, "audio_backend");
    const struct json *input = json_member(qemu, "audio_input");
    int sdl = same(backend, "sdl");

    if (qemu == NULL) {
        return;
    }
    if (card == NULL && (backend != NULL || input != NULL)) {
        finding(r->file, qemu->line, "%s: an audio backend or input without an audio card",
                json_text(r->c, "case_id"));
    } else if (sdl != (input != NULL)) {
        finding(r->file, qemu->line, "%s: an audio input needs the sdl backend, and sdl an input",
                json_text(r->c, "case_id"));
    }
}

/**
 * The audio of each QEMU profile is a card on a backend, and the `sdl`
 * backend alone has, and needs, an input file.
 */
static void rule_audio(const struct rule *r)
{
    const struct json *machines = json_member(r->c, "machines");
    size_t i;

    known_audio(r, json_member(json_member(r->c, "requirements"), "qemu"));
    for (i = 0; machines != NULL && i < machines->count; i++) {
        known_audio(r, json_member(machines->item[i], "qemu"));
    }
}

/**
 * Reports a display of the QEMU profile `qemu` that the launcher refuses:
 * one beside another display device or a USB tablet, or on a machine
 * without the 8042 that its PS/2 mouse is on.
 */
static void known_display(const struct rule *r, const struct json *qemu)
{
    const struct json *devices = json_member(qemu, "devices");
    const struct json *i8042 = json_member(qemu, "i8042");
    const char *model;
    size_t i;

    if (qemu == NULL || json_text(qemu, "display") == NULL)
        return;
    if (i8042 != NULL && !i8042->integer)
        finding(r->file, qemu->line,
                "%s: a display needs the 8042, whose PS/2 mouse is its pointer",
                json_text(r->c, "case_id"));
    for (i = 0; devices != NULL && i < devices->count; i++) {
        model = devices->item[i]->type == JSON_STRING ? devices->item[i]->string
                                                      : json_text(devices->item[i], "model");
        if (etude_qemu_display_device(model))
            finding(r->file, devices->item[i]->line,
                    "%s: the device %s beside the profile's display", json_text(r->c, "case_id"),
                    model);
    }
}

/**
 * A QEMU profile's display is its only display device, with the PS/2 mouse
 * and no USB tablet.
 */
static void rule_display(const struct rule *r)
{
    const struct json *machines = json_member(r->c, "machines");
    size_t i;

    known_display(r, json_member(json_member(r->c, "requirements"), "qemu"));
    for (i = 0; machines != NULL && i < machines->count; i++) {
        known_display(r, json_member(machines->item[i], "qemu"));
    }
}

/** Returns 1 when `list`, an array, has the string `value`. */
static int listed(const struct json *list, const char *value)
{
    size_t i;

    for (i = 0; list != NULL && i < list->count; i++) {
        if (list->item[i]->type == JSON_STRING && same(list->item[i]->string, value)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Says whether the kernel of machine `machine` is an artifact that prepare
 * makes once for both architectures, the kernel of another system.
 */
static int fixed_kernel(const struct rule *r, const struct json *machine)
{
    const struct json *prepare = json_member(r->c, "prepare");
    const char *kernel = json_text(json_member(machine, "qemu"), "kernel");
    const struct json *per;
    size_t i;

    for (i = 0; kernel != NULL && prepare != NULL && i < prepare->count; i++) {
        if (same(json_text(prepare->item[i], "name"), kernel)) {
            per = json_member(prepare->item[i], "per_architecture");
            return per != NULL && !per->integer;
        }
    }
    return 0;
}

/**
 * An architecture override belongs only to a declared helper machine, or to
 * a machine of another system, whose kernel prepare makes once for both
 * architectures, such as the Wi-Fi suite's Linux access point.
 */
static void rule_helper_architecture(const struct rule *r)
{
    const struct json *machines = json_member(r->c, "machines");
    const struct json *qemu = json_member(json_member(r->c, "requirements"), "qemu");
    const struct json *arch = json_member(qemu, "architecture");
    size_t i;

    if (arch != NULL)
        finding(r->file, arch->line, "%s: only a helper machine may override its architecture",
                json_text(r->c, "case_id"));
    for (i = 0; machines != NULL && i < machines->count; i++) {
        const struct json *machine = machines->item[i];
        const struct json *helper = json_member(machine, "helper");
        arch = json_member(json_member(machine, "qemu"), "architecture");
        if (arch != NULL && (helper == NULL || !helper->integer) && !fixed_kernel(r, machine))
            finding(r->file, arch->line,
                    "%s: only a helper machine, or one whose kernel is the same on both "
                    "architectures, may override its architecture",
                    json_text(r->c, "case_id"));
    }
}

/**
 * A machine case's steps run on its machines, under identities of its own,
 * and each of its services runs a tool of its requirements or an artifact
 * that prepare lists.
 */
static void rule_steps(const struct rule *r)
{
    const struct json *steps = json_member(r->c, "steps");
    const struct json *machines = json_member(r->c, "machines");
    const struct json *services = json_member(r->c, "services");
    const struct json *tools = json_member(json_member(r->c, "requirements"), "tools");
    const char *case_id = json_text(r->c, "case_id");
    size_t i;

    unique(r, steps, "step_id", "the step");
    unique(r, machines, "machine_id", "the machine");
    unique(r, services, "service_id", "the service");
    for (i = 0; services != NULL && i < services->count; i++) {
        const char *program = json_text(services->item[i], "program");

        if (program != NULL && !listed(tools, program) &&
            !has_member(json_member(r->c, "prepare"), "name", program)) {
            finding(r->file, services->item[i]->line,
                    "%s: the service %s runs %s, which is neither a tool of its requirements nor "
                    "an artifact that prepare lists",
                    case_id, json_text(services->item[i], "service_id"), program);
        }
    }
    for (i = 0; steps != NULL && i < steps->count; i++) {
        const char *id = json_text(steps->item[i], "step_id");
        const char *machine = json_text(steps->item[i], "machine");

        if (id != NULL && strncmp(id, "etude.", 6) == 0) {
            finding(r->file, steps->item[i]->line,
                    "%s: the step %s has an identity of the executor's", case_id, id);
        }
        if (machine != NULL && !has_member(machines, "machine_id", machine)) {
            finding(r->file, steps->item[i]->line,
                    "%s: the step %s runs on %s, which the case does not declare", case_id, id,
                    machine);
        }
    }
}

/** Returns the witness `id` of the case's `progress`, or NULL. */
static const struct json *witness(const struct rule *r, const char *id)
{
    const struct json *list = json_member(r->c, "progress");
    size_t i;

    for (i = 0; list != NULL && i < list->count; i++) {
        if (same(json_text(list->item[i], "witness_id"), id)) {
            return list->item[i];
        }
    }
    return NULL;
}

/**
 * Each suspension of a workload stops a machine of the case, under an
 * identity of its own, and counts its stops on a counter witness of its own
 * whose limit is at least its cycles.
 */
static void rule_suspensions(const struct rule *r)
{
    const struct json *list = json_member(json_member(r->c, "workload"), "suspensions");
    const struct json *machines = json_member(r->c, "machines");
    const char *case_id = json_text(r->c, "case_id");
    size_t i, j;

    unique(r, list, "suspension_id", "the suspension");
    for (i = 0; list != NULL && i < list->count; i++) {
        const struct json *s = list->item[i];
        const char *id = json_text(s, "suspension_id"), *machine = json_text(s, "machine");
        const char *counter = json_text(s, "witness");
        const struct json *w = witness(r, counter);
        const struct json *limit = json_member(w, "limit"), *cycles = json_member(s, "cycles");

        if (machine != NULL && !has_member(machines, "machine_id", machine)) {
            finding(r->file, line_at(s, "machine"),
                    "%s: the suspension %s stops %s, which the case does not declare", case_id, id,
                    machine);
        }
        if (counter != NULL && !same(json_text(w, "kind"), "counter")) {
            finding(r->file, line_at(s, "witness"),
                    "%s: the suspension %s counts its stops on %s, which is no counter witness of "
                    "the case",
                    case_id, id, counter);
        } else if (limit != NULL && cycles != NULL && limit->type == JSON_INTEGER &&
                   cycles->type == JSON_INTEGER && limit->integer < cycles->integer) {
            finding(r->file, line_at(s, "cycles"),
                    "%s: the suspension %s has %lld cycles, past the limit %lld of its witness %s",
                    case_id, id, cycles->integer, limit->integer, counter);
        }
        for (j = 0; j < i; j++) {
            if (counter != NULL && same(counter, json_text(list->item[j], "witness"))) {
                finding(r->file, line_at(s, "witness"),
                        "%s: the witness %s counts the stops of two suspensions", case_id, counter);
                break;
            }
        }
    }
}

/**
 * Returns 1 when a forward of the devices `devs` that comes before forward
 * `j` of device `i` forwards the local port of `f` by its protocol.
 */
static int forwarded_before(const struct json *devs, size_t i, size_t j, const struct json *f)
{
    const struct json *list, *g;
    size_t k, n;

    for (k = 0; k <= i; k++) {
        list = json_member(devs->item[k], "forwards");
        for (n = 0; list != NULL && n < (k < i ? list->count : j); n++) {
            g = list->item[n];
            if (same(json_text(g, "protocol"), json_text(f, "protocol")) &&
                json_member(g, "port")->integer == json_member(f, "port")->integer) {
                return 1;
            }
        }
    }
    return 0;
}

/**
 * Returns 1 when a TCP forward of the devices `devs` forwards the local port
 * `port` to the guest, so that QEMU listens there.
 */
static int forwarded_tcp(const struct json *devs, long long port)
{
    const struct json *list;
    size_t i, j;

    for (i = 0; i < devs->count; i++) {
        list = json_member(devs->item[i], "forwards");
        for (j = 0; list != NULL && j < list->count; j++) {
            if (same(json_text(list->item[j], "protocol"), "tcp") &&
                json_member(list->item[j], "port")->integer == port) {
                return 1;
            }
        }
    }
    return 0;
}

/**
 * Reports what the launcher would refuse of the guest forwards of card `dev`
 * of machine `id`, one of the devices `devs`: an address of the card's
 * network that QEMU cannot forward, a local port past those the case
 * reserves or one that a TCP forward makes QEMU listen on, an address and a
 * port forwarded twice, and a relay that is neither a tool of the case's
 * requirements nor an artifact that prepare lists.
 */
static void guest_forwards(const struct rule *r, const char *id, const struct json *devs,
                           const struct json *dev, long long ports)
{
    const struct json *list = json_member(dev, "guest_forwards");
    const struct json *tools = json_member(json_member(r->c, "requirements"), "tools");
    const char *case_id = json_text(r->c, "case_id");
    struct etude_qemu_device card;
    size_t i, k;

    card_of(dev, &card);
    for (i = 0; list != NULL && i < list->count; i++) {
        const struct json *g = list->item[i], *relay = json_member(g, "relay");
        const char *address = json_text(g, "address");
        const char *command = relay != NULL && relay->count > 0 ? relay->item[0]->string : NULL;
        long long local = json_member(g, "port")->integer;
        long long guest = json_member(g, "guest_port")->integer;

        if (!etude_qemu_guest_address(&card, address)) {
            finding(r->file, g->line,
                    "%s: the machine %s forwards connections to %s, which QEMU cannot forward",
                    case_id, id, address);
        }
        if (local >= ports) {
            finding(r->file, g->line,
                    "%s: the machine %s forwards port %lld, but the case reserves %lld", case_id,
                    id, local, ports);
        }
        if (forwarded_tcp(devs, local)) {
            finding(r->file, g->line,
                    "%s: the machine %s forwards port %lld both to the guest and from it", case_id,
                    id, local);
        }
        for (k = 0; k < i; k++) {
            if (same(json_text(list->item[k], "address"), address) &&
                json_member(list->item[k], "guest_port")->integer == guest) {
                finding(r->file, g->line,
                        "%s: the machine %s forwards connections to %s:%lld twice", case_id, id,
                        address, guest);
                break;
            }
        }
        if (command != NULL && !listed(tools, command) &&
            !has_member(json_member(r->c, "prepare"), "name", command)) {
            finding(r->file, g->line,
                    "%s: the machine %s relays through %s, which is neither a tool of its "
                    "requirements nor an artifact that prepare lists",
                    case_id, id, command);
        }
    }
}

/**
 * Reports what the launcher would refuse of the forwards of the cards of
 * machine `m`: a forward of a local port past those the case reserves, a
 * local port forwarded twice by one protocol, and what guest_forwards
 * reports.
 */
static void machine_networks(const struct rule *r, const struct json *m)
{
    const struct json *devs = json_member(json_member(m, "qemu"), "devices");
    long long ports = json_member(json_member(r->c, "resources"), "ports")->integer;
    const char *case_id = json_text(r->c, "case_id"), *id = json_text(m, "machine_id");
    size_t i, j;

    for (i = 0; devs != NULL && i < devs->count; i++) {
        const struct json *list = json_member(devs->item[i], "forwards");

        for (j = 0; list != NULL && j < list->count; j++) {
            const struct json *f = list->item[j];
            long long port = json_member(f, "port")->integer;

            if (port >= ports) {
                finding(r->file, f->line,
                        "%s: the machine %s forwards port %lld, but the case reserves %lld",
                        case_id, id, port, ports);
            }
            if (forwarded_before(devs, i, j, f)) {
                finding(r->file, f->line, "%s: the machine %s forwards its %s port %lld twice",
                        case_id, id, json_text(f, "protocol"), port);
            }
        }
        guest_forwards(r, id, devs, devs->item[i], ports);
    }
}

/**
 * A machine's forwards are those the launcher takes, and the cards of a guest
 * case's boot have no forwards, no guest forwards and no restriction, which
 * only a scenario's machines are given.
 */
static void rule_networks(const struct rule *r)
{
    const struct json *qemu = json_member(json_member(r->c, "requirements"), "qemu");
    const struct json *devs = json_member(qemu, "devices");
    const struct json *machines = json_member(r->c, "machines");
    size_t i;

    for (i = 0; devs != NULL && i < devs->count; i++) {
        const struct json *dev = devs->item[i], *restricted = json_member(dev, "restrict");

        if (json_member(dev, "forwards") != NULL || json_member(dev, "guest_forwards") != NULL ||
            (restricted != NULL && restricted->integer != 0)) {
            finding(r->file, dev->line,
                    "%s: a guest case's boot has no forwards, no guest forwards and no "
                    "restricted network, which only a scenario's machines are given",
                    json_text(r->c, "case_id"));
        }
    }
    for (i = 0; machines != NULL && i < machines->count; i++) {
        machine_networks(r, machines->item[i]);
    }
}

/** Reports a bound whose floor, initial value and ceiling are out of order. */
static void ordered(const struct rule *r, const struct json *bound, const char *what)
{
    if (bound != NULL &&
        (json_member(bound, "floor_us")->integer > json_member(bound, "initial_us")->integer ||
         json_member(bound, "initial_us")->integer > json_member(bound, "ceiling_us")->integer)) {
        finding(r->file, bound->line,
                "%s: the %s bound has its floor, initial value and ceiling out of order",
                json_text(r->c, "case_id"), what);
    }
}

/**
 * Reports the bound `b` when it is in order but reaches past the whole case's.
 */
static void inside(const struct rule *r, const struct json *b, const struct json *whole,
                   const char *what)
{
    ordered(r, b, what);
    if (json_member(b, "initial_us")->integer > json_member(whole, "initial_us")->integer ||
        json_member(b, "ceiling_us")->integer > json_member(whole, "ceiling_us")->integer) {
        finding(r->file, b->line, "%s: the %s bound reaches past the whole case's",
                json_text(r->c, "case_id"), what);
    }
}

/**
 * A machine that declares a fault has what shows the fault: a serial console
 * for a panic or for records, and QMP for a halt; and its case has an
 * assertion of its scenario's, whose report gives the boot's end.
 */
static void rule_faults(const struct rule *r)
{
    const struct json *machines = json_member(r->c, "machines");
    const char *case_id = json_text(r->c, "case_id");
    size_t i;

    for (i = 0; machines != NULL && i < machines->count; i++) {
        const struct json *m = machines->item[i];
        const struct json *fault = json_member(m, "fault");
        const char *end = json_text(fault, "end");
        const char *id = json_text(m, "machine_id");

        if (fault == NULL) {
            continue;
        }
        if ((same(end, "panic") || json_member(fault, "records")->count > 0) &&
            !same(json_text(m, "console"), "serial")) {
            finding(r->file, fault->line,
                    "%s: the machine %s ends in a fault whose records only a serial console "
                    "shows",
                    case_id, id);
        }
        if (same(end, "halted") && json_member(m, "qmp")->integer == 0) {
            finding(r->file, fault->line,
                    "%s: the machine %s ends halted, which only its QMP monitor shows", case_id,
                    id);
        }
        if (!program_reports(r->c)) {
            finding(r->file, fault->line,
                    "%s: the machine %s ends in a fault, and no assertion of the case is its "
                    "scenario's, whose report gives a boot's end",
                    case_id, id);
        }
    }
}

/**
 * A machine whose console is a virtio console's port boots by direct
 * firmware, where the launcher names the port to the kernel with a boot
 * option, and has no boot option that names a console itself.
 */
static void rule_consoles(const struct rule *r)
{
    const struct json *machines = json_member(r->c, "machines");
    const char *case_id = json_text(r->c, "case_id");
    size_t i, k;

    for (i = 0; machines != NULL && i < machines->count; i++) {
        const struct json *m = machines->item[i];
        const struct json *qemu = json_member(m, "qemu");
        const struct json *options = json_member(qemu, "boot_options");
        const char *id = json_text(m, "machine_id");

        if (!same(json_text(m, "console"), "virtio")) {
            continue;
        }
        if (!same(json_text(qemu, "firmware"), "direct")) {
            finding(r->file, m->line,
                    "%s: the machine %s has a virtio console, which only a direct boot names "
                    "to the kernel",
                    case_id, id);
        }
        for (k = 0; options != NULL && k < options->count; k++) {
            if (options->item[k]->string != NULL &&
                strncmp(options->item[k]->string, "console=", 8) == 0) {
                finding(r->file, m->line,
                        "%s: the machine %s has a virtio console and a boot option that names "
                        "a console",
                        case_id, id);
            }
        }
    }
}

/** A bound lies between its floor and ceiling, and inside the whole case's. */
static void rule_bounds(const struct rule *r)
{
    const struct json *bounds = json_member(r->c, "bounds");
    const struct json *whole = json_member(bounds, "case");
    const struct json *stages = json_member(bounds, "stages");
    const char *case_id = json_text(r->c, "case_id");
    int host = same(r->domain, "host");
    size_t i;

    ordered(r, whole, "case");
    inside(r, json_member(bounds, "gap"), whole, "gap");
    for (i = 0; i < stages->count; i++) {
        inside(r, stages->item[i], whole, stages->item[i]->name);
    }
    if (!host && json_member(stages, "boot") == NULL) {
        finding(r->file, stages->line, "%s: a case that boots Rach2 has a bound for its boot stage",
                case_id);
    }
    if (host && json_member(stages, "boot") != NULL) {
        finding(r->file, stages->line, "%s: a host case boots nothing, so it has no boot stage",
                case_id);
    }
}

/** A witness's own bound is a quiet witness's alone. */
static void rule_progress(const struct rule *r)
{
    const struct json *list = json_member(r->c, "progress");
    size_t i;

    unique(r, list, "witness_id", "the witness");
    for (i = 0; list != NULL && i < list->count; i++) {
        const struct json *bound = json_member(list->item[i], "bound");

        if (bound != NULL && !same(json_text(list->item[i], "kind"), "quiet")) {
            finding(r->file, bound->line,
                    "%s: the witness %s has a bound of its own, which only a quiet witness has",
                    json_text(r->c, "case_id"), json_text(list->item[i], "witness_id"));
        }
        ordered(r, bound, "witness");
    }
}

/**
 * Returns the sum of `count` over the fixtures `kind` of `list`, 1 for each
 * without one.
 */
static long long fixture_count(const struct json *list, const char *kind)
{
    long long total = 0;
    size_t i;

    for (i = 0; list != NULL && i < list->count; i++) {
        if (same(json_text(list->item[i], "fixture"), kind)) {
            const struct json *count = json_member(list->item[i], "count");

            total += count != NULL ? count->integer : 1;
        }
    }
    return total;
}

/** Resolve a manifest's QEMU memory through the launcher's profile rules. */
static int qemu_memory(const struct rule *r, const struct json *qemu,
                       struct etude_qemu_memory *memory)
{
    static const struct etude_qemu_device device[] = {{.model = "device"}, {0}};
    static const struct etude_qemu_disk disk[] = {{.path = "disk"}, {.path = NULL}};
    const struct json *i8042 = json_member(qemu, "i8042");
    const struct json *arches = json_member(r->c, "architectures");
    const struct json *options = json_member(qemu, "boot_options");
    const char *boot[3] = {NULL, NULL, NULL};
    struct etude_qemu_profile p;

    memset(&p, 0, sizeof(p));
    p.architecture = json_text(qemu, "architecture");
    if (p.architecture == NULL)
        p.architecture = arches->count == 1 ? arches->item[0]->string : "both";
    p.cpu = json_text(qemu, "cpu");
    p.initrd = json_text(qemu, "initrd");
    p.memory_profile = json_text(qemu, "memory_profile");
    p.audio = json_text(qemu, "audio");
    p.display = json_text(qemu, "display");
    p.memory_bytes = (uint64_t)json_member(qemu, "memory_bytes")->integer;
    p.processors = json_member(qemu, "processors")->integer <= 64
                       ? (unsigned)json_member(qemu, "processors")->integer
                       : 0;
    p.firmware = same(json_text(qemu, "firmware"), "direct") ? ETUDE_FIRMWARE_DIRECT
                                                             : ETUDE_FIRMWARE_SEABIOS;
    p.disks = json_member(qemu, "disks")->count ? disk : NULL;
    p.devices = json_member(qemu, "devices")->count ? device : NULL;
    p.no_i8042 = i8042 != NULL && !i8042->integer;
    p.q35 = same(json_text(qemu, "machine"), "q35");
    if (options->count != 0)
        boot[0] = options->item[0]->string;
    if (options->count > 1)
        boot[1] = "extra";
    p.boot_options = boot;
    if (etude_qemu_memory(&p, memory) != 0) {
        finding(r->file, qemu->line, "%s: unsafe QEMU memory profile", json_text(r->c, "case_id"));
        return -1;
    }
    return 0;
}

/**
 * Require a reservation that covers every machine of the case together, each
 * with QEMU's overhead, and a tooling reservation that does too, since the
 * set driver claims it in place of the other where the case runs work in the
 * tooling machine. One boundary machine, declared exactly by `guest_memory`,
 * may be among them; no other machine has reduced backing.
 */
static void memory_resources(const struct rule *r, const struct json *res)
{
    const struct json *machines = json_member(r->c, "machines");
    const struct json *guest = json_member(res, "guest_memory");
    const struct json *tooling = json_member(res, "tooling_memory_bytes");
    const struct json *qemu = json_member(json_member(r->c, "requirements"), "qemu");
    struct etude_qemu_memory m, special = {0, 0, 0, 0};
    uint64_t total = 0;
    size_t i, n = machines != NULL ? machines->count : qemu != NULL ? 1 : 0;
    unsigned lazy = 0;

    for (i = 0; i < n; i++) {
        if (machines != NULL)
            qemu = json_member(machines->item[i], "qemu");
        if (qemu_memory(r, qemu, &m) != 0)
            continue;
        if (m.resident_bytes < m.configured_bytes) {
            special = m;
            lazy++;
        }
        if (m.resident_bytes > UINT64_MAX - m.overhead_bytes ||
            total > UINT64_MAX - (m.resident_bytes + m.overhead_bytes)) {
            finding(r->file, res->line, "%s: QEMU memory reservation overflows",
                    json_text(r->c, "case_id"));
            return;
        }
        total += m.resident_bytes + m.overhead_bytes;
    }
    if (total > (uint64_t)json_member(res, "memory_bytes")->integer)
        finding(r->file, res->line, "%s: memory_bytes does not reserve QEMU memory and overhead",
                json_text(r->c, "case_id"));
    if (tooling != NULL && total > (uint64_t)tooling->integer)
        finding(r->file, res->line,
                "%s: tooling_memory_bytes does not reserve QEMU memory and overhead",
                json_text(r->c, "case_id"));
    if (lazy != 0 &&
        (lazy != 1 || guest == NULL || !same(json_text(guest, "backing"), "lazy") ||
         (uint64_t)json_member(guest, "configured_bytes")->integer != special.configured_bytes ||
         (uint64_t)json_member(guest, "resident_bytes")->integer != special.resident_bytes))
        finding(r->file, res->line, "%s: boundary memory needs its exact guest_memory declaration",
                json_text(r->c, "case_id"));
    if (lazy == 0 && guest != NULL &&
        (!same(json_text(guest, "backing"), "reserved") ||
         json_member(guest, "resident_bytes")->integer !=
             json_member(guest, "configured_bytes")->integer))
        finding(r->file, guest->line, "%s: reduced backing requires the boundary memory profile",
                json_text(r->c, "case_id"));
}

/** The reservations cover what the case declares it takes. */
static void rule_resources(const struct rule *r)
{
    const struct json *res = json_member(r->c, "resources");
    const struct json *memory = json_member(res, "guest_memory");
    const struct json *native = native_fixtures(r->c);
    const struct json *machines = json_member(r->c, "machines");
    const char *case_id = json_text(r->c, "case_id");

    if (json_member(res, "cpu_millis")->integer > json_member(res, "cpu_ceiling_millis")->integer) {
        finding(r->file, res->line, "%s: the processor reservation is above its ceiling", case_id);
    }
    if (json_member(res, "cpu_class") != NULL &&
        json_member(json_member(r->c, "requirements"), "qemu") == NULL &&
        (machines == NULL || machines->count == 0)) {
        finding(r->file, res->line, "%s: a processor class is for a case with a QEMU profile",
                case_id);
    }
    if (memory != NULL &&
        (same(r->domain, "host") || json_member(memory, "resident_bytes")->integer >
                                        json_member(memory, "configured_bytes")->integer)) {
        finding(r->file, memory->line,
                "%s: guest_memory is for a guest's profile, resident within what is configured",
                case_id);
    }
    memory_resources(r, res);
    unique(r, json_member(res, "locks"), "lock", "the lock");
    if (fixture_count(native, "ports") > json_member(res, "ports")->integer) {
        finding(r->file, res->line, "%s: the native ports fixtures take more ports than reserved",
                case_id);
    }
    if (fixture_count(native, "volume") > json_member(res, "loop_devices")->integer) {
        finding(r->file, res->line, "%s: the native volumes take more loop devices than reserved",
                case_id);
    }
}

/** Reports each fixture of `list` that is not of the kinds `allowed`. */
static void allowed_fixtures(const struct rule *r, const struct json *list, const char *allowed,
                             const char *where)
{
    size_t i;

    for (i = 0; list != NULL && i < list->count; i++) {
        const char *kind = json_text(list->item[i], "fixture");
        char word[40];

        snprintf(word, sizeof word, " %s ", kind != NULL ? kind : "");
        if (strstr(allowed, word) == NULL) {
            finding(r->file, list->item[i]->line, "%s: %s has no %s fixture",
                    json_text(r->c, "case_id"), where, kind);
        }
    }
}

/** Each placement has the fixtures the Readme gives it. */
static void rule_fixtures(const struct rule *r)
{
    static const char *const single[] = {"workspace", "mountns", "ipcns", "utsns",   "root",  "log",
                                         "alone",     "console", "pty",   "private", "gpumin"};
    const struct json *native = native_fixtures(r->c);
    const char *case_id = json_text(r->c, "case_id");
    size_t i;

    allowed_fixtures(r, json_member(json_member(r->c, "execution"), "isolation"), " scratch disk ",
                     "a host or QEMU placement");
    if (native == NULL) {
        return;
    }
    allowed_fixtures(r, native,
                     " workspace stage volume tmpfs mountns ipcns utsns root ids pty ports peer "
                     "log alone console private gpumin ",
                     "a native run");
    if (fixture(native, "workspace") == NULL) {
        finding(r->file, native->line, "%s: a native case has a workspace fixture", case_id);
    }
    if (fixture(native, "console") != NULL && fixture(native, "alone") == NULL) {
        finding(r->file, native->line, "%s: the console fixture comes with alone", case_id);
    }
    for (i = 0; i < sizeof single / sizeof single[0]; i++) {
        if (fixture_count(native, single[i]) > 1) {
            finding(r->file, native->line, "%s: the fixture %s is given twice", case_id, single[i]);
        }
    }
}

/** The class architecture is for a case of i686 alone. */
static void rule_native(const struct rule *r)
{
    const struct json *archs = json_member(r->c, "architectures");
    const char *native_class = json_text(json_member(r->c, "native"), "class");

    if (same(native_class, "architecture") &&
        (archs->type != JSON_ARRAY || archs->count != 1 || !same(archs->item[0]->string, "i686"))) {
        finding(r->file, line_at(r->c, "native"),
                "%s: the native class architecture is for a case of i686 alone",
                json_text(r->c, "case_id"));
    }
}

/* ---- where ---- */

/** Splits `where` into its file and its lines; returns 0, or -1 for no line. */
static int split_where(const char *where, char *file, size_t n, long *first, long *last)
{
    const char *colon = strrchr(where, ':');
    char *end;

    *first = *last = 0;
    snprintf(file, n, "%.*s", colon ? (int)(colon - where) : (int)strlen(where), where);
    if (colon == NULL) {
        return -1;
    }
    *first = strtol(colon + 1, &end, 10);
    *last = *end == '-' ? strtol(end + 1, NULL, 10) : *first;
    return 0;
}

/** Returns the number of lines of `path`, or -1 when it cannot be read. */
static long count_lines(const char *path)
{
    FILE *f = fopen(path, "rb");
    long lines = 0;
    int c, last = '\n';

    if (f == NULL) {
        return -1;
    }
    while ((c = getc(f)) != EOF) {
        lines += c == '\n';
        last = c;
    }
    fclose(f);
    return lines + (last != '\n');
}

/** Reports a `where` that names no file or line of the tree at `root`. */
static void check_where(const struct rule *r, const struct json *where, const char *root,
                        int need_line)
{
    char file[512], path[1100];
    long first, last, lines;
    const char *case_id = json_text(r->c, "case_id");
    int has_line;

    if (where == NULL) {
        return;
    }
    has_line = split_where(where->string, file, sizeof file, &first, &last) == 0;
    if (!has_line && need_line) {
        finding(r->file, where->line, "%s: %s names a file without its line", case_id,
                where->string);
        return;
    }
    if (file[0] == '/' || strstr(file, "..") != NULL) {
        finding(r->file, where->line, "%s: %s is not a path inside the tree", case_id,
                where->string);
        return;
    }
    if (root == NULL) {
        return;
    }
    snprintf(path, sizeof path, "%s/%s", root, file);
    lines = count_lines(path);
    if (lines < 0) {
        finding(r->file, where->line, "%s: %s: no such file in the tree", case_id, where->string);
    } else if (has_line && (first < 1 || first > last || last > lines)) {
        finding(r->file, where->line, "%s: %s: the file ends at line %ld", case_id, where->string,
                lines);
    }
}

/** Each `where` names lines of the tree, and each timing assumption its own. */
static void rule_where(const struct rule *r, const char *root)
{
    const struct json *assertions = json_member(r->c, "assertions");
    const struct json *timing = json_member(r->c, "timing");
    size_t i;

    for (i = 0; assertions != NULL && i < assertions->count; i++) {
        check_where(r, json_member(assertions->item[i], "where"), root, 0);
    }
    unique(r, timing, "where", "the timing assumption at");
    for (i = 0; timing != NULL && i < timing->count; i++) {
        check_where(r, json_member(timing->item[i], "where"), root, 1);
    }
}

/* ---- manifests ---- */

/** Applies every rule to the case `c` of the manifest `file`. */
static void check_case(const char *file, const struct json *c, const char *root)
{
    struct rule r;

    r.file = file;
    r.c = c;
    r.domain = json_text(c, "domain");
    r.role = json_text(json_member(c, "execution"), "role");
    r.plan = json_text(json_member(c, "native"), "plan");
    rule_sources(&r);
    rule_branches(&r);
    rule_role(&r);
    rule_artifacts(&r);
    rule_disks(&r);
    rule_processor(&r);
    rule_network(&r);
    rule_attached(&r);
    rule_captures(&r);
    rule_audio(&r);
    rule_display(&r);
    rule_steps(&r);
    rule_helper_architecture(&r);
    rule_suspensions(&r);
    rule_networks(&r);
    rule_faults(&r);
    rule_consoles(&r);
    rule_bounds(&r);
    rule_progress(&r);
    rule_resources(&r);
    rule_fixtures(&r);
    rule_native(&r);
    rule_where(&r, root);
}

/** Returns 1 when `path` is a file `NAME.json` of a directory named `cases`. */
static int in_cases_dir(const char *path, char *name, size_t n)
{
    const char *base = strrchr(path, '/');
    const char *dir;
    size_t len;

    if (base == NULL || base - path < 5) {
        return 0;
    }
    dir = base - 5;
    if (strncmp(dir, "cases", 5) != 0 || (dir > path && dir[-1] != '/')) {
        return 0;
    }
    len = strlen(base + 1);
    if (len < 6 || strcmp(base + 1 + len - 5, ".json") != 0) {
        return 0;
    }
    snprintf(name, n, "%.*s", (int)(len - 5), base + 1);
    return 1;
}

/**
 * Checks the family's name and the identities of its cases against every
 * manifest before.
 */
static void check_family(const struct cases *set, size_t at)
{
    const struct manifest *m = &set->m[at];
    const char *family = json_text(m->doc, "family_id");
    const struct json *list = json_member(m->doc, "cases");
    size_t prefix = strlen(family), i;
    char name[256];

    if (in_cases_dir(m->path, name, sizeof name) && strcmp(name, family) != 0) {
        finding(m->path, line_at(m->doc, "family_id"),
                "the family %s has its manifest in %s.json, not %s.json", family, name, family);
    }
    for (i = 0; i < list->count; i++) {
        const char *id = json_text(list->item[i], "case_id");
        const char *other = NULL;
        const struct json *before = find_case(set, id, &other);

        if (strncmp(id, family, prefix) != 0 || id[prefix] != '.') {
            finding(m->path, list->item[i]->line,
                    "%s: a case of the family %s has an identity "
                    "that begins %s.",
                    id, family, family);
        }
        if (before != list->item[i]) {
            finding(m->path, list->item[i]->line, "%s: the case is declared in %s too", id, other);
        }
    }
}

/**
 * Reads the manifests of `paths` into `set` and checks each: its shape by
 * the case schema, then the rules of this file. Returns 0 when every
 * manifest passes, 1 after a finding and 2 when a file cannot be read.
 * A manifest whose shape is wrong is kept out of `set`.
 */
int load_manifests(struct cases *set, char **paths, int n)
{
    struct json *schema = read_schema(set->schemas, "case");
    int status = 0, i, before = findings();
    size_t j;

    if (schema == NULL) {
        return 2;
    }
    set->m = json_alloc((size_t)(n > 0 ? n : 1) * sizeof *set->m);
    set->n = 0;
    for (i = 0; i < n; i++) {
        int read_status = 0;
        struct json *doc = read_json(paths[i], &read_status);

        if (doc == NULL) {
            status = read_status > status ? read_status : status;
            continue;
        }
        if (schema_findings(schema, doc, paths[i]) != 0) {
            json_free(doc);
            continue;
        }
        set->m[set->n].path = paths[i];
        set->m[set->n].doc = doc;
        set->n++;
        check_family(set, set->n - 1);
        for (j = 0; j < json_member(doc, "cases")->count; j++) {
            check_case(paths[i], json_member(doc, "cases")->item[j], set->root);
        }
    }
    json_free(schema);
    if (status == 0 && findings() > before) {
        status = 1;
    }
    return status;
}

/** Frees the manifests of `set`. */
void free_manifests(struct cases *set)
{
    size_t i;

    for (i = 0; i < set->n; i++) {
        json_free(set->m[i].doc);
    }
    free(set->m);
    set->m = NULL;
    set->n = 0;
}

/**
 * Returns the first case `case_id` of `set`, and its file in `*file`, or NULL.
 */
const struct json *find_case(const struct cases *set, const char *case_id, const char **file)
{
    size_t i, j;

    for (i = 0; i < set->n; i++) {
        const struct json *list = json_member(set->m[i].doc, "cases");

        for (j = 0; j < list->count; j++) {
            if (same(json_text(list->item[j], "case_id"), case_id)) {
                if (file != NULL) {
                    *file = set->m[i].path;
                }
                return list->item[j];
            }
        }
    }
    return NULL;
}

/**
 * Prints each line that a timing assumption of `set` declares, as `FILE:LINE
 * KIND CASE`, for suitecheck.sh to accept at that line and no other.
 */
int print_timing(const struct cases *set)
{
    size_t i, j, k;

    for (i = 0; i < set->n; i++) {
        const struct json *list = json_member(set->m[i].doc, "cases");

        for (j = 0; j < list->count; j++) {
            const struct json *timing = json_member(list->item[j], "timing");

            for (k = 0; k < timing->count; k++) {
                char file[512];
                long first, last, line;

                if (split_where(json_text(timing->item[k], "where"), file, sizeof file, &first,
                                &last) != 0) {
                    continue;
                }
                for (line = first; line <= last; line++) {
                    say("%s:%ld %s %s\n", file, line, json_text(timing->item[k], "kind"),
                        json_text(list->item[j], "case_id"));
                }
            }
        }
    }
    return 0;
}

/**
 * Prints the family of each manifest of `set`, in the order given, as
 * `FAMILY_ID PATH`; the last part of the family, split at dots, is the suite
 * that runs its cases.
 */
void print_families(const struct cases *set)
{
    size_t i;

    for (i = 0; i < set->n; i++) {
        say_text(json_text(set->m[i].doc, "family_id"));
        say_text(" ");
        say_text(set->m[i].path);
        say_text("\n");
    }
}
