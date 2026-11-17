/* Deployment-only SSH commands. Case control and verdicts stay in etude. */
#define _POSIX_C_SOURCE 200809L
#include "deploy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Check the installed machine_identity.sh contract in the same remote
 * process immediately before its write or signal. Every substituted remote
 * value is restricted to one shell word by nd_options. */
int nd_identity(struct nd_deploy *d, char *prefix, size_t size)
{
    const struct nd_config *c = &d->c;
    return nd_format(
        prefix, size,
        "set -eu; test \"$(cat /sys/class/net/eth0/address)\" = '%s'; "
        "slot=$(sed -n 's/.*rach2\\.slot=\\([a-z]*\\).*/\\1/p' /proc/cmdline); "
        "test \"$slot\" = '%s'; status=$(bootctl status); "
        "printf '%%s\\n' \"$status\" | grep -Eq '^bootctl: next_entry: (empty|absent)'; "
        "runs=$(printf '%%s\\n' \"$status\" | awk -v s=\"$slot:\" "
        "'$2 == s && $3 == \"runs:\" { for (i = 4; i <= NF; i++) print $i }'); "
        "if test -z \"$runs\"; then runs=$(printf '%%s\\n' \"$status\" | awk -v s=\"$slot:\" "
        "'$2 == s && $3 == \"first\" { sub(\",\", \"\", $5); print $5 \"+\" $6 }'); fi; "
        "test -n \"$runs\"; "
        "sum=$(for r in $runs; do dd if=/dev/bootpart bs=512 skip=\"${r%%+*}\" "
        "count=\"${r#*+}\" 2>/dev/null; done | head -c %llu | sha256sum); "
        "test \"${sum%%%% *}\" = '%s'; "
        "printf 'native-deploy: verified %s mac %s slot %%s kernel %%s\\n' \"$slot\" '%s' >&2; ",
        c->mac, c->slot, (unsigned long long)c->kernel_size, c->kernel_sha, c->host, c->mac,
        c->kernel_sha);
}

int nd_stage(struct nd_deploy *d)
{
    struct nd_process *process = NULL;
    char guard[4096], command[16384];
    const struct nd_config *c = &d->c;
    if (!nd_identity(d, guard, sizeof(guard)) ||
        !nd_format(
            command, sizeof(command),
            "%s test ! -L '%s'; umask 077; if test ! -e '%s'; then mkdir -m 711 '%s'; fi; "
            "test \"$(stat -c '%%u:%%a' '%s')\" = '0:711'; "
            "transfer='%s/transfer-%s'; dest='%s/p-%.16s'; "
            "test ! -e \"$dest\"; test ! -L \"$dest\"; mkdir -m 700 \"$transfer\"; "
            "upload=\"$transfer/archive.tar.gz\"; made=; "
            "cleanup() { status=$?; rm -rf \"$transfer\"; "
            "if test -n \"$made\"; then rm -rf \"$dest\"; fi; exit \"$status\"; }; "
            "trap cleanup EXIT; trap 'exit 143' TERM; trap 'exit 130' INT; trap 'exit 129' HUP; "
            "cat > \"$upload\"; sum=$(sha256sum \"$upload\"); "
            "test \"${sum%%%% *}\" = '%s'; "
            "printf 'native-deploy: upload verified\\n' >&2; "
            "mkdir -m 700 \"$dest\"; made=yes; "
            "tar -xzf \"$upload\" -C \"$dest\"; "
            "printf 'native-deploy: extraction complete\\n' >&2; "
            "(cd \"$dest/tree\"; sha256sum -c assets.sha256); "
            "printf 'native-deploy: assets verified\\n' >&2; "
            "printf '%%s\\n' '%s' > \"$dest/archive.sha256\"; "
            "if id '%s' >/dev/null 2>&1; then test \"$(id -u '%s')\" = '%u'; "
            "test \"$(id -g '%s')\" = '%u'; else "
            "/bin/busybox addgroup -g %u '%s'; "
            "/bin/busybox adduser -D -u %u -G '%s' -h '%s/account' '%s'; fi; "
            "home=$(awk -F: -v u='%s' '$1 == u { print $6 }' /etc/passwd); test -n \"$home\"; "
            "if test ! -d \"$home\"; then mkdir -p \"$home\"; chown %u:%u \"$home\"; fi; "
            "made=; printf 'native-deploy: staged %%s\\n' \"$dest\"; ",
            guard, c->target, c->target, c->target, c->target, c->target, c->run, c->target,
            c->archive_sha, c->archive_sha, c->archive_sha, c->account, c->account, c->uid,
            c->account, c->uid, c->uid, c->account, c->uid, c->account, c->target, c->account,
            c->account, c->uid, c->uid))
        return 0;
    return nd_ssh(d, "stage", command, c->archive, &process) == 0;
}

/* The receipt records a random owner token plus the PID, the kernel start
 * identity across exec and the kernel's boot_id, which every boot has and no
 * program writes. Cleanup cannot signal another deployment's process, or one
 * that reused the recorded PID. A controller claim or a collection lock whose
 * recorded boot_id is not the running one belongs to a process that no
 * longer exists, and is cleared with a line that names it; a collection lock
 * of this boot, or one with no owner record, is refused by name. The look at
 * the claims and the lock, their removal and the making of this run's claim
 * hold flock(1) on STATE/collection.serial, which the collection holds while
 * it takes its lock, so that no setup removes a claim or a lock made after
 * its reading. The etude boot token is still published for the measurement
 * contexts that read it. */
static int run_setup(struct nd_deploy *d, char *setup, size_t size)
{
    const struct nd_config *c = &d->c;
    return nd_format(
        setup, size,
        "dest='%s/p-%.16s'; test \"$(cat \"$dest/archive.sha256\")\" = '%s'; "
        "claim='%s/controller-%s'; test ! -e '%s/results-%s'; umask 077; "
        "bootfile=/dev/shm/etude-guest-boot; if test ! -e \"$bootfile\"; then "
        "(scratch=/dev/shm/etude-boot-%s; mkdir -m 700 \"$scratch\"; "
        "trap 'rm -rf \"$scratch\"' EXIT; trap 'exit 143' TERM; "
        "trap 'exit 130' INT; trap 'exit 129' HUP; umask 022; "
        "printf '%%s' '%s' > \"$scratch/value\"; "
        "ln \"$scratch/value\" \"$bootfile\" || test -f \"$bootfile\"); fi; "
        "boot=$(cat /proc/sys/kernel/random/boot_id); "
        "case $boot in *[!0-9a-f-]*|'') exit 1;; esac; "
        "state='%s/state'; test ! -L \"$state\"; "
        "if test ! -d \"$state\"; then mkdir -m 700 \"$state\"; fi; "
        "exec 9> \"$state/collection.serial\"; flock 9; "
        "for old in '%s'/controller-*; do test -f \"$old/identity\" || continue; "
        "oboot=; read opid oborn oboot < \"$old/identity\" || true; "
        "test -n \"$oboot\" && test \"$oboot\" != \"$boot\" || continue; "
        "rm -f \"$old/identity\" \"$old/owner\"; rmdir \"$old\" || { "
        "printf 'native-deploy: refused: %%s: cannot clear it\\n' \"$old\" >&2; exit 1; }; "
        "printf 'native-deploy: cleared: %%s: pid %%s of boot %%s\\n' "
        "\"$old\" \"$opid\" \"$oboot\" >&2; done; "
        "lock=\"$state/collection.lock\"; held='held with no owner record'; "
        "if test -d \"$lock\" && test -f \"$lock/owner\"; then "
        "lpid=; lboot=; read lpid lborn lboot < \"$lock/owner\" || true; "
        "held=\"held by pid $lpid of boot $lboot\"; "
        "if test -n \"$lboot\" && test \"$lboot\" != - && test \"$lboot\" != \"$boot\"; then "
        "rm -f \"$lock/owner\"; rmdir \"$lock\" || { "
        "printf 'native-deploy: refused: %%s: cannot clear it\\n' \"$lock\" >&2; exit 1; }; "
        "printf 'native-deploy: cleared: %%s: %%s\\n' \"$lock\" \"$held\" >&2; fi; fi; "
        "if test -d \"$lock\"; then "
        "printf 'native-deploy: refused: %%s: %%s\\n' \"$lock\" \"$held\" >&2; "
        "exit 1; fi; "
        "mkdir -m 700 \"$claim\"; printf '%%s\\n' '%s' > \"$claim/owner\"; "
        "start=$(awk '{print $22}' /proc/$$/stat); "
        "printf '%%s %%s %%s\\n' \"$$\" \"$start\" \"$boot\" > \"$claim/identity\"; "
        "exec 9>&-; ",
        c->target, c->archive_sha, c->archive_sha, c->target, c->run, c->target, c->run, c->nonce,
        c->nonce, c->target, c->target, c->nonce);
}
static int run_arguments(struct nd_deploy *d, char *command, size_t size)
{
    const struct nd_config *c = &d->c;
    const char *build = !strcmp(c->architecture, "i686") ? "build" : "build/x86_64";
    char words[4096] = "";
    size_t used = 0;
    int n;
    for (n = 0; n < c->ncases; n++) {
        int count = snprintf(words + used, sizeof(words) - used, " %s", c->cases[n]);
        if (count < 0 || (size_t)count >= sizeof(words) - used)
            return 0;
        used += (size_t)count;
    }
    return nd_format(command, size,
                     "exec \"$dest/tree/%s/tests/native/etude\" run native %s "
                     "--tree \"$dest/tree\" --arch '%s' "
                     "--guest \"$dest/tree/%s/tools/etude/guest/etude-guest\" "
                     "--state '%s/state' --stage \"$dest/stage\" --results '%s/results-%s' "
                     "--run '%s' --purpose '%s' --machine-id '%s' --kernel-sha256 '%s' --peer "
                     "'%s:%u' --outside-peer '%s' --interface '%s' "
                     "--address '%s' --gateway '%s' --far '%s' --empty '%s' --silent '%s' --%s",
                     build, !strcmp(c->mode, "proof") ? "proof" : "", c->architecture, build,
                     c->target, c->target, c->run, c->run, c->purpose, c->mac, c->kernel_sha,
                     c->peer, c->echo_port, c->peer, c->interface, c->network, c->gateway, c->far,
                     c->empty, c->silent, words);
}
int nd_run(struct nd_deploy *d)
{
    struct nd_process *process = NULL;
    char guard[4096], setup[8192], arguments[8192], command[24576];
    if (!nd_identity(d, guard, sizeof(guard)) || !run_setup(d, setup, sizeof(setup)) ||
        !run_arguments(d, arguments, sizeof(arguments)) ||
        !nd_format(command, sizeof(command), "%s%s%s", guard, setup, arguments))
        return -1;
    d->remote_started = 1;
    return nd_ssh(d, "collection", command, NULL, &process);
}
int nd_remote_cleanup(struct nd_deploy *d)
{
    struct nd_process *process = NULL;
    char guard[4096], command[8192];
    if (!nd_identity(d, guard, sizeof(guard)) ||
        !nd_format(command, sizeof(command),
                   "%s claim='%s/controller-%s'; receipt=\"$claim/identity\"; "
                   "if test -d \"$claim\"; then test \"$(cat \"$claim/owner\")\" = '%s'; "
                   "read pid born boot < \"$receipt\"; "
                   "case $pid:$born in *[!0-9:]*|:*) exit 1;; esac; "
                   "test \"$pid\" -gt 1; test \"$born\" -gt 0; "
                   "while test \"$boot\" = \"$(cat /proc/sys/kernel/random/boot_id)\" && "
                   "test -r /proc/$pid/stat; do "
                   "start=$(awk '{print $22}' /proc/$pid/stat) || break; "
                   "test \"$start\" = \"$born\" || break; kill -TERM \"$pid\" || break; sleep 1; "
                   "done; fi; "
                   "test ! -d '%s/state/collection.lock'; "
                   "if test -d \"$claim\"; then rm -f \"$receipt\" \"$claim/owner\"; rmdir "
                   "\"$claim\"; fi; "
                   "printf 'native-deploy: remote controller ended\\n'",
                   guard, d->c.target, d->c.run, d->c.nonce, d->c.target))
        return 0;
    return nd_ssh(d, "remote-cleanup", command, NULL, &process) == 0;
}
int nd_fetch(struct nd_deploy *d)
{
    struct nd_process *process = NULL;
    char guard[4096], command[8192];
    if (!nd_identity(d, guard, sizeof(guard)) ||
        !nd_format(command, sizeof(command), "%s exec tar -czf - -C '%s' 'results-%s'", guard,
                   d->c.target, d->c.run))
        return 0;
    if (nd_ssh(d, "results-archive", command, NULL, &process) != 0)
        return 0;
    printf("native-deploy: remote results archive %s\n", process->out);
    return 1;
}
