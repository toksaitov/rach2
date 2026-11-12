#!/bin/sh
# shellcheck disable=SC2086 # no path or word list here has a space
# conformance.sh STAGE [PART...]: Linux's references for the Lenovo's GPU
# cases (tests/native/gpu.cases) that the second discovery run did not take,
# in one boot of Debian with the IOMMU's translation off, made by the cases'
# own program, tests/graphics/runners/lenovo.sh, with `-` for the reference.
#   STAGE  a directory holding lenovo.sh beside the runners, guest-env.sh,
#          igt-lenovo-gem.list and webgl-lenovo.omit, flat, as a case's
#          workspace holds them, and
#          the IGT stage of Rach2's root unpacked in STAGE/igt (usr/...)
#   PART   igt, glmark2, gate, probe or webgl, each by default, in that
#          order: IGT's buffer list as root with no compositor, then Weston
#          as gfx on the panel and on it glmark2's validations, glmark2's
#          benchmark, the WebGL probe and the WebGL suite in its 12 chunks,
#          each browser in turn
# Each case runs in a directory of its own under $OUT/conformance/NAME, and
# `collect` then writes $OUT/conformance/expect/, the files that go into
# tests/graphics/ref/lenovo/expect/, and gate.lines, the probe's lines for
# tests/graphics/ref/lenovo/gate.
set -u
. /root/ref80/lib.sh
need python3 weston runuser
S=${1:?usage: conformance.sh STAGE [PART...]|collect}
shift
O=$OUT/conformance
WESTON=''

# webgl_suite: the WebGL suite that step 9 served, the pinned commit's.
webgl_suite() {
	find /srv/run2/step9/web/webgl -maxdepth 4 -name 00_test_list.txt -path '*/sdk/tests/*' |
		head -n 1 | sed 's|/00_test_list.txt$||'
}

# case NAME ARGUMENT...: lenovo.sh's ARGUMENTs with `-` in a new directory.
case_run() {
	n=$1
	shift
	d=$O/$n
	rm -rf $d
	mkdir -p $d
	cp $S/*.sh $S/*.py $S/*.html $S/*.list $S/*.omit $d/ 2> /dev/null
	say "conformance: start $n: $*" | tee $d/run.log
	(cd $d && sh lenovo.sh "$@" -) > $d/lenovo.out 2>&1
	echo "exit $?" >> $d/run.log
	tail -n 3 $d/lenovo.out
	say "conformance: end $n: $(tail -n 1 $d/run.log)" | tee -a $d/run.log
}

weston_start() {
	pgrep -x weston > /dev/null && { echo "conformance: a weston runs already" >&2; exit 1; }
	install -d -o gfx -g gfx -m 700 $RUN
	mkdir -p $O
	chown gfx:gfx $O
	runuser -u gfx -- env HOME=/home/gfx XDG_RUNTIME_DIR=$RUN LIBSEAT_BACKEND=seatd \
		setsid weston --backend=drm --renderer=gl --shell=kiosk --idle-time=0 \
		--socket=wayland-1 --log=$O/weston.log < /dev/null > /dev/null 2>&1 &
	WESTON=$!
	i=0
	until [ -S $RUN/wayland-1 ]; do
		kill -0 $WESTON 2> /dev/null || { echo "conformance: weston ended: $(tail -n 3 $O/weston.log)" >&2; exit 1; }
		i=$((i + 1))
		[ $i -lt 600 ] || { echo "conformance: weston made no socket" >&2; exit 1; }
		sleep 0.1
	done
	say "conformance: weston: $(grep -m 1 -i 'renderer' $O/weston.log)"
}

weston_stop() {
	[ -n "$WESTON" ] || return 0
	pkill -TERM -u gfx -x weston
	wait $WESTON 2> /dev/null
	WESTON=''
}
trap weston_stop EXIT

collect() {
	E=$O/expect
	rm -rf $E
	mkdir -p $E
	cat $O/igt-gem.1/out/*.results $O/igt-gem.2/out/*.results > $E/igt-gem.rows
	for a in gl es2; do
		cp $O/glmark2.$a/out/validations $E/glmark2-$a.validations
	done
	for b in chromium firefox; do
		cat $O/webgl-$b.*/out/$b-2.0.1-*.pages > $E/webgl-$b.pages
	done
	: > $O/gate.lines
	for b in chromium firefox; do
		awk '{ print $1, $2, ($1 ~ /-alu-/ ? "lower" : "higher") }' $O/gate-probe.$b/out/measures >> $O/gate.lines
	done
	for a in gl es2; do
		echo "conformance: glmark2 $a score here $(cat $O/gate-glmark2.$a/out/measures)"
	done
	wc -l $E/* $O/gate.lines
}

[ "${S}" = collect ] && { collect; exit 0; }
[ -f $S/lenovo.sh ] || { echo "conformance: no lenovo.sh in $S" >&2; exit 1; }
[ $# -gt 0 ] || set -- igt glmark2 gate probe webgl
LENOVO_USER=gfx LENOVO_RUNTIME=$RUN LENOVO_DISPLAY=wayland-1
LENOVO_IGT=$S/igt/usr/libexec/igt-gpu-tools
PATH=$S/igt/usr/bin:$PATH
export LENOVO_USER LENOVO_RUNTIME LENOVO_DISPLAY LENOVO_IGT PATH
mkdir -p $O
uname -r > $O/uname.txt
cat /proc/cmdline >> $O/uname.txt
for part; do
	case $part in
	igt)
		weston_stop
		for i in 1 2; do case_run igt-gem.$i igt igt-lenovo-gem.list gem-$i; done
		;;
	glmark2)
		[ -n "$WESTON" ] || weston_start
		for a in gl es2; do case_run glmark2.$a glmark2 $a; done
		;;
	gate)
		[ -n "$WESTON" ] || weston_start
		for a in gl es2; do case_run gate-glmark2.$a gate glmark2 $a; done
		;;
	probe)
		[ -n "$WESTON" ] || weston_start
		for b in chromium firefox; do case_run gate-probe.$b gate probe $b; done
		;;
	webgl)
		[ -n "$WESTON" ] || weston_start
		LENOVO_WEBGL=$(webgl_suite)
		[ -n "$LENOVO_WEBGL" ] || { echo "conformance: no WebGL suite under /srv/run2/step9/web/webgl" >&2; exit 1; }
		export LENOVO_WEBGL
		for b in chromium firefox; do
			for i in 0 1 2 3 4 5 6 7 8 9 10 11; do
				case_run "webgl-$b.$(printf %02d $i)" webgl $b $i
			done
		done
		;;
	*) echo "conformance: no part $part" >&2; exit 2 ;;
	esac
done
say "conformance: done"
