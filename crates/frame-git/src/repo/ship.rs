use std::{
    env,
    ffi::OsStr,
    fmt::Write as _,
    fs,
    path::Path,
    process::{Command, Output},
};

use frame_core::{LineKind, PatchFile, PatchHunk, PatchSet, parse_patch};
use serde::Deserialize;

use super::{GitError, head_exists, repo_root, shell::run_git_allowing_status, untracked_files};

#[cfg(unix)]
use std::os::unix::fs::PermissionsExt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BranchStatus {
    pub head: String,
    pub upstream: Option<String>,
    pub ahead: usize,
    pub behind: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum GitDiffSide {
    Staged,
    Unstaged,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GitStatusSnapshot {
    pub branch: BranchStatus,
    pub staged: PatchSet,
    pub unstaged: PatchSet,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum GitSelection {
    File {
        side: GitDiffSide,
        path: String,
    },
    Hunk {
        side: GitDiffSide,
        path: String,
        hunk_index: usize,
    },
    Line {
        side: GitDiffSide,
        path: String,
        hunk_index: usize,
        line_index: usize,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CommitMode {
    Create,
    Amend,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CommitRequest {
    pub message: String,
    pub mode: CommitMode,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PushMode {
    Normal,
    ForceWithLease,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PullRequestCheck {
    pub name: String,
    pub status: String,
    pub conclusion: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PullRequestStatus {
    pub number: u64,
    pub title: String,
    pub url: String,
    pub head_ref_name: String,
    pub base_ref_name: String,
    pub state: String,
    pub checks: Vec<PullRequestCheck>,
}

/// Loads the current repository's branch summary plus staged and unstaged diffs.
///
/// # Errors
///
/// Returns an error if the current directory cannot be resolved, the directory
/// is not inside a Git repository, Git cannot be executed, or either diff
/// cannot be parsed.
pub fn load_git_status_from_current_dir() -> Result<GitStatusSnapshot, GitError> {
    let current_dir = env::current_dir().map_err(GitError::CurrentDir)?;
    load_git_status_from_dir(&current_dir)
}

/// Loads the provided repository's branch summary plus staged and unstaged diffs.
///
/// # Errors
///
/// Returns an error if `cwd` is not inside a Git repository, Git cannot be
/// executed, or either diff cannot be parsed.
pub fn load_git_status_from_dir(cwd: &Path) -> Result<GitStatusSnapshot, GitError> {
    let repo_root = repo_root(cwd)?;
    let branch = branch_status(&repo_root)?;
    let staged = parse_patch(&staged_patch_text(&repo_root)?)?;
    let unstaged = parse_patch(&unstaged_patch_text(&repo_root)?)?;

    Ok(GitStatusSnapshot {
        branch,
        staged,
        unstaged,
    })
}

/// Returns the currently checked-out branch name for the repository at `cwd`.
///
/// # Errors
///
/// Returns an error if `cwd` is not inside a Git repository, Git cannot be
/// executed, or the repository is in detached `HEAD`.
pub fn current_branch_name_from_dir(cwd: &Path) -> Result<String, GitError> {
    let branch = branch_status(&repo_root(cwd)?)?;
    if branch.head == "(detached)" {
        return Err(GitError::MalformedStatus(
            "detached HEAD is not supported by the git ship lane".to_owned(),
        ));
    }

    Ok(branch.head)
}

/// Returns the current `HEAD` commit message for the working repository.
///
/// # Errors
///
/// Returns an error if the current directory cannot be resolved or Git cannot
/// be executed for the containing repository.
pub fn head_commit_message_from_current_dir() -> Result<String, GitError> {
    let current_dir = env::current_dir().map_err(GitError::CurrentDir)?;
    head_commit_message_from_dir(&current_dir)
}

/// Returns the current `HEAD` commit message for the repository at `cwd`.
///
/// # Errors
///
/// Returns an error if `cwd` is not inside a Git repository or Git cannot be
/// executed.
pub fn head_commit_message_from_dir(cwd: &Path) -> Result<String, GitError> {
    let repo_root = repo_root(cwd)?;
    let output = run_git_allowing_status(&repo_root, ["log", "-1", "--format=%B"], &[0, 128])?;
    if !output.status.success() {
        return Ok(String::new());
    }

    Ok(String::from_utf8_lossy(&output.stdout)
        .trim_end()
        .to_owned())
}

/// Toggles the stage state for a file, hunk, or line in the current repository.
///
/// # Errors
///
/// Returns an error if the current directory cannot be resolved, Git cannot be
/// executed, or the selected diff item cannot be staged independently.
pub fn toggle_stage_from_current_dir(selection: &GitSelection) -> Result<(), GitError> {
    let current_dir = env::current_dir().map_err(GitError::CurrentDir)?;
    toggle_stage_from_dir(&current_dir, selection)
}

/// Toggles the stage state for a file, hunk, or line in the repository at `cwd`.
///
/// # Errors
///
/// Returns an error if `cwd` is not inside a Git repository, Git cannot be
/// executed, diff metadata cannot be loaded, or the selected diff item cannot
/// be staged independently.
pub fn toggle_stage_from_dir(cwd: &Path, selection: &GitSelection) -> Result<(), GitError> {
    let repo_root = repo_root(cwd)?;
    match selection {
        GitSelection::File {
            side: GitDiffSide::Unstaged,
            path,
        } => stage_file(&repo_root, path),
        GitSelection::File {
            side: GitDiffSide::Staged,
            path,
        } => unstage_file(&repo_root, path),
        GitSelection::Hunk {
            side,
            path,
            hunk_index,
        } => {
            let status = load_git_status_from_dir(&repo_root)?;
            let file = find_patch_file(&status, *side, path)?;
            if matches!(
                file.change,
                frame_core::FileChangeKind::Renamed | frame_core::FileChangeKind::Copied
            ) {
                return Err(GitError::UnsupportedSelection(format!(
                    "{path}: partial staging is not supported for renamed/copied files"
                )));
            }

            let hunk = file.hunks.get(*hunk_index).ok_or_else(|| {
                GitError::UnsupportedSelection(format!("{path}: missing hunk {hunk_index}"))
            })?;
            let patch_text = render_patch_text(&repo_root, file, std::slice::from_ref(hunk))?;
            apply_patch_to_index(&repo_root, &patch_text, *side == GitDiffSide::Staged)
        }
        GitSelection::Line {
            side,
            path,
            hunk_index,
            line_index,
        } => {
            let status = load_git_status_from_dir(&repo_root)?;
            let file = find_patch_file(&status, *side, path)?;
            if matches!(
                file.change,
                frame_core::FileChangeKind::Renamed | frame_core::FileChangeKind::Copied
            ) {
                return Err(GitError::UnsupportedSelection(format!(
                    "{path}: partial staging is not supported for renamed/copied files"
                )));
            }
            let hunk = file.hunks.get(*hunk_index).ok_or_else(|| {
                GitError::UnsupportedSelection(format!("{path}: missing hunk {hunk_index}"))
            })?;
            let line_hunk = single_line_hunk(hunk, *line_index).ok_or_else(|| {
                GitError::UnsupportedSelection(format!(
                    "{path}: line staging is only supported for added/removed diff lines"
                ))
            })?;
            let patch_text = render_patch_text(&repo_root, file, std::slice::from_ref(&line_hunk))?;
            apply_patch_to_index(&repo_root, &patch_text, *side == GitDiffSide::Staged)
        }
    }
}

/// Creates or amends a commit in the current repository.
///
/// # Errors
///
/// Returns an error if the current directory cannot be resolved, Git cannot be
/// executed, or the commit request is invalid.
pub fn commit_from_current_dir(request: &CommitRequest) -> Result<(), GitError> {
    let current_dir = env::current_dir().map_err(GitError::CurrentDir)?;
    commit_from_dir(&current_dir, request)
}

/// Creates or amends a commit in the repository at `cwd`.
///
/// # Errors
///
/// Returns an error if `cwd` is not inside a Git repository, Git cannot be
/// executed, or the commit request is invalid.
pub fn commit_from_dir(cwd: &Path, request: &CommitRequest) -> Result<(), GitError> {
    let repo_root = repo_root(cwd)?;
    let message = request.message.trim();
    if message.is_empty() {
        return Err(GitError::CommandFailed(
            "commit message cannot be empty".to_owned(),
        ));
    }

    let mut args = vec![
        OsStr::new("-c"),
        OsStr::new("commit.gpgsign=false"),
        OsStr::new("commit"),
        OsStr::new("--quiet"),
        OsStr::new("--file"),
        OsStr::new("-"),
    ];
    if matches!(request.mode, CommitMode::Amend) {
        args.push(OsStr::new("--amend"));
    }
    run_git_with_input_allowing_status(&repo_root, args, message.as_bytes(), &[0])?;
    Ok(())
}

/// Pushes the current branch for the working repository.
///
/// # Errors
///
/// Returns an error if the current directory cannot be resolved, Git cannot be
/// executed, or the push is rejected.
pub fn push_from_current_dir(mode: PushMode) -> Result<(), GitError> {
    let current_dir = env::current_dir().map_err(GitError::CurrentDir)?;
    push_from_dir(&current_dir, mode)
}

/// Pushes the current branch for the repository at `cwd`.
///
/// # Errors
///
/// Returns an error if `cwd` is not inside a Git repository, Git cannot be
/// executed, or the push is rejected.
pub fn push_from_dir(cwd: &Path, mode: PushMode) -> Result<(), GitError> {
    let repo_root = repo_root(cwd)?;
    let branch = branch_status(&repo_root)?;
    if branch.upstream.is_some() {
        match mode {
            PushMode::Normal => {
                run_git_allowing_status(&repo_root, ["push"], &[0])?;
            }
            PushMode::ForceWithLease => {
                run_git_allowing_status(&repo_root, ["push", "--force-with-lease"], &[0])?;
            }
        }
    } else {
        let branch_name = current_branch_name_from_dir(&repo_root)?;
        let remote_name = configured_remote_for_branch(&repo_root, &branch_name)?
            .unwrap_or_else(|| "origin".to_owned());
        let mut args = vec!["push".to_owned(), "--set-upstream".to_owned()];
        if matches!(mode, PushMode::ForceWithLease) {
            args.push("--force-with-lease".to_owned());
        }
        args.push(remote_name);
        args.push(branch_name);
        run_git_allowing_status(&repo_root, args, &[0])?;
    }

    Ok(())
}

/// Loads the pull request for the current branch in the working repository.
///
/// # Errors
///
/// Returns an error if the current directory cannot be resolved, Git cannot be
/// executed, or `gh` cannot be executed or parsed.
pub fn load_pull_request_status_from_current_dir() -> Result<Option<PullRequestStatus>, GitError> {
    let current_dir = env::current_dir().map_err(GitError::CurrentDir)?;
    load_pull_request_status_from_dir(&current_dir)
}

/// Loads the pull request for the current branch in the repository at `cwd`.
///
/// # Errors
///
/// Returns an error if `cwd` is not inside a Git repository, Git cannot be
/// executed, or `gh` cannot be executed or parsed.
pub fn load_pull_request_status_from_dir(
    cwd: &Path,
) -> Result<Option<PullRequestStatus>, GitError> {
    let repo_root = repo_root(cwd)?;
    let branch = current_branch_name_from_dir(&repo_root)?;
    let output = run_gh(
        &repo_root,
        [
            "pr",
            "list",
            "--head",
            branch.as_str(),
            "--json",
            "number,url,title,state,headRefName,baseRefName,statusCheckRollup",
            "--limit",
            "1",
        ],
        &[0],
    )?;
    let mut prs: Vec<PullRequestRecord> = serde_json::from_slice(&output.stdout)?;
    Ok(prs.drain(..).next().map(PullRequestStatus::from))
}

/// Ensures that the current branch in the working repository has a pull request.
///
/// # Errors
///
/// Returns an error if the current directory cannot be resolved, Git cannot be
/// executed, or `gh` cannot create or reload the pull request.
pub fn ensure_pull_request_from_current_dir() -> Result<PullRequestStatus, GitError> {
    let current_dir = env::current_dir().map_err(GitError::CurrentDir)?;
    ensure_pull_request_from_dir(&current_dir)
}

/// Ensures that the current branch in the repository at `cwd` has a pull request.
///
/// # Errors
///
/// Returns an error if `cwd` is not inside a Git repository, Git cannot be
/// executed, or `gh` cannot create or reload the pull request.
pub fn ensure_pull_request_from_dir(cwd: &Path) -> Result<PullRequestStatus, GitError> {
    let repo_root = repo_root(cwd)?;
    if let Some(pr) = load_pull_request_status_from_dir(&repo_root)? {
        return Ok(pr);
    }

    run_gh(&repo_root, ["pr", "create", "--fill"], &[0])?;
    load_pull_request_status_from_dir(&repo_root)?.ok_or_else(|| {
        GitError::GhCommandFailed("created pull request but could not reload its status".to_owned())
    })
}

fn branch_status(cwd: &Path) -> Result<BranchStatus, GitError> {
    let output = run_git_allowing_status(cwd, ["status", "--porcelain=v2", "--branch"], &[0])?;
    parse_branch_status(&String::from_utf8_lossy(&output.stdout))
}

fn parse_branch_status(output: &str) -> Result<BranchStatus, GitError> {
    let mut head = None;
    let mut upstream = None;
    let mut ahead = 0usize;
    let mut behind = 0usize;

    for line in output.lines() {
        if let Some(value) = line.strip_prefix("# branch.head ") {
            head = Some(value.to_owned());
        } else if let Some(value) = line.strip_prefix("# branch.upstream ") {
            upstream = Some(value.to_owned());
        } else if let Some(value) = line.strip_prefix("# branch.ab ") {
            let mut parts = value.split_whitespace();
            let ahead_part = parts
                .next()
                .ok_or_else(|| GitError::MalformedStatus(line.to_owned()))?;
            let behind_part = parts
                .next()
                .ok_or_else(|| GitError::MalformedStatus(line.to_owned()))?;
            ahead = ahead_part
                .trim_start_matches('+')
                .parse()
                .map_err(|_| GitError::MalformedStatus(line.to_owned()))?;
            behind = behind_part
                .trim_start_matches('-')
                .parse()
                .map_err(|_| GitError::MalformedStatus(line.to_owned()))?;
        }
    }

    Ok(BranchStatus {
        head: head.unwrap_or_else(|| "(detached)".to_owned()),
        upstream,
        ahead,
        behind,
    })
}

fn staged_patch_text(cwd: &Path) -> Result<String, GitError> {
    let mut args = vec![
        "diff",
        "--cached",
        "--no-ext-diff",
        "--find-renames",
        "--no-color",
        "--unified=0",
    ];
    if !head_exists(cwd)? {
        args.push("--root");
    }

    let output = run_git_allowing_status(cwd, args, &[0])?;
    Ok(String::from_utf8_lossy(&output.stdout).into_owned())
}

fn unstaged_patch_text(cwd: &Path) -> Result<String, GitError> {
    let mut patch_text = String::from_utf8_lossy(
        &run_git_allowing_status(
            cwd,
            [
                "diff",
                "--no-ext-diff",
                "--find-renames",
                "--no-color",
                "--unified=0",
            ],
            &[0],
        )?
        .stdout,
    )
    .into_owned();

    for path in untracked_files(cwd)? {
        if !patch_text.is_empty() && !patch_text.ends_with('\n') {
            patch_text.push('\n');
        }
        patch_text.push_str(&untracked_file_patch_text_zero(cwd, &path)?);
    }

    Ok(patch_text)
}

fn untracked_file_patch_text_zero(cwd: &Path, path: &Path) -> Result<String, GitError> {
    let output = run_git_allowing_status(
        cwd,
        [
            OsStr::new("diff"),
            OsStr::new("--no-index"),
            OsStr::new("--no-color"),
            OsStr::new("--unified=0"),
            OsStr::new("--"),
            OsStr::new("/dev/null"),
            path.as_os_str(),
        ],
        &[0, 1],
    )?;
    Ok(String::from_utf8_lossy(&output.stdout).into_owned())
}

fn stage_file(cwd: &Path, path: &str) -> Result<(), GitError> {
    run_git_allowing_status(cwd, ["add", "--", path], &[0])?;
    Ok(())
}

fn unstage_file(cwd: &Path, path: &str) -> Result<(), GitError> {
    if head_exists(cwd)? {
        run_git_allowing_status(
            cwd,
            ["restore", "--staged", "--source=HEAD", "--", path],
            &[0],
        )?;
    } else {
        run_git_allowing_status(cwd, ["rm", "--cached", "--quiet", "--", path], &[0, 128])?;
    }
    Ok(())
}

fn find_patch_file<'a>(
    status: &'a GitStatusSnapshot,
    side: GitDiffSide,
    path: &str,
) -> Result<&'a PatchFile, GitError> {
    let patch_set = match side {
        GitDiffSide::Staged => &status.staged,
        GitDiffSide::Unstaged => &status.unstaged,
    };
    patch_set
        .files
        .iter()
        .find(|file| file.display_path() == path)
        .ok_or_else(|| {
            GitError::UnsupportedSelection(format!("{path}: no diff data for selection"))
        })
}

fn single_line_hunk(hunk: &PatchHunk, line_index: usize) -> Option<PatchHunk> {
    let line = hunk.lines.get(line_index)?;
    if !matches!(line.kind, LineKind::Added | LineKind::Removed) {
        return None;
    }

    let (old_start, old_len, new_start, new_len) = match line.kind {
        LineKind::Added => (
            line.new_lineno.unwrap_or(hunk.new_start).saturating_sub(1),
            0,
            line.new_lineno?,
            1,
        ),
        LineKind::Removed => (
            line.old_lineno?,
            1,
            line.old_lineno.unwrap_or(hunk.old_start).saturating_sub(1),
            0,
        ),
        LineKind::Context => return None,
    };

    Some(PatchHunk {
        header: format!("@@ -{old_start},{old_len} +{new_start},{new_len} @@"),
        old_start,
        old_len,
        new_start,
        new_len,
        lines: vec![line.clone()],
    })
}

fn apply_patch_to_index(cwd: &Path, patch_text: &str, reverse: bool) -> Result<(), GitError> {
    let mut args: Vec<&OsStr> = vec![
        OsStr::new("apply"),
        OsStr::new("--cached"),
        OsStr::new("--unidiff-zero"),
        OsStr::new("-"),
    ];
    if reverse {
        args.insert(2, OsStr::new("-R"));
    }
    run_git_with_input_allowing_status(cwd, args, patch_text.as_bytes(), &[0])?;
    Ok(())
}

fn render_patch_text(
    cwd: &Path,
    file: &PatchFile,
    hunks: &[PatchHunk],
) -> Result<String, GitError> {
    let old_display = display_old_path(file);
    let new_display = display_new_path(file);
    let mut patch = String::new();
    let _ = writeln!(patch, "diff --git {old_display} {new_display}");
    match file.change {
        frame_core::FileChangeKind::Added => {
            let _ = writeln!(patch, "new file mode {}", added_file_mode(cwd, file)?);
        }
        frame_core::FileChangeKind::Deleted => {
            let _ = writeln!(patch, "deleted file mode {}", deleted_file_mode(cwd, file)?);
        }
        frame_core::FileChangeKind::Renamed => {
            if let Some(old_path) = &file.old_path {
                let _ = writeln!(patch, "rename from {old_path}");
            }
            if let Some(new_path) = &file.new_path {
                let _ = writeln!(patch, "rename to {new_path}");
            }
        }
        frame_core::FileChangeKind::Copied => {
            if let Some(old_path) = &file.old_path {
                let _ = writeln!(patch, "copy from {old_path}");
            }
            if let Some(new_path) = &file.new_path {
                let _ = writeln!(patch, "copy to {new_path}");
            }
        }
        frame_core::FileChangeKind::Modified => {}
    }
    let _ = writeln!(patch, "--- {}", display_old_path(file));
    let _ = writeln!(patch, "+++ {}", display_new_path(file));
    for hunk in hunks {
        let _ = writeln!(patch, "{}", hunk.header);
        for line in &hunk.lines {
            let prefix = match line.kind {
                LineKind::Added => '+',
                LineKind::Removed => '-',
                LineKind::Context => ' ',
            };
            patch.push(prefix);
            patch.push_str(&line.text);
            patch.push('\n');
        }
    }
    Ok(patch)
}

fn added_file_mode(cwd: &Path, file: &PatchFile) -> Result<&'static str, GitError> {
    let path = file
        .new_path
        .as_deref()
        .ok_or_else(|| GitError::UnsupportedSelection("added file path unavailable".to_owned()))?;
    worktree_file_mode(&cwd.join(path))
}

fn display_old_path(file: &PatchFile) -> String {
    file.old_path
        .as_deref()
        .map_or_else(|| "/dev/null".to_owned(), |path| format!("a/{path}"))
}

fn display_new_path(file: &PatchFile) -> String {
    file.new_path
        .as_deref()
        .map_or_else(|| "/dev/null".to_owned(), |path| format!("b/{path}"))
}

fn deleted_file_mode(cwd: &Path, file: &PatchFile) -> Result<String, GitError> {
    let path = file.old_path.as_deref().ok_or_else(|| {
        GitError::UnsupportedSelection("deleted file path unavailable".to_owned())
    })?;
    if let Some(mode) = index_file_mode(cwd, path)? {
        return Ok(mode);
    }
    if let Some(mode) = head_file_mode(cwd, path)? {
        return Ok(mode);
    }
    Ok("100644".to_owned())
}

fn worktree_file_mode(path: &Path) -> Result<&'static str, GitError> {
    let metadata = fs::metadata(path)?;
    Ok(mode_for_metadata(&metadata))
}

#[cfg(unix)]
fn mode_for_metadata(metadata: &fs::Metadata) -> &'static str {
    if metadata.permissions().mode() & 0o111 != 0 {
        "100755"
    } else {
        "100644"
    }
}

#[cfg(not(unix))]
fn mode_for_metadata(_metadata: &fs::Metadata) -> &'static str {
    "100644"
}

fn index_file_mode(cwd: &Path, path: &str) -> Result<Option<String>, GitError> {
    let output = run_git_allowing_status(cwd, ["ls-files", "--stage", "--", path], &[0])?;
    Ok(parse_mode_from_listing(&String::from_utf8_lossy(
        &output.stdout,
    )))
}

fn head_file_mode(cwd: &Path, path: &str) -> Result<Option<String>, GitError> {
    if !head_exists(cwd)? {
        return Ok(None);
    }
    let output = run_git_allowing_status(cwd, ["ls-tree", "HEAD", "--", path], &[0])?;
    Ok(parse_mode_from_listing(&String::from_utf8_lossy(
        &output.stdout,
    )))
}

fn parse_mode_from_listing(output: &str) -> Option<String> {
    output
        .lines()
        .find_map(|line| line.split_whitespace().next().map(ToOwned::to_owned))
}

fn configured_remote_for_branch(cwd: &Path, branch_name: &str) -> Result<Option<String>, GitError> {
    let key = format!("branch.{branch_name}.remote");
    let output = run_git_allowing_status(cwd, ["config", "--get", key.as_str()], &[0, 1])?;
    if !output.status.success() {
        return Ok(None);
    }

    let remote = String::from_utf8_lossy(&output.stdout).trim().to_owned();
    Ok((!remote.is_empty()).then_some(remote))
}

fn run_git_with_input_allowing_status<I, S>(
    cwd: &Path,
    args: I,
    input: &[u8],
    allowed_statuses: &[i32],
) -> Result<Output, GitError>
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    super::shell::run_git_with_input_allowing_status(cwd, args, input, allowed_statuses)
}

fn run_gh<I, S>(cwd: &Path, args: I, allowed_statuses: &[i32]) -> Result<Output, GitError>
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    let output = Command::new("gh").current_dir(cwd).args(args).output();
    let output = match output {
        Ok(output) => output,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            return Err(GitError::GhUnavailable);
        }
        Err(error) => return Err(GitError::Io(error)),
    };

    if output
        .status
        .code()
        .is_some_and(|code| allowed_statuses.contains(&code))
    {
        return Ok(output);
    }

    Err(GitError::GhCommandFailed(
        String::from_utf8_lossy(&output.stderr).trim().to_owned(),
    ))
}

#[derive(Debug, Deserialize)]
struct PullRequestRecord {
    number: u64,
    title: String,
    url: String,
    #[serde(rename = "headRefName")]
    head_ref_name: String,
    #[serde(rename = "baseRefName")]
    base_ref_name: String,
    state: String,
    #[serde(rename = "statusCheckRollup", default)]
    checks: Vec<CheckRecord>,
}

#[derive(Debug, Deserialize)]
struct CheckRecord {
    #[serde(default)]
    name: Option<String>,
    #[serde(default)]
    context: Option<String>,
    #[serde(default)]
    status: Option<String>,
    #[serde(default)]
    state: Option<String>,
    #[serde(default)]
    conclusion: Option<String>,
}

impl From<PullRequestRecord> for PullRequestStatus {
    fn from(value: PullRequestRecord) -> Self {
        Self {
            number: value.number,
            title: value.title,
            url: value.url,
            head_ref_name: value.head_ref_name,
            base_ref_name: value.base_ref_name,
            state: value.state,
            checks: value
                .checks
                .into_iter()
                .map(|check| PullRequestCheck {
                    name: check
                        .name
                        .or(check.context)
                        .unwrap_or_else(|| "check".to_owned()),
                    status: check
                        .status
                        .or(check.state)
                        .unwrap_or_else(|| "UNKNOWN".to_owned()),
                    conclusion: check.conclusion,
                })
                .collect(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{
        BranchStatus, CommitMode, CommitRequest, GitDiffSide, GitSelection, commit_from_dir,
        current_branch_name_from_dir, head_commit_message_from_dir, load_git_status_from_dir,
        parse_branch_status, push_from_dir, toggle_stage_from_dir,
    };
    use std::{
        fs,
        path::{Path, PathBuf},
        process::Command,
        sync::atomic::{AtomicUsize, Ordering},
        time::{SystemTime, UNIX_EPOCH},
    };

    #[cfg(unix)]
    use std::os::unix::fs::PermissionsExt;

    static TEMP_COUNTER: AtomicUsize = AtomicUsize::new(0);

    #[derive(Debug)]
    struct TempGitDir {
        path: PathBuf,
    }

    impl TempGitDir {
        fn new(name: &str) -> Self {
            let counter = TEMP_COUNTER.fetch_add(1, Ordering::Relaxed);
            let unique = format!(
                "frame-ship-{name}-{}-{}-{counter}",
                std::process::id(),
                SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .expect("system time should be after the unix epoch")
                    .as_nanos()
            );
            let path = std::env::temp_dir().join(unique);
            fs::create_dir_all(&path).expect("temp dir should be created");
            Self { path }
        }

        fn path(&self) -> &Path {
            &self.path
        }
    }

    impl Drop for TempGitDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.path);
        }
    }

    fn git(cwd: &Path, args: &[&str]) {
        let status = Command::new("git")
            .current_dir(cwd)
            .args(args)
            .status()
            .expect("git command should start");
        assert!(status.success(), "git command should succeed");
    }

    fn git_output(cwd: &Path, args: &[&str]) -> String {
        let output = Command::new("git")
            .current_dir(cwd)
            .args(args)
            .output()
            .expect("git command should start");
        assert!(output.status.success(), "git command should succeed");
        String::from_utf8_lossy(&output.stdout).into_owned()
    }

    fn write(path: &Path, contents: &str) {
        fs::write(path, contents).expect("file write should succeed");
    }

    fn init_repo() -> TempGitDir {
        let temp = TempGitDir::new("repo");
        git(temp.path(), &["init", "--quiet"]);
        git(
            temp.path(),
            &["config", "user.email", "frame-tests@example.com"],
        );
        git(temp.path(), &["config", "user.name", "Frame Tests"]);
        write(
            &temp.path().join("tracked.txt"),
            "line one\nline two\nline three\n",
        );
        git(temp.path(), &["add", "tracked.txt"]);
        git(
            temp.path(),
            &[
                "-c",
                "commit.gpgsign=false",
                "commit",
                "--quiet",
                "-m",
                "init",
            ],
        );
        temp
    }

    #[test]
    fn parses_branch_status_fields() {
        let status = parse_branch_status(
            "# branch.oid deadbeef\n# branch.head feat/demo\n# branch.upstream origin/feat/demo\n# branch.ab +2 -1\n",
        )
        .expect("status should parse");

        assert_eq!(
            status,
            BranchStatus {
                head: "feat/demo".to_owned(),
                upstream: Some("origin/feat/demo".to_owned()),
                ahead: 2,
                behind: 1,
            }
        );
    }

    #[test]
    fn loads_staged_and_unstaged_patch_sets() {
        let repo = init_repo();
        write(
            &repo.path().join("tracked.txt"),
            "line one\nline two changed\nline three\nline four\n",
        );
        git(repo.path(), &["add", "tracked.txt"]);
        write(
            &repo.path().join("tracked.txt"),
            "line one\nline two changed\nline three again\nline four\n",
        );

        let status = load_git_status_from_dir(repo.path()).expect("status should load");
        assert_eq!(status.staged.file_count(), 1);
        assert_eq!(status.unstaged.file_count(), 1);
    }

    #[test]
    fn stages_and_unstages_whole_files() {
        let repo = init_repo();
        write(
            &repo.path().join("tracked.txt"),
            "line one\nline two changed\nline three\n",
        );

        toggle_stage_from_dir(
            repo.path(),
            &GitSelection::File {
                side: GitDiffSide::Unstaged,
                path: "tracked.txt".to_owned(),
            },
        )
        .expect("file should stage");
        assert!(git_output(repo.path(), &["diff", "--cached"]).contains("line two changed"));

        toggle_stage_from_dir(
            repo.path(),
            &GitSelection::File {
                side: GitDiffSide::Staged,
                path: "tracked.txt".to_owned(),
            },
        )
        .expect("file should unstage");
        assert!(git_output(repo.path(), &["diff", "--cached"]).is_empty());
    }

    #[test]
    fn stages_a_single_hunk() {
        let repo = init_repo();
        write(
            &repo.path().join("tracked.txt"),
            "line one changed\nline two\nline three\n",
        );

        let status = load_git_status_from_dir(repo.path()).expect("status should load");
        assert_eq!(status.unstaged.files[0].hunks.len(), 1);

        toggle_stage_from_dir(
            repo.path(),
            &GitSelection::Hunk {
                side: GitDiffSide::Unstaged,
                path: "tracked.txt".to_owned(),
                hunk_index: 0,
            },
        )
        .expect("hunk should stage");

        assert!(git_output(repo.path(), &["diff", "--cached"]).contains("line one changed"));
    }

    #[test]
    fn stages_a_single_added_line_from_a_multi_line_hunk() {
        let repo = init_repo();
        write(
            &repo.path().join("tracked.txt"),
            "line one\nline two\nline three\nline four\n",
        );
        git(repo.path(), &["add", "tracked.txt"]);
        git(
            repo.path(),
            &[
                "-c",
                "commit.gpgsign=false",
                "commit",
                "--quiet",
                "-m",
                "expand fixture",
            ],
        );
        write(
            &repo.path().join("tracked.txt"),
            "line one\nline one point five\nline one point seven five\nline two\nline three\nline four\n",
        );

        toggle_stage_from_dir(
            repo.path(),
            &GitSelection::Line {
                side: GitDiffSide::Unstaged,
                path: "tracked.txt".to_owned(),
                hunk_index: 0,
                line_index: 0,
            },
        )
        .expect("line should stage");

        let cached = git_output(repo.path(), &["diff", "--cached"]);
        let unstaged = git_output(repo.path(), &["diff"]);
        assert!(cached.contains("line one point five"));
        assert!(!cached.contains("line one point seven five"));
        assert!(unstaged.contains("line one point seven five"));
    }

    #[test]
    fn stages_a_single_removed_line_from_a_multi_line_hunk() {
        let repo = init_repo();
        write(
            &repo.path().join("tracked.txt"),
            "line one\nline two\nline three\nline four\n",
        );
        git(repo.path(), &["add", "tracked.txt"]);
        git(
            repo.path(),
            &[
                "-c",
                "commit.gpgsign=false",
                "commit",
                "--quiet",
                "-m",
                "expand fixture",
            ],
        );
        write(&repo.path().join("tracked.txt"), "line one\nline four\n");

        toggle_stage_from_dir(
            repo.path(),
            &GitSelection::Line {
                side: GitDiffSide::Unstaged,
                path: "tracked.txt".to_owned(),
                hunk_index: 0,
                line_index: 0,
            },
        )
        .expect("line should stage");

        let cached = git_output(repo.path(), &["diff", "--cached"]);
        let unstaged = git_output(repo.path(), &["diff"]);
        assert!(cached.contains("-line two"));
        assert!(!cached.contains("-line three"));
        assert!(unstaged.contains("-line three"));
    }

    #[test]
    fn rejects_line_staging_for_renamed_files() {
        let repo = init_repo();
        git(repo.path(), &["mv", "tracked.txt", "renamed.txt"]);
        write(
            &repo.path().join("renamed.txt"),
            "line one\nline two renamed\nline three\n",
        );
        git(repo.path(), &["add", "-A"]);

        let error = toggle_stage_from_dir(
            repo.path(),
            &GitSelection::Line {
                side: GitDiffSide::Staged,
                path: "renamed.txt".to_owned(),
                hunk_index: 0,
                line_index: 2,
            },
        )
        .expect_err("renamed line staging should fail");

        assert!(
            error
                .to_string()
                .contains("partial staging is not supported for renamed/copied files")
        );
    }

    #[cfg(unix)]
    #[test]
    fn preserves_executable_mode_when_partially_staging_new_files() {
        let repo = init_repo();
        let script_path = repo.path().join("script.sh");
        write(&script_path, "#!/bin/sh\necho one\necho two\n");
        let mut permissions = fs::metadata(&script_path)
            .expect("script metadata should load")
            .permissions();
        permissions.set_mode(0o755);
        fs::set_permissions(&script_path, permissions).expect("script mode should be updated");

        toggle_stage_from_dir(
            repo.path(),
            &GitSelection::Line {
                side: GitDiffSide::Unstaged,
                path: "script.sh".to_owned(),
                hunk_index: 0,
                line_index: 0,
            },
        )
        .expect("line should stage");

        let index = git_output(repo.path(), &["ls-files", "--stage", "--", "script.sh"]);
        assert!(index.starts_with("100755 "));
    }

    #[test]
    fn first_push_uses_configured_branch_remote_when_present() {
        let repo = init_repo();
        let remote = TempGitDir::new("remote");
        git(remote.path(), &["init", "--bare", "--quiet"]);
        git(
            repo.path(),
            &[
                "remote",
                "add",
                "upstream",
                remote.path().to_str().expect("utf8"),
            ],
        );
        git(repo.path(), &["checkout", "-b", "feature", "--quiet"]);
        write(
            &repo.path().join("tracked.txt"),
            "line one\nline two changed\nline three\n",
        );
        git(repo.path(), &["add", "tracked.txt"]);
        commit_from_dir(
            repo.path(),
            &CommitRequest {
                message: "feature".to_owned(),
                mode: CommitMode::Create,
            },
        )
        .expect("commit should succeed");
        git(
            repo.path(),
            &["config", "branch.feature.remote", "upstream"],
        );

        push_from_dir(repo.path(), super::PushMode::Normal)
            .expect("push should use configured remote");

        let remote_refs = git_output(
            remote.path(),
            &["for-each-ref", "--format=%(refname)", "refs/heads"],
        );
        assert!(remote_refs.contains("refs/heads/feature"));
    }

    #[test]
    fn commits_and_amends_using_message_input() {
        let repo = init_repo();
        write(
            &repo.path().join("tracked.txt"),
            "line one\nline two changed\nline three\n",
        );
        git(repo.path(), &["add", "tracked.txt"]);

        commit_from_dir(
            repo.path(),
            &CommitRequest {
                message: "ship lane".to_owned(),
                mode: CommitMode::Create,
            },
        )
        .expect("commit should succeed");
        assert_eq!(
            head_commit_message_from_dir(repo.path()).expect("message should load"),
            "ship lane"
        );

        write(
            &repo.path().join("tracked.txt"),
            "line one\nline two amended\nline three\n",
        );
        git(repo.path(), &["add", "tracked.txt"]);
        commit_from_dir(
            repo.path(),
            &CommitRequest {
                message: "ship lane amended".to_owned(),
                mode: CommitMode::Amend,
            },
        )
        .expect("amend should succeed");
        assert_eq!(
            head_commit_message_from_dir(repo.path()).expect("message should load"),
            "ship lane amended"
        );
    }

    #[test]
    fn reports_current_branch_name() {
        let repo = init_repo();
        assert_eq!(
            current_branch_name_from_dir(repo.path()).expect("branch should load"),
            "master".replace(
                "master",
                git_output(repo.path(), &["branch", "--show-current"]).trim()
            )
        );
    }
}
