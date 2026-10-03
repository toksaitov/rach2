/**
 * pwauth: the credential library's implementation.
 *
 * The contract is in `pwauth.h`: who takes the transaction, who never does,
 * why the lock is reported and never broken, and what the replacement idiom
 * promises.
 *
 * The transaction and the terminal modes are file-scope state, not a handle
 * the caller passes around, because the signal handlers that must undo a
 * half-finished write cannot be handed one. A process holds at most one
 * transaction and prompts on at most one terminal, so there is nothing a
 * handle would distinguish.
 */

#define _GNU_SOURCE /* crypt, getspnam, getrandom */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <crypt.h>
#include <shadow.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "pwauth.h"

/* The replacement's name: `/etc/shadow` is rewritten through
 * `/etc/shadow.tmp.<pid>` in the same directory, since a rename is atomic only
 * within one file system. The pid is what makes the name one writer's own, so
 * no two of them can destroy each other's work in flight. */
#define CRED_TMP_FORMAT "%s.tmp.%d"

/* Signals 1..64 are deliverable on this kernel, so the saved dispositions are
 * indexed by number, not packed into a list. */
#define PWAUTH_SIG_MAX 65

/* What an inspection of the lock file found: this process recorded in it, some
 * other writer's lock (or none at all) at the name, or nothing that can be told
 * apart, which every caller must treat as a refusal, not as consent. */
#define CRED_LOCK_OURS    1
#define CRED_LOCK_OTHER   0
#define CRED_LOCK_UNKNOWN (-1)

/* The transaction. `Txn_held` and `Txn_owner` together are "this process holds
 * the lock": a fork copies the state but not the holding, so the recorded pid
 * is what tells a holder from an heir. `Txn_tmp` names the temporary file a
 * `cred_rewrite` currently has open, so a signal death can remove it, and is
 * empty at every other moment. A handler reads all three, and only the first
 * can be `sig_atomic_t`: the pid is one word, written once, but the name is a
 * string that no single store makes whole, and a death served partway through
 * one would read a truncated path and unlink it. That is why the deaths are
 * blocked wherever this state is published, not merely written in a
 * careful order; `pwauth_death_set` lists every stretch they are blocked
 * across, publication and otherwise. */
static volatile sig_atomic_t Txn_held;
static volatile sig_atomic_t Exit_under_way;
static pid_t Txn_owner;
static int Txn_fd = -1;
static char Txn_lock[128];
static char Txn_tmp[256];
static int Txn_atexit_done;
static struct sigaction Txn_saved_death[PWAUTH_SIG_MAX];
static struct sigaction Txn_saved_pipe;

/* The terminal a password prompt took echo away from: `Tty_saved` is the modes
 * to put back, `Tty_quiet` the echo-off copy a resume re-applies, and
 * `Tty_resumed` says a stop has been served, so the line must start over. */
static struct termios Tty_saved;
static struct termios Tty_quiet;
static volatile sig_atomic_t Tty_have_modes;
static volatile sig_atomic_t Tty_echo_off;
static volatile sig_atomic_t Tty_resumed;
static volatile sig_atomic_t Tty_failed;

/* The prompt in flight, kept because a resume has to reprint it. */
static const char *volatile Prompt = "";
static volatile sig_atomic_t Prompt_len;

/* The crypt(3) alphabet, in its conventional order. */
static const char Salt_alphabet[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/**
 * Write the prompt in flight to standard error.
 *
 * A prompt that cannot be printed is not a reason to refuse the read, and this
 * runs from a signal handler on a resume, where there is nobody to report to.
 */
static void pwauth_prompt(void)
{
    const char *text = Prompt;

    if (text && Prompt_len > 0) {
        (void)write(2, text, (size_t)Prompt_len);
    }
}

/**
 * True when this process is the one that took the transaction.
 *
 * A `fork` copies the library's state, so a child inherits `Txn_held` while
 * holding nothing: the lock file names its parent, and an heir that leaves
 * through `exit` or dies of a signal would run the release and take a live
 * writer's lock with it. The pid recorded at the acquire is what separates the
 * two, and every path that would touch the lock asks this first.
 */
static int cred_txn_mine(void)
{
    return Txn_held && Txn_owner == getpid();
}

/**
 * Put the terminal's saved modes back, if this library took echo away.
 *
 * Safe from a signal handler and safe to call twice: `tcsetattr` is
 * async-signal-safe, and putting the same saved modes back twice is the same as
 * putting them back once.
 *
 * The flag is cleared after the call, not before. A handler runs with only its
 * own signal blocked, so a second death can arrive between the two statements,
 * and it enters here again on its way out. Clearing first, that second death
 * would find the flag already down, skip the restore, and leave through `exit`
 * or a re-raise, and the restore it interrupted would never resume: the
 * terminal would keep the echo-off modes with no program left to undo them.
 * Clearing after, the second death finds the flag still up and does the
 * restore itself, which costs one repeated `tcsetattr` and cannot lose it.
 */
static void pwauth_tty_restore(void)
{
    if (Tty_echo_off) {
        tcsetattr(0, TCSANOW, &Tty_saved);
        Tty_echo_off = 0;
    }
}

/**
 * Install `handler` for `sig`, saving the old disposition in `saved` unless it
 * is NULL. Returns what `sigaction` returns.
 *
 * No `SA_RESTART`: this kernel stores the flag and never restarts an
 * interrupted call, so asking for it would suggest a restart that does not
 * happen.
 */
static int pwauth_install(int sig, void (*handler)(int), struct sigaction *saved)
{
    struct sigaction act;

    memset(&act, 0, sizeof(act));
    act.sa_handler = handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    return sigaction(sig, &act, saved);
}

/**
 * Fill `set` with the signals that stop a terminal read and start it again.
 *
 * They are blocked together while the prompt's state is taken down, because the
 * teardown is a sequence a resume must not run inside: the modes are put back
 * first and the handlers are removed after, so a `SIGCONT` served in between
 * finds a handler that re-applies echo-off, and the read returns with the
 * terminal silenced and nothing left installed to undo it. A process in the
 * same session may send that signal, so the window can be reached
 * deliberately. Blocking a stop as well keeps the teardown from being
 * suspended halfway, which is the state the continue would then act on.
 */
static void pwauth_stop_signals(sigset_t *set)
{
    sigemptyset(set);
    sigaddset(set, SIGTSTP);
    sigaddset(set, SIGCONT);
    sigaddset(set, SIGTTIN);
    sigaddset(set, SIGTTOU);
}

/**
 * True when `sig` is one this library must handle to die cleanly.
 *
 * The set is derived from what the kernel does, not from the four names
 * a writer expects. `ksig_default_action` terminates a process for every signal
 * except four it ignores and four it stops for, and `cred_may_signal` lets a
 * process signal one whose real uid matches its own, which a setuid program
 * keeps from whoever ran it. So the user who started `su` or `passwd` can send
 * it `SIGUSR1`, `SIGALRM`, or any of a few dozen others, and each of them would
 * otherwise end the writer where no handler runs: a password prompt with echo
 * off and no program left to restore it, or a transaction with its lock still
 * in `/etc`. Handling only the four that arrive by habit would leave the rest
 * as a way for any user to strand the credential database.
 *
 * Five groups are left out. `SIGKILL` and `SIGSTOP` cannot be caught at all,
 * which is the documented residue: a writer killed that way leaves a lock the
 * next writer reports. The kernel's own ignore and stop sets are not deaths,
 * and the stop family is the prompt's business, which suspends and resumes
 * instead of tearing down. `SIGPIPE` is caught by `pwauth_pipe_handler`, so
 * the write that raised it fails instead. Signals 32, 33 and 34 cannot be
 * handled: this libc reserves them and refuses them from both `sigaddset` and
 * `sigaction`, so no program linked against it can block or catch them, here
 * or on Linux, and the kernel still terminates for them by default. They are
 * listed so that the default branch does not try and silently fail, and
 * `pwauth.h` states them beside the uncatchable pair, since to a writer the
 * consequence is the same.
 *
 * The fault-numbered signals are handled, although elsewhere one avoids
 * catching `SIGSEGV`, since a handler would run `exit` on a process that has
 * just faulted, cleaning up over undefined state. On this kernel a CPU
 * exception never reaches a disposition at all: `_exception_handler`
 * (`rach2/sys/arch/x86/isr.c`) and the page-fault path
 * (`rach2/sys/mm/paging.c`) both call `terminate_current_process`, which
 * zombifies the process directly without consulting `sigacts`. So the only `SIGSEGV` a handler here can ever
 * see is one that was sent, which is an ordinary death from an ordinary
 * `kill`, and one the real uid that launched a setuid writer is allowed to
 * send. Handling them changes nothing about a real fault and closes five more
 * ways to strand the lock. `SIGABRT` and `SIGSYS` are handled for the same
 * reason: the kernel raises neither. A transaction therefore turns an `abort`
 * into a clean exit; with no transaction held the handler restores the
 * default and re-raises, so an `abort` remains one.
 */
static int pwauth_is_death(int sig)
{
    switch (sig) {
    case SIGKILL:
    case SIGSTOP:
        return 0; /* cannot be caught */
    case SIGCHLD:
    case SIGURG:
    case SIGWINCH:
    case SIGCONT:
        return 0; /* the kernel's default is to ignore */
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU:
        return 0; /* the kernel's default is to stop */
    case SIGPIPE:
        return 0; /* handled, but as a failed write, not a death */
    case 32:
    case 33:
    case 34:
        return 0; /* this libc keeps them and refuses to install here */
    default:
        return sig >= 1 && sig < PWAUTH_SIG_MAX;
    }
}

/**
 * Fill `set` with the signals a held transaction handles as a death.
 *
 * They are blocked wherever the transaction's state is published, because a
 * handler acts on that state and no publication here is one store. The lock
 * file becomes this process's when the exclusive create returns, but is not
 * recorded as this process's until several calls later; the temporary file's
 * name reaches `Txn_tmp` a byte at a time. A death served inside either window
 * would act on state describing neither the old situation nor the new one, and
 * in the second window a partial copy of the temporary name can be the name of
 * the file being replaced. Blocking does not lose the signal: it is delivered
 * when the mask goes back, by which time the state and the handler agree.
 *
 * The same set is blocked across four more stretches: the release, whose
 * unlink and flush are one step (`cred_lock_release`); the teardown in
 * `cred_txn_end`; an heir's teardown of what it inherited, in
 * `cred_txn_begin_at`; and the whole of `cred_txn_atexit`, which runs inside
 * `exit` and must not be re-entered through one.
 */
static void pwauth_death_set(sigset_t *set)
{
    int sig;

    sigemptyset(set);
    for (sig = 1; sig < PWAUTH_SIG_MAX; sig++) {
        if (pwauth_is_death(sig)) {
            sigaddset(set, sig);
        }
    }
}

/**
 * Take `SIGPIPE` and do nothing, so the write that raised it fails instead.
 *
 * A handler, not `SIG_IGN`, though the two are indistinguishable to the
 * write itself (this kernel posts the signal and returns `-EPIPE` either way),
 * because an exec resets a caught disposition to the default and keeps an
 * ignored one in the new image. A child forked under a transaction inherits
 * this library's state and has none of it; what it must not also inherit is a
 * `SIGPIPE` that stays ignored into whatever program it goes on to run, since
 * that is this library quietly changing the signal semantics of an image that
 * never linked it.
 */
static void pwauth_pipe_handler(int sig)
{
    (void)sig;
}

/**
 * Attempt to remove the unpublished file. Return -1 on removal failure and
 * keep its name for the caller's diagnostic and a later retry. Use only
 * async-signal-safe calls.
 */
static int cred_drop_temp(void)
{
    if (Txn_tmp[0] != '\0') {
        if (unlink(Txn_tmp) != 0 && errno != ENOENT) {
            return -1;
        }
        Txn_tmp[0] = '\0';
    }
    return 0;
}

/**
 * Report that the temporary file `path` could not be removed. Callers print it
 * with the death signals unblocked, so a blocked write stays interruptible.
 */
static void cred_report_temp(const char *path)
{
    fprintf(stderr, "pwauth: %s: cannot remove temporary file; remove it manually\n", path);
}

/**
 * Disable echo while this process group owns the terminal. Return 1 when the
 * modes were applied, 0 when another process group owns the terminal, and -1,
 * with `Tty_failed` set, when setting the modes fails.
 *
 * The foreground check and the mode change run with the job-control signals
 * blocked, because the check goes stale once a stop is served. A reader that
 * checked while it was the foreground group, took a `SIGTSTP` before it could
 * act, and was continued in the background by a shell that had meanwhile
 * reclaimed the terminal would silence a terminal it no longer owns. Blocking
 * the signals defers the stop until the modes are in place, where the stop
 * handler puts them back before it stops.
 *
 * `SIGSTOP` cannot be blocked, so that one window stays open, and it is the
 * residue `pwauth.h` already states for stops that cannot be caught.
 */
static int pwauth_echo_off(void)
{
    sigset_t stops, previous;
    pid_t owner;
    int taken = 0, was_off;

    pwauth_stop_signals(&stops);
    sigprocmask(SIG_BLOCK, &stops, &previous);
    owner = tcgetpgrp(0);
    if (owner <= 0 || owner == getpgrp()) {
        was_off = Tty_echo_off;
        Tty_echo_off = 1;
        if (tcsetattr(0, TCSAFLUSH, &Tty_quiet) != 0) {
            Tty_echo_off = was_off;
            Tty_failed = 1;
            taken = -1;
        } else {
            taken = 1;
        }
    }
    sigprocmask(SIG_SETMASK, &previous, NULL);

    return taken;
}

/**
 * Handle a signal that would otherwise kill the process outright.
 *
 * With a transaction held it leaves through `exit`, so the release
 * registered with `atexit` drops the lock; releasing it here as well would
 * be two places that must agree on what "held" means. `exit` is not on the
 * async-signal-safe list and is used anyway, because the registered release
 * must run, and the alternative is a lock file outliving its holder.
 *
 * `exit` is entered at most once. A handler runs with just its own signal
 * blocked, so a second death can arrive while the first one's `exit` is
 * walking the registered functions. That walk steps its cursor past a
 * function before calling it, so a second `exit` entered from inside that
 * call resumes below the one in flight: a death landing after
 * `cred_txn_atexit` has been selected and before it has been entered would
 * skip the release and leave the lock in `/etc`. A death that finds
 * `Exit_under_way` set returns instead, and the exit already running
 * finishes. The flag is never cleared, because the `exit` that set it does
 * not return. With no transaction it restores the default disposition and
 * re-raises, so the process dies of the signal it was sent and a waiter sees
 * that, not an invented status.
 *
 * Either way the terminal gets its modes back first, since otherwise a `^C`
 * at a password prompt leaves a console with echo off and no program left to
 * fix it. `Tty_have_modes` is cleared before the restore, because `SIGCONT`
 * is not blocked here and its handler re-applies the echo-off modes while that
 * flag is set: after the store a resume changes no modes, and before it the
 * resume runs first and the restore that follows still puts the terminal back.
 */
static void pwauth_death_handler(int sig)
{
    Tty_have_modes = 0;
    pwauth_tty_restore();
    if (cred_txn_mine()) {
        if (Exit_under_way) {
            return;
        }
        Exit_under_way = 1;
        exit(1);
    }
    pwauth_install(sig, SIG_DFL, NULL);
    kill(getpid(), sig);
}

/**
 * Restore the terminal and stop, on the `^Z` the line discipline maps to
 * `SIGTSTP`.
 *
 * The disposition is reset to the default and the signal re-sent, rather than
 * the handler simply returning, because a caught `SIGTSTP` does not stop the
 * process. The re-send does not take effect inside the handler: delivery
 * blocks the handler's own signal for the run, so the stop is taken when
 * `sigreturn` restores the mask. That is why the resume is a second handler
 * and not the tail of this one; anything written after the re-send here would
 * run before the stop, not after the continue.
 *
 * The flag is raised here as well as on the continue, because the stop can
 * be served after a byte has already been read: the line discipline flushes
 * what is queued when it recognizes the suspend character, but a byte that
 * arrived behind it in the same burst is queued normally and the read returns
 * it before the signal is delivered. The reader drops the line it has whenever
 * this is set, so such a byte cannot become a password byte.
 */
static void pwauth_tstp_handler(int sig)
{
    Tty_resumed = 1;
    pwauth_tty_restore();
    pwauth_install(sig, SIG_DFL, NULL);
    kill(getpid(), sig);
}

/**
 * Re-arm the prompt on the continue that follows a stop.
 *
 * It re-installs the stop handler that the stop itself reset to the default,
 * re-applies the echo-off modes while discarding the input typed meanwhile (a
 * character the terminal echoed at a stopped process must not become a password
 * byte), and reprints the prompt, since the read is about to start over. It
 * also covers `SIGSTOP`, which cannot be caught, so the resume is where the
 * line is dropped whichever stop was taken. Everything reached from here is
 * async-signal-safe: `sigaction`, `sigprocmask`, `tcgetpgrp`, `getpgrp`,
 * `tcsetattr`, `write`, and the signal-set helpers.
 *
 * A continue does not by itself return the terminal: a shell's `bg` is exactly
 * this signal without it. Taking echo away and reprinting from the background
 * would silence a terminal that now belongs to the shell, and this kernel
 * raises no `SIGTTOU` for `TCSETS` to stop it. So the terminal work is skipped
 * unless this process is still the foreground group; the read that follows
 * stops on `SIGTTIN` instead, and the `fg` that continues it runs this handler
 * again with the terminal back.
 */
static void pwauth_cont_handler(int sig)
{
    (void)sig;
    pwauth_install(SIGTSTP, pwauth_tstp_handler, NULL);
    Tty_resumed = 1;
    if (Tty_have_modes && pwauth_echo_off() <= 0) {
        return;
    }
    pwauth_prompt();
}

/**
 * Compare `password` against the `stored` hash. Returns 1 on a match, 0
 * otherwise.
 */
int pwauth_check(const char *stored, const char *password)
{
    char *hash;

    if (!stored || !password || stored[0] != '$') {
        return 0;
    }
    hash = crypt(password, stored);
    return hash != NULL && strcmp(hash, stored) == 0;
}

/**
 * Resolve `user` in the shadow database and check `password` against it.
 * Returns 1 on a match, 0 for a wrong password, an absent account, or a locked
 * one.
 */
int pwauth_verify(const char *user, const char *password)
{
    struct spwd *sp;

    if (!user) {
        return 0;
    }
    sp = getspnam(user);
    if (!sp) {
        return 0;
    }
    return pwauth_check(sp->sp_pwdp, password);
}

/**
 * Save terminal modes and disable echo only for the foreground process group.
 * A background reader keeps the saved modes for its next foreground resume.
 * ENOTTY permits nonterminal input; any other mode-read error refuses input.
 * Return -1 if reading or setting the modes fails, otherwise 0.
 */
static int pwauth_prepare_tty(void)
{
    if (tcgetattr(0, &Tty_saved) != 0) {
        return errno == ENOTTY ? 0 : -1;
    }
    Tty_quiet = Tty_saved;
    Tty_quiet.c_lflag &= ~(tcflag_t)ECHO;
    Tty_have_modes = 1;
    return pwauth_echo_off() < 0 ? -1 : 0;
}

/** Refuse a byte if disabling echo failed before or during the read. */
static ssize_t pwauth_read_byte(char *c)
{
    ssize_t n = -1;

    if (!Tty_failed) {
        n = read(0, c, 1);
    }
    if (Tty_failed) {
        errno = EIO;
        return -1;
    }
    return n;
}

/**
 * Print `prompt` and read one line from standard input into `buf`. Returns 0 on
 * a complete line, -1 on empty EOF, a read error, a terminal-mode error, or a
 * `size` below 2. Reject NUL bytes and input longer than the smaller of the
 * caller's buffer and PWAUTH_MAX_PASSWORD. A failed echo change on resume
 * refuses the line.
 *
 * A refused line (too long, holding a NUL, or read after a failed mode change)
 * leaves `buf` zeroed and is never cut to a prefix: the prefix would be a
 * secret the user never typed, and the change would be reported as a success.
 * A NUL is refused because `strcmp` and `crypt` stop there, so the hash would
 * cover only the bytes before it. After a read error `buf` holds the bytes
 * read so far, terminated.
 *
 * The read loops on `-EINTR` because this kernel stores `SA_RESTART` and never
 * restarts a call, so every signal that reaches a blocked read returns it and
 * a stop-and-continue must not read as end of input. The line is dropped and
 * started over on a resume and only there, not on every `-EINTR`: the resume
 * is the moment the terminal was flushed and the prompt reprinted, and it is
 * also the one moment a byte already in `buf` may have come from before the
 * stop. A signal that is not a stop leaves what the user has typed alone. The
 * resume is checked before each read, where starting over loses nothing, since
 * a stop served while the modes were set or the prompt written leaves no read
 * outstanding; and after each read, where the byte in hand may have been typed
 * before the stop and is dropped with the line.
 *
 * `SIGPIPE` is caught for the length of the call, because the prompt is written
 * to file descriptor 2 after echo has been taken away. A caller whose standard
 * error is a pipe nobody reads would otherwise be killed by the default action
 * at exactly that point, leaving the terminal silenced with no program left to
 * restore it. The write is allowed to fail instead, which is what the ignored
 * result of the prompt already says it may do.
 *
 * The teardown runs with the stop and continue signals blocked: the modes go
 * back before the handlers come off, so a `SIGCONT` arriving in between would
 * re-apply echo-off through a handler about to be removed and the call would
 * return with the terminal silenced for good.
 */
int pwauth_read_password(const char *prompt, char *buf, size_t size)
{
    struct sigaction saved_tstp, saved_cont, saved_pipe, saved_death[PWAUTH_SIG_MAX];
    sigset_t stops, saved_mask;
    size_t len = 0;
    size_t room = size - 1 < (size_t)PWAUTH_MAX_PASSWORD ? size - 1 : (size_t)PWAUTH_MAX_PASSWORD;
    int deaths = 0;
    int over = 0;
    int bad = 0;
    int rc = -1;
    int i;

    if (!buf || size < 2) {
        return -1;
    }
    buf[0] = '\0';
    Prompt = prompt ? prompt : "";
    Prompt_len = (sig_atomic_t)strlen(prompt ? prompt : "");
    Tty_resumed = 0;
    Tty_failed = 0;

    pwauth_install(SIGPIPE, pwauth_pipe_handler, &saved_pipe);
    pwauth_install(SIGTSTP, pwauth_tstp_handler, &saved_tstp);
    pwauth_install(SIGCONT, pwauth_cont_handler, &saved_cont);
    if (!cred_txn_mine()) {
        for (i = 1; i < PWAUTH_SIG_MAX; i++) {
            if (pwauth_is_death(i)) {
                pwauth_install(i, pwauth_death_handler, &saved_death[i]);
            }
        }
        deaths = 1;
    }

    if (pwauth_prepare_tty() != 0) {
        goto done;
    }

    pwauth_prompt();

    for (;;) {
        char c;
        ssize_t n;

        if (Tty_resumed) {
            Tty_resumed = 0;
            len = 0;
            over = 0;
            bad = 0;
        }
        n = pwauth_read_byte(&c);

        if (Tty_resumed) {
            Tty_resumed = 0;
            len = 0;
            over = 0;
            bad = 0;
            continue;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            rc = (len > 0) ? 0 : -1;
            break;
        }
        if (c == '\n' || c == '\r') {
            rc = 0;
            break;
        }
        if (c == '\0') {
            bad = 1;
        } else if (len < room) {
            buf[len++] = c;
        } else {
            over = 1;
        }
    }
done:
    buf[len] = '\0';

    pwauth_stop_signals(&stops);
    sigprocmask(SIG_BLOCK, &stops, &saved_mask);
    if (Tty_echo_off) {
        pwauth_tty_restore();
        (void)write(2, "\n", 1);
    }
    Tty_have_modes = 0;
    sigaction(SIGTSTP, &saved_tstp, NULL);
    sigaction(SIGCONT, &saved_cont, NULL);
    sigprocmask(SIG_SETMASK, &saved_mask, NULL);
    if (deaths) {
        for (i = 1; i < PWAUTH_SIG_MAX; i++) {
            if (pwauth_is_death(i)) {
                sigaction(i, &saved_death[i], NULL);
            }
        }
    }
    sigaction(SIGPIPE, &saved_pipe, NULL);
    Prompt = "";
    Prompt_len = 0;

    if (Tty_failed) {
        memset(buf, 0, size);
        rc = -1;
    } else if (bad) {
        memset(buf, 0, size);
        fprintf(stderr, "pwauth: the entry holds a NUL byte, which no reader here can store\n");
        rc = -1;
    } else if (over) {
        memset(buf, 0, size);
        fprintf(stderr, "pwauth: the entry is longer than %u characters\n", (unsigned)room);
        rc = -1;
    }

    return rc;
}

/**
 * Map `n` bytes onto `n` characters of the crypt(3) alphabet, terminating
 * `out`.
 */
void pwauth_salt_map(const unsigned char *bytes, size_t n, char *out)
{
    size_t i;

    if (!bytes || !out) {
        return;
    }
    for (i = 0; i < n; i++) {
        out[i] = Salt_alphabet[bytes[i] & 0x3F];
    }
    out[n] = '\0';
}

/**
 * Draw `PWAUTH_SALT_LEN` characters of trusted salt into `out`. Returns 0, or
 * -1 with no byte of `out` written.
 */
int pwauth_mint_salt(char *out, size_t size)
{
    unsigned char raw[PWAUTH_SALT_LEN];
    size_t got = 0;

    if (!out || size < PWAUTH_SALT_LEN + 1) {
        fprintf(stderr, "pwauth: salt buffer too small\n");
        return -1;
    }
    while (got < sizeof(raw)) {
        ssize_t n = getrandom(raw + got, sizeof(raw) - got, 0);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            fprintf(stderr, "pwauth: no trusted entropy\n");
            return -1;
        }
        got += (size_t)n;
    }
    pwauth_salt_map(raw, sizeof(raw), out);
    return 0;
}

/**
 * Read one positive decimal PID followed only by spaces, tabs, carriage
 * returns, or newlines. Return 1 on success, 0 for malformed contents, and -1
 * on a read error. Reject more than nine digits.
 */
static int cred_parse_pid(int fd, long *out)
{
    unsigned char buf[32];
    long value = 0;
    int digits = 0, suffix = 0;
    ssize_t n, i;

    for (;;) {
        n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        for (i = 0; i < n; i++) {
            if (!suffix && buf[i] >= '0' && buf[i] <= '9') {
                if (++digits > 9) {
                    return 0;
                }
                value = value * 10 + (buf[i] - '0');
            } else if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n') {
                suffix = 1;
            } else {
                return 0;
            }
        }
    }
    if (digits == 0 || value <= 0) {
        return 0;
    }
    *out = value;
    return 1;
}

/**
 * Read the holder pid recorded in the lock file `path` into `out`, opening it.
 * Returns what `cred_parse_pid` returns, and -1 for a file that cannot be
 * opened at all; `errno` survives the `close`.
 *
 * The release does not use this form: it keeps its descriptor open from the
 * read through the unlink, so that no `close` falls between the two, and
 * flushes the removal through it afterwards.
 */
static int cred_read_pid(const char *path, long *out)
{
    int found, failure;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        return -1;
    }
    found = cred_parse_pid(fd, out);
    failure = errno;
    close(fd);
    errno = failure;

    return found;
}

/**
 * What the file at `Txn_lock` says about this transaction: `CRED_LOCK_OURS`,
 * `CRED_LOCK_OTHER` for another writer's lock or none at all, or
 * `CRED_LOCK_UNKNOWN` when it could not be read.
 *
 * The library checks the recorded PID before publishing. KRFFS inode numbers
 * are name hashes; ext2 provides inode numbers. Neither a PID check nor an
 * inode comparison makes a later pathname operation atomic with the check.
 *
 * The third answer is why this is not a predicate. This opens a file, so it
 * fails when there is no descriptor to open it with, and a caller told only
 * "not ours" would then treat an exhausted descriptor table as a decision.
 */
static int cred_lock_state(void)
{
    long pid = 0;
    int found = cred_read_pid(Txn_lock, &pid);

    if (found < 0) {
        return (errno == ENOENT) ? CRED_LOCK_OTHER : CRED_LOCK_UNKNOWN;
    }

    return (found == 1 && pid == (long)getpid()) ? CRED_LOCK_OURS : CRED_LOCK_OTHER;
}

/**
 * Report why an acquire gave up, naming the program `prog` an operator ran.
 *
 * The recorded pid decides which lines are printed and nothing else: the file
 * is still there afterwards, byte for byte, whatever it said. A pid that is
 * gone, a file that records none, and a file that cannot be read at all are all
 * a lock to remove by hand, and the third says so in its own words rather than
 * claiming to have read an empty file.
 *
 * A pid that answers `kill(pid, 0)` is a holder that may be running, blocked at
 * its own password prompt, or stopped, and nothing is lost by waiting for it.
 * It is reported with the recovery line all the same, because a pid does not
 * identify a process here across a reboot: `next_pid` restarts at every boot
 * and the writers run early, so a lock left on a persistent image by a killed
 * writer records a small pid that some unrelated process now has, and an
 * operator told only to wait would wait forever. The line therefore says what
 * has to be true before removing it. (`EPERM` counts as an answer for a
 * caller that is not root; every writer in the image runs as root, setuid or
 * by refusing anyone else, and never sees it.)
 */
static void cred_report_busy(const char *prog, const char *path)
{
    long pid = 0;
    int found = cred_read_pid(path, &pid);

    if (found == 1) {
        if (kill((pid_t)pid, 0) == 0 || errno == EPERM) {
            fprintf(stderr, "%s: credential database busy (pid %ld)\n", prog, pid);
            fprintf(stderr,
                    "%s: a reboot reuses pids; if pid %ld is not a writer, "
                    "remove %s as root\n",
                    prog, pid, path);
            return;
        }
        fprintf(stderr, "%s: stale lock %s (pid %ld is gone)\n", prog, path, pid);
    } else if (found == 0) {
        fprintf(stderr, "%s: stale lock %s (no holder recorded)\n", prog, path);
    } else {
        fprintf(stderr, "%s: lock %s cannot be read\n", prog, path);
    }
    fprintf(stderr, "%s: remove it as root and retry\n", prog);
}

/**
 * Remove the lock, if it still records this process, and push the removal to
 * the disk. Returns 0 when nothing of this transaction's is left at the name,
 * `CRED_UNFLUSHED` when the lock is gone from the file system but its removal
 * did not reach the disk, and -1 when a lock may have been left behind. On a
 * nonzero return `*report` is set to the diagnostic, which the caller prints
 * once the death signals are unblocked; otherwise it is NULL.
 *
 * It opens no descriptor it does not first give back. The transaction's own
 * write descriptor on the lock is closed at the top and the read that follows
 * takes the slot it freed, so a writer that filled its descriptor table can
 * still release. An open that fails for any reason but `ENOENT` is a failure
 * and never "some other writer's lock", which is the one answer that makes a
 * release decline to unlink: a process holding the lock in its last free slot
 * would strand it for everyone and report success.
 *
 * The failure is returned, not swallowed, because "the lock is gone" and "the
 * lock could not be looked at" are different states, and only the first is a
 * clean release; a caller told 0 for both would report a finished transaction
 * over a lock file still in `/etc`.
 *
 * The read and the unlink are adjacent on purpose. The name is not proof of the
 * file (see `cred_lock_state`), so between deciding the lock is ours and
 * removing it there is a window in which recovery could remove it and a
 * successor create its own, and this unlink would then take the successor's.
 * Nothing in this kernel closes that window: an unlink is by name, and there is
 * no `funlink`. No syscall is put between the two. The holder is compared
 * against `Txn_owner`, not a fresh `getpid`, which is a syscall in this libc
 * and would sit in exactly that gap; the recorded pid is the same number,
 * since every caller of this function has already established it through
 * `cred_txn_mine`. What is left is the scheduler alone: the kernel may still
 * preempt between the read's return and the unlink's entry. `pwauth.h` states
 * the residue.
 *
 * The flush goes through the descriptor the check left open, which still
 * refers to the file after the unlink. Without it the release is the one step
 * of a writer's work that stays in the page cache: the credentials reach the
 * platter and the lock that guarded them does not, so a power cut after a
 * clean run hands the next writer a lock recording a process that died with
 * the boot. A flush that fails is `CRED_UNFLUSHED` and not a printed line
 * alone, since this is the only flush a transaction that ends without a
 * replacement makes.
 *
 * The whole of it runs with the deaths blocked. A death served between the
 * unlink and the flush leaves through `exit`, and the registered release that
 * runs then finds the lock already gone and returns 0, so the removal would be
 * flushed by nobody and reported to nobody.
 */
static int cred_lock_release(const char **report)
{
    sigset_t deaths, saved_mask;
    long pid = 0;
    int fd, found, rc = 0;

    *report = NULL;
    if (Txn_fd >= 0) {
        close(Txn_fd);
        Txn_fd = -1;
    }
    pwauth_death_set(&deaths);
    sigprocmask(SIG_BLOCK, &deaths, &saved_mask);
    fd = open(Txn_lock, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT) {
            *report = "cannot read the lock to release it";
            rc = -1;
        }
    } else {
        found = cred_parse_pid(fd, &pid);
        if (found != 1 || pid != (long)Txn_owner) {
            if (found < 0) {
                *report = "cannot read the lock to release it";
                rc = -1;
            }
        } else if (unlink(Txn_lock) != 0) {
            *report = "cannot remove the lock";
            rc = -1;
        } else if (fsync(fd) != 0) {
            *report = "the lock was removed but not flushed to disk";
            rc = CRED_UNFLUSHED;
        }
        close(fd);
    }
    sigprocmask(SIG_SETMASK, &saved_mask, NULL);

    return rc;
}

/**
 * Forget the transaction: close the lock descriptor, drop the state, and put
 * the death-signal dispositions back.
 *
 * The counterpart to what `cred_txn_begin_at` records and installs, except
 * `SIGPIPE`, which `cred_pipe_restore` puts back after the last diagnostic.
 * It is the one place that clears the state, so the release, the failed
 * acquire and a child disowning what it inherited cannot disagree about what
 * "not held" means. It removes no file: whether the lock at `Txn_lock` is this
 * process's to unlink is a separate question, and `cred_lock_release` is the
 * one that answers it.
 */
static void cred_txn_forget(void)
{
    int i;

    if (Txn_fd >= 0) {
        close(Txn_fd);
        Txn_fd = -1;
    }
    Txn_held = 0;
    Txn_owner = 0;
    Txn_tmp[0] = '\0';
    for (i = 1; i < PWAUTH_SIG_MAX; i++) {
        if (pwauth_is_death(i)) {
            sigaction(i, &Txn_saved_death[i], NULL);
        }
    }
}

/**
 * Put the caller's `SIGPIPE` disposition back, after the last diagnostic.
 *
 * Separate from `cred_txn_forget` because the order matters and is not the
 * obvious one. A report is a write, and the caller's disposition is normally
 * the default, which kills the process on a pipe whose reader has gone. Putting
 * it back before the last line is printed would let the report that says a lock
 * was left behind be the thing that stops the writer from returning that fact
 * to its caller. The deaths go back first, so the process is interruptible
 * while it reports; `SIGPIPE` goes back once there is nothing left to say.
 */
static void cred_pipe_restore(void)
{
    sigaction(SIGPIPE, &Txn_saved_pipe, NULL);
}

/**
 * Attempt to release the transaction on exit and report cleanup failures.
 *
 * A forked child discards the inherited state, closes the inherited lock
 * descriptor, and restores the saved death-signal and SIGPIPE dispositions
 * without unlinking its parent's lock.
 *
 * The death signals are blocked until the transaction state is discarded, and
 * SIGPIPE is restored after the diagnostics. Only dispositions still installed
 * when the call began are put back: a writer that ended its own transaction
 * restored them already, and a refused acquire installed none, so its saved
 * copy is a zeroed `struct sigaction` that would impose the default on the
 * rest of `exit`. `cred_exit_started` prevents a second exit from skipping
 * this callback. `_Exit`, `SIGKILL`, CPU faults, and delivery of
 * musl-reserved signals 32-34 can terminate the process without this cleanup.
 */
static void cred_txn_atexit(void)
{
    sigset_t deaths, saved_mask;
    const char *report = NULL;
    char temp[sizeof(Txn_tmp)] = "";
    int mine, installed;

    pwauth_death_set(&deaths);
    sigprocmask(SIG_BLOCK, &deaths, &saved_mask);
    mine = cred_txn_mine();
    installed = Txn_held;
    if (mine) {
        if (cred_drop_temp() != 0) {
            memcpy(temp, Txn_tmp, sizeof(temp));
        }
        (void)cred_lock_release(&report);
    }
    if (installed) {
        cred_txn_forget();
    }
    sigprocmask(SIG_SETMASK, &saved_mask, NULL);
    if (temp[0] != '\0') {
        cred_report_temp(temp);
    }
    if (report != NULL) {
        fprintf(stderr, "pwauth: %s: %s\n", Txn_lock, report);
    }
    if (installed) {
        cred_pipe_restore();
    }
}

/**
 * Note that an `exit` has begun, registered with `atexit` after the release so
 * that it runs before it.
 *
 * The death handler leaves through `exit`, and must not do so when one is
 * already under way. It can tell for an `exit` a death started, because it set
 * the flag itself; it cannot tell for an ordinary one, and the window matters:
 * the walk of registered functions steps its cursor past a function before
 * calling it, so a death arriving after the release has been selected and
 * before it has been entered would start a second `exit` that resumes below the
 * release and skips its cleanup attempt. Running first marks the exit before
 * the release, so the death handler returns to the exit already in progress.
 */
static void cred_exit_started(void)
{
    Exit_under_way = 1;
}

/**
 * Take the writer's exclusive lock at `lock_path`, retrying `tries` times.
 * Return 0 when it is held, -1 otherwise.
 *
 * Block the selected death signals and SIGPIPE from just before the create
 * until the PID and transaction state are recorded. SIGPIPE is blocked only
 * here: its default action is fatal, its handler goes in several calls after
 * the lock exists, and the user who ran a setuid writer may signal it, so a
 * SIGPIPE in that window would leave the lock in `/etc`. The retry sleeps with
 * the caller's signal mask restored.
 *
 * A failed PID write removes the lock and restores the mask before it
 * reports, since a diagnostic written to a full pipe blocks, and a writer
 * blocked there with the deaths masked could be ended only by `SIGKILL`,
 * which leaves the lock. A lock whose removal fails is reported and requires
 * manual cleanup. `_Exit`, `SIGKILL`, CPU faults, and delivery of
 * musl-reserved signals 32-34 can terminate the process without this cleanup.
 *
 * A forked child discards the inherited lock descriptor and restores the saved
 * death-signal and SIGPIPE dispositions with the death signals blocked.
 * Register the release before the exit marker so the marker runs first. Once
 * registered, the marker prevents later exit callbacks from acquiring a
 * transaction, since `exit` walks the registered functions downwards and
 * nothing would drop a lock taken after the release. Refuse an acquisition if
 * either registration fails.
 *
 * Use O_CLOEXEC because a transaction cannot survive exec. Catch SIGPIPE until
 * the last diagnostic so a closed diagnostic pipe cannot prevent cleanup.
 */
int cred_txn_begin_at(const char *prog, const char *lock_path, int tries)
{
    char line[32];
    sigset_t deaths, saved_mask;
    int attempt, fd, announced = 0, failed, n, i;

    if (!prog) {
        prog = "pwauth";
    }
    if (cred_txn_mine()) {
        fprintf(stderr, "%s: credential database already locked by this program\n", prog);
        return -1;
    }
    if (Exit_under_way) {
        fprintf(stderr, "%s: %s not taken: the program is exiting\n", prog,
                lock_path ? lock_path : "the lock");
        return -1;
    }
    if (Txn_held) {
        sigset_t inherited, previous;

        pwauth_death_set(&inherited);
        sigprocmask(SIG_BLOCK, &inherited, &previous);
        cred_txn_forget();
        cred_pipe_restore();
        sigprocmask(SIG_SETMASK, &previous, NULL);
    }
    if (!lock_path || strlen(lock_path) >= sizeof(Txn_lock)) {
        fprintf(stderr, "pwauth: lock path missing or too long\n");
        return -1;
    }

    if (!Txn_atexit_done) {
        if (atexit(cred_txn_atexit) != 0 || atexit(cred_exit_started) != 0) {
            fprintf(stderr, "%s: cannot register the release of %s\n", prog, lock_path);
            return -1;
        }
        Txn_atexit_done = 1;
    }

    pwauth_death_set(&deaths);
    sigaddset(&deaths, SIGPIPE);
    for (attempt = 0;; attempt++) {
        sigprocmask(SIG_BLOCK, &deaths, &saved_mask);
        fd = open(lock_path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        if (fd >= 0) {
            break;
        }
        failed = errno;
        sigprocmask(SIG_SETMASK, &saved_mask, NULL);
        if (failed != EEXIST) {
            fprintf(stderr, "%s: cannot create %s\n", prog, lock_path);
            return -1;
        }
        if (attempt >= tries) {
            cred_report_busy(prog, lock_path);
            return -1;
        }
        if (!announced) {
            fprintf(stderr, "%s: credential database busy, waiting\n", prog);
            announced = 1;
        }
        sleep(1);
    }

    snprintf(Txn_lock, sizeof(Txn_lock), "%s", lock_path);
    Txn_tmp[0] = '\0';
    Txn_fd = fd;
    Txn_owner = getpid();
    Txn_held = 1;
    for (i = 1; i < PWAUTH_SIG_MAX; i++) {
        if (pwauth_is_death(i)) {
            pwauth_install(i, pwauth_death_handler, &Txn_saved_death[i]);
        }
    }
    pwauth_install(SIGPIPE, pwauth_pipe_handler, &Txn_saved_pipe);

    n = snprintf(line, sizeof(line), "%d\n", (int)getpid());
    if (n <= 0 || write(Txn_fd, line, (size_t)n) != (ssize_t)n) {
        failed = unlink(lock_path) != 0 && errno != ENOENT;
        cred_txn_forget();
        sigprocmask(SIG_SETMASK, &saved_mask, NULL);
        fprintf(stderr, "%s: cannot record the holder pid in %s\n", prog, lock_path);
        if (failed) {
            fprintf(stderr, "%s: %s: cannot remove lock; remove it manually\n", prog, lock_path);
        }
        cred_pipe_restore();
        return -1;
    }
    sigprocmask(SIG_SETMASK, &saved_mask, NULL);
    return 0;
}

/**
 * Take the writer's exclusive lock at `CRED_LOCK_PATH`. Returns 0 when it is
 * held, -1 otherwise.
 */
int cred_txn_begin(const char *prog)
{
    return cred_txn_begin_at(prog, CRED_LOCK_PATH, CRED_LOCK_TRIES);
}

/**
 * Release the transaction. Returns 0 when the lock is gone, `CRED_UNFLUSHED`
 * when it is gone from the file system but the removal did not reach the disk,
 * and -1 when this process holds no transaction, a temporary file could not be
 * removed, or the release could not finish.
 *
 * The lock goes before the dispositions come back, and not the other way
 * around: a signal arriving in between must still find a handler that knows the
 * lock is gone, where the default action would have killed the process with the
 * lock still in place. Release checks the recorded PID before unlinking the
 * lock. Manual removal and replacement between that check and the unlink can
 * still cause removal of a successor's lock.
 *
 * The teardown runs with the deaths blocked, since the dispositions go back one
 * call at a time and a death served midway would meet the handler over state
 * that says the lock is not ours, which re-raises with the default whatever
 * the caller had. The reports after it run with the deaths deliverable, so a
 * writer stuck in a diagnostic can still be interrupted.
 *
 * A failed release is reported to the caller and not only to the operator,
 * since a writer that says it finished cleanly over a lock still in `/etc`
 * sends the next one into a ten-second wait and a refusal it was not warned
 * about. The transaction is forgotten either way: this process holds nothing
 * afterwards, whatever is left at the name.
 */
int cred_txn_end(void)
{
    sigset_t ending, saved_mask;
    const char *report = NULL;
    char temp[sizeof(Txn_tmp)] = "";
    int rc;

    if (!cred_txn_mine()) {
        return -1;
    }
    rc = cred_drop_temp();
    if (rc != 0) {
        memcpy(temp, Txn_tmp, sizeof(temp));
    }
    {
        int released = cred_lock_release(&report);

        if (rc == 0) {
            rc = released;
        }
    }
    pwauth_death_set(&ending);
    sigprocmask(SIG_BLOCK, &ending, &saved_mask);
    cred_txn_forget();
    sigprocmask(SIG_SETMASK, &saved_mask, NULL);
    if (temp[0] != '\0') {
        cred_report_temp(temp);
    }
    if (report != NULL) {
        fprintf(stderr, "pwauth: %s: %s\n", Txn_lock, report);
    }
    cred_pipe_restore();

    return rc;
}

/**
 * The line a replacement is refused with when the lock did not answer for it.
 *
 * A lock that reads as another writer's and a lock that cannot be read at all
 * both stop a publish, and they are different facts: the first is the state the
 * never-break rule describes, the second an open or a read that failed, as on
 * a full descriptor table. Reporting the second as the first would send an
 * operator hunting for a lock file that is exactly where it belongs.
 */
static const char *cred_lock_detail(int state)
{
    return state == CRED_LOCK_UNKNOWN ? "cannot check the transaction's lock"
                                      : "the transaction's lock is gone";
}

/**
 * Undo a replacement that failed before its rename, report `detail`, and return
 * `CRED_UNCHANGED`.
 *
 * Attempt to remove the unpublished file and report any removal failure.
 * Leave the target unchanged. A file whose removal fails requires manual
 * cleanup.
 */
static int cred_rewrite_abort(int fd, const char *path, const char *detail, mode_t saved_umask)
{
    fprintf(stderr, "pwauth: %s: %s\n", path, detail);
    if (fd >= 0) {
        close(fd);
    }
    if (cred_drop_temp() != 0) {
        cred_report_temp(Txn_tmp);
    }
    umask(saved_umask);
    return CRED_UNCHANGED;
}

/**
 * Replace `path` with the `len` bytes at `text`. Returns `CRED_PUBLISHED`,
 * `CRED_UNFLUSHED`, or `CRED_UNCHANGED`.
 *
 * The rename is the moment the new file becomes the real one, and the error
 * handling differs on either side of it. Before the rename, a failure removes
 * the unpublished file, reports any removal failure, leaves the target
 * unchanged and returns `CRED_UNCHANGED`; a file whose removal fails requires
 * manual cleanup. After the rename, the temporary name is gone and the
 * descriptor has followed the file, so a failure is reported and never undone,
 * since unlinking would destroy what was just published; the caller is told
 * `CRED_UNFLUSHED`, because the new file is the one the system now
 * authenticates against whether or not it reached the platter, and
 * "unchanged" would be a lie an operator acts on.
 *
 * The lock is checked before the create's unlink of the temporary name, and
 * again just before the rename. The second check is the one that guards the
 * publish; the first keeps a transaction whose lock is already gone from
 * destroying work that is not its own, since the create begins by removing
 * whatever stands at the temporary name. Between them the temporary belongs to
 * this writer alone: its name has the pid, so two writers loose at once (which
 * takes a broken lock to arrange) still cannot meet on one file.
 *
 * Record the temporary name before creation with death signals blocked. The
 * name begins with the target path; a handler reading a partial copy could
 * unlink the target. Clearing the first byte later is one store.
 */
int cred_rewrite(const char *path, const char *text, size_t len)
{
    char tmp[sizeof(Txn_tmp)];
    struct stat st;
    sigset_t deaths, saved_mask;
    mode_t saved_umask;
    size_t off = 0;
    int fd, named, state;

    if (!cred_txn_mine()) {
        fprintf(stderr, "pwauth: no transaction held; %s left alone\n", path ? path : "(none)");
        return CRED_UNCHANGED;
    }
    if (!path || !text) {
        fprintf(stderr, "pwauth: a replacement needs both a path and its text\n");
        return CRED_UNCHANGED;
    }
    if (cred_drop_temp() != 0) {
        cred_report_temp(Txn_tmp);
        return CRED_UNCHANGED;
    }
    named = snprintf(tmp, sizeof(tmp), CRED_TMP_FORMAT, path, (int)getpid());
    if (named < 0 || (size_t)named >= sizeof(tmp)) {
        fprintf(stderr, "pwauth: %s: path too long for a temporary\n", path);
        return CRED_UNCHANGED;
    }
    state = cred_lock_state();
    if (state != CRED_LOCK_OURS) {
        fprintf(stderr, "pwauth: %s: %s\n", path, cred_lock_detail(state));
        return CRED_UNCHANGED;
    }
    if (stat(path, &st) != 0) {
        fprintf(stderr, "pwauth: %s: cannot read the file being replaced\n", path);
        return CRED_UNCHANGED;
    }

    saved_umask = umask(077);
    pwauth_death_set(&deaths);
    sigprocmask(SIG_BLOCK, &deaths, &saved_mask);
    snprintf(Txn_tmp, sizeof(Txn_tmp), "%s", tmp);
    sigprocmask(SIG_SETMASK, &saved_mask, NULL);
    if (unlink(tmp) != 0 && errno != ENOENT) {
        return cred_rewrite_abort(-1, path, "cannot remove the old temporary file", saved_umask);
    }
    fd = open(tmp, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) {
        Txn_tmp[0] = '\0';
        umask(saved_umask);
        fprintf(stderr, "pwauth: %s: cannot create the replacement\n", tmp);
        return CRED_UNCHANGED;
    }

    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return cred_rewrite_abort(fd, path, "cannot write the replacement", saved_umask);
        }
        off += (size_t)n;
    }
    if (fchown(fd, st.st_uid, st.st_gid) != 0) {
        return cred_rewrite_abort(fd, path, "cannot set the replacement's owner", saved_umask);
    }
    if (fchmod(fd, (mode_t)(st.st_mode & 07777)) != 0) {
        return cred_rewrite_abort(fd, path, "cannot set the replacement's mode", saved_umask);
    }
    if (fsync(fd) != 0) {
        return cred_rewrite_abort(fd, path, "cannot flush the replacement", saved_umask);
    }
    state = cred_lock_state();
    if (state != CRED_LOCK_OURS) {
        return cred_rewrite_abort(fd, path, cred_lock_detail(state), saved_umask);
    }
    if (rename(tmp, path) != 0) {
        return cred_rewrite_abort(fd, path, "cannot rename the replacement into place",
                                  saved_umask);
    }
    Txn_tmp[0] = '\0';
    if (fsync(fd) != 0) {
        close(fd);
        umask(saved_umask);
        fprintf(stderr, "pwauth: %s: published but not flushed to disk\n", path);
        return CRED_UNFLUSHED;
    }
    if (close(fd) != 0) {
        umask(saved_umask);
        fprintf(stderr, "pwauth: %s: published but the close failed\n", path);
        return CRED_UNFLUSHED;
    }
    umask(saved_umask);
    return CRED_PUBLISHED;
}
