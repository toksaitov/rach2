# Shared Ubuntu enrollment

T56 enrolled the testing PC's Ubuntu endpoint for #645 on 2026-10-20. The launch hooks and cached bridge use source `2a05fe344d81f7f93bfafcb7cfb3f6b95b03ecaa`. This enrollment applies one admission authority to the existing test accounts. T94 still selects the fastest machine from whole-collection measurements; the routine accelerator remains TCG.

## Configuration

| Item | Value |
| --- | --- |
| Endpoint | Ubuntu 26.04 in WSL2 on the testing PC |
| Policy | `/etc/rach2/etude-site`, format `etude-site 1`, endpoint `linux` |
| Launcher | `/usr/local/libexec/rach2/etude-launch` |
| Launcher source | `/usr/local/libexec/rach2/etude-launch-v1`, the committed `tools/etude/launch.sh` |
| Shared ledger | `/var/lib/rach2-etude/ledger`, format `etude-ledger 1` |
| Compatibility locks | `/tmp/rach2-host.lock` and `/tmp/rach2-ports.N` |
| Admitted processors | 24000 thousandths of a processor |
| Admitted memory | 103079215104 bytes (96 GiB) |
| Admitted disk | 171798691840 bytes (160 GiB) |
| Admission wait and execution bounds | 20400 seconds each |
| Executable cache | `/var/cache/rach2-etude` |
| Version key | `24bbfa8b7cf08a83f5328304c6b7e727cf694c37c91cdeebdafe3234026a6845` |
| Bridge SHA-256 | `116e7355bd9b0c24280f50045c7e92a48de40650afd6cf3cb8a9b3508efb45e3` |
| Launch script SHA-256 | `4e5067d5e5c6a211ef7b1de808a9856b4e557db36ca0df6b5f8201f03834211c` |

The installed wrapper sets umask 0002 before executing the launch script. The ledger and its verdict directory have group `users` and mode 2770; the lock has mode 0660. All seven existing test accounts belong to that group. The policy is readable by all accounts, and the cache and installed executables are writable only by root. No private per-account machine record is needed: the installed policy precedes it in bisection and supplies the admitted limits instead of the hardware's whole capacity.

Until per-case scheduling replaces the old pool, one command reserves the whole capacity, the compatibility host lock exclusively and one legacy port block. The launcher derives six workers for the old runner and 73728 MiB of guest memory. Nested commands inherit the reservation. This also accounts for a command's builds and services. Native Windows has a separate MSYS2 policy with endpoint `disabled`; a native launch must fail before it executes its command.

The access record covers `testing` for the owner's synced copies and manual checks, `ghrunner` for both CI services and bisection, and the five `toksaitov-*` worker accounts. The root build, `tests/run`, owner commands and CI read the default policy on each launch. Old checkouts and direct commands outside those entry points use the installed external launcher. Existing services need no environment change or restart to find that default file. Root is used only for enrollment maintenance.

## Acceptance

The earlier staged deployment passed all 62 prerequisite probes twice, reused its version and left the machine record unchanged. The current cache version was then built from the source above, outside every clone. Before activation the WSL root had 237 GiB free, its temporary file system 62 GiB, Windows C: 140 GiB and D: 543 GiB. WSL reported 123248 MiB available memory. The disk reservation leaves more than the 40 GiB free-space floor; the memory and processor limits leave capacity for the host and other Windows work.

The unwrapped CI commands and the owner's existing check finished before activation. At `2026-10-20T15:46:45Z`, the installed launcher acquired the compatibility host lock exclusively and a port block. The activation command found no unwrapped build or test, checked free disk and available memory again, and published the policy while it held those locks. Available memory was then 125154 MiB; the disk readings were unchanged. The drain's claim was released after confirmed cleanup.

Each of the seven accounts then ran the installed launcher. Its child observed that account's registered running claim, all three policy resource amounts, the exclusive host lock, a reserved port block and the derived worker and guest memory limits. A nested launch passed in the same reservation. After each command the ledger contained only its header, proving that the next account could replace the previous account's claim file. An old checkout passed through the external `legacy` launcher. A native Windows launch exited 2 with `endpoint is disabled or incompatible`. A second request for the cached version reused it.

The private deployment, access, drain and activation evidence is under `/var/lib/rach2-etude/enrollment/`. No account, key, clone, CI service or accelerator setting changes as part of enrollment.
