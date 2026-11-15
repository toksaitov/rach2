# Lenovo native diagnostics

The 25 October diagnostics exercised all 250 eligible native cases. The initial corrected-session runs passed 247 cases and failed bindholdmusl, fdcallmusl and statmusl. Targeted repairs then passed those three. Four GNU cases whose earlier cleanup left detached processes passed again with the repaired responder: tmux, autotools, dbus and chromium. An independent process and birth-identity census found no survivors beyond the recorded system services and the checking SSH connection, no unknown identities, no private mounts and no loop devices.

[The exact case index](native-results.tsv) identifies each passing result by event and SHA-256 of its original result bytes. It contains 243 retained results and seven targeted replacements. It is an index of diagnostic events, not one whole run on a single candidate. The original failures remain in their archives. T93's final whole run and the phase's review and release are separate acceptance steps.

## Source and artifacts

The original kernel and workload production is `403f6d99b04615013f78ec031636ba25803989ae`. The actual original kernel is 1,767,696 bytes, SHA-256 `9090a7284ec97ba686356b84014c4a31a16df5d589ba270dfa63515d2fc5318a`. The controller's session repair is `1a23727bb16f987e03d8c93f45332a2959633505`: the first two-case run used its actual TinyCC build, SHA-256 `e3373cf2f24a278be05018ada0e17f4680cc935b7094bb5eb017c34cfe4f2218`; the remaining and targeted runs used the distinct actual native GCC 16.2.0 `-O2` build, SHA-256 `668f5fadae4a8a6df4a0a397844ef36a26d6eeca2853a4fa9512bcfb3be05cde`.

The targeted retries used kernel source `320daa6a59a732ed1db084a17e6a4b79996dc069`, 1,767,808 bytes, SHA-256 `5c922e7da6ba5849ae830f5e05b289d37405c71957baf79caae710d0bf149e80`. This fixes the ext2 check's fixed hard-link table capacity. Its source difference from the original kernel is confined to that checker. The combined responder/root repair payload is `41f0fcd66c8b3813b56abbe58392b4f90ccd62e8`, archive SHA-256 `fafa6143b011f112dc2cdd1ed5b87bb9818bba4c26ffb132917d4271fc451793`. Its responder is SHA-256 `c1e38d3739f28c9c5b9b747234a86b2b9922979314bd8f363ebdd71285b35fc9`.

The final bind-holder retry used source `35bd60fc5`, probe SHA-256 `643ce569962bf6d7ad4ea7bf674bb8c12546cae8496d71d1602190ac4a544ae9` and private root SHA-256 `896ab7179e116db0541db6109debb4c83fb146d5be2ed81964d0ea09fc7110b9`. All 20 original assertions passed through the existing public framed root fixture. Its record is a framed exit/stream/cleanup record, with independently checked workspace removal, rather than a fabricated collection result document.

## Events and immutable records

Each collection archive contains the raw per-case result under `results-RUN/CASE/result.json`, where CASE replaces `/` with `_`.

* `pair-1126`: run `t93-fix1126-pair-01`, auxvprobe and bigfileprobe.exfat. Archive SHA-256 `9308f9220e5873a5eb89973094157f5a1517a4a4e2af14ebff4d20e75de63c0b`, 218,601 bytes.
* `remaining-1126`: run `t93-fix1126-remaining-01`, the remaining 248 cases. Archive SHA-256 `d2ccd37b34573f701695a8bf307a60748776f019ccf69bd174a35aef5210bb65`, 1,003,150 bytes. Its three failures and four later-disproved cleanup claims remain unchanged.
* `krffs-retry`: run `t93-k1153-three-01`. Archive SHA-256 `23cc16a21f708d0ef23a981ad112f8ec0ca1a1e5cdf88354ff4e04a82400bbd9`, 722,063 bytes. Only the passing fdcallmusl and statmusl rows enter the index; the newly reached bindhold allocation failure remains a failure.
* `bindhold-layout`: run `t93-bindhold1130-layout-01`, attempt `43fd7cd9cf62f697`. Framed record SHA-256 `b8a3f7e9a5795f442fb4ca49fdb26a852a8d351a6049681bfae2c02fd26e1e81`; complete controller output SHA-256 `17ea3f35d802ece9cd6da059960988bfdfd788e352b1825c404b9b54c4ccfb5d`.
* `owner-retry`: run `t93-gnu-cleanup1131-01`, all four GNU cleanup cases. Archive SHA-256 `2326b55cefe4b7fe0ed79f6d0bd7bc6f81c87b5457c43868f89bdafb4613819a`, 723,904 bytes. All four results are complete, passing, cleaned and have their workspace removed.

The TSV is SHA-256 `fa9c553eda6e90cef329a84a0ad4609ce3db629126d1ea16cd6976a6b2e3c8d6`. The complete source/inventory/event mapping and raw paths are retained in `build/tests/t93-final-remaining/index-02.json`, SHA-256 `04c7a4f744d562a7f0c9aa3bc608c26d17865cde77771a32755a2bd9699d9159`. The raw archives remain under `build/tests/native-attempt-session`, `build/tests/kernel-1153` and `build/tests/t93-final-remaining` in the channel worker's copy.

The existing deployment command ran the four GNU rows with the current kernel, preserved payload, Mac peer address 192.168.0.100, echo port 55090, interface eth0 at 192.168.0.106/24, gateway 192.168.0.1, far address 1.1.1.1, empty address 192.168.0.250 and silent address 192.168.0.252. It performed no new deployment. Its exact vector and all child/peer cleanup receipts are retained with the event.

## Restored machine

The Lenovo is identified by station address `xx:xx:xx:xx:xx:xx` and Samsung SSD serial `XXXXXXXXXXXXXXX`. The repaired kernel booted normally, the root was writable, SSH and the expected address/default route worked, and the original init and DHCP configuration were restored. Temporary nobody/nogroup entries and the trial watchdog were absent. A fresh identity/kernel check preceded `bootctl commit`; saved default and current slot read back candidate, with an empty next entry. The fallback's full-slot SHA-256 remains `b6a68b9451d4c7d09e165e7ee041dbb0959067a5840b5be9ad0c5d52401e7a52`.

Issue 1148 retains the earlier pre-reboot network loss as unexplained. The later ext2 read-only-root failure has its independent diagnosis and repair; the record does not assert that it caused the preceding network outage. Issues 1130, 1131 and 1153 retain the complete targeted output and restoration evidence. These diagnostics do not supply a desktop comparison sample or a phase release.
