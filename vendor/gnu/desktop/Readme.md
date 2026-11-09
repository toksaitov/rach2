# Desktop session

The GNU root starts a Weston kiosk session as `desktop`, uid 1001, with seatd, Debian's udev daemon and PulseAudio. `init-metal` supervises `rach2-desktop`; the script starts the device services before the user session. Its output goes to `/var/log/services`.

## Use

The account has a locked password and belongs to `seat`, `video`, `render`, `input` and `audio`. Root can enter it with `su -s /bin/bash desktop`. Its private runtime directory is `/run/user/1001`, created at boot with mode 0700. Set `XDG_RUNTIME_DIR=/run/user/1001`, `WAYLAND_DISPLAY=wayland-0` and `PULSE_SERVER=unix:/run/user/1001/pulse/native` when starting a program there from another session.

`/etc/xdg/weston/weston.ini` selects kiosk-shell, Xwayland and no idle blanking. The session chooses headless with Pixman when no DRM connector is connected. A connected display uses DRM with Pixman, or GL when a render node exists. Stop the desktop supervisor to restart the device services and session; init starts it again. Each supervisor ends and reaps its children on exit.

## Design

`rach2-desktop` starts systemd-udevd outside systemd, waits for its control socket to answer, triggers existing devices and settles their events. It starts seatd with `SEATD_VTBOUND=0` because Rach2 has no VT interface. The seat socket belongs to the `seat` group. The session starts only after that socket exists, under the desktop account's supplementary groups and a private D-Bus session bus.

`rach2-session` runs PulseAudio and Weston in the foreground. `/etc/pulse/rach2.pa` loads `module-oss` over `/dev/dsp` without mmap and exposes the user's native PulseAudio socket. A machine without sound can still start the session. `/etc/asound.conf` gives ALSA clients a `plug` conversion over the OSS plugin. Both routes use the kernel's mixer; the ALSA default opens OSS directly. Programs that use PulseAudio find its socket through `PULSE_SERVER`.

## Building

`install_root.sh` installs the scripts and configuration after the Debian packages and locks the account the manifest supplies. The root image depends on every file here. `make ARCH=x86_64 os-real-gnu-ext2` rebuilds the image after an edit.

## Tests

The `desktop` family boots the GNU root with `rach2.audio.loopback`. Its headless case reads the compositor's uid, the seat socket, the input properties and the browser and ALSA tones recorded through OSS. It leaves the normal browser sandbox enabled.
