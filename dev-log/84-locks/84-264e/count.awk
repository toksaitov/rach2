# Count QEMU's memory_region_ops trace lines by region name, address, size
# and direction, and rewrite the totals in `out` every 20000 lines, since the
# test runner may kill this process when its case ends.
function dump(    k) {
    for (k in n) print n[k], k > out
    print lines, "lines-read" > out
    close(out)
}
{ lines++ }
/memory_region_ops_(read|write)/ {
    dir = ($0 ~ /ops_read/) ? "r" : "w"
    addr = ""; name = ""; size = ""
    for (i = 1; i <= NF; i++) {
        if ($i == "addr") addr = $(i + 1)
        else if ($i == "size") size = $(i + 1)
        else if ($i == "name") name = $(i + 1)
    }
    n[name " " addr " " size " " dir]++
}
lines % 20000 == 0 { dump() }
END { dump() }
