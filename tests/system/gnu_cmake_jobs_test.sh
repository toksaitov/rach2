#!/bin/sh
#
# gnu_cmake_jobs_test.sh: the cmake port's bootstrap runs no more jobs at once
# than the make that builds the GNU layer allows, however many processors the
# host reports.
#
# The recipe, vendor/gnu/ports/cmake.sh, runs its port_build in a scratch
# tree, as build_port.sh runs it, over a stand-in for CMake's bootstrap and
# with getconf and nproc reporting 32 processors. The stand-in runs make as
# the upstream bootstrap does, with -j N when it is given --parallel=N and
# with none otherwise, over eight jobs that each record how many of them are
# running and then wait a second. The rows: the recipe run by a make -j2, as
# make gnu-ports runs build_port.sh, must run at most two jobs at once; and
# the recipe run with MAKEFLAGS=-j3, as build_port.sh sets it when no make
# runs it, at most three. Each row also requires all eight jobs to have run.
# CMAKE_RECIPE names the recipe to run, by default the tree's.
#
# Usage: sh tests/system/gnu_cmake_jobs_test.sh
#
set -u
unset MAKEFLAGS MFLAGS MAKELEVEL MAKE

[ "$#" -eq 0 ] || { echo 'gnu-cmake-jobs-test: usage: gnu_cmake_jobs_test.sh' >&2; exit 2; }
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-gnu-cmake-jobs-test.XXXXXX") || exit 1
recipe=${CMAKE_RECIPE:-$repo/vendor/gnu/ports/cmake.sh}
case $recipe in
    /*) ;;
    *) recipe=$PWD/$recipe ;;
esac
command -v make > /dev/null 2>&1 || { echo 'gnu-cmake-jobs-test: no make on this host' >&2; exit 1; }
getconf=$(command -v getconf) || { echo 'gnu-cmake-jobs-test: no getconf on this host' >&2; exit 1; }

mkdir -p "$work/bin" "$work/src" || exit 1
cat > "$work/bin/getconf" << EOF
#!/bin/sh
case \$1 in
    _NPROCESSORS_ONLN | _NPROCESSORS_CONF) echo 32 ;;
    *) exec "$getconf" "\$@" ;;
esac
EOF
printf '#!/bin/sh\necho 32\n' > "$work/bin/nproc"
cat > "$work/src/bootstrap" << 'EOF'
#!/bin/sh
parallel=
for a in "$@"; do
    case $a in
        --parallel=*) parallel=-j${a#--parallel=} ;;
    esac
done
printf 'all:\ninstall:\n' > Makefile
exec ${MAKE:-make} -s -f "$JOBS_MK" $parallel
EOF
cat > "$work/job.sh" << 'EOF'
#!/bin/sh
touch "$RUNNING/$1"
ls "$RUNNING" | wc -l >> "$COUNTS"
sleep 1
rm -f "$RUNNING/$1"
EOF
{
    printf 'all: j1 j2 j3 j4 j5 j6 j7 j8\n'
    printf 'j1 j2 j3 j4 j5 j6 j7 j8:\n\t@sh "%s" $@\n' "$work/job.sh"
    printf '.PHONY: all j1 j2 j3 j4 j5 j6 j7 j8\n'
} > "$work/jobs.mk"
cat > "$work/port.sh" << 'EOF'
SRC=$WORK/src
OUT=$WORK/out
configure_cmake() { :; }
. "$RECIPE"
cd "$WORK/build" && port_build
EOF
printf 'all:\n\t+@sh "%s"\n' "$work/port.sh" > "$work/outer.mk"
chmod +x "$work/bin/getconf" "$work/bin/nproc" "$work/src/bootstrap" || exit 1

PATH=$work/bin:$PATH
WORK=$work
RECIPE=$recipe
JOBS_MK=$work/jobs.mk
RUNNING=$work/running
COUNTS=$work/counts
export PATH WORK RECIPE JOBS_MK RUNNING COUNTS

failed=0

# row NAME WIDTH COMMAND...: run COMMAND in a fresh build tree and require
# the eight jobs to have run, at most WIDTH at once.
row()
{
    name=$1
    width=$2
    shift 2
    rm -rf "$work/build" "$work/out" "$RUNNING" "$COUNTS"
    mkdir -p "$work/build" "$RUNNING" || exit 1
    printf 'all:\n' > "$work/build/Makefile"
    : > "$COUNTS"
    if ! "$@" > "$work/log" 2>&1; then
        echo "gnu-cmake-jobs-test: $name: the recipe failed:" >&2
        sed 's/^/    /' "$work/log" >&2
        failed=1
        return
    fi
    runs=$(wc -l < "$COUNTS" | tr -d ' ')
    most=$(sort -n "$COUNTS" | tail -n 1 | tr -d ' ')
    if [ "$runs" -ne 8 ]; then
        echo "gnu-cmake-jobs-test: $name: $runs of the 8 jobs ran" >&2
        failed=1
    elif [ "$most" -gt "$width" ]; then
        echo "gnu-cmake-jobs-test: $name: $most jobs ran at once, more than $width" >&2
        failed=1
    else
        echo "gnu-cmake-jobs-test: $name: ok: at most $most of 8 jobs at once"
    fi
}

row jobserver 2 make -s -j2 -f "$work/outer.mk"
row alone 3 env MAKEFLAGS=-j3 sh "$work/port.sh"
exit "$failed"
