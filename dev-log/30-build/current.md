# Milestone 30 checkpoint

This file holds only the resume state for the item in progress. Replace its contents instead of appending.

## Position

* Active phase: 8, Docker removal.
* Item in progress: not started.
* Exact next action: add `vendor/grub/` with the six pinned artifacts (`bios-boot.img`, `disk-core.img`, `disk-core-a.img`, `disk-core-b.img`, `eltorito.img`, `boot-hybrid.img`), their provenance manifest, the digest verifier, its `Readme.md`, the `CLAUDE.md` exception, and one converted consumer in one reviewed commit. The artifacts are generated once through the existing Docker procedure with every input recorded, then the disk builder, ISO builder, and core selector convert to native mtools, `xorriso`, and new C tools for the MBR layout, environment block, and embedding.

## Phase record

* Phases 1 to 6 complete; see e4d3187 through d622e0d.
* Phase 7 complete: deadline tool 3fa1ed8 (124-contract, group kill proved on a hanging descendant); pty driver 127b28d (capstone 29 s, wrong-password and window-size mutations both failed the suite); asciiscan cdf3770 (byte-parity with Perl on NUL and high-byte fixtures); sha256 7961db5 (FIPS vectors and tarball parity, vendor digest path and corrupt-tarball refusal proved); binfind 2ce62e7; make rules for both f4a1f78. Removed so far: gtimeout, Expect, Perl, host SHA-256 tools, strings; FUSE and pkg-config optional since phase 3.
* A collision to remember: sealed.sh already used `DEADLINE` for its second budget; the tool variable is `DEADLINE_TOOL` there.
