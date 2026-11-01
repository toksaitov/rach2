# The Linux AP guest

The Wi-Fi suite's access points run in a Linux guest of their own: Alpine Linux 3.24's `linux-lts` kernel with `mac80211_hwsim`, hostapd 2.11 on four software radios, dnsmasq and NAT, booted from one initramfs that `make wifi-guest` builds from pinned packages. Its relay card carries the radios' frames to a Rach2 machine in the UDP format of [the wireless Readme](../../../rach2/sys/network/wireless/Readme.md), through `hwsimrelay` of task 15 once that program is built. The same guest records the reference nl80211 traces that `tests/system/fixtures/wifi/` keeps, of Linux's own wpa_supplicant joining its hostapd.

## Running

```sh
make wifi-guest
sh tests/system/wifi/apguest.sh                 # the guest on the terminal, at `wifiap# `
sh tests/system/wifi/apguest.sh -m check        # boot, check the beacons, power off
sh tests/system/wifi/apguest.sh -p sae -d       # as a suite's peer: `apguest: ready: sae`
sh tests/system/wifi/apguest.sh -R os-real      # beside a Rach2 machine of ARCH
sh tests/system/wifi/apguest.sh -m trace -o tests/system/fixtures/wifi
```

`apguest.sh`'s comment gives its options. Every boot runs `wifiap check`, a passive scan from the guest's own station radio, which prints `wifiap: beacon: BSSID SSID` for each access point it hears and `wifiap: check: ok` when it heard all three. The boot ends with `wifiap: ready: PROFILE MODE, at T s` or `wifiap: failed: MODE`. In peer mode the guest's console is a TCP server on the host's port BASE+4, where `wifiap start PROFILE`, `hostapd_cli` and `iw` run.

## Results

The guest boots and its beacons are heard on the three machines, measured on 2026-11-02 with `apguest.sh -m check`, which boots, scans and powers off:

| Machine | QEMU | Accelerator | Ready, guest time | Whole run |
|---|---|---|---|---|
| Mac, Apple M4 | 11.1.3 | TCG | 3.2 to 5.5 s | 5.2 to 6.7 s |
| Testing host, WSL | 10.2.1 | TCG | 4.1 s | 4.8 s |
| Testing host, Windows | 11.1.2 (rach2-whpx-d0) | WHPX | 1.8 s | 2.8 s |
| Testing host, Windows | 11.1.2 (rach2-whpx-d0) | TCG | 3.7 s | 4.7 s |

The trace boot, three joins in a row, takes 40 s on the testing host and 42 s on the Mac, most of it wpa_supplicant's scans. KVM did not start on the testing host: its WSL kernel shows no `svm` flag and has no `kvm_amd`, although `.wslconfig` asks for nested virtualization, so QEMU fell back to TCG, which the host's runs ask for anyway ([tools/Readme.md](../../../tools/Readme.md)). The Mac, Windows and WSL build the same initramfs, byte for byte.

## Interfaces

The kernel's command line sets the guest: `wifiap.profile=PROFILE`, `wifiap.run=shell|check|trace` and `wifiap.peer=PORT`, as `root/sbin/wifiap` says. A profile is network A's security: `open`, `tkip` (WPA), `wpa2`, `sae` (WPA3 with management-frame protection required), `transition` (WPA2 and WPA3, protection optional), `enterprise` (WPA2-Enterprise against `server.pem`) and `enterprise2` (the same against `server2.pem`).

| Radio | Channel | BSSID | Network |
|---|---|---|---|
| wlan0 | 1 | 02:00:00:00:00:00 | A, `rach2-net`, the profile's security, in bridge br0 |
| wlan1 | 6 | 02:00:00:00:01:00 | A, the second access point of the same network |
| wlan2 | 11 | 02:00:00:00:02:00 | B, `rach2-other`, WPA2 always |
| wlan3 | | 02:00:00:00:03:00 | the guest's own station, for the check and the traces |

* Network A: passphrase or SAE password `rach2-net-psk`; 192.168.71.1/24 and fd71::1/64 on br0; DHCP from 192.168.71.50, so a single station holds .50.
* Network B: passphrase `rach2-other-psk`; 192.168.72.1/24 and fd72::1/64; DHCP from 192.168.72.50.
* Enterprise: phase 1 identity `student`, `anonymous@university.test` (eduroam's outer identity) or `client@wifi.test` for EAP-TLS with `client.pem` and `client.key`; phase 2 user `student`, password `rach2-wifi`, under PEAP's MSCHAPv2 or GTC or TTLS's PAP or MSCHAPv2. Both server certificates have the subject `/C=KG/O=Rach2 University/CN=radius.university.test`; their SHA-256 fingerprints are `d3df4010...16f564` for `server.pem` and `aca3e0d9...77bf0c` for `server2.pem`, as `mkcerts.sh` prints them.
* DNS: dnsmasq answers `endpoint.wifi.test` with 10.0.2.2, which is the host, `ap.wifi.test` and `ap-other.wifi.test`, and forwards the rest to QEMU's 10.0.2.3. Router advertisements give each network's prefix for SLAAC.
* eth0, on QEMU's 10.0.2.0/24, masquerades the stations' traffic, so a station reaches the host's 10.0.2.2, the suite's traffic endpoint; its ports 2222 and 2223, the host's BASE+2 and BASE+3, reach port 22 of 192.168.71.50 and 192.168.72.50.
* eth1, 10.0.3.15 on QEMU's 10.0.3.0/24, is the relay's card. The guest runs `/usr/sbin/hwsimrelay 5550 10.0.3.2 PORT` once the check has passed when `wifiap.peer=PORT` is given, and fails the boot when it has no such program: the relay takes its frames on UDP port 5550, the host's BASE, and sends to the host's PORT, the Rach2 machine's relay card, whose port 5550 is the host's BASE+1. The two machines' frames meet only through these two forwards of QEMU's user network, which every host's QEMU has.

`tests/system/fixtures/wifi/` keeps, for wpa2, sae and peap, `NAME.pcap`, every netlink message of the join as `nlmon` saw it (link type 253, `LINKTYPE_NETLINK`, each with its 16-byte cooked header); `NAME.pids`, the processes of wpa_supplicant and the three hostapds, whose netlink port IDs are those PIDs or have them in their low 22 bits; and `NAME.log`, wpa_supplicant's `-d` log. A request of wpa_supplicant's carries its port ID as `nlmsg_pid`; the guest's family IDs are 16 for `nlctrl` and 34 for `nl80211`. Linux's mac80211 offers EAPOL over nl80211, so the traces carry the handshake as `NL80211_CMD_CONTROL_PORT_FRAME`, 21 requests under PEAP; a server that does not set `NL80211_EXT_FEATURE_CONTROL_PORT_OVER_NL80211` gets them through a packet socket of 0x888E instead, as wpa_supplicant falls back.

## Sources and license

The guest is Alpine Linux's packages, unmodified, each under the license its package states: the kernel and BusyBox GPL-2.0-only; hostapd, wpa_supplicant, tcpdump and libpcap BSD-3-Clause; iw ISC; musl MIT; OpenSSL Apache-2.0; dnsmasq, iptables, libxtables and libnftnl GPL-2.0-or-later; libnl and libmnl LGPL-2.1-or-later; D-Bus's library AFL-2.1 or GPL-2.0-or-later; pcsc-lite's BSD and ISC. They run in the test machine alone and are never staged in a Rach2 image. The files under `root/` and `certs/` are Rach2's.

## Design

* The kernel is Alpine's because it is the one measured that has everything: `mac80211_hwsim`, cfg80211, mac80211, `nlmon`, the bridge, nf_tables and e1000 as modules, in a pinned 146 MiB package. Alpine's `linux-virt`, a third of the size, has no wireless stack, and a kernel built from Linux's tarball would need a Linux build host on the Mac and on Windows; the testing host's WSL kernel has no `mac80211_hwsim`.
* The guest is one uncompressed initramfs and no disk, so it boots the same under every accelerator and changes nothing between boots. `mkcpio.c` writes it, copying the packages' tar streams into it member by member: macOS's and Windows' file systems ignore case, which merged iptables' `libxt_MARK.so` and `libxt_mark.so`, and MSYS2's tar can neither keep a program's executable bit nor make a link to an absolute target. The kernel's modules are decompressed at build time, since the kernel has no module decompression, and `/init` loads them by path in the order `modules.dep` gives, so the guest needs no modprobe.
* Network A's two access points are bridged, so a station that roams between them keeps its lease and its connections.
* The traces come out on the serial console in base64, with each file's SHA-256, since the console is the one channel every QEMU has without a disk or a share.

## Building

`make wifi-guest` runs `mkguest.sh`, which writes `build/wifi/guest/vmlinuz` and `initrd`, about 14 and 29 MB, in some seconds once the packages are in the cache, `build/cache/wifi-guest`. `make` alone does not build the guest. The certificates are committed; `mkcerts.sh` makes new ones, which changes the fingerprints a suite pins.

## Updating

Alpine keeps only the newest build of each package in a release's repository, so a pinned file vanishes from the mirrors when Alpine rebuilds that package. A new build is a new line of `packages.txt`, with the SHA-256 of the file downloaded from `dl-cdn.alpinelinux.org` over HTTPS; a machine whose cache has the old file goes on building until its cache is cleared. A new package's libraries come with it as lines of their own. The traces are made again with `apguest.sh -m trace -o tests/system/fixtures/wifi` after a change to the kernel or to wpa_supplicant.

## Limits

* hostapd and wpa_supplicant are 2.11, Alpine's, not the 2.12 that the musl userland builds; task 4's static hostapd can take hostapd's place once it runs on Linux.
* `hwsimrelay` is not yet in the guest; until it is, `-d` and `-R` fail at the boot's relay step, after the check.
* When a relay registers with hwsim's netlink medium, every radio's frames go to it and none between the guest's radios, so `wifiap check` and `wifiap trace` run before the relay starts and not after.
* A pinned package can vanish from Alpine's mirrors, as Updating says.
