# usblatency.awk: check the `usb: latency:` records of a keystroke boot.
#
# There must be as many records as `-v keys` says, 1 when it is not given.
# Each must name each of its eight fields once, each an integer, with one
# tracked event and one dispatch. With `-v random=1`, the boot ran the probe
# beside a getrandom loop, and a record may show pumps and up to 4096 bytes
# of work. Otherwise it ran beside a spinner: every record has no pumps and
# no work, and at least one has `msi 1`, since a key the timer's tick took
# was dispatched by that tick and shows no pump whether or not the
# controller's interrupt preempts. The times are printed for a person and not
# judged. The exit status is 0 when the records are valid and 1 otherwise.
# The log is the machine's console, so every line ends in a carriage return.
BEGIN { if (keys == "") keys = 1 }
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
    if (value["msi"] == 1) interrupt++
    seen++
}
END { exit !(seen == keys && !bad && (random || interrupt > 0)) }
