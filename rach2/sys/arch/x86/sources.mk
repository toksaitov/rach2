# sys/arch/x86/sources.mk: the PC platform's files, which i686 and x86-64
# share: the interrupt controllers, the timers, the firmware's tables, PCI
# configuration, the serial port, the PS/2 devices, and the drivers of the
# devices only a PC has, wherever their subsystem keeps them.

PC_C_SRC = sys/arch/x86/serial.c \
    sys/arch/x86/module.c \
    sys/arch/x86/isr.c \
    sys/arch/x86/pic.c \
    sys/arch/x86/board.c \
    sys/arch/x86/smbios.c \
    sys/arch/x86/pit.c \
    sys/arch/x86/rtc.c \
    sys/arch/x86/kbd.c \
    sys/arch/x86/mouse.c \
    sys/arch/x86/speaker.c \
    sys/arch/x86/pci.c \
    sys/arch/x86/power.c \
    sys/arch/x86/apicdiag.c \
    sys/arch/x86/lapic.c \
    sys/arch/x86/diag.c \
    sys/arch/x86/udelay.c \
    sys/arch/x86/fpu.c \
    sys/arch/x86/kirq.c \
    sys/arch/x86/kcpu.c \
    sys/block/ata.c \
    sys/display/vga_text.c \
    sys/display/vga_gfx.c \
    sys/display/vga_font.c \
    sys/audio/dma.c \
    sys/audio/sb16.c \
    sys/network/driver/rtl8139.c
