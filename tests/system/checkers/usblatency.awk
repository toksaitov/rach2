# usblatency.awk: check the one `usb: latency:` record of a keystroke boot.
#
# The record must name each of its eight fields once, each an integer, with
# one tracked event and one dispatch. With `-v random=1`, the boot ran the
# probe beside a getrandom loop, and the record may show pumps and up to
# 4096 bytes of work; otherwise it ran beside a spinner and both are 0. The
# times are printed for a person and not judged. The exit status is 0 for
# one valid record and 1 otherwise. The log is the machine's console, so
# every line ends in a carriage return.
{ sub(/\r$/, "") }
/usb: latency: msi [01],/ {
    delete value
    delete count
    for (i = 1; i < NF; i++) {
        name = $i
        if (name ~ /^(msi|entry|wake|read|tracked|dispatch|pumps|work)$/) {
            number = $(i + 1)
            sub(/,$/, "", number)
            if (number !~ /^-?[0-9]+$/) bad = 1
            value[name] = number + 0
            count[name]++
        }
    }
    split("msi entry wake read tracked dispatch pumps work", required, " ")
    for (i = 1; i <= 8; i++) if (count[required[i]] != 1) bad = 1
    if (value["tracked"] != 1 || value["dispatch"] != 1) bad = 1
    if (value["pumps"] < 0 || (!random && value["pumps"] != 0)) bad = 1
    if (value["work"] < 0 || value["work"] > (random ? 4096 : 0)) bad = 1
    seen++
}
END { exit !(seen == 1 && !bad) }
