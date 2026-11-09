# Upstream faults of milestone 69

Faults this milestone found in another project's code, one section each, in the format of [`dev-log/upstream.md`](../upstream.md), which indexes them.

## iw dev link reads the DTIM period and beacon interval unchecked

* Project: iw 6.17, `vendor/iw/iw-6.17.tar.xz`.
* Fails: `iw dev link` crashes when the station's BSS parameters lack `NL80211_STA_BSS_PARAM_DTIM_PERIOD`: it passes the attribute to `nla_get_u8` without checking that it is present, and `NL80211_STA_BSS_PARAM_BEACON_INTERVAL` to `nla_get_u16` the same way, though it checks the other BSS parameters (`link.c:238-241`).
* Found: 2026-11-02, in milestone 69, against Rach2's nl80211 in QEMU, before it reported the DTIM period. Linux's nl80211 sends both attributes, so iw on Linux does not meet it.
* Reproduction: answer `NL80211_CMD_GET_STATION` with `NL80211_STA_INFO_BSS_PARAM` holding neither attribute, as a test kernel or a netlink stub can, and run `iw dev wlan0 link`.
* Evidence: commit `44be2ed1a`; `link.c:238-241` in the tarball.
* Rach2: the kernel reports the DTIM period in the station's BSS parameters, as Linux does. iw is not patched.
* Status: unreported.
* Next: check iw's current `link.c`; the fix is a presence test for each of the two attributes.
