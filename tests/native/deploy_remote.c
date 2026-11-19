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
 * that reused the recorded PID. A controller or a collection of an older
 * deployment recorded instead the etude boot token of
 * /dev/shm/etude-guest-boot, which lives as long as the boot, so a recorded
 * boot is this boot's when it is the boot_id or that token. A controller claim
 * or a collection lock of another boot belongs to a process that no longer
 * exists, and is cleared with a line that names it; a collection lock of this
 * boot, or one with no owner record, is refused by name. When the etude boot
 * token cannot be read whole, a recorded boot of hexadecimal digits alone, as
 * the token is, may be this boot's and is kept. Every change of the collection
 * lock, the setup's clearing of a stale one and the collection's making and
 * release of its own, writes STATE/collection.journal first, with the owner
 * record of the lock it changes, and removes it once the change is whole, so
 * that a journal seen under the flock below was left by a process stopped
 * midway: the setup then removes the lock when it has no owner record or the
 * one the journal names, keeps a lock of any other owner, and removes the
 * journal, with a line that names what it removed. KRFFS, the root of the
 * installed machine, refuses a directory's rename, so the lock cannot move
 * aside before its removal. The look at the claims and the lock, their removal
 * and the making of this run's claim hold flock(1) on STATE/collection.serial,
 * which the collection holds while it takes or releases its lock and the
 * cleanup while it looks at or removes its claim, so that no setup removes a
 * claim or a lock made after its reading. A claim already gone counts as
 * cleared. The etude boot token is still published for the measurement contexts
 * that read it. */
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
        "token=; if test -f \"$bootfile\" && test ! -L \"$bootfile\"; then "
        "token=$(head -c 65 \"$bootfile\") || token=; fi; "
        "case $token in *[!0-9a-f]*) token=;; esac; test ${#token} -eq 64 || token=; "
        "thisboot() { test \"$1\" = \"$boot\" && return 0; "
        "if test -n \"$token\"; then test \"$1\" = \"$token\"; return; fi; "
        "case $1 in *[!0-9a-f]*) return 1;; esac; }; "
        "refuse() { printf 'native-deploy: refused: %%s: cannot clear it\\n' \"$1\" >&2; "
        "exit 1; }; "
        "state='%s/state'; test ! -L \"$state\"; "
        "mkdir -m 700 \"$state\" 2>/dev/null || test -d \"$state\"; "
        "exec 9> \"$state/collection.serial\"; flock 9; "
        "for old in '%s'/controller-*; do test -f \"$old/identity\" || continue; "
        "oboot=; read opid oborn oboot < \"$old/identity\" || true; "
        "test -n \"$oboot\" && ! thisboot \"$oboot\" || continue; "
        "rm -f \"$old/identity\" \"$old/owner\"; rmdir \"$old\" || test ! -e \"$old\" || "
        "refuse \"$old\"; "
        "printf 'native-deploy: cleared: %%s: pid %%s of boot %%s\\n' "
        "\"$old\" \"$opid\" \"$oboot\" >&2; done; "
        "lock=\"$state/collection.lock\"; journal=\"$state/collection.journal\"; "
        "if test -f \"$journal\"; then jop=; jrec=; read jop jrec < \"$journal\" || true; "
        "if test -d \"$lock\"; then jown=; "
        "test ! -f \"$lock/owner\" || read jown < \"$lock/owner\" || true; "
        "if test -z \"$jown\" || test \"$jown\" = \"$jrec\"; then rm -rf \"$lock\" || true; "
        "test ! -e \"$lock\" || refuse \"$lock\"; "
        "printf 'native-deploy: cleared: %%s: left by a stopped change: %%s %%s\\n' \"$lock\" "
        "\"$jop\" \"$jrec\" >&2; fi; fi; rm -f \"$journal\"; fi; "
        "held='held with no owner record'; "
        "if test -d \"$lock\" && test -f \"$lock/owner\"; then "
        "lpid=; lboot=; read lpid lborn lboot < \"$lock/owner\" || true; "
        "held=\"held by pid $lpid of boot $lboot\"; "
        "if test -n \"$lboot\" && test \"$lboot\" != - && ! thisboot \"$lboot\"; then "
        "printf 'clear %%s %%s %%s\\n' \"$lpid\" \"$lborn\" \"$lboot\" > \"$journal\"; "
        "rm -f \"$lock/owner\"; rmdir \"$lock\" || refuse \"$lock\"; rm -f \"$journal\"; "
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
/* Stop this deployment's controller and remove its claim. Every look at the
 * claim holds flock(1) on STATE/collection.serial, as the setup does, which
 * makes STATE if it is absent; the setup writes a claim whole under that lock,
 * so a claim seen under it is complete or was left by a writer that died. The
 * first look reads the receipt; the wait for the recorded process to end then
 * runs outside the lock, since the collection takes it to release its own;
 * the second look checks the collection lock and the claim again and removes
 * the claim. A claim absent at the second look counts as removed, and one with
 * no owner record, which only a dead writer leaves, is removed, with a line
 * that says so once its rmdir has succeeded. A claim whose recorded process
 * still runs in this boot fails the cleanup, and so does a claim of another
 * deployment. */
int nd_remote_cleanup(struct nd_deploy *d)
{
    struct nd_process *process = NULL;
    char guard[4096], command[8192];
    if (!nd_identity(d, guard, sizeof(guard)) ||
        !nd_format(command, sizeof(command),
                   "%s claim='%s/controller-%s'; receipt=\"$claim/identity\"; state='%s/state'; "
                   "valid() { case $pid:$born in *[!0-9:]*|:*|*:) return 1;; esac; "
                   "test \"$pid\" -gt 1 && test \"$born\" -gt 0; }; "
                   "alive() { test \"$boot\" = \"$(cat /proc/sys/kernel/random/boot_id)\" && "
                   "test -r /proc/$pid/stat && "
                   "test \"$(awk '{print $22}' /proc/$pid/stat 2>/dev/null)\" = \"$born\"; }; "
                   "serial() { exec 9> \"$state/collection.serial\"; flock 9; }; "
                   "umask 077; test ! -L \"$state\"; "
                   "mkdir -m 700 \"$state\" 2>/dev/null || test -d \"$state\"; "
                   "serial; waiting=; if test -d \"$claim\" && test -f \"$claim/owner\"; then "
                   "test \"$(cat \"$claim/owner\")\" = '%s'; "
                   "if read pid born boot 2>/dev/null < \"$receipt\"; then valid; waiting=yes; fi; "
                   "fi; exec 9>&-; "
                   "if test -n \"$waiting\"; then "
                   "while alive; do kill -TERM \"$pid\" || break; sleep 1; done; fi; "
                   "serial; test ! -d \"$state/collection.lock\"; "
                   "if test -d \"$claim\"; then orphan=; "
                   "if test -f \"$claim/owner\"; then test \"$(cat \"$claim/owner\")\" = '%s'; "
                   "if read pid born boot 2>/dev/null < \"$receipt\"; then valid; "
                   "if alive; then exit 1; fi; fi; else orphan=yes; fi; "
                   "rm -f \"$receipt\" \"$claim/owner\"; rmdir \"$claim\"; "
                   "if test -n \"$orphan\"; then "
                   "printf 'native-deploy: cleared: %%s: no owner record\\n' \"$claim\" >&2; fi; "
                   "fi; exec 9>&-; "
                   "printf 'native-deploy: remote controller ended\\n'",
                   guard, d->c.target, d->c.run, d->c.target, d->c.nonce, d->c.nonce))
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
