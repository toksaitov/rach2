# loop2.sh N MODE [RACE_SO]: run piglit's refcount-multithread test N times
# as piglit runs it, with the virgl piglit case's environment; MODE plain
# runs it alone, trace with race.so preloaded to print each failed
# GEM_CLOSE, delay with race.so widening Mesa's window as well.
export GRAPHICS_CLASS=conformance LP_NUM_THREADS=0 EGL_PLATFORM=surfaceless PIGLIT_PLATFORM=surfaceless_egl PIGLIT_NO_FAST_SKIP=1
export MESA_SHADER_CACHE_DIR=/tmp/t-cache; mkdir -p $MESA_SHADER_CACHE_DIR
B=/usr/lib/x86_64-linux-gnu/piglit/bin/ext_image_dma_buf_import-refcount-multithread
n=$1 mode=$2 so=${3:-/tmp/race.so}
case $mode in
    plain) pre= ;;
    trace) pre="LD_PRELOAD=$so" ;;
    delay) pre="LD_PRELOAD=$so T5777_DELAY=1" ;;
esac
cd /tmp
pass=0 crash=0 other=0 dbl=0 dblruns=0 i=0
while [ $i -lt $n ]; do
    i=$((i+1))
    s0=$(date +%s)
    env $pre $B -auto > /tmp/one.out 2>&1
    st=$?
    d=$(grep -c "GEM_CLOSE .* failed" /tmp/one.out)
    dbl=$((dbl+d)); [ "$d" -eq 0 ] || dblruns=$((dblruns+1))
    if grep -q 'PIGLIT: {"result": "pass"' /tmp/one.out; then pass=$((pass+1));
    elif [ $st -ge 128 ]; then crash=$((crash+1)); echo "run $i: status $st"; grep -v "GEM_CLOSE" /tmp/one.out | head -20;
    else other=$((other+1)); echo "run $i: status $st"; head -20 /tmp/one.out; fi
    [ "$d" -eq 0 ] || { echo "run $i: failed GEM_CLOSEs $d"; grep -m3 "GEM_CLOSE" /tmp/one.out; }
    grep "t5777: closes" /tmp/one.out | sed "s/^/run $i: /"
    [ $i -gt 2 ] || echo "run $i took $(( $(date +%s) - s0 )) s"
    [ $((i % 10)) -ne 0 ] || echo "progress: $mode $i pass $pass crash $crash other $other"
done
echo "result: $mode runs $i pass $pass crash $crash other $other double-closes $dbl in $dblruns runs"
