# shellcheck shell=sh disable=SC2034 # fetch_agents.sh reads the variables
# muse, Meta's Muse Code, a coding agent for the terminal; a static
# executable, Meta's own build. Meta's installer installs a launcher that
# reads the channel manifest, api.meta.ai/muse-code/channels/muse-stable,
# for the current version and the address of its release manifest, which
# lists each platform's file with its SHA-256; Meta signs neither. The pin
# is the SHA-256 that the release manifest of VERSION lists for x86_linux.
# The file is installed unmodified as /opt/muse/bin/muse, without the
# launcher, which would replace it, and /usr/bin/muse links to it. Meta
# publishes no license file: the program comes under the Meta Model API
# Terms of Service, whose address and terms for the program the recipe
# writes into /opt/muse/TERMS.
VERSION=1.4.10-R6427.1
LICENSE="Meta Model API Terms of Service"
FILES="muse-x86-linux https://lookaside.facebook.com/lookaside/muse/download/?channel=muse&version=$VERSION&file=muse-x86-linux sha256:decefc0fb33c48ee43419c3e72ac2dbc781743868611074c7292807f5960dfcf"
NOTICES=opt/muse/TERMS

package_stage()
{
    program=$(fetched muse-x86-linux)
    mkdir -p "$1/opt/muse/bin" "$1/usr/bin"
    cp "$program" "$1/opt/muse/bin/muse"
    chmod 0755 "$1/opt/muse/bin/muse"
    cat > "$1/opt/muse/TERMS" <<EOF
Muse Code $VERSION, Meta's build for Linux on x86-64, was fetched from
Meta's release for the person who built this system and is installed as
Meta published it.

Meta gives it under the Meta Model API Terms of Service,
https://dev.meta.ai/legal/terms-of-service. They grant a limited,
personal, non-transferable and revocable license to install and use it,
and forbid modifying it, redistributing it or making it available to
anyone else.
EOF
    ln -s ../../opt/muse/bin/muse "$1/usr/bin/muse"
}

package_check()
{
    out=$(HOME="$SCRATCH" XDG_CONFIG_HOME="$SCRATCH/config" XDG_DATA_HOME="$SCRATCH/data" \
        XDG_STATE_HOME="$SCRATCH/state" XDG_CACHE_HOME="$SCRATCH/cache" \
        "$1/opt/muse/bin/muse" --version)
    if [ "$out" != "Muse Code ${VERSION%%-*} ($VERSION)" ]; then
        echo "muse: --version printed [$out], not version $VERSION" >&2
        return 1
    fi
}
