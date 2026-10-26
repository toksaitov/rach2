# Windows affected collection results

The native Windows retry on source `03af9b116cc817309bd0289b70c8fcc2f05c7852`, run `tests.i686-x86_64.20261025T211814Z.23708`, allocated and attempted 108 case/architecture pairs under WHPX. It contains every pair of the original 99-pair selection, plus `host.host.etude_quiet` and the four added script partitions on both architectures. The original selection was expanded against its own 0da source manifests, treating `architectures: "none"` as one host case. The comparison found no missing pair.

The outcomes are 66 passes, 27 failures and 15 incomplete attempts. Every row retains its original outcome. The [WHPX report](qemu-whpx-report.md#public-collection-acceptance) accepts the XSAVE and halt repair from the 23 relevant passing pairs; this collection as a whole failed. Follow-up numbers identify the faults in the retained failure analysis. A row with no narrower diagnosis remains with the whole Windows collection, #681; it is not assigned a cause by this record. A later source repair does not change an old result.

The history is SHA-256 `2dc5c370cfbea1bec384598a911d3359ac9460ce1f9bdff78e9a09f1ef1c73cc`, 607,558 bytes. Its selected-case bytes are SHA-256 `b396f31e0d1f3ecb27449f5960ba36694957f3639a658a9b4cbeb4268694b6e3`, 12,184 bytes. The prepared record passed to this run is SHA-256 `7396cdb78356d54d1c6152d510bc480ac42814ff13859279a29936e3072a72a2`, 100,276 bytes. The run used 16 workers, 32 GiB of guest memory and two guest processors. It is an unranked diagnostic, with no preparation or cache-conditioning measurement claimed.

Raw history, allocation, input readback, cleanup and failure mapping are retained in the runner worktree under `build/tests/windows-targeted-diagnostic-03af9b116-03/operator/terminal`. Each report digest below is the original `assertion_results` object of that attempt, under the run's `store/`. The 23 reports used to accept XSAVE and halt were reread from that store and verified against their history digests. Their reports are complete, have no errors and pass every declared and executor assertion.

| Case | Architecture | Outcome | Assertion report SHA-256 | Follow-up issues |
| --- | --- | --- | --- | --- |
| `guest.default.clockmusl` | i686 | incomplete | `d6a6900f89edf890ff2b6c6f6169f77c217fe5219a4a06db7daa79ff552c36ff` | #1181 |
| `guest.default.clockmusl` | x86_64 | incomplete | `33e9b482c0e0cd0d3f20afa61485dd52b88baa61fea7fae313c0c43c9fc4561d` | #1181 |
| `guest.default.tlsprobe` | i686 | incomplete | `b70fb2c2ab1527f974635727189459aefad4dde9ec1beb7524c8f4cb06e6a13c` | #681 |
| `guest.default.xsaveprobe` | i686 | pass | `ab1446c38e8cbeb64b4ed1bd1114805e25e01a3a434bd1414ae515e129484319` | - |
| `guest.default.xsaveprobe` | x86_64 | pass | `1366765b34e8d8642697278f28be5572a3eb559e87aa32865f5474dcab6b25af` | - |
| `host.codexapi.exchange` | x86_64 | incomplete | `cd9d7de3e78a8ad21c4502948f01317cfa57a720e69db6c7088704e9d341e3bc` | #681 |
| `host.codexapi.refusal` | x86_64 | incomplete | `ef0cb6972508927c91b7e110a03203bf86cae61fb7bcbf92757b8a6b3b1c7007` | #681 |
| `host.host.boots` | i686 | failure | `cfa65fbe05c6dd2a1c269aec7a3b14c2fd55ccb85c3b0f7652d1ae1c60dd10f4` | #1170, #1171, #1172, #1173, #1174, #1175, #1178, #1179, #1147 |
| `host.host.boots` | x86_64 | failure | `4ebf6cd62eac914602d7860f4b202d7f8426967ccd8ab94b628d4cef28f2cafe` | #1170, #1171, #1172, #1173, #1174, #1175, #1178, #1179, #1147 |
| `host.host.etude` | none | failure | `730b55a3d5e87d1b118a63e2f6142b7d04a0a7d65254dce2716494bf3e1dbae6` | #1169 |
| `host.host.etude_quiet` | none | failure | `570a8b4a7122caf5cb18b850fda8d3075f6204177b17e23e03ad03bf6360c282` | #1169 |
| `host.host.scripts` | i686 | pass | `1d3de2b5b5ae1d44999b531d16e73aa17afa075990e1e1db7b94f7fa8fbfb081` | - |
| `host.host.scripts` | x86_64 | pass | `d2d0a4ea7fbc049b48e9bae5c889c7b3db0e579c598be098ccb919218768bbb2` | - |
| `host.host.scripts_build` | i686 | pass | `724ec070c837ecdbae4c7b3c8376ad636232a7cfbe5b20138990e135aaf43d55` | - |
| `host.host.scripts_build` | x86_64 | pass | `e5e33badb41c780538e2c1674d6803c541b849678e4aa6f78bc8e1987d6d0242` | - |
| `host.host.scripts_images` | i686 | pass | `7aec2a940cc29bbee042f284e9d3856aff7a6e8ab23acc36be873d9381b9e763` | - |
| `host.host.scripts_images` | x86_64 | pass | `61185e1abe6998cdebfa8294c49ebd34dd40bb2754357bf431d4d799a1a65212` | - |
| `host.host.scripts_native` | i686 | failure | `ba977d2aa9ef63e545e917be1dfe70202f8ab9fb1b19d89b9c6f67f5551d8c7a` | #1180 |
| `host.host.scripts_native` | x86_64 | failure | `402b85f12d2378de37ee628f04a085544c545f45ce1b03accfbfbf552e644289` | #1180 |
| `host.host.scripts_tools` | i686 | pass | `7427bd196b8267e816f1ac24833f88ece4de3d5f4b8731cf7c4f17bd09f48bfa` | - |
| `host.host.scripts_tools` | x86_64 | pass | `861c3d4c96edd6593b9d1488247fa610b9829007df9f547d92148b611a2a1e07` | - |
| `host.host.tests` | i686 | pass | `4d7ae23aea1c5d565b6bb4ffaedf89885acece3304e890059ed48fc6c66fc394` | - |
| `host.host.tests` | x86_64 | failure | `9e079171fcf9592726d8209b72a048d959d04e1595f2836296d4b74e5e81d076` | #1183, #1184 |
| `host.imagerace.prerequisites` | i686 | failure | `dca0f7a2ba848dff1fe30a1e6539b56eac68800aa81221f594f549e542a03598` | #1189 |
| `host.imagerace.prerequisites` | x86_64 | failure | `9f3815f0516fec7a5fc032acf1dcf68f1c4979284daee93d5e32ed4e4424697c` | #1189 |
| `host.krffscheck.dirdup` | none | pass | `a29a382bbdaea6df079086296d4af9812873b02fa533e987b91c56de0078f2b7` | - |
| `host.krffscheck.fatal` | none | pass | `47d82cde59e45c0b3d4beabe5876f75d38cf7a80f0b34869dec87c3e310be1be` | - |
| `host.krffscheck.orders` | none | pass | `cde0b60b80c5d6972ea66a6a9659e995036a550b3a89b2f2617814ff661eb148` | - |
| `host.krffscheck.prefixes` | none | pass | `2814aaf8a7c9bc6f6f931e6984a1fed89d892931b2765559b10cc981e92eb7b3` | - |
| `host.krffscheck.repairable` | none | pass | `43ab45219dfa74c6be6bf327231e86b4a9eeb1e956f24061bb96181c5c76b03a` | - |
| `host.krffscheck.sizes` | none | pass | `98dca695104e98c2320f2bcb625369fba9149d9394c55066b37bb69e92afa265` | - |
| `host.museapi.exchange` | x86_64 | failure | `05d76fe4ca728eaabd5e59eba6d0c685f2e6f9d1b6b592e4d8f1fffad323c118` | #1147 |
| `host.museapi.refusal` | x86_64 | failure | `231903a3bcf7dfe2bcf53c094c3981aec1410702385ae454858958fc1673584b` | #1194 |
| `machine.agents.agy` | x86_64 | failure | `74ddfafd31a0c03628c05ad9d88e06a28ff106c2058f8f80e9a18eee9b36e630` | #1193 |
| `machine.agents.agy-print` | x86_64 | failure | `cd3b65477308b84313592b54633990ec52c8498f2658d8391578b04b47d9271a` | #1147 |
| `machine.agents.claude` | x86_64 | failure | `41e2d4fd5fe4790c0b65abddbe6e006624a18278b33e497b9539999f5f57576a` | #1193 |
| `machine.agents.claude-print` | x86_64 | failure | `20132a9d20d39b557625991f37331b8c744df555f6702839f56bc44bfcb50045` | #1147 |
| `machine.agents.codex` | x86_64 | failure | `87e73e2b0999294274f633a3c1198b33465636da3b310a0a05053755df57f4ba` | #1193 |
| `machine.agents.codex-exec` | x86_64 | failure | `f4162ce84098c50b5f965a2fdfb1516bab1446c09e788e652589963422c54463` | #1147 |
| `machine.agents.gh` | x86_64 | failure | `9b04166161a24d3d23776ef3cbffd18dcdb9939abdd70617af90b62b5c64bbe2` | #1147 |
| `machine.agents.muse` | x86_64 | failure | `bd448ef9bf61ebff92b0cdbe9a233bef6ea169b7044315704bfa88fcea6550c4` | #1147 |
| `machine.agents.worker-session` | x86_64 | incomplete | `d4560d073cac34c87ec91fbc94c46e094ce4cd4dcde1c25ca74aee34dfa5529f` | #681 |
| `machine.agentsaccept.agy` | x86_64 | incomplete | `2335e91ea13c14e5f403e01a609e12989bd227a3e04c54f9cd618417d041127c` | #681 |
| `machine.agentsaccept.claude` | x86_64 | incomplete | `7758261ee559c66b135f92c9fde20d779f6ed3517a93bd289774272dc7ec5a16` | #1194 |
| `machine.agentsaccept.codex` | x86_64 | incomplete | `e0cb6c43c17b25b117966cca122c8a6a826f218953cdd327ab88e5f756c43ca2` | #681 |
| `machine.agentsaccept.muse` | x86_64 | incomplete | `2034958db604479f1dc33b5d3b2910a26abef6f838b0e2fd89d5030549597531` | #1194 |
| `machine.bigmem.boundary-73728` | x86_64 | incomplete | `cb6e061a2ff962f87667e39cee5323935964e576392811c3b20843ec47203a24` | #1190 |
| `machine.bootline.blank` | i686 | pass | `4c9f3e70feb4a68e9c2b3ada0740f9eeed4460bc86845ab50fc05548162744ef` | - |
| `machine.bootline.blank` | x86_64 | pass | `efcfdf6bbcf0a6ff5e859d7952778e2eaf971ba32606942e55059f7e50e71e34` | - |
| `machine.bootline.cpu-486` | i686 | pass | `b69c46317b7e4602819a40686052aabc173f4dc70e4d9dc07464de12dfa860de` | - |
| `machine.bootline.cpu-pentium` | i686 | pass | `f8f39a93eb9a608a98fefaeff77bcfcec85b27b533347dab3f00accfcd5bea5e` | - |
| `machine.bootline.cpu-qemu32` | x86_64 | pass | `4e7ac4e865b7fcc39d07fc70d390c9304734e44f4b8ba277ede04c251c65b456` | - |
| `machine.bootline.cut` | i686 | pass | `523e5a2e4a76e46d2ac9b4e54c03f5c08b717a63b3035cb9de8c6a16447a7424` | - |
| `machine.bootline.cut` | x86_64 | pass | `464c5953c667d1d89524bd8af68b9c5b864b8b967624dd361c5ab889cb72f87b` | - |
| `machine.bootline.init-loop` | i686 | pass | `0db63bcbb7f3c3a3543d80f0591f32a794013e15792b89c687c634a225ba3aa4` | - |
| `machine.bootline.init-loop` | x86_64 | pass | `2599178491d16e2bfdad361c0373cdca39e69e3ba58a169a3b7aeb10d3570854` | - |
| `machine.bootline.nodisk-root` | i686 | pass | `ae989a549aa4b800d8df3fbfa10ad6788e9f5d1122c383b871dda4ef37ea1eef` | - |
| `machine.bootline.nodisk-root` | x86_64 | pass | `1f0facf56621d985c62837cc1c8b1389dd9c45bb37eb821cd49a101671d3e8b5` | - |
| `machine.clock.clockload` | x86_64 | failure | `55f7d306e5875524294d7ba3a67fc9ebe40b22aa6857e6460041dd2e58999db8` | #1191 |
| `machine.ext2.repair` | i686 | pass | `2f6cedec6afd1c4b50c7f5f2b4be4e92113e3c4d6c36ce1aa148be37f906bdcf` | - |
| `machine.ext2.repair` | x86_64 | pass | `eaba11816d05d6057a7d2493a6ce29a8d56e6de00168d2ad448ee2f597949221` | - |
| `machine.fakeapi.curl` | x86_64 | failure | `2d05e7dda8d61eb5550fd796efaf6a008de4cfdf7154cf57c653f67e86d0f64c` | #1147 |
| `machine.gnu.logins` | i686 | pass | `7129000373e7121f02f4cfd4ab2879ca8e2840fb290db72b44a950787cfc591e` | - |
| `machine.gnu.logins` | x86_64 | pass | `f39c03e124c4d05b87c9b4dae7c734b12502d37ede53e1e55d1acfa9c1ceea9f` | - |
| `machine.gnu.ntpd` | i686 | pass | `60dca3bfded9343922ea0510c32214f885ed2df784b087cb862c38a2ed209785` | - |
| `machine.gnu.ntpd` | x86_64 | pass | `f4d7187b5e1876e03ead3e1c55389df6da867b89f32aa37f090980b1af523d9c` | - |
| `machine.grow.interrupted-gpt` | i686 | pass | `857407b4d85ca73eb555c777aa86d62584cb3237dda6731cd863716214819f50` | - |
| `machine.grow.interrupted-gpt` | x86_64 | pass | `b916e2a604335bc8df4117a4a20efb9732727d2035252533dd54f65909695a8f` | - |
| `machine.grow.interrupted-mbr` | i686 | pass | `88a925f46adf4259e97982c7123e0ca391857289f917cf6a36230b259195a8f8` | - |
| `machine.grow.interrupted-mbr` | x86_64 | pass | `1442d80373c1e54881a6ebe655615f3b41bc568430be970f7e429c19e6ae7a06` | - |
| `machine.grow.machine-gpt` | i686 | pass | `6633365021c7f8a164164e723a1a96013f41cbc1df0780eab0aba932be63efe1` | - |
| `machine.grow.machine-gpt` | x86_64 | pass | `66d34d5249e468ee3da14509e510421ab1eac409922d77c93abeab6107023032` | - |
| `machine.grow.machine-mbr` | i686 | pass | `6fc66c83ea670424b8c794febee2fdbd1ccf382fda95687d587062ed03e8c8f2` | - |
| `machine.grow.machine-mbr` | x86_64 | pass | `0576bbd9ce901ab5cf5e68a5713ad356a85f101c0284e82801ab33af650f3f98` | - |
| `machine.grow.ram-full` | i686 | pass | `48e94a9bb74705f95ac1a6cc15fc12b9371d83a29cad568739418b3acfa8a254` | - |
| `machine.grow.ram-full` | x86_64 | pass | `1f3c5a3276df14b425f0a0f0364af3b2cfaa3818171c101f525d44bfab76b941` | - |
| `machine.installer.stick-update` | i686 | pass | `144038ffe53f48b8031fd5a67d2b630a2c1ccae52902566f3a5b773b79f6471a` | - |
| `machine.installer.stick-update` | x86_64 | pass | `b46d25708a4cbe8317ce10b0d6320e41ab0077d0757129af3311f18c60637a98` | - |
| `machine.loopdev.krffs` | i686 | pass | `9214e5b813c7d14a319b0f39e1eef3f5e3cc90096a2bc7cccf522a03644802ca` | - |
| `machine.loopdev.krffs` | x86_64 | pass | `a502efa669e2a53886b647830820d920c266cefea33f245003bea8dd5a4ecd44` | - |
| `machine.loopdev.lag` | i686 | pass | `ae855bd49b915e1757c59d2937da415aa49a9206a86d68d74cafc1aa8869b214` | - |
| `machine.loopdev.lag` | x86_64 | pass | `d478b613ca6ce9b2efd2838176b8476f993d23a351a59776aad0307c0b2a6140` | - |
| `machine.loopdev.vfat` | i686 | pass | `0c9f90d63bda871b66ec48702c48be217076c9184bb59f5caa3e841166e45ff4` | - |
| `machine.loopdev.vfat` | x86_64 | pass | `3bfdb9d693541a6e9b2c962752a7a22b2b82a321392bfbf0d96ffbdebf68fe3e` | - |
| `machine.metal.unattended` | i686 | failure | `81281562a94b1add115d031158e722641c207b7251cbc25effa2bc50e189bd92` | #1151, #1195 |
| `machine.metal.unattended` | x86_64 | failure | `13c7dc66eff3616f5ebfccef8ec63fb450d8d812a2b5e0eaea5faba6ebf970d1` | #1151, #1195 |
| `machine.procfiles.applets` | i686 | pass | `40de9e34a8858b310c363576ac2a0a5835b1c500e437c8be16012ed0effb15d5` | - |
| `machine.procfiles.applets` | x86_64 | pass | `f4eee2ecbe7c07a968481e8fba6eb3c97ae335470c5070e7e8e8769a3589a8db` | - |
| `machine.selfbuild-ext2.kernel` | i686 | incomplete | `c7b74fbc19d41505f9aa17d970783f2501fa50733d351d77e677990fddc4663d` | #1181 |
| `machine.selfbuild-ext2.kernel` | x86_64 | incomplete | `f630b0958014f994e171f023addbf37b0664c2adf56ec3a14023dc91ddaa8f73` | #1181 |
| `machine.selfbuild.kernel` | i686 | incomplete | `fc8cc87c2aef1ba8fa3dff7118a07746a66b98621e78578bfb39ed1bed2aaca9` | #1181 |
| `machine.selfbuild.kernel` | x86_64 | incomplete | `19fdee5cfcccc984be30e587ca5474b154b3afb7115e864d2a715d8ed88c5a93` | #1181 |
| `machine.shutdown.bare-halt` | i686 | pass | `942325a612c2e3e24467de4b7908b12aa0fccffe570253ab4306c6868cbe184c` | - |
| `machine.shutdown.bare-halt` | x86_64 | pass | `ab17421253b0cac0135d5342b63be67b160c286f37faf6d520a37283f8120cc2` | - |
| `machine.shutdown.halt` | i686 | pass | `8e5d60f01230661546ba8b500184fab5f633a05fcbf7c561bcecf58bb54043bf` | - |
| `machine.shutdown.halt` | x86_64 | pass | `0bffee9b954ed16940fb812bed157ed654de52f087beda5f258246f7e70f97d9` | - |
| `machine.shutdown.init-death` | i686 | pass | `5b8688a3b158991a9e69b301f7c14b60c6ed8386415772fafe67e2a9c14424df` | - |
| `machine.shutdown.init-death` | x86_64 | pass | `c80be14346738f4484464a560c621fdef923ff3d9ce757e94a8adc6892d33a0b` | - |
| `machine.smp.nmi` | i686 | pass | `59a93c1d5b47d22871d6e8d7b2d8128295195eb6935e500560a883e453d1490a` | - |
| `machine.smp.nmi` | x86_64 | pass | `82f6f65fe5d3ae953dddf896d6bdfff7b9be61f05ab5efa7cbbe50e56ae52929` | - |
| `machine.ssh.logins` | i686 | failure | `1191e89fd47d466184f9d225578cca3b01d983d0467c556dff82e34c7adc929e` | #1188 |
| `machine.ssh.logins` | x86_64 | failure | `5cbc43fad9f9ec8257e1dc587e83ea1b29c0d9400774a77f9319d3652c533c8f` | #1188 |
| `machine.uefi.wide` | x86_64 | pass | `478f79692bf614b49eb3c4c899b655456fd83bac2917c354ddbf713875ab219e` | - |
| `machine.uefi.wide-bios` | i686 | pass | `da6145d4c6f57e45b55e49b835f9b74941e7c209cb5471279e372f0e76c05228` | - |
| `machine.usb.off` | i686 | pass | `669fafa90a7c07f6228b678c8fcecd150a732ca5b9c5f8b0b9e8d48ef7962c30` | - |
| `machine.usb.off` | x86_64 | pass | `5ed6f50dd1790272e05837e9a0ff7127e086559e80e919c590d5b77a1321ff63` | - |
| `machine.usbdisk.big` | i686 | failure | `324504bf059c8f404cc33ac8d4d31acce77748e411042b6c94110cef323e4cfa` | #1182 |
| `machine.usbdisk.big` | x86_64 | failure | `4cc64eb05470f1afa8fc33d3ee998215db16b0a598f89dbe6d472cd9d4539507` | #1182 |
