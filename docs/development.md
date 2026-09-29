# Development

Use this document for contributor workflow, testing, dependency upgrades, and documentation changes.
[AGENTS.md](../AGENTS.md) carries product/design priorities; [Architecture](architecture.md) explains
ownership and boundaries. Hot-path changes also require the
[performance review workflow](performance.md#performance-review-requirement).

## Workflow

Follow [Build and run](usage.md#build-and-run) to enter the Nix environment. `just run [args...]`
and the shell's `frame` alias share the isolated development runner. It configures `build/dev` only
when toolchain/configuration inputs change and incrementally builds the `frame` target. The `dev`
profile uses `-O1`, debug symbols, invariants, and frame pointers. Release is explicit for packaging,
production validation, and performance measurement.

Linux Release executables statically link the C++ and compiler runtimes and discard unused function
and data sections. This bounds the runtime mappings in the daemon, client, and independent UI helper;
libc remains dynamic. Development, Debug, sanitizer, and Darwin builds retain their default linkage.

C/C++ outputs are path-normalized into the user's shared ccache, so matching compilations can be
reused across worktrees and after removing `build/`. `just clean` preserves the cache;
`just clean-cache` explicitly clears it.

Use focused checks while working and `just check` before completion:

```sh
./test unit
./test mux resize
just test              # fast native and real-mux tests
just fmt               # format C++, Nix, and Python
just docs-check        # local links, catalogs, schemas, and example synchronization
just check             # build, formatting, analysis, tests, Python, and documentation checks
just ci-check          # all merge-blocking CI lanes, including sanitizers
```

The verification profile defaults to `debug`. `just --list` describes individual checks and
`./test --help` lists current test selectors. Report commands actually run and any failing or blocked
checks; a previously passing revision is not evidence for the current change.

## Tests

| Tier | Responsibility |
| --- | --- |
| Native unit | Core values, commands, layout, protocol, queues, configuration, input policy |
| Terminal boundary | Ghostty adapter, rendering, encoding, effects, resize, selection |
| Component integration | Process-opening platform boundaries |
| Python mux | Real daemon, client, PTY, child process, lifecycle, API, terminal consequences |
| Simulation/stress | Deterministic Core/Ghostty worlds, real-mux state machines, history, allocations |

Use native tests for pure invariants and Python when a contract requires real descriptors, PTYs,
processes, or the daemon. Tests should name one failure domain, synchronize on observable state with
bounded deadlines, and report enough state to diagnose timeouts. The
[mux harness](../tests/support/mux_harness.py) uses stable Pane/Tab IDs for semantic identity and
observes PIDs only for real process lifetime assertions.

CTest is the CI integration surface. Cheap tests run in parallel; process tests are serialized where
contention changes behavior. Stress, resource, allocation, and paid model work remain outside the
common edit loop.

### Simulation and replay

```sh
./test sim
FRAME_SIM_SEED=0x1234 FRAME_SIM_OPERATIONS=4096 ./test sim
FRAME_MUX_SIM_SEED=0x1234 FRAME_MUX_SIM_OPERATIONS=4096 ./test sim
FRAME_MUX_SIM_TRACE=path/to/failure.min.trace ./test sim
```

Core, protocol, presentation, composition, and Ghostty worlds print exact replay commands on
failure. The mux world executes the production SessionMachine against simulated Runtime effects;
its concrete, versioned operation/effect histories replay without invoking the generator. Curated
coverage and independent semantic invariants live with [simulation tests](../tests/sim/).

Set `FRAME_MUX_SIM_TRACE_OUT=path/to/trace` with a configured mux seed to retain the in-progress trace,
including the operation active at a sanitizer abort. Ordinary failures write the complete trace and
a bounded deterministic reduction under `build/mux-sim-failures/`. Use `FRAME_SIM_TRACE=1` for non-mux
replays to stream completed operations before a dependency abort.

After reproducing and fixing a genuine finding:

```sh
scripts/promote-mux-trace path/to/failure.min.trace regression-name 'one-line bug description'
```

Promotion replays without stale checkpoints, records fixed outcomes and discovery metadata atomically,
and publishes into [the permanent corpus](../tests/sim/corpus/mux/). Every simulation run replays it
and checks recorded command outcomes/state. `characterization` entries record `introduced-at`;
`regression` entries record `fixed-at`. Both require a description and source trace. Characterization
coverage is not bug-discovery evidence, and checkpoints supplement rather than replace independent
invariants. Promote a regression only after confirming both the broken behavior and the fix.

Scheduled [mux-sim-campaign](../scripts/ci/mux-sim-campaign) runs longer campaigns; use
`FRAME_MUX_CAMPAIGN_SEEDS` and `FRAME_MUX_CAMPAIGN_OPERATIONS` to bound a local equivalent.

### Fuzzing

Parser fuzz targets cover Frame-owned attachment, host-input, extension-framing, and public JSON
boundaries. To replay checked-in corpora:

```sh
scripts/ci/configure sanitizers -DFRAME_BUILD_TESTS=OFF -DFRAME_BUILD_BENCHMARKS=OFF -DFRAME_BUILD_FUZZERS=ON
cmake --build build/sanitizers --target frame_attachment_decoder_fuzz frame_host_input_parser_fuzz frame_api_json_fuzz frame_extension_framing_fuzz
./build/sanitizers/frame_attachment_decoder_fuzz -runs=0 fuzz/corpus/attachment
./build/sanitizers/frame_host_input_parser_fuzz -runs=0 fuzz/corpus/host-input
./build/sanitizers/frame_api_json_fuzz -runs=0 fuzz/corpus/api
./build/sanitizers/frame_extension_framing_fuzz -runs=0 fuzz/corpus/extension
```

Linux links libFuzzer for mutation runs. The [sanitizer lane](../scripts/ci/sanitizers) replays seeds,
runs bounded mutation with protocol dictionaries, then a longer mux simulation with an externally
generated seed. Darwin replays the same seeds under ASan/UBSan because Xcode Clang lacks libFuzzer.
Scheduled [fuzz-campaign](../scripts/ci/fuzz-campaign) retains evolved corpora and minimized failures.

After confirming a fix, run
`scripts/promote-fuzz-input TARGET INPUT CORPUS_NAME 'one-line bug description'`. Promotion replays
the input and records its SHA-256, source, fixing revision, and regression provenance in
[regressions.json](../fuzz/corpus/regressions.json).

### Detector validation

Use bounded source-fault injection to test whether checks reject plausible implementation mistakes:

```sh
just detection-check
just detection-check --case partial-write --case stale-id
```

The opt-in runner copies the current diff and nonignored new files into a temporary detached
worktree; it never mutates the source checkout and removes the worktree on exit. Each case requires
a passing control, then changes exactly one production source anchor and requires an assertion or
observed allocation-budget failure. Build errors, missing/skipped tests, crashes, timeouts, and
unrelated failures do not count as detection. Changed or ambiguous anchors fail closed for review.

Use `just detection-check --help` for current cases and bounds. Logs, fingerprints, patch,
substitutions, and results remain under `build/detection-*/` or a new `--output` directory. Selected
faults are not a mutation-coverage score. Runner contract tests are in `just python-check`; source
mutation runs remain opt-in. Timing rejection has a separate
[performance detector workflow](performance.md#validate-the-performance-detector).

### Coding-agent skill benchmark

```sh
just skill-bench
just skill-bench --help
```

The interactive local benchmark selects provider, model, and thinking level. It uses isolated
workspaces/runtimes, randomizes paired baseline/skill order, verifies terminal consequences
externally, and writes raw traces and JSON/Markdown reports under `build/agent-skill-benchmark/`.
Baseline runs cannot fetch the embedded skill through `frame skill`. Model calls may incur provider
charges; this is not a CI gate.

Pi is the built-in adapter. Another agent can supply `--adapter PATH`, invoked as `PATH REQUEST.json`
in the isolated workspace with its environment. The `frame.agent-skill-benchmark-request/v1` request
provides prompt, optional skill path, provider, model, thinking level, and timeout. The executable
prints one `frame.agent-skill-benchmark-result/v1` JSON object containing `returncode`, `final_text`,
normalized `tool_calls`/`tool_results` arrays, and `skill_loaded`; `usage` is optional. See the
[benchmark implementation](../tools/benchmark_frame_skill.py) and its
[contract tests](../tools/test_ci_agent_skill_benchmark.py).

## CI and Python tooling

Python tooling uses uv, Ruff, and ty: `uv sync --locked` installs the pinned environment and
`just python-check` validates it. Python stays outside native measurement loops.

[quality.yml](../.github/workflows/quality.yml) owns merge-blocking jobs. Documentation checks run
on every change, including source renames/deletions that could break links. Other lanes are selected
by [changed paths](../scripts/ci/changes.py). Schema and executable example changes select native
contract tests; Markdown-only edits do not select expensive C++ lanes.

[extended.yml](../.github/workflows/extended.yml) owns the platform matrix and scheduled simulation,
fuzz, and benchmark sweeps. Only successful trusted `main` jobs publish build caches; pull requests
and merge groups restore without writing. Local lane equivalents live in [scripts/ci](../scripts/ci/);
`just ci-check` runs the merge-blocking set in a safe sequence.

## Ghostty upgrades

Ghostty owns terminal semantics. Update these pins together:

- [PIN.json](../third_party/ghostty-metadata/PIN.json);
- [flake.lock](../flake.lock) and [flake.nix](../flake.nix); and
- the `third_party/ghostty` submodule used for non-Nix builds.

Inspect upstream API/semantic changes and [PATCHES.md](../third_party/ghostty-metadata/PATCHES.md),
then run terminal, mux, sanitizer, and relevant benchmark/resource checks. A local patch documents
why it exists and when to remove it. `PIN.json` owns ordered patch files and SHA-256 hashes. Nix and
submodule builds validate original source and apply patches to a private build-tree copy, never the
submodule or Nix store. The complete pin manifest identifies source/archive caches.

## Documentation

Maintain current supported behavior, contracts, boundaries, and durable rationale—not a record of
work performed. Keep each subject in one home; link instead of repeating its definition. Exact CLI
and JSON grammar belong to binary help and the embedded [schema](../schema/frame-api-v1.schema.json).
Local implementation rationale belongs beside its code.

When behavior changes, update its documentation and examples in the same change. Replace or delete
superseded text rather than append corrections. Resolve docs/code disagreements explicitly: a bug
is not permission to silently rewrite a supported contract. The behavior-change author owns this
maintenance; a change with no documentation impact needs no ceremonial prose edit.

Use Markdown links for paths readers should follow, including AGENTS.md pointers. `just docs-check`
checks local files/heading anchors and native keymap-catalog parity. It parses and schema-validates
JSON examples, and requires fenced blocks marked `example=../examples/FILE` to match their canonical
[example files](../examples/) exactly. Edit the example file, then synchronize its displayed block.
All example files must be referenced by a marked block. This prevents editing the displayed copy
without changing the tested input.

The mux suite loads Lua examples through `frame config check`, exercises the configuration and
custom command, executes the JSON job, and admits the extension Hello. Example changes therefore
select native tests. Other fences are illustrative: no documentation check executes arbitrary shell
commands, attaches to user Sessions, runs performance captures, or calls paid models. Link/schema
checks do not prove prose or runtime semantics correct; retain behavioral tests and review.

Generated measurements and disposable research stay under `build/`. Proposals belong in issues or
PR discussions; completed-work summaries and migration history belong in version control history.
Preserve accepted constraints in current docs or code, but remove obsolete designs rather than
create an archive inside `docs/`.
