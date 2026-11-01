# What the reviews of the Wi-Fi plans raised, and what was done

Codex (gpt-6.1-sol at high effort, since gpt-6-astra was reviewing 67): `codex.md`. Fable: below, from its answer of 2026-11-02. Both found nothing that stops either milestone.

## Taken

* Mac is an Apple M4: x86 guests run under TCG only. Measured line fixed; 69-14a measures the AP guest's boot under TCG (Fable 1, Codex 1).
* The relay's UDP rides on the cable the suite pulls: the relay uses a second card (Fable 2, Codex 1).
* ifplugd's `ifdown` would take down the card wpa_supplicant owns: `wpa_cli -a` drives DHCP on wlan0; ifplugd stays on the cable (Fable 3, Codex 3).
* Done lines needed later tasks: every part closes on a host test; the QEMU proofs are in the suite; 10 (nl80211) blocked by 2; the suite waits on 2 to 15, rfkill and the tools among them (Fable 4, Codex 2).
* Roaming inside one SSID: `CONFIG_BGSCAN_SIMPLE`, `bgscan` in `wifi`'s networks, `GET_STATION` signal, a two-BSSID roam row (Fable 5, Codex 3).
* The certificate was seen only after EAP had succeeded: `ca_cert="probe://"` first, no password before the yes; rows for refusal, no, reboot persistence, changed certificate (Fable 6, Codex 3).
* OpenSSL 4.0.2 untried with hostap 2.12: first subtask of 4, OpenSSL 3.5 LTS as the musl fallback (Fable 7).
* WEP and SAE-PK cut; TKIP kept (Fable 8, Codex 4).
* The MLME split in two (scan and BSS table; authentication, association and the rest); the element parser moved to task 1 so both run at once (Fable 9).
* The relay's frame format fixed in task 1's Readme (Fable 10, Codex 4); the AP guest and its relay split (Codex 4).
* `wireless.h` holds only what crosses between parts; each part owns a private header (Fable 11, in part).
* 70's trials split in two, and the Lenovo's proofs moved out of the parts' Done (Fable 12, Codex 2).
* Default routes: each lease's script touches only its own interface's default; Wi-Fi metric 600, cable 100; resolv.conf keeps every live interface's servers (Fable 14, Codex 3). Task 13 owns the DHCP and resolv scripts.
* Stale facts fixed: 1 ms tick, netlink datagrams of 32448 bytes, the packet-socket checks at creation, bind, send and receive (Fable 15, Codex 1).
* Task 1's Done without TinyCC (Fable 16).
* A note that demonstrations bring their own 2.4 GHz AP (Fable 17).
* 0x888E in `syscall_net.c` belongs to task 6; carrier at association, the control port gating everything but EAPOL until authorized (Codex 1).
* The AP guest also serves DHCP, DNS and router advertisements and forwards to a traffic endpoint (Codex 1).
* 70's branch built on 69's: 70-1 merges 69 first, and the manager merges 69 again before the trials (Codex 2).
* 70's host tests split per part, the chip model its own file (Codex 2).
* The sitting is the last task of 70; without the home network's passphrase, the association rows move to it and task 10 closes (Codex 2).
* Group rekey, protected-frame rejection, reset recovery with keys kept, recovery judged by traffic (Codex 3).
* Unmodified tools as a condition: build patches only, no logic patches (Codex 3).
* Transmit aggregation deferred to its own task after the first scan (Codex 4).
* The ideapad driver moved to 70 with an `/etc/acpi.map` action for `KEY_WLAN` (Codex 4).
* 70-8 adds a read-only PCI `config` in sysfs, since capabilities are not readable from the shell today (Codex 4).

## Set aside

* Giving both `devfs.c` rows and both sysfs trees to one task (Fable 13): the rows are appends that a rebase keeps; splitting by function is enough.
* An ARM64 AP guest for HVF on the Mac (Codex 1): one x86-64 guest for all three machines is simpler; its time under TCG is measured first (69-14a), and the choice is revisited only if it is too slow.
* Keeping a TCP connection across a change of address (Codex 3): no Linux does; the suite proves new connections after a move and an SSH session across a roam that keeps the address.

## Added by the manager

* 70-12: 67's and 68's silent Lenovo steps after the release installs them (sandboxes as root, logins kept, OpenCode's acceptance, Doom's counters), in Backlog until the release.
* 69 owner decision restated: no program logic patched; kernel fixes instead.
