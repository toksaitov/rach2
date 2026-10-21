# Shared Ubuntu enrollment

T56 enrolled the testing PC's Ubuntu endpoint for #645 on 2026-10-20. The initial launch hooks and cached bridge used source `2a05fe344d81f7f93bfafcb7cfb3f6b95b03ecaa`. This enrollment applies one admission authority to the existing test accounts. T94 still selects the fastest machine from whole-collection measurements; the routine accelerator remains TCG.

## Configuration

| Item | Value |
| --- | --- |
| Endpoint | Ubuntu 26.04 in WSL2 on the testing PC |
| Policy | `/etc/rach2/etude-site`, format `etude-site 1`, endpoint `linux` |
| Launcher | `/usr/local/libexec/rach2/etude-launch` |
| Launcher source | `/usr/local/libexec/rach2/etude-launch-v2`, the committed `tools/etude/launch.sh` |
| Shared ledger | `/var/lib/rach2-etude/ledger`, format `etude-ledger 1` |
| Compatibility locks | `/tmp/rach2-host.lock` and `/tmp/rach2-ports.N` |
| Admitted processors | 24000 thousandths of a processor |
| Admitted memory | 103079215104 bytes (96 GiB) |
| Admitted disk | 171798691840 bytes (160 GiB) |
| Admission wait and execution bounds | 20400 seconds each |
| Executable cache | `/var/cache/rach2-etude` |
| Version key | `faf830e7b2ab178941c403f13774eca2505ae4e87c8071c18be7165711fde7f6` |
| Bridge SHA-256 | `589ea49204d4a93c641b5ebaed6c88f15485741d64e04524b18ad73a92aed7ab` |
| Make wrapper | `/usr/local/libexec/rach2/etude-bin/make`, from `tools/etude/make.sh` |
| Make wrapper SHA-256 | `b8ac1aedc0794258e09666c3611642d61cfaf1458935726e2923d448c28f53fa` |
| Launch script SHA-256 | `1d60c013dddaf06ddb2cbfd6de7b0d6ff645225a22ad306b50c5bcf812b2b5b2` |

The installed wrapper sets umask 0002 before executing the launch script. The ledger and its verdict directory have group `users` and mode 2770; the lock has mode 0660. All seven existing test accounts belong to that group. The policy is readable by all accounts, and the cache and installed executables are writable only by root. No private per-account machine record is needed: the installed policy precedes it in bisection and supplies the admitted limits instead of the hardware's whole capacity.

The current launcher reserves a share of the admitted capacity for each build or selected test run, as [installed launch admission](../../tools/etude/Readme.md#installed-launch-admission) specifies. Ordinary selections preserve explicit worker and guest memory limits. The make wrapper bounds old preparation binaries as well as current builds. Large-memory and explicitly serial selections reserve the host alone. CI and the owner transport admit build and test steps separately. Native Windows still has a separate MSYS2 policy with endpoint `disabled`; a native launch must fail before it executes its command.

The access record covers `testing` for the owner's synced copies and manual checks, `ghrunner` for both CI services and bisection, and the five `toksaitov-*` worker accounts. The root build, `tests/run`, owner commands and CI read the default policy on each launch. Old checkouts and direct commands outside those entry points use the installed external launcher. Existing services need no environment change or restart to find that default file. Root is used only for enrollment maintenance.

## Acceptance

The earlier staged deployment passed all 62 prerequisite probes twice, reused its version and left the machine record unchanged. The current cache version was then built from the source above, outside every clone. Before activation the WSL root had 237 GiB free, its temporary file system 62 GiB, Windows C: 140 GiB and D: 543 GiB. WSL reported 123248 MiB available memory. The disk reservation leaves more than the 40 GiB free-space floor; the memory and processor limits leave capacity for the host and other Windows work.

The unwrapped CI commands and the owner's existing check finished before activation. At `2026-10-20T15:46:45Z`, the installed launcher acquired the compatibility host lock exclusively and a port block. The activation command found no unwrapped build or test, checked free disk and available memory again, and published the policy while it held those locks. Available memory was then 125154 MiB; the disk readings were unchanged. The drain's claim was released after confirmed cleanup.

Each of the seven accounts then ran the installed launcher. Its child observed that account's registered running claim, all three policy resource amounts, the exclusive host lock, a reserved port block and the derived worker and guest memory limits. A nested launch passed in the same reservation. After each command the ledger contained only its header, proving that the next account could replace the previous account's claim file. An old checkout passed through the external `legacy` launcher. A native Windows launch exited 2 with `endpoint is disabled or incompatible`. A second request for the cached version reused it.

The private deployment, access, drain and activation evidence is under `/var/lib/rach2-etude/enrollment/`. No account, key, clone, CI service or accelerator setting changes as part of enrollment.

## Shared admission

The first launcher kept asking for `alone` and the site's full capacity after enrollment. On 2026-10-20 ten requests waited behind one while the 24 admitted processors had a load average of 0.4. Issue #792 corrects the launcher and installs the make wrapper before enabling shared admission, so old preparation binaries cannot run an unbounded build beside another account's tests. The policy, bridge, capacity and account access stay unchanged. Earlier running and queued claims retain their original reservations and finish in order.

On 2026-10-21, `testing` and `toksaitov-m` ran their `host` suites concurrently within `tests/run style host`. The ledger recorded two running claims while both status records contained running `legacy.host` attempts. The reservations were four processors and 6 GiB for `testing`, eight processors and 10.5 GiB for `toksaitov-m`, and 40 GiB of disk each. An installed `alone` command stayed queued without executing while these tests ran, and ran after the shared claims ended. The host-tool build, every style case and all 226 host checks passed. The claim snapshots, status records and logs are under `/var/lib/rach2-etude/enrollment/792-shared/`.

## CI cancellation

On 2026-10-22, #820 replaced the policy's bridge with the cached version above after the build, style checks, host suite and `etude-runner-cleanup` proof passed. An installed launch checked the [process markers](../../tools/etude/Readme.md#installed-launch-admission) and left no claim. Earlier jobs kept their previous bridge. The private record is under `/var/lib/rach2-etude/enrollment/820-cleanup/`.
