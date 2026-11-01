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

## Design

### Registry and regulatory rules

`radio.h` is the driver boundary. The registry, Ethernet adaptation and world regulatory rules implement it above the radio. A driver advertises its capabilities once; regulatory rules narrow the channel set rather than expanding it.

### Worker, scan and BSS table

The worker owns queued management work. Scan records contain copied SSIDs and request elements. A current or referenced BSS remains in the table while used; other entries can age out.

### Management state machine

The state machine owns authentication, association and connection monitoring. It emits the shared events, carrying complete SAE exchanges and status values to user space. It keeps authorization separate from association.

### Data, keys, aggregation and rates

The data path owns sequence numbers, encryption selection, receive replay and duplicate checks, fragment assembly and BlockAck reordering. The rate selector returns the same series regardless of which radio implements transmission; completion feeds the actual attempts back to it.

### Cryptography

The primitives have no device, station or allocator state. The data and key layers own counters and replay policy; cryptographic success alone does not authorize delivery.

### nl80211

The server translates generic-netlink attributes into station operations and station events into multicast messages. It advertises only operations and capabilities implemented below it.

### Software radio

The software radio implements the same lower operations as hardware. `/dev/wsim` and the relay preserve complete MPDUs, channel and rate metadata and transmit cookies using the versioned format above.

### rfkill

A switch belongs to each radio. Soft and hardware block changes are queued to the worker; driver stop and start do not execute in the interrupt handler.

## Tests

`tests/host/wlan_elems_hosttest.c` checks fixed wire sizes, empty and maximum-length SSIDs, split rates, optional request elements, truncated headers and payloads, invalid known lengths, singleton and extension duplicates, independent WPA and WMM elements, and bounded builders whose errors preserve their outputs. It allocates no files or processes. Its `host-deps` line compiles the real parser; the host collection registers it as `wlan_elems`.
