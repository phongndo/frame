# Frame

## Product Intent

Frame is a keyboard-first code review tool for AI-scale diffs. This repository now contains the first real implementation slice: Git diff ingestion, a core diff model, and a read-only terminal viewer.

## Planning

The current product scope lives in [PROJECT.md](PROJECT.md). This first commit is intentionally limited to repository scaffolding: Rust workspace setup, CI, dependency policy, and placeholder crates.

## Current Status

Frame is pre-alpha. The repository now includes Git diff ingestion, a typed core diff model, and a read-only TUI viewer with file/hunk navigation. Comments, AI send flows, config loading, and syntax highlighting are still deferred.

## Local Setup

1. Install stable Rust with `clippy` and `rustfmt`.
2. Run `cargo run -p frame` inside a Git repository.

The application should open a read-only diff viewer. In a clean repo it opens an empty-state view. Outside a Git repo it exits with an error.

## Verification Commands

Run the same checks locally that CI runs:

```sh
cargo fmt --all -- --check
cargo check --workspace --all-targets --all-features
cargo clippy --workspace --all-targets --all-features -- -D warnings
cargo test --workspace --all-targets --all-features
taplo fmt --check
typos
cargo deny check bans licenses advisories
actionlint
```

## Initial Workspace Layout

- `crates/frame`: thin terminal application entrypoint
- `crates/frame-view`: read-only TUI rendering and navigation
- `crates/libframe`: Git integration and diff domain model
- `.github/workflows/ci.yml`: Linux pull-request and push checks
