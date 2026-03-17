# Frame

## Product Intent

Frame is a keyboard-first code review tool for AI-scale diffs. The current implementation is a code-first terminal review IDE: it loads changed files, overlays Git changes on full-file buffers, and keeps raw diff as a secondary inspection mode.

## Planning

The current product scope lives in [PROJECT.md](PROJECT.md).

## Current Status

Frame is pre-alpha. The repository now includes:

- Git-backed review snapshot loading for changed files
- a typed patch and review domain model
- a code-first TUI with change overlays
- a secondary raw diff mode
- review-only input affordances: `:` commands and `i` to queue comments for AI

Comments are staged locally in the TUI today; AI send flows, config loading, and syntax highlighting are still deferred.

## Local Setup

1. Install stable Rust with `clippy` and `rustfmt`.
2. Run `cargo run -p frame` inside a Git repository.

The application should open a read-only review IDE. In a clean repo it opens an empty-state view. Outside a Git repo it exits with an error.

Primary keys:

- `j` / `k`, `Ctrl-d` / `Ctrl-u`, `gg` / `G`
- `]c` / `[c` for change jumps
- `]f` / `[f` for changed-file jumps
- `gd` or `Tab` to toggle raw diff
- `:` for review commands
- `i` to queue a comment for AI on the current line

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

## Workspace Layout

- `crates/frame`: thin terminal application entrypoint
- `crates/frame-core`: patch parsing and review-domain types
- `crates/frame-git`: Git-backed snapshot loading
- `crates/frame-view`: read-only TUI rendering, navigation, and review input
- `.github/workflows/ci.yml`: Linux pull-request and push checks
