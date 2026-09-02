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
export PATH
