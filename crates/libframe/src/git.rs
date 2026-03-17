use std::{
    env,
    ffi::OsStr,
    path::{Path, PathBuf},
    process::{Command, Output},
};

use thiserror::Error;

use crate::{Diff, DiffParseError, parse_diff};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RepoDiff {
    pub repo_root: PathBuf,
    pub diff: Diff,
}

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
    Parse(#[from] DiffParseError),
}

/// Loads the current working tree diff for the process working directory.
///
/// # Errors
///
/// Returns an error if the current directory cannot be resolved, if it is not
/// inside a Git repository, if the Git executable is unavailable, if Git
/// command execution fails, or if the diff output cannot be parsed.
pub fn load_diff_from_current_dir() -> Result<RepoDiff, GitError> {
    let current_dir = env::current_dir().map_err(GitError::CurrentDir)?;
    load_diff_from_dir(&current_dir)
}

/// Loads the current working tree diff for the provided directory.
///
/// # Errors
///
/// Returns an error if `cwd` is not inside a Git repository, if the Git
/// executable is unavailable, if Git command execution fails, or if the diff
/// output cannot be parsed.
pub fn load_diff_from_dir(cwd: &Path) -> Result<RepoDiff, GitError> {
    let repo_root = repo_root(cwd)?;
    let output = run_git(
        cwd,
        [
            "diff",
            "--no-ext-diff",
            "--find-renames",
            "--no-color",
            "--unified=3",
        ],
    )?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    let diff = parse_diff(&stdout)?;

    Ok(RepoDiff { repo_root, diff })
}

fn repo_root(cwd: &Path) -> Result<PathBuf, GitError> {
    let output = run_git(cwd, ["rev-parse", "--show-toplevel"])?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    Ok(PathBuf::from(stdout.trim()))
}

fn run_git<I, S>(cwd: &Path, args: I) -> Result<Output, GitError>
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    let output = Command::new("git").current_dir(cwd).args(args).output();

    let output = match output {
        Ok(output) => output,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            return Err(GitError::GitUnavailable);
        }
        Err(error) => return Err(GitError::Io(error)),
    };

    if output.status.success() {
        return Ok(output);
    }

    let stderr = String::from_utf8_lossy(&output.stderr);
    if stderr.contains("not a git repository") {
        return Err(GitError::NotInRepo);
    }

    Err(GitError::CommandFailed(stderr.trim().to_owned()))
}

#[cfg(test)]
mod tests {
    use super::load_diff_from_dir;
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
    fn rejects_non_repo_directories() {
        let temp = TempGitDir::new("non-repo");
        let error = load_diff_from_dir(temp.path()).expect_err("non-repo should fail");
        assert!(matches!(error, super::GitError::NotInRepo));
    }

    #[test]
    fn returns_empty_diff_for_clean_repo() {
        let repo = init_repo();
        let repo_diff = load_diff_from_dir(repo.path()).expect("clean repo should load");
        assert!(repo_diff.diff.is_empty());
    }

    #[test]
    fn loads_diff_for_dirty_repo() {
        let repo = init_repo();
        write(&repo.path().join("tracked.txt"), "line one\nline two\n");
        let repo_diff = load_diff_from_dir(repo.path()).expect("dirty repo should load");
        assert_eq!(repo_diff.diff.file_count(), 1);
        assert_eq!(repo_diff.diff.hunk_count(), 1);
        assert_eq!(repo_diff.diff.changed_line_count(), 1);
        assert_eq!(repo_diff.diff.files[0].display_path(), "tracked.txt");
    }
}
