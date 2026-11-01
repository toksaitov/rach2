# The wireless station interfaces

The wireless interfaces divide a Wi-Fi station into a radio driver, a worker, scanning and the BSS table, the station's management state machine, the data path and keys, and an nl80211 server. The headers define the calls and ownership between these parts. `elems.c` parses management-frame information elements and builds the elements a station sends. The radio operations serve both a software medium and a physical radio; neither exposes its private records to the station layer.

## Interfaces

### Frames and elements

`ieee80211.h` defines the station's management, control and data formats, frame-control bits, elements, authentication status and disconnect reason codes, and cipher and AKM selectors. Wire structures use byte arrays for integers, with little-endian byte order unless the field says otherwise. A suite selector is a host integer whose four wire bytes are big endian. A frame begins at frame control and excludes the FCS. `IEEE80211_MAX_FRAME_LEN` bounds the buffer, including encryption headers and tags.

`ieee80211_parse_elems` returns pointers into its input, each at an element's id byte, followed by its length and payload. The caller keeps that buffer unchanged while using the table. A truncated header or payload, an invalid known length, or a repeated singleton returns `-EINVAL` and clears the whole table. An empty SSID is valid. Unknown elements are retained without interpreting their payload. Vendor elements may repeat: WPA, WMM information and WMM parameters have separate references and each may appear once. Extension elements may repeat with distinct extension ids; the ordinary table retains the first. The parser checks framing and known element lengths; RSN suites and counts, country triplets and HT capability contents are checked by their consumers.

`ieee80211_put_elem` appends one bounded element. `ieee80211_build_sta_elems` writes a station's SSID and rates, splitting rates after eight, and optionally RSN, HT capabilities, extended capabilities, WMM information and RSNX. The optional inputs are element payloads without their id and length. A zero SSID length emits a wildcard SSID; zero optional lengths omit those elements. The builder validates all inputs and the complete size before writing, so a failure changes neither the buffer nor its output length. Buffers must not overlap. A WPA vendor element can be appended with `ieee80211_put_elem`.

### Radio and station

`radio.h` follows the division of Linux mac80211's `ieee80211_ops`, with Rach2's own records and calls. A driver supplies immutable band capabilities, its address, hardware ciphers and operations in a driver-owned `wlan_radio`. The registry assigns its `phyN` number and the station record. Registration takes no ownership of the driver's storage; unregister stops the device and drains callbacks before returning it.

Start, stop, channel/filter configuration and transmit are required. Hardware scanning, key slots, station notifications, BlockAck and TSF are optional. An unsupported key operation selects software encryption; another error fails the operation. Channels are in MHz, legacy rates in 100 kbit/s, and signal in dBm at the driver boundary. The BSS table and counters use mBm for nl80211. HT MCS indexes are distinguished from legacy rate indexes by `WLAN_RATE_MCS`. The first unused rate-series slot has index -1 and count zero. Access categories use mac80211's voice, video, best-effort, background order.

A transmit operation copies its frame and metadata before returning success and later reports exactly one status with its cookie. A refused transmit produces no status. A status says which rates were tried and whether the frame was acknowledged, filtered or failed. Completion delivery must have room independent of receive traffic. Stop quiesces DMA and interrupt callbacks and completes accepted transmissions before returning. Receive copies a frame and its status to work-owned storage; the driver can reuse its buffer on return. A full receive queue returns `-ENOBUFS`. A decrypted receive with its IV stripped must supply the PN and key index, so that stripping cannot bypass replay checks.

`wireless.h` defines BSS snapshots, scan requests, keys, counters, work records and station events. BSS records own their element bytes. The station states are none, authenticating, authenticated, associating, associated and authorized. Association gives carrier; authorization opens the control port to ordinary data. EAPOL must pass before authorization. The management and data implementations have their private declarations in their own headers.

### Execution and ownership

One wireless worker serializes station transitions, scans, BSS changes, keys and events. Driver operations and nl80211 handlers run in process context under the kernel lock without `net_lock`; operations documented as sleeping may release the kernel lock through the scheduler. Code takes `net_lock` only around a network-stack operation and never waits for the worker while holding it. Interrupt entry points only copy and enqueue receive frames, completion records, scan completion or rfkill changes. They never run management or encryption code in the interrupt handler.

A work record remains live until its callback returns or cancellation finishes. Queuing an already pending or running record returns `-EBUSY`. A callback uses another work record for a later operation. Cancellation may sleep and cannot be called by the callback being cancelled. Event delivery borrows its frame only for the call; nl80211 must copy what it queues. Registry and key calls state their context and errors in the headers.

### Encryption and keys

`wcrypto.h` defines allocation-free AES-128, CMAC, CCMP-128, BIP-CMAC-128, Michael, TKIP key mixing and RC4. AES uses no secret-indexed lookup table. Keys and schedules belong to the caller, which erases them at removal. CCMP and BIP operate on a bounded complete MPDU in place. A failure leaves the frame, length and result counters unchanged. BIP signs a group-addressed management frame without setting the Protected bit; its body remains plaintext. The headers give exact expansion sizes and buffer-overlap rules.

PN and IPN are host-order 48-bit counters. The caller assigns a fresh transmit value before encryption and never wraps it or reuses it after a reset. Receive checks replay per TID and commits the counter only after successful authentication. Installing identical key bytes again preserves counters. TKIP key bytes are the temporal key followed by the local transmit and local receive Michael keys. Slots 0 to 3 hold group data keys, 4 and 5 integrity keys; pairwise keys also name their peer. A group rekey does not remove the pairwise key. The radio's hardware slot is separate from the over-the-air key index.

### Generic netlink

[`../socket/genetlink.h`](../socket/genetlink.h) defines the generic-netlink registry used by nl80211. A registered family supplies its name, version, private header size, largest attribute id, policy, commands and multicast groups. Family and group ids are assigned at registration. Command handlers have separate request and dump entry points. `GENL_ADMIN_PERM` asks the dispatcher to check `net_admin` before calling either entry point. Registration and unregister are process-context operations; unregister ends active dumps before returning the family storage.

The dispatcher validates message and private-header bounds before handing a request to a handler. `nla_parse` validates attributes against a policy and produces borrowed pointers; getters require that validation. Native-endian scalar values need no aligned access. Unknown ids above `maxattr` are skipped; duplicate known ids and malformed bounds fail. Strings include their final NUL, flags have no payload, and nested policies are checked with `nla_parse_nested`. Message append errors are sticky. A nest's offset remains valid if its backing buffer moves; nests finish or cancel in reverse order.

A request handler emits replies through `genl_reply`; dispatch owns ACK and error messages. A dump keeps a stable copy of its request and a six-word cursor, appends only complete messages, and advances its cursor only for a message that fits. Its return distinguishes more output, completion and error; dispatch emits `NLMSG_DONE`. The optional cleanup runs once, including when the socket closes. Multicast group ids are global and may exceed 32. A delivery overrun is recorded for each affected socket as `ENOBUFS`.

### Software-medium datagrams

The `/dev/wsim` endpoint and both relay programs use version 1 of this format. The constants and offsets are in `wireless.h`. A UDP datagram is exactly one 40-byte header followed by `frame_len` bytes. All multi-byte integers are big endian, including cookie and TSF. Signed signal is a 16-bit two's-complement dBm value. No C structure, pointer, padding or host-endian integer is sent directly.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Magic `0x5753494d`, ASCII `WSIM` |
| 4 | 1 | Version, 1 |
| 5 | 1 | Type: 1 frame, 2 transmit status |
| 6 | 2 | Header length, 40 |
| 8 | 2 | Endpoint-local radio number |
| 10 | 2 | Flags |
| 12 | 2 | Frequency, MHz |
| 14 | 2 | Rate, 100 kbit/s, or MCS index with the MCS flag |
| 16 | 2 | Signal, signed dBm; 0 if unavailable |
| 18 | 2 | Frame length, 0 to `IEEE80211_MAX_FRAME_LEN` |
| 20 | 8 | Transmit cookie; 0 for an unsolicited receive |
| 28 | 8 | Receive TSF in microseconds, 0 if unavailable |
| 36 | 1 | Actual transmit attempts, 0 if unavailable |
| 37 | 3 | Reserved, zero |

Flags are TX `0x0001`, ACKED `0x0002`, NO_ACK `0x0004`, FAILED `0x0008`, MCS `0x0010`, SHORT_GI `0x0020` and SHORT_PREAMBLE `0x0040`. A frame from a transmitting radio has TX set and a nonzero outstanding cookie. The peer medium routes its bytes to receiving radios tuned to the frequency, clearing TX, cookie and attempts in receive records and filling the destination endpoint's radio number and receive metadata. A relay maps its peer's radio numbers to local radios explicitly; the number is never a kernel pointer or interface index. Encrypted frames stay encrypted across the medium. Payload includes the MAC header, encryption header and tag, and excludes FCS and radiotap.

A transmit-status datagram has no payload, frame length zero, TX set, and the originating radio number and cookie. ACKED means an acknowledged transmission; NO_ACK means no acknowledgement was requested; FAILED means it failed. Exactly one of these three outcomes is set. An acknowledged group-addressed or NO_ACK frame is invalid. The medium reports a status once per accepted transmit, including failure when no suitable receiver exists. The sender correlates it with an outstanding radio/cookie pair, rejects unknown or repeated completions and completes that transmission once. Rate and attempts describe the last attempted series; the driver keeps the original series with the cookie. A disconnected relay fails its outstanding transmissions during stop.

A receiver refuses a wrong magic, version, type or header length, nonzero reserved bytes, unknown flags, an impossible length, a truncated or trailing payload, or a status with an invalid outcome. A frame has at least a ten-byte control header and no ACKED or FAILED flag. A receive record has no TX or NO_ACK flag, no cookie and no attempts. Frequency and rate must belong to the selected radio; MCS requires HT support. Status metadata cannot be used to select a different radio or transmission. The two relay directions use configured peer addresses and ports. The relay's transport card is separate from the cable interface whose link the roaming checks change.

## Sources and license

`crypto/aes.c` takes the S-box circuit, the bit transposition, the key schedule and the rounds of BearSSL's `aes_ct` at tag v0.6 (`src/symcipher/aes_ct.c` and `aes_ct_enc.c`), under the MIT license, whose text the file keeps. The rest of `crypto/` is Rach2's, written from IEEE 802.11-2020 12.5 and checked against Linux's `net/mac80211/wpa.c` and `tkip.c` and hostap's `src/common/ccmp.c`, as each file names.

`scan.c` follows the design of Linux's `net/mac80211/scan.c`, and `bss.c` that of `net/wireless/scan.c`; the code of both is Rach2's.

## Design

### Registry and regulatory rules

`radio.h` is the driver boundary. The registry, Ethernet adaptation and world regulatory rules implement it above the radio. A driver advertises its capabilities once; regulatory rules narrow the channel set rather than expanding it.

`wiphy.h` defines the registry-owned station record. `wlan_register_radio` validates a borrowed radio and attaches an initially down `wlanN` interface. Radio numbers increase through 999 and are not reused; four physical and radio interfaces together fit the network table. A registration failure leaves no interface or radio attachment. Registration starts the shared worker and attaches the embedded scan and BSS table; unregister detaches the scan and destroys the table after holders have drained. `wlan_radio_scan_done` routes a hardware completion to that embedded scan. `wlan_event` dispatches to the consumer installed by nl80211 with `wlan_set_event_handler`; until the server attaches, events have no consumer. `wlan_start` and `wlan_stop` run in process context without `net_lock`. `wlan_set_state` asks the optional driver state callback before committing the state and borrowed BSS; the management layer holds that BSS while using it. Associated and authorized states have carrier. Only the authorized state passes ordinary data.

The data worker installs `tx_ethernet`, which copies an Ethernet frame into its queue and returns without sleeping. The network adapter gates every frame, including a direct packet-socket send, before that callback. A missing callback refuses the frame. The data worker must check the port again before transmitting a queued frame, since association and authorization can change while it waits. On receive it calls `wlan_rx_ethernet` after decryption and replay checks; this applies the same gate before any packet socket or IP handler sees the frame. EAPOL, EtherType `0x888e`, passes while associated with a closed control port. Stopping or blocking the radio prevents EAPOL too. `quiesce` is installed by the worker and cancels and drains this station's work during unregister, after the driver stops and reports its pending completions. Unregister clears routes, addresses and permanent as well as learned neighbours before freeing the station.

`wlan_encap` converts Ethernet II into a station's ToDS frame with the AP, station and destination addresses and an LLC/SNAP header. IPX and AARP use the bridge-tunnel OUI `00:00:f8`; other protocols use RFC 1042's `00:00:00`. `wlan_decap` accepts FromDS three-address data, including QoS and its optional HT control, after decryption and reassembly. It refuses null frames, four addresses, A-MSDU, wrong BSSIDs and remaining protection or fragmentation. RFC 1042 IPX and AARP remain LLC payloads in an 802.3 Ethernet frame; bridge-tunnel versions become Ethernet II. An 802.3 frame retains its LLC payload in both directions; encapsulation checks its length field and omits any Ethernet padding. Both helpers preserve outputs on an error and borrow non-overlapping buffers. The data layer supplies sequence numbers, encryption, duplicate and replay checks, aggregation and rate selection.

`reg.c` intersects each advertised channel with the world domain: channels 1 to 11 permit active use, 12 and 13 are passive, and 14 is disabled. An EEPROM country of US or CA also disables 12 and 13. Unknown country codes keep the world rules; the radio's advertised flags and power can restrict them further. No rule clears a restriction or raises power. The power ceiling is 20 dBm. `wlan_channel` returns no disabled channel and excludes passive channels for active use.

The registry publishes `/sys/class/ieee80211/phyN`, its `index`, `name`, `macaddress` and `uevent` attributes, the interface's `phy80211` link, and `device/driver` with the registered driver name. `/proc/net/wireless` reports link quality, signed dBm signal and noise, decrypt, fragment, retry and discard counters and missed beacons. Wireless extensions are absent: `SIOCGIWNAME` returns `EOPNOTSUPP`.

### Worker, scan and BSS table

`work.h` declares the worker, the scan and the BSS table; the registry embeds a `struct wlan_scan` and a `struct wlan_bss_table` in its station record.

`work.c` is the worker: one kernel thread, `kwlan`, which `wlan_work_start` makes before the first radio starts. The queue is a list in order of due tick, with work of equal ticks in the order queued and work due at zero first. The thread sleeps in an idle wait whose deadline is the earliest due tick, and wakes when that tick comes or when new work changes the earliest one; it then runs every callback due, under the big lock, including work those callbacks queue for the same tick. `wlan_queue_work` takes a spinlock with interrupts off, so an interrupt handler can queue. The queue compares the running record by pointer only, so its owner may reuse it once the callback returns; `wlan_cancel_work` waits for a running callback outside the worker, and never on it.

`scan.c` scans. `wlan_scan_start` checks and copies the request and queues its start; the worker then tries the radio's `scan_start`, and on `-EOPNOTSUPP` scans in software. A software scan visits the requested channels, or every channel the band allows, skipping disabled ones and repeats. On each it tunes the radio with the home filters and `WLAN_FILTER_OTHER_BSS`. Where the channel is active and the request names SSIDs, it sends a probe request for each SSID and stays 30 ms; otherwise it listens for 110 ms, as Linux's mac80211 does. A probe request goes at the lowest rate with no acknowledgement asked for. It carries the SSID, the rates split after eight, the DS Parameter Set, HT Capabilities when the band has HT, and then the request's elements. Its cookie has `WLAN_SCAN_COOKIE` set, so that the transmit status can be told from the data path's and dropped. Each step queues the next on the other of two work records, since a callback cannot queue its own. The scan reports `SCAN_STARTED` when it begins, and `SCAN_DONE` or `SCAN_ABORTED` last, after the software scan has returned to the home channel the MLME set and stale entries have left the table. A radio that refuses its own scan with another error ends the scan as aborted. An abort cancels the radio's scan through `scan_cancel`, or cancels the pending dwell and finishes at once. The scan takes its event call at attach, `wlan_event` in the kernel, so that `scan.c` links before nl80211 does.

`bss.c` keeps up to `WLAN_MAX_BSS` entries, keyed by BSSID and channel, each allocated when first heard. A beacon or probe response gives an entry its SSID, signal in mBm, TSF, beacon interval, capability and elements. Its channel is the DS Parameter Set's when present, since a 2.4 GHz receiver hears neighbouring channels, and a frame from a channel the regulatory rules disable is dropped. A beacon with a hidden SSID keeps the SSID and elements a probe response gave the entry, as cfg80211 does for hidden networks. The MLME holds the current BSS, which then neither ages out nor is evicted. An entry nobody holds leaves the table 30 s after it was last heard, at the end of each scan and before each dump; when the table is full, the oldest such entry makes room. `generation` changes whenever an entry comes or goes, for nl80211's dumps.

### Management state machine

`mlme.c` and `mlme.h` hold the station's MLME, after the design of Linux's `net/mac80211/mlme.c`; no code is taken from it. The interface embeds a `struct wlan_mlme` and gives it three calls: `tx` sends a management frame through the data path, `state` learns each committed station state, so that carrier follows association and the control port follows authorization, and `event` reports to nl80211, as the scan's event call does. The radio's `sta_state` may refuse a step up before it is committed. From authentication until the station leaves, the MLME holds the BSS with `wlan_bss_hold` and makes its channel the scan's home. nl80211's AUTHENTICATE, ASSOCIATE, DEAUTHENTICATE, DISASSOCIATE and SET_STATION's authorized flag map to the `wlan_mlme_*` calls of the same names, all on the wireless worker.

Open System is built here and tried three times, 200 ms apart. SAE runs in user space: each AUTHENTICATE carries a commit or confirm with its transaction and status as AUTH_DATA, and each SAE frame of the AP's goes up with whatever status it has. An anti-clogging request (76), a hash-to-element commit (126) or a commit waits two seconds for user space's answer; the AP's confirm after ours authenticates; any other status ends the attempt. An AUTHENTICATE or ASSOCIATE event without a frame means the request timed out. An authentication to a new AP while associated ends the old association as mac80211 does: reported as a local deauthentication, its frame not sent.

The association request has the capabilities, the listen interval and, when nl80211 names a previous AP, its address, which makes it a reassociation. Its elements are the SSID, the rates both ends know, the supplicant's RSN, HT capabilities for a 20 MHz channel when the AP has HT and the cipher allows it, the supplicant's extended capabilities, WMM information when the AP has WMM, the supplicant's RSNX and then its other elements. The response's AID, capabilities, rates, HT and WMM go into the station's state. A temporary refusal (30) with a comeback time is asked again after it, up to ten seconds.

Associated, the station watches the AP's beacons. Seven beacon intervals without a beacon or probe response start unicast probe requests, 500 ms apart; after three unanswered the station leaves, reported as a local deauthentication of reason 4 with no frame sent. Under 802.11w, from association on, an unprotected robust frame is dropped, and the AP's unprotected deauthentication or disassociation is reported as `UNPROT_DEAUTHENTICATE` or `UNPROT_DISASSOCIATE` so that user space can ask the AP with an SA Query. The station answers the AP's protected SA Query request itself; other action frames, the answers to user space's SA Queries among them, go on to nl80211's registrations.

### Data, keys, aggregation and rates

The data path owns sequence numbers, encryption selection, receive replay and duplicate checks, fragment assembly and BlockAck reordering. The rate selector returns the same series regardless of which radio implements transmission; completion feeds the actual attempts back to it.

### Cryptography

The primitives have no device, station or allocator state. The data and key layers own counters and replay policy; cryptographic success alone does not authorize delivery.

`crypto/aes.c` holds the cipher and CMAC. Its S-box is a Boolean circuit over bitsliced words, so the time taken does not depend on the key or the data. `struct wlan_aes128` keeps the schedule compressed to 44 words, and each pass expands it to 88 on the stack and erases it after. A pass carries two blocks, so CCM pairs each CBC-MAC block with the next counter block, which halves the passes. `crypto/wcrypto_int.h` declares what the cipher files and the host test share: the paired pass, CMAC in pieces, Michael over a plain message, a comparison in constant time and an erase the compiler keeps.

`crypto/ccmp.c` masks the MAC header as 802.11-2020 12.5.3.3.3 says: Retry, Power Management and More Data cleared, the subtype bits 4 to 6 of a data frame cleared, Protected set, and in a QoS data frame Order cleared and only the TID kept of QoS Control. The HT Control field is outside the AAD. The nonce's Management bit marks a robust management frame. Decryption authenticates in a pass that writes nothing and then decrypts in place over the CCMP header, so a forged frame comes back unchanged.

`crypto/bip.c` computes the MIC over the masked frame control, the three addresses and every byte after the 24-byte header, the MMIE's MIC field taken as zero, as Linux and hostap do. Sign and check refuse a frame that is not a group-addressed management frame, or that has Protected set.

`crypto/tkip.c` gives Michael, both phases of key mixing and RC4. TKIP's S-box and RC4 index tables by key bytes by design, so their timing is not constant. The caller builds the TKIP header from the phase 2 key's first three bytes and the IV32, and computes the ICV, a CRC-32 of the MSDU and its MIC.

### nl80211

The server translates generic-netlink attributes into station operations and station events into multicast messages. It advertises only operations and capabilities implemented below it.

### Software radio

The software radio implements the same lower operations as hardware. `/dev/wsim` and the relay preserve complete MPDUs, channel and rate metadata and transmit cookies using the versioned format above.

### rfkill

A switch belongs to each radio. Soft and hardware block changes are queued to the worker; driver stop and start do not execute in the interrupt handler.

## Limits

* A software scan while associated leaves the operating channel without first sending the AP a null frame with power management set, as mac80211 does, so frames the AP sends meanwhile rely on its retries.
* A passive channel stays passive for the whole scan: a beacon heard there does not let the scan probe it, as Linux's beacon hints do.

## Tests

`tests/host/wlan_elems_hosttest.c` checks fixed wire sizes, empty and maximum-length SSIDs, split rates, optional request elements, truncated headers and payloads, invalid known lengths, singleton and extension duplicates, independent WPA and WMM elements, and bounded builders whose errors preserve their outputs. It allocates no files or processes. Its `host-deps` line compiles the real parser; the host collection registers it as `wlan_elems`.

`tests/host/wlan_mlme_hosttest.c` drives the MLME with a scripted AP, a stub radio, a clock and a model of the work queue. It checks Open System with its retries and refusals, the association request's fields and element order, the response in the station's state, a refusal, a comeback and a timeout, a reassociation to a second AP of the same network, an AP that vanishes and one that answers a probe, protected and unprotected deauthentication, disassociation and SA Query under 802.11w, and SAE frames carried both ways with statuses 76, 77 and 126, a confirm that arrives early and user space that does not answer. It makes no files or processes. The host collection registers it as `wlan_mlme`.

`tests/host/wcrypto_hosttest.c` checks AES on FIPS-197 and SP 800-38A, CMAC on SP 800-38B, whole and in pieces, and the ciphers on IEEE 802.11-2020 Annex J: CCMP's data vector, BIP's broadcast Deauthentication, TKIP's MPDU with its phase 1 and 2 keys, and the Michael vectors. Three CCMP MPDUs of P802.11i/D7.0 add key indexes 1 to 3, masked CF subtypes, Order in a non-QoS frame and a QoS TID; a management frame, whose Annex J output the test cannot quote, is checked against OpenSSL's AES-CCM. RC4 is checked on RFC 6229. Every byte of a CCMP and a BIP frame is flipped in turn: those outside the MIC are skipped and every other one is refused with the frame and outputs unchanged. Refusals for counters, key indexes, frame types, short buffers and a missing Ext IV keep their inputs as well. The host collection registers it as `wcrypto`.

`tests/host/wlan_scan_hosttest.c` runs the real queue, scan and table against a stub radio and four scripted access points, which beacon each 100 ms and answer probe requests 2 ms later while the radio is tuned to their channel: one open, one hidden that answers only a directed probe, one on passive channel 12 and one on disabled channel 14. It checks the queue's order, refusals and cancellation; a wildcard scan of every channel with its timing, probes and the table it leaves; a directed scan that reveals the hidden SSID, which later hidden beacons keep; a passive scan; an abort mid-scan and before the start; refused requests; and the radio's own scan completed, cancelled and refused. The table's checks cover the fields, the DS channel, malformed and oversized frames, allocation failure, aging with a held entry, eviction and a table whose entries are all held. The host collection registers it as `wlan_scan`.

`tests/host/wlan_dev_hosttest.c` compiles the real registry, Ethernet and packet-socket paths with a lower radio and a model data-worker queue. It checks stable interface identities, failed starts and state changes, carrier, both directions of the closed control port, authorized IP traffic, station addresses and LLC/SNAP conversion, malformed frames, radio removal and regulatory restrictions. It creates no files or processes.
