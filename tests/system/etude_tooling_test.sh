#!/bin/sh
#
# etude_tooling_test.sh: etude-tooling makes the tooling machine once from a
# recipe whose image it checks by digest, runs a job in a private copy of it
# with the job's files, disks and outputs, tells a job that failed from a
# machine that failed or ran out of time, and records each boot's steps in
# the history.
#
# The rows build a private etude-tooling and etude-volume from a copy of
# tools/etude and run them against stand-ins for curl, qemu-img and QEMU.
# The `curl` stand-in copies a fixture image; `qemu-img` writes an overlay
# that names its backing file, which must be found from the overlay's
# directory; and QEMU plays the machine: on a boot with the seed volume,
# which must have provision.sh's text, it answers as provision.sh does, and on
# a boot with the job volume it does what runner.sh does, on this host,
# reading and writing the volume's archive with the host's tar and each
# attached disk as the file itself. FIX/qemu says how the stand-in
# misbehaves: hang, crash or lie, and FIX/provision `fail` makes provision.sh
# fail. A machine that hangs and is not stopped leaves FIX/woke when its
# sleep ends.
#
# The rows: a machine made, recorded and reused; an image with another
# digest, refused; a provision that fails, which publishes nothing; a job
# with its inputs, a disk and its outputs, recorded; a job that exits 3; a
# run before the machine is made; a hang past the deadline; a hang stopped by
# SIGTERM, which must stop QEMU, remove the job's directory and exit 143;
# QEMU's crash; a runner whose status disagrees with the volume's; QEMU's
# command line; and the arm64 machine, made apart from the amd64 one and
# booted on QEMU's virt board with its UEFI firmware.
#
# Usage: sh tests/system/etude_tooling_test.sh
#
set -u

[ $# -eq 0 ] || { echo 'etude-tooling-test: usage: etude_tooling_test.sh' >&2; exit 2; }
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
trap '[ -z "$work" ] || { chmod -R u+rwx "$work" 2>/dev/null; rm -rf "$work"; }' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-tooling-test.XXXXXX") || exit 1
TREE=$work/tree
FIX=$work/fix
export FIX
mkdir -p "$TREE/tools" "$TREE/utils" "$work/bin" "$FIX" || exit 1
cp -R "$repo/tools/etude" "$TREE/tools/" || exit 1
cp "$repo/utils/sha256.c" "$repo/utils/sha256.h" "$TREE/utils/" || exit 1
command -v tar > /dev/null 2>&1 || { echo 'etude-tooling-test: no tar on this host' >&2; exit 1; }

# The fixture's image and its recipe, whose image lines name it for both
# architectures.
printf 'the cloud image\n' > "$FIX/image"
sum=$(shasum -a 256 "$FIX/image" | cut -c1-64) || exit 1
RECIPE=$TREE/tools/etude/tooling
printf 'amd64 https://images.example/fixture.img %s\narm64 https://images.example/arm.img %s\n' \
    "$sum" "$sum" > "$RECIPE/image"

cat > "$work/bin/curl" <<'EOF'
#!/bin/sh
# curl -fsSL --retry 3 -o OUT URL: copy the fixture's image to OUT.
out=$5
cp "$FIX/image" "$out"
EOF
cat > "$work/bin/qemu-img" <<'EOF'
#!/bin/sh
# qemu-img create -q -f qcow2 -b BASE -F qcow2 OUT [SIZE]: an overlay that
# names BASE, found from OUT's directory.
base=$6 out=$9
case $base in /*) b=$base ;; *) b=$(dirname "$out")/$base ;; esac
[ -e "$b" ] || { echo "qemu-img: $base is not found from $(dirname "$out")" >&2; exit 1; }
printf 'overlay of %s\n' "$base" > "$out"
EOF
cat > "$work/bin/qemu-system-x86_64" <<'EOF'
#!/bin/sh
# The machine. It keeps its name and arguments in FIX/qemu.args.
printf '%s\n' "${0##*/}" "$@" > "$FIX/qemu.args"
serials=0 seed= job=
while [ $# -gt 0 ]; do
    case $1 in
        -drive)
            id=$(printf '%s\n' "$2" | sed -n 's/.*id=\([^,]*\).*/\1/p')
            file=$(printf '%s\n' "$2" | sed -n 's/.*file=\([^,]*\).*/\1/p')
            eval "file_$id=\$file"
            shift ;;
        -device)
            case $2 in
                virtio-blk-pci,*)
                    id=$(printf '%s\n' "$2" | sed -n 's/.*drive=\([^,]*\).*/\1/p')
                    serial=$(printf '%s\n' "$2" | sed -n 's/.*serial=\([^,]*\).*/\1/p')
                    eval "f=\$file_$id"
                    case $serial in
                        etude-job) job=$f ;;
                        etude-d*) n=${serial#etude-d}; eval "export ETUDE_DISK$n=\$f"; serials=$n ;;
                        '') [ "$id" = root ] || seed=$f ;;
                    esac ;;
            esac
            shift ;;
        -serial)
            if [ -z "${console:-}" ]; then console=${2#file:}; else chan=${2#file:}; fi
            shift ;;
    esac
    shift
done
: > "$console"
: > "$chan"
how=$(cat "$FIX/qemu" 2>/dev/null)
if [ -n "$seed" ]; then
    grep -q 'provision.sh: make the tooling machine' "$seed" || exit 1
    if [ "$(cat "$FIX/provision" 2>/dev/null)" = fail ]; then
        echo 'etude-tooling: provision: failed: apt-get install' >> "$chan"
    else
        echo 'etude-tooling: tool unshare /usr/bin/unshare' >> "$chan"
        echo 'etude-tooling: provision: ok' >> "$chan"
    fi
    exit 0
fi
[ -n "$job" ] || exit 1
vol=$FIX/vol ws=$FIX/vol/in st=$FIX/st
rm -rf "$vol" "$st" && mkdir -p "$vol" "$st/out" || exit 1
tar -x -f "$job" -C "$vol" || exit 1
[ -f "$vol/job.sh" ] && [ -f "$vol/outputs" ] && [ -d "$ws" ] || exit 1
export ETUDE_DISKS=$serials ETUDE_WORK=$ws
echo 'etude-job: start' >> "$chan"
[ "$how" = crash ] && exit 1
[ "$how" = hang ] && { sleep 30; touch "$FIX/woke"; exit 0; }
(cd "$ws" && sh "$vol/job.sh" < /dev/null > "$st/stdout" 2> "$st/stderr")
rc=$?
echo "etude-job: ran $rc" >> "$chan"
while IFS= read -r name; do
    [ -n "$name" ] && { cp -R "$ws/$name" "$st/out/" || exit 1; }
done < "$vol/outputs"
[ "$how" = lie ] && echo $((rc + 1)) > "$st/status" || echo "$rc" > "$st/status"
(cd "$st" && tar -c -f - out stdout stderr status) | dd of="$job" conv=notrunc 2> /dev/null || exit 1
echo "etude-job: end $rc" >> "$chan"
exit 0
EOF
chmod +x "$work/bin/curl" "$work/bin/qemu-img" "$work/bin/qemu-system-x86_64" || exit 1
cp "$work/bin/qemu-system-x86_64" "$work/bin/qemu-system-aarch64" || exit 1
PATH=$work/bin:$PATH
export PATH

# sources NAME: the sources of the copy's sources.mk list NAME.
sources() {
    awk -v n="$1" '$1 == n { on = 1; sub(/^[^=]*=/, "") }
        on { more = sub(/\\$/, ""); print; if (!more) exit }' "$TREE/tools/etude/sources.mk"
}

# build: compile the copy's etude-tooling into work/etude-tooling, and
# etude-volume beside it.
build() {
    # shellcheck disable=SC2046 # the lists are words
    (cd "$TREE" && cc -std=c99 -D_DEFAULT_SOURCE -Itools/etude/include -o "$work/etude-tooling" \
        $(sources ETUDE_TOOLING_SRC) && cc -std=c99 -o "$work/etude-volume" \
        $(sources ETUDE_VOLUME_SRC)) > "$work/build.log" 2>&1
}

# tooling ARG...: run the copy with the fixture's recipe and cache.
CACHE=$work/cache
tooling() {
    "$work/etude-tooling" -r "$RECIPE" -C "$CACHE" -a tcg "$@"
}

# history STREAM: the history options of a stream at work/STREAM.jsonl.
history() {
    mkdir -p "$work/store"
    echo "-H $work/$1.jsonl -j $1 -R run -P plan -M machine -B boot -b $work/store"
}

# fresh: forget the made machine and the stand-in's mode.
fresh() {
    chmod -R u+rwx "$CACHE" 2>/dev/null
    rm -rf "$CACHE" "$work"/*.jsonl "$work/out" "$FIX/qemu" "$FIX/provision" "$FIX/woke"
    mkdir -p "$work/out"
}

# made: make the machine, quietly.
made() {
    tooling prepare > /dev/null 2>&1
}

# A job that writes each kind of output and its disk.
cat > "$work/job.sh" <<'EOF'
cat note.txt
cat dir/inner.txt
echo "disks $ETUDE_DISKS" >&2
printf 'written in the machine\n' > "$ETUDE_DISK1"
printf 'made\n' > result.txt
mkdir tree && printf 'deep\n' > tree/leaf.txt
EOF
printf 'the note\n' > "$work/note.txt"
mkdir -p "$work/dir" && printf 'the inner file\n' > "$work/dir/inner.txt"

# row NAME: run one row, its reason in work/why.
# shellcheck disable=SC2015 # each C only records why and returns
row() {
    fresh
    case $1 in
    made)
        # shellcheck disable=SC2046 # the options are words
        tooling prepare $(history made) > "$work/out.log" 2>&1 || { cat "$work/out.log" > "$work/why"; return 1; }
        grep -q '^etude-tooling: result: made ' "$work/out.log" || { echo 'no result line' > "$work/why"; return 1; }
        disk=$(ls "$CACHE"/tooling-*.qcow2 2>/dev/null)
        [ -n "$disk" ] && [ ! -w "$disk" ] || { echo 'no read-only disk was published' > "$work/why"; return 1; }
        grep -q 'overlay of image-' "$disk" || { echo 'the disk does not name its image from the cache' > "$work/why"; return 1; }
        grep -q 'provision: ok' "$CACHE"/tooling-*.tools || { echo 'no tools record' > "$work/why"; return 1; }
        ls -d "$CACHE"/prepare-* > /dev/null 2>&1 && { echo 'the scratch was left' > "$work/why"; return 1; }
        grep -q '"stage":"one_time_setup".*"outcome":"pass"' "$work/made.jsonl" ||
            { echo 'no passing one_time_setup step' > "$work/why"; return 1; }
        tooling check > /dev/null || { echo 'check does not find it' > "$work/why"; return 1; }
        tooling prepare 2>&1 | grep -q '^etude-tooling: result: reused ' || { echo 'not reused' > "$work/why"; return 1; }
        ;;
    digest)
        printf 'another image\n' > "$FIX/image.real"
        cp "$FIX/image" "$FIX/image.kept" && cp "$FIX/image.real" "$FIX/image"
        tooling prepare > "$work/out.log" 2>&1; rc=$?
        cp "$FIX/image.kept" "$FIX/image"
        if [ "$rc" != 1 ] || ! grep -q 'has the digest' "$work/out.log"; then
            cat "$work/out.log" > "$work/why"
            return 1
        fi
        ls "$CACHE"/image-* "$CACHE"/tooling-* > /dev/null 2>&1 && { echo 'an unchecked image or a disk was kept' > "$work/why"; return 1; }
        ;;
    provision)
        echo fail > "$FIX/provision"
        tooling prepare > "$work/out.log" 2>&1; rc=$?
        [ "$rc" = 1 ] && grep -q 'provision.sh: apt-get install' "$work/out.log" || { cat "$work/out.log" > "$work/why"; return 1; }
        tooling check > /dev/null && { echo 'a failed machine was published' > "$work/why"; return 1; }
        ls "$CACHE"/tooling-* > /dev/null 2>&1 && { echo 'a part was left' > "$work/why"; return 1; }
        ;;
    job)
        made || { echo 'not made' > "$work/why"; return 1; }
        printf 'the disk before\n' > "$work/disk.img"
        # shellcheck disable=SC2046 # the options are words
        tooling run -i "$work/note.txt" -i "$work/dir" -d "$work/disk.img" -o result.txt -o tree \
            -O "$work/out" $(history job) "$work/job.sh" > "$work/stdout" 2> "$work/stderr"; rc=$?
        [ "$rc" = 0 ] || { echo "status $rc" > "$work/why"; cat "$work/stderr" >> "$work/why"; return 1; }
        [ "$(cat "$work/stdout")" = "$(printf 'the note\nthe inner file')" ] || { echo 'stdout' > "$work/why"; return 1; }
        grep -q '^disks 1$' "$work/stderr" && grep -q '^etude-tooling: cost: start ' "$work/stderr" ||
            { cat "$work/stderr" > "$work/why"; return 1; }
        [ "$(cat "$work/disk.img")" = 'written in the machine' ] || { echo 'the disk' > "$work/why"; return 1; }
        [ "$(cat "$work/out/result.txt" 2>/dev/null)" = made ] && [ "$(cat "$work/out/tree/leaf.txt" 2>/dev/null)" = deep ] ||
            { echo 'the outputs' > "$work/why"; return 1; }
        ls -d "$CACHE"/job-* > /dev/null 2>&1 && { echo "the job's directory was left" > "$work/why"; return 1; }
        for s in tooling_start execute cleanup; do
            grep -q "\"stage\":\"$s\".*\"outcome\":\"pass\"" "$work/job.jsonl" ||
                { echo "no passing $s step" > "$work/why"; return 1; }
        done
        ;;
    status)
        made || { echo 'not made' > "$work/why"; return 1; }
        printf 'echo failing >&2\nexit 3\n' > "$work/three.sh"
        # shellcheck disable=SC2046 # the options are words
        tooling run $(history status) "$work/three.sh" > /dev/null 2> "$work/stderr"; rc=$?
        [ "$rc" = 3 ] && grep -q '^failing$' "$work/stderr" || { echo "status $rc" > "$work/why"; return 1; }
        grep -q '"stage":"execute".*"outcome":"failure"' "$work/status.jsonl" &&
            grep -q '"stage":"cleanup".*"outcome":"pass"' "$work/status.jsonl" ||
            { echo 'the steps of a job that failed' > "$work/why"; return 1; }
        ;;
    unmade)
        tooling run "$work/job.sh" > /dev/null 2> "$work/stderr"; rc=$?
        [ "$rc" = 126 ] && grep -q 'is not made' "$work/stderr" || { echo "status $rc" > "$work/why"; return 1; }
        ;;
    late)
        made || { echo 'not made' > "$work/why"; return 1; }
        echo hang > "$FIX/qemu"
        # shellcheck disable=SC2046 # the options are words
        tooling run -t 2 -k $(history late) "$work/job.sh" > /dev/null 2> "$work/stderr"; rc=$?
        [ "$rc" = 124 ] && grep -q 'result: late: the deadline passed before the job ended' "$work/stderr" ||
            { echo "status $rc" > "$work/why"; cat "$work/stderr" >> "$work/why"; return 1; }
        [ ! -e "$FIX/woke" ] || { echo 'QEMU was not stopped at the deadline' > "$work/why"; return 1; }
        grep -q '"stage":"execute".*"outcome":"incomplete"' "$work/late.jsonl" ||
            { echo 'the late job is not incomplete' > "$work/why"; return 1; }
        ls -d "$CACHE"/job-* > /dev/null 2>&1 || { echo "the evidence was not kept" > "$work/why"; return 1; }
        ;;
    stop)
        made || { echo 'not made' > "$work/why"; return 1; }
        echo hang > "$FIX/qemu"
        # shellcheck disable=SC2046 # the options are words
        "$work/etude-tooling" -r "$RECIPE" -C "$CACHE" -a tcg run $(history stop) "$work/job.sh" \
            > /dev/null 2> "$work/stderr" &
        pid=$!
        n=0
        until grep -q '^etude-job: start$' "$CACHE"/job-*/channel.log 2> /dev/null; do
            n=$((n + 1))
            if [ "$n" -gt 20 ]; then
                kill -KILL "$pid"; wait "$pid"
                echo 'the job did not start' > "$work/why"
                return 1
            fi
            sleep 1
        done
        kill -TERM "$pid"
        wait "$pid"; rc=$?
        [ "$rc" = 143 ] && grep -q 'result: stopped: a signal stopped the job$' "$work/stderr" ||
            { echo "status $rc" > "$work/why"; cat "$work/stderr" >> "$work/why"; return 1; }
        [ ! -e "$FIX/woke" ] || { echo 'QEMU was not stopped by the signal' > "$work/why"; return 1; }
        ls -d "$CACHE"/job-* > /dev/null 2>&1 && { echo "the job's directory was left" > "$work/why"; return 1; }
        grep -q '"stage":"execute".*"outcome":"cancelled"' "$work/stop.jsonl" ||
            { echo 'the stopped job is not cancelled' > "$work/why"; return 1; }
        ;;
    crash)
        made || { echo 'not made' > "$work/why"; return 1; }
        echo crash > "$FIX/qemu"
        tooling run "$work/job.sh" > /dev/null 2> "$work/stderr"; rc=$?
        [ "$rc" = 125 ] && grep -q 'QEMU exited with status 1' "$work/stderr" ||
            { echo "status $rc" > "$work/why"; cat "$work/stderr" >> "$work/why"; return 1; }
        ;;
    lie)
        made || { echo 'not made' > "$work/why"; return 1; }
        echo lie > "$FIX/qemu"
        tooling run "$work/job.sh" > /dev/null 2> "$work/stderr"; rc=$?
        [ "$rc" = 125 ] && grep -q 'status is not the one the runner reported' "$work/stderr" ||
            { echo "status $rc" > "$work/why"; cat "$work/stderr" >> "$work/why"; return 1; }
        ;;
    line)
        made || { echo 'not made' > "$work/why"; return 1; }
        printf 'x\n' > "$work/d.img"
        tooling run -n -c 3 -m 1024 -d "$work/d.img" "$work/job.sh" > /dev/null 2>&1
        a=$FIX/qemu.args
        for want in -nodefaults -no-reboot tcg 3 1024 'user,model=virtio-net-pci' virtio-rng-pci; do
            grep -qx -- "$want" "$a" || { echo "no argument $want" > "$work/why"; return 1; }
        done
        grep -qx 'virtio-blk-pci,drive=root,bootindex=0' "$a" &&
            grep -qx 'virtio-blk-pci,drive=job,serial=etude-job' "$a" &&
            grep -qx 'virtio-blk-pci,drive=d1,serial=etude-d1' "$a" ||
            { echo 'the disks' > "$work/why"; return 1; }
        grep -qx qemu-system-x86_64 "$a" && ! grep -qx virt "$a" ||
            { echo 'the amd64 machine is not the default PC' > "$work/why"; return 1; }
        tooling run "$work/job.sh" > /dev/null 2>&1
        grep -qx none "$a" || { echo 'a job without -n has a network' > "$work/why"; return 1; }
        ;;
    arm64)
        arm() { "$work/etude-tooling" -r "$RECIPE" -C "$CACHE" -p arm64 -a hvf "$@"; }
        made || { echo 'not made' > "$work/why"; return 1; }
        arm check > /dev/null && { echo 'the arm64 machine is the amd64 one' > "$work/why"; return 1; }
        arm prepare > "$work/out.log" 2>&1 || { cat "$work/out.log" > "$work/why"; return 1; }
        [ "$(find "$CACHE" -maxdepth 1 -name 'tooling-*.qcow2' | wc -l)" -eq 2 ] || { echo 'the two machines share a disk' > "$work/why"; return 1; }
        arm run "$work/job.sh" > /dev/null 2>&1 || { echo 'the arm64 job failed' > "$work/why"; return 1; }
        a=$FIX/qemu.args
        [ "$(sed -n 1p "$a")" = qemu-system-aarch64 ] || { echo 'the arm64 machine is not qemu-system-aarch64' > "$work/why"; return 1; }
        for want in virt edk2-aarch64-code.fd hvf max 'virtio-blk-pci,drive=root,bootindex=0'; do
            grep -qx -- "$want" "$a" || { echo "no argument $want" > "$work/why"; return 1; }
        done
        "$work/etude-tooling" -r "$RECIPE" -C "$CACHE" -p sparc check > "$work/out.log" 2>&1 &&
            { echo 'an unknown architecture was taken' > "$work/why"; return 1; }
        grep -q 'not amd64 or arm64' "$work/out.log" || { cat "$work/out.log" > "$work/why"; return 1; }
        ;;
    esac
    return 0
}

rows='made digest provision job status unmade late stop crash lie line arm64'

# rows: build the copy and run every row, print `etude-tooling-test: failed:
# NAME` for each that fails, and fail when any did.
rows() {
    build || { echo 'etude-tooling-test: failed: build'; cat "$work/build.log"; return 1; }
    failed=0
    for r in $rows; do
        : > "$work/why"
        if ! row "$r"; then
            echo "etude-tooling-test: failed: $r"
            sed 's/^/    /' "$work/why"
            failed=1
        fi
    done
    return "$failed"
}

rows >&2 || exit 1
echo 'etude-tooling-test: result: pass: 12 rows'
