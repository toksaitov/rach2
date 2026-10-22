# Research for etude, 19 October 2026

The research that the owner asked for on 18 October 2026, before etude was designed: how teams that build software with coding agents test it, how other operating systems and test runners work, an audit of every suite Rach2 had then, and a design. Codex wrote it between 00:45 and 01:36 UTC+6 on 19 October, from a read-only checkout of `53b165d34`, as a lead with up to three helpers, and cited 109 sources. It is given here as written, apart from one link into a scratch copy that no longer exists.

It was not shown to the owner whole, and the plan was not checked against it. [The plan](plan.md#research) lists each of its findings with what became of it, and [the incidents](incidents.md#the-coordinating-session) record the failure.

## Recommendation

Replace the current orchestration with a portable C runner, a validated catalog of individual cases, and a scheduler that allocates resources across hosts and accounts.

Keep the useful tests. Change how they are selected, provisioned, executed and judged. Remove redundant mutation sweeps, weak success checks and diagnostic workloads from compulsory regression testing.

A passing result must mean that the complete planned work finished and its assertions passed. Running out of host time must produce an incomplete result, with evidence, rather than a claim that the kernel is wrong.

I inspected commit `53b165d34f74cd2bc5b27b2d6d4a986b68cf4584`. I used the earlier answers and measured run as historical evidence, then checked their important claims against the source. No files were changed. No builds, suites or mutation tests were run.

## Part A. Research

### A1. Testing software built by coding agents

An oracle is the rule that decides whether an observed result is correct. The central problem in this research is oracle quality. More generated tests, more executions and a higher pass rate do not establish that the intended behavior was tested.

#### Published engineering practice

* Anthropic's compiler experiment used 16 agents, established compiler tests, real application builds and GCC as an independent comparison implementation. Its useful lessons are strong verifiers, small reproducible subsets, compact diagnostics and independent work. The author describes a capability experiment with substantial remaining limitations. It is not evidence that unattended agents can maintain an operating system safely. [Building a C compiler with a team of parallel Claudes, 2026-02-05](https://www.anthropic.com/engineering/building-c-compiler).

* Anthropic's work on long sessions uses explicit feature lists, persistent state and end-to-end checks to prevent an agent from declaring unfinished work complete. Its evaluation guidance separates the final outcome from the agent's actions along the way. Instructions against weakening tests help, but do not enforce that rule. [Effective harnesses, 2025-11-26](https://www.anthropic.com/engineering/effective-harnesses-for-long-running-agents); [Demystifying evals, 2026-01-09](https://www.anthropic.com/engineering/demystifying-evals-for-ai-agents).

* OpenAI describes isolated worktrees, executable repository constraints, application observability and diagnostics that agents can act on. Its reported development speed is an experience report, not a controlled comparison. Its willingness to repair some failures after merging should not be copied into Rach2's release criteria without regard to kernel failures and data loss. [Harness engineering, 2026-02-11](https://openai.com/index/harness-engineering/).

* Meta's TestGen-LLM filters generated tests through build, reliability and coverage checks. The paper reports that 75% built, 57% passed reliably and 25% increased coverage. These filters reject much unusable output. They do not independently establish that the asserted behavior is correct. [Automated Unit Test Improvement using Large Language Models at Meta, 2024-02-14; FSE 2024](https://arxiv.org/abs/2402.09171).

* Google's specification-driven work evaluates tests against 90 production bugs and reports improved bug detection when explicit contracts guide generation. This is more relevant than counting assertions. It remains a bounded empirical study. Google's Jules evaluation also examines historical bugs, but some of its assessment uses an LLM judge; that is weaker evidence than an executable regression oracle. [Grounding AI Agents in Contracts, 2026-08-17](https://arxiv.org/abs/2608.17177); [Measuring what matters with Jules, 2026-06-22](https://developers.googleblog.com/en/measuring-what-matters-with-jules/).

* Microsoft's unit-test agent checks discovery, compilation, existing tests and added tests. Its internal benchmark reports 140 completed tasks out of 152, against 120 for the comparison using the same model. Coverage was nearly unchanged, and it generated slightly fewer tests. The demonstrated improvement concerns this workflow and benchmark, not OS testing generally. [From generated code to trusted code with a unit-test agent, 2026-07-31](https://devblogs.microsoft.com/dotnet/polyglot-unit-testing-agent/).

* Microsoft's debug-gym adds runtime inspection and debugging tools instead of giving an agent only a final pass/fail result. Rach2 should similarly expose the failing operation, relevant kernel counters, process status and preserved artifacts. [debug-gym, 2025-04-10](https://www.microsoft.com/en-us/research/blog/debug-gym-an-environment-for-ai-coding-tools-to-learn-how-to-debug-code-like-programmers/).

The common lesson is to give agents executable contracts and useful evidence. It is not to make tests permissive so that autonomous work continues.

#### Benchmarks and their harnesses

SWE-bench distinguishes tests that should change from failing to passing from regression tests that should remain passing. Its harness packages dependencies and applies a candidate patch before evaluation. That separation is useful for Rach2 repairs: demonstrate the original failure, then demonstrate the repair against the same inputs. [SWE-bench evaluation documentation, undated, accessed 2026-10-19](https://www.swebench.com/SWE-bench/guides/evaluation/).

SWE-bench Verified was a human-reviewed subset of 500 tasks. OpenAI later audited 138 difficult tasks and found material test or specification problems in 59.4% of that audited subset. That number does not describe the whole dataset. The examples show both tests tied to implementation details and tests demanding unspecified behavior. [Introduction, 2024-08-13](https://openai.com/index/introducing-swe-bench-verified/); [Later audit, 2026-02-23](https://openai.com/index/why-we-no-longer-evaluate-swe-bench-verified/).

OpenHands separates agent execution, sandbox configuration, benchmark inputs and evaluation. Its current benchmark infrastructure is useful evidence for preserving the complete execution environment. Docker-based execution does not itself solve native Windows operation, guest-kernel isolation or hardware scheduling. [OpenHands paper, 2024-07-23](https://arxiv.org/abs/2407.16741); [Benchmark implementation, undated, accessed 2026-10-19](https://github.com/OpenHands/benchmarks).

Rach2 should adopt these contracts:

* A candidate cannot silently change the evaluator used to judge it.
* Missing expected cases prevent success.
* A reproduced baseline failure is part of repair evidence.
* Artifact, environment and evaluator identities accompany every result.
* Existing test removal, changed expectations and reduced applicability appear explicitly in review.

#### Generated tests, weak oracles and gaming

The recent literature supports caution in several different ways.

* UTBoost found weak benchmark tests that accepted incorrect patches. Its reported dataset includes 36 affected tasks and 345 incorrect accepted patches. A test suite can be too permissive even when it looks substantial. [UTBoost, 2025-06-10](https://arxiv.org/abs/2506.09289).

* Research on generated oracles finds that models often reproduce the implementation's behavior. That can preserve the bug under investigation. Documentation and contracts provide additional evidence, but the oracle still needs review. [Can we find bugs using LLM-generated oracles?, first submitted 2024-10-28, revised 2026-10-05](https://arxiv.org/abs/2410.21136); [Doc2OracLL, 2025-06-19](https://doi.org/10.1145/3729354).

* An ASE study compares thousands of generated and human oracles using mutation testing. Such scores measure sensitivity to the chosen faults. They are useful, but do not prove agreement with the intended specification. [Useful Test Oracles for LLM-Generated Unit Tests, ASE 2025, November 2025](https://www.lucadigrazia.com/papers/ase2025.pdf).

* METR documents agents modifying tests or scoring machinery during evaluations. Anthropic's reward-tampering experiments and EvilGenie study related failure modes under constructed incentives. These establish that evaluator manipulation is possible. They do not establish its frequency in ordinary Rach2 work. [METR, 2025-06-05](https://metr.org/blog/2025-06-05-recent-reward-hacking/); [Anthropic, 2024-06-17](https://www.anthropic.com/research/reward-tampering); [EvilGenie, 2025-11-26](https://arxiv.org/abs/2511.21654).

* SpecGuardBench examines whether generated verification follows a specification rather than merely matching one reference implementation. It is a recent preprint, not established production practice. Its relevance is the distinction between checking a contract and overfitting a verifier. [SpecGuardBench, 2026-10-12](https://arxiv.org/abs/2610.16870).

* A study accepted at ICSE SEIP 2026 shows that examples supplied to a model can transfer flaky behavior into generated database tests. Unordered results were a major cause in the examined cases. [Study, 2026-01-13](https://arxiv.org/abs/2601.08998).

For Rach2, generated tests should identify their contract, use an independent expected result where possible, and demonstrate the intended failure under a deliberate fault. A crash, infrastructure timeout or unrelated failure is not sufficient mutation evidence.

#### Output written for an agent

The compiler experiment explicitly recommends short summaries, searchable errors and detailed logs kept separately. That fits this project.

The first result an agent reads should contain:

* The failed property and the actual and expected values.
* The exact case and environment.
* Whether execution completed.
* The smallest useful transcript excerpt.
* Links or paths to complete logs, images, traces and reproducers.
* A command that reruns the case with the same artifacts.

Do not make the agent reconstruct a failure from thousands of progress lines. Do not make a natural-language assessment the authoritative verdict.

### A2. Operating-system and embedded test systems

The following documentation was checked on 2026-10-19. Where no publication date is available, it is explicitly marked undated.

* **Zephyr Twister.** It separates architecture, platform, toolchain, memory, features, fixtures and harnesses. Compatibility filters differ from the smaller integration selection. Current documentation discovers `tests.yaml`; `testcase.yaml` and `sample.yaml` support is deprecated. Rach2 should copy the separation between applicability and selection policy. [Twister, undated](https://docs.zephyrproject.org/latest/develop/twister/index.html).

* **ChromeOS Tast.** Software and hardware dependencies are explicit. Fixtures share setup and reset state between tests. Reset should be idempotent, with teardown and setup as a fallback. The documentation acknowledges limitations in how reset errors affect results. Rach2 must discard a guest whose reset postcondition failed. [Dependencies, undated](https://chromium.googlesource.com/chromiumos/platform/tast/+/HEAD/docs/test_dependencies.md); [Fixtures and tests, undated](https://chromium.googlesource.com/chromiumos/platform/tast/+/HEAD/docs/writing_tests.md).

* **Fuchsia.** Test manager supplies isolated component environments and a common suite protocol. Language runners adapt to it. Build metadata produces the inventory used locally and in CI. Rach2 needs the same single inventory and common result contract, without adopting Fuchsia's component system. [Test Runner Framework, updated 2025-03-22](https://fuchsia.dev/fuchsia-src/development/testing/components/test_runner_framework); [tests.json, undated](https://fuchsia.dev/fuchsia-src/reference/testing/tests-json-format).

* **Android Tradefed and atest.** `TEST_MAPPING` connects source directories to presubmit and postsubmit tests. Atest gives a common interface for narrow selection, build, installation and execution. Tradefed distinguishes test partitioning from device allocation. Rach2 should begin with explicit source mappings, not a learned selector. [TEST_MAPPING, updated 2026-10-09](https://source.android.com/docs/core/tests/development/test-mapping); [atest, undated](https://source.android.com/docs/core/tests/development/atest); [Sharding, undated](https://source.android.com/docs/core/tests/tradefed/architecture/advanced/sharding).

* **LAVA.** Jobs separate deployment, boot and testing. Device types describe common capabilities; device instances describe serial connections, power and storage. Health checks use known working software to distinguish an unhealthy device from a failed candidate. These are useful distinctions for the Lenovo. A complete LAVA deployment would be excessive for one laptop. [Jobs, undated](https://lava.readthedocs.io/en/latest/technical-references/job-definition/job.html); [Device dictionaries, undated](https://lava.readthedocs.io/en/latest/technical-references/configuration/device-dictionary.html); [Health checks, version 2025.06](https://lava.pengutronix.de/static/docs/v2/healthchecks.html).

* **KernelCI.** Revision, configuration, compiler, platform and artifacts form part of result identity. Execution state differs from test outcome. Setup failure can leave a test incomplete. This is the right basis for Rach2 comparisons. [Pipeline and API, undated](https://docs.kernelci.org/components/maestro/pipeline/developer-documentation/); [API details, undated](https://docs.kernelci.org/components/maestro/api/api-details/).

* **LKFT.** Its real triage reports distinguish testcase failures from boot, connection and infrastructure failures. Its historical documentation also shows the difficulty of matching test versions to kernel versions. Rach2 should preserve probe identity with kernel identity. [Overview, undated](https://lkft.linaro.org/); [Triage report, 2024-07-26](https://lists.linaro.org/archives/list/lkft-triage%40lists.linaro.org/thread/43WNRUU4VWQET5UUNO4BN6IY675TAEG2/); [Triage report, 2025-03-01](https://lists.linaro.org/archives/list/lkft-triage%40lists.linaro.org/thread/PKQETCPYRTTIE7SWNAUS5BFMUCPZXO2F/).

* **Linux kselftest and KUnit.** Kselftest exercises kernel behavior from userspace. KUnit checks kernel components. KTAP provides plans, named cases, nesting and explicit skips. This supports retaining distinct test kinds under one result protocol. [kselftest, undated](https://docs.kernel.org/dev-tools/kselftest.html); [KUnit, undated](https://docs.kernel.org/dev-tools/kunit/); [KTAP, undated](https://docs.kernel.org/dev-tools/ktap.html).

* **virtme-ng.** It makes real-kernel development runs convenient by using a prepared host directory or disk root. That convenience does not make the host directory an immutable release artifact. Rach2 should distinguish development execution from release execution. [Source and documentation, undated](https://github.com/arighi/virtme-ng).

* **syzkaller and syzbot.** Corpus preservation, minimization, reproducibility, artifacts and cautious bisection are directly useful. Syzbot warns that a patched run can appear successful when the comparison environment cannot reproduce the original failure. [Usage, undated](https://github.com/google/syzkaller/blob/master/docs/usage.md); [syzbot, undated](https://github.com/google/syzkaller/blob/master/docs/syzbot.md).

* **ATF and Kyua.** ATF separates metadata, test body, cleanup, diagnostics and results. It declares required programs, files, privilege and resources. Kyua discovers and executes different test types and stores their results. These are close to the contracts Rach2 needs. [NetBSD atf-test-case, 2023-08-27](https://man.netbsd.org/atf-test-case.4); [FreeBSD kyua, undated](https://man.freebsd.org/kyua%281%29).

* **seL4.** Tests complement proofs and cover development configurations and properties outside verification. Configuration identity matters. Rach2 should likewise state which property a test establishes and which execution modes it covers. [seL4test, undated](https://docs.sel4.systems/projects/sel4test/); [Verification status, undated](https://docs.sel4.systems/projects/sel4/status).

* **SerenityOS.** Lagom runs selected real libraries and tools on the host; target tests execute inside SerenityOS. This is a practical example of keeping host sanitizers and guest integration tests together. [Running tests, undated](https://github.com/SerenityOS/serenity/blob/master/Documentation/RunningTests.md); [Lagom, undated](https://github.com/SerenityOS/serenity/blob/master/Meta/Lagom/ReadMe.md).

* **Redox.** Its tools support testing components against prepared VM images. Its CI documentation also records a single-CPU workaround for kernel instability. Such a workaround limits the demonstrated coverage; it does not validate SMP. [CI, undated](https://github.com/redox-os/book/blob/master/src/ci.md); [relibc tests, undated](https://github.com/redox-os/relibc/blob/master/README.md).

* **Haiku.** Developer discussions show both neglected tests and work toward automated test images. They also question expensive framework changes that do not improve execution. This is firsthand project discussion, not a measured successful deployment. [Running tests, 2024-10-02](https://discuss.haiku-os.org/t/how-to-run-interface-kit-tests-for-libbe-so/15645); [Test conversion discussion, beginning 2026-01-23](https://discuss.haiku-os.org/t/unit-test-conversion-to-bdd-style-given-when-then/18589).

* **ReactOS.** The host supervisor and guest test runner preserve progress across crashes. Its journaled test list records the interrupted case before advancing. Rach2 should retain the identity of the running case when a guest disappears, then continue remaining work in a fresh guest. Journals must be bound to the current plan and artifacts. [sysreg2, undated](https://github.com/reactos/sysreg2); [Journaled test list source documentation, generated 2026-06-20](https://doxygen.reactos.org/d4/dc7/classCJournaledTestList.html).

These systems support a common design. A test has a stable identity, declared requirements, controlled inputs, an execution adapter, a cleanup contract and an explicit outcome. They do not justify forcing every test into one framework or one guest.

### A3. Runners, scheduling and selection

#### Bazel, Buck2 and nextest

Bazel's test contract separates size, timeout class, tags, execution properties, runtime inputs, writable directories and process status. Size and timeout are different declarations. Its standard timeout classes remain fixed wall-clock policies; they are not a solution to Rach2's requirement. Sharding includes an acknowledgement mechanism so that an unsupported test cannot silently execute all cases in every shard. [Bazel Test Encyclopedia, undated, accessed 2026-10-19](https://bazel.build/reference/test-encyclopedia); [Hermeticity, same access date](https://bazel.build/basics/hermeticity).

Buck2 separates build information from the external test runner. Its local resource provider can provision expensive resources and queue tests for an instance. That pool is scoped to an invocation. Rach2's separate accounts and simultaneous CI runs require coordination across invocations. [Test execution, undated, accessed 2026-10-19](https://buck2.build/docs/rule_authors/test_execution/); [Local resources, same access date](https://buck2.build/docs/rule_authors/local_resources/).

Cargo-nextest provides weighted scheduling, constrained groups, partitioning and execution archives. Its documented process-leak detection observes inherited output handles; detached children that redirect them can escape that mechanism. Rach2 needs ownership of the process tree independent of stdout. [Groups, undated, accessed 2026-10-19](https://www.nexte.st/docs/configuration/test-groups/); [Archives, same access date](https://nexte.st/docs/ci-features/archiving/); [Leaky tests, updated 2026-03-30](https://www.nexte.st/docs/features/leaky-tests/).

These systems provide useful contracts. Adopting their complete build systems would add work unrelated to the requested portability.

#### Impact analysis and predictive selection

Google's 2025 speculative-testing study reports earlier detection by changing test order and speculative execution. It does not establish that omitted tests are safe. Its 2026 regression-selection study reports that roughly 0.9% of the available tests found about 40% of failing commits in the evaluated setting. That is useful cost reduction with substantial missed failures. [Speculative Testing at Google, 2025](https://research.google/pubs/speculative-testing-at-google-with-transition-prediction/); [Regression Test Selection at Scale, 2026](https://research.google/pubs/regression-test-selection-at-scale-4/).

Meta reported detecting over 99.9% of bad changes while running about one third of transitively affected tests. It also reported exhaustive testing before deployment. That older production result is relevant, but is not a guarantee for a smaller OS repository. [Predictive Test Selection, 2018-11-21](https://engineering.fb.com/2018/11/21/developer-tools/predictive-test-selection/).

Rach2 should begin with:

* Explicit component-to-case mappings.
* Build dependency information.
* Mandatory architecture and device smoke profiles.
* Conservative expansion when selection is uncertain.
* Full release coverage.

Only later should it evaluate coverage-based or predictive selection against the failures found by complete runs.

#### Flakiness and resource interference

Google's and Meta's reports distinguish test defects, intermittent product failures and uncertain failure probabilities. Their measurement methods are useful even though their quarantine policies need not be adopted. [Google, 2016-05-27](https://testing.googleblog.com/2016/05/flaky-tests-at-google-and-how-we.html); [Meta, 2020-12-10](https://engineering.fb.com/2020/12/10/developer-tools/probabilistic-flakiness/).

Recent work examines resource configurations and interference directly. An ASE artifact covers 134 C projects. A TSE study examines 52 projects under 27 resource configurations. These support recording load and testing declared isolation boundaries, rather than repeatedly increasing timeouts. [ASE artifact, 2024-09-25](https://zenodo.org/records/13767954); [Computational resources and flaky tests, TSE 2024](https://www.jonbell.net/preprint/tse24-raft.pdf).

Anthropic's controlled infrastructure experiment found a six percentage-point benchmark success difference across resource configurations with the same model and harness. Resource limits can change the measured result substantially. This is direct evidence against interpreting every stopped run as a product failure. [Infrastructure noise, 2026-02-05](https://www.anthropic.com/engineering/infrastructure-noise).

### A4. Time, simulation and completion

FoundationDB tests production logic in a deterministic simulation with controlled time, network and storage faults. TigerBeetle similarly uses deterministic simulation and explicit safety and liveness checks. These systems were designed to permit that control. Applying the idea to selected Rach2 components is feasible; applying it to the entire SMP kernel is a separate research project. [FoundationDB, SIGMOD 2021](https://www.foundationdb.org/files/fdb-paper.pdf); [TigerBeetle liveness testing, 2023-07-06](https://tigerbeetle.com/blog/2023-07-06-simulation-testing-for-liveness/); [Protocol-aware simulation, 2026-08-20](https://tigerbeetle.com/blog/2026-08-20-protocol-aware-dst/).

Antithesis describes deterministic execution of a complete virtual environment. Its engineering account is useful, but its environment and constraints must not be confused with unrestricted SMP execution on ordinary QEMU accelerators. [Deterministic hypervisor, 2024-03-20](https://antithesis.com/blog/deterministic_hypervisor/).

QEMU `icount` relates virtual time to executed instructions. It is not cycle-accurate hardware timing. It uses TCG and is incompatible with multi-threaded TCG. Record/replay adds further input and device requirements; current replay also rejects multiple virtual CPUs. These are useful specialized profiles, not replacements for accelerated SMP testing. [Instruction counting, undated, accessed 2026-10-19](https://www.qemu.org/docs/master/devel/tcg-icount.html); [Record/replay, same access date](https://www.qemu.org/docs/master/system/replay.html); [Current implementation, same access date](https://raw.githubusercontent.com/qemu/qemu/master/system/vl.c).

No finite watchdog can always distinguish an arbitrarily slow correct program from a hung program. The design must therefore distinguish:

* A violated assertion.
* An unfinished execution.
* A resource-management decision to stop waiting.

A measured host profile can improve the third decision. It cannot turn it into proof of the first.

### A5. QEMU across hosts

QEMU provides KVM on Linux, HVF on macOS, WHPX on Windows and TCG across these hosts. The accelerator changes CPU execution. Device models, CPU features, clock behavior and available backends still need explicit configuration and validation. [QEMU accelerator documentation, undated, accessed 2026-10-19](https://www.qemu.org/docs/master/system/introduction.html).

Host and guest instruction sets matter. HVF on an Apple Silicon Mac does not accelerate an x86 Rach2 guest. Such a Mac needs local TCG or a remote x86 executor. QEMU's HVF build conditions distinguish the host architectures. [QEMU build source, undated, accessed 2026-10-19](https://raw.githubusercontent.com/qemu/qemu/master/meson.build).

Nested virtualization and cross-OS filesystem access are separate costs. Microsoft's documentation requires explicit nested-virtualization support and recommends keeping Linux workloads in the Linux filesystem under WSL. Neither establishes the cost of Rach2's current ATA path on native WHPX. [Nested virtualization, undated, accessed 2026-10-19](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/user-guide/nested-virtualization); [WSL filesystem guidance, same access date](https://learn.microsoft.com/en-us/windows/wsl/filesystems).

The tree contains important historical evidence:

* All 88 suites passed under KVM in 18 minutes 6 seconds on 2026-09-20. `dev-log/53-audit/notes.md:133`.
* Earlier runs exercised both KVM and TCG successfully. `dev-log/52-suites/Readme.md:33`.
* ATA DMA work reduced one recorded 64 MiB operation from about 217 seconds to about two seconds. `dev-log/52-suites/plan.md:16`.

These facts rule out treating TCG performance as an unavoidable lower bound. They do not establish today's 101-suite runtime on native WHPX, KVM or HVF.

The scheduler should select a declared compatible accelerator. It should never silently substitute another accelerator and call the original required profile covered.

### A6. Conference research and industry reports

The following work adds concrete ideas beyond framework documentation.

* **Linux Plumbers 2024:** comparison of statement, branch and MC/DC coverage across kernel test collections. This supports measuring what a suite adds, rather than counting suites. [Measuring and Understanding Linux Kernel Tests, 2024-09-20](https://lpc.events/event/18/contributions/1793/).

* **Linux Plumbers 2025 and 2026:** KFuzzTest targets internal functions that syscall fuzzing reaches poorly. The 2026 presentation describes work under upstream review, not an already adopted kernel standard. [Introduction, 2025-12-12](https://lpc.events/event/19/contributions/2196/); [Follow-up, 2026-10-06](https://lpc.events/event/20/contributions/2532/).

* **Linux Plumbers 2026:** Kernel Build Identity proposes packaging related kernel artifacts with reproducible identity. It supports the artifact design below, but remains a proposal. [Talk, 2026-10-06](https://lpc.events/event/20/contributions/2620/).

* **FOSDEM 2024-2026:** external packet tests, shared ARM/FPGA laboratory infrastructure and host instrumentation of an assembly kernel offer practical examples. The Claude hardware talk is an implementation report, not a controlled reliability result. [Scapy testing, 2024-02-03](https://archive.fosdem.org/2024/schedule/event/fosdem-2024-2812-testing-iptables-firewall-rules-with-scapy/); [ARM/FPGA infrastructure, 2025-02-01](https://archive.fosdem.org/2025/schedule/event/fosdem-2025-5708-advanced-test-harness-infrastructure-for-validating-arm-and-fpga-based-systems/); [Assembly kernel testing slides, 2026-02-01](https://fosdem.org/2026/events/attachments/LTG7XC-instrument_and_unit_test_an_asm-only_os_kernel_by_turning_it_into_an_anykernel/slides/266126/baravy_g4z3gam.pdf); [Hardware with Claude, 2026-02-01](https://archive.fosdem.org/2026/schedule/event/9SZVST-testing-on-hardware-with-claude-ai/).

* **Embedded Open Source Summit 2024:** Zephyr presentations cover reusable peripheral emulators and native testing of memory-mapped drivers. A simulator must follow the hardware specification rather than duplicate the driver's assumptions. [Project report, 2024-07-09](https://zephyrproject.org/expert-insights-into-test-driven-development-pytest-integration-emulators-and-unit-testing/).

* **Open Source Summit Europe 2024:** Yocto's progression from package tests to image tests to LAVA illustrates useful layers of testing. It does not imply that Rach2 needs all of Yocto's infrastructure. [Presentation record, 2024-09-16](https://www.yoctoproject.org/yocto-project-presentations-summary/).

* **ICSE 2025 and 2026:** kernel configuration selection and configuration-sensitive fuzzing find behavior missed by default configurations. The reported improvements are specific to their evaluated Linux targets. Rach2 should preserve architecture, CPU, memory and device variants explicitly. [Configuration selection, 2025-04-30](https://conf.researchr.org/details/icse-2025/icse-2025-research-track/20/A-Little-Goes-a-Long-Way-Tuning-Configuration-Selection-for-Continuous-Kernel-Fuzzin); [Configuration-sensitive fuzzing, 2026-04-17](https://conf.researchr.org/details/icse-2026/icse-2026-research-track/94/Configuration-Sensitive-Linux-Kernel-Fuzzing).

* **FSE 2024:** BRF constructs valid dependencies to reach eBPF runtime behavior. Rach2's fuzzers likewise need valid filesystem states and syscall resource relationships, alongside invalid inputs. [BRF, July 2024](https://2024.esec-fse.org/details/fse-2024-research-papers/113/BRF-Fuzzing-the-eBPF-runtime).

* **ASE 2024:** C test-interference work provides an evaluated artifact for finding order and resource interactions. This supports focused tests of shared files, ports and leaked processes in the new runner. [Artifact presentation, 2024-10-29](https://conf.researchr.org/details/ase-2024/ase-2024-artifact-evaluation-track/2/Efficient-Detection-of-Test-Interference-in-C-Projects-Artifact-).

* **ISSTA 2025 and 2026:** SyzGPT uses dependency information to generate useful syscall seeds; SyzDiversity explores corpus diversity. Models can propose inputs while executable checks retain authority over results. [SyzGPT, 2025-06-26](https://conf.researchr.org/details/issta-2025/issta-2025-papers/38/Unlocking-Low-Frequency-Syscalls-in-Kernel-Fuzzing-with-Dependency-based-RAG); [SyzDiversity, 2026](https://conf.researchr.org/details/issta-2026/issta-2026-research-papers/20/SyzDiversity-Diversity-Guided-Linux-Kernel-Fuzzing).

* **OSDI 2024:** state embedding checks the relationship between concrete execution and an abstract verifier state. IronSpec separately shows that specifications themselves contain bugs. Both support small independently checked models rather than crash-only fuzzing. [eBPF verifier validation, July 2024](https://www.usenix.org/conference/osdi24/presentation/sun-hao); [IronSpec, July 2024](https://www.usenix.org/conference/osdi24/presentation/goldweber).

* **OSDI 2025:** KRR reproduces kernel failures through specialized recording. It reproduced five of six nondeterministic bugs in its evaluated set. This supports preserving replay evidence, not assuming stock QEMU replay covers every SMP failure. [KRR, July 2025](https://www.usenix.org/conference/osdi25/presentation/zhang-tianren).

* **SOSP 2024 and 2025:** Ozz explores concurrency using compiler and KVM changes. Ghost in the Android Shell uses executable C specifications. Ghost found five implementation bugs, but four came from reading code while writing the specification; execution found one. It also found specification errors and required substantial engineering. Start with one small subsystem. [Ozz, November 2024](https://gts3.org/assets/papers/2024/jeong%3Aozz.pdf); [Ghost, October 2025](https://www.cl.cam.ac.uk/~km569/pubs/sosp25-exec-spec.pdf).

Industry reports need equal care. DORA's 2025 report is observational survey research. METR's early-2025 randomized study found experienced developers slower with the tools then tested; its February 2026 update explains why later estimates are difficult to interpret. Neither establishes the productivity of Rach2's current agent team. [DORA announcement, 2025-09-23](https://cloud.google.com/blog/products/ai-machine-learning/announcing-the-2025-dora-report); [METR study, 2025-07-10](https://metr.org/blog/2025-07-10-early-2025-ai-experienced-os-dev-study/); [METR update, 2026-02-24](https://metr.org/blog/2026-02-24-uplift-update/).

## Part B. Audit of the current tree

### B1. Inventory and interpretation

The source inventory is:

* 101 registered suites.
* 148 host test programs.
* 185 guest C programs: 103 raw and 82 musl.
* 119 programs in the default list: 60 raw and 59 musl.
* 46 guest programs used by other suites.
* 20 helpers or manual programs.
* 116 files named `*_mutate.sh`.
* 151 mutation-capable scripts under the actual discovery rule.
* 502 probe fault blocks.

References: `tests/registry:1`, `tests/system/runner/runtests.c:61`, `tests/probe-suites:1`, `tests/probe-helpers:1`, `tools/verify/faulttexts.sh:20`, `tests/system/probe_faults.sh:2`.

The x86-64 default omits `tlsprobe`, reducing its default count to 118. The current runner accounts for that omission as a pass. The replacement should report applicability explicitly. `tests/system/runner/runtests.c:955`.

For the following ledger:

* `M:N` means `tests/system/suites.mk` at line N.
* `S/name.sh:N` means `tests/system/suites/name.sh` at line N.
* **Keep** preserves the substantive test and its oracle, while moving execution into the new system.
* **Merge** makes it a named case or profile in a common family. It does not imply sharing a guest.
* **Rewrite** changes the execution strategy or oracle.
* Deletion applies to specific rows and machinery identified below.

The disposition is 30 keep, 18 rewrite and 53 merge. I found no defensible reason to delete an entire registered suite.

### B2. All 101 suites

1. `gnu`: rewrite. Preserve real GNU applications, SSH and authentication; share roots and select application rows. `S/gnu.sh:6`, `M:1497`.
2. `agents`: rewrite. Preserve vendor agents and the worker session; remove repeated guest workflow mutation sweeps. `S/agents.sh:232`, `S/agents.sh:294`.
3. `agentsaccept`: rewrite. Make installed interactive, credential and reboot acceptance an explicit profile. `M:1550`, `dev-log/61-agents/plan.md:78`.
4. `install`: rewrite. Use distinct prebuilt marked kernels; leave native compilation to selfbuild. `S/install.sh:9`, `S/install.sh:13`.
5. `selfbuild`: rewrite. Preserve native builds and byte reproducibility; avoid repeating full probes against identical built bytes. `S/selfbuild.sh:4`, `S/selfbuild.sh:459`.
6. `installer-repair`: keep. Interrupted updates, recovery and BIOS-to-EFI conversion are distinct contracts. `M:1669`, `M:1688`.
7. `installer`: keep. Exercise the shipped medium and installation-key policy. `S/installer.sh:6`, `M:1663`.
8. `menu`: merge. Preserve torn-menu recovery, fallback numbering and extents in boot-update cases. `M:605`.
9. `uefi`: keep. Preserve firmware boot and NVMe reads beyond 2 TiB. `S/uefi.sh:46`, `S/uefi.sh:517`.
10. `installer-rehearse`: merge. Make rehearsal an installation profile with saved-image digest checks. `M:1674`, `S/installer.sh:1938`.
11. `selfbuild-ext2`: merge. Parameterize native build by root filesystem, retaining ext2 boot and fsck. `M:1820`.
12. `sealed`: rewrite. Keep shipped-artifact recovery and its no-fault control; move mechanism matrices to their owners. `S/sealed.sh:15`, `S/sealed.sh:740`.
13. `fakeapi`: merge. Preserve real guest TLS, SSE, WebSocket and exact request checks in agent transport cases. `S/fakeapi.sh:190`, `S/fakeapi.sh:249`.
14. `dhcp-e1000`: merge. Parameterize the card while retaining driver-dependent cases. `M:1053`, `M:2150`.
15. `dhcp`: rewrite. Combine compatible transitions; preserve cold boot, absent server and separate networks. `M:1058`, `M:1264`, `M:1292`.
16. `netif`: merge. Put interface, route and multiple-card cases in a network topology family. `M:888`, `M:937`.
17. `netif-e1000`: merge. Use the e1000 parameter of that family. `M:2117`, `M:2134`.
18. `mount`: keep. Preserve dirty reclamation, dead-device isolation and mount lifetime. `S/mount.sh:125`, `S/mount.sh:255`.
19. `fdlock`: merge. Preserve relocation, orphan and shared-description behavior in descriptor lifetime cases. `M:514`.
20. `grow`: keep. Preserve written-sector readback, interrupted repair and refusal before writes. `S/grow.sh:11`, `S/grow.sh:151`.
21. `drill`: merge. Retain root destruction, transfer hashes and no-DHCP recovery profiles. `S/drill.sh:13`.
22. `account`: keep. Authentication, deletion and cold-boot persistence warrant integration tests. `S/account.sh:232`, `S/account.sh:426`.
23. `large`: rewrite. Select boundary and full-write cases deliberately; preserve data beyond 4 GiB and external checking. `S/large.sh:87`, `S/large.sh:115`.
24. `usb`: rewrite. Consolidate compatible cases while retaining topology, hot unplug, slot reuse, interrupts and controller failures. `S/usb.sh:185`, `S/usb.sh:443`.
25. `raw-e1000`: merge. Parameterize the driver without repeating stack-only behavior. `M:2117`, `M:2146`.
26. `raw`: merge. Keep raw-packet semantics in the network family. `M:833`.
27. `krffsmount`: keep. Rebooted bytes, repair, two writers and mount isolation have substantive oracles. `S/krffsmount.sh:116`, `S/krffsmount.sh:300`.
28. `diskrate`: rewrite. Keep byte and command-count assertions; remove diagnostic placement/rate work from required regression runs. `S/diskrate.sh:265`, `S/diskrate.sh:313`.
29. `usbdisk`: keep. Preserve partition forms, high offsets, multiple LUNs and selective removal. `S/usbdisk.sh:96`, `S/usbdisk.sh:366`.
30. `stress-ahci`: merge. Make AHCI a controller parameter of storage stress. `M:2053`.
31. `default`: rewrite. Add individual probe selection and expected outcomes; repair weak rows. `tests/system/runner/runtests.c:61`, `:950`.
32. `stress`: merge. Preserve a working set larger than the cache in storage stress. `M:1923`.
33. `procfiles`: merge. Share compatible userland setup while preserving mount-source and live-peer cases. `S/procfiles.sh:7`, `S/procfiles.sh:329`.
34. `bigmem`: keep. Preserve actual patterns above 4 GiB and direct-map boundaries. `S/bigmem.sh:30`, `S/bigmem.sh:67`.
35. `memory`: keep. Preserve low-memory refusal, allocation failures and stack faults. `S/memory.sh:28`, `S/memory.sh:83`.
36. `parts`: keep. Preserve partition identity, protection and high-offset host readback. `S/parts.sh:4`, `S/parts.sh:87`.
37. `bootpart`: merge. Make absent-node and wide-disk behavior partition/boot profiles. `M:484`, `M:533`.
38. `loopdev`: keep. Preserve nesting, aliases, returned-write durability and backing-device removal. `S/loopdev.sh:103`, `S/loopdev.sh:172`.
39. `tcc`: merge. Preserve real compilation and multi-unit linking in native-build cases. `S/tcc.sh:159`, `S/tcc.sh:246`.
40. `make`: merge. Preserve dependency, no-op rebuild and reboot behavior in that family. `M:1799`.
41. `smp`: keep. Preserve preemption, NMI and per-CPU user-time checks. `S/smp.sh:95`, `S/smp.sh:175`.
42. `clock`: rewrite. Replace the testing-host-derived serial-arrival tolerance with a justified clock oracle. `S/clock.sh:6`, `S/clock.sh:17`.
43. `host`: rewrite. Keep real-code sanitizer tests; make builds selective and incremental. `S/host.sh:73`, `S/host.sh:149`.
44. `noentropy`: keep. Feature-dependent fail-closed randomness requires a distinct CPU profile. `M:255`.
45. `shutdown`: merge. Preserve actual exit, clean disks and readback in shutdown/persistence cases. `S/shutdown.sh:246`, `S/shutdown.sh:305`.
46. `fatwrite`: keep. Preserve mtools readback, crash repair, native writers and concurrent writes. `S/fatwrite.sh:62`, `S/fatwrite.sh:149`.
47. `ssh`: keep. Preserve authentication, terminal state, flood handling and resource return. `S/ssh.sh:183`, `S/ssh.sh:389`.
48. `ssh-e1000`: merge. Keep a driver-specific smoke profile instead of repeating all authentication cases. `M:2154`, `S/ssh.sh:298`.
49. `exfat`: keep. Host-created data, repair and external comparison provide independent checks. `S/exfat.sh:120`, `S/exfat.sh:219`.
50. `ext2`: keep. Preserve mappings, links, set-id behavior, command counts, crashes and fsck. `S/ext2.sh:28`, `S/ext2.sh:120`, `S/ext2.sh:239`.
51. `cred-persist`: merge. Keep the three-boot credential behavior in account persistence. `M:747`.
52. `packet-e1000`: merge. Parameterize the real packet-driver profile. `M:2130`.
53. `addressed`: merge. Keep source-address and captured ARP assertions in network addressing. `M:232`.
54. `ping`: merge. Keep real wire, progress and interrupt behavior in network integration. `M:790`.
55. `packet`: merge. Preserve pre-DHCP packet sockets in that family. `M:799`.
56. `ping-e1000`: merge. Parameterize the driver path. `M:2126`.
57. `load`: merge. Preserve kernel delay-count assertions in scheduler/load cases. `M:1452`.
58. `authority`: keep. Preserve ordinary-user refusal, unchanged sectors and recovery authority. `M:627`.
59. `queue`: keep. Preserve device isolation, late answers, deadlines and full-slot waiting. `S/queue.sh:47`, `S/queue.sh:176`.
60. `persist-ahci`: merge. Parameterize persistence by controller. `M:2041`.
61. `subset`: merge. Preserve driver-selection options and the no-options control as boot profiles. `S/subset.sh:26`, `S/subset.sh:84`.
62. `bus`: keep. Preserve IDE positions, mixed roots and high ATA offsets. `S/bus.sh:4`, `S/bus.sh:72`.
63. `exitline`: merge. Preserve ordinary silence and debug markers as a kernel/executor contract. `S/exitline.sh:12`.
64. `login`: keep. Preserve stopped-console recovery, rescue shell and authentication. `S/login.sh:169`, `M:1870`.
65. `man`: rewrite. Check exhaustive staged bytes and aliases on the host; keep guest decompression and pager cases. `S/man.sh:42`, `S/man.sh:59`.
66. `metal`: rewrite. Preserve shipped and installed boot acceptance; remove repeated mechanism coverage. `M:1738`.
67. `bootline`: keep. Preserve conflicting options, unreadable init, unsupported CPUs and truncation. `S/bootline.sh:99`, `S/bootline.sh:180`.
68. `servicelog`: merge. Preserve reaping, log cap and fresh-boot reset in init/service cases. `S/servicelog.sh:85`, `S/servicelog.sh:119`.
69. `persist`: merge. Keep durable reboot/readback in controller-parameterized persistence. `M:737`.
70. `style`: rewrite. Select affected contracts and remove dependence on unrelated historical mutation text. `M:2063`, `tools/verify/faulttexts.sh:5`.
71. `dgram-e1000`: merge. Parameterize the real datagram card path. `M:2138`.
72. `dgram`: merge. Preserve captured broadcasts and peer filtering. `M:991`.
73. `diskdiag`: merge. Preserve guest/host sector hashes and above-driver write refusal. `S/diskdiag.sh:177`, `S/diskdiag.sh:195`.
74. `e1000`: merge. Preserve binding order and unsupported-e1000e refusal as topology cases. `M:1395`, `M:1417`.
75. `disk-ahci`: merge. Parameterize disk integration by controller. `M:1957`.
76. `grub`: merge. Preserve execution of marked replacement cores in boot update. `M:554`, `M:587`.
77. `disk-q35`: keep. AHCI at PCI function 2 exercises a distinct hardware arrangement. `M:1964`, `M:1970`.
78. `doom`: rewrite. Preserve application execution, missing-WAD refusal and demo completion; improve output checking. `S/doom.sh:38`, `S/doom.sh:74`.
79. `disk`: merge. Keep the baseline real controller/data path. `M:478`.
80. `diskpolicy`: merge. Preserve driver-level refusal and whole-image comparison. `S/diskpolicy.sh:90`, `S/diskpolicy.sh:109`.
81. `console`: merge. Preserve exact input and raw/cooked/alternate-terminal behavior. `M:389`, `M:395`.
82. `fpu`: keep. Preserve XSAVE, FXSAVE, no-FXSR and i686 x87 profiles. `M:291`.
83. `socket`: merge. Preserve actual TCP/UDP host peers in network integration. `M:465`.
84. `bootorder`: merge. Preserve booting the real root from disk1 partition3. `S/bootorder.sh:43`.
85. `reset`: keep. Preserve actual emulator exit and fallback-specific reset mechanisms. `S/reset.sh:75`, `S/reset.sh:113`.
86. `netconsole-e1000`: merge. Parameterize the network-console driver path. `M:2142`.
87. `disks`: merge. Preserve mtools proof of the actual destination disk. `S/disks.sh:20`, `S/disks.sh:34`.
88. `audio`: keep. External WAV analysis and interrupt growth are strong output checks. `M:409`, `M:433`, `M:455`.
89. `netconsole`: merge. Preserve buffered pre-NIC records, size limits and shutdown output. `M:1337`, `M:1379`.
90. `crash`: merge. Preserve dirty/clean state and overlapping set-id/root-dirty behavior. `S/crash.sh:96`, `S/crash.sh:247`.
91. `pools`: keep. Preserve exhaustion and reclamation with many live objects. `M:344`.
92. `runner`: rewrite. Keep runner regression tests, selected when their implementation changes. `M:2085`, `S/host.sh:171`.
93. `diskfull`: keep. Preserve ENOSPC versus EIO and reuse after deletion. `M:456`.
94. `fault`: merge. Describe expected SIGSEGV and kernel survival in probe metadata. `M:353`.
95. `imagerace`: merge. Keep publication/readability races in selected build-tool tests. `M:2102`, `M:2113`.
96. `krffscheck`: merge. Keep malformed-image checking in host filesystem-tool tests. `M:2106`.
97. `codexapi`: merge. Preserve pinned tool interoperability, exact changes and wrong-result refusal. `S/codexapi.sh:207`, `S/codexapi.sh:243`.
98. `museapi`: merge. Preserve exact steps, git state and wrong-result refusal. `S/museapi.sh:236`, `S/museapi.sh:268`.
99. `conload`: keep. Numbered, checksummed input across parking, SIGINT and exec has a substantive oracle. `S/conload.sh:58`.
100. `drive`: merge. Keep fake-machine readiness/wait behavior with runner regression tests. `M:2094`.
101. `textbusy`: merge. Keep ETXTBSY and removed-bind retention in VFS lifetime cases. `M:2168`.

### B3. Host tests, probes and mutation families

#### Host tests

The 148 host programs partition into six families:

* Libraries, memory and process: 31. Keep actual implementation, sanitizer checks, allocation failures and selected interleavings. Add a ChaCha known-answer test; seed variation and bit balance do not establish the algorithm. `tests/host/Readme.md:116`, `tests/host/krandom_hosttest.c:42`.
* Descriptors, files and filesystems: 35. Keep lifetime, failed-write, crash-state and allocation checks. Share fixture code. Rewrite configuration-specific scaffolding where necessary. `tests/host/Readme.md:150`.
* Buses, storage and block layer: 21. Keep command sequencing, DMA boundaries, late completions and failure injection. `tests/host/Readme.md:188`.
* Platform, display and audio: 18. Keep register/table models and output formats; pair important models with guest profiles. `tests/host/Readme.md:212`.
* Networking: 23. Keep parsers, protocol state and deterministic failure cases; avoid repeating all stack tests for each NIC. `tests/host/Readme.md:234`.
* Boot, tools, accounts and shell: 20. Keep rollback, short I/O, authentication and shell-status checks; select them by owner. `tests/host/Readme.md:260`.

The host suite also runs six sanitized checker selftests, 26 scripted checks and eight fuzz replays. Keep them as separate selectable cases. `S/host.sh:171`, `S/host.sh:197`, `S/host.sh:399`.

Host models do not establish actual privilege transitions, interrupts or all possible interleavings. The existing documentation says so. `tests/host/Readme.md:108`.

#### Guest probes

The 165 scored or driven guest programs partition as follows:

* File/VFS: 43. Keep raw ABI and libc semantics; repair weak byte checks.
* Process, ABI, signals, time and namespaces: 39. Keep actual frames, exits, waits and namespace state.
* Memory and FPU: 19. Keep contents, permissions, mapping lifetime and CPU variants.
* Credentials and permissions: 18. Keep privileged/unprivileged transitions and fixture restoration.
* IPC, TTY and readiness: 13. Keep exact bytes, status and readiness.
* Networking: 12. Keep focused APIs plus selected wire tests.
* Hardware/storage integration: 20. Keep actual device, interrupt, data and counter checks.
* Broad compatibility: `callsmusl`, one program. Keep its assertions, but expose named selections and diagnostics.

The remaining programs are three executable fixtures and 17 manual or diagnostic programs. They should remain distinguishable from compulsory coverage. References: `tests/probe-suites:1`, `tests/probe-helpers:1`, `tests/system/runner/runtests.c:61`.

Raw and libc probes are complementary where they take different ABI paths. Removing one family wholesale would lose coverage.

#### Mutation scripts

* The 68 host mutation scripts should become selective proof jobs with shared mutation/build/restore support.
* The 45 system mutation scripts should run the affected case rather than a full default boot for each fault.
* The three acceptance mutation scripts should remain separate from ordinary acceptance.
* The other 35 mutation-capable scripts should retain their ordinary regressions under the owning tool, with mutation mode selected explicitly.
* The eight mutation-named scripts invoked with `--check` contain ordinary regression behavior. Preserve that behavior outside mutation plumbing. `S/host.sh:197`.
* Preserve pass/fail/pass evidence for a new test. Bind each step to the actual source and artifact digests.
* Remove the requirement that unrelated work preserve every historical source-text mutation pattern. `tools/verify/faulttexts.sh:5`.

The probe mutation driver currently runs the full default set for each fault and relies on a later full run for restored evidence. Both should become explicit selected steps. `tests/system/probe_faults.sh:5`, `:15`.

### B4. Pointless and overengineered work

These cuts are supported by the code.

* Delete raw `hello` as an independently scored correctness row. It ignores the write result and returns zero. Keep it as an executable fixture where the surrounding test checks a fault or kernel survival. `tests/hello.c:35`.
* Rewrite `filetest`: a nonnegative read is insufficient. Require the exact count and contents. `tests/filetest.c:86`.
* Rewrite the USB keyboard oracle. It sends specific bytes but accepts a broad stdin token; `stdintest` accepts any positive input. Use exact bytes. `S/usb.sh:95`, `tests/stdintest.c:110`, `:138`.
* Remove the six 32 MiB disk-placement transfers from compulsory regression work. They are explicitly diagnostic and do not judge a rate. `S/diskrate.sh:313`.
* Move exhaustive manual-page content and alias verification to the host. Keep guest decompression, option and pager behavior. `S/man.sh:42`, `:59`.
* Remove the five copied-tree workflow mutation sweeps from ordinary `agents` execution. Preserve a short unmodified guest workflow smoke and the real worker session. `tests/system/agents_rows.sh:78`.
* Select runner selftests for runner changes. They are useful infrastructure tests, but do not validate an unrelated kernel change. `M:2085`.
* Remove the inert `--bootwait` option. It is accepted and ignored. `tests/system/runner/harness.c:28`.
* Replace shell-header scraping for agent metadata with validated fields. `S/agents.sh:239`, `:246`, `:248`, `:255`.
* Replace eight e1000 Make wrappers with profile parameters. `M:2117`.
* Replace general prompt matching with structured readiness where the prompt is not itself under test.
* Keep prompt/screen checks for login, tmux and paging, where the visible interface is the behavior.

Doom currently checks reported geometry, survival and exit, not rendered pixels. State that limit accurately. Audio has an external sampled-output oracle and should remain. `S/doom.sh:42`, `:60`; `M:409`.

### B5. Missing coverage

#### System calls

The union in `rach2/syscalls.md` contains 311 names: 284 registered on i686 and 261 on x86-64.

Static inspection found:

* 195 names with explicit raw-number, macro or probe-helper references in participating guest tests.
* 84 with focused libc/API or shared-handler semantic exercise.
* Nine compatibility entries without a focused exact-entry test.
* 22 entries without a focused exact-entry probe or executed host-handler test found.
* One application-use inference: `clone3`.

These are evidence categories, not a dynamic coverage percentage. A GNU program may call an entry without a focused assertion about that entry.

The 22 focused-test gaps are:

```text
epoll_create epoll_wait eventfd fdatasync getrlimit inotify_init
mq_timedreceive_time64 mq_timedsend_time64 preadv pselect6 pwritev
readlinkat rt_tgsigqueueinfo setdomainname setregid setregid32
setrlimit signalfd symlinkat timer_settime64 timerfd_settime64
ugetrlimit
```

Prioritize them by behavior:

* `preadv` and `pwritev`: wide offsets, iovec boundaries, partial I/O and exact contents. `rach2/sys/syscall_fs.c:9143`, `:9164`, `:9219`.
* `pselect6`: atomic signal-mask replacement, readiness and interruption. `rach2/sys/io/poll.c:702`.
* `readlinkat` and `symlinkat`: relative directory descriptors, errors and resulting bytes. `rach2/sys/syscall_fs.c:6517`, `:8700`.
* `rt_tgsigqueueinfo`: target identity and delivered signal information. `rach2/sys/syscall_proc.c:3803`.
* `setdomainname`, `setregid` and `setregid32`: privilege and credential transitions. `rach2/sys/syscall.c:2655`, `rach2/sys/syscall_proc.c:5071`.
* Legacy epoll, eventfd, signalfd, inotify and rlimit adapters: direct ABI checks. Modern musl normally selects newer entries.
* Time64 setters and message queues: large seconds and old-value output on i686.

Do not describe all time64 calls as untested. `clock_settime64` has a past-2038 test; musl selects time64 timer getters on i686. `tests/musl/clockidmusl.c:532`, `:563`, `tests/musl/callsmusl.c:1101`.

The nine compatibility entries are:

```text
fgetxattr fremovexattr fsetxattr lgetxattr llistxattr lremovexattr
removexattr setxattr mlock2
```

These need focused validation/refusal tests, not invented feature tests. The xattr implementation returns no attributes or `EOPNOTSUPP`; memory is already resident without swap. `rach2/sys/syscall_fs.c:3890`, `:3933`, `rach2/sys/syscall_proc.c:7011`.

`clone3` may be exercised by the GNU Perl-thread row, but that is an inference without entry tracing or a focused test. `tests/system/gnu_rows.sh:185`, `rach2/sys/syscall_proc.c:2471`.

#### Drivers and subsystems

* NVMe is tested by a host model and the UEFI suite. Add a small routine read/write/flush/reboot profile independent of the large UEFI installation case. `S/uefi.sh:517`, `:544`.
* Add PC-speaker fallback checks for frequency division, clamping, gating and silence. Current sampled audio covers HDA and SB16. `rach2/sys/arch/x86/speaker.c:25`, `:54`, `M:413`.
* Add actual PS/2 mouse input. The guest mouse profile is USB with i8042 disabled. `S/usb.sh:115`.
* Add Lenovo built-in keyboard/touchpad acceptance. Existing records do not establish it. `rach2/hardware.md:114`, `:141`.
* Add selected reference-clock fallback profiles. The host model checks stalled HPET and fallback, but does not establish every real HPET/PM/TSC/PIT path. `tests/host/refclock_hosttest.c:81`, `:103`, `:115`.
* Add UART register-error and unusual interrupt-state fault tests. Normal serial I/O already has extensive coverage. `rach2/hardware.md:96`, `S/conload.sh:58`.
* Preserve real RTL8168 acceptance. QEMU does not replace that device with an equivalent emulation. `rach2/hardware.md:90`, `:134`.
* Preserve physical EHCI takeover and USB2 socket/hub acceptance. Generic xHCI does not establish chipset routing. `rach2/hardware.md:52`.
* Add ELF64 fuzzing and stateful writable-filesystem fuzzing with crash points. Current ELF fuzzing is i686-oriented, and filesystem fuzzing mainly mounts and reads. `tests/fuzz/elf_fuzz.c:15`, `:117`, `tests/fuzz/fs_common.h:175`.
* Add the ChaCha known-answer test noted above.
* Keep explicit SMP boundary profiles; selected host interleavings do not cover arbitrary concurrent execution.

Unsupported wireless, IPv6, UAS, USB audio/video and lid handling are implementation limits, not missing tests of implemented features. `rach2/hardware.md:46`, `:57`, `:92`, `:143`.

#### Agent milestone behavior

The redesign must preserve:

* Unmodified native GNU and vendor agents.
* Scripted tool use judged from files, commits and recorded requests.
* Ordinary workflow execution on Rach2.
* A real two-session worker run.
* Installed interactive, credential and reboot acceptance.

These are explicit milestone requirements. Host-only workflow tests cannot replace them. `dev-log/61-agents/plan.md:11`, `:33`, `:72`, `:73`, `:78`.

### B6. Dependencies on a Unix or Linux-like host

The dependency set is larger than the registry.

#### Declared external tools

The registry contains 37 distinct names:

```text
clang clang-format codex cppcheck curl debugfs e2fsck fsck.exfat
git hdiutil ip mcopy mdel mdeltree mdir mformat minfo mke2fs
mkfs.exfat mmd mtype nc newfs_exfat openssl python3 qemu
qemu-img qemu-system-x86_64 scp sftp shasum shellcheck ssh
ssh-keygen tmux unshare xorriso
```

These include alternatives and QEMU aliases, not 37 mandatory executables for every run. `tests/registry:5`, `:30`, `:37`.

Builders additionally require GNU Make, host C tools, cross GCC/binutils, archive tools, e2fsprogs, mandoc and patching tools. Host sanitizer tests require compiler support for AddressSanitizer and UndefinedBehaviorSanitizer. `tools/Readme.md:9`, `:31`, `S/host.sh:27`.

#### Shell and process environment

* The runner depends on a Unix shell and `awk`, `sed`, `grep`, `find`, `sort`, `cut`, `tr`, `wc`, `cmp`, `cksum`, `tar`, `dd`, `mktemp`, `chmod`, file utilities, `tee`, `date`, `ps`, `kill`, `wait`, `mkfifo`, `head`, `tail` and `uname`. `tests/run:56`, `:86`, `:163`, `:193`, `:285`, `:348`, `:840`.
* GNU Make's database and command options are part of discovery/execution. `tests/run:809`, `:1102`.
* Resource discovery uses `sysctl`, `nproc` and `/proc` fallbacks. `tests/run:107`, `:125`.
* `/dev/null`, `/dev/zero`, `/dev/urandom`, FIFOs, executable permissions and signal semantics are assumed by shell fixtures.
* Native C uses `fork`, `exec`, process groups, `waitpid`, `sigaction`, `fcntl`, `poll`, `select`, `nanosleep` and host clocks. `tests/system/runner/runutil.c:35`.
* The monitor uses `AF_UNIX`; interactive execution uses `forkpty`, with Apple and other Unix headers. `tests/system/runner/harness.c:305`, `tests/system/runner/ptydrive.c:46`, `:433`.

There is no explicit `-lutil` option in the current Make rules. The dependency is the PTY API itself.

#### Linux and Darwin special cases

* Apple process inspection uses `libproc`. Other branches assume Linux `/proc/PID/stat` and `/proc/PID/fd`. `runutil.c:61`, `:388`, `:519`, `:755`.
* `flock(1)` occurs in the remote WSL sync adapter. `tools/dev/testing.sh:167`.
* The runner's `struct flock` is the `fcntl` record-lock API, not the `flock` command. `runutil.c:1358`, `:1464`, `:1626`.
* Flow uses Perl `flock`. `tools/flow/worker.sh:80`, `tools/flow/release.sh:468`.
* Host `setsid(1)` occurs in `tests/system/bindhold_mutate.sh:86` and `textbusy_mutate.sh:54`.
* Host `setsid(2)` occurs in `tests/host/pwauth_hosttest.c:2985`, `:3130`, `:3173`; Python `os.setsid` occurs in `tests/host/ish_mutate.sh:155`.
* The `codexapi` and `museapi` host adapters require Linux user/network namespaces and iproute2. `S/codexapi.sh:56`, `S/museapi.sh:65`.
* No cgroup, pidfd or subreaper implementation was found in the audited runner.

#### Languages, images and orchestration

* Python provides socket, TLS, subprocess, threading, signal, JSON and filesystem support. Some fixtures use `fcntl`, including the fake `gh` helper. `tests/system/agents/fakegh.py:30`.
* Perl supplies process supervision, locks, globbing, alarms and mutation watchdogs. `tools/flow/session.pl:24`, `tools/flow/worker.sh:80`, `tests/host/ext2_batch_mutate.sh:225`.
* Image preparation uses e2fsprogs, mtools, xorriso, qemu-img and x86 firmware. ExFAT selects macOS `hdiutil`/`newfs_exfat` or Linux tools. `S/large.sh:38`.
* CI uses GNU `timeout -k` and GNU `find` depth options. `tools/ci/bisect.sh:153`, `.github/workflows/ci.yml:86`, `tools/ci/run.sh:117`.
* Flow and owner adapters add `gh`, `jq`, curl, tmux, OpenSSH and rsync 3. `tools/Readme.md:31`, `tools/dev/testing.sh:101`.
* The owner adapter explicitly crosses Windows OpenSSH, MSYS, `wsl.exe` and bash. `tools/dev/testing.sh:67`, `:167`, `:206`, `:250`, `:286`.
* Remote inspection uses `/proc` and `find -mmin`. `tools/dev/testing.sh:147`, `:351`.
* Resource monitoring uses macOS `memory_pressure`, BSD/GNU date alternatives and PowerShell/WSL. `tools/flow/status.sh:128`, `:132`, `:197`.
* The testing host's `/tmp` tmpfs is deployment configuration. It should not be a test-system requirement. Shared port locks currently use `/tmp`.

Guest `/proc`, guest `flock`, guest `setsid` and GNU commands intentionally tested inside Rach2 are not host dependencies. For example, the GNU `dd` options in `tools/dev/machine_grow.sh:207` run in the guest.

### B7. Timing census

#### Scope and counting rule

The audit found 2,331 execution timing expressions, plus 23 unpaced retry or CPU-work proxies.

This is a source count. It is not a sum of seconds and not a count of runtime invocations.

One expression means an independently written duration, tolerance, factor, timed retry ceiling or explicit timing boundary:

* A timespec or duration range counts once.
* Separate timeout and step options count separately.
* A named definition counts once; variable-only reuse adds nothing.
* Repeated literal arguments count separately.
* Both architecture branches count.
* Calendar fixtures, unit conversions, ordinary workload iteration counts, scanner fixtures and model-clock state sequences are excluded.

The excluded model-clock data are deliberate simulation inputs. They must remain separate from hardware-dependent completion limits.

The disjoint areas are:

* Production suite control: 891.
* `tests/run`: 10.
* Runner C: 95.
* Other system scripts, embedded fixtures and helper C: 541.
* System Python: 35.
* Consumed JSON fixtures: eight.
* Host tests and mutation scripts: 126.
* CI, development adapters, flow loops and workflows: 111.
* Raw guest C: 163.
* Musl guest C: 351.

The 490 literal timeout-related CLI options and 121 production `scaled` expressions are subsets of these counts.

Outside this primary scope are one 60-second prerequisite-adapter limit in `tools/setup/space.sh:37` and three three-second generated boot-menu settings in `tools/image/mkiso.sh:183` and `tools/image/mkdisk_grub.sh:379`, `:432`.

#### Classification

The 1,817 non-probe expressions comprise:

* 1,117 completion budgets.
* 286 polling, delay or holder intervals.
* 33 SSH interval, timeout or count settings.
* Seven zero or invalid timing boundaries.
* 29 cleanup grace settings.
* 179 runner/selftest timing settings.
* 66 timed retry ceilings or period counts.
* Nine timed workloads.
* 35 factor, adaptive or retention-policy settings.
* 19 semantic timing assertions or API inputs.
* 26 guest fault-timing settings.
* 11 QEMU clock, audio or throttle settings.

There are 33 factor expressions in total. Fourteen are classified with selftest settings rather than the policy group.

Raw guest C adds:

* 15 completion guards.
* 90 readiness/race delays, paced retries and accounting quanta.
* 28 semantic inputs, expected durations or tolerances.
* 12 timed workloads.
* 18 zero, invalid or nonblocking boundaries.
* Separately, 15 unpaced retry or work proxies.

Musl guest C adds:

* 79 completion guards and accounting quanta.
* 78 readiness/race settings, including 28 paced retry ceilings.
* 101 semantic timer inputs, expected durations and tolerances.
* Three timed workloads.
* 90 zero, disarm, infinite, nonblocking or invalid boundaries.
* Separately, eight unpaced retry or work proxies.

A zero-timeout poll and a requested timer duration are valid API tests. A sleep used to assume that another process has reached a state needs replacement.

#### Production per-file counts

The following names have prefix `tests/system/suites/` and suffix `.sh`:

```text
account=22 agents=13 bigmem=6 bootline=8 bootorder=2 bus=6
codexapi=5 conload=2 crash=3 diskdiag=7 diskpolicy=1 diskrate=10
disks=2 doom=3 drill=20 exfat=16 exitline=8 ext2=10 fakeapi=13
fatwrite=13 gnu=89 grow=4 grub=6 install=9 installer=80
krffsmount=25 large=2 load=5 login=6 loopdev=11 make=26 man=4
memory=7 menu=7 metal=14 mount=38 museapi=6 parts=8 procfiles=10
queue=24 reset=4 sealed=14 selfbuild=24 servicelog=4 shutdown=5
smp=12 socket=3 ssh=15 subset=3 tcc=20 uefi=26 usb=50 usbdisk=26
host=1
```

`tests/system/suites.mk` adds 133.

Representative dense sites are `S/gnu.sh:244`, `S/installer.sh:236`, `S/mount.sh:71`, `S/queue.sh:96`, `S/usb.sh:211` and `M:1036`.

The 42 production shell sleeps divide into 20 readiness waits, five FIFO holders, five timestamp waits, three continued-execution checks, four workload/descriptor holders, four settling waits and one reset wait.

#### Probe per-file counts

Raw C names below have prefix `tests/` and suffix `.c`:

```text
ahciprobe=4 conloadprobe=4 dgramprobe=4 exitprobe=4 fpuprobe=1
fsbaseprobe=1 jobprobe=1 lagprobe=7 limitprobe=3 loadprobe=3
loopprobe=9 memprobe=1 modeprobe=1 mountprobe=5 mouseprobe=2
netifprobe=7 packetprobe=10 pingtest=3 pollprobe=10 procprobe=12
ptyprobe=11 rawprobe=13 sigprobe=28 stattest=2 timeprobe=3
ttyprobe=1 udptest=2 usbdiskprobe=4 usbprobe=2 waitprobe=2
xsaveprobe=2 zombieprobe=1
```

Examples include `tests/ahciprobe.c:112`, `tests/pingtest.c:61`, `tests/procprobe.c:1729`, `tests/sigprobe.c:357` and `tests/usbdiskprobe.c:482`.

Musl names below have prefix `tests/musl/` and suffix `.c`:

```text
addusermusl=9 authmusl=3 callsmusl=30 capmusl=2 clockidmusl=35
clockloadmusl=2 clockmusl=4 clonemusl=5 cputimemusl=8
epollmusl=64 fifomusl=9 hdaprobe=1 inotifymusl=2 ipcmusl=12
lockmusl=7 loginmusl=6 netlinkmusl=1 opathmusl=2 passwdmusl=11
pollmusl=5 prctlmusl=1 ptracemusl=24 pwauthmusl=34 schedmusl=1
sigframemusl=3 smpmusl=4 sockflagmusl=2 stampmusl=1 statmusl=3
threadmusl=20 timemusl=2 timerfdmusl=23 unixmusl=11 vforkmusl=1
waitidmusl=3
```

These counts exclude the separately listed eight work proxies. Important sites include `clockidmusl.c:74`, `epollmusl.c:135`, `pwauthmusl.c:744`, `threadmusl.c:397` and `timerfdmusl.c:181`.

#### Runner, host and outer limits

Runner C counts are:

* `deadline.c`: three, beginning at line 110.
* `runutil.c`: 17, beginning at line 111.
* `hostpeer.c`: three, beginning at line 86.
* `ptydrive.c`: seven, beginning at line 327.
* `runtests.c`: 12, beginning at line 635.
* `harness.c`: 53, beginning at line 279.

The 541 other system expressions include 157 in `tests/system/selftest.sh`, 54 in `accept/agents_accept.sh`, 32 in `drivetest.sh`, 28 in `gnu_rows.sh`, agent interaction scripts, mutation watchdogs, helper processes and four sample-clock assertions in `checkers/wavcheck.c:195`, `:225`, `:231`, `:238`.

The host total includes 43 real execution expressions in `pwauth_hosttest.c`, three `host-seconds` declarations and 80 mutation alarm/sleep settings. The pwauth test includes elapsed-time assertions at lines 1978, 1981 and 2032. Those need semantic replacement.

Outer fixed limits include:

* Worker session: 21,600 seconds. `tools/flow/worker.sh:66`.
* Review: 10,800 seconds. `tools/flow/reviewer.sh:71`.
* Manager: 2,400 seconds. `tools/flow/manager.sh:33`.
* Core command: 170 minutes. `.github/workflows/ci.yml:86`.
* Full architecture command: 340 minutes. `.github/workflows/full.yml:65`, `:70`.
* Whole full job: 720 minutes. `.github/workflows/full.yml:44`.

These are operational policies. They should not become guest correctness criteria.

#### Concrete timing defects

1. Scaling can occur twice. `S/agents.sh:179` passes `scaled 120` to `runutil until`; `runutil.c:108` and `:173` multiply it again. Factor four becomes sixteen on that path.
2. Several helpers use calendar time for deadlines. `ptydrive.c:318`, `runtests.c:626` and `hostpeer.c:77` use `gettimeofday`; `runutil` and `harness` use a monotonic clock.
3. Suite activity can keep an execution alive indefinitely. Output, CPU activity or process changes refresh the idle mechanism. A busy loop is activity. `runutil.c:1037`, `:1074`, `:1182`.
4. Some guest expectation helpers charge only poll timeouts or failed-read naps. Continuous irrelevant output can extend their nominal limits. `tests/musl/addusermusl.c:301`, `passwdmusl.c:293`, `loginmusl.c:164`, `pwauthmusl.c:944`.
5. The clock test compares host receipt times within 250 ms. Receipt includes host scheduling and serial-delivery uncertainty. `tests/system/clockload.py:51`, `:122`; `S/clock.sh:17`.
6. Unpaced read/spin ceilings are hidden timing assumptions. Examples include `tests/bootprobe.c:433`, `tests/usbdiskprobe.c:561` and `tests/musl/smpmusl.c:61`.

### B8. What the measured run establishes

The supplied run at `91786e2f` used TCG, 12 workers, 24 GiB and two virtual CPUs. It took 73 minutes for i686 and 163 minutes for x86-64. The x86-64 `agents` suite consumed 6,532 seconds. The run did not pass completely. `../measured.txt`, after `MEASURED`.

That identifies expensive work and a critical path. It does not establish a lower bound for the redesign.

The older KVM result is real and must be retained in the analysis. It used a different revision, suite count and concurrency. Neither old result supports promising a particular new release duration.

## Part C. Design

### C1. The test description

Use validated JSON with a small versioned schema.

JSON is supported by current tools and can be parsed by a small portable C library. Avoid YAML's larger parsing surface and avoid a new executable configuration language.

Each case declares:

* Stable case ID and owning component.
* Test kind and execution context.
* Supported guest architectures.
* Required CPU features, devices, firmware and image roles.
* Required host capabilities and external tools.
* Accelerator constraints.
* Memory, processors, disk reservation and exclusive resources.
* Network topology and allowed peers.
* Fixture setup and reset contract.
* Executable or named adapter.
* Parameters that are functional test inputs.
* Expected exit, signal, artifacts and independent verifiers.
* Source dependencies and selection groups.
* Expected terminal observation boundary.

The schema must not contain a per-case wall-clock pass/fail timeout.

Profiles hold repeated machine configurations. A case references a profile and adds requirements. The resolved plan expands all references, records artifact digests and rejects unknown fields, duplicate IDs, missing tools and unsatisfied requirements before starting.

Use exact integer parsing for sizes and offsets. Do not route 64-bit disk values through floating-point JSON numbers.

#### Example 1: the existing kernel string host test

```json
{
  "schema": 1,
  "id": "host/kstring",
  "kind": "host-native",
  "context": "isolated-process",
  "requires": {
    "profile": "native-c-sanitized",
    "tools": ["c-compiler"],
    "features": ["address-sanitizer", "undefined-sanitizer"]
  },
  "inputs": {
    "program": "artifact:host/kstring-test"
  },
  "sources": [
    "tests/host/kstring_hosttest.c",
    "rach2/sys/lib/kstring.c",
    "rach2/sys/lib/kctypes.c"
  ],
  "resources": {
    "class": "host-unit",
    "processors": 1
  },
  "execute": {
    "adapter": "native-process",
    "argv": []
  },
  "oracle": {
    "exit": 0,
    "sanitizer_reports": 0
  },
  "terminal": "process-reaped"
}
```

This preserves today's test. Its host execution tests the portable byte path; target string instructions still need the existing guest sweep. `tests/host/kstring_hosttest.c:5`, `:43`.

A plain native profile can run the same assertions on Rach2. It must not be reported as sanitizer coverage.

#### Example 2: the existing SB16 scale test

```json
{
  "schema": 1,
  "id": "audio/sb16-scale",
  "kind": "qemu",
  "context": "kernel-debug-shell",
  "requires": {
    "guest_arch": ["i686", "x86_64"],
    "profile": "pc-sb16-rescue",
    "accelerator": ["tcg"],
    "features": ["qmp", "wav-audio", "icount"]
  },
  "inputs": {
    "kernel": "artifact:boot-kernel",
    "cpu": "max",
    "boot_options": ["rach2.rescue", "rach2.nonet"]
  },
  "resources": {
    "class": "guest-small",
    "processors": 2
  },
  "clock": {
    "mode": "icount",
    "shift": 3,
    "sleep": false
  },
  "execute": {
    "adapter": "kernel-command",
    "command": "audio test scale",
    "arguments": {"duration_ms": 2000}
  },
  "outputs": ["audio.wav", "console.log"],
  "oracle": {
    "command_completed": true,
    "verifier": "wav-scale",
    "frequencies_hz": [262, 294, 330, 349, 392]
  },
  "terminal": "command-complete-and-capture-closed"
}
```

The 2,000 ms value is an audio input. It is not a host completion limit. This is the SB16 case at `M:444`. The HDA case remains separate and retains its interrupt-growth assertion at `M:433`.

The current wrapper supplies two virtual CPUs by default. `tests/run:980`, `:1197`.

#### Example 3: the real worker session

```json
{
  "schema": 1,
  "id": "agents/worker-session",
  "kind": "guest-session",
  "context": "ordinary-user",
  "requires": {
    "guest_arch": ["x86_64"],
    "profile": "gnu-ahci-rtl8139",
    "images": ["gnu-agents-root"],
    "services": ["ssh", "recorded-agent-api"]
  },
  "inputs": {
    "kernel": "artifact:kernel",
    "root": "artifact:gnu-agents-root",
    "scenario": "artifact:worker-session"
  },
  "resources": {
    "guest_memory_mib": 4096,
    "processors": 2,
    "class": "guest-large-with-api"
  },
  "network": {
    "mode": "isolated-user-network",
    "peers": ["recorded-agent-api"]
  },
  "execute": {
    "adapter": "guest-scenario",
    "scenario": "worker-session",
    "user": "test"
  },
  "outputs": ["session-record", "repository-archive", "transcripts"],
  "oracle": {
    "exit": 0,
    "verifier": "worker-session",
    "sessions": 2,
    "verify_exact_api_steps": true,
    "verify_files_commits_board_and_binary_digests": true
  },
  "terminal": "session-reaped-and-artifacts-verified"
}
```

This preserves the existing external verifier, which checks ordered sessions, exact requests, files, commits, closing tags, final state and binary digests. `tests/system/agents/worker_session_verify.py:43`, `:86`, `:109`, `:150`.

The profile selects an explicitly validated CPU/accelerator combination. It does not assume that `-cpu max` means the same thing under every accelerator.

### C2. Environments and artifacts

An environment declaration describes capabilities discovered or provisioned on a particular host:

* Host OS, version and instruction set.
* QEMU version, supported machine versions, devices and accelerators.
* Available firmware and tools with versions/digests.
* Process containment, filesystem isolation, PTY and suspension capabilities.
* CPU, memory and disk capacity available to the broker.
* Network fixture support.
* Physical devices and exclusive access requirements.

A configured capability is not enough. Startup probes should verify that the selected accelerator and essential backends can actually start.

Use separate environment kinds:

* Native host process.
* QEMU guest.
* Native Rach2 process or session.
* Installed QEMU system.
* Physical Lenovo acceptance.

A Mac that cannot accelerate x86 can still run portable host tests and local TCG cases, and submit accelerated work to another host. Native Windows uses the Win32 runner and native QEMU/WHPX. It does not require WSL.

Build inputs once into an immutable artifact bundle containing:

* Kernel, roots, probes and required firmware.
* Symbols and source revision.
* Build configuration and toolchain identity.
* Catalog and verifier versions.
* Digests and runtime dependencies.

Mutable disks belong to attempts. Record which exact disk each verifier reads.

The release bundle must include the hardware artifacts that will be installed. Today the release builds the Lenovo artifacts after the full run. The new flow should validate the hardware bundle before installation and install those same bytes. `tools/flow/release.sh:30`.

Selfbuild is a deliberate exception to fixed inputs: it produces a derived artifact bundle. Extract, hash and boot the newly built bytes. Installation tests must prove which marked kernel actually executed, not merely which bytes were copied.

### C3. Scheduler

Use the same planner locally and in CI.

The planner:

1. Expands selection into a complete case/profile plan.
2. Resolves artifact and fixture dependencies.
3. Matches requirements against environment capabilities.
4. Reports unsatisfied requirements before execution.
5. Assigns cases to available hosts.
6. Records selection and placement reasons.

A small resource broker on each physical host owns allocations across accounts and runs. It accounts for QEMU, host tests, compilers, image preparation, API servers and external verifiers.

A lease is a temporary reservation of those resources. It includes CPU capacity, memory, expected writable disk consumption and exclusive objects such as hardware, ports or image builders.

Do not release a reservation merely because its lease expired. First confirm that its processes and writable resources have been reclaimed. Broker restart must recover ownership without confusing reused PIDs with old jobs.

Use measured durations to order work and balance shards. Do not use them to decide correctness.

Fixed account shares can remain administrative limits. The broker permits safe use of otherwise idle capacity and prevents independent runs from exceeding the physical host's budget.

For one exclusive local invocation, the broker can be embedded. Multiple accounts require the shared service. Remote coordination should be thin; a coordinator failure must not erase running attempts or release their resources.

### C4. Runner language and implementation

Use portable C with a conservative C99 subset.

Reasons:

* TinyCC can build it on Rach2.
* The project already maintains C tools and host tests.
* The core does not require a language runtime.
* Linux, macOS and Windows can share planning, JSON, result and protocol code.
* Existing useful checker algorithms can be retained.

The kernel's C89 rule does not require every hosted tool to use C89. Avoid VLAs, compiler-specific types and extensions in the shared core.

C does not supply process or network APIs. Put those behind explicit modules:

* POSIX process and filesystem module.
* Darwin-specific process inspection and PTY support.
* Win32 process, Job Object, path and file module.
* Rach2 module for supported native execution and remote coordination.

Windows needs deliberate handling of UTF-16 paths, argument quoting, handle inheritance, large file offsets, sparse files and LLP64 types. A successful MinGW build alone would not establish a correct native port.

Alternatives have costs:

* Python is portable across the three desktop hosts, but adds a substantial runtime to the small Rach2 system.
* Rust and Go provide useful libraries, but native Rach2 runtime/toolchain support would become another project.
* Shell retains the central native-Windows problem.
* C++ adds toolchain and library requirements without a clear benefit for this runner.

Use existing portable libraries where they solve a real problem. Do not implement TLS, filesystem repair or SSH from scratch. External verifiers remain declared tools with native providers or remote execution placement.

#### Isolation

Process ownership and filesystem isolation are separate capabilities.

* On Linux, use a delegated cgroup where available, plus controlled working directories and restricted mounts for tests that require filesystem isolation. Cgroups alone do not isolate files. [cgroup v2 documentation, undated, accessed 2026-10-19](https://docs.kernel.org/admin-guide/cgroup-v2.html).
* On Windows, create the process suspended, assign it to a Job Object, then start it. Prevent unwanted breakaway and close the job to terminate its process tree. A Job Object does not isolate files. [Job Objects, undated, accessed 2026-10-19](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects).
* On macOS, ordinary trusted tests can use process groups and private directories. A descendant can escape a process group. Tests requiring strict containment should run in a VM or another declared isolation provider.
* On Rach2, advertise the isolation actually implemented. Do not claim desktop-host guarantees that its kernel does not provide.

QEMU gets no arbitrary host filesystem mount. Its writable images, sockets and logs belong to the attempt.

### C5. Execution context and result protocol

This is the most important compatibility constraint.

Today's default probes run as transient init processes. When init exits, the kernel ends the remaining user processes before returning to the debug shell. A persistent test service that forks every probe changes orphan handling, signals and resource baselines. `tests/system/runner/runtests.c:21`, `rach2/sys/syscall_proc.c:481`, `:496`, `:531`.

Keep three distinct executors:

* Transient init under the kernel debug shell.
* Ordinary process or service session.
* Installed system across boots.

Initially preserve each test's current context.

The host owns the expected plan and the final verdict. It requires:

* Every planned case exactly once.
* Correct run, case, attempt and boot-generation identity.
* The expected process exit or signal.
* Required artifacts.
* Successful independent verifiers.
* Successful cleanup where cleanup is part of the contract.

Subject stdout is data. It cannot authorize completion.

The current runner already tests forged marker text and a successful child followed by a failed parent. Preserve those regressions. `tests/system/runner/runtests.c:481`.

Use framed control records with lengths, sequence numbers and corruption detection. A checksum is not authentication. Records must originate from the trusted executor or kernel completion path.

Rach2 currently has COM1, not an existing second control UART. Preserve console-input tests without having a relay consume their bytes. Use the existing serial path carefully, or a separate network/result channel where the test permits it. A second UART or virtio-serial driver would be explicit implementation work. `rach2/hardware.md:96`.

Expected faults remain explicit:

* A protection test expects the intended process to receive SIGSEGV and the kernel to execute another program afterward.
* A reset test requires actual emulator termination.
* An expected boot refusal is different from an unexpected panic.
* A runner kill cannot satisfy a poweroff test.

Define the observation boundary for each test. Do not add a universal sleep after a success marker.

#### Result format

Write append-only JSON Lines during execution and a final JSON summary. Generate human summaries from those records.

For example:

```json
{
  "case": "fs/ext2/small-rewrites",
  "attempt": 1,
  "state": "complete",
  "outcome": "fail",
  "assertion": "device-command-count",
  "expected": {"maximum": 250},
  "actual": 312,
  "artifact_set": "sha256:...",
  "environment": "linux-x86-kvm-07",
  "evidence": ["case.log", "disk.raw", "counters.json"]
}
```

Use distinct outcomes:

* `pass`
* `fail`
* `incomplete`
* `unsupported`
* `cancelled`

An aggregate succeeds only when every required applicable case passes. Unsupported or unfinished required work blocks release.

A retry creates another attempt. It never overwrites the original failure.

### C6. Time model

Use three separate notions of time.

#### Semantic time

This belongs to the behavior under test:

* A timer requested for a particular interval.
* A socket timeout.
* An audio sample duration.
* A lease expiry.
* A clock's resolution or monotonicity.

Keep such inputs. Judge them against the relevant clock or state, with a justified tolerance.

Use deterministic injected clocks for portable components. Use `icount` only in profiles that require it. Preserve accelerated and SMP profiles separately.

#### Work and progress

Prefer observable finite progress:

* A child reached a barrier.
* A socket became readable.
* A device command completed.
* A fixed operation sequence finished.
* The expected file bytes appeared.
* A process exited with the expected status.

Replace readiness sleeps with such events.

Progress records must identify completed work. Repeated output or heartbeats cannot indefinitely extend an execution.

Kernel work limits remain valid when they express a property. Ext2's command-count bounds for fixed writes are examples. They must not be multiplied by a host calibration factor. `S/ext2.sh:120`, `:141`.

#### Operational host time

Use one monotonic host-clock abstraction for leases, cancellation and cleanup.

Operational limits come from host/profile measurements and an execution policy, not per-test seconds embedded in the catalog. Record the calibration and applied limits in the result.

When a limit expires:

1. Capture process, guest and resource evidence.
2. Stop and clean up the attempt.
3. Report `incomplete`.
4. Retry under an appropriate unloaded or better-provisioned environment if policy permits.
5. Keep the required plan unsuccessful until it completes.

Calibration must not hide performance regressions. Compare work, CPU use, I/O and elapsed-time distributions separately.

#### Clock-test replacement

For host-reference clock tests, bracket guest samples:

* Record host request time A.
* Obtain a guest clock sample.
* Record host reply time B.

The sample occurred within `[A, B]`. Two exchanges give an interval for the possible elapsed host time. Add only justified clock resolution/error terms. If the interval is too wide to judge the property, report an inconclusive measurement.

Do not equate Python's receipt time with the guest sampling time.

Do not replace host `SIGSTOP` with QMP stop without changing the test's meaning. QMP stop freezes virtual execution/time differently. A profile requiring external process suspension must declare that capability.

### C7. Test kinds to retain

Keep five kinds.

* Host tests of real portable code: fast assertions, sanitizers, allocation failures and deterministic interleavings.
* Guest probes: ABI, privilege, address spaces, signals, scheduler and actual driver paths.
* End-to-end sessions: shipped applications, shells, agents, SSH, native builds and installed workflows.
* Fuzzing and replay: malformed inputs, stateful sequences, minimized regressions and preserved seeds.
* Hardware acceptance: physical devices, chipset behavior, installed boot and interactive use.

End-to-end testing is appropriate for the owner's application examples and for the agent milestone. It cannot efficiently force every allocation failure, damaged structure, late device completion or unusual interleaving.

The project's own history supports both levels. Host/fuzz tests found filesystem and checker failures; real applications found VM, proc-path and clock problems. `dev-log/53-tests/notes.md:21`, `:33`, `:45`; `dev-log/61-agents/plan.md:21`.

Start with fresh guests. Share a guest only after a checked reset contract exists. That contract includes processes, descriptors, credentials, mounts, routes, leases, sockets, terminal settings and counter baselines.

Durability tests reboot the same modified disk. Snapshot rollback must not conceal a missing flush.

### C8. Worker, CI and release policy

#### Before a push

A worker runs one resolved affected-case plan after rebasing:

* Compile the affected configurations.
* Run relevant style and document checks.
* Run host tests that include changed code.
* Run guest cases that exercise the changed behavior.
* Include both architectures where their code or ABI differs.
* Run runner tests when runner or catalog behavior changes.
* Run the deliberate-fault proof when introducing a new test.

The planner prints why each case was selected. Uncertain mappings expand conservatively.

#### On each push

CI creates a plan tied to the accepted commit:

* Build and validate the catalog.
* Run mandatory boot, ABI and representative driver cases on both architectures.
* Run affected cases from changes since the last fully checked head.
* Run applicable host tests and style checks concurrently with guest work, within the host budget.
* Preserve failures, cancellations and incomplete attempts.

Expensive superseded runs may be cancelled. Their commits must not be recorded as checked merely because a later run exists.

Architecture-independent host work can be shared only when its actual inputs, defines and host ABI are identical.

#### Release

A release runs:

* Every required case in the declared release configuration matrix.
* Both guest architectures.
* Relevant CPU, firmware, filesystem, controller and SMP variants.
* Fresh uncached executions.
* The production hardware bundle's VM acceptance.
* Lenovo trial installation and acceptance using the same verified artifact bytes.

This is a deliberate matrix, not every possible Cartesian product.

Manual diagnostics and open-ended fuzz campaigns are not compulsory release cases. Their minimized regressions are.

No required incomplete or unsupported result can produce a successful release.

### C9. Migration phases

Each line below is a task for one worker. Tasks marked parallel can proceed together after the phase's shared contracts are fixed. Every task includes its own check.

#### Phase 1: inventory and contracts

1. Define the JSON schema and validator; check duplicate IDs, unknown fields and broken references.
2. Map all 101 suite names to stable case families; check that no current suite or automated probe is omitted.
3. Define artifact bundles and exact digest verification; check missing and altered inputs.
4. Define result states and completeness rules; check missing, duplicate, forged and stale-boot records.
5. Define platform API contracts and execution contexts; check them against current transient-init and installed behavior.
6. Record representative baseline measurements by setup, boot, execution, verification and cleanup.

#### Phase 2: portable execution

7. Implement the shared catalog, planner and result library in C; check it with fixed parser and planning cases.
8. Implement Linux process execution and cleanup; check descendant escape, cancellation and restart recovery. Parallel.
9. Implement Darwin execution and PTYs; check identity, cleanup and unsupported isolation reporting. Parallel.
10. Implement native Windows execution with Job Objects; check quoting, handles, paths and large files. Parallel.
11. Build the portable core with TinyCC on Rach2; check native process execution and remote submission. Parallel.
12. Implement QMP and artifact transfer without a shell; check partial messages, disconnects and wrong digests.
13. Implement the host resource broker; check concurrent accounts, exhausted capacity and restart recovery.
14. Implement remote placement; check incompatible hosts and interrupted coordinator connections.

#### Phase 3: preserve existing execution

15. Implement the transient-init executor; check parent/child status and kernel cleanup.
16. Implement ordinary guest sessions; check credentials, terminal behavior and child cleanup.
17. Implement installed multi-boot sessions; check boot-generation identity and persistence.
18. Migrate host units to individual targets with incremental compilation; compare the 148-program inventory. Parallel.
19. Migrate default probes and expected faults; compare all 119 planned entries and architecture exclusions. Parallel.
20. Migrate checker and runner regressions; prove malformed output cannot produce a pass. Parallel.
21. Replace completion limits and readiness sleeps in migrated cases; check slow-host behavior as incomplete execution.
22. Replace the clock oracle; check known lost-tick faults and deliberately uncertain measurements.

#### Phase 4: migrate semantic families

23. Migrate storage/controller profiles; preserve command counts and host byte comparisons. Parallel.
24. Migrate filesystem persistence, corruption and repair; preserve same-disk reboot and external checking. Parallel.
25. Migrate network profiles and portable peers; preserve captured packets and driver-specific cases. Parallel.
26. Migrate USB, audio and console profiles; preserve physical-output and exact-input assertions. Parallel.
27. Migrate boot, firmware, install and recovery; verify executed marked kernels. Parallel.
28. Migrate native build and selfbuild; verify derived artifact extraction and boot. Parallel.
29. Migrate accounts, login and SSH; preserve privilege, persistence and terminal behavior. Parallel.
30. Migrate GNU and agent sessions; retain unmodified tools and the worker-session verifier. Parallel.
31. Replace guest workflow mutation sweeps with selected host proofs and a short ordinary guest workflow check.
32. Move manual-content scans and diagnostic transfers out of compulsory guest execution.
33. Migrate mutation proof jobs with explicit pass/fail/pass artifact records.
34. Add focused syscall gaps, starting with vector positional I/O and `pselect6`.
35. Add NVMe smoke, PS/2 input, clock fallback and ChaCha known-answer cases.
36. Extend fuzzing to ELF64 and writable filesystem operation sequences.

#### Phase 5: selection and cutover

37. Add component-to-case mappings; check representative changes and conservative fallback.
38. Switch worker checks to resolved affected-case plans; compare selected coverage with current policy.
39. Switch CI to the shared planner and broker; check cancellation, retries and missing-case reporting.
40. Switch release to build-once artifact bundles and the required configuration matrix.
41. Compare old and new plans for every registry entry; resolve every coverage difference.
42. Remove obsolete shell orchestration, metadata scraping, duplicate wrappers and inert options.
43. Publish the runner contract, case-writing guide and platform support matrix; check examples on all supported hosts.

### C10. Risks and measurements

The main risks are concrete:

* Changing probe execution context can remove coverage.
* Shared guests can leak state between cases.
* A structured protocol can still accept forged output if its authority is wrong.
* External verifiers can inspect the wrong image.
* Accelerator substitution can remove a required CPU or device configuration.
* A broker can leak allocations or kill a reused PID after restart.
* Windows ports can compile while retaining incorrect Unix assumptions.
* Calibration can conceal regressions if its provenance is not recorded.
* A test catalog can become a second scripting language.
* An immutable bundle can still contain an incorrect oracle.

Measure first:

* The same current artifacts under native KVM, WHPX and available x86 HVF, plus TCG.
* ATA polling and DMA cases separately.
* Guest compute, native build, agent workflow and storage cases separately.
* Setup, image preparation, boot, execution, verification, queue and cleanup time.
* CPU, peak memory, actual disk writes and concurrent interference.
* Failure detection preserved by each proposed merge or deletion.
* The frequency and cause of incomplete runs.
* The cost of a fresh boot versus a verified reset.

Do not promise a release duration before those measurements. The justified outcome is a portable, inspectable test system whose results state exactly what ran, where it ran, which bytes it tested and why it passed or failed.
