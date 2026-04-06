#![doc = r"
`frame-git` loads review snapshots from a Git working tree.

It keeps `git diff` as the source of truth for the changed file set and hunk
metadata, then resolves file bodies for code-first rendering.
"]

mod repo;

pub use repo::{
    BranchStatus, CommitMode, CommitRequest, GitDiffSide, GitError, GitSelection,
    GitStatusSnapshot, PullRequestCheck, PullRequestStatus, PushMode, commit_from_current_dir,
    commit_from_dir, current_branch_name_from_dir, ensure_pull_request_from_current_dir,
    ensure_pull_request_from_dir, head_commit_message_from_current_dir,
    head_commit_message_from_dir, ignored_paths, is_path_git_ignored,
    load_git_status_from_current_dir, load_git_status_from_dir,
    load_pull_request_status_from_current_dir, load_pull_request_status_from_dir,
    load_review_snapshot_from_current_dir, load_review_snapshot_from_dir, push_from_current_dir,
    push_from_dir, resolve_git_common_dir_from_dir, resolve_git_dir_from_dir,
    toggle_stage_from_current_dir, toggle_stage_from_dir,
};
