#!/bin/sh
# relocations.sh: count and time i915's relocation and execbuffer calls in
# the backend VM, with Linux's function profiler or, on a kernel built
# without it, the function graph tracer. Run as root there.
set -eu
trace=/sys/kernel/tracing

die()
{
    echo "nctx-relocations: $*" >&2
    exit 1
}

# The relocation step is eb_relocate_parse from Linux 5.10 on, eb_relocate
# before it; eb_relocate_vma, which it calls, is the last resort when both
# are inlined.
functions()
{
    awk '
        { sub(/ .*/, "") }
        /^i915_gem_execbuffer2_ioctl(\.|$)/ { eb = eb $0 "\n" }
        /^eb_relocate_parse(\.|$)/ { parse = parse $0 "\n" }
        /^eb_relocate(\.|$)/ { old = old $0 "\n" }
        /^eb_relocate_vma(\.|$)/ { vma = vma $0 "\n" }
        END {
            reloc = parse != "" ? parse : old != "" ? old : vma
            printf "%s%s", eb, reloc
        }' "$trace/available_filter_functions"
}

case ${1:-} in
    start)
        [ -d "$trace" ] || die "the kernel has no tracing directory"
        [ "$(cat "$trace/current_tracer")" = nop ] || die "another tracer is in use"
        list=$(functions)
        names=$(printf "%s\n" "$list" | tr "\n" " ")
        echo "$list" | grep -q '^i915_gem_execbuffer2_ioctl' || die "execbuffer is not traceable"
        echo "$list" | grep -q '^eb_relocate' || die "no relocation function is traceable"
        printf '%s\n' "$list" > "$trace/set_ftrace_filter"
        if [ -e "$trace/function_profile_enabled" ]; then
            [ "$(cat "$trace/function_profile_enabled")" = 0 ] || die "the profiler is in use"
            echo 1 > "$trace/options/graph-time"
            echo 1 > "$trace/function_profile_enabled"
            echo "nctx-relocations: started: profiler: $names"
        else
            grep -qw function_graph "$trace/available_tracers" ||
                die "the kernel has neither the function profiler nor the graph tracer"
            echo 0 > "$trace/tracing_on"
            echo 65536 > "$trace/buffer_size_kb"
            echo 1 > "$trace/options/funcgraph-tail"
            echo function_graph > "$trace/current_tracer"
            : > "$trace/trace"
            echo 1 > "$trace/tracing_on"
            echo "nctx-relocations: started: graph tracer: $names"
        fi
        ;;
    stop)
        if [ "$(cat "$trace/current_tracer")" = function_graph ]; then
            echo 0 > "$trace/tracing_on"
            # A leaf is "TIME us | name();", a call with traced callees ends
            # "TIME us | } /* name */"; its opening line has no time. A
            # module's function is "name [module]". The time is the field
            # before "us", after an optional marker such as + or !.
            awk '
                /\|/ {
                    name = ""
                    if (match($0, /[A-Za-z0-9_.]+( \[[A-Za-z0-9_]+\])?\(\);/))
                        name = substr($0, RSTART, RLENGTH - 3)
                    else if (match($0, /\/\* [A-Za-z0-9_.]+( \[[A-Za-z0-9_]+\])? \*\//))
                        name = substr($0, RSTART + 3, RLENGTH - 6)
                    sub(/ .*/, "", name)
                    if (name == "")
                        next
                    us = ""
                    for (i = 2; i <= NF; i++)
                        if ($i == "us") { us = $(i - 1); break }
                    if (name ~ /^i915_gem_execbuffer2_ioctl/) {
                        batches++
                        eb_us += us
                    } else if (name ~ /^eb_relocate/) {
                        reloc++
                        reloc_us += us
                        relname = name
                    }
                }
                END {
                    if (!batches || !reloc) {
                        print "nctx-relocations: result: fail: no batches or relocation calls"
                        exit 1
                    }
                    printf "nctx-relocations: graph: %d execbuffer calls, %.3f us in them; %d calls of %s\n", batches, eb_us, reloc, relname
                    printf "nctx-relocations: result: %d batches, %d relocation calls, %.3f us total, %.3f us per batch\n", batches, reloc, reloc_us, reloc_us / batches
                }' "$trace/trace"
            grep -H . "$trace"/per_cpu/cpu*/stats | grep -E 'overrun|dropped' || true
            echo nop > "$trace/current_tracer"
            : > "$trace/set_ftrace_filter"
            exit 0
        fi
        echo 0 > "$trace/function_profile_enabled"
        cat "$trace"/trace_stat/function*
        awk '
            $1 ~ /^eb_relocate/ { reloc += $2; us += $3 }
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
