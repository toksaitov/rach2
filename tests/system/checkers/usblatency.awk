# Bound pumps beside a spinner and bytes beside getrandom; times are diagnostic.
# The log is the machine's console, so every line ends in a carriage return.
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
