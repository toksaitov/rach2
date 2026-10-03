#ifndef _APICDIAG_H_
#define _APICDIAG_H_

/*
 * apicdiag: the `rach2.apicdiag` report. It reads the processor's APIC
 * feature bits, `IA32_APIC_BASE`, the APIC page's identity, version, task
 * priority, spurious vector and the two local-interrupt entries, and the
 * MSI and MSI-X capabilities of the xHCI and HDA controllers, and changes
 * nothing. Without the boot flag it does nothing at all.
 */
void apic_diag_report(void);

#endif /* _APICDIAG_H_ */
