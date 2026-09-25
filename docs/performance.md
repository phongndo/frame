# Performance and resources

Use this document when changing hot paths, running comparisons, or reviewing performance evidence.
Product/design priorities live in [AGENTS.md](../AGENTS.md#design); ordinary build and test workflow
is in [Development](development.md).

## Performance review requirement

Changes to input routing, PTY parsing/writes, rendering/composition, layout projection, resize,
scheduling, or client output require dedicated-host paired-gate evidence before approval. Identify
the affected multiplier (bytes, events, panes, frames, or clients) and provide:

- the reviewed baseline revision, candidate revision/working-diff identity, and Release profile;
- the approved host and retained artifact location, including `paired-regression.json`, raw
  distributions, and before/after host checks;
- the paired result and any separate absolute-target misses; and
- relevant correctness tests and deterministic allocation/work-budget results.

Reviewers must verify that evidence covers the actual candidate being approved. Shared-runner smoke
timings and a previously passing revision are not substitutes. Documentation-only and test-only edits
do not require a timing capture. This is a review requirement, not an automated merge-blocking
candidate-evidence check.

Host-dependent gates run manually through [scripts/performance](../scripts/performance). No GitHub
workflow executes candidate code on a persistent self-hosted runner. Automation would require a
base-controlled review boundary or disposable runner, while preserving the host lock, policy checks,
CPU affinity, candidate-owned fixtures, and before/after host validation.

## Run measurements

Short native benchmarks always use Release:

```sh
./bench
./bench terminal
./bench layout
./bench protocol
./bench mux
```

Use the benchmark shell for cross-subject comparisons, dedicated-host budgets, and memory census:

```sh
nix develop .#benchmarks --command ./bench extended
nix develop .#benchmarks --command scripts/ci/regression-budgets
nix develop .#benchmarks --command scripts/ci/memory smoke
```

The manual paired workflow is:

```sh
just performance-calibrate 3
just performance-gate main
just performance-extension
```

[workloads.json](../benchmarks/workloads.json) owns scenarios, suites, numeric targets, sample
policies, and terminal-lab selection. [performance_hosts.json](../benchmarks/performance_hosts.json)
owns approved hardware and CPU policy for both preflight and report validation. Consult those
manifests rather than copying thresholds or host settings into another document.

## Measurement semantics

Native C++ owns timing loops. Python may select adapters, launch isolated subjects, verify
completion, retain raw reports, and analyze results; it must not timestamp a measured interaction.
Repeated warm-scroll commands use sequence-numbered, delimited completion markers, so a redraw of
an earlier completion cannot finish a later sample. Execution randomizes workload blocks and
subjects, with direct controls bracketing each supported block.

A workload's `comparison_sampling` makes the comparison run several independently launched blocks,
each with scaled repetitions, shuffled among the other workload blocks. Reports pool the raw
samples, add p90 and per-block medians, and keep block-varying CPU and resource snapshots per block.
Samples within a block are correlated, so the direct-baseline confidence interval resamples whole
blocks, and direct-control drift pairs each block's own before and after controls. A block that
crashes or fails is recorded with its error, fragment, and stderr tail; its subject's pooled result
fails, and the remaining blocks and workloads still run. The comparison then writes the report and
validates it, so an unreviewed failure exits nonzero after the evidence is saved. Other workloads
still stop at their first unreviewed failure. Validation requires every declared block in pooled
results and direct after-controls. Gate and extension captures ignore `comparison_sampling` and
keep one repetition count per report.

`interactive_open_loop` uses this because each 120 Hz key arrives after the probe, client, daemon,
and peer have idled for about 8 ms, so every sample includes wake-from-idle costs. They grow with the
inter-key interval and as the rest of the host goes quiet, for the direct control too. On the
powersave host, consecutive keys settle on latency plateaus that switch every few tens of keys and
can hold a whole block, so one 20-key block (about 0.2 s) mostly samples host state. Read the pooled
p50/p90 with block medians and direct controls; p95 and above also absorb transient host load. The
workload never keeps CPUs awake: absolute values include the wake-up cost a typist pays on that host.

Latency endpoints are deliberately distinct:

```text
key_to_pty          injected outer-PTY input to fixture receipt
key_to_outer_bytes  injected outer-PTY input to matching bytes emitted toward the host terminal
input_to_photon     external terminal-lab HID event to measured display change
```

Only the last is user-visible latency. Compare the same build profile, fixture, work, completion
condition, and host. Distinguish CPU time, elapsed time, output bytes, physical footprint, RSS,
descriptors, and wakeups. Darwin wakeups are `proc_pid_rusage` package-idle and interrupt counts.
Linux instead reports `context_switches`: voluntary plus involuntary switches summed over live
threads from `/proc/PID/task/TID/status`. It is a wakeup proxy, not comparable with Darwin wakeups,
and loses counts from threads that exit within an interval. Shared-runner timing is diagnostic, not
a stable regression gate.

Navigation workloads time one keyboard trigger written to the attached client until a unique token
is visible in its output. Session and tab switches alternate between two full-screen painters. Before
each sample, the hidden target paints its token and a fill that differs from the visible screen, then
settles so pane ingestion is outside the sample. New-pane and new-tab samples include shell startup.
Every subject and the direct control start the account shell as a login shell, and only login
startup prints the marker, so a non-login shell cannot complete a sample; reports record each
subject's startup mode. Zellij reaches login startup through a one-exec wrapper. Each resize sample
follows a quiet interval, so it measures an isolated resize rather than a drag burst and includes
any subject-side resize coalescing delay. Per-subject trigger bindings and intervals live in the
[harness](../benchmarks/mux_benchmark.py); reviewed unsupported subjects and reasons live in the
manifest.

Reports retain raw distributions, source/manifest identity, executable SHA-256 values, and failures
or unsupported capabilities as outcomes rather than samples. Cross-subject validation admits only
failure signatures reviewed in the manifest; new adapter/competitor failures fail validation.
Each comparison attempt retains its execution order, raw per-workload reports, and subprocess
stderr in a unique run directory beside the requested output (`NAME.fragments/run-*`), including
when capture or validation fails. The scheduled benchmark artifact includes these fragments.
Sparse smoke p95/p99 statistics are marked invalid. Scaling sweeps expose shape and contention
knees, not reliable tail latency. Generated evidence stays under `build/`.

The blocked-PTY workload withholds pane reads, not host-terminal reads. A scoped reader consumes
that client's output during input saturation, the other session's native latency probe, and
payload recovery; completion still requires the full byte count and digest. The fixture's receive
guard measures time without progress, not total transfer time; the harness still imposes hard
bounds on sending and completion. Blocking terminal output is a separate workload. Results from
captures that did not drain output are not comparable to this isolated blocked-input scenario.

The blocked-client fixture continuously fills its 500×200 screen with changing printable rows.
Repeated short text can settle into an unchanged screen and delay socket saturation, especially
with blank-tail compression. The native probe still measures from flood launch, allowing 0.5 seconds
for saturation before the five-second no-progress deadline. The producer has a ten-second safety
guard; this does not extend the probe deadline. Fixture changes require fresh baseline and candidate
captures with the same producer.

The merge-blocking [deterministic budgets](../scripts/ci/deterministic-budgets) enforce zero
steady-state allocations and reviewed work/queue bounds independently of timing: routed bytes,
frames, wire amplification, flushes, writes, polls, readiness, sends, recovery, and child wakeups.

## Paired gate and calibration

The gate holds a host-wide lock and validates host state before and after capture. It builds the
baseline and candidate with the candidate's manifest, harness, and Nix toolchain. Each subject uses
its own pinned Ghostty source and offline Zig dependencies; the candidate-owned PTY fixture and
native probe are built once and shared. This compares actual dependency upgrades without requiring
an old revision to contain a newer harness. Evidence stays under `build/performance/`. Gate process
captures run only the manifest's `regression` suite, which must contain every budgeted workload;
diagnostic comparison workloads join it only with reviewed budgets.

Host fingerprints must agree across report types, paired captures, and before/after checks.
Linux `MemTotal` is usable memory, not exact installed RAM, and can vary across boots. Host policy
admits memory at or above the approved minimum, but exact reported memory still participates in
within-run fingerprint agreement.

Calibration repeats the unchanged checkout and rejects noise outside the reviewed ratio and
absolute floors; it never relaxes policy automatically. Paired regressions block independently of
stricter absolute product targets. An existing target miss cannot authorize further degradation.

Linux process CPU evidence sums nanosecond runtime across `/proc/PID/task/TID/schedstat`, not just
the main thread or scheduler-tick-rounded `/proc/PID/stat` values. Live-thread snapshots can lose
accounting for threads that exit between endpoints. Workload CPU is a batch average, not a latency
percentile or event-exact measurement. Its interval excludes fixture setup but includes probe
launch, settling, and resource census. Keep raw endpoints and CPU sources, and do not compare
unavailable or changing process populations as stable per-operation CPU.

## Extension isolation

`just performance-extension` pairs the same candidate with extensions off and with external
workloads: idle peers, mostly hidden retained Surfaces, bounded row changes, update storms,
incomplete near-limit producers, a non-reading maximum-paste owner, and focused/docked crashes.
Daemon and extension-process resources are reported separately; pane output must continue after
cleanup. Idle helper microbenchmarks do not establish whole-reactor or endpoint-specific isolation.

At the gate's 100 process samples, nearest-rank p99 endpoints are diagnostics and absolute-target
evidence, not paired blockers: frame-cadence outliers make their rank unstable.

## Interpret targets

| Question | Evidence |
| --- | --- |
| Is interaction responsive and isolated from blocked peers? | Input/echo, attach, navigation, resize, blocked-PTY/client latency at the named headless endpoint |
| Is resource use bounded and efficient? | Idle CPU, wakeups or context switches, memory, native CPU, deterministic work/queue bounds |
| Is output efficient? | Warm-scroll completion and byte limits, not interactive frame deadlines |
| Did this change regress? | Paired baseline/candidate checks independent of absolute-target misses |
| Does Lemma match competitors? | Same-host, same-fixture comparison with the best supported subject for each workload and metric |

Absolute targets in the workload manifest are not automatically competitor-parity thresholds.
Warm-scroll limits must not be interpreted as feasible end-to-end deadlines without measuring the
direct-PTY control. The fixture makes many separate row writes through a PTY, including line-discipline
work. A control already over target invalidates that interpretation; it does not establish that
parsing consumes all measured elapsed time. Profile daemon and child CPU separately. Changing write
batching changes the fixture and requires fresh controls, not comparison with old results.

Select the lowest valid latency, CPU, memory, or byte statistic per workload rather than naming one
universally fastest mux. Lower bytes need not mean lower CPU or latency. Use process-tree PSS/private
memory alongside RSS and separate daemon/client roles from descendants. Lemma's `daemon_helpers`
census occurs before panes exist; `attached_client` includes descendant terminal-restoration
guardians. `pane_or_mux_children` can contain unclassified helpers and is not equivalent to pane
memory. Active pane profiles drive the focused pane, not every pane simultaneously.

Retain raw evidence and review target changes independently. Do not relax absolute targets or paired
blockers merely to turn an observed miss green.

## Validate the performance detector

On a clean tracked checkout on the approved host:

```sh
nix develop .#benchmarks --command just detection-check --performance
```

The detector first requires an unchanged A/A paired gate to pass, then adds bounded non-elidable CPU
work to production command dispatch in an isolated candidate. It runs the complete paired gate again
and requires `command_dispatch_cpu_p95` to fail its paired threshold. An absolute-target miss,
host-policy rejection, or capture failure is not successful detection; raw samples are never edited.
Normal host locking, policy, affinity, provenance, and before/after checks remain in force. Each gate
permits at least four hours. This expensive manual check is separate from
[native source-fault checks](development.md#detector-validation); passing validates this slowdown,
not every workload or threshold.

## Display measurements

The GUI-ready contract is [terminal_lab.schema.json](../benchmarks/terminal_lab.schema.json).
Hardware-photodiode and software-pixel methods stay separate and are ingested with
[terminal_lab.py](../benchmarks/terminal_lab.py). Each run identifies the Ghostty, Kitty, or WezTerm
executable/configuration, display refresh profile, sensor position, randomized input jitter, and
direct or mux subject.
