# stackoverrun_row.sh: the memory suite's row for a kernel stack that runs out.
# shellcheck shell=sh
#
# tests/system/suites/memory.sh sources it to run the row, and
# tests/system/stackoverrun_mutate.sh to prove the row against compiled
# faults, so the two cannot come to judge different things.
#
# stackoverrun_row HARNESS KERNEL IMAGE WHERE LOG boots KERNEL on IMAGE with
# `rach2.stackoverrun=WHERE`: `fork` descends a stack from kstack_alloc, as a
# fork's child has one, and `boot` the boot stack, which init and the
# programs the debug shell starts run on, both before the root is mounted.
# The page below every kernel stack is left unmapped, so the first store
# past the stack's base faults, and since the page fault's own frame cannot
# be pushed there either, the processor raises a double fault, whose task
# gate runs its handler on a stack of its own. The row requires the
# descent's announcement, a record, the double fault's or the page fault's,
# that names the pid the announcement gave, and the panic `a kernel stack ran
# out`, and that the debug shell is never reached; a gate on the exhausted
# stack would have faulted a third time and stopped the machine with neither
# line. The console is kept in LOG. The row prints one line,
# `stackoverrun: WHERE: pass` or `stackoverrun: WHERE: fail: REASON`, and
# returns 0 or 1. Its variables begin with `so_`, since a sourced function
# shares the caller's.
stackoverrun_row()
{
    so_harness=$1
    so_kernel=$2
    so_image=$3
    so_where=$4
    so_log=$5
    so_status=0
    rm -f "$so_log"
    "$so_harness" --kernel "$so_kernel" --initrd "$so_image" --qemu-arg=-cpu --qemu-arg=max \
        --append "rach2.rescue rach2.stackoverrun=$so_where" --timeout 60 \
        --ok "a kernel stack ran out" --never "Type 'help'" --never "the descent came back" \
        --log "$so_log" "expect=kstack: overrun: pid" "expect=ran past the end of its kernel stack" \
        "expect=panic: stopped: .*a kernel stack ran out" >/dev/null 2>&1 || so_status=$?
    so_pid=$(grep -a 'kstack: overrun: pid' "$so_log" 2>/dev/null | head -1 |
        sed -n 's/.*overrun: pid \([0-9]*\) .*/\1/p')
    if [ -z "$so_pid" ]; then
        echo "stackoverrun: $so_where: fail: the boot did not reach the descent"
        return 1
    fi
    if [ "$so_status" -ne 0 ]; then
        echo "stackoverrun: $so_where: fail: the overrun was not reported:" \
            "$(grep -a 'kernel exception\|vm: fault\|panic:' "$so_log" | head -2 | tr -d '\r' | tr '\n' ' ')"
        return 1
    fi
    if ! grep -aq "pid $so_pid ran past the end of its kernel stack" "$so_log"; then
        echo "stackoverrun: $so_where: fail: the record did not name pid $so_pid, which descended"
        return 1
    fi
    echo "stackoverrun: $so_where: pass"
}
