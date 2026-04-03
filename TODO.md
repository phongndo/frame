# Frame TODO

This checklist turns the current MVP spec into concrete work.

## Before MVP

- [ ] Complete the AI feedback loop.
  - Load and validate `~/.config/frame/config.toml`.
  - Implement `A` to build the review prompt from queued comments.
  - Spawn the configured AI command without a shell and write the prompt to `stdin`.
  - Surface subprocess progress, success, and failure in the TUI.
  - Refresh the review snapshot after a successful AI run.

- [ ] Make queued comments match the intended MVP contract.
  - Capture comment `side` (`added`, `removed`, `context`).
  - Capture `symbol` when it is cheap and reliable to derive.
  - Serialize queued comments into the prompt format defined in `PROJECT.md`.
  - Clear comments only after a successful AI send and refresh.

- [ ] Add the missing MVP git actions.
  - Implement `s` to toggle staging for the current file or hunk scope.
  - Implement `C` to create a commit from inside the app.
  - Show clear errors when Git rejects the action.

- [ ] Resolve spec drift between `PROJECT.md`, `README.md`, and the current TUI.
  - Decide whether the comment key is `space` or `i`.
  - Decide whether explorer mode is part of MVP or extra surface area.
  - Either implement the spec as written or update the spec to match the intended product.

- [ ] Close the syntax-highlighting gap for common review targets.
  - Add at least Python and TypeScript support.
  - Decide whether C/C++ is required for MVP.
  - If not, narrow the written claim in `PROJECT.md`.

- [ ] Add end-to-end coverage for the core loop.
  - Snapshot load from Git.
  - Queue comments.
  - Send comments to a fake AI subprocess.
  - Auto-refresh after the subprocess updates the worktree.

- [ ] Validate the interaction model on a real changed repository.
  - Review a non-trivial patch in Rust, Python, and TypeScript repos.
  - Confirm navigation, comment entry, and refresh are fast enough to feel editor-like.
  - Remove or simplify UI elements that do not improve review speed.

## Before Release

- [ ] Tighten failure handling and user messaging.
  - Missing config.
  - AI command not found.
  - AI command exits non-zero.
  - Git action failures.
  - Watcher startup or runtime failures.

- [ ] Add install and usage documentation for first-time users.
  - Exact Rust toolchain requirements.
  - How to configure the AI command.
  - Supported keybindings.
  - What happens in clean repos, dirty repos, and outside Git repos.

- [ ] Decide and document platform support.
  - Confirm macOS and Linux behavior.
  - Decide whether Windows is supported, unsupported, or deferred.

- [ ] Expand test coverage beyond unit tests.
  - Add integration tests around Git fixtures and subprocess behavior.
  - Add regression tests for renamed files, deleted files, binary files, and large diffs.
  - Add difftest coverage for rendering-sensitive UI behavior.

- [ ] Add release packaging and versioning.
  - Define the first supported version and changelog format.
  - Document install paths or package targets.
  - Create a repeatable release checklist.

- [ ] Run the full policy and CI toolchain locally before tagging a release.
  - `cargo fmt --all -- --check`
  - `cargo check --workspace --all-targets --all-features`
  - `cargo clippy --workspace --all-targets --all-features -- -D warnings`
  - `cargo test --workspace --all-targets --all-features`
  - `taplo fmt --check`
  - `typos`
  - `cargo deny check bans licenses advisories`
  - `actionlint`

- [ ] Prove the app is stable on representative repos before calling it release-ready.
  - Small repo with a few changed files.
  - Medium repo with nested directories and renames.
  - Repo with ignored-file churn while Frame is open.
  - Repo with large generated diffs or unsupported file types.

## Exit Criteria

MVP is done when the user can review code, leave comments, send them to an AI CLI, and see the updated patch reload without leaving Frame.

Release readiness is done when that loop is stable, documented, tested, and repeatable across supported environments.
