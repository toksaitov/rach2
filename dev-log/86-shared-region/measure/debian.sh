#!/bin/sh
# debian.sh: task 5676's measurement of piglit's streaming-texture-leak in
# the Debian reference guest under the virgl profile, KVM, 8 CPUs and
# 8 GiB, with QEMU's virtio-gpu trace events on.
#
#   sh dev-log/86-shared-region/measure/debian.sh OUT
#
# Boots the guest by tests/graphics/debian-ref/debian-ref.sh run, installs
# piglit from the snapshot for that boot as tests/graphics/runners/debian.sh
# piglit does, runs stl.sh there with guest-env.sh's conformance class, and
# puts in OUT the guest's stl-out (stl.sh's files), the run's record and
# QEMU's trace, `trace`, of the events `events` names. debian-ref.sh starts
# QEMU by its name, so a wrapper of that name first in PATH adds the trace's
# words to the guest's QEMU and to no other call.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
real=$(command -v qemu-system-x86_64)
wrap=
trap '[ -z "$wrap" ] || rm -rf "$wrap"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$tree/build/tmp"
wrap=$(mktemp -d "$tree/build/tmp/rach2-stl-wrap.XXXXXX")
cat > "$wrap/qemu-system-x86_64" <<WRAP
#!/bin/sh
case " \$* " in
    *" -drive "*) exec "$real" "\$@" -trace events=$here/events -D $out/trace -msg timestamp=on ;;
esac
exec "$real" "\$@"
WRAP
chmod +x "$wrap/qemu-system-x86_64"
rm -f "$out/trace"
PATH=$wrap:$PATH RACH2_QEMU_ACCEL=kvm ACCEL=kvm sh "$tree/tests/graphics/debian-ref/debian-ref.sh" run \
    -p virgl -c 8 -m 8192 -g conformance -d -r "$out/record" \
    -i "$here/stl.sh" -i "$here/ioctlcount.c" -o "$out/stl-out.tar.gz" \
    "sudo DEBIAN_FRONTEND=noninteractive apt-get -q -y install piglit >> /tmp/apt.log 2>&1 &&
     sh /tmp/stl.sh /tmp /tmp/stl-out; tar -czf /tmp/stl-out.tar.gz -C /tmp stl-out"
tar -xzf "$out/stl-out.tar.gz" -C "$out"
echo "debian: result: $(cat "$out/stl-out/result" | head -n 2 | tr '\n' ' ')"
