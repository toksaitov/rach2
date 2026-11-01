# The Lenovo's figures and proofs

Milestone 67's measurements and proofs on the Lenovo, one section per owner-only task of the plan: 23a the kernel's figures, 23b IPv6, 23c the agents and term.

## IPv6 (task 23b)

On 2026-11-02 between 00:24 and 00:40 (UTC+6) the x86-64 kernel of milestone/67-agents at d11e3ca53, `build/x86_64/rach2/kernel` of 2,318,312 bytes with SHA-256 `48ed7a65...`, was put in the candidate slot with `bootctl kernel` and booted once. The root was the installed release's, milestone 66's at 2dd8dd1b7 with the GNU system, whose kernel is `8b9611b2...`. That root writes no IPv6 stable secret at start-up and its BusyBox `ping` has no `-6`, so the steps used the GNU programs and Python's raw socket, and the secret was written by hand for the last step.

Before the trial both slots were checked against the release record. The candidate held the release; the fallback's first 2 MiB did too, but its bytes read as one run from its first sector did not. The fallback file is in two runs on the boot partition, with `grub.cfg` between them, and read by its FAT chain it held the release as well. `bootctl status` prints only a slot's first sector and size, so `tools/dev/machine_identity.sh`, which reads a slot as one run, cannot confirm a kernel in the Lenovo's fallback slot and refuses when the fallback is the booted slot.

| Step | Result |
|---|---|
| Address | SLAAC took `fd91:381e:e667:4a0c:2ad2:44ff:fe7e:e0af/64`, the modified EUI-64 with no secret written, 5 s after the link-local `fe80::2ad2:44ff:fe7e:e0af`; the prefix route `fd91:381e:e667:4a0c::/64 proto ra` and the router `fe80::10a0:35cb:9f95:89b7` in the neighbour cache; no default route, since the network's router advertises none |
| The Mac pings it | 5 of 5 to the ULA, 0.32-1.0 ms; 3 of 3 to the link-local address |
| `ssh -6` from the Mac | OpenSSH's sshd, started at boot by the release's root, answered on the ULA: `SSH_CONNECTION` named both ULAs |
| curl to a server on the Mac | `python3 -m http.server --bind ::` on the Mac; three GETs of a 4 MiB random file over the ULA, each 0.04 s with the file's SHA-256, and one over the link-local address with `%eth0` |
| An Internet name with AAAA and A | glibc's getaddrinfo returns both, IPv4 first by RFC 6724, since the only IPv6 source is unique-local; curl reached www.google.com, www.cloudflare.com and github.com over IPv4, connecting in 0.13-0.18 s; a connect to Google's IPv6 address failed with ENETUNREACH in 0.1 ms, and `ip -6 route get` of a global address answered "Network is unreachable" |
| Fragmented echo, Mac to Lenovo | `ping6 -s 4000` 5 of 5 and `-s 8000` 2 of 2: `Ip6ReasmOKs` rose by the requests and `Ip6FragOKs` by the replies, three fragments for each 4000-byte one |
| Fragmented echo, Lenovo to Mac | Echo requests of 4000, 8000, 16000 and 20000 bytes from a raw ICMPv6 socket: the Mac's `netstat -s -p ip6` counted each reassembled, and each fragmented reply was reassembled on the Lenovo, `Ip6ReasmOKs` one higher for each |
| Stable secret | With `default/stable_secret` written and `eth0/disable_ipv6` set to 1 and back to 0, the card took `fd91:381e:e667:4a0c:69e1:a0:b051:4daf` by RFC 7217 within 2 s; the Mac pinged it and reached sshd on it |

The kernel log had only the addresses added and removed, and `/proc/net/snmp6` no header error, discard or ICMPv6 error.

What the steps found beyond the plan's checks:

* The Mac sends no ICMP message past 8192 bytes (`net.inet.raw.maxdgram`), so its `ping6 -s 12000` and larger never left it, and it reassembles at most 16 fragments: the Lenovo's echoes of 24000 and 30000 bytes, 17 and 21 fragments, reached it whole and were dropped there.
* Rach2 hands a program at most 1500 bytes of a datagram, though it reassembles larger ones: a raw socket's queue keeps 1500 bytes of each (`socket/raw.c`), as UDP's does (`SOCK_RECV_BUF_SIZE`), and `readv_dgram` reads one datagram through a 1500-byte buffer, reporting the whole length for `MSG_TRUNC`. The 4000-byte replies above reached the Python program as their first 1492 bytes past the ICMPv6 header. UDP refuses a payload past 1500 bytes on either family. The network Readme's Limits now say so; a program that wants a large datagram over IPv6, `ping -6 -s 4000` among them, needs them lifted.

Afterwards the release's kernel was put back with `bootctl kernel`, booted, committed and copied to the fallback with `bootctl fallback`; read by their FAT chains both slots hold `8b9611b2...`, the saved entry is the candidate and no trial is armed. The volume was set with `mixer vol 1` after each boot; nothing played sound or took the screen.
