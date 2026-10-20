# Etude measurement records

This is the timing and comparison contract for T02 of [the plan](plan.md). Version 1 of the [history schema](../../tools/etude/schemas/history.schema.json) defines record shapes. The rules below define their meaning and the relationships a reader must validate. They describe the history to be implemented, not measurements already made. No environment has been timed by etude yet.

## Identities and storage

History is UTF-8 JSON Lines, one object followed by a newline. Writers use separate streams or serialize a shared stream. A reader accepts only complete newline-terminated objects, rejects duplicate JSON member names and unsupported versions, and reports a truncated last line as lost data. It never infers success from the records before that line. A damaged interior line makes the stream invalid; records in other streams remain usable if their dependencies can be established.

Every envelope has `schema_version`, `record_id`, `stream_id`, `sequence`, `run_id`, `recorded_utc`, `kind` and `data`. IDs are opaque, case-sensitive ASCII strings. Run, stream, record and attempt IDs are globally unique; a controller allocates them before execution. Stream sequences start at zero and increase by one. A replay with the same record ID and bytes is a duplicate, not another sample; conflicting bytes or a sequence gap are a history error. Records in separate streams have no order until their references are resolved.

| Identity | Meaning |
| --- | --- |
| `run_id` | One planned selection and its preparation, attempts and cleanup |
| Plan record ID | The immutable inputs fixed before any attempt starts |
| `item_id` | One selected case, architecture, configuration and repetition within the run |
| `case_id`, `family_id` | Stable identities of a behavior and its test family |
| Assertion ID | A stable assertion in the inventory referenced by the item |
| `attempt_id` | One execution, including a retry that has an ID of its own |
| `step_id` | A stable operation within a case or shared preparation recipe |
| Step record ID | One invocation of that step; loops and reboots produce separate records |
| `machine_id`, `boot_id`, `clock_id` | One machine, its current boot and the clock used for an interval |
| `baseline_id` | An accepted, immutable choice of sample record IDs and comparison policy |

A step and an attempt reference their plan and item. A step of shared preparation has null `item_id` and `attempt_id`; both must be null together. The run lists these steps in `preparation_steps`. A case's repeated step has a different record ID while keeping its `step_id`. A replacement case or assertion gets a new stable ID when its meaning changes. A rename alone keeps its ID and records the old name in the inventory.

`blob` references give the SHA-256 and byte length of exact immutable file bytes, with one or more locations from which to obtain them. A location is a locator, not an identity; a reader checks its bytes before use. A blob may contain a manifest of more blobs. Retained records retain their referenced content. Hashing does not depend on JSON member order because the producer publishes and hashes the exact bytes once. Hashes and integer counts have the representations in the schema; counters and times fit in 53 unsigned value bits. Overflow is an unavailable measurement with a reason, never a wrapped value.

The commit is the full Git object name. The source blob identifies the exact source bundle, the runner blob its executable, and `artifacts` the inputs actually used, including kernels, roots, probes, services and fixtures. Their manifests associate each hash with its role and architecture. Measurements of a modified checkout cannot be used as a baseline. A comparison may cross kernel commits; that is how it finds a slowdown.

## Planned inputs

A `plan` record precedes execution. Its selection manifest lists every item and the full assertion inventory, including both architectures and required repetitions or configurations. Each item has a workload, assertion inventory and execution profile by content hash. The plan's inline items must equal the manifest's items exactly; they cannot omit an unavailable case. A case that is not applicable to an architecture has its reason in the inventory before selection. An applicable case that cannot run stays incomplete.

The workload defines inputs, sizes, seeds, repetition counts, expected values and faults, process role, required features, and the operations under test. The profile defines devices, processor features, guest processors and memory, firmware, native fixtures, privileges and isolation. The assertion inventory lists the expected assertion IDs and outcomes. Merely counting received assertions does not prove that the expected ones ran.

The environment records the machine's stable identity, boot, OS and version, accelerator, and content-addressed snapshots of the following inputs. The accelerator is explicit; `native` is used for a run without QEMU. A mixed run's host cases still use their `host` execution domain.

| Snapshot | Required contents |
| --- | --- |
| Site profile | Machine model, processor model and topology, physical memory, storage, OS configuration, accelerator settings and tooling VM configuration |
| Tools | Versions, executable hashes, build recipes and options for etude, compiler, QEMU and every supporting tool used |
| Capacity | Available processors, memory and disk; admission limits, overhead and reservations; guest configured memory, allowed resident memory and disk backing as separate values |
| Cache state | Cold, warm or unknown for each artifact, build, tool and OS cache; preparation applied to establish that state |
| Load | Observation method and bounds, other jobs, memory pressure, paging and disk traffic before admission |
| Concurrency | Worker and reservation limits, planned ordering, shuffle seed, load fixtures and resource allocation policy |
| Policies | Versioned measurement methods, watchdog floors, ceilings, margins and initial bounds; resource headroom; comparison and budget policies |
| Reproduce | Source and artifact locators, working directory, environment variables, argument arrays, preparation commands and the exact case and profiler commands |

Snapshots report unavailable observations as null with a reason. An absent observation is never evidence of an idle machine. `load` in the run result covers the whole interval, including outside activity and disturbances; the plan's snapshot alone cannot establish repeatable conditions. Reservations, claim IDs and changes are kept in the resource samples. The history records actual admission and useful concurrency as well as the planned worker limit.

## Time and resources

Times ending in `_us` are integer microseconds, sizes ending in `_bytes` are bytes, and processor time is summed user plus system microseconds. UTC timestamps use `YYYY-MM-DDTHH:MM:SS[.ffffff]Z` and locate events for people. Monotonic readings measure intervals. `end_us - start_us` equals a measured `elapsed_us.value`; end must not precede start. Clock resolution is rounded upward to at least one microsecond.

Readings may be subtracted only within the same machine, boot and clock. A controller measures the end-to-end interval, including remote execution, on its own clock. A remote step uses its executor's clock. A reboot starts a new clock domain. UTC adjustment does not change a duration. An end not observed has null `end_utc`, null `end_us`, and an unavailable elapsed time; it cannot establish a completed measurement. A terminal record is still written after failure or lost contact, with the information that is known.

A measurement is either a nonnegative value with its method, quality, sampling interval and error bound, or null with a reason. Zero is a measured zero. `exact` means complete accounting at the recorded resolution, not infinite precision. `sampled` reports observations at the stated interval. `lower_bound` states that some cost is unobserved. A null error bound means the error is not bounded; it does not mean zero. A null sampling interval means the method did not sample. Method definitions in the policy state the platform API, scope, resolution, rounding and known omissions.

For example, these are different facts:

```json
{"value":0,"method":"child-accounting-v1","quality":"exact","interval_us":null,"error_bound":1000}
{"value":null,"reason":"the platform cannot account for ended descendants"}
```

A step records its queue, build, prepare, transfer, boot, execute, capture, cleanup, tooling start or one-time setup interval. The case time starts at its first reservation and ends after its cleanup. Admission wait before that point is `queue_us`; admission waits after it remain in elapsed time. Step durations may overlap. Stage and family totals are sums of work, not another measure of elapsed time. Their blobs give the member record IDs, available sums, missing member IDs and reasons. They never add missing values as zero or charge shared preparation to every case.

The run's elapsed interval starts at its first capacity grant and ends only after cleanup is confirmed for all owned work. It includes ordinary builds and preparation, transfers, local tooling VMs, boot, execution, evidence capture, subsequent admission waits and cleanup. Useful concurrency records the integral of actively executing cases over that interval, the peak active count, the elapsed denominator and the sampling method or exact transition records. Waiting for capacity is not useful execution. The report can therefore distinguish a larger worker limit from work actually done in parallel.

`resources.scope` identifies the owned process trees and tooling machines, by machine, boot and process creation identities, and defines which samples include which children. Processor time includes descendants that have already exited. A parent's inclusive accounting must not be added to the same children's accounting again. QEMU accounts for guest execution; guest processor time is diagnostic and is not added to QEMU's. The same rule applies to a tooling VM and the processes inside it. A platform that cannot provide full descendant accounting reports that metric unavailable or as a lower bound with its omissions.

`combined_peak_bytes` is the maximum concurrent resident memory of the whole owned scope. It is never the sum of each process's separate peak. If only samples can establish it, the quality and interval say so. The run peak is measured over the run's whole scope, not the sum of case peaks. Resource samples also record reservations, external load, disk use and backing, pressure and paging, with units and unavailable reasons. Combined memory and processor measurements are kept apart from configured guest memory and declared reservation ceilings.

Each step keeps its declared progress witnesses, their sequence, offset from the step's start and cumulative completed units. Units and limits are defined by the workload. Only a new completed step or an advancing bounded-work counter counts as progress. Heartbeats, repeated records and processor use do not count. `max_progress_gap_us` includes the initial and final gaps. Evidence retains the complete progress trace when a summary cannot show it. It also records watchdog bounds fixed at planning, the last progress and capture errors after a timeout.

## Completion and unavailable data

A `step` records an operation's terminal state. An `attempt` lists all its step record IDs, assertions, elapsed and queue time, resources and result. `previous_attempt_id` links a retry to the earlier attempt; it never replaces it. A `run` lists every selected item and every attempt record, shared preparation, totals, resources, observed load, budget and result. No summary is authoritative until the records and hashed evidence it depends on have been checked.

The following rules are semantic validation, in addition to JSON Schema:

* A reference resolves to exactly one record of the expected kind, run, plan, item and attempt. All IDs are unique in their stated scope. Every started attempt appears in the summary. All required steps and assertion IDs appear once per invocation, with their expected results. A skip is absent coverage, never a pass.
* A selected item with no attempt is incomplete, or cancelled when cancellation was requested before it started. An item with unfinished required work is incomplete. Missing terminal records cannot be filled in from an exit code or a success token.
* An observed assertion failure is kept as a failure even if a later retry passes. A run result is `failure` if any attempt has an observed failure, otherwise `incomplete` if any required work or cleanup is unconfirmed, otherwise `cancelled` if any selection was cancelled, otherwise `pass`. The summary retains missing and cancelled work even when an earlier failure determines the overall result.
* `pass` requires the entire selected inventory, required preparation and cleanup to be confirmed. `cleanup: unconfirmed` always prevents a pass and a completed budget result. It does not release any uncertain resource claim.
* A correctness pass can have missing processor or peak-memory data; those metrics remain unavailable. Missing elapsed time prevents a performance or budget conclusion. A disrupted attempt never enters an adaptive bound or performance baseline, even if all its assertions passed.
* Capture errors state what was lost. Missing assertion or cleanup evidence makes the result incomplete. A lost optional resource sample need not change correctness, but limits the claims the report can make.

Successful non-disrupted samples with matching keys may inform processor reservations and watchdogs. The plan fixes the resulting bounds between declared floors and ceilings, using completion time and maximum progress gaps plus variation and margin. With no eligible history it uses the initial ceiling. Failed, incomplete, cancelled and disrupted attempts do not teach a larger bound. Resource estimates, watchdog bounds and budgets have distinct policy versions; none rewrites another automatically.

## Comparisons and noise

A `comparison` identifies its purpose, scope, subject, policy, baseline, candidate samples, statistics, equivalence review and report. Sample IDs reference terminal step, attempt or run records according to the scope. `baseline_id` resolves in the retained report to the immutable sample list and policy used, not to a moving name such as "latest". A new baseline gets a new ID. The report records eligible and excluded samples with reasons, exact commits and artifacts, the commits in between, changed steps, measurement uncertainty and commands to reproduce and profile the result.

For a same-site case comparison, match stable case and assertion identities, architecture, domain, workload, profile, machine, OS, accelerator, tools, cache state, declared load and concurrency policy. Step comparisons also match `step_id`. Commit and tested executable hashes are recorded but are not part of the match key. A change to a tool being measured is stated in the report; other tool changes prevent a match. A changed workload or assertion contract starts a new baseline unless a retained review establishes equivalence for that comparison. The review identifies both hashes, the unchanged obligations and its evidence. A familiar case name alone does not establish equivalence.

The initial policy `median-mad-v1` requires at least five independent, successful, non-disrupted baseline samples. Retransmitted results and retries of a failed run are not independent samples. Failed attempts stay visible and cannot be removed to make a run eligible. Samples with unknown cache or load conditions, incomplete elapsed time or an unbounded timing error do not establish a performance baseline. Too few eligible samples yields `insufficient_history`, with null statistics that cannot yet be estimated. A mismatched key yields `not_comparable`, with the differing fields, regardless of the number of samples.

For eligible durations, take the median `m` and median absolute deviation `d = median(abs(x - m))`. An even-sized median is the midpoint of the two central values. The noise half-width in microseconds is `b = ceil(max(3 * 1.4826 * d, 0.01 * m, e))`, where `e` is the largest clock resolution or duration error bound of the samples. Store fractional medians and deviations rounded upward to microseconds; calculate the band from the unrounded values. The one-percent floor prevents a zero MAD from making every small change a regression. The policy, rounding, sample IDs and count accompany the numbers.

A same-site candidate may be one fresh eligible sample. Its median is its elapsed time, its MAD is zero and its band uses the same floor and error bound. Several independent candidate samples use the same formula. If candidate and baseline medians differ by no more than the sum of their bands, report `within_noise`. Otherwise report `regression` for a slower candidate or `improved` for a faster one. These are performance reports; they do not change correctness results. Repeated slow samples do not silently move an accepted baseline.

For example, baseline seconds `[99,100,100,100,101]` give median 100, MAD 0 and band 1 when measurement error is smaller. Candidate 101 has band 1.01; its difference of 1 is within the combined 2.01. Candidate 110 has band 1.1; its difference of 10 exceeds the combined 2.1 and is a regression. Three baseline samples give insufficient history, even when all three have the same time.

Site selection compares complete whole-collection runs on one fixed candidate source and assertion inventory, both architectures together. Profiles keep the same testing obligations and workload. Machine, OS, accelerator, native host compiler and concurrency limits are allowed to differ because they are what is being compared; all are recorded. Tested artifacts must match, or a retained equivalence review must establish that platform-specific products test the same candidate and obligations. Each configuration uses at least five independent complete passing runs under repeatable conditions. Both samples' bands use their measured run times. A difference within their combined bands is a tie, with more measurement needed before claiming one faster.

Cases and family totals explain differences; the whole charged interval decides the ranking. TCG on the testing PC can win only with complete whole-collection measurements. An isolated fast case, a single architecture or a run with unknown preparation cost cannot establish the winner. Windows and WSL measurements on the same PC use separate windows after other jobs finish; the run load evidence must confirm the conditions. The Lenovo's smaller native selection has a different inventory and cannot compete with these full runs. A case failure under an accelerator cannot justify leaving it out.

## Budgets and the first runs

The budget policy fixes the selected site's reference capacity and cost declarations. A routine full run has a 3,600-second goal, a further 3,600 seconds reserved for growth and a 14,400-second ceiling. ci has a 600-second ceiling and the push check has 300 seconds. The schema stores these in microseconds; the policy states the run class and how growth beyond the goal is reported. The measured time is the charged run interval defined above. Outside queue wait and one-time prerequisite installation or first vendored-toolchain construction are reported separately. Ordinary preparation and incremental builds are charged. Lenovo installation is a separate release step.

The budget blob records the charged time, excluded time, limits, cleanup reserve and a verdict apart from correctness. It must agree with the plan's policy. A forecast includes every selected item and preparation node; an unknown cost is unknown, not zero. Measured overruns fail the budget check even if every assertion passed. The hard execution deadline precedes the ceiling by the declared cleanup reserve. Cancellation at that point leaves unexecuted work incomplete. Unobserved duration or unconfirmed cleanup yields `unknown`, never a pass within budget.

Only `purpose: phase_first_run` can have a non-null exemption. It identifies phase #589, the exact full-conversion selection manifest, and an explicit set of run and attempt IDs allocated before work starts. The current run and every attempt must be in that set, and the manifest must equal the plan's selection. It covers the first environment measurements required by the plan, including the named native selection; an unrelated focused run cannot claim it. A further measurement needing this exemption requires its own recorded plan before execution. The exemption removes the budget verdict only: cost collection, correctness, completeness, watchdogs and bounded cleanup still apply.

Push checks, ci and releases require a null exemption and can never reuse exempt attempts as their own budget result. Their cost forecasts may use eligible first-run samples. A run with a valid exemption reports `exempt`; an invalid exemption is a record error, not permission to exceed a limit. Lost contact or unconfirmed cleanup still leaves that run incomplete and its charged interval unavailable.

## Examples and checks

[`history.example.jsonl`](../../tools/etude/schemas/history.example.jsonl) contains a plan, execution and cleanup steps, their attempt, the run result and two comparison records. It is a synthetic one-case focused run, not the milestone's full collection. IDs, hashes and locations illustrate the format; referenced blobs are not supplied and the file is not evidence of an actual pass. All clocks are on one invented boot, with microsecond readings and UTC labels. The case and run have complete elapsed time and confirmed cleanup; descendant processor accounting is unavailable, which does not turn the missing measurement into zero. The single sample is insufficient for a baseline. The other comparison cannot rank that focused selection against a whole collection.

[`history.incomplete.example.jsonl`](../../tools/etude/schemas/history.incomplete.example.jsonl) shows the same workload losing its executor connection. The step and attempt remain incomplete, cleanup is unconfirmed, and the run has neither an end reading nor a passing budget verdict. Its separate run and attempt IDs prevent these records from completing the earlier run. The missing cleanup step and partial assertion evidence cannot be inferred from what was received before the loss.

The following examples require semantic checks after schema validation:

| Records or conditions | Required interpretation |
| --- | --- |
| Complete inventory, matching assertion results, confirmed cleanup, unavailable processor time | Correctness may pass; processor cost remains unknown |
| Plan lists two items but summary lists one passing item | Invalid summary; the other item is incomplete |
| Last line is a truncated attempt result | Ignore that line and report the attempt incomplete |
| Five expected assertions but a duplicate replaces one missing ID | Incomplete coverage, even if the count is five |
| Passing retry after an assertion failure | Keep both attempts and report the observed failure |
| Confirmed assertions but lost contact before cleanup | Incomplete; retain claims and no completed-budget result |
| Same workload and profile, different kernel commits | Eligible for same-site slowdown analysis |
| Same name, changed input size or assertion inventory without equivalence review | Not comparable; start a new baseline |
| Same case, i686 versus x86-64 or KVM versus TCG | Not a same-site regression comparison |
| Three otherwise eligible baseline samples | Insufficient history |
| Equal medians with overlapping noise bands | Within measured noise; no winner |
| Fast case timings but no complete collection run | Cannot select a test machine |
| Native Lenovo selection versus a complete VM collection | Not comparable for site selection |
| ci plan with a first-run exemption | Schema rejects the plan |
| `pass` with unconfirmed cleanup, or a null metric without a reason | Schema rejects the record |

Schema validation must enable date-time format checking. It cannot prove hash contents, references, unique assertion coverage, arithmetic, statistical eligibility or exemption membership. T05 defines execution records and manifest details against this contract; [The history store](../../tools/etude/Readme.md#the-history-store) implements writing and collection, including schema and local interval checks. Consumers validate references, inventories and aggregate outcomes after reading all streams. [The comparison](../../tools/etude/Readme.md#the-comparison) implements the comparisons of the same site, and T58 implements budget checking. Their tests must exercise both the shape errors and these semantic refusals.

## Retention

Each site's policy sets explicit byte and age limits for ordinary history and its referenced content. Pruning removes unreferenced ordinary records and blobs together; it never breaks retained plans, summaries or comparisons. Accepted baselines, their samples and policies, and evidence of open issues remain retained under AGENTS.md. When retained evidence alone exceeds the limit, report the excess and stop ordinary retention growth according to policy; do not delete that evidence. An incomplete stream and its recovery diagnostics remain evidence until the fault is recorded, with the excerpt or attachment stored in the issue before ordinary scratch is removed.
