# Wi-Fi above the chip

Milestone 69 gives Rach2 everything a Wi-Fi station needs above the radio, proven in QEMU on a software radio: generic netlink and nl80211, an 802.11 station layer, the encryption, rfkill, and Linux's own tools (wpa_supplicant 2.12, hostapd 2.12, iw 6.17) running unmodified. A user types `wifi connect NAME` and joins a WPA, WPA2 or WPA3 personal network or a WPA2-Enterprise one such as eduroam; on an enterprise network's first connection the command shows the server certificate's subject and SHA-256 fingerprint before it sends any password and, on the user's yes, pins it for that network. The machine moves between the access points of one network, between known networks, and between the cable and Wi-Fi by itself. Milestone 70 writes the QCA9565 driver under the same interface and proves it on the Lenovo; it ends with the owner's one sitting.

## The owner's words

The four messages this plan comes from, whole:

* 2026-11-01T13:19:51Z: "No, make sane defaults about bell and change the settings on the machine yourself, and flashing is not from vim, it is from the terminal itself!!! In addition, do a rough check, how hard would it be to support our WIFI on the notebook. We want the driver, and some interface similar to Linux, and some userspace, preferably from Linux to configure, maintain all that. Probably we will not include that in this run, so do a rough check, research... I need a great networking stack to show the notebook to colleges, investors, conferences, WIFI is a must in such a case, a stable one. It would be nice to piggy back on knowledge accumulated from Linux or BSD fro this specific model, wifi chip. By the way, I am leaning toward GPL license for the kernel, it does not mean we can copy code as is, but looking on it and using that experience should be less of a gray area."
* 2026-11-01T18:44:52Z: "`Linux allows about 64 KiB`, I told you, robust Networking, so I can actually use the notebook on the go, in different wifi, lan networks!"
* 2026-11-01T19:15:25Z: "Hotel Login pages are probably useless to support without GUI and Chrome that is not headless. So no, no need. No USB network... Focus on what we have in the notebook."
* 2026-11-01T19:22:49Z: "By the way, my university network, the first time I connect, asks to accept a self-signed certificate. Do we plan to handle certificates, all the different WPA/2... whatever protocols?"

The study is `build/flow/coordinator/wifi/research.md` (2026-11-01), and the owner's notes `build/flow/coordinator/wifi/owner-notes.md`. The reviews are in `build/flow/coordinator/wifi/review/`.

## The owner's decisions

* Wi-Fi now, in two milestones as the study split them: this one above the chip, tested in QEMU with a software radio; then the QCA9565 driver (milestone 70). The owner's order of 2026-11-02 is the go; the owner may change the plan in the morning.
* One release for 67, 68, 69 and 70, at the end: "no release merged with that to save more time" and "agree it to be another milestone, but merged" (2026-11-02 02:49 and 02:52 UTC+6).
* The owner's university network: WPA2-Enterprise or eduroam, EAP with PEAP or TTLS, whose server certificate is self-signed. On the first connection its subject and SHA-256 fingerprint are shown (wpa_supplicant's `CTRL-EVENT-EAP-PEER-CERT`) and pinned on the user's yes as `ca_cert="hash://server/sha256/..."`, as phones and NetworkManager do. Proof: the owner joins it from the Lenovo, in the sitting that ends milestone 70.
* Protocols: "all the different WPA/2... whatever protocols". WPA (TKIP), WPA2 (CCMP), WPA3 (SAE with management-frame protection) and WPA2/WPA3 transition, personal and enterprise (PEAP, TTLS, TLS).
* No captive portals and no USB networking; the Lenovo's own QCA9565 is the radio.
* Robust networking on the go: the machine joins whichever known network is in range, roams between the access points of one network (eduroam and a university are one name with many), moves between networks and between the cable and Wi-Fi without the user's help, and keeps its addresses, routes and DNS right through each move. This builds on 67's link watcher (`ifplugd` with `krffs/ifplugd.action`, `ndwatch`) and DHCP scripts.
* Stable enough to show at conferences, to investors and to colleagues: recovery from a lost AP, a restarted AP, a deauthentication and a group rekey is in the suite, and milestone 70 soaks the chip for hours.
* Unmodified tools: wpa_supplicant, hostapd and iw are built from their release tarballs with a configuration and, where the build needs it, a build-system patch; a missing kernel behaviour is fixed in the kernel, never by patching the program's logic.
* License: GPLv2 for Rach2's own code, GPL-2.0-only (the owner's decision of 2026-11-01, `dev-log/67-agents/plan.md`). So Linux's mac80211, cfg80211 and nl80211 may be read and their design followed, and ath9k (ISC), FreeBSD's net80211 and ath (BSD) and OpenBSD's net80211 (ISC) may be taken; a file that follows a source names it and its function, as `rtl8168.c` does, and keeps a copied permission text.
* When a capability is missing, look in BusyBox first: its `rfkill` applet is used, not a new one.

## What the plan takes from the study and the reviews, and what it sets aside

* Follows the study: Linux's split, an nl80211 server over a softMAC station layer, so that unmodified tools run; wpa_supplicant 2.12 with the SME in user space (`NL80211_CMD_AUTHENTICATE` and `ASSOCIATE`, SAE frames carried by the kernel); the smallest nl80211 set of its Section 3.2, confirmed against a captured trace; EAPOL through a packet socket of 0x888E; a software radio, `wsim`, with the chip's lower operations, and hostapd on `mac80211_hwsim` in a Linux guest as the real AP; the world regulatory domain; power save, PAPRD and coexistence off.
* Takes from the reviews: the certificate is probed before any credential is sent (`ca_cert="probe://"`); roaming within one network by wpa_supplicant's `bgscan`; the relay's frames on a second card, so that pulling the cable does not cut the software medium; ifplugd stays on the cable and wpa_supplicant's own events drive DHCP on `wlan0`; WEP and SAE-PK cut; the station's MLME split in two for the critical path; the relay's frame format fixed in task 1; Done lines provable without the later tasks, the QEMU proofs gathered in the suite; the ideapad driver moved to milestone 70; milestone 70's branch built on this one's.
* Sets aside, with the reason: WEP (broken, off by default in wpa_supplicant 2.12 and hostapd, not asked for); wireless extensions (legacy); iwd and NetworkManager (they need D-Bus, `AF_ALG` and keyrings); FreeBSD's net80211 as the architecture (its ABI is not Linux's); WPA in the kernel (would not run Linux's tools); the virtio-hwsim and vhost-user path (nobody has published it); AP mode on Rach2 (not asked for); HT40, power save, S3 and Bluetooth coexistence (later, as the study says); a TCP connection kept across a change of address (no Linux keeps one; the suite proves new connections after a move, and an SSH session across a roam that keeps the address); captive portals and USB networking (the owner's no).
* A note for demonstrations: the chip is 2.4 GHz only, and many conference networks are 5 GHz or crowded, so a demonstration brings its own 2.4 GHz access point or a phone's hotspot.

## Measured

* The testing host: QEMU 10.2.1 with `/dev/kvm`; its WSL kernel 6.18.54.1 has cfg80211 and mac80211 as modules but not `mac80211_hwsim` (`# CONFIG_MAC80211_HWSIM is not set`), and Ubuntu offers hostapd 2.11. So the Linux AP runs as a QEMU guest of its own, not on the host. The Mac is an Apple M4 with QEMU 11.1.3, which runs x86 guests under TCG only (`tests/Readme.md:35`); Windows runs QEMU with its own accelerator.
* Rach2 at 67's head 9daa19e51: netlink answers `NETLINK_ROUTE`, `NETLINK_SOCK_DIAG` and `NETLINK_KOBJECT_UEVENT` and refuses `NETLINK_GENERIC` (`socket/netlink.c:300`); 67 added `NETLINK_ROUTE` multicast groups and notifications (`socket/netlink.h:35`); a netlink datagram holds `32768 - 320` bytes (`netlink.c:196`); packet sockets refuse 0x888E at creation and bind (`syscall_net.c:599`, `:863`), refuse a non-IP send (`packet.c:340`) and match one protocol on receive (`packet.c:238`); `ethernet_tx` refuses a frame without carrier (`ethernet.c:97`); a driver is a `struct netdev_ops` of Ethernet frames (`driver/netdev.h:125`); the tick is 1 ms (`timer.h:11`), and kernel threads and deadline waits exist (`acpi/osl.c:711`, `proc/wait_queue.h:69`); the DHCP script installs its default route without a metric and removes every other default (`krffs/udhcpc-default.script:75-90`), and `resolv.script` merges servers by file name (`krffs/resolv.script:58`); BusyBox's `CONFIG_RFKILL` is off (`vendor/busybox/rach2.config:834`); the GNU root pins OpenSSL 4.0.2 (`vendor/gnu/sources.txt:133`).
* In milestones 65 to 68 a task of 3 to 7 subtasks took a loop 10 to 60 minutes; builds, boots and the GNU root's ports took the most.

## The split

Sixteen tasks here and thirteen in milestone 70, for nine loops, from one queue. This milestone's branch is cut from 67's joined head 9daa19e51, not master, since it builds on 67's netlink notifications, datagrams and link watcher; milestone 70's branch is cut from this one's and takes it again by merge when 70's trials need it. Milestones 67, 68, 69 and 70 are released together, once, at the end: each later branch is rebased onto the one before it, and one `release.sh -c` releases all four once every task but the sitting is done. The release does not wait for the sitting, which comes last. 67's and 68's end checks and Codex's fixes go on now, so that the final end checks repeat only what changed.

Task 1 writes what every other kernel task codes against: the radio's lower operations, the 802.11 definitions with the element parser, the calls between the parts, the encryption calls, the generic-netlink registration and the relay's frame format. It is small and heads the queue. Tasks 4, 5, 13 and 14 here, and 70's task 8, start at once beside it; tasks 2, 3, 6 to 12 and 15, and 70's 1, start as soon as it merges. Task 16, the suite, waits for the rest. The longest chain is 1, then 8 (the MLME), then 16.

Each part's private declarations go in a header of its own that its task owns; `wireless.h` holds only what crosses between parts.

Shared files are split by function, each function with one owner:

* `socket/netlink.c` and `netlink.h`: 2, generic netlink and `RTM_SETLINK`'s operstate.
* `sys/syscall_net.c`: 2 the `SOL_NETLINK` options; 6 the 0x888E checks at creation and bind and the `SIOCGIWNAME` refusal.
* `ethernet/ethernet.c`, `socket/packet.c`: 6.
* `driver/netdev.c` and `netdev.h`: 6, the interface of a radio.
* `procfs.c`: 6 `/proc/net/wireless`. `synthfs.c`: 6 `/sys/class/ieee80211` and `phy80211`; 12 `/sys/class/rfkill`.
* `devfs.c`: 11 `/dev/wsim`; 12 `/dev/rfkill`.
* `krffs/udhcpc-default.script`, `udhcpc6-default.script`, `resolv.script`, `ifplugd.action`, `utils/init.c`'s network start and the images' `/etc/network/interfaces`: 13 alone.
* Appends to the same list (`rach2/sources.mk`, `utils/programs.mk`, the `krffs/manifests/*.manifest`, `vendor/gnu/sources.txt`, `man/programs.md`, `devfs.c`'s `dev_table`) conflict on a rebase when two tasks append at the same place; the worker keeps both, which is expected.

A task labelled `linux-host` builds the GNU layer and goes only to a loop that can (the team's five, the Codex loop and m1). Every Lenovo step is in milestone 70: what a loop can do over SSH on a trial boot, silently, runs as soon as its code is merged; what needs the owner at the machine waits for the one sitting at the very end of all the open work.

## Tasks

### 1. The Wi-Fi interfaces

Owns `rach2/sys/network/wireless/radio.h`, `ieee80211.h`, `wireless.h`, `wcrypto.h`, `elems.c`, `wireless/Readme.md` and `rach2/sys/network/socket/genetlink.h`. Waits on nothing.

* 1a. `radio.h`: the lower operations a radio gives the station layer, shaped like mac80211's `ieee80211_ops` (start, stop, config of channel and filters, tx of an 802.11 frame with its rate series and a status callback, set_key into a hardware key slot or a refusal that means software, sta_state, ampdu_action, scan start and complete, get_tsf), with each one's context and buffer ownership, and the record a radio registers: its band of 2.4 GHz channels, legacy and HT rates, HT capabilities, ciphers in hardware, the MAC address, and the rate-control interface (a call per transmit status and a call that picks a frame's rate series).
* 1b. `ieee80211.h`: frame control, the management, control and data headers, the QoS and HT control fields, the element identifiers (SSID, rates, DS, TIM, country, RSN, RSNX, HT capabilities and operation, extended capabilities, WMM, MMIE), reason and status codes, the AKM and cipher suite selectors, from IEEE 802.11-2020 and checked against Linux's `ieee80211.h`.
* 1c. `elems.c`: element parsing into a table of the elements found, refusing a truncated or repeated one as Linux does, and the builders of the elements a station sends; a host test of both.
* 1d. `wireless.h`: what crosses between the parts: the BSS record, the station's states (none, authenticating, authenticated, associating, associated, authorized), the work thread's queue, key installation, the events nl80211 sends (scan done, authenticate, associate, deauthenticate, disassociate, unprotected frames, Michael MIC failure) and the counters `/proc/net/wireless` reports.
* 1e. `wcrypto.h`: AES-128 with its key schedule, CCMP encrypt and decrypt of an MPDU with its PN, BIP-CMAC-128 sign and check, TKIP's Michael and key mixing with RC4, each with its buffer ownership and errors.
* 1f. `genetlink.h`: register a family with its name, version, header size, `maxattr`, operations (do and dump) and multicast groups; send to a group; reply; the attribute helpers (`nla_put_*`, nests, parsing against a policy).
* 1g. `wireless/Readme.md`: the summary, the Interfaces (the relay's UDP frame format among them: a version, the 802.11 frame, its frequency, rate, signal and flags, and the transmit status sent back with the frame's cookie, which tasks 11 and 15 both build to) and Design with a heading for each part, so that later edits fall in different places.
* Done: the headers and `elems.c` compile under `-ansi` with both cross compilers; `elems.c`'s host test passes.

### 2. Generic netlink

Owns `socket/genetlink.c`, the generic-netlink half of `socket/netlink.c` and `netlink.h`, the `SOL_NETLINK` options in `syscall_net.c`, `tests/host/genetlink_hosttest.c`, `man/man7/netlink.7` and a new `man/man7/genetlink.7`. Waits on 1.

* 2a. `NETLINK_GENERIC` sockets and the family registry behind `genetlink.h`.
* 2b. `nlctrl`: `CTRL_CMD_GETFAMILY` by name and as a dump, with the family's id, version, header size, `maxattr`, operations and multicast groups, as libnl's `genl_ctrl_resolve` and `genl_ctrl_alloc_cache` read them.
* 2c. Multicast groups by `bind` and `NETLINK_ADD_MEMBERSHIP` and `NETLINK_DROP_MEMBERSHIP`, delivery to every member, an overrun reported as `ENOBUFS`; group ids past 32 through the options.
* 2d. Dumps that span several reads (`NLM_F_MULTI`, `NLMSG_DONE`), acknowledgements and extended `NLMSG_ERROR`; `NETLINK_EXT_ACK` and `NETLINK_CAP_ACK` taken.
* 2e. `RTM_SETLINK` with `IFLA_LINKMODE` and `IFLA_OPERSTATE`, and the operstates dormant and up in `RTM_NEWLINK`, which wpa_supplicant sets around the handshake.
* 2f. The host test: a family registered, resolved by name, a dump of three reads, a group joined and left, an overrun. The manual pages.
* Done: the host test passes.

### 3. Encryption

Owns `rach2/sys/network/wireless/crypto/` and `tests/host/wcrypto_hosttest.c`. Waits on 1.

* 3a. AES-128 in constant time (no table indexed by secret bytes), from a permissive source named in the file, such as BearSSL's `aes_ct` (MIT) or wpa_supplicant's.
* 3b. CCMP: the CCM mode, the AAD from the MAC header, the nonce from the PN.
* 3c. BIP-CMAC-128: AES-CMAC and the MMIE.
* 3d. TKIP: Michael, RC4, the phase 1 and 2 key mixing.
* 3e. The host test on the vectors of IEEE 802.11-2020 Annex J (CCMP, BIP, TKIP) and NIST's AES and CMAC vectors.
* Done: the host test passes with both architectures' compilers.

### 4. Linux's tools in the musl userland

Owns `vendor/openssl/`, `vendor/libnl/`, `vendor/hostap/` (wpa_supplicant and hostapd, one tarball), `vendor/iw/`, `CONFIG_RFKILL` in `vendor/busybox/rach2.config`, their lines in the musl manifests and their pages in `man/`. Waits on nothing.

* 4a. First, since it is the untried step: wpa_supplicant 2.12 and hostapd 2.12 against OpenSSL 4.0.2 for musl, static, from the tarball the GNU root pins; if 4.0 does not build or link with them, OpenSSL 3.5 LTS, pinned, for the musl userland, and the finding in the Readme for task 5.
* 4b. libnl 3.12.0 (`libnl-3`, `libnl-genl-3`, `libnl-route-3`), static for musl.
* 4c. wpa_supplicant, `wpa_cli` and `wpa_passphrase` with `CONFIG_DRIVER_NL80211`, `CONFIG_LIBNL32`, OpenSSL, SAE, `CONFIG_IEEE80211W`, TKIP, PEAP, TTLS, TLS, MSCHAPv2, GTC, `CONFIG_CTRL_IFACE`, `CONFIG_BGSCAN_SIMPLE`, and the `probe://` and `hash://server/sha256/` certificate forms.
* 4d. hostapd, static for musl, with nl80211, SAE, 802.11w, WPA and WPA2, and its integrated EAP server (PEAP, TTLS, TLS), for the test AP of task 14 if its guest is x86-64.
* 4e. iw 6.17 and BusyBox's `rfkill` applet.
* 4f. The programs staged in `os-real`, the metal disk and recovery as the manifests say, and a manual page or alias for each: `wpa_supplicant(8)`, `wpa_cli(8)`, `wpa_passphrase(8)`, `wpa_supplicant.conf(5)`, `hostapd(8)`, `iw(8)`, `rfkill(8)`.
* Done: `make` builds them for both architectures from nothing and again with nothing to do; each prints its version in a QEMU boot of os-real; no program logic is patched.

### 5. Linux's tools in the GNU root

Labels: `linux-host`. Owns the ports `vendor/gnu/ports/libnl`, `wpa_supplicant`, `iw` and their lines in `vendor/gnu/sources.txt` and the GNU root's manifest. Waits on nothing.

* 5a. libnl 3.12.0 against glibc.
* 5b. wpa_supplicant 2.12 against the GNU root's OpenSSL 4.0.2, with 4c's features, and in the GNU Readme what OpenSSL 4.0 needed.
* 5c. iw 6.17, and util-linux's `rfkill` if the GNU root's util-linux does not build it.
* 5d. Each port built alone with `build_port.sh NAME` in the loop's copy.
* Done: the three ports build; `wpa_supplicant -v` and `iw --version` run in a QEMU boot of the GNU root.

### 6. The wireless device and its interface

Owns `wireless/wiphy.c`, `wiphy.h`, `wlan.c`, `reg.c`, the radio's interface in `driver/netdev.c` and `netdev.h`, `ethernet/ethernet.c`'s and `socket/packet.c`'s EAPOL paths, the 0x888E checks and `SIOCGIWNAME` in `syscall_net.c`, `/proc/net/wireless`, the `ieee80211` and `phy80211` entries of sysfs, `tests/host/wlan_dev_hosttest.c` and `man/man4/wlan.4`. Waits on 1.

* 6a. The radio registry: a radio registers its record from `radio.h` and becomes `phyN` with an interface `wlanN` of Ethernet type, so that IP, ARP, DHCP and packet sockets work unchanged above it. Carrier is up while associated; the control port, below, keeps everything but EAPOL off the air until the station is authorized, so DHCP starts only after the handshake.
* 6b. 802.11 to Ethernet and back: LLC/SNAP and the 802.1H bridge tunnel, a station's three addresses.
* 6c. EAPOL (0x888E): packet sockets of that protocol made and bound (`syscall_net.c:599`, `:863`), sent (`packet.c:340`) and delivered (`packet.c:238`, `ethernet.c:51`); the control-port gate, which passes only EAPOL either way until `SET_STATION` authorizes the port.
* 6d. `/sys/class/ieee80211/phyN`, `/sys/class/net/wlanN/phy80211`, `device/driver`, and `/proc/net/wireless` with the link quality, signal and the discard counters.
* 6e. The world regulatory domain: channels 1 to 11 active, 12 and 13 passive, 14 off; a country code a radio reads from its EEPROM narrows it.
* 6f. `SIOCGIWNAME` refused with `EOPNOTSUPP`, as a cfg80211 device without wireless extensions answers.
* 6g. The host test: a stub radio of `radio.h` brought up, an IP datagram through `wlan0` read as an 802.11 frame, EAPOL passed while the port is closed and nothing else.
* Done: the host test passes.

### 7. Scan and the BSS table

Owns `wireless/work.c`, `work.h`, `scan.c`, `bss.c` and `tests/host/wlan_scan_hosttest.c`. Waits on 1.

* 7a. A kernel thread and timers for the 802.11 work, so that none of it runs in an interrupt handler.
* 7b. Scan, active and passive by the regulatory rules, with a dwell on each channel, and its abort.
* 7c. The BSS table: entries from beacons and probe responses, with signal, TSF, capability and the elements, aged out, and the current BSS held while in use.
* 7d. The host test with a scripted AP's beacons and probe responses.
* Done: the host test passes.

### 8. The station's MLME

Owns `wireless/mlme.c`, `mlme.h` and `tests/host/wlan_mlme_hosttest.c`. Waits on 1.

* 8a. Authentication: Open System built here; SAE commit and confirm frames carried from and to user space with every status code, anti-clogging (76) and hash-to-element (126) among them.
* 8b. Association: the request with the rates, HT capabilities, WMM and the RSN and RSNX elements the supplicant gives; the response parsed into the station's state; reassociation to another BSS of the same network.
* 8c. Connection monitoring: beacon loss, then a probe to the AP, then a disconnect reported to user space.
* 8d. Deauthentication and disassociation sent and received, protected under 802.11w; unprotected ones while protection is on dropped and reported as `UNPROT_DEAUTHENTICATE` and `UNPROT_DISASSOCIATE`; SA Query answered.
* 8e. The host test with a scripted AP: Open authentication, association, reassociation, an AP that vanishes, a protected and an unprotected deauthentication, the SAE frames carried.
* Done: the host test passes.

### 9. The data path

Owns `wireless/tx.c`, `rx.c`, `key.c`, `agg_rx.c`, `rate.c`, their header `data.h` and `tests/host/wlan_data_hosttest.c`. Waits on 1.

* 9a. Transmit: QoS data and the four access categories, sequence numbers per TID, the PN, encryption in hardware when the radio took the key and in software otherwise.
* 9b. Receive: duplicates, defragmentation, decryption and the replay check per TID, BIP on group-addressed management frames, the TKIP Michael check and its countermeasures report.
* 9c. The key table: pairwise, group and integrity keys with their indexes and sequence counters, a group rekey that keeps the pairwise key, and the radio's hardware slots behind `set_key`, put back after a radio's reset.
* 9d. BlockAck receive: ADDBA answered and the reordering buffer with its timeout.
* 9e. Rate control's interface from `radio.h` and its first algorithm, a fixed table of the basic rates; milestone 70's task 6 writes the real one behind the same interface.
* 9f. The host test: a frame encrypted by software and by a stub that takes the key, replayed, duplicated, fragmented, reordered under BlockAck, and a group rekey.
* Done: the host test passes.

### 10. nl80211

Owns `wireless/nl80211.c`, `tests/host/nl80211_hosttest.c` and `man/man7/nl80211.7`. Waits on 1 and 2.

* 10a. The family `nl80211` and its groups `config`, `scan`, `regulatory`, `mlme`, `vendor`; `GET_PROTOCOL_FEATURES`; `GET_WIPHY` split across dumps, with the interface types, the supported commands (`AUTHENTICATE` and `ASSOCIATE` among them), ciphers, the band, rates, HT, scan limits and `NL80211_FEATURE_SAE`.
* 10b. `GET_INTERFACE`, `SET_INTERFACE` (station), `NEW_INTERFACE` refused.
* 10c. `TRIGGER_SCAN`, `GET_SCAN` and the events `TRIGGER_SCAN`, `NEW_SCAN_RESULTS`, `SCAN_ABORTED`.
* 10d. `AUTHENTICATE` (Open, and SAE with `AUTH_DATA`), `ASSOCIATE` with the RSN element, `USE_MFP`, `CONTROL_PORT`, the ciphers and AKM and `PREV_BSSID`, `DEAUTHENTICATE`, `DISASSOCIATE`, `CONNECT` and `DISCONNECT`, and their events.
* 10e. `NEW_KEY`, `SET_KEY` (default, default-management), `DEL_KEY`, `SET_STATION` (authorized), `GET_STATION` with the signal, which `bgscan` reads; `GET_REG`, `REGISTER_FRAME` and `FRAME` for action frames; the rest answered `EOPNOTSUPP` as the study lists.
* 10f. The host test against stubs of the station layer, replaying the requests of task 14's trace and comparing the answers' attributes; until the trace exists, the study's list. The commands of the trace that are missing are added or named in the Readme's Limits.
* Done: the host test passes.
* 10h. One cfg ops struct in the shape of `cfg80211_ops` between nl80211 and the station layer (scan, authenticate, associate, connect, disconnect, the keys, the station and `get_station`), whose one implementation is the softMAC station layer over `wlan_radio_ops`, so that a fullMAC driver can later plug in at that level. The owner, 2026-11-02: "Ensure, the driver abstractions are in place to add drivers easily for more chipsets and to be able to reuse common structures."

### 11. The software radio

Owns `wireless/wsim.c`, `wsim.h`, `/dev/wsim`'s row in `devfs.c`, `utils/wsimrelay.c`, `man/man4/wsim.4` and `man/man8/wsimrelay.8`. Waits on 1.

* 11a. `wsim`: a radio of `radio.h` with encryption in software, as `mac80211_hwsim` is for mac80211, made by a boot option or by opening `/dev/wsim`, one or more per machine.
* 11b. `/dev/wsim`: each frame the radio sends is read from it with its channel and rate, each frame written to it is received with a signal level, and transmit status comes back by the frame's cookie.
* 11c. `wsimrelay`: carries `/dev/wsim`'s frames in task 1's UDP format to task 15's peer and back, over a second card that `/etc/network/interfaces` and ifplugd leave alone, so that pulling `eth0` does not cut the medium (two e1000s already work, `tests/cases/machine/dhcp_scenario.c:544`).
* 11d. Two radios in one machine joined by the relay on loopback, for rows that need no Linux guest.
* Done: a host test passes frames between two wsim radios through the relay's code.

### 12. rfkill

Owns `rach2/sys/rfkill.c` and `rfkill.h`, `/dev/rfkill`'s row in `devfs.c`, `/sys/class/rfkill` in `synthfs.c`, `tests/host/rfkill_hosttest.c` and `man/man4/rfkill.4`. Waits on 1.

* 12a. The rfkill core: `/dev/rfkill` (one `ADD` event per switch, then changes, `struct rfkill_event` and the extended form) and `/sys/class/rfkill/rfkillN/` with `name`, `type`, `soft`, `hard`, `state`.
* 12b. A switch per radio, `phyN`, registered through `radio.h`, whose soft block stops the radio and whose release starts it; a hard block a driver reports.
* 12c. The host test of the core; the manual page.
* Done: the host test passes.

### 13. Networks on the go

Owns `utils/wifi.c`, `tests/host/wifi_hosttest.c`, `krffs/wpa_action`, `krffs/udhcpc-default.script`, `udhcpc6-default.script`, `resolv.script`, `ifplugd.action`, the network start in `utils/init.c`, the images' `/etc/network/interfaces` and `/etc/wpa_supplicant/`, and `man/man1/wifi.1`. Waits on nothing.

* 13a. At boot, when a radio is present, wpa_supplicant runs on `wlan0` with `/etc/wpa_supplicant/wlan0.conf` and its control socket, and `wpa_cli -a` runs `krffs/wpa_action` on `CONNECTED` and `DISCONNECTED`, which start and stop `udhcpc` and `udhcpc6` on `wlan0` without taking the card down; ifplugd stays on the cable, since its `ifdown` would take down the card wpa_supplicant owns.
* 13b. Wired and Wi-Fi together: each lease's script adds and removes only its own interface's default route, IPv4 and IPv6, with Wi-Fi's at metric 600 and the cable's at 100 as NetworkManager sets them (`RTA_PRIORITY` is honoured, `netlink.c:1751`), so traffic moves to Wi-Fi when the cable is pulled and back when it returns; `resolv.conf` keeps the servers of every interface that is up, the cable's first, and drops an interface's when it goes.
* 13c. `wifi`, a C program on wpa_supplicant's control socket: `wifi scan`, `wifi list`, `wifi connect NAME` (asks the passphrase, or the identity and password for an enterprise network), `wifi forget NAME`, `wifi status`; it writes networks with `bgscan="simple:30:-65:300"`, so that wpa_supplicant roams between the access points of one network and moves among known networks by itself.
* 13d. The first connection to an enterprise network: `wifi connect` first adds it with `ca_cert="probe://"`, so that the TLS handshake stops after the server's certificate and no password is sent; it shows the subject and SHA-256 fingerprint from `CTRL-EVENT-EAP-PEER-CERT` at depth 0 and, on the user's yes, sets `ca_cert="hash://server/sha256/HASH"` and connects; on no, it removes the network. A later certificate that does not match is refused and `wifi status` says why; eduroam's anonymous identity and `domain_suffix_match` are asked for and kept.
* 13e. Suspend to idle: the station disconnects before the freeze and wpa_supplicant reconnects after it.
* 13f. The host test of `wifi` against a fake control socket: the probe, the yes and the no, a changed certificate; the manual page `wifi(1)` with the university and eduroam examples.
* Done: the host test passes; the scripts are read against 67's `ifplugd.action` and the suite's rows.

### 14. The Linux AP guest

Owns `tests/system/wifi/` (the guest's image and its build) and the trace under `tests/system/fixtures/wifi/`. Waits on nothing.

* 14a. Measure first: an x86-64 Linux kernel with `mac80211_hwsim`, from a pinned download checked by SHA-256 (such as Alpine's `linux-lts` netboot kernel and modules) or Linux 7.3's pinned tarball with a small configuration, that boots under KVM on the testing host, the Mac's TCG and Windows' QEMU; record which, how long a boot takes on each, and why.
* 14b. The guest's root: hostapd (task 4's static build, or a pinned distribution package until it merges) with its EAP server and a self-signed server certificate for PEAP, TTLS and TLS, and a second certificate for the changed-certificate row; configurations for open, WPA-TKIP, WPA2, WPA3-SAE with 802.11w and transition mode; one network on two radios (two BSSIDs, one SSID) and a second network; dnsmasq for DHCP, DNS and IPv6 router advertisements on the Wi-Fi side, and forwarding to a traffic endpoint the suite reaches.
* 14c. The reference trace: in the guest, Linux's own wpa_supplicant joins the guest's hostapd under WPA2, SAE and PEAP while `nlmon` captures nl80211, kept for task 10.
* 14d. A script that starts the guest beside a Rach2 machine, with the relay's card and ports, as the suites' peers start.
* Done: the guest boots in QEMU on the three machines, its hostapd beacons, and the trace is kept.

### 15. The guest's relay

Owns `tests/system/wifi/hwsimrelay.c`. Waits on 1.

* 15a. hwsim's netlink medium (`HWSIM_CMD_REGISTER`, `HWSIM_CMD_FRAME`, `HWSIM_CMD_TX_INFO_FRAME`) to task 1's UDP format and back, with transmit status.
* 15b. Built for the guest and staged in its root by task 14's build.
* Done: two Linux guests' hwsim radios join through two relays, a Linux station to the guest's hostapd.

### 16. The Wi-Fi suite

Owns `tests/cases/machine.wifi.json` and its scenarios, the suite's line in `tests/registry` and `suites.mk`, and its rows in the Readmes. Waits on 2 to 15.

* 16a. WPA2-Personal: `wifi connect`, DHCP, DNS, ping and a TCP transfer through the AP to the endpoint; `iw dev`, `iw phy`, `iw dev wlan0 scan`, `iw dev wlan0 link` and `/proc/net/wireless` read.
* 16b. WPA3-SAE with management-frame protection, an unprotected deauthentication ignored; transition mode; WPA (TKIP); an open network.
* 16c. WPA2-Enterprise: PEAP-MSCHAPv2 and TTLS-PAP through the probe, no password sent before the yes, the no, the pin kept across a reboot without asking again, the changed certificate refused; eduroam's anonymous identity.
* 16d. Robustness: a group rekey, the AP restarted, a deauthentication, the AP gone and back, the roam between two BSSIDs of one network with an SSH session kept, the move to the second network, the cable pulled and back with the default route and DNS following and new connections working; each judged by counters and traffic.
* 16e. rfkill: `rfkill block wifi` stops the radio and `rfkill unblock wifi` reconnects.
* 16f. The GNU root's wpa_supplicant joins under WPA2, SAE and PEAP as the musl one does.
* Done: the suite passes on both architectures in QEMU, with the Linux AP guest.

## Done

The suite passes on both architectures, with the host tests of tasks 1, 2, 3, 6 to 13; the end runs and the release run them on the three machines. What the chip and the Lenovo add is milestone 70's.
