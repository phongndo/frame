use std::{
    ffi::OsStr,
    path::Path,
    process::{Command, Output},
};

use super::GitError;

pub(crate) fn run_git<I, S>(cwd: &Path, args: I) -> Result<Output, GitError>
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    run_git_allowing_status(cwd, args, &[0])
}

pub(crate) fn run_git_allowing_status<I, S>(
    cwd: &Path,
    args: I,
    allowed_statuses: &[i32],
) -> Result<Output, GitError>
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

    if output
        .status
        .code()
        .is_some_and(|code| allowed_statuses.contains(&code))
    {
        return Ok(output);
    }

    let stderr = String::from_utf8_lossy(&output.stderr);
    if stderr.contains("not a git repository") {
        return Err(GitError::NotInRepo);
    }

    Err(GitError::CommandFailed(stderr.trim().to_owned()))
}
