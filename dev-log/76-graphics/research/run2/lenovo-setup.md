# The Lenovo's setup: Debian 13 on the trial partition

Written as it ran, from 20:11 on 2026-11-08 (UTC+6), by the agent of the Lenovo's setup for the second graphics run. Raw outputs and the scripts are in `run2/lenovo-setup/`; images and downloads are in the Mac's `build/cache/run2/lenovo-setup/`.

## The machine before

* 20:11: Rach2 answered at 192.168.0.106 (`rach2-before.txt`): kernel "6.18.0-rach2", `/etc/rach2/release` commit bb151e3b; booted from the candidate slot; `bootctl status` gave `saved_entry: candidate`, `next_entry: empty (a trial was consumed)`, `booted: candidate`. The lock `build/flow/lenovo/issue` read `run2` and was left in place.
* The root p3 is mounted as `ext2` (`rach2.rootfstype=ext2`); blkid calls it ext4 with only ext2 features (no journal). The brief said ext4; it mounts read-only either way.
* Partition table (`gpt-before.txt`, `sfdisk -d /dev/disk0` on Rach2): disk GUID 1ACEF00D-0F1E-4D2C-8B3A-C0FFEE000001, last LBA 488397134.
  * p1 "Rach2 boot", sectors 2048 to 133119 (64 MiB), vfat RACH2BOOT, UUID 1ACE-F00D, 55,516 KiB free.
  * p2 "Rach2 recovery", 133120 to 264191, vfat 1ACE-F003.
  * p3 "Rach2 root", 266240 to 421287935, PARTUUID 45444342-4746-4948-4849-4a4b4c4d4e4f.
  * p4 "BIOS boot", 34 to 2047.
  * p5 "Rach2 trial root", 421287936 to 488396799 (32 GiB), PARTUUID f640712e-b236-4c5a-b7d8-1b149d598ec0. Before this setup it held an ext2 copy with the same label and UUID as p3 (`rach2`, 52616368-3200-4578-7432-000000000001).
* Rach2 has `resize2fs`/`e2fsck` 1.47.4, `zstd`, `xz`, `curl`, `sfdisk`, `rtcwake`.

## p1 saved

* 20:13: `rssh 'dd if=/dev/disk0p1 bs=1M' > build/cache/run2/lenovo-setup/p1-orig.img`, 67,108,864 bytes. SHA-256 on the Mac and of `/dev/disk0p1` on the Lenovo, both `33829ae85d4283cd9076f03d8267727481e2f9b2caa7bdc776f52c4c91305193`.
* Its menu slot `/boot/boot.cfg` (partition sectors 10259 to 10274) holds `# no menu staged; bootctl config replaces this file` and `#` padding; its grubenv (sectors 10275 and 10276) `next_entry=` (empty) and `saved_entry=candidate`. Kernel slots: candidate at 11848 (8192 sectors), fallback in two runs, 2053+4096 and 20040+4096.

## Debian's root

* The way: Debian's generic cloud image, its root partition written to p5 from the Mac over SSH, configured by files written into it on the Mac with `debugfs` (Homebrew e2fsprogs 1.47.4) before it was written, among them a NoCloud seed. Chosen over debootstrap in WSL because it needs no work on the testing PC, where step 1 times its runs, and the Lenovo installs the packages itself from deb.debian.org over its wired port.
* 20:13: `debian-13-generic-amd64.qcow2` (438,829,056 bytes) and its `.json` manifest from `https://cloud.debian.org/images/cloud/trixie/latest/` (redirected to the Debian mirror gemmei.ftp.acc.umu.se); both check against that directory's `SHA512SUMS` (`sha512sum -c`: OK). Qcow2 SHA-512 `1207e6e3efaff8ee...57fee4b`. Image version 20261103-2651: kernel `linux-image-6.12.115+deb13.1-amd64` 6.12.115-2, cloud-init 25.1.4-1+deb13u1, netplan 1.1.2-7, systemd 257.13-1~deb13u1 with systemd-timesyncd, openssh-server 1:10.0p1-7+deb13u4, initramfs-tools 0.148.4 with cloud-initramfs-growroot, e2fsprogs 1.47.2.
* `qemu-img convert -O raw` gave a 3 GiB disk; its GPT (`gpt.py`) has the root at sectors 262144 to 6289407 (6,027,264 sectors, 3,085,959,168 bytes, ext4 with metadata_csum and orphan_file, UUID 96e538c4-e118-47a5-ad63-ddbd0e67f846), BIOS boot p14 and an ESP p15. The root was cut out with `dd` to `root.ext4`.
* Its kernel is 12,167,104 bytes (SHA-256 `96758c40...5d14f2`) and its initrd, built with `MODULES=most` and zstd, 37,146,995 bytes (`6e3bc161...19525b`): 49,314,099 bytes together, which fit p1's 55,516 KiB free with 7,504,384 bytes left, so the image's own initrd went to p1 unchanged.
* Written into a copy, `root-run2.ext4`, with `debugfs -w -f debugfs-inject.txt` (files in `inject/`), each owned by root, mode 644, directories 755; `e2fsck -fn` then returned 0:
  * `/etc/fstab`: root by `PARTUUID=f640712e-b236-4c5a-b7d8-1b149d598ec0`, ext4, `rw,errors=remount-ro,x-systemd.growfs`; p3 by its PARTUUID on `/mnt/rach2`, `ro,noauto,nofail`. The image's own lines named its own root and ESP; the ESP line would have stopped the boot in emergency mode.
  * `/etc/growroot-disabled`, which keeps the initrd's growroot from running `growpart` on the Lenovo's table; cloud-init's `growpart` is off in the seed too. The file system grows to the partition by cloud-init's `resize_rootfs` and systemd's `x-systemd.growfs`, online.
  * `/etc/udev/rules.d/90-run2-backlight.rules`: `intel_backlight` set to 47 (of 937, the first run's value) when it appears.
  * `/etc/modprobe.d/run2-blacklist.conf`: `blacklist snd_hda_intel`, `blacklist ath9k`, besides the kernel's `modprobe.blacklist=`.
  * `/var/lib/cloud/seed/nocloud/`: `meta-data` (instance `rach2-lenovo-run2`, host `lenovo-debian`), `network-config` (version 2: DHCP on the wired port, matched as `en*`, with `dhcp-identifier: mac` so that the router sees the same client as under Rach2 and Alpine), `user-data` (root's SSH key the development key's public half `krffs/rach2_demo_ed25519.pub`, `disable_root: false`, `ssh_pwauth: false`, no default `debian` user, a user `gfx` with a locked password in `video`, `render`, `input` and `kvm`, `growpart` off, `resize_rootfs` on). The image's `sshd_config` already has `PasswordAuthentication no`.

## The boot partition

* `p1-run2.img`, a copy of `p1-orig.img`, given with mtools: `/run2/vmlinuz`, `/run2/initrd.img` and `/boot/grub/x86_64-efi/linux.mod`. The module is the first run's copy from Ubuntu's `grub-efi-amd64-bin_2.12-1ubuntu7.3_amd64.deb` (deb SHA-256 `92ae3a84...067e108e`, module 28,328 bytes, `c53e3aec...03b09089`), which matches the disk's GRUB, "GRUB 2.12-1ubuntu7.3" by the strings in p1; it was not checked again against Ubuntu's index tonight. Its dependencies `boot mmap relocator video` are in the core image, as the first run found.
* It differs from `p1-orig.img` in 9 runs of 97,888 sectors (`p1-runs.txt`, by the first run's `runs.py`); none touches a kernel slot, the menu slot or the grubenv.
* The menu block, `boot.cfg`, is the first run's block with Debian's kernel: when `next_entry` is `recovery`, set it empty and `save_env` it, find p1 by `1ACE-F00D`, `insmod linux`, then

  `linux /run2/vmlinuz root=PARTUUID=f640712e-b236-4c5a-b7d8-1b149d598ec0 ro ds=nocloud modprobe.blacklist=ath9k,snd_hda_intel panic=10`

  and `initrd /run2/initrd.img`. The IOMMU is left at the kernel's default.

## The rehearsal in QEMU

* 20:21 to 20:23, on the Mac under TCG (`rehearse.sh`): a sparse disk the size of the Lenovo's (488,397,168 sectors) with the Lenovo's own GPT (its first 34 and last 33 sectors, read with `dd` over SSH), `p1-run2.img` at sector 2048 with the menu block staged as `bootctl config` stages it (`#` padding) and `next_entry=recovery` in its grubenv (`rehearse-p1.py`), and `root-run2.ext4` at p5's sector 421287936. OVMF (`edk2-x86_64-code.fd` of Homebrew's QEMU 11.1.3), q35, its AHCI controller, an e1000e NIC with SSH forwarded to 127.0.0.1:2252.
* GRUB took the block (serial: `EFI stub: Loaded initrd from LINUX_EFI_INITRD_MEDIA_GUID device path`, after two `Unknown command '#'.` lines, which GRUB prints while it sources the menu; they did not stop it). SSH for root by the development key answered at the first try, 45 s after the start. In the guest: the command line as above; `/` on `/dev/sda5`, grown to 32G; `cloud-init status --long` "done", datasource `DataSourceNoCloud [seed=cmdline,/var/lib/cloud/seed/nocloud]`, no errors; `systemctl is-system-running` "running", no failed units; `enp0s2` by DHCP; `gfx` in video, input, kvm and render; `NTPSynchronized=yes`; no ath9k or snd_hda_intel; the grubenv afterwards `next_entry=` (empty), `saved_entry=candidate`. The rehearsal disk was then deleted.

## Writing the Lenovo

* 20:22: the first try to write p5 from the running Rach2, `zstd -dc | dd of=/dev/disk0p5`, failed at once with `dd: error writing '/dev/disk0p5': Operation not permitted`, 0 bytes written. That is by design: disk(7) and reflash(8) say the ordinary kernel returns EPERM for writes to whole-disk and partition nodes, and only the recovery kernel permits them. `/dev/bootpart` takes root's writes on every kernel (`rach2/sys/block/Readme.md`), so p1 is written from the running Rach2 and p5 from the recovery system.
* 20:24: p1 from Rach2: `p1-run2.img` copied to `/dev/shm` with `scp`, its 9 changed runs written through `/dev/bootpart` with coreutils `dd` (`p1-write.sh`); `sha256sum /dev/bootpart` then gave `0cc6fbe5c5411f38476b49a173d34b214cedc98aabc391134d5e6dfa8c744ef1`, the SHA-256 of `p1-run2.img`; `bootctl status` unchanged (candidate saved, next entry empty). The disk is a Samsung SSD 870 EVO 250GB (`/proc/disk`).
* 20:24:41: `bootctl trial recovery` ("armed: the next boot tries recovery once") and `reboot`, with the menu slot still unstaged, so that the disk's own recovery system boots. The recovery partition's `boot/recovery_authorized_keys` has the development key (same fingerprint, SHA256:413NBsqJ...).
* 20:25:08: the recovery system answered SSH at 192.168.0.106 (its own host key, kept in `run2/lenovo-known-recovery`; `recssh`), with `rach2.slot=recovery` on its line and `bootctl status` "booted: recovery". It has `reflash`, `gunzip` and `xz`, but no `zstd`. `/proc/disk`: `part 0 5 421287936 67108864 f640712e-b236-4c5a-b7d8-1b149d598ec0 Rach2 trial root`.
* 20:25: p5 from recovery: `gzip -1` of `root-run2.ext4` on the Mac (444,070,728 bytes), streamed as `recssh 'gunzip | reflash -p /dev/disk0 215699423232 3085959168'` (p5's first sector times 512, the image's length), at about 15 MB/s over the Mac's Wi-Fi (`reflash-p5.out`).
* 20:27:21: reflash wrote 3,085,959,168 bytes at 215699423232 (`reflash-p5.out`). `reflash --verify /dev/disk0 215699423232 3085959168` gave `b48dd1d0fe7aa64fe80a6c7f5b4ce3ed45f0a85a105ad46df67f0adb24002610`, the SHA-256 of `root-run2.ext4` (`reflash-p5-verify.out`).
* 20:28:18, still in recovery: `bootctl config /dev/shm/boot.cfg` ("staged: 671 bytes of menu in a 8192-byte slot"), `bootctl trial recovery` ("armed"), `bootctl status`: `saved_entry: candidate`, `next_entry: recovery` (`arm-debian.out`). p1 read back then (`p1-armed.img`, SHA-256 `1374671f...5f40bc`) differs from `p1-run2.img` only in the menu slot's first 2 sectors and the grubenv's first sector. 20:28:29: `reboot` from recovery, a reboot of Rach2's own kernel, not of Linux.

## Debian on the Lenovo

* 20:28:59, 30 s after the reboot: Debian answered SSH at 192.168.0.106 by the development key, with the line above. Its host key is in `run2/lenovo-known-debian` (`dssh`). GRUB had cleared `next_entry` (`saved_entry=candidate`, `next_entry=` read from p1 under Debian).
* First boot (`debian-first.txt`): Debian 13.7, kernel 6.12.115+deb13.1-amd64; `cloud-init status --long` "done", NoCloud from the seed, no errors; `systemctl is-system-running` "running", no failed units; `enp1s0` (r8169, RTL8168gu, xx:xx:xx:xx:xx:xx) 192.168.0.106/24 by DHCP, gateway 192.168.0.1, link-local `fe80::xxxx:xxff:fexx:xxxx`; `/` is `/dev/sda5`, grown online to 32G; `intel_backlight` 47 of 937; neither `ath9k` nor `snd_hda_intel` loaded; root and `gfx` have locked passwords; root's `authorized_keys` holds the development key alone, without cloud-init's "Please login as" prefix.
* The clock: `systemd-timesyncd` synced at 20:33 from 17.253.18.131, the NTP server the router's DHCP gives (`facts/time.txt`); time zone Etc/UTC.
* The flash drive is not seen under Debian: `lsblk` lists only the Samsung SSD (sda1 to sda5) and the optical drive.
* r8169 asks for `rtl_nic/rtl8168g-3.fw` (non-free-firmware, not installed), fails to load it and runs without it: link up at 1 Gb/s full duplex.

## Packages

* 20:29 to 20:34: `install.sh` (copied to `/root/run2/`), run under `nohup` on the Lenovo with its log in `/root/run2/install.log` (copy in `install.log`): a pin of priority -1 for `pipewire*`, `wireplumber*` and `pulseaudio*` (`/etc/apt/preferences.d/run2-no-sound-server`); Google's repository with its key from `https://dl.google.com/linux/linux_signing_key.pub` as `Signed-By` (`/etc/apt/sources.list.d/google-chrome.sources`), and `/etc/default/google-chrome` with `repo_add_once="false"`, so that Chrome's own setup adds no second, conflicting source; apt checks every package against the signed indexes of deb.debian.org and dl.google.com. `apt-get install` with recommends: "1 upgraded, 519 newly installed", 704 MB fetched, 4,459 MB on disk, no `W:` or `E:` line. No sound server, no new kernel and no initramfs rebuild came with it. The Lenovo fetched 87 MB from deb.debian.org in 2.3 s; `apt-get update` fetched 28.6 MB in 33 s, its many small files.
* Debian 13 has no `glmark2` or `glmark2-es2` package: its variants are `glmark2-{drm,wayland,x11}` and `glmark2-es2-{drm,wayland,x11}`, all installed. Sources are `main` only, so `i965-va-driver-shaders` (non-free, the shaders for H.264 encoding on i965) is not there.
* Then `apt-daily.timer` and `apt-daily-upgrade.timer` were disabled and stopped, so that no unattended upgrade changes Mesa or the kernel under a reference run. Seven packages stay upgradable (apt, apt-utils, libapt-pkg7.0, vim and its parts), none of them graphics.
* Installed versions (`versions.txt`; all 841 packages in `facts/packages-all.txt`):
  * Kernel 6.12.115-2 (`linux-image-6.12.115+deb13.1-amd64`); systemd 257.13-1~deb13u1; cloud-init 25.1.4-1+deb13u1.
  * Mesa 25.0.7-2+deb13u1 (`libgl1-mesa-dri`, `libegl-mesa0`, `libgbm1`, `libglx-mesa0`, `mesa-vulkan-drivers`, `mesa-va-drivers`): the plan's pin. `mesa-utils` 9.0.0-2+b2, `libdrm2` 2.4.124-2, `libvulkan1` 1.4.309.0-1, `vulkan-tools` 1.4.304.0+dfsg1-1.
  * glmark2 2023.01+dfsg-2 (six variants and `glmark2-data`); `piglit` 0.0~git20250409.af62c0dea-2; `intel-gpu-tools` 2.0-1; `kmscube` 0.0.0~git20210103-1+b2; `drm-info` 2.6.0-2; `libdrm-tests` 2.4.124-2; `edid-decode` 0.1~git20220315.cb74358c2896-1.1.
  * `weston` 14.0.2-1; `cage` 0.2.0-2 on `libwlroots-0.18` 0.18.2-3; `seatd` 0.9.1-1; `xwayland` 2:24.1.6-1; `wayland-utils` 1.2.0-2.
  * `chromium` 156.0.8078.79-1~deb13u1; `firefox-esr` 153.6.0esr-1~deb13u1; `google-chrome-stable` 157.0.8103.38-1.
  * `libva2` 2.22.0-3, `vainfo` 2.22.0+ds1-2, `i965-va-driver` 2.4.1+dfsg1-2, `intel-media-va-driver` 25.2.3+dfsg1-1; `ffmpeg` 7:7.1.5-0+deb13u1.
  * `qemu-system-x86` 1:10.0.13+ds-0+deb13u1; `libvirglrenderer1` and `virgl-server` 1.1.0-2.
  * `python3-websocket` 1.8.0-2; `curl` 8.14.1-2+deb13u7; `fio` 3.39-1; `strace` 6.13+ds-1; `pciutils` 1:3.13.0-2.

## The machine under Debian

From `facts.sh` at 20:34, into `/root/run2/setup-facts/` on Debian and `run2/lenovo-setup/facts/` on the Mac.

* `lspci -nnk`: 00:02.0 Haswell-ULT `8086:0a16` rev 0b, subsystem `17aa:380c`, i915; 01:00.0 RTL8111/8168 `10ec:8168` rev 10, r8169; 02:00.0 QCA9565 `168c:0036`, no driver bound (ath9k blacklisted).
* i915: "Found HASWELL/ULT (device ID 0a16) display version 7.00", "Initialized i915 1.6.0 ... on minor 0": the GPU is `card0` here (the first run's Alpine had simpledrm take minor 0), render node `renderD128`; `fbcon: i915drmfb (fb0) is primary device`.
* `eglinfo -B -p gbm`: EGL 1.5, "Mesa Intel(R) HD Graphics 4400 (HSW GT2)", OpenGL 4.6 core and compatibility, GLSL 4.60, OpenGL ES 3.2, all "Mesa 25.0.7-2+deb13u1".
* `vulkaninfo --summary`: GPU0 hasvk, apiVersion 1.2.305, driver 25.0.7, "MESA-INTEL: warning: Haswell Vulkan support is incomplete"; GPU1 llvmpipe (LLVM 19.1.7, 256 bits), apiVersion 1.4.305.
* `vainfo --display drm --device /dev/dri/renderD128`: iHD fails to initialise, i965 2.4.1 ("Intel i965 driver for Intel(R) Haswell Mobile") loads, VA-API 1.22: decoding MPEG-2, H.264 (constrained baseline, main, high, multiview high, stereo high), VC-1, JPEG; encoding MPEG-2 and H.264; video processing. The same list as the first run.
* `drm_info`: eDP connector, 1366x768 at 60.02 Hz, atomic, universal planes, modifiers, syncobj timelines (`facts/drm_info.txt`).
* DMAR (`facts/dmesg-dmar.txt`): two units (dmar0 at 0xfed90000, dmar1 at 0xfed91000), RMRRs 0xaae11000-0xaae30fff and 0xab800000-0xaf9fffff, interrupt remapping in x2apic mode, as the first run found. Unlike the first run's Alpine kernel, Debian's 6.12 turns DMA translation on by default: "iommu: Default domain type: Translated", 12 IOMMU groups (0 to 11), the GPU alone in group 0, and i915 says "VT-d active for gfx access" and "DMAR active, disabling use of stolen memory". The command line has no `intel_iommu=` option.
* `lsmod`: no ath9k and no snd module. `cfg80211` is loaded with no user and cannot find `regulatory.db`; `ath3k`, the Bluetooth driver for the card's USB half, is loaded.

## Rach2's root and Crysis

* p3 mounts read-only under Debian (`p3-ro-check.txt`): `mount /mnt/rach2` by the fstab line gives `/dev/sda3 ext4 ro,relatime`, the kernel logs "mounted filesystem 52616368-3200-4578-7432-000000000001 ro without journal", `touch` gives "Read-only file system", and p3's superblock (1024 bytes at 1024) has the same SHA-256 before the mount and after the unmount (`26345983...388b1b`). It was unmounted afterwards.
* Crysis is not on p3: `/opt` there holds agy, claude, codex, fd, gh, git-lfs, muse, ninja, node, opencode, ripgrep and shellcheck (1.8G), and `find -maxdepth 4 -iname '*crysis*'` finds nothing. Those directories are dated 09:30 today, the release's root update, which rewrites `/opt`; the game was not put back after it (`build/cache/crysis/README` says a root update needs `push-lenovo.sh`).
* So the game was copied into Debian's root instead, from the Mac's master copy: `tar --no-xattrs --no-mac-metadata` of `build/cache/crysis/{game,game.sha256,README}` piped over `dssh` into `/srv/crysis`, owned by root, 20:35:32 to 20:37:47; `sha256sum --quiet -c ../game.sha256` in `/srv/crysis/game`: "crysis: all 2358 files match" (`crysis-copy.out`). 8.1G; Debian's root then had 17G free.

## Leaving Debian running

* At 20:38 Debian runs with `systemctl is-system-running` "running", the backlight at 47, the clock synced. Nothing of this setup runs in it: the processes are systemd's, dbus, logind, polkit, getty on tty1, avahi-daemon (pulled in by a recommends), seatd (`seatd -g video`, the package's service, for the compositors) and the `unattended-upgrade-shutdown` waiter, which only finishes an upgrade at shutdown. p3 is not mounted.
* Kept in `/root/run2/` on Debian: `install.sh`, `install.log`, `facts.sh`, `setup-facts/`, `rearm.sh`.

## Files and sizes

* On the Mac, `build/cache/run2/lenovo-setup/` (2.6G on disk): `debian-13-generic-amd64.qcow2` (438,829,056), its `.json` and `SHA512SUMS`; `root-run2.ext4` (3,085,959,168, the image on p5) and its `.sha256`; `vmlinuz-6.12.115+deb13.1-amd64` (12,167,104), `initrd.img-6.12.115+deb13.1-amd64` (37,146,995) and `linux.mod` (28,328), the files on p1; p1 images of 67,108,864 bytes each: `p1-orig.img` (as found, `33829ae8...305193`), `p1-run2.img` (with the files, `0cc6fbe5...744ef1`), `p1-armed.img` (menu staged and trial armed, `1374671f...5f40bc`), `p1-debian.img` (under Debian at 20:37, the trial consumed, `0fb3269c...762ccd`).
* Removed: the rehearsal disk, the 3 GiB raw image, the unmodified root, the compressed root, `/dev/shm` copies on the Lenovo, the mount points made on Rach2.
* To repeat the rehearsal, `rehearse.sh` needs `build/cache/run2/lenovo-setup/rehearse/` rebuilt as above: the GPT's head and tail from the Lenovo, `p1-run2.img` through `rehearse-p1.py`, `root-run2.ext4` at sector 421287936, and `vars.fd` from Homebrew's `edk2-i386-vars.fd`.

## What it changes

* Step 9 cannot read Crysis from p3: the game is not there since the release's root update this morning. It is in Debian's root at `/srv/crysis/game`, checked file by file; use it as the overlay's lower layer.
* Debian's kernel runs with DMA translation on by default, where the first run's Alpine had it off, and i915 then gives up stolen memory. Step 9's reference numbers are taken with VT-d active for the GPU unless step 9 boots with `intel_iommu=off`; step 11 needs no `intel_iommu=on`, since the groups exist now (the GPU alone in group 0). Whether the reference should be taken with translation off is the chat's call; changing it takes one boot by `rearm.sh`.
* Rach2's ordinary kernel refuses raw writes to p5, so any rewrite of Debian's root goes through the recovery system and `reflash`, as above; p1 can be written from the running Rach2 through `/dev/bootpart`.
* While this run's block is in the menu slot, `bootctl trial recovery` boots Debian, not the recovery system. To reach the real recovery, restore p1 first.

## For step 9's agent

* Address: 192.168.0.106 (DHCP, the same lease Rach2 gets); link-local `fe80::xxxx:xxff:fexx:xxxx%en1` if IPv4 does not answer. SSH, from the repository root on the Mac, with Apple's `ssh`:

  `ssh -i krffs/rach2_demo_ed25519 -o UserKnownHostsFile=build/flow/coordinator/graphics-research/run2/lenovo-known-debian -o StrictHostKeyChecking=accept-new root@192.168.0.106`

  or `build/flow/coordinator/graphics-research/run2/lenovo-setup/dssh COMMAND`. The file `run2/lenovo-known` holds Rach2's host key and `run2/lenovo-known-recovery` the recovery system's; Debian's key is only in `lenovo-known-debian`.
* Users: root, by the development key only (no password logins). `gfx` (uid 1000), locked password, no SSH key, in `video`, `render`, `input` and `kvm`: run compositors and browsers as it from root, for example `runuser -u gfx -- env XDG_RUNTIME_DIR=/run/user/1000 ...` after `install -d -o gfx -g gfx -m 700 /run/user/1000`; `seatd` is running for `-g video`. No sound driver, no sound server.
* Where things are: the GPU is `/dev/dri/card0` and `renderD128`; Crysis in `/srv/crysis/game` (read-only use; overlay it); Rach2's root by `mount /mnt/rach2` (read-only, `noauto`), unmount it after; this setup's scripts and facts in `/root/run2/`; Debian's root has 17G free. Packages come from deb.debian.org (`main` only) and dl.google.com; apt's daily timers are off.
* The screen: tty1 shows a login prompt at backlight 47 of 937; the udev rule `/etc/udev/rules.d/90-run2-backlight.rules` sets 47 at each boot.
* Kernel options: change them only by `/root/run2/rearm.sh [--arm] 'EXTRA ARGS'` on Debian, which rewrites the menu slot in place with the extra arguments and reads it back (tested at 20:36 with no extra arguments, writing the same bytes), and with `--arm` sets `next_entry=recovery` in p1's grubenv (the edit was tested on a copy, not written). Then leave by `rtcwake -m off -s 120`: the next power-up boots Debian once more. Never reboot Linux warm.
* Leaving Linux for Rach2: with `next_entry` empty, which it is now, `rtcwake -m off -s 120` powers the machine off, and 120 s later the firmware starts GRUB, which boots Rach2's saved candidate. Rach2 answers at 192.168.0.106 with the host key in `run2/lenovo-known`.
* Restoring p1, once the Lenovo is back in Rach2 at the end of the Lenovo track: `sh build/flow/coordinator/graphics-research/run2/lenovo-setup/p1-restore.sh` on the Mac reads p1 through `/dev/bootpart`, writes back through `/dev/bootpart` the runs of sectors that differ from `p1-orig.img`, and prints `sha256sum /dev/bootpart` and `bootctl status`. The proof is `33829ae85d4283cd9076f03d8267727481e2f9b2caa7bdc776f52c4c91305193` for `/dev/bootpart`, and `bootctl status` giving `saved_entry: candidate`, `next_entry: empty (a trial was consumed)`, `booted: candidate`. The script has not been run; the same `dd` through `/dev/bootpart` wrote p1 at 20:24 and read back equal. p5 keeps Debian; only p1 is restored.
