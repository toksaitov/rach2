# shellcheck shell=sh
# e2fsprogs_path.sh: put e2fsprogs on the path where a package manager keeps
# it off. Sourced by preflight.sh, doctor.sh and tools/image/mkext2.sh, so the
# three look in the same places: the path first, then the Homebrew keg, which
# is keg-only and never linked, then /sbin and /usr/sbin, where Debian keeps
# the administrator's programs off an ordinary user's path.
for _e2fs_d in /opt/homebrew/opt/e2fsprogs/sbin /usr/local/opt/e2fsprogs/sbin /sbin /usr/sbin; do
    case ":$PATH:" in
        *":$_e2fs_d:"*) ;;
        *) [ -d "$_e2fs_d" ] && PATH="$PATH:$_e2fs_d" ;;
    esac
done
# The three programs come from one installation: the first directory on
# the path that has all three goes first, so that an older e2fsck found
# earlier on the path is not paired with a newer mke2fs and debugfs.
_e2fs_ifs=$IFS
IFS=:
for _e2fs_d in $PATH; do
    if [ -x "$_e2fs_d/mke2fs" ] && [ -x "$_e2fs_d/debugfs" ] && [ -x "$_e2fs_d/e2fsck" ]; then
        PATH="$_e2fs_d:$PATH"
        break
    fi
done
IFS=$_e2fs_ifs
unset _e2fs_ifs _e2fs_d
export PATH
