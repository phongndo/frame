#![doc = r"
`frame-git` loads review snapshots from a Git working tree.

It keeps `git diff` as the source of truth for the changed file set and hunk
metadata, then resolves file bodies for code-first rendering.
"]

mod repo;

pub use repo::{GitError, load_review_snapshot_from_current_dir, load_review_snapshot_from_dir};
