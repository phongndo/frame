use std::{
    env, fs,
    path::{Path, PathBuf},
};

use frame_core::{
    BufferSource, CodeBuffer, FileChangeKind, PatchFile, PatchSet, ReviewFile, ReviewFileInput,
    ReviewSnapshot, parse_patch,
};
use thiserror::Error;

mod shell;

use shell::{run_git, run_git_allowing_status};

#[derive(Debug, Error)]
pub enum GitError {
    #[error("current directory is not inside a git repository")]
    NotInRepo,
    #[error("git executable is not available on PATH")]
    GitUnavailable,
    #[error("failed to determine current directory: {0}")]
    CurrentDir(#[source] std::io::Error),
    #[error("git command failed: {0}")]
    CommandFailed(String),
    #[error("failed to run git: {0}")]
    Io(#[from] std::io::Error),
    #[error("failed to parse git diff: {0}")]
    Parse(#[from] frame_core::PatchParseError),
}

/// Loads a review snapshot for the current process working directory.
///
/// # Errors
///
/// Returns an error if the current directory cannot be resolved, the directory
/// is not inside a Git repository, Git cannot be executed, diff parsing fails,
/// or any file body required for review cannot be loaded.
pub fn load_review_snapshot_from_current_dir() -> Result<ReviewSnapshot, GitError> {
    let current_dir = env::current_dir().map_err(GitError::CurrentDir)?;
    load_review_snapshot_from_dir(&current_dir)
}

/// Loads a review snapshot for the provided directory.
///
/// # Errors
///
/// Returns an error if `cwd` is not inside a Git repository, Git cannot be
/// executed, diff parsing fails, or any file body required for review cannot be
/// loaded.
pub fn load_review_snapshot_from_dir(cwd: &Path) -> Result<ReviewSnapshot, GitError> {
    let repo_root = repo_root(cwd)?;
    let patch_text = collect_patch_text(&repo_root)?;
    let patch_set = parse_patch(&patch_text)?;
    let files = load_review_files(&repo_root, patch_set)?;

    Ok(ReviewSnapshot { repo_root, files })
}

/// Resolves the Git metadata directory for the repository that contains `cwd`.
///
/// # Errors
///
/// Returns an error if `cwd` is not inside a Git repository or Git cannot be
/// executed.
pub fn resolve_git_dir_from_dir(cwd: &Path) -> Result<PathBuf, GitError> {
    resolve_git_path_from_dir(cwd, "--git-dir")
}

/// Resolves the shared Git metadata directory for the repository that contains
/// `cwd`.
///
/// # Errors
///
/// Returns an error if `cwd` is not inside a Git repository or Git cannot be
/// executed.
pub fn resolve_git_common_dir_from_dir(cwd: &Path) -> Result<PathBuf, GitError> {
    resolve_git_path_from_dir(cwd, "--git-common-dir")
}

/// Returns whether `path` is ignored by Git ignore rules for the repository at
/// `repo_root`.
///
/// # Errors
///
/// Returns an error if Git cannot be executed or reports an unexpected failure.
pub fn is_path_git_ignored(repo_root: &Path, path: &Path) -> Result<bool, GitError> {
    let Ok(relative_path) = path.strip_prefix(repo_root) else {
        return Ok(false);
    };

    let output = run_git_allowing_status(
        repo_root,
        [
            std::ffi::OsStr::new("check-ignore"),
            std::ffi::OsStr::new("-q"),
            std::ffi::OsStr::new("--"),
            relative_path.as_os_str(),
        ],
        &[0, 1],
    )?;

    Ok(output.status.success())
}

fn resolve_git_path_from_dir(cwd: &Path, rev_parse_flag: &str) -> Result<PathBuf, GitError> {
    let output = run_git(cwd, ["rev-parse", rev_parse_flag])?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    let git_path = PathBuf::from(stdout.trim());

    Ok(if git_path.is_absolute() {
        git_path
    } else {
        cwd.join(git_path)
    })
}

fn load_review_files(repo_root: &Path, patch_set: PatchSet) -> Result<Vec<ReviewFile>, GitError> {
    patch_set
        .files
        .into_iter()
        .map(|patch| {
            let (buffer, source) = load_buffer(repo_root, &patch)?;
            Ok(ReviewFile::new(ReviewFileInput {
                patch,
                buffer,
                source,
            }))
        })
        .collect()
}

fn load_buffer(
    repo_root: &Path,
    patch: &PatchFile,
) -> Result<(CodeBuffer, BufferSource), GitError> {
    if patch.has_binary_or_unrenderable_change && patch.hunks.is_empty() {
        return Ok((
            CodeBuffer::placeholder("[binary or unrenderable change]"),
            BufferSource::Placeholder,
        ));
    }

    match patch.change {
        FileChangeKind::Deleted => {
            let Some(old_path) = patch.old_path.as_deref() else {
                return Ok((
                    CodeBuffer::placeholder("[deleted file path unavailable]"),
                    BufferSource::Placeholder,
                ));
            };

            let contents = read_deleted_file(repo_root, old_path)?;
            Ok((CodeBuffer::from_text(&contents), BufferSource::PreImage))
        }
        FileChangeKind::Added
        | FileChangeKind::Copied
        | FileChangeKind::Modified
        | FileChangeKind::Renamed => {
            let file_path = patch
                .new_path
                .as_deref()
                .or(patch.old_path.as_deref())
                .unwrap_or("<unknown>");
            let contents = read_worktree_file(repo_root, file_path)?;
            Ok((CodeBuffer::from_text(&contents), BufferSource::PostImage))
        }
    }
}

fn collect_patch_text(cwd: &Path) -> Result<String, GitError> {
    let mut patch_text = if head_exists(cwd)? {
        tracked_patch_text(cwd)?
    } else {
        String::new()
    };

    for path in untracked_files(cwd)? {
        if !patch_text.is_empty() && !patch_text.ends_with('\n') {
            patch_text.push('\n');
        }

        patch_text.push_str(&untracked_file_patch_text(cwd, &path)?);
    }

    Ok(patch_text)
}

fn head_exists(cwd: &Path) -> Result<bool, GitError> {
    let output = run_git_allowing_status(cwd, ["rev-parse", "--verify", "HEAD"], &[0, 128])?;
    Ok(output.status.success())
}

fn tracked_patch_text(cwd: &Path) -> Result<String, GitError> {
    let output = run_git(
        cwd,
        [
            "diff",
            "HEAD",
            "--no-ext-diff",
            "--find-renames",
            "--no-color",
            "--unified=3",
        ],
    )?;
    Ok(String::from_utf8_lossy(&output.stdout).into_owned())
}

fn untracked_files(cwd: &Path) -> Result<Vec<PathBuf>, GitError> {
    let output = run_git(cwd, ["ls-files", "--others", "--exclude-standard", "-z"])?;
    let stdout = String::from_utf8_lossy(&output.stdout);

    Ok(stdout
        .split('\0')
        .filter(|path| !path.is_empty())
        .map(PathBuf::from)
        .collect())
}

fn untracked_file_patch_text(cwd: &Path, path: &Path) -> Result<String, GitError> {
    let output = run_git_allowing_status(
        cwd,
        [
            std::ffi::OsStr::new("diff"),
            std::ffi::OsStr::new("--no-index"),
            std::ffi::OsStr::new("--no-color"),
            std::ffi::OsStr::new("--unified=3"),
            std::ffi::OsStr::new("--"),
            std::ffi::OsStr::new("/dev/null"),
            path.as_os_str(),
        ],
        &[0, 1],
    )?;
    Ok(String::from_utf8_lossy(&output.stdout).into_owned())
}

fn read_worktree_file(repo_root: &Path, path: &str) -> Result<String, GitError> {
    let bytes = fs::read(repo_root.join(path))?;
    Ok(String::from_utf8_lossy(&bytes).into_owned())
}

fn read_deleted_file(repo_root: &Path, path: &str) -> Result<String, GitError> {
    let spec = format!("HEAD:{path}");
    let output = run_git(
        repo_root,
        [std::ffi::OsStr::new("show"), std::ffi::OsStr::new(&spec)],
    )?;
    Ok(String::from_utf8_lossy(&output.stdout).into_owned())
}

fn repo_root(cwd: &Path) -> Result<PathBuf, GitError> {
    let output = run_git(cwd, ["rev-parse", "--show-toplevel"])?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    Ok(PathBuf::from(stdout.trim()))
}

#[cfg(test)]
mod tests {
    use super::{
        is_path_git_ignored, load_review_snapshot_from_dir, resolve_git_common_dir_from_dir,
        resolve_git_dir_from_dir,
    };
    use frame_core::{BufferSource, ChangeKind, FileChangeKind};
    use std::{
        fs,
        path::{Path, PathBuf},
        process::Command,
        sync::atomic::{AtomicUsize, Ordering},
        time::{SystemTime, UNIX_EPOCH},
    };

    static TEMP_COUNTER: AtomicUsize = AtomicUsize::new(0);

    #[derive(Debug)]
    struct TempGitDir {
        path: PathBuf,
    }

    impl TempGitDir {
        fn new(name: &str) -> Self {
            let counter = TEMP_COUNTER.fetch_add(1, Ordering::Relaxed);
            let unique = format!(
                "frame-{name}-{}-{}-{counter}",
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
        write(&temp.path().join("tracked.txt"), "line one\n");
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

    fn init_unborn_repo() -> TempGitDir {
        let temp = TempGitDir::new("unborn");
        git(temp.path(), &["init", "--quiet"]);
        git(
            temp.path(),
            &["config", "user.email", "frame-tests@example.com"],
        );
        git(temp.path(), &["config", "user.name", "Frame Tests"]);
        temp
    }

    #[test]
    fn returns_empty_snapshot_for_clean_repo() {
        let repo = init_repo();
        let snapshot = load_review_snapshot_from_dir(repo.path()).expect("clean repo should load");
        assert!(snapshot.is_empty());
    }

    #[test]
    fn loads_modified_files_into_postimage_buffers() {
        let repo = init_repo();
        write(&repo.path().join("tracked.txt"), "line one\nline two\n");

        let snapshot = load_review_snapshot_from_dir(repo.path()).expect("dirty repo should load");
        let file = &snapshot.files[0];
        assert_eq!(file.patch.change, FileChangeKind::Modified);
        assert_eq!(file.source, BufferSource::PostImage);
        assert_eq!(file.buffer.line(1), Some("line two"));
        assert_eq!(file.line_change(1), Some(ChangeKind::Added));
    }

    #[test]
    fn loads_untracked_files_as_added() {
        let repo = init_repo();
        write(&repo.path().join("new.rs"), "pub fn preview() {}\n");

        let snapshot =
            load_review_snapshot_from_dir(repo.path()).expect("untracked file should load");
        let file = &snapshot.files[0];
        assert_eq!(file.patch.change, FileChangeKind::Added);
        assert_eq!(file.source, BufferSource::PostImage);
        assert_eq!(file.buffer.line(0), Some("pub fn preview() {}"));
    }

    #[test]
    fn loads_untracked_files_from_nested_cwd() {
        let repo = init_repo();
        let nested = repo.path().join("subdir");
        fs::create_dir_all(&nested).expect("nested directory should be created");
        write(&nested.join("new.rs"), "pub fn preview() {}\n");

        let snapshot =
            load_review_snapshot_from_dir(&nested).expect("nested untracked file should load");
        let file = &snapshot.files[0];
        assert_eq!(file.patch.change, FileChangeKind::Added);
        assert_eq!(file.source, BufferSource::PostImage);
        assert_eq!(file.patch.new_path.as_deref(), Some("subdir/new.rs"));
        assert_eq!(file.buffer.line(0), Some("pub fn preview() {}"));
    }

    #[test]
    fn loads_deleted_files_from_head() {
        let repo = init_repo();
        fs::remove_file(repo.path().join("tracked.txt")).expect("file delete should succeed");

        let snapshot =
            load_review_snapshot_from_dir(repo.path()).expect("deleted file should load");
        let file = &snapshot.files[0];
        assert_eq!(file.patch.change, FileChangeKind::Deleted);
        assert_eq!(file.source, BufferSource::PreImage);
        assert_eq!(file.buffer.line(0), Some("line one"));
    }

    #[test]
    fn loads_untracked_files_from_unborn_repo() {
        let repo = init_unborn_repo();
        write(&repo.path().join("new.rs"), "pub fn preview() {}\n");

        let snapshot = load_review_snapshot_from_dir(repo.path()).expect("unborn repo should load");
        let file = &snapshot.files[0];
        assert_eq!(file.patch.change, FileChangeKind::Added);
        assert_eq!(file.source, BufferSource::PostImage);
        assert_eq!(file.patch.new_path.as_deref(), Some("new.rs"));
        assert_eq!(file.buffer.line(0), Some("pub fn preview() {}"));
    }

    #[test]
    fn resolves_git_dir_relative_to_repo_root() {
        let repo = init_repo();

        let git_dir =
            resolve_git_dir_from_dir(repo.path()).expect("git dir should resolve successfully");

        assert_eq!(git_dir, repo.path().join(".git"));
    }

    #[test]
    fn resolves_git_common_dir_for_linked_worktree() {
        let repo = init_repo();
        let worktree = TempGitDir::new("worktree");
        fs::remove_dir_all(worktree.path()).expect("precreated temp dir should be removed");

        git(repo.path(), &["branch", "linked"]);
        git(
            repo.path(),
            &[
                "worktree",
                "add",
                "--quiet",
                worktree
                    .path()
                    .to_str()
                    .expect("worktree path should be valid"),
                "linked",
            ],
        );

        let git_dir =
            resolve_git_dir_from_dir(worktree.path()).expect("git dir should resolve for worktree");
        let common_dir = resolve_git_common_dir_from_dir(worktree.path())
            .expect("git common dir should resolve for worktree");

        assert_ne!(git_dir, common_dir);
        assert_eq!(
            common_dir
                .canonicalize()
                .expect("common dir should canonicalize"),
            repo.path()
                .join(".git")
                .canonicalize()
                .expect("repo git dir should canonicalize")
        );
    }

    #[test]
    fn detects_git_ignored_paths_without_hiding_tracked_files() {
        let repo = init_repo();
        write(&repo.path().join(".gitignore"), "target/\n");
        fs::create_dir_all(repo.path().join("target")).expect("target dir should be created");
        write(&repo.path().join("target/generated.txt"), "generated\n");

        assert!(
            is_path_git_ignored(repo.path(), &repo.path().join("target/generated.txt"))
                .expect("ignored path should be checked")
        );
        assert!(
            !is_path_git_ignored(repo.path(), &repo.path().join("tracked.txt"))
                .expect("tracked file should not be treated as ignored")
        );
    }
}
