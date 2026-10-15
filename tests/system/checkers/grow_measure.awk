# grow_measure.awk: check growroot's --measure lines in a grow suite's log
# against the counters fixtures/grow_tool.sh read from /proc/disk.
#
# growroot prints a `growroot: measure:` line before and after each of its
# four phases, table, check-readonly, check-repair and resize, with the
# monotonic clock and the disk's command and flush counts. The lines must
# come in that order with counts that never fall, and each phase must issue
# at least one disk command; the script prints each phase's differences. A
# phase may take no clock time, since the clock advances by whole ticks.
# grow_tool.sh prints a `grow-witness:` line for the whole grow and for each
# side of the three phases that run a tool. The exit status is 0 when all
# eight lines of each kind are there and agree, and 1 otherwise.
BEGIN { split("table check-readonly check-repair resize", phases, " ") }
$1 == "growroot:" && $2 == "measure:" {
    n++
    if (NF != 10 || n > 8 || $3 != phases[int((n + 1) / 2)] ||
        $4 != (n % 2 ? "before" : "after") ||
        $5 != "monotonic-us" || $7 != "disk-commands" || $9 != "disk-flushes" ||
        $6 !~ /^[0-9]+$/ || $8 !~ /^[0-9]+$/ || $10 !~ /^[0-9]+$/ ||
        $6 + 0 < time || $8 + 0 < commands || $10 + 0 < flushes) bad = 1
    print
    key = $3 "/" $4
    c[key] = $8 + 0; f[key] = $10 + 0
    if (n % 2 == 0) {
        if ($8 + 0 <= commands) bad = 1
        printf "grow-test: phase: %s %.0f guest-tick-us %.0f commands %.0f flushes\n",
            $3, $6 - time, $8 - commands, $10 - flushes
    }
    time = $6 + 0; commands = $8 + 0; flushes = $10 + 0
}
$1 == "grow-witness:" {
    key = $2 "/" $3
    if (NF != 7 || $4 != "disk-commands" || $6 != "disk-flushes" ||
        $5 !~ /^[0-9]+$/ || $7 !~ /^[0-9]+$/ || seen[key]++) bad = 1
    wc[key] = $5 + 0; wf[key] = $7 + 0
    w++
}
END {
    # growroot reads the disk before table/before, and only memory after
    # resize/after, so the last sample must equal the whole grow's witness.
    if (!seen["whole/before"] || !seen["whole/after"] ||
        c["table/before"] < wc["whole/before"] ||
        f["table/before"] < wf["whole/before"] ||
        c["resize/after"] != wc["whole/after"] ||
        f["resize/after"] != wf["whole/after"]) bad = 1
    for (i = 2; i <= 4; i++) {
        for (j = 0; j < 2; j++) {
            key = phases[i] "/" (j ? "after" : "before")
            if (!seen[key] || c[key] != wc[key] || f[key] != wf[key]) bad = 1
        }
    }
    exit bad || n != 8 || w != 8
}
