#!/bin/sh
# relocations.sh: count and time i915's relocation and execbuffer calls in
# the backend VM, with Linux's function profiler. Run as root there.
set -eu
trace=/sys/kernel/tracing

die()
{
    echo "nctx-relocations: $*" >&2
    exit 1
}

case ${1:-} in
    start)
        [ -e "$trace/function_profile_enabled" ] || die "the kernel has no function profiler"
        [ "$(cat "$trace/function_profile_enabled")" = 0 ] || die "the profiler is in use"
        [ "$(cat "$trace/current_tracer")" = nop ] || die "another tracer is in use"
        functions=$(awk '$1 ~ /^(eb_relocate|i915_gem_execbuffer2_ioctl)(\.|$)/ { print $1 }' \
            "$trace/available_filter_functions")
        echo "$functions" | grep -q '^eb_relocate' || die "eb_relocate is not traceable"
        echo "$functions" | grep -q '^i915_gem_execbuffer2_ioctl' || die "execbuffer is not traceable"
        printf '%s\n' "$functions" > "$trace/set_ftrace_filter"
        echo 1 > "$trace/options/graph-time"
        echo 1 > "$trace/function_profile_enabled"
        echo "nctx-relocations: started: $functions"
        ;;
    stop)
        echo 0 > "$trace/function_profile_enabled"
        cat "$trace"/trace_stat/function*
        awk '
            $1 ~ /^eb_relocate(\.|$)/ { reloc += $2; us += $3 }
            $1 ~ /^i915_gem_execbuffer2_ioctl(\.|$)/ { batches += $2 }
            END {
                if (!batches || !reloc) {
                    print "nctx-relocations: result: fail: no batches or relocation calls"
                    exit 1
                }
                printf "nctx-relocations: result: %d batches, %d relocation calls, %.3f us total, %.3f us per batch\n", batches, reloc, us, us / batches
            }' "$trace"/trace_stat/function*
        ;;
    *) die "usage: relocations.sh start | stop" ;;
esac
