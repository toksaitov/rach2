#!/bin/sh
#
# prereq_test.sh: tools/setup/prereq.sh reports the missing tools of a
# platform with the exact commands that install them, runs only those the
# account has the rights for, and tools/setup/prereqs names every tool the
# build, the style check and the suites need.
#
# The rows run a private copy of prereq.sh against a fixture recipe whose
# probes look for files under the fixture directory and whose install lines
# make them, with stand-ins on the path for `id`, `sudo` and the Windows
# machine's PowerShell. They check the report and the commands printed for
# an account without administrator rights, with sudo and as root, and on
# Windows unelevated and elevated; what --install runs and leaves; that a
# probe starts in an empty directory of its own and in the tree's root; the
# placeholders; and the refusal of a malformed recipe. The last rows read the
# real recipe with --list: every command of tests/registry, of the build's
# tool check and of the style check is named on linux, macos and windows,
# but for the Linux-only commands named below, which etude's tooling machine
# must provide, with what the machine itself needs named on macos and
# windows; and the Codex version of the linux recipe and of the tooling
# machine is the one the codexapi cases run. The OVMF rows run each POSIX
# platform's real probe with missing, directory, unreadable and readable
# overrides, and with a readable pair found beside a directory of PATH.
#
# With --mutate each fault below is put into the copy and must fail exactly
# its expected rows; the copy is restored before the rows run again at the
# end. --texts puts each fault into the copy and requires only that it
# changes the file, and runs no row.
#
# Usage: sh tests/system/prereq_test.sh [--mutate | --texts | --ovmf]
#
set -u

case ${1:-} in
    '') mode=check ;;
    --mutate) mode=mutate ;;
    --texts) mode=texts ;;
    --ovmf) mode=ovmf ;;
    *) echo 'prereq-test: usage: prereq_test.sh [--mutate | --texts | --ovmf]' >&2; exit 2 ;;
esac
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-prereq-test.XXXXXX") || exit 1
tree=$work/tree
copy=$tree/tools/setup/prereq.sh
mkdir -p "$tree/tools/setup" "$work/bin" || exit 1
cp "$repo/tools/setup/e2fsprogs_path.sh" "$repo/tools/setup/ovmf_path.sh" \
    "$tree/tools/setup/" || exit 1
FIX=$work/fix
export FIX

# The Linux-only commands, which macos and windows do not install: unshare
# and ip make the network namespaces of the codexapi and museapi cases, which
# run Codex and Muse in them, and exfatprogs has no Windows port. On those
# platforms etude's tooling machine supplies them
# (tools/etude/tooling/tools), and the commands it runs on the host are named
# in their recipes.
linux_only_macos='unshare ip codex'
linux_only_windows='unshare ip codex mkfs.exfat fsck.exfat'
tooling_needs='qemu-system-x86_64 qemu-img curl mformat mmd mcopy'

# Stand-ins: `id` answers as the account `fixuser`, uid 0 when FIX/root
# exists; `sudo -n` succeeds when FIX/sudo exists and logs what it runs; and
# `pwsh` decodes the script prereq.sh gives PowerShell and answers the
# fixture's two words: `Have NAME` is true when FIX/have/NAME exists, and
# `Get NAME` makes it.
cat > "$work/bin/id" <<'EOF'
#!/bin/sh
case $1 in
    -u) if [ -e "$FIX/root" ]; then echo 0; else echo 1000; fi ;;
    -un) echo fixuser ;;
    *) exit 1 ;;
esac
EOF
cat > "$work/bin/sudo" <<'EOF'
#!/bin/sh
[ -e "$FIX/sudo" ] && [ "$1" = -n ] || { echo 'sudo: a password is required' >&2; exit 1; }
shift
[ "$*" = true ] && exit 0
echo "sudo $*" >> "$FIX/log"
exec "$@"
EOF
cat > "$work/bin/pwsh" <<'EOF'
#!/bin/sh
[ "$1" = -EncodedCommand ] && [ "$#" -eq 2 ] || exit 2
[ "$(pwd)" = / ] || { echo "pwsh: started in $(pwd)" >&2; exit 2; }
script=$(printf '%s' "$2" | base64 -d | iconv -f UTF-16LE -t UTF-8) || exit 2
case $(printf '%s\n' "$script" | sed -n 1p) in
    *'function Runs('*'function Has('*) ;;
    *) echo 'pwsh: no definitions' >&2; exit 2 ;;
esac
line=$(printf '%s\n' "$script" | sed -n 2p)
case $line in
    *IsInRole*) [ -e "$FIX/elevated" ]; exit ;;
    'if (Have '*') { exit 0 } else { exit 1 }') name=${line#if (Have }; [ -e "$FIX/have/${name%%)*}" ]; exit ;;
    'Get '*) echo "pwsh ${line}" >> "$FIX/log"; : > "$FIX/have/${line#Get }"; exit ;;
esac
echo "pwsh: unknown line: $line" >&2
exit 2
EOF
chmod 755 "$work/bin/id" "$work/bin/sudo" "$work/bin/pwsh" || exit 1

cat > "$work/recipe" <<'EOF'
# A recipe whose probes look under FIX/have.
platform linux

need alpha beta
probe [ -e "$FIX/have/@TOOL@" ]
admin touch "$FIX/have/alpha" "$FIX/have/beta"

need gamma
probe [ -e "$FIX/have/gamma" ]
user touch "$FIX/have/gamma"
user echo @TREE@ @USER@ > "$FIX/subst"

need delta
probe [ -e "$FIX/have/delta" ]
admin touch "$FIX/have/alpha" "$FIX/have/beta"
manual touch "$FIX/have/delta"

need scratch1 scratch2
probe [ -f tools/setup/prereq.sh ] && [ -z "$(ls -A "$PREREQ_TMP")" ] && : > "$PREREQ_TMP/@TOOL@"
user true

platform windows

need wa
probe Have wa
user Get wa

need wb
probe Have wb
admin Get wb

need wc
probe Have wc
manual Get wc
EOF

# fault NAME: put fault NAME into the private copy of prereq.sh, or the
# original for `none`, and fail when a fault leaves the copy unchanged.
fault()
{
    # shellcheck disable=SC2016 # the dollars are in the text sed matches
    case $1 in
        none) cat ;;
        admin-run) sed 's#manual/\*|admin/no) continue ;;#manual/*) continue ;;#' ;;
        manual-run) sed 's#manual/\*|admin/no) continue ;;#admin/no) continue ;;#' ;;
        no-sudo) sed 's#command="sudo \$command"#:#' ;;
        no-reprobe) sed 's#^    probe_all$#    :#' ;;
        shared-scratch) sed 's#rm -rf "\$tmp/probe" \&\& mkdir "\$tmp/probe"#mkdir -p "$tmp/probe"#' ;;
    esac < "$repo/tools/setup/prereq.sh" > "$copy"
    [ "$1" = none ] || ! cmp -s "$repo/tools/setup/prereq.sh" "$copy"
}

# state HAVE...: reset the fixture with the named files present; ROOT, SUDO
# and ELEVATED among them give the account those rights.
state()
{
    rm -rf "$FIX" && mkdir -p "$FIX/have" || exit 1
    : > "$FIX/log"
    for f in "$@"; do
        case $f in
            ROOT) : > "$FIX/root" ;;
            SUDO) : > "$FIX/sudo" ;;
            ELEVATED) : > "$FIX/elevated" ;;
            *) : > "$FIX/have/$f" ;;
        esac
    done
}

# run ARGS...: run the copy against the fixture recipe, writing its output
# and errors to work/out and its status to work/status.
run()
{
    PATH="$work/bin:$PATH" RACH2_PREREQ_POWERSHELL="$work/bin/pwsh -EncodedCommand" \
        sh "$copy" --recipe "$work/recipe" "$@" > "$work/out" 2>&1
    echo "$?" > "$work/status"
}

# expect STATUS: require the status, and the output to be the text on input.
expect()
{
    cat > "$work/want"
    [ "$(cat "$work/status")" = "$1" ] && cmp -s "$work/want" "$work/out" && return 0
    { echo "status $(cat "$work/status"), wanted $1; output:"; cat "$work/out"; } > "$work/why"
    return 1
}

# report PASS...: the probe lines of the linux fixture with the named PASS.
report()
{
    for n in alpha beta gamma delta scratch1 scratch2; do
        case " $* " in
            *" $n "*) printf '  [PASS]     %s\n' "$n" ;;
            *) printf '  [MISSING]  %s\n' "$n" ;;
        esac
    done
}

# row NAME: run row NAME and succeed when its result is the expected one.
# shellcheck disable=SC2016 # the dollars are in the recipe's commands
row()
{
    case $1 in
        linux-present)
            state alpha beta gamma delta
            run --platform linux
            { echo 'prereq: linux; administrator rights: no'
              report alpha beta gamma delta scratch1 scratch2
              echo 'prereq: result: pass: 6 tools present on linux'; } | expect 0 ;;
        linux-report)
            state
            run --platform linux
            { echo 'prereq: linux; administrator rights: no'
              report scratch1 scratch2
              echo 'prereq: this account runs:'
              echo '    touch "$FIX/have/gamma"'
              echo "    echo $tree fixuser > \"\$FIX/subst\""
              echo 'prereq: an administrator runs:'
              echo '    sudo touch "$FIX/have/alpha" "$FIX/have/beta"'
              echo 'prereq: a person at the machine runs:'
              echo '    touch "$FIX/have/delta"'
              echo 'prereq: result: missing: 4 of 6 tools on linux'; } | expect 1 &&
                [ -z "$(ls "$FIX/have")" ] && [ ! -s "$FIX/log" ] ;;
        linux-root)
            state ROOT gamma
            run --platform linux
            { echo 'prereq: linux; administrator rights: root'
              report gamma scratch1 scratch2
              echo 'prereq: this account runs, as an administrator:'
              echo '    touch "$FIX/have/alpha" "$FIX/have/beta"'
              echo 'prereq: a person at the machine runs:'
              echo '    touch "$FIX/have/delta"'
              echo 'prereq: result: missing: 3 of 6 tools on linux'; } | expect 1 ;;
        linux-install)
            state
            run --platform linux --install
            { echo 'prereq: linux; administrator rights: no'
              report scratch1 scratch2
              echo 'prereq: running: touch "$FIX/have/gamma"'
              echo "prereq: running: echo $tree fixuser > \"\$FIX/subst\""
              echo 'prereq: probing again'
              report gamma scratch1 scratch2
              echo 'prereq: an administrator runs:'
              echo '    sudo touch "$FIX/have/alpha" "$FIX/have/beta"'
              echo 'prereq: a person at the machine runs:'
              echo '    touch "$FIX/have/delta"'
              echo 'prereq: result: missing: 3 of 6 tools on linux'; } | expect 1 &&
                [ "$(cat "$FIX/subst")" = "$tree fixuser" ] && [ ! -s "$FIX/log" ] ;;
        linux-sudo)
            state SUDO
            run --platform linux --install
            { echo 'prereq: linux; administrator rights: sudo'
              report scratch1 scratch2
              echo 'prereq: running: touch "$FIX/have/alpha" "$FIX/have/beta"'
              echo 'prereq: running: touch "$FIX/have/gamma"'
              echo "prereq: running: echo $tree fixuser > \"\$FIX/subst\""
              echo 'prereq: probing again'
              report alpha beta gamma scratch1 scratch2
              echo 'prereq: this account runs, as an administrator:'
              echo '    sudo touch "$FIX/have/alpha" "$FIX/have/beta"'
              echo 'prereq: a person at the machine runs:'
              echo '    touch "$FIX/have/delta"'
              echo 'prereq: result: missing: 1 of 6 tools on linux'; } | expect 1 &&
                [ "$(cat "$FIX/log")" = 'sudo sh -c touch "$FIX/have/alpha" "$FIX/have/beta"' ] ;;
        windows-report)
            state wa
            run --platform windows
            { echo 'prereq: windows; administrator rights: no'
              echo '  [PASS]     wa'
              echo '  [MISSING]  wb'
              echo '  [MISSING]  wc'
              echo 'prereq: an administrator runs, in an elevated PowerShell on the Windows machine:'
              echo '    Get wb'
              echo 'prereq: a person at the Windows machine runs:'
              echo '    Get wc'
              echo 'prereq: result: missing: 2 of 3 tools on windows'; } | expect 1 ;;
        windows-install)
            state
            run --platform windows --install
            { echo 'prereq: windows; administrator rights: no'
              echo '  [MISSING]  wa'
              echo '  [MISSING]  wb'
              echo '  [MISSING]  wc'
              echo 'prereq: running: Get wa'
              echo 'prereq: probing again'
              echo '  [PASS]     wa'
              echo '  [MISSING]  wb'
              echo '  [MISSING]  wc'
              echo 'prereq: an administrator runs, in an elevated PowerShell on the Windows machine:'
              echo '    Get wb'
              echo 'prereq: a person at the Windows machine runs:'
              echo '    Get wc'
              echo 'prereq: result: missing: 2 of 3 tools on windows'; } | expect 1 &&
                [ "$(cat "$FIX/log")" = 'pwsh Get wa' ] ;;
        windows-elevated)
            state ELEVATED
            run --platform windows --install
            { echo 'prereq: windows; administrator rights: elevated'
              echo '  [MISSING]  wa'
              echo '  [MISSING]  wb'
              echo '  [MISSING]  wc'
              echo 'prereq: running: Get wa'
              echo 'prereq: running: Get wb'
              echo 'prereq: probing again'
              echo '  [PASS]     wa'
              echo '  [PASS]     wb'
              echo '  [MISSING]  wc'
              echo 'prereq: a person at the Windows machine runs:'
              echo '    Get wc'
              echo 'prereq: result: missing: 1 of 3 tools on windows'; } | expect 1 ;;
        malformed)
            for bad in 'probe' 'need' 'user x' 'need x|probe p|user u|need x' 'need x|user y' \
                'need x|probe y|probe z|user w' 'need x|user y|probe z' 'platform beos'; do
                printf 'platform linux\n%s\n' "$bad" | tr '|' '\n' > "$work/bad"
                sh "$copy" --recipe "$work/bad" --platform linux > "$work/out" 2>&1
                st=$?
                if [ "$st" != 2 ] || ! grep -q "^prereq: $work/bad:[0-9]*: " "$work/out"; then
                    echo "malformed: $bad: status $st" > "$work/why"
                    return 1
                fi
            done
            printf 'platform macos\nneed x\nprobe y\nuser z\n' > "$work/bad"
            sh "$copy" --recipe "$work/bad" --platform linux > "$work/out" 2>&1
            [ "$?" = 2 ] && grep -q 'no platform linux' "$work/out" ;;
        ovmf-linux) ovmf linux ;;
        ovmf-macos) ovmf macos ;;
        coverage) coverage ;;
        *) return 1 ;;
    esac
}

# ovmf PLATFORM: run the real OVMF stanza against private firmware files.
# PREREQ_RECIPE_SOURCE selects the recipe artifact made by the proof driver.
ovmf()
{
    state
    printf 'platform %s\n' "$1" > "$work/ovmf-recipe"
    awk -v want="$1" '
        /^platform / { platform = $2; selected = 0 }
        /^need / { selected = platform == want && $0 == "need OVMF" }
        selected { print }
    ' "${PREREQ_RECIPE_SOURCE:-$repo/tools/setup/prereqs}" >> "$work/ovmf-recipe" || return 1
    mkdir -p "$FIX/firmware/bin" "$FIX/firmware/share/qemu" || return 1
    code=$FIX/firmware/share/qemu/edk2-x86_64-code.fd
    : > "$code"
    : > "$FIX/firmware/share/qemu/edk2-i386-vars.fd"
    : > "$FIX/unreadable"
    chmod 000 "$FIX/unreadable" || return 1
    for image in missing directory unreadable readable discovered; do
        expected=1
        case $image in
            missing) override=$FIX/missing ;;
            directory) override=$FIX/firmware ;;
            unreadable)
                if [ -r "$FIX/unreadable" ]; then
                    echo 'prereq-test: not run: unreadable OVMF override: account can read mode 000 files' >&2
                    continue
                fi
                override=$FIX/unreadable ;;
            readable) override=$code; expected=0 ;;
            discovered) override=; expected=0 ;;
        esac
        PATH="$work/bin:$FIX/firmware/bin:$PATH" RACH2_OVMF_CODE=$override RACH2_OVMF_VARS='' \
            sh "$copy" --recipe "$work/ovmf-recipe" --platform "$1" > "$work/out" 2>&1
        status=$?
        if [ "$expected" = 0 ]; then
            result='  [PASS]     OVMF'
        else
            result='  [MISSING]  OVMF'
        fi
        if [ "$status" != "$expected" ] || ! grep -qxF "$result" "$work/out"; then
            { echo "ovmf: $1 $image: status $status, wanted $expected"; cat "$work/out"; } > "$work/why"
            return 1
        fi
    done
}

# names PLATFORM: the names the real recipe needs on PLATFORM.
names()
{
    sh "$repo/tools/setup/prereq.sh" --list --platform "$1" | awk -F '\t' '$1 == "N" { print $3 }'
}

# coverage: every command the tree's checks name is in the real recipe on
# linux, macos and windows, but the Linux-only ones, and rach2 parses.
# shellcheck disable=SC2016 # the dollars are in the text sed matches
coverage()
{
    : > "$work/why"
    sed -n '/^TOOLS="/,/"$/p' "$repo/tools/setup/preflight.sh" | tr -d '\042\134' |
        sed 's/^TOOLS=//' | tr ' ' '\n' | grep . > "$work/preflight" || return 1
    {
        sed 's/\${CROSS}/i686-elf-/' "$work/preflight"
        sed 's/\${CROSS}/x86_64-elf-/' "$work/preflight"
        printf '%s\n' cppcheck shellcheck clang-format clang OVMF qemu-img \
            qemu-system-i386 qemu-system-x86_64
        awk -F '|' '!/^#/ && NF > 1 { print $2 }' "$repo/tests/registry" | tr ',' '\n' |
            grep -v -e '^-$' -e '^qemu$'
    } | sort -u > "$work/required"
    if ! grep -q '^i686-elf-gcc$' "$work/required" || ! grep -q '^mke2fs$' "$work/required"; then
        echo 'coverage: the required list was not read' > "$work/why"
        return 1
    fi
    for p in linux macos windows; do
        names "$p" > "$work/have-$p" || { echo "coverage: $p does not parse" >> "$work/why"; continue; }
        case $p in
            macos) skip=$linux_only_macos ;;
            windows) skip=$linux_only_windows ;;
            *) skip= ;;
        esac
        for c in $skip; do
            grep -qx -- "$c" "$repo/tools/etude/tooling/tools" ||
                echo "coverage: the tooling machine does not provide $c, which $p lacks" >> "$work/why"
        done
        if [ -n "$skip" ]; then
            for c in $tooling_needs; do
                grep -qx -- "$c" "$work/have-$p" ||
                    echo "coverage: $p lacks $c, which the tooling machine needs" >> "$work/why"
            done
        fi
        while read -r need; do
            case " $skip " in *" $need "*) continue ;; esac
            # An entry of choices is met by every command of one of them.
            met=0
            for choice in $(echo "$need" | tr '/' ' '); do
                all=1
                for c in $(echo "$choice" | tr '+' ' '); do
                    case " $skip " in *" $c "*) continue ;; esac
                    grep -qx -- "$c" "$work/have-$p" || all=0
                done
                [ "$all" = 0 ] || met=1
            done
            [ "$met" = 1 ] || echo "coverage: $p lacks $need" >> "$work/why"
        done < "$work/required"
    done
    names rach2 | grep -qx cc || echo 'coverage: rach2 lacks cc' >> "$work/why"
    version=$(sed -n 's/^CODEX_VERSION=//p' "$repo/tests/cases/host/agentapi.sh")
    sh "$repo/tools/setup/prereq.sh" --list --platform linux |
        grep -q "	admin	npm install -g @openai/codex@$version\$" ||
        echo "coverage: linux does not install Codex $version, the codexapi cases'" >> "$work/why"
    grep -qx "CODEX_VERSION=$version" "$repo/tools/etude/tooling/provision.sh" ||
        echo "coverage: the tooling machine does not install Codex $version, the codexapi cases'" >> "$work/why"
    grep -o 'tools/setup/[a-z_]*\.sh' "$repo/tools/setup/prereqs" | sort -u | while read -r script; do
        [ -f "$repo/$script" ] || echo "coverage: the recipe runs $script, which is missing" >> "$work/why"
    done
    [ ! -s "$work/why" ]
}

rows='linux-present linux-report linux-root linux-install linux-sudo windows-report windows-install windows-elevated malformed coverage ovmf-linux ovmf-macos'
[ "$mode" != ovmf ] || rows='ovmf-linux ovmf-macos'

# rows: run every row against the copy, print `prereq-test: failed: NAME`
# for each that fails, and fail when any did.
rows()
{
    failed=0
    for r in $rows; do
        : > "$work/why"
        if ! row "$r"; then
            echo "prereq-test: failed: $r"
            sed 's/^/    /' "$work/why"
            failed=1
        fi
    done
    return "$failed"
}

# expect_fault FAULT ROWS: with FAULT in the copy, require exactly ROWS to
# fail.
expect_fault()
{
    fault "$1" || { echo "prereq-test: fault $1 changes nothing" >&2; return 1; }
    rows > "$work/mutated.out" && { echo "prereq-test: fault passed: $1" >&2; return 1; }
    got=$(sed -n 's/^prereq-test: failed: //p' "$work/mutated.out" | tr '\n' ' ')
    if [ "$got" != "$2 " ]; then
        echo "prereq-test: fault $1 failed rows: $got; expected: $2" >&2
        return 1
    fi
}

faults='admin-run manual-run no-sudo no-reprobe shared-scratch'
if [ "$mode" = texts ]; then
    for f in $faults; do
        fault "$f" || { echo "prereq-test: fault $f changes nothing in tools/setup/prereq.sh" >&2; exit 1; }
    done
    exit 0
fi
fault none
rows >&2 || exit 1
if [ "$mode" = mutate ]; then
    expect_fault admin-run 'linux-install windows-install' || exit 1
    expect_fault manual-run 'linux-install linux-sudo windows-install windows-elevated' || exit 1
    expect_fault no-sudo 'linux-report linux-install linux-sudo' || exit 1
    expect_fault no-reprobe 'linux-install linux-sudo windows-install windows-elevated' || exit 1
    expect_fault shared-scratch 'linux-present linux-report linux-root linux-install linux-sudo' || exit 1
    fault none
    rows >&2 || exit 1
    echo 'prereq-test: result: pass: five faults caught and restored'
fi
echo "prereq-test: result: pass: $(echo "$rows" | wc -w | tr -d ' ') rows"
