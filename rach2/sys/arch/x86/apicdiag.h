#ifndef _APICDIAG_H_
#define _APICDIAG_H_

/* apicdiag: the `rach2.apicdiag` boot report of the local APIC and of the
 * message capabilities of the xHCI and HDA controllers. */

/**
 * Log, when the command line has `rach2.apicdiag`, the processor's APIC
 * feature bits, `IA32_APIC_BASE`, the APIC page's identity, version, task
 * priority, spurious vector and two local-interrupt entries, and the MSI
 * and MSI-X capabilities of the xHCI and HDA controllers. It maps the APIC
 * page and writes no register; without the word it does nothing. The boot
 * calls it once, after paging is on and before the local APIC is enabled.
 */
void apic_diag_report(void);

#endif /* _APICDIAG_H_ */
