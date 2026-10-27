/**
 * Verify that a descriptor open on a regular file names that file from open to
 * close: across an unlink, across a rename onto its name, and across a write
 * that relocates it.
 *
 * A KRFFS node is kept alive by the descriptor tables and freed by the last
 * close, so read, lseek, _llseek and fchmod through a descriptor whose name
 * went succeed and report the file the descriptor opened. The credential
 * readers are what rest on it: each is several syscalls long (musl's
 * getspnam_r opens /etc/shadow and then reads it through fgets), the kernel
 * preempts between two syscalls, and a replacement landing in that window
 * would make su, login and dropbear refuse a correct password if the reader
 * lost its file.
 *
 * The control comes first, so that no later result can be credited to a
 * mis-staged fixture: a descriptor with nothing happening to it reads its file
 * and then reads end of file.
 *
 * Unlink under an open descriptor: the read returns the original bytes in
 * full, lseek and _llseek to SEEK_END and fstat all report the original
 * size, fchmod succeeds, and a fresh open of the name returns -ENOENT. That
 * last one is what separates a lifetime from a lookup that merely forgot to
 * fail.
 *
 * The orphan is hidden from the scans that enumerate by parent id, not only
 * from the resolver. After such an unlink, getdents64 on the parent directory
 * lists neither the name nor a nameless entry, and rmdir of a directory whose
 * only child is an orphan this probe still has succeeds, which is what POSIX
 * says. Both fail on an orphan that keeps its parent id and nothing else
 * changes, the second with -ENOTEMPTY.
 *
 * A rename that fails after its destination was resolved must leave that
 * destination alone, and it is asserted in both the shapes this kernel has.
 * With a descriptor open on the file, renaming a directory onto its name
 * returns -EPERM on the boot root and -ENOTDIR on ext2, and the name stays
 * readable, through the held descriptor and through a fresh open alike. An
 * implementation that orphans before it refuses cannot pass that. The other
 * shape refuses for the destination, not the source: renaming a file onto an
 * open directory returns -EISDIR, which the boot root decides once it has
 * looked the destination up and before it counts the destination's holders,
 * so it takes no orphan, and ext2 before it writes anything. The directory's
 * name must survive the refusal; were the refusal moved after the count, the
 * name would still survive, but only because a directory descriptor is not a
 * holder.
 *
 * The rename onto the reader's name is the case the credential readers need,
 * and it runs across two processes, since a holder scan that looked only at
 * the calling process would pass a single-process version of it. The child
 * opens two files and blocks; the parent writes a replacement, renames it over
 * the first name, unlinks the second name and creates a different file at it,
 * and only then releases the child. Both descriptors must still read what they
 * opened, whole: not a mixture, which is what re-resolving the path would give
 * a reader that had already consumed part of the old file, and not an error.
 * The parent then reads both names itself and gets the replacements. No sleep
 * anywhere: each side blocks on a pipe the other writes, and a syscall on this
 * kernel is not preempted, so the interleaving is the one named here. Each
 * child signals the moment its opens have returned and before it judges them,
 * so a failed open reports itself through the exit status instead of hanging
 * the parent on a byte that never comes.
 *
 * Two descriptors on one orphan, and a child that has the last one, cover
 * the release itself: closing one of a dup'd pair leaves the other reading, so
 * the release counts descriptors and not files, and a child that _exits
 * without closing proves release_fd_table reaches the same release sys_close
 * does.
 *
 * Relocation is forced, not guessed at. krffs_resize_reserved_node
 * extends a node in place when the node that follows it is free and at least
 * `difference` bytes, and moves the file otherwise, so the trigger is the
 * neighbour and never a byte count. Two empty files created back to back put a
 * reserved node behind the first: a creat with no data takes the first free
 * node in the image whatever its size, since the allocator is first-fit and no
 * free node is ever smaller than a header, and the second creat then takes the
 * remainder split off right behind the first file. Where there was no
 * remainder to split, the first file absorbed its whole free node, and what
 * followed that node was already reserved, because the free list never has
 * two adjacent free nodes. Either way the first write past the capacity must
 * move the file, and 1024 bytes is past it: a creat leaves room for no data at
 * all beyond the slack too small to split off, which is under one 357-byte
 * node header. It is asserted twice, once with both descriptors in one table
 * and once with the holder in another process, for the reason the rename case
 * is two processes: a repointing that walked only the writer's own table would
 * pass the first and still leave every other table on the freed node.
 *
 * The leak check judges free space by whether the image runs out, and it is
 * made in all three shapes, because the release lives at the last close and
 * only a descriptor ever reaches it. First 256 rounds of open, unlink under
 * the descriptor, close, on a 64 KB file: a kernel that orphans and never
 * releases needs 16 MB of a 10 MB image and fails at or before round 160,
 * where a correct one uses one extent 256 times.
 * Then the same count of the two shapes that must take no orphan at all: the
 * removal nothing is holding, which is almost every removal, and the write of
 * a new file renamed onto a name, which is what every credential replacement
 * actually does. Neither may exhaust the image, and between them they are what
 * the condition on the orphan is judged by, since an orphan taken at either
 * call site with no holder is one nobody is left to close.
 *
 * KRFFS refuses rmdir with -EBUSY while a descriptor, including O_PATH,
 * or a working directory refers to the directory. A descriptor finds its
 * directory by the node's place, so freeing it would leave the descriptor
 * on whatever node the allocator put there next. The descriptor must still
 * list the directory, and rmdir must succeed once nothing refers to it.
 * Ext2 removes the name and keeps the inode until its last reference goes;
 * getdents64 through the descriptor then returns -ENOENT. Each kind of
 * reference is tested on a separate directory on ext2.
 *
 * Every wait checks WIFEXITED before WEXITSTATUS: a signal death has the
 * signal in the low bits and would otherwise decode as exit 0, scoring a
 * crashed child as a passing one. Every fixture is minted and removed here, so
 * the probe hands the image back as it found it and can run twice in one
 * boot.
 *
 * Prints "FDSTALE OK" and exits 0 only when every case passed; any failure
 * names the step and the answer it got, and exits 1. Writes etude's report
 * records (probe_report.h) for the case ETUDE_CASE names, an assertion for
 * each check `checks` lists, with the branch `root` krffs or ext2; a failure,
 * a child's included, leaves the checks after it not run. Freestanding;
 * direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

typedef unsigned int u32;

#define STDOUT 1

#define EPERM   1
#define ENOTDIR 20
#define ENOENT  2
#define EBADF   9
#define EBUSY   16
#define EISDIR  21

#define O_RDONLY    0
#define O_RDWR      2
#define O_CREAT     0100
#define O_DIRECTORY 0200000
#define O_PATH      010000000
#define SEEK_END    2

/* linux_dirent64: d_reclen is at offset 16, the name begins at 19. */
#define D_RECLEN 16
#define D_NAME   19

/* Linux wait-status decode: a normal exit has the low 7 bits clear and the
 * code in bits 8-15; a signal death has the signal in the low 7 bits. */
#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

#define CTL   "/fdstale.ctl"  /* the control, which nothing happens to */
#define HELD  "/fdstale.held" /* unlinked under the descriptor */
#define ODIR  "/fdstale.odir" /* the directory whose only child is orphaned */
#define KID   "/fdstale.odir/kid"
#define KEEP  "/fdstale.keep" /* the destination a refused rename must spare */
#define RDIR  "/fdstale.rdir" /* the directory both refusals rest on */
#define OLD   "/fdstale.old"  /* renamed over while the child has it */
#define NEW   "/fdstale.new"  /* the replacement, before the rename */
#define GONE  "/fdstale.gone" /* re-created at its own name under the child */
#define EXITF "/fdstale.exit" /* held by a child that _exits without closing */
#define DUPF  "/fdstale.dup"  /* two descriptors on one orphan */
#define GROW  "/fdstale.grow" /* the file a write must relocate */
#define WALL  "/fdstale.wall" /* the reserved node that leaves it no room */
#define XGROW "/fdstale.xgrw" /* the same, moved by another process */
#define XWALL "/fdstale.xwal" /* and its wall */
#define LEAK  "/fdstale.leak" /* the unlink-under-a-descriptor leak loop */
#define SOLO  "/fdstale.solo" /* the loop that removes what nothing has */
#define STAGE "/fdstale.stg"  /* the rename loop's new file */
#define REPL  "/fdstale.repl" /* the name it lands on, which nobody has */
#define DDIR  "/fdstale.ddir" /* the directory rmdir refuses while in use */

/* The pairs a torn read would betray are the same length, so that only their
 * bytes, and not a byte count, can tell them apart. */
#define OLD_LINE   "old:x:0:0:the file the rename replaced\n"
#define NEW_LINE   "new:x:0:0:the file the rename produced\n"
#define GONE_LINE  "held:x:0:0:the file the reader opened\n"
#define FRESH_LINE "made:x:0:0:the file the writer minted\n"

#define CTL_LINE  "the control, which nothing happens to\n"
#define KID_LINE  "the only child of a directory to be removed\n"
#define HELD_LINE "the file whose name went while it was open\n"
#define KEEP_LINE "the file a refused rename must not touch\n"
#define EXIT_LINE "the file whose last holder never closed it\n"
#define DUP_LINE  "the file two descriptors share\n"

/* The relocating write: past the capacity a creat leaves, which is under one
 * 357-byte node header. */
#define GROW_BYTES 1024

/* 256 rounds of 64 KB is 16 MB, which a 10 MB image cannot hold twice over. */
#define LEAK_ROUNDS 256
#define LEAK_CHUNK  4096
#define LEAK_CHUNKS 16

/** The length of the string `s`. */
static u32 slen(const char *s)
{
    u32 n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` to standard output. */
static void puts_(const char *s)
{
    syscall3(SYS_WRITE, STDOUT, (long)s, (int)slen(s));
}

/** Print a signed decimal, so a failing line has the answer it got. */
static void putd(int v)
{
    char buf[13];
    int i = 12;
    unsigned int u = (v < 0) ? (unsigned int)(-v) : (unsigned int)v;

    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + (u % 10u));
        u /= 10u;
    } while (u != 0u);
    if (v < 0) {
        buf[--i] = '-';
    }
    puts_(&buf[i]);
}

/* The checks, one assertion each, in the order the probe makes them, and
 * the step each prints when it fails. A step of `mint`, `fill_round` or a
 * leak loop is not here: it belongs to the check after the last one that
 * held, a row whose text no step prints. A child's checks come before the
 * reap that waits for it. `root` is 'K' for a check that only KRFFS makes,
 * 'E' for one that only ext2 makes, and 0 for the rest. */
enum {
    MINT_CONTROL,
    CONTROL_OPEN,
    CONTROL_READ,
    CONTROL_CLOSE,
    REMOVE_CONTROL,
    MINT_HELD,
    OPEN_UNLINKED,
    UNLINK_UNDER,
    READ_UNLINKED,
    LSEEK_UNLINKED,
    LLSEEK_UNLINKED,
    LLSEEK_RESULT,
    FSTAT_UNLINKED,
    FCHMOD_UNLINKED,
    OPEN_UNLINKED_NAME,
    CLOSE_UNLINKED,
    OPEN_AFTER_CLOSE,
    CREATE_ORPHAN,
    SEED_ORPHAN,
    ORPHAN_NEIGHBOUR,
    ORPHAN_INDEPENDENT,
    ORPHAN_DUPLICATE,
    UNLINK_BEFORE_TRUNCATE,
    SHRINK_ORPHAN,
    SEES_SHRINK,
    SHRINK_KEEPS_OFFSET,
    SHRINK_KEEPS_BYTE,
    GROW_ORPHAN,
    SEES_RELOCATED,
    DUPLICATE_SEES,
    GROWTH_KEEPS_OFFSET,
    REWIND_RELOCATED,
    READ_RELOCATED,
    RELOCATED_FIRST_BYTE,
    GROWTH_ZERO,
    STAYS_NAMELESS,
    CLOSE_ORPHAN_WRITER,
    CLOSE_ORPHAN_DUPLICATE,
    LAST_OPEN_RETAINS,
    CLOSE_LAST_HOLDER,
    CLOSE_NEIGHBOUR,
    REMOVE_NEIGHBOUR,
    MKDIR_ORPHAN_DIR,
    MINT_KID,
    LIST_DIR,
    COUNT_CHILDREN,
    OPEN_CHILD,
    UNLINK_CHILD,
    LIST_PAST_ORPHAN,
    COUNT_PAST_ORPHAN,
    RMDIR_OVER_ORPHAN,
    READ_ORPHAN,
    CLOSE_ORPHAN,
    MINT_KEEP,
    MKDIR_REFUSED,
    OPEN_DESTINATION,
    RENAME_DIR_ONTO_HELD,
    READ_SPARED_HELD,
    READ_SPARED_NAME,
    OPEN_DIR_DESTINATION,
    RENAME_FILE_ONTO_DIR,
    OPEN_SPARED_DIR,
    CLOSE_REOPENED_DIR,
    CLOSE_DIR_DESTINATION,
    CLOSE_SPARED,
    REMOVE_REFUSED,
    REMOVE_SPARED,
    PIPE_UP,
    PIPE_DOWN,
    MINT_READER_FILES,
    FORK_READER,
    PARENT_WAITS_OPENS,
    MINT_REPLACEMENT,
    RENAME_ONTO_HELD,
    UNLINK_RECREATED,
    MINT_FRESH,
    PARENT_RELEASES,
    CHILD_SIGNALS,
    CHILD_OPENS_OLD,
    CHILD_OPENS_GONE,
    CHILD_WAITS,
    CHILD_READS_RENAMED,
    CHILD_READS_RECREATED,
    CHILD_CLOSES_OLD,
    CHILD_CLOSES_GONE,
    READER_CHILD,
    RENAMED_HAS_REPLACEMENT,
    RECREATED_HAS_NEW,
    REMOVE_RENAMED,
    REMOVE_RECREATED,
    MINT_EXIT,
    FORK_HOLDER,
    PARENT_WAITS_HOLDER,
    UNLINK_UNDER_HOLDER,
    PARENT_RELEASES_HOLDER,
    HOLDER_SIGNALS,
    HOLDER_OPENS,
    HOLDER_WAITS,
    HOLDER_READS,
    HOLDER_CHILD,
    NAME_LET_GO,
    MINT_XGROW,
    CLOSE_XGROW,
    MINT_XWALL,
    CLOSE_XWALL,
    FORK_MOVED,
    PARENT_WAITS_MOVED,
    OPEN_MOVER,
    WRITE_MOVES_SHARED,
    CLOSE_MOVER,
    PARENT_RELEASES_MOVED,
    MOVED_SIGNALS,
    MOVED_OPENS,
    MOVED_WAITS,
    MOVED_SEEKS_END,
    MOVED_REWINDS,
    MOVED_READS,
    MOVED_CLOSES,
    MOVED_CHILD,
    REMOVE_SHARED_MOVED,
    REMOVE_SHARED_WALL,
    MKDIR_DDIR,
    OPEN_DDIR,
    RMDIR_UNDER_FD,
    GETDENTS_HELD,
    RELATIVE_HELD,
    CLOSE_RELATIVE,
    GETDENTS_REMOVED,
    CLOSE_HELD_DIR,
    MKDIR_OPATH,
    OPEN_OPATH,
    RMDIR_UNDER_OPATH,
    CLOSE_OPATH,
    MKDIR_CHILD_CWD,
    FORK_CWD_CHILD,
    PARENT_WAITS_CHDIR,
    RMDIR_UNDER_CWD,
    PARENT_RELEASES_CWD,
    CWD_SIGNALS,
    CWD_ENTERS,
    CWD_WAITS,
    CWD_CHILD,
    MKDIR_OWN_CWD,
    CHDIR_INTO,
    RMDIR_OWN_CWD,
    CHDIR_ROOT,
    RMDIR_UNREFERENCED,
    CLOSE_UP_READ,
    CLOSE_UP_WRITE,
    CLOSE_DOWN_READ,
    CLOSE_DOWN_WRITE,
    MINT_DUP,
    OPEN_DUP,
    DUP_FD,
    UNLINK_DUP_PAIR,
    CLOSE_ONE,
    READ_SURVIVOR,
    CLOSE_SURVIVOR,
    MINT_GROW,
    CLOSE_GROW,
    MINT_WALL,
    CLOSE_WALL,
    OPEN_READER,
    OPEN_WRITER,
    WRITE_MOVES,
    LSEEK_MOVED_FROM,
    REWIND_MOVED_FROM,
    READ_MOVED_FROM,
    FSTAT_MOVED_FROM,
    CLOSE_READER,
    CLOSE_WRITER,
    REMOVE_MOVED,
    REMOVE_WALL,
    LEAK_BY_CLOSE,
    LEAK_BY_UNLINK,
    LEAK_BY_RENAME,
    REMOVE_REPLACED,
    CHECKS
};

static const struct {
    const char *id;
    const char *text;
    char root;
} checks[CHECKS] = {
    {"mint-control", "mint the control", 0},
    {"control-open", "the control open", 0},
    {"control-read", "the control read", 0},
    {"control-close", "the control close", 0},
    {"remove-control", "remove the control", 0},
    {"mint-held", "mint the file to be unlinked", 0},
    {"open-unlinked", "open the file to be unlinked", 0},
    {"unlink-under", "unlink under the descriptor", 0},
    {"read-unlinked", "read through the unlinked descriptor", 0},
    {"lseek-unlinked", "lseek SEEK_END through the unlinked descriptor", 0},
    {"llseek-unlinked", LLSEEK_NAME " SEEK_END through the unlinked descriptor", 0},
    {"llseek-result", LLSEEK_NAME "'s result through the unlinked descriptor", 0},
    {"fstat-unlinked", "fstat64 through the unlinked descriptor", 0},
    {"fchmod-unlinked", "fchmod through the unlinked descriptor", 0},
    {"open-unlinked-name", "open the unlinked name", 0},
    {"close-unlinked", "close the unlinked descriptor", 0},
    {"open-after-close", "open the unlinked name after the last close", 0},
    {"create-orphan", "create orphan truncate file", 0},
    {"seed-orphan", "seed orphan truncate file", 0},
    {"orphan-neighbour", "allocate orphan truncate neighbour", 0},
    {"orphan-independent", "independent orphan truncate open", 0},
    {"orphan-duplicate", "duplicate orphan truncate open", 0},
    {"unlink-before-truncate", "unlink before truncate", 0},
    {"shrink-orphan", "shrink orphan", 0},
    {"sees-shrink", "independent open sees orphan shrink", 0},
    {"shrink-keeps-offset", "shrink preserves shared offset", 0},
    {"shrink-keeps-byte", "orphan shrink preserves first byte", 0},
    {"grow-orphan", "grow orphan", 0},
    {"sees-relocated", "independent open sees relocated orphan", 0},
    {"duplicate-sees", "duplicate sees relocated orphan", 0},
    {"growth-keeps-offset", "growth preserves shared offset", 0},
    {"rewind-relocated", "rewind relocated orphan", 0},
    {"read-relocated", "read relocated orphan", 0},
    {"relocated-first-byte", "relocated orphan retains first byte", 0},
    {"growth-zero", "orphan growth is zero filled", 0},
    {"stays-nameless", "truncate keeps orphan nameless", 0},
    {"close-orphan-writer", "close orphan writer", 0},
    {"close-orphan-duplicate", "close orphan duplicate", 0},
    {"last-open-retains", "last open retains relocated orphan", 0},
    {"close-last-holder", "close last orphan holder", 0},
    {"close-neighbour", "close orphan neighbour", 0},
    {"remove-neighbour", "remove orphan neighbour", 0},
    {"mkdir-orphan-dir", "mkdir the orphan's directory", 0},
    {"mint-kid", "mint the child file", 0},
    {"list-dir", "list the directory", 0},
    {"count-children", "count the directory's children", 0},
    {"open-child", "open the child", 0},
    {"unlink-child", "unlink the child under the descriptor", 0},
    {"list-past-orphan", "list past the orphan", 0},
    {"count-past-orphan", "count the children past the orphan", 0},
    {"rmdir-over-orphan", "rmdir over the orphan", 0},
    {"read-orphan", "read the orphan after its directory went", 0},
    {"close-orphan", "close the orphan", 0},
    {"mint-keep", "mint the file a rename spares", 0},
    {"mkdir-refused", "mkdir the refused source", 0},
    {"open-destination", "open the refused rename's destination", 0},
    {"rename-dir-onto-held", "rename a directory onto a held name", 0},
    {"read-spared-held", "read the spared file through the held descriptor", 0},
    {"read-spared-name", "read the spared file by name", 0},
    {"open-dir-destination", "open the directory destination", 0},
    {"rename-file-onto-dir", "rename a file onto a held directory", 0},
    {"open-spared-dir", "open the spared directory by name", 0},
    {"close-reopened-dir", "close the re-opened directory", 0},
    {"close-dir-destination", "close the directory destination", 0},
    {"close-spared", "close the spared file", 0},
    {"remove-refused", "remove the refused source", 0},
    {"remove-spared", "remove the spared file", 0},
    {"pipe-up", "pipe for the child's signal", 0},
    {"pipe-down", "pipe for the parent's signal", 0},
    {"mint-reader-files", "mint the reader's two files", 0},
    {"fork-reader", "fork the reader", 0},
    {"parent-waits-opens", "the parent waits for the child's opens", 0},
    {"mint-replacement", "mint the replacement", 0},
    {"rename-onto-held", "rename the replacement onto the held name", 0},
    {"unlink-recreated", "unlink the re-created name", 0},
    {"mint-fresh", "mint the re-created name", 0},
    {"parent-releases", "the parent releases the child", 0},
    {"child-signals", "the child signals its opens", 0},
    {"child-opens-old", "the child opens the renamed-over name", 0},
    {"child-opens-gone", "the child opens the re-created name", 0},
    {"child-waits", "the child waits for the replacement", 0},
    {"child-reads-renamed", "the child reads across a rename onto its name", 0},
    {"child-reads-recreated", "the child reads across a re-created name", 0},
    {"child-closes-old", "the child closes the renamed-over file", 0},
    {"child-closes-gone", "the child closes the re-created file", 0},
    {"reader-child", "the reader child", 0},
    {"renamed-has-replacement", "the renamed-onto name has the replacement", 0},
    {"recreated-has-new", "the re-created name has the new file", 0},
    {"remove-renamed", "remove the renamed-onto name", 0},
    {"remove-recreated", "remove the re-created name", 0},
    {"mint-exit", "mint the file the holder keeps", 0},
    {"fork-holder", "fork the holder", 0},
    {"parent-waits-holder", "the parent waits for the holder's open", 0},
    {"unlink-under-holder", "unlink under the holder", 0},
    {"parent-releases-holder", "the parent releases the holder", 0},
    {"holder-signals", "the holder signals its open", 0},
    {"holder-opens", "the holder opens the file", 0},
    {"holder-waits", "the holder waits for the unlink", 0},
    {"holder-reads", "the holder reads the file whose name went", 0},
    {"holder-child", "the holder child", 0},
    {"name-let-go", "open the name the holder let go", 0},
    {"mint-xgrow", "mint the file another process moves", 0},
    {"close-xgrow", "close the file another process moves", 0},
    {"mint-xwall", "mint the wall behind the shared file", 0},
    {"close-xwall", "close the wall behind the shared file", 0},
    {"fork-moved", "fork the moved-file reader", 0},
    {"parent-waits-moved", "the parent waits for the moved-file child's open", 0},
    {"open-mover", "open the writer that moves the file", 0},
    {"write-moves-shared", "the write that must move the file under another process", 0},
    {"close-mover", "close the writer that moved the file", 0},
    {"parent-releases-moved", "the parent releases the moved-file child", 0},
    {"moved-signals", "the moved-file child signals its open", 0},
    {"moved-opens", "the moved-file child opens the file", 0},
    {"moved-waits", "the moved-file child waits for the write", 0},
    {"moved-seeks-end", "the moved-file child seeks to the new end", 0},
    {"moved-rewinds", "the moved-file child rewinds", 0},
    {"moved-reads", "the moved-file child reads what another process grew", 0},
    {"moved-closes", "the moved-file child closes the moved file", 0},
    {"moved-child", "the moved-file child", 0},
    {"remove-shared-moved", "remove the shared moved file", 0},
    {"remove-shared-wall", "remove the wall behind it", 0},
    {"mkdir-ddir", "mkdir the directory to be removed", 0},
    {"open-ddir", "open the directory", 0},
    {"rmdir-under-fd", "rmdir under the directory descriptor", 0},
    {"getdents-held", "getdents64 through the held directory", 'K'},
    {"relative-held", "a relative path through the held directory", 'K'},
    {"close-relative", "close what the relative path opened", 'K'},
    {"getdents-removed", "getdents64 through the removed directory", 'E'},
    {"close-held-dir", "close the held directory", 0},
    {"mkdir-opath", "mkdir for the O_PATH descriptor", 'E'},
    {"open-opath", "open the directory with O_PATH", 0},
    {"rmdir-under-opath", "rmdir under the O_PATH descriptor", 0},
    {"close-opath", "close the O_PATH descriptor", 0},
    {"mkdir-child-cwd", "mkdir for the child's working directory", 'E'},
    {"fork-cwd-child", "fork the working-directory child", 0},
    {"parent-waits-chdir", "the parent waits for the child's chdir", 0},
    {"rmdir-under-cwd", "rmdir under a working directory", 0},
    {"parent-releases-cwd", "the parent releases the working-directory child", 0},
    {"cwd-signals", "the working-directory child signals its chdir", 0},
    {"cwd-enters", "the child enters the directory", 0},
    {"cwd-waits", "the working-directory child waits for the rmdir", 0},
    {"cwd-child", "the working-directory child", 0},
    {"mkdir-own-cwd", "mkdir for the caller's working directory", 'E'},
    {"chdir-into", "chdir into the directory", 0},
    {"rmdir-own-cwd", "rmdir under the caller's own working directory", 0},
    {"chdir-root", "chdir back to the root", 0},
    {"rmdir-unreferenced", "rmdir once nothing refers to the directory", 'K'},
    {"close-up-read", "close the child's signal read end", 0},
    {"close-up-write", "close the child's signal write end", 0},
    {"close-down-read", "close the parent's signal read end", 0},
    {"close-down-write", "close the parent's signal write end", 0},
    {"mint-dup", "mint the file to be dup'd", 0},
    {"open-dup", "open the file to be dup'd", 0},
    {"dup-fd", "dup the descriptor", 0},
    {"unlink-dup-pair", "unlink under the dup'd pair", 0},
    {"close-one", "close one of the pair", 0},
    {"read-survivor", "read through the surviving descriptor", 0},
    {"close-survivor", "close the survivor", 0},
    {"mint-grow", "mint the file to be moved", 0},
    {"close-grow", "close the file to be moved", 0},
    {"mint-wall", "mint the wall behind it", 0},
    {"close-wall", "close the wall", 0},
    {"open-reader", "open the reader before the move", 0},
    {"open-writer", "open the writer", 0},
    {"write-moves", "the write that must move the file", 0},
    {"lseek-moved-from", "lseek SEEK_END through the moved-from descriptor", 0},
    {"rewind-moved-from", "rewind the moved-from descriptor", 0},
    {"read-moved-from", "read through the moved-from descriptor", 0},
    {"fstat-moved-from", "fstat64 through the moved-from descriptor", 0},
    {"close-reader", "close the reader", 0},
    {"close-writer", "close the writer", 0},
    {"remove-moved", "remove the moved file", 0},
    {"remove-wall", "remove the wall", 0},
    {"leak-by-close", "the unlink leak loop", 0},
    {"leak-by-unlink", "the unheld unlink loop", 0},
    {"leak-by-rename", "the rename leak loop", 0},
    {"remove-replaced", "remove the replaced name", 0},
};

/* The last check that held, an index of `checks`, or -1 before the first. */
static int at = -1;

/* Whether the root is KRFFS, which decides the branch `root`. */
static int krffs_root;

/* In a child, the write end of the pipe on which it tells its parent the
 * check it reached; -1 in the parent. */
static int child_fd = -1;

/* In the parent, the read end of that pipe for the child not yet reaped. */
static int child_rd = -1;

/* The step of a child's failed check, which the parent reports for it. */
static const char *child_step;

/** Whether the strings `a` and `b` are equal. */
static int same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** The check whose step is `step`: the last one that held, or the first of
 * the checks after it; -1 for a step that is no check's. */
static int find(const char *step)
{
    int i;

    if (at >= 0 && same(checks[at].text, step)) {
        return at;
    }
    for (i = at + 1; i < CHECKS; i++) {
        if (same(checks[i].text, step)) {
            return i;
        }
    }
    return -1;
}

/** Record that the check of `step`, if it is one, held. */
static void held(const char *step)
{
    int i = find(step);

    if (i >= 0) {
        at = i;
    }
}

/** Record that every check before `check` held, as a part of the probe
 * whose steps are no check's begins. */
static void pass_to(int check)
{
    at = check - 1;
}

/** Whether the check `i` is one that the root, KRFFS or not, does not make. */
static int other_root(int i)
{
    return checks[i].root == (krffs_root ? 'E' : 'K');
}

/** In the process that began the report, report the branch of the group
 * `root`, every check of it before `check` as held, `check` as failed with
 * `why`, those after it as not run, and the report's end; with `check`
 * CHECKS, every check held. */
static void report_at(int check, const char *why)
{
    int i;

    if (report_ended || report_getpid() != report_owner) {
        return;
    }
    report_branch("root", krffs_root ? "krffs" : "ext2");
    for (i = 0; i < CHECKS; i++) {
        if (other_root(i)) {
            continue;
        }
        if (i < check) {
            report_pass(checks[i].id);
        } else if (i == check) {
            report_fail(checks[i].id, why);
        } else {
            report_notrun(checks[i].id, "an earlier check failed");
        }
    }
    report_end();
}

/** End the process at the failed step `step`, a check's or one inside the
 * check after the last that held: a child tells its parent which check
 * failed, and the process that began the report reports it. Exits 1. */
static void stop(const char *step)
{
    int i = child_step != 0 ? -1 : find(step);

    if (i < 0) {
        i = at + 1 < CHECKS ? at + 1 : CHECKS - 1;
    }
    if (child_fd >= 0) {
        syscall3(SYS_WRITE, child_fd, (long)&i, (int)sizeof(i));
    }
    report_at(i, child_step != 0 ? child_step : step);
    syscall1(SYS_EXIT, 1);
}

/** End a child whose checks held: tell its parent the last check that held,
 * and exit 0. */
static void child_done(void)
{
    if (child_fd >= 0) {
        syscall3(SYS_WRITE, child_fd, (long)&at, (int)sizeof(at));
    }
    syscall1(SYS_EXIT, 0);
}

/** Print "FDSTALE FAIL: ", the step and the answer it got, and exit 1. */
static void fail(const char *step, int got)
{
    puts_("FDSTALE FAIL: ");
    puts_(step);
    puts_(" (got ");
    putd(got);
    puts_(")\n");
    stop(step);
}

/** A failure with its round, since the leak loops are judged by where they
 * stop: print the step, the round and the answer, and exit 1. */
static void fail_round(const char *step, int round, int got)
{
    puts_("FDSTALE FAIL: ");
    puts_(step);
    puts_(" (round ");
    putd(round);
    puts_(", got ");
    putd(got);
    puts_(")\n");
    stop(step);
}

/** Fail at `step` unless the answer `got` is exactly `want`: an errno, not
 * merely a sign. */
static void want_eq(const char *step, int got, int want)
{
    if (got != want) {
        fail(step, got);
    }
    held(step);
}

/** Fail at `step` unless the call succeeded, which for open is a descriptor
 * and not just 0. */
static void want_ok(const char *step, int got)
{
    if (got < 0) {
        fail(step, got);
    }
    held(step);
}

/* The bytes every filling write emits, and what the relocation case reads
 * back; no NUL, so a compare never ends early. */
static char blob[LEAK_CHUNK];

static char rdbuf[LEAK_CHUNK];

/* One stat buffer, of the larger struct of the two architectures, x86-64's:
 * the kernel writes it, want_fd_size reads it back. */
static char statbuf[144];

/** Whether the strings `a` and `b` are equal. */
static int streq(const char *a, const char *b)
{
    u32 i;

    for (i = 0; a[i] != '\0' && a[i] == b[i]; i++) {
    }
    return a[i] == b[i];
}

/** Read the rest of the file through `fd` and require exactly `len` bytes
 * equal to `want`, then end of file. A short read, a long one, or one byte
 * out of place is a different file or a mixture of two. */
static void want_bytes(const char *step, int fd, const char *want, int len)
{
    int n = syscall3(SYS_READ, fd, (long)rdbuf, (int)sizeof(rdbuf));
    int i;

    if (n != len) {
        fail(step, n);
    }
    for (i = 0; i < len; i++) {
        if (rdbuf[i] != want[i]) {
            puts_("FDSTALE FAIL: ");
            puts_(step);
            puts_(" (byte ");
            putd(i);
            puts_(")\n");
            stop(step);
        }
    }
    n = syscall3(SYS_READ, fd, (long)rdbuf, (int)sizeof(rdbuf));
    if (n != 0) {
        fail(step, n);
    }
    held(step);
}

/** Open `path` read-only, read it whole, close it: the view by name, which
 * the held descriptor's view is judged against. */
static void want_named_bytes(const char *step, const char *path, const char *want, int len)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);

    want_ok(step, fd);
    want_bytes(step, fd, want, len);
    want_eq(step, syscall3(SYS_CLOSE, fd, 0, 0), 0);
}

/** Fail at `step` unless fstat through `fd` reports the size `size`. */
static void want_fd_size(const char *step, int fd, int size)
{
    int got;

    want_eq(step, syscall3(SYS_FSTAT, fd, (long)statbuf, 0), 0);
    got = *(int *)(statbuf + KSTAT_SIZE_AT);
    if (got != size) {
        fail(step, got);
    }
    held(step);
}

/** Create `path` holding `text`, whatever was there before. */
static void mint(const char *path, const char *text)
{
    int fd = syscall3(SYS_CREAT, (long)path, 0644, 0);
    int len = (int)slen(text);

    want_ok("mint a fixture", fd);
    want_eq("fill a fixture", syscall3(SYS_WRITE, fd, (long)text, len), len);
    want_eq("close a fixture", syscall3(SYS_CLOSE, fd, 0, 0), 0);
}

/** Count the entries of `dirpath`, refusing a nameless one and refusing
 * `forbidden`. An orphan that kept its parent id and nothing else shows up
 * here as one or the other. One call drains the directories this probe
 * builds, which hold one child at most. `.` and `..`, which a root that keeps
 * them on the disk returns and the boot root does not, are not counted. */
static int dir_entries(const char *step, const char *dirpath, const char *forbidden)
{
    static char dbuf[1024];
    int fd = syscall3(SYS_OPEN, (long)dirpath, O_RDONLY | O_DIRECTORY, 0);
    int seen = 0;
    int pos = 0;
    int n;

    want_ok(step, fd);
    n = syscall3(SYS_GETDENTS64, fd, (long)dbuf, (int)sizeof(dbuf));
    if (n < 0) {
        fail(step, n);
    }
    while (pos < n) {
        int reclen = *(unsigned short *)(dbuf + pos + D_RECLEN);
        const char *name = dbuf + pos + D_NAME;

        if (reclen <= 0) {
            fail(step, reclen);
        }
        if (name[0] == '\0' || streq(name, forbidden)) {
            fail(step, pos);
        }
        pos += reclen;
        if (streq(name, ".") || streq(name, "..")) {
            continue;
        }
        seen++;
    }
    want_eq(step, syscall3(SYS_CLOSE, fd, 0, 0), 0);
    return seen;
}

/* The rendezvous: a byte on `up` says the child holds its files, a byte on
 * `down` that the parent has replaced them. */
static int up[2];
static int down[2];

/** Write one byte to `fd`, the other side's signal to go on. */
static void notify(const char *step, int fd)
{
    want_eq(step, syscall3(SYS_WRITE, fd, (long)"x", 1), 1);
}

/** Read one byte from `fd`: block until the other side signals. */
static void wait_for(const char *step, int fd)
{
    char c;

    want_eq(step, syscall3(SYS_READ, fd, (long)&c, 1), 1);
}

/** Fork a child that runs `body`, which exits itself; the child's pid. The
 * child closes the parent's ends of the two pipes, so that a parent that
 * fails and exits ends the child's wait on `down` with end of file, and
 * tells the parent the check it reached on a pipe of its own, which `reap`
 * reads. */
static int spawn(void (*body)(void), const char *what)
{
    int fds[2] = {-1, -1};
    int pid;

    held(what);
    if (syscall1(SYS_PIPE, (long)fds) != 0) {
        fds[0] = fds[1] = -1;
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail(what, pid);
    }
    if (pid == 0) {
        syscall3(SYS_CLOSE, up[0], 0, 0);
        syscall3(SYS_CLOSE, down[1], 0, 0);
        if (fds[0] >= 0) {
            syscall1(SYS_CLOSE, fds[0]);
        }
        child_fd = fds[1];
        body();
        /* Every body exits itself; 127 fails one that returns. */
        syscall1(SYS_EXIT, 127);
    }
    if (fds[1] >= 0) {
        syscall1(SYS_CLOSE, fds[1]);
    }
    child_rd = fds[0];
    return pid;
}

/** Read from `child_rd`, unless it is -1, the check the child reached, -1
 * when it said none, and close it. After the child's exit with `status`, a
 * check that held is the parent's last, and one that failed is the parent's
 * to report. */
static void child_reached(int exited_zero)
{
    int reached = -1;

    if (child_rd >= 0) {
        if (syscall3(SYS_READ, child_rd, (long)&reached, (int)sizeof(reached)) !=
                (int)sizeof(reached) ||
            reached < 0 || reached >= CHECKS) {
            reached = -1;
        }
        syscall1(SYS_CLOSE, child_rd);
        child_rd = -1;
    }
    if (reached >= 0 && exited_zero) {
        at = reached;
    } else if (reached >= 0) {
        at = reached - 1;
        child_step = checks[reached].text;
    }
}

/** Reap one child and require a clean exit 0. WIFEXITED comes first: a
 * signal death would otherwise decode as exit 0. The child has already named
 * the case it failed. */
static void reap(int pid, const char *what)
{
    int status = 0;
    int reaped = syscall3(SYS_WAITPID, pid, (long)&status, 0);

    child_reached(reaped == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    if (reaped != pid) {
        fail(what, reaped);
    }
    if (!WIFEXITED(status)) {
        puts_("FDSTALE FAIL: ");
        puts_(what);
        puts_(": died by signal ");
        putd(status & 0x7F);
        puts_("\n");
        stop(what);
    }
    if (WEXITSTATUS(status) != 0) {
        fail(what, WEXITSTATUS(status));
    }
    held(what);
}

/** Nothing happens to this descriptor, so what it reads is what every later
 * case is judged against. */
static void control_case(void)
{
    int fd;

    mint(CTL, CTL_LINE);
    fd = syscall3(SYS_OPEN, (long)CTL, O_RDONLY, 0);
    want_ok("the control open", fd);
    want_bytes("the control read", fd, CTL_LINE, (int)slen(CTL_LINE));
    want_eq("the control close", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    want_eq("remove the control", syscall3(SYS_UNLINK, (long)CTL, 0, 0), 0);
}

/** Unlink under an open descriptor: read, lseek, _llseek, fstat and fchmod
 * through it succeed and report the file it opened, while the name is gone. */
static void unlink_case(void)
{
    int len = (int)slen(HELD_LINE);
    int fd;

    mint(HELD, HELD_LINE);
    fd = syscall3(SYS_OPEN, (long)HELD, O_RDONLY, 0);
    want_ok("open the file to be unlinked", fd);
    want_eq("unlink under the descriptor", syscall3(SYS_UNLINK, (long)HELD, 0, 0), 0);

    want_bytes("read through the unlinked descriptor", fd, HELD_LINE, len);
    want_eq("lseek SEEK_END through the unlinked descriptor", syscall3(SYS_LSEEK, fd, 0, SEEK_END),
            len);
    {
        /* musl's lseek is _llseek on i386 and lseek itself on x86-64. */
        long long res = -1;
        int r;

        r = probe_llseek(fd, 0, SEEK_END, &res);
        want_eq(LLSEEK_NAME " SEEK_END through the unlinked descriptor", r, 0);
        want_eq(LLSEEK_NAME "'s result through the unlinked descriptor", (int)res, len);
    }
    want_fd_size("fstat64 through the unlinked descriptor", fd, len);
    want_eq("fchmod through the unlinked descriptor", syscall3(SYS_FCHMOD, fd, 0600, 0), 0);

    /* The lifetime is the descriptor's, not the name's: the name is gone. */
    want_eq("open the unlinked name", syscall3(SYS_OPEN, (long)HELD, O_RDONLY, 0), -ENOENT);
    want_eq("close the unlinked descriptor", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    want_eq("open the unlinked name after the last close",
            syscall3(SYS_OPEN, (long)HELD, O_RDONLY, 0), -ENOENT);
}

/**
 * Shrink and relocate an unlinked file through its open description.
 *
 * The neighbouring allocation forces growth to move the node. An independent
 * open and a duplicate must both see the new size and the zero-filled growth;
 * neither truncation may change the shared seek offset or recreate the name.
 */
static void orphan_truncate_case(void)
{
    int fd, other, duplicate, wall, i;

    fd = syscall3(SYS_OPEN, (long)HELD, O_CREAT | O_RDWR, 0600);
    want_ok("create orphan truncate file", fd);
    want_eq("seed orphan truncate file", syscall3(SYS_WRITE, fd, (long)"abc", 3), 3);
    wall = syscall3(SYS_CREAT, (long)WALL, 0600, 0);
    want_ok("allocate orphan truncate neighbour", wall);
    other = syscall3(SYS_OPEN, (long)HELD, O_RDWR, 0);
    want_ok("independent orphan truncate open", other);
    duplicate = syscall1(SYS_DUP, fd);
    want_ok("duplicate orphan truncate open", duplicate);
    want_eq("unlink before truncate", syscall1(SYS_UNLINK, (long)HELD), 0);
    want_eq("shrink orphan", syscall3(SYS_FTRUNCATE, fd, 1, 0), 0);
    want_fd_size("independent open sees orphan shrink", other, 1);
    want_eq("shrink preserves shared offset", syscall3(SYS_LSEEK, duplicate, 0, 1), 3);
    want_bytes("orphan shrink preserves first byte", other, "a", 1);
    want_eq("grow orphan", syscall3(SYS_FTRUNCATE, fd, LEAK_CHUNK, 0), 0);
    want_fd_size("independent open sees relocated orphan", other, LEAK_CHUNK);
    want_fd_size("duplicate sees relocated orphan", duplicate, LEAK_CHUNK);
    want_eq("growth preserves shared offset", syscall3(SYS_LSEEK, duplicate, 0, 1), 3);
    want_eq("rewind relocated orphan", syscall3(SYS_LSEEK, other, 0, 0), 0);
    want_eq("read relocated orphan", syscall3(SYS_READ, other, (long)rdbuf, LEAK_CHUNK),
            LEAK_CHUNK);
    want_eq("relocated orphan retains first byte", rdbuf[0], 'a');
    for (i = 1; i < LEAK_CHUNK; i++) {
        want_eq("orphan growth is zero filled", rdbuf[i], 0);
    }
    want_eq("truncate keeps orphan nameless", syscall3(SYS_OPEN, (long)HELD, O_RDONLY, 0), -ENOENT);
    want_eq("close orphan writer", syscall1(SYS_CLOSE, fd), 0);
    want_eq("close orphan duplicate", syscall1(SYS_CLOSE, duplicate), 0);
    want_fd_size("last open retains relocated orphan", other, LEAK_CHUNK);
    want_eq("close last orphan holder", syscall1(SYS_CLOSE, other), 0);
    want_eq("close orphan neighbour", syscall1(SYS_CLOSE, wall), 0);
    want_eq("remove orphan neighbour", syscall1(SYS_UNLINK, (long)WALL), 0);
}

/** The orphan is unreachable by name; it must be invisible to the scans
 * that enumerate by parent id as well. */
static void orphan_scan_case(void)
{
    int fd;

    want_eq("mkdir the orphan's directory", syscall3(SYS_MKDIR, (long)ODIR, 0755, 0), 0);
    mint(KID, KID_LINE);
    want_eq("count the directory's children", dir_entries("list the directory", ODIR, ""), 1);

    fd = syscall3(SYS_OPEN, (long)KID, O_RDONLY, 0);
    want_ok("open the child", fd);
    want_eq("unlink the child under the descriptor", syscall3(SYS_UNLINK, (long)KID, 0, 0), 0);

    want_eq("count the children past the orphan", dir_entries("list past the orphan", ODIR, "kid"),
            0);
    want_eq("rmdir over the orphan", syscall3(SYS_RMDIR, (long)ODIR, 0, 0), 0);

    want_bytes("read the orphan after its directory went", fd, KID_LINE, (int)slen(KID_LINE));
    want_eq("close the orphan", syscall3(SYS_CLOSE, fd, 0, 0), 0);
}

/**
 * Read whether the root is KRFFS by `statfs` of `/`, whose magic is
 * 0x4B524646 on the boot root and 0x4B52 on a KRFFS volume mounted by
 * mount(2). The rename and rmdir rows need different results on KRFFS and
 * ext2. Assume KRFFS if the call fails. `statfs` gives the type of the root
 * the probe sees, a private root inside a chroot among them, where
 * /proc/mounts would name the machine's.
 */
static int root_is_krffs(void)
{
    static unsigned buf[KSTATFS_SIZE / sizeof(unsigned)];

    if (probe_statfs("/", buf) != 0) {
        return 1;
    }
    return buf[0] == 0x4B524646u || buf[0] == 0x4B52u;
}

/** Whether this root performs a directory's rename. The boot root refuses
 * every one of them with EPERM, since its children name their parent by id;
 * ext2 moves a directory and answers a rename onto a file with ENOTDIR,
 * which is what POSIX asks for. The row below expects whichever answer its
 * root gives. */
static int root_moves_directories(void)
{
    return !root_is_krffs();
}

/** A refusal must destroy nothing, in both the shapes this kernel has: the
 * directory source, which the boot root refuses before it looks the
 * destination up and ext2 before it writes anything, and the directory
 * destination, which the boot root refuses with EISDIR once it has looked
 * the destination up and before it counts the destination's holders, so it
 * takes no orphan. Both names are asserted to survive, not assumed to. */
static void failed_rename_case(void)
{
    int len = (int)slen(KEEP_LINE);
    int fd;
    int dirfd;
    int again;

    mint(KEEP, KEEP_LINE);
    want_eq("mkdir the refused source", syscall3(SYS_MKDIR, (long)RDIR, 0755, 0), 0);
    fd = syscall3(SYS_OPEN, (long)KEEP, O_RDONLY, 0);
    want_ok("open the refused rename's destination", fd);

    want_eq("rename a directory onto a held name", syscall3(SYS_RENAME, (long)RDIR, (long)KEEP, 0),
            root_moves_directories() ? -ENOTDIR : -EPERM);
    want_bytes("read the spared file through the held descriptor", fd, KEEP_LINE, len);
    want_named_bytes("read the spared file by name", KEEP, KEEP_LINE, len);

    dirfd = syscall3(SYS_OPEN, (long)RDIR, O_RDONLY | O_DIRECTORY, 0);
    want_ok("open the directory destination", dirfd);
    want_eq("rename a file onto a held directory", syscall3(SYS_RENAME, (long)KEEP, (long)RDIR, 0),
            -EISDIR);
    again = syscall3(SYS_OPEN, (long)RDIR, O_RDONLY | O_DIRECTORY, 0);
    want_ok("open the spared directory by name", again);
    want_eq("close the re-opened directory", syscall3(SYS_CLOSE, again, 0, 0), 0);
    want_eq("close the directory destination", syscall3(SYS_CLOSE, dirfd, 0, 0), 0);

    want_eq("close the spared file", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    want_eq("remove the refused source", syscall3(SYS_RMDIR, (long)RDIR, 0, 0), 0);
    want_eq("remove the spared file", syscall3(SYS_UNLINK, (long)KEEP, 0, 0), 0);
}

/** The child of the cross-process case: it opens both names and then
 * blocks, so the parent's replacement lands squarely between its open and
 * its read. It signals before its opens are judged, so that a failed open
 * reports itself through the exit status and does not hang the parent on a
 * byte that never comes. */
static void reader_child(void)
{
    int fd_old = syscall3(SYS_OPEN, (long)OLD, O_RDONLY, 0);
    int fd_gone = syscall3(SYS_OPEN, (long)GONE, O_RDONLY, 0);

    notify("the child signals its opens", up[1]);
    want_ok("the child opens the renamed-over name", fd_old);
    want_ok("the child opens the re-created name", fd_gone);
    wait_for("the child waits for the replacement", down[0]);

    want_bytes("the child reads across a rename onto its name", fd_old, OLD_LINE,
               (int)slen(OLD_LINE));
    want_bytes("the child reads across a re-created name", fd_gone, GONE_LINE,
               (int)slen(GONE_LINE));
    want_eq("the child closes the renamed-over file", syscall3(SYS_CLOSE, fd_old, 0, 0), 0);
    want_eq("the child closes the re-created file", syscall3(SYS_CLOSE, fd_gone, 0, 0), 0);
    child_done();
}

/** The rename onto a reader's name, with the reader in another process,
 * which a single-process version would not reach. */
static void cross_process_case(void)
{
    int pid;

    mint(OLD, OLD_LINE);
    mint(GONE, GONE_LINE);

    pid = spawn(reader_child, "fork the reader");
    wait_for("the parent waits for the child's opens", up[0]);

    mint(NEW, NEW_LINE);
    want_eq("rename the replacement onto the held name",
            syscall3(SYS_RENAME, (long)NEW, (long)OLD, 0), 0);
    want_eq("unlink the re-created name", syscall3(SYS_UNLINK, (long)GONE, 0, 0), 0);
    mint(GONE, FRESH_LINE);

    notify("the parent releases the child", down[1]);
    reap(pid, "the reader child");

    want_named_bytes("the renamed-onto name has the replacement", OLD, NEW_LINE,
                     (int)slen(NEW_LINE));
    want_named_bytes("the re-created name has the new file", GONE, FRESH_LINE,
                     (int)slen(FRESH_LINE));
    want_eq("remove the renamed-onto name", syscall3(SYS_UNLINK, (long)OLD, 0, 0), 0);
    want_eq("remove the re-created name", syscall3(SYS_UNLINK, (long)GONE, 0, 0), 0);
}

/** The child of the release case: it has the only descriptor on an orphan
 * and exits without closing it. */
static void holder_child(void)
{
    int fd = syscall3(SYS_OPEN, (long)EXITF, O_RDONLY, 0);

    notify("the holder signals its open", up[1]);
    want_ok("the holder opens the file", fd);
    wait_for("the holder waits for the unlink", down[0]);
    want_bytes("the holder reads the file whose name went", fd, EXIT_LINE, (int)slen(EXIT_LINE));
    child_done();
}

/** The release at exit: the holder child exits with the only descriptor on
 * an orphan, and the name stays gone. */
static void exit_release_case(void)
{
    int pid;

    mint(EXITF, EXIT_LINE);
    pid = spawn(holder_child, "fork the holder");
    wait_for("the parent waits for the holder's open", up[0]);
    want_eq("unlink under the holder", syscall3(SYS_UNLINK, (long)EXITF, 0, 0), 0);
    notify("the parent releases the holder", down[1]);
    reap(pid, "the holder child");
    want_eq("open the name the holder let go", syscall3(SYS_OPEN, (long)EXITF, O_RDONLY, 0),
            -ENOENT);
}

/** The child of the cross-process relocation case: it opens the file before
 * the write that moves it and reads it after. */
static void moved_child(void)
{
    int fd = syscall3(SYS_OPEN, (long)XGROW, O_RDONLY, 0);

    notify("the moved-file child signals its open", up[1]);
    want_ok("the moved-file child opens the file", fd);
    wait_for("the moved-file child waits for the write", down[0]);

    want_eq("the moved-file child seeks to the new end", syscall3(SYS_LSEEK, fd, 0, SEEK_END),
            GROW_BYTES);
    want_eq("the moved-file child rewinds", syscall3(SYS_LSEEK, fd, 0, 0), 0);
    want_bytes("the moved-file child reads what another process grew", fd, blob, GROW_BYTES);
    want_eq("the moved-file child closes the moved file", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    child_done();
}

/** Relocation with the holder in another process. The two-descriptor case
 * below keeps both in one table, so a repointing that walked only the
 * writer's own process would pass it and still leave every other table on
 * the node the allocator freed. That is the same hole the rename case is two
 * processes for, in the half of the contract a write owns. */
static void cross_reloc_case(void)
{
    int pid, fd, writer;

    fd = syscall3(SYS_CREAT, (long)XGROW, 0644, 0);
    want_ok("mint the file another process moves", fd);
    want_eq("close the file another process moves", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    fd = syscall3(SYS_CREAT, (long)XWALL, 0644, 0);
    want_ok("mint the wall behind the shared file", fd);
    want_eq("close the wall behind the shared file", syscall3(SYS_CLOSE, fd, 0, 0), 0);

    pid = spawn(moved_child, "fork the moved-file reader");
    wait_for("the parent waits for the moved-file child's open", up[0]);

    writer = syscall3(SYS_OPEN, (long)XGROW, O_RDWR, 0);
    want_ok("open the writer that moves the file", writer);
    want_eq("the write that must move the file under another process",
            syscall3(SYS_WRITE, writer, (long)blob, GROW_BYTES), GROW_BYTES);
    want_eq("close the writer that moved the file", syscall3(SYS_CLOSE, writer, 0, 0), 0);

    notify("the parent releases the moved-file child", down[1]);
    reap(pid, "the moved-file child");
    want_eq("remove the shared moved file", syscall3(SYS_UNLINK, (long)XGROW, 0, 0), 0);
    want_eq("remove the wall behind it", syscall3(SYS_UNLINK, (long)XWALL, 0, 0), 0);
}

/** The release counts descriptors, not files: a dup'd pair has one orphan
 * and the survivor goes on reading. */
static void dup_case(void)
{
    int len = (int)slen(DUP_LINE);
    int fd, dupfd;

    mint(DUPF, DUP_LINE);
    fd = syscall3(SYS_OPEN, (long)DUPF, O_RDONLY, 0);
    want_ok("open the file to be dup'd", fd);
    dupfd = syscall1(SYS_DUP, fd);
    want_ok("dup the descriptor", dupfd);
    want_eq("unlink under the dup'd pair", syscall3(SYS_UNLINK, (long)DUPF, 0, 0), 0);
    want_eq("close one of the pair", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    want_bytes("read through the surviving descriptor", dupfd, DUP_LINE, len);
    want_eq("close the survivor", syscall3(SYS_CLOSE, dupfd, 0, 0), 0);
}

/** A write through one descriptor moves the file; the other must follow it.
 * The opening comment says why the wall is what forces the move. */
static void reloc_case(void)
{
    int fd, reader, writer;

    fd = syscall3(SYS_CREAT, (long)GROW, 0644, 0);
    want_ok("mint the file to be moved", fd);
    want_eq("close the file to be moved", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    fd = syscall3(SYS_CREAT, (long)WALL, 0644, 0);
    want_ok("mint the wall behind it", fd);
    want_eq("close the wall", syscall3(SYS_CLOSE, fd, 0, 0), 0);

    reader = syscall3(SYS_OPEN, (long)GROW, O_RDONLY, 0);
    want_ok("open the reader before the move", reader);
    writer = syscall3(SYS_OPEN, (long)GROW, O_RDWR, 0);
    want_ok("open the writer", writer);

    want_eq("the write that must move the file",
            syscall3(SYS_WRITE, writer, (long)blob, GROW_BYTES), GROW_BYTES);

    want_eq("lseek SEEK_END through the moved-from descriptor",
            syscall3(SYS_LSEEK, reader, 0, SEEK_END), GROW_BYTES);
    want_eq("rewind the moved-from descriptor", syscall3(SYS_LSEEK, reader, 0, 0), 0);
    want_bytes("read through the moved-from descriptor", reader, blob, GROW_BYTES);
    want_fd_size("fstat64 through the moved-from descriptor", reader, GROW_BYTES);

    want_eq("close the reader", syscall3(SYS_CLOSE, reader, 0, 0), 0);
    want_eq("close the writer", syscall3(SYS_CLOSE, writer, 0, 0), 0);
    want_eq("remove the moved file", syscall3(SYS_UNLINK, (long)GROW, 0, 0), 0);
    want_eq("remove the wall", syscall3(SYS_UNLINK, (long)WALL, 0, 0), 0);
}

/** Fill `fd` to 64 KB, the size at which 256 rounds outgrow the image;
 * fail, naming `step` and `round`, at a short write. */
static void fill_round(const char *step, int fd, int round)
{
    int i;

    for (i = 0; i < LEAK_CHUNKS; i++) {
        int n = syscall3(SYS_WRITE, fd, (long)blob, LEAK_CHUNK);

        if (n != LEAK_CHUNK) {
            fail_round(step, round, n);
        }
    }
}

/** An orphan released at the last close costs one extent however often it
 * is made; one that is never released costs 16 MB of a 10 MB image. */
static void leak_by_close_case(void)
{
    int round;

    pass_to(LEAK_BY_CLOSE);
    for (round = 0; round < LEAK_ROUNDS; round++) {
        int fd = syscall3(SYS_OPEN, (long)LEAK, O_RDWR | O_CREAT, 0644);
        int rc;

        if (fd < 0) {
            fail_round("open in the unlink leak loop", round, fd);
        }
        fill_round("fill in the unlink leak loop", fd, round);
        rc = syscall3(SYS_UNLINK, (long)LEAK, 0, 0);
        if (rc != 0) {
            fail_round("unlink in the unlink leak loop", round, rc);
        }
        rc = syscall3(SYS_CLOSE, fd, 0, 0);
        if (rc != 0) {
            fail_round("close in the unlink leak loop", round, rc);
        }
    }
}

/** The removal nothing is reading, which is almost every removal. The loop
 * above cannot reach it: it always unlinks under a descriptor, and that
 * descriptor is what releases the orphan. Here there is none, so an orphan
 * taken anyway is never closed and never released. */
static void leak_by_unlink_case(void)
{
    int round;

    pass_to(LEAK_BY_UNLINK);
    for (round = 0; round < LEAK_ROUNDS; round++) {
        int fd = syscall3(SYS_CREAT, (long)SOLO, 0644, 0);
        int rc;

        if (fd < 0) {
            fail_round("creat in the unheld unlink loop", round, fd);
        }
        fill_round("fill in the unheld unlink loop", fd, round);
        rc = syscall3(SYS_CLOSE, fd, 0, 0);
        if (rc != 0) {
            fail_round("close in the unheld unlink loop", round, rc);
        }
        rc = syscall3(SYS_UNLINK, (long)SOLO, 0, 0);
        if (rc != 0) {
            fail_round("unlink in the unheld unlink loop", round, rc);
        }
    }
}

/** The shape every credential replacement has, onto a name no descriptor
 * has. An orphan made unconditionally is never closed and never released,
 * so this is the loop that catches it. */
static void leak_by_rename_case(void)
{
    int round;

    pass_to(LEAK_BY_RENAME);
    for (round = 0; round < LEAK_ROUNDS; round++) {
        int fd = syscall3(SYS_CREAT, (long)STAGE, 0644, 0);
        int rc;

        if (fd < 0) {
            fail_round("creat in the rename leak loop", round, fd);
        }
        fill_round("fill in the rename leak loop", fd, round);
        rc = syscall3(SYS_CLOSE, fd, 0, 0);
        if (rc != 0) {
            fail_round("close in the rename leak loop", round, rc);
        }
        rc = syscall3(SYS_RENAME, (long)STAGE, (long)REPL, 0);
        if (rc != 0) {
            fail_round("rename in the rename leak loop", round, rc);
        }
    }
    want_eq("remove the replaced name", syscall3(SYS_UNLINK, (long)REPL, 0, 0), 0);
}

/** The child of the directory case: its working directory is the directory
 * the parent tries to remove. */
static void cwd_child(void)
{
    int rc = syscall1(SYS_CHDIR, (long)DDIR);

    notify("the working-directory child signals its chdir", up[1]);
    want_eq("the child enters the directory", rc, 0);
    wait_for("the working-directory child waits for the rmdir", down[0]);
    child_done();
}

/**
 * Check rmdir with a reading descriptor, an O_PATH descriptor, a child's
 * working directory and the caller's working directory. KRFFS refuses each
 * removal with -EBUSY and removes the directory after the last reference
 * goes. Ext2 removes each name immediately, so each case needs a new
 * directory; the open reading descriptor then lists it with -ENOENT.
 */
static void directory_case(void)
{
    static char dbuf[256];
    int krffs = root_is_krffs();
    int result = krffs ? -EBUSY : 0;
    int fd;
    int dot;
    int pid;

    want_eq("mkdir the directory to be removed", syscall3(SYS_MKDIR, (long)DDIR, 0755, 0), 0);
    fd = syscall3(SYS_OPEN, (long)DDIR, O_RDONLY | O_DIRECTORY, 0);
    want_ok("open the directory", fd);
    want_eq("rmdir under the directory descriptor", syscall3(SYS_RMDIR, (long)DDIR, 0, 0), result);
    if (krffs) {
        want_ok("getdents64 through the held directory",
                syscall3(SYS_GETDENTS64, fd, (long)dbuf, (int)sizeof(dbuf)));
        dot = syscall4(SYS_OPENAT, fd, (long)".", O_RDONLY | O_DIRECTORY, 0);
        want_ok("a relative path through the held directory", dot);
        want_eq("close what the relative path opened", syscall3(SYS_CLOSE, dot, 0, 0), 0);
    } else {
        want_eq("getdents64 through the removed directory",
                syscall3(SYS_GETDENTS64, fd, (long)dbuf, (int)sizeof(dbuf)), -ENOENT);
    }
    want_eq("close the held directory", syscall3(SYS_CLOSE, fd, 0, 0), 0);

    if (!krffs) {
        want_eq("mkdir for the O_PATH descriptor", syscall3(SYS_MKDIR, (long)DDIR, 0755, 0), 0);
    }
    fd = syscall3(SYS_OPEN, (long)DDIR, O_PATH, 0);
    want_ok("open the directory with O_PATH", fd);
    want_eq("rmdir under the O_PATH descriptor", syscall3(SYS_RMDIR, (long)DDIR, 0, 0), result);
    want_eq("close the O_PATH descriptor", syscall3(SYS_CLOSE, fd, 0, 0), 0);

    if (!krffs) {
        want_eq("mkdir for the child's working directory", syscall3(SYS_MKDIR, (long)DDIR, 0755, 0),
                0);
    }
    pid = spawn(cwd_child, "fork the working-directory child");
    wait_for("the parent waits for the child's chdir", up[0]);
    want_eq("rmdir under a working directory", syscall3(SYS_RMDIR, (long)DDIR, 0, 0), result);
    notify("the parent releases the working-directory child", down[1]);
    reap(pid, "the working-directory child");

    if (!krffs) {
        want_eq("mkdir for the caller's working directory",
                syscall3(SYS_MKDIR, (long)DDIR, 0755, 0), 0);
    }
    want_eq("chdir into the directory", syscall1(SYS_CHDIR, (long)DDIR), 0);
    want_eq("rmdir under the caller's own working directory", syscall3(SYS_RMDIR, (long)DDIR, 0, 0),
            result);
    want_eq("chdir back to the root", syscall1(SYS_CHDIR, (long)"/"), 0);
    if (krffs) {
        want_eq("rmdir once nothing refers to the directory", syscall3(SYS_RMDIR, (long)DDIR, 0, 0),
                0);
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Run every case, the cross-process ones over the two pipes, and print
 * FDSTALE OK; each case exits 1 at its first failure. `_start` calls it with
 * the initial stack, whose environment gives the case's identity. */
void probe_main(unsigned long *sp)
{
    int i;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.fdstale"));
    krffs_root = root_is_krffs();
    for (i = 0; i < LEAK_CHUNK; i++) {
        blob[i] = (char)('a' + (i % 26));
    }

    control_case();
    unlink_case();
    orphan_truncate_case();
    orphan_scan_case();
    failed_rename_case();

    want_eq("pipe for the child's signal", syscall3(SYS_PIPE, (long)up, 0, 0), 0);
    want_eq("pipe for the parent's signal", syscall3(SYS_PIPE, (long)down, 0, 0), 0);
    cross_process_case();
    exit_release_case();
    cross_reloc_case();
    directory_case();
    want_eq("close the child's signal read end", syscall3(SYS_CLOSE, up[0], 0, 0), 0);
    want_eq("close the child's signal write end", syscall3(SYS_CLOSE, up[1], 0, 0), 0);
    want_eq("close the parent's signal read end", syscall3(SYS_CLOSE, down[0], 0, 0), 0);
    want_eq("close the parent's signal write end", syscall3(SYS_CLOSE, down[1], 0, 0), 0);

    dup_case();
    reloc_case();
    leak_by_close_case();
    leak_by_unlink_case();
    leak_by_rename_case();

    report_at(CHECKS, 0);
    puts_("FDSTALE OK\n");
    syscall1(SYS_EXIT, 0);
}
