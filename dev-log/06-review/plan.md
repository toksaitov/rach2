# Rach2 review and test plan

This work order defines a later autonomous, multi-agent review of Rach2, with independent Codex verification. It covers code, comments, documentation, support files, and manual tests. The review seeks correctness, safety, ABI, concurrency, resource-leak, and edge-path defects in the kernel, network stack, userland, and host build tools. It also brings comments and Readmes to the current contract and terse K&R prose. Manual tests cover all three images (`test`, `os-simple`, and `os-real`) and the end-to-end SSH, Doom, audio, and network paths. A finding counts only after an independent Codex pass fails to refute it. A test result counts only after reproduction against the recorded Phase 0 baseline. Phases are grouped by area for parallel work, subject to the dependencies below. Findings use the form `file:line severity claim evidence`; the final phase triages them and fixes only those selected for correction in this run.

## Conventions for every phase

* Severity: `critical` (memory unsafety, use-after-free, kernel panic from user input, lost wakeup, double-free), `high` (wrong result, leak, ABI break reached by a binary), `medium` (edge-case miss, doc states a false current fact), `low` (voice, unused code, cosmetic).
* A CODE finding gives `file:line`, the contract it violates (cite the survey item or the Readme/CLAUDE.md clause), the concrete trigger, and the expected-vs-actual behavior.
* Independent verification: every nonzero-severity finding is handed to an independent Codex agent with the prompt "refute this: <finding>". The agent reads the same code independently and either confirms, downgrades, or rejects the finding. A finding counts only if Codex cannot refute it.
* A TEST result is a row in a pass/fail matrix: `component, command, expected token/outcome, observed, pass|fail`. A failing row is re-run once clean to rule out flake, then becomes a finding.
* All review reads use read + grep + cross-check against the owning Readme and CLAUDE.md; no finding rests on memory of the code.
* Parallelism: Phase 0 runs first and alone. The CODE phases (1-6) are mutually independent and run in parallel. COMMENTS+DOCS (7-9) and SUPPORT (10-11) run in parallel with the CODE phases (they touch the same files read-only). The MANUAL-TEST phases (12-20) run after Phase 0 and may overlap each other except where they share a single QEMU console. The final phase (21) runs last.
* Working directory: every command is given from the REPO ROOT. Build and test targets are `make <target>` (the root Makefile); run a guest with `rach2/run.sh <image> [audio]` or `rach2/run_from_iso.sh <image> [audio]`; the SSH key is `krffs/rach2_demo_ed25519`. The targets in `rach2/Makefile` are kernel-only (it has no `full`, `test`, or `images`); do not run root targets after `cd rach2`.
* Isolation and serialization: any phase that mutates the tree (edits sources, removes or truncates build artifacts, adds or deletes files) runs in a disposable git worktree, not the main checkout. QEMU boots are serialized unless each uses a unique host forward or `NET=off` (the default `rach2/run.sh` grabs host port 8080 via qemu_devices.sh), so two concurrent default boots collide.

---

## Phase 0, Baseline

Goal: establish a clean baseline so every later regression is attributable.

Scope: the whole tree from a clean checkout. `make distclean`, `make doctor`, `make full`, `make test`, `make verify-style`, `make verify-host`, and a boot of each of the three images plus the end-to-end SSH test.

Method, in order:
* Host-tool preflight: `make doctor` checks the cross toolchain and the style tools but NOT the peers the later phases need. Before the manual-test phases, confirm `qemu-system-i386`, `docker` (Phase 19 ISOs), `ssh` and `ssh-keygen` (Phase 15), and `socat` and `curl` (Phase 17) are present, and skip-with-a-note any test whose tool is missing.
* `make distclean && make doctor` -> every required tool PASS.
* `make full` from a clean tree -> rebuilds musl, BusyBox, Dropbear from the vendored tarballs (sha256 lines print verified), builds the kernel, stages build/images/{test,os-simple,os-real}.krffs, and prints the per-image "built ... (N of M bytes used, K free)". Record the byte counts and the kernel size.
* `make verify-style` -> record the exact step labels 0/8..8/8 and the PASS/advisory-count at each.
* `make verify-host` -> record the eleven `PASS: <name>` lines.
* `make test` -> record `runtests: N probes, 0 failures` and the probe count N.
* Boot each image: `cd rach2 && ./run.sh test` (debug shell + probes), `./run.sh os-simple` (ish init), `./run.sh os-real` (BusyBox /bin/sh init). Record what comes up.
* End-to-end SSH test: `NET_HOSTFWD="hostfwd=tcp::2222-:22" ./run.sh os-real`, then host `ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost` -> pubkey auth into BusyBox ash, rc 0.

What to look for: any build warning under -ansi -Werror (must be zero), any failed verification step, any image that does not boot to its expected init, or an end-to-end SSH failure. The build does not run QEMU; the scripts do (note run.sh defaults to -cpu max).

Verification: the baseline is the recorded artifact. A second agent independently re-runs `make verify-style` and `make verify-host` and must reproduce the same passing result. A divergence means the tree is not clean, and the run halts here.

Output: BASELINE.md recording every command, its exit, the verification step results, the three image byte counts, the kernel size, the probe count, and the SSH rc. All later phases compare their results with this baseline.

---

## Phase 1, CODE: kernel core (boot, MM, process/scheduler, syscalls, traps, low-level devices)

Goal: prove the boot, memory, scheduling, signal, and syscall machinery is correct, reentrancy-safe, and ABI-faithful.

Scope: rach2/kernel.c, loader.s, link.ld (the linker script: `_k_entry`, the 0x100000 load address, section alignment, `k_phys_mem_end`); sys/syscall.c (the ~7222-line dispatch, fork, execve, wait, signals, brk/mmap/munmap, read/write/writev, ioctl, nanosleep, set_thread_area, fd-table release, exit); sys/mm/{paging,heap,vm,frame,usercopy}.c; sys/proc/{process,sched,switch.s,wait_queue,elf,uimage,tls}.c; sys/tables/{gdt,tss,idt + handlers}; sys/isr.c + handlers; sys/{pic,pit,panic,module,serial,kio}.c. Phase 6 may also note link.ld as a build-support input.

Method: read each component against its survey `code_review_focus` items and against sys/Readme.md, sys/mm/Readme.md, sys/proc/signal.Readme.md, sys/proc/tls.Readme.md, auxv.h. Grep for the specific patterns the survey calls out. Cross-check the switch.s byte offsets against the live `struct process` in process.h, recomputed by hand.

What to look for (from the survey, each its own finding if confirmed):
* Timer-IRQ reentrancy in timer_handler: that schedule()->switch_context cannot be re-entered; that no callee (hda_tick, tcp_tick, tty_console_poll, proc_wake_sleepers) re-enables interrupts; that a process preempted while NOT in_syscall has a fully built trapframe.
* schedule() lost-wakeup/TLS window: pushf/cli ordering closes the publish-current_proc-then-swap gap; the first-time-scheduled path's skip of the IF restore is consistent across every first-entry path (setup_process_stack children, fork child via isr_return, preempt_proc_*).
* switch.s offsets PROC_REGS_ESP=48 / PROC_CR3=52 / PROC_KERNEL_STACK=56 / PROC_TLS_BASE=584: recompute against process.h after the v2 fd table; verify GDT_TLS descriptor byte offsets match gdt.c packing; the inline TLS rewrite under pushfl/cli cannot be split by an IRQ; the `_kernel_dir + 4096` CR3 math matches page_dir_t.
* _page_fault_handler ordering: COW tried before vm_resolve, a read-only-text write segfaults rather than being silently backed; in_user_range uses USER_ADDR_MIN/MAX consistent with usercopy.h and elf.c; a kernel-mode fault on a lazy user page resolves yet a true kernel bug panics.
* frame refcount: frame_ref caps at 0xFF, frame_unref pins a saturated frame, cow_fault sole-owner uses `_frames<=1`; every free path (free_user_pages, unmap_user_range, cow_fault) goes through frame_unref with no raw `_frames[]=0` on a shared frame; copy_user_pages ref/unref pairing exact.
* mem_reserve/alloc_user_page frame-bitmap interlock: no window aliases a page table with a user page; ensure_user_table runs before the free-frame scan in both alloc_user_page and the map_user_frame COW path; the k_main module-end push past modules (recently moved for the RAM-disk redesign) does not break the two-pass marking.
* execve teardown atomicity: every failure after free_user_pages reaches exec_fail_destroyed and never iret's into a freed space; the segment reset, CR3 self-reload TLB flush, sigacts reset (caught->DFL, IGN preserved), and console-ctty release/republish are all inside the destructive window correctly; close_cloexec_fds runs before free_user_pages.
* fork: release_fd_table on the child's pre-built stdio OFDs is no double-free; the OFD/socket refcount bumps in the copy loop are exact; the child kernel stack (copied isr_params eax=0 + isr_return) offsets are right and first-child deliver_signals is safe (sig_pending=0); copy_user_pages write-protects the parent in place and the parent is _curr_dir at fork time.
* Signal concurrency: every 64-bit sig_pending/sig_blocked RMW under sig_irq_save/restore; deliver_signals reading sig_pending without a lock is safe only because IF=0; signal_setup_frame's copy_to_user to a demand-paged stack is backed; signal_restore's eflags mask (0x00000cd5|0x202), cs/ss/ds reset, and safe_user_seg against a forged frame; the STOP loop (sti;hlt;cli) is lost-wakeup-safe.
* do_wait: a cr3==0 kernel-thread child is never wrongly reaped/freed; the up-front user_buffer_ok prefault of status/rusage so copy_to_user cannot fail after free_process; wait_pid_match INT_MIN; WUNTRACED/WCONTINUED consume-once.
* wq_wait_event discipline vs the four hand-rolled copies (do_wait, sig_wait, sys_nanosleep, deliver_signals STOP loop): each independently gets cli-across-the-pending-check-and-BLOCKED-set right; WQ_MAX_WAITERS overflow silently drops.
* User-pointer validation completeness: every syscall handing a raw user pointer to a lower layer goes through user_buffer_ok (range + vm_prefault_range), not just user_access_ok; audit read/write_fd/writev/ioctl and any deref after a range-only check.
* build_user_stack bounds: underflow-guarded subtractions, argc/envc <= UIMAGE_MAX_ARGS reject-not-truncate, 16-byte ESP alignment, esp>=base_vaddr, whole layout in one page; the auxv set matches auxv.h; cross-check tests/host/uimage_hosttest.c.
* elf_load_image hostile-ELF policy: overlap/ascending/disjoint PT_LOADs, window check, p_filesz<=p_memsz, wrap checks, e_phnum<=ELF_MAX_PHNUM, nsegs<=ELF_MAX_SEGS, read-only second pass, AT_PHDR derivation. Confirm which exec paths use elf_load() (weaker) vs elf_load_image() (hardened); flag any production path on the weaker one.
* C89/-ansi: no decl-after-statement, no //, no designated/named init, no long long division slipping in beyond the documented avoidance, no VLA, no anonymous struct; freestanding (no libgcc, k_memcpy/k_memset).
* ISR/IRQ trapframe: gs/fs/es/ds per-slot save/restore preserves %gs TLS; struct isr_params field order matches push order; ISR_WITH_ERROR_CODE vs dummy-0, the addl $8 cleanup, vector 14 carries the real error code.
* _syscall_handler in_syscall/IF discipline: set in_syscall=1 before sti, cli before clearing it; no syscall body breaks the assumption; in_syscall is volatile; a nested demand-page fault during a syscall does not clear it.

Verification: each confirmed finding is reproduced in two ways where possible: a code-path argument and, where a host test covers it (vm_hosttest, frame_hosttest, uimage_hosttest, tls_hosttest, wait_queue_hosttest), a failing or asserting test case under ASan/UBSan. The independent Codex refutation pass follows. The boot-time init_proc assertion and a separate hand calculation verify the switch.s offset claims.

Output: a kernel-core findings list (file:line, severity, claim, trigger, refutation status). Parallel-safe with phases 2-6.

---

## Phase 2, CODE: kernel I/O and filesystems (TTY/n_tty/pty, KRFFS, fd/OFD, synthfs, devfs, procfs, pipes, poll/select)

Goal: prove the line discipline, the KRFFS v2 allocator and hierarchy, the fd/OFD refcount model, the synthetic /dev and /proc, pipes, and the poll/select family are correct, leak-free, and ABI-faithful, especially with host-built images and repeated pty allocation during the end-to-end SSH test.

Scope: sys/tty/{tty,n_tty,pty}.c; sys/fs/{krffs,fd,synthfs,devfs,procfs}.c + file.h; sys/ipc/{pipe,pipe_ring}.c; sys/io/{poll,poll_select}.c; the syscall.c glue (fd_tty ~5229, tty_common_ioctl ~5250, fd_release_backing ~106, the synth_classify call sites, the leader-death ctty release sites ~293/~3042/~3269); the kernel.c boot wiring (init_krffs, pipe/pty pool init, tty_console_init, tty_console_poll).

Method: read against the survey `code_review_focus` and against sys/fs/Readme.md, synthfs.Readme.md, sys/io/Readme.md, sys/ipc/Readme.md, pty.h, fd.h. Grep the pinned ioctl numbers and the ktermios 36-byte/19-cc assertion.

What to look for:
* KRFFS allocator overflow/arithmetic: size_with_header and offset+size uint64 sums with no overflow check; krffs_resize_reserved_node grow-vs-relocate, remainder-absorb branches, previous_node_size back-link repair after every split/coalesce; krffs_write_node uint32 offset+size unclamped against the region.
* KRFFS image trust: init_krffs validates only the first node's magic+type; every do/while node walk (krffs_walk, find_child, readdir, count_files, rmdir scan) trusts node->size with no traversal bound or visited set; a corrupt/hostile image (zero/huge size, broken back-link, a cycle that never returns to fs->node) can hang or walk out of bounds. This is freshly exposed by mkimage.sh/stageimage producing images mounted verbatim.
* KRFFS id collision/orphaning: krffs_mix_id FNV collision between two dir ids or a file's DJB id and a dir's mix id mis-routes children; rename of a regular file updates name+parent_id with no same-parent collision check (file-over-dir / dir-over-file); rmdir emptiness scan and parent_id comparisons.
* TTY/pty concurrency under the single-CPU cli lock: tty_read re-test-under-cli loop; the m_buf producer race between ptm_write echo and pts_write output both via pty_slave_sink; console_pump_irq running n_tty_receive from the timer IRQ against the SAME ldisc ring a thread-context console_pump could be draining; no path mutates the discipline with IF=1.
* Timer-IRQ serial drain vs ISIG targeting: console_pump_irq delivers ISIG to fg_pgid read non-atomically against tty_set_session's two stores; verify every (sid,fg_pgid) publish/release on console_tty and each pty slave is cli-bracketed (tty_set_session, tty_make_sole_ctty); the waiter-gated branch cannot steal the kernel debug shell's serial bytes (sid!=0 else-branch).
* pty lifecycle refcounting: in_use cleared only when master_open==0 and slave_opens==0; pty_alloc's claim not locked against pty_session_slave/tty_make_sole_ctty/pty_release_leader pool scans; slave_opens controlled by the OFD refcount through fd_release_backing's refcount==1 check; dup/fork of a slave fd cannot double/under-count; a reused slot cannot alias a stale struct tty still referenced as someone's ctty.
* OFD pool leak on open-then-late-failure: every error path in pty_open_master/pty_open_slave/devfs_open/procfs_open/sys_pipe2 after a successful fd_alloc must fd_free (which unrefs the OFD), not just set in_use=0; OFD_POOL_SIZE is only 16*MAX_FDS; fd_ofd_unref guards refcount>0 but nothing detects double-unref.
* User-pointer validation consistency: ptm_read/write and pipe_read/write user_range_ok then per-chunk copy; devfs/procfs rely on copy_*_user alone; the (__force uintptr_t) casts sound on i386; a copy_to_user fault mid-drain returns the partial count (pipe_read documents it; verify ptm_read/pts_read parity).
* poll core do_poll: scan-then-park-on-all under one cli, sti;hlt;cli idle, and a defensive dequeue; POLL_MAX_FDS arrays vs nfds; negative-fd skip vs POLLNVAL-as-ready; the (int32)(deadline-ticks) wraparound; select_common's POLLNVAL->-EBADF cannot mask a blocking condition.
* i386 ABI fidelity: the 36-byte/19-cc ktermios prefix; pinned ioctls (TIOCGPTN 0x80045430, TIOCSPTLCK 0x40045431, FIONREAD 0x541B, the TIOC*PGRP/SCTTY/NOTTY set); struct kernel_pollfd layout; 32-bit poll_timespec/timeval; pselect6/ppoll sigmask accepted-but-ignored; select(82) single-pointer unpack.
* synth_classify edges: SYNTH_MAXC=5 cap treats a longer path as a miss (/proc/<pid>/fd/<n>/extra -> ENOENT not a false match); comp_uint overflow guard; SYNTH_PID_SELF sentinel; leading/trailing/doubled slashes and empty-root; the result honored identically at all ~15 call sites.
* EOF/eof_pos in n_tty: pending EOF at rq_tail, n_tty_read returns 0 only when rq_head==eof_pos, ISIG flush sets rq_head=rq_tail and eof=0; VEOF-then-more-input, ISIG-with-pending-EOF, the monotonic head/tail 32-bit wrap in rq_count.
* Buffer bounds: krffs_node_path (KRFFS_PATH_MAX_DEPTH=64, the pos+1+n+1>size check) and procfs_readlink_fd (256-byte path[], NOT-NUL-terminated contract honored by callers).
* procfs_gen_stat staleness as a correctness gap: emits pgrp/session as the pid and tty_nr=0/tpgid=-1 though job control is implemented; ps or job-control-aware tools could fail.
* C89/-ansi across the recently changed tty/n_tty/pty/procfs files.

Verification: the host tests n_tty_hosttest, pipe_ring_hosttest, poll_select_hosttest, and synthfs_hosttest directly cover the isolated logic. A confirmed discipline, ring, or classifier finding receives a failing case there under ASan/UBSan. KRFFS allocator and image-trust findings are reproduced with a malformed image passed through the host fsck.krffs and, where reachable, a guest mount. The Codex refutation pass follows. Parallel-safe with phases 1, 3-6.

Output: kernel-io-fs findings list.

---

## Phase 3, CODE: kernel device drivers and the freestanding lib (input, PCI, audio, display, speaker, kstring/kstd/kctypes, krandom)

Goal: prove the drivers, the SPSC audio rings across the syscall/IRQ boundary, the ISA-DMA physical-address assumptions, and the CSPRNG are correct, and find defects in the freestanding library helpers.

Scope: sys/{kbd,mouse,pci,speaker}.c; sys/audio/{audio,dma,sb16,hda,audio_debug}.c; sys/display/{vga_text,vga_gfx}.c + fb.h; sys/lib/{kstring,kstd,kctypes,krandom}.c; the integration seams in isr.c, kernel.c timer_handler, and syscall.c AUDIO_*/FB_*.

Method: read against the survey `code_review_focus`, krandom.Readme.md, and the top-level Readme driver lines (16-17, 62-67, 137-145, 218, 237, 352, 518). Grep for the (uint32_t)(uintptr_t) DMA-address casts, the varargs hack, the serial_printf diagnostics.

What to look for:
* SPSC ring correctness: hda.c ring_write (syscall) vs hda_service_stream (PIT tick AND HDA IRQ) and sb16 ring_write vs sb16_irq_handler; prove the two vectors cannot interleave given 0x8E gates (IF cleared, no nesting) and EOF-before-dispatch; every byte lands before the pointer is published; ring_available's two volatile reads cannot mislead.
* hda_service_stream LPIB->period arithmetic: lpib_mod, wrap detection, hw_period across a CBL wrap; no missed/double period transition; current_buf/next_buf pick the buffer the engine is NOT reading; lpib_tracking_valid reset avoids a stale first sample.
* ring_render_to_hw_buffer format math: U8->S16, S16 LE reassembly across the wrap, stereo mixdown, mono->stereo upmix, the 16.16 sample-hold advance; the signedness of (int16_t)(mixed-128)*256 for U8 stereo; avail_bytes underflow impossible.
* ISA DMA safety: dma_address_valid's address+length>0x1000000 wrap; the 16-bit addr=buffer>>1 and (length>>1)-1 even-base/even-length assumption, length==0 underflow; sb16.c passes the static dma_buffer as a physical address valid only under identity mapping with no link-time 64KB-boundary guarantee; a boundary cross makes dma_start return -1 and play silently fails with no surfaced error.
* HDA physical-address assumption for audio_buffer/bdl/corb/rirb/dma_posbuf; identity map vs HARD_MEMORY_LIMIT/map_mmio; BDL(1024)/buffer(128) alignment and CBL=BUFFER_SIZE*NUM_BUFFERS within controller constraints.
* The varargs ABI hack (char **args=(char**)&format; ++args) in hda_probe_log/hda_log: i386 cdecl assumption, no arg wider than a pointer; flag for -ansi/ABI review.
* krandom reentrancy/policy: file-static non-reentrant ChaCha state; the in_syscall protection argument cross-checked against kernel.c keeping timer IRQs enabled during syscalls; getrandom/dev-random draws cannot be interrupted mid-block; CPUID max-leaf checks prevent #UD; rdseed/rdrand CF retry budgets; krandom_is_trusted fail-closed policy.
* k_itoa edges: INT_MIN negation on the unsigned cast path well-defined; char digit signedness; force_sign reserves but never writes +; the two failure modes both return 0.
* kstring safety: k_strncpy/k_memcpy/k_memset no overlap/NULL guard; k_strreverse on '' computes (size_t)-1; k_toupper/k_tolower signedness; the kbd shift path only valid for letters (live console correctly uses EN_us_shift).
* PCI enumeration: pci_find_device_skip resume-past-skip across three header-type breakpoints cannot skip or double-visit; the byte-selection shifts; the read-modify-write 16-bit write.
* mouse handshake: ps2_wait spins with no time bound; init_mouse re-enables sti on every error path; packet sign-extension; packet_index desync recovery cannot wedge.
* Display no-locking: vga_text x/y globals and the 512-line history have only the console/tty writer, no IRQ writer; vga_gfx mode switches only from syscall/shell context, re-entry a clean no-op, font plane-2 save/restore balanced.
* Silent failure: sb16_start_auto_init ignores dsp_write returns; hda_send_verb returns 0 indistinguishably for busy, timeout, no response, or a valid 0, and enumeration trusts it; AUDIO_START/WRITE rc passed straight to userspace.
* C89/-ansi: no //, no mixed decl/code -ansi rejects, the fallthrough attribute, packed/aligned on hda_bdl_entry_t and DMA buffers, inline asm constraints compile under -m32 -mno-sse.
* Messages policy: kbd.c _keyboard_test per-key serial_printf (dead boot stub); SB16/HDA/mouse diagnostics casing/format against `component: detail` lowercase.

Verification: kstring.c and krandom.c have host tests. A confirmed defect receives a failing case in kstring_hosttest or krandom_hosttest. Also propose the missing k_itoa host cases: INT_MIN, a buffer that is too small, a radix outside the allowed range, and each format letter. The in-guest audio matrix in Phase 16 reproduces SPSC and DMA findings through "LPIB did not move", "DMA: Invalid address", or underrun jumps. The Codex refutation pass follows. Parallel-safe with phases 1-2, 4-6.

Output: kernel-drivers findings list.

---

## Phase 4, CODE: from-scratch TCP/IP network stack

Goal: prove the RFC-793 state machine, the SPSC TCP rings across the unlocked syscall reader vs the IRQ producer, the net_lock discipline, the RTL8139 driver, and the socketcall ABI are correct under bursty load and hostile input.

Scope: sys/network/{net.h, core/net_core.c, core/byteorder.h, driver/rtl8139.c, ethernet/ethernet.c, arp/arp.c, ip/{ip,icmp}.c, udp/udp.c, tcp/tcp.c, socket/socket.c}; the syscall.c socketcall multiplexer ~5558-6310 and the FD-layer socket seams (fd_release_backing socket_unref ~124, read dispatch ~1221, dup ~1404/~1455, fstat S_IFSOCK ~1996, fork ~3467); sys/proc/wait_queue.c; kernel.c tcp_tick + netping/netbad; sys/io/poll.c routing to socket_poll.

Method: read against the survey `code_review_focus` and sys/network/Readme.md, net.h's concurrency invariant, tcp.c/socket.c headers. Grep every shared TX buffer name and confirm each syscall-context touch is net_lock-bracketed.

What to look for:
* The TCP receive-ring SPSC boundary, the most important item: socket_tcp_recv/try_recv copy from recv_buf and advance recv_head with NO net_lock while IRQ-side socket_tcp_deliver advances recv_tail; prove the single-producer/single-consumer claim, and critically that an IRQ-side RST/tcp_giveup that tcb_free's (in_use=0) and a concurrent SYN that re-tcb_init's the slot cannot zero/reuse the buffer the unlocked reader is mid-copy on (the snapshot-tcb-once pattern guards NULL deref, not slot recycle); do_recv/do_recvfrom hold no lock across the blocking call.
* net_lock completeness: every syscall-context touch of eth_tx_frame, ip_tx_buf, tcp_tx_buf, tcp_cksum_buf, arp_reply/req_buf, icmp_reply/echo_buf, udp_tx_buf, the TCB pool, the socket pool, the ARP cache, the UDP port_table is bracketed; icmp_ping builds under net_lock then UNLOCKS before the sti;hlt poll while ping_active/replied/peer_ip (only volatile) are touched by IRQ-side icmp_rx; verify that hand-off is race-free; confirm tcp_rx/socket_tcp_deliver run only in IRQ context.
* tcp_rx edges: SYN_RCVD conn_tcb+conn_sock refcount accounting matches socket_tcp_accept and do_accept (no double-free on RST-in-SYN_RCVD or full-accept-queue unref); a retransmitted SYN does not create a second TCB (tcp_find exact-match-before-listen); options bytes between 20 and hdr_len skipped without breaking seq/ack math.
* TCP sequence/ring arithmetic: free-running uint32 masked to 4096; tcp_output usable = window - in_flight guard, tcp_send free_space, the (int32)(ack-una)>0 && (int32)(ack-nxt)<=0 window test cannot underflow or accept out-of-window; send_ack always points at still-buffered data, never ahead of send_tail; tcp_retransmit in_flight==0 guard, tcp_send_zwp send_tail==send_head guard.
* tcb_alloc reclaim: recycling the soonest-to-expire TIME_WAIT/FIN_WAIT_2 never steals a TCB with a live sock; tcp_close/giveup actually NULL sock->tcb and tcb->sock in those states; no orphaned still-open fd.
* RTL8139: rtl8139_rx reads status/pkt_len before bounds-checking rx_ptr (the 2KB overhang covers WRAP=1 max-frame-at-ring-end); pkt_len bounds check, pkt_len-4 cannot underflow; RTL_RX_EARLY 0xFFF0 vs a valid length; rtl8139_tx drop bound and the cli/popfl bracket; the QEMU synchronous-TX no-OWN-poll assumption documented and QEMU-only; the shared-line irq service-loop bound.
* ICMP: icmp_rx copies <=1500 into icmp_reply_buf[1500] and recomputes over len (len already clamped, came validated from ip_rx); the single global in-flight ping state colliding between debug-shell netping and a syscall ping.
* IP: ip_rx validation order rejects total_len<hdr_len and total_len>len before payload; ip_tx ARP-miss silent drop requires every caller to retry (icmp_ping, tcp SYN via tcp_tick, the test retry loops); ip_id_counter wrap benign (DF).
* socketcall user-pointer validation: user_buffer_ok on recv/send/sendto/recvfrom buffers; sockaddr via copy_from_user; copy_sockaddr_to_user's caller_len read and the (caller_len<16) truncation and unconditional len=16 writeback; addrlen<SOCKADDR_MIN rejection; do_accept's bad-sa_ptr path releases the socket ref exactly once.
* sin_family byte order: family host-order, sin_port/sin_addr big-endian (the Dropbear-discovered fact) consistent across getsockname/getpeername/accept/recvfrom; SC_BE16/SC_BE32 read the caller correctly.
* socketcall arg-count table: nargs per SC_* matches each do_* arity; SC_PING=0x1000 cannot alias a BSD sub-call and the renumber matches tests/pingtest.c and utils/ping.c.
* socket refcount: do_socket alloc-then-fd_alloc-fail unref; dup/dup2/fork ref; fd_release_backing drops one per descriptor (not OFD-gated); freed exactly at last close; socket_unref at 0 calls udp_unbind or tcp_close exactly once.
* C89/-ansi: every block-scoped mid-function declaration opens a new block (tcp_rx ESTABLISHED, the RST struct-socket case, tcp_tick new_ss, do_accept rc); no //, no designated init, the (void) casts.
* ephemeral port wraps cannot return 0 or collide; auto-bind in do_sendto records local_port so close unbinds.
* socket_poll: ESTABLISHED sets POLLOUT unconditionally (no send-buffer-full backpressure); listener-readable and DGRAM-always-writable rules; *wq set so poll can sleep without a lost wakeup.

Verification: the host has no network boundary test, so confirmed concurrency findings require a code-path argument and in-guest reproduction under load (Phase 17: httpd plus parallel curl requests, and the simultaneous audio and network run). Negative user-pointer findings are reproduced by the udptest Test-5-style kernel-pointer, overflow, and unmapped-sockaddr probes expecting -EFAULT, never a panic. The Codex refutation pass follows. Parallel-safe with phases 1-3, 5-6.

Output: network findings list.

---

## Phase 5, CODE: in-house userland (utils/, ish/, tests/ probes+musl+host, ports/doom/)

Goal: prove the userland parsers (especially the untrusted-input DNS resolver and the hand-rolled httpd), the shell, the freestanding probes, and the Doom from-scratch libc are memory-safe and correct.

Scope: utils/resolv.c + netdb.h; utils/{ping,nc,httpd}.c; the small coreutils (rm.c the only recursive one); utils/Makefile(.linux); ish/{ish,ish_cstring_utilities,ish_shell_utilities,ish_syscalls*,crt0,ish_common}.c/.h; the tests/ probes; tests/dnstest.c; tests/musl/*; tests/host/*; ports/doom/{crt0,doomgeneric_rach2,i_pcsound,i_oplmusic,opl3,midifile,mus2mid}.c + include/; the three manifests.

Method: read against the survey `code_review_focus`, ish/Readme.md, utils/Readme.md, ports/doom/Readme.md, tests/Readme.md, and tests/host/Readme.md. Note the check scope: cppcheck --std=c89 covers only rach2/sys + *probe.c; ish/utils/ports-doom are NOT cppchecked; ports/doom is outside textcheck/clang-format entirely.

What to look for:
* resolv.c untrusted-input parsing: dns_parse_a/dns_skip_name bounds (pos+10>len, pos+rdlen>len, the 0xC0 compression-pointer branch returning pos+2, the 0x40 reserved-label rejection); the id comparison ((resp[0]<<8)|resp[1]) != (int)id for ids >= 0x8000; dns_query's recvfrom into resp[512] cannot under/overflow; the id/QR pre-check matches dns_parse_a.
* resolv.c fixed-buffer tokenizers: hosts_find/read_nameserver tok[64]/key[16]/val[64] with t<63/t<15 guards, no off-by-one on the NUL, the no-newline-at-EOF path; read_file caps correctly.
* ish redirection fd checks: the >0 / >1 dup2 condition silently skips a legitimate fd 0/1 and a failed open(-1), running the command without the redirection rather than erroring; decide the intended semantics.
* ish builtins: cd/exit argument_count semantics; cd with no arg uses home, extra args ignored; exit parsing yields 0 on garbage with no error.
* ish freestanding stat scratch: the kernel stat result fits the 256-byte array; syscall 106 with two args matches the kernel ABI.
* ish C-standard: C99 for-loop decls built -ffreestanding with no -ansi (GNU C); decide whether the userland is intentionally not C89 and whether cppcheck should cover it.
* ish/crt0.c _start asm reading argc/argv off EBP relies on the prologue with no other stack adjustment and a frame pointer; fragile against -fomit-frame-pointer; envp = argv+argc+1.
* httpd hand-rolled buffer arithmetic: put_s/put_u/put_ip/put_uptime/put_bytes write through int*pos with NO bounds check against body[4096]/resp[6144]/reqbuf[2048]; the worst case (PEER_RING=6 full rows + max-length escaped agent/host/reqline) vs the buffers; the cap-then-escape interaction (copy_request/header_value cap into REQLINE/AGENT/HOST_MAX before put_esc).
* httpd peers ring index math: record_peer, the post-send patch, build_body idx all in range and the live row lines up.
* rm.c recursive removal: full[MAX_PATH=256] children past MAX_PATH silently left partial returning -1; the . / .. skip; opendir/closedir no leak on the error path.
* doom crt0.c 64-bit division: __divdi3/__udivdi3 normalization and long-division loops; b>ua-from-start, the b&(1<<63) early-stop, INT64_MIN, __moddi3/__umoddi3 (a-q*b); a wrong quotient corrupts timing/fixed-point silently.
* doom crt0.c allocator: bump never frees; realloc memcpy(new,ptr,size) with size==new reads past the old object on a grow; zone init survives NULL; calloc nmemb*size integer-overflow before malloc.
* doom crt0.c printf/vsnprintf: the 1024-byte outbuf not bounds-rechecked on every path; the unknown-specifier int-consume va_list assumption; no format Doom uses overflows or desyncs.
* doom i_pcsound.c: mix_buffer[315]/opl_buffer index ranges, the opl_idx_start/end math, the MAX_CATCHUP clamp, the AUDIO_WRITE off+=w loop termination; PCSoundStartSound trusts a WAD lump num_samples with no check against lumplen-8 (over-read of a cached lump).
* doom doomgeneric_rach2.c: key_queue[32] full-drop, the scancode table, the mouse_packet layout matching kernel mouse.h, DG_DrawFrame writes exactly W*H, a partial mouse read handled.
* probes i386 ABI: the naked _start pushing %esp; the syscall-number/register bindings; auxvprobe's AT_* (PAGESZ 4096, PHENT 32, CLKTCK 100, AT_SECURE 0, AT_RANDOM) vs the uimage builder; errnoprobe's -ENOSYS/-ENOENT/-EBADF/-EFAULT contract.
* host tests assert by return code, avoid host stdio/string, include kernel headers; the host-deps: lines name every linked kernel source and stay in sync.
* probe C89-cleanliness (they ARE cppchecked); fd hygiene across utils (nc's read-loop break-without-close, httpd's continue branches).

Verification: a confirmed resolv.c parsing finding is reproduced by compiling resolv.c with a small native driver under ASan/UBSan and fuzzing dns_parse_a/dns_skip_name with truncated or adversarial compression-pointer buffers. This parser has no sanitizer host test; propose adding one. Reproduce httpd overflow findings with curl, a maximum-length User-Agent, and a <script> path against the running server in Phase 17. Doom crt0 division and allocation findings receive a host driver under UBSan. The Codex refutation pass follows. Parallel-safe with phases 1-4, 6.

Output: a userland findings list and a recommendation on the in-house-C89-vs-GNU-C policy difference and the ports/doom exemption from the checks.

---

## Phase 6, CODE: build system and host tools

Goal: prove the make graph, the manifest-driven KRFFS builder and host tools, the vendored stamp build, and the verification and run scripts are correct, atomic, and parallel-safe, and that the duplicated host/kernel KRFFS hash stays in lockstep.

Scope: root Makefile and mk/{toolchain,build,deps,rules}.mk; rach2/Makefile, tests/Makefile, utils/Makefile(.linux), ish/Makefile(.linux), krffs/Makefile; krffs/stageimage.krffs.c, the three manifests, mkimage.sh, gen_demo_key.sh, create_file_system.krffs.sh; the shared krffs core (allocator/node/file_system/platform/utilities); the host tools (mkfs/fsck/defrag/fsviz/edfs/krffs+fuse); tools/{check,check_sources,hosttest,doctor,gen_compile_commands,runtests.py,harness.py,mkiso,textcheck}.sh + cppcheck.suppress + .clangd; the vendored build scripts + the two muslgcc wrappers; the run scripts.

Method: read against the survey `code_review_focus`. Diff stageimage's krffs_mix_id/djb against the kernel's by hand. Grep the make DAG edges, the stamp rules, the readelf address pins.

What to look for:
* stageimage read_whole_file: the n>=0 guard, ftell fitting long, the buf[n]='\0'.
* stage_node: the SIZE_MAX branch for each host width; data_size (uint64) not truncated when passed to the allocator; krffs_allocate_reserved_node reserves sizeof(node)+data_size.
* krffs_mix_id/djb byte-identical between stageimage and kernel: the most serious coupling; no shared header, duplicated source; any drift desyncs host-staged ids from in-guest ids.
* is_valid_guest_path/is_synthetic_path: the /dev//proc rejection and the doubled-slash/dot-component rejection; resolve_dir does not re-check synthetic-ness on parents (trace that a manifest cannot create /dev as an implicit parent).
* Fixed on-stack buffers: resolve_dir buf[256], split_path bufs[256], stage_exec_dir hostpath[1024]/guestpath[512]; a path COMPONENT near 255 truncated by krffs_set_reserved_node_name with no error -> silent id collision; the whole path is bounded <256 but a single component is not bounded against the 256 name buffer independently.
* unescape_text + the text directive: strtok_r rest-of-line, leading-whitespace strip, interior preserved, the lone-trailing-backslash r[1] guard.
* mkimage.sh atomicity: TMP=.new + trap + mv-on-success; an empty/non-numeric SIZE under set -e.
* stage_file rejects a duplicate while stage_dir is find-or-create; the explicit-dir-after-files mode-update ordering holds.
* defrag system("./fsck.krffs <path>"): the ./ prefix and CWD dependence, the command_buffer bounds, and the UNSANITIZED path interpolated into a shell command (space/metachar injection); the "././fsck.krffs" fallback.
* fsck.krffs unconditional success message: trace that the walk-loop break on a structural fault sets a negative exit_status but then falls through to the unconditional "no errors found" puts() (suspected bug: the message contradicts the failure on a corrupt image even though the exit code is correct).
* The krffs node-walk family trusts node->size/previous_node_size with no in-walk bounds check; a corrupt size loops forever; fsck is the only validator.
* C89/-ansi: stageimage and host tools build gnu99 (C99-isms fine); confirm nothing in the SHARED krffs core breaks -ansi if ever pulled into the kernel (it is not).
* gen_compile_commands.sh / check_sources.sh parse `make print-c-src` via tr/sed/sort/grep; a path with a space or glob char breaks the coverage assertion; the mv-temp-on-success guard.
* tools/check.sh /tmp/rach2_verify predictable path (no mktemp -d) TOCTOU/symlink risk on a multi-user box; the // grep `-vE 'https?://|://'` false-pass; the readelf ABI hard-coded LOAD@0x08048000 / entry 0x804xxxx brittle contract.
* runtests.py/harness.py: the LAST "Process exited with status N" before the "returning to kernel shell" marker; a no-marker hang reported as fail; the PROBES list is a hand-maintained subset of tests/Makefile PROGS with no guard that they agree.
* mkiso.sh: IMAGE allowlist before the destructive rm -rf; a repo path with a space breaks -v/-w; a swallowed apt-get/network failure surfacing as a grub-mkrescue failure.
* Make -j correctness: every inter-component edge is in the root Makefile DAG (rach2: krffs; tests/utils: rach2 + LIBC_STAMP; image targets list every prereq; the tests->libc-stamp edge the last commit fixed); a new edge missing only fails intermittently under -j.
* Stamp rules: rm-artifact-then-build-then-test-s-then-touch, .DELETE_ON_ERROR; check-busybox/check-dropbearmulti catch a missing/zero-byte artifact behind a present stamp.

Verification: a byte comparison of the two functions and a round-trip test verify the host/kernel hash coupling. Stage an image, mount and walk it in the guest, and confirm that a deep path resolves. Reproduce the fsck success-message bug by corrupting a node's magic with edfs and observing the contradictory message with the correct exit code. Stress the Make graph with repeated `make -j` runs from a clean tree; Phase 0 already ran one, and this adds parallel-job runs. Support the injection and TOCTOU findings with the code and a crafted input. The Codex refutation pass follows. Parallel-safe with phases 1-5.

Output: build-host-tools findings list. Propose adding an automated cross-check of the duplicated krffs hash and a runtests-vs-manifest probe-list guard.

---

## Phase 7, COMMENTS: Doxygen headers and in-body comments across the kernel

Goal: bring every kernel comment to the CLAUDE.md standard (Doxygen /** header carries the contract and the non-obvious why; in-body only a one-line hack or ABI fact; terse K&R voice; no diary, no restating).

Scope: all kernel .c/.h headers and in-body comments, with priority on the heavily-commented concurrency code flagged across surveys 1-3: sched.c schedule(), wait_queue.c wq_wait_event, the signal functions, terminate_current_process, _syscall_handler, the keyboard_handler CR3 block, the deliver_signals STOP loop, the do_wait sleep block; switch.s PROC_TLS_BASE=584 and the "(456 -> 584)" note; process.h "APPEND-ONLY below this line"; auxv.h and tls.c populated-set claims; panic.c/pit.c (no headers; _pit_test dead stub); the tty/n_tty/pty/procfs rationale comments touched by the divider-removal and over-80-wrap commits; hda.c's dense in-body ABI comments; the cli-lock idiom comment duplicated in tty.c/pty.c/poll.c and net.h.

Method: read each header against the code it describes; grep for multi-line in-body explanations that belong in the header; grep for "Phase N"/"Part N" diary tags and dashed banners; re-read the important rationale comments for accuracy after the wraps (the prior phase already had to restore three dropped words).

What to look for: a header whose claim no longer matches the code; an in-body comment that is a reasoning trace rather than a one-line fact; a diary tag; a banner divider; a duplicated idiom comment that has drifted between its copies; panic.c/pit.c missing /** headers and the _pit_test dead-stub status; k_vprintf's documented conversions vs the switch; auxv.h/tls.c drift vs build_user_stack and tls_resolve_base.

Verification: each comment finding pairs the comment text with the code line it misdescribes (file:line for both). The independent pass re-reads both and confirms the mismatch or voice violation. A person judges voice against the named models (K&R, Stevens, and Lions) because no checker can. Parallel-safe with the CODE phases (read-only on the same files).

Output: a comments findings list, grouped by file, each with the proposed terse correction.

---

## Phase 8, COMMENTS: userland, drivers, network, and host-tool comments

Goal: same standard as Phase 7, for the non-kernel-core trees and the network/driver in-body ABI comments.

Scope: utils/resolv.c + netdb.h headers (dns_parse_a/skip_name bounds contract, the "0.0.0.0 succeeds" note, the "6s budget / 30x4x50ms" math); the tcp.c/socket.c/icmp.c/rtl8139.c/udp.c/byteorder.h headers and in-body ABI-fact comments (the go-back-N vs single-oldest claim, tcp_isn caveat, the RTL ABI one-liners, the udp_rx leak-guard rationale); the audio in-body comments (hda.c Conexant/EAPD/amp-verb magic as ABI facts not narration); kbd.c/kbd.h (the misleading _keyboard_test name and why two handlers exist); the dead init_pci/init_keyboard and the "Bus scan complete" message; vga_gfx.h dual VGA_MODE13H_*/VGA_GFX_* macros; stageimage's apply_line directive-table docstring and the total_free_space raw-vs-payload note; ports/doom local platform files (decide exempt vs in-scope: they use //, banners, TODO).

Method: read each header/comment against the code and the survey `doc_comment_focus`. Grep ports/doom for //, banner blocks, TODO.

What to look for: the same classes as Phase 7 plus the resolv.c header's recently-corrected musl-not-int-0x80 story staying accurate; the stageimage docstring overstating file idempotency (stage_file rejects duplicates, only dirs are find-or-create); the ports/doom voice violations and the policy decision on whether the owned fork's platform files are in scope.

Verification: pair each comment with its code line, obtain an independent re-read, and judge voice against the models. Record the ports/doom scope decision as an explicit recommendation rather than applying it without notice.

Output: a comments findings list for these trees plus the ports/doom scope recommendation. Parallel-safe with the CODE phases.

---

## Phase 9, DOCS: every Readme and the dev-log, with the three-image redesign drift as the focus

Goal: make every Readme state the current contract in the required voice; find drift from the musl port and the recent three-image redesign; leave the historical dev-log unchanged except for one Status line.

Scope: CLAUDE.md, AGENTS.md, the root Readme.md; rach2/Readme.md (the most outdated file, 1348 lines); the per-subsystem Readmes (fs, synthfs, mm, io, ipc, network, signal, tls, krandom); krffs/Readme.md; tests/Readme.md, tests/host/Readme.md; ish/Readme.md, utils/Readme.md, mk/Readme.md; the three vendor Readmes; ports/doom/Readme.md; dev-log/Readme.md and the five milestone dirs.

Method: read each Readme against the code it owns and against the other Readmes for cross-document consistency. For every command literal cited in a Readme, confirm an existing make target or script (`grep -nE '^<target>:' Makefile` or `make -n <target>`).

What to look for (the confirmed drift the surveys enumerate):
* rach2/Readme.md: "20 syscalls" (now ~110+), "32 MB limit / identity map only" (now demand-paged COW), KRFFS "flat file system, no directory hierarchy" and "chdir only / . .." and "getcwd always /" and "stat st_ino=0" (now v2 hierarchical + *at), "Not Supported: Threads, signals" (signals/sessions/job control/TLS all exist), the Limitations table, the Source Structure tree (omits sys/proc/{wait_queue,uimage,tls}, sys/mm/{vm,frame,usercopy}, sys/tty, sys/io, sys/ipc, sys/network, sys/fs/{devfs,synthfs,procfs,fd}, sys/lib/{krandom,...}), "Boots into ISH shell as init" (os-real boots /bin/sh via /sbin/init->/bin/sh->ish), the debug-shell table omitting netping/netbad, harness_exit/RACH2_HARNESS undocumented. Decide: scoped rewrite to the current contract, or an explicit, accurate "foundation snapshot" framing that does not state superseded facts as current.
* sys/mm/Readme.md: mostly current; verify VMA "16-entry array" vs VMA_MAX, the testing token list, and the COW paragraph reconciled with cow_fault/copy_user_pages reality ("fork copies the parent's map verbatim" vs COW page-sharing).
* sys/fs/Readme.md: the lightly-touched (4-line) lines about "the interactive host FUSE mount stays flat" and stageimage.krffs/in-guest-mkdir vs the current mkimage.sh + manifests model; the stageimage.krffs /etc reference; no removed artifact (build_ramdisk.sh, file_system.krffs, stagetree) implied; the directory-fd-not-pinned-against-rmdir caveat re-validated.
* synthfs.Readme.md: the device table vs dev_table[], the ttyname(0) walkthrough vs devfs_open + procfs_readlink_fd, the "honest simplifications" still literally true.
* sys/network/Readme.md: the Testing example image name (the named test binaries live in test.krffs, not os-simple); the raw qemu line vs run.sh/qemu_devices.sh (NET=on, -cpu max default); the "flat KRFFS root" /hosts+/resolv.conf claim now image-specific (os-real moved them under /etc); the resolv.c name-resolution prose after the musl conversion.
* procfs accuracy (cross-ref Phase 2): the gen_stat field comments stale vs implemented job control.
* krffs/Readme.md: Disk Layout v1/v2 accuracy (the per-field "id = DJB hash" vs the v2 parent_id/mix_id scheme); the stageimage overstated-idempotency docstring; the FUSE-mount-is-flat claim consistent with sys/fs/Readme.md; no comment points a student at the FUSE mount for image creation.
* tests/Readme.md, tests/host/Readme.md: the probe roster, each success token (the parenthesized forms, a test contract that must not drift), the dnstest "links the ported musl" claim, the host-test count (eleven) and host-deps, the echo-server expectation.
* ish/Readme.md: STALE; "links no C library / int-0x80-only" vs the ISH_USE_STDLIB path and mk/ refactor; the "exec ish" usage wrong for the /ish-init os-simple boot; the Required Syscalls and Structure lists predate mk/.
* utils/Readme.md: the vendor/musl repoint, the os-simple staging claims, the "run build_libc.sh once first", the Linux build/utils/linux path, the /hosts+/resolv.conf flat-root claim (true per os-simple.manifest), the Required Syscalls list (utimensat, getdents64).
* ports/doom/Readme.md: "Heap: 4MB" (crt0.c is 8MB), the understated total, the audio prose numbers vs i_pcsound.c, the Source Structure tree, the "~390KB executable" figures re-measured.
* vendor Readmes: the patches/NNNN-reason.patch escape-hatch claim is STALE (no build_*.sh applies patches); the musl Readme startup-contract auxv/AT/version (1.2.5) accuracy.
* CLAUDE.md: "no manual build_*.sh step" vs the vendor/utils/tests Readmes telling readers to run them; reconcile to the same contract everywhere.
* dev-log: historical record; .build/, file_system.krffs, build_busybox.sh, ramdisk, and "Phase N" are correct in that context and must NOT be changed. The only allowed edit is the "candidate final draft" Status line in dev-log/02-networking-and-userland/plan.md, for consistency with 04's "complete". Confirm that dev-log/05-images/Readme.md and the index entry match what shipped.

Verification: each documentation finding pairs the claim with the contradictory code, Makefile, or manifest fact (file:line on both sides). Boot evidence confirms the important claims. Phase 14 (`os-simple` with `mkdir d && cd d && pwd`) directly refutes "no directory hierarchy / chdir only"; Phase 0 init selection refutes "ISH-only init"; and the make-target audit refutes any cited but missing target. The independent pass re-reads the document and code and confirms the contradiction. The dev-log boundary is stated explicitly.

Output: a docs findings list with, for the in-scope-and-now-false claims, a proposed correction or a scoping decision (especially the rach2/Readme.md rewrite-vs-snapshot call). Parallel-safe with the CODE phases.

---

## Phase 10, SUPPORT: the verification checks and configs

Goal: prove the verification checks are correct and complete, and cover the demonstrated omission in which they do not inspect their own config prose.

Scope: tools/textcheck.sh, tools/check.sh, tools/check_sources.sh, tools/hosttest.sh, tools/cppcheck.suppress, .clangd, .clang-format, the .gitignore set; cross-ref the Phase 6 verification-script findings.

Method: read each check script's scope and file sets; run the checks and negative tests; grep the configs for the same violations those checks enforce.

What to look for:
* textcheck.sh scope: the in-house roots and the four prune paths; its file sets exclude .clangd, .clang-format, and tools/cppcheck.suppress, while .clangd and cppcheck.suppress currently violate the standard (spaced em dash, /* --- */ banners, and a "Phase 1" diary tag). Demonstrate: `grep -n ' -- ' .clangd tools/cppcheck.suppress` and `grep -n '\-\-\- ' tools/cppcheck.suppress` show hits, then `sh tools/textcheck.sh 2>&1 | grep -E 'clangd|suppress'` shows no output. Decide whether the checks should cover them.
* textcheck.sh awk Markdown block: the fence toggle skips 4-space but not tab-indented code; an inline-code ' -- ' on a prose line still flagged; a table row or setext line cannot misfire; the single-line string-strip in the em-dash-in-C and "Phase|Part" checks can slip a continued-string or char-literal ' -- '.
* check.sh stale self-documentation: the header still says clang-format is hard on "libc/" (pre-rename; newcode_ch globs vendor/musl); the suppress file's kernel.c "Phase 1 segment loader" note (verify it is still true and note it is itself a diary tag in a config); the inhouse_ch/newcode_ch prune sets vs the post-vendor/-move tree; the readelf LOAD@0x08048000 / entry 0x804 pins vs the post-vendor build.
* cppcheck.suppress correctness: every bare-id and scoped id:*path suppression still corresponds to a justified finding; a scoped path that no longer exists silently does nothing; a bare suppression can mask a new defect.
* check.sh /tmp/rach2_verify predictable-path TOCTOU (cross-ref Phase 6).
* Portability: textcheck.sh and check.sh perl/sed/awk under the macOS default toolchain; the mktemp/trap EXIT INT TERM cleanup.
* The .gitignore set covers the extracted vendor trees and build output and does not ignore a tracked source.

Verification: the grep and textcheck pair above demonstrates the omission with a reproducible result. The stale-comment findings pair each comment with the renamed path. The negative tests inject a Cyrillic byte, a ' -- ', and a "Phase 7" tag into tracked .c and .md files, re-run textcheck, and require a nonzero exit with the correct tag. This validates the checks as commit a426d68 did. An independent pass confirms the result. Parallel-safe with the CODE phases.

Output: a support-check findings list and an explicit decision on extending textcheck to the configs and fixing the three current config violations.

---

## Phase 11, SUPPORT: the build system, scripts, manifests, and run wiring (manual exercise)

Goal: prove the make graph, stamps, incrementality, the manifest builder, the ISOs, and the run-script matrix behave as documented, by running them.

Scope: the root Makefile and mk/, the component Makefiles, the stamp rules, the manifests, mkimage.sh, the run scripts (run.sh, run_from_iso.sh, debug.sh, grub_install.sh, qemu_devices.sh), rach2/.gdbinit (debug wiring; check it is valid and current; it currently contains a bare `wh` that looks like a stale or ambiguous GDB command), tools/mkiso.sh, the krffs host tools.

Method: the concrete runnable matrix from the survey:
* Stamp correctness: `touch vendor/busybox/rach2.config && make busybox` rebuilds; `make busybox` again does nothing; `rm build/vendor/busybox/busybox` then `make os-real-image` -> check-busybox FAILs with the documented message; `rm vendor/busybox/.stamp && make busybox` recovers. Then `: > build/vendor/dropbear/dropbearmulti` and `make check-dropbearmulti` -> FAIL on the zero-byte test -s.
* Header-dep incrementality: `make`, `touch rach2/sys/fs/krffs.h`, `make -C rach2` rebuilds ONLY the krffs.h includers (watch the compile lines), not all 59; `rm` a header in a .d and confirm -MP does not wedge.
* check-sources: add a dummy rach2/sys/zzz_unlisted.c, `make check-sources` -> FAIL listing it; remove it; transiently delete an existing source -> the converse error; restore.
* Manifest builder happy + negatives (run directly in build/krffs): build a fresh image with each directive; then negatives in separate manifests: a /dev/foo path -> synthetic-namespace error; a //etc -> malformed; a duplicate file path -> staged-more-than-once; a `dir 0700 /a/b` after a file landed at /a/b -> descends-through-non-directory; a tiny image so a file does not fit -> not-enough-space + non-zero exit.
* mkimage.sh atomicity: a manifest that fails midway (missing required host file) against an existing image -> the old image intact, exit non-zero.
* fsck/fsviz/edfs walkthrough: mkfs + fsck (clean) + fsviz (-t ribbon matching the file count); the edfs corrupt-magic + fsck loop and the SUSPECTED BUG watch: confirm fsck does NOT print "completed successfully ... no errors found" after a fault (the unconditional success puts()).
* ISO path (needs Docker): `make os-simple-iso` then `./run_from_iso.sh os-simple`; negatives: `sh tools/mkiso.sh bogus` -> usage, no rm -rf; Docker absent -> "docker is required".
* Toolchain knobs: `make CROSS_CC=$(command -v i686-elf-gcc)`; `make clean` with no cross GCC must succeed (the toolchain.mk error check excludes clean).
* Run-script matrix smoke: `./run.sh os-real {sb16,hda,speaker}` wire the right -device; `NET=off ./run.sh os-simple` no NIC; `CPU=qemu64 ./run.sh os-real` then Dropbear keygen fails closed.

What to look for: any stamp or check that does not catch a missing or zero-length artifact; any unnecessary rebuild; any non-atomic image write; the contradictory fsck success message; or any run-script device-wiring or environment mismatch with qemu_devices.sh.

Verification: each row is a pass/fail entry reproduced once. The fsck bug, if present, is the Phase 6 finding confirmed end to end. Parallel-safe with other test phases except where it shares the single QEMU console (serialize the boot rows).

Output: a build/scripts test matrix.

---

## Phase 12, MANUAL TEST: boot all three images and the kernel-core self-tests + host seams

Goal: confirm that each image boots to its expected environment and that the kernel-core self-tests and host tests pass.

Scope: the three images; the 10 in-kernel self-tests; `make verify-host`.

Method (all commands from the repo root):
* `make full && rach2/run.sh` -> the 10 self-tests print [PASS] (memory, strings, timer, paging, syscall, context switch, round-robin A>0/B>0, memory isolation, KRFFS write, ELF parse) and the `>` prompt appears (no image argument boots the kernel into the debug shell).
* `rach2/run.sh os-simple` -> ish; `rach2/run.sh os-real` -> BusyBox /bin/sh; `make test` then `rach2/run.sh test` -> the probe pass. Confirms boot_init_process order (/sbin/init, /bin/sh, ish).
* `make verify-host` -> all eleven host tests pass under ASan/UBSan (vm/frame/uimage/tls/wait_queue cover kernel-core without booting).
* The PCB-layout assertion regression guard (transient code edit, so run it in a worktree): temporarily insert a PCB field before tls_base, confirm the boot panic "process_t layout drifted from switch.s offsets" fires, then discard the worktree.
* Kernel-survives-fault: from the debug shell `fault` -> the BSOD panic screen; `halt` stops cleanly.
* Style and build checks: `make verify-style` -> 8/8; in a worktree, intentionally introduce a declaration-after-statement and confirm that the check catches the C89 violation, then discard the worktree.

What to look for: a self-test [FAIL], an image booting the wrong init, a failed host test, or a layout assertion that does not fire.

Verification: each item is a matrix row. The [PASS] tokens and host-test `PASS:` lines are the contracts. The layout-assertion and style-check rows deliberately introduce and then revert defects, proving that each check fires. This phase shares the QEMU console with later boots; serialize it.

Output: a boot, self-test, and host-test matrix.

---

## Phase 13, MANUAL TEST: process lifecycle, signals, TLS, nanosleep (probes + by-hand)

Goal: confirm fork/exec/wait under load, signals end to end, TLS/auxv, and timed sleeps.

Scope: tests/{vmprobe,cowprobe,forkro,vmro,vmfault,sigprobe,jobprobe,tlsprobe,auxvprobe,errnoprobe,timeprobe}; BusyBox by-hand sequences on os-real.

Method (from the survey):
* Fork+exec+wait under load: on os-real run `yes | head -5`, then `sleep 5 &` + `wait`; run several times to stress the page-dir/page-table free lists; no leaked slots.
* Demand-paging/COW: vmprobe -> VMPROBE OK; cowprobe, forkro, vmro -> COW holds; vmfault -> the process is killed by a wild write and the KERNEL SURVIVES (shell returns).
* Hostile ELF: from the debug shell `exec` a non-ELF and a truncated/garbage file -> ENOEXEC and the CALLER stays intact; if a crafted out-of-window/overlapping-PT_LOAD binary is available, elf_load_image rejects it.
* Signals: in BusyBox install a trap, `kill -INT`, `kill -TSTP` then `kill -CONT` a stopped job, Ctrl+C / Ctrl+Z on the console -> the foreground group gets SIGINT/SIGTSTP, the job stops (PROC_STOPPED) and resumes, jobs/wait report it; sigprobe and jobprobe OK; a SIGSEGV on a child returns the parent prompt, on init returns the debug shell.
* TLS/auxv: tlsprobe, auxvprobe, errnoprobe OK.
* Nanosleep/timer: timeprobe OK; `sleep`/`usleep` in BusyBox; gettimeofday/clock_gettime/times advance at 100 Hz; nanosleep wakes on time and is -EINTR-interruptible with remaining time; proc_wake_sleepers fires from the timer while the sleeper is parked.
* Out-of-slots / OOM: a fork bomb in BusyBox -> fork returns -EAGAIN, system stays up and recovers; large mmap/brk until -ENOMEM refuses cleanly.

What to look for: a missing OK token; a kernel panic on the vmfault/segfault/fork-bomb/OOM paths (must survive); a leaked process slot after repeated cycles.

Verification: each OK token is the contract; the kernel-survives rows require the shell to return. This phase also produces the live evidence that refutes the rach2/Readme.md "no signals / no demand paging" claims (feeds Phase 9). Shares the console; serialize.

Output: a process/signal/TLS/timer matrix.

---

## Phase 14, MANUAL TEST: TTY/n_tty, pty, KRFFS hierarchy, OFD, pipes, poll/select, /dev + /proc, job control

Goal: confirm the I/O and file-system boundary behavior through the probes, musl programs, and manual tests.

Scope: the test image probes (ttyprobe, ptyprobe, pipeprobe, pollprobe, dirprobe, ofdprobe, devprobe, jobprobe) and the musl tokens (catfile, ofdmusl, pipemusl, pollmusl, ptymusl, ttymusl, ctrlcmusl, devtest); interactive os-simple/os-real.

Method (from the survey):
* `make test` runs only the runtests.py PROBES list (the freestanding probes that exit 0), so it covers PTYPROBE/PIPEPROBE/POLLPROBE/DIRPROBE/OFDPROBE/DEVPROBE/JOBPROBE -> each exits status 0. It does NOT run ttyprobe (it blocks on interactive input) and does NOT run any /musl program.
* The musl integration programs are exercised by hand: `rach2/run.sh test`, then at the kernel monitor `exec /musl/<name>` (they are staged under /musl by test.manifest under their source names). Rows: `exec /musl/catfile` -> CATFILE OK; `exec /musl/ofdmusl` -> OFDMUSL OK; `exec /musl/pipemusl` -> PIPEMUSL OK; `exec /musl/pollmusl` -> POLLMUSL OK; `exec /musl/ptymusl` -> PTYMUSL OK; `exec /musl/ttymusl` -> MTTY OK; `exec /musl/devtest` -> DEVTEST OK. `exec /musl/ctrlcmusl` prints CTRLC READY and then SUCCEEDS by being killed (Ctrl-C); it has no OK token, and a "CTRLC NOT INTERRUPTED" line is the failure.
* ttyprobe and stdintest block on interactive terminal input: run them by hand or under harness.py, one boot each, not in `make test`.
* Interactive line discipline (os-simple/os-real): a line with ^H/DEL, ^W, ^U then Enter -> ERASE/WERASE/KILL applied and echoed; ^C mid-line -> line flushed and SIGINT to the foreground job; ^D on empty -> EOF.
* Serial-console ISIG (the timer-IRQ drain): with -serial stdio, a long foreground program, ^C from the SERIAL terminal -> SIGINT via console_pump_irq (echo suppressed) with no reader parked; ^Z -> SIGTSTP; negative: with NO session owning the console (debug shell), serial input is NOT stolen.
* pty lifecycle (ptyprobe/ptymusl): open /dev/ptmx, TIOCGPTN, TIOCSPTLCK(0), open /dev/pts/N, master->slave cooked+echo, read slave; close all slaves -> master EOF; close master while a slave reader blocks -> slave read 0 (hangup) and SIGHUP to the slave fg group; negatives: open /dev/pts/N before unlockpt -> -EIO; open an unallocated N -> -ENXIO.
* KRFFS hierarchy + *at (dirprobe/catfile): mkdir, nested create/write/read, duplicate-mkdir reject, create-in-missing-dir reject, non-empty rmdir refuse, getdents listing exactly a subdir's children, openat/fstatat64/_llseek/fcntl64, O_APPEND/F_DUPFD, chdir+getcwd+relative, ENOTDIR-vs-ENOENT, /x/.. -> root.
* KRFFS allocator stress: repeatedly write past capacity to force relocation, truncate down to force coalescing, interleave files so a hole is reused -> data integrity preserved; fsck.krffs on a dumped image reports a consistent free-list with correct previous_node_size back-links.
* OFD sharing (ofdprobe/ofdmusl): dup/dup2/fork share one offset, last-close frees, a dup/close loop leaks nothing; exhaust the OFD pool -> graceful -EMFILE/-ENFILE.
* Pipes (pipeprobe/pipemusl): round-trip + EOF, O_NONBLOCK -EAGAIN, write-to-reader-closed -> SIGPIPE + -EPIPE (yes | head terminates), 5000 bytes through a 4KB pipe across a fork, create/close past MAX_PIPES leaks no slot.
* poll/select (pollprobe/pollmusl): timeout 0, POLLIN, POLLHUP on a closed writer, the same fd through poll/select/_newselect (413->142 fallback), cross-process block-then-wake, wide nfds, an out-of-range/closed bit -EBADF, nfds past FD_SETSIZE -EINVAL, nfds==0 a pure sleep, a negative fd skipped.
* /dev + /proc (devprobe/devtest): /dev/zero zeros, /dev/null EOF+sink, /dev/full -ENOSPC, /dev/urandom varies under -cpu max (and -EIO under CPU=qemu64), /proc/self/stat caller pid+comm, /proc/self/fd/0 -> /dev/console, ttyname(0) -> /dev/console, opendir over /dev and /proc; negatives: /dev/pts/<not-a-number> or /proc/<not-a-pid> -> ENOENT, /dev/<name>/<more> -> ENOENT.
* Job control / ctty (jobprobe/ctrlcmusl): setsid/setpgid, TIOCSCTTY sole-ctty claim, TIOCGPGRP/TIOCSPGRP session gating (-ENOTTY/-EPERM/-EINVAL), a background read -> SIGTTIN (or -EIO if blocked/ignored), a leader exit releases its ctty so a later session re-claims; a foreground ^C reaches only the foreground group.
* Negative boundary contracts: ioctl(-1, TCGETS) -> -EBADF; TIOCSCTTY on a regular file/pipe -> -ENOTTY; read on a write-only fd / write on a read-only fd -> -EBADF; _llseek past 4GB on a KRFFS file rejected; a copy_from_user fault on the pipe2 fd array tears the pipe down with no leak (loop and confirm no exhaustion).

What to look for: a missing OK token; a byte-stealing or signal-to-group-0 hazard on the serial path; a leaked OFD/pipe/pty slot; a wrong errno on a negative case; the kernel hanging or panicking on a hostile KRFFS image.

Verification: probe tokens are contracts; the interactive rows are observed at the console and re-run once. The allocator-stress integrity is cross-checked with host fsck.krffs. The serial-ISIG and ctty-handoff rows directly exercise the Phase 2 concurrency findings. Shares the console; serialize the interactive rows.

Output: an I/O+fs test matrix.

---

## Phase 15, MANUAL TEST: SSH server

Goal: confirm the highest-integration path (fork/exec/signals/tty/pty/TLS/entropy/TCP/socket) end to end.

Scope: os-real image, Dropbear server, host ssh client. The guest's outbound client (`dropbearmulti dbclient`) is built and present but not exercised here. Outbound use over SLIRP is optional because SLIRP forwards inbound host ports but constrains arbitrary outbound connections. This phase tests the server path, with host ssh connecting to the guest.

Method:
* `make full`, boot `NET_HOSTFWD="hostfwd=tcp::2222-:22" ./run.sh os-real`, at the guest `exec dropbearmulti dropbear -F -R -p 22` (no -E), host `ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost` -> pubkey auth into interactive BusyBox ash, rc 0. Requires -cpu max.
* Inside: a job-control sequence (start a job, ^C, ^Z, fg/bg) -> signals route through the pty slave fg_pgid; `tty` -> /dev/pts/0; `ls -l /dev/pts` -> the allocated slave.
* Confirm `make full` ran gen_demo_key.sh when krffs/authorized_keys was absent, and the os-real manifest modes (/root/.ssh 0700, authorized_keys 0600).

What to look for: keygen failure (points at the entropy/CSPRNG path); auth failure; a pty/ctty or signal-routing fault under the login.

Verification: rc 0 and an interactive prompt is the contract; the job-control and tty rows confirm the pty path. A keygen failure is cross-checked against the -cpu max requirement (boot with CPU=qemu64 and confirm keygen fails closed, refuting any "entropy is fine without -cpu max" assumption). Owns the console for its run.

Output: end-to-end SSH pass/fail with the inside-session observations.

---

## Phase 16, MANUAL TEST: Doom and the audio matrix (sb16, hda, speaker)

Goal: confirm Doom (graphics, raw input, audio) and the three audio drivers.

Scope: ports/doom (needs doom1.wad); sys/audio/{sb16,hda,speaker} via the audio-debug shell commands.

Method: split the matrix two ways, because `rach2/run.sh` does NOT pass `-display none` and selects an audio backend (CoreAudio on macOS). The serial-observable diagnostics run headless and are read off the serial log: the `audio` debug commands, device detection, and the kernel's audio self-test lines (LPIB movement, fills, underruns, "DMA: ..." lines). The audible and graphical rows (Doom's graphics and mouse, VGA scrollback, and audible SB16/HDA/speaker output) need a display and an audio device, so they require an interactive run and a human to confirm. The rows below are tagged [serial] or [interactive] accordingly.
* [serial] Audio HDA: `rach2/run.sh os-real hda`, then `audio info` (driver=Intel HD Audio, real=yes), `audio test scale 2000` (callback, 48k S16 stereo), `audio test write 2000` (write/ring/resample, 11025 U8 mono -> 48k stereo), `audio hda dump/stats/pins` (LPIB advancing, fills>0, underruns low, "DMA: LPIB n->m" moving). "LPIB did not move" is the failure signal. `audio test watch 440 3000 200` -> periodic "t=...ms lpib=N (+delta)" with positive deltas wrapping mod CBL.
* [serial] Audio SB16: `rach2/run.sh os-real sb16`, `audio info` (Sound Blaster 16), `audio test scale`/`write` -> no AUDIO_WRITE failures; watch for "DMA: Invalid address" on serial (the static dma_buffer 64KB-boundary edge from Phase 3). [interactive] the actual audible output needs a human.
* [serial] Audio speaker: `rach2/run.sh os-real speaker`, `audio info` (PC Speaker, real=no), `audio test scale` reports it cannot play samples (write -1).
* [serial] PCI/HDA negative: boot with no HDA device (speaker/sb16 only) -> init_audio falls through HDA to the next driver without hanging in pci_find_device_skip.
* [interactive] Doom: `rach2/run.sh os-real sb16` then `doom` -> E1M1 in Mode 13h, arrow/Ctrl/Space via raw scancodes, mouse turn/move, SB16 SFX + OPL3 music; repeat with hda (kernel resample/upmix) and the speaker fallback (DP-lump beeps via KIOCSOUND). Quitting Doom must restore text mode cleanly (watch for a corrupted console/font after exit).
* [serial] Reentrancy stress: `audio test write` sustained while watching `audio hda stats` for ring_high_water near HDA_RING_SIZE and underrun jumps; run audio under heavy syscall load (the timer keeps streaming during syscalls).
* [interactive] VGA text scrollback: >25 lines then PageUp/PageDown/Home/End -> scrolls the 512-line history, snaps to live on End, clamps at history start with no crash.
* [interactive] PC speaker direct: a beep at a known frequency tracks 1193182/divisor; speaker_off silences; the divisor-underflow edge.
* [serial] Mouse bring-up: init_mouse returns 0 (no "enable failed"); a non-answering mouse still lets init proceed but a failed enable returns -1 leaving interrupts enabled (no wedge). [interactive] deltas with correct sign need a human moving the mouse.

What to look for: "LPIB did not move", "DMA: Invalid address", audio underrun jumps, a corrupted text console after Doom exit (font/plane-2 restore bug), a hang in pci_find_device_skip on the no-HDA path.

Verification: the audio-debug command outputs are the contract; the Doom run and the post-exit console are observed. The SB16 DMA-boundary and HDA period-tracking findings from Phase 3 are confirmed or refuted here. Owns the console per run; serialize the matrix.

Output: a Doom + audio-matrix pass/fail matrix.

---

## Phase 17, MANUAL TEST: the network path (ping, udp, tcp, httpd via hostfwd, dns, nc) and concurrency stress

Goal: confirm ICMP, UDP, TCP active and passive open, DNS, the interactive client, and the net+audio concurrency invariant.

Scope: tests/{pingtest,udptest,tcptest,sockettest,dnstest}; utils/{ping,nc,httpd}; the netping/netbad debug handlers; a host echo server and curl.

Method (from the surveys; these need host peers and are excluded from `make test`):
* socket lifecycle (no peer): sockettest -> 4 OK (30x create+close, dup2 share, fork share, 30x exit-without-close); a leak shows as pool exhaustion.
* ICMP: `rach2/run.sh` then `netping` -> reply in N ticks; `netbad` -> timeout (expected) after ~4 attempts.
* UDP: needs a host echo server reachable from the guest at 10.0.2.2:7777: host `socat -v UDP-LISTEN:7777,fork,reuseaddr EXEC:cat`, run udptest -> Tests 1-5 OK incl. recvfrom into 0x00100000 rejected (below USER_ADDR_MIN); negative: kill the server, Test 4 fails cleanly after 40 attempts, no hang.
* TCP: needs a host echo server reachable from the guest at 10.0.2.2:7777: host `socat TCP-LISTEN:7777,fork,reuseaddr EXEC:cat`, tcptest -> 4 OK (SYN-dropped-during-ARP then retransmit, read/write echo, graceful close, connect-to-closed-port refused not hang).
* httpd (passive open): needs a hostfwd to reach the guest's listener. Boot `NET_HOSTFWD="hostfwd=tcp::8080-:80" rach2/run.sh os-simple`, `exec httpd`, host `curl -v http://127.0.0.1:8080/` -> HTTP/1.0 200 with the client ip:port and request line; reload to fill PEER_RING(6) and confirm distinct source ports (TCB reclaim); stress `for i in $(seq 1 20); do curl -s localhost:8080 >/dev/null & done` (the 16-slot accept_queue + TIME_WAIT reclaim); negative: a very long User-Agent and a <script> path -> HTML-escaped, no overflow/crash (refutes/confirms Phase 5 httpd findings); nc-and-close-before-sending -> the "closed before sending a request" warn.
* DNS (dnstest + live): boot the test image with NET on (forwarder 10.0.2.3), dnstest -> 45/45 incl. one.one.one.one->1.1.1.1, dns.google->8.8.8.8; run twice (the live block is flaky).
* nc (interactive): host `socat TCP-LISTEN:7777,reuseaddr -`, guest `nc gateway 7777`, type lines -> bidirectional echo, EOF on host close -> recv 0; negatives: `nc host 0` invalid port, `nc onlyonearg` usage, `nc 10.0.2.2 9` connect failed.
* ping/nc by hand on os-simple: `ping` default gateway, `ping gateway`/`ping dns` (name via /hosts), `ping nxdomain.invalid` -> cannot resolve exit 1, `ping 999.1.1.1` resolve fails cleanly.
* RTO visibility: watch the serial log for "TCP: retransmit (RTO N, try M)" on the first SYN and confirm the connection still completes.
* Concurrency stress (the net_lock-vs-IRQ invariant, the most serious Phase 4 boundary): `rach2/run.sh os-real hda`, start audio playback AND run httpd/curl traffic at once -> no corruption (garbled body, kernel fault, audio underrun panic).
* ARP cache: `netping` warms the gateway, a second is faster; fill the 8-entry cache and confirm the 9th still works.
* Negative user-pointer handling of socketcall: there is no fuzz probe today, so this is a code-review cross-check rather than a runnable test. Read the user_buffer_ok and range checks in sys_socketcall and confirm by argument that recvfrom/recv/send/sendto with buf in the kernel window, an overflowing buf, or an unmapped sockaddr returns -EFAULT (never a panic), an out-of-range sub-call returns -EINVAL, and SC_GETSOCKOPT with NULL optlen returns -EFAULT. The udptest Test-5-style kernel-pointer, overflow, and unmapped-sockaddr cases provide runnable in-guest evidence. A dedicated fuzz probe is optional.

What to look for: a hang where a clean failure is expected; a socket-pool exhaustion (leak/double-free); an httpd overflow/crash; a kernel fault or audio glitch in the concurrency stress (the unlocked-TCP-reader-vs-slot-recycle finding); a -EFAULT path that panics instead.

Verification: each token/HTTP-200/echo is the contract; the concurrency-stress and user-pointer-fuzz rows are the live confirmation of the Phase 4 critical findings; a fault there elevates the finding to confirmed-critical. Re-run flaky live-DNS once. Shares the console; serialize.

Output: a network test matrix, with the concurrency-stress result cross-linked to the Phase 4 SPSC finding.

---

## Phase 18, MANUAL TEST: ish, utils, and the musl integration programs by hand

Goal: confirm the shell, the coreutils, and the musl programs behave per their Readmes and tokens.

Scope: ish on os-simple; the utils; tests/musl/*.

Method:
* ish by hand (os-simple): `echo hello world`; `cd /` then `pwd`; `echo test > /tmp.txt` then `cat < /tmp.txt`; `ls /` (staged utils + type flags); a missing command silently ignored; `cat /hello.txt`; ish is PID-1 init and survives a child exit.
* ish redirection edges: `cat < /nonexistent` (open fails fd=-1 -> runs without redirection; observe); `cmd > /somefile` with cmd absent (creat before the PATH-miss continue); `cd` with no HOME; `exit abc` -> 0; `exit -1`.
* ish PATH search: a bare applet under a PATH dir vs an absolute-only path; a no-match name skipped silently.
* musl programs (test image, `rach2/run.sh test` then `exec /musl/<name>` at the kernel monitor; staged under their source names): `exec /musl/mallocqsort` -> MALLOC QSORT OK; `exec /musl/sockfillmusl` -> SOCKFILLMUSL OK; `exec /musl/devtest` -> DEVTEST OK; permmusl -> PERMMUSL OK; authmusl -> AUTHMUSL OK; budgetmusl -> BUDGETMUSL OK; cowfork -> COWFORK OK; sigmusl -> SIGMUSL OK; rngmusl -> RNGMUSL OK; timemusl -> TIMEMUSL OK; tcpsendrecv -> TCPSENDRECV OK. `exec /musl/ctrlcmusl` prints CTRLC READY then succeeds by being killed (Ctrl-C); it has no OK token.
* ttyprobe and stdintest via harness.py (one boot each; they block on interactive input): first build a harness kernel (`make HARNESS=1`, in a worktree or a separate build dir, since it reports each process exit status to QEMU's isa-debug-exit; see rach2/Makefile) and point harness.py at it. ttyprobe -> TTYPROBE OK; stdintest driven by a line over serial. ptyprobe is NOT run here (it is already in the runtests.py PROBES list covered by `make test`). Use the normal kernel for the baseline comparisons.
* Build matrix sanity: `make -C utils` (musl) and `make -C utils -f Makefile.linux` (file utils only); `make -C ish` and `-f Makefile.linux`; `make -C ports/doom` -> build/ports/doom/doom; a header edit rebuilds only dependents.

What to look for: a missing token; the redirection-fd-guard behavior (does a failed input redirect wrongly run the command); a leaked fd; a build that touches more than dependents.

Verification: tokens are contracts; the redirection rows are the live evidence for the Phase 5 ish-redirection finding and the Phase 9 ish/Readme drift. Shares the console; serialize.

Output: an ish/utils/musl test matrix.

---

## Phase 19, MANUAL TEST: krffs host tools, FUSE mount, and per-image ISOs

Goal: confirm the host tools agree byte-for-byte with the kernel and the ISOs boot via GRUB.

Scope: the krffs host tools (mkfs/fsck/fsviz/defrag/edfs/krffs+fuse), the manifests, mkimage.sh, tools/mkiso.sh, run_from_iso.sh.

Method:
* On a built image: `fsck.krffs` (magic, node-list, free-list, parent_id links), `fsviz.krffs` (layout), and a FUSE mount to read files -> the host tools and the kernel agree on the byte-identical header; build a fresh image with `mkimage.sh krffs/manifests/test.manifest out.krffs 16` and confirm stageimage writes v2 directory nodes the guest then mounts.
* For os-simple/os-real confirm /hosts+/resolv.conf (os-simple flat root) and /etc/hosts+/etc/resolv.conf (os-real) exist with the expected contents (nameserver 10.0.2.3, gateway 10.0.2.2).
* defrag + fsck loop and the suspected contradictory fsck success message (cross-ref Phase 6/11): corrupt a node magic with edfs, run fsck -> "invalid file system signature" exit -12, and confirm it does NOT also print "completed successfully".
* ISO (needs Docker): `make os-simple-iso` -> build/images/os-simple.iso; `./run_from_iso.sh os-simple` boots via GRUB; repeat for os-real if built.

What to look for: a host/kernel header mismatch, a malformed v2 image the guest cannot mount, the contradictory fsck success message, or an ISO that does not boot.

Verification: the byte-identical header is the contract (Phase 6 hash coupling confirmed here end to end via a deep-path round trip); the ISO boot is observed. Shares the console; serialize the boot rows.

Output: a host-tools + ISO matrix.

---

## Phase 20, MANUAL TEST: negative and edge cases

Goal: confirm the fail-closed entropy, OOM, missing-WAD, missing-key, and default-CPU paths behave as documented.

Scope: the entropy fail-closed contract; OOM with an oversized image; Doom with a missing WAD; SSH with a missing key; the default-CPU run.

Method:
* Entropy fail-closed: boot the test image with `CPU=qemu64 rach2/run.sh test` (no RDRAND) -> rngprobe FAILs closed; read /dev/urandom or call getrandom -> -EIO, while a process still starts (AT_RANDOM/canary best-effort); Dropbear keygen fails without -cpu max. Positive control: `make test` (default -cpu max) -> rngprobe/RNGPROBE OK and two getrandom draws differ.
* OOM: an oversized image / large mmap+brk until -ENOMEM -> the allocator refuses cleanly without corrupting the kernel; a tiny image so a manifest file does not fit -> mkimage "not enough space" non-zero exit (cross-ref Phase 11).
* Missing Doom WAD: launch doom with doom1.wad absent -> a clean failure, not a kernel fault.
* Missing SSH key: attempt the end-to-end SSH test with krffs/rach2_demo_ed25519 absent or wrong -> auth fails cleanly; confirm `make full` regenerates the key via gen_demo_key.sh when krffs/authorized_keys was absent.
* Default-CPU general boot: confirm the run scripts default to -cpu max and that overriding it surfaces the documented entropy consequence.

What to look for: any negative path that panics, hangs, or corrupts instead of failing cleanly; the entropy check not refusing operation without -cpu max.

Verification: each is a deliberately induced failure with an expected clean outcome. The rngprobe positive and negative pair proves both outcomes of the entropy check. This phase shares the console; serialize it.

Output: a negative/edge matrix.

---

## Phase 21, SYNTHESIZE + FIX + RE-VERIFY

Goal: deduplicate and triage all findings, fix those selected for this run, repeat the full verification and boot tests, and leave the rest in a documented triaged list.

Scope: every findings list and test matrix from phases 1-20.

Method:
* Collate all CODE/COMMENT/DOC/SUPPORT findings and all failing test rows into one register keyed by file:line + severity, deduplicating the cross-phase overlaps (e.g. procfs_gen_stat staleness appears in Phases 2 and 9; the contradictory fsck success message in 6/11/19; the unlocked-TCP-reader in 4/17; the rach2/Readme.md drift in 1's doc focus and Phase 9; the host/kernel hash coupling in 6/19).
* Triage each into fix-now (localized, no behavior change for comments/docs; clearly correct bug fixes), defer (risky, needs design, or behavior-changing), or reject (refuted by Codex or by a test).
* Fix the selected findings in scoped commits, one logical change each, in the repository's one-line house style without attribution, keeping the vendored trees out of the diff. Comment and documentation fixes change no behavior and no built artifact. Decide and apply the rach2/Readme.md correction (rewrite to the current contract or explicit foundation-snapshot framing) according to the Phase 9 recommendation. Address the three config-file text violations and the textcheck coverage omission from Phase 10 if the decision was to fix them.
* Re-run the full verification and the important boots: `make verify-style` (8/8), `make verify-host` (eleven), `make full`, `make test`, a boot of each image, and the end-to-end SSH test. The success TOKENS and pass/fail matrix must match the Phase 0 baseline. A kernel or image byte-size delta is allowed but must be explained by the actual diff because a fix changes bytes by design. Only an unexplained token or matrix regression blocks the commit.

What to look for: any fix that breaks verification, a boot, or the end-to-end SSH test; any deduplication that merges two distinct findings; or any fix that changes a vendored tree.

Verification: every fix commit receives an independent Codex review, and every finding is fixed before the commit stands, as required by CLAUDE.md. The review includes the voice of comment and documentation changes, which no checker can judge. Repeated verification, boots, and the end-to-end SSH test must reproduce the Phase 0 success tokens and pass/fail matrix. An unexplained token or matrix divergence is a regression and blocks the commit; an explained byte-size delta is allowed.

Output: the final triaged register (fixed / deferred-with-rationale / rejected-with-refutation), the scoped commits, and passing verification, boots, and end-to-end SSH test confirming no regression from the Phase 0 baseline.
