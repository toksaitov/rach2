#!/bin/sh
# Read the installed admission policy for a native routine invocation.

native_site_refuse()
{
    echo "native-site: $*" >&2
    return 2
}

native_site_positive()
{
    case $1 in ''|*[!0-9]*|0*) return 1 ;; esac
    [ "${#1}" -le 15 ]
}

# A disabled physical endpoint can be inspected through its fixed authority.
# This probe grants no work; only the native owner can open a phase.
native_site_route()
{
    native_site_routed=''
    native_site_mode=''
    native_site_authority_program=''
    [ -n "${ETUDE_ROUTINE_ROUTE:-}" ] || return 0
    native_site_tree=${tree:-}
    [ -n "$native_site_tree" ] || { native_site_refuse 'caller tree is unavailable'; return 2; }
    native_site_mode_script=$native_site_tree/tools/ci/routine_entry.py
    case $(uname -s) in
        MINGW*|MSYS*|UCRT*|CLANG*) native_site_mode_script=$(cygpath -au "$native_site_mode_script") ;;
    esac
    native_site_mode=$(python3 "$native_site_mode_script" mode "$ETUDE_ROUTINE_ROUTE") || return 2
    case $native_site_mode in
        macos-import)
            [ "$(uname -s)" = Darwin ] || { native_site_refuse 'Mac import requires its native site'; return 2; }
            return 0 ;;
        windows-import|wsl-native) ;;
        *) native_site_refuse 'invalid routine route mode'; return 2 ;;
    esac
    native_site_binding=/etc/rach2/etude-authority
    case $(uname -s) in
        MINGW*|MSYS*|UCRT*|CLANG*) native_site_binding=C:/ProgramData/rach2/etude-authority ;;
    esac
    native_site_authority_program=$(awk '
        /^program / { value=substr($0,9); count++ }
        END { if (count != 1 || value == "") exit 2; print value }
    ' "$native_site_binding") || return 2
    case $native_site_authority_program in
        /*|[A-Za-z]:/*) ;;
        *) native_site_refuse 'invalid authority program'; return 2 ;;
    esac
    if [ ! -x "$native_site_authority_program" ] ||
        ! "$native_site_authority_program" authority route "$1"; then
        native_site_refuse 'routine route does not match the installed authority'; return 2;
    fi
    native_site_routed=1
}

# This uses the same version-two fields and bounds as etude/launch.sh.
native_site_load() # native_site_load ABSOLUTE_SITE
{
    native_site_route "$1" || return 2
    native_site_file=$1
    native_site_header='' native_site_endpoint='' native_site_launcher=''
    native_site_ledger='' native_site_locks='' native_site_etude=''
    native_site_capacity='' native_site_wait='' native_site_seconds=''
    native_site_boundary='' native_site_seen=' '
    [ -r "$native_site_file" ] || { native_site_refuse "cannot read $native_site_file"; return 2; }
    while IFS=' ' read -r native_site_key native_site_value || [ -n "$native_site_key$native_site_value" ]; do
        case $native_site_seen in *" $native_site_key "*) native_site_refuse "duplicate field $native_site_key"; return 2 ;; esac
        native_site_seen="$native_site_seen$native_site_key "
        [ -n "$native_site_value" ] || { native_site_refuse "empty field $native_site_key"; return 2; }
        case $native_site_key in
            etude-site) native_site_header=$native_site_value ;;
            endpoint) native_site_endpoint=$native_site_value ;;
            launcher) native_site_launcher=$native_site_value ;;
            ledger) native_site_ledger=$native_site_value ;;
            locks) native_site_locks=$native_site_value ;;
            etude) native_site_etude=$native_site_value ;;
            capacity) native_site_capacity=$native_site_value ;;
            wait) native_site_wait=$native_site_value ;;
            seconds) native_site_seconds=$native_site_value ;;
            boundary_lazy) native_site_boundary=$native_site_value ;;
            *) native_site_refuse "unknown field $native_site_key"; return 2 ;;
        esac
    done < "$native_site_file"
    native_site_validate || return 2
    native_site_build_resources
}

native_site_validate()
{
    [ "$native_site_header" = 2 ] || { native_site_refuse 'incompatible site format'; return 2; }
    case $native_site_boundary in ''|bigmem-boundary-v1) ;; *) native_site_refuse 'invalid boundary profile'; return 2 ;; esac
    if [ -n "$native_site_routed" ]; then
        [ "$native_site_etude" = "$native_site_authority_program" ] || {
            native_site_refuse 'site program differs from the installed authority'; return 2;
        }
        if [ "$native_site_endpoint" = disabled ]; then
            case $(uname -s) in
                Linux) native_site_endpoint=linux ;;
                Darwin) native_site_endpoint=macos ;;
                MINGW*|MSYS*|UCRT*|CLANG*) native_site_endpoint=windows ;;
            esac
        fi
    fi
    case $(uname -s)/$native_site_endpoint in
        Linux/linux|Darwin/macos|MINGW*/windows|MSYS*/windows|UCRT*/windows|CLANG*/windows) ;;
        *) native_site_refuse 'endpoint is disabled or incompatible'; return 2 ;;
    esac
    for native_site_path in "$native_site_file" "$native_site_launcher" "$native_site_ledger" "$native_site_locks" "$native_site_etude"; do
        case $native_site_path in /*|[A-Za-z]:/*) ;; *) native_site_refuse "not an absolute path: $native_site_path"; return 2 ;; esac
    done
    [ -x "$native_site_launcher" ] && [ -x "$native_site_etude" ] &&
        [ -d "$native_site_locks" ] && [ -d "$native_site_ledger" ] || {
        native_site_refuse 'installed endpoint is unavailable'; return 2;
    }
    IFS= read -r native_site_line < "$native_site_ledger/claims" || return 2
    [ "$native_site_line" = 'etude-ledger 1' ] || { native_site_refuse 'incompatible ledger'; return 2; }
    printf '%s\n' "$native_site_capacity" | awk -F, '
        NF != 3 { exit 1 }
        { for (i = 1; i <= 3; i++) if ($i !~ /^[1-9][0-9]*$/ || length($i) > 15) exit 1 }
    ' || { native_site_refuse 'invalid capacity'; return 2; }
    for native_site_input in "${ETUDE_REFERENCE:-}" "${ETUDE_PREPARATION_IMPORT:-}"; do
        [ -n "$native_site_input" ] || continue
        case $native_site_input in
            /*|[A-Za-z]:/*) ;;
            *) native_site_refuse 'preparation configuration must be absolute'; return 2 ;;
        esac
        [ -f "$native_site_input" ] && [ -r "$native_site_input" ] || {
            native_site_refuse 'preparation configuration is unavailable'; return 2;
        }
    done
    for native_site_bound in "$native_site_wait" "$native_site_seconds"; do
        native_site_positive "$native_site_bound" && [ "${#native_site_bound}" -le 5 ] &&
            [ "$native_site_bound" -le 86400 ] || { native_site_refuse 'invalid time bound'; return 2; }
    done
}

native_site_build_resources()
{
    native_site_jobs=${JOBS:-4}
    native_site_positive "$native_site_jobs" && [ "$native_site_jobs" -le 1024 ] || {
        native_site_refuse 'invalid JOBS'; return 2;
    }
    native_site_cpu=$((native_site_jobs * 1000))
    native_site_memory=$(((native_site_jobs + 1) * 1073741824))
    native_site_disk=42949672960
    native_site_memory_cap=${native_site_capacity#*,}
    native_site_memory_cap=${native_site_memory_cap%%,*}
    [ "$native_site_cpu" -le "${native_site_capacity%%,*}" ] &&
        [ "$native_site_memory" -le "$native_site_memory_cap" ] &&
        [ "$native_site_disk" -le "${native_site_capacity##*,}" ] || {
        native_site_refuse 'preparation exceeds site capacity'; return 2;
    }
    native_site_resources=$native_site_cpu,$native_site_memory,$native_site_disk
    native_site_account=$(id -un) || return 2
    [ -n "$native_site_account" ] || return 2
    export native_site_resources native_site_account native_site_jobs
}
