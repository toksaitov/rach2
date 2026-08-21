# Milestone 27: reading every file

One reading of every document and every comment the project owns, judged
against the code it describes. The method and the reasons are in `plan.md`; the
order of work and its state are in `worklist.md`. This file records what the
reading found: the claims that were false, the comments that described the
reasoning that produced the code rather than what the code does, and what was
noticed about the code itself and deliberately left for milestone 28.

This record is written as the work proceeds, so an interrupted run still has the
findings up to where it stopped.

## What was fixed

### Set one: what a reader consults

* `vendor/busybox/Readme.md`: used "load-bearing", a term `CLAUDE.md` bans at
  any frequency in Readmes. `make verify-style` passed with it present, which
  shows `tools/prose_check.py` does not scan the `vendor/*/Readme.md` files;
  the vendor Readmes are therefore hand-checked for the banned vocabulary in
  this milestone. Replaced with "essential".
* Scope of `tools/prose_check.py` (read to bound the gap): it scans `.c .h .py
  .sh .mk .md` under `rach2 utils ish krffs tests tools mk ports/doom` plus the
  three top-level `.md`, and skips `vendor/` entirely. So the banned-vocabulary
  check never reaches: files literally named `Makefile` (no extension), anything
  under `vendor/`, `dev-log/`, or the non-scanned extensions (`.s`, `.ld`,
  `.manifest`, `.cfg`, `.template`, `.script`, `.txt`). Those are hand-checked
  for banned vocabulary in this milestone; the scanned files are already clean
  by the gate.
* The `Makefile`s used "seam" (a mechanically banned term) in build comments,
  as "seam test" for a host test -- root `Makefile` (2), `utils/Makefile` (2),
  `krffs/Makefile` (1), none caught because `Makefile` has no scanned extension.
  Changed each to "host test" / "test".
* `rach2/guest.mk`: a comment said the link names "71 objects on one command
  line", but `OBJS` is 87 (5 assembly + 82 C, matching `rach2/Makefile` and
  rach2/Readme's "87 objects"). The count went stale as the kernel grew; no
  checker verifies a number in a comment. Corrected to 87.
* `mk/toolchain.mk`: an override example's inline annotation was mangled by a
  line wrap into "# odd" then "#names " (no space after `#`, trailing space).
  Rewrote it as a clean parenthetical line.
* `mk/rules.mk`: a comment wrap left "#directly." (no space after `#`, trailing
  space) on its own line. Fixed to "# directly."
* `vendor/pdpmake/build_pdpmake.sh`: its comment called pdpmake's POSIX makefile
  "26-line" while `vendor/pdpmake/Readme.md` called the same file "30-line". The
  extracted `pdpmake-2.0.4/Makefile` is 30 lines, so the Readme was right and
  the build-script comment wrong. Corrected the comment to 30.
* `vendor/dropbear/build_dropbear.sh`: a build progress line named the multi
  binary's contents as "dropbear server + dropbearkey", omitting `dbclient`,
  which the `PROGRAMS` line just below and the header comment above both
  include. Added `dbclient` to the echo.
* `vendor/dropbear/Readme.md`: the crypto list said "`localoptions.h` curates
  the crypto: ... RSA (signkey.c needs `DROPBEAR_SIGNKEY_RSA`) ...", but
  `localoptions.h` sets no RSA option at all. RSA is on because dropbear's own
  `default_options.h` defines `DROPBEAR_RSA 1` (verified in the extracted
  `dropbear-2022.83` tree), and `sysoptions.h` derives `DROPBEAR_SIGNKEY_RSA`
  from it. Reworded so RSA is attributed to the dropbear default, not to
  `localoptions.h`. Found while reading `localoptions.h`, after that Readme was
  already marked read.

* `utils/Readme.md`: `init.c` was documented twice, a full entry among the
  privileged programs (after `login.c`) and a shorter recap just before the
  Credentials section. The recap repeated what the full entry and the staging
  paragraphs already stated, and its "see the staging rule below" pointed at a
  rule that is above it, not below. Removed the recap; the full entry stands.
* `tools/Readme.md`: two Markdown defects. A closing code fence in the recovery
  section had the following paragraph glued onto the same line, so the fence did
  not close and the prose rendered inside the block. And the "GRUB in the gap"
  heading had its whole text written twice on one line. Fixed both.
* `krffs/Readme.md`: the `make rach2-sealed-disk` build command was a stray
  indented code line stranded between two prose paragraphs, cut off from the
  other `make *-disk` commands in the code block above. Moved it up to join
  them. Also rewrote a Build sentence with two grammar faults ("builds with
  host version", "to allow to create").
* `ish/Readme.md`: the guest-build sentence said tcc "compiles the five objects
  and links them ... with ish's own `crt0.o`", which counts crt0.o twice:
  `ish/guest.mk` links exactly five objects and crt0.o is one of them. Reworded
  so crt0.o is named as among the five, not linked as a sixth.

* `rach2/sys/fs/krffs.h`: the on-disk node comment gave the header as "344
  bytes", but the packed `krffs_node_t` is 357 bytes (confirmed by compiling a
  `sizeof` of the struct: small fields 13 + id 8 + name[256] + three 16-byte
  timestamps + four 8-byte size/id fields). The count went stale as the struct
  grew (v2 added `parent_id`). No check verifies a size written in a comment.
  Corrected to 357.

* `rach2/sys/network/net.h`: the header comment said "The stack targets QEMU's
  SLIRP user network only", but the same file (and the metal milestone's RTL8168
  work) contradicts it -- a real LAN is configured through netconf.h. Removed the
  stale "only". Also generalized "filled in by the RTL8139 driver at init" to
  "the bound NIC driver", since the RTL8168 driver fills `net_mac` too.

* `rach2/sys/pci.h`: the doxygen comment "Set the Bus Master bit so the device
  can drive DMA" was orphaned above the PCIe-capability-walk block comment and
  its defines, while the function it documents, `pci_enable_bus_master`, sat far
  below with no comment. The PCIe functions had been inserted between the two.
  Moved the comment back down to its function.

* `rach2/sys/io/poll_select.c`: the doxygen block for `pollfds_to_fdsets`
  ("Project poll results back into select()'s three fd_sets", with the
  ready-count `@return`) was stranded above `pollfd_select_ready`, which had its
  own doc below it, while `pollfds_to_fdsets` itself was left undocumented --
  `pollfd_select_ready` had been inserted between the doc and its function. Moved
  the block down to `pollfds_to_fdsets`, the same orphaned-doc pattern as the
  `pci.h` bus-master comment.
* `rach2/sys/network/arp/arp.c`: the same orphaned-doc pattern. The block "Store
  or refresh an IP-to-MAC mapping" (learning from every ARP packet, entries not
  aging out) describes `arp_cache_update`, but sat above `arp_flush` (which only
  clears the cache), leaving `arp_cache_update` undocumented. Moved the block to
  `arp_cache_update` and wrote `arp_flush` its own one-line doc.
* `rach2/sys/isr.h`: a comment read "First 15 interrupt request handlers" above
  the declarations `irq0` through `irq15`, which are 16 handlers (IRQ0-15). The
  "First 32 interrupt handlers" comment above it correctly counts isr0-isr31.
  Corrected the IRQ count to 16.

* `rach2/sys/syscall.c` (copy_path_cwd doc): a comment read "resolve correctly
  only from cwd `/`, which has because programs open them at startup" -- "which
  has" is a garbled clause missing a word. Reworded to "which is the case
  because programs open them at startup".
* `rach2/sys/syscall.c` (do_access doc): the comment claimed "There is no real
  uid/credential model, so R/W/X are checked against the object's mode bits" --
  stale since the multi-user milestone. The code below it judges access(2) by
  the caller's REAL ids (`cred_access_bits(..., 1)`), which its own later comment
  states. Reworded to describe the real credential model.
* `rach2/sys/syscall.c` (sys_execve): a comment said the program-path copy
  buffer "is 64 bytes (NUL included)", but `_exec_path_buf` is `[256]` and the
  copy uses `sizeof(_exec_path_buf)` (256, matching the 255-byte path limit).
  The 64 was stale. Corrected to 256.
* `rach2/sys/syscall.c` (do_getsockname): a comment wrote the identifier
  `copy_sockaddr_to_user` with a stray space, "copy_sockaddr_to_ user". Removed
  the space.
* `rach2/sys/syscall.c` (socket-option defines): `SO_REUSEADDR 2` was defined
  twice, a few lines apart, both to the same value (so the build did not flag
  the redefinition). Removed the duplicate.
* `rach2/kernel.c`: an isolation-test comment read "isolation has if each reads
  back its own value" -- "has if" is a mangled verb (the same pattern as the
  copy_path_cwd comment); the word is "holds". Corrected.
* `rach2/sys/network/tcp/tcp.c`: two more of the same mangled-"has" defect. In
  `tcp_accept_data`'s block comment, "so it has in ESTABLISHED while queued data
  drains" was missing its verb; reworded to "so it happens in ESTABLISHED".
  And in `tcp_rx`'s ESTABLISHED FIN handling, "seq + payload_len == rcv_nxt has
  only when the whole payload was accepted" should read "holds only when".
  Corrected both. (The "surfaced by SO_ERROR" comments are the sanctioned
  error-surfacing sense, left as is.)
* `rach2/sys/proc/wait_queue.c`: `wq_wait_event`'s summary read "Block the
  current process on `wq` until `cond` has or a signal arrives" -- the same
  mangled-"has" defect; the verb is "holds" (the function's own later line says
  "when the condition became true"). Corrected.
* `rach2/sys/lib/kstring.c`: the standard-memory-wrapper comment ended "the type
  each compiler's builtin prototype expects, which `-Werror` has these
  definitions to" -- the same mangled-"has"; the verb is "holds" (`-Werror`
  holds these definitions to that type). Corrected.
* `rach2/sys/network/driver/netdev.c`: a probe comment read "probing stops here,
  not let the next driver configure a machine whose card already failed" -- "not
  let" is ungrammatical; reworded to "stops here rather than let the next
  driver...".
* `rach2/sys/tables/idt.c`: the `_load_idt` comment said it is "Defined in
  idt_handler.asm", but the file is `idt_handler.s` (no `.asm` exists in the
  tree; `gdt.c`'s parallel comment correctly says `gdt_handler.s`). Corrected
  the extension.
* `rach2/sys/rtc.c`: a decode comment read "a clock that reports month 0 is not
  a clock to build a build on" -- the doubled "build a build" is a slip no
  careful author writes. Reworded to "not a clock to base a build on" (the RTC
  time anchors reproducible-build timestamps, so the build reference is kept).

* `rach2/grub_install.sh`: the same mangled-wrap defect as `run.sh`. A usage
  example's comment "build the image to copy" wrapped so "copy" fell to the
  start of the next line, turning that example command into "copy cd rach2 &&
  make". Reflowed the two lines so "copy" ends its comment and the command reads
  "cd rach2 && make".
* `rach2/run.sh`: a usage-comment line wrap split "Sound Blaster 16" so the
  "16" fell to the start of the next example line, which then read
  "#   16 ./run.sh test" -- a broken command and misaligned with the other
  examples. Removed the stray "16 " (the line above keeps "Sound Blaster", and
  the `sb16` argument already names the device).

### Set two: the rest of what we wrote

* `utils/pwauth.c`: one more mangled-"has". The `pwauth_pipe_handler` comment
  read "an exec resets a caught disposition to the default and has an ignored one
  into the new image" -- "has ... into" wants a motion verb, and the fact is that
  exec carries a SIG_IGN disposition into the new image (while resetting a caught
  one to the default). Reworded to "carries an ignored one into the new image".
* `utils/pwauth.h`: the same mangled-"has" in the `cred_txn_begin` contract:
  "It is caught, not ignored because an exec has an ignored disposition into the
  new image" -- reworded to "an exec carries an ignored disposition into the new
  image" (the file's own `pwauth_read_password` doc already says "an exec passes
  into the next image", the correct form).
* `utils/dhcpc.c`: another orphaned doc, at a large distance. The block "Claim
  the interface, or report who has it" (with its `@return 0 when the interface is
  this client's`) documents `claim_interface`, but sat stranded above `pid_alive`
  (which had its own doc below it), while `claim_interface` -- defined ~360 lines
  further down after many functions were inserted between -- was undocumented.
  Moved the block onto `claim_interface`.
* `utils/bootctl.c`: another orphaned doc. The block "Replace GRUB's core image,
  writing the tail that is not in use" (the write-verify-then-flip-LBA-1 safety
  argument) documents `cmd_grub`, but sat above the `grubcore_io` helper -- which
  has its own doc -- while `cmd_grub` was undocumented. Moved the block onto
  `cmd_grub`.
* `utils/passwd.c`: a register-rule (person-judged half) fix. The header read
  "That ordering is the point, not a detail: ..." -- the "not a detail" reversal
  is the exact construction CLAUDE.md names as an example to avoid ("Not a
  detail. A design decision."). Reworded to "That ordering is deliberate: ...",
  which states the claim plainly; the sentence already explains why (one image
  of the databases).
* `krffs/gen_demo_password.sh`: the header said it "Mints a random password per
  demo account", but the script's default is a fixed memorable password
  ("1qA2wS3eD"/"4rF5tG6yH"), and the body itself says "the same on every build...
  Short and memorable beats random here" (only the salt is random, or the password
  if `RACH2_*_PASSWORD` overrides it). Removed the false "random".
* `utils/dhcpmsg.c`: a doubled word across a line break in the `dhcp_boundaries`
  doc: "...into zero. The / The lease must be at least DHCP_MIN_LEASE". Removed
  the repeated "The".
* `krffs/manifests/{grub-test,os-real,rach2-metal-disk}.manifest`: a garbled
  sentence in the shared `/etc` comment, "passwd keeps world-readable x fields,
  the split shadow exists for." -- a comma splice ending on a dangling "for".
  Reworded to "...which is what the split shadow exists for." Fixed in all three
  that carried it (the other manifests do not); grub-test and os-real also had to
  stay in step because `make grub-test` diffs the two.
* `utils/adduser.c`: a badly-wrapped comment. The `ADDUSER_CRASH_POINT` block
  broke as "...never in a shipped / image. It / takes a leading count", leaving
  "It" orphaned on its own short line. Reflowed the block (all lines under 80).
* `utils/grubslot.h`: another orphaned doc. The block "Read or write one 512-byte
  sector of the gap" documents the `grubslot_io_fn` typedef (the sector read/write
  callback), but sat above `grubslot_prepare`'s own doc while the typedef was
  undocumented. Moved the block onto the typedef.
* `krffs/stageimage.krffs.c`: two in-function comment lines ran past the 80-column
  comment limit (the `optional-file` skip note and the fgets-truncation note).
  `tools/check_comments.py` scopes its 80-column check to `rach2/`, so these
  krffs-tool lines were never flagged. Rewrapped both; CLAUDE.md's "code comments
  wrap at 80 columns" holds tree-wide, not only in the kernel.
* `tools/check_setid.py`: one more mangled-"has". The main-busybox forbidden-applet
  comment read "So the rule still has for all five; what has changed is that one
  of them is now a protocol away" -- the verb is "holds". Corrected. Also four
  docstring lines had lost their 80-column wrap (editing artifacts running to 135
  columns among their ~72-column neighbours): `strtoul`, `check_init_line`, and
  the paragraph duplicated in `applet_names` and `check_applets`. Reflowed each;
  `tools/check_comments.py` does not scan `tools/`, so the width was unchecked,
  and prose in a Python docstring wraps at 80 the same as a `#` comment.
* `tools/mkdisk_grub.sh`: the staged `grub.cfg` header comment joined two
  independent statements with a semicolon but kept the capital, "...whatever
  that file has; Every entry names its own root." Changed the semicolon to a
  period. (A `#` comment in GRUB config; the disk image already carries a
  per-tree SSH key and salt, so it is not byte-reproducible and the edit changes
  no test.)
* `tools/account_test.sh`: two fixes. A comment used "Three traps this repo has
  already paid for" -- "trap" for a difficulty is the register rule's banned
  invented sense, and this script uses the shell `trap` builtin throughout, so
  the word is doubly wrong here. Reworded to "Three mistakes ... avoided by every
  phase here". And "adduser exits, not invent a group that is not there" is the
  same mangled "not X" as the `netdev.c` probe comment; reworded to "exits rather
  than invent a group".
* `tools/shutdown_test.sh`, `tools/tcc_test.sh`, `tools/make_test.sh`: the same
  banned "traps" idiom account_test.sh carried ("Three traps this repo has
  already paid for", "Two traps ..."). Reworded each to "mistakes". The many
  other "trap" mentions in these harnesses ("not only in the trap", "the trap
  calls it again") are the shell `trap` builtin and stay. `prose_check.py`'s own
  occurrence is the rule text and is exempt.
* `tools/check.sh`: the header's numbered summary of the thirteen checks listed
  "8. text style" out of order, after 12, though the code runs it eighth (its
  `hr "8/13 text style"` sits between the source-list guard and the probe-scoring
  step). An editing artifact from a renumber. Moved the bullet to its numeric
  place.
* `tools/menu_test.sh`: two stale counts left by milestone 25's additions. The
  header said "Four boots of one disk prove it" and narrated a first/second/
  third/fourth, but the script now runs nine boots, with the static-address
  trial and the saved-static-default scenarios the narrative never mentions.
  Rewrote the header to describe the boots that are actually there. And a failure
  message read "the six stub entries are not in the order..." while the `want`
  list it checks has seven (the recovery-static entry was added). Corrected to
  seven.
* `tools/diskdiag_test.sh`: the header opened "This is the instrument the phase E
  carve decision rests on" -- "phase E" is an undefined reference to a milestone-24
  dev-log phase (the deliberately-unbuilt SSD carve) and means nothing to a reader
  of the script. Reworded to "the decision to carve the laptop's SSD rests on",
  which names the decision plainly.
* `tools/cppcheck.suppress`: a comment used a spaced `--` as an em-dash, "a
  deliberate, justified exception -- this file is NOT a place to silence real
  bugs", which CLAUDE.md forbids in prose. The `.suppress` extension is scanned
  by neither `textcheck.sh` nor `prose_check.py`, so it was never caught. Recast
  to a semicolon. (The `# --- section --- ` dividers are triple dashes, not the
  spaced `--` the rule names, and stay.)
* `tools/check_guest.py`: the docstring introduced "the four contracts" and then
  listed five numbered items, and `main()` runs five checks (kernel lists, utils,
  ish, limits, staging). The staging contract was added later without updating
  the count. Corrected "four" to "five".
* `tools/netconsole_check.py`: the docstring had the mangled-"has" again, "This
  reads that file and has the frames to the contract the header states" -- the
  verb is "holds" (it holds each frame to the contract). Corrected.
* `tools/check_recovery.py`: a docstring line lost its wrap and ran to ~145
  columns (the `rach2/sys/block/recovery.c` path was inserted without reflowing),
  out of step with its ~78-column neighbours. Reflowed.
* `tools/check_diskpolicy.py`: a comment read "The read-write banner is a
  substring of nothing, but the two banners share a prefix..." -- the "substring
  of nothing" clause is confusing, and its near-twin `check_recovery.py` states
  the identical fact cleanly. Reworded to match: "The two banners share a prefix,
  so each is counted whole and the pair is judged together, not one being tested
  by itself."

### Set two: the tests

* `tests/host/pwauth_hosttest.c`: two fixes. The `slurp` doc block ("Read at most
  `size - 1` bytes...") was orphaned above `mask_changed`, with the interposed
  `fsync`/`tcsetattr`/`getpid`/`snprintf` functions inserted between it and the
  real `slurp` far below, which had lost its comment. Moved the block onto
  `slurp`. And the `check_sigpipe_exec` doc carried the same mangled-"has" as
  `pwauth.c`/`.h`: "an exec resets a caught signal to the default and has an
  ignored one into the new image" -- reworded to "carries an ignored one".
* `tests/host/rtc_hosttest.c`: a doubled inline comment on the twelve-hour PM
  case, `raw.status_b = 0; /* 12-hour, BCD */ /* 12-hour, BCD */`. Removed the
  repeat.
* `tests/musl/creddisk.c`: the `temps_left` doc block was mangled by two bad
  wraps, orphaning "cannot" and "must" each onto a line of its own ("A root
  directory that / cannot / be opened..."). Reflowed the paragraph at 80
  columns.
* `tests/idreport.c`: the header had a mangled wrap that left "non-login" alone
  on a line ("told apart from a / non-login / one by nothing"). Reflowed the two
  lines.

### Set two: the Doom port and the rest

* `ports/doom/doomgeneric_rach2.c`: the `DG_SleepMs` comment ran two sentences
  together, "Sleep for a number of milliseconds Since we don't have a real sleep
  syscall" -- a missing period and a stranded capital `Since`. Added the period.
  (The Doom glue is a port adaptation in its own more-verbose, first-person
  register; its comments are accurate to the code and were not restyled, only its
  errors fixed.)
* `.clangd`: a `#` comment used a spaced `--` as an em-dash, "the compiler named
  in the database -- but only for drivers you allow", which CLAUDE.md forbids in
  prose. The file is scanned by neither `textcheck.sh` nor `prose_check.py`.
  Recast to a comma.
* The Doom sound and music sources are third-party ports and were read for
  provenance, not edited: `ports/doom/opl3.c` and `opl3.h` are Nuke.YKT's Nuked
  OPL3 (LGPL 2.1); `mus2mid.c`, `midifile.c`/`.h` and `i_oplmusic.c` are Chocolate
  Doom (Simon Howard and others, with Id Software and Ben Ryves on mus2mid). Each
  carries a "Ported to Rach2 freestanding environment" note as the only local
  change to its header. They use `//` comments and a first-person register the
  in-house rules do not govern; the project-added guard comments (the underflow
  and zero-divisor guards in `i_oplmusic.c`) are accurate to the code.

## Cross-checks to settle against code later in set one

These are numeric or path claims in documents that read plausibly but can only
be judged against source read later in this same set. Each is to be confirmed
when its source file is reached, and the document fixed retroactively if wrong.

* `Readme.md`, `tools/Readme.md`, `tests/Readme.md`: "eighteen smoke checks"
  (against `tools/selfbuild_test.sh`, in set two tooling).
  RESOLVED: "29 utilities" is correct -- `utils/Makefile` builds 19 `UTILITIES`
  plus 10 `SBIN_PROGS` = 29 programs.
* `rach2/Readme.md`: RESOLVED -- "over 100 syscalls" holds. `syscall.h` defines
  ~111 `SYS_*` numbers and `syscall.c`'s dispatch table (`init_syscall`) wires
  well over 100 handlers.
  RESOLVED: the `cr3` field "offset 52" is correct -- in `process.h`, pid (0-3),
  state (4-7), regs (8-51, eleven uint32), cr3 (52-55).
  RESOLVED: "make subset-test boots six lines" is correct -- the recipe
  (`Makefile:1018`) loops over "", noaudio, nomouse, nonet, nodisk, and all
  four together, which is six.
* `rach2/sys/network/Readme.md`: "all 61 probes" (against `tools/runtests.py`).
* `rach2/Readme.md`: RESOLVED. `gdt.h` defines `GDT_ENTRY_COUNT` as 7 (null,
  kernel code/data, user code/data, TSS, TLS), with the TLS entry at index 6, so
  the GDT has 7 descriptors. rach2/Readme said "The GDT contains 6 segment
  descriptors" (its foundation-scoped table lists indices 0-5). Reworded to
  "The foundation GDT has 6 segment descriptors (the TLS subsystem adds a 7th at
  index 6...)", pointing at tls.Readme, so the count is no longer false in
  absolute terms.
* `krffs/Readme.md`: RESOLVED -- `make images` builds three (test, os-simple,
  os-real; `Makefile:213`), so the "all three" comment is correct; the metal
  image is a separate target.
* `rach2/sys/block/Readme.md`: RESOLVED. `diskinfo.h` defines the predecessor
  of `DISKINFO_WINDOW_MAX_SECTORS` and frames 8388607 as "the last sector
  /dev/disk0 can reach", so 8388607 is the last reachable sector index (its last
  byte, 8388607*512+511 = 4294967295 = 2^32-1, is exactly the largest a 32-bit
  offset can name). Two fixes: block/Readme framed 8388607 as a count ("the
  first 8388607 sectors") -- reworded to "the first 4 GiB, through sector
  8388607"; and both block/Readme and `diskinfo.h` said "the byte just past that
  sector is the largest one such an offset can name", which is off by one (the
  byte just past is 2^32, the first UNnameable) -- changed both to "that
  sector's last byte is the largest one such an offset can name".

## Vendored upstream, read but not edited

* `krffs/vendor/termbox2.h` and `krffs/vendor/termbox2_win.h`: the MIT-licensed
  termbox2 single-header library (nsf / Adam Saponara), vendored for krffs's
  `fsviz` tool. Confirmed upstream by the license header; the project's writing
  rules and the register rule do not apply to third-party code, and CLAUDE.md
  forbids churning the vendored trees, so these were read for provenance and
  left byte-for-byte as they are.
* `krffs/vendor/windows.sh`: the upstream termbox2 generator that sed-patches
  `termbox2.h` into the Windows `termbox2_win.h` (same origin as the two headers
  above). It emits a misspelled error string, "Unsupporrted (Windows)" (double
  `r`), but it is upstream and generates a vendored header, so it is left as is
  for the same reason.

## What was refuted

(none yet)

## Noticed in the code, left for milestone 28

* `rach2/.gdbinit`: line 1 is `file kernel`, which loads `./kernel`, but the
  build now emits the kernel at `build/rach2/kernel` (the build-directory
  unification noted in the milestone-22 record). Run from `rach2/` this loads no
  symbols. A config-path fix (point it at `../build/rach2/kernel`), so left for
  milestone 28.
* `rach2/sys/kio.c` (`vga_putc_raw`): the control-byte guard is `if (c < 31 ||
  c == 127)`, so byte 0x1F (31, the unit separator) is not caught and is drawn
  as a glyph, while the function's own comment says "any other control byte is
  shown as `CHAR_NON_PRINTABLE`". The bound reads as an off-by-one (`< 32` would
  match the comment). Harmless in practice (0x1F is almost never emitted to the
  console), a code fix, so left for milestone 28.
