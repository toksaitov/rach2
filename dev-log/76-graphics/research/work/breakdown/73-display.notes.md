# Notes for the 73 display breakdown

Drafter: Fable, for milestone 73 only. Output: `73-display.md` beside this file.

## Read so far

* brief.md: form of dev-log/70-wifi-chip/plan.md; one file per milestone; tasks keep plan.md's numbers and Done; subtasks lettered; tests and documents as their own subtasks; Done per task; owner-only and linux-host labels; size in lines; Shared with other milestones; Open questions; notes file beside; reply with path, line count, subtask count, three least-sure points.
* plan.md: 73's table (18 tasks), its Done, decisions, order, verification, risks. 73 is about 11,000 kernel lines.
* dev-log/70-wifi-chip/plan.md: the form.

## Code facts (path:line) gathered while reading

(filled in as I go)

## Decisions taken in the draft

(filled in as I go)

## Open points to put under Open questions

(filled in as I go)

### Batch 1 (tree survey)

* Display today: `rach2/sys/display/` = fbcon.c (512), fbdev.c (515, `/fb` device, no mmap: Readme Limits), vga_gfx.c, vga_text.c, vga_font.c, blank.c (170), backlight.c (221, `/sys/class/backlight`), intelbl.c (273, PCH PWM). uapi header `rach2/include/rach2/fb.h`. `fbcon_window` maps the loader's framebuffer write-combining through `map_mmio_typed` (Readme "The framebuffer console"); `pat_setup` in `arch/x86/pat.c`.
* `struct file_ops` (`rach2/sys/fs/file.h:23`): read, write, poll(e, events, wq), ioctl(e, request, arg), close; NO mmap hook. `/dev/mem` mmap is special-cased in `rach2/sys/syscall_proc.c:6170-6322` (`VM_PROT_IO` regions, checks `devfs_rdev(e) == 1:1` at :6241). So 73-2 adds a per-description mmap operation and generalizes that path.
* `VM_PROT_IO` at `rach2/sys/mm/vm.h:38`: no fault backs it, core files and /proc/PID/mem skip it; mremap growth refused (`syscall_proc.c:6724`).
* evdev: `rach2/sys/evdev.c` (650), keys only (EV_KEY, EV_MSC, EV_LED, EV_REP: `evdev.h:36-47`), EVIOCGRAB (`evdev.c:85,457`), EVDEV_MAX 32, major 13 minor 64+N. No EV_REL/EV_ABS, no EVIOCREVOKE, no EVIOCSCLOCKID. Mouse is `sys/mouse.c` (59 lines) + `input_mouse` (`input.h:186`), the PS/2 driver `arch/x86/ps2mouse.c`, USB HID `sys/usb/hid.c`.
* PCI: `rach2/sys/pci.h`: `pci_device_t`, `pci_find_capability`, `pci_msi_enable`, `pci_msix_table`/`pci_msix_hold`/`pci_msix_enable` (entry 0 only), `pci_bar_address64`, `pci_read_config32_ext`. No virtio anywhere in the tree.
* Syscall tables: `rach2/sys/arch/{i686,x86_64}/systab.c`, `table[SYS_IOCTL] = sys_ioctl` (x86_64:142, i686:122). `sys_ioctl` in `rach2/sys/syscall.c:2167`; it dispatches by request on the description's file_ops ioctl (file.h:35) or the tty cases.
* Manifests: `rach2/sources.mk` (C_SRC_SHARED, arch lists in `sys/arch/$(ARCH)/sources.mk`); `utils/programs.mk` (UTILITIES, SBIN_PROGS, SRC_/HDR_/DEF_).
* Tests: probes `tests/*probe.c` at top, host tests `tests/host/*_hosttest.c` (262 files), `tests/system/` suites, `tests/cases/` manifests, `tests/registry` (suite, tier), `tests/fuzz/`. Existing `screen` suite judges by QMP screen dumps (display Readme Tests). `uefi` suite boots OVMF and dumps the screen.
* man: man4 has backlight, console, input, loop, mem, dsp, mixer...; `man/aliases` NAME SECTION TARGET; `tools/verify/mancheck.c` requires a page per program/applet/syscall. Section 4 pages for devices.
* QEMU: `rach2/qemu_devices.sh` sets AUDIO_OPTS, NET_OPTS, DISPLAY_OPTS; no -device for the display (QEMU's default std VGA). `rach2/run.sh` boots kernel + krffs initrd, DISK= for disks.
* GRUB: `tools/image/mkdisk_grub.sh:277-324` sets `gfxpayload=auto` (pc) or `text` for the text entry.
* GNU layer: `vendor/gnu/` builds ports from pinned tarballs (`sources.txt` SHA-256, `fetch.sh`); Debian packages for chromium-headless-shell (`vendor/gnu/chromium/`) and the agents layer (`vendor/gnu/agents/`, `fetch_agents.sh`); `install_root.sh` with a SKIP list; `os-real-gnu.ext2` 4096 MiB sparse, built by `tools/image/mkext2.sh`; init is `init-metal` (sshd, dbus system bus, crond, ntpd).
* Upstream on hand: Mesa 25.0.7, Weston 14.0.2, libdrm 2.4.x, libinput 1.28.1, seatd 0.9.1, libudev-zero.
