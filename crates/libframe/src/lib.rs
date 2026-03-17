#![doc = r"
`libframe` hosts the core Git and diff logic for Frame.

The library is intentionally UI-agnostic: it knows how to obtain diffs from Git
and normalize them into typed review data structures, but it does not render
anything itself.
"]

mod diff;
mod git;

pub(crate) use diff::parse_diff;
pub use diff::{Diff, DiffFile, DiffLine, DiffParseError, FileChangeKind, Hunk, LineKind};
pub use git::{GitError, RepoDiff, load_diff_from_current_dir, load_diff_from_dir};
