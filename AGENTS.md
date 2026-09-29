# Frame

Frame is a terminal multiplexer focused on performance and extensibility. It prioritizes responsive
terminal interaction and low resource overhead as workloads, panes, and clients scale.

Keep the daemon's `Session -> Tab -> Pane` kernel small. Projects, worktrees, and agent workflows
compose the public API rather than become kernel objects. Extensions run as external programs,
observing Events, submitting Procs, and presenting UI through Surfaces; terminal parsing and frame
composition stay native and independent of extension execution.

## Working in the repository

Read only the code, tests, and documentation relevant to the change. Public documentation defines
the supported contract; code defines current implementation reality.

Use:

- [Usage](docs/usage.md) for user behavior;
- [Configuration](docs/configuration.md) for Lua settings, keymaps, and custom commands;
- [Automation API](docs/api.md) for Proc and Event contracts;
- [Runtime extensions](docs/extensions.md) for external processes and Surfaces;
- [Architecture](docs/architecture.md#core-and-user-layers) for core/user layer separation, UI ownership,
  and data flow;
- [Performance](docs/performance.md) for hot-path changes and measurement requirements;
- [Development](docs/development.md) for verification and dependency upgrades; and
- [Documentation](docs/development.md#documentation) when changing docs or examples.

## Design

- Treat performance as a product property, especially on per-byte, event, pane, frame, and client
  paths.
- Encode invariants in types, construction, and ownership so invalid states are hard to represent.
- Keep one authority per mutable fact; derive projections instead of duplicating state.
- Prefer bounded, direct designs with fewer owners, transitions, copies, and abstractions.
- Preserve ordering, lifetimes, failure behavior, and dependency-owned semantics.
- Measure when added hot-path complexity or a performance claim depends on the result.

Do not create plans, roadmaps, TODO documents, or historical reports unless requested.

## Verification

Use repository entry points (`just --list`, `./test --help`). Run focused checks while developing
and `just check` before completion. Report commands actually run and any failing or blocked checks.
