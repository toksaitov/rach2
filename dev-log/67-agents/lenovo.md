# The Lenovo's figures and proofs

Milestone 67's measurements and proofs on the Lenovo, one section per owner-only task of the plan: 23a the kernel's figures, 23b IPv6, 23c the agents and term.

## The kernel's figures (task 23a)

Measured on 2026-11-01 between 18:40 and 19:13 UTC. The release of milestone 66 (`2dd8dd1b`, kernel sha256 `8b9611b2`, 100 Hz) was measured from the candidate slot, then milestone 67's kernel alone went on a trial boot by `bootctl kernel` over the release's root. The trial kernel measured last, sha256 `bb31494c`, 1000 Hz, is milestone 67's branch at `f48adac86` with the fixes below; the first trial, `f48adac86` alone, is shown where it differs. Each trial was checked by `machine_identity.sh` before it was timed, and the release went back to both slots after.

The programs are the reports' own, `dev-log/66-terminal/agents/claude-code/rach2-repro/`, built with the root's gcc at `-O2` in a directory on the ext2 root: `syscall_latency` on ext2 and on `/dev/shm`, `fs_thread_scaling`, `thread_scaling`, `py_thread_wakeup.py`. `git status` ran five times on a fresh repository of 5,000 files in 50 directories, after one run that refreshed the index; `sleep(0.001)` 50 times from Python's `time.perf_counter`; the idle share is the change in `/proc/stat`'s first line over 30 seconds of an idle machine.

### Figures

| Figure | Reports (Measured) | Release, today | 67 | Target | Verdict |
|---|---|---|---|---|---|
| Warm stat on ext2 | 132-190 us | 31.4 us | 6.56 us | within a few times tmpfs | met |
| Warm stat on tmpfs | 4.6 us | 4.77 us | 5.20 us | | |
| `git status`, 5,000 files | 2.0-2.6 s (4,857 files) | 0.40-0.43 s | 0.083-0.085 s | well under 0.5 s | met |
| 2,000 `pthread_create` | 4.6-5.1 s | 4.10 s | 0.192 s (first trial 2.67 s) | under 0.1 s | missed |
| Python, 500 threads set and join | 31 s | over 10.5 min, stopped | 0.069 s | under 0.5 s | met |
| Python, 250 threads set and join | 2.0-3.9 s | 24.6 s | 0.013 s | | |
| 4 threads stat, aggregate | 5,293/s (1 thread 6,711/s) | 14,232/s (1 thread 20,878/s) | 75,060/s (1 thread 113,527/s) | no slower than before | met |
| `sleep(0.001)` | 7.2-10 ms | median 10.0 ms, max 10.0 ms | median 1.999 ms, max 2.000 ms | under 2 ms | met at the bound |
| `clock_gettime` | 1.58-1.62 us | 1.57 us | 0.04 us, `vdso_tsc 1` | under 100 ns on a valid TSC | met |
| `lscpu` | | 0 sockets, 0 cores, 0 threads | 1 socket, 2 cores, 2 threads a core | one socket, two cores, two threads | met |
| Idle share of the ticks | | 1.000 at 100 Hz | 0.99975 at 1000 Hz | recorded | |

The TSC's verdict: the trial's log has `vdso: clock: the tsc, 1895614 counts a tick`, and `/proc/stat` showed `vdso_tsc 1` with `clock_calls` unchanged across 200,000 calls of `clock_gettime`. `/proc/stat` counts in hundredths of a second: over 30 seconds of the four processors' 120 seconds, the release was busy for none and the trial for 0.03 s.

The release's stat on ext2 today, 31 us, is a fifth of the reports' figure: its buffer cache was nearly empty a minute after the boot, and the reports say the cost grows as the cache fills.

### Misses and their fixes

2,000 `pthread_create` took 2.67 s on the first trial, and the time per 500 threads grew with the threads already made. Three causes were found on the machine and fixed on `task/1930`:

* `k_memmove` copied overlapping memory upward one byte at a time. A process's regions are one sorted array, and each thread's stack is placed below the others, so every `mmap` and every guard-page `mprotect` moved the whole array up byte by byte: 4,000 stacks took 3.2 s in `mmap` alone. The copy now moves words with `std; rep movs`, with interrupts masked while the direction flag is set. 2,000 threads: 0.355 s.
* `fd_table_share` walked every process in the system on each clone with `CLONE_FILES`, to widen the sharers' maps of held descriptors, which already fit once a table is shared. It now walks them only on a table's first share. A bare `clone` of 2,000 threads went from 0.205 s, growing, to 0.047 s, flat. 2,000 threads: 0.192 s.
* `bootctl status` gave the fallback slot as one extent, but its clusters lie in two runs, 2053+4096 and 20040+4096, so `machine_identity.sh` hashed other bytes for it. `bootctl status` now adds a `runs:` line for such a slot, and the script reads the slot run by run.

What is left of the miss is no single bug. On the trial, clones with no new mapping cost 24 us each and do not grow; `pthread_create` with threads that only `pause` costs about 48 us each at the start, the cost of the unoptimised kernel's clone, `mmap` and `mprotect`, and grows by about 18 us a thread for each 500 threads alive, from moving the region array on each insert. 2,000 such threads take 0.150 s, and 0.192 s when each also waits on the condition variable. Reaching 0.1 s needs the regions out of one sorted array, a tree or a gap at the point of insertion, and a cheaper clone. That is a task of its own, not a fix to 7a's.

`sleep(0.001)` met its bound with no room: the sleep ends at the first tick at or after its deadline (`sleep_for`, `syscall.c`), and a sleep that starts just after a tick, as each in a loop does, ends at the second tick after it. Going below 2 ms needs a one-shot timer.

### Thread creation (task 30)

Measured on 2026-11-02 at 01:43-01:49 (UTC+6) in QEMU 11.1.3 on the Mac, with TCG, one processor, `-cpu max` and 1024 MiB. Both architectures booted os-real, logged in, created and joined all 2,000 threads without an error, and powered off. The program is 23a's `thread_scaling.c` with every create and join checked, a report after each 500 creations, and an explicit stack size: 8 MiB on x86-64, as GNU libc uses on the Lenovo, and 256 KiB on i686, whose address space cannot hold 2,000 stacks of 8 MiB. It was linked statically against the ported musl, rebuilt with the milestone's condition-variable wake fallback. The first boots used an older libc and stopped in broadcast; their incomplete runs were stopped and excluded.

The x86-64 comparison alternated three boots of the old kernel at `a9fd8db03` (SHA-256 `9960a016cfc74f72baa9cf757a32cb516291d33fd4f6ad9fa9b4f8c62c92aa84`) with three of task 30's implementation at `288895958` (`be81c9a71f882c993276801daf981644da6c6f32e60bb4cdc3017cc29e62700f`). No hardware was written.

| 2,000 creations | Old kernel | Regions in a tree, cheaper clone |
|---|---|---|
| QEMU run 1 | 1.146106 s | 0.579632 s |
| QEMU run 2 | 1.176432 s | 0.457495 s |
| QEMU run 3 | 1.069933 s | 0.544135 s |
| Median | 1.146106 s | 0.544135 s |
| Last 500 creations, median | 0.371874 s | 0.115873 s |

The new median is 0.475 of the old. Applying that ratio to 23a's Lenovo figure, 0.192 s, predicts 0.091 s for 2,000 creations. This is an estimate from emulation and a different C library, not a measurement on the Lenovo. The first and last 500 in the new runs have similar costs; the old runs grow with the regions already present. The i686 boot, kernel SHA-256 `4fe14139b7b5d55ee93e13cd41784cea759ba3023abe676c3d41a8313dda1597`, created all 2,000 in 0.535078 s and joined them in 0.718645 s.

Two further x86-64 boots instrumented `do_fork` with `arch_cycles` at its stages. The averages after 2,000 clones were as follows; the counts include preemption and are a profile of the emulator, not hardware cycles on the Lenovo. Allocation includes reclaim, the user limit check, record allocation and the old clone's deletion of its initial stdio. The old profile's kernel hash is `5d1f1d521e326e031f02281736794205ad55ca503ba29fd91386eb35ab990492`, the new one's `a81333565b8dee3914c6b9bec5d4dd0763a66382e5831a5c6ce113edf9439764`; the instrumentation was removed after these boots.

| Clone stage | Old counts per clone | New counts per clone |
|---|---|---|
| Record allocation and initial state | 102245 | 22170 |
| Address-space reference and kernel stack | 28139 | 22864 |
| Credentials, namespaces, TLS and FPU | 18260 | 5317 |
| Shared files, directories and TID stores | 24361 | 9290 |
| Signal state and return frame | 19903 | 3198 |
| Trace handling and ready queue | 10135 | 5922 |

The old allocation stage accounts for half of the measured clone cost. `alloc_process_fork` now starts without stdio and directly references the group and directories that a thread shares. Region insertion, removal and lookup use an AVL tree; ordered readers use iterators, and the covered byte count is maintained at each change instead of summed for each address-space limit check. Whole-region `mprotect` needs no spare node at the mapping limit. The VM host test passed with AddressSanitizer and UndefinedBehaviorSanitizer on the testing host for both architecture selections, including its new balance, stable-node, split, merge, limit and allocation-failure rows.

## IPv6 (task 23b)

On 2026-11-02 between 00:24 and 00:40 (UTC+6) the x86-64 kernel of milestone/67-agents at d11e3ca53, `build/x86_64/rach2/kernel` of 2,318,312 bytes with SHA-256 `48ed7a65...`, was put in the candidate slot with `bootctl kernel` and booted once. The root was the installed release's, milestone 66's at 2dd8dd1b7 with the GNU system, whose kernel is `8b9611b2...`. That root writes no IPv6 stable secret at start-up and its BusyBox `ping` has no `-6`, so the steps used the GNU programs and Python's raw socket, and the secret was written by hand for the last step.

Before the trial both slots were checked against the release record. The candidate held the release; the fallback's first 2 MiB did too, but its bytes read as one run from its first sector did not. The fallback file is in two runs on the boot partition, with `grub.cfg` between them, and read by its FAT chain it held the release as well. `bootctl status` now adds a `runs:` line for such a slot, and `tools/dev/machine_identity.sh` reads it run by run (task 23a's fixes).

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
