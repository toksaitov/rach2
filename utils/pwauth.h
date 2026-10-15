/**
 * pwauth: the credential library.
 *
 * The only code in the image that compares a password with a stored hash,
 * makes a salt, or replaces a credential file. It is linked into the programs
 * that authenticate (`su`, `login`) and into the programs that write
 * (`passwd`, `adduser`), as `resolv.c` is linked into `ping` and `nc`, so the
 * image has one comparison, one salt alphabet, one replacement idiom and one
 * lock.
 *
 * A reader and a writer are treated differently. A reader resolves the entry
 * itself and takes no lock (`pwauth_verify`), because the replacement idiom
 * below is atomic for a reader: a descriptor on a regular file refers to that
 * file from open to close, so a reader that races a writer sees the old file
 * or the new one, whole, whether it opened before the rename or after it.
 * Since no reader locks, a writer stuck with the lock keeps nobody from
 * logging in. A writer holds a transaction across its whole
 * read-modify-write, which is why `pwauth_check` takes the stored hash as a
 * string instead of resolving the account itself: the writer has already
 * read the file it is about to replace, and a second lookup could answer from
 * a different one.
 *
 * A diagnostic that reports the library's own failure names `pwauth`; one an
 * operator must act on names the program they ran, which is why
 * `cred_txn_begin` takes `prog` and the lock lines begin with it while
 * `pwauth: no trusted entropy` does not. Each caller prints its own line
 * beside the library's (`passwd: password unchanged`).
 *
 * The transaction. A credential change is a read-modify-write, and writing a
 * new file and renaming it does not make that interval exclusive: two
 * sessions can read the same file and rename in turn, and the second rename
 * restores the first one's old hash. The transaction is a lock file:
 * `cred_txn_begin` creates `CRED_LOCK_PATH` with `O_CREAT|O_EXCL|O_WRONLY` at
 * mode 0600 and writes the holder's pid as one decimal line, and the
 * exclusive create is the exclusion. A writer may authenticate before
 * locking, then read and check the records again under the lock before it
 * publishes. `cred_rewrite` changes nothing when the lock is not held.
 *
 * A transaction belongs to the process that took it. A `fork` copies the
 * library's state but not the lock: in the child, `cred_txn_end`,
 * `cred_rewrite` and its death leave the parent's lock alone, and an acquire
 * of its own waits and fails as any other would, naming the parent. A
 * transaction does not survive an `exec` either, since the new image has
 * neither the state nor the registered release, so a writer must not exec
 * while it holds one. Both rules matter: a child that exits must not drop its
 * parent's lock, and the pid in the file is the only record of which of the
 * two holds it.
 *
 * No caller breaks the lock, for three reasons. First, an age limit cannot
 * tell a stopped holder from an abandoned one: `^Z` at a password prompt is
 * one keystroke and `SIGSTOP` cannot be caught, so a holder can be stopped
 * for any length of time. Breaking its lock lets a second writer read,
 * publish and finish; the first then resumes and renames its own snapshot
 * over that change, and the lost update the lock exists to prevent happens
 * anyway. Second, a break cannot be made safe: between reading the pid and
 * unlinking the name there is no `funlink` and no way to bind the removal to
 * the file that was read, so a second breaker can already have removed that
 * lock and created its own, and the unlink then removes a live writer's lock.
 * Third, a lock left behind costs little, because no reader takes it: `su`,
 * `login` and Dropbear authenticate without it, so logging in never depends
 * on it. What a writer killed by `SIGKILL` leaves is root's to remove: the
 * lock and, if it died between the create and the rename, its own
 * `<path>.tmp.<pid>`. The pid in that name keeps two writers off one file,
 * and it is why no later writer reclaims the file; it blocks nothing and is
 * never read. So no caller breaks a lock, no caller installs signal handlers
 * of its own, and a lock left behind is root's to remove.
 *
 * The library checks the recorded pid before it publishes and before it
 * releases, and neither check makes the operation by name that follows it
 * atomic with it: a lock removed by hand and replaced between the check and
 * the rename or the unlink is a successor's, and the rename publishes under
 * it or the unlink removes it. An inode comparison would leave the same gap,
 * and on the KRFFS boot root an inode number is a hash of names.
 *
 * Ordering, which no lock supplies. A change that spans several files can
 * still stop between two renames, and the transaction cannot prevent that;
 * the order of the renames can limit it. A writer orders its replacements so
 * that every prefix is a state the system reads safely: `/etc/shadow` with a
 * locked `!` first, then `/etc/group`, then `/etc/passwd`, so a crash leaves
 * at worst an account nobody can enter and no name that refers to it.
 */
#ifndef _PWAUTH_H_
#define _PWAUTH_H_

#include <stddef.h>

/* The writer's lock. Root owns /etc, so a lock left by a killed writer is
 * root's to remove: `rm /etc/.pwlock`. */
#define CRED_LOCK_PATH "/etc/.pwlock"

/* Retries after the first busy create, one second apart, before an acquire
 * gives up and reports the holder. */
#define CRED_LOCK_TRIES 10

/* Characters in a minted salt, the most a SHA-crypt setting string uses. A
 * buffer passed to `pwauth_mint_salt` has this many plus the terminator. */
#define PWAUTH_SALT_LEN 16

/* The longest password any reader in the image accepts. Dropbear rejects an
 * SSH password longer than `DROPBEAR_MAX_PASSWORD_LEN`, 100, before it calls
 * `crypt`, to keep the hash inside its failure delay, so a longer password
 * would work at the console and never over SSH. */
#define PWAUTH_MAX_PASSWORD 100

/* The longest account name any reader here takes, and the limit every writer
 * here keeps to. It is the length of the console login's name line
 * (`LOGIN_NAME_MAX` in login.c), which rejects a longer line: an account
 * whose name cannot be typed at `rach2 login:` could be entered only over
 * SSH. */
#define PWAUTH_MAX_NAME 63

/* The returns of `cred_rewrite`: the replacement is live and on the disk,
 * live but perhaps not on the disk, or not made at all. The caller cannot
 * tell these apart for itself. `cred_txn_end` returns 0, `CRED_UNFLUSHED` or
 * -1, and its `CRED_UNFLUSHED` means that the lock is gone from the running
 * system and its removal perhaps not on the disk. */
#define CRED_PUBLISHED 0
#define CRED_UNFLUSHED 1
#define CRED_UNCHANGED (-1)

/**
 * Compare `password` with the `stored` hash; 1 on a match, 0 otherwise.
 *
 * It reads no file: `stored` is the second field of a shadow line, supplied
 * by whoever read the account. A NULL, empty or non-`$` field matches no
 * password, so `!` and `*` locks and a missing entry deny alike. The salt
 * and the algorithm come from `stored` itself, the `crypt(3)` convention, so
 * a stronger stored hash needs no change here.
 */
int pwauth_check(const char *stored, const char *password);

/**
 * Resolve `user` in the shadow database and check `password` against it.
 *
 * The whole of a reader's check: `getspnam` followed by `pwauth_check`. It
 * takes no lock (see the file header). Returns 1 on a match, and 0 for a
 * wrong password, an absent account or a locked one, which the caller must
 * report with one message, so that a probe cannot tell the three apart. A
 * writer must not use this: it would read the database a second time, and a
 * replacement made in between would authenticate against a file other than
 * the one it rewrites.
 */
int pwauth_verify(const char *user, const char *password);

/**
 * Print `prompt` and read one line from standard input into `buf`, of `size`
 * bytes; 0 for a complete line, and -1 for end of input with nothing read, a
 * read error, a terminal-mode error, a line that is not accepted, or a `size`
 * below 2.
 *
 * The prompt goes to descriptor 2, unbuffered, whatever standard input is,
 * and only the echo suppression depends on standard input being a terminal,
 * so a driver holding a pipe on descriptor 0 can wait for the prompt instead
 * of guessing with a sleep. On a terminal the modes are saved with
 * `tcgetattr` and a copy with `ECHO` cleared is installed. A failure to read
 * or apply those modes fails the call, and the saved modes are restored
 * before it returns. It never opens `/dev/tty`: a probe can run with
 * descriptor 0 bound to the console and no controlling terminal, where that
 * open fails with `ENXIO`.
 *
 * The trailing newline is not stored, a carriage return ends the line as a
 * newline does, and end of input after some bytes is a complete line.
 *
 * A line longer than the limit is not accepted, rather than truncated, and
 * the limit is the smaller of `size - 1` and `PWAUTH_MAX_PASSWORD`, so every
 * caller gets the limit every reader in the image keeps to. Keeping the
 * prefix that fitted would store a secret the user never chose and never
 * typed, and then report the change as made. A line with a NUL byte is not
 * accepted either, and is not cut short at it, since `strcmp` and `crypt`
 * both stop at a NUL: two entries that differ only after one would match
 * where a caller asks twice, and the secret stored would be the prefix
 * alone. Such a line leaves `buf` zeroed and a diagnostic on standard error;
 * after a read error `buf` holds the bytes read so far. A resume clears
 * either condition with the line, since the line typed after it is a new
 * one.
 *
 * Each signal is handled by what it would leave behind. `SIGTSTP` and
 * `SIGCONT` are always handled, with a transaction or without: the terminal
 * maps `^Z` to `SIGTSTP`, so one keystroke would otherwise leave the console
 * with echo off. Echo is taken away only while this process group owns the
 * terminal: a reader started in the background would otherwise silence a
 * terminal belonging to the shell that started it and then stop on `SIGTTIN`
 * at its first read, and the modes it left are the terminal's, not the
 * process's, so killing it does not restore them. The `SIGCONT` that a `fg`
 * sends applies them through the resume instead. The signals that would kill
 * the process are handled here only when it holds no transaction, which
 * leaves `cred_txn_begin`'s handlers in force when it holds one. Every
 * disposition is restored before the call returns. `SIGSTOP` cannot be
 * caught, so a stop by it leaves the console with echo off until the reader
 * is continued or killed; no lock is broken and no update is lost by it.
 *
 * A resume starts the line over, and nothing else does. The line discipline
 * discards queued input when it reads the suspend character, but a byte that
 * arrived after that character in the same write is queued like any other,
 * and the read returns it before the stop is taken: a paste, a scripted
 * driver and an `ssh` segment all deliver `^Z` and the next keystroke
 * together. So a byte from before the stop can already be in the buffer when
 * the continue arrives, and everything collected is dropped then rather than
 * joined to what is typed after the prompt is printed again. A signal that
 * is not a stop leaves the collected bytes alone.
 *
 * The resume is checked before each read as well as after it. A stop can be
 * taken while the modes are set or the prompt written, with no read
 * outstanding, and a reader that looked only after its reads would find the
 * flag still set when the first one returned and drop the first character
 * typed after the `fg`: a correct password rejected, with nothing on the
 * terminal to say why.
 *
 * A stop taken in the few instructions between that check and the `read`
 * looks afterwards like one taken during the read, so the byte the read
 * returns is dropped, and that costs a whole attempt, not a keystroke: the
 * line discipline is canonical, so the one-byte read returns only after the
 * whole line has been entered, the dropped byte is that line's first, and
 * the rest is read as a password without its first character. No caller
 * retries, so the attempt fails and the user starts again. Telling the two
 * cases apart would take an unblock and a wait done as one operation, as
 * `ppoll` with a signal mask does, and this reader does not use one. The
 * loss is the safe way round, since the alternative is a byte typed and
 * echoed before the stop becoming part of a password.
 *
 * A continue does not by itself give the terminal back, since a shell's `bg`
 * sends that signal without it, so the resume applies echo-off and prints
 * the prompt again only while this process is still the foreground group.
 * Continued in the background, the reader leaves the terminal to the shell
 * that owns it and stops again on `SIGTTIN`; the `fg` that follows does the
 * work.
 *
 * Two more dispositions keep the terminal from being left silent. `SIGPIPE`
 * is caught for the length of the call, because echo is taken away before
 * the prompt is written to descriptor 2: a caller whose standard error is a
 * pipe with no reader would otherwise be killed at that point and leave the
 * console with echo off. The handler does nothing; it is not `SIG_IGN`, for
 * the reason `cred_txn_begin` gives: an ignored disposition passes through an
 * exec into the next image. And the prompt's state is taken down with the
 * stop and continue signals blocked, since the modes are restored before the
 * handlers are removed, and a `SIGCONT` arriving between the two would apply
 * echo-off again through a handler about to be removed. Both are restored
 * before the call returns.
 */
int pwauth_read_password(const char *prompt, char *buf, size_t size);

/**
 * Map `n` bytes onto `n` characters of the `crypt(3)` alphabet `./0-9A-Za-z`,
 * writing them and a terminator to `out`.
 *
 * A function of the bytes alone, apart from the draw, so a test can check the
 * mapping without entropy. Each byte gives its low six bits, so the output is
 * uniform when the input is, and `out` must have room for `n + 1`
 * characters. A NULL `bytes` or `out` writes nothing.
 */
void pwauth_salt_map(const unsigned char *bytes, size_t n, char *out);

/**
 * Draw `PWAUTH_SALT_LEN` characters of trusted salt into `out`, of `size`
 * bytes; 0 on success and -1 on failure.
 *
 * The bytes come from `getrandom(2)` without `GRND_INSECURE`, so the kernel
 * fails the call with `EIO`, rather than give bytes of its untrusted
 * boot-jitter stream, when the CPU has neither RDSEED nor RDRAND. On any
 * failure no byte of `out` is written and -1 is returned. Without trusted
 * bytes `pwauth: no trusted entropy` is printed: a predictable salt is worse
 * than a password left unchanged, and the caller is the one that must say
 * what it did not do. A `size` below `PWAUTH_SALT_LEN + 1` prints `pwauth:
 * salt buffer too small`.
 */
int pwauth_mint_salt(char *out, size_t size);

/**
 * Take the writer's lock at `CRED_LOCK_PATH`; 0 when it is held, and -1
 * otherwise.
 *
 * `prog` names the program an operator ran, and every line this function
 * prints begins with it, since a failure here is one they must act on. An
 * acquire that finds the lock present prints `<prog>: credential database
 * busy, waiting` once and retries the create `CRED_LOCK_TRIES` times, one
 * second apart. If the lock is still there the acquire fails, and the pid
 * recorded in the file decides only which diagnostic is printed, never
 * whether anything is removed: a pid that answers `kill(pid, 0)` (or answers
 * `EPERM`) is reported as busy, and a pid that is gone, a file that records
 * no pid and a file that cannot be read are reported as a stale lock to
 * remove as root. The file is not touched in any case.
 *
 * Every one of those reports names the removal, the busy one included,
 * because a pid does not identify a process across a reboot. This kernel
 * restarts `next_pid` at every boot and the writers run early, so a lock a
 * killed writer left on a persistent image records a small pid that some
 * unrelated process now has: an operator told only that the database is
 * busy would wait for a holder that will never release it. The busy line
 * therefore says what to check before removing the lock.
 *
 * The release is registered with `atexit` before the lock is created, and an
 * acquire whose registration fails returns -1 without the lock: `atexit`
 * allocates once its built-in slots are full and so can fail, and a
 * transaction with no registered release could leave its lock behind at an
 * ordinary exit. While the transaction is held, each signal that would kill
 * the process runs a handler that restores the terminal and leaves through
 * `exit`, so the registered release runs: it tries to remove any temporary
 * file and the lock, and reports a removal that fails, which then needs
 * cleaning up by hand.
 *
 * The signals handled are the ones the kernel ends a process for, not the few
 * a writer expects. By default the kernel terminates a process for every
 * signal but the four it ignores and the four it stops for, and it lets a
 * process signal another whose real uid matches its own, which a setuid
 * writer keeps from whoever ran it. So the user who ran `passwd` can send it
 * `SIGUSR1` or `SIGSEGV` as readily as `SIGTERM`, and each would otherwise
 * leave a lock in `/etc`. The library catches every signal that can be
 * caught, the fault signals included. A CPU fault in user mode reaches the
 * handler as well, so a writer that faults while it holds a transaction runs
 * the release on its way out. All of them are blocked across the acquire
 * itself, from just before the lock file is created until the holder's pid
 * is in it, so no death can be served while the file exists and does not yet
 * say whose it is.
 *
 * `_Exit`, `SIGKILL`, a CPU fault taken while those signals are blocked, and
 * the signals 32 to 34, which musl reserves, end the process without this
 * cleanup, and `SIGSTOP` suspends it until it is continued. `SIGPIPE` is
 * caught for the length of the transaction, so that a diagnostic written to
 * a closed pipe cannot stop the release. It is caught, not ignored, because
 * an exec passes an ignored disposition into the new image and resets a
 * caught one, and a child forked under a transaction must not pass this
 * library's `SIGPIPE` disposition to a program that never linked it. Every
 * disposition this library installs is restored by `cred_txn_end`, and
 * `SIGPIPE` last of all, once there is nothing left to report.
 *
 * An acquire that cannot record the holder's pid tries to remove the lock,
 * reports a removal that fails, and returns -1.
 *
 * A second `cred_txn_begin` in the process that holds the transaction is a
 * programming error, not contention, so it returns -1 at once without
 * touching the file. The same call in a child that inherited the state is
 * neither: the child holds nothing, so it drops what it inherited and goes on
 * to acquire for itself, which finds the parent's lock and waits.
 */
int cred_txn_begin(const char *prog);

/**
 * As `cred_txn_begin`, with the lock's path and the number of retries as
 * arguments.
 *
 * This form exists for the host test, which must neither touch `/etc` nor
 * wait ten seconds to watch an acquire fail. The shipped programs call
 * `cred_txn_begin`, whose path is compiled in and never read from the
 * environment, because this library is linked into a setuid-root program. A
 * `lock_path` that is NULL or longer than 127 bytes returns -1.
 */
int cred_txn_begin_at(const char *prog, const char *lock_path, int tries);

/**
 * End the transaction: remove the temporary file a failed replacement left,
 * unlink the lock, close it, and restore the dispositions `cred_txn_begin`
 * installed. Returns 0 when the lock is gone, `CRED_UNFLUSHED` when it is
 * gone from the file system but the removal did not reach the disk, and -1
 * when this process holds no transaction, a temporary file could not be
 * removed, or the lock could not be read or removed.
 *
 * The lock is unlinked only if it records this process, for the second
 * reason a lock is never broken: the name is not proof of the file. A release
 * that finds another writer's lock at the name leaves it, reports nothing,
 * and returns 0, since nothing of this transaction is left there; a lock
 * replaced by hand between that check and the unlink is still removed (see
 * the file header). A lock that could not be read or removed is still a file
 * in `/etc`, and the next writer's ten-second wait and failure, so the -1
 * must be reported, not a clean finish. The release needs no free descriptor,
 * so a caller that has filled its descriptor table still releases: the
 * transaction keeps the lock's descriptor, and the release closes it and
 * opens the lock again in that slot.
 *
 * `CRED_UNFLUSHED` means what it means for a replacement: the running system
 * is in the state the caller asked for, and the disk may not be. The removal
 * of the lock is the last write a transaction makes, and often the only one,
 * so a writer that reported a clean finish after a failed flush could leave,
 * after a power cut, a lock that names a process that died with it. A caller
 * reports it as a failure to finish cleanly and does not retry: the lock is
 * already gone from the running system.
 *
 * There is no separate abort. Ending the transaction removes a temporary file
 * a replacement left, and returns -1, with a diagnostic that names the file
 * to clean up by hand, if that removal fails. A replacement already published
 * as `CRED_UNFLUSHED` is live, and ending the transaction does not undo it.
 */
int cred_txn_end(void);

/**
 * Replace `path` with the `len` bytes at `text`; returns `CRED_PUBLISHED`,
 * `CRED_UNFLUSHED` or `CRED_UNCHANGED`.
 *
 * The whole replacement idiom, and the one to use for any file the system
 * authenticates against. It changes nothing unless this process holds the
 * transaction. It then checks the lock, unlinks any leftover temporary file,
 * creates `<path>.tmp.<pid>` with `O_EXCL`, writes the text, takes the owner
 * and then the mode from the file being replaced, flushes, checks the lock
 * again, renames, and flushes once more through the same descriptor before
 * closing it. Every step is checked.
 *
 * Five details matter. The temporary name has the writer's pid, so it belongs
 * to one process and no other: with a fixed `<path>.tmp` a writer whose lock
 * had been removed could unlink and recreate the file a successor already had
 * open, and the successor's rename by name would then publish it. The lock is
 * checked before that unlink for the same reason, and again between the first
 * flush and the rename, since a publish writes the shared database and needs
 * the lock. The descriptor stays open across the rename so that there is a
 * descriptor to flush through once the file is published: `fsync` needs a
 * live descriptor, and `sync(2)` would not do, since it returns 0 even when a
 * write-back fails and would lose the `EIO` this call exists to report. The
 * owner is set before the mode, because a `chown` clears the set-id bits. And
 * the temporary file's mode becomes the result's, because a rename changes
 * only the name and the parent, so copying the original's mode and owner is
 * what keeps a file that must be unreadable from becoming readable. The
 * library makes no hard-link backup.
 *
 * The two lock checks do not bind the rename that follows the second to the
 * file that was checked; the file header says what that leaves.
 *
 * The file being replaced must therefore exist: its mode and owner become the
 * result's, and there is no other correct source for them. Creating a
 * credential file is the image's job, not this idiom's. The copy is exact and
 * has no floor: a database staged 0644, or with a set-id bit, is reproduced
 * as it is, and every later replacement reproduces it again. The attributes
 * are read by name at the start and published by name at the end, so a
 * `chmod` by root between the two is reverted to what was read; the lock
 * excludes other writers that use this library, not an operator at the
 * console.
 *
 * The return is what the caller must report. A failure before the rename
 * tries to remove the unpublished file, reports a removal that fails (that
 * file then needs cleaning up by hand), leaves the target unchanged and
 * returns `CRED_UNCHANGED`, the one return a caller may report as
 * "unchanged". A failure of the second flush or of the close returns
 * `CRED_UNFLUSHED`: the file system already has the new file, the disk may
 * still have the old one, and the caller must report that the change is live
 * but may not be durable, since the account now takes the new password
 * whatever the disk has. Both flushes do nothing when the root is the KRFFS
 * image in RAM, so the durability claim belongs to a disk-backed image.
 */
int cred_rewrite(const char *path, const char *text, size_t len);

#endif /* _PWAUTH_H_ */
