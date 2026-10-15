# sys/arch/x86/sources.mk: the PC platform's files, which i686 and x86-64
# share: the boot record, the processor's features, the interrupt
# controllers, the timers, the firmware's tables, PCI configuration, the
# serial port and the PS/2 devices; and the files outside arch/ that only a
# PC build compiles: ATA, VGA, the framebuffer console, and the Sound
# Blaster 16 with its DMA controller.

PC_C_SRC = sys/arch/x86/boot.c \
    sys/arch/x86/mbinfo.c \
    sys/arch/x86/portio.c \
    sys/arch/x86/cpu.c \
    sys/arch/x86/cpuinfo.c \
    sys/arch/x86/debugreg.c \
    sys/arch/x86/serial.c \
    sys/arch/x86/module.c \
    sys/arch/x86/isr.c \
    sys/arch/x86/pic.c \
    sys/arch/x86/board.c \
    sys/arch/x86/smbios.c \
    sys/arch/x86/pit.c \
    sys/arch/x86/cmos.c \
    sys/arch/x86/ps2mouse.c \
    sys/arch/x86/ps2kbd.c \
    sys/arch/x86/platform.c \
    sys/arch/x86/speaker.c \
    sys/arch/x86/pciconf.c \
    sys/arch/x86/power.c \
    sys/arch/x86/apicdiag.c \
    sys/arch/x86/lapic.c \
    sys/arch/x86/acpi.c \
    sys/arch/x86/ioapic.c \
    sys/arch/x86/refclock.c \
    sys/arch/x86/apictimer.c \
    sys/arch/x86/smpboot.c \
    sys/arch/x86/diag.c \
    sys/arch/x86/udelay.c \
    sys/arch/x86/fpu.c \
    sys/arch/x86/kirq.c \
    sys/arch/x86/kcpu.c \
    sys/arch/x86/pat.c \
    sys/block/ata.c \
    sys/display/vga_text.c \
    sys/display/vga_gfx.c \
    sys/display/vga_font.c \
    sys/display/fbcon.c \
    sys/audio/dma.c \
    sys/audio/sb16.c
