/**
 * pwauth: the credential library.
 *
 * The only code in the image that compares a password against a stored hash,
 * mints a salt, or replaces a credential file. It is linked into the programs
 * that authenticate (`su`, `login`) and into the programs that write (`passwd`,
 * `adduser`) the way `resolv.c` is linked into `ping` and `nc`. One object
 * holding all three operations is what makes the rules below rules instead of
 * habits: there is one comparison, one alphabet, one replacement idiom, and one
 * lock.
 *
 * A reader and a writer are not symmetric here. A pure reader resolves the
 * entry itself and takes no lock (`pwauth_verify`), because the replacement
 * idiom below is atomic for a reader in fact and not by assertion: a descriptor
 * on a regular file names that file from open to close, so a reader that loses
 * the race against a writer sees the old file or the new one, whole, whether it
 * opened before the rename or after it. Readers never lock, which is what keeps
 * a stuck writer from keeping anyone out of the box. A writer is different: it
 * holds a transaction across its whole read-modify-write, which is why
 * `pwauth_check` takes the stored hash as a string rather than resolving the
 * account itself. The writer has already read the file it is about to replace,
 * and a second, independent lookup could answer from a different one.
 *
 * Diagnostics follow one split, so the two halves do not look arbitrary. A line
 * reporting the library's own failure names `pwauth`; a line an operator must
 * act on names the program they ran, which is why `cred_txn_begin` takes `prog`
 * and the lock lines carry it while `pwauth: no trusted entropy` does not. Each
 * caller adds its own refusal beside the library's line (`passwd: password
 * unchanged`).
 *
 * The transaction. A credential change is a read-modify-write, and writing a
 * new file and renaming it does not make that interval exclusive: two sessions
 * can read the same pre-image and rename in turn, and the second rename
 * restores the first one's old hash. The kernel offers no help, since every
 * `F_SETLK` is granted and `F_GETLK` always answers `F_UNLCK` (documented
 * no-conflict semantics dropbear depends on), so an advisory lock records
 * nothing at all. The library therefore carries its own transaction, and it is
 * a lock file: `cred_txn_begin` creates `CRED_LOCK_PATH` with
 * `O_CREAT|O_EXCL|O_WRONLY` at mode 0600 and writes the holder's pid as one
 * decimal line. The exclusive create is the whole of the exclusion, because a
 * syscall on this kernel does not yield, so nothing can interleave between the
 * existence test and the create. A file rather than a directory because it
 * carries the holder's pid, and the pid is what lets a refusal name the process
 * an operator has to deal with. Every writer takes it before its first read and
 * holds it through its last rename; `cred_rewrite` refuses outright when it is
 * not held, so a caller that forgot cannot ship quietly.
 *
 * A transaction belongs to the process that took it. A `fork` copies the
 * library's state but not the holding: the child holds nothing, its
 * `cred_txn_end`, its `cred_rewrite` and its death are all no-ops against its
 * parent's lock, and an acquire of its own goes through the ordinary wait and
 * refusal, naming the parent. A transaction does not survive an `exec` either,
 * since the new image carries neither the state nor the registered release, so
 * a writer must not exec while holding one. Both halves of that carry weight: a
 * child that merely exits must not drop its parent's lock, and the pid in the
 * file is the only thing that says which of the two the holder is.
 *
 * The lock is never broken. That is the rule, and these are its three reasons,
 * because "never break it" is the sentence a later reader will want to
 * overturn. First, an age bound cannot tell a stopped holder from an abandoned
 * one: `^Z` at a password prompt is one keystroke and `SIGSTOP` cannot be
 * caught at all, so a holder can sit for any length of time. Breaking its lock
 * lets a second writer read, publish and finish; the first then resumes and
 * renames its own snapshot over that change, and the lost update the lock
 * exists to prevent happens with the lock's blessing. Second, the break cannot
 * be made safe once the decision to break has been taken: between reading the
 * pid and unlinking the name there is no `funlink` and no way to bind the
 * removal to the file that was inspected, so a second breaker can have already
 * removed that lock and created its own, and the unlink then takes a live
 * writer's lock. The library holds itself to that: its own release and its own
 * publish unlink and rename only while the file still records this process, so
 * one lock cleared by hand cannot cascade through every writer after it. Third,
 * the cost of refusing is bounded and falls on the right person, because no
 * reader takes the lock: `su`, `login` and dropbear authenticate with none, so
 * nothing about entering the box depends on it, and what a `SIGKILL`ed or
 * segfaulted writer leaves behind is root's to remove: the lock, and, if it
 * died between the create and the rename, its own `<path>.tmp.<pid>`. The pid
 * in that name is what keeps two writers off one file, and it is also why no
 * later writer reclaims it the way a shared name was reclaimed. It blocks
 * nothing and it is never read; it is one more `rm`.
 * This is what `lckpwdf` does on Linux, and why `vipw` and `passwd` there
 * report a lock rather than breaking one. So: no caller breaks a lock, no
 * caller installs handlers of its own, and a lock left behind is root's to
 * remove.
 *
 * What the library cannot promise, stated here rather than left to be
 * discovered. Every name it acts on is resolved twice: once when it reads the
 * lock and once when it unlinks or renames, and this kernel has no call that
 * binds the second to the file the first saw. There is no `funlink`, no rename
 * taking a descriptor for its source, and no inode identity worth comparing,
 * since `st_ino` is a hash of the name here and two files that carried one name
 * in turn share it. So a release that has just read its own pid out of the lock
 * can still unlink a successor's, if recovery removed the lock and the
 * successor
 * took the name inside that window; and a publish that has just checked the
 * lock
 * still renames by name. The window is one scheduling boundary wide (this
 * kernel
 * does not preempt inside a syscall, and the read and the unlink have nothing
 * between them), and it opens only after the never-break rule has already been
 * broken by hand. It is narrowed as far as the syscall surface allows and it is
 * not closed. Closing it needs a primitive this kernel does not have: an unlink
 * and a rename that name an open descriptor rather than a path.
 *
 * Ordering, which no lock supplies. A change spanning several files can still
 * crash between two renames, and the transaction cannot fix that; the ordering
 * can. A writer orders its replacements so that every prefix is a state the
 * system reads safely: `/etc/shadow` with a locked `!` first, then
 * `/etc/group`, then `/etc/passwd`, so a crash leaves at worst an account
 * nobody can enter and no name that points at it.
 */
#ifndef _PWAUTH_H_
#define _PWAUTH_H_

#include <stddef.h>

/* The writer's lock. Root owns /etc, so removing a lock left by a killed writer
 * is root's one-line recovery: `rm /etc/.pwlock`. */
#define CRED_LOCK_PATH "/etc/.pwlock"

/* Retries after the first busy create, one second apart, before an acquire
 * gives up and reports the holder. */
#define CRED_LOCK_TRIES 10

/* Characters in a minted salt, the most a SHA-crypt setting string uses. A
 * buffer passed to `pwauth_mint_salt` holds this many plus the terminator. */
#define PWAUTH_SALT_LEN 16

/* The longest password this system can actually use, and the reason it is not
 * simply "as long as the buffer". Dropbear refuses an SSH password over
 * `DROPBEAR_MAX_PASSWORD_LEN`, which is 100, before it ever calls `crypt`, and
 * it does so to keep the hash inside its failure delay. A longer password would
 * therefore work at the console and never over the network, which is a way to
 * lock yourself out of the box by succeeding. One limit, and it is the smallest
 * one any reader in the image imposes. */
#define PWAUTH_MAX_PASSWORD 100

/* What `cred_rewrite` returns, and two of them what `cred_txn_end` returns. The
 * three are one distinction the caller cannot make for itself and must report
 * honestly: the replacement is live and on the platter, live but perhaps not on
 * the platter, or never happened at all. A release borrows the middle one for
 * the same shape, the lock being gone from the running system with its removal
 * perhaps not on the platter. */
#define CRED_PUBLISHED 0
#define CRED_UNFLUSHED 1
#define CRED_UNCHANGED (-1)

/**
 * Compare `password` against the `stored` hash, returning 1 on a match and 0
 * otherwise.
 *
 * The pure comparison, with no file access at all: `stored` is the second field
 * of a shadow line, supplied by whoever read the account. A NULL, empty, or
 * non-`$` field denies everyone, which is what makes `!` and `*` locks and a
 * missing entry deny alike. The salt and algorithm come from `stored` itself,
 * the `crypt(3)` convention, so a stronger stored hash needs no change here.
 */
int pwauth_check(const char *stored, const char *password);

/**
 * Resolve `user` in the shadow database and check `password` against it.
 *
 * The reader's whole path: `getspnam` followed by `pwauth_check`. It takes no
 * lock, deliberately (see the file header). Returns 1 on a match, 0 for a wrong
 * password, an absent account, or a locked one, which the caller must report as
 * one refusal so a probe cannot tell the three apart. A writer must not use
 * this: it would read the database a second time, and a replacement landing in
 * between would authenticate against a file other than the one it rewrites.
 */
int pwauth_verify(const char *user, const char *password);

/**
 * Print `prompt` and read one line from standard input into `buf`, returning 0
 * on success and -1 on end of input or a read error.
 *
 * The prompt goes to file descriptor 2 unbuffered whatever standard input is,
 * and only the echo suppression is conditional on `isatty(0)`; that is what
 * lets a driver holding a pipe on fd 0 rendezvous on the prompt instead of
 * guessing at a sleep. On a terminal the modes are saved with `tcgetattr` and a
 * copy with `ECHO` cleared installed, restored on every exit path. It never
 * opens `/dev/tty`: a probe can run with fd 0 bound to the console and no
 * controlling terminal at all, where a `/dev/tty` reader would block forever.
 *
 * The trailing newline is not stored, a carriage return ends the line the way a
 * newline does, and end of input with nothing read is -1, which is the refusal
 * a caller reports; end of input after some bytes is a complete line.
 *
 * A line longer than the limit is refused rather than truncated, and the limit
 * is the smaller of the caller's buffer and `PWAUTH_MAX_PASSWORD`, so every
 * caller gets the one every reader in the image agrees on without having to ask
 * for it. Keeping the prefix that fitted would store a secret the user never
 * chose and never typed, and then tell them the change succeeded. A line
 * holding a NUL byte is refused the same way and not cut short at it, since
 * `strcmp` and `crypt` both stop at a NUL: two entries differing only past one
 * would confirm each other where a caller asks twice, and the secret stored
 * would be the prefix alone. A resume clears either refusal along with the line
 * that earned it, since the line being started again is a new one and the old
 * one's length is not its fault.
 *
 * Signals are handled by what each one endangers. A stop is not a death, so
 * `SIGTSTP` and `SIGCONT` are handled always, transaction or none: the terminal
 * maps `^Z` to `SIGTSTP`, so one keystroke would otherwise leave the console
 * with echo off. Echo is taken away only while this process group owns the
 * terminal: a reader started in the background would otherwise silence one
 * belonging to the shell that started it and then stop on `SIGTTIN` at its
 * first read, and the modes it left are the terminal's, not the process's, so
 * killing it does not undo them. The `SIGCONT` that a `fg` sends applies them
 * through the resume path instead. A death is the transaction's business, so
 * the death set is installed here only when this process holds no transaction, which leaves
 * `cred_txn_begin`'s in force when it does. Every
 * disposition is restored before returning. `SIGSTOP` cannot be caught, so a
 * stop that way leaves the console with echo off until the reader is continued
 * or killed; nothing correctness-bearing rests on it, since no lock is broken
 * and no update is lost.
 *
 * A resume starts the line over, and it is the only thing that does. The line
 * discipline flushes what is queued when it recognizes the suspend character,
 * but a byte that arrived behind that character in the same write is queued
 * like any other, and the read returns it before the stop is served: a paste, a
 * scripted driver, and an `ssh` segment all deliver `^Z` and the next keystroke
 * in one burst. So a byte from before the stop can already be in the buffer
 * when the continue arrives, and everything collected is dropped there rather
 * than spliced onto what is typed after the reprinted prompt. A signal that is
 * not a stop leaves the collected bytes alone.
 *
 * The resume is consumed before a read as well as after one. A stop can be
 * served while the modes are being set or the prompt written, with no read
 * outstanding at all, and a reader that looked only after its reads would find
 * the flag still up when the first one returned and throw away the first
 * character typed after the `fg`. That is a correct password refused, with
 * nothing on the terminal to say why.
 *
 * What remains is the few instructions between that check and the `read`
 * itself. A stop served there looks, afterwards, exactly like one served while
 * the read was outstanding, so the byte in hand is dropped, and that costs a
 * whole authentication rather than a keystroke: the line discipline stays
 * canonical, so the one-byte read returns only once the whole line has been
 * submitted, the discarded byte is that line's first, and what follows is
 * consumed as a password missing its first character. No caller retries, so
 * the attempt is refused and the user starts again. No arrangement of flags distinguishes
 * them: it would take an unblock and a wait that are one operation, which is
 * what `ppoll`'s signal mask is for and what this kernel accepts without
 * applying. The residue points the safe way, since the alternative is a byte
 * typed and echoed before the stop becoming part of a password.
 *
 * A continue does not by itself return the terminal, since a shell's `bg` is
 * that signal without it, so the resume re-applies echo-off and reprints only
 * while this process is still the foreground group. Continued in the
 * background, the reader leaves the terminal to the shell that owns it and
 * stops again on `SIGTTIN`; the `fg` that follows does the work.
 *
 * Two more dispositions, both about a terminal that must not be left silenced.
 * `SIGPIPE` is caught for the length of the call, because echo is taken away
 * before the prompt is written to file descriptor 2: a caller whose standard
 * error is a pipe with no reader would otherwise be killed at exactly that
 * point and leave the console with echo off. It is a handler that does nothing
 * rather than `SIG_IGN`, which no write can tell apart, for the reason
 * `cred_txn_begin` gives: an ignored disposition is the one an exec carries
 * into the next image. And the prompt's state is taken down with the stop and
 * continue signals blocked, since the modes go back before the handlers come
 * off and a `SIGCONT` arriving in between would re-apply echo-off through a
 * handler that is about to be removed. Both are restored before the call
 * returns.
 */
int pwauth_read_password(const char *prompt, char *buf, size_t size);

/**
 * Map `n` bytes onto `n` characters of the `crypt(3)` alphabet `./0-9A-Za-z`,
 * writing them and a terminator to `out`.
 *
 * A function of the bytes alone, separate from the draw so a test can exercise
 * the mapping without entropy. Each byte contributes its low six bits, so the
 * output is uniform when the input is, and `out` must have room for `n + 1`
 * characters.
 */
void pwauth_salt_map(const unsigned char *bytes, size_t n, char *out);

/**
 * Draw `PWAUTH_SALT_LEN` characters of trusted salt into `out`, returning 0 on
 * success and -1 on failure.
 *
 * The bytes come from `getrandom(2)` with no `GRND_INSECURE`, so the kernel
 * refuses rather than hand out its untrusted boot-jitter stream when the CPU
 * exposes neither RDSEED nor RDRAND. On any failure no byte of `out` is
 * written, `pwauth: no trusted entropy` is printed, and -1 returned: a
 * predictable salt is worse than a refused password change, and the caller is
 * the one that must say what it did not do. `size` must be at least
 * `PWAUTH_SALT_LEN + 1`.
 */
int pwauth_mint_salt(char *out, size_t size);

/**
 * Take the writer's exclusive lock, returning 0 when it is held and -1
 * otherwise.
 *
 * `prog` names the program an operator ran; it appears in every line this
 * function prints, since a refusal is something they must act on. An acquire
 * that finds the lock present prints `<prog>: credential database busy,
 * waiting` once and retries the create `CRED_LOCK_TRIES` times one second
 * apart. If the lock is still there the acquire fails, and the pid recorded in
 * the file decides only which diagnostic is printed, never whether anything is
 * removed: a pid that answers `kill(pid, 0)` (or answers `EPERM`) is reported
 * as busy, a pid that is gone and a file that records no pid are both reported
 * as a stale lock to remove as root. The file is not touched in any case.
 *
 * Every one of those refusals names the removal, the busy one included, because
 * a pid does not identify a process across a reboot. This kernel restarts
 * `next_pid` at every boot and the writers run early, so a lock a killed writer
 * left on a persistent image names a small pid that some unrelated process now
 * holds: an operator told only that the database is busy would wait for a
 * holder that will never release. The busy line therefore states the condition
 * to check before removing it rather than withholding the recovery.
 *
 * The release is registered with `atexit` before the lock is created, and an
 * acquire whose registration fails is refused rather than held: `atexit`
 * allocates once its builtin slots are full and can therefore fail, and a
 * transaction with nothing left to drop it would leak the lock on the very
 * ordinary `exit` this promises to cover. So an ordinary `exit` cannot leak
 * the lock, and every signal that can kill the writer is handled for the
 * transaction's whole lifetime, because `atexit` runs on no signal death: the
 * handler restores the terminal, unlinks any temporary file `cred_rewrite` left
 * open, and leaves through `exit` so the registered release drops the lock
 * rather than the handler duplicating it. That is why a caller installs no
 * handlers of its own: the lock outlives the terminal read, and the terminal
 * read is only one part of what a signal must undo.
 *
 * Which signals those are is decided by what the kernel does, not by the four
 * a writer expects. It terminates a process by default for every signal but
 * the four it ignores and the four it stops for, and it lets a process signal
 * one whose real uid matches its own, which a setuid writer keeps from whoever
 * ran it. So the user who ran `passwd` can send it `SIGUSR1` or `SIGSEGV` as
 * readily as `SIGTERM`, and each would otherwise leave a lock in `/etc`. The
 * library therefore takes every catchable one, the fault numbers included: a
 * real CPU exception on this kernel never reaches a disposition at all, so the
 * only `SIGSEGV` a handler sees is one that was sent, and handling it changes
 * nothing about a genuine fault. All of them are blocked across the acquire
 * itself, from just before the lock file is created until the holder pid is in
 * it, so no death can be served while the file exists and nothing yet says
 * whose it is.
 *
 * Two residues are left, and a caller should know both. `SIGKILL` and
 * `SIGSTOP` cannot be caught, and signals 32, 33 and 34 are reserved by this
 * libc, which refuses them from `sigaction` and `sigaddset` alike, here and on
 * Linux. A writer ended by any of those five leaves its lock behind, and the
 * next writer reports it rather than breaking it. `SIGPIPE` is caught for that same lifetime and
 * for the same reason as the deaths: a writer reports as it goes, and a
 * diagnostic written to a standard error that is a pipe with no reader would
 * otherwise kill it where no `atexit` runs and leave the lock in `/etc`. It is
 * caught rather than ignored because an exec carries an ignored disposition
 * into the new image and resets a caught one, and a child forked under a
 * transaction must not hand this library's `SIGPIPE` policy to a program that
 * never linked it. Every disposition this library installs is put back by
 * `cred_txn_end`, and `SIGPIPE` last of all, once there is nothing left to
 * report.
 *
 * An acquire that cannot record the holder pid fails and removes the lock it
 * just created, rather than holding an empty one: an empty lock is what every
 * later acquire reports as recording no holder and tells an operator to remove,
 * which would put a live writer's lock in their hands.
 *
 * A second `cred_txn_begin` in one process is a programming error and not
 * contention, so it is refused immediately, without touching the file. The same
 * call in a child that inherited the state is neither: the child holds nothing,
 * so it drops what it inherited and goes on to acquire for itself, which finds
 * the parent's lock and waits.
 */
int cred_txn_begin(const char *prog);

/**
 * As `cred_txn_begin`, with the lock path and the retry budget as parameters.
 *
 * This form exists for the host seam test, which must neither touch `/etc` nor
 * wait ten seconds to watch an acquire refuse. The shipped programs call
 * `cred_txn_begin`, whose path is compiled in and never read from the
 * environment, because this library is linked into a setuid-root program.
 */
int cred_txn_begin_at(const char *prog, const char *lock_path, int tries);

/**
 * Release the transaction: unlink the lock, close it, and restore the
 * dispositions `cred_txn_begin` installed. Returns 0 when the lock is gone,
 * `CRED_UNFLUSHED` when it is gone from the file system but the removal did not
 * reach the disk, and -1 when this process holds no transaction or when the
 * release could not finish.
 *
 * The lock is unlinked only while it still records this process, for the second
 * reason a lock is never broken: the name is not proof of the file. A release
 * that finds another writer's lock at the name leaves it and reports nothing,
 * having none of its own to drop, and returns 0: nothing of this transaction's
 * is left there. What earns the -1 is a lock that could not be looked at or
 * could not be removed, which is a lock file still in `/etc` and the next
 * writer's ten-second wait and refusal; the caller must report that rather than
 * a clean finish. It needs no descriptor of its own to do any of this, so a
 * caller that has filled its descriptor table still releases: the transaction
 * holds the lock's own, and the release closes it and reuses the slot.
 *
 * `CRED_UNFLUSHED` is the third answer and means what it means for a
 * replacement: the running system is in the state the caller asked for, and the
 * disk may not be. The removal of a lock is the last write a transaction makes
 * and often the only one, so a writer that reported a clean finish on a failed
 * flush would be promising the one thing a power cut takes back, and the next
 * boot would meet a lock naming a process that died with it. A caller reports
 * it as a failure to finish cleanly and does not retry: there is nothing left
 * to retry, the lock being already gone from the system it is running on.
 *
 * There is no separate abort. `cred_rewrite` leaves no temporary file behind
 * whichever way it went, so every exit path may simply end the transaction.
 * What it may already have done is publish: a `CRED_UNFLUSHED` replacement is
 * live, and ending the transaction neither undoes it nor pretends otherwise.
 */
int cred_txn_end(void);

/**
 * Replace `path` with the `len` bytes at `text`, returning `CRED_PUBLISHED`,
 * `CRED_UNFLUSHED` or `CRED_UNCHANGED`.
 *
 * The whole replacement idiom, and the required one for any file the system
 * authenticates against. It changes nothing at all unless this process holds
 * the transaction. It then checks the lock, unlinks any leftover temporary
 * file,
 * creates `<path>.tmp.<pid>` with `O_EXCL`, writes the text, takes the owner
 * and
 * then the mode from the file being replaced, flushes, checks the lock again,
 * renames, and flushes once more through the same descriptor before closing.
 * Every step is checked.
 *
 * Five details carry the weight. The temporary name carries the writer's pid,
 * so
 * it belongs to one process and no other: a fixed `<path>.tmp` would be shared,
 * and a writer whose lock had been removed could unlink and recreate the file a
 * successor already had open, whose rename by name would then publish it. The
 * lock is checked before that unlink for the same reason and again between the
 * first flush and the rename, since a publish is a write to the shared database
 * and may not happen without one. The descriptor stays open across the rename
 * so
 * that there is still a descriptor to flush through once the file is published:
 * `fsync` on this kernel pushes the whole page cache, but it must be reached
 * through a live descriptor, and `sync(2)` would not do since it returns 0
 * unconditionally and would throw away exactly the `-EIO` this call exists to
 * report. The owner is set before the mode because a `chown` clears the set-id
 * bits. And the temporary file's mode becomes the result's mode, because a
 * rename changes only the name and the parent, so inheriting the original's
 * mode
 * and owner is the only thing that keeps a file that exists to be unreadable
 * from becoming readable. There is no hard-link backup because this file system
 * has no links at all.
 *
 * What the two lock checks do not buy is stated in the file header: the rename
 * that follows the second one is still by name, and no call here can bind it to
 * the file that was checked.
 *
 * The file being replaced must therefore exist: its mode and owner are the
 * result's, and there is nowhere else honest to get them. Bringing a credential
 * file into being is the image's job, not this idiom's. The inheritance is
 * faithful and has no floor: a database staged 0644, or carrying a set-id bit,
 * is reproduced as it is, and every later replacement reproduces it again. The
 * snapshot is taken by name at the top and published by name at the bottom, so
 * a `chmod` by root in between is reverted to what was read; the lock excludes
 * other writers through this library, not an operator at the console.
 *
 * The return is what the caller must report, and the three differ where it
 * matters most. A failure before the rename unlinks the temporary file, leaves
 * the target byte for byte as it was, and is `CRED_UNCHANGED`, the one a caller
 * may report as "unchanged". A failure of the second flush or of the close is
 * `CRED_UNFLUSHED`: the file system already carries the new file, the disk may
 * still carry the old one, and the caller must report a failure that says the
 * change is live but not durable, since the account now answers to the new
 * password whatever the platter holds. Both flushes are no-ops when the file
 * system is the RAM module, so the durability claim belongs to a disk-backed
 * image.
 */
int cred_rewrite(const char *path, const char *text, size_t len);

#endif /* _PWAUTH_H_ */
