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
        FileChangeKind::Added | FileChangeKind::Modified | FileChangeKind::Renamed => {
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
    let mut patch_text = tracked_patch_text(cwd)?;

    for path in untracked_files(cwd)? {
        if !patch_text.is_empty() && !patch_text.ends_with('\n') {
            patch_text.push('\n');
        }

        patch_text.push_str(&untracked_file_patch_text(cwd, &path)?);
    }

    Ok(patch_text)
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
    use super::load_review_snapshot_from_dir;
    use frame_core::{BufferSource, ChangeKind, FileChangeKind};
    use std::{
        fs,
        path::{Path, PathBuf},
        process::Command,
        time::{SystemTime, UNIX_EPOCH},
    };

    #[derive(Debug)]
    struct TempGitDir {
        path: PathBuf,
    }

    impl TempGitDir {
        fn new(name: &str) -> Self {
            let unique = format!(
                "frame-{name}-{}-{}",
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
}
