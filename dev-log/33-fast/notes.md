# Milestone 33 notes

Deviations from `plan.md`, measurements, and defects found but not fixed
in the phase that found them.

## The baseline

The last complete run of the current tree, 4877s (81.3 min) across 44
suites. Adopted rather than re-run: the only commits between it and the
start of this milestone are this plan, so a fresh run would have spent 81
minutes reproducing a number already in hand. Per suite, slowest first:

* e1000          915s
* account        436s
* sealed         327s
* dhcp           316s
* selfbuild      227s
* menu           160s
* procfiles      152s
* install        152s
* drill          146s
* raw            144s
* netif          139s
* stress-ahci    135s
* stress         135s
* tcc            115s
* bootpart       95s
* default        94s
* addressed      94s
* noentropy      91s
* shutdown       86s
* make           78s
* packet         66s
* ping           65s
* cred-persist   60s
* exitline       59s
* login          54s
* bootline       54s
* authority      49s
* persist-ahci   48s
* persist        47s
* subset         42s
* host           39s
* dgram          36s
* ssh            29s
* disk-q35       24s
* disk-ahci      24s
* disk           24s
* style          22s
* grub           22s
* diskdiag       20s
* diskpolicy     17s
* crash          13s
* netconsole     10s
* metal          10s
* reset          6s
