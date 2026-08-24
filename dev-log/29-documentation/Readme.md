# Milestone 29: the documentation

This milestone reviewed every project-owned Markdown file against the current tree and rewrote the documents that were stale, inaccurate, or inconsistent with the writing policy. It changed documentation only.

## Scope

The inventory contained 123 files: 28 current-contract documents and 95 historical records. The current documents were checked against source, build rules, scripts, configurations, tests, and generated artifacts. Historical files under `dev-log/` received proofreading for prose and style, without changing their technical claims. The work ended with subsystem reviews and a complete repository review.

Official vendor documents, archives, extracted trees, legal text, code, scripts, manifests, configurations, and generated files were excluded. Six vendor integration Readmes and the BusyBox patch Readme are project documents and were included. The fixed-base diff contains 88 content documents and the milestone control files, with no non-Markdown paths.

## Corrected contracts

The review replaced general descriptions with the contracts implemented by the tree. Representative corrections include these:

* `make help` is a curated summary, not a list of every target. In-house outputs use the repository `build/` directory; vendor builds also use extracted and installed trees under `vendor/`.
* Image targets build images without booting QEMU. Run scripts and test targets boot them.
* Pipes provide byte streams and partial-transfer behavior, but do not provide atomic writes.
* The network documentation covers both RTL8139 and RTL8168 devices.
* The compilation database generator's current Homebrew failure is documented instead of presenting its output as valid JSON.
* The test inventory distinguishes 70 root programs and 35 musl programs from the default runner's 47 root and 16 musl programs. `make test` scores those 63 program exits and checks the guest clock.
* `/proc/disk` always exists and initially reports `state not-asked`; publication of `/dev/disk0` is conditional. The raw disk documents now distinguish device modes, kernel write policy, recovery authority, mount sources, and bounded device windows.
* The random subsystem documentation states its seed order, global stream, trust state, consumers, and untested branches without claiming address-space randomization.
* BusyBox documentation distinguishes its two builds, the 319 applets in the main build, the three set-id applets, and the features enabled by the current configurations.

The review also assigned each contract to one owning Readme, normalized component and artifact names, repaired relative links, and checked named targets and repeated numeric claims against their authorities.

## Prose

The edited documents now use the register required by `CLAUDE.md`: short, concrete statements with qualifications beside the claims they limit. The pass removed private model shorthand, dramatic fragments, false reversals, figurative verbs, duplicated development history, and claims whose scope could not be determined from their wording. All edited prose is ASCII.

Historical records retain their technical content even where later work made it obsolete. Rewriting those claims would change the record of what was known at the time. Upstream vendor documentation and legal text also remain unchanged.

## Defects recorded for later work

The documentation review found code and build defects outside this milestone's scope. `notes.md` retains the complete list. Material examples include invalid JSON from `make compile-commands`, ineffective region options in `defrag.krffs`, four global `/proc` files whose descriptor links resolve incorrectly, raw ICMP payloads from 1481 through 1500 bytes that report success but are dropped, and incomplete patch-stamp invalidation for musl and Dropbear. The notes also record stale source and test comments, incomplete host-test branches, and guest probes whose exit status does not expose every reported failure.

## Verification

The complete 88-file documentation diff was read in repository order against base `6e253221525273571b7ba12dcb5d1569bd746556`. The manifest comparison found exactly those content files plus the five milestone control files, and no non-Markdown path. The tracked Markdown inventory, relative links, referenced paths, named Make targets, banned-prose checks, and ASCII checks passed.

The completed tree passed `make full`, `make verify-style` with 13 checks, `make verify-host` with 36 host tests, and `make test` with all 63 probes and the clock comparison. Direct evidence also passed through `make bootpart-test`, `make tcc-test`, `make make-test`, `make selfbuild-test`, `make ssh-test`, and `make metal-test`. The self-build produced identical kernels with SHA-256 `fba1c906dc1ee900e7c24f758c8f8f2d088bedac0993a21ef6a112a47ddf1a4e`, checked the ELF image, passed all 63 probes, and booted through GRUB. `python3 tools/check_devlog.py --selftest` passed all 10 cases.
