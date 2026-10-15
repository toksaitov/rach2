# Build and verification tools

The programs and scripts under `tools/` prepare a machine to build Rach2 and check what the build makes. `setup/` checks a host for the tools the build and the suites need; `verify/` is the style check, `tests/run style`, which judges the source tree; `inspect/` judges built kernels, programs and images; `image/` makes the ext2 roots, GRUB disks, sticks and ISOs; `ci/` is what the GitHub workflows run; `flow/` runs the worker and manager loops; `dev/` reaches the testing host and installed machines and builds the compilation database; `font/` makes the console's font; and `git-hooks/` has the commit-msg hook. The root Makefile runs most of them with the arguments they need, so a person seldom calls one directly. The suites, their runner and their checkers are under [`tests/system/`](../tests/system/Readme.md).

## Use

### Setting up a machine

A build needs GNU Make, a host C compiler, the cross toolchain of its architecture (`i686-elf` for `make`, `x86_64-elf` for `make ARCH=x86_64`), e2fsprogs 1.43 or later, mandoc, `ssh-keygen`, awk, sed, tar, gzip, bzip2 and patch. `tests/run style` adds cppcheck, ShellCheck, clang and clang-format. The suites add QEMU, OVMF (its UEFI firmware for x86-64), xorriso for the ISOs and sticks, and the commands `tests/registry` lists for each suite, which `tests/run` looks for before it builds: mtools, `qemu-img`, python3, `nc` and `shasum` among them. `tests/run` runs one architecture's suites, i686's unless `ARCH=x86_64` is set. The default `make` downloads nothing; only the GNU layer, which the `gnu` suite builds, fetches its tarballs, with curl or wget ([fetch.sh](../vendor/gnu/fetch.sh)). Install the packages below, then run `sh tools/setup/doctor.sh`.

#### macOS

Xcode's command line tools supply `make`, `cc`, clang and python3, and Homebrew the rest. Run the `shellenv` line the Homebrew installer prints, since it does not put `brew` on the path.

    xcode-select --install
    /bin/bash -c "$(curl -fsSL \
        https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    eval "$(/opt/homebrew/bin/brew shellenv)"
    brew install i686-elf-gcc i686-elf-binutils x86_64-elf-gcc x86_64-elf-binutils \
                 qemu mtools e2fsprogs mandoc cppcheck shellcheck clang-format \
                 xorriso rsync

The build finds e2fsprogs in its keg, which Homebrew never links, and the suites find OVMF in QEMU's `share/qemu`. rsync 3 is for [testing.sh](dev/testing.sh). `pkg-config` and the `fuse-t` cask serve `make -C krffs mount`, which no build or suite needs.

#### Debian and Ubuntu

    sudo apt install build-essential curl qemu-system-x86 qemu-utils ovmf mtools \
                     e2fsprogs mandoc openssh-client netcat-openbsd python3 xorriso \
                     cppcheck shellcheck clang clang-format

`pkg-config` and `libfuse-dev` serve `make -C krffs mount`. No package provides an `i686-elf` toolchain, but the archive's `i686-linux-gnu` cross compiler builds this tree, since the kernel and guest programs are freestanding and the ports link against its libgcc. Link its tools under the names the tree calls:

    sudo apt install gcc-i686-linux-gnu binutils-i686-linux-gnu
    mkdir -p "$HOME/bin"
    for t in ld ar objcopy strip nm objdump readelf as ranlib size strings; do
        ln -sf "/usr/bin/i686-linux-gnu-$t" "$HOME/bin/i686-elf-$t"
    done

The compiler is wrapped, not linked. A GCC that targets Linux defines `__linux__`, builds position-independent code, and adds the stack protector, control-flow protection, stack-clash probes, unwind tables and `_FORTIFY_SOURCE`; the ports and guest programs are written for a bare `i686-elf-gcc`, and a static `tcc` built position-independent faults at address 0x1c. `$HOME/bin/i686-elf-gcc` is:

    #!/bin/sh
    exec /usr/bin/i686-linux-gnu-gcc \
        -fno-pie -no-pie -fno-stack-protector -fcf-protection=none \
        -fno-stack-clash-protection -fno-asynchronous-unwind-tables \
        -fno-unwind-tables -U_FORTIFY_SOURCE -U__linux__ -U__gnu_linux__ -Ulinux \
        "$@"

Make the x86-64 toolchain the same way from `x86_64-linux-gnu`, the host's own compiler on amd64 and the packages `gcc-x86-64-linux-gnu` and `binutils-x86-64-linux-gnu` elsewhere: link its tools as `x86_64-elf-*`, and write `$HOME/bin/x86_64-elf-gcc` as the script above with `/usr/bin/x86_64-linux-gnu-gcc`. The x86-64 compiler with `-m32` cannot stand in for the i686 one, since it has no 32-bit libgcc. The archive's GCC 15.2 and binutils 2.46, on Ubuntu 26.04, are the oldest this tree supports.

#### The testing host

The suites, the sets and the analyses run on the testing host, a Windows machine with an AMD Ryzen 9 7950X3D, 32 threads and 200 GB of memory, running Ubuntu 26.04 under WSL2. The Windows user's `.wslconfig` gives WSL 128 GB (`memory=128GB`), sets `nestedVirtualization=true`, and sets both idle timeouts to -1; the scheduled task `Rach2 WSL` starts WSL when Windows boots, with nobody logged in. The distribution's disk, a file that grows, is under `D:\WSL\Ubuntu-26.04`. When its drive fills, Linux turns its root read-only part way through a run, so before a run of any suite but `style` and `host`, `tests/run` asks Windows for the drive's free space and refuses a run below `RACH2_MIN_FREE_GIB` ([tests/Readme.md](../tests/Readme.md)). The host is set up as Debian and Ubuntu above, with the analysers that `tests/run style` and `make analyze` skip elsewhere:

    sudo apt install clang-tidy sparse pmccabe

Each person has an account in WSL named after their GitHub login, with a clone of their own, and logs in by key as `ssh rach2`, an alias for port 2227 of `auca.space`, which Windows forwards to the ssh server in WSL; that server refuses passwords. The top of the account's `.bashrc`, which bash reads for a command sent over ssh too, sets `RACH2_QEMU_ACCEL=tcg` and the person's share, `RACH2_WORKERS=8` and `RACH2_MEMORY_MIB=16384`, and sets `WSL_DISTRO_NAME` and puts the PowerShell directory on `PATH`, which `tests/run` needs to ask Windows for the free space. Two GitHub Actions runners, `rach2-ci-1` and `rach2-ci-2`, run as the user `ghrunner`, each with its own directory and checkout, and take the jobs labelled `rach2`; each one's `.env` sets the same variables with 12 workers and 24576 MiB, and its `.path` has the PowerShell directory. A share keeps one run from taking the machine, which also runs Hyper-V guests; the shares do not bound all runs together. The owner's copy of the tree is `/home/testing/rach2`, and each of the owner's worker loops has one of its own beside it; they are reached as `ssh testing` through [testing.sh](dev/testing.sh).

The accounts are in the `kvm` group, and the runner uses `/dev/kvm` when it can open it and refuses a device it cannot. Runs on this host ask for the emulator all the same: the KVM is nested under Hyper-V, and the ATA driver polls a port after every sector it writes, each poll exiting through two hypervisors, so a disk write is slower than under the emulator.

### Checking a machine: `setup/`

* [preflight.sh](setup/preflight.sh) is the build's tool check, an order-only prerequisite of the root Makefile's build targets, so every `make` and `tests/run` runs it. It checks the build's tools and the cross toolchain of `RACH2_ARCH`, and caches the result in `build/.preflight` or `build/x86_64/.preflight`, so that a check with nothing changed runs none of the tools. A missing tool fails the build by name before the cross toolchain compiles anything. `--report` prints a line for each tool.
* [doctor.sh](setup/doctor.sh) is the wider report, run by hand, in five groups: the build and boot of both architectures, OVMF, e2fsprogs and mandoc included; the tools of `tests/run style`; the mtools commands and xorriso, which the suites need; the optional tools; and the version of each tool in [judges](setup/judges) beside the one recorded there from the testing host, whose version decides a check that depends on one. It runs each required tool, and exits nonzero when one is missing or broken.
* [e2fsprogs_path.sh](setup/e2fsprogs_path.sh) adds Homebrew's keg, `/sbin` and `/usr/sbin` to the path and puts first the directory that has all of `mke2fs`, `debugfs` and `e2fsck`, so that the three come from one installation.
* [ovmf_path.sh](setup/ovmf_path.sh) finds OVMF for `doctor.sh` and the UEFI suites: `RACH2_OVMF_CODE` and `RACH2_OVMF_VARS`, else QEMU's own pair, else the `ovmf` package's. It leaves out the Secure Boot builds, since the pinned GRUB is unsigned.
* [hostcheck.c](setup/hostcheck.c) tries what a run needs before `tests/run` builds: for any suite but `style` and `host`, TCP and UDP sockets bound on loopback and a Unix socket in the run's directory. `RACH2_HOSTCHECK` substitutes another checker.

### Running on the testing host: `dev/testing.sh`

[testing.sh](dev/testing.sh) puts the owner's tree in the copy at `/home/testing/rach2`, or the copy that `RACH2_TESTING_COPY` names, and runs commands there, as the [testing skill](../.agents/skills/testing/SKILL.md) requires:

    tools/dev/testing.sh sync
    tools/dev/testing.sh run tests/run all
    tools/dev/testing.sh get PATH...
    tools/dev/testing.sh sh
    tools/dev/testing.sh wait NAME [SEEN]

`sync` makes the copy match this tree, by content, except what `.gitignore` lists, of which it sends only the Doom WAD, so the host keeps its own `build/` and vendor trees. It refuses while a run or any process but a person's shell works in the copy, or another caller has the copy's lock. `run` syncs and runs the command, one string to the remote shell, with `RACH2_QEMU_ACCEL=tcg`. `get` copies paths of the copy here, such as products only Linux builds. `wait` watches a run started as `/home/testing/NAME.sh` that writes its result lines to `NAME.out`, and returns on a new line, the run's end, 15 minutes with nothing changed under `NAME/`, or 100 minutes. The script needs rsync 3; macOS's openrsync would delete the host's build and vendor trees.

### The worker and manager loops: `flow/`

The work and the management of the process run as loops: shell scripts that start one agent session after another, so that no session waits on a person and each piece of work is a process that ends. [worker.sh](flow/worker.sh) runs worker sessions in the clone it is in, as the account that clone's `gh` and git use, and [manager.sh](flow/manager.sh) runs the manager's checks. Each runs in a detached tmux session, from a clone of its own. An account on the testing host runs one, with its state in `~/flow`:

    tmux new-session -d -s worker 'sh ~/rach2/tools/flow/worker.sh'

The owner's computer runs three, with tmux from Homebrew:

    git clone https://github.com/toksaitov/rach2 ~/flow/claude/rach2
    git clone https://github.com/toksaitov/rach2 ~/flow/codex/rach2
    git clone https://github.com/toksaitov/rach2 ~/flow/manager/rach2
    tmux new-session -d -s claude 'RACH2_TESTING_COPY=/home/testing/rach2-claude \
        sh ~/flow/claude/rach2/tools/flow/worker.sh -s ~/flow/claude'
    tmux new-session -d -s codex 'RACH2_TESTING_COPY=/home/testing/rach2-codex \
        sh ~/flow/codex/rach2/tools/flow/worker.sh -a codex -s ~/flow/codex'
    tmux new-session -d -s manager 'sh ~/flow/manager/rach2/tools/flow/manager.sh'

`touch STATE/stop` ends a loop after its current session; `tmux kill-session -t NAME` ends it at once and leaves that session running. A loop needs `claude` or `codex` signed in, `gh` with the `project` scope, git that can push, and perl. A loop on the owner's computer also needs `ssh testing` and a copy of its own on the host, which `RACH2_TESTING_COPY` names, so that the loops do not refuse each other's syncs.

Before a session, a worker loop that has no issue claims one with [board.sh](flow/board.sh), under a lock shared by the loops of one user on one machine, and keeps its number in `STATE/issue`, since the owner's two loops share one account. The session's prompt, [worker.txt](flow/worker.txt), names the issue, and the session builds, checks and pushes as `AGENTS.md` says; the loop gives the next session the same issue until it is closed or handed back. Claude Code sessions run at high effort, cannot schedule a wakeup, and wait for their helpers; Codex sessions run `gpt-6.1-sol` at high effort without a sandbox, as Claude Code's run with permissions bypassed. The loop stops a session after six hours, with every process in its group, waits fifteen minutes after a usage limit, backs off after sessions that end within two minutes, and slows after three sessions that leave one issue open. It runs from a copy of `tools/flow` taken from `origin/master` and restarts when master's copy changes, so that a change to the loops reaches every machine at its next session.

The manager loop starts a headless Claude Code check with [manager.txt](flow/manager.txt) and the last lines of `STATE/manager.log`, stops a check still running after forty minutes, and starts the next thirty minutes after the last ended. A check reads the process through [status.sh](flow/status.sh), which reads the loops that [workers](flow/workers) lists, and warns the owner of an incident with [notify.sh](flow/notify.sh): a push to the phone from an interactive Claude Code session with Remote Control, the only kind that sends one. That session waits two and a half minutes before the push, because Claude Code sends none while its terminal was just typed in, and the prompt a session starts with counts as typing; the owner can then open the session from the phone.

### ci: `ci/`

[ci.yml](../.github/workflows/ci.yml) starts on each push to `master`. One core run goes at a time, never cancelled, and only the newest push waits for it. The run takes the newest `master`, passes over a commit that has a `ci/core` status, and runs [run.sh](ci/run.sh) `core` on i686 and x86-64, one runner each, within 180 minutes: `make`, then `tests/run style`, `host` and `core`. Each architecture's output, failures and kept serial logs are uploaded as `core-ARCH` for 14 days. [report.sh](ci/report.sh) sets the statuses `ci/core/ARCH` and `ci/core`, files a failure as a `bug` issue titled `ci: core: WHAT fails on ARCH` with the commits since the last pass and the `gh run download` command, or comments on that issue when it is open, and opens or closes the `hold` issue. A new issue with a known last pass starts [bisect.yml](../.github/workflows/bisect.yml): [bisect.sh](ci/bisect.sh) tests the failing commit again, halves the first-parent commits since the last pass, and comments with the commit that fails while its parent passes, an unresolved range, or a failure that did not repeat, labelled `intermittent`. An unfinished run is reported as `unfinished-core` or `unfinished-night`; bisection repeats it through `run.sh`, including the build. [night.yml](../.github/workflows/night.yml) runs `run.sh night` at 19:00 UTC on one runner, i686 then x86-64, every suite of each with `bigmem` last and alone, and reports under `ci/night/ARCH`. [AGENTS.md](../AGENTS.md) says what a person does with each result.

### The style check: `verify/`

`tests/run style` runs [check.sh](verify/check.sh) over the in-house tree: the kernel, the KRFFS tools, the utilities, the shell, the tests and the tools, never the vendored projects. `make test` runs it first, and in a set the runner runs it before the pool and starts no pooled suite when it fails.

    tests/run style
    ARCH=x86_64 tests/run style

`ARCH`, i686 unless set, selects the kernel that step 1 builds and steps 19 and 20 read, and the musl that step 6 links; steps 2 and 18 read both architectures in every run. `tests/run` builds the components first. `sh tools/verify/check.sh [FILE...]` runs the checks alone on a tree that `make` has built, and adds each FILE to the format check of step 5.

Each step prints a header, `---- N/22 NAME ----`, and then PASS, FAIL or SKIP with a count. The logs go to a private directory under `${TMPDIR:-/tmp}`, whose path the first line prints, and are kept. The last line is `verify-style: PASS` or `verify-style: FAIL`, and the exit status is 0 only for PASS.

Three steps depend on a tool's version: cppcheck (2), clang-format (5) and clang (19). Each is judged only by the version that [setup/judges](setup/judges) records, the testing host's; another version reports what it found and decides nothing. Without sparse step 20 is skipped, and without pmccabe step 21 does not judge complexity. A run elsewhere can therefore pass where the testing host's fails.

Before the numbered steps the script runs five checks: [verify_temp_test.sh](../tests/system/verify_temp_test.sh), on the script's own log directory; [probecheck.sh](verify/probecheck.sh) and its test [probecheck_test.sh](../tests/system/probecheck_test.sh); `tests/system/probe_faults.sh --check`; and [faulttexts.sh](verify/faulttexts.sh), which runs every fault script with `--texts` to find each fault's text in its source. `probecheck.sh` requires every guest program either architecture builds to be in the default suite's list, in `tests/probe-suites` with a suite of `tests/registry`, or in `tests/probe-helpers` with a reason. A preflight then requires `i686-elf-gcc` and `i686-elf-readelf`, which the link-dependency test of step 6 runs under either architecture, `x86_64-elf-gcc` and `x86_64-elf-readelf` as well when `ARCH=x86_64`, `cppcheck`, `shellcheck`, `clang-format` and `clang`, and a missing one fails the run. The steps are:

1. The kernel build, `make -C rach2`, under `-Werror`.
2. cppcheck at the warning, style, performance and portability levels, with the exceptions in [cppcheck.suppress](verify/cppcheck.suppress): at C89 over the i686 kernel and the guest programs that share its headers as 32-bit code, and over the x86-64 kernel as 64-bit code; at C99 over the host programs. The lists come from [srclists.sh](verify/srclists.sh). A cppcheck that fails to run fails the step under any version.
3. ShellCheck over every script that [scripts.sh](verify/scripts.sh) lists, with SC1091 off. The scripts at `rach2/*.sh` and `krffs/*.sh` also waive SC2034 and SC2086. An empty list fails.
4. Conventions. In the C files and headers: no `//` comment, no byte other than tab, newline, carriage return and printable ASCII (by [asciiscan.c](verify/asciiscan.c)), a guard in every header (`#ifndef` or `#pragma once`), and no `sti; hlt` outside `sys/arch/*/cpu.c` and no call of `arch_idle` or `arch_idle_masked` outside those files and `sys/proc/sched.c`, so that every idle wait is counted as idle. In the scripts: no `--` before a process group in a `kill`, which dash refuses, and a `finished` flag in each script under `tools/` or `krffs/`, and in `tests/system/manual_probes.sh` and `tests/system/accept/su_command.sh`, that sets both `-e` and `-u` and traps EXIT, since bash 3.2 exits 0 when `set -u` stops such a script.
5. `clang-format --dry-run --Werror` over every in-house C file and header. The commits that reformatted the tree are listed in [blame-ignore-revs](verify/blame-ignore-revs), which `git config blame.ignoreRevsFile tools/verify/blame-ignore-revs` makes blame skip.
6. The user ABI. [build_inputs_test.sh](../tests/system/build_inputs_test.sh) first requires a build to relink each program whose library, startup object or linker script changed. Then a program linked statically against ARCH's musl must be an `EXEC` with no `INTERP`: ELF32 Intel 80386 with a load segment at 0x08048000 on i686, ELF64 X86-64 with one at 0x400000 on x86-64, and the entry point in that region. Where that musl is not installed, the step fails and names `make musl`, which builds it for ARCH.
7. The source lists and the architecture layer. [check_sources.sh](verify/check_sources.sh), which `make check-sources` also runs, compares `C_SRC` with the kernel's C files on disk; [guestcheck.c](verify/guestcheck.c) compares the guest makefiles with the host build; no rule in `tests/system/suites.mk` may make an image, disk, ISO or stick; every in-house C source must be in a compilation database (below); and `make -C rach2 check-arch` runs [archcheck.sh](verify/archcheck.sh) over the kernel's shared code.
8. Text. [textcheck.sh](verify/textcheck.sh) refuses non-ASCII bytes, a spaced `--` used as a dash, `-` bullets in Markdown and `Phase N` or `Part N` tags, and in the component documents (every in-house `Readme.md` and `NAME.Readme.md` but the front page) a `##` heading out of the order of `AGENTS.md`'s Documentation section or a citation of a milestone by number; [prosecheck.c](verify/prosecheck.c) then runs `columns` (comment lines at 80 columns), its self-test, and `banned` (the terms of the register rule in `AGENTS.md`).
9. [fmtcheck.sh](verify/fmtcheck.sh): every `klog`, `console_printf` and `k_printf` format uses only conversions `k_vprintf` has.
10. The runner's scoring rule and the harness's escape decoder: `runtests --selftest` and `harness --selftest`.
11. The set-id guard: `setidcheck --selftest`, then every image manifest against `inspect/setid.allow`.
12. Reproducibility. The KRFFS tools are built, and [check_repro.sh](verify/check_repro.sh) stages one manifest twice and requires the same bytes, with directory entries in name order.
13. The checkers' self-tests: `elfcheck`, `cfgcheck`, `gen_compile_commands.sh` and `suitecheck.sh` with `--selftest`, and the tests [recovery_key_test.sh](../tests/system/recovery_key_test.sh), [dryrun_test.sh](../tests/system/dryrun_test.sh), [build_rules_test.sh](../tests/system/build_rules_test.sh) and [style_scope_test.sh](../tests/system/style_scope_test.sh). The last runs `scripts.sh` and `textcheck.sh` over a private tree and requires each kind of file they claim to be read.
14. `prosecheck devlog`: `dev-log/Readme.md` lists exactly the finished milestones, in the directories' order.
15. The registry. Each row of `tests/registry` has a `run-` target in `tests/system/suites.mk` and each target a row, no suite appears twice, and `tests/system/suites.md` is the output of `tests/run inventory`, with a sentence in `suites.mk` for every suite. `rach2/syscalls.md` must be what `make syscalls` writes: `mancheck --calls` reads the two kernels' tables and numbers as step 18 does, and prints a row for each call with its i386 and x86-64 numbers and its page. `man/programs.md` is the output of `make programs-md`.
16. Every `build/` product a `run-` target depends on is built by `prepare-all`, read from make's database (`make -pn prepare-all`).
17. [suitecheck.sh](verify/suitecheck.sh): no suite sleeps for a number of seconds or bounds a duration, except at an interval that `tests/system/intervals` lists with its reason.
18. `make mancheck` runs [mancheck.c](verify/mancheck.c) once for each architecture: every staged program, BusyBox applet and system call has a manual page or an alias, and every page passes `mandoc -T lint`.
19. clang `-fsyntax-only` with `-Wcast-align -Wconditional-uninitialized -Wshadow-all -Wtypedef-redefinition -Wcomma` over ARCH's kernel sources.
20. sparse, with `__CHECKER__` defined, over the same sources, which checks the `__user` address-space annotations. It runs without `-Wdecl` and `-Wnon-pointer-null`; [exceptions.md](verify/exceptions.md) gives the reasons.
21. [sizecheck.c](verify/sizecheck.c) over every in-house C file: at most 100 lines in a body, 6 parameters, a nesting depth of 4 and a cyclomatic complexity of 25 as pmccabe counts it, except for the functions [sizes.reasons](verify/sizes.reasons) lists, each with the ceiling it may not pass. `sizecheck --report` prints every function's measures.
22. [linkcheck.sh](verify/linkcheck.sh): every link in the Markdown files git tracks outside `dev-log/`, and in the milestone checklists under `dev-log/`, reaches a file that exists, and every anchor a heading in it.

The other files of `verify/` are not part of the style check:

* [analyze.sh](verify/analyze.sh) is `make analyze`: GCC's `-fanalyzer` over the kernel and the guest programs and cppcheck at its exhaustive level, either of which fails the run on a finding, and two advisory reports, clang-tidy over the kernel and the sections the linker drops as unused. `--fast` leaves out the exhaustive cppcheck. The logs are in `build/verify/analyze/`.
* [coverage.sh](verify/coverage.sh) `[NAME...]` builds each host test again with `--coverage` and prints, for every kernel source the test compiles, the lines executed and the branches taken. No suite runs it.
* [sha256.c](verify/sha256.c) prints the SHA-256 digest of each file. The build makes it as `build/tools/verify/sha256`, and the vendor scripts compile it on first use, so the build needs neither `sha256sum` nor `shasum`.
* [prose_report.sh](verify/prose_report.sh) compares the rates of watched phrases in the tree's prose with those in the pinned upstream sources, and never fails. `--baseline` and `--fixture` compare the counts with the records [prose_baseline.txt](verify/prose_baseline.txt) and [prose_upstream.txt](verify/prose_upstream.txt), and `--write` rewrites a record.

### The commit-msg hook: `git-hooks/`

[commit-msg](git-hooks/commit-msg) refuses a commit message with an attribution line, which the Git section of `AGENTS.md` forbids: a `Co-Authored-By:`, `Claude-Session:` or `Signed-off-by:` line in any letter case, a "Generated with Claude Code" line with or without an emoji before it, or a trailer whose value ends in `<noreply@anthropic.com>`. It prints `commit-msg: refused: attribution on line N: TEXT` and exits 1. Each pattern starts in the first column, so a sentence that mentions a trailer passes, and so does the diff that `git commit -v` adds. A clone turns the hook on with

    git config core.hooksPath tools/git-hooks

[.claude/settings.json](../.claude/settings.json) runs that command, with `--local`, when a Claude Code session starts, and sets `blame.ignoreRevsFile` in the same way. It also turns off the attribution Claude Code adds by default: `includeCoAuthoredBy` is false, the commit and pull request attributions are empty, and `sessionUrl` is false.

### The compilation database

[gen_compile_commands.sh](dev/gen_compile_commands.sh) writes `compile_commands.json` at the root of the tree, from which clangd reads each file's flags:

    sh tools/dev/gen_compile_commands.sh

GCC has no `-MJ`, so the script takes the commands from the build. It dry-runs thirteen units with `make -Bn`, or a build script's print mode, joins continued recipe lines, and keeps each command whose first word is the unit's compiler, one entry for each C source on it. The units are the four i686 kernels (`rach2`, `rach2-recovery`, `rach2-metal`, `rach2-harness`) and the x86-64 kernel (`rach2-x86_64`); the guest programs (`tests`, `ish`, `utils`, `musl-programs`, `doom`); and the host programs (`krffs`, `tools`, `host-tests`). Every unit but `rach2-x86_64` is i686's whatever `ARCH` says. Each unit's database is validated line by line and written as `build/compile_commands/UNIT.json`. Their union replaces the root file only when it is valid and has every i686 kernel source. Git ignores the unit databases and the root file. The projects built by their own build systems (musl, BusyBox, Dropbear, TinyCC, pdpmake and e2fsprogs) are not in the database; the Doom engine under `ports/doom/src` is, since the port's Makefile compiles it.

Run the script again after adding or removing a source or changing flags. Step 7 of the style check runs it too, so `tests/run style` leaves a current database, and then runs [dbcheck.sh](verify/dbcheck.sh), which requires every in-house C source to be in a unit's database, to be included by name by a source in its directory, or to be a fixture under `tests/system/fixtures` or `tests/host/fixtures` that a script of that tree refers to. Step 13 runs the generator's `--selftest`, five cases of its joining, selection, escaping and validation.

[.clangd](../.clangd) makes `i686-elf-gcc` the compiler of every entry under `rach2/`, `tests/`, `ish/` and `utils/`, the x86-64 kernel's among them, so that clangd reads the cross compiler's freestanding headers there; the KRFFS tools keep the host compiler their entries give. clangd asks a compiler for its headers only when it is allowed to, so start it with

    --query-driver=**/i686-elf-*gcc

In VS Code, set `"clangd.arguments": ["--query-driver=**/i686-elf-*gcc"]`; other editors pass the same option.

### Artifact checkers: `inspect/`

The programs and scripts under `inspect/` judge a built artifact: a kernel or a program, a staged image, a configuration a disk depends on. `make` builds the C programs into `build/tools/inspect/` and runs each check where its artifact is made; stamps in the root `Makefile` run some checks again when the checker changes, and remove an artifact that fails. Each file's opening comment states its whole contract.

* [elfcheck.c](inspect/elfcheck.c) judges ELF files by what Rach2 loads. `elfcheck [--arch i686|x86_64] FILE...` applies the loader's rules to user programs and refuses a dynamic one: `i686`, the default, takes ELF32 i386 programs in `[0x02000000, 0xBFFFF000)`, and `x86_64` takes ELF64 x86-64 programs in `[0x10000, 0x00007FFFFFFFE000)` with file offsets held to 32 bits. `elfcheck --kernel [--tcc] FILE` judges a kernel by what QEMU's `-kernel` and GRUB read: an ELF32 kernel by its Multiboot header and two load segments, an ELF64 kernel as the header's a.out kludge loads it; `--tcc` accepts a kernel TinyCC linked. `--selftest` runs the refusal cases of both. `rach2/Makefile` runs `--kernel` on every kernel it links, and `make check-tcc`, `check-pdpmake` and `check-e2fsck` run the user check on the programs those ports stage.
* [check_link.sh](inspect/check_link.sh) `LIB_OR_OBJECT...` requires that musl's `libc.a`, TinyCC's `libtcc1.a` and the startup objects staged for the guest compiler leave no symbol undefined between them, so that a program linked on Rach2 needs no libgcc. It reads symbols with `CHECK_LINK_NM`, `i686-elf-nm` when unset; the callers pass `x86_64-elf-nm` for x86-64. `vendor/tcc/build_tcc.sh` runs it, and the root Makefile's TinyCC stamp runs it again when TinyCC or the script changes.
* [setidcheck.c](inspect/setidcheck.c) is the guard on set-id bits: privilege in an image comes only through the paths declared in [setid.allow](inspect/setid.allow), whose opening comment defines its directives. `setidcheck [MANIFEST...]` checks manifests, all of `krffs/manifests/` when none is given. `setidcheck --image FILE --manifest PATH` compares every node of a finished KRFFS image with the declarations, and `--ext2 FILE` in place of `--image` does so for an ext2 volume with a reader of its own, which shares no code with the kernel's driver and refuses a volume outside the profile `mkext2.sh` writes. `--applets` checks BusyBox's applet tables and configurations and the init binaries. In an x86-64 build it reads the host files a manifest gives through `RACH2_PATH_MAP`, as the stager does. `make check-setid` runs the sweep and `--applets`; `krffs/mkimage.sh`, `krffs/mkdisk.sh` and `image/mkext2.sh` check what they build; and the root Makefile's `.checked` stamps check the test, os-simple and os-real images again when the guard or its list changes.
* [binfind.c](inspect/binfind.c) `[--offset] STRING FILE` reports whether a file contains a byte string, by its exit status or with `--offset` by the first match's offset, and `binfind --replace OLD NEW FILE` overwrites the first match with a string of the same length. `make check-tcc` reads TinyCC's four refusal messages out of the shipped compiler with it, and the `sealed`, `menu`, `install` and `drill` suites find and mark bytes in kernels and disks with it.
* [check_diskpolicy.sh](inspect/check_diskpolicy.sh) `KERNEL read-only|read-write` requires a kernel to contain the banner of the given disk policy and not the other's, and [check_recovery.sh](inspect/check_recovery.sh) `KERNEL recovery|ordinary` does the same for disk authority. They catch a kernel built into the wrong tree; the `diskpolicy` and `authority` suites test what the kernels write. `make rach2-metal` requires the metal kernel to be read-only, `make rach2-recovery` requires the recovery kernel to have recovery authority and the read-write policy and the ordinary kernel ordinary authority, and `image/mkiso.sh` checks the kernel it stages.
* [cfgcheck.c](inspect/cfgcheck.c) checks two configurations a sealed disk depends on. `cfgcheck grubembed CONFIG` requires exactly the four commands of [grub-embed.cfg](image/grub-embed.cfg), in order, so that GRUB's core opens the boot partition's menu first and the recovery partition's only when that fails; `mkdisk_grub.sh` runs it for every disk. `cfgcheck sealednet ADDRESS PREFIX MASK GATEWAY SCRIPT` requires the sealed disk's fallback address, prefix, mask and gateway to describe one subnet and `SCRIPT` to apply exactly those numbers; `make rach2-sealed-disk` runs it. `--selftest` runs both on their fixtures.
* [growset.c](inspect/growset.c) compiles [utils/growset.c](../utils/growset.c), which computes the sectors a grow of an ext2 root writes, for the host; [growset(8)](../man/man8/growset.8) describes its use. `make` builds it through a rule in `tests/system/suites.mk`, and the `grow` and `installer` suites and `dev/machine_grow_plan.sh` run it.

### Disk, stick and ISO builders: `image/`

The scripts under `image/` assemble boot media on the host with mtools, xorriso, e2fsprogs, the C programs beside them and the GRUB images pinned in [vendor/grub/](../vendor/grub/Readme.md); no GRUB toolchain, container or loop mount is needed. `mk/images.mk` builds the C programs into `build/tools/image/`. Each script writes its product under a temporary name and renames it only after every check passes. The products are under `build/images/` for i686 and `build/x86_64/images/` for x86-64.

#### ext2 roots

    sh tools/image/mkext2.sh [--overlay PROGRAM] MANIFEST OUTPUT [SIZE_MB [BLOCKS_PER_GROUP [BYTES_PER_INODE]]]

[mkext2.sh](image/mkext2.sh) builds an ext2 volume from a manifest: `stageimage.krffs --tree` stages it into a directory, the host's `mke2fs` formats the directory on the one profile the kernel's driver writes, `debugfs` sets each node's owner, mode and set-id bits from the manifest, and the volume must pass `e2fsck -fn` and `setidcheck --ext2`. The size is 1024 MiB unless given, sparse on the host. A group size and an inode ratio replace mke2fs's choices; a grow keeps the inodes of a group, so the ratio decides how much a grow to a large disk writes. The times, the UUID and the directory hash seed are fixed, so one manifest makes one image. `--overlay PROGRAM` runs PROGRAM on the staged directory before formatting, for a system a manifest cannot describe. `make` builds `os-real.ext2` this way, and `make os-real-gnu-ext2` the GNU system's root with `--overlay vendor/gnu/install_root.sh`.

#### GRUB disks

    RACH2_DISK_RECOVERY_MB=64 RACH2_DISK_EXT2_MB=1024 RACH2_DISK_EXT2_IMAGE=ROOT.ext2 \
        sh tools/image/mkdisk_grub.sh [KERNEL [OUTPUT]]

[mkdisk_grub.sh](image/mkdisk_grub.sh) builds a disk that boots by BIOS, and on a GUID table by UEFI as well. KERNEL is the built kernel unless given, and OUTPUT is `os-real-grub.disk` in the image directory, `os-real-grub-with-key.disk` in a tree with a recovery key. [disk(7)](../man/man7/disk.7) and [rach2-install(8)](../man/man8/rach2-install.8) describe the partitions, and [bootctl(8)](../man/man8/bootctl.8) the kernel slots, the menu and the environment block. From the first MiB the script lays out the FAT32 boot partition, the FAT32 recovery partition, one spare MiB, the ext2 root and one spare MiB; on a GUID table the BIOS boot partition is sectors 34 through 2047. The kernel slots are 2 MiB, and a larger kernel is refused. Every disk has the disk GUID `1acef00d-0f1e-4d2c-8b3a-c0ffee000001`, or the MBR identifier `1acef00d`, and the FAT serials and labels `1acef00d` and `RACH2BOOT` on the boot partition and `1acef003` and `RACH2RECOV` on recovery. The script writes the table with `mkgpt` or `mkmbr` and GRUB with `grubembed`, and checks the embedded configuration with `cfgcheck` and the boot files' cluster chains with `fatchains` before it publishes the disk.

It reads eleven settings from the environment:

* `RACH2_DISK_TABLE`: `gpt`, the default, or `mbr`.
* `RACH2_DISK_BOOT_MB`: the boot partition's size, 33 by default on a GUID table and 16 on an MBR. An EFI system partition has one sector per cluster, and the script refuses one outside 65525 to 131070 clusters, the least UEFI firmware reads as FAT32 and the most `bootctl` walks: 33 to 64 MiB.
* `RACH2_DISK_CLUSTER`: sectors per cluster on the recovery partition and on an MBR's boot partition; unset, mformat chooses.
* `RACH2_DISK_RECOVERY_MB`: the recovery partition's size, required.
* `RACH2_DISK_RECOVERY_KERNEL` and `RACH2_DISK_RECOVERY_IMAGE`: the recovery kernel and RAM image staged there; without them the recovery entry's title says the system is not built.
* `RACH2_DISK_EXT2_MB` and `RACH2_DISK_EXT2_IMAGE`: the root partition's size and the image `mkext2.sh` built for it, which must fit; both required.
* `RACH2_DISK_APPEND`: kernel arguments added to every entry.
* `RACH2_DISK_DHCP_APPEND`: the network arguments, kept apart because the kernel refuses an address given twice.
* `RACH2_DISK_HELLO`: a file copied to `/hello.txt` on the boot and recovery partitions.

`RACH2_ARCH`, or `ARCH`, selects the build tree, and `SOURCE_DATE_EPOCH`, 1700000000 unless set, fixes the FAT timestamps.

`make rach2-sealed-disk` builds the disk the installer writes, `rach2-sealed.disk` (`rach2-sealed-with-key.disk` with a key): a 64 MiB boot partition, a 64 MiB recovery partition with the recovery system, and a 1024 MiB root, 1155 MiB in all. Its rule first runs `cfgcheck sealednet` over `SEALED_ADDRESS`, `SEALED_PREFIX`, `SEALED_MASK` and `SEALED_GATEWAY`, which may be set on the command line, and `krffs/rescue-static.script`. `make os-real-grub-disk` builds a disk of the same shape with the os-real root for `rach2/run_from_disk.sh`, and the suites build their own disks by rules in `tests/system/suites.mk`.

The other GRUB files:

* [grubembed.c](image/grubembed.c) `DISK BOOT.IMG CORE.IMG` installs GRUB on a disk image, which `grub-bios-setup` does only on a device: the boot image's first 440 bytes go into the MBR, which keeps its disk identifier and table, and the core into the sectors after the MBR on an MBR disk or into the BIOS boot partition on a GUID one. The core must fit the window `utils/grubslot.h` lays out, so that `bootctl grub` can replace it later.
* [grub-embed.cfg](image/grub-embed.cfg) and [grub-efi-embed.cfg](image/grub-efi-embed.cfg) are the configurations built into the BIOS cores and the EFI image, which [vendor/grub/Readme.md](../vendor/grub/Readme.md) describes. A build fails when either differs from the one the pinned images were made with.
* [mkgrubcore.sh](image/mkgrubcore.sh) `A|B OUTPUT` copies and verifies one of two pinned cores that print `GRUBCORE=A` or `GRUBCORE=B` on the serial line; the `grub` suite installs them in turn.
* [fatchains.c](image/fatchains.c) `IMAGE PATH...` prints the runs of clusters of files on a FAT16 or FAT32 image, as `mshowfat` does.
* [ubuntu-image.txt](image/ubuntu-image.txt) gives by digest the Ubuntu container in which `vendor/grub/regen_grub.sh` makes the GRUB images; no build or suite uses a container.

#### Partition tables

* [mkmbr.c](image/mkmbr.c) `[--lba] [--id HEX] DISK START:SIZE:TYPE[:boot|:logical] ...` writes an MBR's table, with START and SIZE in MiB, or sectors with `--lba`, at most four primary and sixteen logical partitions, and `--id` the disk identifier. It writes only the table, the signature and the identifier, so the boot code stays.
* [mkgpt.c](image/mkgpt.c) `[--seed N] [--guid GUID] [--damage-primary] DISK START:SIZE:KIND[@INDEX][=LABEL] ...` writes a GUID table with its protective MBR, both headers and both arrays, with START and SIZE in sectors, KIND `fat`, `data`, `linux` or `bios`, and at most fifteen entries. The GUIDs are made from the seed, so one seed gives one set of identities.

Both print a table with `--list DISK`. The disk builder and the suites write their tables with them.

#### Sticks

A stick image is a hybrid ISO from `mkiso.sh`, padded to a 1 MiB boundary and followed by an exFAT volume that `mkexfatfixture` ([tests/host/](../tests/host/Readme.md)) writes. `mkmbr --lba` gives the MBR three entries around GRUB's boot code: the ISO's file system, of type `0x17`; the volume, `0x07`; and the ISO's EFI boot image, `0xEF`, which UEFI firmware boots a USB device from. [stickcheck.c](image/stickcheck.c) `IMAGE VOLUME-START VOLUME-SECTORS`, linked with the kernel's `mbr.c` so that it applies the kernel's rules, judges the whole image, and `mkexfatfixture --check` walks the volume.

* [mkstick.sh](image/mkstick.sh) `ISO OUT MKEXFATFIXTURE MKMBR STICKCHECK` puts a 16 MiB volume after the metal ISO. `make rach2-stick-image` builds `rach2-stick.img`, which the `usbdisk` suite boots.
* [mkinstallstick.sh](image/mkinstallstick.sh) `ISO DISK OUT MKEXFATFIXTURE MKMBR STICKCHECK [EXTRA [FREE-MIB]]` puts after the recovery ISO a volume with the disk image, a manifest of its name, length and SHA-256, the EXTRA file, GRUB's EFI image with its digest, and FREE-MIB of free space, 4 unless given; [rach2-install(8)](../man/man8/rach2-install.8) describes what the installer reads there. `make rach2-install-stick` builds `rach2-install-stick.img` with the sealed disk, its root as EXTRA and 256 MiB free.

#### ISOs

    RACH2_ARCH=x86_64 sh tools/image/mkiso.sh IMAGE

[mkiso.sh](image/mkiso.sh) builds `IMAGE.iso` beside `IMAGE.krffs` for `test`, `os-simple`, `os-real`, `rach2-metal`, `rach2-recovery` or `rach2-recovery-with-key`, with xorriso and the pinned El Torito, hybrid and EFI images; GRUB loads the kernel and the image as its module. Every ISO boots by BIOS from a CD or a USB device, and by UEFI through a FAT image with GRUB's EFI image, appended as the MBR's second partition. The script requires the metal kernel to be read-only and every other kernel read-write. The metal menu's first entry sets the address `RACH2_METAL_IP` and gateway `RACH2_METAL_GATEWAY`, 192.168.0.105/24 and 192.168.0.1 unless set; the recovery ISO's one entry takes an address from DHCP. `make isos` builds the five ISOs, and `make test-iso`, `os-simple-iso`, `os-real-iso`, `rach2-metal-iso` and `rach2-recovery-iso` one each; `rach2/run_from_iso.sh` boots one in QEMU.

#### Names of keyed artifacts

[keyname.sh](image/keyname.sh) `PATH` refuses an artifact whose name disagrees with the tree: a name with `-with-key` while `krffs/recovery_authorized_keys` is absent, or one without it while the key is present. The rules of the recovery image, the recovery ISO, the sealed disk, the os-real GRUB disk and the install stick run it, since Make chooses those names when it starts. [krffs/Readme.md](../krffs/Readme.md) describes the recovery key list, and [the section below](#working-an-installed-machine-from-the-host) `make release`.

### The console's font: `font/`

    sh tools/font/mkfont.sh unifont_all-18.0.01.hex.gz > rach2/sys/display/vga_font.c

[mkfont.sh](font/mkfont.sh) writes the console's font from GNU Unifont 18.0.01. It refuses any file but `unifont_all-18.0.01.hex.gz`, by its SHA-256, whose signature was checked with `gpgv` and the GNU keyring; the script's opening comment gives where to fetch it and the signing key. The output is committed, since a build on Rach2 has no network to fetch the font from, and no build step runs the script. [rach2/sys/display/Readme.md](../rach2/sys/display/Readme.md) describes the tables it writes, and [console(4)](../man/man4/console.4) the characters the console shows.

### Working an installed machine from the host

The manual has the procedures that install and update a machine: [rach2-install(8)](../man/man8/rach2-install.8), [recovery(7)](../man/man7/recovery.7), [bootctl(8)](../man/man8/bootctl.8), [reflash(8)](../man/man8/reflash.8) and [disk(7)](../man/man7/disk.7). This section is what the host does around them; the [machine skill](../.agents/skills/machine/SKILL.md) has the check before a write.

Each root has an SSH host key of its own, a RAM root a new one at each boot, so give each system its own known-hosts file:

    ssh -i krffs/rach2_demo_ed25519 -o UserKnownHostsFile=/tmp/k-NAME \
        -o StrictHostKeyChecking=accept-new root@ADDRESS

Write `-i KEY` before the destination: after it, ssh takes the two words as part of the remote command, offers its default identities, and the login fails. Root logs in by key unless [sshd.conf(5)](../man/man5/sshd.conf.5) permits a password; `su` from another account is the other way in. Dropbear has no sftp server, so a file goes over as `ssh ... 'cat > /tmp/f' < f` and comes back as `ssh ... 'cat /tmp/f' > f`; the GNU system's root runs OpenSSH.

The development keys are committed, `krffs/rach2_demo_ed25519` for root and `krffs/rach2_test_ed25519` for the test account, so every image from any clone trusts them, and any host reaches any development machine on the private network. Git does not keep their mode, and ssh refuses a private key others can read; `krffs/gen_demo_key.sh`, which the build runs, and `tests/run` set it to 0600. A machine for any other network is built after `RACH2_REPLACE_KEYS=1 sh krffs/gen_demo_key.sh`; [krffs/Readme.md](../krffs/Readme.md) describes the key files.

A recovery image answers SSH only when built in a tree that has `krffs/recovery_authorized_keys`, which `make recovery-key` and `tests/run` create. To let in a key other than the development key, copy only its `.pub` file to the build host and run two commands, since Make chooses the `-with-key` artifact names when it starts:

```sh
make recovery-key RECOVERY_EXTRA_KEYS=/path/operator.pub
make rach2-install-stick
```

The second builds `build/images/rach2-install-stick-with-key.img`. `make no-recovery-key` removes the key and every artifact built with it, and `make release`, the install stick and recovery ISO for somebody else, is refused in a tree that has one.

With `rach2.netconsole` on the boot line, the kernel broadcasts its log in Ethernet frames of type `0x88b5` ([the network Readme](../rach2/sys/network/Readme.md)). `sudo tcpdump -i en0 -U 'ether proto 0x88b5' -w FILE` on the Mac keeps them, the only record of a panic on a machine with no serial port.

`build/images/rach2-install-stick.img`, written to a whole stick, boots recovery and has the installer's images; `build/images/rach2-stick.img` boots the metal system as a test. `tests/system/accept/metal_accept.sh HOST [PORT] [STICK-SERIAL]` is the scripted half of acceptance ([the system tests' Readme](../tests/system/Readme.md)).

### Machine measurements and the grow: `dev/`

These scripts measure an installed machine over SSH and run the recovery grow. Each script's header states its contract.

* [machine_identity.sh](dev/machine_identity.sh) runs before every measurement, as `RACH2_MEASURE_USER`, root by default. It lets the measurement go on only when the machine booted the slot `RACH2_MEASURE_SLOT` gives, `candidate` or `fallback`, has no trial armed, and has the host file `RACH2_MEASURE_KERNEL` in that slot by SHA-256, and logs what the machine reported.
* `machine_load.sh KEY ADDRESS SESSIONS SECONDS [DIR]` starts SESSIONS sessions a second apart, since Dropbear admits five unauthenticated connections from one address at a time. Each runs [machine_load_session.sh](dev/machine_load_session.sh): it copies `/src/rach2` under `/tmp`, builds it with the staged TinyCC and pdpmake for SECONDS of the machine's clock, and prints `SESSION N builds B failed F before-deadline W late L`. `RACH2_LOAD_USER` selects the account.
* [machine_net.c](dev/machine_net.c) times a TCP transfer with the same code on the host, where `make` builds `build/tools/dev/machine_net`, and on Rach2, where `tcc -static` builds it: `local MIB WRITE-BYTES TIMEOUT` on loopback port 27157, or `receive PORT MIB TIMEOUT` and `send ADDRESS PORT MIB WRITE-BYTES` on two machines, each stopped at 180 seconds.
* `machine_net_in.sh KEY ADDRESS REMOTE-PROGRAM SENDER MIB WRITE-BYTES TIMEOUT DIR` sends one transfer to the machine's port 27158 and keeps both ends' logs, the programs' digests and the interrupt counts under DIR. `RACH2_NET_USER` selects the account and `RACH2_NET_KNOWN` a known-hosts file.
* `poll_kernels.sh` builds the current kernel and one whose `poll` removes its waiters before the halt, in a private copy of the tree, and prints each as `RACH2_MEASURE_KERNEL=PATH`. It needs `build/tests/system/runutil` and `build/tools/inspect/elfcheck`.

An interval read on the machine can be short by timer ticks lost while interrupts were masked, which inflates a rate. For an independent bound, time the whole command with the host's `/usr/bin/time -p`, which also counts SSH and setup. Try 1 MiB before a long transfer.

The recovery grow writes a seed root to the installed disk, times `growroot --measure` growing it, and restores the disk. On the host:

    sh tools/dev/machine_grow_plan.sh SEED DISK-SECTORS SERIAL MBR-ID DIRECTORY

SEED is a clean ext2 root such as `build/images/rach2-metal-disk.ext2`. The disk must have an MBR with 64 MiB FAT partitions at sectors 2048 and 133120 and the root from sector 266240; `save` and `grow` refuse any other layout. The new DIRECTORY receives the plan, the sector ranges that the seed's write and the grow change, as the host's [growset](../man/man8/growset.8) computes them, the seed's digest, a copy of `machine_grow.sh`, and the digests of the recovery programs of `ARCH`, i686 by default. In recovery, as root, with the plan copied under `/tmp`:

    sh /tmp/grow-plan/machine_grow.sh save|grow|restore DISK STORE /tmp/grow-plan

STORE is an exFAT volume on another disk with the seed as `seed.ext2`; each action logs to a new `machine-grow.*` directory there, and recomputes the ranges before it writes. `save` copies the ranges and the 64 MiB boot partition to `STORE/saved`; check its `SHA256SUMS` on the host and return that file as `saved/host.sha256`, which `grow` and `restore` require. `grow` unmounts the store while it measures, so close every file on it first. `restore` puts everything back, repeatable after an interruption or power loss.

The install stick's 256 MiB of free space is too little for a full grow. Build the grow's stick with [mkinstallstick.sh](image/mkinstallstick.sh), a tar of the plan as EXTRA, and room for the seed and the saved copy as FREE-MIB, at most 8192; the builder makes the whole volume in host memory. `tests/run grow` rehearses the procedure on a 64 MiB root, and with `RACH2_GROW_SMALL=1` also its interruptions and a full RAM root; run that before a large build.

## Building

The C programs are compiled on the host with the rule text of [`mk/host.mk`](../mk/Readme.md), under `-Werror`, into `build/tools/SUBDIR/NAME`, and serve both architectures. `make` builds them all as `host-tools`, a prerequisite of `all`, and `make print-host-tools` prints their paths. A script that runs a checker names its built path, and the root Makefile builds the program before the rule that runs it. The scripts need no build.

## Tests

* `tests/run style` tests the tools of `verify/` by running them over the tree, and runs the self-tests of `elfcheck`, `cfgcheck`, `setidcheck`, `prosecheck`, `suitecheck.sh` and `gen_compile_commands.sh`, and the scripts `tests/system/verify_temp_test.sh`, `probecheck_test.sh`, `probe_faults.sh --check`, `build_inputs_test.sh`, `recovery_key_test.sh`, `dryrun_test.sh`, `build_rules_test.sh` and `style_scope_test.sh`. `faulttexts.sh` runs `dev/poll_kernels.sh --texts`.
* `tests/run host` runs the self-tests of `elfcheck`, `cfgcheck`, `setidcheck` and `prosecheck` again under the sanitizers, and `tests/system/commit_msg_test.sh` (the hook), `mkext2_overlay_test.sh` (`mkext2.sh --overlay`, and the diagnostic of a failing mke2fs or debugfs) and `machine_measure_test.sh` (`machine_identity.sh`, `machine_load.sh`, `machine_load_session.sh` and `machine_net_in.sh`). Its host tests `grubembed_hosttest.c` and `grubslot_hosttest.c` test the GRUB installer's table reader and window, and `ext2_hosttest.c` the set-id guard's ext2 reader.
* The suites test the image tools by booting what they make: `grub`, `bootorder`, `menu`, `install`, `drill`, `sealed`, `uefi`, `installer` and `grow` boot GRUB disks; `usbdisk` boots the stick; `diskpolicy` and `drill` run `check_diskpolicy.sh` and `check_recovery.sh`; and `grow` runs `dev/machine_grow_plan.sh` and `dev/machine_grow.sh` on a 64 MiB root. `tests/run runner` tests `tests/run`'s use of `hostcheck`.
* The fault scripts `tests/host/elfcheck_mutate.sh`, `tests/system/host_tools_mutate.sh`, `install_stick_mutate.sh` and `rehearse_group_mutate.sh`, and the `--mutate` modes of the tests above, prove that these tests fail on a planted fault.

`tests/system/ci_test.sh`, run by `tests/run host`, checks unfinished reports and bisection with local command fixtures; its `--mutate` mode proves both checks. Nothing tests `ci/run.sh`, the scripts of `flow/`, `dev/testing.sh`, `setup/doctor.sh`, `mkiso.sh` apart from the ISOs the suites boot, `keyname.sh`, `check_link.sh`, `mkfont.sh` or `sha256.c` on their own.
