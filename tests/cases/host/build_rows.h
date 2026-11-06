/* Fixed behavioral rows, independent of host paths and process adapters. */
#ifndef HOST_BUILD_ROWS_H
#define HOST_BUILD_ROWS_H
struct message_row {
    const char *name, *text;
    int status;
};
static const struct message_row message_rows[] = {
    {"plain", "Describe the three-partition disk in the demo Readme\n", 0},
    {"mention",
     "Refuse a Co-Authored-By trailer, a Claude-Session link, noreply@anthropic.com "
     "and the generated with Claude Code footer\n",
     0},
    {"body", "Move the sync detail to the tools Readme\n\nThe policy says what to do.\n", 1},
    {"period", "Move the sync detail to the tools Readme.\n", 1},
    {"past", "Moved the sync detail to the tools Readme\n", 1},
    {"gerund", "Moving the sync detail to the tools Readme\n", 1},
    {"third", "Moves the sync detail to the tools Readme\n", 1},
    {"lowercase", "move the sync detail to the tools Readme\n", 1},
    {"verbose",
     "Subject\n\n# ------------------------ >8 ------------------------\n"
     "# Do not modify or remove the line above.\n"
     "diff --git a/a b/a\n--- a/a\n+++ b/a\n@@ -1,2 +1,2 @@\n"
     " Co-Authored-By: Someone <someone@example.com>\n"
     "-Reviewed-by: Claude <noreply@anthropic.com>\n"
     "+* Generated with [Claude Code](https://claude.com/claude-code)\n",
     0},
    {"coauthor", "Subject\n\nCo-Authored-By: Someone <someone@example.com>\n", 1},
    {"session", "Subject\n\nClaude-Session: https://claude.ai/code/session_x\n", 1},
    {"signoff", "Subject\n\nSigned-off-by: Someone <someone@example.com>\n", 1},
    {"generated", "Subject\n\nGenerated with [Claude Code](https://claude.com/claude-code)\n", 1},
    {"robot",
     "Subject\n\n\360\237\244\226 Generated with [Claude Code](https://claude.com/claude-code)\n",
     1},
    {"noreply", "Subject\n\nReviewed-by: Claude <noreply@anthropic.com>\n", 1}};
struct run_row {
    const char *name, *settings, *script, *argument, *extra;
    const char *wanted, *absent;
    int status;
    const char *diagnostic;
};
/* $W is the canonical private tree; settings are separated by newlines. */
static const struct run_row run_rows[] = {
    {"run-default", "", "run.sh", "test", NULL,
     "-cpu\nmax\n-kernel\n$W/rach2/../build/rach2/kernel\n-initrd\n$W/rach2/../build/images/"
     "test.krffs\n"
     "-machine\npcspk-audiodev=audio0\n-netdev\nuser,id=net0,hostfwd=tcp::8080-:80\n"
     "-device\nrtl8139,netdev=net0",
     NULL, 0, NULL},
    {"run-cpu", "CPU=qemu64", "run.sh", "test", NULL, "-cpu\nqemu64", NULL, 0, NULL},
    {"run-sb16", "", "run.sh", "test", "sb16", "-device\nsb16,audiodev=audio0", NULL, 0, NULL},
    {"run-hda", "", "run.sh", "test", "hda",
     "-device\nintel-hda\n-device\nhda-duplex,audiodev=audio0", NULL, 0, NULL},
    {"run-nonet", "NET=off", "run.sh", "test", NULL, "-nic\nnone", "-netdev", 0, NULL},
    {"run-e1000", "NIC=e1000", "run.sh", "test", NULL, "-device\ne1000,netdev=net0", NULL, 0, NULL},
    {"run-ide", "DISK=$W/disk.img", "run.sh", "test", NULL,
     "file=$W/disk.img,format=raw,if=ide,index=0,media=disk\n-append\nrach2.fsdisk", NULL, 0, NULL},
    {"run-ahci", "DISK=$W/disk.img\nDISKBUS=ahci", "run.sh", "test", NULL,
     "-device\nich9-ahci,id=ahci0\n-append\nrach2.disk=ahci rach2.fsdisk", NULL, 0, NULL},
    {"run-nomount", "DISK=$W/disk.img\nDISKNOMOUNT=1", "run.sh", "test", NULL, "", "rach2.fsdisk",
     0, NULL},
    {"run-rescue", "RESCUE=1", "run.sh", "test", NULL, "-append\nrach2.rescue", NULL, 0, NULL},
    {"run-nodisplay", "", "run.sh", "test", NULL, "-display\nnone", NULL, 0, NULL},
    {"run-x11", "DISPLAY=:0", "run.sh", "test", NULL, "", "-display", 0, NULL},
    {"run-wayland", "WAYLAND_DISPLAY=wayland-0", "run.sh", "test", NULL, "", "-display", 0, NULL},
    {"run-macos", "UNAME_S=Darwin", "run.sh", "test", NULL, "", "-display", 0, NULL},
    {"run-display-mode", "DISPLAY=:0\nDISPLAY_MODE=sdl", "run.sh", "test", NULL, "-display\nsdl",
     NULL, 0, NULL},
    {"run-missing", "", "run.sh", "os-simple", NULL, NULL, NULL, 1,
     "build it with 'make os-simple-image'"},
    {"run-x86_64", "ARCH=x86_64", "run.sh", "test", NULL,
     "qemu-system-x86_64\n-kernel\n$W/rach2/../build/x86_64/rach2/kernel\n-initrd\n"
     "$W/rach2/../build/x86_64/images/test.krffs",
     NULL, 0, NULL},
    {"run-x86_64-missing", "ARCH=x86_64", "run.sh", "os-simple", NULL, NULL, NULL, 1,
     "build it with 'make ARCH=x86_64 os-simple-image'"},
    {"run-arch", "ARCH=arm", "run.sh", "test", NULL, NULL, NULL, 2,
     "ARCH is i686 or x86_64, not arm"},
    {"debug-rescue", "", "debug.sh", NULL, NULL,
     "-s\n-S\n-cpu\nmax\n-kernel\n$W/rach2/../build/rach2/kernel\n-append\nrach2.rescue", NULL, 0,
     NULL},
    {"debug-image", "", "debug.sh", "test", NULL, "-initrd\n$W/rach2/../build/images/test.krffs",
     "rach2.rescue", 0, NULL},
    {"debug-missing", "", "debug.sh", "os-simple", NULL, NULL, NULL, 1,
     "build it with 'make os-simple-image'"},
    {"disk-default", "", "run_from_disk.sh", NULL, NULL,
     "-cpu\nmax\nfile=$W/rach2/../build/images/"
     "os-real-grub.disk,format=raw,if=ide,index=0,media=disk\n"
     "-netdev\nuser,id=net0,hostfwd=tcp::2222-:22",
     NULL, 0, NULL},
    {"disk-cpu", "CPU=qemu64", "run_from_disk.sh", NULL, NULL, "-cpu\nqemu64", NULL, 0, NULL},
    {"disk-nodisplay", "", "run_from_disk.sh", NULL, NULL, "-display\nnone", NULL, 0, NULL},
    {"iso-default", "", "run_from_iso.sh", "test", NULL,
     "-cpu\nmax\n-cdrom\n$W/rach2/../build/images/test.iso", NULL, 0, NULL},
    {"iso-cpu", "CPU=qemu64", "run_from_iso.sh", "test", NULL, "-cpu\nqemu64", NULL, 0, NULL},
    {"iso-nodisplay", "", "run_from_iso.sh", "test", NULL, "-display\nnone", NULL, 0, NULL},
    {"iso-usage", "", "run_from_iso.sh", NULL, NULL, NULL, NULL, 2, "usage: ./run_from_iso.sh"}};
#endif
