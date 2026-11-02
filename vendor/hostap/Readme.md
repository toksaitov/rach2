# hostap

hostap is Linux's Wi-Fi user space: wpa_supplicant, the station that finds, joins and keeps a network, with `wpa_cli` and `wpa_passphrase` beside it, and hostapd, an access point, with `hostapd_cli`. Rach2 builds them unmodified and statically against the ported [musl](../musl/Readme.md), [OpenSSL](../openssl/Readme.md) and [libnl](../libnl/Readme.md), and they reach the radio through nl80211 as on Linux. `os-real`, the metal disk and the recovery system stage the station's three programs. No Rach2 image stages hostapd, since Rach2 is never an access point; it is the access point of the Wi-Fi suite's Linux guest.

## Use

```
wpa_passphrase home 'a long passphrase' >> /etc/wpa_supplicant/wlan0.conf
wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant/wlan0.conf
wpa_cli -i wlan0 status
```

[wpa_supplicant(8)](../../man/man8/wpa_supplicant.8), [wpa_supplicant.conf(5)](../../man/man5/wpa_supplicant.conf.5), [wpa_cli(8)](../../man/man8/wpa_cli.8), [wpa_passphrase(8)](../../man/man8/wpa_passphrase.8) and [hostapd(8)](../../man/man8/hostapd.8) describe the programs as built here.

## Sources and license

The project releases the two programs as two tarballs of one tree, each with the shared `src/` and its own program's directory: `wpa_supplicant-2.12.tar.gz` and `hostapd-2.12.tar.gz` from <https://w1.fi/releases/>. Their signatures, made with Jouni Malinen's key `EC4AA0A991A5F2464582D52D2B6EF432EFC895FA`, were verified with gpg; [`build_hostap.sh`](build_hostap.sh) checks their SHA-256 digests, `08e23937e16d0155e55cab2b51f51fbe10d80a1aa91c4e15442645059b737ef6` and `f43502561c28ba47ab77e18e1a973d07361c68cc8b14178e619bd5796b70eabd`, before extracting them.

hostap is under the BSD license that `COPYING` gives. Every image that stages a program of it stages `COPYING` as `/etc/hostap.COPYING`, with the notices of the libraries it links.

## Local changes

There are no source patches. Each program is built in its own tarball's tree with the configuration beside the script, copied there as `.config`.

[`wpa_supplicant.config`](wpa_supplicant.config) has the nl80211 driver over libnl 3; OpenSSL for TLS and every primitive; SAE for WPA3-Personal; 802.1X with EAP-PEAP, EAP-TTLS and EAP-TLS, and MSCHAPv2, GTC and MD5 inside them; the control interface and `wpa_cli`'s line editor; and `bgscan` `simple`, which roams among the access points of one network. Management-frame protection (802.11w) is always built in 2.12 and has no option, and TKIP is built unless `CONFIG_NO_TKIP` is set, so WPA (TKIP), WPA2 (CCMP) and WPA3 (SAE with 802.11w) all work. With OpenSSL, `ca_cert="probe://"` makes wpa_supplicant report the server's certificate in a `CTRL-EVENT-EAP-PEER-CERT` event, with its subject and SHA-256 hash, and fail the authentication before any credential is sent, and `ca_cert="hash://server/sha256/HASH"` accepts the server whose certificate has that hash. WPS, P2P, the access-point mode, mesh, DPP, OWE, Hotspot 2.0, D-Bus, MACsec and the wired driver are left out.

[`hostapd.config`](hostapd.config) has the nl80211 driver, OpenSSL, SAE, 802.11n, the integrated EAP server with PEAP, TTLS, TLS, MSCHAPv2, GTC and MD5, and the control interface for `hostapd_cli`. The suite's networks need nothing more: open, WPA, WPA2, WPA3 and its transition mode, and WPA2-Enterprise.

`i686-elf-gcc` and `x86_64-elf-gcc` define no `__linux__`. hostap chooses Linux's byte-order headers and its Linux code by that macro, and Rach2 offers Linux's interface, so the script defines it on the command line. The Linux headers the programs include come from libnl's stage, never from the build host's.

`wpa_passphrase` computes only PBKDF2-SHA1. Linked with OpenSSL it is 2 MB; the script builds it first with the project's internal SHA-1 and no driver, which makes it 34 KB, and then cleans the tree for the build with OpenSSL.

## Building

```
make hostap
make hostap ARCH=x86_64
```

The build makes OpenSSL and libnl first. Each architecture extracts under `build/vendor-work/hostap/` or `build/x86_64/vendor-work/hostap/`, where each make's log is kept, and stages the stripped `wpa_supplicant`, `wpa_cli`, `wpa_passphrase`, `hostapd` and `hostapd_cli` and `COPYING` under `build/vendor/hostap/` or `build/x86_64/vendor/hostap/`. Stripped, wpa_supplicant is 2.9 MB on i686 and 4.1 MB on x86-64, of which OpenSSL is about two thirds. The stamp's inputs are the script, both tarballs, both configurations, the musl wrapper, the architecture table and the stamps of musl, OpenSSL and libnl.

## Updating

Put the new release's two tarballs here after checking their signatures, change `VERSION` and both digests in the script and `HOSTAP_TARBALLS` in the root Makefile, read the new `defconfig`s for options that moved, and build both architectures. Check in QEMU that each program prints the new version.

## Limits

wpa_supplicant can do on Rach2 only what the kernel's nl80211 offers; the [wireless Readme](../../rach2/sys/network/wireless/Readme.md#limits) lists what it refuses, such as WEP, FT and the connection-quality monitor. hostapd does not run on Rach2, whose nl80211 has no access-point mode.

## Tests

The port is checked by booting `os-real` in QEMU on each architecture and running `wpa_supplicant -v`, `wpa_cli -v` and `wpa_passphrase` on a known network, whose PSK must be the one IEEE 802.11 gives for it. The Wi-Fi suite joins the Linux guest's hostapd with this wpa_supplicant under each protocol.
