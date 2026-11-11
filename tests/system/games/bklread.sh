#!/bin/sh
# bklread.sh KERNEL PROFILE [ENTRY...]: name the samples of debugfs's
# bkl/profile, which bkl.sh saves, by the kernel.debug KERNEL that took them:
# the samples of each entry, and for each ENTRY, such as `syscall 9` or
# `ioctl 0x40406469`, its sampled functions and its paths of four frames,
# each with its count and its share of the entry. Run on the host;
# ADDR2LINE names the binutils' addr2line for the kernel's architecture.
set -u

[ $# -ge 2 ] || { echo "usage: bklread.sh KERNEL PROFILE [ENTRY...]" >&2; exit 2; }
kernel=$1
profile=$2
shift 2
a2l=${ADDR2LINE:-x86_64-elf-addr2line}
[ -r "$kernel" ] || { echo "bklread: no $kernel" >&2; exit 1; }
[ -r "$profile" ] || { echo "bklread: no $profile" >&2; exit 1; }
command -v "$a2l" > /dev/null || { echo "bklread: no $a2l; set ADDR2LINE" >&2; exit 1; }

# addr2line -a gives each address, padded with zeros, then its function and
# its line; the profile writes an address without them.
awk 'NR > 2 { for (i = 3; i <= 6; i++) print $i }' "$profile" | sort -u |
    "$a2l" -a -f -e "$kernel" | paste - - - |
    awk -v want="$*" '
    function bare(a) { sub(/^0x0*/, "0x", a); return a == "0x" ? "0x0" : a }
    NR == FNR { name[bare($1)] = $2; next }
    FNR == 1 { total = $2; next }
    FNR == 2 { lost = $2; next }
    {
        e = $1 " " $2
        n[e] += $7
        leaf[e, name[bare($3)]] += $7
        path = name[bare($3)]
        for (i = 4; i <= 6; i++) {
            path = path " <- " (bare($i) == "0x0" ? "-" : name[bare($i)])
        }
        paths[e, path] += $7
    }
    END {
        printf "samples %d, lost %d\n", total, lost
        for (e in n) {
            printf "entry %d %.1f%% %s\n", n[e], 100 * n[e] / total, e | "sort -k2 -rn"
        }
        close("sort -k2 -rn")
        m = split(want, w, " ")
        for (j = 1; j + 1 <= m; j += 2) {
            e = w[j] " " w[j + 1]
            printf "\n%s: %d samples\n", e, n[e]
            for (k in leaf) {
                split(k, part, SUBSEP)
                if (part[1] == e) {
                    printf "function %d %.1f%% %s\n", leaf[k], 100 * leaf[k] / n[e], part[2] | "sort -k2 -rn | head -n 20"
                }
            }
            close("sort -k2 -rn | head -n 20")
            for (k in paths) {
                split(k, part, SUBSEP)
                if (part[1] == e) {
                    printf "path %d %.1f%% %s\n", paths[k], 100 * paths[k] / n[e], part[2] | "sort -k2 -rn | head -n 40"
                }
            }
            close("sort -k2 -rn | head -n 40")
        }
    }' - "$profile"
