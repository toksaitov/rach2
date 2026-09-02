# shellcheck shell=sh
# e2fsprogs_path.sh: put e2fsprogs on the path where a package manager keeps
# it off. Sourced by preflight.sh, doctor.sh and tools/image/mkext2.sh, so the
# three look in the same places: the path first, then the Homebrew keg, which
# is keg-only and never linked, then /sbin and /usr/sbin, where Debian keeps
# the administrator's programs off an ordinary user's path.
for d in /opt/homebrew/opt/e2fsprogs/sbin /usr/local/opt/e2fsprogs/sbin /sbin /usr/sbin; do
    case ":$PATH:" in
        *":$d:"*) ;;
        *) [ -d "$d" ] && PATH="$PATH:$d" ;;
    esac
done
# The three programs come from one installation: the first directory on
# the path that has all three goes first, so that an older e2fsck found
# earlier on the path is not paired with a newer mke2fs and debugfs.
_e2fs_ifs=$IFS
IFS=:
for d in $PATH; do
    if [ -x "$d/mke2fs" ] && [ -x "$d/debugfs" ] && [ -x "$d/e2fsck" ]; then
        PATH="$d:$PATH"
        break
    fi
done
IFS=$_e2fs_ifs
unset _e2fs_ifs
export PATH
