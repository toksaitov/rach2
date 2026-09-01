Phase 3 done; phase 4 (HDA on MSI) is next.
Evidence gathered: the xHCI takes its interrupts as messages on vector 48; ten of them for five keys typed, with the timer's pump finding none of their work; `rach2.nomsi` and a machine with no MSI capability keep the timer as the only pump; `rach2.msidrop=3` silences the controller and the machine goes on typing with the fallback count rising; the handler enters within a millisecond of its own event and the reader runs on the wake rather than at the next tick.
Next action: HDA's MSI on vector 49 beside its legacy line and its timer servicing, the audio suite's count, and metal_accept.sh's lines for milestone 43.
File being read when written: dev-log/39-msi/plan.md, phase 4.
