# Upstream faults of milestone 68

Faults this milestone found in another project's code, one section each, in the format of [`dev-log/upstream.md`](../upstream.md), which indexes them.

## cmus's configure tests the build host's OSS header

* Project: cmus 2.12.0, the source package of Debian unstable with Debian's patches (`vendor/gnu/ports/cmus.sh`, `DEBIAN=cmus-debian`).
* Fails: cmus's `configure` decides OSS support by looking for the build host's `/usr/include/sys/soundcard.h` rather than compiling the header with the compiler it was given, so a cross build takes its answer from the build host (`configure:413`).
* Found: 2026-11-01, in milestone 68's port of cmus to the GNU root.
* Reproduction: cross-build cmus on a host without `sys/soundcard.h` for a target that has it, and the reverse, and read which outputs configure enables.
* Evidence: `dev-log/68-sound/plan.md`, 6b; `vendor/gnu/ports/cmus.sh:6-10`; commit `559922cb2`. The tarball is not kept in this repository, so `configure:413` was not read again here.
* Rach2: the recipe sets `CONFIG_OSS=y` and `CONFIG_ALSA=n` and compiles the header with the cross compiler.
* Status: unreported.
* Next: read cmus's current `configure`; a fix would test the header through the compiler.

## cmus takes its version from an enclosing git clone

* Project: cmus 2.12.0.
* Fails: cmus's Makefile takes the version from `git describe`, which in a build tree inside an unrelated clone describes that clone.
* Found: 2026-11-01, in milestone 68, building inside Rach2's clone.
* Reproduction: unpack the release tarball inside any git clone, build, and run `cmus --version`.
* Evidence: `vendor/gnu/ports/cmus.sh:11-13`; commit `a11c83e5b`. The Makefile line was not read again here.
* Rach2: the recipe gives make the version.
* Status: unreported.
* Next: read cmus's current Makefile; a fix would use `git describe` only when the source tree is itself the top of a clone.

## glibc formats a rounded alternate g conversion without its zeros

* Project: glibc 2.43, as Ubuntu 26.04 ships it; the package version is missing.
* Fails: `printf("%#g", 999999.5)` prints "1.e+06" where C99 gives "1.00000e+06": the zeros that the rounding carry into a new digit leaves are dropped, though `%e` and `%f` are right.
* Found: 2026-11-03, when the release's host tests failed `doom_format_hosttest` on the testing host's Ubuntu 26.04. The commit names no milestone; filing it under 68 follows from the date and is not recorded.
* Reproduction: `printf("%#g\n", 999999.5);` compiled against glibc 2.43.
* Evidence: commit `84bdab465`; the comment above `alt_g_snprintf` in `tests/host/doom_format_hosttest.c`.
* Rach2: the test judges its `%#g` rows by C99's definition through the host's `%e` and `%f`.
* Status: unreported.
* Next: reproduce on upstream glibc 2.43 apart from Ubuntu's patches, check whether earlier releases print the same, and search glibc's Bugzilla.
