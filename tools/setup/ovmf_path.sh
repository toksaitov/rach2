# shellcheck shell=sh disable=SC2034 # the variables are the callers'
# ovmf_path.sh: find OVMF, QEMU's UEFI firmware for x86-64, which the suites
# that boot by UEFI give QEMU. Sourced by doctor.sh and by those suites, so
# the report and the suites look in the same places. It sets OVMF_CODE to the
# firmware's code image and OVMF_VARS to the template of its variable store,
# each empty when no file is found, and OVMF_PATH and OVMF_ARGS to the code
# image and the arguments that give it to qemu-system-x86_64: the code as the
# first flash device, read-only, with the firmware's variables kept in
# memory, which a boot of a test disk needs no more of.
#
# RACH2_OVMF_CODE and RACH2_OVMF_VARS, when set, name the two files and are
# taken as they are. Otherwise the first pair found is used: the one QEMU
# ships in its own data directory beside its binary, which is where Homebrew
# installs it on macOS (share/qemu/edk2-x86_64-code.fd, with the variable
# store edk2-i386-vars.fd, which serves both x86 firmwares); then the ovmf
# package of Debian and Ubuntu, whose 4 MiB build is OVMF_CODE_4M.fd and
# whose older 2 MiB build is OVMF_CODE.fd. A code image and a variable store
# are taken in pairs of one build, since QEMU refuses a store of the other
# size. The Secure Boot builds are left out: the pinned GRUB is not signed.
OVMF_CODE=${RACH2_OVMF_CODE:-}
OVMF_VARS=${RACH2_OVMF_VARS:-}
if [ -z "$OVMF_CODE" ]; then
    _ovmf_qemu=$(command -v qemu-system-x86_64 2>/dev/null) || _ovmf_qemu=
    _ovmf_share=
    [ -n "$_ovmf_qemu" ] &&
        _ovmf_share=$(cd "$(dirname "$_ovmf_qemu")/../share/qemu" 2>/dev/null && pwd)
    for _ovmf_pair in \
        "$_ovmf_share/edk2-x86_64-code.fd:$_ovmf_share/edk2-i386-vars.fd" \
        /usr/share/OVMF/OVMF_CODE_4M.fd:/usr/share/OVMF/OVMF_VARS_4M.fd \
        /usr/share/OVMF/OVMF_CODE.fd:/usr/share/OVMF/OVMF_VARS.fd; do
        if [ -f "${_ovmf_pair%%:*}" ] && [ -f "${_ovmf_pair#*:}" ]; then
            OVMF_CODE=${_ovmf_pair%%:*}
            OVMF_VARS=${_ovmf_pair#*:}
            break
        fi
    done
    unset _ovmf_qemu _ovmf_share _ovmf_pair
fi
OVMF_PATH=$OVMF_CODE
OVMF_ARGS=
[ -n "$OVMF_CODE" ] && OVMF_ARGS="-drive if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
