# Frame

## Product Intent

Frame is a keyboard-first code review tool for AI-scale diffs. This repository is the initial scaffold for the project and does not include the review UI, diff engine, or language tooling yet.

## Planning

The current product scope lives in [PROJECT.md](PROJECT.md). This first commit is intentionally limited to repository scaffolding: Rust workspace setup, CI, dependency policy, and placeholder crates.

## Current Status

Frame is pre-alpha and intentionally infrastructure-only in this first PR. TUI bootstrapping, syntax highlighting, LSP integration, git diff ingestion, and GitHub review flows are deferred to follow-up work.

## Local Setup

1. Install stable Rust with `clippy` and `rustfmt`.
2. Run `cargo run -p frame`.

The placeholder binary should print a scaffold status line from `libframe` and exit successfully.

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

- `crates/frame`: placeholder CLI crate for the future application entrypoint
- `crates/libframe`: placeholder library crate for future reusable diff and review primitives
- `.github/workflows/ci.yml`: Linux pull-request and push checks
