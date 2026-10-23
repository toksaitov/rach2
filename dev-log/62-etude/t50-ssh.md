# T50: TinyCC over SSH on the Lenovo

The four TinyCC modes passed on 24 October 2026 from the Mac through actual SSH to the Lenovo G50-70. This meets T50 and [#963](https://github.com/toksaitov/rach2/issues/963#issuecomment-6375581096). It does not establish T93, the run of every eligible case on the final candidate kernel.

The target was `root@192.168.0.106`, Ethernet address `28:d2:44:7e:e0:af`, booted in the candidate slot. Before staging and each connection, the identity check required that address, `rach2.slot=candidate`, `bootctl: booted: candidate`, no pending next entry, and the candidate's first sector 2053 and length 4096 sectors. The first 1766480 kernel bytes had SHA-256 `b18555143514b7c21105e93e2cbb1fddf60c0be9524c752f8190872ab36f3555`, matching the retained `h682/build/x86_64/rach2/kernel` artifact. This is an artifact identity; no kernel source commit is claimed.

## Inputs and commands

* The native Mac controller was built from `46202f4dc014a556f423d59eceb3ed0da10189ef`; SHA-256 `853205b162db20ca0d237856c9956e966eab5ae56f436d26bc1fe6a5f603446f`.
* The guest helper was built from `eaa519e2ed9c31093deb6ad47f51d1c04b8a742f`; all its recorded source inputs were unchanged at the controller commit. Its SHA-256 was `e16c50e6d35931e34c55fcc63e304f3e20fca81dd3d8a3b48017a19706d820f5`.
* The Mac relay was the existing `etude_native_hosttest --relay` role, SHA-256 `0b11c368d539421066598ade0009f417a3acf1bfac6f1d04fb859b8190f0995b`. Its compile record used `73f5ea5cb84dc1deac768de6c4b123087c47daac`; every recorded source and header hash also matches the controller commit. Apple Clang 21.0.0 compiled it with ASan and UBSan; `/usr/bin/cc` had SHA-256 `34129c71a01a74f7f3b2443521519b2e5447553fa187f5fcafaaf8c42cc192e2`.
* The guest's `/bin/tcc` was 304456 bytes, SHA-256 `7d3425594c394a410c5a70e4bfdddae97197eda0248ab05da6dc8d8f67be64ee`. The transferred `tests/native/tccbuild.sh` was 1661 bytes, SHA-256 `09427542f37c192a8d5d5662dba78e1cfbad4923675521ffd026d1461d6f8c7e`.

The retained command file defines `ROOT=/Users/toksaitov/Desktop/rach2`, `OUT=$ROOT/build/tests/portable-batch/t50-relay`, `RUN=t50-b075b2f4b5e8` and `TARGET=/var/t50-b075b2f4`. The private owner token was `b075b2f4b5e8e1f39bf58cb240227a1e`. The helper was staged at `$TARGET/guest`, checked by hash and executed as:

```sh
exec /var/t50-b075b2f4/guest --stdio --state /var/t50-b075b2f4/state
```

The transport is `exec ` followed by shell-quoted arguments for `/usr/bin/ssh -T -i $ROOT/krffs/rach2_demo_ed25519 -o BatchMode=yes -o IdentitiesOnly=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=yes -o UserKnownHostsFile=$OUT/identity/known-hosts root@192.168.0.106 REMOTE`. `REMOTE` performs the identity checks above, verifies the private owner token and helper hash, then executes that helper. The cut transport inserts `$OUT/etude_native_hosttest --relay kill $OUT/ssh-cut.marker` before `/usr/bin/ssh` in this same argument vector. No guest command uses a debug-exit port.

Each mode ran under the installed Mac admission launcher:

```sh
ETUDE_SITE=/Users/toksaitov/flow/claude/r680/site/etude-site \
  /Users/toksaitov/flow/claude/r680/site/bin/etude-launch \
  -r "$OUT/$MODE.admission" build \
  "$OUT/etude-46202f4d" run native -R "$RUN" -a "$RUN:$MODE" \
  -c "t50.tcc.$MODE" -j "$OUT/journal" -e "$OUT/$MODE.evidence" \
  -t "$TRANSPORT" -f "$ROOT/tests/native/tccbuild.sh" -E TCC=/bin/tcc -k \
  -- tccbuild.sh "$WORKLOAD"
```

For `pass` and `fail`, WORKLOAD equals MODE. For `cancel` and `cut`, WORKLOAD is `loop`; `cancel` additionally supplies `-x 3000` before `--`.

## Results and cleanup

* `pass` returned 0 and printed `tccbuild: sum 5050` and `tccbuild: pass`.
* `fail` returned 1 with `broken.c:1: error: #error this build fails on purpose`.
* `cancel` returned 3 after 62 completed builds. Journal command 15 sent `cancel c91deeb7ed32d08b`; the terminal record reports `stopped` and confirmed cleanup.
* `cut` returned 4. After the first completed build, the relay killed and reaped the actual SSH client. One reconnect recovered the same attempt token, its stopped result, complete stream hashes and confirmed cleanup. Status 4 is the expected connection-loss outcome for this acceptance.

All four records identify session `ab323aae7116e00f` and guest boot `c548050ca60fb8da`. The raw records include each owner and workload PID with its creation identity:

```text
cmd 4
session ab323aae7116e00f
owner 13466 4935271
child 13485 4935337
boot c548050ca60fb8da
state cleaned
exit 4 b85621b381956634 exited 0
stream 4 b85621b381956634 1 34 34 6be8012a94694478e596f14d1f579ae2bce64e54e343094c8ef835fc0269e0db
stream 4 b85621b381956634 2 0 0 e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
usage 4 b85621b381956634 370000+ wait-reaped-descendants-v1 1851392+ posix-maxrss-v1
cleaned 4 b85621b381956634 confirmed
cmd 9
session ab323aae7116e00f
owner 13493 4935408
child 13512 4935473
boot c548050ca60fb8da
state cleaned
exit 9 e99167008c23827b exited 1
stream 9 e99167008c23827b 1 0 0 e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
stream 9 e99167008c23827b 2 54 54 c3230faf36746de5d79d78b2371c5bfea7b291123d605d947433fced902f185a
usage 9 e99167008c23827b 60000+ wait-reaped-descendants-v1 712704+ posix-maxrss-v1
cleaned 9 e99167008c23827b confirmed
cmd 14
session ab323aae7116e00f
owner 13517 4935508
child 13536 4935572
boot c548050ca60fb8da
state cleaned
exit 14 c91deeb7ed32d08b stopped -
stream 14 c91deeb7ed32d08b 1 1169 1169 07fd2876372cc4e9d2c9d35eb561ea03c55d981ff219a408673017374b241b6a
stream 14 c91deeb7ed32d08b 2 0 0 e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
usage 14 c91deeb7ed32d08b 2700000+ wait-reaped-descendants-v1 1851392+ posix-maxrss-v1
cleaned 14 c91deeb7ed32d08b confirmed
cmd 20
session ab323aae7116e00f
owner 13666 4935949
child 13685 4936013
boot c548050ca60fb8da
state cleaned
exit 20 1e8461bb32226d69 stopped -
stream 20 1e8461bb32226d69 1 18 18 4ba65d330febb0d1120e39223c699bb9a83c1a5f74dbc9b74f9097bcb7c6fa42
stream 20 1e8461bb32226d69 2 0 0 e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
usage 20 1e8461bb32226d69 330000+ wait-reaped-descendants-v1 1847296+ posix-maxrss-v1
cleaned 20 1e8461bb32226d69 confirmed
```

The independent cleanup check found all eight recorded process identities ended. The private target was removed after archiving the records, invocation files, output and guest log. The Mac admission receipts were `exited 0`, `exited 1`, `exited 3` and `exited 4`; its ledger then contained only `etude-ledger 1`. Only the helper and private test files were written. The installed kernel, root and boot settings were unchanged; `bootctl` still reported the candidate boot, saved fallback and empty next entry.

The retained evidence is under `build/tests/portable-batch/t50-relay/` in the owner's primary tree. `commands.json` preserves the full quoted argument vectors (SHA-256 `d2028c7e4c8ad2f7127145f982989baaf25b03c4e4124f91e015bfff8252d067`); `verified.json` has SHA-256 `1b448c033f656164d0ee9599f017d2a6a9c4a62feaeb90481d8c601eb30709e2`; `cleanup.log` has SHA-256 `846da3ff0b82c002336bbab577f3a8040e20786c65d444f87c97b217df65d83f`. The guest-state archive has SHA-256 `9e30acd52984c17bfda5950077cbf0244fba29a8e63cdaefd2785d89a4956475`. The issue comment retains the result and raw records independently of this scratch directory.
