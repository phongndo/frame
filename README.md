# Frame

## Product Intent

Frame is a keyboard-first code review tool for AI-scale diffs. The current implementation is a review-first terminal IDE: it loads changed files, overlays Git changes on full-file buffers, keeps raw diff as a secondary inspection mode, and exposes a current-branch ship lane for staging, committing, pushing, and pull-request checks.

## Planning

The current product scope lives in [PROJECT.md](PROJECT.md).
Git-operation scope and roadmap live in [GIT.md](GIT.md).

## Current Status

Frame is pre-alpha. The repository now includes:

- Git-backed review snapshot loading for changed files
- a typed patch and review domain model
- a code-first TUI with change overlays
- a secondary raw diff mode
- review input affordances: `:` commands and `i` to queue comments for AI
- a floating Git panel plus direct review-surface git actions for current-branch staging, commit/amend, push, and PR status

Comments are staged locally in the TUI today; AI send flows and config loading are still deferred. GitHub integration is `gh`-backed and current-branch-only. Syntax highlighting is supported for built-in languages through Tree-sitter-derived highlights in the review snapshot.

## Local Setup

1. Install stable Rust with `clippy` and `rustfmt`.
2. Ensure `git` is available on `PATH`.
3. Install `gh` if you want PR creation and check summaries from inside `frame`.
4. Run `cargo run -p frame` inside a Git repository.

The application should open a review IDE. In a clean repo it opens an empty-state view. Outside a Git repo it exits with an error.

Primary keys:

- `j` / `k`, `Ctrl-d` / `Ctrl-u`, `gg` / `G`
- `]c` / `[c` for change jumps
- `]f` / `[f` for changed-file jumps
- `gd` or `Tab` to toggle raw diff
- `Ctrl-g` or `:git` to open the Git panel
- `s` to toggle stage for the current reviewed change
- `C` to open the commit dialog, `P` to push, `F` to force-with-lease, `R` to create or refresh the PR
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
- `crates/frame-git`: Git-backed snapshot loading plus typed ship-lane operations
- `crates/frame-view`: review-first TUI rendering, navigation, review input, and git ship-lane UI
- `.github/workflows/ci.yml`: Linux pull-request and push checks
