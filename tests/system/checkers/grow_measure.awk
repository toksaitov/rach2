# Compare growroot's samples with independent proc reads around the tools.
# Clock differences are delivered guest ticks, not independent wall time.
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
    # Discovery reads precede table/before. Everything after resize/after is
    # RAM work, so its counters must equal the independent final snapshot.
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
