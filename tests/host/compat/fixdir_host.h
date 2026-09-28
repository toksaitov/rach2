#ifndef FIXDIR_HOST_H
#define FIXDIR_HOST_H

/*
 * The directory a host test keeps its fixtures in (fixdir_host.c). A test
 * compiled with FIXDIR, as a mutation proof compiles one, has the directory
 * FIXDIR names. Any other has one of its own, made the first time it is asked
 * for and removed when the test exits, so that two runs at once, of one test
 * or of two, never write the same file. It is made under $TMPDIR when that is
 * an absolute path of at most 100 characters, each a letter, a digit or one
 * of / . _ + -, which a shell passes through unchanged, so a test may put the
 * path in a command unquoted; under /tmp otherwise. The header includes
 * nothing, since the kernel headers the tests include define types the C
 * library's headers would define again.
 */

/** The fixture directory; a test that cannot have one is ended. */
const char *fix_dir(void);

/**
 * The path of `name` in the fixture directory, in a buffer of that name's own
 * that stays for the run, so a caller may keep it; a test has room for 64
 * names of fewer than 64 characters each.
 */
const char *fix_path(const char *name);

/** Keep a directory of the test's own at exit, and say where it is, so that a
 * failed test's fixtures can be read. */
void fix_keep(void);

#endif /* FIXDIR_HOST_H */
