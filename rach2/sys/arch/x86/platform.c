/*
 * platform.c: the PC's part of ../../platform.h, the order the boot brings
 * the machine's own devices up in kept by kernel.c and carried out here.
 */

#include "apicdiag.h"
#include "board.h"
#include "diag.h"
#include "ps2kbd.h"
#include "lapic.h"
#include "ps2mouse.h"
#include "pit.h"

#include "../arch.h"
#if defined(__x86_64__)
#include "../x86_64/percpu.h"
#else
#include "../i686/gdt.h"
#include "../i686/idt.h"
#include "../i686/tss.h"
#endif

#include "../../audio/sb16.h"
#include "../../block/ata.h"
#include "../../cmdline.h"
#include "../../input.h"
#include "../../klog.h"
#include "../../platform.h"
#include "../../timer.h"
#include "../../udelay.h"

void platform_cpu_init(uintptr_t boot_stack_top)
{
    init_gdt();
    klog(KLOG_DEBUG, "boot", "gdt: ready");

#if defined(__x86_64__)
    init_tss(boot_stack_top);
#else
    init_tss((uint32_t)boot_stack_top);
#endif
    klog(KLOG_DEBUG, "boot", "tss: ready");

    init_idt();
    klog(KLOG_DEBUG, "boot", "idt: ready");

    init_isr();
    klog(KLOG_DEBUG, "boot", "isr: ready");
}

void platform_report(void)
{
    /* The boot report, after the page cache's window is mapped and before
     * the disk and network drivers have configured a device. After, because
     * the window's page tables and the report's own device peek take
     * memory, and a figure printed before those is not what the machine has
     * left. */
    if (kcmdline_has("rach2.diag")) {
        boot_diag_report();
    }

    /* What the machine's firmware left in the APIC and in the two devices
     * this kernel drives by message, read and printed, nothing written. */
    apic_diag_report();
}

void platform_report_hold(void)
{
    boot_diag_hold();
}

void platform_timer_start(isr_handler_t tick)
{
    arch_irq_disable();
    /* The local APIC, so that a device can be told to send its interrupts
     * as messages. A machine that refuses leaves every device on its legacy
     * line, which is the kernel as it was. Interrupts are off across it: it
     * moves the road the 8259's interrupts take to the processor, and there
     * is no reason to have one arrive in the middle of the move. */
    lapic_init();
    init_pit(100);
    set_isr_handler(32, tick, "pit"); /* install the timer handler on IRQ0 */
    arch_irq_enable();

    /*
     * The 8259's interrupts now reach the processor through the APIC's
     * `LINT0`, so the first thing the timer is asked is whether it still
     * arrives. A masked entry would otherwise show as a machine that boots
     * and never ticks, with nothing said about why. The wait is measured
     * against the 8254's own counter, which needs no interrupt.
     */
    if (lapic_ready()) {
        uint32_t before = kernel_ticks;
        int ms;

        for (ms = 0; ms < 50 && kernel_ticks == before; ms++) {
            k_udelay(1000);
        }
        if (kernel_ticks == before) {
            klog(KLOG_ERR, "lapic", "timer: no tick in 50 ms; writing lint0 again");
            lapic_repair_lint0();
            for (ms = 0; ms < 50 && kernel_ticks == before; ms++) {
                k_udelay(1000);
            }
            if (kernel_ticks == before) {
                klog(KLOG_ERR, "lapic",
                     "timer: still no tick; the 8259's interrupts are not arriving");
            }
        }
    }
}

uint32_t platform_timer_hz(void)
{
    return 1193182u / pit_reload;
}

void platform_board_init(void)
{
    /* Which power controller this machine has. With ACPI on, an unhandled
     * SCI is level-triggered and would be taken forever, so the line it sits
     * on is masked here. A machine no profile matches keeps exactly the
     * behaviour it had before this existed. */
    const char *why = "";
    int verdict = board_init(&why);

    if (verdict == BOARD_FOUND) {
        klog(KLOG_INFO, "board", "ready: %s", board_name());
    } else if (verdict == BOARD_REFUSED) {
        klog(KLOG_WARN, "board", "refused: %s", why);
    } else {
        klog(KLOG_WARN, "board", "none: no profile matched; poweroff will halt");
    }
}

void platform_keyboard_init(void)
{
    kbd_init();
}

void platform_mouse_init(void)
{
    if (kcmdline_has("rach2.nomouse")) {
        klog(KLOG_INFO, "mouse", "skipped: rach2.nomouse");
        return;
    }
    klog(KLOG_DEBUG, "mouse", "started");
    if (init_mouse() == 0) {
        input_mouse_found();
        klog(KLOG_DEBUG, "mouse", "ready");
    } else {
        klog(KLOG_WARN, "mouse", "absent: nothing answered the probe");
    }
}

void platform_tick(void)
{
    ata_tick();
}

void platform_disks_probe(void)
{
    (void)ata_init();
}

struct audio_driver *platform_audio_driver(const char **name)
{
    *name = "Sound Blaster 16";
    return &sb16_driver;
}
